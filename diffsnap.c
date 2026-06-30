#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/wait.h>
#include <sys/file.h>

#ifndef CONF_PATH
#define CONF_PATH "/usr/local/etc/diffsnap.conf"
#endif
#ifndef LOG_PATH
#define LOG_PATH "/var/log/diffsnap.log"
#endif
#ifndef LOCK_PATH
#define LOCK_PATH "/var/run/diffsnap.lock"
#endif
#ifndef ZFS_PATH
#define ZFS_PATH "/sbin/zfs"
#endif
#define DIFFSNAP_VERSION "1.0"

#define ALLOC_CHUNK_BATCH 32
#define ALLOC_CHUNK_PRUNE 128
#define ALLOC_CHUNK_METRIC 256
#define STR_BUF_SMALL 32
#define STR_BUF_MED 64
#define ZFS_NAME_MAX 256
#define STR_BUF_LARGE (ZFS_NAME_MAX + 1)
#define STR_BUF_XLARGE 512

#define EXIT_EXEC_FAILED 127

#define REQUIRE_TOKEN(err_msg) \
    do { \
        token = strtok_r(NULL, " \t", &saveptr); \
        if (!token) { \
            log_msg("Error: Config error for %s: %s", dataset, err_msg); \
            global_status = 1; \
            continue; \
        } \
    } while (0)

static FILE *log_fp = NULL;

typedef struct { char *dataset; char *prefix; size_t retention; } batch_item_t;
typedef struct { batch_item_t *items; size_t count; size_t capacity; } batch_ctx_t;
typedef struct { char **snaps; size_t count; size_t capacity; const char *match_str; size_t match_len; } prune_ctx_t;
typedef struct { char **names; size_t count; size_t capacity; } name_list_t;
typedef struct { char name[STR_BUF_LARGE]; long long written; } metric_item_t;
typedef struct { metric_item_t *items; size_t count; size_t capacity; } metric_ctx_t;

static void print_version(void) {
    printf("diffsnap %s\n", DIFFSNAP_VERSION);
}

static void print_help(const char *progname) {
    printf(
        "Usage: %s [--help] [--version]\n"
        "\n"
        "Create and prune ZFS snapshots from a space-separated config file.\n"
        "\n"
        "Files:\n"
        "  Config: %s\n"
        "  Log:    %s\n"
        "  Lock:   %s\n"
        "\n"
        "Config fields, space separated:\n"
        "  dataset interval_minutes retention prefix recursive min_bytes\n"
        "\n"
        "Field notes:\n"
        "  dataset           ZFS dataset name\n"
        "  interval_minutes  Intervals less than 60 must divide evenly into 60. Intervals greater than 60 must divide evenly into 1440\n"
        "  retention         Number of matching snapshots to keep\n"
        "  prefix            Snapshot prefix using letters, numbers, '_' or '-'. To avoid pruning snapshots created outside of diffsnap make sure this is unique\n"
        "  recursive         yes or no\n"
        "  min_bytes         Minimum written bytes needed before snapshotting\n"
        "\n"
        "Example config line:\n"
        "  zroot/home 60 24 hourly no 0\n"
        "\n"
        "Example cron line:\n"
        "  */15 * * * * /usr/local/sbin/diffsnap\n",
        progname, CONF_PATH, LOG_PATH, LOCK_PATH
    );
}

static void trim_trailing_whitespace(char *str) {
    size_t len = strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) str[--len] = '\0';
}

static int copy_token(char *dst, size_t dst_size, const char *src) {
    size_t len = strlen(src);
    if (len >= dst_size) return -1;
    memcpy(dst, src, len + 1);
    return 0;
}

static int valid_prefix(const char *prefix) {
    if (prefix[0] == '\0') return 0;
    for (size_t i = 0; prefix[i]; i++) {
        if (!isalnum((unsigned char)prefix[i]) && prefix[i] != '_' && prefix[i] != '-') return 0;
    }
    return 1;
}

