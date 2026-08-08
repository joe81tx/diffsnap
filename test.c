/*
 * test_metrics_scoping_standalone.c
 *
 * Whitebox test for the new "skip if nothing due / scope to due roots /
 * fall back to unscoped if too big / lenient exit-code handling" logic.
 *
 * Unlike test_chunking_standalone.c, this does NOT copy functions out of
 * diffsnap.c -- it #includes diffsnap.c directly, so every function under
 * test here is the actual, real, static function that ships, with zero
 * risk of the test silently drifting from the implementation. main() is
 * renamed out of the way via a macro so this file can supply its own.
 *
 * The default suite requires only a POSIX system with /bin/true and
 * /bin/false (or equivalents).  --system additionally exercises the real
 * ZFS command paths using an isolated child dataset below rpool on Linux
 * and zroot on FreeBSD.
 *
 * Build:
 *   cc -Wall -Wextra -std=c11 -o test_metrics_scoping test_metrics_scoping_standalone.c
 * Run:
 *   ./test_metrics_scoping
 */

#define DIFFSNAP_TESTING 1
#define main diffsnap_real_main
#include "diffsnap.c"
#undef main

#include <assert.h>
#include <sys/stat.h>
#include <limits.h>

static int g_tests_run = 0;
static int g_tests_failed = 0;
static char g_fake_zfs_dir[] = "/tmp/diffsnap-test.XXXXXX";
static char g_fake_zfs[PATH_MAX];
static char g_inventory_args[PATH_MAX];

#define CHECK(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        printf("    FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } else { \
        printf("    ok: %s\n", msg); \
    } \
} while (0)

static int setup_fake_zfs(void) {
    if (!mkdtemp(g_fake_zfs_dir)) return -1;
    if (snprintf(g_fake_zfs, sizeof(g_fake_zfs), "%s/zfs", g_fake_zfs_dir) >= (int)sizeof(g_fake_zfs)) return -1;
    if (snprintf(g_inventory_args, sizeof(g_inventory_args), "%s/inventory-args", g_fake_zfs_dir) >= (int)sizeof(g_inventory_args)) return -1;
    zfs_path = g_fake_zfs;
    return 0;
}

static void cleanup_fake_zfs(void) {
    (void)unlink(g_fake_zfs);
    (void)unlink(g_inventory_args);
    (void)rmdir(g_fake_zfs_dir);
    zfs_path = ZFS_PATH;
}

static int write_fake_zfs(const char *script) {
    FILE *fp = fopen(g_fake_zfs, "w");
    if (!fp) return -1;
    int failed = fputs(script, fp) == EOF;
    if (fclose(fp) != 0) failed = 1;
    if (failed) return -1;
    return chmod(g_fake_zfs, 0700);
}

/*
 * ensure_std_fds() only does anything observable when fd 0, 1, or 2 starts
 * closed, which is never true for this test binary's own stdio -- so it
 * has to be driven directly: close one of the low-numbered slots (stderr,
 * saved aside first so real error output still works everywhere else),
 * call the function, and confirm it specifically refilled that slot with
 * /dev/null rather than merely leaving it closed or opening something
 * else. The real stderr is restored immediately afterward regardless of
 * outcome.
 */
static void run_ensure_std_fds_test(void) {
    printf("== ensure_std_fds: refills a closed low-numbered stdio slot with /dev/null ==\n");
    int saved = dup(STDERR_FILENO);
    if (saved < 0) {
        CHECK(0, "dup'd the real stderr aside before closing it for the ensure_std_fds test");
        printf("\n");
        return;
    }
    fflush(stderr);
    CHECK(close(STDERR_FILENO) == 0, "closed fd 2 to simulate a launcher that starts diffsnap with stderr already closed");

    int rc = ensure_std_fds();
    CHECK(rc == 0, "ensure_std_fds succeeds when fd 2 starts closed");
    CHECK(fcntl(STDERR_FILENO, F_GETFD) != -1, "ensure_std_fds leaves fd 2 open afterward");

    int null_fd = open("/dev/null", O_RDONLY);
    struct stat fd2_st, null_st;
    CHECK(null_fd >= 0 && fstat(STDERR_FILENO, &fd2_st) == 0 && fstat(null_fd, &null_st) == 0 &&
          fd2_st.st_dev == null_st.st_dev && fd2_st.st_rdev == null_st.st_rdev,
          "ensure_std_fds refills the closed slot specifically with /dev/null, not just any open file");
    if (null_fd >= 0) close(null_fd);

    CHECK(dup2(saved, STDERR_FILENO) != -1, "restored the real stderr after the ensure_std_fds test");
    close(saved);

    /* All three fds are open now (real ones, restored above), so a second
     * call is a documented no-op: it must report success without touching
     * anything. */
    CHECK(ensure_std_fds() == 0, "ensure_std_fds is a successful no-op when fd 0/1/2 are all already open");
    printf("\n");
}

static void run_chunk_test(void) {
    char trace_path[PATH_MAX];
    CHECK(snprintf(trace_path, sizeof(trace_path), "%s/chunks", g_fake_zfs_dir) < (int)sizeof(trace_path),
          "chunk trace path fits in the isolated test directory");
    char script[PATH_MAX * 2 + 128];
    CHECK(snprintf(script, sizeof(script),
                   "#!/bin/sh\nif [ \"$1\" = snapshot ]; then\n shift\n printf '%%s\\n' \"$@\" >> '%s'\n printf '\\036\\n' >> '%s'\nfi\n",
                   trace_path, trace_path) < (int)sizeof(script),
          "chunk fake-zfs script fits in its buffer");
    CHECK(write_fake_zfs(script) == 0, "installed fake zfs for chunking test");

    batch_ctx_t batch = {0};
    const size_t item_count = 600;
    char padding[236];
    memset(padding, 'x', sizeof(padding) - 1);
    padding[sizeof(padding) - 1] = '\0';
    for (size_t i = 0; i < item_count; i++) {
        char dataset[STR_BUF_LARGE];
        snprintf(dataset, sizeof(dataset), "pool/%s%03zu", padding, i);
        CHECK(batch_add(&batch, dataset, "chunk", 1, 0) == 0, "chunk test batch item added");
    }
    CHECK(zfs_snapshot_batch(&batch, 0, "2026-01-01_00:00:00") == 0,
          "large batch is accepted by the fake zfs command");

    FILE *trace = fopen(trace_path, "r");
    size_t calls = 0, seen = 0, current_bytes = 0, largest_bytes = 0;
    char line[STR_BUF_XLARGE];
    while (trace && fgets(line, sizeof(line), trace)) {
        if ((unsigned char)line[0] == 036) {
            calls++;
            if (current_bytes > largest_bytes) largest_bytes = current_bytes;
            current_bytes = 0;
        } else {
            seen++;
            current_bytes += strlen(line);
        }
    }
    if (trace) fclose(trace);
    CHECK(calls > 1, "a large batch splits into multiple zfs calls");
    CHECK(seen == item_count, "--chunk accounts for every snapshot exactly once");
    CHECK(largest_bytes <= ARGV_BYTES_CAP, "every chunk stays within ARGV_BYTES_CAP");
    batch_free(&batch);
    (void)unlink(trace_path);
    (void)unlink(g_fake_zfs);
    printf("\n");
}

static void run_system_tests(void) {
#if defined(__FreeBSD__)
    const char *pool = "zroot";
#else
    const char *pool = "rpool";
#endif
    char dataset[STR_BUF_LARGE];
    char standard[STR_BUF_LARGE];
    char tree[STR_BUF_LARGE];
    char tree_child[STR_BUF_LARGE];
    int created = 0;

    printf("== System tests: real ZFS snapshot, inventory, recursive, and pruning paths ==\n");
    zfs_path = ZFS_PATH;
    CHECK(snprintf(dataset, sizeof(dataset), "%s/diffsnap-test-%ld", pool, (long)getpid()) < (int)sizeof(dataset),
          "isolated real-ZFS test dataset name fits in the ZFS name buffer");
    CHECK(snprintf(standard, sizeof(standard), "%s/standard", dataset) < (int)sizeof(standard) &&
          snprintf(tree, sizeof(tree), "%s/tree", dataset) < (int)sizeof(tree) &&
          snprintf(tree_child, sizeof(tree_child), "%s/child", tree) < (int)sizeof(tree_child),
          "real-ZFS child dataset names fit in the ZFS name buffer");

    const char *const check_pool[] = {zfs_path, "list", "-H", "-o", "name", pool, NULL};
    if (exec_cmd_stream(check_pool, NULL, NULL) != 0) {
        CHECK(0, "the required real ZFS pool is available (rpool on Linux, zroot on FreeBSD)");
        zfs_path = g_fake_zfs;
        printf("\n");
        return;
    }
    CHECK(1, "the required real ZFS pool is available (rpool on Linux, zroot on FreeBSD)");

    const char *const create_standard[] = {zfs_path, "create", "-p", standard, NULL};
    int create_issued = 1;
    if (exec_cmd_stream(create_standard, NULL, NULL) != 0) {
        CHECK(0, "created an isolated real-ZFS standard dataset");
        if (create_issued) {
            const char *const destroy_dataset[] = {zfs_path, "destroy", "-r", dataset, NULL};
            (void)exec_cmd_stream(destroy_dataset, NULL, NULL);
        }
        zfs_path = g_fake_zfs;
        printf("\n");
        return;
    }
    created = 1;
    CHECK(1, "created an isolated real-ZFS standard dataset");
    const char *const create_tree[] = {zfs_path, "create", "-p", tree_child, NULL};
    if (exec_cmd_stream(create_tree, NULL, NULL) != 0) {
        CHECK(0, "created an isolated nested real-ZFS dataset tree");
        const char *const destroy_dataset[] = {zfs_path, "destroy", "-r", dataset, NULL};
        (void)exec_cmd_stream(destroy_dataset, NULL, NULL);
        zfs_path = g_fake_zfs;
        printf("\n");
        return;
    }
    CHECK(1, "created an isolated nested real-ZFS dataset tree");

    metric_ctx_t real_metrics = {0};
    const char *const get_written[] = {
        zfs_path, "get", "-H", "-p", "-r", "-t", "filesystem,volume",
        "-o", "name,value", "written", dataset, NULL
    };
    CHECK(exec_cmd_stream_lenient(get_written, handle_metric_line, &real_metrics) == 0 && real_metrics.count == 4,
          "the real ZFS written-metrics command feeds the production metric parser");
    qsort(real_metrics.items, real_metrics.count, sizeof(*real_metrics.items), compare_metrics);
    CHECK(find_metric(&real_metrics, dataset) && find_metric(&real_metrics, dataset)->written >= 0 &&
          find_metric(&real_metrics, standard) && find_metric(&real_metrics, standard)->written >= 0 &&
          find_metric(&real_metrics, tree) && find_metric(&real_metrics, tree)->written >= 0 &&
          find_metric(&real_metrics, tree_child) && find_metric(&real_metrics, tree_child)->written >= 0,
          "the real metrics contain each fresh expected dataset with a valid (non-negative) written value");
    free(real_metrics.items);

    batch_ctx_t standard_batch = {0};
    batch_ctx_t recursive_batch = {0};
    name_list_t inventory = {0};
    char **matches = NULL;
    size_t matches_cap = 0;
    const char *stamp = "2026-01-01_00:00:00";

    CHECK(batch_add(&standard_batch, standard, "system", 1, 0) == 0,
          "assembled a real-ZFS standard snapshot batch");
    CHECK(zfs_snapshot_batch(&standard_batch, 0, stamp) == 0,
          "zfs_snapshot_batch creates a real non-recursive snapshot");
    CHECK(batch_add(&recursive_batch, tree, "system-rec", 1, 0) == 0,
          "assembled a real-ZFS recursive snapshot batch");
    CHECK(zfs_snapshot_batch(&recursive_batch, 1, stamp) == 0,
          "zfs_snapshot_batch creates a real recursive snapshot");
    CHECK(load_combined_snapshot_inventory(&inventory, &standard_batch, &recursive_batch) == 0,
          "real ZFS snapshot inventory loads across standard and recursive roots");

    char standard_snap[STR_BUF_XLARGE];
    char tree_child_snap[STR_BUF_XLARGE];
    CHECK(format_snapshot_name(standard_snap, sizeof(standard_snap), standard, "system", stamp) > 0 &&
          inventory_contains(&inventory, standard_snap),
          "the real inventory contains the standard snapshot created by the batch path");
    CHECK(format_snapshot_name(tree_child_snap, sizeof(tree_child_snap), tree_child, "system-rec", stamp) > 0 &&
          inventory_contains(&inventory, tree_child_snap),
          "the real inventory contains the child snapshot created by the recursive batch path");
    name_list_free(&inventory);

    const char *old_snapshot[] = {zfs_path, "snapshot", NULL, NULL};
    char old_name[STR_BUF_XLARGE];
    char new_name[STR_BUF_XLARGE];
    CHECK(format_snapshot_name(old_name, sizeof(old_name), standard, "system-prune", "2025-01-01_00:00:00") > 0 &&
          format_snapshot_name(new_name, sizeof(new_name), standard, "system-prune", stamp) > 0,
          "real-ZFS pruning snapshot names format safely");
    old_snapshot[2] = old_name;
    CHECK(exec_cmd_stream(old_snapshot, NULL, NULL) == 0, "created an older real-ZFS snapshot for pruning");
    sleep(1);
    batch_ctx_t prune_batch = {0};
    CHECK(batch_add(&prune_batch, standard, "system-prune", 1, 0) == 0 &&
          zfs_snapshot_batch(&prune_batch, 0, stamp) == 0,
          "created the retained real-ZFS snapshot through the batch path");
    CHECK(load_combined_snapshot_inventory(&inventory, &prune_batch, &recursive_batch) == 0,
          "real-ZFS inventory reloads before pruning");
    CHECK(prune_from_inventory(&inventory, standard, "system-prune", 1, 0, &matches, &matches_cap) == 0,
          "prune_from_inventory destroys the older real-ZFS snapshot");
    name_list_free(&inventory);
    CHECK(load_combined_snapshot_inventory(&inventory, &prune_batch, &recursive_batch) == 0 &&
          !inventory_contains(&inventory, old_name) && inventory_contains(&inventory, new_name),
          "real-ZFS pruning leaves only the newest matching snapshot");

    free(matches);
    name_list_free(&inventory);
    batch_free(&prune_batch);
    batch_free(&recursive_batch);
    batch_free(&standard_batch);
    if (created) {
        const char *const destroy_dataset[] = {zfs_path, "destroy", "-r", dataset, NULL};
        CHECK(exec_cmd_stream(destroy_dataset, NULL, NULL) == 0,
              "destroyed the isolated real-ZFS test dataset and its snapshots");
    }
    zfs_path = g_fake_zfs;
    printf("\n");
}

