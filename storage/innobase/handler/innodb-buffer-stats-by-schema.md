# INNODB_BUFFER_PAGE_STATS_BY_SCHEMA

A fast `INFORMATION_SCHEMA` table that returns buffer-pool memory usage aggregated by schema. The existing `sys.innodb_buffer_stats_by_schema` view is far too slow for medium/large buffer pools (~10 minutes for 128 GiB). The new table targets ~10 seconds for the same workload.

---

## The Problem

A 128 GiB buffer pool holds ~8 million 16 KiB pages. For each page, the existing path does:

### 1. Per-page row materialization

`i_s_innodb_buffer_page_fill()` in `storage/innobase/handler/i_s.cc` produces one row per page in `INFORMATION_SCHEMA.INNODB_BUFFER_PAGE`. Each row carries ~20 columns (BLOCK_ID, SPACE, PAGE_NUMBER, TABLE_NAME, INDEX_NAME, IS_HASHED, IS_OLD, …). With 8 million pages that is 8 million rows piped into a SQL `GROUP BY`, including string columns up to 1024 bytes.

### 2. Per-page dictionary lookup for `table_name`

For every index page, [i_s.cc:3911-3936](server/storage/innobase/handler/i_s.cc#L3911-L3936) does:

```cpp
dict_sys.freeze(SRW_LOCK_CALL);
const dict_index_t* index = dict_index_get_if_in_cache_low(page_info->index_id);
…
dict_sys.unfreeze();
```

`dict_index_get_if_in_cache_low()` in [dict0dict.cc:3606](server/storage/innobase/dict/dict0dict.cc#L3606) is an **O(T·I)** linear scan:

```cpp
for (dict_table_t *table = UT_LIST_GET_FIRST(dict_sys.table_LRU); table; …)
    if (dict_index_t *index = dict_table_find_index_on_id(table, index_id))
        return index;
```

With 1000 schemas × ~10 tables × ~2 indexes ≈ 20 000 indexes in cache, and 8 million pages, that is **~160 billion comparisons** plus a dict_sys rd-lock acquire/release **per page**.

### 3. The user pays the storage cost too

The 8 M rows must be stored in a temp table, then aggregated. That is many GiB of throwaway data the user never reads.

### 4. SQL-layer aggregation on a string key

The `sys.innodb_buffer_stats_by_schema` view groups on a derived expression (`SUBSTRING_INDEX(table_name, '.', 1)`). The aggregation cannot push down into InnoDB and runs entirely in the SQL layer on the materialized temp table.

**Result:** ~10 minutes for 128 GiB. Wall-clock is dominated by (2) — dict_sys scans — followed by (1) and (3).

---

## The Idea

Skip everything that produces per-page rows. Walk the buffer pool once, and aggregate **outside** the buffer-pool lock keyed by `index_id`. Resolve schema names once at the end (one lookup per *distinct index*, not per page).

This makes the work **proportional to (pages + indexes)** instead of **(pages × dictionary)**.

**Why key by `index_id`, not `dict_table_t*`?** During the chunk scan we hold `buf_pool.mutex` and must not allocate or block; touching `dict_sys` to resolve `index_id → dict_index_t* → dict_table_t*` requires `dict_sys.freeze()`, which adds work to the hot path and contends with DDL. Keying by the bare `index_id` (a 64-bit int read straight from the page header) lets us stay completely out of `dict_sys` during the scan and resolve names *after* the buffer-pool lock is released.

**Why not hash-insert directly under `buf_pool.mutex`?** This is the critical scale fix. At a target of 128 GiB pool × 1000 schemas × 1000 tables × ~2 indexes, the by-index hash map can grow to ~2 M entries. `std::unordered_map` rehashes log₂ times as it grows; one rehash at 2 M entries allocates and memmoves ~64 MiB. Doing that **under the global buffer-pool mutex** stalls every page read and eviction on the server for the duration. The implementation instead writes scanned pages into a pre-allocated **scratch buffer** under the lock (a flat array, zero allocation), and folds the scratch into the hash map *after* the lock is released. The scratch is sized once at fill start to the largest chunk (~192 KiB for a 128 MiB chunk at 16 KiB pages — fits in L2).

### What we save

| Cost                                  | INNODB_BUFFER_PAGE                | INNODB_BUFFER_PAGE_STATS_BY_SCHEMA          |
| ------------------------------------- | --------------------------------- | ------------------------------------------- |
| Per-page work                         | parse page, store ~20 fields/row  | parse page, write 24 B to a flat scratch    |
| Allocations under `buf_pool.mutex`    | yes (heap zalloc per chunk)       | **none** — scratch is sized once up-front   |
| dict_sys lookups                      | 1 per page (~8 M)                 | 1 per distinct index (~2 M worst-case)      |
| dict_sys lock acquires                | 1 per page (~8 M)                 | 1, held briefly after pool scan             |
| SQL rows generated                    | ~8 M                              | ~1 per schema (~1000)                       |
| Temp table / `GROUP BY` cost          | huge                              | nothing                                     |
| String comparisons in `GROUP BY`      | 8 M                               | 0                                           |

Expected wall-clock at 128 GiB: well under 10 s, dominated by raw memory scan of the buffer pool (which on modern hardware is single-digit GB/s — 128 GiB is ~20 s sequential, much less if NUMA-local and parallel; the per-page work is trivial).

### Memory bound at target scale

Target: 128 GiB pool, 1000 schemas × 1000 tables × ~2 indexes ≈ 2 M indexes worst case.

| Structure                    | Size at 128 GiB / 2 M indexes                 |
| ---------------------------- | --------------------------------------------- |
| Scratch buffer               | 24 B × largest_chunk (~192 KiB at 128 MiB chunks) |
| `by_index` (pre-reserved)    | ~32 MiB initial bucket array + ~80 MiB entries at full saturation (~120 MiB total) |
| Per-table name cache         | ~80 MiB (1 M tables × ~80 B/entry)            |
| `by_schema`                  | ~64 KiB (1000 schemas × small string + agg)   |

Peak ≈ 200 MiB transient during a query. Freed immediately after. Acceptable on a 128 GiB server.

### What we deliberately give up

Per-page detail. This table is **only** for "which schema is the noisy neighbour?" Operators who want per-page detail keep using `INNODB_BUFFER_PAGE`.

---

## Specification

### Table name

`INFORMATION_SCHEMA.INNODB_BUFFER_PAGE_STATS_BY_SCHEMA`

### Columns

| Column           | Type          | Meaning                                                            |
| ---------------- | ------------- | ------------------------------------------------------------------ |
| `SCHEMA_NAME`    | VARCHAR(192)  | Schema name, or `InnoDB System` for system tables / orphan pages  |
| `PAGES`          | BIGINT        | Number of pages in the buffer pool belonging to this schema       |
| `ALLOCATED_BYTES`| BIGINT        | `SUM(compressed_size or 16384)`                                    |
| `DATA_BYTES`     | BIGINT        | `SUM(page_header data_size)` — live row bytes                     |

That's it. Four columns. Sized for one row per schema.

The columns match the high-value subset of `sys.innodb_buffer_stats_by_schema`. `pages_hashed`, `pages_old`, `rows_cached` are dropped — operators making placement decisions care about bytes, and dropping them keeps the per-page hot path minimal.

### Schema-name buckets

| Source                                 | Bucket                                |
| -------------------------------------- | ------------------------------------- |
| Index page with resolvable table       | schema portion of `table->name` (text before `/`) |
| Index page whose index is not in cache | `InnoDB System`                       |
| Non-index pages (undo, sys, free, …)  | `InnoDB System`                       |

Rationale: a sysadmin trying to find a noisy schema does not care about distinguishing "system overhead" from "evicted-from-dict-cache index"; both are noise relative to the per-schema buckets.

### Behavior

- Behaves like `INNODB_BUFFER_PAGE`: requires `PROCESS` privilege, returns nothing if InnoDB is not started.
- Snapshots the buffer pool one chunk at a time (same chunked-with-mutex-release pattern as the existing implementation, so we do not pin `buf_pool.mutex` for the whole scan).
- The result is approximate. Pages can move between chunks while we scan. That is fine — the table is for relative sizing, not accounting.

### System variable

```
innodb_buffer_page_stats_by_schema_enabled  (BOOLEAN, default: ON, dynamic, global)
```

When `OFF`, queries against the table return an empty result and a warning. The table is always *defined* (plugin loaded) so monitoring tooling does not fail with "unknown table"; the variable only gates execution.

`PLUGIN_VAR_OPCMDARG` (settable both on command line and at runtime via `SET GLOBAL`).

Default `ON` because the table is cheap to leave enabled — it has no runtime cost when not queried.

---

## Implementation Outline

### Files added

```
server/storage/innobase/handler/i_s_buffer_page_stats_by_schema.cc
server/mysql-test/suite/innodb/t/innodb_buffer_page_stats_by_schema.test
server/mysql-test/suite/innodb/t/innodb_buffer_page_stats_by_schema.opt
server/mysql-test/suite/innodb/r/innodb_buffer_page_stats_by_schema.result
server/mysql-test/suite/innodb/t/innodb_buffer_page_stats_by_schema_compare.test
server/mysql-test/suite/innodb/t/innodb_buffer_page_stats_by_schema_compare.opt
server/mysql-test/suite/innodb/r/innodb_buffer_page_stats_by_schema_compare.result
```

The `.cc` is ~330 lines including the helpers, plugin descriptor, fields-info, fill function, and license header.

The `.opt` is required: every InnoDB I_S plugin is disabled by default and must be opted in via a command-line flag (`--loose-innodb-buffer-page-stats-by-schema`). The umbrella `mysql-test/include/have_innodb.opt` enables only the existing plugins; rather than modify it, the per-test `.opt` enables ours just for this test.

### Files modified

1. `server/storage/innobase/handler/i_s.h` — add `extern struct st_maria_plugin i_s_innodb_buffer_page_stats_by_schema;` and `extern my_bool innodb_buffer_page_stats_by_schema_enabled;`.
2. `server/storage/innobase/handler/ha_innodb.cc` — add the new plugin to the `maria_declare_plugin(innobase)` list; declare and register the boolean sysvar.
3. `server/storage/innobase/CMakeLists.txt` — add `handler/i_s_buffer_page_stats_by_schema.cc` to the source list.

No changes to any existing implementation file beyond these registrations.

### Algorithm

```
fill():
  if (!innodb_started) return empty
  if (!has PROCESS) error (1227 ER_SPECIFIC_ACCESS_DENIED_ERROR)
  if (!enabled) push warning, return empty

  // Allocate once, before the scan loop. Reserve based on pool size.
  scratch = vector<entry>(max_chunk_size_in_blocks)   // ~192 KiB
  by_index = unordered_map<index_id_t, agg>
  by_index.reserve(n_pool_pages / 4 + 1024)           // avoid rehashing
  orphan = {}

  for each chunk in buf_pool.chunks:
    lock buf_pool.mutex
    n = 0
    for each block in chunk:
      if inspect_block(block, scratch[n]):            // pure read, no alloc
        ++n
    unlock buf_pool.mutex                              // released ASAP
    fold_scratch(scratch, n, by_index, orphan)         // rehash safely here

  // Resolve index_id → schema, with a per-table cache so multi-index
  // tables only pay strchr+string once.
  name_cache = unordered_map<dict_table_t*, string>
  by_schema  = unordered_map<string, agg>
  dict_sys.freeze()
  for (index_id, a) in by_index:
    index = dict_index_get_if_in_cache_low(index_id)
    if !index || !index->table:
      orphan += a                                      // evicted from dict cache
      continue
    schema = name_cache.get_or_compute(index->table, schema_of(name))
    by_schema[schema] += a
  dict_sys.unfreeze()
  if orphan.pages: by_schema["InnoDB System"] += orphan

  for (schema, a) in by_schema:
    emit row
```

Key invariants enforced in the implementation:

- **Zero allocations under `buf_pool.mutex`.** The scratch buffer is pre-sized to the largest chunk before the scan loop starts. The hash map is filled *after* releasing the mutex per chunk. This is the critical scale fix — without it, `unordered_map` rehashes at millions of entries hold the global pool lock for tens of ms each, stalling the whole server.
- **`dict_sys` is touched only once total**, after the entire pool scan — not per chunk. Per-chunk freeze would have been correct but adds DDL contention proportional to chunk count for no benefit, since the `index_id` we recorded is stable.
- **`PROCESS` privilege check uses `check_global_access`**, which in MariaDB 10.6 raises `ER_SPECIFIC_ACCESS_DENIED_ERROR` (1227) and returns non-zero — it does *not* silently return empty. This matches the behaviour of `INNODB_BUFFER_PAGE`.
- **Allocation failures surface as `ER_OUTOFMEMORY`** to the client, not `std::terminate`. The scratch / map allocations are wrapped in a `try/catch(bad_alloc)`.

### Why this is fast

- **Per page (under lock):** one cache-friendly read of `bpage->state`, `bpage->frame`, and a few page-header bytes; one 24-byte sequential write into the scratch array. No hash, no string, no SQL plumbing, no allocation.
- **Per page (after unlock):** one hash-map insert keyed by a 64-bit int. Hash is pre-reserved to avoid rehashing — `O(1)` amortized with no surprises.
- **Per distinct index:** one `dict_index_get_if_in_cache_low`. Each call is itself O(T·I) — a linear scan of the dict cache — so the resolve loop is O(distinct_indexes × dict_size). At worst-case scale (50 K distinct resident indexes × 2 M dict entries) this is ~100 B comparisons, executed *once per query* outside any InnoDB lock. Still dramatically less work than the existing per-page path; not the O(indexes) one might hope for. A `dict_sys.table_id_hash`-based O(1) lookup keyed on `table_id` instead of `index_id` would remove this — see follow-up in PR 2.
- **Per distinct table:** one `bps_schema_of` call (cached) — pays the `strchr` + `string` allocation once per table, not per index.
- **dict_sys lock contention:** acquired once per query, held only for resolve_to_schemas.

### Helper functions

```cpp
struct bps_agg { ulonglong pages, allocated_bytes, data_bytes; ... };
struct bps_scratch_entry { index_id_t index_id;          // 0 = orphan
                           ulonglong alloc_size, data_size; };

// Under buf_pool.mutex. Pure inspection. Returns whether in_file.
static bool bps_inspect_block(const buf_block_t&, bps_scratch_entry&);

// Takes the lock, scans into scratch, releases. Returns entries written.
static size_t bps_scan_chunk(const buf_block_t* blocks, size_t n,
                             bps_scratch_entry* scratch);

// Runs OUTSIDE any InnoDB lock. Safe to rehash here.
static void bps_fold_scratch(const bps_scratch_entry*, size_t,
                             std::unordered_map<index_id_t, bps_agg>&,
                             bps_agg& orphan);

static std::string bps_schema_of(const char* full_name);

// dict_sys.freeze() inside. Caches per dict_table_t* so two indexes on
// the same table share one strchr + string allocation.
static void bps_resolve_to_schemas(
  const std::unordered_map<index_id_t, bps_agg>&,
  std::unordered_map<std::string, bps_agg>&,
  bps_agg& orphan);

static size_t bps_max_chunk_size();
static int bps_emit_rows(THD*, TABLE*,
                         const std::unordered_map<std::string, bps_agg>&);
```

#### Footgun: `buf_pool_t::chunk_t` is private

The chunk type is declared `private` inside `buf_pool_t`. Existing code in `i_s.cc` accesses `buf_pool.chunks[n].blocks` and `.size` without ever *naming* the type — that compiles because the instance is reached via a public field. Helpers in a separate file cannot take `const buf_pool_t::chunk_t&` as a parameter (compile error: "struct buf_pool_t::chunk_t is private within this context"). Pass `const buf_block_t* blocks, size_t n_blocks` instead. Discovered the hard way during the first build.

---

## Testing

Two test files. The first (`innodb_buffer_page_stats_by_schema.test`) is the
self-contained behaviour suite — schema, gating, privilege, scale. The second
(`innodb_buffer_page_stats_by_schema_compare.test`) cross-checks the table's
numbers against the slow canonical sources of the same aggregation so a future
change to the C++ aggregation can't silently diverge.

### `innodb_buffer_page_stats_by_schema.test`

1. Verify the table exists, columns are as specified.
2. Create two schemas, insert data into both, fill the buffer pool by `SELECT *`.
3. Query the new table; assert both schemas appear with `PAGES > 0` and the totals are non-zero.
4. Toggle `innodb_buffer_page_stats_by_schema_enabled = OFF`; assert the table returns an empty result and a warning.
5. Verify `ER_SPECIFIC_ACCESS_DENIED_ERROR` is raised without `PROCESS` privilege (not "empty result" — see algorithm note above).
6. **Scale path:** create 50 schemas, touch each so its index pages enter the buffer pool, assert all 50 appear with `PAGES > 0`. This exercises the scratch-buffer + post-aggregation code path that handles production buffer pools (one schema is not enough to trigger the fold loop with multiple distinct keys).

The test does not assert exact byte counts — buffer pool contents are non-deterministic; the test asserts shape and presence.

### `innodb_buffer_page_stats_by_schema_compare.test`

Cross-checks the new table against the two slower sources of the same
aggregation:

* `INFORMATION_SCHEMA.INNODB_BUFFER_PAGE` — row-per-page raw data.
* `sys.x$innodb_buffer_stats_by_schema` — the SQL view atop `INNODB_BUFFER_PAGE`.

Three schemas (`bps_cmp_a/b/c`) each get a 200-row table. Multi-page tables are
the case the new table is designed for, and they exercise the `index_id`
deduplication path — many rows in `INNODB_BUFFER_PAGE` for the same index fold
into one row in the new table. Then:

1. **Schema-set agreement.** The set of user schemas reported by the new table
   for `bps_cmp_%` must equal the set derived from `INNODB_BUFFER_PAGE`. A
   symmetric-difference query asserts the result is empty. Catches a missed
   schema bucket or a wrong "InnoDB System" classification.
2. **Per-schema page count within tolerance.** For each user schema, compare
   `PAGES` from the new table against `COUNT(*)` grouped by the sys-view's
   `SUBSTRING_INDEX(table_name, '.', 1)` bucketing of `INNODB_BUFFER_PAGE`.
   Allow 5 pages of drift per schema — the buffer pool is live and pages can
   move between the two queries. Tight enough to catch a real divergence
   (wrong `index_id`, wrong schema bucket would shift the count by the whole
   index, not by 1–2 pages); lax enough not to flake.
3. **`ALLOCATED_BYTES` agrees with sys-view `allocated`.** The new table's
   definition (`SUM(compressed_size or 16384)`) is identical to the sys view's;
   verify the two return the same value within `5 × 16384 = 81920` bytes of
   drift (the byte equivalent of the 5-page tolerance above).

The "InnoDB System" bucket is excluded — undo/sys/free pages churn during the
test and neither source claims determinism for it.

### Companion `.opt` files

Each test gets a one-line `.opt` containing `--loose-innodb-buffer-page-stats-by-schema`.
InnoDB I_S plugins are disabled by default; without the flag the server logs
`Plugin '…' is disabled.` and `DESC` returns `ER_UNKNOWN_TABLE`.

### Verified

Built on Ubuntu 20.04 (gcc 9.4, cmake 3.16) inside the documented Docker image.
`mariadbd` links cleanly with the new plugin. Both new tests pass; the compare
test passes 5/5 sequential runs (no flakiness). Existing tests
`innodb_information_schema_buffer`, `innodb_buffer_pool_load_now`,
`innodb_information_schema_tables`, and `innodb_skip_innodb_is_tables` still
pass — no regressions.

---

## What we are not doing (and why)

- **Not caching results between queries.** Each query is independent. Cache invalidation is a tar pit, and the cost is already low enough.
- **Not adding a background aggregator thread.** It would consume CPU continuously; users querying once per minute would get worse responsiveness for free constant overhead.
- **Not extending `INNODB_BUFFER_POOL_STATS`.** That table is per-pool. Aggregations belong in a separate table.
- **Not changing `INNODB_BUFFER_PAGE` or the `sys.innodb_buffer_stats_by_schema` view.** Existing behavior preserved for compatibility.