static int date_stamp_like(const char *s) {
    static const int digit_pos[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    if (strlen(s) != 19 || s[4] != '-' || s[7] != '-' || s[10] != '_' || s[13] != ':' || s[16] != ':') return 0;
    for (size_t i = 0; i < sizeof(digit_pos) / sizeof(digit_pos[0]); i++) {
        if (!isdigit((unsigned char)s[digit_pos[i]])) return 0;
    }
    return 1;
}

static void log_msg(const char *fmt, ...) {
    if (!log_fp) return;
    va_list args;
    va_start(args, fmt);
    time_t t = time(NULL);
    struct tm tm_info;
    char timestamp[STR_BUF_SMALL];
    
    if (localtime_r(&t, &tm_info) == NULL || strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_info) == 0) {
        snprintf(timestamp, sizeof(timestamp), "unknown-time");
    }
    fprintf(log_fp, "[%s] ", timestamp);
    vfprintf(log_fp, fmt, args);
    fprintf(log_fp, "\n");
    va_end(args);
}

static void format_bytes(long long bytes, char *buf, size_t buf_size) {
    if (bytes <= 0) {
        snprintf(buf, buf_size, "0");
        return;
    }
    const char *units[] = {"", "K", "M", "G", "T", "P"};
    int i = 0;
    double d_bytes = (double)bytes;
    while (d_bytes >= 1024.0 && i < 5) {
        d_bytes /= 1024.0;
        i++;
    }
    if (i == 0) {
        snprintf(buf, buf_size, "%lld", bytes);
    } else {
        snprintf(buf, buf_size, "%.2f%s", d_bytes, units[i]);
    }
}

static void prune_ctx_free(prune_ctx_t *ctx) {
    if (!ctx->snaps) return;
    for (size_t i = 0; i < ctx->count; i++) free(ctx->snaps[i]);
    free(ctx->snaps);
    ctx->snaps = NULL; ctx->count = 0; ctx->capacity = 0;
}

static void name_list_free(name_list_t *list) {
    if (!list->names) return;
    for (size_t i = 0; i < list->count; i++) free(list->names[i]);
    free(list->names);
    list->names = NULL; list->count = 0; list->capacity = 0;
}

typedef int (*line_handler_t)(const char *line, void *data);

static int process_stream(int fd, line_handler_t handler, void *data) {
    FILE *stream = fdopen(fd, "r");
    if (!stream) { close(fd); return -1; }
    char line_buf[STR_BUF_XLARGE];
    int handler_failed = 0;
    for (;;) {
        if (fgets(line_buf, sizeof(line_buf), stream) != NULL) {
            trim_trailing_whitespace(line_buf);
            if (!handler_failed && handler(line_buf, data) != 0) handler_failed = 1;
            continue;
        }
        if (ferror(stream) && errno == EINTR) { clearerr(stream); continue; }
        break;
    }
    int err = ferror(stream);
    fclose(stream);
    return (handler_failed || err) ? -1 : 0;
}

