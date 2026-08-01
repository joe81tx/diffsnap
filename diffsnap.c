#define _GNU_SOURCE

#include <assert.h>
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
#include <poll.h>

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
#ifndef BUILD_SHA
#define BUILD_SHA "unknown"
#endif

#define ALLOC_CHUNK_BATCH 32
#define ALLOC_CHUNK_PRUNE 128
#define ALLOC_CHUNK_METRIC 256
/*
 * Conservative cap on the total bytes of snapshot-name arguments packed
 * into a single `zfs snapshot` invocation. Real OS command-line limits vary
 * (Linux's ARG_MAX is usually a couple MB; other platforms differ), so
 * rather than querying per-platform limits we use one fixed, comfortably
 * conservative budget everywhere. If a root+pass would exceed it, it's
 * split into multiple `zfs snapshot` calls instead of one call per dataset.
 */
#define ARGV_BYTES_CAP (128 * 1024)
#define STR_BUF_SMALL 32
#define STR_BUF_MED 64
#define ZFS_NAME_MAX 256
#define STR_BUF_LARGE (ZFS_NAME_MAX + 1)
#define STR_BUF_XLARGE 512

#define EXIT_EXEC_FAILED 127

static_assert(sizeof(void *) == 8, "diffsnap requires a 64-bit architecture");

#define REQUIRE_TOKEN(err_msg) \
    do { \
        token = strtok_r(NULL, ",", &saveptr); \
        if (!token) { \
            log_msg("Error: Config error for %s: %s", dataset, err_msg); \
            global_status = 1; \
            goto next_line; \
        } \
    } while (0)

static FILE *log_fp = NULL;
/* Set when any fprintf/vfprintf to log_fp fails (e.g. disk full, EIO).
 * log_msg() has no return value for callers to check, so this is how a
 * mid-run logging failure gets surfaced to main()'s exit status instead of
 * being silently swallowed. */
static int log_io_failed = 0;

typedef int (*line_handler_t)(const char *line, void *data);
typedef struct { char *dataset; char *prefix; size_t retention; size_t pass; int snap_failed; long long written; long long min_bytes; } batch_item_t;
typedef struct { batch_item_t *items; size_t count; size_t capacity; } batch_ctx_t;
typedef struct { char **names; size_t count; size_t capacity; } name_list_t;
typedef struct { char name[STR_BUF_LARGE]; long long written; } metric_item_t;
typedef struct { metric_item_t *items; size_t count; size_t capacity; } metric_ctx_t;
typedef struct { char **keys; size_t count; size_t capacity; } seen_set_t;
typedef struct { int fd; line_handler_t handler; void *data; int is_stderr; char buf[STR_BUF_XLARGE]; size_t used; int failed; } stream_reader_t;

static void print_version(void) {
    printf("diffsnap %s (%s)\n", DIFFSNAP_VERSION, BUILD_SHA);
}

static void print_help(const char *progname) {
    printf(
        "Usage: %s [--help] [--version]\n"
        "\n"
        "Create ZFS snapshots based on the amount of data written\n"
        "\n"
        "Files:\n"
        "  Config: %s\n"
        "  Log:    %s\n"
        "  Lock:   %s\n"
        "\n"
        "Config fields, comma separated:\n"
        "  dataset,interval_minutes,retention,prefix,recursive,min_bytes\n"
        "\n"
        "Field notes:\n"
        "  dataset           ZFS dataset name (starts with a letter; no repeated/trailing '/')\n"
        "  interval_minutes  Minutes between snapshots. Intervals carry over hour boundaries and reset at midnight\n"
        "  retention         Number of matching snapshots to keep\n"
        "  prefix            Snapshot prefix using letters, numbers, '_' or '-'. Unique values prevent pruning snapshots from outside diffsnap\n"
        "  recursive         yes or no. Yes: written bytes = parent + children; parent overrides configured child with same prefix\n"
        "  min_bytes         Minimum written bytes before snapshotting. Try 1000000 to limit unwanted snapshots from small metadata changes\n"
        "\n"
        "Example intervals:\n"
        "  50 evaluates at 00:00 00:50 01:40... 23:20 00:00 (not 00:10).\n"
        "  Intervals greater than 1439 only match at midnight.\n"
        "\n"
        "Example config line:\n"
        "  zroot/home,60,24,hourly,no,1000000\n"
        "\n"
        "Example cron line:\n"
        "  * * * * * root /usr/local/sbin/diffsnap\n",
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
    for (size_t i = 0; prefix[i]; i++) {
        if (!isalnum((unsigned char)prefix[i]) && prefix[i] != '_' && prefix[i] != '-') return 0;
    }
    return 1;
}

/* Dataset components may use the ZFS separator '/', but a name must begin
 * with a letter and may not end in, or contain two consecutive, separators.
 * Spaces are intentionally permitted because ZFS permits them in dataset
 * names and the command paths pass names as separate argv entries. */
static int valid_dataset(const char *dataset) {
    if (!isalpha((unsigned char)dataset[0])) return 0;
    for (size_t i = 0; dataset[i]; i++) {
        unsigned char c = (unsigned char)dataset[i];
        if (!isalnum(c) && c != '_' && c != '.' && c != ':' && c != ' ' && c != '-' && c != '/') return 0;
        if (c == '/' && (dataset[i + 1] == '\0' || dataset[i + 1] == '/')) return 0;
    }
    return 1;
}

static int date_stamp_like(const char *s) {
    static const int digit_pos[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
    size_t len = strlen(s);
    if (len != 19 && len != 24) return 0;
    if (s[4] != '-' || s[7] != '-' || s[10] != '_' || s[13] != ':' || s[16] != ':') return 0;
    for (size_t i = 0; i < sizeof(digit_pos) / sizeof(digit_pos[0]); i++) {
        if (!isdigit((unsigned char)s[digit_pos[i]])) return 0;
    }
    if (len == 24 && ((s[19] != 'p' && s[19] != '-') ||
                      !isdigit((unsigned char)s[20]) || !isdigit((unsigned char)s[21]) ||
                      !isdigit((unsigned char)s[22]) || !isdigit((unsigned char)s[23]))) return 0;
    return 1;
}

static void log_msg(const char *fmt, ...) {
    if (!log_fp) return;
    va_list args;
    va_start(args, fmt);
    time_t t = time(NULL);
    struct tm tm_info;
    char timestamp[STR_BUF_SMALL];
    
    /* Either failure (can't get the local time, or can't format it) falls
     * back to a placeholder timestamp. When it's specifically localtime_r
     * that failed, that's surfaced with an inline marker on THIS SAME
     * line -- not as a second, separate log_fp write -- so an operator
     * still sees it was localtime_r that broke without log_msg() ever
     * doubling its own output for one call. */
    int localtime_failed = 0, strftime_failed = 0;
    if (localtime_r(&t, &tm_info) == NULL) {
        localtime_failed = 1;
        snprintf(timestamp, sizeof(timestamp), "unknown-time");
    } else if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_info) == 0) {
        strftime_failed = 1;
        snprintf(timestamp, sizeof(timestamp), "unknown-time");
    }
    if (fprintf(log_fp, "%s ", timestamp) < 0) log_io_failed = 1;
    if (localtime_failed) {
        if (fprintf(log_fp, "[localtime_r failed, using fallback timestamp] ") < 0) log_io_failed = 1;
    } else if (strftime_failed) {
        if (fprintf(log_fp, "[strftime failed, using fallback timestamp] ") < 0) log_io_failed = 1;
    }
    if (vfprintf(log_fp, fmt, args) < 0) log_io_failed = 1;
    if (fprintf(log_fp, "\n") < 0) log_io_failed = 1;
    va_end(args);
}

