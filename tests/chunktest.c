/*
 * test_chunking_standalone.c
 *
 * Standalone, self-contained unit test for diffsnap's snapshot-batch
 * chunking logic (the ARGV_BYTES_CAP change): splitting a large
 * `zfs snapshot` batch into multiple invocations so no single command
 * line exceeds a fixed byte budget, and making sure a failure in one
 * chunk only marks that chunk's items as failed.
 *
 * This file does NOT link against diffsnap.c or invoke it. It embeds
 * verbatim copies of the relevant functions (zfs_root_len, same_zfs_root,
 * batch_item_in_root_pass, batch_mark_indices_failed,
 * zfs_snapshot_exec_chunk, zfs_snapshot_batch_root_pass) with
 * exec_cmd_stream replaced by a stub that records call counts and payload
 * sizes instead of actually forking/execing zfs. That means:
 *
 *   - No real ZFS pool, datasets, or root privileges are required.
 *   - It runs in milliseconds regardless of how many synthetic "datasets"
 *     a test case uses (triggering a real-world overflow with actual ZFS
 *     datasets would require ~400 real datasets with near-max-length
 *     names; this harness gets the same coverage with a handful of
 *     synthetic in-memory items).
 *   - It tests the chunking algorithm in isolation, not the full
 *     diffsnap binary end-to-end. Treat it as a unit test that
 *     complements, not replaces, the shell regression suites.
 *
 * IMPORTANT: if zfs_root_len / same_zfs_root / batch_item_in_root_pass /
 * batch_mark_indices_failed / zfs_snapshot_exec_chunk /
 * zfs_snapshot_batch_root_pass change in diffsnap.c, re-sync the copies
 * below to match, or this test will silently drift from what actually
 * ships.
 *
 * Build:
 *   cc -Wall -Wextra -std=c11 -o test_chunking_standalone test_chunking_standalone.c
 * Run:
 *   ./test_chunking_standalone
 * Exit code is 0 if all tests pass, 1 otherwise.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- constants copied from diffsnap.c ---- */
#define ZFS_PATH "/sbin/zfs"
#define ALLOC_CHUNK_BATCH 32
#define ARGV_BYTES_CAP (128 * 1024)

/* ---- struct copied from diffsnap.c (batch_item_t / batch_ctx_t) ---- */
typedef struct { char *dataset; char *prefix; size_t retention; size_t pass; int snap_failed; long long written; } batch_item_t;
typedef struct { batch_item_t *items; size_t count; size_t capacity; } batch_ctx_t;

/* ================= stub replacing exec_cmd_stream ================= */
/* Instrumentation: what would-be zfs invocations looked like. */
static int g_exec_calls = 0;
static size_t g_last_argc = 0;
static size_t g_last_payload_bytes = 0;
static int g_force_fail_call = -1;   /* 1-indexed call number to force-fail, -1 = never */
static int g_verbose = 1;

static int exec_cmd_stream(const char *const argv[], void *handler, void *data) {
    (void)handler; (void)data;
    g_exec_calls++;
    size_t argc = 0, payload_bytes = 0;
    for (size_t i = 0; argv[i]; i++) {
        argc++;
        /* Skip the fixed "zfs", "snapshot", "-r" prefix when tallying
         * payload size -- we only care about the snapshot-name arguments,
         * matching how ARGV_BYTES_CAP is accounted for in the real code. */
        int is_fixed = (i == 0) || (strcmp(argv[i], "snapshot") == 0) || (strcmp(argv[i], "-r") == 0);
        if (!is_fixed) payload_bytes += strlen(argv[i]) + 1;
    }
    g_last_argc = argc;
    g_last_payload_bytes = payload_bytes;
    if (g_verbose) {
        printf("    [exec #%d] argc=%zu payload_bytes=%zu%s\n",
               g_exec_calls, argc, payload_bytes,
               payload_bytes > ARGV_BYTES_CAP ? "  <-- WOULD EXCEED OS LIMIT" : "");
    }
    if (g_force_fail_call == g_exec_calls) return -1;
    return 0;
}

