/*
 * test_oom_standalone.c
 *
 * Whitebox fault-injection test for zfs_snapshot_batch_root_pass's index
 * collection loop: when the `indices` realloc fails partway through
 * collecting a root+pass's items, EVERY item that belongs to that
 * root+pass must end up marked snap_failed -- both the ones already
 * collected into `indices` (never passed to zfs_snapshot_exec_chunk) and
 * the ones further ahead in the batch that the loop never even reaches
 * before returning. Before the fix, only the single item being added at
 * the moment of failure was marked; every other item in the root+pass
 * silently kept snap_failed==0, so finalize_batch would log a false
 * "Created=" line for it and prune real, existing snapshots on that false
 * premise.
 *
 * Isolated into its own translation unit (rather than added to
 * test_metrics_scoping_standalone.c) because it needs to #define realloc
 * to a fault-injecting shim BEFORE #include "diffsnap.c" -- doing that in
 * a file shared with other whitebox tests would affect every realloc call
 * made during their setup too, not just the one call site under test here.
 *
 * No ZFS or network access required: zfs_snapshot_batch_root_pass returns
 * before issuing any zfs command when index collection itself fails, so
 * this never reaches exec_cmd_stream / fork / exec at all.
 *
 * Build:
 *   cc -Wall -Wextra -std=c11 -o test_oom test_oom_standalone.c
 * Run:
 *   ./test_oom
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Fault-injecting realloc shim, installed via macro substitution before
 * diffsnap.c is included, so every realloc call inside diffsnap.c itself
 * -- not just in this test file -- goes through it. Off by default
 * (g_fail_after_call == -1, "never fail") so ordinary setup code like
 * batch_add's own capacity growth behaves normally; a test arms it
 * immediately before the call under test and disarms it immediately after,
 * so failures can be targeted at one specific call site without needing to
 * predict or avoid every other realloc happening elsewhere in the program.
 */
static long g_realloc_call_count = 0;
static long g_fail_after_call = -1; /* -1 = never fail; N = fail starting on call N+1 */

static void *test_realloc(void *ptr, size_t size) {
    g_realloc_call_count++;
    if (g_fail_after_call >= 0 && g_realloc_call_count > g_fail_after_call) {
        return NULL;
    }
    return realloc(ptr, size);
}
#define realloc test_realloc

#define main diffsnap_real_main
#include "diffsnap.c"
#undef main
#undef realloc

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
    printf("== Test: realloc failure mid-index-collection marks EVERY item in the root+pass as failed ==\n");
    {
        batch_ctx_t ctx = {0};
        /* ALLOC_CHUNK_BATCH (32) + 8 guarantees a second realloc call
         * (32 -> 64 growth) happens partway through collection, with
         * items both before AND after that point still belonging to the
         * same root+pass -- exercising both halves of the fix. */
        const size_t N = ALLOC_CHUNK_BATCH + 8;
        char name[64];
        for (size_t i = 0; i < N; i++) {
            snprintf(name, sizeof(name), "pool/ds%03zu", i);
            int rc = batch_add(&ctx, name, "p", 1, -1, 0);
            CHECK(rc == 0, "batch_add succeeded during setup (shim not yet armed, real realloc used)");
        }
        CHECK(ctx.count == N, "all items were added to the batch");
        int all_start_unfailed = 1;
        for (size_t i = 0; i < N; i++) if (ctx.items[i].snap_failed) all_start_unfailed = 0;
        CHECK(all_start_unfailed, "snap_failed starts false for every item before the call under test");

        /* Arm the shim to fail on the SECOND realloc call made during
         * zfs_snapshot_batch_root_pass's index-collection loop -- i.e. the
         * growth from ALLOC_CHUNK_BATCH to ALLOC_CHUNK_BATCH*2, which by
         * construction happens after 32 items are already in `indices`
         * but before all N=40 are collected. */
        g_realloc_call_count = 0;
        g_fail_after_call = 1; /* allow 1 successful realloc, fail from the 2nd on */

        int rc = zfs_snapshot_batch_root_pass(&ctx, 0, "2026-01-01_00:00:00", "pool", 0);

        g_fail_after_call = -1; /* disarm immediately so nothing else is affected */

        CHECK(rc == -1, "zfs_snapshot_batch_root_pass reports failure when index collection runs out of memory");
        CHECK(g_realloc_call_count == 2, "sanity check: the shim actually saw exactly 2 realloc calls before returning (confirms we failed the call we meant to)");

        size_t failed_count = 0;
        for (size_t i = 0; i < ctx.count; i++) {
            if (ctx.items[i].snap_failed) failed_count++;
        }
        /*
         * This is the actual regression check. Before the fix: only the
         * one item being added at the moment of failure was marked, so
         * failed_count would be 1. An earlier, incomplete version of the
         * fix marked only the already-collected items plus that one
         * (failed_count == 33 here), still leaving items 33..39 --
         * belonging to the same root+pass but never visited by the loop
         * before it returned -- incorrectly unmarked. The correct fix
         * marks all N.
         */
        CHECK(failed_count == N, "ALL items in the root+pass are marked snap_failed after the OOM -- both those already collected and those never reached");

        batch_free(&ctx);
    }

    printf("================================\n");
    printf("RESULTS: %d checks run, %d failed\n", g_tests_run, g_tests_failed);
    printf("================================\n");
    if (g_tests_failed == 0) { printf("ALL CLEAR\n"); return 0; }
    printf("REVIEW FAILURES ABOVE\n");
    return 1;
}