static int exec_cmd_stream(const char *const argv[], line_handler_t handler, void *data) {
    int pfd[2];
    if (handler && pipe2(pfd, O_CLOEXEC) == -1) return -1;
    pid_t pid = fork();
    if (pid == -1) { if (handler) { close(pfd[0]); close(pfd[1]); } return -1; }
    if (pid == 0) {
        if (handler) {
            if (dup2(pfd[1], STDOUT_FILENO) == -1 || dup2(pfd[1], STDERR_FILENO) == -1) _exit(EXIT_EXEC_FAILED);
            close(pfd[0]); close(pfd[1]);
        } else {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull == -1 || dup2(devnull, STDOUT_FILENO) == -1 || dup2(devnull, STDERR_FILENO) == -1) _exit(EXIT_EXEC_FAILED);
            close(devnull);
        }
        execv(argv[0], (char *const *)argv);
        _exit(EXIT_EXEC_FAILED);
    }
    int processing_rc = 0;
    if (handler) { close(pfd[1]); processing_rc = process_stream(pfd[0], handler, data); }
    int status; pid_t wpid;
    do { wpid = waitpid(pid, &status, 0); } while (wpid == -1 && errno == EINTR);
    return (wpid != -1 && processing_rc == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int handle_metric_line(const char *line, void *data) {
    metric_ctx_t *ctx = (metric_ctx_t *)data;
    char line_copy[STR_BUF_XLARGE];
    if (strlen(line) >= sizeof(line_copy)) return -1;
    strcpy(line_copy, line);
    char *saveptr = NULL;
    char *name = strtok_r(line_copy, " \t", &saveptr);
    char *value = strtok_r(NULL, " \t", &saveptr);
    if (!name || !value) return 0; 
    if (ctx->count >= ctx->capacity) {
        size_t new_cap = ctx->capacity == 0 ? ALLOC_CHUNK_METRIC : ctx->capacity * 2;
        metric_item_t *tmp = realloc(ctx->items, new_cap * sizeof(metric_item_t));
        if (!tmp) return -1;
        ctx->items = tmp; ctx->capacity = new_cap;
    }
    if (strlen(name) >= sizeof(ctx->items[ctx->count].name)) return -1;
    strcpy(ctx->items[ctx->count].name, name);
    char *endptr; errno = 0;
    long long val = strtoll(value, &endptr, 10);
    ctx->items[ctx->count].written = (errno == ERANGE || *endptr != '\0' || val < 0) ? -1 : val;
    ctx->count++; return 0;
}

static int compare_metrics(const void *a, const void *b) { return strcmp(((metric_item_t *)a)->name, ((metric_item_t *)b)->name); }
static int zfs_destroy(const char *snap_name) { const char *const argv[] = {ZFS_PATH, "destroy", snap_name, NULL}; return exec_cmd_stream(argv, NULL, NULL); }

static int handle_prune_line(const char *line, void *data) {
    prune_ctx_t *ctx = (prune_ctx_t *)data;
    const char *at = strchr(line, '@');
    if (at && strncmp(at + 1, ctx->match_str, ctx->match_len) == 0) {
        if (date_stamp_like(at + 1 + ctx->match_len)) {
            if (ctx->count >= ctx->capacity) {
                size_t new_cap = ctx->capacity == 0 ? ALLOC_CHUNK_PRUNE : ctx->capacity * 2;
                char **tmp = realloc(ctx->snaps, new_cap * sizeof(char *));
                if (!tmp) return -1;
                ctx->snaps = tmp; ctx->capacity = new_cap;
            }
            if ((ctx->snaps[ctx->count] = strdup(line)) == NULL) return -1;
            ctx->count++;
        }
    }
    return 0;
}

static int prune_old_snapshots(const char *dataset, const char *prefix, size_t max_snaps) {
    char match_str[STR_BUF_LARGE];
    int match_rc = snprintf(match_str, sizeof(match_str), "%s_", prefix);
    if (match_rc < 0 || (size_t)match_rc >= sizeof(match_str)) return -1;
    prune_ctx_t ctx = { .snaps = NULL, .count = 0, .capacity = 0, .match_str = match_str, .match_len = (size_t)match_rc };
    const char *const argv[] = {ZFS_PATH, "list", "-H", "-t", "snapshot", "-o", "name", "-S", "creation", dataset, NULL};
    if (exec_cmd_stream(argv, handle_prune_line, &ctx) != 0) {
        log_msg("Error: Failed to list snapshots for %s", dataset);
        prune_ctx_free(&ctx); return -1;
    }
    int prune_errors = 0;
    while (ctx.count > max_snaps) {
        char *oldest = ctx.snaps[--ctx.count];
        if (zfs_destroy(oldest) == 0) log_msg("Pruned snapshot: %s", oldest);
        else { log_msg("Error: Failed to prune snapshot %s", oldest); prune_errors++; }
        free(oldest); ctx.snaps[ctx.count] = NULL;
    }
    prune_ctx_free(&ctx);
    return (prune_errors == 0) ? 0 : -1;
}

static int batch_add(batch_ctx_t *ctx, const char *dataset, const char *prefix, size_t retention) {
    if (ctx->count >= ctx->capacity) {
        size_t new_cap = ctx->capacity == 0 ? ALLOC_CHUNK_BATCH : ctx->capacity * 2;
        batch_item_t *tmp = realloc(ctx->items, new_cap * sizeof(batch_item_t));
        if (!tmp) return -1;
        ctx->items = tmp; ctx->capacity = new_cap;
    }
    char *d = strdup(dataset), *p = strdup(prefix);
    if (!d || !p) { free(d); free(p); return -1; }
    ctx->items[ctx->count++] = (batch_item_t){ .dataset = d, .prefix = p, .retention = retention };
    return 0;
}

static void batch_free(batch_ctx_t *ctx) {
    for (size_t i = 0; i < ctx->count; i++) { free(ctx->items[i].dataset); free(ctx->items[i].prefix); }
    free(ctx->items);
}

static int zfs_snapshot_batch(batch_ctx_t *ctx, int recursive, const char *timestamp) {
    if (ctx->count == 0) return 0;
    size_t total_args = (recursive ? 4 : 3) + ctx->count + 1;
    const char **argv = malloc(total_args * sizeof(char *));
    if (!argv) return -1;
    size_t idx = 0; argv[idx++] = ZFS_PATH; argv[idx++] = "snapshot";
    if (recursive) argv[idx++] = "-r";
    size_t total_bytes = 0;
    for (size_t i = 0; i < ctx->count; i++) total_bytes += strlen(ctx->items[i].dataset) + strlen(ctx->items[i].prefix) + strlen(timestamp) + 3;
    char *arena = malloc(total_bytes), *offset = arena;
    if (!arena) { free(argv); return -1; }
    for (size_t i = 0; i < ctx->count; i++) {
        size_t remaining = total_bytes - (size_t)(offset - arena);
        int written = snprintf(offset, remaining, "%s@%s_%s", ctx->items[i].dataset, ctx->items[i].prefix, timestamp);
        if (written < 0 || (size_t)written >= remaining) { free(arena); free(argv); return -1; }
        argv[idx++] = offset; offset += written + 1;
    }
    argv[idx] = NULL;
    int rc = exec_cmd_stream(argv, NULL, NULL);
    free(arena); free(argv); return rc;
}

static int compare_names(const void *a, const void *b) {
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;
    return strcmp(*sa, *sb);
}

static int name_index_contains(char **names, size_t count, const char *name) {
    if (count == 0) return 0;
    char *key = (char *)name;
    return bsearch(&key, names, count, sizeof(*names), compare_names) != NULL;
}

static int handle_snapshot_inventory_line(const char *line, void *data) {
    name_list_t *list = (name_list_t *)data;
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity == 0 ? ALLOC_CHUNK_PRUNE : list->capacity * 2;
        char **tmp = realloc(list->names, new_cap * sizeof(*list->names));
        if (!tmp) return -1;
        list->names = tmp; list->capacity = new_cap;
    }
    if ((list->names[list->count] = strdup(line)) == NULL) return -1;
    list->count++;
    return 0;
}