/* ============ verbatim copies of the functions under test ============ */
/* --- from diffsnap.c: zfs_root_len / same_zfs_root --- */
static size_t zfs_root_len(const char *dataset) {
    const char *slash = strchr(dataset, '/');
    return slash ? (size_t)(slash - dataset) : strlen(dataset);
}

static int same_zfs_root(const char *a, const char *b) {
    size_t a_len = zfs_root_len(a), b_len = zfs_root_len(b);
    return a_len == b_len && strncmp(a, b, a_len) == 0;
}

/* --- from diffsnap.c: batch_item_in_root_pass --- */
static int batch_item_in_root_pass(const batch_ctx_t *ctx, size_t item_idx, const char *root_dataset, size_t pass) {
    return same_zfs_root(ctx->items[item_idx].dataset, root_dataset) && ctx->items[item_idx].pass == pass;
}

/* --- from diffsnap.c: batch_mark_indices_failed --- */
static void batch_mark_indices_failed(batch_ctx_t *ctx, const size_t *indices, size_t count) {
    for (size_t k = 0; k < count; k++) ctx->items[indices[k]].snap_failed = 1;
}

/* --- from diffsnap.c: zfs_snapshot_exec_chunk --- */
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
        int written = snprintf(offset, remaining, "%s@%s_%s", ctx->items[i].dataset, ctx->items[i].prefix, timestamp);
        if (written < 0 || (size_t)written >= remaining) { free(arena); free(argv); batch_mark_indices_failed(ctx, indices, chunk_count); return -1; }
        argv[idx++] = offset; offset += written + 1;
    }
    argv[idx] = NULL;
    int rc = exec_cmd_stream(argv, NULL, NULL);
    if (rc != 0) batch_mark_indices_failed(ctx, indices, chunk_count);
    free(arena); free(argv); return rc;
}

