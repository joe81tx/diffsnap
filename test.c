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
#include <sys/resource.h>
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

/*
 * Forward-declared so the fatal restore-failure paths below (which run
 * before setup_fake_zfs()/cleanup_fake_zfs() are defined later in this
 * file) can still tear down the mkdtemp()'d fake-zfs directory before
 * calling exit(1) -- otherwise a FATAL abort here leaks that temp
 * directory (and whatever fake-zfs script/trace files were installed in
 * it) on disk instead of removing it like every non-fatal exit path does.
 */
static void cleanup_fake_zfs(void);

/*
 * Tests that temporarily lower RLIMIT_NOFILE to force pipe2()/open()
 * failures must restore it afterward like every other piece of global
 * state this suite mutates. But unlike an in-process hook (realloc_now_fn,
 * localtime_now_fn, ...), a failed setrlimit() restore can't just be
 * CHECK()'d and shrugged off: every later test in this process would then
 * silently run under an artificially starved file-descriptor budget and
 * could fail for a reason completely unrelated to what it's testing,
 * turning one flaky restore into an unbounded number of misleading
 * downstream failures. So this is fatal rather than a normal CHECK: report
 * it clearly and stop the whole run instead of letting it cascade.
 */
static void restore_rlimit_or_die(int resource, const char *resource_name,
                                   const struct rlimit *old_limit, const char *msg) {
    g_tests_run++;
    if (setrlimit(resource, old_limit) == 0) {
        printf("    ok: %s\n", msg);
        return;
    }
    g_tests_failed++;
    printf("    FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);
    fprintf(stderr,
            "FATAL: could not restore %s to rlim_cur=%llu after lowering it for a fault-injection "
            "test; every remaining test would run under a starved resource limit and produce "
            "misleading failures, so stopping now instead of continuing. (tests run so far: %d, "
            "failed: %d)\n",
            resource_name, (unsigned long long)old_limit->rlim_cur, g_tests_run, g_tests_failed);
    cleanup_fake_zfs();
    exit(1);
}

static void restore_rlimit_nofile_or_die(const struct rlimit *old_limit, const char *msg) {
    restore_rlimit_or_die(RLIMIT_NOFILE, "RLIMIT_NOFILE", old_limit, msg);
}

/*
 * Tests that temporarily dup2() a standard fd (stdin/stderr) away from its
 * original target to capture output/starve input must restore it
 * afterward, and a failed restore can't just be CHECK()'d and shrugged off
 * either: unlike an in-process hook, leaving fd 0/2 pointed at a closed
 * file (or never reattached to the real terminal/pipe) would corrupt
 * whatever every later test in this process reads from or prints to,
 * turning one flaky restore into an unbounded number of misleading
 * downstream failures. So this is fatal, mirroring restore_rlimit_or_die
 * above -- and it always closes saved_fd itself (success or failure) so
 * callers never need a second cleanup step.
 */
static void restore_stdfd_or_die(int saved_fd, int target_fd, const char *fd_name, const char *msg) {
    g_tests_run++;
    int rc = dup2(saved_fd, target_fd);
    int dup2_errno = errno;
    close(saved_fd);
    if (rc >= 0) {
        printf("    ok: %s\n", msg);
        return;
    }
    g_tests_failed++;
    printf("    FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);
    /* fd_name here may itself be corrupted (this IS the stderr-restore
     * failure path), so this diagnostic is best-effort; the exit() below
     * is what actually prevents the corruption from cascading. */
    fprintf(stderr,
            "FATAL: could not restore fd %d (%s) after redirecting it for a test (dup2 errno=%d: %s); "
            "every remaining test would run with a corrupted standard fd and produce misleading "
            "failures, so stopping now instead of continuing. (tests run so far: %d, failed: %d)\n",
            target_fd, fd_name, dup2_errno, strerror(dup2_errno), g_tests_run, g_tests_failed);
    cleanup_fake_zfs();
    exit(1);
}

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
/*
 * Drives ensure_std_fds() with exactly one of fd 0/1/2 closed, and confirms
 * that slot specifically -- and only that slot -- gets refilled with
 * /dev/null. The real fd is saved aside first (via dup) and restored
 * immediately afterward regardless of outcome, so a failure partway through
 * never leaves the test binary's own stdio permanently damaged.
 *
 * CHECK()'s own diagnostic output goes through printf(), which is buffered
 * and targets fd 1 (stdout). When the fd under test IS fd 1, calling
 * CHECK() (or anything else that touches stdio) between closing it and
 * ensure_std_fds() refilling it corrupts stdout: buffered content queued
 * during that window ends up flushed to whatever winds up sitting on fd 1
 * once it's next valid, not necessarily the real terminal/pipe, silently
 * scrambling this and unrelated later output. So every step below uses
 * only raw syscalls -- no printf/CHECK -- until the real fd is fully
 * restored; results are captured into locals and reported via CHECK()
 * only afterward, once stdio is safe to use again for every one of fd
 * 0/1/2, not just the two (0 and 2) that happen not to alias stdout.
 */
static void run_ensure_std_fds_one(int fd, const char *fd_name) {
    int saved = dup(fd);
    int dup_ok = saved >= 0;
    int close_ok = 0, rc = -1, leaves_open = 0, refilled_with_devnull = 0, restore_ok = 0;

    if (dup_ok) {
        if (fd == STDERR_FILENO) fflush(stderr);
        else if (fd == STDOUT_FILENO) fflush(stdout);

        close_ok = (close(fd) == 0);

        rc = ensure_std_fds();
        leaves_open = (fcntl(fd, F_GETFD) != -1);

        int null_fd = open("/dev/null", O_RDONLY);
        struct stat target_st, null_st;
        refilled_with_devnull = (null_fd >= 0 && fstat(fd, &target_st) == 0 && fstat(null_fd, &null_st) == 0 &&
                                  target_st.st_dev == null_st.st_dev && target_st.st_rdev == null_st.st_rdev);
        if (null_fd >= 0) close(null_fd);

        restore_ok = (dup2(saved, fd) != -1);
        close(saved);
    }

    /* fd is now fully restored (or was never touched, if dup itself
     * failed) -- safe to use stdio/CHECK from here on. */
    printf("-- ensure_std_fds: fd %d (%s) --\n", fd, fd_name);
    CHECK(dup_ok, "dup'd the real fd aside before closing it for the ensure_std_fds test");
    if (!dup_ok) return;
    CHECK(close_ok, "closed the target fd to simulate a launcher that starts diffsnap with it already closed");
    CHECK(rc == 0, "ensure_std_fds succeeds when the target fd starts closed");
    CHECK(leaves_open, "ensure_std_fds leaves the target fd open afterward");
    CHECK(refilled_with_devnull,
          "ensure_std_fds refills the closed slot specifically with /dev/null, not just any open file");
    CHECK(restore_ok, "restored the real fd after the ensure_std_fds test");
}

static void run_ensure_std_fds_test(void) {
    printf("== ensure_std_fds: refills a closed low-numbered stdio slot with /dev/null ==\n");
    /*
     * The loop inside ensure_std_fds() is identical for fd 0, 1, and 2, but
     * that's exactly the kind of "obviously symmetric" code a coverage gap
     * likes to hide behind -- only exercising fd 2 leaves fd 0 (stdin) and
     * fd 1 (stdout) entirely untested. Drive all three explicitly.
     */
    run_ensure_std_fds_one(STDIN_FILENO, "stdin");
    run_ensure_std_fds_one(STDOUT_FILENO, "stdout");
    run_ensure_std_fds_one(STDERR_FILENO, "stderr");

    /*
     * All three fds are open now (real ones, restored above), so a second
     * call is a documented no-op: it must report success WITHOUT touching
     * any of fd 0/1/2. Checking only the return code can't tell a true
     * no-op apart from a spurious close-and-refill-with-/dev/null of an
     * already-good fd, since ensure_std_fds() returns 0 in both cases.
     * fstat() each fd before and after and require the exact same open
     * file description (device/inode/rdev unchanged) -- not just "some fd
     * happens to be open again" -- to actually prove nothing was touched.
     */
    struct stat before_st[3], after_st[3];
    int stat_before_ok = 1;
    for (int fd = 0; fd <= 2; fd++) stat_before_ok = stat_before_ok && (fstat(fd, &before_st[fd]) == 0);
    int noop_rc = ensure_std_fds();
    int stat_after_ok = 1;
    for (int fd = 0; fd <= 2; fd++) stat_after_ok = stat_after_ok && (fstat(fd, &after_st[fd]) == 0);
    CHECK(stat_before_ok && stat_after_ok, "fstat succeeded on fd 0/1/2 both before and after the no-op ensure_std_fds() call");
    CHECK(noop_rc == 0, "ensure_std_fds is a successful no-op when fd 0/1/2 are all already open");
    int identity_preserved = stat_before_ok && stat_after_ok;
    for (int fd = 0; fd <= 2 && identity_preserved; fd++) {
        identity_preserved = before_st[fd].st_dev == after_st[fd].st_dev &&
                              before_st[fd].st_ino == after_st[fd].st_ino &&
                              before_st[fd].st_rdev == after_st[fd].st_rdev;
    }
    CHECK(identity_preserved,
          "ensure_std_fds leaves fd 0/1/2 pointing at exactly the same open file description as before -- not silently closed and refilled with /dev/null even though that would also return 0");
    printf("\n");
}

/*
 * Coverage gap: every ensure_std_fds() test above (and the one in the
 * top-level main() gap test) closes exactly ONE of fd 0/1/2 before
 * calling ensure_std_fds(). That's a real gap, but not quite the one it
 * looks like -- ensure_std_fds()'s loop processes fd 0, then 1, then 2,
 * strictly in that order, and fully refills each closed slot before
 * advancing to the next. So by induction, at the start of the iteration
 * that examines fd k, fds 0..k-1 are *already* guaranteed valid (either
 * they were never closed, or a prior iteration just refilled them).
 * open("/dev/null") always returns the lowest-numbered available fd; with
 * 0..k-1 all occupied and k itself the only closed one, that lowest
 * available fd can only be k. So `devnull != fd` -- and therefore the
 * dup2()/close() rebind inside it, plus its own dup2 failure path -- is
 * provably unreachable through ensure_std_fds()'s only real call pattern
 * (fds 0/1/2, examined low to high) no matter which single fd a test
 * closes. No single-fd-closed test, however arranged, can ever reach it.
 *
 * What was never actually tested is the realistic scenario diffsnap.c's
 * own comment above ensure_std_fds() describes: a launcher (e.g. cron)
 * starting the process with fd 0, 1, AND 2 all closed at once. This test
 * drives exactly that, closing all three simultaneously, and confirms
 * ensure_std_fds() still fills every one of them with /dev/null -- which
 * is also the concrete proof that the low-to-high fill-before-advancing
 * invariant above actually holds in the multi-closed case, not just the
 * single-closed case, i.e. real evidence for *why* the rebind branch is
 * dead, rather than just an assertion that it is.
 *
 * Same discipline as run_ensure_std_fds_one(): no printf/CHECK (both
 * ultimately write through fd 1/2) between closing the real fds and
 * ensure_std_fds() refilling them -- everything is captured into locals
 * and reported via CHECK() only once fd 0/1/2 are all back to a valid
 * state.
 */
static void run_ensure_std_fds_all_closed_test(void) {
    int saved[3] = {-1, -1, -1};
    int dup_ok = 1;
    for (int fd = 0; fd <= 2; fd++) {
        saved[fd] = dup(fd);
        if (saved[fd] < 0) dup_ok = 0;
    }

    int close_ok = 1, rc = -1, restore_ok = 1;
    int leaves_open[3] = {0, 0, 0};
    int refilled_with_devnull[3] = {0, 0, 0};

    if (dup_ok) {
        fflush(stdout);
        fflush(stderr);
        for (int fd = 0; fd <= 2; fd++) {
            if (close(fd) != 0) close_ok = 0;
        }

        rc = ensure_std_fds();

        for (int fd = 0; fd <= 2; fd++) leaves_open[fd] = (fcntl(fd, F_GETFD) != -1);

        int null_fd = open("/dev/null", O_RDONLY);
        struct stat null_st;
        int have_null_st = (null_fd >= 0 && fstat(null_fd, &null_st) == 0);
        for (int fd = 0; fd <= 2; fd++) {
            struct stat target_st;
            refilled_with_devnull[fd] = have_null_st && fstat(fd, &target_st) == 0 &&
                                         target_st.st_dev == null_st.st_dev &&
                                         target_st.st_rdev == null_st.st_rdev;
        }
        if (null_fd >= 0) close(null_fd);

        for (int fd = 0; fd <= 2; fd++) {
            if (dup2(saved[fd], fd) == -1) restore_ok = 0;
            close(saved[fd]);
        }
    }

    /* fd 0/1/2 are now fully restored (or were never touched, if dup
     * itself failed) -- safe to use stdio/CHECK from here on. */
    printf("== ensure_std_fds: refills fd 0, 1, AND 2 with /dev/null when all three are closed simultaneously ==\n");
    CHECK(dup_ok, "dup'd the real fd 0/1/2 aside before closing all three for the all-closed ensure_std_fds test");
    if (!dup_ok) {
        for (int fd = 0; fd <= 2; fd++) if (saved[fd] >= 0) close(saved[fd]);
        printf("\n");
        return;
    }
    CHECK(close_ok, "closed fd 0, 1, and 2 simultaneously to simulate a launcher that starts diffsnap with none of them open");
    CHECK(rc == 0, "ensure_std_fds succeeds when fd 0, 1, and 2 all start closed at once");
    CHECK(leaves_open[0] && leaves_open[1] && leaves_open[2], "ensure_std_fds leaves all three fds open afterward");
    CHECK(refilled_with_devnull[0] && refilled_with_devnull[1] && refilled_with_devnull[2],
          "every one of fd 0/1/2 is refilled specifically with /dev/null when all three start closed together -- "
          "confirming the low-to-high fill-before-advancing invariant holds in the multi-closed case, which is why "
          "the dup2()/close() rebind branch inside ensure_std_fds() is unreachable through its only real call pattern");
    CHECK(restore_ok, "restored the real fd 0/1/2 after the all-closed ensure_std_fds test");
    printf("\n");
}

static void run_chunk_test(void) {
    char trace_path[PATH_MAX];
    CHECK(snprintf(trace_path, sizeof(trace_path), "%s/chunks", g_fake_zfs_dir) < (int)sizeof(trace_path),
          "chunk trace path fits in the isolated test directory");
    char script[PATH_MAX * 2 + 128];
    /* Explicit trailing `exit 0` so any call this script receives (not
     * just "snapshot") exits successfully like real zfs would, instead of
     * falling through to the exit status of the `[ "$1" = snapshot ]`
     * test itself (nonzero when it doesn't match). */
    CHECK(snprintf(script, sizeof(script),
                   "#!/bin/sh\nif [ \"$1\" = snapshot ]; then\n shift\n printf '%%s\\n' \"$@\" >> '%s'\n printf '\\036\\n' >> '%s'\nfi\nexit 0\n",
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
    /*
     * PID alone is not a reliable uniqueness key: after PID reuse, a stale
     * rpool/diffsnap-test-<pid> left behind by a previous (e.g. killed or
     * crashed) run under the same PID could still exist, and `zfs create
     * -p` against it would silently succeed against that pre-existing
     * fixture rather than a fresh dataset -- after which the final
     * recursive destroy would remove content this run never created.
     * Folding in the current time narrows the window but can't eliminate
     * it by itself, so this is paired below with an explicit pre-flight
     * check that the target dataset name does not already exist before
     * anything is created under it.
     */
    CHECK(snprintf(dataset, sizeof(dataset), "%s/diffsnap-test-%ld-%llx", pool, (long)getpid(),
                    (unsigned long long)time(NULL)) < (int)sizeof(dataset),
          "isolated real-ZFS test dataset name fits in the ZFS name buffer");
    CHECK(snprintf(standard, sizeof(standard), "%s/standard", dataset) < (int)sizeof(standard) &&
          snprintf(tree, sizeof(tree), "%s/tree", dataset) < (int)sizeof(tree) &&
          snprintf(tree_child, sizeof(tree_child), "%s/child", tree) < (int)sizeof(tree_child),
          "real-ZFS child dataset names fit in the ZFS name buffer");

    const char *const check_pool[] = {zfs_path, "list", "-H", "-o", "name", pool, NULL};
    int pool_available = exec_cmd_stream(check_pool, NULL, NULL) == 0;
    CHECK(pool_available, "the required real ZFS pool is available (rpool on Linux, zroot on FreeBSD)");
    if (!pool_available) {
        zfs_path = g_fake_zfs;
        printf("\n");
        return;
    }

    /*
     * Refuse to proceed if the target dataset name already exists: this is
     * the only way to be sure the create/destroy below operates solely on
     * a fixture this run created, and never adopts (and later destroys) a
     * pre-existing dataset left over from a prior PID-reused run or
     * outside interference.
     */
    const char *const check_target[] = {zfs_path, "list", "-H", "-o", "name", dataset, NULL};
    int target_already_exists = exec_cmd_stream(check_target, NULL, NULL) == 0;
    CHECK(!target_already_exists,
          "the isolated real-ZFS test dataset name does not already exist before this run creates anything under it");
    if (target_already_exists) {
        zfs_path = g_fake_zfs;
        printf("\n");
        return;
    }

    const char *const create_standard[] = {zfs_path, "create", "-p", standard, NULL};
    int standard_created = exec_cmd_stream(create_standard, NULL, NULL) == 0;
    CHECK(standard_created, "created an isolated real-ZFS standard dataset");
    if (!standard_created) {
        /* Cleanup attempt even though `create` itself reported failure: a
         * partial/racy create (e.g. ancestor already existed) can still
         * have left something under `dataset` to remove. The pre-flight
         * check above already established that nothing under `dataset`
         * pre-dates this run, so anything found here is this run's own
         * partial state and the destroy's result is worth reporting
         * rather than silently discarding -- an unreported failure here
         * would abandon a fixture with no test remaining to catch it. */
        const char *const destroy_dataset[] = {zfs_path, "destroy", "-r", dataset, NULL};
        CHECK(exec_cmd_stream(destroy_dataset, NULL, NULL) == 0,
              "cleaned up the partially-created real-ZFS test dataset after a standard-dataset create failure");
        zfs_path = g_fake_zfs;
        printf("\n");
        return;
    }
    created = 1;
    const char *const create_tree[] = {zfs_path, "create", "-p", tree_child, NULL};
    int tree_created = exec_cmd_stream(create_tree, NULL, NULL) == 0;
    CHECK(tree_created, "created an isolated nested real-ZFS dataset tree");
    if (!tree_created) {
        const char *const destroy_dataset[] = {zfs_path, "destroy", "-r", dataset, NULL};
        CHECK(exec_cmd_stream(destroy_dataset, NULL, NULL) == 0,
              "cleaned up the partially-created real-ZFS test dataset after a nested-tree create failure");
        zfs_path = g_fake_zfs;
        printf("\n");
        return;
    }

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

static int g_strftime_fail;
static int g_strftime_calls;
static size_t test_strftime(char *buf, size_t max, const char *fmt, const struct tm *tm_info) {
    g_strftime_calls++;
    if (g_strftime_fail) {
        /* strftime() signals failure by returning 0 and leaving buf's
         * contents unspecified; a zero-size buffer forces glibc's real
         * strftime() to do exactly that, so this stays a faithful failure
         * rather than a hand-waved short-circuit. */
        return strftime(buf, 0, fmt, tm_info);
    }
    return strftime(buf, max, fmt, tm_info);
}

static long g_realloc_calls;
static long g_realloc_fail_after = -1;
static void *test_realloc(void *ptr, size_t size) {
    g_realloc_calls++;
    return (g_realloc_fail_after >= 0 && g_realloc_calls > g_realloc_fail_after) ? NULL : realloc(ptr, size);
}

static int g_fclose_fail;
static int g_fclose_calls;
/* If nonzero, only the g_fclose_fail_at_call'th invocation of
 * test_fclose_failure() is made to fail; every other call succeeds
 * normally. diffsnap_fclose() is shared by both the config-file close and
 * the log-file close (in that fixed order, at main()'s single cleanup
 * label), so leaving this at 0 would fail whichever close happens first
 * to reach the hook -- broader than "the log close specifically" -- and a
 * test asserting only that *some* hooked close occurred can't tell those
 * apart. Targeting a call index lets a test pin down and confirm exactly
 * which of the two closes was made to fail. */
static int g_fclose_fail_at_call;
static int test_fclose_failure(FILE *fp) {
    g_fclose_calls++;
    int rc = fclose(fp);
    if (g_fclose_fail && (g_fclose_fail_at_call == 0 || g_fclose_calls == g_fclose_fail_at_call)) {
        errno = ENOSPC; return -1;
    }
    return rc;
}

/* fork_now_fn injection: deterministically forces exec_cmd_stream_core()
 * into its pid == -1 branch, independent of any platform-specific
 * RLIMIT_NPROC behavior. g_fork_calls lets a test confirm the hook was
 * actually reached (i.e. fork_now_fn, not fork(), supplied the pid), which
 * is what makes the resulting failure attributable specifically to the
 * fork()==-1 branch rather than to a downstream pipe/drain/wait failure --
 * with this injection, pipe2() already succeeded and drain/waitpid are
 * never reached, so a nonzero exec_cmd_stream() return here can only come
 * from the branch under test. */
static int g_fork_fail;
static int g_fork_calls;
static pid_t test_fork_failure(void) {
    g_fork_calls++;
    if (g_fork_fail) { errno = EAGAIN; return -1; }
    return fork();
}

/*
 * Counts exact occurrences of "<stem><n>" in hay, where a match is only
 * counted if it is NOT followed by another digit (so "stderr-1" does not
 * also match inside "stderr-10", "stderr-17-corrupt", etc.). Used to
 * verify every expected numbered log/stdout line is present exactly once,
 * rather than only spot-checking the first and last.
 */
static int count_exact_numbered_occurrences(const char *hay, const char *stem, int n) {
    if (!hay) return 0;
    char needle[64];
    snprintf(needle, sizeof(needle), "%s%d", stem, n);
    size_t needle_len = strlen(needle);
    int count = 0;
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) {
        unsigned char after = (unsigned char)p[needle_len];
        if (!isdigit(after)) count++;
        p += needle_len;
    }
    return count;
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

static struct tm *test_negative_offset_localtime(const time_t *value, struct tm *result) {
    (void)value;
    memset(result, 0, sizeof(*result));
    result->tm_year = 126; result->tm_mon = 2; result->tm_mday = 8;
    result->tm_hour = 0; result->tm_min = 0; result->tm_sec = 0;
#if defined(__GLIBC__) || defined(__FreeBSD__)
    result->tm_gmtoff = -5 * 60 * 60;
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
    restore_stdfd_or_die(saved, STDERR_FILENO, "stderr",
                          "restored the real stderr after run_main_capture_stderr()'s redirection");
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

    printf("== Main(): its own lock/log/config file-open failure branches ==\n");
    {
        /*
         * Gap: main() has three distinct "failed to open X" early-return
         * branches (lock, log, config), each reachable in production from
         * an ordinary permissions or missing-directory problem, but none
         * had a corresponding test. All three point at a path whose
         * *parent directory* does not exist, so open()/fopen() fails with
         * ENOENT regardless of this process's privileges (unlike e.g.
         * chmod 000, which root would sail through).
         */
        char missing_dir_path[PATH_MAX];
        CHECK(snprintf(missing_dir_path, sizeof(missing_dir_path), "%s/missing-subdir", g_fake_zfs_dir) < (int)sizeof(missing_dir_path),
              "constructed a deliberately-nonexistent parent directory for the open-failure tests");
        CHECK(access(missing_dir_path, F_OK) != 0, "the parent directory used for these tests really is absent");

        char bad_lock[PATH_MAX], bad_log[PATH_MAX], bad_conf[PATH_MAX];
        CHECK(snprintf(bad_lock, sizeof(bad_lock), "%s/lock", missing_dir_path) < (int)sizeof(bad_lock) &&
              snprintf(bad_log, sizeof(bad_log), "%s/log", missing_dir_path) < (int)sizeof(bad_log) &&
              snprintf(bad_conf, sizeof(bad_conf), "%s/conf", missing_dir_path) < (int)sizeof(bad_conf),
              "unreachable lock/log/config paths constructed");

        char stderr_buf[512];

        lock_path = bad_lock;
        memset(stderr_buf, 0, sizeof(stderr_buf));
        CHECK(run_main_capture_stderr(1, (char *[]){"diffsnap-test", NULL}, stderr_buf, sizeof(stderr_buf)) == 1,
              "main() fails when the lock file itself cannot be opened");
        CHECK(strstr(stderr_buf, "failed to open lock file") != NULL,
              "the lock-file open failure is reported on stderr");
        lock_path = lock_file;

        log_path = bad_log;
        memset(stderr_buf, 0, sizeof(stderr_buf));
        CHECK(run_main_capture_stderr(1, (char *[]){"diffsnap-test", NULL}, stderr_buf, sizeof(stderr_buf)) == 1,
              "main() fails when the log file itself cannot be opened (lock already succeeded)");
        CHECK(strstr(stderr_buf, "failed to open log file") != NULL,
              "the log-file open failure is reported on stderr");
        log_path = log_file;

        conf_path = bad_conf;
        unlink(log_file);
        memset(stderr_buf, 0, sizeof(stderr_buf));
        CHECK(run_main_capture_stderr(1, (char *[]){"diffsnap-test", NULL}, stderr_buf, sizeof(stderr_buf)) == 1,
              "main() fails when the config file itself cannot be opened (lock and log already succeeded)");
        CHECK(strstr(stderr_buf, "failed to open config file") != NULL,
              "the config-file open failure is reported on stderr");
        FILE *log = fopen(log_file, "r");
        char log_contents[512] = {0};
        if (log) { size_t rd = fread(log_contents, 1, sizeof(log_contents) - 1, log); (void)rd; fclose(log); }
        CHECK(strstr(log_contents, "failed to open config file") != NULL,
              "unlike the lock/log-open failures above, log_fp is already open by the time the config open fails, so this one is ALSO written through log_msg() into the log file itself");
        conf_path = conf_file;
    }
    printf("\n");

    printf("== Gap: main() fails at its own top-level ensure_std_fds() call when a closed low fd cannot be refilled ==\n");
    {
        /*
         * Coverage gap: ensure_std_fds() itself is unit-tested directly
         * (run_ensure_std_fds_test above), but main()'s own
         * "if (ensure_std_fds() != 0) { early_fail(...); return 1; }"
         * branch -- the very first thing main() does, before the lock
         * file is even opened -- was never driven to failure through
         * diffsnap_real_main() itself. ensure_std_fds()'s only failure
         * mode is its internal open("/dev/null") call failing for a
         * closed fd 0/1/2 slot.
         *
         * fd 0 (stdin) is the target rather than fd 1/2 so the normal
         * run_main_capture_stderr()-style stderr capture keeps working
         * throughout: the capture stream is dup2()'d onto stderr BEFORE
         * fd 0 is closed and RLIMIT_NOFILE is lowered, so that dup2()
         * call itself still has a normal fd budget, and only fd 0's own
         * re-creation is starved afterward.
         *
         * Starving fd 0 specifically requires RLIMIT_NOFILE's soft limit
         * lowered all the way to 0: fd 0 is the lowest possible
         * descriptor number, so unlike Test 46/46a's technique of leaving
         * headroom for exactly one pipe2() call, there is no higher
         * cutoff that blocks fd 0 alone while leaving already-open
         * descriptors (which remain valid regardless of a lowered soft
         * limit -- only *new* fd-allocating calls are refused) usable.
         *
         * The original RLIMIT_NOFILE and the real fd 0/stderr are
         * restored on every path out of this block; a failed RLIMIT_NOFILE
         * restore is routed through restore_rlimit_nofile_or_die (fatal),
         * the same discipline Test 46 uses, since a failed restore here
         * would starve every later test in this process of new fds.
         */
        FILE *capture = tmpfile();
        int saved_stderr = capture ? dup(STDERR_FILENO) : -1;
        int capture_ok = capture != NULL && saved_stderr >= 0;
        CHECK(capture_ok, "opened a capture file and saved the real stderr aside for the ensure_std_fds()-failure test");
        if (capture_ok) {
            fflush(stderr);
            int redirected = dup2(fileno(capture), STDERR_FILENO) >= 0;
            CHECK(redirected, "redirected stderr to the capture file before starving fd 0 (so the redirect itself isn't affected by the lowered rlimit)");
            if (redirected) {
                int saved_stdin = dup(STDIN_FILENO);
                int dup_ok = saved_stdin >= 0;
                CHECK(dup_ok, "saved the real fd 0 aside before closing it for the ensure_std_fds()-failure test");
                if (dup_ok) {
                    int close_ok = close(STDIN_FILENO) == 0;
                    CHECK(close_ok, "closed fd 0 to simulate a launcher that starts diffsnap with stdin already closed");
                    struct rlimit old_limit;
                    int got_limit = getrlimit(RLIMIT_NOFILE, &old_limit) == 0;
                    CHECK(got_limit, "read the current RLIMIT_NOFILE before starving fd 0 for the ensure_std_fds()-failure test");
                    if (close_ok && got_limit) {
                        struct rlimit zero_limit = { .rlim_cur = 0, .rlim_max = old_limit.rlim_max };
                        int lowered = setrlimit(RLIMIT_NOFILE, &zero_limit) == 0;
                        if (!lowered) {
                            printf("    SKIP: could not lower RLIMIT_NOFILE to 0 on this platform/permission level; ensure_std_fds()-failure test skipped\n");
                        } else {
                            int rc = diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL});

                            restore_rlimit_nofile_or_die(&old_limit,
                                  "restored the original RLIMIT_NOFILE after the ensure_std_fds()-failure test");

                            fflush(stderr);
                            rewind(capture);
                            char stderr_buf[256] = {0};
                            size_t got = fread(stderr_buf, 1, sizeof(stderr_buf) - 1, capture);
                            (void)got;

                            CHECK(rc == 1, "main() fails when its own top-level ensure_std_fds() call cannot refill a closed fd 0");
                            CHECK(strstr(stderr_buf, "failed to establish standard file descriptors") != NULL,
                                  "the ensure_std_fds() failure is reported on stderr with its own distinct diagnostic");
                        }
                    }
                    restore_stdfd_or_die(saved_stdin, STDIN_FILENO, "stdin",
                                         "restored the real stdin after starving fd 0 for the ensure_std_fds()-failure test");
                }
            }
            fflush(stderr);
            restore_stdfd_or_die(saved_stderr, STDERR_FILENO, "stderr",
                                  "restored the real stderr after capturing it for the ensure_std_fds()-failure test");
        }
        if (capture) fclose(capture);
        printf("\n");
    }

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
    CHECK(fp != NULL, "opened isolated main() config for the non-due interval-scheduling test");
    if (fp) {
        fputs("pool/due,7,1,p,no,0\n", fp); fclose(fp);
        unlink(args_file);
        /* test_non_due_localtime ignores the time_t it's passed, so
         * overriding diffsnap_now()'s value has no observable effect on
         * this test -- only the localtime_now_fn override matters here. */
        localtime_now_fn = test_non_due_localtime;
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 0,
              "the interval-scheduling gate permits a clean no-work run");
        localtime_now_fn = localtime_r;
        CHECK(access(args_file, F_OK) != 0, "a non-due entry does not invoke zfs through main()");
    }

    fp = fopen(conf_file, "w");
    CHECK(fp != NULL, "opened isolated main() config for embedded-NUL rejection");
    if (fp) {
        const char nul_config[] = "pool/hidden,1,1,p,no,0\0,ignored\n";
        CHECK(fwrite(nul_config, 1, sizeof(nul_config) - 1, fp) == sizeof(nul_config) - 1,
              "wrote a config record containing an embedded NUL byte");
        fclose(fp);
        CHECK(diffsnap_real_main(1, (char *[]) {"diffsnap-test", NULL}) == 1,
              "main() rejects a config record whose NUL would otherwise conceal later fields");
        FILE *log = fopen(log_file, "r");
        char contents[4096] = {0};
        if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
        CHECK(strstr(contents, "Config error: NUL byte in line") != NULL,
              "the embedded-NUL config rejection is logged explicitly");
    }

    fp = fopen(conf_file, "w");
    CHECK(fp != NULL, "opened isolated main() config for the positive-DST-offset due-entry test");
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

    fp = fopen(conf_file, "w");
    CHECK(fp != NULL, "opened isolated main() config for the negative-UTC-offset due-entry test");
    if (fp) {
        /*
         * Coverage gap: main()'s `if (snap_time[19] == '+') snap_time[19]
         * = 'p';` (diffsnap.c ~line 1434) had a deterministic,
         * host-independent test for the branch being taken (positive
         * offset, above) but none for the branch NOT being taken. Every
         * other "due" test that doesn't override localtime_now_fn only
         * incidentally covers whichever offset sign the test host's real
         * local timezone happens to produce, which isn't reliable
         * coverage of this specific branch on every machine or CI runner
         * this suite might run on. This pins down the negative-offset
         * side explicitly: '-' must survive unmodified, since only '+'
         * is rejected by ZFS snapshot names.
         */
        fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
        unlink(args_file);
        localtime_now_fn = test_negative_offset_localtime;
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 0,
              "a due entry completes through main() with the fake zfs command under a negative UTC offset");
        localtime_now_fn = localtime_r;
        FILE *args = fopen(args_file, "r");
        char contents[8192] = {0};
        if (args) { size_t rd = fread(contents, 1, sizeof(contents) - 1, args); (void)rd; fclose(args); }
        CHECK(strstr(contents, "pool/due@p_2026-03-08_00:00:00-0500") != NULL,
              "main() leaves a negative UTC offset's '-' unmodified in snapshot names (only '+' is rewritten)");
    }

    fp = fopen(conf_file, "w");
    CHECK(fp != NULL, "opened isolated main() config for the recursive=yes end-to-end pipeline test");
    if (fp) {
        /*
         * Coverage gap: every recursive-specific behavior (ancestor
         * overlap resolution, pass assignment, subtree written-bytes
         * summing) was previously exercised only by calling the internal
         * batch functions directly against hand-built batch_ctx_t values
         * (Tests 12-19, 33-35, 35a, 38-39) -- never through the actual
         * config-parsing-to-snapshot pipeline. This drives a `recursive`
         * field of "yes" all the way through diffsnap_real_main(), so a
         * wiring bug in how main() dispatches a parsed config line into
         * rec_b (rather than std_b) -- or in how the recursive path
         * interacts with metrics scoping, snapshot creation, and
         * finalize_batch's logging at the main() level -- would actually
         * be caught.
         */
        fputs("pool/tree,1,1,rectest,yes,0\n", fp); fclose(fp);
        unlink(args_file);
        unlink(log_file);
        char rec_script[PATH_MAX + 256];
        CHECK(snprintf(rec_script, sizeof(rec_script),
                       "#!/bin/sh\n"
                       "if [ \"$1\" = get ]; then printf 'pool/tree\\t500\\n'; exit 0; fi\n"
                       "if [ \"$1\" = snapshot ]; then shift; printf '%%s\\n' \"$@\" >> '%s'; exit 0; fi\n"
                       "if [ \"$1\" = list ]; then printf 'pool/tree@rectest_2026-03-08_00:00:00p0500\\n'; exit 0; fi\n"
                       "exit 0\n", args_file) < (int)sizeof(rec_script) &&
              write_fake_zfs(rec_script) == 0,
              "fake zfs for main()'s recursive=yes end-to-end test installed");
        localtime_now_fn = test_positive_offset_localtime;
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 0,
              "a recursive=yes config entry completes successfully through the full main() pipeline");
        localtime_now_fn = localtime_r;
        FILE *args = fopen(args_file, "r");
        char contents[2048] = {0};
        if (args) { size_t rd = fread(contents, 1, sizeof(contents) - 1, args); (void)rd; fclose(args); }
        CHECK(strstr(contents, "-r\n") != NULL,
              "main() invokes the real `zfs snapshot` call with -r for a recursive=yes config entry");
        CHECK(strstr(contents, "pool/tree@rectest_2026-03-08_00:00:00p0500\n") != NULL,
              "the recursive snapshot invocation names the correctly formatted dataset@prefix_timestamp");
        FILE *log = fopen(log_file, "r");
        char log_contents[4096] = {0};
        if (log) { size_t rd = fread(log_contents, 1, sizeof(log_contents) - 1, log); (void)rd; fclose(log); }
        CHECK(strstr(log_contents, "Created=pool/tree@rectest_2026-03-08_00:00:00p0500 Written=500 Recursive") != NULL,
              "main() logs the recursive Created= line with the subtree written total, proving the recursive path -- not the standard one -- ran end to end through the real pipeline");
    }

    /*
     * Fixture-isolation fix: the recursive=yes block just above installed
     * rec_script, whose `get` branch unconditionally answers
     * "pool/tree\t500" no matter which dataset was actually requested.
     * Every block below this point that uses a "pool/due" config (the
     * log_io_failed exit-code test and the timestamp strftime-failure
     * test) must not be left silently running against a script that
     * doesn't understand "pool/due" -- batch_filter_by_metrics() would
     * treat pool/due as "not found" and drop it, which happens to not
     * break either of those two tests' own assertions today (one only
     * needs *some* log write to fail, which /dev/full guarantees
     * regardless of content; the other calls strftime_now_fn
     * unconditionally of batch state) but is exactly the kind of
     * incidental, order-dependent pass this suite otherwise goes out of
     * its way to avoid. Reinstall the known-good generic script here so
     * later "pool/due" blocks are verified against a fixture that
     * actually answers for the dataset they configure, not one left over
     * from an unrelated test.
     */
    CHECK(write_fake_zfs(script) == 0,
          "reinstalled the generic pool/due-aware fake zfs after the recursive=yes block replaced it with one that only understands pool/tree");

    int held_lock = open(lock_file, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    /* Capture the flock() outcome itself rather than re-deriving it from
     * held_lock alone: if open() succeeds but flock() fails, held_lock >= 0
     * would still be true, and gating the next CHECK on that alone would run
     * main() without the lock actually held -- main() would then likely
     * acquire it and exit 0, so the next CHECK ("main() exits unsuccessfully
     * when flock reports an existing instance") would fail for a completely
     * different, misleading reason instead of being cleanly skipped. */
    int lock_acquired = held_lock >= 0 && flock(held_lock, LOCK_EX | LOCK_NB) == 0;
    CHECK(lock_acquired, "test process acquired the isolated lock before invoking main()");
    if (lock_acquired) {
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
              "main() exits unsuccessfully when flock reports an existing instance");
        flock(held_lock, LOCK_UN);
    }
    if (held_lock >= 0) close(held_lock);

    fp = fopen(conf_file, "w");
    CHECK(fp != NULL, "opened isolated main() config for the log_io_failed exit-code test");
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
    CHECK(fp != NULL, "opened isolated main() config for the ZFS_NAME_MAX rejection test");
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
    CHECK(fp != NULL, "opened isolated main() config for the getline failure test");
    if (fp) {
        fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
        getline_now_fn = test_getline_failure;
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
              "main() fails when getline reports a config read error before EOF");
        getline_now_fn = getline;
    }

    fp = fopen(conf_file, "w");
    CHECK(fp != NULL, "opened isolated main() config for the top-level localtime_r failure test");
    if (fp) {
        fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
        g_localtime_calls = 0;
        g_localtime_fail = 1; localtime_now_fn = test_localtime;
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
              "main() fails when its top-level localtime_r call fails");
        FILE *log = fopen(log_file, "r");
        char contents[8192] = {0};
        if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
        CHECK(strstr(contents, "Error: localtime_r failed") != NULL,
              "the localtime_r failure, rather than an unrelated error, is logged");
        localtime_now_fn = localtime_r; g_localtime_fail = 0;
        CHECK(g_localtime_calls >= 1, "the localtime hook was actually reached by main()");
    }

    fp = fopen(conf_file, "w");
    CHECK(fp != NULL, "opened isolated main() config for the timestamp strftime failure test");
    if (fp) {
        fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
        g_strftime_calls = 0;
        g_strftime_fail = 1; strftime_now_fn = test_strftime;
        CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
              "main() fails when formatting the snapshot timestamp fails");
        FILE *log = fopen(log_file, "r");
        char contents[8192] = {0};
        if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
        CHECK(strstr(contents, "Failed to format timestamp") != NULL,
              "the timestamp-formatting failure, rather than an unrelated error, is logged");
        strftime_now_fn = strftime; g_strftime_fail = 0;
        CHECK(g_strftime_calls >= 1, "the strftime hook was actually reached by main()");
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
    CHECK(fp != NULL, "opened isolated main() config for the scoped metrics-fetch test");
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
    CHECK(fp != NULL, "opened isolated main() config for the unscoped metrics-fetch fallback test");
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
    CHECK(fp != NULL, "opened isolated main() config for the metrics-fetch argv allocation-failure test");
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

    printf("== Gap: main()'s own resolve_recursive_ancestor_overlaps() failure path is exercised through the real pipeline, not just the helper directly ==\n");
    {
        /*
         * Coverage gap: resolve_recursive_ancestor_overlaps()'s own -1
         * return (its "covered" array allocation failing) is unit-tested
         * directly against a hand-built batch_ctx_t (the OOM block in
         * run_fault_injection_tests), but main()'s own
         * "if (resolve_recursive_ancestor_overlaps(&rec_b) != 0) { ...
         * goto cleanup; }" wiring was never driven through
         * diffsnap_real_main() itself. A single due, recursive config
         * entry puts exactly one item into rec_b, so its first (and only)
         * allocation is deterministically the 10th diffsnap_realloc call
         * of the whole run: seen_set_add's growth + key copy (2 calls),
         * batch_add's growth + dataset copy + prefix copy (3 calls),
         * collect_due_roots' single root_list_add_unique growth + string
         * copy (2 calls), the metrics-fetch m_argv allocation (1 call),
         * and handle_metric_line's own growth allocation for the fake
         * zfs's one "pool/tree 500" line (1 call) account for the first
         * 9; resolve_recursive_ancestor_overlaps' "covered" allocation is
         * therefore call 10. Asserting the exact call count turns any
         * drift in this derivation into a loud, explicit failure instead
         * of a silently mistargeted injection (same rationale as the
         * metrics-fetch-argv test above).
         */
        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for the resolve_recursive_ancestor_overlaps() OOM test");
        if (fp) {
            fputs("pool/tree,1,1,rectest,yes,0\n", fp); fclose(fp);
            unlink(args_file);
            unlink(log_file);
            CHECK(write_fake_zfs("#!/bin/sh\nif [ \"$1\" = get ]; then printf 'pool/tree\\t500\\n'; exit 0; fi\nexit 0\n") == 0,
                  "fake zfs for main()'s resolve_recursive_ancestor_overlaps() OOM test installed");
            g_realloc_calls = 0; g_realloc_fail_after = 9; realloc_now_fn = test_realloc;
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
                  "main() fails when its own resolve_recursive_ancestor_overlaps() call reports an allocation failure");
            CHECK(g_realloc_calls == 10,
                  "the injected failure landed on exactly the 10th realloc call -- resolve_recursive_ancestor_overlaps' own 'covered' allocation, not an earlier or later one");
            realloc_now_fn = realloc; g_realloc_fail_after = -1;
            FILE *log = fopen(log_file, "r");
            char contents[1024] = {0};
            if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
            CHECK(strstr(contents, "Failed to check recursive ancestor overlaps") != NULL,
                  "the resolve_recursive_ancestor_overlaps() failure is logged with its own distinct diagnostic");
        }
        printf("\n");
    }

    printf("== Gap: main()'s own remove_recursive_overlaps() failure path is exercised through the real pipeline, not just the helper directly ==\n");
    {
        /*
         * Same rationale as the resolve_recursive_ancestor_overlaps() gap
         * above, for remove_recursive_overlaps()'s own -1 return (its
         * rec_keys array allocation failing). Reaching this call with
         * both std_b.count>0 and rec_b.count>0 (remove_recursive_overlaps
         * short-circuits to 0, allocating nothing, if either is empty)
         * requires one standard and one recursive due config entry on two
         * unrelated pools, so neither is dropped by ancestor-coverage
         * filtering before this call is reached.
         *
         * Call-by-call: line 1 (std) costs seen_set_add's growth+key copy
         * (2) and batch_add's growth+dataset+prefix copy (3) = 5; line 2
         * (rec) costs seen_set_add's key copy alone -- no growth, since
         * the shared seen_set_t already grew on line 1 -- (1) and
         * batch_add's own growth+dataset+prefix copy for the separate
         * rec_b array (3) = 4; collect_due_roots costs one
         * root_list_add_unique growth+copy for the std root (2) plus one
         * copy-only call for the unrelated rec root, whose growth was
         * already covered (1) = 3; the metrics-fetch m_argv allocation is
         * 1 call; handle_metric_line's single growth allocation covers
         * both fake-zfs output lines (metrics.count stays under
         * ALLOC_CHUNK_METRIC, so only the first line triggers growth) = 1
         * call; resolve_recursive_ancestor_overlaps -- which must SUCCEED
         * here, unlike the gap test above, so remove_recursive_overlaps
         * is actually reached -- costs its "covered" allocation plus its
         * "order" allocation for the one surviving rec_b item = 2 calls.
         * Total: 5+4+3+1+1+2 = 16 calls precede remove_recursive_overlaps'
         * own first allocation, which is therefore call 17.
         */
        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for the remove_recursive_overlaps() OOM test");
        if (fp) {
            fputs("poolstd/a,1,1,p,no,0\n", fp);
            fputs("poolrec/b,1,1,p,yes,0\n", fp);
            fclose(fp);
            unlink(args_file);
            unlink(log_file);
            CHECK(write_fake_zfs("#!/bin/sh\nif [ \"$1\" = get ]; then printf 'poolstd/a\\t500\\n'; printf 'poolrec/b\\t500\\n'; exit 0; fi\nexit 0\n") == 0,
                  "fake zfs for main()'s remove_recursive_overlaps() OOM test installed");
            g_realloc_calls = 0; g_realloc_fail_after = 16; realloc_now_fn = test_realloc;
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
                  "main() fails when its own remove_recursive_overlaps() call reports an allocation failure");
            CHECK(g_realloc_calls == 17,
                  "the injected failure landed on exactly the 17th realloc call -- remove_recursive_overlaps' own rec_keys allocation, not an earlier or later one");
            realloc_now_fn = realloc; g_realloc_fail_after = -1;
            FILE *log = fopen(log_file, "r");
            char contents[1024] = {0};
            if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
            CHECK(strstr(contents, "Failed to check recursive overlaps") != NULL &&
                  strstr(contents, "Failed to check recursive ancestor overlaps") == NULL,
                  "the remove_recursive_overlaps() failure is logged with its own distinct diagnostic, not the ancestor-overlap or metrics-fetch ones");
        }
        printf("\n");
    }

    printf("== Gap: main()'s own seen_set_add() allocation-failure branch is exercised through the real pipeline, not just the helper directly ==\n");
    {
        /*
         * Coverage gap: seen_set_add()'s own -1 return is unit-tested
         * directly against a hand-built seen_set_t (the OOM block in
         * run_fault_injection_tests), but main()'s own
         * "if (seen_rc == -1) { log_msg(...); global_status = 1;
         * continue; }" wiring (diffsnap.c ~line 1341) was never driven
         * through diffsnap_real_main() itself. A bug in that wiring --
         * e.g. forgetting the continue and falling through into the
         * duplicate-check or batch_add() with a dataset that was never
         * successfully tracked -- would go undetected by the suite's own
         * stated methodology for this exact class of gap (see the
         * resolve_recursive_ancestor_overlaps()/remove_recursive_overlaps()
         * gap tests above).
         *
         * A single due config entry makes seen_set_add()'s own growth
         * allocation (the empty seen->keys array) the very first
         * diffsnap_realloc call of the entire run -- nothing upstream of
         * it in main() allocates through this hook. Failing call 1
         * therefore lands deterministically on that growth, before the
         * key-copy strdup even runs.
         */
        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for the seen_set_add() OOM test");
        if (fp) {
            fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
            unlink(args_file);
            unlink(log_file);
            CHECK(write_fake_zfs("#!/bin/sh\nexit 0\n") == 0,
                  "fake zfs for main()'s seen_set_add() OOM test installed");
            g_realloc_calls = 0; g_realloc_fail_after = 0; realloc_now_fn = test_realloc;
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
                  "main() fails when its own seen_set_add() call reports an allocation failure");
            CHECK(g_realloc_calls == 1,
                  "the injected failure landed on exactly the 1st realloc call -- seen_set_add's own growth allocation, not an earlier or later one");
            realloc_now_fn = realloc; g_realloc_fail_after = -1;
            FILE *log = fopen(log_file, "r");
            char contents[1024] = {0};
            if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
            CHECK(strstr(contents, "Failed to track config entry for") != NULL,
                  "the seen_set_add() failure is logged with its own distinct diagnostic");
            CHECK(access(args_file, F_OK) != 0,
                  "main() actually takes the continue: the untracked entry never reaches zfs at all (batch_add/zfs_snapshot_batch is never invoked for it)");
        }
        printf("\n");
    }

    printf("== Gap: main()'s own batch_add() allocation-failure branch is exercised through the real pipeline, not just the helper directly ==\n");
    {
        /*
         * Same rationale as the seen_set_add() gap above, for batch_add()'s
         * own -1 return inside main()'s parsing loop (diffsnap.c
         * ~lines 1344-1345). Unlike the seen_rc==-1 branch, this one has
         * no `continue` after logging -- a distinct control-flow shape
         * (it's already the last statement in the loop body, so the
         * absence of `continue` is not currently observable, but the
         * wiring -- that global_status is set and the batch stays exactly
         * one item short -- still deserves its own end-to-end assertion
         * rather than only the helper-level one).
         *
         * A single due, non-recursive config entry makes seen_set_add()
         * succeed fully first (growth + key-copy strdup = 2 calls), so
         * batch_add()'s own growth allocation for std_b is exactly the
         * 3rd diffsnap_realloc call of the run.
         */
        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for the batch_add() OOM test");
        if (fp) {
            fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
            unlink(args_file);
            unlink(log_file);
            CHECK(write_fake_zfs("#!/bin/sh\nexit 0\n") == 0,
                  "fake zfs for main()'s batch_add() OOM test installed");
            g_realloc_calls = 0; g_realloc_fail_after = 2; realloc_now_fn = test_realloc;
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
                  "main() fails when its own batch_add() call reports an allocation failure");
            CHECK(g_realloc_calls == 3,
                  "the injected failure landed on exactly the 3rd realloc call -- batch_add's own growth allocation, not an earlier or later one");
            realloc_now_fn = realloc; g_realloc_fail_after = -1;
            FILE *log = fopen(log_file, "r");
            char contents[1024] = {0};
            if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
            CHECK(strstr(contents, "Failed to allocate batch entry for") != NULL,
                  "the batch_add() failure is logged with its own distinct diagnostic");
            CHECK(access(args_file, F_OK) != 0,
                  "main() never reaches zfs for an entry whose batch_add() call failed");
        }
        printf("\n");
    }

    printf("== Gap: comment and blank config lines are actually skipped (not just accepted as data by accident) ==\n");
    {
        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for comment/blank-line coverage");
        if (fp) {
            fputs("# a leading comment line\n", fp);
            fputs("\n", fp); /* a truly blank line: line[0] == '\\0' after trim */
            /* A comment containing an adjacent-comma pair: if the '#'/blank
             * check ever moved after the ",," check instead of before it,
             * this line would misfire "Error: Config error: adjacent comma
             * delimiters" even though it's just a comment. */
            fputs("# a comment,, with adjacent commas in it\n", fp);
            fputs("pool/due,1,1,p,no,0\n", fp);
            fputs("\n", fp);
            fputs("# a trailing comment\n", fp);
            fclose(fp);
            unlink(args_file);
            unlink(log_file);
            char due_script[PATH_MAX + 128];
            CHECK(snprintf(due_script, sizeof(due_script),
                           "#!/bin/sh\nprintf '%%s\\n' \"$@\" >> '%s'\n"
                           "if [ \"$1\" = get ]; then printf 'pool/due\\t100\\n'; fi\nexit 0\n", args_file) < (int)sizeof(due_script) &&
                  write_fake_zfs(due_script) == 0,
                  "fake zfs reinstalled for the comment/blank-line test (the previous block's script does not answer for pool/due)");
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 0,
                  "main() succeeds on a config mixing comment lines, blank lines, and one real entry");
            FILE *args = fopen(args_file, "r");
            char contents[1024] = {0};
            if (args) { size_t rd = fread(contents, 1, sizeof(contents) - 1, args); (void)rd; fclose(args); }
            CHECK(strstr(contents, "pool/due") != NULL,
                  "the real data line after the comments/blank lines is still parsed and acted on");
            FILE *log = fopen(log_file, "r");
            char log_contents[2048] = {0};
            if (log) { size_t rd = fread(log_contents, 1, sizeof(log_contents) - 1, log); (void)rd; fclose(log); }
            CHECK(strstr(log_contents, "Config error") == NULL,
                  "comment and blank lines never reach any config-error branch, including the adjacent-comma-delimiter check that runs right after the skip check");
        }
        printf("\n");
    }

    printf("== Gap: fclose(log_fp) flush-failure is a distinct branch from log_io_failed ==\n");
    {
        /*
         * log_io_failed (already covered via /dev/full) is set the moment
         * any individual fprintf/vfprintf to log_fp fails. fclose(log_fp)
         * returning nonzero is a SEPARATE branch reached when every write
         * to the stream already succeeded (log_io_failed stays 0) and only
         * the implicit final flush/close fails -- a real occurrence of
         * that against an ordinary file can't be coaxed into existing
         * portably (line-buffering, which main() enables successfully in
         * every environment tried, means each log line is already flushed,
         * and any failure already caught, before fclose() ever runs). So
         * this drives it through fclose_now_fn, the same style of
         * DIFFSNAP_TESTING-only injection point already used for
         * strftime/getline/localtime failures elsewhere in this suite.
         */
        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for the fclose(log_fp) flush-failure test");
        if (fp) {
            fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
            unlink(log_file);
            /* main()'s single cleanup label always closes conf (if
             * non-NULL) before log_fp, in that fixed order -- so with a
             * valid config file present (as set up above), the 1st hooked
             * close is the config close and the 2nd is the log close.
             * Target call #2 specifically so this test's failure is
             * attributable to the log close, not "whichever close the
             * hook happens to see first". */
            g_fclose_fail = 1; g_fclose_calls = 0; g_fclose_fail_at_call = 2; fclose_now_fn = test_fclose_failure;
            char stderr_buf[512] = {0};
            CHECK(run_main_capture_stderr(1, (char *[]){"diffsnap-test", NULL}, stderr_buf, sizeof(stderr_buf)) == 1,
                  "main() fails when fclose(log_fp) itself reports a flush/close error");
            fclose_now_fn = fclose; g_fclose_fail = 0; g_fclose_fail_at_call = 0;
            CHECK(g_fclose_calls == 2,
                  "diffsnap_fclose was invoked exactly twice (config, then log), confirming call #2 -- the one made to fail -- was the log close");
            CHECK(strstr(stderr_buf, "failed to flush log file") != NULL,
                  "the flush-failure diagnostic, distinct from the log_io_failed write-failure diagnostic, is reported on stderr");
        }
        printf("\n");
    }

    printf("== Gap: fclose(conf) failure is surfaced, not silently discarded ==\n");
    {
        /*
         * Mirrors the fclose(log_fp) test just above, but targets call #1
         * (the config close) instead of call #2 (the log close).
         * diffsnap_fclose(conf)'s return value used to be discarded
         * outright at main()'s cleanup label -- unlike log_fp, whose close
         * failure is checked and reported -- so a config file that reads
         * back completely successfully (every getline() call returns data,
         * feof() is set, the parse loop runs to completion normally) but
         * then fails specifically at close() -- the deferred-error pattern
         * some NFS/FUSE filesystems use, where a read that was actually
         * served from a stale or since-invalidated cache is only flagged
         * at the final close-to-open consistency check -- would previously
         * be silently ignored: main() would go on to create/prune real
         * snapshots based on that config and still exit 0. This drives
         * that path via the same fclose_now_fn hook, targeting the config
         * close specifically so the failure is unambiguously attributable
         * to it and not to the log close covered above.
         */
        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for the fclose(conf) close-failure test");
        if (fp) {
            fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
            unlink(log_file);
            /* Target call #1 (config close), the opposite of the log-close
             * test above. */
            g_fclose_fail = 1; g_fclose_calls = 0; g_fclose_fail_at_call = 1; fclose_now_fn = test_fclose_failure;
            char stderr_buf[512] = {0};
            int rc = run_main_capture_stderr(1, (char *[]){"diffsnap-test", NULL}, stderr_buf, sizeof(stderr_buf));
            fclose_now_fn = fclose; g_fclose_fail = 0; g_fclose_fail_at_call = 0;
            CHECK(rc == 1, "main() fails when fclose(conf) itself reports a close error, even though every line was read and acted on successfully");
            CHECK(g_fclose_calls == 2,
                  "diffsnap_fclose was invoked exactly twice (config, then log), confirming call #1 -- the one made to fail -- was the config close, and that the (unhooked) log close still ran afterward");
            FILE *log = fopen(log_file, "r");
            char log_contents[1024] = {0};
            if (log) { size_t rd = fread(log_contents, 1, sizeof(log_contents) - 1, log); (void)rd; fclose(log); }
            CHECK(strstr(log_contents, "failed to close config file") != NULL,
                  "the config close-failure diagnostic is written to the log file, not just stderr");
            CHECK(strstr(log_contents, "Created=pool/due@p_") != NULL,
                  "the config close failure is surfaced without suppressing the snapshot the run legitimately created before hitting cleanup -- confirming this test exercises the 'read succeeded, close failed after the fact' case, not a read failure");
        }
        printf("\n");
    }

    printf("== Gap: dataset/prefix/timestamp length check is exact-boundary tested against ZFS_NAME_MAX, not just clearly-oversized ==\n");
    {
        /*
         * Mirrors Test 45's treatment of the ARGV_BYTES_CAP boundary: a
         * sum that lands EXACTLY on ZFS_NAME_MAX must be accepted ('>'
         * excludes equality), and growing it by a single byte must flip
         * to rejected. The dataset is not due (interval 7, forced to
         * tm_min=1 via test_non_due_localtime) purely so this stays a
         * pure validation-path test -- the length check runs unconditionally
         * before the due check either way, so due-ness is irrelevant to
         * what's being verified here.
         */
        char boundary_dataset[201], boundary_prefix[30];
        memset(boundary_dataset, 'a', sizeof(boundary_dataset) - 1); boundary_dataset[200] = '\0';
        memset(boundary_prefix, 'p', sizeof(boundary_prefix) - 1); boundary_prefix[29] = '\0';
        CHECK(strlen(boundary_dataset) + 1 + strlen(boundary_prefix) + 1 + SNAPSHOT_TIMESTAMP_MAX == ZFS_NAME_MAX,
              "exact-boundary test constants land precisely on ZFS_NAME_MAX");

        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for the ZFS_NAME_MAX exact-boundary acceptance test");
        if (fp) {
            fprintf(fp, "%s,7,1,%s,no,0\n", boundary_dataset, boundary_prefix);
            fclose(fp);
            unlink(log_file);
            localtime_now_fn = test_non_due_localtime;
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 0,
                  "main() accepts a dataset/prefix/timestamp name whose length lands exactly on ZFS_NAME_MAX");
            localtime_now_fn = localtime_r;
            FILE *log = fopen(log_file, "r");
            char contents[1024] = {0};
            if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
            CHECK(strstr(contents, "exceeds ZFS limit") == NULL,
                  "the exact-boundary name is NOT flagged as exceeding the ZFS limit");
        }

        char over_dataset[202];
        memset(over_dataset, 'a', sizeof(over_dataset) - 1); over_dataset[201] = '\0'; /* one byte longer */
        CHECK(strlen(over_dataset) + 1 + strlen(boundary_prefix) + 1 + SNAPSHOT_TIMESTAMP_MAX == ZFS_NAME_MAX + 1,
              "over-boundary test constants land exactly one byte past ZFS_NAME_MAX");

        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for the ZFS_NAME_MAX over-boundary rejection test");
        if (fp) {
            fprintf(fp, "%s,7,1,%s,no,0\n", over_dataset, boundary_prefix);
            fclose(fp);
            unlink(log_file);
            localtime_now_fn = test_non_due_localtime;
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
                  "main() rejects a dataset/prefix/timestamp name exactly one byte past ZFS_NAME_MAX");
            localtime_now_fn = localtime_r;
            FILE *log = fopen(log_file, "r");
            char contents[1024] = {0};
            if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
            CHECK(strstr(contents, "exceeds ZFS limit") != NULL,
                  "the one-byte-over-boundary name IS flagged as exceeding the ZFS limit");
        }
        printf("\n");
    }

    printf("== Gap: a real fake-zfs script emitting ZFS's literal '-' (not applicable) written value ==\n");
    {
        /*
         * handle_metric_line's invalid (-1) written-value path was only
         * ever driven by directly-constructed metric_ctx_t values (Test 9)
         * or malformed rows with no such literal (Test 21a). ZFS itself
         * commonly prints a literal "-" for `written` when the property
         * doesn't apply to a given dataset/snapshot -- strtoll("-", ...)
         * leaves endptr pointing at the '-' itself (no digits consumed),
         * so this correctly falls into the same invalid-metric path, but
         * that had never been exercised through the real text-output ->
         * handle_metric_line -> batch_filter_by_metrics pipeline before.
         */
        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for the literal '-' written-value test");
        if (fp) {
            fputs("pool/dashval,1,1,p,no,0\n", fp); fclose(fp);
            unlink(args_file); unlink(log_file);
            char dash_script[PATH_MAX + 192];
            CHECK(snprintf(dash_script, sizeof(dash_script),
                           "#!/bin/sh\n"
                           "if [ \"$1\" = get ]; then printf 'pool/dashval\\t-\\n'; exit 0; fi\n"
                           "if [ \"$1\" = snapshot ]; then shift; printf '%%s\\n' \"$@\" >> '%s'; exit 0; fi\n"
                           "exit 0\n", args_file) < (int)sizeof(dash_script) &&
                  write_fake_zfs(dash_script) == 0,
                  "fake zfs emitting a literal '-' written value installed");
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
                  "main() fails when the real zfs get output reports a literal '-' written value");
            CHECK(access(args_file, F_OK) != 0,
                  "a dataset with an unparsable '-' written value is never handed to zfs snapshot");
            FILE *log = fopen(log_file, "r");
            char contents[1024] = {0};
            if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
            CHECK(strstr(contents, "invalid written metric") != NULL,
                  "the literal '-' value is logged as an invalid written metric, exactly like a directly-constructed -1");
        }
        printf("\n");
    }

    printf("== Gap: log_path is opened in append mode (\"ae\"), not truncated, across separate main() invocations ==\n");
    {
        /*
         * Coverage gap: main() opens the log file with
         * fopen(log_path, "ae") (diffsnap.c ~line 1280) -- append mode,
         * so a log line written by one cron-scheduled run must survive
         * into the next run's log file rather than being wiped out.
         * Every other main()-pipeline test in this suite unlink(log_file)s
         * before its own scenario (by design, so each test's log
         * assertions see only its own fresh content), so the "a" vs a
         * hypothetical "w" distinction was never actually exercised
         * anywhere: a regression that changed the fopen mode to
         * truncate-on-open would still pass every other test here, since
         * none of them ever look at more than one run's log file at once.
         *
         * This drives two separate diffsnap_real_main() invocations back
         * to back, deliberately WITHOUT unlinking log_file in between,
         * each against a fake zfs reporting a different (and therefore
         * individually identifiable) written-bytes value, then confirms
         * the log file contains BOTH runs' Created= lines afterward, in
         * the order they were written -- not just the second run's,
         * which is what a truncating open would leave behind.
         */
        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for the log-append test");
        if (fp) {
            fputs("pool/due,1,1,p,no,0\n", fp); fclose(fp);
            unlink(log_file);

            CHECK(write_fake_zfs("#!/bin/sh\nif [ \"$1\" = get ]; then printf 'pool/due\\t111\\n'; exit 0; fi\nexit 0\n") == 0,
                  "fake zfs for the log-append test's first run installed");
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 0,
                  "the first of two back-to-back main() runs succeeds");

            CHECK(write_fake_zfs("#!/bin/sh\nif [ \"$1\" = get ]; then printf 'pool/due\\t222\\n'; exit 0; fi\nexit 0\n") == 0,
                  "fake zfs for the log-append test's second run installed");
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 0,
                  "the second of two back-to-back main() runs succeeds without the log file being unlinked first");

            FILE *log = fopen(log_file, "r");
            char contents[4096] = {0};
            if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
            char *first_pos = strstr(contents, "Written=111");
            char *second_pos = strstr(contents, "Written=222");
            CHECK(first_pos != NULL,
                  "the FIRST run's log entry (Written=111) is still present in the log file after the second run -- proves append, not truncate");
            CHECK(second_pos != NULL,
                  "the second run's own log entry (Written=222) is also present");
            CHECK(first_pos != NULL && second_pos != NULL && first_pos < second_pos,
                  "the first run's entry precedes the second run's entry -- new content was appended after it, not written over it");
        }
        printf("\n");
    }

    printf("== Gap: main()'s own inventory_ok=0 wiring (a genuine `zfs list` failure, not an allocation failure) is exercised through the real pipeline ==\n");
    {
        /*
         * Coverage gap: load_combined_snapshot_inventory()'s strict-failure
         * branch is now unit-tested directly (Test 29b), but main()'s own
         * "if (load_combined_snapshot_inventory(...) != 0) { log_msg(...);
         * inventory_ok = 0; }" wiring, and finalize_batch()'s handling of
         * inventory_ok=0 for a snapshot that itself SUCCEEDED (as opposed
         * to Test 43's direct, hand-built inventory_ok=0 call), was never
         * driven through diffsnap_real_main() itself. Unlike every other
         * "Gap:" failure above, this one must let snapshot creation
         * succeed and only fail the subsequent `zfs list` call, so the
         * fake zfs script here handles "get" (metrics) and "snapshot"
         * normally and fails only "list".
         */
        fp = fopen(conf_file, "w");
        CHECK(fp != NULL, "opened isolated main() config for the inventory_ok=0 end-to-end test");
        if (fp) {
            fputs("pool/tree,1,1,invtest,no,0\n", fp); fclose(fp);
            unlink(log_file);
            CHECK(write_fake_zfs(
                      "#!/bin/sh\n"
                      "if [ \"$1\" = get ]; then printf 'pool/tree\\t500\\n'; exit 0; fi\n"
                      "if [ \"$1\" = snapshot ]; then exit 0; fi\n"
                      "if [ \"$1\" = list ]; then echo 'strict-list-failure' >&2; exit 1; fi\n"
                      "exit 0\n") == 0,
                  "fake zfs for the inventory_ok=0 end-to-end test installed: snapshot creation succeeds, `list` genuinely fails");
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
                  "main() reports overall failure when snapshot creation succeeded but the post-creation inventory listing genuinely failed");
            FILE *log = fopen(log_file, "r");
            char contents[2048] = {0};
            if (log) { size_t rd = fread(contents, 1, sizeof(contents) - 1, log); (void)rd; fclose(log); }
            CHECK(strstr(contents, "Error: Unable to list snapshots for batch verification and pruning") != NULL,
                  "main() logs the distinct inventory-listing-failed diagnostic, proving the failure was attributed to load_combined_snapshot_inventory, not some other step");
            CHECK(strstr(contents, "Created=pool/tree@invtest_") != NULL,
                  "the snapshot itself is still reported as created -- inventory_ok=0 must not be confused with (or mask) snapshot creation failure");
            CHECK(strstr(contents, "Error: Unable to prune snapshots for pool/tree: snapshot inventory unavailable") != NULL,
                  "finalize_batch's own inventory_ok=0 branch fires for a real, successfully-created snapshot reached through main(), not just the hand-built inventory_ok=0 call in Test 43");
        }
        printf("\n");
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
    int prior_time_override_active = diffsnap_time_override_is_active();
    time_t prior_time_override_value = diffsnap_time_override_get_value();
    time_t overridden_time;
    diffsnap_override_time((time_t)0);
    CHECK(diffsnap_now(&overridden_time) == 0 && overridden_time == (time_t)0,
      "the test overrides diffsnap's clock without changing the system clock");
    diffsnap_time_override_restore(prior_time_override_active, prior_time_override_value);

    char *buf = NULL;
    size_t buf_len = 0;
    FILE *prior_log_fp = log_fp;
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
        fclose(log_fp);
    }
    /* Restored unconditionally -- including when open_memstream() itself
     * failed above and this block's body never ran -- so a rare
     * allocation failure here can never leave log_fp silently stuck at a
     * dead/NULL stream for every later test in this process. */
    log_fp = prior_log_fp;
    free(buf);

    buf = NULL;
    buf_len = 0;
    prior_log_fp = log_fp;
    log_fp = open_memstream(&buf, &buf_len);
    CHECK(log_fp != NULL, "memory-backed log capture opened");
    if (log_fp) {
        g_strftime_calls = 0;
        g_strftime_fail = 1;
        strftime_now_fn = test_strftime;
        log_msg("message during strftime failure");
        strftime_now_fn = strftime;
        g_strftime_fail = 0;
        fflush(log_fp);
        size_t newlines = 0;
        for (size_t i = 0; i < buf_len; i++) if (buf[i] == '\n') newlines++;
        CHECK(g_strftime_calls == 1, "strftime hook intercepted the logging call");
        CHECK(buf && strstr(buf, "strftime failed") && strstr(buf, "message during strftime failure"),
              "strftime failure (not localtime_r failure) is described on the log message");
        CHECK(buf && strstr(buf, "localtime_r failed") == NULL,
              "a strftime-only failure is not misreported as a localtime_r failure");
        CHECK(newlines == 1, "timestamp fallback produces exactly one log line");
        fclose(log_fp);
    }
    /* Restored unconditionally -- see the equivalent fix above. */
    log_fp = prior_log_fp;
    free(buf);

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

    /* batch_add() does `d = diffsnap_strdup(dataset); p = diffsnap_strdup(prefix);
     * if (!d || !p) { free(d); free(p); return -1; }`. Drive each strdup's
     * allocation failure separately: fail_after=1 fails on the dataset copy
     * (call 2, right after the growth realloc succeeds as call 1), and
     * fail_after=2 fails on the prefix copy specifically -- the branch where
     * `d` is non-NULL and must be freed alongside the failed `p`. Testing
     * only the first case would leave that free(d) path unexercised. */
    g_realloc_calls = 0; g_realloc_fail_after = 1;
    batch_ctx_t batch_dup = {0};
    CHECK(batch_add(&batch_dup, "pool/a", "p", 1, 0) == -1,
          "batch_add reports an injected dataset string-copy allocation failure after a successful growth");
    CHECK(batch_dup.count == 0, "batch_add leaves the batch empty when the dataset string copy fails");
    batch_free(&batch_dup);

    g_realloc_calls = 0; g_realloc_fail_after = 2;
    batch_ctx_t batch_dup2 = {0};
    CHECK(batch_add(&batch_dup2, "pool/a", "p", 1, 0) == -1,
          "batch_add reports an injected prefix string-copy allocation failure after the dataset copy succeeds");
    CHECK(batch_dup2.count == 0, "batch_add leaves the batch empty when the prefix string copy fails");
    batch_free(&batch_dup2);
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
    /* Explicitly set (not relied-upon carryover): this must fail on the
     * very first realloc call inside remove_recursive_overlaps (the
     * rec_keys array growth), regardless of what any earlier block in
     * this function left g_realloc_fail_after set to. See the comment
     * above the two OOM blocks below for why implicit carryover here is
     * exactly the ordering hazard this suite tries to avoid. */
    g_realloc_calls = 0; g_realloc_fail_after = 0; realloc_now_fn = test_realloc;
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

    name_list_t inventory = {0};
    inventory.names = calloc(1, sizeof(*inventory.names));
    if (inventory.names) {
        inventory.names[0] = strdup("pool/a@p_2026-01-01_00:00:00"); inventory.count = 1; inventory.capacity = 1;
        char **matches = NULL; size_t matches_cap = 0;
        /* Explicit, not relied-upon carryover: this must fail on the very
         * first realloc call inside prune_from_inventory (the match-list
         * growth) regardless of what any earlier block in this function
         * left g_realloc_fail_after set to. Previously this read
         * g_realloc_fail_after=0 left over from the load_combined_
         * snapshot_inventory cleanup a few lines above -- harmless today,
         * but that value was never actually *for* this block, so removing
         * or reordering that unrelated cleanup line would have silently
         * disabled the failure injection here instead of loudly breaking
         * this CHECK. See the comment above the remove_recursive_overlaps()
         * OOM blocks earlier in this function for why implicit carryover
         * here is exactly the ordering hazard this suite otherwise avoids. */
        g_realloc_calls = 0; g_realloc_fail_after = 0; realloc_now_fn = test_realloc;
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
    run_ensure_std_fds_all_closed_test();
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

    printf("== Test 3a: signal death after partial stdout output is still treated as failure, and the partial output already delivered is not lost ==\n");
    {
        /*
         * Coverage gap: every fake-zfs/subprocess test up to this point
         * either produces no output at all before dying (Test 3) or
         * produces output and exits cleanly (Test 4, Test 30). A real
         * `zfs` process that gets OOM-killed or otherwise signaled
         * partway through a long listing would do both at once: emit
         * some genuinely valid lines, then die abnormally. Combining
         * Test 3's self-signal with Test 4's handler-delivery check
         * proves exec_cmd_stream_core drains and hands off whatever was
         * already written to the pipe before deciding the overall
         * outcome, rather than discarding buffered-but-unprocessed
         * output just because the child terminated abnormally.
         */
        const char *const sh_candidates[] = {"/bin/sh", "/usr/bin/sh", NULL};
        const char *sh_bin = find_bin(sh_candidates);
        CHECK(sh_bin != NULL, "found a shell for the partial-output-then-signal test");
        if (sh_bin) {
            metric_ctx_t ctx = {0};
            const char *const argv[] = {sh_bin, "-c",
                "printf 'pool/child\\t12345\\n'; kill -9 $$", NULL};
            int rc = exec_cmd_stream(argv, handle_metric_line, &ctx);
            CHECK(rc != 0, "signal death is treated as failure even after valid output was already delivered");
            CHECK(ctx.count == 1, "the single line written before the signal was still delivered to the handler");
            if (ctx.count == 1) {
                CHECK(strcmp(ctx.items[0].name, "pool/child") == 0, "the pre-signal line's dataset name was parsed correctly");
                CHECK(ctx.items[0].written == 12345, "the pre-signal line's written value was parsed correctly");
            }
            free(ctx.items);
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

            /*
             * exec_cmd_stream_lenient is the wrapper main() actually uses
             * for the scoped `zfs get` metrics fetch, and it's expected to
             * deliver handler output identically to the strict wrapper
             * above; only its exit-code tolerance differs. Every other
             * lenient-specific test in this file (Test 2, Test 20) passes
             * handler=NULL and checks only exit-code semantics, so without
             * this block the lenient wrapper's handler-delivery path would
             * be exercised only by the optional --system suite. Cover it
             * here directly so a regression that broke handler wiring
             * specifically inside exec_cmd_stream_lenient (e.g. dropping
             * the handler pointer before calling exec_cmd_stream_core)
             * fails the default suite too.
             */
            metric_ctx_t lenient_ctx = {0};
            const char *const lenient_argv[] = {sh_bin, "-c", "printf 'pool/child2\\t54321\\n'", NULL};
            int lenient_rc = exec_cmd_stream_lenient(lenient_argv, handle_metric_line, &lenient_ctx);
            CHECK(lenient_rc == 0, "lenient call with echo succeeds");
            CHECK(lenient_ctx.count == 1, "lenient wrapper's handler received exactly one parsed line");
            if (lenient_ctx.count == 1) {
                CHECK(strcmp(lenient_ctx.items[0].name, "pool/child2") == 0,
                      "lenient wrapper's parsed dataset name is correct");
                CHECK(lenient_ctx.items[0].written == 54321,
                      "lenient wrapper's parsed written value is correct");
            }
            free(lenient_ctx.items);
        }
        printf("\n");
    }

    printf("== Test 4a: unterminated stdout records are rejected by both wrappers ==\n");
    {
        const char *const sh_candidates[] = {"/bin/sh", "/usr/bin/sh", NULL};
        const char *sh_bin = find_bin(sh_candidates);
        CHECK(sh_bin != NULL, "found a POSIX shell for the unterminated-record test");
        if (sh_bin) {
            const char *const argv[] = {sh_bin, "-c", "printf 'pool/partial\\t12345'", NULL};
            metric_ctx_t strict_ctx = {0}, lenient_ctx = {0};
            CHECK(exec_cmd_stream(argv, handle_metric_line, &strict_ctx) != 0,
                  "strict rejects stdout ending in an unterminated metric record");
            CHECK(strict_ctx.count == 0,
                  "strict never delivers the unterminated metric record to its handler");
            CHECK(exec_cmd_stream_lenient(argv, handle_metric_line, &lenient_ctx) != 0,
                  "lenient also rejects an unterminated record despite its relaxed exit-status policy");
            CHECK(lenient_ctx.count == 0,
                  "lenient never delivers the unterminated metric record to its handler");
            free(strict_ctx.items);
            free(lenient_ctx.items);
        }
        printf("\n");
    }

    printf("== Test 4b: zfs_pool_len/same_zfs_root correctly distinguish adjacent pool names that are textual prefixes of one another ==\n");
    {
        /* Every other test in this file that exercises pass-grouping
         * (batch_item_in_root_pass/batch_root_pass_count, via same_zfs_root)
         * uses pool names that are never prefixes of one another (pool,
         * otherpool, thirdpool, poolA, poolB, ...). That leaves an
         * off-by-one in zfs_pool_len's slash-boundary arithmetic, or in
         * same_zfs_root's length-equality check, completely uncaught: e.g.
         * comparing only via strncmp without also requiring a_len == b_len
         * would let "pool" wrongly match a genuinely different, longer pool
         * like "pool2". This test drives that directly, plus the boundary
         * case in the other direction where two *different-looking*
         * strings genuinely share one top-level pool. */
        CHECK(zfs_pool_len("pool") == 4, "zfs_pool_len on a bare pool name (no slash) returns the full length");
        CHECK(zfs_pool_len("pool/a") == 4, "zfs_pool_len stops exactly at the slash, not one before or after it");
        CHECK(zfs_pool_len("pool2") == 5, "zfs_pool_len on a longer, unrelated bare pool name returns ITS full length, not the shorter one's");
        CHECK(zfs_pool_len("pool2/a") == 5, "zfs_pool_len stops at the slash for the longer pool name too");

        CHECK(same_zfs_root("pool", "pool") == 1, "identical bare pool names are the same root");
        CHECK(same_zfs_root("pool", "pool2") == 0,
              "\"pool\" and \"pool2\" are NOT the same root -- same_zfs_root must reject this on length alone, not just a strncmp prefix match");
        CHECK(same_zfs_root("pool2", "pool") == 0, "the same check holds with the operands swapped");
        CHECK(same_zfs_root("pool/a", "pool2/b") == 0,
              "datasets under two different, prefix-related pools (\"pool\" vs \"pool2\") are not the same root");
        CHECK(same_zfs_root("pool2/a", "pool2/b") == 1, "two datasets genuinely under the SAME longer pool name are the same root");
        CHECK(same_zfs_root("poolX/a", "poolXX/b") == 0,
              "a one-character difference in otherwise-identical-looking pool names (poolX vs poolXX) is not the same root");

        /* The genuinely-nested case in the other direction: a bare pool
         * name and a dataset one level under that SAME pool must compare
         * equal, since zfs_pool_len reduces both to the same top-level
         * pool string. */
        CHECK(same_zfs_root("tank", "tank/a") == 1,
              "a bare pool name and a dataset nested one level under that same pool are the same root");
        CHECK(same_zfs_root("tank/a", "tank/b") == 1, "two datasets nested under the same pool are the same root");
        CHECK(same_zfs_root("tank/a", "tank2/a") == 0,
              "same relative sub-path under two different, prefix-related pools (tank vs tank2) is not the same root");

        /* Exercise the real call sites (batch_item_in_root_pass via
         * batch_root_pass_count), not just same_zfs_root in isolation, with
         * the same adjacent "pool"/"pool2" pair. */
        batch_ctx_t ctx = {0};
        CHECK(batch_add(&ctx, "pool/a", "p", 1, 0) == 0 && batch_add(&ctx, "pool2/b", "p", 1, 0) == 0,
              "batch_add succeeded for both adjacent-name items during setup");
        CHECK(batch_root_pass_count(&ctx, "pool") == 1,
              "batch_root_pass_count for root \"pool\" counts only the item actually under \"pool\", not the unrelated \"pool2\" item");
        CHECK(batch_root_pass_count(&ctx, "pool2") == 1,
              "batch_root_pass_count for root \"pool2\" likewise counts only its own item");
        CHECK(batch_item_in_root_pass(&ctx, 0, "pool", 0) == 1 && batch_item_in_root_pass(&ctx, 0, "pool2", 0) == 0,
              "the \"pool\" item is in root \"pool\"'s pass but NOT wrongly reported as being in root \"pool2\"'s pass");
        CHECK(batch_item_in_root_pass(&ctx, 1, "pool2", 0) == 1 && batch_item_in_root_pass(&ctx, 1, "pool", 0) == 0,
              "the \"pool2\" item is in root \"pool2\"'s pass but NOT wrongly reported as being in root \"pool\"'s pass");
        batch_free(&ctx);
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

    printf("== Test 17a: find_metric's own length guard rejects an oversized dataset name before it could overflow key.name ==\n");
    {
        /*
         * find_metric() does `memcpy(key.name, dataset, len + 1)` into a
         * fixed key.name[STR_BUF_LARGE] buffer, guarded by
         * `len >= sizeof(key.name) -> return NULL`. handle_metric_line()
         * has its own, separate oversized-name guard on the way IN, so a
         * name this long could never actually reach the metrics array
         * through normal ingestion -- meaning find_metric's own guard,
         * called directly (as it is here, and as sum_subtree_written()
         * and batch_filter_by_metrics() call it internally), would never
         * otherwise be exercised. Pin both the accepted boundary (one
         * byte below the buffer size) and the rejected one (exactly at
         * the buffer size) so an off-by-one in this guard doesn't quietly
         * turn into a stack buffer overflow.
         */
        metric_ctx_t metrics = {0};
        char boundary_name[STR_BUF_LARGE]; /* STR_BUF_LARGE == sizeof(key.name) == 256; 255 chars + NUL fits exactly */
        memset(boundary_name, 'a', sizeof(boundary_name) - 1);
        boundary_name[sizeof(boundary_name) - 1] = '\0';
        char boundary_line[STR_BUF_LARGE + 16];
        CHECK(snprintf(boundary_line, sizeof(boundary_line), "%s\t123", boundary_name) < (int)sizeof(boundary_line),
              "constructed a metric line for the longest name find_metric can legally hold (255 chars)");
        CHECK(handle_metric_line(boundary_line, &metrics) == 0 && metrics.count == 1,
              "handle_metric_line accepts and stores the 255-char boundary name");
        qsort(metrics.items, metrics.count, sizeof(*metrics.items), compare_metrics);
        CHECK(find_metric(&metrics, boundary_name) != NULL && find_metric(&metrics, boundary_name)->written == 123,
              "find_metric succeeds for a dataset name exactly one byte below sizeof(key.name)");

        char too_long_name[STR_BUF_LARGE + 1]; /* 256 chars: exactly at sizeof(key.name), one past the accepted boundary */
        memset(too_long_name, 'a', sizeof(too_long_name) - 1);
        too_long_name[sizeof(too_long_name) - 1] = '\0';
        CHECK(find_metric(&metrics, too_long_name) == NULL,
              "find_metric's own length guard rejects a 256-char dataset name (len >= sizeof(key.name)) instead of memcpy-ing past key.name's end");

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

    printf("== Test 19a: batch_filter_by_metrics(recursive=1) -- a subtree-sum overflow is dropped and flagged exactly like a missing/invalid root, not silently mishandled ==\n");
    {
        /*
         * Coverage gap: Test 12a proves sum_subtree_written() itself
         * returns -1 on an LLONG_MAX overflow, but nothing previously
         * drove that overflow through the actual batch_filter_by_metrics
         * call chain the recursive/min_bytes feature uses in production.
         * A regression that special-cased the overflow return differently
         * from an ordinary "root not found" failure (e.g. silently
         * treating it as written==0 and applying the min_bytes threshold
         * instead of dropping the item outright) would not have been
         * caught anywhere else in this suite.
         */
        batch_ctx_t b = {0};
        CHECK(batch_add(&b, "pool/parent", "p", 1, 0) == 0,
              "Test 19a batch setup succeeds");

        metric_ctx_t metrics = {0};
        metrics.items = calloc(2, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/parent");       metrics.items[0].written = LLONG_MAX;
        strcpy(metrics.items[1].name, "pool/parent/child"); metrics.items[1].written = 1;
        metrics.count = 2;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        char *buf = NULL;
        size_t buf_len = 0;
        FILE *prior_log_fp = log_fp;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for Test 19a's log capture");

        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, 1, &global_status);
        if (log_fp) fflush(log_fp);

        CHECK(b.count == 0, "the item is dropped when its recursive subtree sum overflows, exactly as it would be for a missing/invalid root");
        CHECK(global_status == 1, "global_status is flagged for the overflow case, the same as the missing/invalid-root cases in Tests 8-9");
        CHECK(buf != NULL && strstr(buf, "Configured recursive dataset not found or has invalid written metric: pool/parent") != NULL,
              "the overflow is reported through the same diagnostic used for a missing/invalid root, not a distinct or silent path");

        if (log_fp) fclose(log_fp);
        log_fp = prior_log_fp;
        free(buf);
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
        FILE *prior_log_fp = log_fp;
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
            fclose(log_fp);
        }
        /* Restored unconditionally, even if open_memstream() above failed
         * and this block's body never ran. */
        log_fp = prior_log_fp;
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

        /* A raw NUL must be rejected before the handler's C-string parsing
         * can make it disappear along with a malicious suffix. */
        stream_reader_t nul_reader = {0};
        nul_reader.is_stderr = 0;
        nul_reader.handler = spy_line_handler;
        char nul_stdout[] = {'p', 'o', 'o', 'l', '\t', '1', '\0', 'x', '\n'};
        g_spy_calls = 0;
        stream_reader_consume(&nul_reader, nul_stdout, (ssize_t)sizeof(nul_stdout));
        CHECK(nul_reader.failed == 1, "a NUL-containing stdout record fails the command stream");
        CHECK(g_spy_calls == 0, "a NUL-containing stdout record is never handed to its parser");

        char *log_buf = NULL; size_t log_len = 0;
        FILE *prior_log_fp = log_fp;
        log_fp = open_memstream(&log_buf, &log_len);
        CHECK(log_fp != NULL, "opened a log capture for NUL-containing stderr");
        if (log_fp) {
            stream_reader_t stderr_reader = {0};
            stderr_reader.is_stderr = 1;
            char nul_stderr[] = {'b', 'a', 'd', '\0', 'x', '\n'};
            stream_reader_consume(&stderr_reader, nul_stderr, (ssize_t)sizeof(nul_stderr));
            fflush(log_fp);
            CHECK(stderr_reader.failed == 0 && strstr(log_buf, "bad\\x00x") != NULL,
                  "a NUL in stderr is safely escaped in its diagnostic log line");
            fclose(log_fp);
        }
        /* Restored unconditionally, even if open_memstream() above failed
         * and this block's body never ran. */
        log_fp = prior_log_fp;
        free(log_buf);

        printf("\n");
    }

    printf("== Test 22d: stream_reader_consume flushes (not drops) an overlong stderr line across multiple buffer-overflow boundaries, unlike stdout's fail-and-discard ==\n");
    {
        /*
         * Coverage gap: Test 22 proves the stdout overflow branch marks
         * the reader failed and never hands truncated content to
         * handler(). The comment right above that branch in
         * stream_reader_consume() ("only stderr lines ... still get
         * flushed after this point") documents a DIFFERENT behavior for
         * stderr that no test exercised: for stderr, overflow does NOT
         * set failed, and the buffered content so far is flushed via
         * stream_reader_line() as its own diagnostic log line, then
         * accumulation continues for the rest of the same (still
         * newline-less) line. A single overlong stderr line can
         * therefore legitimately produce several separate
         * "Error: zfs: ..." log entries rather than one. This drives a
         * stderr chunk long enough to cross that overflow boundary
         * twice, then explicitly flushes the trailing remainder the same
         * way drain_command_streams does at EOF (diffsnap.c ~line 491),
         * and proves every byte of the original line reappears across
         * the log entries -- none dropped, none handed to a parser, and
         * the reader never marked failed.
         */
        char *log_buf = NULL; size_t log_len = 0;
        FILE *prior_log_fp = log_fp;
        log_fp = open_memstream(&log_buf, &log_len);
        CHECK(log_fp != NULL, "opened a log capture for the overlong-stderr-line test");
        if (log_fp) {
            stream_reader_t reader = {0};
            reader.is_stderr = 1;

            /* 511 usable bytes per buffer fill (STR_BUF_XLARGE-1); two
             * full fills plus a final partial remainder, all one
             * logical (newline-less) line. */
            const size_t total = 511 * 2 + 50;
            char *overlong = malloc(total);
            CHECK(overlong != NULL, "allocated the overlong single-line stderr payload");
            if (overlong) {
                memset(overlong, 'B', total);
                stream_reader_consume(&reader, overlong, (ssize_t)total);
                CHECK(reader.failed == 0,
                      "an overlong stderr line does NOT mark the reader failed (unlike the equivalent stdout case in Test 22)");
                CHECK(reader.used == 50,
                      "the trailing 50-byte remainder (after two 511-byte overflow flushes) is still buffered, not discarded, pending EOF or a newline");

                /* Mirror drain_command_streams' own EOF-time flush of a
                 * stderr reader with leftover content (diffsnap.c line
                 * ~491), the only place the trailing remainder gets
                 * logged in the real pipeline. */
                stream_reader_line(&reader);
                fflush(log_fp);

                int error_lines = 0;
                size_t total_bs_logged = 0;
                const char *p = log_buf;
                while ((p = strstr(p, "Error: zfs: ")) != NULL) {
                    error_lines++;
                    p += strlen("Error: zfs: ");
                    const char *q = p;
                    while (*q == 'B') { total_bs_logged++; q++; }
                }
                CHECK(error_lines == 3,
                      "the overlong stderr line produced exactly three separate diagnostic log entries: two mid-line overflow flushes plus the final EOF-time flush of the remainder");
                CHECK(total_bs_logged == total,
                      "every one of the original line's bytes shows up across those log entries -- the overflow-flush boundary neither drops nor duplicates content");

                free(overlong);
            }
            fclose(log_fp);
        }
        log_fp = prior_log_fp;
        free(log_buf);
        printf("\n");
    }

    printf("== Test 22a: snapshot inventory accepts exactly one complete snapshot name per line ==\n");
    {
        name_list_t inventory = {0};
        char *log_buf = NULL; size_t log_len = 0;
        FILE *prior_log_fp = log_fp;
        log_fp = open_memstream(&log_buf, &log_len);
        CHECK(log_fp != NULL, "opened a log capture for invalid snapshot inventory rows");
        if (log_fp) {
            CHECK(handle_snapshot_inventory_line("pool/ds@snap", &inventory) == 0 &&
                      handle_snapshot_inventory_line("pool/ds", &inventory) == -1 &&
                      handle_snapshot_inventory_line("pool/ds@snap\textra", &inventory) == -1 &&
                      handle_snapshot_inventory_line("pool/ds@snap@extra", &inventory) == -1,
                  "an inventory row must contain exactly one complete snapshot name");
            /* The remaining two of handle_snapshot_inventory_line's five
             * validation branches: a line where '@' is the very first
             * character (at == line, no dataset part at all) and a line
             * where '@' is the very last character (at[1] == '\0', an
             * empty snapshot suffix). Neither was previously driven by any
             * test here. */
            CHECK(handle_snapshot_inventory_line("@snap", &inventory) == -1,
                  "a line starting with '@' (no dataset name before it) is rejected, not treated as a valid empty-dataset row");
            CHECK(handle_snapshot_inventory_line("pool/ds@", &inventory) == -1,
                  "a line ending right at '@' (empty snapshot suffix) is rejected, not treated as a valid empty-snapshot row");
            fflush(log_fp);
            CHECK(inventory.count == 1 && strstr(log_buf, "Invalid snapshot inventory line"),
                  "invalid inventory rows are logged and rejected before they enter pruning");
            fclose(log_fp);
        }
        /* Restored unconditionally, even if open_memstream() above failed
         * and this block's body never ran. */
        log_fp = prior_log_fp;
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

    printf("== Test 22c: zfs_destroy omits -r for non-recursive destruction ==\n");
    {
        /*
         * Coverage gap: prune_from_inventory-driven tests (e.g. Test 23)
         * exercise zfs_destroy(..., 0) but only ever assert on the
         * "Pruned=" log line, never on the literal argv passed to zfs --
         * so a copy/paste bug that added "-r" to (or dropped "destroy"
         * from) the non-recursive branch of zfs_destroy would go
         * undetected even though the symmetric recursive branch is
         * checked directly in Test 22b above. This asserts the
         * non-recursive argv directly, the same way Test 22b does for
         * the recursive one.
         */
        char trace_path[PATH_MAX];
        CHECK(snprintf(trace_path, sizeof(trace_path), "%s/destroy-args-nr", g_fake_zfs_dir) < (int)sizeof(trace_path),
              "non-recursive-destroy argv trace path fits in the isolated test directory");
        char script[PATH_MAX + 128];
        CHECK(snprintf(script, sizeof(script), "#!/bin/sh\nprintf '%%s\\n' \"$@\" >> '%s'\n", trace_path) < (int)sizeof(script) &&
                  write_fake_zfs(script) == 0,
              "installed fake zfs that records non-recursive destroy argv");
        CHECK(zfs_destroy("pool/ds@snap", 0) == 0, "non-recursive zfs_destroy succeeds through the fake zfs command");
        FILE *trace = fopen(trace_path, "r");
        char contents[256] = {0};
        if (trace) { size_t rd = fread(contents, 1, sizeof(contents) - 1, trace); (void)rd; fclose(trace); }
        CHECK(strstr(contents, "destroy\n") && strstr(contents, "pool/ds@snap\n"),
              "non-recursive zfs_destroy still passes the expected destroy/target arguments");
        CHECK(strstr(contents, "-r\n") == NULL,
              "non-recursive zfs_destroy does NOT pass -r to zfs, unlike the recursive branch in Test 22b");
        unlink(trace_path);
        unlink(g_fake_zfs);
        printf("\n");
    }

    printf("== Test 23: prune_from_inventory's date_stamp_like gate excludes prefix-matching but non-date-shaped snapshot names ==\n");
    {
        /* This test owns its fake command: ordinary destroy calls succeed
         * without touching real ZFS.
         *
         * Fidelity check: the script also appends every invocation's own
         * args to call_log. Without this, the test's only evidence that
         * the malformed entry was "never selected for pruning" is
         * prune_from_inventory's OWN log_msg() output -- which really only
         * proves what the function logged, not what it actually told zfs
         * to destroy. A bug that destroyed the malformed snapshot without
         * (or incorrectly) logging that destroy would still pass a
         * log-text-only check. Reading call_log lets the test confirm
         * which snapshot names zfs was actually invoked against,
         * independent of what prune_from_inventory chose to log. */
        char call_log23[PATH_MAX];
        CHECK(snprintf(call_log23, sizeof(call_log23), "%s/test23-calls", g_fake_zfs_dir) < (int)sizeof(call_log23),
              "constructed a path for Test 23's fake-zfs call log");
        unlink(call_log23);

        /*
         * Fidelity fix: this previously logged each invocation's argv via
         * `echo "$*"`, which space-joins every argument onto one line and
         * so cannot distinguish an argument boundary from a literal space
         * inside an argument. valid_dataset() explicitly permits spaces in
         * dataset/snapshot names (see its own comment), so a space-joined
         * log is fundamentally ambiguous for verifying real zfs argv in
         * general, even though this particular test doesn't currently use
         * space-containing names. Every other fake script in this file
         * that needs to verify argv (Test 22b/22c, Test 24, run_chunk_test,
         * Test 44, Test 45, Test 29a/29b) uses `printf '%s\n' "$@"`
         * instead, which preserves exact argument boundaries one per line
         * regardless of an argument's own content. Matching that here
         * keeps this test's call-log verification meaningful even if it's
         * later extended to a space-containing name, instead of being a
         * landmine that only looks correct today.
         */
        char script23[PATH_MAX + 64];
        CHECK(snprintf(script23, sizeof(script23), "#!/bin/sh\nprintf '%%s\\n' \"$@\" >> '%s'\nexit 0\n", call_log23) < (int)sizeof(script23) &&
              write_fake_zfs(script23) == 0,
              "fake zfs script created outside the system ZFS path for Test 23");

        /* Capture log_msg() output so we can see exactly what got pruned,
         * the same technique test_localtime_standalone.c uses. */
        char *buf = NULL;
        size_t buf_len = 0;
        FILE *prior_log_fp = log_fp;
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
              "the non-date-shaped entry was never selected for pruning, per the application's own log");
        CHECK(buf != NULL && strstr(buf, "myprefix_2026-01-01_00:00:00") != NULL,
              "the oldest genuinely date-stamped entry is the one that was pruned, leaving the newest one retained, per the application's own log");

        char call_log23_contents[1024] = {0};
        FILE *call_log23_fp = fopen(call_log23, "r");
        if (call_log23_fp) {
            size_t rd = fread(call_log23_contents, 1, sizeof(call_log23_contents) - 1, call_log23_fp);
            (void)rd;
            fclose(call_log23_fp);
        }
        int destroy_calls = 0;
        {
            const char *p = call_log23_contents;
            while ((p = strstr(p, "destroy")) != NULL) { destroy_calls++; p += 7; }
        }
        CHECK(destroy_calls == 1,
              "exactly one `zfs destroy` invocation actually reached the fake command, matching the single logged prune");
        CHECK(strstr(call_log23_contents, "myprefix_garbage-not-a-date") == NULL,
              "the malformed non-date-shaped name was never passed as an argument to any real zfs invocation, not merely absent from the application's own log text");
        CHECK(strstr(call_log23_contents, "myprefix_2026-01-01_00:00:00") != NULL,
              "the actual `zfs destroy` invocation's argv names the oldest genuinely date-stamped snapshot");

        unlink(call_log23);
        if (log_fp) fclose(log_fp);
        log_fp = prior_log_fp;
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
         * stderr, which exec_cmd_stream must drain and route to log_msg.
         *
         * Fidelity check: the script also appends its own invocation's
         * args plus which branch it took to call_log. Without this, the
         * test's only evidence that "the only nonzero exit is on the
         * matched root" is ctx.items[*].snap_failed AFTER the fact --
         * which really only proves diffsnap.c's own handling of whatever
         * exit codes it happened to receive, not that this script
         * actually returned exit 1 for badroot specifically and exit 0
         * for goodroot specifically (e.g. a typo'd case pattern that
         * accidentally exits 1 for every invocation, or 0 for every
         * invocation, could still leave snap_failed looking plausible if
         * some other bug happened to compensate). Reading call_log lets
         * the test confirm the exit code actually taken for each
         * dataset's own invocation, independent of how diffsnap.c later
         * interprets it. */
        char call_log[PATH_MAX];
        CHECK(snprintf(call_log, sizeof(call_log), "%s/test24-calls", g_fake_zfs_dir) < (int)sizeof(call_log),
              "constructed a path for Test 24's fake-zfs call log");
        unlink(call_log);

        char script[PATH_MAX + 256];
        CHECK(snprintf(script, sizeof(script),
                       "#!/bin/sh\ncase \"$*\" in\n"
                       "  *badroot*) echo \"$* :: exit1\" >> '%s'; echo 'badroot diagnostic' >&2; exit 1 ;;\n"
                       "  *) echo \"$* :: exit0\" >> '%s'; exit 0 ;;\nesac\n",
                       call_log, call_log) < (int)sizeof(script) &&
                  write_fake_zfs(script) == 0,
              "fake zfs script created outside the system ZFS path for Test 24");
        batch_ctx_t ctx = {0};
        int rc1 = batch_add(&ctx, "badroot/x", "p", 1, 0);
        int rc2 = batch_add(&ctx, "goodroot/y", "p", 1, 0);
        CHECK(rc1 == 0 && rc2 == 0, "batch_add succeeded for both items during setup");

        char *buf = NULL;
        size_t buf_len = 0;
        FILE *prior_log_fp = log_fp;
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

        char call_log_contents[1024] = {0};
        FILE *call_log_fp = fopen(call_log, "r");
        if (call_log_fp) {
            size_t rd = fread(call_log_contents, 1, sizeof(call_log_contents) - 1, call_log_fp);
            (void)rd;
            fclose(call_log_fp);
        }
        int line_count = 0, bad_took_exit1 = 0, good_took_exit0 = 0, cross_contamination = 0;
        char log_copy[sizeof(call_log_contents)];
        memcpy(log_copy, call_log_contents, sizeof(call_log_contents));
        char *save = NULL;
        for (char *ln = strtok_r(log_copy, "\n", &save); ln != NULL; ln = strtok_r(NULL, "\n", &save)) {
            line_count++;
            int has_bad = strstr(ln, "badroot/x") != NULL;
            int has_good = strstr(ln, "goodroot/y") != NULL;
            int has_exit1 = strstr(ln, "exit1") != NULL;
            int has_exit0 = strstr(ln, "exit0") != NULL;
            if (has_bad && has_exit1 && !has_exit0) bad_took_exit1 = 1;
            if (has_good && has_exit0 && !has_exit1) good_took_exit0 = 1;
            if ((has_bad && has_exit0) || (has_good && has_exit1)) cross_contamination = 1;
        }
        CHECK(line_count == 2, "the fake zfs script recorded exactly one invocation each for badroot and goodroot");
        CHECK(bad_took_exit1 == 1,
              "the badroot invocation genuinely took the script's exit-1 branch (verified directly, not just inferred from snap_failed afterward)");
        CHECK(good_took_exit0 == 1,
              "the goodroot invocation genuinely took the script's exit-0 branch (verified directly, not just inferred from snap_failed afterward)");
        CHECK(cross_contamination == 0,
              "neither invocation's recorded branch is mismatched against its own root -- the nonzero exit is confirmed to be on the matched root only");

        batch_free(&ctx);
        if (log_fp) fclose(log_fp);
        log_fp = prior_log_fp;
        free(buf);
        unlink(call_log);
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
        printf("\n");
    }

    printf("== Test 29a: load_combined_snapshot_inventory scopes to configured roots, and falls back to unscoped when the roots' bytes exceed ARGV_BYTES_CAP ==\n");
    {
        /*
         * This coverage previously lived unlabeled inside "Test 29:
         * valid_dataset enforces the configured naming grammar", with no
         * printf header of its own -- a reviewer grepping test headers
         * for load_combined_snapshot_inventory's scoping behavior would
         * find nothing until "Test 30" and could easily conclude (or,
         * when trimming "Test 29" down to just its named valid_dataset
         * purpose, accidentally cause) this coverage not to exist. Giving
         * it its own header makes it discoverable and independently
         * prunable/extendable.
         *
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

    printf("== Test 29b: load_combined_snapshot_inventory reports failure and frees the list when the underlying `zfs list` genuinely fails (not an allocation failure) ==\n");
    {
        /*
         * Coverage gap: every other failure exercised against
         * load_combined_snapshot_inventory (the run_fault_injection_tests
         * "Gap:" blocks) drives it to -1 purely via injected allocation
         * failure. None of them ever exercise the
         * `if (rc != 0) { name_list_free(list); return -1; }` branch at the
         * end of load_combined_snapshot_inventory itself, i.e. the case
         * where exec_cmd_stream() successfully runs `zfs list` but the
         * command genuinely exits non-zero. diffsnap.c's own comment calls
         * this listing "intentionally strict" (unlike the pre-validation
         * metrics fetch, which is lenient) -- exactly the kind of asymmetry
         * a regression (e.g. accidentally swapping in
         * exec_cmd_stream_lenient here) would silently break with nothing
         * in this file noticing. This drives that exact branch directly,
         * and also proves the failure path frees whatever the list already
         * held rather than leaking it.
         */
        char fail_script[PATH_MAX + 64];
        CHECK(snprintf(fail_script, sizeof(fail_script),
                       "#!/bin/sh\nif [ \"$1\" = list ]; then echo 'strict-list-failure' >&2; exit 1; fi\nexit 0\n") <
              (int)sizeof(fail_script) && write_fake_zfs(fail_script) == 0,
              "installed a fake zfs script whose `list` subcommand genuinely fails");

        batch_ctx_t std_b = {0}, rec_b = {0};
        batch_add(&std_b, "pool/child", "p", 1, 0);
        name_list_t inventory = {0};
        inventory.names = (char **)diffsnap_realloc(NULL, sizeof(char *));
        CHECK(inventory.names != NULL, "seeded the inventory with pre-existing content before the failing call");
        if (inventory.names) {
            inventory.capacity = 1;
            inventory.names[0] = diffsnap_strdup("pool/child@stale");
            inventory.count = 1;
        }

        int rc = load_combined_snapshot_inventory(&inventory, &std_b, &rec_b);
        CHECK(rc == -1,
              "load_combined_snapshot_inventory reports failure when `zfs list` itself exits non-zero, distinct from an allocation failure");
        CHECK(inventory.names == NULL && inventory.count == 0 && inventory.capacity == 0,
              "load_combined_snapshot_inventory frees (and does not leak) whatever the list already held before the strict `zfs list` failure, via name_list_free(list) on that path");

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
            FILE *prior_log_fp = log_fp;
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
            /*
             * Checking count==40 plus just the first/last names leaves a
             * duplicate-plus-drop (e.g. "pool/ds1" delivered twice while
             * "pool/ds17" is silently lost) able to pass this test, since
             * that failure mode preserves both the count and the first/last
             * names. The setup makes every line's dataset name AND written
             * value independently derivable ($i and $i*10), so verify all
             * 40 are present exactly once with their correct values. The
             * sscanf uses %n and compares the consumed length against the
             * full name length so a malformed replacement like
             * "pool/ds17-corrupt" is rejected rather than silently parsed
             * as ds17.
             */
            int seen[41] = {0};
            int all_present_once = 1, all_values_correct = 1;
            for (size_t i = 0; i < ctx.count; i++) {
                int n = 0, consumed = 0;
                if (sscanf(ctx.items[i].name, "pool/ds%d%n", &n, &consumed) == 1 &&
                    consumed == (int)strlen(ctx.items[i].name) && n >= 1 && n <= 40) {
                    seen[n]++;
                    if (ctx.items[i].written != (long long)n * 10) all_values_correct = 0;
                } else {
                    all_present_once = 0;
                }
            }
            for (int n = 1; n <= 40; n++) if (seen[n] != 1) all_present_once = 0;
            CHECK(all_present_once, "every one of pool/ds1..pool/ds40 was delivered exactly once, none dropped, duplicated, or corrupted");
            CHECK(all_values_correct, "every delivered line's written value matches its own $i*10, not a neighbor's (no cross-line corruption during interleaved draining)");

            /*
             * Checking only stderr-1 and stderr-40 leaves a drain bug that
             * drops stderr-2..stderr-39 (while the first/last still make it
             * through) undetected, despite the CHECK message previously
             * claiming full draining. Verify all 40 are present exactly
             * once, the same standard already applied to stdout above.
             */
            int stderr_all_present_once = 1;
            for (int n = 1; n <= 40; n++) {
                if (count_exact_numbered_occurrences(buf, "stderr-", n) != 1) stderr_all_present_once = 0;
            }
            CHECK(buf != NULL && stderr_all_present_once,
                  "every one of stderr-1..stderr-40 was drained and logged exactly once, not just the first and last, and not starved by the stdout side of poll()");

            if (log_fp) fclose(log_fp);
            log_fp = prior_log_fp;
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
            FILE *prior_log_fp = log_fp;
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
            CHECK(ctx.count == 1 && ctx.items[0].written == 99,
                  "the single stdout line's written value was parsed correctly, not just its name (a parser that preserves the name but corrupts the value would otherwise pass)");

            /*
             * Checking only late-stderr-1 and late-stderr-10 leaves a drain
             * bug that drops late-stderr-2..late-stderr-9 undetected,
             * despite the CHECK message previously claiming full draining.
             * Verify all 10 are present exactly once.
             */
            int late_stderr_all_present_once = 1;
            for (int n = 1; n <= 10; n++) {
                if (count_exact_numbered_occurrences(buf, "late-stderr-", n) != 1) late_stderr_all_present_once = 0;
            }
            CHECK(buf != NULL && late_stderr_all_present_once,
                  "every one of late-stderr-1..late-stderr-10 emitted AFTER stdout's fd closed was drained to completion exactly once, not cut off early");

            if (log_fp) fclose(log_fp);
            log_fp = prior_log_fp;
            free(buf);
            free(ctx.items);
        }
        printf("\n");
    }

    printf("== Test 31a: drain_command_streams correctly reassembles lines delivered as large, block-buffered bursts spanning multiple read() calls, not just small line-by-line writes ==\n");
    {
        /*
         * Fidelity gap: Test 30 and Test 31 both exercise a producer that
         * issues one small, unbuffered write(2) per line -- each shell
         * `printf`/`echo` call is its own syscall, closely matching a
         * LINE-buffered producer. A real `zfs` binary writing to a pipe is
         * normally FULLY (block) buffered by libc, so in practice its
         * output usually arrives as one or a few large writes right before
         * the process exits, not one write per line. Since
         * drain_command_streams() reads into a fixed STR_BUF_XLARGE
         * (512-byte) buffer per read(2) call, a multi-kilobyte burst
         * forces several read() calls back-to-back and is very likely to
         * land at least one line's own bytes split across two separate
         * read() calls / stream_reader_consume() invocations -- exactly
         * the reassembly case a producer that trickles output a line at a
         * time can never exercise, and a case the poll()-based rewrite
         * specifically has to get right.
         *
         * This drives that shape directly for BOTH streams: the fake
         * command builds its entire stdout output (200 lines, comfortably
         * several times STR_BUF_XLARGE) in a shell variable and emits it
         * with a single printf, then does the same for stderr.
         */
        const char *const sh_candidates[] = {"/bin/sh", "/usr/bin/sh", NULL};
        const char *sh_bin = find_bin(sh_candidates);
        CHECK(sh_bin != NULL, "found a shell for the bulk-burst reassembly test");
        if (sh_bin) {
            char *buf = NULL;
            size_t buf_len = 0;
            FILE *prior_log_fp = log_fp;
            log_fp = open_memstream(&buf, &buf_len);
            CHECK(log_fp != NULL, "open_memstream succeeded for capturing bulk-burst stderr output");

            metric_ctx_t ctx = {0};
            const char *const argv[] = {sh_bin, "-c",
                "i=1; out=\"pool/ds1\t10\"; err=\"stderr-1\"; i=2; "
                "while [ $i -le 200 ]; do "
                "out=\"$out\n"
                "pool/ds$i\t$((i*10))\"; "
                "err=\"$err\n"
                "stderr-$i\"; "
                "i=$((i+1)); done; "
                "printf '%s\\n' \"$out\"; "
                "printf '%s\\n' \"$err\" >&2", NULL};
            int rc = exec_cmd_stream(argv, handle_metric_line, &ctx);
            fflush(log_fp);

            CHECK(rc == 0, "the bulk-burst command succeeds");
            CHECK(ctx.count == 200,
                  "all 200 stdout lines were delivered to the handler, even though they arrived as one large multi-kilobyte burst instead of one write per line");

            int seen[201] = {0};
            int all_present_once = 1, all_values_correct = 1;
            for (size_t i = 0; i < ctx.count; i++) {
                int n = 0, consumed = 0;
                if (sscanf(ctx.items[i].name, "pool/ds%d%n", &n, &consumed) == 1 &&
                    consumed == (int)strlen(ctx.items[i].name) && n >= 1 && n <= 200) {
                    seen[n]++;
                    if (ctx.items[i].written != (long long)n * 10) all_values_correct = 0;
                } else {
                    all_present_once = 0;
                }
            }
            for (int n = 1; n <= 200; n++) if (seen[n] != 1) all_present_once = 0;
            CHECK(all_present_once,
                  "every one of pool/ds1..pool/ds200 was delivered exactly once, none dropped, duplicated, or corrupted at a read()-boundary split");
            CHECK(all_values_correct,
                  "every delivered line's written value is correct, proving a line split across two read() calls was reassembled before parsing rather than parsed from a truncated half");

            int stderr_all_present_once = 1;
            for (int n = 1; n <= 200; n++) {
                if (count_exact_numbered_occurrences(buf, "stderr-", n) != 1) stderr_all_present_once = 0;
            }
            CHECK(buf != NULL && stderr_all_present_once,
                  "every one of stderr-1..stderr-200 was reassembled correctly from the same kind of large stderr burst");

            if (log_fp) fclose(log_fp);
            log_fp = prior_log_fp;
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

    printf("== Test 32a: the exact caller-invariant Test 32 relies on -- an empty PREFIX field is rejected by the adjacent-comma gate before valid_prefix() ever sees it ==\n");
    {
        /*
         * Test 32 above documents that valid_prefix("") is unreachable
         * because "the only caller never passes an empty prefix". The
         * generic adjacent-comma coverage elsewhere in this suite exercises
         * a leading comma (empty dataset field) and an empty retention
         * field, but never specifically an empty prefix field -- exactly
         * the field valid_prefix() itself validates. Supply that exact
         * case (an empty 4th/prefix field via "pool/a,1,1,,no,0") through
         * the real end-to-end config pipeline and confirm it is rejected
         * at the adjacent-comma stage, never reaching -- let alone being
         * accepted or rejected by -- valid_prefix(). std_b/rec_b both stay
         * empty for a config with no valid entries, so main() never issues
         * any zfs command for this case; no fake-zfs script is needed.
         */
        char conf_file32a[PATH_MAX], log_file32a[PATH_MAX], lock_file32a[PATH_MAX];
        CHECK(snprintf(conf_file32a, sizeof(conf_file32a), "%s/t32a.conf", g_fake_zfs_dir) < (int)sizeof(conf_file32a) &&
              snprintf(log_file32a, sizeof(log_file32a), "%s/t32a.log", g_fake_zfs_dir) < (int)sizeof(log_file32a) &&
              snprintf(lock_file32a, sizeof(lock_file32a), "%s/t32a.lock", g_fake_zfs_dir) < (int)sizeof(lock_file32a),
              "isolated files for Test 32a fit in the test directory");
        conf_path = conf_file32a; log_path = log_file32a; lock_path = lock_file32a;

        FILE *fp32a = fopen(conf_file32a, "w");
        CHECK(fp32a != NULL, "opened isolated config for the empty-prefix-field adjacent-comma test");
        if (fp32a) {
            fputs("pool/a,1,1,,no,0\n", fp32a);
            fclose(fp32a);
            CHECK(diffsnap_real_main(1, (char *[]){"diffsnap-test", NULL}) == 1,
                  "main() rejects a config line whose prefix field is empty");
            FILE *log32a = fopen(log_file32a, "r");
            char contents32a[4096] = {0};
            if (log32a) { size_t rd = fread(contents32a, 1, sizeof(contents32a) - 1, log32a); (void)rd; fclose(log32a); }
            CHECK(strstr(contents32a, "adjacent comma delimiters") != NULL,
                  "an empty prefix field (adjacent commas around it) is caught by the adjacent-comma gate");
            CHECK(strstr(contents32a, "invalid prefix") == NULL,
                  "the line never reaches valid_prefix() at all, so no 'invalid prefix' diagnostic is emitted for it -- the caller invariant Test 32 relies on is genuinely exercised here, not just asserted in a comment");
        }
        unlink(conf_file32a); unlink(log_file32a); unlink(lock_file32a);
        conf_path = CONF_PATH; log_path = LOG_PATH; lock_path = LOCK_PATH;
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

    printf("== Test 35a: resolve_recursive_ancestor_overlaps bumps the SAME item's pass more than once when a 3-level ancestor chain collides at consecutive levels ==\n");
    {
        /*
         * Coverage gap: Test 35 only forces a single pass bump (two
         * items, one collision). The collision-avoidance loop's
         * do { ... } while (collision) deliberately restarts its inner
         * scan from oa=0 every time it bumps a pass, specifically so a
         * later collision can be caused by a bump that loop itself just
         * made. A 3-level same-root chain with distinct prefixes (so
         * nothing is dropped by the same-prefix ancestor-coverage check
         * Tests 33-34 exercise) forces exactly that: the deepest item
         * collides with its grandparent at pass 0 and bumps to pass 1,
         * then -- because the scan restarted from oa=0 -- immediately
         * collides with its parent (which is already sitting at pass 1)
         * and must bump again to pass 2. A loop that only ran its inner
         * scan once per item (continuing from where it left off instead
         * of restarting) would stop at pass 1 and miss the second,
         * parent-level collision entirely.
         */
        batch_ctx_t rec_b = {0};
        CHECK(batch_add(&rec_b, "pool/a", "p1", 1, 0) == 0 &&
              batch_add(&rec_b, "pool/a/b", "p2", 1, 0) == 0 &&
              batch_add(&rec_b, "pool/a/b/c", "p3", 1, 0) == 0,
              "3-level ancestor-chain pass-collision test batch setup succeeds");

        int rc = resolve_recursive_ancestor_overlaps(&rec_b);

        CHECK(rc == 0, "resolve_recursive_ancestor_overlaps succeeds for a 3-level ancestor chain with distinct prefixes");
        CHECK(rec_b.count == 3, "distinct prefixes mean none of the three items is covered/dropped, unlike Tests 33-34");
        CHECK(rec_b.count == 3 &&
              strcmp(rec_b.items[0].dataset, "pool/a") == 0 &&
              strcmp(rec_b.items[1].dataset, "pool/a/b") == 0 &&
              strcmp(rec_b.items[2].dataset, "pool/a/b/c") == 0,
              "all three items keep their original relative order (nothing was covered, so nothing was compacted)");
        CHECK(rec_b.count == 3 &&
              rec_b.items[0].pass == 0 && rec_b.items[1].pass == 1 && rec_b.items[2].pass == 2,
              "each level lands on a strictly higher pass than its ancestor -- the deepest item was bumped TWICE (0->1 colliding with its grandparent, then 1->2 colliding with its now-already-bumped parent), not just once");

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
        FILE *prior_log_fp = log_fp;
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
        log_fp = prior_log_fp;
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
        FILE *prior_log_fp = log_fp;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded");

        int status = finalize_batch(&b, &inventory, 1, &matches, &matches_cap, "2026-01-01_00:00:00", 0);
        fflush(log_fp);

        CHECK(status == 0, "status is 0: normal-length name formats fine, and pruning against the (empty) inventory has nothing to do");
        CHECK(buf != NULL && strstr(buf, "Created=pool/normal@p_2026-01-01_00:00:00") != NULL,
              "the Created= line uses the correctly-formatted, non-truncated snapshot name");

        if (log_fp) fclose(log_fp);
        log_fp = prior_log_fp;
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
        FILE *prior_log_fp = log_fp;
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
        log_fp = prior_log_fp;
        free(buf);
        batch_free(&std_b); batch_free(&rec_b);
        printf("\n");
    }

    printf("== Test 39: remove_recursive_overlaps compacts before its oversized-name error path ==\n");
    {
        /*
         * Earlier revision of this test used a std_b whose only non-
         * oversized entry ("pool/keep", prefix "standard") could never
         * match rec_b's prefix ("recursive"), so no entry was ever
         * actually covered/dropped before the oversized-name failure --
         * write_idx tracked i exactly, and the test could not have
         * detected a version of remove_recursive_overlaps that repaired
         * std_b->count using `i` instead of `write_idx`. This version
         * adds "pool/a/covered" (prefix "p", genuinely covered by rec_b's
         * "pool/a"/"p") BEFORE the oversized entry, so real compaction
         * happens first: write_idx stops tracking i one-for-one, and
         * "pool/keep" gets moved from index 1 down to index 0 before the
         * error fires at index 2. If write_idx-based compaction were
         * broken, items[0] after the error would still be the stale,
         * already-freed "pool/a/covered" entry instead of "pool/keep".
         */
        batch_ctx_t std_b = {0}, rec_b = {0};
        char oversized[STR_BUF_LARGE + 32];
        memset(oversized, 'a', sizeof(oversized) - 1);
        oversized[sizeof(oversized) - 1] = '\0';
        CHECK(batch_add(&std_b, "pool/a/covered", "p", 1, 0) == 0 &&
              batch_add(&std_b, "pool/keep", "standard", 1, 0) == 0 &&
              batch_add(&std_b, oversized, "standard", 1, 0) == 0 &&
              batch_add(&rec_b, "pool/a", "p", 1, 0) == 0 &&
              batch_add(&rec_b, "pool/recursive", "recursive", 1, 0) == 0,
              "overlap error-path setup succeeds");

        char *buf = NULL;
        size_t buf_len = 0;
        FILE *prior_log_fp = log_fp;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for Test 39's coverage-message capture");

        CHECK(remove_recursive_overlaps(&std_b, &rec_b) == -1,
              "an oversized standard dataset reaches the overlap-check error path");
        if (log_fp) fflush(log_fp);
        CHECK(buf != NULL && strstr(buf, "Skipping pool/a/covered") != NULL,
              "the genuinely-covered entry was dropped by the recursive-coverage check before the oversized entry was ever reached");
        CHECK(std_b.count == 1 && strcmp(std_b.items[0].dataset, "pool/keep") == 0,
              "after a real compaction (one item dropped, one kept) the surviving entry is the compacted-down \"pool/keep\", not a stale freed slot");
        if (log_fp) fclose(log_fp);
        log_fp = prior_log_fp;
        free(buf);

        batch_free(&std_b); /* the count repair matters for the OVERSIZED entry, not pool/keep:
                              * the error path already freed items[2..count-1] (just the oversized
                              * entry here) and shrank std_b->count to exclude them, so batch_free
                              * only iterates surviving items. pool/keep -- moved down to items[0]
                              * by the earlier compaction -- was never touched by the error path
                              * and is safe to free; without correct write_idx-based repair, it's
                              * the already-freed pool/a/covered or oversized entry that batch_free
                              * would double-free by walking past the reduced count. */
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
        FILE *prior_log_fp = log_fp;
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
        log_fp = prior_log_fp;
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
        FILE *prior_log_fp = log_fp;
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
        log_fp = prior_log_fp;
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
        FILE *prior_log_fp = log_fp;
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
        log_fp = prior_log_fp;
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
        FILE *prior_log_fp = log_fp;
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
        log_fp = prior_log_fp;
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
        /* Explicit trailing `exit 0`, matching the same fix in
         * run_chunk_test's identical script -- see comment there. */
        CHECK(snprintf(script, sizeof(script),
                       "#!/bin/sh\nif [ \"$1\" = snapshot ]; then\n shift\n printf '%%s\\n' \"$@\" >> '%s'\n printf '\\036\\n' >> '%s'\nfi\nexit 0\n",
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
            /* Explicit, not relied-upon carryover: this growth must use
             * the real realloc() regardless of what any earlier block in
             * this suite left realloc_now_fn/g_realloc_fail_after set to.
             * See the comment above the OOM blocks in
             * run_fault_injection_tests for why implicit carryover here
             * is exactly the ordering hazard this suite otherwise avoids. */
            realloc_now_fn = realloc;
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
        CHECK(strstr(args2, "p0000") == NULL,
              "none of the over-cap roots' own names leak into the unscoped call either (mirrors Test 29a's equivalent check)");
        name_list_free(&inventory2);

        batch_free(&std_b);
        unlink(g_inventory_args);
        unlink(g_fake_zfs);
        printf("\n");
    }

    printf("== Test 46: exec_cmd_stream reports failure when pipe2() itself cannot allocate a descriptor ==\n");
    {
        /*
         * Coverage gap: exec_cmd_stream_core()'s own pipe2()/fork() failure
         * branches (the "internal plumbing failures" the exec_cmd_stream_
         * lenient comment distinguishes from a tolerable nonzero zfs exit)
         * were never exercised anywhere else in this suite. Forcing a real
         * pipe2() failure deterministically -- without needing a handler-
         * count-dependent bug in diffsnap.c itself -- is done by lowering
         * RLIMIT_NOFILE far enough that no new descriptor can be allocated
         * at all: fd 0/1/2 are already in use, so a soft limit of 3 leaves
         * no numerically-eligible slot free for pipe2()'s two new fds,
         * guaranteeing EMFILE on the very first pipe2() call regardless of
         * how many other descriptors this process happens to have open.
         * The original limit is restored on every path out of this block.
         */
        struct rlimit old_limit;
        int got_limit = getrlimit(RLIMIT_NOFILE, &old_limit) == 0;
        CHECK(got_limit, "read the current RLIMIT_NOFILE before lowering it for the pipe2() failure test");
        if (got_limit) {
            struct rlimit low_limit = { .rlim_cur = 3, .rlim_max = old_limit.rlim_max };
            int lowered = setrlimit(RLIMIT_NOFILE, &low_limit) == 0;
            CHECK(lowered, "lowered RLIMIT_NOFILE to exhaust available file descriptors");
            if (lowered) {
                const char *const argv[] = {"/bin/true", NULL};
                metric_ctx_t ctx = {0};
                /* handler != NULL forces exec_cmd_stream_core() down the
                 * "if (handler && pipe2(out_pfd, ...) == -1)" branch --
                 * the specific pipe2() call site this test targets. */
                int rc = exec_cmd_stream(argv, handle_metric_line, &ctx);
                CHECK(rc != 0, "exec_cmd_stream reports failure when pipe2() cannot allocate a descriptor for stdout");
                CHECK(ctx.count == 0, "no metric lines were parsed since the command never actually ran");
                free(ctx.items);

                restore_rlimit_nofile_or_die(&old_limit,
                      "restored the original RLIMIT_NOFILE after the pipe2() failure test");
            }
        }
        printf("\n");
    }

    printf("== Test 46a: exec_cmd_stream reports failure when the SECOND pipe2() (stderr) cannot allocate a descriptor after the first (stdout) already succeeded ==\n");
    {
        /*
         * Coverage gap: Test 46 forces EMFILE with RLIMIT_NOFILE=3, which
         * leaves zero descriptors free -- the very first pipe2() call
         * (out_pfd) always fails first under that limit. The separate
         * cleanup branch at "if (pipe2(err_pfd, ...) == -1) { if (handler)
         * close(out_pfd...) ...}" -- reached only when out_pfd's pipe2()
         * already succeeded and err_pfd's then fails -- was never
         * exercised anywhere in this suite. This targets exactly that
         * branch by leaving just enough fd headroom for one pipe2() call
         * (2 fds) but not two (4 fds): probe the next fd number the
         * kernel will hand out, then cap RLIMIT_NOFILE two descriptors
         * above it.
         */
        int probe = open("/dev/null", O_RDONLY);
        int got_probe = probe >= 0;
        CHECK(got_probe, "opened a probe descriptor to find the next available fd number for the second-pipe2() test");
        if (got_probe) {
            int next_fd = probe;
            close(probe);
            struct rlimit old_limit;
            int got_limit = getrlimit(RLIMIT_NOFILE, &old_limit) == 0;
            CHECK(got_limit, "read the current RLIMIT_NOFILE before tightening it for the second-pipe2() failure test");
            if (got_limit) {
                struct rlimit tight_limit = { .rlim_cur = (rlim_t)(next_fd + 2), .rlim_max = old_limit.rlim_max };
                int lowered = setrlimit(RLIMIT_NOFILE, &tight_limit) == 0;
                CHECK(lowered, "tightened RLIMIT_NOFILE to allow exactly one pipe2() worth of new descriptors");
                if (lowered) {
                    const char *const argv[] = {"/bin/true", NULL};
                    metric_ctx_t ctx = {0};
                    /* handler != NULL: out_pfd's pipe2() gets fds
                     * next_fd/next_fd+1 and succeeds (fits exactly under
                     * the limit); err_pfd's pipe2() then needs two more
                     * and fails with EMFILE. */
                    int rc = exec_cmd_stream(argv, handle_metric_line, &ctx);
                    CHECK(rc != 0, "exec_cmd_stream reports failure when the second pipe2() (stderr) cannot allocate a descriptor");
                    CHECK(ctx.count == 0, "no metric lines were parsed since the command never actually ran");
                    free(ctx.items);

                    restore_rlimit_nofile_or_die(&old_limit,
                          "restored the original RLIMIT_NOFILE after the second-pipe2() failure test");
                }
            }
        }
        printf("\n");
    }

    printf("== Test 46b: exec_cmd_stream reports failure when fork() itself cannot create a new process ==\n");
    {
        /*
         * Coverage gap: fork()'s own failure branch in
         * exec_cmd_stream_core() (pid == -1, distinct cleanup path that
         * closes both pipe pairs without ever reaching drain/waitpid) was
         * never exercised anywhere in this suite.
         *
         * Primary check: fork_now_fn is hooked to deterministically return
         * -1/EAGAIN without ever calling the real fork(). Because pipe2()
         * has already succeeded by this point and the pid == -1 branch
         * returns before drain_command_streams()/waitpid() are ever
         * reached, a nonzero exec_cmd_stream() result here can only come
         * from the branch under test -- unlike checking rc != 0 alone,
         * which a pipe/drain/wait failure could equally satisfy.
         * g_fork_calls == 1 additionally confirms the hook (and therefore
         * this exact injected failure) was actually reached, rather than
         * some earlier branch returning nonzero before fork_now_fn was
         * ever called.
         */
        const char *const argv[] = {"/bin/true", NULL};
        metric_ctx_t ctx = {0};
        g_fork_fail = 1; g_fork_calls = 0; fork_now_fn = test_fork_failure;
        int rc = exec_cmd_stream(argv, handle_metric_line, &ctx);
        fork_now_fn = fork; g_fork_fail = 0;
        CHECK(rc != 0, "exec_cmd_stream reports failure when the fork_now_fn hook forces fork() to fail");
        CHECK(g_fork_calls == 1, "fork_now_fn was invoked exactly once, confirming the reported failure is attributable to the fork()==-1 branch specifically");
        CHECK(ctx.count == 0, "no metric lines were parsed since the command never actually ran (fork failed before any child existed)");
        free(ctx.items);

        /*
         * Secondary, best-effort check: also exercise a genuine OS-level
         * fork() failure (not just the injected hook) by lowering
         * RLIMIT_NPROC far enough that no new process can be created for
         * this user. Skipped (not failed) if the platform lacks
         * RLIMIT_NPROC, this process lacks permission to lower it, or this
         * environment doesn't enforce it against fork() at all -- e.g.
         * some containerized or privileged environments.
         */
#ifdef RLIMIT_NPROC
        struct rlimit old_nproc;
        int got_nproc = getrlimit(RLIMIT_NPROC, &old_nproc) == 0;
        CHECK(got_nproc, "read the current RLIMIT_NPROC before lowering it for the fork() failure test");
        if (got_nproc) {
            struct rlimit low_nproc = { .rlim_cur = 1, .rlim_max = old_nproc.rlim_max };
            if (setrlimit(RLIMIT_NPROC, &low_nproc) == 0) {
                const char *const argv[] = {"/bin/true", NULL};
                metric_ctx_t ctx = {0};
                int rc = exec_cmd_stream(argv, handle_metric_line, &ctx);
                if (rc == 0) {
                    /* Some environments (commonly: running as root, or
                     * inside certain containers/namespaces) don't enforce
                     * RLIMIT_NPROC against fork() at all -- the limit was
                     * accepted by setrlimit() but never actually bites.
                     * That's an environment limitation, not evidence
                     * about exec_cmd_stream_core()'s fork()==-1 handling,
                     * so skip visibly instead of recording a false
                     * failure. */
                    printf("    SKIP: this platform/privilege level does not enforce RLIMIT_NPROC against fork(); fork() failure test skipped\n");
                    free(ctx.items);
                } else {
                    CHECK(rc != 0, "(secondary, real-OS fork() failure via RLIMIT_NPROC) exec_cmd_stream reports failure when fork() cannot create a new process");
                    CHECK(ctx.count == 0, "(secondary) no metric lines were parsed since the command never actually ran (fork failed before any child existed)");
                    free(ctx.items);
                }

                restore_rlimit_or_die(RLIMIT_NPROC, "RLIMIT_NPROC", &old_nproc,
                      "restored the original RLIMIT_NPROC after the fork() failure test");
            } else {
                printf("    SKIP: could not lower RLIMIT_NPROC on this platform/permission level; fork() failure test skipped\n");
            }
        }
#else
        printf("    SKIP: RLIMIT_NPROC is not defined on this platform; fork() failure test skipped\n");
#endif
        printf("\n");
    }

    printf("== Test 47: create_batch_snapshots logs its own diagnostic (not just zfs_snapshot_batch's) on failure ==\n");
    {
        /*
         * Coverage gap: every other test drives zfs_snapshot_batch()
         * directly, so create_batch_snapshots()'s own wrapper log line
         * ("Error: %s zfs snapshot batch execution failed") was never
         * itself asserted on. Covers both the standard and recursive
         * message variants.
         */
        CHECK(write_fake_zfs("#!/bin/sh\nexit 1\n") == 0,
              "installed a fake zfs that always fails, for the create_batch_snapshots wrapper test");

        batch_ctx_t std_b = {0};
        CHECK(batch_add(&std_b, "pool/wrap-std", "p", 1, 0) == 0,
              "batch_add succeeded for the standard create_batch_snapshots wrapper test");
        char *buf = NULL;
        size_t buf_len = 0;
        FILE *prior_log_fp = log_fp;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for Test 47's standard-batch log capture");
        int rc_std = create_batch_snapshots(&std_b, "2026-01-01_00:00:00", 0);
        if (log_fp) fflush(log_fp);
        CHECK(rc_std != 0, "create_batch_snapshots propagates zfs_snapshot_batch's failure for the standard batch");
        CHECK(buf != NULL && strstr(buf, "Error: standard zfs snapshot batch execution failed") != NULL,
              "create_batch_snapshots logs its own standard-batch diagnostic, not just the underlying zfs error");
        if (log_fp) fclose(log_fp);
        log_fp = prior_log_fp;
        free(buf);
        batch_free(&std_b);

        batch_ctx_t rec_b = {0};
        CHECK(batch_add(&rec_b, "pool/wrap-rec", "p", 1, 0) == 0,
              "batch_add succeeded for the recursive create_batch_snapshots wrapper test");
        buf = NULL; buf_len = 0;
        prior_log_fp = log_fp;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for Test 47's recursive-batch log capture");
        int rc_rec = create_batch_snapshots(&rec_b, "2026-01-01_00:00:00", 1);
        if (log_fp) fflush(log_fp);
        CHECK(rc_rec != 0, "create_batch_snapshots propagates zfs_snapshot_batch's failure for the recursive batch");
        CHECK(buf != NULL && strstr(buf, "Error: recursive zfs snapshot batch execution failed") != NULL,
              "create_batch_snapshots logs its own recursive-batch diagnostic, distinct from the standard-batch wording");
        if (log_fp) fclose(log_fp);
        log_fp = prior_log_fp;
        free(buf);
        batch_free(&rec_b);

        unlink(g_fake_zfs);
        printf("\n");
    }

    printf("== Test 48: zfs_snapshot_exec_chunk fails safely instead of silently truncating when handed an arena smaller than the formatted name needs ==\n");
    {
        /*
         * Coverage gap: zfs_snapshot_batch_root_pass always sizes
         * chunk_bytes to exactly the sum of each item's own
         * strlen(dataset)+strlen(prefix)+strlen(timestamp)+3, so
         * format_snapshot_name() can never actually overflow the arena
         * through that real caller -- the truncation guard itself,
         * "if (written < 0 || (size_t)written >= remaining)", was never
         * reachable from any test in this suite, the same way Test 32
         * documents valid_prefix's dead empty-string check. Rather than
         * leaving it unacknowledged, this drives it directly: call
         * zfs_snapshot_exec_chunk (the real static function, no shim)
         * with a deliberately undersized chunk_bytes -- smaller than the
         * formatted "dataset@prefix_timestamp" actually needs -- bypassing
         * the caller's own correct sizing, exactly as Test 36 does to
         * finalize_batch's analogous snprintf guard. realloc_now_fn is
         * set explicitly (not relied-upon carryover) per this suite's own
         * established discipline for exactly this reason.
         */
        realloc_now_fn = realloc;
        batch_ctx_t chunk_ctx2 = {0};
        CHECK(batch_add(&chunk_ctx2, "pool/chunk", "p", 1, 0) == 0,
              "batch_add succeeded during Test 48 setup");
        size_t chunk_indices2[1] = {0};

        /* "pool/chunk@p_2026-01-01_00:00:00" needs 33 bytes (32 chars +
         * NUL); chunk_bytes=10 is deliberately far too small, forcing
         * format_snapshot_name's return value to be >= remaining on the
         * chunk's only item.
         *
         * The truncation check is expected to fire before exec_cmd_stream
         * is ever called -- but that "expected" is exactly what this test
         * must prove, not assume. Merely having no fake zfs installed (or
         * one that just exits nonzero) can't distinguish "the guard
         * fired, zfs was never invoked" from "the guard is broken, a
         * truncated/garbled argv was execed and zfs -- or execv itself,
         * if no binary happened to be installed at zfs_path -- merely
         * failed for a completely unrelated reason": both produce
         * rc == -1 and snap_failed == 1. So install a fake zfs that
         * leaves an unambiguous trace if it's ever invoked at all, and
         * assert that trace is absent afterward. This makes the test
         * fail loudly if the guard is ever removed or broken, instead of
         * coincidentally still passing because the binary was missing. */
        char sentinel[PATH_MAX];
        CHECK(snprintf(sentinel, sizeof(sentinel), "%s/test48-invoked", g_fake_zfs_dir) < (int)sizeof(sentinel),
              "Test 48 sentinel path fits in its buffer");
        unlink(sentinel);
        char sentinel_script[PATH_MAX + 64];
        CHECK(snprintf(sentinel_script, sizeof(sentinel_script), "#!/bin/sh\ntouch '%s'\nexit 0\n", sentinel) < (int)sizeof(sentinel_script) &&
              write_fake_zfs(sentinel_script) == 0,
              "installed a fake zfs for Test 48 that records whether it was ever invoked");

        int rc = zfs_snapshot_exec_chunk(&chunk_ctx2, 0, "2026-01-01_00:00:00", chunk_indices2, 1, 10);
        CHECK(rc == -1, "zfs_snapshot_exec_chunk reports failure when the formatted name would not fit the arena it was given");
        CHECK(chunk_ctx2.count == 1 && chunk_ctx2.items[0].snap_failed == 1,
              "the item is marked snap_failed rather than silently snapshotted under a truncated name");
        CHECK(access(sentinel, F_OK) != 0,
              "the truncation guard genuinely fires before exec_cmd_stream ever runs zfs -- proven by a real invocation trace being absent, not merely by an incidental exit code");

        unlink(sentinel);
        unlink(g_fake_zfs);
        batch_free(&chunk_ctx2);
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
