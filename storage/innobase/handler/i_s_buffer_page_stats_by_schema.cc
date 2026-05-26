/*****************************************************************************

Copyright (c) 2026, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file handler/i_s_buffer_page_stats_by_schema.cc
INFORMATION_SCHEMA.INNODB_BUFFER_PAGE_STATS_BY_SCHEMA — a fast,
aggregated view of buffer-pool memory usage per schema. See
innodb-buffer-stats-by-schema.md for design rationale.
*******************************************************/

#include "univ.i"
#include <mysql_version.h>
#include <field.h>
#include <sql_acl.h>
#include <sql_show.h>

#include "i_s.h"
#include "buf0buf.h"
#include "dict0dict.h"
#include "dict0mem.h"
#include "page0page.h"
#include "btr0btr.h"
#include "fil0fil.h"
#include "srv0start.h"

#include <algorithm>
#include <unordered_map>
#include <string>
#include <vector>
#include <new>

/** Guard for SELECTing from the table. When OFF, queries return an empty
result with a warning. */
my_bool innodb_buffer_page_stats_by_schema_enabled = TRUE;

/** Maximum blocks scanned per buf_pool.mutex acquisition. Mirrors the
MAX_BUF_INFO_CACHED cap used by INNODB_BUFFER_PAGE. Keeps mutex hold
time bounded regardless of innodb_buffer_pool_chunk_size. */
static constexpr size_t BPS_BLOCKS_PER_BATCH= 8192;

/** Per-bucket counters. */
struct bps_agg
{
  ulonglong pages;
  ulonglong allocated_bytes;
  ulonglong data_bytes;

  void add(ulonglong alloc, ulonglong data) noexcept
  {
    ++pages;
    allocated_bytes+= alloc;
    data_bytes+= data;
  }
  void merge(const bps_agg &o) noexcept
  {
    pages+= o.pages;
    allocated_bytes+= o.allocated_bytes;
    data_bytes+= o.data_bytes;
  }
};

/** One scanned page recorded under the buf_pool.mutex. Three 8-byte fields
keep the scratch buffer cache-friendly and dense (24 bytes per page).
index_id == 0 marks a non-index ("orphan") page. */
struct bps_scratch_entry
{
  index_id_t index_id;
  ulonglong  alloc_size;
  ulonglong  data_size;
};

static const char BPS_ORPHAN_BUCKET[]= "InnoDB System";

/** Inspect one block. Cheap, lock-friendly. Returns whether the page is
in_file() and worth recording. Sets index_id=0 for non-index pages so the
caller can fold them into the orphan bucket. */
static bool bps_inspect_block(const buf_block_t &block,
                              bps_scratch_entry &out) noexcept
{
  const buf_page_t &bp= block.page;
  uint32_t state= bp.state();
  if (state < buf_page_t::UNFIXED)
    return false;

  ulint zip_size= bp.zip.ssize
    ? ((UNIV_ZIP_SIZE_MIN >> 1) << bp.zip.ssize) : 0;
  out.alloc_size= zip_size ? zip_size : srv_page_size;
  out.data_size= 0;
  out.index_id= 0;

  if (state >= buf_page_t::READ_FIX && state < buf_page_t::WRITE_FIX)
    return true; /* in use, but contents not stable; treat as orphan */

  const byte *frame= bp.frame ? bp.frame : bp.zip.data;
  if (!frame)
    return true;

  uint16_t page_type= fil_page_get_type(frame);
  if (!fil_page_type_is_index(page_type))
    return true;

  out.index_id= btr_page_get_index_id(frame);
  if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_TYPE_INSTANT)
  {
    ulint top= page_header_get_field(frame, PAGE_HEAP_TOP);
    ulint sup_end= page_is_comp(frame)
      ? PAGE_NEW_SUPREMUM_END : PAGE_OLD_SUPREMUM_END;
    ulint garbage= page_header_get_field(frame, PAGE_GARBAGE);
    out.data_size= (top > sup_end + garbage) ? top - sup_end - garbage : 0;
  }
  return true;
}

