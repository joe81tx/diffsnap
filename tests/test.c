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
 * Requires only a POSIX system with /bin/true and /bin/false (or
 * equivalents) -- no ZFS needed, since exec_cmd_stream/_lenient are
 * generic process-exec plumbing, not ZFS-specific.
 *
 * Build:
 *   cc -Wall -Wextra -std=c11 -o test_metrics_scoping test_metrics_scoping_standalone.c
 * Run:
 *   ./test_metrics_scoping
 */

#define main diffsnap_real_main
#include "diffsnap.c"
#undef main

#include <assert.h>
#include <sys/stat.h>

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

static const char *find_bin(const char *const candidates[]) {
    for (size_t i = 0; candidates[i]; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0 && (st.st_mode & S_IXUSR)) return candidates[i];
    }
    return NULL;
}

int main(void) {
    printf("== Test 1: exec_cmd_stream (strict) vs exec_cmd_stream_lenient on a clean-success process ==\n");
    {
        const char *const true_candidates[] = {"/bin/true", "/usr/bin/true", NULL};
        const char *true_bin = find_bin(true_candidates);
        CHECK(true_bin != NULL, "found a 'true' binary on this system");
        if (true_bin) {
            const char *const argv[] = {true_bin, NULL};
            CHECK(exec_cmd_stream(argv, NULL, NULL) == 0, "strict: clean success (exit 0) succeeds");
            CHECK(exec_cmd_stream_lenient(argv, NULL, NULL) == 0, "lenient: clean success (exit 0) succeeds");
        }
        printf("\n");
    }

    printf("== Test 2: exec_cmd_stream (strict) vs exec_cmd_stream_lenient on a nonzero-exit process ==\n");
    {
        const char *const false_candidates[] = {"/bin/false", "/usr/bin/false", NULL};
        const char *false_bin = find_bin(false_candidates);
        CHECK(false_bin != NULL, "found a 'false' binary on this system");
        if (false_bin) {
            const char *const argv[] = {false_bin, NULL};
            CHECK(exec_cmd_stream(argv, NULL, NULL) != 0, "strict: nonzero exit is treated as failure (this is the existing, unchanged behavior)");
            CHECK(exec_cmd_stream_lenient(argv, NULL, NULL) == 0, "lenient: nonzero exit is NOT treated as failure (the new behavior for scoped zfs get)");
        }
        printf("\n");
    }

    printf("== Test 3: abnormal termination (signal) is still fatal in BOTH modes ==\n");
    {
        /* /bin/sh -c 'kill -9 $$' makes the child kill itself with SIGKILL --
         * WIFEXITED will be false, exercising the wait_failed/child_exited
         * path rather than the exit-status path. */
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
        const char *const echo_candidates[] = {"/bin/echo", "/usr/bin/echo", NULL};
        const char *echo_bin = find_bin(echo_candidates);
        CHECK(echo_bin != NULL, "found an echo binary");
        if (echo_bin) {
            metric_ctx_t ctx = {0};
            const char *const argv[] = {echo_bin, "pool/child\t12345", NULL};
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

    printf("== Test 5: root_list_add_unique deduplicates by ROOT, not by full dataset name ==\n");
    {
        root_list_t list = {0};
        root_list_add_unique(&list, "pool/a");
        root_list_add_unique(&list, "pool/b");
        CHECK(list.count == 1, "pool/a and pool/b share root 'pool' -> deduplicated to 1 entry");
        CHECK(strcmp(list.roots[0], "pool") == 0, "the single root is 'pool'");
        root_list_add_unique(&list, "otherpool/x");
        CHECK(list.count == 2, "a genuinely different root is added");
        root_list_add_unique(&list, "otherpool");
        CHECK(list.count == 2, "a bare root name matching an existing root is not duplicated");
        root_list_free(&list);
        printf("\n");
    }

    printf("== Test 6: collect_due_roots across both batches, with duplicates and cross-batch overlap ==\n");
    {
        batch_ctx_t std_b = {0}, rec_b = {0};
        batch_add(&std_b, "pool/a", "p", 1, -1, 0);
        batch_add(&std_b, "pool/b", "p", 1, -1, 0);       /* same root as pool/a */
        batch_add(&std_b, "otherpool/x", "p", 1, -1, 0);
        batch_add(&rec_b, "pool", "p", 1, -1, 0);          /* same root as pool/a, pool/b -- via rec_b this time */
        batch_add(&rec_b, "thirdpool/y/z", "p", 1, -1, 0);

        root_list_t due_roots = {0};
        int rc = collect_due_roots(&due_roots, &std_b, &rec_b);
        CHECK(rc == 0, "collect_due_roots succeeds");
        CHECK(due_roots.count == 3, "3 distinct roots across both batches (pool, otherpool, thirdpool), despite 5 items");

        int has_pool = 0, has_other = 0, has_third = 0;
        for (size_t i = 0; i < due_roots.count; i++) {
            if (strcmp(due_roots.roots[i], "pool") == 0) has_pool = 1;
            if (strcmp(due_roots.roots[i], "otherpool") == 0) has_other = 1;
            if (strcmp(due_roots.roots[i], "thirdpool") == 0) has_third = 1;
        }
        CHECK(has_pool && has_other && has_third, "all three expected roots are present");

        root_list_free(&due_roots);
        batch_free(&std_b);
        batch_free(&rec_b);
        printf("\n");
    }

    printf("== Test 7: batch_filter_by_metrics -- found/valid/above-threshold items are kept with .written cached ==\n");
    {
        batch_ctx_t b = {0};
        batch_add(&b, "pool/a", "p", 1, -1, 100);   /* min_bytes=100 */

        metric_ctx_t metrics = {0};
        metrics.items = calloc(1, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/a");
        metrics.items[0].written = 5000;
        metrics.count = 1;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, &global_status);
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
        batch_add(&b, "pool/missing", "p", 1, -1, 0);

        metric_ctx_t metrics = {0}; /* empty: nothing matches */
        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, &global_status);
        CHECK(b.count == 0, "not-found item is removed from the batch");
        CHECK(global_status == 1, "global_status is flagged (matches old inline-check behavior)");

        batch_free(&b);
        printf("\n");
    }

    printf("== Test 9: batch_filter_by_metrics -- invalid (-1) written metric is removed and flagged ==\n");
    {
        batch_ctx_t b = {0};
        batch_add(&b, "pool/a", "p", 1, -1, 0);

        metric_ctx_t metrics = {0};
        metrics.items = calloc(1, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/a");
        metrics.items[0].written = -1; /* invalid */
        metrics.count = 1;

        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, &global_status);
        CHECK(b.count == 0, "invalid-metric item is removed");
        CHECK(global_status == 1, "global_status is flagged");

        free(metrics.items);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 10: batch_filter_by_metrics -- below min_bytes is removed SILENTLY (no error flag) ==\n");
    {
        batch_ctx_t b = {0};
        batch_add(&b, "pool/a", "p", 1, -1, 999999); /* high min_bytes threshold */

        metric_ctx_t metrics = {0};
        metrics.items = calloc(1, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/a");
        metrics.items[0].written = 100; /* below threshold */
        metrics.count = 1;

        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, &global_status);
        CHECK(b.count == 0, "below-threshold item is removed");
        CHECK(global_status == 0, "global_status is NOT flagged (this is a normal skip, not an error)");

        free(metrics.items);
        batch_free(&b);
        printf("\n");
    }

    printf("== Test 11: batch_filter_by_metrics -- mixed batch, compaction preserves surviving items correctly ==\n");
    {
        batch_ctx_t b = {0};
        batch_add(&b, "pool/keep1", "p", 1, -1, 0);
        batch_add(&b, "pool/notfound", "p", 1, -1, 0);
        batch_add(&b, "pool/keep2", "p", 1, -1, 0);
        batch_add(&b, "pool/belowthresh", "p", 1, -1, 999999);
        batch_add(&b, "pool/keep3", "p", 1, -1, 0);

        metric_ctx_t metrics = {0};
        metrics.items = calloc(4, sizeof(metric_item_t));
        strcpy(metrics.items[0].name, "pool/keep1"); metrics.items[0].written = 100;
        strcpy(metrics.items[1].name, "pool/keep2"); metrics.items[1].written = 200;
        strcpy(metrics.items[2].name, "pool/keep3"); metrics.items[2].written = 300;
        strcpy(metrics.items[3].name, "pool/belowthresh"); metrics.items[3].written = 5;
        metrics.count = 4;
        qsort(metrics.items, metrics.count, sizeof(metric_item_t), compare_metrics);

        int global_status = 0;
        batch_filter_by_metrics(&b, &metrics, &global_status);
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

    printf("================================\n");
    printf("RESULTS: %d checks run, %d failed\n", g_tests_run, g_tests_failed);
    printf("================================\n");
    if (g_tests_failed == 0) { printf("ALL CLEAR\n"); return 0; }
    printf("REVIEW FAILURES ABOVE\n");
    return 1;
}