/* --- from diffsnap.c: zfs_snapshot_batch_root_pass --- */
static int zfs_snapshot_batch_root_pass(batch_ctx_t *ctx, int recursive, const char *timestamp, const char *root_dataset, size_t pass) {
    size_t *indices = NULL, count = 0, capacity = 0;
    for (size_t i = 0; i < ctx->count; i++) {
        if (!batch_item_in_root_pass(ctx, i, root_dataset, pass)) continue;
        if (count >= capacity) {
            size_t new_cap = capacity == 0 ? ALLOC_CHUNK_BATCH : capacity * 2;
            size_t *tmp = realloc(indices, new_cap * sizeof(size_t));
            if (!tmp) { free(indices); ctx->items[i].snap_failed = 1; return -1; }
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

/* ============================ test harness ============================ */
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

static batch_item_t make_item(const char *dataset, const char *prefix, size_t pass) {
    batch_item_t it = {0};
    it.dataset = strdup(dataset);
    it.prefix = strdup(prefix);
    it.pass = pass;
    it.snap_failed = 0;
    it.retention = 1;
    it.written = 0;
    return it;
}

static void free_batch(batch_ctx_t *ctx) {
    for (size_t i = 0; i < ctx->count; i++) { free(ctx->items[i].dataset); free(ctx->items[i].prefix); }
    free(ctx->items);
    ctx->items = NULL; ctx->count = 0; ctx->capacity = 0;
}

static char *make_padded(size_t len, char fill) {
    char *s = malloc(len + 1);
    memset(s, fill, len);
    s[len] = '\0';
    return s;
}

int main(void) {
    const char *ts = "2026-07-10_00:00:00"; /* 19 chars, same format diffsnap uses */

    printf("== Test 1: small batch (50 items), well under cap -> exactly 1 exec call ==\n");
    {
        g_exec_calls = 0; g_force_fail_call = -1;
        batch_ctx_t ctx = {0};
        ctx.items = calloc(50, sizeof(batch_item_t));
        char name[64];
        for (int i = 0; i < 50; i++) {
            snprintf(name, sizeof(name), "pool/ds%03d", i);
            ctx.items[i] = make_item(name, "hourly", 0);
        }
        ctx.count = 50;
        int rc = zfs_snapshot_batch_root_pass(&ctx, 0, ts, "pool", 0);
        CHECK(rc == 0, "return code is success");
        CHECK(g_exec_calls == 1, "exactly one exec call for a small batch");
        int all_ok = 1;
        for (int i = 0; i < 50; i++) if (ctx.items[i].snap_failed) all_ok = 0;
        CHECK(all_ok, "no items marked failed");
        free_batch(&ctx);
        printf("\n");
    }

    printf("== Test 2: large batch (2000 items, ~265B each) forces multiple chunks, all under cap ==\n");
    {
        g_exec_calls = 0; g_force_fail_call = -1;
        int n = 2000;
        batch_ctx_t ctx = {0};
        ctx.items = calloc(n, sizeof(batch_item_t));
        for (int i = 0; i < n; i++) {
            char *ds = malloc(280);
            snprintf(ds, 280, "pool/%0240d", i); /* ~245-char dataset name */
            ctx.items[i].dataset = ds;
            ctx.items[i].prefix = strdup("hourly");
            ctx.items[i].pass = 0;
        }
        ctx.count = n;
        int rc = zfs_snapshot_batch_root_pass(&ctx, 0, ts, "pool", 0);
        CHECK(rc == 0, "return code is success");
        CHECK(g_exec_calls > 1, "more than one exec call was needed");
        int all_ok = 1;
        for (int i = 0; i < n; i++) if (ctx.items[i].snap_failed) all_ok = 0;
        CHECK(all_ok, "all 2000 items still succeed despite being split");
        free_batch(&ctx);
        printf("    total calls: %d\n\n", g_exec_calls);
    }

    printf("== Test 3: multi-chunk batch, 2nd exec call fails -> ONLY that chunk's items marked failed ==\n");
    {
        g_exec_calls = 0; g_force_fail_call = 2;
        int n = 2000;
        batch_ctx_t ctx = {0};
        ctx.items = calloc(n, sizeof(batch_item_t));
        for (int i = 0; i < n; i++) {
            char *ds = malloc(280);
            snprintf(ds, 280, "pool/%0240d", i);
            ctx.items[i].dataset = ds;
            ctx.items[i].prefix = strdup("hourly");
            ctx.items[i].pass = 0;
        }
        ctx.count = n;
        int rc = zfs_snapshot_batch_root_pass(&ctx, 0, ts, "pool", 0);
        CHECK(rc == -1, "overall return code reflects the failure");
        CHECK(g_exec_calls >= 2, "at least two exec calls occurred");
        int first_failed = -1, last_failed = -1, failed_count = 0;
        for (int i = 0; i < n; i++) {
            if (ctx.items[i].snap_failed) {
                failed_count++;
                if (first_failed == -1) first_failed = i;
                last_failed = i;
            }
        }
        CHECK(failed_count > 0 && failed_count < n, "some but not all items failed (not a whole-pass failure)");
        int contiguous_and_isolated = 1;
        for (int i = 0; i < first_failed; i++) if (ctx.items[i].snap_failed) contiguous_and_isolated = 0;
        for (int i = first_failed; i <= last_failed; i++) if (!ctx.items[i].snap_failed) contiguous_and_isolated = 0;
        for (int i = last_failed + 1; i < n; i++) if (ctx.items[i].snap_failed) contiguous_and_isolated = 0;
        CHECK(contiguous_and_isolated, "failed items are exactly one contiguous chunk, other chunks untouched");
        free_batch(&ctx);
        printf("    failed items: %d (indices %d..%d) out of %d\n\n", failed_count, first_failed, last_failed, n);
    }

    printf("== Test 4: recursive flag (-r) chunks the same way ==\n");
    {
        g_exec_calls = 0; g_force_fail_call = -1;
        int n = 2000;
        batch_ctx_t ctx = {0};
        ctx.items = calloc(n, sizeof(batch_item_t));
        for (int i = 0; i < n; i++) {
            char *ds = malloc(280);
            snprintf(ds, 280, "pool/%0240d", i);
            ctx.items[i].dataset = ds;
            ctx.items[i].prefix = strdup("hourly");
            ctx.items[i].pass = 0;
        }
        ctx.count = n;
        int rc = zfs_snapshot_batch_root_pass(&ctx, 1 /* recursive */, ts, "pool", 0);
        CHECK(rc == 0, "return code is success");
        CHECK(g_exec_calls > 1, "recursive path also splits into multiple calls");
        free_batch(&ctx);
        printf("\n");
    }

    printf("== Test 5: empty pass -> zero exec calls ==\n");
    {
        g_exec_calls = 0; g_force_fail_call = -1;
        batch_ctx_t ctx = {0};
        ctx.items = calloc(5, sizeof(batch_item_t));
        for (int i = 0; i < 5; i++) ctx.items[i] = make_item("pool/ds", "hourly", 1); /* pass 1, we query pass 0 */
        ctx.count = 5;
        int rc = zfs_snapshot_batch_root_pass(&ctx, 0, ts, "pool", 0);
        CHECK(rc == 0, "return code is success");
        CHECK(g_exec_calls == 0, "no exec calls when nothing matches this pass");
        free_batch(&ctx);
        printf("\n");
    }

    printf("== Test 6: exactly at the cap (two items summing to precisely ARGV_BYTES_CAP) -> 1 chunk ==\n");
    {
        g_exec_calls = 0; g_force_fail_call = -1;
        /* item_bytes = strlen(dataset) + strlen(prefix) + strlen(ts) + 3.
         * ts is 19 chars, so item_bytes = dslen + preflen + 22.
         * Build two items whose item_bytes sum to exactly ARGV_BYTES_CAP. */
        batch_ctx_t ctx = {0};
        ctx.items = calloc(2, sizeof(batch_item_t));
        size_t half = ARGV_BYTES_CAP / 2;             /* 65536 */
        size_t ds_len = half - 22, pref_len = 0;       /* item_bytes == half exactly, using empty-ish prefix padding below */
        /* Use dataset for all padding, minimal fixed prefix "p" (1 char) to
         * keep arithmetic simple: item_bytes = ds_len + 1 + 22 = ds_len+23.
         * Solve ds_len so two identical items sum to exactly the cap. */
        ds_len = (ARGV_BYTES_CAP / 2) - 23;
        char *ds1 = malloc(ds_len + 6); snprintf(ds1, ds_len + 6, "pool/%s", "");
        /* build "pool/" + padding of length (ds_len - 5) so total dataset strlen == ds_len */
        {
            char *pad = make_padded(ds_len - 5, 'x');
            free(ds1);
            ds1 = malloc(ds_len + 1);
            snprintf(ds1, ds_len + 1, "pool/%s", pad);
            free(pad);
        }
        char *ds2 = strdup(ds1);
        ds2[strlen(ds2) - 1] = 'y'; /* make it a distinct dataset name */
        ctx.items[0].dataset = ds1; ctx.items[0].prefix = strdup("p"); ctx.items[0].pass = 0;
        ctx.items[1].dataset = ds2; ctx.items[1].prefix = strdup("p"); ctx.items[1].pass = 0;
        ctx.count = 2;
        size_t item_bytes_each = strlen(ds1) + strlen("p") + strlen(ts) + 3;
        size_t total = item_bytes_each * 2;
        printf("    item_bytes_each=%zu total=%zu (cap=%d)\n", item_bytes_each, total, ARGV_BYTES_CAP);
        CHECK(total == (size_t)ARGV_BYTES_CAP, "test setup: two items sum to exactly the cap");
        int rc = zfs_snapshot_batch_root_pass(&ctx, 0, ts, "pool", 0);
        CHECK(rc == 0, "return code is success");
        CHECK(g_exec_calls == 1, "exactly-at-cap total still fits in a single chunk (boundary is inclusive)");
        free_batch(&ctx);
        (void)pref_len;
        printf("\n");
    }

    printf("== Test 7: one byte over the cap -> forces a 2nd chunk ==\n");
    {
        g_exec_calls = 0; g_force_fail_call = -1;
        batch_ctx_t ctx = {0};
        ctx.items = calloc(2, sizeof(batch_item_t));
        size_t ds_len = (ARGV_BYTES_CAP / 2) - 23;
        char *pad1 = make_padded(ds_len - 5, 'x');
        char *ds1 = malloc(ds_len + 1); snprintf(ds1, ds_len + 1, "pool/%s", pad1); free(pad1);
        char *pad2 = make_padded(ds_len - 4, 'x'); /* one char longer than ds1's padding */
        char *ds2 = malloc(ds_len + 2); snprintf(ds2, ds_len + 2, "pool/%s", pad2); free(pad2);
        ctx.items[0].dataset = ds1; ctx.items[0].prefix = strdup("p"); ctx.items[0].pass = 0;
        ctx.items[1].dataset = ds2; ctx.items[1].prefix = strdup("p"); ctx.items[1].pass = 0;
        ctx.count = 2;
        size_t b1 = strlen(ds1) + strlen("p") + strlen(ts) + 3;
        size_t b2 = strlen(ds2) + strlen("p") + strlen(ts) + 3;
        printf("    item1_bytes=%zu item2_bytes=%zu total=%zu (cap=%d)\n", b1, b2, b1 + b2, ARGV_BYTES_CAP);
        CHECK(b1 + b2 == (size_t)ARGV_BYTES_CAP + 1, "test setup: total is exactly cap+1");
        int rc = zfs_snapshot_batch_root_pass(&ctx, 0, ts, "pool", 0);
        CHECK(rc == 0, "return code is success");
        CHECK(g_exec_calls == 2, "one byte over the cap forces a second chunk");
        free_batch(&ctx);
        printf("\n");
    }

    printf("== Test 8: an oversized single item is isolated into its own chunk, doesn't block neighbors ==\n");
    {
        g_exec_calls = 0; g_force_fail_call = 2; /* fail the chunk containing the huge item */
        batch_ctx_t ctx = {0};
        ctx.items = calloc(3, sizeof(batch_item_t));
        ctx.items[0] = make_item("pool/small0", "p", 0);
        char *huge_pad = make_padded(ARGV_BYTES_CAP + 5000, 'x'); /* item alone exceeds cap */
        char *huge_ds = malloc(strlen(huge_pad) + 8);
        snprintf(huge_ds, strlen(huge_pad) + 8, "pool/%s", huge_pad);
        free(huge_pad);
        ctx.items[1].dataset = huge_ds; ctx.items[1].prefix = strdup("p"); ctx.items[1].pass = 0;
        ctx.items[2] = make_item("pool/small2", "p", 0);
        ctx.count = 3;
        int rc = zfs_snapshot_batch_root_pass(&ctx, 0, ts, "pool", 0);
        CHECK(rc == -1, "overall status reflects the forced failure");
        CHECK(g_exec_calls == 3, "3 items -> 3 separate chunks (small, huge, small)");
        CHECK(ctx.items[0].snap_failed == 0, "small0 (its own chunk, call #1) is unaffected");
        CHECK(ctx.items[1].snap_failed == 1, "the oversized item (call #2, forced to fail) is marked failed");
        CHECK(ctx.items[2].snap_failed == 0, "small2 (its own chunk, call #3) is unaffected by call #2's failure");
        free_batch(&ctx);
        printf("\n");
    }

    printf("================================\n");
    printf("RESULTS: %d checks run, %d failed\n", g_tests_run, g_tests_failed);
    printf("================================\n");
    if (g_tests_failed == 0) {
        printf("ALL CLEAR\n");
        return 0;
    } else {
        printf("REVIEW FAILURES ABOVE\n");
        return 1;
    }
}