/** Scan one chunk into the caller's scratch buffer. The scratch buffer
must already have room for chunk.size entries. No allocations, no map
inserts under the buf_pool.mutex. Returns the number of entries written.
At 8192 pages/chunk this writes ~192 KiB sequentially — cache-friendly. */
static size_t bps_scan_chunk(const buf_block_t *blocks, size_t n_blocks,
                             bps_scratch_entry *scratch) noexcept
{
  size_t written= 0;
  mysql_mutex_lock(&buf_pool.mutex);
  for (size_t i= 0; i < n_blocks; ++i)
    if (bps_inspect_block(blocks[i], scratch[written]))
      ++written;
  mysql_mutex_unlock(&buf_pool.mutex);
  return written;
}

/** Fold scratch entries into the by-index map and orphan bucket. Runs
outside any InnoDB lock so std::unordered_map rehashes are safe. */
static void bps_fold_scratch(const bps_scratch_entry *scratch, size_t n,
                             std::unordered_map<index_id_t, bps_agg> &by_index,
                             bps_agg &orphan)
{
  for (size_t i= 0; i < n; ++i)
  {
    const bps_scratch_entry &e= scratch[i];
    if (e.index_id)
      by_index[e.index_id].add(e.alloc_size, e.data_size);
    else
      orphan.add(e.alloc_size, e.data_size);
  }
}

/** Copy the schema portion of a `db/table` name (everything before the
first '/'). Returns BPS_ORPHAN_BUCKET if the format is unexpected. */
static std::string bps_schema_of(const char *full_name)
{
  if (!full_name)
    return BPS_ORPHAN_BUCKET;
  const char *slash= strchr(full_name, '/');
  if (!slash || slash == full_name)
    return BPS_ORPHAN_BUCKET;
  return std::string(full_name, size_t(slash - full_name));
}

/** Resolve index_id → schema once per distinct index. Caches the schema
string per dict_table_t* so multi-index tables don't pay strchr+string twice. */
static void bps_resolve_to_schemas(
  const std::unordered_map<index_id_t, bps_agg> &by_index,
  std::unordered_map<std::string, bps_agg> &by_schema,
  bps_agg &orphan)
{
  std::unordered_map<const dict_table_t*, std::string> name_cache;
  name_cache.reserve(by_index.size() / 2 + 1);

  dict_sys.freeze(SRW_LOCK_CALL);
  for (const auto &kv : by_index)
  {
    const dict_index_t *index= dict_index_get_if_in_cache_low(kv.first);
    if (!index || !index->table)
    {
      orphan.merge(kv.second);
      continue;
    }
    auto cached= name_cache.find(index->table);
    if (cached == name_cache.end())
      cached= name_cache.emplace(index->table,
                                 bps_schema_of(index->table->name.m_name))
              .first;
    by_schema[cached->second].merge(kv.second);
  }
  dict_sys.unfreeze();
}

/** Emit one I_S row per schema bucket. */
static int bps_emit_rows(
  THD *thd, TABLE *table,
  const std::unordered_map<std::string, bps_agg> &by_schema)
{
  Field **fields= table->field;
  for (const auto &kv : by_schema)
  {
    fields[0]->store(kv.first.data(), uint(kv.first.size()),
                     system_charset_info);
    fields[0]->set_notnull();
    fields[1]->store(kv.second.pages, true);
    fields[2]->store(kv.second.allocated_bytes, true);
    fields[3]->store(kv.second.data_bytes, true);
    if (schema_table_store_record(thd, table))
      return 1;
  }
  return 0;
}

/** Allocate the scratch buffer and reserve the by-index map. Sizing the
scratch to BPS_BLOCKS_PER_BATCH (not the largest chunk) keeps mutex hold
time bounded even if innodb_buffer_pool_chunk_size is tuned up. Throws
std::bad_alloc on OOM. */
static void bps_allocate_workspace(
  std::vector<bps_scratch_entry> &scratch,
  std::unordered_map<index_id_t, bps_agg> &by_index)
{
  scratch.resize(BPS_BLOCKS_PER_BATCH);
  /* Reserve assuming up to ~one distinct index per 4 resident pages.
  With 8 M pages that's a 2 M-bucket initial map (~32 MiB) — preferable
  to growing 20+ times during fold. */
  by_index.reserve(buf_pool.get_n_pages() / 4 + 1024);
}