static const char *find_bin(const char *const candidates[]) {
    for (size_t i = 0; candidates[i]; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0 && (st.st_mode & S_IXUSR)) return candidates[i];
    }
    return NULL;
}

/*
 * Spy handler for Test 22 (stream_reader_consume overflow-ordering fix).
 * File-scope so it can be passed as a plain line_handler_t function
 * pointer while still recording what, if anything, it was called with.
 */
static int g_spy_calls = 0;
static char g_spy_last_line[STR_BUF_XLARGE];

static int spy_line_handler(const char *line, void *data) {
    (void)data;
    g_spy_calls++;
    snprintf(g_spy_last_line, sizeof(g_spy_last_line), "%s", line);
    return 0;
}

static int g_localtime_fail;
static int g_localtime_calls;
static struct tm *test_localtime(const time_t *value, struct tm *result) {
    g_localtime_calls++;
    return g_localtime_fail ? NULL : localtime_r(value, result);
}

static long g_realloc_calls;
static long g_realloc_fail_after = -1;
static void *test_realloc(void *ptr, size_t size) {
    g_realloc_calls++;
    return (g_realloc_fail_after >= 0 && g_realloc_calls > g_realloc_fail_after) ? NULL : realloc(ptr, size);
}

static struct tm *test_positive_offset_localtime(const time_t *value, struct tm *result) {
    (void)value;
    memset(result, 0, sizeof(*result));
    result->tm_year = 126; result->tm_mon = 2; result->tm_mday = 8;
    result->tm_hour = 0; result->tm_min = 0; result->tm_sec = 0;
#if defined(__GLIBC__) || defined(__FreeBSD__)
    result->tm_gmtoff = 5 * 60 * 60;
#endif
    return result;
}

static struct tm *test_non_due_localtime(const time_t *value, struct tm *result) {
    (void)value;
    memset(result, 0, sizeof(*result));
    result->tm_year = 126; result->tm_mon = 0; result->tm_mday = 1;
    result->tm_min = 1; /* 1 minute after midnight is not divisible by 7. */
    return result;
}

static ssize_t test_getline_failure(char **lineptr, size_t *n, FILE *stream) {
    (void)lineptr; (void)n; (void)stream;
    errno = EIO;
    return -1;
}

static int run_main_capture_stderr(int argc, char **argv, char *buf, size_t buf_size) {
    int saved = dup(STDERR_FILENO);
    FILE *capture = tmpfile();
    if (saved < 0 || !capture) { if (saved >= 0) close(saved); if (capture) fclose(capture); return -1; }
    fflush(stderr);
    if (dup2(fileno(capture), STDERR_FILENO) < 0) { close(saved); fclose(capture); return -1; }
    int rc = diffsnap_real_main(argc, argv);
    fflush(stderr);
    rewind(capture);
    if (buf_size) {
        size_t got = fread(buf, 1, buf_size - 1, capture);
        buf[got] = '\0';
    }
    (void)dup2(saved, STDERR_FILENO);
    close(saved);
    fclose(capture);
    return rc;
}

static void run_main_pipeline_tests(void) {
    char conf_file[PATH_MAX], log_file[PATH_MAX], lock_file[PATH_MAX], args_file[PATH_MAX];
    CHECK(snprintf(conf_file, sizeof(conf_file), "%s/main.conf", g_fake_zfs_dir) < (int)sizeof(conf_file) &&
          snprintf(log_file, sizeof(log_file), "%s/main.log", g_fake_zfs_dir) < (int)sizeof(log_file) &&
          snprintf(lock_file, sizeof(lock_file), "%s/main.lock", g_fake_zfs_dir) < (int)sizeof(lock_file) &&
          snprintf(args_file, sizeof(args_file), "%s/main-args", g_fake_zfs_dir) < (int)sizeof(args_file),
          "isolated files for direct main() pipeline tests fit in the test directory");
    conf_path = conf_file; log_path = log_file; lock_path = lock_file;

    char script[PATH_MAX * 2 + 256];
    CHECK(snprintf(script, sizeof(script),
                   "#!/bin/sh\nprintf '%%s\\n' \"$@\" >> '%s'\n"
                   "if [ \"$1\" = get ]; then printf 'pool/due\\t100\\n'; fi\nexit 0\n", args_file) < (int)sizeof(script) &&
          write_fake_zfs(script) == 0, "fake zfs for direct main() tests installed");

    printf("== Main/config pipeline tests ==\n");
    FILE *fp = fopen(conf_file, "w");
    CHECK(fp != NULL, "opened isolated main() config for per-field validation");
    if (fp) {
        fputs("1pool,1,1,p,no,0\n", fp);
        fputs("pool/a,0,1,p,no,0\n", fp);
        fputs("pool/a,1,0,p,no,0\n", fp);
        fputs("pool/a,1,1,bad prefix,no,0\n", fp);
        fputs("pool/a,1,1,p,maybe,0\n", fp);
        fputs("pool/a,1,1,p,no,-1\n", fp);
        fputs("pool/a,1,1,p,no,0,extra\n", fp);
        fputs(",1,1,p,no,0\n", fp);
        fputs("pool/a,1,,p,no,0\n", fp);
        fputs("pool/due,1,1,p,no,0\n", fp);
        fputs("pool/due,1,1,p,no,0\n", fp);
        fclose(fp);
        unlink(args_file);
        char *argv[] = {"diffsnap-test", NULL};
        CHECK(diffsnap_real_main(1, argv) == 1, "main() returns failure when every config field has an invalid example");
        CHECK(log_fp == NULL,
              "main() nulls the global log_fp after closing it, so no dangling FILE* survives into the next diffsnap_real_main() call in this in-process suite");
        FILE *log = fopen(log_file, "r");
        char contents[8192] = {0};
        if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
        CHECK(strstr(contents, "invalid dataset") && strstr(contents, "invalid interval") &&
              strstr(contents, "invalid retention") && strstr(contents, "invalid prefix") &&
              strstr(contents, "invalid recursive") && strstr(contents, "invalid byte threshold") &&
              strstr(contents, "adjacent comma delimiters") && strstr(contents, "duplicate dataset/prefix"),
              "main() validates each config field through the real parsing pipeline");
    }

    fp = fopen(conf_file, "w");
    CHECK(fp != NULL, "opened isolated main() config for REQUIRE_TOKEN missing-field validation");
    if (fp) {
        /* Each line is truncated one field earlier than the last, so every
         * one of REQUIRE_TOKEN's five "missing field" error paths (interval,
         * retention, prefix, recursive, min_bytes) is driven by a line that
         * has every field up to that point valid, but simply ends before
         * strtok_r() has another token to hand back. */
        fputs("pool/a\n", fp);
        fputs("pool/a,1\n", fp);
        fputs("pool/a,1,1\n", fp);
        fputs("pool/a,1,1,p\n", fp);
        fputs("pool/a,1,1,p,no\n", fp);
        fclose(fp);
        unlink(args_file);
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
              "main() returns failure when every config line is missing a required trailing field");
        FILE *log = fopen(log_file, "r");
        char contents[4096] = {0};
        if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
        CHECK(strstr(contents, "missing interval field") != NULL,
              "REQUIRE_TOKEN reports a missing interval field when a line ends after the dataset");
        CHECK(strstr(contents, "missing retention field") != NULL,
              "REQUIRE_TOKEN reports a missing retention field when a line ends after the interval");
        CHECK(strstr(contents, "missing prefix field") != NULL,
              "REQUIRE_TOKEN reports a missing prefix field when a line ends after the retention");
        CHECK(strstr(contents, "missing recursive field") != NULL,
              "REQUIRE_TOKEN reports a missing recursive field when a line ends after the prefix");
        CHECK(strstr(contents, "missing min_bytes field") != NULL,
              "REQUIRE_TOKEN reports a missing min_bytes field when a line ends after the recursive flag");
    }

    fp = fopen(conf_file, "w");
    if (fp) {
        fputs("pool/due,7,1,p,no,0\n", fp); fclose(fp);
        unlink(args_file);
        diffsnap_override_time((time_t)0);
        localtime_now_fn = test_non_due_localtime;
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 0,
              "the interval-scheduling gate permits a clean no-work run");
        localtime_now_fn = localtime_r;
        diffsnap_clear_time_override();
        CHECK(access(args_file, F_OK) != 0, "a non-due entry does not invoke zfs through main()");
    }

    fp = fopen(conf_file, "w");
    if (fp) {
        fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
        unlink(args_file);
        localtime_now_fn = test_positive_offset_localtime;
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 0,
              "a due entry completes through main() with the fake zfs command");
        localtime_now_fn = localtime_r;
        FILE *args = fopen(args_file, "r");
        char contents[8192] = {0};
        if (args) { size_t rd = fread(contents, 1, sizeof(contents) - 1, args); (void)rd; fclose(args); }
        CHECK(strstr(contents, "pool/due@p_2026-03-08_00:00:00p0500") != NULL,
              "main() rewrites a positive DST offset's '+' to 'p' in snapshot names");
    }

    int held_lock = open(lock_file, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    CHECK(held_lock >= 0 && flock(held_lock, LOCK_EX | LOCK_NB) == 0,
          "test process acquired the isolated lock before invoking main()");
    if (held_lock >= 0) {
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
              "main() exits unsuccessfully when flock reports an existing instance");
        flock(held_lock, LOCK_UN); close(held_lock);
    }

    fp = fopen(conf_file, "w");
    if (fp) {
        fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
        /*
         * /dev/full is Linux-specific. On a platform without it,
         * fopen(log_path, "ae") itself would fail (a different code
         * path entirely -- "failed to open log file", not "writes to
         * log file ... failed"), so asserting the writes-failed message
         * unconditionally would fail this test for a reason unrelated
         * to the log_io_failed logic it's meant to cover. Skip visibly
         * instead of asserting blindly.
         */
        if (access("/dev/full", W_OK) == 0) {
            log_path = "/dev/full";
            char stderr_contents[1024] = {0};
            CHECK(run_main_capture_stderr(1, (char *[]){"diffsnap-test", NULL}, stderr_contents, sizeof(stderr_contents)) == 1,
                  "a detected log_io_failed condition changes main()'s process exit code to failure");
            CHECK(strstr(stderr_contents, "writes to log file /dev/full failed") != NULL,
                  "a log write failure reports the incomplete-log diagnostic on stderr");
            log_path = log_file;
        } else {
            printf("    SKIP: /dev/full not available on this platform; log_io_failed exit-code test skipped\n");
        }
    }

    fp = fopen(conf_file, "w");
    if (fp) {
        char long_dataset[240], long_prefix[32];
        memset(long_dataset, 'a', sizeof(long_dataset) - 1); long_dataset[sizeof(long_dataset) - 1] = '\0';
        memset(long_prefix, 'p', sizeof(long_prefix) - 1); long_prefix[sizeof(long_prefix) - 1] = '\0';
        fprintf(fp, "%s,1,1,%s,no,0\n", long_dataset, long_prefix);
        fclose(fp);
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
              "main() rejects a dataset/prefix/timestamp name longer than ZFS permits");
        FILE *log = fopen(log_file, "r");
        char contents[8192] = {0};
        if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
        CHECK(strstr(contents, "dataset/prefix timestamp name exceeds ZFS limit") != NULL,
              "the overlong snapshot-name validation, rather than an unrelated metrics failure, is logged");
    }

    fp = fopen(conf_file, "w");
    if (fp) {
        fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
        getline_now_fn = test_getline_failure;
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
              "main() fails when getline reports a config read error before EOF");
        getline_now_fn = getline;
    }

    fp = fopen(conf_file, "w");
    if (fp) {
        fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
        g_localtime_fail = 1; localtime_now_fn = test_localtime;
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
              "main() fails when its top-level localtime_r call fails");
        localtime_now_fn = localtime_r; g_localtime_fail = 0;
    }

    /*
     * main()'s own metrics-fetch scoped/unscoped fallback is a separate,
     * independently-implemented use_scoped/roots_bytes > ARGV_BYTES_CAP
     * check from the structurally similar one in
     * load_combined_snapshot_inventory (covered directly by Test 29). It --
     * and, with it, main()'s choice between exec_cmd_stream_lenient
     * (scoped) and exec_cmd_stream (unscoped) for this call site -- can
     * only be driven by real due roots gathered from parsed config
     * entries, so these two blocks go through diffsnap_real_main() itself
     * rather than unit-testing either helper in isolation.
     */
    fp = fopen(conf_file, "w");
    if (fp) {
        fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
        unlink(args_file);
        char get_script[PATH_MAX + 128];
        CHECK(snprintf(get_script, sizeof(get_script),
                       "#!/bin/sh\nif [ \"$1\" = get ]; then\n shift\n printf '%%s\\n' \"$@\" >> '%s'\n"
                       " printf 'pool/due\\t100\\n'\n exit 5\nfi\nexit 0\n",
                       args_file) < (int)sizeof(get_script) &&
              write_fake_zfs(get_script) == 0,
              "fake zfs for main()'s scoped metrics-fetch test installed");
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 0,
              "main() tolerates a nonzero exit from its own scoped `zfs get` when valid metrics were still printed for the one due root (exec_cmd_stream_lenient)");
        FILE *args = fopen(args_file, "r");
        char contents[1024] = {0};
        if (args) { size_t rd = fread(contents, 1, sizeof(contents) - 1, args); (void)rd; fclose(args); }
        CHECK(strstr(contents, "-r\n") != NULL && strstr(contents, "pool/due\n") != NULL,
              "main()'s own metrics fetch scopes the `zfs get` call to the due root when the root bytes fit ARGV_BYTES_CAP");
    }

    fp = fopen(conf_file, "w");
    if (fp) {
        /* Enough due roots, each long but still within the per-line ZFS
         * name limit main() enforces during parsing, to push their combined
         * bytes past ARGV_BYTES_CAP and force main()'s own fallback to an
         * unscoped `zfs get`. */
        char padding[190];
        memset(padding, 'x', sizeof(padding) - 1);
        padding[sizeof(padding) - 1] = '\0';
        for (size_t i = 0; i < 700; i++) fprintf(fp, "pool/%s%03zu,1,1,p,no,0\n", padding, i);
        fclose(fp);
        unlink(args_file);
        unlink(log_file);
        char get_script[PATH_MAX + 128];
        CHECK(snprintf(get_script, sizeof(get_script),
                       "#!/bin/sh\nif [ \"$1\" = get ]; then\n shift\n printf '%%s\\n' \"$@\" >> '%s'\n exit 3\nfi\nexit 0\n",
                       args_file) < (int)sizeof(get_script) &&
              write_fake_zfs(get_script) == 0,
              "fake zfs for main()'s unscoped metrics-fetch fallback test installed");
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
              "main() fails when its own unscoped `zfs get` fallback exits nonzero (exec_cmd_stream, strict)");
        FILE *args = fopen(args_file, "r");
        char contents[1024] = {0};
        if (args) { size_t rd = fread(contents, 1, sizeof(contents) - 1, args); (void)rd; fclose(args); }
        CHECK(strstr(contents, "-r\n") == NULL,
              "main()'s own metrics fetch falls back to an unscoped `zfs get` when the due roots' combined bytes exceed ARGV_BYTES_CAP");
        FILE *log = fopen(log_file, "r");
        char log_contents[4096] = {0};
        if (log) { size_t rd = fread(log_contents, 1, sizeof(log_contents) - 1, log); (void)rd; fclose(log); }
        CHECK(strstr(log_contents, "Failed to read ZFS written metrics") != NULL,
              "the strict unscoped fallback's nonzero exit surfaces through main()'s top-level metrics-fetch error");
    }

    fp = fopen(conf_file, "w");
    if (fp) {
        fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
        unlink(log_file);
        /* Preceding this run's own m_argv allocation are: seen_set_add's
         * growth + key copy (2 calls), batch_add's growth + dataset copy +
         * prefix copy (3 calls), and collect_due_roots' single
         * root_list_add_unique growth + string copy (2 calls) -- 7 calls
         * total, so failing call 8 targets exactly the metrics-fetch argv
         * allocation itself. */
        g_realloc_calls = 0; g_realloc_fail_after = 7; realloc_now_fn = test_realloc;
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
              "main() fails when its own metrics-fetch argv allocation fails");
        /*
         * This test's call count (7 successful calls before the
         * injected failure) is derived by manually counting every
         * diffsnap_realloc call made upstream of the metrics-fetch
         * m_argv allocation for this exact config (see comment above).
         * That derivation is fragile: if diffsnap.c's internal
         * allocation order or count ever changes, this could silently
         * start injecting the failure into a *different* allocation
         * than m_argv while still returning rc==1 for an unrelated
         * reason. Asserting the exact call count directly turns that
         * silent retargeting into a loud, explicit failure instead.
         */
        CHECK(g_realloc_calls == 8,
              "the injected failure landed on exactly the 8th realloc call -- the metrics-fetch argv allocation itself, not an earlier or later one");
        realloc_now_fn = realloc; g_realloc_fail_after = -1;
        FILE *log = fopen(log_file, "r");
        char contents[1024] = {0};
        if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
        CHECK(strstr(contents, "Failed to allocate metrics command") != NULL,
              "the metrics-fetch argv allocation failure is logged with its own diagnostic");
    }

    CHECK(diffsnap_real_main(2, (char *[]){"diffsnap-test", "--help", NULL}) == 0,
          "main() accepts --help");
    CHECK(diffsnap_real_main(2, (char *[]){"diffsnap-test", "--version", NULL}) == 0,
          "main() accepts --version");
    CHECK(diffsnap_real_main(2, (char *[]){"diffsnap-test", "--unknown", NULL}) == 2,
          "main() rejects an unknown option");
    CHECK(diffsnap_real_main(3, (char *[]){"diffsnap-test", "--help", "extra", NULL}) == 2,
          "main() rejects more than one command-line option");
    unlink(g_fake_zfs);
    unlink(conf_file); unlink(log_file); unlink(lock_file); unlink(args_file);
    conf_path = CONF_PATH; log_path = LOG_PATH; lock_path = LOCK_PATH;
    printf("\n");
}

