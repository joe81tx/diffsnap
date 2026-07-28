/*
 * test_localtime_standalone.c
 *
 * Whitebox fault-injection test for the localtime_r failure logging added
 * to log_msg(). Before the fix, a failing localtime_r inside log_msg()
 * silently fell back to an "unknown-time" timestamp with no indication
 * anything had gone wrong -- an operator staring at a log full of
 * "unknown-time" entries with no explanation. The fix makes log_msg()
 * write an explicit failure notice (via a direct fprintf to log_fp,
 * bypassing log_msg() itself to avoid recursing back into the very call
 * that's failing) whenever its own localtime_r call fails, while still
 * writing the caller's original message on the very next line.
 *
 * main()'s own top-level localtime_r check (log_msg("Error: localtime_r
 * failed"); goto cleanup;) is deliberately NOT exercised here. That
 * call site is an ordinary log-then-bail pattern identical in shape to
 * dozens of other error paths in main() that aren't individually
 * whitebox-tested, and driving it would require invoking main() itself
 * (renamed diffsnap_real_main), which needs real lock/log/config file
 * paths and has side effects the other whitebox suites deliberately
 * avoid too. The interesting, novel logic is entirely inside log_msg()'s
 * own recursion-avoidance fallback, which is what this file covers.
 *
 * Isolated into its own translation unit (rather than added to
 * test_metrics_scoping_standalone.c or test_oom_standalone.c) for the
 * same reason those two are separate from each other: it needs to
 * #define localtime_r to a fault-injecting shim BEFORE #include
 * "diffsnap.c", and mixing that substitution into a file used for other
 * whitebox tests would affect every localtime_r call made during their
 * setup too, not just the one under test here.
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

    printf("================================\n");
    printf("RESULTS: %d checks run, %d failed\n", g_tests_run, g_tests_failed);
    printf("================================\n");
    if (g_tests_failed == 0) { printf("ALL CLEAR\n"); return 0; }
    printf("REVIEW FAILURES ABOVE\n");
    return 1;
}