static int load_snapshot_inventory(name_list_t *list) {
    const char *const argv[] = {ZFS_PATH, "list", "-H", "-t", "snapshot", "-o", "name", NULL};
    if (exec_cmd_stream(argv, handle_snapshot_inventory_line, list) != 0) {
        name_list_free(list);
        return -1;
    }
    if (list->count > 0) qsort(list->names, list->count, sizeof(*list->names), compare_names);
    return 0;
}

static int is_recursively_covered(const char *dataset, char **rec_names, size_t rec_count) {
    char candidate[STR_BUF_LARGE];
    if (copy_token(candidate, sizeof(candidate), dataset) != 0) return 0;
    for (;;) {
        if (name_index_contains(rec_names, rec_count, candidate)) return 1;
        char *slash = strrchr(candidate, '/');
        if (!slash) return 0;
        *slash = '\0';
    }
}

static int remove_recursive_overlaps(batch_ctx_t *std_b, const batch_ctx_t *rec_b) {
    if (std_b->count == 0 || rec_b->count == 0) return 0;
    char **rec_names = malloc(rec_b->count * sizeof(*rec_names));
    if (!rec_names) return -1;
    for (size_t i = 0; i < rec_b->count; i++) rec_names[i] = rec_b->items[i].dataset;
    qsort(rec_names, rec_b->count, sizeof(*rec_names), compare_names);
    size_t write_idx = 0;
    for (size_t i = 0; i < std_b->count; i++) {
        if (is_recursively_covered(std_b->items[i].dataset, rec_names, rec_b->count)) {
            free(std_b->items[i].dataset);
            free(std_b->items[i].prefix);
        } else {
            if (write_idx != i) std_b->items[write_idx] = std_b->items[i];
            write_idx++;
        }
    }
    std_b->count = write_idx;
    free(rec_names);
    return 0;
}