static void run_fault_injection_tests(void) {
    printf("== Fault injection: in-process time, localtime, and allocation hooks ==\n");
    diffsnap_override_time((time_t)0);
    CHECK(diffsnap_now() == (time_t)0, "the test overrides diffsnap's clock without changing the system clock");
    diffsnap_clear_time_override();

    char *buf = NULL;
    size_t buf_len = 0;
    log_fp = open_memstream(&buf, &buf_len);
    CHECK(log_fp != NULL, "memory-backed log capture opened");
    if (log_fp) {
        g_localtime_calls = 0;
        g_localtime_fail = 1;
        localtime_now_fn = test_localtime;
        log_msg("message during localtime failure");
        localtime_now_fn = localtime_r;
        g_localtime_fail = 0;
        fflush(log_fp);
        size_t newlines = 0;
        for (size_t i = 0; i < buf_len; i++) if (buf[i] == '\n') newlines++;
        CHECK(g_localtime_calls == 1, "localtime hook intercepted the logging call");
        CHECK(buf && strstr(buf, "localtime_r failed") && strstr(buf, "message during localtime failure"),
              "timestamp failure is described on the log message");
        CHECK(newlines == 1, "timestamp fallback produces exactly one log line");
        fclose(log_fp); log_fp = NULL; free(buf);
    }

    batch_ctx_t ctx = {0};
    const size_t count = ALLOC_CHUNK_BATCH + 8;
    for (size_t i = 0; i < count; i++) {
        char dataset[64];
        snprintf(dataset, sizeof(dataset), "pool/ds%03zu", i);
        CHECK(batch_add(&ctx, dataset, "p", 1, 0) == 0, "OOM test batch setup succeeds");
    }
    g_realloc_calls = 0;
    g_realloc_fail_after = 1;
    realloc_now_fn = test_realloc;
    int rc = zfs_snapshot_batch_root_pass(&ctx, 0, "2026-01-01_00:00:00", "pool", 0);
    realloc_now_fn = realloc;
    g_realloc_fail_after = -1;
    size_t failed = 0;
    for (size_t i = 0; i < ctx.count; i++) failed += ctx.items[i].snap_failed != 0;
    CHECK(rc == -1 && g_realloc_calls == 2, "index collection reports the injected second realloc failure");
    CHECK(failed == count, "an index allocation failure marks every affected root/pass item failed");
    batch_free(&ctx);

    batch_ctx_t chunk_ctx = {0};
    realloc_now_fn = realloc;
    int chunk_setup = batch_add(&chunk_ctx, "pool/chunk", "p", 1, 0) == 0;
    size_t chunk_indices[1] = {0};

    g_realloc_calls = 0; g_realloc_fail_after = 0; realloc_now_fn = test_realloc;
    CHECK(chunk_setup && zfs_snapshot_exec_chunk(&chunk_ctx, 0, "2026-01-01_00:00:00", chunk_indices, 1, 256) == -1,
          "zfs_snapshot_exec_chunk reports an injected argv allocation failure");
    CHECK(chunk_ctx.count == 1 && chunk_ctx.items[0].snap_failed == 1,
          "an argv allocation failure marks the chunk's items snap_failed");
    chunk_ctx.items[0].snap_failed = 0;

    g_realloc_calls = 0; g_realloc_fail_after = 1;
    CHECK(zfs_snapshot_exec_chunk(&chunk_ctx, 0, "2026-01-01_00:00:00", chunk_indices, 1, 256) == -1,
          "zfs_snapshot_exec_chunk reports an injected arena allocation failure after a successful argv allocation");
    CHECK(chunk_ctx.items[0].snap_failed == 1,
          "an arena allocation failure also marks the chunk's items snap_failed");
    realloc_now_fn = realloc; g_realloc_fail_after = -1;
    batch_free(&chunk_ctx);

    /* Exercise every other dynamic-growth site through the same realloc
     * hook. Each starts empty, so failing its first growth is deterministic. */
    g_realloc_fail_after = 0; realloc_now_fn = test_realloc;
    batch_ctx_t batch = {0};
    CHECK(batch_add(&batch, "pool/a", "p", 1, 0) == -1,
          "batch_add reports an injected growth allocation failure");
    batch_free(&batch);

    g_realloc_calls = 0; g_realloc_fail_after = 1;
    batch_ctx_t batch_dup = {0};
    CHECK(batch_add(&batch_dup, "pool/a", "p", 1, 0) == -1,
          "batch_add reports an injected dataset/prefix string-copy allocation failure after a successful growth");
    CHECK(batch_dup.count == 0, "batch_add leaves the batch empty when the string copy fails");
    batch_free(&batch_dup);
    g_realloc_fail_after = 0;

    seen_set_t seen = {0};
    CHECK(seen_set_add(&seen, "pool/a", "p") == -1,
          "seen_set_add reports an injected growth allocation failure");
    seen_set_free(&seen);

    g_realloc_calls = 0; g_realloc_fail_after = 1;
    seen_set_t seen_dup = {0};
    CHECK(seen_set_add(&seen_dup, "pool/a", "p") == -1,
          "seen_set_add reports an injected key-copy allocation failure after a successful growth");
    CHECK(seen_dup.count == 0, "seen_set_add leaves the set empty when the key copy fails");
    seen_set_free(&seen_dup);
    g_realloc_fail_after = 0;

    root_list_t roots = {0};
    CHECK(root_list_add_unique(&roots, "pool/a") == -1,
          "root_list_add_unique reports an injected growth allocation failure");
    root_list_free(&roots);

    g_realloc_calls = 0; g_realloc_fail_after = 1;
    root_list_t roots_dup = {0};
    CHECK(root_list_add_unique(&roots_dup, "pool/a") == -1,
          "root_list_add_unique reports an injected string-copy allocation failure after a successful growth");
    CHECK(roots_dup.count == 0, "root_list_add_unique leaves the list empty when the string copy fails");
    root_list_free(&roots_dup);
    g_realloc_fail_after = 0;

    /*
     * These two blocks now set up realloc_now_fn/g_realloc_fail_after
     * explicitly rather than relying on the state left over from the
     * root_list_add_unique block above: relying on carried-over state
     * makes a test's target allocation depend on the exact order of the
     * blocks around it, so a harmless reordering elsewhere in this
     * function could silently point the injected failure at a different
     * allocation than the one the CHECK message claims to cover.
     */
    g_realloc_calls = 0; g_realloc_fail_after = 0; realloc_now_fn = test_realloc;
    metric_ctx_t metrics = {0};
    CHECK(handle_metric_line("pool/a\t1", &metrics) == -1,
          "handle_metric_line reports an injected growth allocation failure");
    free(metrics.items);

    g_realloc_calls = 0; g_realloc_fail_after = 0; realloc_now_fn = test_realloc;
    name_list_t names = {0};
    CHECK(handle_snapshot_inventory_line("pool/a@s_2026-01-01_00:00:00", &names) == -1,
          "handle_snapshot_inventory_line reports an injected growth allocation failure");
    name_list_free(&names);

    g_realloc_calls = 0; g_realloc_fail_after = 1; realloc_now_fn = test_realloc;
    name_list_t names_dup = {0};
    CHECK(handle_snapshot_inventory_line("pool/a@s_2026-01-01_00:00:00", &names_dup) == -1,
          "handle_snapshot_inventory_line reports an injected string-copy allocation failure after a successful growth");
    CHECK(names_dup.count == 0, "handle_snapshot_inventory_line leaves the list empty when the string copy fails");
    name_list_free(&names_dup);
    g_realloc_fail_after = 0;

    batch_ctx_t recursive = {0};
    realloc_now_fn = realloc;
    int recursive_setup = batch_add(&recursive, "pool/a", "p", 1, 0);
    g_realloc_calls = 0; realloc_now_fn = test_realloc;
    CHECK(recursive_setup == 0 && resolve_recursive_ancestor_overlaps(&recursive) == -1,
          "resolve_recursive_ancestor_overlaps reports an injected allocation failure");
    batch_free(&recursive);

    batch_ctx_t recursive_compacted = {0};
    realloc_now_fn = realloc;
    int compacted_setup = batch_add(&recursive_compacted, "pool/a", "p", 1, 0) == 0 &&
                          batch_add(&recursive_compacted, "pool/a/child", "p", 1, 0) == 0;
    g_realloc_calls = 0; g_realloc_fail_after = 1; realloc_now_fn = test_realloc;
    CHECK(compacted_setup && resolve_recursive_ancestor_overlaps(&recursive_compacted) == -1,
          "resolve_recursive_ancestor_overlaps cleans up when its post-compaction ordering allocation fails");
    CHECK(recursive_compacted.count == 1 && strcmp(recursive_compacted.items[0].dataset, "pool/a") == 0,
          "the surviving recursive ancestor remains safely owned after ordering-allocation failure");
    batch_free(&recursive_compacted);
    g_realloc_fail_after = 0;

    batch_ctx_t standard = {0}, recursive_cover = {0};
    realloc_now_fn = realloc;
    int overlap_setup = batch_add(&standard, "pool/a/child", "p", 1, 0) == 0 &&
                        batch_add(&recursive_cover, "pool/a", "p", 1, 0) == 0;
    g_realloc_calls = 0; realloc_now_fn = test_realloc;
    CHECK(overlap_setup && remove_recursive_overlaps(&standard, &recursive_cover) == -1,
          "remove_recursive_overlaps reports an injected allocation failure");
    CHECK(standard.count == 1 && recursive_cover.count == 1,
          "recursive-overlap allocation failure leaves both batches owned by their callers");
    batch_free(&standard); batch_free(&recursive_cover);

    batch_ctx_t standard2 = {0}, recursive_cover2 = {0};
    realloc_now_fn = realloc;
    int overlap_setup2 = batch_add(&standard2, "pool/a/child", "p", 1, 0) == 0 &&
                         batch_add(&recursive_cover2, "pool/a", "p", 1, 0) == 0;
    g_realloc_calls = 0; g_realloc_fail_after = 1; realloc_now_fn = test_realloc;
    CHECK(overlap_setup2 && remove_recursive_overlaps(&standard2, &recursive_cover2) == -1,
          "remove_recursive_overlaps reports an injected key-copy allocation failure after a successful rec_keys growth");
    CHECK(standard2.count == 1 && recursive_cover2.count == 1,
          "the per-item key-copy allocation failure also leaves both batches owned by their callers");
    realloc_now_fn = realloc; g_realloc_fail_after = -1;
    batch_free(&standard2); batch_free(&recursive_cover2);
    g_realloc_fail_after = 0; realloc_now_fn = test_realloc;

    batch_ctx_t inv_std = {0}, inv_rec = {0};
    realloc_now_fn = realloc;
    int inv_setup = batch_add(&inv_std, "pool/inv", "p", 1, 0) == 0;
    name_list_t inv_list = {0};
    g_realloc_calls = 0; g_realloc_fail_after = 2; realloc_now_fn = test_realloc;
    CHECK(inv_setup && load_combined_snapshot_inventory(&inv_list, &inv_std, &inv_rec) == -1,
          "load_combined_snapshot_inventory reports an injected argv allocation failure after collecting due roots");
    CHECK(inv_list.count == 0,
          "load_combined_snapshot_inventory leaves its output list empty when the argv allocation fails");
    realloc_now_fn = realloc; g_realloc_fail_after = -1;
    name_list_free(&inv_list);
    batch_free(&inv_std); batch_free(&inv_rec);
    g_realloc_fail_after = 0; realloc_now_fn = test_realloc;

    name_list_t inventory = {0};
    inventory.names = calloc(1, sizeof(*inventory.names));
    if (inventory.names) {
        inventory.names[0] = strdup("pool/a@p_2026-01-01_00:00:00"); inventory.count = 1; inventory.capacity = 1;
        char **matches = NULL; size_t matches_cap = 0;
        g_realloc_calls = 0; realloc_now_fn = test_realloc;
        CHECK(prune_from_inventory(&inventory, "pool/a", "p", 1, 0, &matches, &matches_cap) == -1,
              "prune_from_inventory reports an injected match-list growth allocation failure");
        free(matches);
    } else {
        CHECK(0, "allocated setup inventory for prune allocation-failure test");
    }
    name_list_free(&inventory);
    realloc_now_fn = realloc; g_realloc_fail_after = -1;
    printf("\n");
}