/** Walk the entire buffer pool, batching by BPS_BLOCKS_PER_BATCH. */
static void bps_scan_pool(
  bps_scratch_entry *scratch,
  std::unordered_map<index_id_t, bps_agg> &by_index,
  bps_agg &orphan)
{
  ulint n_chunks= ut_min(buf_pool.n_chunks, buf_pool.n_chunks_new);
  for (ulint n= 0; n < n_chunks; ++n)
  {
    const buf_block_t *blocks= buf_pool.chunks[n].blocks;
    size_t remaining= buf_pool.chunks[n].size;
    while (remaining)
    {
      size_t batch= std::min(remaining, BPS_BLOCKS_PER_BATCH);
      size_t written= bps_scan_chunk(blocks, batch, scratch);
      bps_fold_scratch(scratch, written, by_index, orphan);
      blocks+= batch;
      remaining-= batch;
    }
  }
}

namespace Show {

static ST_FIELD_INFO i_s_innodb_buffer_page_stats_by_schema_fields_info[]=
{
  Column("SCHEMA_NAME", Varchar(NAME_LEN), NOT_NULL),
  Column("PAGES", ULonglong(), NOT_NULL),
  Column("ALLOCATED_BYTES", ULonglong(), NOT_NULL),
  Column("DATA_BYTES", ULonglong(), NOT_NULL),
  CEnd()
};

} // namespace Show

static int bps_fill_table(THD *thd, TABLE_LIST *tables, Item *)
{
  DBUG_ENTER("i_s_innodb_buffer_page_stats_by_schema_fill");

  if (!srv_was_started)
    DBUG_RETURN(0);
  if (check_global_access(thd, PROCESS_ACL))
    DBUG_RETURN(0);
  if (!innodb_buffer_page_stats_by_schema_enabled)
  {
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_OPTION_PREVENTS_STATEMENT,
                 "innodb_buffer_page_stats_by_schema_enabled is OFF");
    DBUG_RETURN(0);
  }

  std::vector<bps_scratch_entry> scratch;
  std::unordered_map<index_id_t, bps_agg> by_index;
  try { bps_allocate_workspace(scratch, by_index); }
  catch (const std::bad_alloc &)
  {
    my_error(ER_OUTOFMEMORY, MYF(0), int(sizeof(bps_scratch_entry)));
    DBUG_RETURN(1);
  }

  bps_agg orphan= {};
  bps_scan_pool(scratch.data(), by_index, orphan);

  std::unordered_map<std::string, bps_agg> by_schema;
  bps_resolve_to_schemas(by_index, by_schema, orphan);
  if (orphan.pages)
    by_schema[BPS_ORPHAN_BUCKET].merge(orphan);

  DBUG_RETURN(bps_emit_rows(thd, tables->table, by_schema));
}

static int bps_plugin_init(void *p)
{
  DBUG_ENTER("i_s_innodb_buffer_page_stats_by_schema_init");
  ST_SCHEMA_TABLE *schema= reinterpret_cast<ST_SCHEMA_TABLE*>(p);
  schema->fields_info=
    Show::i_s_innodb_buffer_page_stats_by_schema_fields_info;
  schema->fill_table= bps_fill_table;
  DBUG_RETURN(0);
}

static int bps_plugin_deinit(void *)
{
  DBUG_ENTER("i_s_innodb_buffer_page_stats_by_schema_deinit");
  DBUG_RETURN(0);
}

static struct st_mysql_information_schema bps_i_s_info=
{
  MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION
};

struct st_maria_plugin i_s_innodb_buffer_page_stats_by_schema=
{
  MYSQL_INFORMATION_SCHEMA_PLUGIN,
  &bps_i_s_info,
  "INNODB_BUFFER_PAGE_STATS_BY_SCHEMA",
  maria_plugin_author,
  "InnoDB buffer pool memory usage, aggregated per schema",
  PLUGIN_LICENSE_GPL,
  bps_plugin_init,
  bps_plugin_deinit,
  INNODB_VERSION_SHORT,
  NULL, /* status variables */
  NULL, /* system variables */
  INNODB_VERSION_STR,
  MariaDB_PLUGIN_MATURITY_STABLE,
};