/* These formats are shared by all snapshot construction and recursive
 * coverage lookups, respectively, so their separators stay synchronized. */
static int format_snapshot_name(char *dst, size_t dst_size, const char *dataset,
                                const char *prefix, const char *timestamp) {
    return snprintf(dst, dst_size, "%s@%s_%s", dataset, prefix, timestamp);
}

static void format_dataset_prefix_key(char *dst, size_t dst_size,
                                      const char *dataset, const char *prefix) {
    (void)snprintf(dst, dst_size, "%s\x1f%s", dataset, prefix);
}

/* Whether any write to the log file has failed since it was opened. */
static int log_had_io_failure(void) { return log_io_failed; }

static void early_fail(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void format_bytes(long long bytes, char *buf, size_t buf_size) {
    if (bytes <= 0) {
        snprintf(buf, buf_size, "0");
        return;
    }
    const char *units[] = {"", "K", "M", "G", "T", "P", "E"};
    int i = 0;
    double d_bytes = (double)bytes;
    while (d_bytes >= 1024.0 && i < 6) {
        d_bytes /= 1024.0;
        i++;
    }
    if (i == 0) {
        snprintf(buf, buf_size, "%lld", bytes);
    } else {
        snprintf(buf, buf_size, "%.2f%s", d_bytes, units[i]);
    }
}

static void name_list_free(name_list_t *list) {
    if (!list->names) return;
    for (size_t i = 0; i < list->count; i++) free(list->names[i]);
    free(list->names);
    list->names = NULL; list->count = 0; list->capacity = 0;
}

static int seen_set_add(seen_set_t *set, const char *dataset, const char *prefix) {
    char key[STR_BUF_LARGE + STR_BUF_MED + 2];
    format_dataset_prefix_key(key, sizeof(key), dataset, prefix);
    for (size_t i = 0; i < set->count; i++) {
        if (strcmp(set->keys[i], key) == 0) return 1;
    }
    if (set->count >= set->capacity) {
        size_t new_cap = set->capacity == 0 ? ALLOC_CHUNK_BATCH : set->capacity * 2;
        char **tmp = realloc(set->keys, new_cap * sizeof(*set->keys));
        if (!tmp) return -1;
        set->keys = tmp; set->capacity = new_cap;
    }
    char *k = strdup(key);
    if (!k) return -1;
    set->keys[set->count++] = k;
    return 0;
}

static void seen_set_free(seen_set_t *set) {
    for (size_t i = 0; i < set->count; i++) free(set->keys[i]);
    free(set->keys);
    set->keys = NULL; set->count = 0; set->capacity = 0;
}

static void stream_reader_line(stream_reader_t *reader) {
    reader->buf[reader->used] = '\0';
    trim_trailing_whitespace(reader->buf);
    if (reader->is_stderr) {
        if (reader->buf[0] != '\0') log_msg("Error: zfs: %s", reader->buf);
    } else if (!reader->failed && reader->handler(reader->buf, reader->data) != 0) {
        reader->failed = 1;
    }
    reader->used = 0;
}

static void stream_reader_consume(stream_reader_t *reader, const char *buf, ssize_t len) {
    for (ssize_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            stream_reader_line(reader);
        } else if (reader->used < sizeof(reader->buf) - 1) {
            reader->buf[reader->used++] = buf[i];
        } else {
            /* Mark failed *before* flushing, so a truncated (overlong)
             * line is never handed to handler() -- only stderr lines
             * (which are just logged, not parsed) still get flushed
             * after this point. */
            if (!reader->is_stderr) reader->failed = 1;
            stream_reader_line(reader);
            reader->buf[reader->used++] = buf[i];
        }
    }
}

static int drain_command_streams(stream_reader_t *out_reader, stream_reader_t *err_reader) {
    int out_open = out_reader && out_reader->fd >= 0;
    int err_open = err_reader && err_reader->fd >= 0;
    while (out_open || err_open) {
        struct pollfd pfds[2];
        stream_reader_t *pfd_reader[2];
        nfds_t npfds = 0;
        if (out_open) {
            pfd_reader[npfds] = out_reader;
            pfds[npfds].fd = out_reader->fd;
            pfds[npfds].events = POLLIN;
            pfds[npfds].revents = 0;
            npfds++;
        }
        if (err_open) {
            pfd_reader[npfds] = err_reader;
            pfds[npfds].fd = err_reader->fd;
            pfds[npfds].events = POLLIN;
            pfds[npfds].revents = 0;
            npfds++;
        }
        if (poll(pfds, npfds, -1) == -1) {
            if (errno == EINTR) continue;
            if (out_open) {
                out_reader->failed = 1;
                close(out_reader->fd);
                out_reader->fd = -1;
                out_open = 0;
            }
            if (err_open) {
                err_reader->failed = 1;
                close(err_reader->fd);
                err_reader->fd = -1;
                err_open = 0;
            }
            break;
        }
        for (nfds_t i = 0; i < npfds; i++) {
            stream_reader_t *reader = pfd_reader[i];
            /* POLLIN, POLLHUP, and POLLERR all mean "read() won't block":
             * a hangup or error still needs a read() call to observe EOF
             * or the error, same as select() waking us on a dead fd. */
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char buf[STR_BUF_XLARGE];
            ssize_t nread = read(reader->fd, buf, sizeof(buf));
            if (nread > 0) {
                stream_reader_consume(reader, buf, nread);
            } else {
                if (nread < 0 && errno == EINTR) continue;
                if (nread < 0) reader->failed = 1;
                if (reader->used > 0) stream_reader_line(reader);
                close(reader->fd);
                reader->fd = -1;
                if (reader == out_reader) out_open = 0;
                else err_open = 0;
            }
        }
    }
    return ((out_reader && out_reader->failed) || (err_reader && err_reader->failed)) ? -1 : 0;
}

typedef struct { int processing_rc; int child_exited; int child_status; int wait_failed; } exec_result_t;

static exec_result_t exec_cmd_stream_core(const char *const argv[], line_handler_t handler, void *data) {
    exec_result_t result = {0};
    int out_pfd[2] = {-1, -1}, err_pfd[2] = {-1, -1};
    if (handler && pipe2(out_pfd, O_CLOEXEC) == -1) { result.processing_rc = -1; return result; }
    if (pipe2(err_pfd, O_CLOEXEC) == -1) {
        if (handler) { close(out_pfd[0]); close(out_pfd[1]); }
        result.processing_rc = -1; return result;
    }
    pid_t pid = fork();
    if (pid == -1) {
        if (handler) { close(out_pfd[0]); close(out_pfd[1]); }
        close(err_pfd[0]); close(err_pfd[1]);
        result.processing_rc = -1; return result;
    }
    if (pid == 0) {
        if (handler) {
            if (dup2(out_pfd[1], STDOUT_FILENO) == -1) _exit(EXIT_EXEC_FAILED);
            close(out_pfd[0]); close(out_pfd[1]);
        } else {
            int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (devnull == -1 || dup2(devnull, STDOUT_FILENO) == -1) _exit(EXIT_EXEC_FAILED);
            close(devnull);
        }
        if (dup2(err_pfd[1], STDERR_FILENO) == -1) _exit(EXIT_EXEC_FAILED);
        close(err_pfd[0]); close(err_pfd[1]);
        execv(argv[0], (char *const *)argv);
        _exit(EXIT_EXEC_FAILED);
    }
    if (handler) close(out_pfd[1]);
    close(err_pfd[1]);
    stream_reader_t out_reader = { .fd = handler ? out_pfd[0] : -1, .handler = handler, .data = data, .is_stderr = 0, .used = 0, .failed = 0 };
    stream_reader_t err_reader = { .fd = err_pfd[0], .handler = NULL, .data = NULL, .is_stderr = 1, .used = 0, .failed = 0 };
    result.processing_rc = drain_command_streams(handler ? &out_reader : NULL, &err_reader);
    int status; pid_t wpid;
    do { wpid = waitpid(pid, &status, 0); } while (wpid == -1 && errno == EINTR);
    result.wait_failed = (wpid == -1);
    result.child_exited = (wpid != -1) && WIFEXITED(status);
    result.child_status = result.child_exited ? WEXITSTATUS(status) : -1;
    return result;
}