int main(int argc, char **argv) {
    int run_system = argc == 2 && strcmp(argv[1], "--system") == 0;
    if (argc != 1 && !run_system) {
        fprintf(stderr, "Usage: %s [--system]\n", argv[0]);
        return 2;
    }
    CHECK(setup_fake_zfs() == 0, "created an isolated fake-zfs directory");
    if (zfs_path != g_fake_zfs) return 1;
    run_ensure_std_fds_test();
    run_main_pipeline_tests();
    run_fault_injection_tests();
    run_chunk_test();
    printf("== Test 1: exec_cmd_stream (strict) vs exec_cmd_stream_lenient on a clean-success process ==\n");
    {
        const char *const sh_candidates[] = {"/bin/sh", "/usr/bin/sh", NULL};
        const char *sh_bin = find_bin(sh_candidates);
        CHECK(sh_bin != NULL, "found a POSIX shell on this system");
        if (sh_bin) {
            const char *const argv[] = {sh_bin, "-c", "exit 0", NULL};
            CHECK(exec_cmd_stream(argv, NULL, NULL) == 0, "strict: clean success (exit 0) succeeds");
            CHECK(exec_cmd_stream_lenient(argv, NULL, NULL) == 0, "lenient: clean success (exit 0) succeeds");
        }
        printf("\n");
    }

    printf("== Test 2: exec_cmd_stream (strict) vs exec_cmd_stream_lenient on a nonzero-exit process ==\n");
    {
        const char *const sh_candidates[] = {"/bin/sh", "/usr/bin/sh", NULL};
        const char *sh_bin = find_bin(sh_candidates);
        CHECK(sh_bin != NULL, "found a POSIX shell on this system");
        if (sh_bin) {
            const char *const argv[] = {sh_bin, "-c", "exit 1", NULL};
            CHECK(exec_cmd_stream(argv, NULL, NULL) != 0, "strict: nonzero exit is treated as failure (this is the existing, unchanged behavior)");
            CHECK(exec_cmd_stream_lenient(argv, NULL, NULL) == 0, "lenient: nonzero exit is NOT treated as failure (the new behavior for scoped zfs get)");
        }
        printf("\n");
    }

    printf("== Test 3: abnormal termination (signal) is still fatal in BOTH modes ==\n");
    {
        /* /bin/sh -c 'kill -9 $$' makes the child kill itself with SIGKILL --
         * WIFEXITED will be false, exercising the child_exited==false path
         * rather than the exit-status path. */
        const char *const sh_candidates[] = {"/bin/sh", "/usr/bin/sh", NULL};
        const char *sh_bin = find_bin(sh_candidates);
        CHECK(sh_bin != NULL, "found a shell to use for the self-signal test");
        if (sh_bin) {
            const char *const argv[] = {sh_bin, "-c", "kill -9 $$", NULL};
            CHECK(exec_cmd_stream(argv, NULL, NULL) != 0, "strict: signal death is treated as failure");
            CHECK(exec_cmd_stream_lenient(argv, NULL, NULL) != 0, "lenient: signal death is STILL treated as failure (only exit *status* is relaxed, not abnormal termination)");
        }
        printf("\n");
    }

    printf("== Test 4: handler still receives real stdout output through both wrappers ==\n");
    {
        const char *const sh_candidates[] = {"/bin/sh", "/usr/bin/sh", NULL};
        const char *sh_bin = find_bin(sh_candidates);
        CHECK(sh_bin != NULL, "found a POSIX shell");
        if (sh_bin) {
            metric_ctx_t ctx = {0};
            const char *const argv[] = {sh_bin, "-c", "printf 'pool/child\\t12345\\n'", NULL};
            int rc = exec_cmd_stream(argv, handle_metric_line, &ctx);
            CHECK(rc == 0, "strict call with echo succeeds");
            CHECK(ctx.count == 1, "handler received exactly one parsed line");
            if (ctx.count == 1) {
                CHECK(strcmp(ctx.items[0].name, "pool/child") == 0, "parsed dataset name is correct");
                CHECK(ctx.items[0].written == 12345, "parsed written value is correct");
            }
            free(ctx.items);
        }
        printf("\n");
    }

    printf("== Test 5: root_list_add_unique preserves configured paths and coalesces only ancestor/descendant roots ==\n");
    {
        root_list_t list = {0};
        root_list_add_unique(&list, "pool/a");
        root_list_add_unique(&list, "pool/b");
        CHECK(list.count == 2, "sibling datasets remain distinct scoped roots");
        CHECK(strcmp(list.roots[0], "pool/a") == 0 && strcmp(list.roots[1], "pool/b") == 0,
              "full configured dataset paths are retained");
        root_list_add_unique(&list, "pool/a/child");
        CHECK(list.count == 2, "a descendant is covered by its existing ancestor root");
        root_list_add_unique(&list, "pool");
        CHECK(list.count == 1 && strcmp(list.roots[0], "pool") == 0,
              "a newly seen ancestor replaces its descendant roots");
        root_list_add_unique(&list, "otherpool/x");
        CHECK(list.count == 2, "a genuinely different root is added");
        root_list_add_unique(&list, "otherpool");
        CHECK(list.count == 2 && strcmp(list.roots[1], "otherpool") == 0,
              "an ancestor is stored as its complete configured path");
        root_list_free(&list);
        printf("\n");
    }

    printf("== Test 6: collect_due_roots across both batches, with duplicates and cross-batch overlap ==\n");
    {
        batch_ctx_t std_b = {0}, rec_b = {0};
        batch_add(&std_b, "pool/a", "p", 1, 0);
        batch_add(&std_b, "pool/b", "p", 1, 0);       /* sibling: remains separately scoped */
        batch_add(&std_b, "otherpool/x", "p", 1, 0);
        batch_add(&rec_b, "pool", "p", 1, 0);          /* ancestor covers pool/a and pool/b */
        batch_add(&rec_b, "thirdpool/y/z", "p", 1, 0);

        root_list_t due_roots = {0};
        int rc = collect_due_roots(&due_roots, &std_b, &rec_b);
        CHECK(rc == 0, "collect_due_roots succeeds");
        CHECK(due_roots.count == 3, "three ancestor-minimal configured paths remain across both batches");

        int has_pool = 0, has_other = 0, has_third = 0;
        for (size_t i = 0; i < due_roots.count; i++) {
            if (strcmp(due_roots.roots[i], "pool") == 0) has_pool = 1;
            if (strcmp(due_roots.roots[i], "otherpool/x") == 0) has_other = 1;
            if (strcmp(due_roots.roots[i], "thirdpool/y/z") == 0) has_third = 1;
        }
        CHECK(has_pool && has_other && has_third, "all three full scoped dataset paths are present");

        root_list_free(&due_roots);
        batch_free(&std_b);
        batch_free(&rec_b);
        printf("\n");
    }

    printf("== Test 7: batch_filter_by_metrics -- found/valid/above-threshold items are kept with .written cached ==\n");
    {
        batch_ctx_t b = {0};
        batch_add(&b, "pool/a", "p", 1, 100);   /* min_bytes=100 */

        metric_ctx_t metrics = {0};
        metrics.items = calloc(1, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/a");
        metrics.items[0].written = 5000;
        metrics.count = 1;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, 0, &global_status);
        CHECK(b.count == 1, "item is kept (found, valid, written >= min_bytes)");
        CHECK(b.items[0].written == 5000, ".written is cached from the metric lookup");
        CHECK(global_status == 0, "no error flagged");

        free(metrics.items);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 8: batch_filter_by_metrics -- dataset not found is removed and flagged ==\n");
    {
        batch_ctx_t b = {0};
        batch_add(&b, "pool/missing", "p", 1, 0);

        metric_ctx_t metrics = {0}; /* empty: nothing matches */
        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, 0, &global_status);
        CHECK(b.count == 0, "not-found item is removed from the batch");
        CHECK(global_status == 1, "global_status is flagged (matches old inline-check behavior)");

        batch_free(&b);
        printf("\n");
    }

    printf("== Test 9: batch_filter_by_metrics -- invalid (-1) written metric is removed and flagged ==\n");
    {
        batch_ctx_t b = {0};
        batch_add(&b, "pool/a", "p", 1, 0);

        metric_ctx_t metrics = {0};
        metrics.items = calloc(1, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/a");
        metrics.items[0].written = -1; /* invalid */
        metrics.count = 1;

        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, 0, &global_status);
        CHECK(b.count == 0, "invalid-metric item is removed");
        CHECK(global_status == 1, "global_status is flagged");

        free(metrics.items);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 10: batch_filter_by_metrics -- below min_bytes is removed SILENTLY (no error flag) ==\n");
    {
        batch_ctx_t b = {0};
        batch_add(&b, "pool/a", "p", 1, 999999); /* high min_bytes threshold */

        metric_ctx_t metrics = {0};
        metrics.items = calloc(1, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/a");
        metrics.items[0].written = 100; /* below threshold */
        metrics.count = 1;

        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, 0, &global_status);
        CHECK(b.count == 0, "below-threshold item is removed");
        CHECK(global_status == 0, "global_status is NOT flagged (this is a normal skip, not an error)");

        batch_add(&b, "pool/a", "p", 1, 100);
        global_status = 0;
        batch_filter_by_metrics(&b, &metrics, 0, &global_status);
        CHECK(b.count == 1 && b.items[0].written == 100,
              "written == min_bytes is retained at the inclusive threshold boundary");
        CHECK(global_status == 0, "an exactly-at-threshold item is not an error");

        free(metrics.items);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 11: batch_filter_by_metrics -- mixed batch, compaction preserves surviving items correctly ==\n");
    {
        batch_ctx_t b = {0};
        batch_add(&b, "pool/keep1", "p", 1, 0);
        batch_add(&b, "pool/notfound", "p", 1, 0);
        batch_add(&b, "pool/keep2", "p", 1, 0);
        batch_add(&b, "pool/belowthresh", "p", 1, 999999);
        batch_add(&b, "pool/keep3", "p", 1, 0);

        metric_ctx_t metrics = {0};
        metrics.items = calloc(4, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/keep1"); metrics.items[0].written = 100;
        strcpy(metrics.items[1].name, "pool/keep2"); metrics.items[1].written = 200;
        strcpy(metrics.items[2].name, "pool/keep3"); metrics.items[2].written = 300;
        strcpy(metrics.items[3].name, "pool/belowthresh"); metrics.items[3].written = 5;
        metrics.count = 4;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, 0, &global_status);
        CHECK(b.count == 3, "exactly 3 of 5 items survive (2 removed: not-found, below-threshold)");
        int has_keep1 = 0, has_keep2 = 0, has_keep3 = 0;
        long long w1 = 0, w2 = 0, w3 = 0;
        for (size_t i = 0; i < b.count; i++) {
            if (strcmp(b.items[i].dataset, "pool/keep1") == 0) { has_keep1 = 1; w1 = b.items[i].written; }
            if (strcmp(b.items[i].dataset, "pool/keep2") == 0) { has_keep2 = 1; w2 = b.items[i].written; }
            if (strcmp(b.items[i].dataset, "pool/keep3") == 0) { has_keep3 = 1; w3 = b.items[i].written; }
        }
        CHECK(has_keep1 && has_keep2 && has_keep3, "all three surviving items are the correct ones");
        CHECK(w1 == 100 && w2 == 200 && w3 == 300, "each surviving item's .written matches its own metric, not a neighbor's (compaction didn't scramble anything)");
        CHECK(global_status == 1, "global_status flagged once for the not-found item");

        free(metrics.items);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 12: sum_subtree_written -- sums root + nested descendants at multiple depths ==\n");
    {
        metric_ctx_t metrics = {0};
        metrics.items = calloc(4, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/data");           metrics.items[0].written = 1000;
        strcpy(metrics.items[1].name, "pool/data/child");     metrics.items[1].written = 2000;
        strcpy(metrics.items[2].name, "pool/data/child/gc");  metrics.items[2].written = 3000; /* grandchild */
        strcpy(metrics.items[3].name, "pool/data/child2");    metrics.items[3].written = 4000;
        metrics.count = 4;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        long long sum = -999;
        int rc = sum_subtree_written(&metrics, "pool/data", &sum);
        CHECK(rc == 0, "sum_subtree_written succeeds for a subtree with nested descendants");
        CHECK(sum == 10000, "sum includes root + all descendants at every depth (1000+2000+3000+4000)");

        free(metrics.items);
        printf("\n");
    }

    printf("== Test 12a: sum_subtree_written rejects totals above LLONG_MAX ==\n");
    {
        metric_ctx_t metrics = {0};
        metrics.items = calloc(2, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/data");       metrics.items[0].written = LLONG_MAX;
        strcpy(metrics.items[1].name, "pool/data/child"); metrics.items[1].written = 1;
        metrics.count = 2;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);
        long long sum = 0;
        CHECK(sum_subtree_written(&metrics, "pool/data", &sum) == -1,
              "recursive written-byte accumulation fails instead of overflowing LLONG_MAX");
        free(metrics.items);
        printf("\n");
    }

    printf("== Test 13: sum_subtree_written -- lexicographic sibling ordering does not corrupt the sum ==\n");
    {
        /*
         * '-' and '.' both sort before '/' in ASCII, so siblings like
         * "pool/data-old" and "pool/data.bak" sort BETWEEN "pool/data" and
         * "pool/data/child" in the metrics array. A naive
         * scan-forward-from-bsearch-hit approach would either include
         * these siblings' written bytes in "pool/data"'s subtree sum (if it
         * didn't check the boundary at all) or stop too early once it hit
         * the first non-descendant (missing pool/data/child entirely). The
         * lower-bound search for "pool/data/" plus the is_strict_descendant
         * boundary check must get this right regardless of what sorts in
         * between.
         */
        metric_ctx_t metrics = {0};
        metrics.items = calloc(4, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/data");        metrics.items[0].written = 100;
        strcpy(metrics.items[1].name, "pool/data-old");    metrics.items[1].written = 999999; /* sibling, sorts before pool/data/child */
        strcpy(metrics.items[2].name, "pool/data.bak");    metrics.items[2].written = 888888; /* sibling, sorts before pool/data/child */
        strcpy(metrics.items[3].name, "pool/data/child");  metrics.items[3].written = 200;
        metrics.count = 4;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        long long sum = -999;
        int rc = sum_subtree_written(&metrics, "pool/data", &sum);
        CHECK(rc == 0, "sum_subtree_written succeeds despite lexicographically-interleaved siblings");
        CHECK(sum == 300, "siblings 'pool/data-old' and 'pool/data.bak' are excluded from the sum (100+200, not +999999+888888)");

        free(metrics.items);
        printf("\n");
    }

    printf("== Test 14: sum_subtree_written -- invalid (-1) descendant is excluded and logged, not fatal ==\n");
    {
        metric_ctx_t metrics = {0};
        metrics.items = calloc(3, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/data");        metrics.items[0].written = 100;
        strcpy(metrics.items[1].name, "pool/data/child1"); metrics.items[1].written = -1; /* invalid */
        strcpy(metrics.items[2].name, "pool/data/child2"); metrics.items[2].written = 200;
        metrics.count = 3;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        long long sum = -999;
        int rc = sum_subtree_written(&metrics, "pool/data", &sum);
        CHECK(rc == 0, "an invalid descendant does NOT void the whole subtree sum");
        CHECK(sum == 300, "invalid descendant's bytes are excluded from the sum (100+200, not counting child1)");

        free(metrics.items);
        printf("\n");
    }

    printf("== Test 15: sum_subtree_written -- invalid (-1) value on the ROOT itself IS fatal ==\n");
    {
        metric_ctx_t metrics = {0};
        metrics.items = calloc(2, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/data");        metrics.items[0].written = -1; /* invalid root */
        strcpy(metrics.items[1].name, "pool/data/child");  metrics.items[1].written = 200;
        metrics.count = 2;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        long long sum = -999;
        int rc = sum_subtree_written(&metrics, "pool/data", &sum);
        CHECK(rc == -1, "an invalid value on the root dataset itself fails the whole call (unlike a bad descendant)");

        free(metrics.items);
        printf("\n");
    }

    printf("== Test 16: sum_subtree_written -- root missing from metrics entirely is fatal ==\n");
    {
        metric_ctx_t metrics = {0}; /* empty */
        long long sum = -999;
        int rc = sum_subtree_written(&metrics, "pool/data", &sum);
        CHECK(rc == -1, "a root dataset absent from the metrics array fails the call");
        printf("\n");
    }

    printf("== Test 17: sum_subtree_written -- leaf dataset with no descendants sums to just its own value ==\n");
    {
        metric_ctx_t metrics = {0};
        metrics.items = calloc(1, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/leaf"); metrics.items[0].written = 42;
        metrics.count = 1;

        long long sum = -999;
        int rc = sum_subtree_written(&metrics, "pool/leaf", &sum);
        CHECK(rc == 0, "a leaf with no descendants still succeeds");
        CHECK(sum == 42, "sum is exactly the root's own written value");

        free(metrics.items);
        printf("\n");
    }

    printf("== Test 18: batch_filter_by_metrics(recursive=1) -- quiescent parent kept because a descendant is active ==\n");
    {
        /*
         * This is the actual bug the recursive/min_bytes fix addresses:
         * a recursive entry whose named (parent) dataset is itself
         * near-idle must still be snapshotted if activity happened
         * anywhere in its subtree, since a recursive snapshot covers the
         * whole tree.
         */
        batch_ctx_t b = {0};
        batch_add(&b, "pool/parent", "p", 1, 5000); /* min_bytes=5000 */

        metric_ctx_t metrics = {0};
        metrics.items = calloc(2, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/parent");       metrics.items[0].written = 10;   /* parent itself is quiescent */
        strcpy(metrics.items[1].name, "pool/parent/child"); metrics.items[1].written = 9000; /* child is very active */
        metrics.count = 2;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, 1, &global_status);
        CHECK(b.count == 1, "recursive entry is KEPT: subtree total (10+9000) clears min_bytes even though the parent alone would not");
        if (b.count == 1) CHECK(b.items[0].written == 9010, ".written is cached as the full subtree sum, not just the parent's own value");
        CHECK(global_status == 0, "no error flagged");

        free(metrics.items);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 19: batch_filter_by_metrics(recursive=1) -- whole subtree quiescent is still skipped silently ==\n");
    {
        batch_ctx_t b = {0};
        batch_add(&b, "pool/parent", "p", 1, 5000);

        metric_ctx_t metrics = {0};
        metrics.items = calloc(2, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/parent");       metrics.items[0].written = 10;
        strcpy(metrics.items[1].name, "pool/parent/child"); metrics.items[1].written = 20;
        metrics.count = 2;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, 1, &global_status);
        CHECK(b.count == 0, "recursive entry is dropped: subtree total (30) is still below min_bytes (5000)");
        CHECK(global_status == 0, "global_status is NOT flagged (normal skip, not an error, same as the non-recursive case)");

        free(metrics.items);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 20: exec_cmd_stream_lenient treats a total execv failure as a hard failure, not lenient success ==\n");
    {
        /*
         * Test 2 above proves the lenient wrapper does what it was built
         * for: tolerate a nonzero exit from a target that DID run (one bad
         * root among several). That is a different failure mode from the
         * one here: argv[0] pointing at something that can't be executed
         * at all (bad ZFS_PATH, permissions, binary removed). In that
         * case the forked child hits _exit(EXIT_EXEC_FAILED) before ever
         * running real zfs logic. Before the fix, exec_cmd_stream_lenient
         * only checked WIFEXITED (true here) and ignored the exit status
         * entirely, so this was indistinguishable from lenient success --
         * the scoped `zfs get` call would silently return zero metrics,
         * and every due dataset would then get logged as "Configured
         * dataset not found" instead of one clear top-level error.
         */
        const char *const argv[] = {"/nonexistent/path/definitely/not/a/real/binary", NULL};
        int strict_rc = exec_cmd_stream(argv, NULL, NULL);
        int lenient_rc = exec_cmd_stream_lenient(argv, NULL, NULL);
        CHECK(strict_rc != 0, "strict: execv failure is treated as failure (baseline, unaffected by the fix)");
        CHECK(lenient_rc != 0, "lenient: execv failure is ALSO treated as failure (the bug fix under test -- must not be conflated with a tolerable nonzero zfs exit)");
        printf("\n");
    }

    printf("== Test 21: handle_metric_line preserves a space embedded in the dataset name (tab-only delimiter) ==\n");
    {
        /*
         * ZFS dataset names are permitted to contain spaces. Before the
         * fix, handle_metric_line tokenized zfs get output on " \t"
         * (space OR tab), so a space embedded in the dataset name itself
         * was indistinguishable from the name/value column separator --
         * the name would be truncated at the space and the remainder
         * would corrupt/misalign the written-value field. Tokenizing on
         * tab only (the actual column separator `zfs get -H` uses) is
         * required to parse these lines correctly.
         */
        const char *const sh_candidates[] = {"/bin/sh", "/usr/bin/sh", NULL};
        const char *sh_bin = find_bin(sh_candidates);
        CHECK(sh_bin != NULL, "found a POSIX shell");
        if (sh_bin) {
            metric_ctx_t ctx = {0};
            const char *const argv[] = {sh_bin, "-c", "printf 'pool/my dataset\\t54321\\n'", NULL};
            int rc = exec_cmd_stream(argv, handle_metric_line, &ctx);
            CHECK(rc == 0, "strict call with echo succeeds");
            CHECK(ctx.count == 1, "handler received exactly one parsed line");
            if (ctx.count == 1) {
                CHECK(strcmp(ctx.items[0].name, "pool/my dataset") == 0, "dataset name with an embedded space is parsed intact, not split at the space");
                CHECK(ctx.items[0].written == 54321, "written value parsed correctly despite the space earlier in the line");
            }
            free(ctx.items);
        }
        printf("\n");
    }

    printf("== Test 21a: malformed metric rows are skipped and logged ==\n");
    {
        metric_ctx_t metrics = {0};
        char *log_buf = NULL; size_t log_len = 0;
        log_fp = open_memstream(&log_buf, &log_len);
        CHECK(log_fp != NULL, "opened a log capture for malformed metric rows");
        if (log_fp) {
            CHECK(handle_metric_line("pool/no-tab", &metrics) == 0 &&
                  handle_metric_line("pool/extra\t1\textra", &metrics) == 0 &&
                  handle_metric_line("\t1", &metrics) == 0 &&
                  handle_metric_line("pool/empty\t", &metrics) == 0,
                  "malformed metric rows are skipped without failing the command stream");
            fflush(log_fp);
            CHECK(metrics.count == 0 && strstr(log_buf, "missing tab delimiter") &&
                  strstr(log_buf, "unexpected fields") && strstr(log_buf, "empty dataset or written value"),
                  "missing, extra, and empty metric fields each produce a diagnostic log entry");
            fclose(log_fp); log_fp = NULL;
        }
        free(log_buf); free(metrics.items);
        printf("\n");
    }

    printf("== Test 22: stream_reader_consume marks the reader failed BEFORE flushing an overlong line, so handler() never sees truncated data ==\n");
    {
        /*
         * Before the fix, an overlong (>STR_BUF_XLARGE-1 byte, no embedded
         * newline) chunk was flushed to handler() and only marked the
         * reader failed AFTER that call -- a truncated line was briefly
         * treated as a complete one before the failure was recorded. The
         * fix reorders this so `failed` is set first. Calling
         * stream_reader_consume() directly (the real static function, no
         * shim needed) with a spy handler proves the truncated content is
         * never handed off, regardless of how the caller might otherwise
         * observe reader->failed.
         */
        g_spy_calls = 0;
        g_spy_last_line[0] = '\0';

        stream_reader_t reader = {0};
        reader.is_stderr = 0;
        reader.handler = spy_line_handler;
        reader.data = NULL;

        /* 600 bytes of 'A', no newline anywhere -- exceeds
         * STR_BUF_XLARGE-1 (511) usable bytes, forcing the overflow
         * branch inside stream_reader_consume to fire mid-line. */
        char oversized[600];
        memset(oversized, 'A', sizeof(oversized));

        stream_reader_consume(&reader, oversized, (ssize_t)sizeof(oversized));

        CHECK(reader.failed == 1, "reader is marked failed once an overlong no-newline chunk overflows the buffer");
        CHECK(g_spy_calls == 0, "handler() was never invoked with the truncated (overflowed) line content");

        /* Baseline contrast: an ordinary, well-formed line through the
         * same function still reaches the handler normally. */
        stream_reader_t ok_reader = {0};
        ok_reader.is_stderr = 0;
        ok_reader.handler = spy_line_handler;
        ok_reader.data = NULL;
        g_spy_calls = 0;
        const char *normal_line = "pool/child\t12345\n";
        stream_reader_consume(&ok_reader, normal_line, (ssize_t)strlen(normal_line));
        CHECK(ok_reader.failed == 0, "an ordinary in-bounds line does not mark the reader failed");
        CHECK(g_spy_calls == 1, "handler() IS invoked normally for a well-formed, in-bounds line");
        CHECK(strcmp(g_spy_last_line, "pool/child\t12345") == 0, "handler() receives the complete, untruncated line content");

        printf("\n");
    }

    printf("== Test 22a: snapshot inventory accepts exactly one complete snapshot name per line ==\n");
    {
        name_list_t inventory = {0};
        char *log_buf = NULL; size_t log_len = 0;
        log_fp = open_memstream(&log_buf, &log_len);
        CHECK(log_fp != NULL, "opened a log capture for invalid snapshot inventory rows");
        if (log_fp) {
            CHECK(handle_snapshot_inventory_line("pool/ds@snap", &inventory) == 0 &&
                      handle_snapshot_inventory_line("pool/ds", &inventory) == -1 &&
                      handle_snapshot_inventory_line("pool/ds@snap\textra", &inventory) == -1 &&
                      handle_snapshot_inventory_line("pool/ds@snap@extra", &inventory) == -1,
                  "an inventory row must contain exactly one complete snapshot name");
            fflush(log_fp);
            CHECK(inventory.count == 1 && strstr(log_buf, "Invalid snapshot inventory line"),
                  "invalid inventory rows are logged and rejected before they enter pruning");
            fclose(log_fp); log_fp = NULL;
        }
        free(log_buf);
        name_list_free(&inventory);
        printf("\n");
    }

    printf("== Test 22b: zfs_destroy passes -r for recursive destruction ==\n");
    {
        char trace_path[PATH_MAX];
        CHECK(snprintf(trace_path, sizeof(trace_path), "%s/destroy-args", g_fake_zfs_dir) < (int)sizeof(trace_path),
              "recursive-destroy argv trace path fits in the isolated test directory");
        char script[PATH_MAX + 128];
        CHECK(snprintf(script, sizeof(script), "#!/bin/sh\nprintf '%%s\\n' \"$@\" >> '%s'\n", trace_path) < (int)sizeof(script) &&
                  write_fake_zfs(script) == 0,
              "installed fake zfs that records recursive destroy argv");
        CHECK(zfs_destroy("pool/ds@snap", 1) == 0, "recursive zfs_destroy succeeds through the fake zfs command");
        FILE *trace = fopen(trace_path, "r");
        char contents[256] = {0};
        if (trace) { size_t rd = fread(contents, 1, sizeof(contents) - 1, trace); (void)rd; fclose(trace); }
        CHECK(strstr(contents, "destroy\n") && strstr(contents, "-r\n") && strstr(contents, "pool/ds@snap\n"),
              "recursive zfs_destroy passes the -r argument to zfs");
        unlink(trace_path);
        unlink(g_fake_zfs);
        printf("\n");
    }

    printf("== Test 23: prune_from_inventory's date_stamp_like gate excludes prefix-matching but non-date-shaped snapshot names ==\n");
    {
        /* This test owns its fake command: ordinary destroy calls succeed
         * without touching real ZFS. */
        CHECK(write_fake_zfs("#!/bin/sh\nexit 0\n") == 0,
              "fake zfs script created outside the system ZFS path for Test 23");

        /* Capture log_msg() output so we can see exactly what got pruned,
         * the same technique test_localtime_standalone.c uses. */
        char *buf = NULL;
        size_t buf_len = 0;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for capturing log_msg output");

        name_list_t inventory = {0};
        /* Newest-first, matching the real "-S creation" ordering that
         * load_combined_snapshot_inventory hands to this function. The
         * middle entry matches the prefix_ literal but is NOT date-shaped
         * (this is exactly the case date_stamp_like exists to reject). */
        handle_snapshot_inventory_line("pool/ds@myprefix_2026-01-03_00:00:00", &inventory);
        handle_snapshot_inventory_line("pool/ds@myprefix_garbage-not-a-date", &inventory);
        handle_snapshot_inventory_line("pool/ds@myprefix_2026-01-01_00:00:00", &inventory);
        CHECK(inventory.count == 3, "all three inventory lines were recorded");

        char **matches = NULL;
        size_t matches_cap = 0;
        /*
         * max_snaps=1 with 2 genuinely date-stamped entries means exactly
         * one prune (the older one). If the malformed entry were wrongly
         * treated as a match too, match_count would be 3 instead of 2 and
         * pruning would fire TWICE -- destroying the malformed "snapshot"
         * along with a real one, exactly the false-Created-line/false-prune
         * failure mode described when this gap was first identified.
         */
        int rc = prune_from_inventory(&inventory, "pool/ds", "myprefix", 1, 0, &matches, &matches_cap);
        CHECK(rc == 0, "prune_from_inventory reports success");

        fflush(log_fp);
        int pruned_count = 0;
        if (buf) {
            const char *p = buf;
            while ((p = strstr(p, "Pruned=")) != NULL) { pruned_count++; p += 7; }
        }
        CHECK(pruned_count == 1, "exactly one snapshot was pruned -- the malformed-name entry was never counted as a match");
        CHECK(buf != NULL && strstr(buf, "myprefix_garbage-not-a-date") == NULL,
              "the non-date-shaped entry was never selected for pruning");
        CHECK(buf != NULL && strstr(buf, "myprefix_2026-01-01_00:00:00") != NULL,
              "the oldest genuinely date-stamped entry is the one that was pruned, leaving the newest one retained");

        if (log_fp) fclose(log_fp);
        log_fp = NULL;
        free(buf);
        free(matches); /* matches holds borrowed pointers into inventory -- only the array itself is ours to free */
        name_list_free(&inventory);
        unlink(g_fake_zfs);
        printf("\n");
    }

    printf("== Test 24: zfs_snapshot_batch isolates failure to the failing root -- one root's failure does not block or falsely fail a sibling root ==\n");
    {
        /* Install our own fake command so this test has no ordering
         * dependency on Test 23. Its genuine nonzero failure also emits
         * stderr, which exec_cmd_stream must drain and route to log_msg. */
        CHECK(write_fake_zfs("#!/bin/sh\ncase \"$*\" in\n  *badroot*) echo 'badroot diagnostic' >&2; exit 1 ;;\n  *) exit 0 ;;\nesac\n") == 0,
              "fake zfs script created outside the system ZFS path for Test 24");
        batch_ctx_t ctx = {0};
        int rc1 = batch_add(&ctx, "badroot/x", "p", 1, 0);
        int rc2 = batch_add(&ctx, "goodroot/y", "p", 1, 0);
        CHECK(rc1 == 0 && rc2 == 0, "batch_add succeeded for both items during setup");

        char *buf = NULL;
        size_t buf_len = 0;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for Test 24 stderr capture");
        int rc = zfs_snapshot_batch(&ctx, 0, "2026-01-01_00:00:00");
        if (log_fp) fflush(log_fp);

        CHECK(rc == -1, "zfs_snapshot_batch reports overall failure because one root's snapshot call failed");
        CHECK(ctx.items[0].snap_failed == 1, "the item under the failing root (badroot) is marked snap_failed");
        CHECK(ctx.items[1].snap_failed == 0,
              "the item under the OTHER, healthy root (goodroot) is NOT marked failed -- one root's failure does not leak onto a sibling root");
        CHECK(buf != NULL && strstr(buf, "badroot diagnostic") != NULL,
              "stderr from the genuine nonzero fake-zfs failure is drained and logged");

        batch_free(&ctx);
        if (log_fp) fclose(log_fp);
        log_fp = NULL;
        free(buf);
        unlink(g_fake_zfs); /* done with the fake zfs script now */
        printf("\n");
    }

    printf("== Test 25: trim_trailing_whitespace and copy_token handle their edge cases correctly ==\n");
    {
        char s1[] = "hello world   ";
        trim_trailing_whitespace(s1);
        CHECK(strcmp(s1, "hello world") == 0, "trailing spaces are trimmed");

        char s2[] = "no trailing ws";
        trim_trailing_whitespace(s2);
        CHECK(strcmp(s2, "no trailing ws") == 0, "a string with no trailing whitespace is left unchanged");

        char s3[] = "tabs and crlf\t\r\n";
        trim_trailing_whitespace(s3);
        CHECK(strcmp(s3, "tabs and crlf") == 0, "trailing tabs, CR, and LF are all trimmed together, not just plain spaces");

        char s4[] = "   ";
        trim_trailing_whitespace(s4);
        CHECK(strcmp(s4, "") == 0, "an all-whitespace string is trimmed down to empty, not left partially intact");

        char dst[8];
        CHECK(copy_token(dst, sizeof(dst), "short") == 0, "copy_token succeeds when the source fits with room to spare");
        CHECK(strcmp(dst, "short") == 0, "copy_token copies the exact source content");

        CHECK(copy_token(dst, sizeof(dst), "1234567") == 0, "copy_token succeeds when source length is exactly dst_size-1 (leaves exactly enough room for the NUL)");
        CHECK(strcmp(dst, "1234567") == 0, "copy_token's boundary-fitting copy is byte-exact");

        char before[8];
        memcpy(before, dst, sizeof(before));
        CHECK(copy_token(dst, sizeof(dst), "12345678") == -1, "copy_token rejects a source that is exactly dst_size long (no room left for the NUL terminator)");
        CHECK(memcmp(dst, before, sizeof(before)) == 0, "dst is left completely untouched when copy_token rejects an oversized source");
        printf("\n");
    }

    printf("== Test 26: seen_set_add detects an exact (dataset, prefix) duplicate without false-positiving on a partial match ==\n");
    {
        seen_set_t seen = {0};
        int rc1 = seen_set_add(&seen, "pool/ds", "myprefix");
        CHECK(rc1 == 0, "first insertion of a (dataset, prefix) pair succeeds and is reported as new");
        CHECK(seen.count == 1, "the set now holds exactly one key");

        int rc2 = seen_set_add(&seen, "pool/ds", "myprefix");
        CHECK(rc2 == 1, "re-inserting the exact same (dataset, prefix) pair is reported as an already-seen duplicate");
        CHECK(seen.count == 1, "the duplicate insertion did not grow the set");

        /* Same dataset, different prefix: diffsnap explicitly allows
         * multiple prefixes per dataset, so this must NOT be a duplicate. */
        int rc3 = seen_set_add(&seen, "pool/ds", "otherprefix");
        CHECK(rc3 == 0, "the same dataset with a different prefix is treated as a distinct, new entry");
        CHECK(seen.count == 2, "the set grew to two keys after the genuinely distinct entry");

        /* Different dataset, same prefix: also must NOT be a duplicate. */
        int rc4 = seen_set_add(&seen, "pool/other", "myprefix");
        CHECK(rc4 == 0, "a different dataset with the same prefix is treated as a distinct, new entry");
        CHECK(seen.count == 3, "the set grew to three keys after the second genuinely distinct entry");

        seen_set_free(&seen);
        printf("\n");
    }

    printf("== Test 27: format_bytes boundary and edge-case formatting ==\n");
    {
        char buf[32];

        format_bytes(0, buf, sizeof(buf));
        CHECK(strcmp(buf, "0") == 0, "zero bytes formats as plain \"0\"");

        format_bytes(-500, buf, sizeof(buf));
        CHECK(strcmp(buf, "0") == 0, "a negative byte count defensively also formats as \"0\", not a negative or garbage value");

        format_bytes(1023, buf, sizeof(buf));
        CHECK(strcmp(buf, "1023") == 0, "one byte under the 1024 threshold stays in plain decimal bytes, no unit suffix");

        format_bytes(1024, buf, sizeof(buf));
        CHECK(strcmp(buf, "1.00K") == 0, "exactly 1024 bytes crosses into the K unit");

        format_bytes(1024LL * 1024, buf, sizeof(buf));
        CHECK(strcmp(buf, "1.00M") == 0, "exactly 1 MiB formats with the M unit");

        format_bytes(1024LL * 1024 * 1024 * 1024 * 1024, buf, sizeof(buf));
        CHECK(strcmp(buf, "1.00P") == 0, "the largest unit (P) is used for petabyte-scale values, with no further division past it");

        format_bytes(1024LL * 1024 * 1024 * 1024 * 1024 * 1024, buf, sizeof(buf));
        CHECK(strcmp(buf, "1.00E") == 0, "exactly 1 EiB formats with the new exabyte unit");
        printf("\n");
    }

    printf("== Test 28: compare_order_entry sorts strictly by len, treating equal-len entries as tied ==\n");
    {
        order_entry_t entries[5] = {
            { .idx = 0, .len = 30 },
            { .idx = 1, .len = 10 },
            { .idx = 2, .len = 20 },
            { .idx = 3, .len = 10 },
            { .idx = 4, .len = 5  },
        };
        qsort(entries, 5, sizeof(entries[0]), compare_order_entry);

        CHECK(entries[0].len == 5, "the shortest entry sorts first");
        CHECK(entries[4].len == 30, "the longest entry sorts last");
        int nondecreasing = 1;
        for (size_t i = 0; i + 1 < 5; i++) if (entries[i].len > entries[i + 1].len) nondecreasing = 0;
        CHECK(nondecreasing, "the whole array ends up in non-decreasing len order");

        /* Direct comparator contract, independent of qsort's algorithm
         * choice: equal-length entries compare as tied, and the comparator
         * is antisymmetric for unequal lengths. */
        order_entry_t a = { .idx = 0, .len = 10 };
        order_entry_t b = { .idx = 1, .len = 10 };
        order_entry_t c = { .idx = 2, .len = 20 };
        CHECK(compare_order_entry(&a, &b) == 0, "two entries with equal len compare as tied, regardless of differing idx");
        CHECK(compare_order_entry(&a, &c) < 0, "a shorter entry compares less than a longer one");
        CHECK(compare_order_entry(&c, &a) > 0, "the comparison is antisymmetric: the longer entry compares greater than the shorter one");
        printf("\n");
    }

    printf("== Test 29: valid_dataset enforces the configured naming grammar ==\n");
    {
        CHECK(valid_dataset("pool/child-set.1:two three") == 1,
              "letters, numbers, underscore, dot, colon, space, hyphen, and single separators are accepted");
        CHECK(valid_dataset("1pool/child") == 0, "a dataset must start with a letter");
        CHECK(valid_dataset("pool//child") == 0, "repeated separators are rejected");
        CHECK(valid_dataset("pool/child/") == 0, "a trailing separator is rejected");
        CHECK(valid_dataset("pool/child$") == 0, "characters outside the allowed set are rejected");
        CHECK(date_stamp_like("2026-11-01_01:30:00p0500") == 1,
              "the sanitized positive offset used in new snapshot names is accepted for pruning");
        CHECK(date_stamp_like("2026-11-01_01:30:00+0500") == 0,
              "an invalid plus-sign offset is rejected");
        CHECK(date_stamp_like("2026-11-01_01:30:00-0500") == 1,
              "negative offsets remain accepted for pruning");
        CHECK(date_stamp_like("2026-11-01_01:30:00-05x0") == 0,
              "a malformed timezone offset is rejected");

        /*
         * Previously this whole sub-test was wrapped in
         * `if (write_fake_zfs(...) == 0) { CHECK(1, "..."); ... }`: the
         * CHECK(1,...) could never fail (it's only reached once the `if`
         * already proved the condition true), and if write_fake_zfs DID
         * fail, every assertion below was silently skipped with no
         * CHECK(0,...) ever recorded. Asserting the setup directly --
         * the same idiom used elsewhere in this file (e.g. Test 22b) --
         * makes a setup failure a loud, counted failure instead of a
         * silent drop in coverage, and lets the rest of the block run
         * unconditionally so any knock-on failures are visible too.
         */
        char inventory_script[PATH_MAX + 64];
        CHECK(snprintf(inventory_script, sizeof(inventory_script),
                       "#!/bin/sh\nprintf '%%s\\n' \"$@\" > '%s'\n", g_inventory_args) < (int)sizeof(inventory_script) &&
              write_fake_zfs(inventory_script) == 0,
              "installed fake zfs script for inventory-root scoping tests");

        batch_ctx_t std_b = {0}, rec_b = {0};
        batch_add(&std_b, "poolA/child", "p", 1, 0);
        batch_add(&rec_b, "poolB/child", "p", 1, 0);
        name_list_t inventory = {0};
        CHECK(load_combined_snapshot_inventory(&inventory, &std_b, &rec_b) == 0,
              "inventory loading succeeds for multiple roots through the fake zfs command");
        FILE *args_fp = fopen(g_inventory_args, "r");
        char args[1024] = {0};
        if (args_fp) { size_t rd = fread(args, 1, sizeof(args) - 1, args_fp); (void)rd; fclose(args_fp); }
        CHECK(strstr(args, "-r\n") != NULL && strstr(args, "poolA/child\n") != NULL && strstr(args, "poolB/child\n") != NULL,
              "multi-root inventory list is scoped recursively to every distinct configured subtree");
        CHECK(strstr(args, "poolA\n") == NULL && strstr(args, "poolB\n") == NULL,
              "inventory list preserves configured descendants instead of truncating them to pool names");
        name_list_free(&inventory);
        batch_free(&std_b); batch_free(&rec_b);
        std_b = (batch_ctx_t){0};
        rec_b = (batch_ctx_t){0};

        for (size_t i = 0; i < 600; i++) {
            char root[STR_BUF_LARGE];
            snprintf(root, sizeof(root), "p%03zu%.*s", i, 250, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
            batch_add(&std_b, root, "p", 1, 0);
        }
        CHECK(load_combined_snapshot_inventory(&inventory, &std_b, &rec_b) == 0,
              "inventory loading falls back successfully when root arguments exceed ARGV_BYTES_CAP");
        args_fp = fopen(g_inventory_args, "r");
        memset(args, 0, sizeof(args));
        if (args_fp) { size_t rd = fread(args, 1, sizeof(args) - 1, args_fp); (void)rd; fclose(args_fp); }
        CHECK(strstr(args, "-r\n") == NULL && strstr(args, "p000") == NULL,
              "oversized multi-root inventory call falls back to unscoped zfs list");
        name_list_free(&inventory);
        batch_free(&std_b); batch_free(&rec_b);
        unlink(g_fake_zfs);
        printf("\n");
    }

    printf("== Test 30: drain_command_streams (poll()-based) drains concurrent, interleaved stdout+stderr without hanging or dropping data ==\n");
    {
        /*
         * Regression coverage for the select()->poll() rewrite of
         * drain_command_streams. select()'s fd_set/FD_ISSET bookkeeping
         * was replaced with a struct pollfd[2] array; this exercises the
         * real subprocess path (not a direct stream_reader_consume() call
         * like Test 22) so both fds are genuinely ready/not-ready across
         * many poll() wakeups, not just fed a single in-memory buffer.
         */
        const char *const sh_candidates[] = {"/bin/sh", "/usr/bin/sh", NULL};
        const char *sh_bin = find_bin(sh_candidates);
        CHECK(sh_bin != NULL, "found a shell for the concurrent stdout/stderr test");
        if (sh_bin) {
            char *buf = NULL;
            size_t buf_len = 0;
            log_fp = open_memstream(&buf, &buf_len);
            CHECK(log_fp != NULL, "open_memstream succeeded for capturing stderr-routed log output");

            metric_ctx_t ctx = {0};
            const char *const argv[] = {sh_bin, "-c",
                "i=1; while [ $i -le 40 ]; do printf 'pool/ds%d\\t%d\\n' $i $((i*10)); "
                "echo \"stderr-$i\" >&2; i=$((i+1)); done", NULL};
            int rc = exec_cmd_stream(argv, handle_metric_line, &ctx);
            fflush(log_fp);

            CHECK(rc == 0, "the concurrent stdout+stderr command succeeds");
            CHECK(ctx.count == 40, "all 40 stdout lines were delivered to the handler through the poll()-driven drain, none dropped");
            int has_first = 0, has_last = 0;
            for (size_t i = 0; i < ctx.count; i++) {
                if (strcmp(ctx.items[i].name, "pool/ds1") == 0) has_first = 1;
                if (strcmp(ctx.items[i].name, "pool/ds40") == 0) has_last = 1;
            }
            CHECK(has_first && has_last, "both the first and last stdout lines survived interleaving with concurrent stderr output");
            CHECK(buf != NULL && strstr(buf, "stderr-1") != NULL && strstr(buf, "stderr-40") != NULL,
                  "stderr output interleaved with stdout was also fully drained and logged, not starved by the stdout side of poll()");

            if (log_fp) fclose(log_fp);
            log_fp = NULL;
            free(buf);
            free(ctx.items);
        }
        printf("\n");
    }

    printf("== Test 31: one stream closing early does not stall or truncate draining of the other (poll() per-fd open-flag independence) ==\n");
    {
        const char *const sh_candidates[] = {"/bin/sh", "/usr/bin/sh", NULL};
        const char *sh_bin = find_bin(sh_candidates);
        CHECK(sh_bin != NULL, "found a shell for the early-close test");
        if (sh_bin) {
            char *buf = NULL;
            size_t buf_len = 0;
            log_fp = open_memstream(&buf, &buf_len);
            CHECK(log_fp != NULL, "open_memstream succeeded for capturing log output");

            metric_ctx_t ctx = {0};
            /* stdout emits exactly one line and then its fd is explicitly
             * closed; stderr keeps emitting lines well after that. If
             * out_open's transition to closed ever incorrectly affected
             * err_open's poll() slot (or vice versa), this would either
             * hang forever or silently stop draining stderr the moment
             * stdout closes. */
            const char *const argv[] = {sh_bin, "-c",
                "printf 'pool/only\\t99\\n'; exec 1>&-; "
                "i=1; while [ $i -le 10 ]; do echo \"late-stderr-$i\" >&2; i=$((i+1)); done", NULL};
            int rc = exec_cmd_stream(argv, handle_metric_line, &ctx);
            fflush(log_fp);

            CHECK(rc == 0, "the command succeeds even though stdout closes long before stderr finishes");
            CHECK(ctx.count == 1 && strcmp(ctx.items[0].name, "pool/only") == 0,
                  "the single stdout line emitted before the close was captured");
            CHECK(buf != NULL && strstr(buf, "late-stderr-1") != NULL && strstr(buf, "late-stderr-10") != NULL,
                  "all stderr lines emitted AFTER stdout's fd closed were still drained to completion, not cut off early");

            if (log_fp) fclose(log_fp);
            log_fp = NULL;
            free(buf);
            free(ctx.items);
        }
        printf("\n");
    }

    printf("== Test 32: valid_prefix's former empty-string check is confirmed dead code -- its only caller never passes an empty prefix ==\n");
    {
        /*
         * The explicit `if (prefix[0] == '\0') return 0;` guard was
         * removed as unreachable: valid_prefix's only caller in main()
         * receives only lines whose adjacent commas were rejected before
         * strtok_r() could skip an empty token, and REQUIRE_TOKEN already
         * goto's away on a NULL token before that call. Calling
         * valid_prefix("") directly here (something the real program
         * never does) now returns 1, since guarding against an empty
         * prefix is entirely the caller's responsibility going forward.
         */
        CHECK(valid_prefix("") == 1,
              "valid_prefix(\"\") returns 1 now that the unreachable empty-string rejection was removed -- documents the new contract rather than silently changing behavior");
        CHECK(valid_prefix("ok_name-1") == 1, "an ordinary alnum/underscore/hyphen prefix is still accepted");
        CHECK(valid_prefix("bad name") == 0, "a prefix containing a disallowed character (space) is still rejected");
        printf("\n");
    }

    printf("== Test 33: resolve_recursive_ancestor_overlaps never drives rec_b->count to 0, even when all but one item is covered ==\n");
    {
        /*
         * Confirms the invariant documented above the malloc() in
         * resolve_recursive_ancestor_overlaps: is_strict_descendant()
         * only marks an item covered when some OTHER item is a strictly
         * shorter ancestor, so the shortest item in the set can never be
         * covered. A 3-level same-prefix ancestor chain drives the count
         * down to its practical minimum (1), the closest this code path
         * can get to 0, and must neither crash nor mis-handle that.
         */
        batch_ctx_t rec_b = {0};
        batch_add(&rec_b, "pool/a", "p", 1, 0);
        batch_add(&rec_b, "pool/a/b", "p", 1, 0);
        batch_add(&rec_b, "pool/a/b/c", "p", 1, 0);

        int rc = resolve_recursive_ancestor_overlaps(&rec_b);

        CHECK(rc == 0, "resolve_recursive_ancestor_overlaps succeeds on a 3-level same-prefix ancestor chain");
        CHECK(rec_b.count == 1, "only the top-level ancestor survives; both descendants are covered and dropped");
        CHECK(rec_b.count > 0 && strcmp(rec_b.items[0].dataset, "pool/a") == 0,
              "the single survivor is the shortest (topmost) dataset in the chain, exactly as the invariant predicts");

        batch_free(&rec_b);
        printf("\n");
    }

    printf("== Test 34: resolve_recursive_ancestor_overlaps -- equal-length siblings with the same prefix cover neither one ==\n");
    {
        batch_ctx_t rec_b = {0};
        batch_add(&rec_b, "pool/aaa", "p", 1, 0);
        batch_add(&rec_b, "pool/bbb", "p", 1, 0); /* same length as pool/aaa, same prefix, NOT an ancestor/descendant of it */

        int rc = resolve_recursive_ancestor_overlaps(&rec_b);

        CHECK(rc == 0, "resolve_recursive_ancestor_overlaps succeeds on two equal-length siblings");
        CHECK(rec_b.count == 2, "neither sibling can be an ancestor of the other (equal length), so both survive uncovered");

        batch_free(&rec_b);
        printf("\n");
    }

    printf("== Test 35: resolve_recursive_ancestor_overlaps assigns different passes to ancestrally-overlapping items that survive because their prefixes differ ==\n");
    {
        /*
         * "pool/a" and "pool/a/b" have DIFFERENT prefixes, so the
         * same-prefix coverage check above never drops either one -- both
         * survive into the pass-assignment step below. batch_assign_
         * duplicate_passes() only bumps pass on an EXACT dataset-string
         * repeat, so it assigns both of these pass 0. Without the
         * collision-avoidance loop that follows, they'd stay in the same
         * pass despite "pool/a/b" being a descendant of "pool/a" -- and
         * zfs_snapshot_batch_root_pass() groups same-root/same-pass items
         * into ONE chunked `zfs snapshot -r ...` command, so both would be
         * named in a single recursive-snapshot invocation where one
         * dataset's `-r` coverage already includes the other. The
         * collision loop exists specifically to bump the descendant to a
         * separate pass so the two recursive snapshots run as separate
         * `zfs snapshot -r` invocations instead.
         */
        batch_ctx_t rec_b = {0};
        CHECK(batch_add(&rec_b, "pool/a", "p1", 1, 0) == 0 &&
              batch_add(&rec_b, "pool/a/b", "p2", 1, 0) == 0,
              "pass-collision test batch setup succeeds");

        int rc = resolve_recursive_ancestor_overlaps(&rec_b);

        CHECK(rc == 0, "resolve_recursive_ancestor_overlaps succeeds for ancestrally-overlapping items with different prefixes");
        CHECK(rec_b.count == 2,
              "different prefixes mean neither item is covered/dropped, unlike the same-prefix cases in Tests 33-34");
        CHECK(rec_b.count == 2 && strcmp(rec_b.items[0].dataset, "pool/a") == 0 &&
              strcmp(rec_b.items[1].dataset, "pool/a/b") == 0,
              "both items keep their original relative order (neither was covered, so nothing was compacted)");
        CHECK(rec_b.count == 2 && rec_b.items[0].pass != rec_b.items[1].pass,
              "the descendant (pool/a/b) is bumped to a pass distinct from its ancestor's (pool/a), avoiding a same-pass collision between two overlapping `zfs snapshot -r` calls");

        batch_free(&rec_b);
        printf("\n");
    }

    printf("== Test 36: finalize_batch fails safely, not silently, when a snapshot name would not fit its buffer ==\n");
    {
        /*
         * Reachable only by handing finalize_batch a batch_ctx_t built
         * directly (bypassing config-parsing's length validation, exactly
         * like the OOM/localtime whitebox tests bypass their own normal
         * call paths) -- config parsing itself can never produce a
         * dataset this long, but finalize_batch's own snprintf check must
         * still hold if that invariant is ever violated.
         */
        batch_ctx_t b = {0};
        char long_ds[500];
        memset(long_ds, 'a', sizeof(long_ds) - 1);
        long_ds[sizeof(long_ds) - 1] = '\0';
        /* dataset(499) + '@'(1) + prefix(1) + '_'(1) + timestamp(19) + NUL(1)
         * = 522 bytes, safely over STR_BUF_XLARGE (512). */
        int rc = batch_add(&b, long_ds, "p", 1, -1);
        CHECK(rc == 0, "batch_add succeeded during setup with a deliberately oversized dataset name");

        name_list_t inventory = {0};
        char **matches = NULL;
        size_t matches_cap = 0;
        char *buf = NULL;
        size_t buf_len = 0;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for capturing finalize_batch's log output");

        int status = finalize_batch(&b, &inventory, 1, &matches, &matches_cap, "2026-01-01_00:00:00", 0);
        fflush(log_fp);

        CHECK(status == 1, "finalize_batch reports failure for an item whose formatted snapshot name would not fit its buffer");
        CHECK(buf != NULL && strstr(buf, "Failed to format") != NULL,
              "an explicit formatting-failure error is logged instead of proceeding on a silently truncated name");
        CHECK(buf != NULL && strstr(buf, "Created=") == NULL,
              "no false 'Created=' line is emitted for a name that couldn't be safely formatted");

        if (log_fp) fclose(log_fp);
        log_fp = NULL;
        free(buf);
        free(matches);
        name_list_free(&inventory);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 37: finalize_batch's snap_name formatting still succeeds normally for an ordinary, well-sized dataset ==\n");
    {
        /* Baseline contrast to Test 36: proves the new truncation check
         * doesn't disturb the ordinary, overwhelmingly common case. */
        batch_ctx_t b = {0};
        int rc = batch_add(&b, "pool/normal", "p", 1, -1);
        CHECK(rc == 0, "batch_add succeeded during setup");

        name_list_t inventory = {0};
        char **matches = NULL;
        size_t matches_cap = 0;
        char *buf = NULL;
        size_t buf_len = 0;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded");

        int status = finalize_batch(&b, &inventory, 1, &matches, &matches_cap, "2026-01-01_00:00:00", 0);
        fflush(log_fp);

        CHECK(status == 0, "status is 0: normal-length name formats fine, and pruning against the (empty) inventory has nothing to do");
        CHECK(buf != NULL && strstr(buf, "Created=pool/normal@p_2026-01-01_00:00:00") != NULL,
              "the Created= line uses the correctly-formatted, non-truncated snapshot name");

        if (log_fp) fclose(log_fp);
        log_fp = NULL;
        free(buf);
        free(matches);
        name_list_free(&inventory);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 38: remove_recursive_overlaps removes only matching recursive coverage ==\n");
    {
        batch_ctx_t std_b = {0}, rec_b = {0};
        batch_add(&std_b, "pool/a/child", "p", 1, 0);
        batch_add(&std_b, "pool/a/other", "other", 1, 0);
        batch_add(&std_b, "pool/b/child", "p", 1, 0);
        /* "pool/a/child" is covered after is_recursively_covered's for(;;)
         * walk strips just ONE path segment ("pool/a/child" -> "pool/a").
         * "pool/a/b/c" needs the walk to strip TWO segments in succession
         * ("pool/a/b/c" -> "pool/a/b" -> "pool/a") before it finds the
         * match, exercising the loop beyond its first iteration. */
        batch_add(&std_b, "pool/a/b/c", "p", 1, 0);
        batch_add(&rec_b, "pool/a", "p", 1, 0);

        char *buf = NULL;
        size_t buf_len = 0;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "opened a log capture for Test 38's coverage messages");

        CHECK(remove_recursive_overlaps(&std_b, &rec_b) == 0,
              "recursive-overlap filtering succeeds for matching and nonmatching entries");
        if (log_fp) fflush(log_fp);
        CHECK(std_b.count == 2 && strcmp(std_b.items[0].dataset, "pool/a/other") == 0 &&
              strcmp(std_b.items[1].dataset, "pool/b/child") == 0,
              "only the standard entries under the matching recursive dataset and prefix are dropped");
        CHECK(buf != NULL && strstr(buf, "Skipping pool/a/child") != NULL,
              "the one-level-gap descendant is recognized as covered");
        CHECK(buf != NULL && strstr(buf, "Skipping pool/a/b/c") != NULL,
              "a two-level-gap descendant is also recognized as covered, exercising is_recursively_covered's ancestor walk past its first iteration");

        if (log_fp) fclose(log_fp);
        log_fp = NULL;
        free(buf);
        batch_free(&std_b); batch_free(&rec_b);
        printf("\n");
    }

    printf("== Test 39: remove_recursive_overlaps compacts before its oversized-name error path ==\n");
    {
        batch_ctx_t std_b = {0}, rec_b = {0};
        char oversized[STR_BUF_LARGE + 32];
        memset(oversized, 'a', sizeof(oversized) - 1);
        oversized[sizeof(oversized) - 1] = '\0';
        CHECK(batch_add(&std_b, "pool/keep", "standard", 1, 0) == 0 &&
              batch_add(&std_b, oversized, "standard", 1, 0) == 0 &&
              batch_add(&rec_b, "pool/recursive", "recursive", 1, 0) == 0,
              "overlap error-path setup succeeds");
        CHECK(remove_recursive_overlaps(&std_b, &rec_b) == -1,
              "an oversized standard dataset reaches the overlap-check error path");
        CHECK(std_b.count == 1 && strcmp(std_b.items[0].dataset, "pool/keep") == 0,
              "already-compacted entries remain the only owned entries after the error");
        batch_free(&std_b); /* the count repair matters for the OVERSIZED entry, not pool/keep:
                              * the error path already freed items[1..count-1] (just the oversized
                              * entry here) and shrank std_b->count to exclude them, so batch_free
                              * only iterates surviving items. pool/keep at items[0] was never
                              * touched by the error path and would be safe to free either way --
                              * without the repair, it's the already-freed oversized entry that
                              * batch_free would double-free by walking past the reduced count. */
        batch_free(&rec_b);
        printf("\n");
    }

    printf("== Test 40: finalize_batch on a snap_failed item without a usable inventory only logs 'unable to verify', no false Created= line ==\n");
    {
        /*
         * snap_failed==1 with inventory_ok==0: finalize_batch cannot ask
         * the (unavailable) inventory whether the snapshot actually made
         * it, so it must not claim success (no "Created=") and must not
         * attempt pruning. Note neither of finalize_batch's two
         * snap_failed-verification `continue`s sets `status`, so a
         * single-item batch here returns status==0 -- the snapshot
         * failure itself was already reported by zfs_snapshot_batch's
         * own return code; finalize_batch's job here is just to not lie
         * about what happened next.
         */
        batch_ctx_t b = {0};
        CHECK(batch_add(&b, "pool/unverified", "p", 1, -1) == 0,
              "batch_add succeeded during Test 40 setup");
        b.items[0].snap_failed = 1;

        name_list_t inventory = {0}; /* never read: inventory_ok==0 short-circuits before it is consulted */
        char **matches = NULL;
        size_t matches_cap = 0;
        char *buf = NULL;
        size_t buf_len = 0;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for Test 40's log capture");

        int status = finalize_batch(&b, &inventory, 0 /* inventory_ok */, &matches, &matches_cap,
                                     "2026-01-01_00:00:00", 0);
        if (log_fp) fflush(log_fp);

        CHECK(status == 0, "an unverifiable snap_failed item alone does not flip finalize_batch's status");
        CHECK(buf != NULL && strstr(buf, "Unable to verify") != NULL &&
              strstr(buf, "pool/unverified@p_2026-01-01_00:00:00") != NULL,
              "the specific unverifiable snapshot name is named in the diagnostic");
        CHECK(buf != NULL && strstr(buf, "Created=") == NULL,
              "no Created= line is emitted when the snapshot's existence could not be verified");

        if (log_fp) fclose(log_fp);
        log_fp = NULL;
        free(buf);
        free(matches);
        name_list_free(&inventory);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 41: finalize_batch on a snap_failed item confirmed ABSENT from a usable inventory skips pruning for that dataset ==\n");
    {
        batch_ctx_t b = {0};
        CHECK(batch_add(&b, "pool/absent", "p", 1, -1) == 0,
              "batch_add succeeded during Test 41 setup");
        b.items[0].snap_failed = 1;

        name_list_t inventory = {0}; /* usable, but empty -- the snapshot really isn't there */
        char **matches = NULL;
        size_t matches_cap = 0;
        char *buf = NULL;
        size_t buf_len = 0;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for Test 41's log capture");

        int status = finalize_batch(&b, &inventory, 1 /* inventory_ok */, &matches, &matches_cap,
                                     "2026-01-01_00:00:00", 0);
        if (log_fp) fflush(log_fp);

        CHECK(status == 0, "a confirmed-absent snap_failed item alone does not flip finalize_batch's status");
        CHECK(buf != NULL && strstr(buf, "Snapshot not created") != NULL &&
              strstr(buf, "pruning skipped for dataset 'pool/absent'") != NULL,
              "the pruning-skipped diagnostic names the correct dataset and prefix");
        CHECK(buf != NULL && strstr(buf, "Created=") == NULL,
              "no Created= line is emitted for a snapshot the inventory confirms does not exist");

        if (log_fp) fclose(log_fp);
        log_fp = NULL;
        free(buf);
        free(matches);
        name_list_free(&inventory);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 42: finalize_batch on a snap_failed item that the inventory shows DID succeed proceeds exactly like a normal success ==\n");
    {
        /*
         * The recovery path: zfs_snapshot_batch's own execution wrapper
         * reported failure, but the snapshot inventory -- fetched fresh,
         * after the fact -- proves the snapshot exists. finalize_batch
         * must not discard a real snapshot's Created=/pruning handling
         * just because the creation call's own return code was
         * pessimistic.
         */
        batch_ctx_t b = {0};
        CHECK(batch_add(&b, "pool/actually-there", "p", 1, -1) == 0,
              "batch_add succeeded during Test 42 setup");
        b.items[0].snap_failed = 1;

        name_list_t inventory = {0};
        CHECK(handle_snapshot_inventory_line("pool/actually-there@p_2026-01-01_00:00:00", &inventory) == 0,
              "seeded the inventory with the snapshot that 'actually' exists");
        char **matches = NULL;
        size_t matches_cap = 0;
        char *buf = NULL;
        size_t buf_len = 0;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for Test 42's log capture");

        int status = finalize_batch(&b, &inventory, 1 /* inventory_ok */, &matches, &matches_cap,
                                     "2026-01-01_00:00:00", 0);
        if (log_fp) fflush(log_fp);

        CHECK(status == 0, "a snap_failed item the inventory confirms exists does not flip finalize_batch's status");
        CHECK(buf != NULL && strstr(buf, "Created=pool/actually-there@p_2026-01-01_00:00:00") != NULL,
              "a Created= line IS emitted once the inventory confirms the snapshot exists, despite snap_failed");
        CHECK(buf != NULL && strstr(buf, "Unable to verify") == NULL && strstr(buf, "Snapshot not created") == NULL,
              "neither of the unverifiable/absent diagnostics fires once the inventory confirms success");

        if (log_fp) fclose(log_fp);
        log_fp = NULL;
        free(buf);
        free(matches);
        name_list_free(&inventory);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 43: finalize_batch on a genuinely successful snapshot still reports failure to prune when the inventory is unavailable ==\n");
    {
        batch_ctx_t b = {0};
        CHECK(batch_add(&b, "pool/created-ok", "p", 1, -1) == 0,
              "batch_add succeeded during Test 43 setup");
        /* snap_failed stays 0: the snapshot creation itself succeeded. */

        name_list_t inventory = {0};
        char **matches = NULL;
        size_t matches_cap = 0;
        char *buf = NULL;
        size_t buf_len = 0;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for Test 43's log capture");

        int status = finalize_batch(&b, &inventory, 0 /* inventory_ok */, &matches, &matches_cap,
                                     "2026-01-01_00:00:00", 0);
        if (log_fp) fflush(log_fp);

        CHECK(status == 1, "an unavailable inventory after a genuine success is reported as a finalize_batch failure");
        CHECK(buf != NULL && strstr(buf, "Created=pool/created-ok@p_2026-01-01_00:00:00") != NULL,
              "the Created= line is still emitted -- the snapshot itself really was created");
        CHECK(buf != NULL && strstr(buf, "Unable to prune") != NULL &&
              strstr(buf, "snapshot inventory unavailable") != NULL,
              "pruning is explicitly reported as skipped due to the unavailable inventory, not silently dropped");

        if (log_fp) fclose(log_fp);
        log_fp = NULL;
        free(buf);
        free(matches);
        name_list_free(&inventory);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 44: zfs_snapshot_batch issues one zfs snapshot call PER PASS when the same dataset is configured twice with different prefixes ==\n");
    {
        /*
         * batch_assign_duplicate_passes' own exact-dataset-repeat branch
         * (bumping .pass on the second occurrence of the SAME dataset
         * string) is what's under test here, not the ancestor/descendant
         * collision handling Test 35 exercises. Two config entries for
         * the identical dataset with different prefixes are legitimate
         * (seen_set_add only rejects an exact dataset+prefix repeat),
         * and ZFS itself rejects two snapshots of the same filesystem in
         * one `zfs snapshot` call, so this must become two separate
         * invocations -- one per pass -- not one call naming the
         * dataset twice.
         */
        char trace_path[PATH_MAX];
        CHECK(snprintf(trace_path, sizeof(trace_path), "%s/pass-calls", g_fake_zfs_dir) < (int)sizeof(trace_path),
              "Test 44 trace path fits in the isolated test directory");
        char script[PATH_MAX * 2 + 128];
        CHECK(snprintf(script, sizeof(script),
                       "#!/bin/sh\nif [ \"$1\" = snapshot ]; then\n shift\n printf '%%s\\n' \"$@\" >> '%s'\n printf '\\036\\n' >> '%s'\nfi\n",
                       trace_path, trace_path) < (int)sizeof(script),
              "Test 44 fake-zfs script fits in its buffer");
        CHECK(write_fake_zfs(script) == 0, "installed fake zfs for the multi-pass test");

        batch_ctx_t b = {0};
        CHECK(batch_add(&b, "pool/dup", "first", 1, 0) == 0 &&
              batch_add(&b, "pool/dup", "second", 1, 0) == 0,
              "the same dataset was added twice with different prefixes (legitimate per seen_set_add)");
        batch_assign_duplicate_passes(&b);
        CHECK(b.items[0].pass == 0 && b.items[1].pass == 1,
              "batch_assign_duplicate_passes gives the second occurrence of an EXACT dataset repeat the next pass");

        int rc = zfs_snapshot_batch(&b, 0, "2026-01-01_00:00:00");
        CHECK(rc == 0, "zfs_snapshot_batch succeeds against the fake zfs command");

        FILE *trace = fopen(trace_path, "r");
        size_t calls = 0;
        int saw_first = 0, saw_second = 0;
        char line[STR_BUF_XLARGE];
        while (trace && fgets(line, sizeof(line), trace)) {
            if ((unsigned char)line[0] == 036) { calls++; continue; }
            if (strstr(line, "pool/dup@first_")) saw_first = 1;
            if (strstr(line, "pool/dup@second_")) saw_second = 1;
        }
        if (trace) fclose(trace);
        CHECK(calls == 2, "the duplicate dataset produces exactly two separate `zfs snapshot` invocations, one per pass");
        CHECK(saw_first && saw_second, "each invocation names the expected pass's own snapshot, not both at once");

        batch_free(&b);
        (void)unlink(trace_path);
        (void)unlink(g_fake_zfs);
        printf("\n");
    }

    printf("== Test 45: load_combined_snapshot_inventory's scoped/unscoped root-bytes cutoff is inclusive of ARGV_BYTES_CAP exactly ==\n");
    {
        /*
         * roots_bytes sums strlen(root)+1 per due root. This targets the
         * boundary of `int use_scoped = (roots_bytes <= ARGV_BYTES_CAP);`
         * with realistic multi-root input (many distinct top-level
         * pools, as a real config would produce) rather than one
         * implausibly huge dataset name: 1024 roots of 127 bytes each
         * (+1 separator byte = 128 bytes/root) sum to exactly
         * ARGV_BYTES_CAP. Growing just one of those roots by a single
         * byte pushes the same 1024-root set one byte past the cap.
         */
        char inventory_script[PATH_MAX + 64];
        CHECK(snprintf(inventory_script, sizeof(inventory_script),
                       "#!/bin/sh\nprintf '%%s\\n' \"$@\" > '%s'\n", g_inventory_args) < (int)sizeof(inventory_script) &&
              write_fake_zfs(inventory_script) == 0,
              "installed fake zfs for the root-bytes boundary test");

        enum { N_ROOTS = 1024, ROOT_LEN = 127 }; /* 1024 * (127 + 1) == ARGV_BYTES_CAP exactly */
        CHECK((size_t)N_ROOTS * (ROOT_LEN + 1) == ARGV_BYTES_CAP,
              "Test 45's root sizing constants sum to exactly ARGV_BYTES_CAP");

        batch_ctx_t std_b = {0}, rec_b = {0};
        int add_ok = 1;
        for (int i = 0; i < N_ROOTS && add_ok; i++) {
            char root[ROOT_LEN + 1];
            int n = snprintf(root, sizeof(root), "p%04d", i);
            for (int p = n; p < ROOT_LEN; p++) root[p] = 'x';
            root[ROOT_LEN] = '\0';
            if (batch_add(&std_b, root, "p", 1, 0) != 0) add_ok = 0;
        }
        CHECK(add_ok, "assembled 1024 distinct, exactly-127-byte root datasets");

        name_list_t inventory = {0};
        unlink(g_inventory_args);
        CHECK(load_combined_snapshot_inventory(&inventory, &std_b, &rec_b) == 0,
              "inventory load succeeds when roots_bytes lands exactly on ARGV_BYTES_CAP");
        FILE *args_fp = fopen(g_inventory_args, "r");
        char args[1024] = {0};
        if (args_fp) { size_t rd = fread(args, 1, sizeof(args) - 1, args_fp); (void)rd; fclose(args_fp); }
        CHECK(strstr(args, "-r\n") != NULL,
              "a roots_bytes total exactly equal to ARGV_BYTES_CAP stays scoped ('<=' is inclusive of the boundary)");
        name_list_free(&inventory);

        CHECK(std_b.count == N_ROOTS, "boundary batch retained all 1024 roots before the +1-byte mutation");
        if (std_b.count == N_ROOTS) {
            size_t last = std_b.count - 1;
            char *grown = diffsnap_realloc(std_b.items[last].dataset, ROOT_LEN + 2);
            CHECK(grown != NULL, "grew the last root's dataset string by one byte for the over-cap case");
            if (grown) {
                grown[ROOT_LEN] = 'y';
                grown[ROOT_LEN + 1] = '\0';
                std_b.items[last].dataset = grown;
            }
        }

        name_list_t inventory2 = {0};
        unlink(g_inventory_args);
        CHECK(load_combined_snapshot_inventory(&inventory2, &std_b, &rec_b) == 0,
              "inventory load still succeeds via the unscoped fallback when roots_bytes exceeds ARGV_BYTES_CAP by one");
        FILE *args_fp2 = fopen(g_inventory_args, "r");
        char args2[1024] = {0};
        if (args_fp2) { size_t rd = fread(args2, 1, sizeof(args2) - 1, args_fp2); (void)rd; fclose(args_fp2); }
        CHECK(strstr(args2, "-r\n") == NULL,
              "a roots_bytes total just one byte over ARGV_BYTES_CAP already falls back to unscoped");
        name_list_free(&inventory2);

        batch_free(&std_b);
        unlink(g_inventory_args);
        unlink(g_fake_zfs);
        printf("\n");
    }

    if (run_system) run_system_tests();
    cleanup_fake_zfs();
    printf("================================\n");
    printf("RESULTS: %d checks run, %d failed\n", g_tests_run, g_tests_failed);
    printf("================================\n");
    if (g_tests_failed == 0) { printf("ALL CLEAR\n"); return 0; }
    printf("REVIEW FAILURES ABOVE\n");
    return 1;
}