static int process_batch(batch_ctx_t *batch, metric_ctx_t *metrics, const char *snap_time, int recursive) {
    int rc = zfs_snapshot_batch(batch, recursive, snap_time);
    int status = rc != 0 ? 1 : 0;
    name_list_t snapshots = { NULL, 0, 0 };
    int can_verify = 1;
    if (rc != 0) log_msg("Error: %s zfs snapshot batch execution failed", recursive ? "recursive" : "standard");
    if (rc != 0 && load_snapshot_inventory(&snapshots) != 0) {
        log_msg("Error: Unable to list snapshots for %sbatch verification", recursive ? "recursive " : "");
        can_verify = 0;
    }
    for (size_t i = 0; i < batch->count; i++) {
        char snap_name[STR_BUF_XLARGE];
        int len = snprintf(snap_name, sizeof(snap_name), "%s@%s_%s", batch->items[i].dataset, batch->items[i].prefix, snap_time);
        if (len < 0 || (size_t)len >= sizeof(snap_name)) { log_msg("Error: %sSnapshot name too long for %s", recursive ? "Recursive " : "", batch->items[i].dataset); status = 1; continue; }
        if (rc != 0) {
            if (!can_verify) { log_msg("Error: Unable to verify %ssnapshot exists: %s", recursive ? "recursive " : "", snap_name); continue; }
            if (!name_index_contains(snapshots.names, snapshots.count, snap_name)) { log_msg("Error: %sSnapshot not created: %s", recursive ? "Recursive " : "", snap_name); continue; }
        }
        metric_item_t key = {0};
        memcpy(key.name, batch->items[i].dataset, strlen(batch->items[i].dataset) + 1);
        metric_item_t *found = bsearch(&key, metrics->items, metrics->count, sizeof(metric_item_t), compare_metrics);
        char h_bytes[STR_BUF_SMALL] = "0";
        if (found && found->written != -1) format_bytes(found->written, h_bytes, sizeof(h_bytes));
        log_msg("Snapshot created%s: %s (%s written)", recursive ? " (recursive)" : "", snap_name, h_bytes);
        if (prune_old_snapshots(batch->items[i].dataset, batch->items[i].prefix, batch->items[i].retention) != 0) status = 1;
    }
    name_list_free(&snapshots);
    return status;
}