static int exec_cmd_stream(const char *const argv[], line_handler_t handler, void *data) {
    exec_result_t r = exec_cmd_stream_core(argv, handler, data);
    return (!r.wait_failed && r.processing_rc == 0 && r.child_exited && r.child_status == 0) ? 0 : -1;
}

/*
 * Like exec_cmd_stream, but a nonzero child exit status is NOT treated as
 * failure -- only internal plumbing failures (pipe/fork/handler) and
 * abnormal termination (signal, failure to reap) are. Used for `zfs get`
 * calls scoped to specific dataset roots: one bad/nonexistent root makes
 * the whole `zfs get` invocation exit nonzero even though it still printed
 * valid, usable output for every other root. Rejecting the whole result in
 * that case would turn "one typo logged" into "entire run aborted" --
 * worse than the unscoped call's behavior it's meant to optimize.
 * Command-level failures are tolerated only when their stdout remains valid
 * and parseable; malformed stdout is a protocol violation that causes the
 * command to fail through its line handler.
 */
static int exec_cmd_stream_lenient(const char *const argv[], line_handler_t handler, void *data) {
    exec_result_t r = exec_cmd_stream_core(argv, handler, data);
    if (r.wait_failed || r.processing_rc != 0 || !r.child_exited) return -1;
    /*
     * EXIT_EXEC_FAILED is our own sentinel for "execv never ran the target
     * at all" (bad ZFS_PATH, permissions, etc.) -- a total failure, not a
     * per-root lookup miss. Treating it as lenient-success would silently
     * accept zero metrics and make every due dataset get logged as
     * "Configured dataset not found" instead of one clear top-level error.
     * If ZFS ever adopts exit status 127 itself, this convention would
     * mistakenly classify that genuine ZFS failure as an execv failure.
     */
    if (r.child_status == EXIT_EXEC_FAILED) return -1;
    return 0;
}

static int handle_metric_line(const char *line, void *data) {
    metric_ctx_t *ctx = (metric_ctx_t *)data;
    char line_copy[STR_BUF_XLARGE];
    int line_len = snprintf(line_copy, sizeof(line_copy), "%s", line);
    if (line_len < 0 || (size_t)line_len >= sizeof(line_copy)) {
        log_msg("Error: Skipping oversized metric line");
        return -1;
    }
    char *saveptr = NULL;
    char *name = strtok_r(line_copy, "\t", &saveptr);
    char *value = strtok_r(NULL, "\t", &saveptr);
    if (!name || !value) return 0;
    if (strlen(name) >= sizeof(ctx->items[0].name)) {
        log_msg("Error: Skipping metric line with oversized dataset name: %s", name);
        return 0;
    }
    if (ctx->count >= ctx->capacity) {
        size_t new_cap = ctx->capacity == 0 ? ALLOC_CHUNK_METRIC : ctx->capacity * 2;
        metric_item_t *tmp = realloc(ctx->items, new_cap * sizeof(metric_item_t));
        if (!tmp) return -1;
        ctx->items = tmp; ctx->capacity = new_cap;
    }
    strcpy(ctx->items[ctx->count].name, name);
    char *endptr; errno = 0;
    long long val = strtoll(value, &endptr, 10);
    ctx->items[ctx->count].written = (errno == ERANGE || *endptr != '\0' || val < 0) ? -1 : val;
    ctx->count++; return 0;
}

static int compare_metrics(const void *a, const void *b) { return strcmp(((metric_item_t *)a)->name, ((metric_item_t *)b)->name); }

/* Forward declaration: defined later in the file, needed here by
 * sum_subtree_written(). */
static int is_strict_descendant(const char *child, const char *parent);

/*
 * Sums `written` for `dataset` itself plus every strict descendant present
 * in the sorted metrics array. Uses the same is_strict_descendant()
 * name/child boundary check used elsewhere, not a bare prefix match, so a
 * sibling like "tank/data2" is never mistaken for a child of "tank/data".
 *
 * The root dataset (the one actually named in the config) must have a
 * valid written value or the whole entry is rejected -- same as the
 * non-recursive path. A descendant with an invalid (-1) written value is
 * logged and excluded from the sum rather than voiding the whole subtree,
 * since one unreadable child shouldn't suppress a legitimate recursive
 * snapshot triggered by activity elsewhere in the tree.
 *
 * Returns 0 and sets *out_sum on success; -1 if the root itself is
 * missing/invalid.
 * `zfs get` cannot emit duplicate dataset names in one invocation, so the
 * metrics array needs no deduplication before its binary searches.
 */
static int sum_subtree_written(const metric_ctx_t *metrics, const char *dataset, long long *out_sum) {
    metric_item_t key = {0};
    assert(strlen(dataset) < sizeof(key.name));
    memcpy(key.name, dataset, strlen(dataset) + 1);
    metric_item_t *root = metrics->count ?
        bsearch(&key, metrics->items, metrics->count, sizeof(metric_item_t), compare_metrics) : NULL;
    if (!root || root->written == -1) return -1;
    long long sum = root->written;

    char prefix_key[STR_BUF_LARGE + 1];
    int n = snprintf(prefix_key, sizeof(prefix_key), "%s/", dataset);
    if (n < 0 || (size_t)n >= sizeof(prefix_key)) return -1;

    size_t lo = 0, hi = metrics->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(metrics->items[mid].name, prefix_key) < 0) lo = mid + 1;
        else hi = mid;
    }
    for (size_t i = lo; i < metrics->count; i++) {
        if (!is_strict_descendant(metrics->items[i].name, dataset)) break;
        if (metrics->items[i].written == -1) {
            log_msg("Error: Invalid written metric for %s, excluding from recursive total for %s",
                     metrics->items[i].name, dataset);
            continue;
        }
        sum += metrics->items[i].written;
    }
    *out_sum = sum;
    return 0;
}

/*
 * Applies the metrics-based filtering that used to happen inline during
 * config parsing (found/invalid/min_bytes checks), now deferred until
 * after metrics have actually been fetched -- which itself is deferred
 * until after parsing, so it can be skipped or scoped based on what's
 * actually due this run. Removes items that fail the check; keeps and
 * caches .written for items that pass, exactly as batch_add used to.
 *
 * When `recursive` is set, .written is the sum of the named dataset's own
 * written bytes plus every strict descendant's (see sum_subtree_written),
 * so min_bytes reflects activity anywhere in the subtree -- not just on
 * the named dataset itself.
 */
