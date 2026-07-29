/*
 * test_localtime_standalone.c
 *
 * Whitebox fault-injection tests for two related log_msg() robustness
 * fixes:
 *
 *   1. localtime_r failure logging: before the fix, a failing localtime_r
 *      inside log_msg() silently fell back to an "unknown-time" timestamp
 *      with no indication anything had gone wrong -- an operator staring
 *      at a log full of "unknown-time" entries with no explanation. The
 *      fix makes log_msg() write an explicit failure notice (via a direct
 *      fprintf to log_fp, bypassing log_msg() itself to avoid recursing
 *      back into the very call that's failing) whenever its own
 *      localtime_r call fails, while still writing the caller's original
 *      message on the very next line.
 *
 *   2. Log write-failure detection: before the fix, log_msg()'s own
 *      fprintf/vfprintf return values were discarded, so a disk-full or
 *      EIO on the log file mid-run silently and permanently dropped the
 *      audit trail (Created=/Pruned=/error lines) with no effect on the
 *      process exit status. The fix checks every fprintf/vfprintf return
 *      value against the log file and records any failure via the
 *      log_had_io_failure() flag, which main() checks at shutdown to
 *      force a non-zero exit status.
 *
 * main()'s own top-level localtime_r check (log_msg("Error: localtime_r
 * failed"); goto cleanup;) is deliberately NOT exercised here. That
 * call site is an ordinary log-then-bail pattern identical in shape to
 * dozens of other error paths in main() that aren't individually
 * whitebox-tested, and driving it would require invoking main() itself
 * (renamed diffsnap_real_main), which needs real lock/log/config file
 * paths and has side effects the other whitebox suites deliberately
 * avoid too. The interesting, novel logic covered here is entirely
 * inside log_msg()'s own recursion-avoidance fallback and its write-error
 * bookkeeping.
 *
 * Isolated into its own translation unit (rather than added to
 * test_metrics_scoping_standalone.c or test_oom_standalone.c) for the
 * same reason those two are separate from each other: it needs to
 * #define localtime_r to a fault-injecting shim BEFORE #include
 * "diffsnap.c", and mixing that substitution into a file used for other
 * whitebox tests would affect every localtime_r call made during their
 * setup too, not just the one under test here. The log write-failure
 * test added alongside it needs no such shim (it forces a real write
 * failure by closing the underlying fd out from under log_fp), but it
 * belongs in this file rather than test.c because it exercises the same
 * function (log_msg) and the same log_fp global that the localtime_r
 * tests above it already set up and tear down.
 *
 * No ZFS or network access required.
 *
 * Build:
 *   cc -Wall -Wextra -std=c11 -o test_localtime test_localtime_standalone.c
 * Run:
 *   ./test_localtime
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Fault-injecting localtime_r shim, installed via macro substitution
 * before diffsnap.c is included, so the localtime_r call inside
 * diffsnap.c's own log_msg() goes through it. Off by default
 * (g_should_fail == 0) so ordinary calls behave normally; a test arms
 * it immediately before the call under test and disarms it immediately
 * after.
 */
static int g_should_fail = 0;
static int g_call_count = 0;

static struct tm *test_localtime_r(const time_t *timep, struct tm *result) {
    g_call_count++;
    if (g_should_fail) return NULL;
    return localtime_r(timep, result);
}
#define localtime_r test_localtime_r