int main(int argc, char *argv[]) {
    const char *progname = (argc > 0 && argv[0]) ? argv[0] : "diffsnap";
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [--help] [--version]\n", progname);
        return 2;
    }
    if (argc == 2) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            print_help(progname);
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
            print_version();
            return 0;
        }
        fprintf(stderr, "%s: unknown option: %s\n", progname, argv[1]);
        fprintf(stderr, "Usage: %s [--help] [--version]\n", progname);
        return 2;
    }

    int lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT, 0600);
    if (lock_fd < 0) return 1;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) { close(lock_fd); return 1; }
    int global_status = 0, ret_code = 0;
    metric_ctx_t metrics = { NULL, 0, 0 };
    batch_ctx_t std_b = { NULL, 0, 0 }, rec_b = { NULL, 0, 0 };
    char *line = NULL; FILE *conf = NULL;
    if ((log_fp = fopen(LOG_PATH, "a")) == NULL) { ret_code = 1; goto cleanup; }
    setvbuf(log_fp, NULL, _IOLBF, 0);
    const char *const m_argv[] = {ZFS_PATH, "get", "-H", "-p", "-o", "name,value", "written", NULL};
    if (exec_cmd_stream(m_argv, handle_metric_line, &metrics) != 0) { log_msg("Error: Failed to read ZFS written metrics"); ret_code = 1; goto cleanup; }
    qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);
    conf = fopen(CONF_PATH, "r");
    if (!conf) { ret_code = 1; goto cleanup; }
    time_t t = time(NULL); struct tm tm_info; 
    if (localtime_r(&t, &tm_info) == NULL) { ret_code = 1; goto cleanup; }
    size_t line_cap = 0;
    long long current_day_mins = (tm_info.tm_hour * 60) + tm_info.tm_min;
    while (getline(&line, &line_cap, conf) != -1) {
        trim_trailing_whitespace(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char dataset[STR_BUF_LARGE] = {0}, prefix[STR_BUF_MED] = {0}, recursive_str[STR_BUF_SMALL] = {0};
        long long interval_mins = 0, retention_val = 0, min_bytes = 0;
        char *endptr, *saveptr = NULL, *token = strtok_r(line, " \t", &saveptr);
        if (!token) continue;
        if (copy_token(dataset, sizeof(dataset), token) != 0) { log_msg("Error: Config error: dataset name too long"); global_status = 1; continue; }
        REQUIRE_TOKEN("missing interval field"); errno = 0; interval_mins = strtoll(token, &endptr, 10);
        if (errno == ERANGE || *endptr != '\0' || interval_mins <= 0 || interval_mins > 1440 || (1440 % interval_mins != 0 && interval_mins > 60)) {
            log_msg("Error: Config error for %s: invalid or non-uniform day interval '%s'", dataset, token); global_status = 1; continue;
        }
        REQUIRE_TOKEN("missing retention field"); errno = 0; retention_val = strtoll(token, &endptr, 10);
        if (errno == ERANGE || *endptr != '\0' || retention_val < 1) {
            log_msg("Error: Config error for %s: invalid retention count '%s'", dataset, token); global_status = 1; continue;
        }
        REQUIRE_TOKEN("missing prefix field");
        if (copy_token(prefix, sizeof(prefix), token) != 0) { log_msg("Error: Config error for %s: prefix too long", dataset); global_status = 1; continue; }
        if (!valid_prefix(prefix)) { log_msg("Error: Config error for %s: invalid prefix '%s'", dataset, prefix); global_status = 1; continue; }
        REQUIRE_TOKEN("missing recursive field");
        if (copy_token(recursive_str, sizeof(recursive_str), token) != 0) { log_msg("Error: Config error for %s: recursive field too long", dataset); global_status = 1; continue; }
        REQUIRE_TOKEN("missing min_bytes field"); errno = 0; min_bytes = strtoll(token, &endptr, 10);
        if (errno == ERANGE || *endptr != '\0' || min_bytes < 0 || strtok_r(NULL, " \t", &saveptr) != NULL) {
            log_msg("Error: Config error for %s: invalid byte threshold or trailing garbage", dataset); global_status = 1; continue;
        }
        int is_recursive;
        if (strcmp(recursive_str, "yes") == 0) is_recursive = 1;
        else if (strcmp(recursive_str, "no") == 0) is_recursive = 0;
        else { log_msg("Error: Config error for %s: invalid recursive flag option '%s'", dataset, recursive_str); global_status = 1; continue; }
        if (current_day_mins % interval_mins != 0) continue;
        metric_item_t key = {0}; memcpy(key.name, dataset, strlen(dataset) + 1);
        metric_item_t *found = bsearch(&key, metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);
        if (!found) { log_msg("Error: Configured dataset not found: %s", dataset); global_status = 1; continue; }
        if (found->written == -1) { log_msg("Error: Invalid written metric for %s", dataset); global_status = 1; continue; }
        if (found->written < min_bytes) continue;
        if (is_recursive) { if (batch_add(&rec_b, dataset, prefix, (size_t)retention_val) != 0) { log_msg("Error: Failed to allocate batch entry for %s", dataset); global_status = 1; } }
        else { if (batch_add(&std_b, dataset, prefix, (size_t)retention_val) != 0) { log_msg("Error: Failed to allocate batch entry for %s", dataset); global_status = 1; } }
    }
    if (remove_recursive_overlaps(&std_b, &rec_b) != 0) { log_msg("Error: Failed to check recursive overlaps"); ret_code = 1; goto cleanup; }
    char snap_time[STR_BUF_SMALL];
    if (strftime(snap_time, sizeof(snap_time), "%Y-%m-%d_%H:%M:%S", &tm_info) == 0) { log_msg("Error: Failed to format timestamp"); ret_code = 1; goto cleanup; }
    
    if (process_batch(&std_b, &metrics, snap_time, 0) != 0) global_status = 1;
    if (process_batch(&rec_b, &metrics, snap_time, 1) != 0) global_status = 1;
    ret_code = global_status;
cleanup:
    free(line); if (conf) fclose(conf); free(metrics.items); batch_free(&std_b); batch_free(&rec_b); if (log_fp) fclose(log_fp); close(lock_fd); return ret_code;
}