static void batch_filter_by_metrics(batch_ctx_t *ctx, const metric_ctx_t *metrics, int recursive, int *global_status) {
    size_t write_idx = 0;
    for (size_t i = 0; i < ctx->count; i++) {
        long long written = 0;
        int ok;
        if (recursive) {
            ok = (sum_subtree_written(metrics, ctx->items[i].dataset, &written) == 0);
        } else {
            metric_item_t key = {0};
            assert(strlen(ctx->items[i].dataset) < sizeof(key.name));
            memcpy(key.name, ctx->items[i].dataset, strlen(ctx->items[i].dataset) + 1);
            metric_item_t *found = metrics->count ?
                bsearch(&key, metrics->items, metrics->count, sizeof(metric_item_t), compare_metrics) : NULL;
            ok = found && found->written != -1;
            if (ok) written = found->written;
        }
        int keep;
        if (!ok) {
            log_msg("Error: %sdataset not found or has invalid written metric: %s",
                     recursive ? "Configured recursive " : "Configured ", ctx->items[i].dataset);
            *global_status = 1; keep = 0;
        } else if (written < ctx->items[i].min_bytes) {
            keep = 0; /* below threshold: skip silently, same as before */
        } else {
            ctx->items[i].written = written;
            keep = 1;
        }
        if (keep) {
            if (write_idx != i) ctx->items[write_idx] = ctx->items[i];
            write_idx++;
        } else {
            free(ctx->items[i].dataset);
            free(ctx->items[i].prefix);
        }
    }
    ctx->count = write_idx;
}
static int zfs_destroy(const char *snap_name, int recursive) {
    if (recursive) {
        const char *const argv[] = {ZFS_PATH, "destroy", "-r", snap_name, NULL};
        return exec_cmd_stream(argv, NULL, NULL);
    }
    const char *const argv[] = {ZFS_PATH, "destroy", snap_name, NULL};
    return exec_cmd_stream(argv, NULL, NULL);
}

/*
 * Prunes snapshots for a single dataset/prefix using a snapshot inventory that
 * was already fetched once (in creation-descending order, newest first) for
 * the whole run. `matches` is scratch space owned by the caller and reused
 * across calls so we don't malloc/free per dataset. Entries in `matches` are
 * borrowed pointers into `inventory`; they must not be freed here.
 */
static int prune_from_inventory(const name_list_t *inventory, const char *dataset, const char *prefix,
                                 size_t max_snaps, int recursive, char ***matches, size_t *matches_cap) {
    char match_str[STR_BUF_LARGE];
    int match_rc = snprintf(match_str, sizeof(match_str), "%s_", prefix);
    if (match_rc < 0 || (size_t)match_rc >= sizeof(match_str)) return -1;
    size_t match_len = (size_t)match_rc;
    size_t dataset_len = strlen(dataset);

    size_t match_count = 0;
    for (size_t i = 0; i < inventory->count; i++) {
        const char *line = inventory->names[i];
        const char *at = strchr(line, '@');
        if (!at) continue;
        if ((size_t)(at - line) != dataset_len || strncmp(line, dataset, dataset_len) != 0) continue;
        if (strncmp(at + 1, match_str, match_len) != 0) continue;
        if (!date_stamp_like(at + 1 + match_len)) continue;
        if (match_count >= *matches_cap) {
            size_t new_cap = *matches_cap == 0 ? ALLOC_CHUNK_PRUNE : *matches_cap * 2;
            char **tmp = realloc(*matches, new_cap * sizeof(char *));
            if (!tmp) {
                log_msg("Error: Failed to allocate prune match list for %s", dataset);
                return -1;
            }
            *matches = tmp; *matches_cap = new_cap;
        }
        (*matches)[match_count++] = (char *)line;
    }
    int prune_errors = 0;
    while (match_count > max_snaps) {
        char *oldest = (*matches)[--match_count];
        if (zfs_destroy(oldest, recursive) == 0) log_msg("Pruned=%s%s", oldest, recursive ? " Recursive" : "");
        else { log_msg("Error: Failed to prune snapshot %s", oldest); prune_errors++; }
    }
    return (prune_errors == 0) ? 0 : -1;
}

static int batch_add(batch_ctx_t *ctx, const char *dataset, const char *prefix, size_t retention, long long written, long long min_bytes) {
    if (ctx->count >= ctx->capacity) {
        size_t new_cap = ctx->capacity == 0 ? ALLOC_CHUNK_BATCH : ctx->capacity * 2;
        batch_item_t *tmp = realloc(ctx->items, new_cap * sizeof(batch_item_t));
        if (!tmp) return -1;
        ctx->items = tmp; ctx->capacity = new_cap;
    }
    char *d = strdup(dataset), *p = strdup(prefix);
    if (!d || !p) { free(d); free(p); return -1; }
    ctx->items[ctx->count++] = (batch_item_t){ .dataset = d, .prefix = p, .retention = retention, .pass = 0, .snap_failed = 0, .written = written, .min_bytes = min_bytes };
    return 0;
}

static void batch_free(batch_ctx_t *ctx) {
    for (size_t i = 0; i < ctx->count; i++) { free(ctx->items[i].dataset); free(ctx->items[i].prefix); }
    free(ctx->items);
}

static size_t zfs_root_len(const char *dataset) {
    const char *slash = strchr(dataset, '/');
    return slash ? (size_t)(slash - dataset) : strlen(dataset);
}

static int same_zfs_root(const char *a, const char *b) {
    size_t a_len = zfs_root_len(a), b_len = zfs_root_len(b);
    return a_len == b_len && strncmp(a, b, a_len) == 0;
}