#define main diffsnap_real_main
#include "diffsnap.c"
#undef main
#undef localtime_r

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define CHECK(cond, msg) do { \
    g_tests_run++; \
    if (!(cond)) { \
        g_tests_failed++; \
        printf("    FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    } else { \
        printf("    ok: %s\n", msg); \
    } \
} while (0)

int main(void) {
    printf("== Test: log_msg's own localtime_r failure is logged, not silently swallowed ==\n");
    {
        /* Point diffsnap.c's own static log_fp at a memory-backed stream
         * so we can inspect exactly what log_msg() writes, without
         * touching a real file on disk. */
        char *buf = NULL;
        size_t buf_len = 0;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for capturing log_msg output");

        g_call_count = 0;
        g_should_fail = 1;
        log_msg("test message while localtime_r is failing");
        g_should_fail = 0; /* disarm immediately so nothing else is affected */

        fflush(log_fp);
        CHECK(g_call_count == 1, "sanity check: the shim actually intercepted exactly one localtime_r call");
        CHECK(buf != NULL && strstr(buf, "localtime_r failed") != NULL,
              "log output contains an explicit localtime_r failure notice, not just a silent 'unknown-time' substitution");
        CHECK(buf != NULL && strstr(buf, "test message while localtime_r is failing") != NULL,
              "the original log message is still written despite the timestamp failure (not dropped entirely)");

        fclose(log_fp);
        log_fp = NULL;
        free(buf);
        printf("\n");
    }

    printf("== Test: log_msg behaves normally (no false positives) when localtime_r succeeds ==\n");
    {
        char *buf = NULL;
        size_t buf_len = 0;
        log_fp = open_memstream(&buf, &buf_len);
        CHECK(log_fp != NULL, "open_memstream succeeded for capturing log_msg output");

        g_call_count = 0;
        g_should_fail = 0;
        log_msg("ordinary message, localtime_r should succeed");

        fflush(log_fp);
        CHECK(g_call_count == 1, "sanity check: exactly one localtime_r call was made");
        CHECK(buf != NULL && strstr(buf, "localtime_r failed") == NULL,
              "no spurious failure notice is written when localtime_r actually succeeds");
        CHECK(buf != NULL && strstr(buf, "ordinary message, localtime_r should succeed") != NULL,
              "the ordinary message is logged normally");

        fclose(log_fp);
        log_fp = NULL;
        free(buf);
        printf("\n");
    }

    printf("== Test: log_msg detects and records its own write failures instead of discarding them ==\n");
    {
        /*
         * Force a real write failure -- not a shimmed one -- by fdopen'ing
         * a temp file for log_fp and then closing its underlying fd out
         * from under the FILE* without going through fclose(). The next
         * write attempted through log_fp then fails at the OS level
         * (EBADF), the same class of failure a disk-full or EIO would
         * produce in production. log_had_io_failure() is the static
         * accessor added alongside log_msg()'s fix; being in the same
         * translation unit (diffsnap.c is #include'd above), it's called
         * directly here.
         */
        char tmpl[] = "/tmp/diffsnap_test_logfail_XXXXXX";
        int fd = mkstemp(tmpl);
        CHECK(fd >= 0, "created a temp file to back log_fp for the write-failure test");
        if (fd >= 0) {
            log_fp = fdopen(fd, "w");
            CHECK(log_fp != NULL, "fdopen succeeded on the temp file");
            if (log_fp) {
                /* Match production: log_fp is line-buffered there too, via
                 * setvbuf(log_fp, NULL, _IOLBF, 0) right after fopen(). */
                setvbuf(log_fp, NULL, _IOLBF, 0);
                log_io_failed = 0; /* reset the static flag before the call under test */
                CHECK(log_had_io_failure() == 0, "sanity check: no failure recorded yet against a freshly opened, healthy log_fp");

                close(fd); /* invalidate the fd without telling stdio */

                log_msg("this write should fail because the underlying fd was closed out from under it");
                CHECK(log_had_io_failure() == 1, "log_msg's write failure against the closed fd is recorded via log_had_io_failure(), not silently discarded");

                fclose(log_fp); /* expected to itself report an error (EBADF); either way we're done with this FILE* */
                log_fp = NULL;
                log_io_failed = 0; /* disarm immediately so nothing else in this binary is affected */
            }
        }
        unlink(tmpl);
        printf("\n");
    }

    printf("================================\n");
    printf("RESULTS: %d checks run, %d failed\n", g_tests_run, g_tests_failed);
    printf("================================\n");
    if (g_tests_failed == 0) { printf("ALL CLEAR\n"); return 0; }
    printf("REVIEW FAILURES ABOVE\n");
    return 1;
}