static int same_zfs_dataset(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

typedef struct { char **roots; size_t count; size_t capacity; } root_list_t;

static void root_list_free(root_list_t *list) {
    for (size_t i = 0; i < list->count; i++) free(list->roots[i]);
    free(list->roots);
    list->roots = NULL; list->count = 0; list->capacity = 0;
}

static int root_list_add_unique(root_list_t *list, const char *dataset) {
    size_t len = zfs_root_len(dataset);
    for (size_t i = 0; i < list->count; i++) {
        if (strlen(list->roots[i]) == len && strncmp(list->roots[i], dataset, len) == 0) return 0;
    }
    if (list->count >= list->capacity) {
        size_t new_cap = list->capacity == 0 ? ALLOC_CHUNK_BATCH : list->capacity * 2;
        char **tmp = realloc(list->roots, new_cap * sizeof(char *));
        if (!tmp) return -1;
        list->roots = tmp; list->capacity = new_cap;
    }
    char *r = malloc(len + 1);
    if (!r) return -1;
    memcpy(r, dataset, len);
    r[len] = '\0';
    list->roots[list->count++] = r;
    return 0;
}

/* Collects the distinct dataset roots referenced by both batches (i.e. the
 * roots that are actually due this run), for scoping the `zfs get written`
 * call to just what's needed instead of the whole pool. */
static int collect_due_roots(root_list_t *list, const batch_ctx_t *b1, const batch_ctx_t *b2) {
    for (size_t i = 0; i < b1->count; i++) if (root_list_add_unique(list, b1->items[i].dataset) != 0) return -1;
    for (size_t i = 0; i < b2->count; i++) if (root_list_add_unique(list, b2->items[i].dataset) != 0) return -1;
    return 0;
}

static void batch_assign_duplicate_passes(batch_ctx_t *ctx) {
    for (size_t i = 0; i < ctx->count; i++) {
        size_t pass = 0;
        for (size_t k = 0; k < i; k++) {
            if (same_zfs_dataset(ctx->items[k].dataset, ctx->items[i].dataset)) pass++;
        }
        ctx->items[i].pass = pass;
    }
}

static int is_strict_descendant(const char *child, const char *parent) {
    size_t plen = strlen(parent);
    return strncmp(child, parent, plen) == 0 && child[plen] == '/';
}

typedef struct { size_t idx; size_t len; } order_entry_t;

static int compare_order_entry(const void *a, const void *b) {
    const order_entry_t *ea = (const order_entry_t *)a, *eb = (const order_entry_t *)b;
    if (ea->len != eb->len) return (ea->len < eb->len) ? -1 : 1;
    return 0;
}

static int resolve_recursive_ancestor_overlaps(batch_ctx_t *rec_b) {
    if (rec_b->count == 0) return 0;
    int *covered = calloc(rec_b->count, sizeof(int));
    if (!covered) return -1;
    for (size_t j = 0; j < rec_b->count; j++) {
        for (size_t i = 0; i < rec_b->count; i++) {
            if (i == j) continue;
            if (strcmp(rec_b->items[i].prefix, rec_b->items[j].prefix) == 0 &&
                is_strict_descendant(rec_b->items[j].dataset, rec_b->items[i].dataset)) {
                covered[j] = 1;
                break;
            }
        }
    }
    size_t write_idx = 0;
    for (size_t j = 0; j < rec_b->count; j++) {
        if (covered[j]) {
            log_msg("Skipping %s: already covered by a recursive ancestor with prefix '%s'",
                     rec_b->items[j].dataset, rec_b->items[j].prefix);
            free(rec_b->items[j].dataset);
            free(rec_b->items[j].prefix);
        } else {
            if (write_idx != j) rec_b->items[write_idx] = rec_b->items[j];
            write_idx++;
        }
    }
    /* write_idx (and therefore rec_b->count below) can never reach 0 here.
     * is_strict_descendant() only marks an item covered when some *other*
     * item in the set is a strict ancestor, which requires a strictly
     * shorter dataset name. We entered with rec_b->count > 0, so the
     * item(s) with the minimum dataset-name length in the set have no
     * candidate ancestor (nothing in the set is shorter, and equal-length
     * items can never be ancestor/descendant of each other) -- they can
     * never be marked covered. At least one item therefore always
     * survives, so rec_b->count is always >= 1 past this point, and the
     * `malloc(rec_b->count * sizeof(order_entry_t))` below is never a
     * malloc(0). */
    rec_b->count = write_idx;
    free(covered);
    batch_assign_duplicate_passes(rec_b);
    order_entry_t *order = malloc(rec_b->count * sizeof(order_entry_t));
    if (!order) return -1;
    for (size_t i = 0; i < rec_b->count; i++) {
        order[i].idx = i;
        order[i].len = strlen(rec_b->items[i].dataset);
    }
    qsort(order, rec_b->count, sizeof(order_entry_t), compare_order_entry);
    for (size_t oi = 0; oi < rec_b->count; oi++) {
        size_t j = order[oi].idx;
        int collision;
        do {
            collision = 0;
            for (size_t oa = 0; oa < oi; oa++) {
                size_t i = order[oa].idx;
                if (is_strict_descendant(rec_b->items[j].dataset, rec_b->items[i].dataset) &&
                    rec_b->items[i].pass == rec_b->items[j].pass) {
                    rec_b->items[j].pass++;
                    collision = 1;
                    break;
                }
            }
        } while (collision);
    }
    free(order);
    return 0;
}

static int batch_item_in_root_pass(const batch_ctx_t *ctx, size_t item_idx, const char *root_dataset, size_t pass) {
    return same_zfs_root(ctx->items[item_idx].dataset, root_dataset) && ctx->items[item_idx].pass == pass;
}

static size_t batch_root_pass_count(const batch_ctx_t *ctx, const char *root_dataset) {
    size_t pass_count = 0;
    for (size_t i = 0; i < ctx->count; i++) {
        if (!same_zfs_root(ctx->items[i].dataset, root_dataset)) continue;
        size_t item_pass = ctx->items[i].pass + 1;
        if (item_pass > pass_count) pass_count = item_pass;
    }
    return pass_count;
}

static void batch_mark_indices_failed(batch_ctx_t *ctx, const size_t *indices, size_t count) {
    for (size_t k = 0; k < count; k++) ctx->items[indices[k]].snap_failed = 1;
}

/*
 * Executes one `zfs snapshot [-r] ds1@snap ds2@snap ...` call covering
 * exactly the items referenced by `indices` (a subset of one root+pass that
 * fits under ARGV_BYTES_CAP). On failure, only these items are marked
 * snap_failed -- not the whole root+pass -- since a sibling chunk that
 * already succeeded (or hasn't run yet) shouldn't be penalized for this
 * chunk's failure.
 */
static int zfs_snapshot_exec_chunk(batch_ctx_t *ctx, int recursive, const char *timestamp,
                                    const size_t *indices, size_t chunk_count, size_t chunk_bytes) {
    size_t total_args = (recursive ? 3 : 2) + chunk_count + 1;
    const char **argv = malloc(total_args * sizeof(char *));
    if (!argv) { batch_mark_indices_failed(ctx, indices, chunk_count); return -1; }
    size_t idx = 0; argv[idx++] = ZFS_PATH; argv[idx++] = "snapshot";
    if (recursive) argv[idx++] = "-r";
    char *arena = malloc(chunk_bytes), *offset = arena;
    if (!arena) { free(argv); batch_mark_indices_failed(ctx, indices, chunk_count); return -1; }
    for (size_t k = 0; k < chunk_count; k++) {
        size_t i = indices[k];
        size_t remaining = chunk_bytes - (size_t)(offset - arena);
        int written = format_snapshot_name(offset, remaining, ctx->items[i].dataset,
                                           ctx->items[i].prefix, timestamp);
        if (written < 0 || (size_t)written >= remaining) { free(arena); free(argv); batch_mark_indices_failed(ctx, indices, chunk_count); return -1; }
        argv[idx++] = offset; offset += written + 1;
    }
    argv[idx] = NULL;
    int rc = exec_cmd_stream(argv, NULL, NULL);
    if (rc != 0) batch_mark_indices_failed(ctx, indices, chunk_count);
    free(arena); free(argv); return rc;
}

/*
 * Snapshots every item in this root+pass, splitting into as many
 * `zfs snapshot` invocations as needed to keep each one's total argument
 * bytes under ARGV_BYTES_CAP. Items are packed greedily in their existing
 * order; a chunk always contains at least one item even if that single
 * item's own bytes exceed the cap (can't split further, and in practice
 * unreachable given the ~800-byte ceiling on one snapshot name).
 */
static int zfs_snapshot_batch_root_pass(batch_ctx_t *ctx, int recursive, const char *timestamp, const char *root_dataset, size_t pass) {
    size_t *indices = NULL, count = 0, capacity = 0;
    for (size_t i = 0; i < ctx->count; i++) {
        if (!batch_item_in_root_pass(ctx, i, root_dataset, pass)) continue;
        if (count >= capacity) {
            size_t new_cap = capacity == 0 ? ALLOC_CHUNK_BATCH : capacity * 2;
            size_t *tmp = realloc(indices, new_cap * sizeof(size_t));
            if (!tmp) {
                /* The whole root+pass attempt is abandoned here: items
                 * already collected in `indices` were never passed to
                 * zfs_snapshot_exec_chunk, AND every remaining item from i
                 * onward that belongs to this root+pass is never even
                 * visited by the rest of this loop (we return immediately).
                 * Both groups must be marked snap_failed, or the unvisited
                 * ones silently keep snap_failed==0 and finalize_batch logs
                 * a false "Created=" line and prunes real snapshots on that
                 * premise -- exactly like the already-collected ones would
                 * without the first part of this fix. */
                batch_mark_indices_failed(ctx, indices, count);
                for (size_t j = i; j < ctx->count; j++) {
                    if (batch_item_in_root_pass(ctx, j, root_dataset, pass)) ctx->items[j].snap_failed = 1;
                }
                free(indices);
                return -1;
            }
            indices = tmp; capacity = new_cap;
        }
        indices[count++] = i;
    }
    if (count == 0) { free(indices); return 0; }

    int status = 0;
    size_t start = 0;
    while (start < count) {
        size_t chunk_bytes = 0, end = start;
        while (end < count) {
            size_t i = indices[end];
            size_t item_bytes = strlen(ctx->items[i].dataset) + strlen(ctx->items[i].prefix) + strlen(timestamp) + 3;
            if (end > start && chunk_bytes + item_bytes > ARGV_BYTES_CAP) break;
            chunk_bytes += item_bytes;
            end++;
        }
        size_t chunk_count = end - start;
        if (zfs_snapshot_exec_chunk(ctx, recursive, timestamp, &indices[start], chunk_count, chunk_bytes) != 0) status = -1;
        start = end;
    }
    free(indices);
    return status;
}

static int zfs_snapshot_batch(batch_ctx_t *ctx, int recursive, const char *timestamp) {
    if (ctx->count == 0) return 0;
    int status = 0;
    for (size_t i = 0; i < ctx->count; i++) {
        int seen_root = 0;
        for (size_t j = 0; j < i; j++) {
            if (same_zfs_root(ctx->items[i].dataset, ctx->items[j].dataset)) {
                seen_root = 1;
                break;
            }
        }
        if (seen_root) continue;
        /* A pass contains no duplicate target dataset and no recursive
         * ancestor/descendant overlap. ZFS rejects both combinations in one
         * invocation 'cannot create snapshots : multiple snapshots of same
         * fs not allowed' so multiple passes are required. */
        size_t pass_count = batch_root_pass_count(ctx, ctx->items[i].dataset);
        for (size_t pass = 0; pass < pass_count; pass++)
            if (zfs_snapshot_batch_root_pass(ctx, recursive, timestamp, ctx->items[i].dataset, pass) != 0) status = -1;
    }
    return status;
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

/*
 * Loads every snapshot relevant to this run exactly once, covering both the
 * standard and recursive batches together (items 5 and 6). Sorted newest-
 * first by creation time, matching what per-item pruning previously relied
 * on from its own individual `zfs list -S creation` call. This single
 * listing is reused both to verify snapshots that zfs_snapshot_batch flagged
 * as failed, and to drive pruning for every item in both batches.
 */
static int load_combined_snapshot_inventory(name_list_t *list, const batch_ctx_t *std_b, const batch_ctx_t *rec_b) {
    size_t total = std_b->count + rec_b->count;
    if (total == 0) return 0;
    root_list_t roots = { NULL, 0, 0 };
    if (collect_due_roots(&roots, std_b, rec_b) != 0) {
        root_list_free(&roots);
        return -1;
    }
    size_t roots_bytes = 0;
    for (size_t i = 0; i < roots.count; i++) roots_bytes += strlen(roots.roots[i]) + 1;

    /* Match the scoped `zfs get` strategy: list every relevant root in one
     * command, unless the root arguments alone would exceed our argv budget.
     * The fallback deliberately omits all roots and lists every pool. */
    int use_scoped = (roots_bytes <= ARGV_BYTES_CAP);
    size_t fixed_argc = use_scoped ? 10 : 9; /* "-r" only when roots are supplied */
    size_t argc = fixed_argc + (use_scoped ? roots.count : 0) + 1;
    const char **argv = malloc(argc * sizeof(*argv));
    if (!argv) { root_list_free(&roots); return -1; }
    size_t idx = 0;
    argv[idx++] = ZFS_PATH; argv[idx++] = "list"; argv[idx++] = "-H";
    if (use_scoped) argv[idx++] = "-r";
    argv[idx++] = "-t"; argv[idx++] = "snapshot"; argv[idx++] = "-o";
    argv[idx++] = "name"; argv[idx++] = "-S"; argv[idx++] = "creation";
    if (use_scoped) for (size_t i = 0; i < roots.count; i++) argv[idx++] = roots.roots[i];
    argv[idx] = NULL;
    /* Unlike the pre-validation metrics fetch, this post-validation inventory
     * is intentionally strict: every requested root must be listable. */
    int rc = exec_cmd_stream(argv, handle_snapshot_inventory_line, list);
    free(argv);
    root_list_free(&roots);
    if (rc != 0) {
        name_list_free(list);
        return -1;
    }
    return 0;
}

static int is_recursively_covered(const char *dataset, const char *prefix, char **rec_keys, size_t rec_count) {
    char candidate[STR_BUF_LARGE];
    if (copy_token(candidate, sizeof(candidate), dataset) != 0) return -1;
    for (;;) {
        char key[STR_BUF_LARGE + STR_BUF_MED + 2];
        format_dataset_prefix_key(key, sizeof(key), candidate, prefix);
        if (name_index_contains(rec_keys, rec_count, key)) return 1;
        char *slash = strrchr(candidate, '/');
        if (!slash) return 0;
        *slash = '\0';
    }
}

static int remove_recursive_overlaps(batch_ctx_t *std_b, const batch_ctx_t *rec_b) {
    if (std_b->count == 0 || rec_b->count == 0) return 0;
    char **rec_keys = malloc(rec_b->count * sizeof(*rec_keys));
    if (!rec_keys) return -1;
    size_t built = 0;
    for (size_t i = 0; i < rec_b->count; i++) {
        char key[STR_BUF_LARGE + STR_BUF_MED + 2];
        format_dataset_prefix_key(key, sizeof(key), rec_b->items[i].dataset, rec_b->items[i].prefix);
        if ((rec_keys[built] = strdup(key)) == NULL) {
            for (size_t j = 0; j < built; j++) free(rec_keys[j]);
            free(rec_keys);
            return -1;
        }
        built++;
    }
    qsort(rec_keys, built, sizeof(*rec_keys), compare_names);
    size_t write_idx = 0;
    for (size_t i = 0; i < std_b->count; i++) {
        int covered = is_recursively_covered(std_b->items[i].dataset, std_b->items[i].prefix, rec_keys, built);
        if (covered < 0) {
            for (size_t j = 0; j < built; j++) free(rec_keys[j]);
            free(rec_keys);
            return -1;
        }
        if (covered) {
            log_msg("Skipping %s: covered by a recursive ancestor with prefix '%s'",
                     std_b->items[i].dataset, std_b->items[i].prefix);
            free(std_b->items[i].dataset);
            free(std_b->items[i].prefix);
        } else {
            if (write_idx != i) std_b->items[write_idx] = std_b->items[i];
            write_idx++;
        }
    }
    std_b->count = write_idx;
    for (size_t i = 0; i < built; i++) free(rec_keys[i]);
    free(rec_keys);
    return 0;
}

/* Plain linear scan over the (unsorted) inventory, used only for verifying
 * items flagged snap_failed -- a rare case, so there's no need for the
 * sorted-copy/bsearch machinery a hot path would justify. Same style as
 * the scan in prune_from_inventory. */
static int inventory_contains(const name_list_t *inventory, const char *name) {
    for (size_t i = 0; i < inventory->count; i++) {
        if (strcmp(inventory->names[i], name) == 0) return 1;
    }
    return 0;
}

static int create_batch_snapshots(batch_ctx_t *batch, const char *snap_time, int recursive) {
    int rc = zfs_snapshot_batch(batch, recursive, snap_time);
    if (rc != 0) log_msg("Error: %s zfs snapshot batch execution failed", recursive ? "recursive" : "standard");
    return rc;
}

/*
 * Verifies (for items zfs_snapshot_batch flagged as failed) and prunes every
 * item in `batch`, using an inventory that was already fetched once for the
 * whole run (both std_b and rec_b together) rather than issuing a fresh zfs
 * list call here. Verification is a linear scan over `inventory` (see
 * inventory_contains) -- snap_failed items are rare, so there's no need to
 * build a sorted index for it. `matches`/`matches_cap` is pruning scratch
 * space reused across calls.
 */
static int finalize_batch(batch_ctx_t *batch, const name_list_t *inventory, int inventory_ok,
                           char ***matches, size_t *matches_cap,
                           const char *snap_time, int recursive) {
    int status = 0;
    for (size_t i = 0; i < batch->count; i++) {
        char snap_name[STR_BUF_XLARGE];
        int sn_rc = format_snapshot_name(snap_name, sizeof(snap_name),
                                         batch->items[i].dataset, batch->items[i].prefix, snap_time);
        if (sn_rc < 0 || (size_t)sn_rc >= sizeof(snap_name)) {
            log_msg("Error: Failed to format %ssnapshot name for %s",
                     recursive ? "recursive " : "", batch->items[i].dataset);
            status = 1;
            continue;
        }
        /* Pruning intentionally only considers configured batch items after
         * this run confirms their target snapshot exists; unconfigured
         * datasets and snapshot attempts we could not confirm must not prune
         * existing snapshots. */
        if (batch->items[i].snap_failed) {
            if (!inventory_ok) { log_msg("Error: Unable to verify %ssnapshot exists: %s", recursive ? "recursive " : "", snap_name); continue; }
            if (!inventory_contains(inventory, snap_name)) {
                log_msg("Error: %sSnapshot not created: %s; pruning skipped for dataset '%s' with prefix '%s'",
                        recursive ? "Recursive " : "", snap_name,
                        batch->items[i].dataset, batch->items[i].prefix);
                continue;
            }
        }
        char h_bytes[STR_BUF_SMALL];
        format_bytes(batch->items[i].written, h_bytes, sizeof(h_bytes));
        log_msg("Created=%s Written=%s%s", snap_name, h_bytes, recursive ? " Recursive" : "");
        if (!inventory_ok) {
            log_msg("Error: Unable to prune %ssnapshots for %s: snapshot inventory unavailable", recursive ? "recursive " : "", batch->items[i].dataset);
            status = 1;
            continue;
        }
        if (prune_from_inventory(inventory, batch->items[i].dataset, batch->items[i].prefix,
                                  batch->items[i].retention, recursive, matches, matches_cap) != 0) status = 1;
    }
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

    int lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        int saved_errno = errno;
        early_fail("%s: failed to open lock file %s: %s", progname, LOCK_PATH, strerror(saved_errno));
        return 1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        int saved_errno = errno;
        if (saved_errno == EWOULDBLOCK) {
            early_fail("%s: another instance is already running (lock held on %s)", progname, LOCK_PATH);
        } else {
            early_fail("%s: failed to acquire lock %s: %s", progname, LOCK_PATH, strerror(saved_errno));
        }
        close(lock_fd);
        return 1;
    }
    int global_status = 0, ret_code = 0;
    metric_ctx_t metrics = { NULL, 0, 0 };
    batch_ctx_t std_b = { NULL, 0, 0 }, rec_b = { NULL, 0, 0 };
    seen_set_t seen = { NULL, 0, 0 };
    char *line = NULL; FILE *conf = NULL;
    if ((log_fp = fopen(LOG_PATH, "ae")) == NULL) {
        int saved_errno = errno;
        early_fail("%s: failed to open log file %s: %s", progname, LOG_PATH, strerror(saved_errno));
        ret_code = 1; goto cleanup;
    }
    if (setvbuf(log_fp, NULL, _IOLBF, 0) != 0) {
        fprintf(stderr, "%s: failed to enable line-buffered logging; continuing with fully-buffered logging\n", progname);
    }
    conf = fopen(CONF_PATH, "re");
    if (!conf) {
        int saved_errno = errno;
        early_fail("%s: failed to open config file %s: %s", progname, CONF_PATH, strerror(saved_errno));
        log_msg("Error: failed to open config file %s: %s", CONF_PATH, strerror(saved_errno));
        ret_code = global_status ? global_status : 1; goto cleanup;
    }
    time_t t = time(NULL); struct tm tm_info; 
    if (localtime_r(&t, &tm_info) == NULL) { log_msg("Error: localtime_r failed"); ret_code = 1; goto cleanup; }
    size_t line_cap = 0;
    long long current_day_mins = (tm_info.tm_hour * 60) + tm_info.tm_min;
    while (getline(&line, &line_cap, conf) != -1) {
        trim_trailing_whitespace(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (line[0] == ',' || strstr(line, ",,") != NULL) {
            log_msg("Error: Config error: adjacent comma delimiters");
            global_status = 1;
            continue;
        }
        char dataset[STR_BUF_LARGE] = {0}, prefix[STR_BUF_MED] = {0}, recursive_str[STR_BUF_SMALL] = {0};
        long long interval_mins = 0, retention_val = 0, min_bytes = 0;
        char *endptr, *saveptr = NULL, *token = strtok_r(line, ",", &saveptr);
        if (!token) continue;
        if (copy_token(dataset, sizeof(dataset), token) != 0) { log_msg("Error: Config error: dataset name too long"); global_status = 1; continue; }
        if (!valid_dataset(dataset)) { log_msg("Error: Config error: invalid dataset name '%s'", dataset); global_status = 1; continue; }
        REQUIRE_TOKEN("missing interval field"); errno = 0; interval_mins = strtoll(token, &endptr, 10);
        if (errno == ERANGE || *endptr != '\0' || interval_mins <= 0) {
            log_msg("Error: Config error for %s: invalid interval '%s'", dataset, token); global_status = 1; continue;
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
        if (errno == ERANGE || *endptr != '\0' || min_bytes < 0 || strtok_r(NULL, ",", &saveptr) != NULL) {
            log_msg("Error: Config error for %s: invalid byte threshold or trailing garbage", dataset); global_status = 1; continue;
        }
        int is_recursive;
        if (strcmp(recursive_str, "yes") == 0) is_recursive = 1;
        else if (strcmp(recursive_str, "no") == 0) is_recursive = 0;
        else { log_msg("Error: Config error for %s: invalid recursive flag option '%s'", dataset, recursive_str); global_status = 1; continue; }
        int seen_rc = seen_set_add(&seen, dataset, prefix);
        if (seen_rc == -1) { log_msg("Error: Failed to track config entry for %s", dataset); global_status = 1; continue; }
        if (seen_rc == 1) { log_msg("Error: Config error for %s: duplicate dataset/prefix '%s' in config, skipping", dataset, prefix); global_status = 1; continue; }
        if (current_day_mins % interval_mins != 0) continue;
        if (is_recursive) { if (batch_add(&rec_b, dataset, prefix, (size_t)retention_val, -1, min_bytes) != 0) { log_msg("Error: Failed to allocate batch entry for %s", dataset); global_status = 1; } }
        else { if (batch_add(&std_b, dataset, prefix, (size_t)retention_val, -1, min_bytes) != 0) { log_msg("Error: Failed to allocate batch entry for %s", dataset); global_status = 1; } }
    next_line: ;
    }
    /* getline() may fail before it performs a stream read (for example,
     * while growing line).  Such failures need not set ferror(), so EOF is
     * the only successful reason for the loop to end. */
    int config_read_errno = errno;
    if (!feof(conf)) {
        log_msg("Error: Failed to read config file %s: %s", CONF_PATH, strerror(config_read_errno));
        ret_code = global_status ? global_status : 1; goto cleanup;
    }
    if (std_b.count + rec_b.count > 0) {
        /* Only fetch metrics -- and only for the roots actually due this
         * run -- when something is actually due. Skipping the call
         * entirely when nothing's due, and scoping it to exact due roots
         * when something is, both follow from the same idea: don't pay for
         * `zfs get written` data nothing will use.
         *
         * "Scoped" below means the call adds "-r" plus one dataset
         * argument per due root: `zfs get -r ... written root1 root2 ...`,
         * so ZFS only walks those roots' subtrees. "Unscoped" means the
         * call has no "-r" and no dataset arguments at all, so ZFS
         * defaults to reporting `written` for every dataset in every
         * imported pool on the system -- not "the whole pool" in the
         * sense of one pool, but literally everything. Unscoped is only
         * the fallback used when the due roots' own names wouldn't fit
         * the argv byte budget (see use_scoped below). */
        root_list_t due_roots = { NULL, 0, 0 };
        if (collect_due_roots(&due_roots, &std_b, &rec_b) != 0) {
            log_msg("Error: Failed to determine dataset roots for metrics fetch");
            root_list_free(&due_roots);
            ret_code = global_status ? global_status : 1; goto cleanup;
        }
        size_t roots_bytes = 0;
        for (size_t i = 0; i < due_roots.count; i++) roots_bytes += strlen(due_roots.roots[i]) + 1;
        /* Reuse the same byte budget as snapshot-batch chunking: if the
         * due roots' names alone would risk an overlong command line for
         * the scoped call, fall back to the unscoped call (no "-r", no
         * dataset arguments, queries every dataset) rather than trying to
         * chunk `zfs get` -- there's no less-granular substitute to chunk
         * down to the way there is for `zfs snapshot`, so falling back to
         * the thing that already works is strictly simpler. */
        int use_scoped = (roots_bytes <= ARGV_BYTES_CAP);

        size_t fixed_argc = use_scoped ? 10 : 9; /* "-r" only present when scoped */
        size_t m_argc = fixed_argc + (use_scoped ? due_roots.count : 0) + 1; /* +1 for NULL */
        const char **m_argv = malloc(m_argc * sizeof(char *));
        if (!m_argv) {
            log_msg("Error: Failed to allocate metrics command");
            root_list_free(&due_roots);
            ret_code = global_status ? global_status : 1; goto cleanup;
        }
        size_t idx = 0;
        m_argv[idx++] = ZFS_PATH; m_argv[idx++] = "get"; m_argv[idx++] = "-H"; m_argv[idx++] = "-p";
        if (use_scoped) m_argv[idx++] = "-r";
        m_argv[idx++] = "-t"; m_argv[idx++] = "filesystem,volume";
        m_argv[idx++] = "-o"; m_argv[idx++] = "name,value"; m_argv[idx++] = "written";
        if (use_scoped) for (size_t i = 0; i < due_roots.count; i++) m_argv[idx++] = due_roots.roots[i];
        m_argv[idx] = NULL;

        /* Unlike the post-validation snapshot inventory (which is strict),
         * this pre-validation metrics fetch is intentionally lenient.
         * Scoped calls use the lenient exit-code handling: a single bad
         * root (typo, since-destroyed dataset) makes `zfs get` exit
         * nonzero even though it printed valid output for every other
         * root. The unscoped fallback never names a target that could be
         * wrong, so it keeps the strict check. */
        int fetch_rc = use_scoped ? exec_cmd_stream_lenient(m_argv, handle_metric_line, &metrics)
                                   : exec_cmd_stream(m_argv, handle_metric_line, &metrics);
        free(m_argv);
        root_list_free(&due_roots);
        if (fetch_rc != 0) { log_msg("Error: Failed to read ZFS written metrics"); ret_code = global_status ? global_status : 1; goto cleanup; }
        if (metrics.count > 0) qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        batch_filter_by_metrics(&std_b, &metrics, 0, &global_status);
        batch_filter_by_metrics(&rec_b, &metrics, 1, &global_status);
    }
    if (resolve_recursive_ancestor_overlaps(&rec_b) != 0) { log_msg("Error: Failed to check recursive ancestor overlaps"); ret_code = global_status ? global_status : 1; goto cleanup; }
    if (remove_recursive_overlaps(&std_b, &rec_b) != 0) { log_msg("Error: Failed to check recursive overlaps"); ret_code = global_status ? global_status : 1; goto cleanup; }
    batch_assign_duplicate_passes(&std_b);
    char snap_time[STR_BUF_SMALL];
    /* Local wall-clock time repeats during the autumn DST change. The UTC
     * offset makes those otherwise identical seconds produce distinct names.
     * The spring transition can skip up to an hour of wall-clock intervals;
     * intervals that divide 60 evenly avoid additional hour-boundary skips
     * caused by the scheduler's interval arithmetic. */
    if (strftime(snap_time, sizeof(snap_time), "%Y-%m-%d_%H:%M:%S%z", &tm_info) == 0) { log_msg("Error: Failed to format timestamp"); ret_code = global_status ? global_status : 1; goto cleanup; }
    /* ZFS snapshot names reject '+'. Preserve the offset's sign while
     * encoding a positive one as 'p' (for example, p0500). */
    if (snap_time[19] == '+') snap_time[19] = 'p';
    
    if (create_batch_snapshots(&std_b, snap_time, 0) != 0) global_status = 1;
    if (create_batch_snapshots(&rec_b, snap_time, 1) != 0) global_status = 1;

    /* One shared snapshot listing, fetched after all snapshot creation is
     * done, reused for verification and pruning across both batches. */
    name_list_t inventory = { NULL, 0, 0 };
    int inventory_ok = 1;
    char **prune_matches = NULL;
    size_t prune_matches_cap = 0;

    if (std_b.count > 0 || rec_b.count > 0) {
        if (load_combined_snapshot_inventory(&inventory, &std_b, &rec_b) != 0) {
            log_msg("Error: Unable to list snapshots for batch verification and pruning");
            inventory_ok = 0;
        }
    }

    if (finalize_batch(&std_b, &inventory, inventory_ok, &prune_matches, &prune_matches_cap, snap_time, 0) != 0) global_status = 1;
    if (finalize_batch(&rec_b, &inventory, inventory_ok, &prune_matches, &prune_matches_cap, snap_time, 1) != 0) global_status = 1;

    free(prune_matches);
    name_list_free(&inventory);
    ret_code = global_status;
cleanup:
free(line);
if (conf)
    fclose(conf);

free(metrics.items);
batch_free(&std_b);
batch_free(&rec_b);
seen_set_free(&seen);

if (log_fp) {
    if (log_had_io_failure()) {
        fprintf(stderr, "%s: one or more writes to log file %s failed; log is incomplete\n",
                progname, LOG_PATH);
        ret_code = 1;
    }
    if (fclose(log_fp) != 0) {
        fprintf(stderr, "%s: failed to flush log file %s: %s\n",
                progname, LOG_PATH, strerror(errno));
        ret_code = 1;
    }
}

close(lock_fd);
return ret_code;
}
