/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * heap_file.c - heap file manager
 */

#ident "$Id$"

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "porting.h"
#include "heap_file.h"
#include "storage_common.h"
#include "memory_alloc.h"
#include "system_parameter.h"
#include "oid.h"
#include "error_manager.h"
#include "locator.h"
#include "file_io.h"
#include "page_buffer.h"
#include "file_manager.h"
#include "disk_manager.h"
#include "slotted_page.h"
#include "overflow_file.h"
#include "object_representation.h"
#include "object_representation_sr.h"
#include "log_manager.h"
#include "lock_manager.h"
#include "memory_hash.h"
#include "critical_section.h"
#include "boot_sr.h"
#include "locator_sr.h"
#include "btree.h"
#include "thread.h"		/* MAX_NTHRDS */
#include "transform.h"		/* for CT_SERIAL_NAME */
#include "serial.h"
#include "object_primitive.h"
#include "dbtype.h"
#include "db.h"
#include "object_print.h"
#include "xserver_interface.h"
#include "boot_sr.h"
#include "chartype.h"
#include "query_executor.h"
#include "fetch.h"
#include "server_interface.h"
#include "page_buffer.h"

/* For getting and dumping attributes */
#include "language_support.h"

/* For creating multi-column sequence keys (sets) */
#include "set_object.h"

#if defined (SERVER_MODE)
#include "connection_error.h"
#endif

#include "tsc_timer.h"

#if defined(ENABLE_SYSTEMTAP)
#include "probes.h"
#endif /* ENABLE_SYSTEMTAP */

#if !defined (SERVER_MODE)
#include "transaction_cl.h"
#endif

/* this must be the last header file included!!! */
#include "dbval.h"

#if !defined(SERVER_MODE)
#define pthread_mutex_init(a, b)
#define pthread_mutex_destroy(a)
#define pthread_mutex_lock(a)   0
#define pthread_mutex_trylock(a)   0
#define pthread_mutex_unlock(a)
static int rv;
#endif /* not SERVER_MODE */

#define HEAP_BESTSPACE_SYNC_THRESHOLD (0.1f)

/* ATTRIBUTE LOCATION */

#define OR_FIXED_ATTRIBUTES_OFFSET_BY_OBJ(obj, nvars) \
  (OR_HEADER_SIZE(obj) + OR_VAR_TABLE_SIZE_INTERNAL(nvars, OR_GET_OFFSET_SIZE(obj)))

#define HEAP_GUESS_NUM_ATTRS_REFOIDS 100
#define HEAP_GUESS_NUM_INDEXED_ATTRS 100

#define HEAP_CLASSREPR_MAXCACHE	100

#define HEAP_STATS_ENTRY_MHT_EST_SIZE 1000
#define HEAP_STATS_ENTRY_FREELIST_SIZE 1000

/* A good space to accept insertions */
#define HEAP_DROP_FREE_SPACE (int)(DB_PAGESIZE * 0.3)

#define HEAP_DEBUG_SCANCACHE_INITPATTERN (12345)

#if defined(CUBRID_DEBUG)
#define HEAP_DEBUG_ISVALID_SCANRANGE(scan_range) \
  heap_scanrange_isvalid(scan_range)
#else /* CUBRID_DEBUG */
#define HEAP_DEBUG_ISVALID_SCANRANGE(scan_range) (DISK_VALID)
#endif /* !CUBRID_DEBUG */

#define HEAP_IS_PAGE_OF_OID(pgptr, oid) \
  (((pgptr) != NULL) \
   && pgbuf_get_volume_id (pgptr) == (oid)->volid \
   && pgbuf_get_page_id (pgptr) == (oid)->pageid)

#define HEAP_SET_RELOCATE_DELETE_INFO(mvcc_relocate_delete, datasource_oid_p, \
				      next_version_p) \
  do \
    { \
      (mvcc_relocate_delete)->mvcc_delete_oid = (datasource_oid_p); \
      (mvcc_relocate_delete)->next_version = (next_version_p); \
    } \
  while (0)

#define MVCC_GET_DELETE_INFO(mvcc_delete_info_p, row_delid_p, \
			     next_row_version_p, satisfies_del_result_p) \
  do \
    { \
      assert ((mvcc_delete_info_p) != NULL); \
      if ((row_delid_p) != NULL) \
        { \
	  *(row_delid_p) = (mvcc_delete_info_p)->row_delid; \
        }	\
      if ((next_row_version_p) != NULL) \
        {	\
	  COPY_OID ((next_row_version_p), \
		    &((mvcc_delete_info_p)->next_row_version)); \
        } \
      if ((satisfies_del_result_p) != NULL) \
        { \
	  *(satisfies_del_result_p) = \
	    ((mvcc_delete_info_p)->satisfies_delete_result); \
        } \
    } \
  while (0)

#define MVCC_SET_DELETE_INFO(mvcc_delete_info_p, row_delete_id, \
			     next_row_version_p, satisfies_del_result) \
  do \
    { \
      assert (((mvcc_delete_info_p) != NULL) \
	      && ((next_row_version_p) != NULL)); \
      (mvcc_delete_info_p)->row_delid = (row_delete_id); \
      COPY_OID (&(mvcc_delete_info_p)->next_row_version, (next_row_version_p)); \
      (mvcc_delete_info_p)->satisfies_delete_result = (satisfies_del_result); \
    } \
  while (0)

#define HEAP_MVCC_SET_HEADER_MAXIMUM_SIZE(mvcc_rec_header_p) \
  do \
    { \
      if (!MVCC_IS_FLAG_SET (mvcc_rec_header_p, OR_MVCC_FLAG_VALID_INSID)) \
	{ \
	  MVCC_SET_FLAG_BITS (mvcc_rec_header_p, OR_MVCC_FLAG_VALID_INSID); \
	  MVCC_SET_INSID (mvcc_rec_header_p, MVCCID_ALL_VISIBLE); \
	} \
      if (!MVCC_IS_FLAG_SET (mvcc_rec_header_p, OR_MVCC_FLAG_VALID_DELID \
			     | OR_MVCC_FLAG_VALID_LONG_CHN)) \
	{ \
	  MVCC_SET_FLAG_BITS (mvcc_rec_header_p, OR_MVCC_FLAG_VALID_LONG_CHN); \
	} \
      if (!MVCC_IS_FLAG_SET (mvcc_rec_header_p, OR_MVCC_FLAG_VALID_NEXT_VERSION)) \
	{ \
	  MVCC_SET_FLAG_BITS (mvcc_rec_header_p, \
			      OR_MVCC_FLAG_VALID_NEXT_VERSION); \
	  MVCC_SET_NEXT_VERSION (mvcc_rec_header_p, &oid_Null_oid); \
	} \
      if (!MVCC_IS_FLAG_SET (mvcc_rec_header_p, OR_MVCC_FLAG_VALID_PARTITION_OID)) \
	{ \
	  MVCC_SET_FLAG_BITS (mvcc_rec_header_p, \
			      OR_MVCC_FLAG_VALID_PARTITION_OID); \
	  COPY_OID (&(mvcc_rec_header_p)->partition_oid, &oid_Null_oid); \
	} \
    } \
  while (0)

#define HEAP_SCAN_ORDERED_HFID(scan) \
  (((scan) != NULL) ? (&(scan)->node.hfid) : (PGBUF_ORDERED_NULL_HFID))

typedef enum
{
  HEAP_FINDSPACE_FOUND,
  HEAP_FINDSPACE_NOTFOUND,
  HEAP_FINDSPACE_ERROR
} HEAP_FINDSPACE;

/*
 * Prefetching directions
 */

typedef enum
{
  HEAP_DIRECTION_NONE,		/* No prefetching */
  HEAP_DIRECTION_LEFT,		/* Prefetching at the left */
  HEAP_DIRECTION_RIGHT,		/* Prefetching at the right */
  HEAP_DIRECTION_BOTH		/* Prefetching at both directions.. left and right */
} HEAP_DIRECTION;

/*
 * Heap file header
 */

#define HEAP_NUM_BEST_SPACESTATS   10

/* calculate an index of best array */
#define HEAP_STATS_NEXT_BEST_INDEX(i)   \
  (((i) + 1) % HEAP_NUM_BEST_SPACESTATS)
#define HEAP_STATS_PREV_BEST_INDEX(i)   \
  (((i) == 0) ? (HEAP_NUM_BEST_SPACESTATS - 1) : ((i) - 1));

typedef struct heap_hdr_stats HEAP_HDR_STATS;
struct heap_hdr_stats
{
  /* the first must be class_oid */
  OID class_oid;
  VFID ovf_vfid;		/* Overflow file identifier (if any) */
  VPID next_vpid;		/* Next page (i.e., the 2nd page of heap file) */
  int unfill_space;		/* Stop inserting when page has run below this. leave it for updates */
  struct
  {
    int num_pages;		/* Estimation of number of heap pages. Consult file manager if accurate number is
				 * needed */
    int num_recs;		/* Estimation of number of objects in heap */
    float recs_sumlen;		/* Estimation total length of records */
    int num_other_high_best;	/* Total of other believed known best pages, which are not included in the best array
				 * and we believe they have at least HEAP_DROP_FREE_SPACE */
    int num_high_best;		/* Number of pages in the best array that we believe have at least
				 * HEAP_DROP_FREE_SPACE. When this number goes to zero and there is at least other
				 * HEAP_NUM_BEST_SPACESTATS best pages, we look for them. */
    int num_substitutions;	/* Number of page substitutions. This will be used to insert a new second best page
				 * into second best hints. */
    int num_second_best;	/* Number of second best hints. The hints are in "second_best" array. They are used
				 * when finding new best pages. See the function "heap_stats_sync_bestspace". */
    int head_second_best;	/* Index of head of second best hints. */
    int tail_second_best;	/* Index of tail of second best hints. A new second best hint will be stored on this
				 * index. */
    int head;			/* Head of best circular array */
    VPID last_vpid;
    VPID full_search_vpid;
    VPID second_best[HEAP_NUM_BEST_SPACESTATS];
    HEAP_BESTSPACE best[HEAP_NUM_BEST_SPACESTATS];
  } estimates;			/* Probably, the set of pages with more free space on the heap. Changes to any values
				 * of this array (either page or the free space for the page) are not logged since
				 * these values are only used for hints. These values may not be accurate at any given
				 * time and the entries may contain duplicated pages. */

  int has_partition_link;	/* true if the class has at least one partition link */
  int reserve1_for_future;	/* Nothing reserved for future */
  int reserve2_for_future;	/* Nothing reserved for future */
};

typedef struct heap_stats_entry HEAP_STATS_ENTRY;
struct heap_stats_entry
{
  HFID hfid;			/* heap file identifier */
  HEAP_BESTSPACE best;		/* best space info */
  HEAP_STATS_ENTRY *next;
};

/* Define heap page flags. */
#define HEAP_PAGE_FLAG_VACUUM_STATUS_MASK	  0xC0000000
#define HEAP_PAGE_FLAG_VACUUM_ONCE		  0x80000000
#define HEAP_PAGE_FLAG_VACUUM_UNKNOWN		  0x40000000

#define HEAP_PAGE_SET_VACUUM_STATUS(chain, status) \
  do \
    { \
      assert ((status) == HEAP_PAGE_VACUUM_NONE \
	      || (status) == HEAP_PAGE_VACUUM_ONCE \
	      || (status) == HEAP_PAGE_VACUUM_UNKNOWN); \
      (chain)->flags &= ~HEAP_PAGE_FLAG_VACUUM_STATUS_MASK; \
      if ((status) == HEAP_PAGE_VACUUM_ONCE) \
        { \
	  (chain)->flags |= HEAP_PAGE_FLAG_VACUUM_ONCE; \
	} \
      else if ((status) == HEAP_PAGE_VACUUM_UNKNOWN) \
	{ \
	  (chain)->flags |= HEAP_PAGE_FLAG_VACUUM_UNKNOWN; \
	} \
    } \
  while (false)

#define HEAP_PAGE_GET_VACUUM_STATUS(chain) \
  (((chain)->flags & HEAP_PAGE_FLAG_VACUUM_STATUS_MASK) == 0 \
   ? HEAP_PAGE_VACUUM_NONE \
   : ((((chain)->flags & HEAP_PAGE_FLAG_VACUUM_STATUS_MASK) \
        == HEAP_PAGE_FLAG_VACUUM_ONCE) \
      ? HEAP_PAGE_VACUUM_ONCE : HEAP_PAGE_VACUUM_UNKNOWN))

typedef struct heap_chain HEAP_CHAIN;
struct heap_chain
{				/* Double-linked */
  /* the first must be class_oid */
  OID class_oid;
  VPID prev_vpid;		/* Previous page */
  VPID next_vpid;		/* Next page */
  MVCCID max_mvccid;		/* Max MVCCID of any MVCC operations in page. */
  INT32 flags;			/* Flags for heap page. 2 bits are used for vacuum state. */
};

typedef struct heap_chain_tolast HEAP_CHAIN_TOLAST;
struct heap_chain_tolast
{
  PAGE_PTR hdr_pgptr;
  PAGE_PTR last_pgptr;
  HEAP_HDR_STATS *heap_hdr;
};

#define HEAP_CHK_ADD_UNFOUND_RELOCOIDS 100

typedef struct heap_chk_relocoid HEAP_CHK_RELOCOID;
struct heap_chk_relocoid
{
  OID real_oid;
  OID reloc_oid;
};

typedef struct heap_chkall_relocoids HEAP_CHKALL_RELOCOIDS;
struct heap_chkall_relocoids
{
  MHT_TABLE *ht;		/* Hash table to be used to keep relocated records The key of hash table is the
				 * relocation OID, the date is the real OID */
  bool verify;
  bool verify_not_vacuumed;	/* if true then each record will be checked if it wasn't vacuumed although it must've
				 * be vacuumed */
  DISK_ISVALID not_vacuumed_res;	/* The validation result of the "not vacuumed" objects */
  int max_unfound_reloc;
  int num_unfound_reloc;
  OID *unfound_reloc_oids;	/* The relocation OIDs that have not been found in hash table */
};

#define DEFAULT_REPR_INCREMENT 16

enum
{ ZONE_VOID = 1, ZONE_FREE = 2, ZONE_LRU = 3 };

typedef struct heap_classrepr_entry HEAP_CLASSREPR_ENTRY;
struct heap_classrepr_entry
{
  pthread_mutex_t mutex;
  int idx;			/* Cache index. Used to pass the index when a class representation is in the cache */
  int fcnt;			/* How many times this structure has been fixed. It cannot be deallocated until this
				 * value is zero.  */
  int zone;			/* ZONE_VOID, ZONE_LRU, ZONE_FREE */
  int force_decache;

  THREAD_ENTRY *next_wait_thrd;
  HEAP_CLASSREPR_ENTRY *hash_next;
  HEAP_CLASSREPR_ENTRY *prev;	/* prev. entry in LRU list */
  HEAP_CLASSREPR_ENTRY *next;	/* prev. entry in LRU or free list */

  /* real data */
  OID class_oid;		/* Identifier of the class representation */

  OR_CLASSREP **repr;		/* A particular representation of the class */
  int max_reprid;
  REPR_ID last_reprid;
};

typedef struct heap_classrepr_lock HEAP_CLASSREPR_LOCK;
struct heap_classrepr_lock
{
  OID class_oid;
  HEAP_CLASSREPR_LOCK *lock_next;
  THREAD_ENTRY *next_wait_thrd;
};

typedef struct heap_classrepr_hash HEAP_CLASSREPR_HASH;
struct heap_classrepr_hash
{
  pthread_mutex_t hash_mutex;
  int idx;
  HEAP_CLASSREPR_ENTRY *hash_next;
  HEAP_CLASSREPR_LOCK *lock_next;
};

typedef struct heap_classrepr_LRU_list HEAP_CLASSREPR_LRU_LIST;
struct heap_classrepr_LRU_list
{
  pthread_mutex_t LRU_mutex;
  HEAP_CLASSREPR_ENTRY *LRU_top;
  HEAP_CLASSREPR_ENTRY *LRU_bottom;
};

typedef struct heap_classrepr_free_list HEAP_CLASSREPR_FREE_LIST;
struct heap_classrepr_free_list
{
  pthread_mutex_t free_mutex;
  HEAP_CLASSREPR_ENTRY *free_top;
  int free_cnt;
};

typedef struct heap_classrepr_cache HEAP_CLASSREPR_CACHE;
struct heap_classrepr_cache
{
  int num_entries;
  HEAP_CLASSREPR_ENTRY *area;
  int num_hash;
  HEAP_CLASSREPR_HASH *hash_table;
  HEAP_CLASSREPR_LOCK *lock_table;
  HEAP_CLASSREPR_LRU_LIST LRU_list;
  HEAP_CLASSREPR_FREE_LIST free_list;
  HFID rootclass_hfid;
#ifdef DEBUG_CLASSREPR_CACHE
  int num_fix_entries;
  pthread_mutex_t num_fix_entries_mutex;
#endif				/* DEBUG_CLASSREPR_CACHE */
};

static HEAP_CLASSREPR_CACHE heap_Classrepr_cache = {
  -1,
  NULL,
  -1,
  NULL,
  NULL,
  {
   PTHREAD_MUTEX_INITIALIZER,
   NULL,
   NULL},
  {
   PTHREAD_MUTEX_INITIALIZER,
   NULL,
   -1},
  {{NULL_FILEID, NULL_VOLID}, NULL_PAGEID}	/* rootclass_hfid */
#ifdef DEBUG_CLASSREPR_CACHE
  , 0, PTHREAD_MUTEX_INITIALIZER
#endif /* DEBUG_CLASSREPR_CACHE */
};

#define CLASSREPR_REPR_INCREMENT	10
#define CLASSREPR_HASH_SIZE  (heap_Classrepr_cache.num_entries * 2)
#define REPR_HASH(class_oid) (OID_PSEUDO_KEY(class_oid)%CLASSREPR_HASH_SIZE)

#define HEAP_MAYNEED_DECACHE_GUESSED_LASTREPRS(class_oid, hfid) \
  do \
    { \
      if (heap_Classrepr != NULL && (hfid) != NULL) \
	{ \
	  if (HFID_IS_NULL (&(heap_Classrepr->rootclass_hfid))) \
	    (void) boot_find_root_heap (&(heap_Classrepr->rootclass_hfid)); \
	  if (HFID_EQ ((hfid), &(heap_Classrepr->rootclass_hfid))) \
	    (void) heap_classrepr_decache_guessed_last (class_oid); \
	} \
    } \
  while (0)

#define HEAP_CHNGUESS_FUDGE_MININDICES (100)
#define HEAP_NBITS_IN_BYTE	     (8)
#define HEAP_NSHIFTS                   (3)	/* For multiplication/division by 8 */
#define HEAP_BITMASK                   (HEAP_NBITS_IN_BYTE - 1)
#define HEAP_NBITS_TO_NBYTES(bit_cnt)  \
  ((unsigned int)((bit_cnt) + HEAP_BITMASK) >> HEAP_NSHIFTS)
#define HEAP_NBYTES_TO_NBITS(byte_cnt) ((unsigned int)(byte_cnt) << HEAP_NSHIFTS)
#define HEAP_NBYTES_CLEARED(byte_ptr, byte_cnt) \
  memset((byte_ptr), '\0', (byte_cnt))
#define HEAP_BYTEOFFSET_OFBIT(bit_num) ((unsigned int)(bit_num) >> HEAP_NSHIFTS)
#define HEAP_BYTEGET(byte_ptr, bit_num) \
  ((unsigned char *)(byte_ptr) + HEAP_BYTEOFFSET_OFBIT(bit_num))

#define HEAP_BITMASK_INBYTE(bit_num)   \
  (1 << ((unsigned int)(bit_num) & HEAP_BITMASK))
#define HEAP_BIT_GET(byte_ptr, bit_num) \
  (*HEAP_BYTEGET(byte_ptr, bit_num) & HEAP_BITMASK_INBYTE(bit_num))
#define HEAP_BIT_SET(byte_ptr, bit_num) \
  (*HEAP_BYTEGET(byte_ptr, bit_num) = \
   *HEAP_BYTEGET(byte_ptr, bit_num) | HEAP_BITMASK_INBYTE(bit_num))
#define HEAP_BIT_CLEAR(byte_ptr, bit_num) \
  (*HEAP_BYTEGET(byte_ptr, bit_num) = \
   *HEAP_BYTEGET(byte_ptr, bit_num) & ~HEAP_BITMASK_INBYTE(bit_num))

typedef struct heap_chnguess_entry HEAP_CHNGUESS_ENTRY;
struct heap_chnguess_entry
{				/* Currently, only classes are cached */
  int idx;			/* Index number of this entry */
  int chn;			/* Cache coherence number of object */
  bool recently_accessed;	/* Reference value 0/1 used by replacement clock algorithm */
  OID oid;			/* Identifier of object */
  unsigned char *bits;		/* Bit index array describing client transaction indices. Bit n corresponds to client
				 * tran index n If Bit is ON, we guess that the object is cached in the workspace of
				 * the client. */
};

typedef struct heap_chnguess HEAP_CHNGUESS;
struct heap_chnguess
{
  MHT_TABLE *ht;		/* Hash table for guessing chn */
  HEAP_CHNGUESS_ENTRY *entries;	/* Pointers to entry structures. More than one entry */
  unsigned char *bitindex;	/* Bit index array for each entry. Describe all entries. Each entry is subdivided into
				 * nbytes. */
  bool schema_change;		/* Has the schema been changed */
  int clock_hand;		/* Clock hand for replacement */
  int num_entries;		/* Number of guesschn entries */
  int num_clients;		/* Number of clients in bitindex for each entry */
  int nbytes;			/* Number of bytes in bitindex. It must be aligned to multiples of 4 bytes (integers) */
};

typedef struct heap_stats_bestspace_cache HEAP_STATS_BESTSPACE_CACHE;
struct heap_stats_bestspace_cache
{
  int num_stats_entries;	/* number of cache entries in use */
  MHT_TABLE *hfid_ht;		/* HFID Hash table for best space */
  MHT_TABLE *vpid_ht;		/* VPID Hash table for best space */
  int num_alloc;
  int num_free;
  int free_list_count;		/* number of entries in free */
  HEAP_STATS_ENTRY *free_list;
  pthread_mutex_t bestspace_mutex;
};

typedef struct heap_show_scan_ctx HEAP_SHOW_SCAN_CTX;
struct heap_show_scan_ctx
{
  HFID *hfids;			/* Array of class HFID */
  int hfids_count;		/* Count of above hfids array */
};

static int heap_Maxslotted_reclength;
static int heap_Slotted_overhead = 4;	/* sizeof (SPAGE_SLOT) */
static const int heap_Find_best_page_limit = 100;

static HEAP_CLASSREPR_CACHE *heap_Classrepr = NULL;
static HEAP_CHNGUESS heap_Guesschn_area = { NULL, NULL, NULL, false, 0,
  0, 0, 0
};

static HEAP_CHNGUESS *heap_Guesschn = NULL;

static HEAP_STATS_BESTSPACE_CACHE heap_Bestspace_cache_area =
  { 0, NULL, NULL, 0, 0, 0, NULL, PTHREAD_MUTEX_INITIALIZER };

static HEAP_STATS_BESTSPACE_CACHE *heap_Bestspace = NULL;

static HEAP_PARTITION_LINK_CACHE heap_Partition_link_cache_area =
  { LF_HASH_TABLE_INITIALIZER, LF_ENTRY_DESCRIPTOR_INITIALIZER,
  LF_FREELIST_INITIALIZER
};
static HEAP_PARTITION_LINK_CACHE *heap_Partition_link_cache = NULL;

static HEAP_HFID_TABLE heap_Hfid_table_area = { LF_HASH_TABLE_INITIALIZER, LF_ENTRY_DESCRIPTOR_INITIALIZER,
  LF_FREELIST_INITIALIZER
};

static HEAP_HFID_TABLE *heap_Hfid_table = NULL;

/* Recovery. */
#define HEAP_RV_FLAG_VACUUM_STATUS_CHANGE 0x8000

#define HEAP_PERF_START(thread_p, context) \
  PERF_UTIME_TRACKER_START (thread_p, (context)->time_track)
#define HEAP_PERF_TRACK_PREPARE(thread_p, context) \
  do \
    { \
      if ((context)->time_track == NULL) break; \
      switch ((context)->type) { \
      case HEAP_OPERATION_INSERT: \
	PERF_UTIME_TRACKER_TIME_AND_RESTART (thread_p, \
					     (context)->time_track,\
					     mnt_heap_insert_prepare_time); \
	break; \
      case HEAP_OPERATION_DELETE: \
	PERF_UTIME_TRACKER_TIME_AND_RESTART (thread_p, \
					     (context)->time_track,\
					     mnt_heap_delete_prepare_time); \
	break; \
      case HEAP_OPERATION_UPDATE: \
	PERF_UTIME_TRACKER_TIME_AND_RESTART (thread_p, \
					     (context)->time_track,\
					     mnt_heap_update_prepare_time); \
	break; \
      default: \
	assert (false); \
      } \
    } \
  while (false)
#define HEAP_PERF_TRACK_EXECUTE(thread_p, context) \
  do \
    { \
      if ((context)->time_track == NULL) break; \
      switch ((context)->type) { \
      case HEAP_OPERATION_INSERT: \
	PERF_UTIME_TRACKER_TIME_AND_RESTART (thread_p, \
					     (context)->time_track,\
					     mnt_heap_insert_execute_time); \
	break; \
      case HEAP_OPERATION_DELETE: \
	PERF_UTIME_TRACKER_TIME_AND_RESTART (thread_p, \
					     (context)->time_track,\
					     mnt_heap_delete_execute_time); \
	break; \
      case HEAP_OPERATION_UPDATE: \
	PERF_UTIME_TRACKER_TIME_AND_RESTART (thread_p, \
					     (context)->time_track,\
					     mnt_heap_update_execute_time); \
	break; \
      default: \
	assert (false); \
      } \
    } \
  while (false)
#define HEAP_PERF_TRACK_LOGGING(thread_p, context) \
  do \
    { \
      if ((context)->time_track == NULL) break; \
      switch ((context)->type) { \
      case HEAP_OPERATION_INSERT: \
	PERF_UTIME_TRACKER_TIME_AND_RESTART (thread_p, \
					     (context)->time_track,\
					     mnt_heap_insert_log_time); \
	break; \
      case HEAP_OPERATION_DELETE: \
	PERF_UTIME_TRACKER_TIME_AND_RESTART (thread_p, \
					     (context)->time_track,\
					     mnt_heap_delete_log_time); \
	break; \
      case HEAP_OPERATION_UPDATE: \
	PERF_UTIME_TRACKER_TIME_AND_RESTART (thread_p, \
					     (context)->time_track,\
					     mnt_heap_update_log_time); \
	break; \
      default: \
	assert (false); \
      } \
    } \
  while (false)

#if defined (NDEBUG)
static PAGE_PTR heap_scan_pb_lock_and_fetch (THREAD_ENTRY * thread_p, const VPID * vpid_ptr, PAGE_FETCH_MODE fetch_mode,
					     LOCK lock, HEAP_SCANCACHE * scan_cache, PGBUF_WATCHER * pg_watcher);
#else /* !NDEBUG */
#define heap_scan_pb_lock_and_fetch(...) \
  heap_scan_pb_lock_and_fetch_debug (__VA_ARGS__, ARG_FILE_LINE)

static PAGE_PTR heap_scan_pb_lock_and_fetch_debug (THREAD_ENTRY * thread_p, const VPID * vpid_ptr,
						   PAGE_FETCH_MODE fetch_mode, LOCK lock, HEAP_SCANCACHE * scan_cache,
						   PGBUF_WATCHER * pg_watcher, const char *caller_file,
						   const int caller_line);
#endif /* !NDEBUG */


static int heap_classrepr_initialize_cache (void);
static int heap_classrepr_finalize_cache (void);
static int heap_classrepr_decache_guessed_last (const OID * class_oid);
#ifdef SERVER_MODE
static int heap_classrepr_lock_class (THREAD_ENTRY * thread_p, HEAP_CLASSREPR_HASH * hash_anchor, OID * class_oid);
static int heap_classrepr_unlock_class (HEAP_CLASSREPR_HASH * hash_anchor, OID * class_oid, int need_hash_mutex);
#endif

static int heap_classrepr_dump (THREAD_ENTRY * thread_p, FILE * fp, const OID * class_oid, const OR_CLASSREP * repr);
#ifdef DEBUG_CLASSREPR_CACHE
static int heap_classrepr_dump_cache (bool simple_dump);
#endif /* DEBUG_CLASSREPR_CACHE */

static int heap_classrepr_entry_reset (HEAP_CLASSREPR_ENTRY * cache_entry);
static int heap_classrepr_entry_remove_from_LRU (HEAP_CLASSREPR_ENTRY * cache_entry);
static HEAP_CLASSREPR_ENTRY *heap_classrepr_entry_alloc (void);
static int heap_classrepr_entry_free (HEAP_CLASSREPR_ENTRY * cache_entry);

static OR_CLASSREP *heap_classrepr_get_from_record (THREAD_ENTRY * thread_p, REPR_ID * last_reprid, OID * class_oid,
						    RECDES * class_recdes, REPR_ID reprid);
static int heap_stats_get_min_freespace (HEAP_HDR_STATS * heap_hdr);
static int heap_stats_update_internal (THREAD_ENTRY * thread_p, const HFID * hfid, VPID * lotspace_vpid,
				       int free_space);
static void heap_stats_put_second_best (HEAP_HDR_STATS * heap_hdr, VPID * vpid);
static int heap_stats_get_second_best (HEAP_HDR_STATS * heap_hdr, VPID * vpid);
#if defined(ENABLE_UNUSED_FUNCTION)
static int heap_stats_quick_num_fit_in_bestspace (HEAP_BESTSPACE * bestspace, int num_entries, int unit_size,
						  int unfill_space);
#endif
static HEAP_FINDSPACE heap_stats_find_page_in_bestspace (THREAD_ENTRY * thread_p, const HFID * hfid,
							 HEAP_BESTSPACE * bestspace, int *idx_badspace,
							 int record_length, int needed_space,
							 HEAP_SCANCACHE * scan_cache, PGBUF_WATCHER * pg_watcher);
static PAGE_PTR heap_stats_find_best_page (THREAD_ENTRY * thread_p, const HFID * hfid, int needed_space, bool isnew_rec,
					   int newrec_size, HEAP_SCANCACHE * space_cache, PGBUF_WATCHER * pg_watcher);
static int heap_stats_sync_bestspace (THREAD_ENTRY * thread_p, const HFID * hfid, HEAP_HDR_STATS * heap_hdr,
				      VPID * hdr_vpid, bool scan_all, bool can_cycle);

static PAGE_PTR heap_get_last_page (THREAD_ENTRY * thread_p, const HFID * hfid, HEAP_HDR_STATS * heap_hdr,
				    HEAP_SCANCACHE * scan_cache, VPID * last_vpid, PGBUF_WATCHER * pg_watcher);
static bool heap_link_to_new (THREAD_ENTRY * thread_p, const VFID * vfid, const VPID * new_vpid,
			      HEAP_CHAIN_TOLAST * link);

static bool heap_vpid_init_new (THREAD_ENTRY * thread_p, const VFID * vfid, const FILE_TYPE file_type,
				const VPID * vpid, INT32 ignore_npages, void *xchain);
static PAGE_PTR heap_vpid_alloc (THREAD_ENTRY * thread_p, const HFID * hfid, PAGE_PTR hdr_pgptr,
				 HEAP_HDR_STATS * heap_hdr, int needed_space, HEAP_SCANCACHE * scan_cache,
				 PGBUF_WATCHER * new_pg_watcher);
static VPID *heap_vpid_remove (THREAD_ENTRY * thread_p, const HFID * hfid, HEAP_HDR_STATS * heap_hdr, VPID * rm_vpid);

static HFID *heap_create_internal (THREAD_ENTRY * thread_p, HFID * hfid, int exp_npgs, const OID * class_oid,
				   const bool reuse_oid);
static const HFID *heap_reuse (THREAD_ENTRY * thread_p, const HFID * hfid, const OID * class_oid, const bool reuse_oid);
static bool heap_delete_all_page_records (THREAD_ENTRY * thread_p, const VPID * vpid, PAGE_PTR pgptr);
static int heap_reinitialize_page (THREAD_ENTRY * thread_p, PAGE_PTR pgptr, const bool is_header_page);
#if defined(CUBRID_DEBUG)
static DISK_ISVALID heap_hfid_isvalid (HFID * hfid);
static DISK_ISVALID heap_scanrange_isvalid (HEAP_SCANRANGE * scan_range);
#endif /* CUBRID_DEBUG */
static OID *heap_ovf_insert (THREAD_ENTRY * thread_p, const HFID * hfid, OID * ovf_oid, RECDES * recdes,
			     MVCC_RELOCATE_DELETE_INFO * mvcc_relocate_delete);
static const OID *heap_ovf_update (THREAD_ENTRY * thread_p, const HFID * hfid, const OID * ovf_oid, RECDES * recdes);
static int heap_ovf_flush (THREAD_ENTRY * thread_p, const OID * ovf_oid);
static int heap_ovf_get_length (THREAD_ENTRY * thread_p, const OID * ovf_oid);
static SCAN_CODE heap_ovf_get (THREAD_ENTRY * thread_p, const OID * ovf_oid, RECDES * recdes, int chn,
			       MVCC_SNAPSHOT * mvcc_snapshot);
static int heap_ovf_get_capacity (THREAD_ENTRY * thread_p, const OID * ovf_oid, int *ovf_len, int *ovf_num_pages,
				  int *ovf_overhead, int *ovf_free_space);

static int heap_scancache_check_with_hfid (THREAD_ENTRY * thread_p, HFID * hfid, OID * class_oid,
					   HEAP_SCANCACHE ** scan_cache);
static int heap_scancache_start_internal (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, const HFID * hfid,
					  const OID * class_oid, int cache_last_fix_page, int is_queryscan,
					  int is_indexscan, MVCC_SNAPSHOT * mvcc_snapshot);
static int heap_scancache_force_modify (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache);
static int heap_scancache_reset_modify (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, const HFID * hfid,
					const OID * class_oid);
static int heap_scancache_quick_start_internal (HEAP_SCANCACHE * scan_cache, const HFID * hfid);
static int heap_scancache_quick_end (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache);
static int heap_scancache_end_internal (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, bool scan_state);
static SCAN_CODE heap_get_if_diff_chn (THREAD_ENTRY * thread_p, PAGE_PTR pgptr, INT16 slotid, RECDES * recdes,
				       int ispeeking, int chn, MVCC_SNAPSHOT * mvcc_snapshot);
static SCAN_CODE heap_get_internal (THREAD_ENTRY * thread_p, OID * class_oid, const OID * oid, RECDES * recdes,
				    HEAP_SCANCACHE * scan_cache, int ispeeking, int chn);
static int heap_estimate_avg_length (THREAD_ENTRY * thread_p, const HFID * hfid);
static int heap_get_capacity (THREAD_ENTRY * thread_p, const HFID * hfid, INT64 * num_recs, INT64 * num_recs_relocated,
			      INT64 * num_recs_inovf, INT64 * num_pages, int *avg_freespace, int *avg_freespace_nolast,
			      int *avg_reclength, int *avg_overhead);
#if 0				/* TODO: remove unused */
static int heap_moreattr_attrinfo (int attrid, HEAP_CACHE_ATTRINFO * attr_info);
#endif

static int heap_attrinfo_recache_attrepr (HEAP_CACHE_ATTRINFO * attr_info, int islast_reset);
static int heap_attrinfo_recache (THREAD_ENTRY * thread_p, REPR_ID reprid, HEAP_CACHE_ATTRINFO * attr_info);
static int heap_attrinfo_check (const OID * inst_oid, HEAP_CACHE_ATTRINFO * attr_info);
static int heap_attrinfo_set_uninitialized (THREAD_ENTRY * thread_p, OID * inst_oid, RECDES * recdes,
					    HEAP_CACHE_ATTRINFO * attr_info);
static int heap_attrinfo_start_refoids (THREAD_ENTRY * thread_p, OID * class_oid, HEAP_CACHE_ATTRINFO * attr_info);
static int heap_attrinfo_get_disksize (HEAP_CACHE_ATTRINFO * attr_info, int *offset_size_ptr);

static int heap_attrvalue_read (RECDES * recdes, HEAP_ATTRVALUE * value, HEAP_CACHE_ATTRINFO * attr_info);

static int heap_midxkey_get_value (RECDES * recdes, OR_ATTRIBUTE * att, DB_VALUE * value,
				   HEAP_CACHE_ATTRINFO * attr_info);
static OR_ATTRIBUTE *heap_locate_attribute (ATTR_ID attrid, HEAP_CACHE_ATTRINFO * attr_info);

static DB_MIDXKEY *heap_midxkey_key_get (RECDES * recdes, DB_MIDXKEY * midxkey, OR_INDEX * index,
					 HEAP_CACHE_ATTRINFO * attrinfo, DB_VALUE * func_res, TP_DOMAIN * func_domain,
					 TP_DOMAIN ** key_domain);
static DB_MIDXKEY *heap_midxkey_key_generate (THREAD_ENTRY * thread_p, RECDES * recdes, DB_MIDXKEY * midxkey,
					      int *att_ids, HEAP_CACHE_ATTRINFO * attrinfo, DB_VALUE * func_res,
					      int func_col_id, int func_attr_index_start);

static int heap_dump_hdr (FILE * fp, HEAP_HDR_STATS * heap_hdr);

static int heap_eval_function_index (THREAD_ENTRY * thread_p, FUNCTION_INDEX_INFO * func_index_info, int n_atts,
				     int *att_ids, HEAP_CACHE_ATTRINFO * attr_info, RECDES * recdes, int btid_index,
				     DB_VALUE * result, FUNC_PRED_UNPACK_INFO * func_pred, TP_DOMAIN ** fi_domain);

static DISK_ISVALID heap_check_all_pages_by_heapchain (THREAD_ENTRY * thread_p, HFID * hfid,
						       HEAP_CHKALL_RELOCOIDS * chk_objs, INT32 * num_checked);

static DISK_ISVALID heap_check_all_pages_by_allocset (THREAD_ENTRY * thread_p, HFID * hfid,
						      HEAP_CHKALL_RELOCOIDS * chk_objs, INT32 * num_checked);

static DISK_ISVALID heap_chkreloc_start (HEAP_CHKALL_RELOCOIDS * chk);
static DISK_ISVALID heap_chkreloc_end (HEAP_CHKALL_RELOCOIDS * chk);
static int heap_chkreloc_print_notfound (const void *ignore_reloc_oid, void *ent, void *xchk);
static DISK_ISVALID heap_chkreloc_next (THREAD_ENTRY * thread_p, HEAP_CHKALL_RELOCOIDS * chk, PAGE_PTR pgptr);

static int heap_chnguess_initialize (void);
static int heap_chnguess_realloc (void);
static int heap_chnguess_finalize (void);
static int heap_chnguess_decache (const OID * oid);
static int heap_chnguess_remove_entry (const void *oid_key, void *ent, void *xignore);

static int heap_stats_bestspace_initialize (void);
static int heap_stats_bestspace_finalize (void);

static int heap_get_spage_type (void);
static bool heap_is_reusable_oid (const FILE_TYPE file_type);
static int heap_rv_redo_newpage_internal (THREAD_ENTRY * thread_p, LOG_RCV * rcv, const bool reuse_oid);

static SCAN_CODE heap_attrinfo_transform_to_disk_internal (THREAD_ENTRY * thread_p, HEAP_CACHE_ATTRINFO * attr_info,
							   RECDES * old_recdes, RECDES * new_recdes,
							   int lob_create_flag);
static int heap_stats_del_bestspace_by_vpid (THREAD_ENTRY * thread_p, VPID * vpid);
static int heap_stats_del_bestspace_by_hfid (THREAD_ENTRY * thread_p, const HFID * hfid);
static HEAP_BESTSPACE heap_stats_get_bestspace_by_vpid (THREAD_ENTRY * thread_p, VPID * vpid);
static HEAP_STATS_ENTRY *heap_stats_add_bestspace (THREAD_ENTRY * thread_p, const HFID * hfid, VPID * vpid,
						   int freespace);
static int heap_stats_entry_free (THREAD_ENTRY * thread_p, void *data, void *args);
static int heap_get_partitions_from_subclasses (THREAD_ENTRY * thread_p, const OID * subclasses, int *parts_count,
						OR_PARTITION * partitions);
static int heap_class_get_partition_info (THREAD_ENTRY * thread_p, const OID * class_oid, OR_PARTITION * partition_info,
					  HFID * class_hfid, REPR_ID * repr_id, int *has_partition_info);
static int heap_get_partition_attributes (THREAD_ENTRY * thread_p, const OID * cls_oid, ATTR_ID * type_id,
					  ATTR_ID * values_id);
static int heap_get_class_subclasses (THREAD_ENTRY * thread_p, const OID * class_oid, int *count, OID ** subclasses);

static void heap_stats_dump_bestspace_information (FILE * out_fp);
static int heap_stats_print_hash_entry (FILE * outfp, const void *key, void *ent, void *ignore);
static unsigned int heap_hash_vpid (const void *key_vpid, unsigned int htsize);
static int heap_compare_vpid (const void *key_vpid1, const void *key_vpid2);
static unsigned int heap_hash_hfid (const void *key_hfid, unsigned int htsize);
static int heap_compare_hfid (const void *key_hfid1, const void *key_hfid2);

static char *heap_bestspace_to_string (char *buf, int buf_size, const HEAP_BESTSPACE * hb);

static int fill_string_to_buffer (char **start, char *end, const char *str);

static int heap_scan_get_record (THREAD_ENTRY * thread_p, const OID oid, OID * class_oid, RECDES * recdes,
				 RECDES forward_recdes, PGBUF_WATCHER * home_pg_watcher, HEAP_SCANCACHE * scan_cache,
				 int ispeeking, bool check_snapshot);
static int heap_get_record_info (THREAD_ENTRY * thread_p, const OID oid, RECDES * recdes, RECDES forward_recdes,
				 PGBUF_WATCHER * page_watcher, HEAP_SCANCACHE * scan_cache, int ispeeking,
				 DB_VALUE ** record_info);
static SCAN_CODE heap_next_internal (THREAD_ENTRY * thread_p, const HFID * hfid, OID * class_oid, OID * next_oid,
				     RECDES * recdes, HEAP_SCANCACHE * scan_cache, int ispeeking,
				     bool reversed_direction, DB_VALUE ** cache_recordinfo);

static SCAN_CODE heap_get_page_info (THREAD_ENTRY * thread_p, const OID * cls_oid, const HFID * hfid, const VPID * vpid,
				     const PAGE_PTR pgptr, DB_VALUE ** page_info);
static int heap_scancache_start_chain_update (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * new_scan_cache,
					      HEAP_SCANCACHE * old_scan_cache, OID * next_row_version);
static int heap_mvcc_check_and_lock_for_delete (THREAD_ENTRY * thread_p, OID * oid, OID * class_oid, LOCK lock,
						bool vacuum_allowed, MVCC_REC_HEADER * recdes_header,
						PGBUF_WATCHER * home_page_watcher, PGBUF_WATCHER * fwd_page_watcher,
						OID * forward_oid, INT16 * record_type,
						HEAP_MVCC_DELETE_INFO * mvcc_delete_info);
static SCAN_CODE heap_mvcc_lock_and_get_object_version (THREAD_ENTRY * thread_p, const OID * oid, OID * class_oid,
							RECDES * recdes, HEAP_SCANCACHE * scan_cache,
							SCAN_OPERATION_TYPE op_type, int ispeeking, int old_chn,
							MVCC_REEV_DATA * mvcc_reev_data, OID * updated_oid,
							NON_EXISTENT_HANDLING non_ex_handling_type);
static SCAN_CODE heap_get_record_data_when_all_ready (THREAD_ENTRY * thread_p, OID * oid, OID * forward_oid,
						      PAGE_PTR home_page, PAGE_PTR forward_page, INT16 record_type,
						      RECDES * recdes, HEAP_SCANCACHE * scan_cache, int ispeeking);
static DB_LOGICAL mvcc_reevaluate_filters (THREAD_ENTRY * thread_p, MVCC_SCAN_REEV_DATA * mvcc_reev_data, OID * oid,
					   RECDES * recdes);
static SCAN_CODE heap_get_bigone_content (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, int ispeeking,
					  OID * forward_oid, RECDES * recdes);
static DB_LOGICAL heap_mvcc_reeval_scan_filters (THREAD_ENTRY * thread_p, OID * oid, HEAP_SCANCACHE * scan_cache,
						 RECDES * recdes, UPDDEL_MVCC_COND_REEVAL * mvcc_cond_reeval,
						 bool is_upddel);
static DB_LOGICAL heap_mvcc_reev_cond_assigns (THREAD_ENTRY * thread_p, OID * class_oid, OID * oid,
					       HEAP_SCANCACHE * scan_cache, RECDES * recdes,
					       MVCC_UPDDEL_REEV_DATA * mvcc_reev_data);

static void heap_mvcc_log_insert (THREAD_ENTRY * thread_p, RECDES * p_recdes, LOG_DATA_ADDR * p_addr);
static void heap_mvcc_log_delete (THREAD_ENTRY * thread_p, INT32 undo_chn, OID * p_redo_next_version,
				  OID * partition_oid, LOG_DATA_ADDR * p_addr, LOG_RCVINDEX rcvindex);
static int heap_rv_mvcc_redo_delete_internal (THREAD_ENTRY * thread_p, PAGE_PTR page, PGSLOTID slotid, MVCCID mvccid,
					      OID * next_version, OID * partition_oid);
static void heap_mvcc_log_home_change_on_delete (THREAD_ENTRY * thread_p, RECDES * old_recdes, RECDES * new_recdes,
						 LOG_DATA_ADDR * p_addr);
static void heap_mvcc_log_home_no_change_on_delete (THREAD_ENTRY * thread_p, LOG_DATA_ADDR * p_addr);
static int heap_mvcc_lock_scan_and_set_scancache_node (THREAD_ENTRY * thread_p, OID * partition_oid_p,
						       HEAP_SCANCACHE * scan_cache, PGBUF_WATCHER * home_page_watcher_p,
						       PGBUF_WATCHER * fwd_page_watcher_p, bool * unfixed_watchers);
static SCAN_CODE heap_mvcc_cleanup_partition_link (THREAD_ENTRY * thread_p, HFID * hfid, const OID * oid,
						   OID * class_oid, PGBUF_WATCHER * home_page_watcher, OID * oid_list,
						   int nr_oids);
static SCAN_CODE heap_mvcc_find_next_valid_version (THREAD_ENTRY * thread_p, const OID * oid, OID * class_oid,
						    OID * oid_list, int no_oids, OID * next_version,
						    OID * next_version_class_oid);
static void heap_mvcc_log_remove_partition_link (THREAD_ENTRY * thread_p, OID * next_version, OID * partition_oid,
						 OID * new_next_version, OID * new_partition_oid, int is_bigone,
						 LOG_DATA_ADDR * p_addr);

static DB_LOGICAL heap_mvcc_reev_cond_and_assignment (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache,
						      MVCC_REEV_DATA * mvcc_reev_data, MVCC_REC_HEADER * mvcc_header,
						      OID * p_curr_row_version, RECDES * recdes);
static SCAN_CODE heap_get_from_page_no_snapshot (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache,
						 MVCC_REEV_DATA * mvcc_reev_data_p, MVCC_REC_HEADER * mvcc_header_p,
						 OID * curr_row_version_oid_p, RECDES * recdes, int ispeeking);
static SCAN_CODE heap_get_mvcc_record_header (THREAD_ENTRY * thread_p, MVCC_REC_HEADER * mvcc_header_p,
					      MVCC_SNAPSHOT * mvcc_snapshot_p, PAGE_PTR pgptr, PGSLOTID slotid);
static SCAN_CODE heap_ovf_get_mvcc_record_header (THREAD_ENTRY * thread_p, MVCC_REC_HEADER * mvcc_header_p,
						  MVCC_SNAPSHOT * mvcc_snapshot_p, PAGE_PTR ovf_first_pgptr);

static void heap_mvcc_log_redistribute (THREAD_ENTRY * thread_p, RECDES * p_recdes, LOG_DATA_ADDR * p_addr);

#if defined(ENABLE_UNUSED_FUNCTION)
static INLINE int heap_try_fetch_header_page (THREAD_ENTRY * thread_p, PAGE_PTR * home_pgptr_p,
					      const VPID * home_vpid_p, const OID * oid_p, PAGE_PTR * hdr_pgptr_p,
					      const VPID * hdr_vpid_p, HEAP_SCANCACHE * scan_cache, int *again_count,
					      int again_max) __attribute__ ((ALWAYS_INLINE));
static INLINE int heap_try_fetch_forward_page (THREAD_ENTRY * thread_p, PAGE_PTR * home_pgptr_p,
					       const VPID * home_vpid_p, const OID * oid_p, PAGE_PTR * fwd_pgptr_p,
					       const VPID * fwd_vpid_p, const OID * fwd_oid_p,
					       HEAP_SCANCACHE * scan_cache, int *again_count, int again_max)
  __attribute__ ((ALWAYS_INLINE));
static INLINE int heap_try_fetch_header_with_forward_page (THREAD_ENTRY * thread_p, PAGE_PTR * home_pgptr_p,
							   const VPID * home_vpid_p, const OID * oid_p,
							   PAGE_PTR * hdr_pgptr_p, const VPID * hdr_vpid_p,
							   PAGE_PTR * fwd_pgptr_p, const VPID * fwd_vpid_p,
							   const OID * fwd_oid_p, HEAP_SCANCACHE * scan_cache,
							   int *again_count, int again_max)
  __attribute__ ((ALWAYS_INLINE));
#endif /* ENABLE_UNUSED_FUNCTION */

/* common */
static void heap_link_watchers (HEAP_OPERATION_CONTEXT * child, HEAP_OPERATION_CONTEXT * parent);
static void heap_unfix_watchers (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context);
static void heap_clear_operation_context (HEAP_OPERATION_CONTEXT * context, HFID * hfid_p);
static int heap_mark_class_as_modified (THREAD_ENTRY * thread_p, OID * oid_p, int chn, bool decache);
static FILE_TYPE heap_get_file_type (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context);
static int heap_is_valid_oid (OID * oid);
static int heap_fix_header_page (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context);
static int heap_fix_forward_page (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, OID * forward_oid_hint);
static void heap_build_forwarding_recdes (RECDES * recdes_p, INT16 rec_type, OID * oid_buffer_p,
					  bool reserve_extra_oid);

/* heap insert related functions */
static int heap_insert_adjust_recdes_header (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context,
					     RECDES * recdes_p, bool is_mvcc_op, bool is_mvcc_class);
static int heap_insert_handle_multipage_record (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context);
static int heap_get_insert_location_with_lock (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context,
					       PGBUF_WATCHER * home_hint_p);
static int heap_find_location_and_insert_rec_newhome (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context);
static int heap_insert_newhome (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * parent_context, RECDES * recdes_p,
				OID * out_oid_p);
static int heap_insert_newver (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, PGBUF_WATCHER * home_hint);
static int heap_insert_physical (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context);
static void heap_log_insert_physical (THREAD_ENTRY * thread_p, PAGE_PTR page_p, VFID * vfid_p, OID * oid_p,
				      RECDES * recdes_p, bool is_mvcc_op, bool is_redistribute_op);

/* heap delete related functions */
void heap_delete_adjust_header (MVCC_REC_HEADER * header_p, MVCCID mvcc_id, OID * next_version_p, OID * partition_oid_p,
				int record_size);
MVCC_REC_HEADER heap_delete_adjust_recdes_header (RECDES * recdes_p, MVCCID mvcc_id, OID * next_version_p,
						  OID * partition_oid_p);
static int heap_get_record_location (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context);
static int heap_delete_bigone (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op);
static int heap_delete_relocation (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op);
static int heap_delete_home (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op);
static int heap_delete_physical (THREAD_ENTRY * thread_p, HFID * hfid_p, PAGE_PTR page_p, OID * oid_p);
static void heap_log_delete_physical (THREAD_ENTRY * thread_p, PAGE_PTR page_p, VFID * vfid_p, OID * oid_p,
				      RECDES * recdes_p, bool mark_reusable);

/* heap update related functions */
static int heap_update_bigone (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op);
static int heap_update_relocation (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op);
static int heap_update_home (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op);
static int heap_update_physical (THREAD_ENTRY * thread_p, PAGE_PTR page_p, short slot_id, RECDES * recdes_p);
static void heap_log_update_physical (THREAD_ENTRY * thread_p, PAGE_PTR page_p, VFID * vfid_p, OID * oid_p,
				      RECDES * old_recdes_p, RECDES * new_recdes_p, bool is_mvcc_op);

static void *heap_partition_link_entry_alloc (void);
static int heap_partition_link_entry_free (void *unique_stat);
static int heap_partition_link_entry_init (void *unique_stat);
static int heap_partition_link_entry_key_copy (void *src, void *dest);
static unsigned int heap_partition_link_entry_key_hash (void *key, int hash_table_size);
static int heap_partition_link_entry_key_compare (void *k1, void *k2);
static int heap_get_partition_link_from_cache (THREAD_ENTRY * thread_p, OID * class_oid);
static int heap_update_partition_link_flag (THREAD_ENTRY * thread_p, HFID * hfid, int set_flag);
static int heap_get_partition_link_flag (THREAD_ENTRY * thread_p, HFID * hfid, int *partition_link);
static int heap_delete_from_partition_link_cache (THREAD_ENTRY * thread_p, const OID * class_oid);

static void *heap_hfid_table_entry_alloc (void);
static int heap_hfid_table_entry_free (void *unique_stat);
static int heap_hfid_table_entry_init (void *unique_stat);
static int heap_hfid_table_entry_key_copy (void *src, void *dest);
static unsigned int heap_hfid_table_entry_key_hash (void *key, int hash_table_size);
static int heap_hfid_table_entry_key_compare (void *k1, void *k2);
static int heap_insert_hfid_for_class_oid (THREAD_ENTRY * thread_p, const OID * class_oid, HFID * hfid);
static int heap_get_hfid_from_cache (THREAD_ENTRY * thread_p, const OID * class_oid, HFID * hfid);
static int heap_get_hfid_from_class_record (THREAD_ENTRY * thread_p, const OID * class_oid, HFID * hfid);

static void heap_page_update_chain_after_mvcc_op (THREAD_ENTRY * thread_p, PAGE_PTR heap_page, MVCCID mvccid);
static void heap_page_rv_chain_update (THREAD_ENTRY * thread_p, PAGE_PTR heap_page, MVCCID mvccid,
				       bool vacuum_status_change);

static int heap_scancache_add_partition_node (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache,
					      OID * partition_oid);

/*
 * heap_hash_vpid () - Hash a page identifier
 *   return: hash value
 *   key_vpid(in): VPID to hash
 *   htsize(in): Size of hash table
 */
static unsigned int
heap_hash_vpid (const void *key_vpid, unsigned int htsize)
{
  const VPID *vpid = (VPID *) key_vpid;

  return ((vpid->pageid | ((unsigned int) vpid->volid) << 24) % htsize);
}

/*
 * heap_compare_vpid () - Compare two vpids keys for hashing
 *   return: int (key_vpid1 == key_vpid2 ?)
 *   key_vpid1(in): First key
 *   key_vpid2(in): Second key
 */
static int
heap_compare_vpid (const void *key_vpid1, const void *key_vpid2)
{
  const VPID *vpid1 = (VPID *) key_vpid1;
  const VPID *vpid2 = (VPID *) key_vpid2;

  return VPID_EQ (vpid1, vpid2);
}

/*
 * heap_hash_hfid () - Hash a file identifier
 *   return: hash value
 *   key_hfid(in): HFID to hash
 *   htsize(in): Size of hash table
 */
static unsigned int
heap_hash_hfid (const void *key_hfid, unsigned int htsize)
{
  const HFID *hfid = (HFID *) key_hfid;

  return ((hfid->hpgid | ((unsigned int) hfid->vfid.volid) << 24) % htsize);
}

/*
 * heap_compare_hfid () - Compare two hfids keys for hashing
 *   return: int (key_hfid1 == key_hfid2 ?)
 *   key_hfid1(in): First key
 *   key_hfid2(in): Second key
 */
static int
heap_compare_hfid (const void *key_hfid1, const void *key_hfid2)
{
  const HFID *hfid1 = (HFID *) key_hfid1;
  const HFID *hfid2 = (HFID *) key_hfid2;

  return HFID_EQ (hfid1, hfid2);
}

/*
 * heap_stats_dump_bestspace_information () -
 *   return:
 *
 *     out_fp(in/out):
 */
static void
heap_stats_dump_bestspace_information (FILE * out_fp)
{
  int rc;

  rc = pthread_mutex_lock (&heap_Bestspace->bestspace_mutex);

  mht_dump (out_fp, heap_Bestspace->hfid_ht, false, heap_stats_print_hash_entry, NULL);
  mht_dump (out_fp, heap_Bestspace->vpid_ht, false, heap_stats_print_hash_entry, NULL);

  pthread_mutex_unlock (&heap_Bestspace->bestspace_mutex);
}

/*
 * heap_stats_print_hash_entry()- Print the entry
 *                              Will be used by mht_dump() function
 *  return:
 *
 *   outfp(in):
 *   key(in):
 *   ent(in):
 *   ignore(in):
 */
static int
heap_stats_print_hash_entry (FILE * outfp, const void *key, void *ent, void *ignore)
{
  HEAP_STATS_ENTRY *entry = (HEAP_STATS_ENTRY *) ent;

  fprintf (outfp, "HFID:Volid = %6d, Fileid = %6d, Header-pageid = %6d,VPID = %4d|%4d, freespace = %6d\n",
	   entry->hfid.vfid.volid, entry->hfid.vfid.fileid, entry->hfid.hpgid, entry->best.vpid.volid,
	   entry->best.vpid.pageid, entry->best.freespace);

  return true;
}

/*
 * heap_stats_entry_free () - release all memory occupied by an best space
 *   return:  NO_ERROR
 *   data(in): a best space associated with the key
 *   args(in): NULL (not used here, but needed by mht_map)
 */
static int
heap_stats_entry_free (THREAD_ENTRY * thread_p, void *data, void *args)
{
  HEAP_STATS_ENTRY *ent;

  ent = (HEAP_STATS_ENTRY *) data;
  assert_release (ent != NULL);

  if (ent)
    {
      if (heap_Bestspace->free_list_count < HEAP_STATS_ENTRY_FREELIST_SIZE)
	{
	  ent->next = heap_Bestspace->free_list;
	  heap_Bestspace->free_list = ent;

	  heap_Bestspace->free_list_count++;
	}
      else
	{
	  free_and_init (ent);

	  heap_Bestspace->num_free++;
	}
    }

  return NO_ERROR;
}

/*
 * heap_stats_add_bestspace () -
 */
static HEAP_STATS_ENTRY *
heap_stats_add_bestspace (THREAD_ENTRY * thread_p, const HFID * hfid, VPID * vpid, int freespace)
{
  HEAP_STATS_ENTRY *ent;
  int rc;

  assert (prm_get_integer_value (PRM_ID_HF_MAX_BESTSPACE_ENTRIES) > 0);

  rc = pthread_mutex_lock (&heap_Bestspace->bestspace_mutex);

  ent = (HEAP_STATS_ENTRY *) mht_get (heap_Bestspace->vpid_ht, vpid);

  if (ent)
    {
      ent->best.freespace = freespace;
      goto end;
    }

  if (heap_Bestspace->num_stats_entries >= prm_get_integer_value (PRM_ID_HF_MAX_BESTSPACE_ENTRIES))
    {
      er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE, ER_HF_MAX_BESTSPACE_ENTRIES, 1,
	      prm_get_integer_value (PRM_ID_HF_MAX_BESTSPACE_ENTRIES));

      mnt_hf_stats_bestspace_maxed (thread_p);

      ent = NULL;
      goto end;
    }

  if (heap_Bestspace->free_list_count > 0)
    {
      assert_release (heap_Bestspace->free_list != NULL);

      ent = heap_Bestspace->free_list;
      if (ent == NULL)
	{
	  goto end;
	}
      heap_Bestspace->free_list = ent->next;
      ent->next = NULL;

      heap_Bestspace->free_list_count--;
    }
  else
    {
      ent = (HEAP_STATS_ENTRY *) malloc (sizeof (HEAP_STATS_ENTRY));
      if (ent == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, sizeof (HEAP_STATS_ENTRY));

	  goto end;
	}

      heap_Bestspace->num_alloc++;
    }

  HFID_COPY (&ent->hfid, hfid);
  ent->best.vpid = *vpid;
  ent->best.freespace = freespace;
  ent->next = NULL;

  if (mht_put (heap_Bestspace->vpid_ht, &ent->best.vpid, ent) == NULL)
    {
      assert_release (false);
      (void) heap_stats_entry_free (thread_p, ent, NULL);
      ent = NULL;
      goto end;
    }

  if (mht_put_new (heap_Bestspace->hfid_ht, &ent->hfid, ent) == NULL)
    {
      assert_release (false);
      (void) mht_rem (heap_Bestspace->vpid_ht, &ent->best.vpid, NULL, NULL);
      (void) heap_stats_entry_free (thread_p, ent, NULL);
      ent = NULL;
      goto end;
    }

  heap_Bestspace->num_stats_entries++;
  mnt_hf_stats_bestspace_entries (thread_p, heap_Bestspace->num_stats_entries);

end:

  assert (mht_count (heap_Bestspace->vpid_ht) == mht_count (heap_Bestspace->hfid_ht));

  pthread_mutex_unlock (&heap_Bestspace->bestspace_mutex);

  return ent;
}

/*
 * heap_stats_del_bestspace_by_hfid () -
 *   return: deleted count
 *
 *   hfid(in):
 */
static int
heap_stats_del_bestspace_by_hfid (THREAD_ENTRY * thread_p, const HFID * hfid)
{
  HEAP_STATS_ENTRY *ent;
  int del_cnt = 0;
  int rc;

  rc = pthread_mutex_lock (&heap_Bestspace->bestspace_mutex);

  while ((ent = (HEAP_STATS_ENTRY *) mht_get2 (heap_Bestspace->hfid_ht, hfid, NULL)) != NULL)
    {
      (void) mht_rem2 (heap_Bestspace->hfid_ht, &ent->hfid, ent, NULL, NULL);
      (void) mht_rem (heap_Bestspace->vpid_ht, &ent->best.vpid, NULL, NULL);
      (void) heap_stats_entry_free (thread_p, ent, NULL);
      ent = NULL;

      del_cnt++;
    }

  assert (del_cnt <= heap_Bestspace->num_stats_entries);

  heap_Bestspace->num_stats_entries -= del_cnt;

  mnt_hf_stats_bestspace_entries (thread_p, heap_Bestspace->num_stats_entries);

  assert (mht_count (heap_Bestspace->vpid_ht) == mht_count (heap_Bestspace->hfid_ht));
  pthread_mutex_unlock (&heap_Bestspace->bestspace_mutex);

  return del_cnt;
}

/*
 * heap_stats_del_bestspace_by_vpid () -
 *   return: NO_ERROR
 *
 *  vpid(in):
 */
static int
heap_stats_del_bestspace_by_vpid (THREAD_ENTRY * thread_p, VPID * vpid)
{
  HEAP_STATS_ENTRY *ent;
  int rc;

  rc = pthread_mutex_lock (&heap_Bestspace->bestspace_mutex);

  ent = (HEAP_STATS_ENTRY *) mht_get (heap_Bestspace->vpid_ht, vpid);
  if (ent == NULL)
    {
      goto end;
    }

  (void) mht_rem2 (heap_Bestspace->hfid_ht, &ent->hfid, ent, NULL, NULL);
  (void) mht_rem (heap_Bestspace->vpid_ht, &ent->best.vpid, NULL, NULL);
  (void) heap_stats_entry_free (thread_p, ent, NULL);
  ent = NULL;

  heap_Bestspace->num_stats_entries -= 1;

  mnt_hf_stats_bestspace_entries (thread_p, heap_Bestspace->num_stats_entries);

end:
  assert (mht_count (heap_Bestspace->vpid_ht) == mht_count (heap_Bestspace->hfid_ht));

  pthread_mutex_unlock (&heap_Bestspace->bestspace_mutex);

  return NO_ERROR;
}

/*
 * heap_stats_get_bestspace_by_vpid () -
 *   return: NO_ERROR
 *
 *  vpid(in):
 */
static HEAP_BESTSPACE
heap_stats_get_bestspace_by_vpid (THREAD_ENTRY * thread_p, VPID * vpid)
{
  HEAP_STATS_ENTRY *ent;
  HEAP_BESTSPACE best;
  int rc;

  best.freespace = -1;
  VPID_SET_NULL (&best.vpid);

  rc = pthread_mutex_lock (&heap_Bestspace->bestspace_mutex);

  ent = (HEAP_STATS_ENTRY *) mht_get (heap_Bestspace->vpid_ht, vpid);
  if (ent == NULL)
    {
      goto end;
    }

  best = ent->best;

end:
  assert (mht_count (heap_Bestspace->vpid_ht) == mht_count (heap_Bestspace->hfid_ht));

  pthread_mutex_unlock (&heap_Bestspace->bestspace_mutex);

  return best;
}

/*
 * Scan page buffer and latch page manipulation
 */

/*
 * heap_scan_pb_lock_and_fetch () -
 *   return:
 *   vpid_ptr(in):
 *   fetch_mode(in):
 *   lock(in):
 *   scan_cache(in):
 *
 * NOTE: Because this function is called in too many places and because it
 *	 is useful where a page was fixed for debug purpose, we pass the
 *	 caller file/line arguments to pgbuf_fix.
 */
#if defined (NDEBUG)
static PAGE_PTR
heap_scan_pb_lock_and_fetch (THREAD_ENTRY * thread_p, const VPID * vpid_ptr, PAGE_FETCH_MODE fetch_mode, LOCK lock,
			     HEAP_SCANCACHE * scan_cache, PGBUF_WATCHER * pg_watcher)
#else /* !NDEBUG */
static PAGE_PTR
heap_scan_pb_lock_and_fetch_debug (THREAD_ENTRY * thread_p, const VPID * vpid_ptr, PAGE_FETCH_MODE fetch_mode,
				   LOCK lock, HEAP_SCANCACHE * scan_cache, PGBUF_WATCHER * pg_watcher,
				   const char *caller_file, const int caller_line)
#endif				/* !NDEBUG */
{
  PAGE_PTR pgptr = NULL;
  LOCK page_lock;
  PGBUF_LATCH_MODE page_latch_mode;

  if (scan_cache != NULL)
    {
      if (scan_cache->page_latch == NULL_LOCK)
	{
	  page_lock = NULL_LOCK;
	}
      else
	{
	  assert (scan_cache->page_latch >= NULL_LOCK);
	  assert (lock >= NULL_LOCK);
	  page_lock = lock_Conv[scan_cache->page_latch][lock];
	  assert (page_lock != NA_LOCK);
	}
    }
  else
    {
      page_lock = lock;
    }

  if (page_lock == S_LOCK)
    {
      page_latch_mode = PGBUF_LATCH_READ;
    }
  else
    {
      page_latch_mode = PGBUF_LATCH_WRITE;
    }

  if (pg_watcher != NULL)
    {
#if defined (NDEBUG)
      if (pgbuf_ordered_fix_release (thread_p, vpid_ptr, fetch_mode, page_latch_mode, pg_watcher) != NO_ERROR)
#else /* !NDEBUG */
      if (pgbuf_ordered_fix_debug (thread_p, vpid_ptr, fetch_mode, page_latch_mode, pg_watcher,
				   caller_file, caller_line) != NO_ERROR)
#endif /* !NDEBUG */
	{
	  return NULL;
	}
      pgptr = pg_watcher->pgptr;
    }
  else
    {
#if defined (NDEBUG)
      pgptr = pgbuf_fix_release (thread_p, vpid_ptr, fetch_mode, page_latch_mode, PGBUF_UNCONDITIONAL_LATCH);
#else /* !NDEBUG */
      pgptr =
	pgbuf_fix_debug (thread_p, vpid_ptr, fetch_mode, page_latch_mode, PGBUF_UNCONDITIONAL_LATCH, caller_file,
			 caller_line);
#endif /* !NDEBUG */
    }

  if (pgptr != NULL)
    {
      (void) pgbuf_check_page_ptype (thread_p, pgptr, PAGE_HEAP);
    }

  return pgptr;
}

/*
 * heap_is_big_length () -
 *   return: true/false
 *   length(in):
 */
bool
heap_is_big_length (int length)
{
  return (length > heap_Maxslotted_reclength) ? true : false;
}

/*
 * heap_get_spage_type () -
 *   return: the type of the slotted page of the heap file.
 */
static int
heap_get_spage_type (void)
{
  return ANCHORED_DONT_REUSE_SLOTS;
}

/*
 * heap_is_reusable_oid () -
 *   return: true if the heap file is reuse_oid table
 *   file_type(in): the file type of the heap file
 */
static bool
heap_is_reusable_oid (const FILE_TYPE file_type)
{
  if (file_type == FILE_HEAP)
    {
      return false;
    }
  else if (file_type == FILE_HEAP_REUSE_SLOTS)
    {
      return true;
    }
  else
    {
      assert (false);
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
    }
  return false;
}

/* TODO: STL::list for _cache.area */
/*
 * heap_classrepr_initialize_cache () - Initialize the class representation cache
 *   return: NO_ERROR
 */
static int
heap_classrepr_initialize_cache (void)
{
  HEAP_CLASSREPR_ENTRY *cache_entry;
  HEAP_CLASSREPR_LOCK *lock_entry;
  HEAP_CLASSREPR_HASH *hash_entry;
  int i, ret = NO_ERROR;
  size_t size;

  if (heap_Classrepr != NULL)
    {
      ret = heap_classrepr_finalize_cache ();
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }

  /* initialize hash entries table */
  heap_Classrepr_cache.num_entries = HEAP_CLASSREPR_MAXCACHE;

  heap_Classrepr_cache.area =
    (HEAP_CLASSREPR_ENTRY *) malloc (sizeof (HEAP_CLASSREPR_ENTRY) * heap_Classrepr_cache.num_entries);
  if (heap_Classrepr_cache.area == NULL)
    {
      ret = ER_OUT_OF_VIRTUAL_MEMORY;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1,
	      sizeof (HEAP_CLASSREPR_ENTRY) * heap_Classrepr_cache.num_entries);
      goto exit_on_error;
    }

  cache_entry = heap_Classrepr_cache.area;
  for (i = 0; i < heap_Classrepr_cache.num_entries; i++)
    {
      pthread_mutex_init (&cache_entry[i].mutex, NULL);

      cache_entry[i].idx = i;
      cache_entry[i].fcnt = 0;
      cache_entry[i].zone = ZONE_FREE;
      cache_entry[i].next_wait_thrd = NULL;
      cache_entry[i].hash_next = NULL;
      cache_entry[i].prev = NULL;
      cache_entry[i].next = (i < heap_Classrepr_cache.num_entries - 1) ? &cache_entry[i + 1] : NULL;

      cache_entry[i].force_decache = false;

      OID_SET_NULL (&cache_entry[i].class_oid);
      cache_entry[i].max_reprid = DEFAULT_REPR_INCREMENT;
      cache_entry[i].repr = (OR_CLASSREP **) malloc (cache_entry[i].max_reprid * sizeof (OR_CLASSREP *));
      if (cache_entry[i].repr == NULL)
	{
	  ret = ER_OUT_OF_VIRTUAL_MEMORY;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1, cache_entry[i].max_reprid * sizeof (OR_CLASSREP *));
	  goto exit_on_error;
	}
      memset (cache_entry[i].repr, 0, cache_entry[i].max_reprid * sizeof (OR_CLASSREP *));

      cache_entry[i].last_reprid = NULL_REPRID;
    }

  /* initialize hash bucket table */
  heap_Classrepr_cache.num_hash = CLASSREPR_HASH_SIZE;
  heap_Classrepr_cache.hash_table =
    (HEAP_CLASSREPR_HASH *) malloc (heap_Classrepr_cache.num_hash * sizeof (HEAP_CLASSREPR_HASH));
  if (heap_Classrepr_cache.hash_table == NULL)
    {
      ret = ER_OUT_OF_VIRTUAL_MEMORY;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1, heap_Classrepr_cache.num_hash * sizeof (HEAP_CLASSREPR_HASH));
      goto exit_on_error;
    }

  hash_entry = heap_Classrepr_cache.hash_table;
  for (i = 0; i < heap_Classrepr_cache.num_hash; i++)
    {
      pthread_mutex_init (&hash_entry[i].hash_mutex, NULL);
      hash_entry[i].idx = i;
      hash_entry[i].hash_next = NULL;
      hash_entry[i].lock_next = NULL;
    }

  /* initialize hash lock table */
  size = thread_num_total_threads () * sizeof (HEAP_CLASSREPR_LOCK);
  heap_Classrepr_cache.lock_table = (HEAP_CLASSREPR_LOCK *) malloc (size);
  if (heap_Classrepr_cache.lock_table == NULL)
    {
      ret = ER_OUT_OF_VIRTUAL_MEMORY;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1, size);
      goto exit_on_error;
    }
  lock_entry = heap_Classrepr_cache.lock_table;
  for (i = 0; i < thread_num_total_threads (); i++)
    {
      OID_SET_NULL (&lock_entry[i].class_oid);
      lock_entry[i].lock_next = NULL;
      lock_entry[i].next_wait_thrd = NULL;
    }

  /* initialize LRU list */

  pthread_mutex_init (&heap_Classrepr_cache.LRU_list.LRU_mutex, NULL);
  heap_Classrepr_cache.LRU_list.LRU_top = NULL;
  heap_Classrepr_cache.LRU_list.LRU_bottom = NULL;

  /* initialize free list */
  pthread_mutex_init (&heap_Classrepr_cache.free_list.free_mutex, NULL);
  heap_Classrepr_cache.free_list.free_top = &heap_Classrepr_cache.area[0];
  heap_Classrepr_cache.free_list.free_cnt = heap_Classrepr_cache.num_entries;

  heap_Classrepr = &heap_Classrepr_cache;

  return ret;

exit_on_error:

  heap_Classrepr_cache.num_entries = 0;

  return (ret == NO_ERROR) ? ER_FAILED : ret;
}

/* TODO: STL::list for _cache.area */
/*
 * heap_classrepr_finalize_cache () - Destroy any cached structures
 *   return: NO_ERROR
 *
 * Note: Any cached representations are deallocated at this moment and
 * the hash table is also removed.
 */
static int
heap_classrepr_finalize_cache (void)
{
  HEAP_CLASSREPR_ENTRY *cache_entry;
  HEAP_CLASSREPR_HASH *hash_entry;
  int i, j;
  int ret = NO_ERROR;

  if (heap_Classrepr == NULL)
    {
      return NO_ERROR;		/* nop */
    }

#ifdef DEBUG_CLASSREPR_CACHE
  ret = heap_classrepr_dump_anyfixed ();
  if (ret != NO_ERROR)
    {
      return ret;
    }
#endif /* DEBUG_CLASSREPR_CACHE */

  /* finalize hash entries table */
  cache_entry = heap_Classrepr_cache.area;
  for (i = 0; cache_entry != NULL && i < heap_Classrepr_cache.num_entries; i++)
    {
      pthread_mutex_destroy (&cache_entry[i].mutex);

      if (cache_entry[i].repr == NULL)
	{
	  assert (cache_entry[i].repr != NULL);
	  continue;
	}

      for (j = 0; j <= cache_entry[i].last_reprid; j++)
	{
	  if (cache_entry[i].repr[j] != NULL)
	    {
	      or_free_classrep (cache_entry[i].repr[j]);
	      cache_entry[i].repr[j] = NULL;
	    }
	}
      free_and_init (cache_entry[i].repr);
    }
  if (heap_Classrepr_cache.area != NULL)
    {
      free_and_init (heap_Classrepr_cache.area);
    }
  heap_Classrepr_cache.num_entries = -1;

  /* finalize hash bucket table */
  hash_entry = heap_Classrepr_cache.hash_table;
  for (i = 0; hash_entry != NULL && i < heap_Classrepr_cache.num_hash; i++)
    {
      pthread_mutex_destroy (&hash_entry[i].hash_mutex);
    }
  heap_Classrepr_cache.num_hash = -1;
  if (heap_Classrepr_cache.hash_table != NULL)
    {
      free_and_init (heap_Classrepr_cache.hash_table);
    }

  /* finalize hash lock table */
  if (heap_Classrepr_cache.lock_table != NULL)
    {
      free_and_init (heap_Classrepr_cache.lock_table);
    }

  /* finalize LRU list */

  pthread_mutex_destroy (&heap_Classrepr_cache.LRU_list.LRU_mutex);

  /* initialize free list */
  pthread_mutex_destroy (&heap_Classrepr_cache.free_list.free_mutex);

  heap_Classrepr = NULL;

  return ret;
}

/*
 * heap_classrepr_entry_reset () -
 *   return: NO_ERROR
 *   cache_entry(in):
 *
 * Note: Reset the given class representation entry.
 */
static int
heap_classrepr_entry_reset (HEAP_CLASSREPR_ENTRY * cache_entry)
{
  int i;
  int ret = NO_ERROR;

  if (cache_entry == NULL)
    {
      return NO_ERROR;		/* nop */
    }

  /* free all classrepr */
  for (i = 0; i <= cache_entry->last_reprid; i++)
    {
      if (cache_entry->repr[i] != NULL)
	{
	  or_free_classrep (cache_entry->repr[i]);
	  cache_entry->repr[i] = NULL;
	}
    }

  cache_entry->force_decache = false;
  OID_SET_NULL (&cache_entry->class_oid);
  if (cache_entry->max_reprid > DEFAULT_REPR_INCREMENT)
    {
      OR_CLASSREP **t;

      t = cache_entry->repr;
      cache_entry->repr = (OR_CLASSREP **) malloc (DEFAULT_REPR_INCREMENT * sizeof (OR_CLASSREP *));
      if (cache_entry->repr == NULL)
	{
	  ret = ER_OUT_OF_VIRTUAL_MEMORY;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1, DEFAULT_REPR_INCREMENT * sizeof (OR_CLASSREP *));
	  cache_entry->repr = t;
	}
      else
	{
	  free_and_init (t);
	  cache_entry->max_reprid = DEFAULT_REPR_INCREMENT;
	  memset (cache_entry->repr, 0, DEFAULT_REPR_INCREMENT * sizeof (OR_CLASSREP *));
	}

    }
  cache_entry->last_reprid = NULL_REPRID;

  return ret;
}

/*
 * heap_classrepr_entry_remove_from_LRU () -
 *   return: NO_ERROR
 *   cache_entry(in):
 */
static int
heap_classrepr_entry_remove_from_LRU (HEAP_CLASSREPR_ENTRY * cache_entry)
{
  if (cache_entry)
    {
      if (cache_entry == heap_Classrepr_cache.LRU_list.LRU_top)
	{
	  heap_Classrepr_cache.LRU_list.LRU_top = cache_entry->next;
	}
      else
	{
	  cache_entry->prev->next = cache_entry->next;
	}

      if (cache_entry == heap_Classrepr_cache.LRU_list.LRU_bottom)
	{
	  heap_Classrepr_cache.LRU_list.LRU_bottom = cache_entry->prev;
	}
      else
	{
	  cache_entry->next->prev = cache_entry->prev;
	}
    }

  return NO_ERROR;
}

/* TODO: STL::list for ->prev */
/*
 * heap_classrepr_decache_guessed_last () -
 *   return: NO_ERROR
 *   class_oid(in):
 *
 * Note: Decache the guessed last representations (i.e., that with -1)
 * from the given class.
 *
 * Note: This function should be called when a class is updated.
 *       1: During normal update
 */
static int
heap_classrepr_decache_guessed_last (const OID * class_oid)
{
  HEAP_CLASSREPR_ENTRY *cache_entry, *prev_entry, *cur_entry;
  HEAP_CLASSREPR_HASH *hash_anchor;
  int rv;
  int ret = NO_ERROR;

  if (class_oid != NULL)
    {
      hash_anchor = &heap_Classrepr->hash_table[REPR_HASH (class_oid)];

    search_begin:
      rv = pthread_mutex_lock (&hash_anchor->hash_mutex);

      for (cache_entry = hash_anchor->hash_next; cache_entry != NULL; cache_entry = cache_entry->hash_next)
	{
	  if (OID_EQ (class_oid, &cache_entry->class_oid))
	    {
	      rv = pthread_mutex_trylock (&cache_entry->mutex);
	      if (rv == 0)
		{
		  goto delete_begin;
		}

	      if (rv != EBUSY)
		{
		  ret = ER_CSS_PTHREAD_MUTEX_LOCK;
		  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 0);
		  pthread_mutex_unlock (&hash_anchor->hash_mutex);
		  goto exit_on_error;
		}

	      pthread_mutex_unlock (&hash_anchor->hash_mutex);
	      rv = pthread_mutex_lock (&cache_entry->mutex);

	      /* cache_entry can be used by others. check again */
	      if (!OID_EQ (class_oid, &cache_entry->class_oid))
		{
		  pthread_mutex_unlock (&cache_entry->mutex);
		  goto search_begin;
		}
	      break;
	    }
	}

      /* class_oid cache_entry is not found */
      if (cache_entry == NULL)
	{
	  pthread_mutex_unlock (&hash_anchor->hash_mutex);
	  goto exit_on_error;
	}

      /* hash anchor lock has been released */
      rv = pthread_mutex_lock (&hash_anchor->hash_mutex);

    delete_begin:

      /* delete classrepr from hash chain */
      prev_entry = NULL;
      cur_entry = hash_anchor->hash_next;
      while (cur_entry != NULL)
	{
	  if (cur_entry == cache_entry)
	    {
	      break;
	    }
	  prev_entry = cur_entry;
	  cur_entry = cur_entry->hash_next;
	}

      /* class_oid cache_entry is not found */
      if (cur_entry == NULL)
	{
	  /* This cannot happen */
	  pthread_mutex_unlock (&hash_anchor->hash_mutex);
	  pthread_mutex_unlock (&cache_entry->mutex);

	  goto exit_on_error;
	}

      if (prev_entry == NULL)
	{
	  hash_anchor->hash_next = cur_entry->hash_next;
	}
      else
	{
	  prev_entry->hash_next = cur_entry->hash_next;
	}
      cur_entry->hash_next = NULL;

      pthread_mutex_unlock (&hash_anchor->hash_mutex);

      cache_entry->force_decache = true;

      /* Remove from LRU list */
      if (cache_entry->zone == ZONE_LRU)
	{
	  rv = pthread_mutex_lock (&heap_Classrepr_cache.LRU_list.LRU_mutex);
	  (void) heap_classrepr_entry_remove_from_LRU (cache_entry);
	  pthread_mutex_unlock (&heap_Classrepr_cache.LRU_list.LRU_mutex);
	  cache_entry->zone = ZONE_VOID;
	}
      cache_entry->prev = NULL;
      cache_entry->next = NULL;

      if (cache_entry->fcnt == 0)
	{
	  /* move cache_entry to free_list */
	  ret = heap_classrepr_entry_reset (cache_entry);
	  if (ret == NO_ERROR)
	    {
	      ret = heap_classrepr_entry_free (cache_entry);
	    }
	}

      pthread_mutex_unlock (&cache_entry->mutex);
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_classrepr_decache () - Deache any unfixed class representations of
 *                           given class
 *   return: NO_ERROR
 *   class_oid(in):
 *
 * Note: Decache all class representations of given class. If a class
 * is not given all class representations are decached.
 *
 * Note: This function should be called when a class is updated.
 *       1: At the end/beginning of rollback since we do not have any
 *          idea of a heap identifier of rolled back objects and we
 *          expend too much time, searching for the OID, every time we
 *          rolled back an updated object.
 */
int
heap_classrepr_decache (THREAD_ENTRY * thread_p, const OID * class_oid)
{
  int ret;

  ret = heap_classrepr_decache_guessed_last (class_oid);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  if (csect_enter (thread_p, CSECT_HEAP_CHNGUESS, INF_WAIT) != NO_ERROR)
    {
      return ER_FAILED;
    }
  if (heap_Guesschn != NULL && heap_Guesschn->schema_change == false)
    {
      ret = heap_chnguess_decache (class_oid);
    }
  csect_exit (thread_p, CSECT_HEAP_CHNGUESS);

  return ret;
}

/* TODO: STL::list for _cache.area */
/*
 * heap_classrepr_free () - Free a class representation
 *   return: NO_ERROR
 *   classrep(in): The class representation structure
 *   idx_incache(in): An index if the desired class representation is part of
 *                    the cache, otherwise -1 (no part of cache)
 *
 * Note: Free a class representation. If the class representation was
 * part of the class representation cache, the fix count is
 * decremented and the class representation will continue be
 * cached. The representation entry will be subject for
 * replacement when the fix count is zero (no one is using it).
 * If the class representatin was not part of the cache, it is
 * freed.
 */
int
heap_classrepr_free (OR_CLASSREP * classrep, int *idx_incache)
{
  HEAP_CLASSREPR_ENTRY *cache_entry;
  int rv;
  int ret = NO_ERROR;

  if (*idx_incache < 0)
    {
      or_free_classrep (classrep);
      return NO_ERROR;
    }

  cache_entry = &heap_Classrepr_cache.area[*idx_incache];

  rv = pthread_mutex_lock (&cache_entry->mutex);
  cache_entry->fcnt--;
  if (cache_entry->fcnt == 0)
    {
      /* 
       * Is this entry declared to be decached
       */
#ifdef DEBUG_CLASSREPR_CACHE
      rv = pthread_mutex_lock (&heap_Classrepr_cache.num_fix_entries_mutex);
      heap_Classrepr_cache.num_fix_entries--;
      pthread_mutex_unlock (&heap_Classrepr_cache.num_fix_entries_mutex);
#endif /* DEBUG_CLASSREPR_CACHE */
      if (cache_entry->force_decache == true)
	{
	  /* cache_entry is already removed from LRU list. */

	  /* move cache_entry to free_list */
	  ret = heap_classrepr_entry_free (cache_entry);
	  if (ret == NO_ERROR)
	    {
	      ret = heap_classrepr_entry_reset (cache_entry);
	    }
	}
      else
	{
	  /* relocate entry to the top of LRU list */
	  if (cache_entry != heap_Classrepr_cache.LRU_list.LRU_top)
	    {
	      rv = pthread_mutex_lock (&heap_Classrepr_cache.LRU_list.LRU_mutex);
	      if (cache_entry->zone == ZONE_LRU)
		{
		  /* remove from LRU list */
		  (void) heap_classrepr_entry_remove_from_LRU (cache_entry);
		}

	      /* insert into LRU top */
	      cache_entry->prev = NULL;
	      cache_entry->next = heap_Classrepr_cache.LRU_list.LRU_top;
	      if (heap_Classrepr_cache.LRU_list.LRU_top == NULL)
		{
		  heap_Classrepr_cache.LRU_list.LRU_bottom = cache_entry;
		}
	      else
		{
		  heap_Classrepr_cache.LRU_list.LRU_top->prev = cache_entry;
		}
	      heap_Classrepr_cache.LRU_list.LRU_top = cache_entry;
	      cache_entry->zone = ZONE_LRU;

	      pthread_mutex_unlock (&heap_Classrepr_cache.LRU_list.LRU_mutex);
	    }
	}
    }
  pthread_mutex_unlock (&cache_entry->mutex);
  *idx_incache = -1;

  return ret;
}

#ifdef SERVER_MODE

enum
{ NEED_TO_RETRY = 0, LOCK_ACQUIRED };

/*
 * heap_classrepr_lock_class () - Prevent other threads accessing class_oid
 *                              class representation.
 *   return: ER_FAILED, NEED_TO_RETRY or LOCK_ACQUIRED
 *   hash_anchor(in):
 *   class_oid(in):
 */
static int
heap_classrepr_lock_class (THREAD_ENTRY * thread_p, HEAP_CLASSREPR_HASH * hash_anchor, OID * class_oid)
{
  HEAP_CLASSREPR_LOCK *cur_lock_entry;
  THREAD_ENTRY *cur_thrd_entry;

  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
      if (thread_p == NULL)
	{
	  return ER_FAILED;
	}
    }
  cur_thrd_entry = thread_p;

  for (cur_lock_entry = hash_anchor->lock_next; cur_lock_entry != NULL; cur_lock_entry = cur_lock_entry->lock_next)
    {
      if (OID_EQ (&cur_lock_entry->class_oid, class_oid))
	{
	  cur_thrd_entry->next_wait_thrd = cur_lock_entry->next_wait_thrd;
	  cur_lock_entry->next_wait_thrd = cur_thrd_entry;

	  thread_lock_entry (cur_thrd_entry);
	  pthread_mutex_unlock (&hash_anchor->hash_mutex);
	  thread_suspend_wakeup_and_unlock_entry (cur_thrd_entry, THREAD_HEAP_CLSREPR_SUSPENDED);

	  if (cur_thrd_entry->resume_status == THREAD_HEAP_CLSREPR_RESUMED)
	    {
	      return NEED_TO_RETRY;	/* traverse hash chain again */
	    }
	  else
	    {
	      /* probably due to an interrupt */
	      assert ((cur_thrd_entry->resume_status == THREAD_RESUME_DUE_TO_INTERRUPT));
	      return ER_FAILED;
	    }
	}
    }

  cur_lock_entry = &heap_Classrepr_cache.lock_table[cur_thrd_entry->index];
  cur_lock_entry->class_oid = *class_oid;
  cur_lock_entry->next_wait_thrd = NULL;
  cur_lock_entry->lock_next = hash_anchor->lock_next;
  hash_anchor->lock_next = cur_lock_entry;

  pthread_mutex_unlock (&hash_anchor->hash_mutex);

  return LOCK_ACQUIRED;		/* lock acquired. */
}

/*
 * heap_classrepr_unlock_class () -
 *   return: NO_ERROR
 *   hash_anchor(in):
 *   class_oid(in):
 *   need_hash_mutex(in):
 */
static int
heap_classrepr_unlock_class (HEAP_CLASSREPR_HASH * hash_anchor, OID * class_oid, int need_hash_mutex)
{
  HEAP_CLASSREPR_LOCK *prev_lock_entry, *cur_lock_entry;
  THREAD_ENTRY *cur_thrd_entry;
  int rv;

  /* if hash mutex lock is not acquired */
  if (need_hash_mutex)
    {
      rv = pthread_mutex_lock (&hash_anchor->hash_mutex);
    }

  prev_lock_entry = NULL;
  cur_lock_entry = hash_anchor->lock_next;
  while (cur_lock_entry != NULL)
    {
      if (OID_EQ (&cur_lock_entry->class_oid, class_oid))
	{
	  break;
	}
      prev_lock_entry = cur_lock_entry;
      cur_lock_entry = cur_lock_entry->lock_next;
    }

  /* if lock entry is found, remove it from lock list */
  if (cur_lock_entry == NULL)
    {				/* this cannot happen */
      pthread_mutex_unlock (&hash_anchor->hash_mutex);
      return ER_FAILED;
    }

  if (prev_lock_entry == NULL)
    {
      hash_anchor->lock_next = cur_lock_entry->lock_next;
    }
  else
    {
      prev_lock_entry->lock_next = cur_lock_entry->lock_next;
    }
  cur_lock_entry->lock_next = NULL;
  pthread_mutex_unlock (&hash_anchor->hash_mutex);
  for (cur_thrd_entry = cur_lock_entry->next_wait_thrd; cur_thrd_entry != NULL;
       cur_thrd_entry = cur_lock_entry->next_wait_thrd)
    {
      cur_lock_entry->next_wait_thrd = cur_thrd_entry->next_wait_thrd;
      cur_thrd_entry->next_wait_thrd = NULL;

      thread_wakeup (cur_thrd_entry, THREAD_HEAP_CLSREPR_RESUMED);
    }

  return NO_ERROR;
}
#endif /* SERVER_MODE */

/* TODO: STL::list for ->prev */
/*
 * heap_classrepr_entry_alloc () -
 *   return:
 */
static HEAP_CLASSREPR_ENTRY *
heap_classrepr_entry_alloc (void)
{
  HEAP_CLASSREPR_HASH *hash_anchor;
  HEAP_CLASSREPR_ENTRY *cache_entry, *prev_entry, *cur_entry;
  int rv;

  cache_entry = NULL;

/* check_free_list: */

  /* 1. Get entry from free list */
  if (heap_Classrepr_cache.free_list.free_top == NULL)
    {
      goto check_LRU_list;
    }

  rv = pthread_mutex_lock (&heap_Classrepr_cache.free_list.free_mutex);
  if (heap_Classrepr_cache.free_list.free_top == NULL)
    {
      pthread_mutex_unlock (&heap_Classrepr_cache.free_list.free_mutex);
      cache_entry = NULL;
    }
  else
    {
      cache_entry = heap_Classrepr_cache.free_list.free_top;
      heap_Classrepr_cache.free_list.free_top = cache_entry->next;
      heap_Classrepr_cache.free_list.free_cnt--;
      pthread_mutex_unlock (&heap_Classrepr_cache.free_list.free_mutex);

      rv = pthread_mutex_lock (&cache_entry->mutex);
      cache_entry->next = NULL;
      cache_entry->zone = ZONE_VOID;

      return cache_entry;
    }

check_LRU_list:
  /* 2. Get entry from LRU list */
  if (heap_Classrepr_cache.LRU_list.LRU_bottom == NULL)
    {
      goto expand_list;
    }

  rv = pthread_mutex_lock (&heap_Classrepr_cache.LRU_list.LRU_mutex);
  for (cache_entry = heap_Classrepr_cache.LRU_list.LRU_bottom; cache_entry != NULL; cache_entry = cache_entry->prev)
    {
      if (cache_entry->fcnt == 0)
	{
	  /* remove from LRU list */
	  (void) heap_classrepr_entry_remove_from_LRU (cache_entry);
	  cache_entry->zone = ZONE_VOID;
	  cache_entry->next = cache_entry->prev = NULL;
	  break;
	}
    }
  pthread_mutex_unlock (&heap_Classrepr_cache.LRU_list.LRU_mutex);

  if (cache_entry == NULL)
    {
      goto expand_list;
    }

  rv = pthread_mutex_lock (&cache_entry->mutex);
  /* if some has referenced, retry */
  if (cache_entry->fcnt != 0)
    {
      pthread_mutex_unlock (&cache_entry->mutex);
      goto check_LRU_list;
    }

  /* delete classrepr from hash chain */
  hash_anchor = &heap_Classrepr->hash_table[REPR_HASH (&cache_entry->class_oid)];
  rv = pthread_mutex_lock (&hash_anchor->hash_mutex);
  prev_entry = NULL;
  cur_entry = hash_anchor->hash_next;
  while (cur_entry != NULL)
    {
      if (cur_entry == cache_entry)
	{
	  break;
	}
      prev_entry = cur_entry;
      cur_entry = cur_entry->hash_next;
    }

  if (cur_entry == NULL)
    {
      /* This cannot happen */
      pthread_mutex_unlock (&hash_anchor->hash_mutex);
      pthread_mutex_unlock (&cache_entry->mutex);

      return NULL;
    }
  if (prev_entry == NULL)
    {
      hash_anchor->hash_next = cur_entry->hash_next;
    }
  else
    {
      prev_entry->hash_next = cur_entry->hash_next;
    }
  cur_entry->hash_next = NULL;
  pthread_mutex_unlock (&hash_anchor->hash_mutex);

  (void) heap_classrepr_entry_reset (cache_entry);

end:

  return cache_entry;

expand_list:

  /* not supported */
  cache_entry = NULL;
  goto end;
}

/* TODO: STL::list for ->next */
/*
 * heap_classrepr_entry_free () -
 *   return: NO_ERROR
 *   cache_entry(in):
 */
static int
heap_classrepr_entry_free (HEAP_CLASSREPR_ENTRY * cache_entry)
{
  int rv;
  rv = pthread_mutex_lock (&heap_Classrepr_cache.free_list.free_mutex);

  cache_entry->next = heap_Classrepr_cache.free_list.free_top;
  heap_Classrepr_cache.free_list.free_top = cache_entry;
  cache_entry->zone = ZONE_FREE;
  heap_Classrepr_cache.free_list.free_cnt++;

  pthread_mutex_unlock (&heap_Classrepr_cache.free_list.free_mutex);

  return NO_ERROR;
}

/*
 * heap_classrepr_get_from_record ()
 *   return: classrepr
 *
 *   last_reprid(out):
 *   class_oid(in): The class identifier
 *   class_recdes(in): The class recdes (when know) or NULL
 *   reprid(in): Representation of the class or NULL_REPRID for last one
 */
static OR_CLASSREP *
heap_classrepr_get_from_record (THREAD_ENTRY * thread_p, REPR_ID * last_reprid, OID * class_oid, RECDES * class_recdes,
				REPR_ID reprid)
{
  RECDES peek_recdes;
  RECDES *recdes = NULL;
  HEAP_SCANCACHE scan_cache;
  OR_CLASSREP *repr = NULL;

  if (last_reprid != NULL)
    {
      *last_reprid = NULL_REPRID;
    }

  if (class_recdes != NULL)
    {
      recdes = class_recdes;
    }
  else
    {
      heap_scancache_quick_start_root_hfid (thread_p, &scan_cache);
      if (heap_get (thread_p, class_oid, &peek_recdes, &scan_cache, PEEK, NULL_CHN) != S_SUCCESS)
	{
	  goto end;
	}
      recdes = &peek_recdes;
    }

  repr = or_get_classrep (recdes, reprid);
  if (last_reprid != NULL)
    {
      *last_reprid = or_rep_id (recdes);
    }

end:
  if (class_recdes == NULL)
    {
      heap_scancache_end (thread_p, &scan_cache);
    }
  return repr;
}

/*
 * heap_classrepr_get () - Obtain the desired class representation
 *   return: classrepr
 *   class_oid(in): The class identifier
 *   class_recdes(in): The class recdes (when know) or NULL
 *   reprid(in): Representation of the class or NULL_REPRID for last one
 *   idx_incache(in): An index if the desired class representation is part
 *                    of the cache
 *
 * Note: Obtain the desired class representation for the given class.
 */
OR_CLASSREP *
heap_classrepr_get (THREAD_ENTRY * thread_p, OID * class_oid, RECDES * class_recdes, REPR_ID reprid, int *idx_incache)
{
  HEAP_CLASSREPR_ENTRY *cache_entry;
  HEAP_CLASSREPR_HASH *hash_anchor;
  OR_CLASSREP *repr = NULL;
  REPR_ID last_reprid;
  int r;

  hash_anchor = &heap_Classrepr->hash_table[REPR_HASH (class_oid)];

  /* search entry with class_oid from hash chain */
search_begin:
  r = pthread_mutex_lock (&hash_anchor->hash_mutex);

  for (cache_entry = hash_anchor->hash_next; cache_entry != NULL; cache_entry = cache_entry->hash_next)
    {
      if (OID_EQ (class_oid, &cache_entry->class_oid))
	{
	  r = pthread_mutex_trylock (&cache_entry->mutex);
	  if (r == 0)
	    {
	      pthread_mutex_unlock (&hash_anchor->hash_mutex);
	    }
	  else
	    {
	      if (r != EBUSY)
		{
		  /* some error code */
		  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_LOCK, 0);
		  pthread_mutex_unlock (&hash_anchor->hash_mutex);
		  return NULL;
		}
	      /* if cache_entry lock is busy. release hash mutex lock and lock cache_entry lock unconditionally */
	      pthread_mutex_unlock (&hash_anchor->hash_mutex);
	      r = pthread_mutex_lock (&cache_entry->mutex);
	    }
	  /* check if cache_entry is used by others */
	  if (!OID_EQ (class_oid, &cache_entry->class_oid))
	    {
	      pthread_mutex_unlock (&cache_entry->mutex);
	      goto search_begin;
	    }

	  break;
	}
    }

  if (cache_entry == NULL)
    {
#ifdef SERVER_MODE
      /* class_oid was not found. Lock class_oid. heap_classrepr_lock_class () release hash_anchor->hash_lock */
      if (heap_classrepr_lock_class (thread_p, hash_anchor, class_oid) != LOCK_ACQUIRED)
	{
	  if (r == NEED_TO_RETRY)
	    {
	      goto search_begin;
	    }
	  else
	    {
	      assert (r == ER_FAILED);
	      return NULL;
	    }
	}
#endif

      repr = heap_classrepr_get_from_record (thread_p, &last_reprid, class_oid, class_recdes, reprid);
      if (repr == NULL)
	{
#ifdef SERVER_MODE
	  (void) heap_classrepr_unlock_class (hash_anchor, class_oid, true);
#endif
	  return NULL;
	}

      /* Get free entry */
      cache_entry = heap_classrepr_entry_alloc ();
      if (cache_entry == NULL)
	{
	  /* if all cache entry is busy, allocate memory for repr. */
	  *idx_incache = -1;

#ifdef SERVER_MODE
	  /* free lock for class_oid */
	  (void) heap_classrepr_unlock_class (hash_anchor, class_oid, true);
#endif

	  return repr;
	}

      /* check if cache_entry->repr[last_reprid] is valid. */
      if (last_reprid >= cache_entry->max_reprid)
	{
	  free_and_init (cache_entry->repr);

	  cache_entry->repr = (OR_CLASSREP **) malloc ((last_reprid + 1) * sizeof (OR_CLASSREP *));
	  if (cache_entry->repr == NULL)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
		      (last_reprid + 1) * sizeof (OR_CLASSREP *));

	      pthread_mutex_unlock (&cache_entry->mutex);
	      (void) heap_classrepr_entry_free (cache_entry);
#ifdef SERVER_MODE
	      (void) heap_classrepr_unlock_class (hash_anchor, class_oid, true);
#endif
	      if (repr != NULL)
		{
		  or_free_classrep (repr);
		}
	      return NULL;
	    }
	  cache_entry->max_reprid = last_reprid + 1;

	  memset (cache_entry->repr, 0, cache_entry->max_reprid * sizeof (OR_CLASSREP *));
	}

      if (reprid == NULL_REPRID)
	{
	  reprid = last_reprid;
	}

      if (reprid <= NULL_REPRID || reprid > last_reprid || reprid > cache_entry->max_reprid)
	{
	  assert (false);

	  pthread_mutex_unlock (&cache_entry->mutex);
	  (void) heap_classrepr_entry_free (cache_entry);
#ifdef SERVER_MODE
	  (void) heap_classrepr_unlock_class (hash_anchor, class_oid, true);
#endif

	  if (repr != NULL)
	    {
	      or_free_classrep (repr);
	    }

	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CT_UNKNOWN_REPRID, 1, reprid);
	  return NULL;
	}

      cache_entry->repr[reprid] = repr;
      cache_entry->last_reprid = last_reprid;

      if (reprid != last_reprid)
	{			/* if last repr is not cached */
	  cache_entry->repr[last_reprid] =
	    heap_classrepr_get_from_record (thread_p, &last_reprid, class_oid, class_recdes, last_reprid);
	}

      cache_entry->fcnt = 1;
      cache_entry->class_oid = *class_oid;
#ifdef DEBUG_CLASSREPR_CACHE
      r = pthread_mutex_lock (&heap_Classrepr_cache.num_fix_entries_mutex);
      heap_Classrepr_cache.num_fix_entries++;
      pthread_mutex_unlock (&heap_Classrepr_cache.num_fix_entries_mutex);

#endif /* DEBUG_CLASSREPR_CACHE */
      *idx_incache = cache_entry->idx;

      /* Add to hash chain, and remove lock for class_oid */
      r = pthread_mutex_lock (&hash_anchor->hash_mutex);
      cache_entry->hash_next = hash_anchor->hash_next;
      hash_anchor->hash_next = cache_entry;

#ifdef SERVER_MODE
      (void) heap_classrepr_unlock_class (hash_anchor, class_oid, false);
#endif
    }
  else
    {
      /* now, we have already cache_entry for class_oid. if it contains repr info for reprid, return it. else load
       * classrepr info for it */

      if (reprid == NULL_REPRID)
	{
	  reprid = cache_entry->last_reprid;
	}

      if (reprid <= NULL_REPRID || reprid > cache_entry->last_reprid || reprid > cache_entry->max_reprid)
	{
	  assert (false);

	  pthread_mutex_unlock (&cache_entry->mutex);

	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CT_UNKNOWN_REPRID, 1, reprid);
	  return NULL;
	}

      /* reprid cannot be greater than cache_entry->last_reprid. */
      repr = cache_entry->repr[reprid];
      if (repr == NULL)
	{
	  /* load repr. info. for reprid of class_oid */
	  repr = heap_classrepr_get_from_record (thread_p, &last_reprid, class_oid, class_recdes, reprid);
	  if (repr == NULL)
	    {
	      pthread_mutex_unlock (&cache_entry->mutex);
	      return NULL;
	    }

	  cache_entry->repr[reprid] = repr;
	}

      cache_entry->fcnt++;
      *idx_incache = cache_entry->idx;
    }
  pthread_mutex_unlock (&cache_entry->mutex);

  return repr;
}

#ifdef DEBUG_CLASSREPR_CACHE
/*
 * heap_classrepr_dump_cache () - Dump the class representation cache
 *   return: NO_ERROR
 *   simple_dump(in):
 *
 * Note: Dump the class representation cache.
 */
static int
heap_classrepr_dump_cache (bool simple_dump)
{
  OR_CLASSREP *classrepr;
  HEAP_CLASSREPR_ENTRY *cache_entry;
  int i, j;
  int rv;
  int ret = NO_ERROR;

  if (heap_Classrepr == NULL)
    {
      return NO_ERROR;		/* nop */
    }

  (void) fflush (stderr);
  (void) fflush (stdout);

  fprintf (stdout, "*** Class Representation cache dump *** \n");
  fprintf (stdout, " Number of entries = %d, Number of used entries = %d\n", heap_Classrepr->num_entries,
	   heap_Classrepr->num_entries - heap_Classrepr->free_list.free_cnt);

  for (cache_entry = heap_Classrepr->area, i = 0; i < heap_Classrepr->num_entries; cache_entry++, i++)
    {
      fprintf (stdout, " \nEntry_id %d\n", cache_entry->idx);

      rv = pthread_mutex_lock (&cache_entry->mutex);
      for (j = 0; j <= cache_entry->last_reprid; j++)
	{
	  classrepr = cache_entry->repr[j];
	  if (classrepr == NULL)
	    {
	      fprintf (stdout, ".....\n");
	      continue;
	    }
	  fprintf (stdout, " Fix count = %d, force_decache = %d\n", cache_entry->fcnt, cache_entry->force_decache);

	  if (simple_dump == true)
	    {
	      fprintf (stdout, " Class_oid = %d|%d|%d, Reprid = %d\n", (int) cache_entry->class_oid.volid,
		       cache_entry->class_oid.pageid, (int) cache_entry->class_oid.slotid, cache_entry->repr[j]->id);
	      fprintf (stdout, " Representation address = %p\n", classrepr);

	    }
	  else
	    {
	      ret = heap_classrepr_dump (&cache_entry->class_oid, classrepr);
	    }
	}

      pthread_mutex_unlock (&cache_entry->mutex);
    }

  return ret;
}
#endif /* DEBUG_CLASSREPR_CACHE */

/*
 * heap_classrepr_dump () - Dump schema of a given class representation
 *   return: NO_ERROR
 *   class_oid(in):
 *   repr(in): The class representation
 *
 * Note: Dump the class representation cache.
 */
static int
heap_classrepr_dump (THREAD_ENTRY * thread_p, FILE * fp, const OID * class_oid, const OR_CLASSREP * repr)
{
  OR_ATTRIBUTE *volatile attrepr;
  volatile int i;
  int k, j;
  char *classname;
  const char *attr_name;
  DB_VALUE def_dbvalue;
  PR_TYPE *pr_type;
  int disk_length;
  OR_BUF buf;
  bool copy;
  RECDES recdes;		/* Used to obtain attrnames */
  int ret = NO_ERROR;
  char *index_name = NULL;

  /* 
   * The class is feteched to print the attribute names.
   *
   * This is needed since the name of the attributes is not contained
   * in the class representation structure.
   */

  recdes.data = NULL;
  recdes.area_size = 0;

  if (repr == NULL)
    {
      goto exit_on_error;
    }

  if (heap_get_alloc (thread_p, class_oid, &recdes) != NO_ERROR)
    {
      goto exit_on_error;
    }

  classname = heap_get_class_name (thread_p, class_oid);
  if (classname == NULL)
    {
      goto exit_on_error;
    }

  fprintf (fp, "\n");
  fprintf (fp,
	   " Class-OID = %d|%d|%d, Classname = %s, reprid = %d,\n"
	   " Attrs: Tot = %d, Nfix = %d, Nvar = %d, Nshare = %d, Nclass = %d,\n Total_length_of_fixattrs = %d\n",
	   (int) class_oid->volid, class_oid->pageid, (int) class_oid->slotid, classname, repr->id, repr->n_attributes,
	   (repr->n_attributes - repr->n_variable - repr->n_shared_attrs - repr->n_class_attrs), repr->n_variable,
	   repr->n_shared_attrs, repr->n_class_attrs, repr->fixed_length);
  free_and_init (classname);

  if (repr->n_attributes > 0)
    {
      fprintf (fp, "\n");
      fprintf (fp, " Attribute Specifications:\n");
    }

  for (i = 0, attrepr = repr->attributes; i < repr->n_attributes; i++, attrepr++)
    {

      attr_name = or_get_attrname (&recdes, attrepr->id);
      if (attr_name == NULL)
	{
	  attr_name = "?????";
	}

      fprintf (fp, "\n Attrid = %d, Attrname = %s, type = %s,\n location = %d, position = %d,\n", attrepr->id,
	       attr_name, pr_type_name (attrepr->type), attrepr->location, attrepr->position);

      if (!OID_ISNULL (&attrepr->classoid) && !OID_EQ (&attrepr->classoid, class_oid))
	{
	  classname = heap_get_class_name (thread_p, &attrepr->classoid);
	  if (classname == NULL)
	    {
	      goto exit_on_error;
	    }
	  fprintf (fp, " Inherited from Class: oid = %d|%d|%d, Name = %s\n", (int) attrepr->classoid.volid,
		   attrepr->classoid.pageid, (int) attrepr->classoid.slotid, classname);
	  free_and_init (classname);
	}

      if (attrepr->n_btids > 0)
	{
	  fprintf (fp, " Number of Btids = %d,\n", attrepr->n_btids);
	  for (k = 0; k < attrepr->n_btids; k++)
	    {
	      index_name = NULL;
	      /* find index_name */
	      for (j = 0; j < repr->n_indexes; ++j)
		{
		  if (BTID_IS_EQUAL (&(repr->indexes[j].btid), &(attrepr->btids[k])))
		    {
		      index_name = repr->indexes[j].btname;
		      break;
		    }
		}

	      fprintf (fp, " BTID: VFID %d|%d, Root_PGID %d, %s\n", (int) attrepr->btids[k].vfid.volid,
		       attrepr->btids[k].vfid.fileid, attrepr->btids[k].root_pageid,
		       (index_name == NULL) ? "unknown" : index_name);
	    }
	}

      /* 
       * Dump the default value if any.
       */
      fprintf (fp, " Default disk value format:\n");
      fprintf (fp, "   length = %d, value = ", attrepr->default_value.val_length);

      if (attrepr->default_value.val_length <= 0)
	{
	  fprintf (fp, "NULL");
	}
      else
	{
	  or_init (&buf, (char *) attrepr->default_value.value, attrepr->default_value.val_length);
	  buf.error_abort = 1;

	  switch (_setjmp (buf.env))
	    {
	    case 0:
	      /* Do not copy the string--just use the pointer.  The pr_ routines for strings and sets have different
	       * semantics for length. A negative length value for strings means "don't copy the string, just use the
	       * pointer". */

	      disk_length = attrepr->default_value.val_length;
	      copy = (pr_is_set_type (attrepr->type)) ? true : false;
	      pr_type = PR_TYPE_FROM_ID (attrepr->type);
	      if (pr_type)
		{
		  (*(pr_type->data_readval)) (&buf, &def_dbvalue, attrepr->domain, disk_length, copy, NULL, 0);

		  db_value_fprint (stdout, &def_dbvalue);
		  (void) pr_clear_value (&def_dbvalue);
		}
	      else
		{
		  fprintf (fp, "PR_TYPE is NULL");
		}
	      break;
	    default:
	      /* 
	       * An error was found during the reading of the attribute value
	       */
	      fprintf (fp, "Error transforming the default value\n");
	      break;
	    }
	}
      fprintf (fp, "\n");
    }

  free_and_init (recdes.data);

  return ret;

exit_on_error:

  if (recdes.data)
    {
      free_and_init (recdes.data);
    }

  fprintf (fp, "Dump has been aborted...");

  return (ret == NO_ERROR) ? ER_FAILED : ret;
}

#ifdef DEBUG_CLASSREPR_CACHE
/*
 * heap_classrepr_dump_anyfixed() - Dump class representation cache if
 *                                   any entry is fixed
 *   return: NO_ERROR
 *
 * Note: The class representation cache is dumped if any cache entry is fixed
 *
 * This is a debugging function that can be used to verify if
 * entries were freed after a set of operations (e.g., a
 * transaction or a API function).
 *
 * Note:
 * This function will not give you good results when there are
 * multiple users in the system (multiprocessing). However, it
 * can be used during shuttdown.
 */
int
heap_classrepr_dump_anyfixed (void)
{
  int ret = NO_ERROR;

  if (heap_Classrepr->num_fix_entries > 0)
    {
      er_log_debug (ARG_FILE_LINE, "heap_classrepr_dump_anyfixed: Some entries are fixed\n");
      ret = heap_classrepr_dump_cache (true);
    }

  return ret;
}
#endif /* DEBUG_CLASSREPR_CACHE */

/*
 * heap_stats_get_min_freespace () - Minimal space to consider a page for statistics
 *   return: int minspace
 *   heap_hdr(in): Current header of heap
 *
 * Note: Find the minimal space to consider to continue caching a page
 * for statistics.
 */
static int
heap_stats_get_min_freespace (HEAP_HDR_STATS * heap_hdr)
{
  int min_freespace;
  int header_size;

  header_size = OR_MVCC_MAX_HEADER_SIZE;

  /* 
   * Don't cache as a good space page if page does not have at least
   * unfill_space + one record
   */

  if (heap_hdr->estimates.num_recs > 0)
    {
      min_freespace = (int) (heap_hdr->estimates.recs_sumlen / heap_hdr->estimates.num_recs);

      if (min_freespace < (header_size + 20))
	{
	  min_freespace = header_size + 20;	/* Assume very small records */
	}
    }
  else
    {
      min_freespace = header_size + 20;	/* Assume very small records */
    }

  min_freespace += heap_hdr->unfill_space;

  min_freespace = MIN (min_freespace, HEAP_DROP_FREE_SPACE);

  return min_freespace;
}

/*
 * heap_stats_update () - Update one header hinted page space statistics
 *   return: NO_ERROR
 *   pgptr(in): Page pointer
 *   hfid(in): Object heap file identifier
 *   prev_freespace(in):
 *
 * NOTE: There should be at least HEAP_DROP_FREE_SPACE in order to
 *       insert this page to best hint array.
 *       If we cannot fix a heap header page due to holding it by
 *       others, we will postpone this updating until next deletion.
 *       In this case, unfortunately, if some record is not deleted
 *       from this page in the future, we may not use this page until
 *       heap_stats_sync_bestspace function searches all pages.
 */
void
heap_stats_update (THREAD_ENTRY * thread_p, PAGE_PTR pgptr, const HFID * hfid, int prev_freespace)
{
  VPID *vpid;
  int freespace, error;
  bool need_update;

  freespace = spage_get_free_space_without_saving (thread_p, pgptr, &need_update);
  if (prm_get_integer_value (PRM_ID_HF_MAX_BESTSPACE_ENTRIES) > 0)
    {
      if (prev_freespace < freespace)
	{
	  vpid = pgbuf_get_vpid_ptr (pgptr);
	  assert_release (vpid != NULL);

	  (void) heap_stats_add_bestspace (thread_p, hfid, vpid, freespace);
	}
    }

  if (need_update || prev_freespace <= HEAP_DROP_FREE_SPACE)
    {
      if (freespace > HEAP_DROP_FREE_SPACE)
	{
	  vpid = pgbuf_get_vpid_ptr (pgptr);
	  assert_release (vpid != NULL);

	  error = heap_stats_update_internal (thread_p, hfid, vpid, freespace);
	  if (error != NO_ERROR)
	    {
	      spage_set_need_update_best_hint (thread_p, pgptr, true);
	    }
	  else if (need_update == true)
	    {
	      spage_set_need_update_best_hint (thread_p, pgptr, false);
	    }
	}
      else if (need_update == true)
	{
	  spage_set_need_update_best_hint (thread_p, pgptr, false);
	}
    }
}

/*
 * heap_stats_update_internal () - Update one header hinted page space statistics
 *   return: NO_ERROR
 *   hfid(in): Object heap file identifier
 *   lotspace_vpid(in): Page which has a lot of free space
 *   free_space(in): The free space on the page
 *
 * Note: Update header hinted best space page information. This
 * function is used during deletions and updates when the free
 * space on the page is greater than HEAP_DROP_FREE_SPACE.
 */
static int
heap_stats_update_internal (THREAD_ENTRY * thread_p, const HFID * hfid, VPID * lotspace_vpid, int free_space)
{
  HEAP_HDR_STATS *heap_hdr;	/* Header of heap structure */
  PAGE_PTR hdr_pgptr = NULL;	/* Page pointer to header page */
  VPID vpid;			/* Page-volume identifier */
  RECDES recdes;		/* Header record descriptor */
  LOG_DATA_ADDR addr;		/* Address of logging data */
  int i, best;
  int ret = NO_ERROR;

  /* Retrieve the header of heap */
  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;

  /* 
   * We do not want to wait for the following operation.
   * So, if we cannot lock the page return.
   */
  hdr_pgptr = pgbuf_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_WRITE, PGBUF_CONDITIONAL_LATCH);
  if (hdr_pgptr == NULL)
    {
      /* Page is busy or other type of error */
      goto exit_on_error;
    }

  (void) pgbuf_check_page_ptype (thread_p, hdr_pgptr, PAGE_HEAP);

  /* 
   * Peek the header record to find statistics for insertion.
   * Update the statistics directly.
   */
  if (spage_get_record (hdr_pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, PEEK) != S_SUCCESS)
    {
      goto exit_on_error;
    }

  heap_hdr = (HEAP_HDR_STATS *) recdes.data;
  best = heap_hdr->estimates.head;

  if (free_space >= heap_stats_get_min_freespace (heap_hdr))
    {
      /* 
       * We do not compare with the current stored values since these values
       * may not be accurate at all. When the given one is supposed to be
       * accurate.
       */

      /* 
       * Find a good place to insert this page
       */
      for (i = 0; i < HEAP_NUM_BEST_SPACESTATS; i++)
	{
	  if (VPID_ISNULL (&heap_hdr->estimates.best[best].vpid)
	      || heap_hdr->estimates.best[best].freespace <= HEAP_DROP_FREE_SPACE)
	    {
	      break;
	    }

	  best = HEAP_STATS_NEXT_BEST_INDEX (best);
	}

      if (VPID_ISNULL (&heap_hdr->estimates.best[best].vpid))
	{
	  heap_hdr->estimates.num_high_best++;
	  assert (heap_hdr->estimates.num_high_best <= HEAP_NUM_BEST_SPACESTATS);
	}
      else if (heap_hdr->estimates.best[best].freespace > HEAP_DROP_FREE_SPACE)
	{
	  heap_hdr->estimates.num_other_high_best++;

	  heap_stats_put_second_best (heap_hdr, &heap_hdr->estimates.best[best].vpid);
	}
      /* 
       * Now substitute the entry with the new information
       */

      heap_hdr->estimates.best[best].freespace = free_space;
      heap_hdr->estimates.best[best].vpid = *lotspace_vpid;

      heap_hdr->estimates.head = HEAP_STATS_NEXT_BEST_INDEX (best);

      /* 
       * The changes to the statistics are not logged. They are fixed
       * automatically sooner or later
       */

      addr.vfid = &hfid->vfid;
      addr.pgptr = hdr_pgptr;
      addr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;
      log_skip_logging (thread_p, &addr);
      pgbuf_set_dirty (thread_p, hdr_pgptr, FREE);
      hdr_pgptr = NULL;
    }
  else
    {
      pgbuf_unfix_and_init (thread_p, hdr_pgptr);
    }

  return ret;

exit_on_error:
  if (hdr_pgptr)
    {
      pgbuf_unfix_and_init (thread_p, hdr_pgptr);
    }

  return (ret == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_stats_put_second_best () - Put a free page into second best hint array
 *   return: void
 *   heap_hdr(in): Statistics of heap file
 *   vpid(in): VPID to be added
 *
 * NOTE: A free page is not always inserted to the second best hint array.
 *       Second best hints will be collected for every 1000 pages in order
 *       to increase randomness for "emptying contiguous pages" scenario.
 */
static void
heap_stats_put_second_best (HEAP_HDR_STATS * heap_hdr, VPID * vpid)
{
  int tail;

  if (heap_hdr->estimates.num_substitutions++ % 1000 == 0)
    {
      tail = heap_hdr->estimates.tail_second_best;

      heap_hdr->estimates.second_best[tail] = *vpid;
      heap_hdr->estimates.tail_second_best = HEAP_STATS_NEXT_BEST_INDEX (tail);

      if (heap_hdr->estimates.num_second_best == HEAP_NUM_BEST_SPACESTATS)
	{
	  assert (heap_hdr->estimates.head_second_best == tail);
	  heap_hdr->estimates.head_second_best = heap_hdr->estimates.tail_second_best;
	}
      else
	{
	  assert (heap_hdr->estimates.num_second_best < HEAP_NUM_BEST_SPACESTATS);
	  heap_hdr->estimates.num_second_best++;
	}

      /* If both head and tail refer to the same index, the number of second best hints is
       * HEAP_NUM_BEST_SPACESTATS(10). */
      assert (heap_hdr->estimates.num_second_best != 0);
      assert ((heap_hdr->estimates.tail_second_best > heap_hdr->estimates.head_second_best)
	      ? ((heap_hdr->estimates.tail_second_best - heap_hdr->estimates.head_second_best)
		 == heap_hdr->estimates.num_second_best)
	      : ((10 + heap_hdr->estimates.tail_second_best - heap_hdr->estimates.head_second_best)
		 == heap_hdr->estimates.num_second_best));

      heap_hdr->estimates.num_substitutions = 1;
    }
}

/*
 * heap_stats_put_second_best () - Get a free page from second best hint array
 *   return: NO_ERROR or ER_FAILED
 *   heap_hdr(in): Statistics of heap file
 *   vpid(out): VPID to get
 */
static int
heap_stats_get_second_best (HEAP_HDR_STATS * heap_hdr, VPID * vpid)
{
  int head;

  assert (vpid != NULL);

  if (heap_hdr->estimates.num_second_best == 0)
    {
      assert (heap_hdr->estimates.tail_second_best == heap_hdr->estimates.head_second_best);
      VPID_SET_NULL (vpid);
      return ER_FAILED;
    }

  head = heap_hdr->estimates.head_second_best;

  heap_hdr->estimates.num_second_best--;
  heap_hdr->estimates.head_second_best = HEAP_STATS_NEXT_BEST_INDEX (head);

  /* If both head and tail refer to the same index, the number of second best hints is 0. */
  assert (heap_hdr->estimates.num_second_best != HEAP_NUM_BEST_SPACESTATS);
  assert ((heap_hdr->estimates.tail_second_best >= heap_hdr->estimates.head_second_best)
	  ? ((heap_hdr->estimates.tail_second_best - heap_hdr->estimates.head_second_best)
	     == heap_hdr->estimates.num_second_best)
	  : ((10 + heap_hdr->estimates.tail_second_best - heap_hdr->estimates.head_second_best)
	     == heap_hdr->estimates.num_second_best));

  *vpid = heap_hdr->estimates.second_best[head];
  return NO_ERROR;
}

#if defined(ENABLE_UNUSED_FUNCTION)
/*
 * heap_stats_quick_num_fit_in_bestspace () - Guess the number of unit_size entries that
 *                                  can fit in best space
 *   return: number of units
 *   bestspace(in): Array of best pages along with their freespace
 *                  (The freespace fields may be updated as a SIDE EFFECT)
 *   num_entries(in): Number of estimated entries in best space.
 *   unit_size(in): Units of this size
 *   unfill_space(in): Unfill space on the pages
 *
 * Note: Find the number of units of "unit_size" that can fit in
 * current betsspace.
 */
static int
heap_stats_quick_num_fit_in_bestspace (HEAP_BESTSPACE * bestspace, int num_entries, int unit_size, int unfill_space)
{
  int total_nunits = 0;
  int i;

  if (unit_size <= 0)
    {
      return ER_FAILED;
    }

  for (i = 0; i < num_entries; i++)
    {
      if ((bestspace[i].freespace - unfill_space) >= unit_size)
	{
	  /* 
	   * How many min_spaces can fit in this page
	   */
	  total_nunits += (bestspace[i].freespace - unfill_space) / unit_size;
	}
    }

  return total_nunits;
}
#endif

/*
 * heap_stats_find_page_in_bestspace () - Find a page within best space
 * 					  statistics with the needed space
 *   return: HEAP_FINDPSACE (found, not found, or error)
 *   hfid(in): Object heap file identifier
 *   bestspace(in): Array of best pages along with their freespace
 *                  (The freespace fields may be updated as a SIDE EFFECT)
 *   idx_badspace(in/out): An index into best space with no so good space.
 *   needed_space(in): The needed space.
 *   scan_cache(in): Scan cache if any
 *   pgptr(out): Best page with enough space or NULL
 *
 * Note: Search for a page within the best space cache which has the
 * needed space. The free space fields of best space cache along
 * with some other index information are updated (as a side
 * effect) as the best space cache is accessed.
 */
static HEAP_FINDSPACE
heap_stats_find_page_in_bestspace (THREAD_ENTRY * thread_p, const HFID * hfid, HEAP_BESTSPACE * bestspace,
				   int *idx_badspace, int record_length, int needed_space, HEAP_SCANCACHE * scan_cache,
				   PGBUF_WATCHER * pg_watcher)
{
#define BEST_PAGE_SEARCH_MAX_COUNT 100

  HEAP_FINDSPACE found;
  int old_wait_msecs;
  int notfound_cnt;
  HEAP_STATS_ENTRY *ent;
  HEAP_BESTSPACE best;
  int rc;
  int idx_worstspace;
  int i, best_array_index = -1;
  bool hash_is_available;
  bool best_hint_is_used;

  assert (PGBUF_IS_CLEAN_WATCHER (pg_watcher));

  /* 
   * If a page is busy, don't wait continue looking for other pages in our
   * statistics. This will improve some contentions on the heap at the
   * expenses of storage.
   */

  /* LK_FORCE_ZERO_WAIT doesn't set error when deadlock occurs */
  old_wait_msecs = xlogtb_reset_wait_msecs (thread_p, LK_FORCE_ZERO_WAIT);

  found = HEAP_FINDSPACE_NOTFOUND;
  notfound_cnt = 0;
  best_array_index = 0;
  hash_is_available = prm_get_integer_value (PRM_ID_HF_MAX_BESTSPACE_ENTRIES) > 0;

  while (found == HEAP_FINDSPACE_NOTFOUND)
    {
      best.freespace = -1;	/* init */
      best_hint_is_used = false;

      if (hash_is_available)
	{
	  rc = pthread_mutex_lock (&heap_Bestspace->bestspace_mutex);

	  while (notfound_cnt < BEST_PAGE_SEARCH_MAX_COUNT
		 && (ent = (HEAP_STATS_ENTRY *) mht_get2 (heap_Bestspace->hfid_ht, hfid, NULL)) != NULL)
	    {
	      if (ent->best.freespace >= needed_space)
		{
		  best = ent->best;
		  assert (best.freespace > 0 && best.freespace <= PGLENGTH_MAX);
		  break;
		}

	      /* remove in memory bestspace */
	      (void) mht_rem2 (heap_Bestspace->hfid_ht, &ent->hfid, ent, NULL, NULL);
	      (void) mht_rem (heap_Bestspace->vpid_ht, &ent->best.vpid, NULL, NULL);
	      (void) heap_stats_entry_free (thread_p, ent, NULL);
	      ent = NULL;

	      heap_Bestspace->num_stats_entries--;

	      mnt_hf_stats_bestspace_entries (thread_p, heap_Bestspace->num_stats_entries);

	      notfound_cnt++;
	    }

	  pthread_mutex_unlock (&heap_Bestspace->bestspace_mutex);
	}

      if (best.freespace == -1)
	{
	  /* Maybe PRM_ID_HF_MAX_BESTSPACE_ENTRIES <= 0 or There is no best space in heap_Bestspace hashtable. We will
	   * use bestspace hint in heap_header. */
	  while (best_array_index < HEAP_NUM_BEST_SPACESTATS)
	    {
	      if (bestspace[best_array_index].freespace >= needed_space)
		{
		  best.vpid = bestspace[best_array_index].vpid;
		  best.freespace = bestspace[best_array_index].freespace;
		  assert (best.freespace > 0 && best.freespace <= PGLENGTH_MAX);
		  best_hint_is_used = true;
		  break;
		}
	      best_array_index++;
	    }
	}

      if (best.freespace == -1)
	{
	  break;		/* not found, exit loop */
	}

      /* If page could not be fixed, we will interrogate er_errid () to see the error type. If an error is already
       * set, the interrogation will be corrupted.
       * Make sure an error is not set.
       */
      if (er_errid () != NO_ERROR)
	{
	  if (er_errid () == ER_INTERRUPTED)
	    {
	      /* interrupt arrives at any time */
	      break;
	    }
#if defined (SERVER_MODE)
	  assert (er_errid () == ER_INTERRUPTED);
#endif /* SERVER_MODE */
	  er_clear ();
	}

      pg_watcher->pgptr = heap_scan_pb_lock_and_fetch (thread_p, &best.vpid, OLD_PAGE, X_LOCK, scan_cache, pg_watcher);
      if (pg_watcher->pgptr == NULL)
	{
	  /* 
	   * Either we timeout and we want to continue in this case, or
	   * we have another kind of problem.
	   */
	  switch (er_errid ())
	    {
	    case NO_ERROR:
	      /* In case of latch-timeout in pgbuf_fix, the timeout error(ER_LK_PAGE_TIMEOUT) is not set, because lock
	       * wait time is LK_FORCE_ZERO_WAIT. So we will just continue to find another page. */
	      break;

	    case ER_INTERRUPTED:
	      found = HEAP_FINDSPACE_ERROR;
	      break;

	    default:
	      /* 
	       * Something went wrong, we are unable to fetch this page.
	       */
	      if (best_hint_is_used == true)

		{
		  assert (best_array_index < HEAP_NUM_BEST_SPACESTATS);
		  bestspace[best_array_index].freespace = 0;
		}
	      else
		{
		  (void) heap_stats_del_bestspace_by_vpid (thread_p, &best.vpid);
		}
	      found = HEAP_FINDSPACE_ERROR;

	      /* Do not allow unexpected errors. */
	      assert (false);
	      break;
	    }
	}
      else
	{
	  best.freespace = spage_max_space_for_new_record (thread_p, pg_watcher->pgptr);
	  if (best.freespace >= needed_space)
	    {
	      /* 
	       * Decrement by only the amount space needed by the caller. Don't
	       * include the unfill factor
	       */
	      best.freespace -= record_length + heap_Slotted_overhead;
	      found = HEAP_FINDSPACE_FOUND;
	    }

	  if (hash_is_available)
	    {
	      /* Add or refresh the free space of the page */
	      (void) heap_stats_add_bestspace (thread_p, hfid, &best.vpid, best.freespace);
	    }

	  if (best_hint_is_used == true)
	    {
	      assert (VPID_EQ (&best.vpid, &(bestspace[best_array_index].vpid)));
	      assert (best_array_index < HEAP_NUM_BEST_SPACESTATS);

	      bestspace[best_array_index].freespace = best.freespace;
	    }

	  if (found != HEAP_FINDSPACE_FOUND)
	    {
	      pgbuf_ordered_unfix (thread_p, pg_watcher);
	    }
	}

      if (found == HEAP_FINDSPACE_NOTFOUND)
	{
	  if (best_hint_is_used)
	    {
	      /* Increment best_array_index for next search */
	      best_array_index++;
	    }
	  else
	    {
	      notfound_cnt++;
	    }
	}
    }

  idx_worstspace = 0;
  for (i = 0; i < HEAP_NUM_BEST_SPACESTATS; i++)
    {
      /* find worst space in bestspace */
      if (bestspace[idx_worstspace].freespace > bestspace[i].freespace)
	{
	  idx_worstspace = i;
	}

      /* update bestspace of heap header page if found best page at memory hash table */
      if (best_hint_is_used == false && found == HEAP_FINDSPACE_FOUND && VPID_EQ (&best.vpid, &bestspace[i].vpid))
	{
	  bestspace[i].freespace = best.freespace;
	}
    }

  /* 
   * Set the idx_badspace to the index with the smallest free space
   * which may not be accurate. This is used for future lookups (where to
   * start) into the findbest space ring.
   */
  *idx_badspace = idx_worstspace;

  /* 
   * Reset back the timeout value of the transaction
   */
  (void) xlogtb_reset_wait_msecs (thread_p, old_wait_msecs);

  return found;
}

/*
 * heap_stats_find_best_page () - Find a page with the needed space.
 *   return: pointer to page with enough space or NULL
 *   hfid(in): Object heap file identifier
 *   needed_space(in): The minimal space needed
 *   isnew_rec(in): Are we inserting a new record to the heap ?
 *   newrec_size(in): Size of the new record
 *   scan_cache(in/out): Scan cache used to estimate the best space pages
 *
 * Note: Find a page among the set of best pages of the heap which has
 * the needed space. If we do not find any page, a new page is
 * allocated. The heap header and the scan cache may be updated
 * as a side effect to reflect more accurate space on some of the
 * set of best pages.
 */
static PAGE_PTR
heap_stats_find_best_page (THREAD_ENTRY * thread_p, const HFID * hfid, int needed_space, bool isnew_rec,
			   int newrec_size, HEAP_SCANCACHE * scan_cache, PGBUF_WATCHER * pg_watcher)
{
  VPID vpid;			/* Volume and page identifiers */
  LOG_DATA_ADDR addr_hdr;	/* Address of logging data */
  RECDES hdr_recdes;		/* Record descriptor to point to space statistics */
  HEAP_HDR_STATS *heap_hdr;	/* Heap header */
  VPID *hdr_vpidp;
  int total_space;
  int try_find, try_sync;
  int num_pages_found;
  float other_high_best_ratio;
  PGBUF_WATCHER hdr_page_watcher;
  int error_code = NO_ERROR;

  /* 
   * Try to use the space cache for as much information as possible to avoid
   * fetching and updating the header page a lot.
   */

  assert (scan_cache == NULL || scan_cache->cache_last_fix_page == false || scan_cache->page_watcher.pgptr == NULL);
  PGBUF_INIT_WATCHER (&hdr_page_watcher, PGBUF_ORDERED_HEAP_HDR, hfid);

  /* 
   * Get the heap header in exclusive mode since it is going to be changed.
   *
   * Note: to avoid any possibilities of deadlocks, I should not have any locks
   *       on the heap at this moment.
   *       That is, we must assume that locking the header of the heap in
   *       exclusive mode, the rest of the heap is locked.
   */

  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;

  addr_hdr.vfid = &hfid->vfid;
  addr_hdr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;

  error_code = pgbuf_ordered_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_WRITE, &hdr_page_watcher);
  if (error_code != NO_ERROR)
    {
      /* something went wrong. Unable to fetch header page */
      ASSERT_ERROR ();
      return NULL;
    }
  assert (hdr_page_watcher.pgptr != NULL);

  (void) pgbuf_check_page_ptype (thread_p, hdr_page_watcher.pgptr, PAGE_HEAP);

  if (spage_get_record (hdr_page_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes, PEEK) != S_SUCCESS)
    {
      assert (false);
      pgbuf_ordered_unfix (thread_p, &hdr_page_watcher);
      return NULL;
    }

  heap_hdr = (HEAP_HDR_STATS *) hdr_recdes.data;

  if (isnew_rec == true)
    {
      heap_hdr->estimates.num_recs += 1;
      if (newrec_size > DB_PAGESIZE)
	{
	  heap_hdr->estimates.num_pages += CEIL_PTVDIV (newrec_size, DB_PAGESIZE);
	}
    }
  heap_hdr->estimates.recs_sumlen += (float) newrec_size;

  assert (!heap_is_big_length (needed_space));
  /* Take into consideration the unfill factor for pages with objects */
  total_space = needed_space + heap_Slotted_overhead + heap_hdr->unfill_space;
  if (heap_is_big_length (total_space))
    {
      total_space = needed_space + heap_Slotted_overhead;
    }

  try_find = 0;
  while (true)
    {
      try_find++;
      assert (pg_watcher->pgptr == NULL);
      if (heap_stats_find_page_in_bestspace (thread_p, hfid, heap_hdr->estimates.best, &(heap_hdr->estimates.head),
					     needed_space, total_space, scan_cache, pg_watcher) == HEAP_FINDSPACE_ERROR)
	{
	  ASSERT_ERROR ();
	  assert (pg_watcher->pgptr == NULL);
	  pgbuf_ordered_unfix (thread_p, &hdr_page_watcher);
	  return NULL;
	}
      if (pg_watcher->pgptr != NULL)
	{
	  /* found the page */
	  break;
	}

      assert (hdr_page_watcher.page_was_unfixed == false);

      if (heap_hdr->estimates.num_other_high_best <= 0 || heap_hdr->estimates.num_pages <= 0)
	{
	  assert (heap_hdr->estimates.num_pages > 0);
	  other_high_best_ratio = 0;
	}
      else
	{
	  other_high_best_ratio =
	    (float) heap_hdr->estimates.num_other_high_best / (float) heap_hdr->estimates.num_pages;
	}

      if (try_find >= 2 || other_high_best_ratio < HEAP_BESTSPACE_SYNC_THRESHOLD)
	{
	  /* We stop to find free pages if: (1) we have tried to do it twice (2) it is first trying but we have no
	   * hints Regarding (2), we will find free pages by heap_stats_sync_bestspace only if we know that a free page 
	   * exists somewhere. and (num_other_high_best/total page) > HEAP_BESTSPACE_SYNC_THRESHOLD.
	   * num_other_high_best means the number of free pages existing somewhere in the heap file. */
	  break;
	}

      /* 
       * The followings will try to find free pages and fill best hints with them.
       */

      if (scan_cache != NULL)
	{
	  assert (HFID_EQ (hfid, &scan_cache->node.hfid));
	  assert (scan_cache->file_type != FILE_UNKNOWN_TYPE);
	}

      hdr_vpidp = pgbuf_get_vpid_ptr (hdr_page_watcher.pgptr);

      try_sync = 0;
      do
	{
	  try_sync++;
	  er_log_debug (ARG_FILE_LINE,
			"heap_stats_find_best_page: call heap_stats_sync_bestspace() "
			"hfid { vfid  { fileid %d volid %d } hpgid %d } hdr_vpid { pageid %d volid %d } "
			"scan_all %d ", hfid->vfid.fileid, hfid->vfid.volid, hfid->hpgid, hdr_vpidp->pageid,
			hdr_vpidp->volid, 0);

	  num_pages_found = heap_stats_sync_bestspace (thread_p, hfid, heap_hdr, hdr_vpidp, false, true);
	  if (num_pages_found < 0)
	    {
	      pgbuf_ordered_unfix (thread_p, &hdr_page_watcher);
	      ASSERT_ERROR ();
	      return NULL;
	    }
	}
      while (num_pages_found == 0 && try_sync <= 2);

      /* If we cannot find free pages, give up. */
      if (num_pages_found <= 0)
	{
	  break;
	}
    }

  if (pg_watcher->pgptr == NULL)
    {
      /* 
       * None of the best pages has the needed space, allocate a new page.
       * Set the head to the index with the smallest free space, which may not
       * be accurate.
       */
      pg_watcher->pgptr =
	heap_vpid_alloc (thread_p, hfid, hdr_page_watcher.pgptr, heap_hdr, total_space, scan_cache, pg_watcher);
      assert (pg_watcher->pgptr != NULL || er_errid () == ER_INTERRUPTED
	      || er_errid () == ER_FILE_NOT_ENOUGH_PAGES_IN_DATABASE);
    }

  addr_hdr.pgptr = hdr_page_watcher.pgptr;
  log_skip_logging (thread_p, &addr_hdr);
  pgbuf_ordered_set_dirty_and_free (thread_p, &hdr_page_watcher);

  return pg_watcher->pgptr;
}

/*
 * heap_stats_sync_bestspace () - Synchronize the statistics of best space
 *   return: the number of pages found
 *   hfid(in): Heap file identifier
 *   heap_hdr(in): Heap header (Heap header page should be acquired in
 *                 exclusive mode)
 *   hdr_vpid(in):
 *   scan_all(in): Scan the whole heap or stop after HEAP_NUM_BEST_SPACESTATS
 *                best pages have been found.
 *   can_cycle(in): True, it allows to go back to beginning of the heap.
 *                 FALSE, don't go back to beginning of the heap. FALSE is used
 *                 when it is known that there is not free space at the
 *                 beginning of heap. For example, it can be used when we
 *                 pre-allocate. pages
 *
 * Note: Synchronize for best space, so that we can reuse heap space as
 * much as possible.
 *
 * Note: This function does not do any logging.
 */
static int
heap_stats_sync_bestspace (THREAD_ENTRY * thread_p, const HFID * hfid, HEAP_HDR_STATS * heap_hdr, VPID * hdr_vpid,
			   bool scan_all, bool can_cycle)
{
  int i, best, num_high_best, num_other_best, start_pos;
  VPID vpid = { NULL_PAGEID, NULL_VOLID };
  VPID start_vpid = { NULL_PAGEID, NULL_VOLID };
  VPID next_vpid = { NULL_PAGEID, NULL_VOLID };
  VPID stopat_vpid = { NULL_PAGEID, NULL_VOLID };
  int num_pages = 0;
  int num_recs = 0;
  float recs_sumlen = 0.0;
  int free_space = 0;
  int min_freespace;
  int ret = NO_ERROR;
  int npages = 0, nrecords = 0, rec_length;
  int num_iterations = 0, max_iterations;
  HEAP_BESTSPACE *best_pages_hint_p;
  bool iterate_all = false;
  bool search_all = false;
  PGBUF_WATCHER pg_watcher;
  PGBUF_WATCHER old_pg_watcher;
#if defined (CUBRID_DEBUG)
  TSC_TICKS start_tick, end_tick;
  TSCTIMEVAL tv_diff;

  float elapsed;

  tsc_getticks (&start_tick);
#endif /* CUBRID_DEBUG */

  mnt_heap_stats_sync_bestspace (thread_p);
  PGBUF_INIT_WATCHER (&pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  PGBUF_INIT_WATCHER (&old_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);

  min_freespace = heap_stats_get_min_freespace (heap_hdr);

  best = 0;
  start_pos = -1;
  num_high_best = num_other_best = 0;

  if (scan_all != true)
    {
      if (prm_get_integer_value (PRM_ID_HF_MAX_BESTSPACE_ENTRIES) > 0)
	{
	  search_all = true;
	  start_pos = -1;
	  next_vpid = heap_hdr->estimates.full_search_vpid;
	}
      else
	{
	  if (heap_hdr->estimates.num_high_best > 0)
	    {
	      /* Use recently inserted one first. */
	      start_pos = HEAP_STATS_PREV_BEST_INDEX (heap_hdr->estimates.head);
	      for (i = 0; i < HEAP_NUM_BEST_SPACESTATS; i++)
		{
		  if (!VPID_ISNULL (&heap_hdr->estimates.best[start_pos].vpid))
		    {
		      next_vpid = heap_hdr->estimates.best[start_pos].vpid;
		      start_vpid = next_vpid;
		      break;
		    }

		  start_pos = HEAP_STATS_PREV_BEST_INDEX (start_pos);
		}
	    }
	  else
	    {
	      /* If there are hint pages in second best array, we will try to use it first. Otherwise, we will search
	       * all pages in the file. */
	      if (heap_hdr->estimates.num_second_best > 0)
		{
		  if (heap_stats_get_second_best (heap_hdr, &next_vpid) != NO_ERROR)
		    {
		      /* This should not be happened. */
		      assert (false);
		      search_all = true;
		    }
		}
	      else
		{
		  search_all = true;
		}

	      if (search_all == true)
		{
		  assert (VPID_ISNULL (&next_vpid));
		  next_vpid = heap_hdr->estimates.full_search_vpid;
		}

	      start_vpid = next_vpid;
	      start_pos = -1;
	    }
	}

      if (can_cycle == true)
	{
	  stopat_vpid = next_vpid;
	}
    }

  if (VPID_ISNULL (&next_vpid))
    {
      /* 
       * Start from beginning of heap due to lack of statistics.
       */
      next_vpid.volid = hfid->vfid.volid;
      next_vpid.pageid = hfid->hpgid;
      start_vpid = next_vpid;
      start_pos = -1;
      can_cycle = false;
    }

  /* 
   * Note that we do not put any locks on the pages that we are scanning
   * since the best space array is only used for hints, and it is OK
   * if it is a little bit wrong.
   */
  best_pages_hint_p = heap_hdr->estimates.best;

  num_iterations = 0;
  max_iterations = MIN ((int) (heap_hdr->estimates.num_pages * 0.2), heap_Find_best_page_limit);
  max_iterations = MAX (max_iterations, HEAP_NUM_BEST_SPACESTATS);

  while (!VPID_ISNULL (&next_vpid) || can_cycle == true)
    {
      if (can_cycle == true && VPID_ISNULL (&next_vpid))
	{
	  /* 
	   * Go back to beginning of heap looking for good pages with a lot of
	   * free space
	   */
	  next_vpid.volid = hfid->vfid.volid;
	  next_vpid.pageid = hfid->hpgid;
	  can_cycle = false;
	}

      while ((scan_all == true || num_high_best < HEAP_NUM_BEST_SPACESTATS) && !VPID_ISNULL (&next_vpid)
	     && (can_cycle == true || !VPID_EQ (&next_vpid, &stopat_vpid)))
	{
	  if (scan_all == false)
	    {
	      if (++num_iterations > max_iterations)
		{
		  er_log_debug (ARG_FILE_LINE,
				"heap_stats_sync_bestspace: num_iterations %d best %d "
				"next_vpid { pageid %d volid %d }\n", num_iterations, num_high_best, next_vpid.pageid,
				next_vpid.volid);

		  /* TODO: Do we really need to update the last scanned */
		  /* in case we found less than 10 pages. */
		  /* It is obivous we didn't find any pages. */
		  if (start_pos != -1 && num_high_best == 0)
		    {
		      /* Delete a starting VPID. */
		      VPID_SET_NULL (&best_pages_hint_p[start_pos].vpid);
		      best_pages_hint_p[start_pos].freespace = 0;

		      heap_hdr->estimates.num_high_best--;
		    }
		  iterate_all = true;
		  break;
		}
	    }

	  vpid = next_vpid;
	  ret = pgbuf_ordered_fix (thread_p, &vpid, OLD_PAGE_PREVENT_DEALLOC, PGBUF_LATCH_READ, &pg_watcher);
	  if (ret != NO_ERROR)
	    {
	      break;
	    }
	  (void) pgbuf_check_page_ptype (thread_p, pg_watcher.pgptr, PAGE_HEAP);

	  if (old_pg_watcher.pgptr != NULL)
	    {
	      pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
	    }

	  ret = heap_vpid_next (hfid, pg_watcher.pgptr, &next_vpid);
	  if (ret != NO_ERROR)
	    {
	      assert (false);
	      pgbuf_ordered_unfix (thread_p, &pg_watcher);
	      break;
	    }
	  if (search_all)
	    {
	      /* Save the last position to be searched next time. */
	      heap_hdr->estimates.full_search_vpid = next_vpid;
	    }

	  spage_collect_statistics (pg_watcher.pgptr, &npages, &nrecords, &rec_length);

	  num_pages += npages;
	  num_recs += nrecords;
	  recs_sumlen += rec_length;

	  free_space = spage_max_space_for_new_record (thread_p, pg_watcher.pgptr);

	  if (free_space >= min_freespace && free_space > HEAP_DROP_FREE_SPACE)
	    {
	      if (prm_get_integer_value (PRM_ID_HF_MAX_BESTSPACE_ENTRIES) > 0)
		{
		  (void) heap_stats_add_bestspace (thread_p, hfid, &vpid, free_space);
		}

	      if (num_high_best < HEAP_NUM_BEST_SPACESTATS)
		{
		  best_pages_hint_p[best].vpid = vpid;
		  best_pages_hint_p[best].freespace = free_space;

		  best = HEAP_STATS_NEXT_BEST_INDEX (best);
		  num_high_best++;
		}
	      else
		{
		  num_other_best++;
		}
	    }

	  pgbuf_replace_watcher (thread_p, &pg_watcher, &old_pg_watcher);
	}

      assert (pg_watcher.pgptr == NULL);
      if (old_pg_watcher.pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
	}

      if (scan_all == false
	  && (iterate_all == true || num_high_best == HEAP_NUM_BEST_SPACESTATS
	      || (can_cycle == false && VPID_EQ (&next_vpid, &stopat_vpid))))
	{
	  break;
	}

      VPID_SET_NULL (&next_vpid);
    }

  er_log_debug (ARG_FILE_LINE,
		"heap_stats_sync_bestspace: scans from {%d|%d} to {%d|%d}, num_iterations(%d) "
		"max_iterations(%d) num_high_best(%d)\n", start_vpid.volid, start_vpid.pageid, vpid.volid, vpid.pageid,
		num_iterations, max_iterations, num_high_best);

  /* If we have scanned all pages, we should update all statistics even if we have not found any hints. This logic is
   * used to handle "select count(*) from table". */
  if (scan_all == false && num_high_best == 0 && heap_hdr->estimates.num_second_best == 0)
    {
      return 0;
    }

  if (num_high_best < HEAP_NUM_BEST_SPACESTATS)
    {
      for (i = best; i < HEAP_NUM_BEST_SPACESTATS; i++)
	{
	  VPID_SET_NULL (&best_pages_hint_p[i].vpid);
	  best_pages_hint_p[i].freespace = 0;
	}
    }

  heap_hdr->estimates.head = best;	/* reinit */
  heap_hdr->estimates.num_high_best = num_high_best;
  assert (heap_hdr->estimates.head >= 0 && heap_hdr->estimates.head < HEAP_NUM_BEST_SPACESTATS
	  && heap_hdr->estimates.num_high_best <= HEAP_NUM_BEST_SPACESTATS);

  if (scan_all == true || heap_hdr->estimates.num_pages <= num_pages)
    {
      /* 
       * We scan the whole heap.
       * Reset its statistics with new found statistics
       */
      heap_hdr->estimates.num_other_high_best = num_other_best;
      heap_hdr->estimates.num_pages = num_pages;
      heap_hdr->estimates.num_recs = num_recs;
      heap_hdr->estimates.recs_sumlen = recs_sumlen;
    }
  else
    {
      /* 
       * We did not scan the whole heap.
       * We reset only some of its statistics since we do not have any idea
       * which ones are better the ones that are currently recorded or the ones
       * just found.
       */
      heap_hdr->estimates.num_other_high_best -= heap_hdr->estimates.num_high_best;

      if (heap_hdr->estimates.num_other_high_best < num_other_best)
	{
	  heap_hdr->estimates.num_other_high_best = num_other_best;
	}

      if (num_recs > heap_hdr->estimates.num_recs || recs_sumlen > heap_hdr->estimates.recs_sumlen)
	{
	  heap_hdr->estimates.num_pages = num_pages;
	  heap_hdr->estimates.num_recs = num_recs;
	  heap_hdr->estimates.recs_sumlen = recs_sumlen;
	}
    }

#if defined (CUBRID_DEBUG)
  tsc_getticks (&end_tick);
  tsc_elapsed_time_usec (&tv_diff, end_tick, start_tick);
  elapsed = (float) tv_diff.tv_sec * 1000000;
  elapsed += (float) tv_diff.tv_usec;
  elapsed /= 1000000;

  er_log_debug (ARG_FILE_LINE, "heap_stats_sync_bestspace: elapsed time %.6f", elapsed);
#endif /* CUBRID_DEBUG */

  return num_high_best;
}

/*
 * heap_get_last_page () - Get the last page pointer.
 *   return: PAGE_PTR
 *   hfid(in): Object heap file identifier
 *   heap_hdr(in): The heap header structure
 *   scan_cache(in): Scan cache
 *   last_vpid(out): VPID of the last page
 *
 * Note: The last vpid is saved on heap header. But we do not write log
 *       related to it. So, if the server stops unintentionally, heap header
 *       may have a wrong value of last vpid. This function will protect it.
 *
 *       The page pointer should be unfixed by the caller.
 *
 */
static PAGE_PTR
heap_get_last_page (THREAD_ENTRY * thread_p, const HFID * hfid, HEAP_HDR_STATS * heap_hdr, HEAP_SCANCACHE * scan_cache,
		    VPID * last_vpid, PGBUF_WATCHER * pg_watcher)
{
  RECDES recdes;
  HEAP_CHAIN *chain;

  assert (pg_watcher != NULL);
  assert (last_vpid != NULL);
  assert (!VPID_ISNULL (&heap_hdr->estimates.last_vpid));

  *last_vpid = heap_hdr->estimates.last_vpid;

  pg_watcher->pgptr = heap_scan_pb_lock_and_fetch (thread_p, last_vpid, OLD_PAGE, X_LOCK, scan_cache, pg_watcher);
  if (pg_watcher->pgptr == NULL)
    {
      goto exit_on_error;
    }

  if (spage_get_record (pg_watcher->pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, PEEK) != S_SUCCESS)
    {
      assert (false);
      goto exit_on_error;
    }

  chain = (HEAP_CHAIN *) recdes.data;
  if (!VPID_ISNULL (&chain->next_vpid))
    {
      /* 
       * Hint is wrong! We should update last_vpid on heap header.
       */
      pgbuf_ordered_unfix (thread_p, pg_watcher);

      if (file_find_last_page (thread_p, &hfid->vfid, last_vpid) == NULL)
	{
	  goto exit_on_error;
	}

      er_log_debug (ARG_FILE_LINE, "heap_get_last_page: update last vpid from %d|%d to %d|%d\n",
		    heap_hdr->estimates.last_vpid.volid, heap_hdr->estimates.last_vpid.pageid, last_vpid->volid,
		    last_vpid->pageid);

      heap_hdr->estimates.last_vpid = *last_vpid;

      /* 
       * Fix a real last page.
       */
      pg_watcher->pgptr = heap_scan_pb_lock_and_fetch (thread_p, last_vpid, OLD_PAGE, X_LOCK, scan_cache, pg_watcher);
      if (pg_watcher->pgptr == NULL)
	{
	  goto exit_on_error;
	}
    }

  assert (!VPID_ISNULL (last_vpid));
  assert (pg_watcher->pgptr != NULL);

  return pg_watcher->pgptr;

exit_on_error:

  if (pg_watcher->pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, pg_watcher);
    }

  VPID_SET_NULL (last_vpid);

  return NULL;
}

/*
 * heap_link_to_new () - Chain previous last page to new page
 *   return: bool
 *   vfid(in): File where the new page belongs
 *   new_vpid(in): The new page
 *   link(in): Specifications of previous and header page
 *
 * Note: Link previous page with newly created page.
 */
static bool
heap_link_to_new (THREAD_ENTRY * thread_p, const VFID * vfid, const VPID * new_vpid, HEAP_CHAIN_TOLAST * link)
{
  LOG_DATA_ADDR addr;
  HEAP_CHAIN chain;
  RECDES recdes;
  int sp_success;

  /* 
   * Now, Previous page should point to newly allocated page
   */

  /* 
   * Update chain next field of previous last page
   * If previous best1 space page is the heap header page, it contains a heap
   * header instead of a chain.
   */

  addr.vfid = vfid;
  addr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;

  if (link->hdr_pgptr == link->last_pgptr)
    {
      /* 
       * Previous last page is the heap header page. Update the next_pageid
       * field.
       */

      addr.pgptr = link->hdr_pgptr;


      if (log_is_tran_in_system_op (thread_p) == true)
	{
	  log_append_undo_data (thread_p, RVHF_STATS, &addr, sizeof (*(link->heap_hdr)), link->heap_hdr);
	}

      link->heap_hdr->next_vpid = *new_vpid;

      log_append_redo_data (thread_p, RVHF_STATS, &addr, sizeof (*(link->heap_hdr)), link->heap_hdr);

      pgbuf_set_dirty (thread_p, link->hdr_pgptr, DONT_FREE);
    }
  else
    {
      /* 
       * Chain the old page to the newly allocated last page.
       */

      addr.pgptr = link->last_pgptr;

      recdes.area_size = recdes.length = sizeof (chain);
      recdes.type = REC_HOME;
      recdes.data = (char *) &chain;

      /* Get the chain record and put it in recdes...which points to chain */

      if (spage_get_record (addr.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, COPY) != S_SUCCESS)
	{
	  /* Unable to obtain chain record */
	  return false;		/* Initialization has failed */
	}


      if (log_is_tran_in_system_op (thread_p) == true)
	{
	  log_append_undo_data (thread_p, RVHF_CHAIN, &addr, recdes.length, recdes.data);
	}

      chain.next_vpid = *new_vpid;
      sp_success = spage_update (thread_p, addr.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes);
      if (sp_success != SP_SUCCESS)
	{
	  /* 
	   * This looks like a system error: size did not change, so why did
	   * it fail?
	   */
	  if (sp_success != SP_ERROR)
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_CANNOT_UPDATE_CHAIN_HDR, 4,
		      pgbuf_get_volume_id (addr.pgptr), pgbuf_get_page_id (addr.pgptr), vfid->fileid,
		      pgbuf_get_volume_label (addr.pgptr));
	    }
	  return false;		/* Initialization has failed */
	}

      log_append_redo_data (thread_p, RVHF_CHAIN, &addr, recdes.length, recdes.data);
      pgbuf_set_dirty (thread_p, addr.pgptr, DONT_FREE);
    }

  return true;
}

/*
 * heap_vpid_init_new () - Initialize a newly allocated heap page
 *   return: bool
 *   vfid(in): File where the new page belongs
 *   file_type(in):
 *   new_vpid(in): The new page
 *   ignore_npages(in): Number of contiguous allocated pages
 *                      (Ignored in this function. We allocate only one page)
 *   xlink(in): Chain to next and previous page
 */
static bool
heap_vpid_init_new (THREAD_ENTRY * thread_p, const VFID * vfid, const FILE_TYPE file_type, const VPID * new_vpid,
		    INT32 ignore_npages, void *xlink)
{
  LOG_DATA_ADDR addr;
  HEAP_CHAIN_TOLAST *link;
  HEAP_CHAIN chain;
  RECDES recdes;
  INT16 slotid;
  int sp_success;

  link = (HEAP_CHAIN_TOLAST *) xlink;

  assert (link->heap_hdr != NULL);

  addr.vfid = vfid;
  addr.offset = -1;		/* No header slot is initialized */

  /* 
   * fetch and initialize the new page. This page should point to previous
   * page.
   */

  addr.pgptr = pgbuf_fix (thread_p, new_vpid, NEW_PAGE, PGBUF_LATCH_WRITE, PGBUF_UNCONDITIONAL_LATCH);
  if (addr.pgptr == NULL)
    {
      return false;		/* Initialization has failed */
    }

  (void) pgbuf_set_page_ptype (thread_p, addr.pgptr, PAGE_HEAP);

  /* Initialize the page and chain it with the previous last allocated page */
  spage_initialize (thread_p, addr.pgptr, heap_get_spage_type (), HEAP_MAX_ALIGN, SAFEGUARD_RVSPACE);

  /* 
   * Add a chain record.
   * Next to NULL and Prev to last allocated page
   */
  COPY_OID (&chain.class_oid, &(link->heap_hdr->class_oid));
  pgbuf_get_vpid (link->last_pgptr, &chain.prev_vpid);
  VPID_SET_NULL (&chain.next_vpid);
  chain.max_mvccid = MVCCID_NULL;
  chain.flags = 0;
  HEAP_PAGE_SET_VACUUM_STATUS (&chain, HEAP_PAGE_VACUUM_NONE);

  recdes.area_size = recdes.length = sizeof (chain);
  recdes.type = REC_HOME;
  recdes.data = (char *) &chain;

  sp_success = spage_insert (thread_p, addr.pgptr, &recdes, &slotid);
  if (sp_success != SP_SUCCESS || slotid != HEAP_HEADER_AND_CHAIN_SLOTID)
    {
      /* 
       * Initialization has failed !!
       */
      if (sp_success != SP_SUCCESS)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	}
      pgbuf_unfix_and_init (thread_p, addr.pgptr);
      return false;		/* Initialization has failed */
    }

  /* 
   * We don't need to log before images for undos since allocation of pages is
   * an operation-destiny which does not depend on the transaction except for
   * newly created files. Pages may be shared by multiple concurrent
   * transactions, thus the deallocation cannot be undone. Note that new files
   * and their pages are deallocated when the transactions that create the
   * files are aborted.
   */

  log_append_redo_data (thread_p, heap_is_reusable_oid (file_type) ? RVHF_NEWPAGE_REUSE_OID : RVHF_NEWPAGE, &addr,
			recdes.length, recdes.data);
  pgbuf_set_dirty (thread_p, addr.pgptr, FREE);
  addr.pgptr = NULL;

  /* 
   * Now, link previous page to newly allocated page
   */
  return heap_link_to_new (thread_p, vfid, new_vpid, link);
}

/*
 * heap_vpid_alloc () - allocate, fetch, and initialize a new page
 *   return: ponter to newly allocated page or NULL
 *   hfid(in): Object heap file identifier
 *   hdr_pgptr(in): The heap page header
 *   heap_hdr(in): The heap header structure
 *   needed_space(in): The minimal space needed on new page
 *   scan_cache(in): Scan cache
 *
 * Note: Allocate and initialize a new heap page. The heap header is
 * updated to reflect a newly allocated best space page and
 * the set of best space pages information may be updated to
 * include the previous best1 space page.
 */
static PAGE_PTR
heap_vpid_alloc (THREAD_ENTRY * thread_p, const HFID * hfid, PAGE_PTR hdr_pgptr, HEAP_HDR_STATS * heap_hdr,
		 int needed_space, HEAP_SCANCACHE * scan_cache, PGBUF_WATCHER * new_pg_watcher)
{
  VPID vpid;			/* Volume and page identifiers */
  LOG_DATA_ADDR addr;		/* Address of logging data */
  int best;
  HEAP_CHAIN_TOLAST tolast;
  VPID last_vpid;
  PGBUF_WATCHER last_pg_watcher;
  FILE_TYPE file_type;

  assert (PGBUF_IS_CLEAN_WATCHER (new_pg_watcher));

  PGBUF_INIT_WATCHER (&last_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  addr.vfid = &hfid->vfid;
  addr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;

  if (log_start_system_op (thread_p) == NULL)
    {
      return NULL;
    }

  last_pg_watcher.pgptr = heap_get_last_page (thread_p, hfid, heap_hdr, scan_cache, &last_vpid, &last_pg_watcher);
  if (last_pg_watcher.pgptr == NULL)
    {
      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);

      /* something went wrong, return error */
      return NULL;
    }
  assert (!VPID_ISNULL (&last_vpid));

  /* 
   * Now allocate a new page as close as possible to the last allocated page.
   * Note that a new page is allocated when the best1 space page in the
   * statistics is the actual last page on the heap.
   */

  /* 
   * Prepare initialization fields, so that current page will point to
   * previous page.
   */

  tolast.hdr_pgptr = hdr_pgptr;
  tolast.last_pgptr = last_pg_watcher.pgptr;
  tolast.heap_hdr = heap_hdr;

  if (file_alloc_pages_with_outer_sys_op (thread_p, &hfid->vfid, &vpid, 1, &last_vpid, &file_type, heap_vpid_init_new,
					  &tolast) == NULL)
    {
      /* Unable to allocate a new page */
      if (last_pg_watcher.pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, &last_pg_watcher);
	}

      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);

      return NULL;
    }

  if (last_pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &last_pg_watcher);
    }

  /* 
   * Now update header statistics for best1 space page.
   * The changes to the statistics are not logged.
   * They are fixed automatically sooner or later.
   */

  /* last page hint */
  heap_hdr->estimates.last_vpid = vpid;
  heap_hdr->estimates.num_pages++;

  best = heap_hdr->estimates.head;
  heap_hdr->estimates.head = HEAP_STATS_NEXT_BEST_INDEX (best);
  if (VPID_ISNULL (&heap_hdr->estimates.best[best].vpid))
    {
      heap_hdr->estimates.num_high_best++;
      assert (heap_hdr->estimates.num_high_best <= HEAP_NUM_BEST_SPACESTATS);
    }
  else
    {
      if (heap_hdr->estimates.best[best].freespace > HEAP_DROP_FREE_SPACE)
	{
	  heap_hdr->estimates.num_other_high_best++;
	  heap_stats_put_second_best (heap_hdr, &heap_hdr->estimates.best[best].vpid);
	}
    }

  heap_hdr->estimates.best[best].vpid = vpid;
  heap_hdr->estimates.best[best].freespace = DB_PAGESIZE;

  if (prm_get_integer_value (PRM_ID_HF_MAX_BESTSPACE_ENTRIES) > 0)
    {
      (void) heap_stats_add_bestspace (thread_p, hfid, &vpid, heap_hdr->estimates.best[best].freespace);
    }

  addr.pgptr = hdr_pgptr;
  log_skip_logging_set_lsa (thread_p, &addr);

  /* 
   * Note: we fetch the page as old since it was initialized during the
   * allocation by heap_vpid_init_new, therefore, we care about the current
   * content of the page.
   */
  new_pg_watcher->pgptr = heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, X_LOCK, scan_cache, new_pg_watcher);
  /* 
   * Even though an error is returned from heap_scan_pb_lock_and_fetch,
   * we will just return new_pgptr (maybe NULL)
   * and do not deallocate the newly added page.
   * Because file_alloc_page was committed with top operation.
   * The added page will be used later by other insert operation.
   */

  log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);

  return new_pg_watcher->pgptr;	/* new_pgptr is lock and fetch */
}

/*
 * heap_vpid_remove () - Deallocate a heap page
 *   return: rm_vpid on success or NULL on error
 *   hfid(in): Object heap file identifier
 *   heap_hdr(in): The heap header stats
 *   rm_vpid(in): Page to remove
 *
 * Note: The given page is removed from the heap. The linked list of heap
 * pages is updated to remove this page, and the heap header may
 * be updated if this page was part of the statistics.
 */
static VPID *
heap_vpid_remove (THREAD_ENTRY * thread_p, const HFID * hfid, HEAP_HDR_STATS * heap_hdr, VPID * rm_vpid)
{
  RECDES rm_recdes;		/* Record descriptor which holds the chain of the page to be removed */
  HEAP_CHAIN *rm_chain;		/* Chain information of the page to be removed */
  VPID vpid;			/* Real identifier of previous page */
  LOG_DATA_ADDR addr;		/* Log address of previous page */
  RECDES recdes;		/* Record descriptor to page header */
  HEAP_CHAIN chain;		/* Chain to next and prev page */
  int sp_success;
  int i;
  PGBUF_WATCHER rm_pg_watcher;
  PGBUF_WATCHER prev_pg_watcher;

  PGBUF_INIT_WATCHER (&rm_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  PGBUF_INIT_WATCHER (&prev_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);

  /* 
   * Make sure that this is not the header page since the header page cannot
   * be removed. If the header page is removed.. the heap is gone
   */

  if (rm_vpid->pageid == hfid->hpgid && rm_vpid->volid == hfid->vfid.volid)
    {
      er_log_debug (ARG_FILE_LINE, "heap_vpid_remove: Trying to remove header page = %d|%d of heap file = %d|%d|%d",
		    (int) rm_vpid->volid, rm_vpid->pageid, (int) hfid->vfid.volid, hfid->vfid.fileid, hfid->hpgid);
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      goto error;
    }

  /* Get the chain record */
  rm_pg_watcher.pgptr = heap_scan_pb_lock_and_fetch (thread_p, rm_vpid, OLD_PAGE, X_LOCK, NULL, &rm_pg_watcher);
  if (rm_pg_watcher.pgptr == NULL)
    {
      /* Look like a system error. Unable to obtain chain header record */
      goto error;
    }

  if (spage_get_record (rm_pg_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &rm_recdes, PEEK) != S_SUCCESS)
    {
      /* Look like a system error. Unable to obtain chain header record */
      goto error;
    }

  rm_chain = (HEAP_CHAIN *) rm_recdes.data;

  /* 
   * UPDATE PREVIOUS PAGE
   *
   * Update chain next field of previous last page
   * If previous page is the heap header page, it contains a heap header
   * instead of a chain.
   */

  vpid = rm_chain->prev_vpid;
  addr.vfid = &hfid->vfid;
  addr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;

  prev_pg_watcher.pgptr = heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, X_LOCK, NULL, &prev_pg_watcher);
  if (prev_pg_watcher.pgptr == NULL)
    {
      /* something went wrong, return */
      goto error;
    }

  if (rm_pg_watcher.page_was_unfixed)
    {
      /* TODO : unexpected: need to reconsider the algorithm, if this is an ordinary case */
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PB_UNEXPECTED_PAGE_REFIX, 4, rm_vpid->volid, rm_vpid->pageid,
	      vpid.volid, vpid.pageid);
      goto error;
    }

  /* 
   * Make sure that the page to be removed is not referenced on the heap
   * statistics
   */

  assert (heap_hdr != NULL);

  /* 
   * We cannot break in the following loop since a best page could be
   * duplicated
   */
  for (i = 0; i < HEAP_NUM_BEST_SPACESTATS; i++)
    {
      if (VPID_EQ (&heap_hdr->estimates.best[i].vpid, rm_vpid))
	{
	  VPID_SET_NULL (&heap_hdr->estimates.best[i].vpid);
	  heap_hdr->estimates.best[i].freespace = 0;
	  heap_hdr->estimates.head = i;
	}
    }

  if (VPID_EQ (&heap_hdr->estimates.last_vpid, rm_vpid))
    {
      /* If the page is the last page of the heap file, update the hint */
      heap_hdr->estimates.last_vpid = rm_chain->prev_vpid;
    }

  /* 
   * Is previous page the header page ?
   */
  if (vpid.pageid == hfid->hpgid && vpid.volid == hfid->vfid.volid)
    {
      /* 
       * PREVIOUS PAGE IS THE HEADER PAGE.
       * It contains a heap header instead of a chain record
       */
      heap_hdr->next_vpid = rm_chain->next_vpid;
    }
  else
    {
      /* 
       * PREVIOUS PAGE IS NOT THE HEADER PAGE.
       * It contains a chain...
       * We need to make sure that there is not references to the page to delete
       * in the statistics of the heap header
       */

      /* NOW check the PREVIOUS page */

      if (spage_get_record (prev_pg_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, PEEK) != S_SUCCESS)
	{
	  /* Look like a system error. Unable to obtain header record */
	  goto error;
	}

      /* Copy the chain record to memory.. so we can log the changes */
      memcpy (&chain, recdes.data, sizeof (chain));

      /* Modify the chain of the previous page in memory */
      chain.next_vpid = rm_chain->next_vpid;

      /* Get the chain record */
      recdes.area_size = recdes.length = sizeof (chain);
      recdes.type = REC_HOME;
      recdes.data = (char *) &chain;

      /* Log the desired changes.. and then change the header */
      addr.pgptr = prev_pg_watcher.pgptr;
      log_append_undoredo_data (thread_p, RVHF_CHAIN, &addr, sizeof (chain), sizeof (chain), recdes.data, &chain);

      /* Now change the record */

      sp_success = spage_update (thread_p, prev_pg_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes);
      if (sp_success != SP_SUCCESS)
	{
	  /* 
	   * This look like a system error, size did not change, so why did it
	   * fail
	   */
	  if (sp_success != SP_ERROR)
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	    }
	  goto error;
	}

    }

  /* Now set dirty, free and unlock the previous page */
  pgbuf_ordered_set_dirty_and_free (thread_p, &prev_pg_watcher);

  /* 
   * UPDATE NEXT PAGE
   *
   * Update chain previous field of next page
   */

  if (!(VPID_ISNULL (&rm_chain->next_vpid)))
    {
      vpid = rm_chain->next_vpid;
      addr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;

      prev_pg_watcher.pgptr = heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, X_LOCK, NULL, &prev_pg_watcher);
      if (prev_pg_watcher.pgptr == NULL)
	{
	  /* something went wrong, return */
	  goto error;
	}

      /* Get the chain record */
      if (spage_get_record (prev_pg_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, PEEK) != S_SUCCESS)
	{
	  /* Look like a system error. Unable to obtain header record */
	  goto error;
	}

      /* Copy the chain record to memory.. so we can log the changes */
      memcpy (&chain, recdes.data, sizeof (chain));

      /* Modify the chain of the next page in memory */
      chain.prev_vpid = rm_chain->prev_vpid;

      /* Log the desired changes.. and then change the header */
      addr.pgptr = prev_pg_watcher.pgptr;
      log_append_undoredo_data (thread_p, RVHF_CHAIN, &addr, sizeof (chain), sizeof (chain), recdes.data, &chain);

      /* Now change the record */
      recdes.area_size = recdes.length = sizeof (chain);
      recdes.type = REC_HOME;
      recdes.data = (char *) &chain;

      sp_success = spage_update (thread_p, prev_pg_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes);
      if (sp_success != SP_SUCCESS)
	{
	  /* 
	   * This look like a system error, size did not change, so why did it
	   * fail
	   */
	  if (sp_success != SP_ERROR)
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	    }
	  goto error;
	}

      /* Now set dirty, free and unlock the next page */

      pgbuf_ordered_set_dirty_and_free (thread_p, &prev_pg_watcher);
    }

  /* Free the page to be deallocated and deallocate the page */
  pgbuf_ordered_unfix (thread_p, &rm_pg_watcher);

  if (file_dealloc_page (thread_p, &hfid->vfid, rm_vpid, FILE_HEAP) != NO_ERROR)
    {
      goto error;
    }

  (void) heap_stats_del_bestspace_by_vpid (thread_p, rm_vpid);

  return rm_vpid;

error:
  if (rm_pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &rm_pg_watcher);
    }
  if (addr.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &prev_pg_watcher);
    }

  return NULL;
}

/*
 * heap_remove_page_on_vacuum () - Remove heap page from heap file during
 *				   vacuum process. Function is trying to
 *				   be as least intrusive as possible and all
 *				   required pages are latched conditionally.
 *				   Give up on any failed operation.
 *
 * return	 : True if page was deallocated, false if not.
 * thread_p (in) : Thread entry.
 * page_ptr (in) : Pointer to page being deallocated.
 * hfid (in)	 : Heap file identifier.
 */
bool
heap_remove_page_on_vacuum (THREAD_ENTRY * thread_p, PAGE_PTR * page_ptr, HFID * hfid)
{
  VPID page_vpid = VPID_INITIALIZER;	/* VPID of page being removed. */
  VPID prev_vpid = VPID_INITIALIZER;	/* VPID of previous page. */
  VPID next_vpid = VPID_INITIALIZER;	/* VPID of next page. */
  VPID header_vpid = VPID_INITIALIZER;	/* VPID of heap header page. */
  HEAP_HDR_STATS heap_hdr;	/* Header header & stats. */
  HEAP_CHAIN chain;		/* Heap page header used to read and update page links. */
  RECDES copy_recdes;		/* Record to copy header from pages. */
  /* Buffer used for copy record. */
  char copy_recdes_buffer[MAX (sizeof (HEAP_CHAIN), sizeof (HEAP_HDR_STATS)) + MAX_ALIGNMENT];
  RECDES update_recdes;		/* Record containing updated header data. */
  int i = 0;			/* Iterator. */
  bool is_system_op_started = false;	/* Set to true once system operation is started. */
  PGBUF_WATCHER crt_watcher;	/* Watcher for current page. */
  PGBUF_WATCHER header_watcher;	/* Watcher for header page. */
  PGBUF_WATCHER prev_watcher;	/* Watcher for previous page. */
  PGBUF_WATCHER next_watcher;	/* Watcher for next page. */

  /* Assert expected arguments. */
  /* Page to remove must be fixed. */
  assert (page_ptr != NULL && *page_ptr != NULL);
  /* Page to remove must be empty. */
  assert (spage_number_of_records (*page_ptr) <= 1);
  /* Heap file identifier must be known. */
  assert (hfid != NULL && !HFID_IS_NULL (hfid));

  /* Get VPID of page to be removed. */
  pgbuf_get_vpid (*page_ptr, &page_vpid);

  if (page_vpid.pageid == hfid->hpgid && page_vpid.volid == hfid->vfid.volid)
    {
      /* Cannot remove heap file header page. */
      return false;
    }

  /* Use page watchers to do the ordered fix. */
  PGBUF_INIT_WATCHER (&crt_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  PGBUF_INIT_WATCHER (&header_watcher, PGBUF_ORDERED_HEAP_HDR, hfid);
  PGBUF_INIT_WATCHER (&prev_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  PGBUF_INIT_WATCHER (&next_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);

  /* Current page is already fixed. Just attach watcher. */
  pgbuf_attach_watcher (thread_p, *page_ptr, PGBUF_LATCH_WRITE, hfid, &crt_watcher);

  /* Header vpid. */
  header_vpid.volid = hfid->vfid.volid;
  header_vpid.pageid = hfid->hpgid;

  /* Fix required pages: Heap header page. Previous page (always exists). Next page (if exists). */

  /* Fix header page first, because it has higher priority. */
  if (pgbuf_ordered_fix (thread_p, &header_vpid, OLD_PAGE, PGBUF_LATCH_WRITE, &header_watcher) != NO_ERROR)
    {
      /* Give up. */
      vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
		     "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
		     page_vpid.pageid);
      goto error;
    }
  assert (header_watcher.pgptr != NULL);

  /* Get previous and next page VPID's. */
  if (heap_vpid_prev (hfid, *page_ptr, &prev_vpid) != NO_ERROR
      || heap_vpid_next (hfid, *page_ptr, &next_vpid) != NO_ERROR)
    {
      /* Give up. */
      vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
		     "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
		     page_vpid.pageid);
      goto error;
    }

  /* Fix previous page if it is not same as header. */
  if (!VPID_ISNULL (&prev_vpid) && !VPID_EQ (&prev_vpid, &header_vpid))
    {
      if (pgbuf_ordered_fix (thread_p, &prev_vpid, OLD_PAGE, PGBUF_LATCH_WRITE, &prev_watcher) != NO_ERROR)
	{
	  /* Give up. */
	  vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
			 "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
			 page_vpid.pageid);
	  goto error;
	}
    }

  /* Fix next page if current page is not last in heap file. */
  if (!VPID_ISNULL (&next_vpid))
    {
      if (pgbuf_ordered_fix (thread_p, &next_vpid, OLD_PAGE, PGBUF_LATCH_WRITE, &next_watcher) != NO_ERROR)
	{
	  /* Give up. */
	  vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
			 "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
			 page_vpid.pageid);
	  goto error;
	}
    }

  /* All pages are fixed. */

  if (crt_watcher.page_was_unfixed && spage_number_of_records (crt_watcher.pgptr) > 1)
    {
      /* Current page has new data. It is no longer a candidate for removal. */
      vacuum_er_log (VACUUM_ER_LOG_HEAP,
		     "VACUUM: Candidate heap page %d|%d to remove was changed and has new data.\n", page_vpid.volid,
		     page_vpid.pageid);
      goto error;
    }

  /* recheck the dealloc flag after all latches are acquired */
  if (pgbuf_has_prevent_dealloc (crt_watcher.pgptr))
    {
      assert (pgbuf_has_any_waiters (crt_watcher.pgptr) == false);
      /* Even though we have fixed all required pages, somebody was doing a heap scan, and already reached our page. We 
       * cannot deallocate it. */
      vacuum_er_log (VACUUM_ER_LOG_HEAP | VACUUM_ER_LOG_WARNING,
		     "VACUUM: Candidate heap page %d|%d to remove has waiters.\n", page_vpid.volid, page_vpid.pageid);
      goto error;
    }

  /* Start changes under the protection of system operation. */
  if (log_start_system_op (thread_p) == NULL)
    {
      assert_release (false);
      vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
		     "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
		     page_vpid.pageid);
      goto error;
    }
  is_system_op_started = true;

  /* Remove page from statistics in header page. */
  copy_recdes.data = PTR_ALIGN (copy_recdes_buffer, MAX_ALIGNMENT);
  copy_recdes.area_size = sizeof (heap_hdr);
  if (spage_get_record (header_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &copy_recdes, COPY) != S_SUCCESS)
    {
      assert_release (false);
      vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
		     "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
		     page_vpid.pageid);
      goto error;
    }
  memcpy (&heap_hdr, copy_recdes.data, sizeof (heap_hdr));

  for (i = 0; i < HEAP_NUM_BEST_SPACESTATS; i++)
    {
      if (VPID_EQ (&heap_hdr.estimates.best[i].vpid, &page_vpid))
	{
	  VPID_SET_NULL (&heap_hdr.estimates.best[i].vpid);
	  heap_hdr.estimates.best[i].freespace = 0;
	  heap_hdr.estimates.head = i;
	  heap_hdr.estimates.num_high_best--;
	}
      if (VPID_EQ (&heap_hdr.estimates.second_best[i], &page_vpid))
	{
	  VPID_SET_NULL (&heap_hdr.estimates.second_best[i]);
	}
    }
  if (VPID_EQ (&heap_hdr.estimates.last_vpid, &page_vpid))
    {
      VPID_COPY (&heap_hdr.estimates.last_vpid, &prev_vpid);
    }
  if (VPID_EQ (&prev_vpid, &header_vpid))
    {
      /* Update next link. */
      VPID_COPY (&heap_hdr.next_vpid, &next_vpid);
    }
  if (VPID_EQ (&heap_hdr.estimates.full_search_vpid, &page_vpid))
    {
      VPID_SET_NULL (&heap_hdr.estimates.full_search_vpid);
    }

  /* Update header and log changes. */
  update_recdes.data = (char *) &heap_hdr;
  update_recdes.length = sizeof (heap_hdr);
  if (spage_update (thread_p, header_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &update_recdes) != SP_SUCCESS)
    {
      assert_release (false);
      vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
		     "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
		     page_vpid.pageid);
      goto error;
    }
  log_append_undoredo_data2 (thread_p, RVHF_STATS, &hfid->vfid, header_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID,
			     sizeof (heap_hdr), sizeof (heap_hdr), copy_recdes.data, update_recdes.data);
  pgbuf_set_dirty (thread_p, header_watcher.pgptr, DONT_FREE);

  /* Update links in previous and next page. */

  if (prev_watcher.pgptr != NULL)
    {
      /* Next link in previous page. */
      assert (!VPID_EQ (&header_vpid, &prev_vpid));
      copy_recdes.area_size = sizeof (chain);
      if (spage_get_record (prev_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &copy_recdes, COPY) != S_SUCCESS)
	{
	  assert_release (false);
	  vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
			 "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
			 page_vpid.pageid);
	  goto error;
	}
      memcpy (&chain, copy_recdes.data, copy_recdes.length);
      VPID_COPY (&chain.next_vpid, &next_vpid);
      update_recdes.data = (char *) &chain;
      update_recdes.length = sizeof (chain);
      if (spage_update (thread_p, prev_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &update_recdes) != SP_SUCCESS)
	{
	  assert_release (false);
	  vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
			 "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
			 page_vpid.pageid);
	  goto error;
	}
      log_append_undoredo_data2 (thread_p, RVHF_CHAIN, &hfid->vfid, prev_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID,
				 sizeof (chain), sizeof (chain), copy_recdes.data, update_recdes.data);
      pgbuf_set_dirty (thread_p, prev_watcher.pgptr, DONT_FREE);
    }

  if (next_watcher.pgptr != NULL)
    {
      /* Previous link in next page. */
      copy_recdes.area_size = sizeof (chain);
      if (spage_get_record (next_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &copy_recdes, COPY) != S_SUCCESS)
	{
	  assert_release (false);
	  vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
			 "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
			 page_vpid.pageid);
	  goto error;
	}
      memcpy (&chain, copy_recdes.data, sizeof (chain));
      VPID_COPY (&chain.prev_vpid, &prev_vpid);
      update_recdes.data = (char *) &chain;
      update_recdes.length = sizeof (chain);

      if (spage_update (thread_p, next_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &update_recdes) != SP_SUCCESS)
	{
	  assert_release (false);
	  vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
			 "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
			 page_vpid.pageid);
	  goto error;
	}
      log_append_undoredo_data2 (thread_p, RVHF_CHAIN, &hfid->vfid, next_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID,
				 sizeof (chain), sizeof (chain), copy_recdes.data, update_recdes.data);
      pgbuf_set_dirty (thread_p, next_watcher.pgptr, DONT_FREE);
    }

  /* Unfix current page. */
  pgbuf_ordered_unfix_and_init (thread_p, *page_ptr, &crt_watcher);
  /* Deallocate current page. */
  if (file_dealloc_page (thread_p, &hfid->vfid, &page_vpid, FILE_HEAP) != NO_ERROR)
    {
      ASSERT_ERROR ();
      vacuum_er_log (VACUUM_ER_LOG_WARNING | VACUUM_ER_LOG_HEAP,
		     "VACUUM WARNING: Could not remove candidate empty heap page %d|%d.\n", page_vpid.volid,
		     page_vpid.pageid);
      goto error;
    }

  /* Remove page from best space cached statistics. */
  (void) heap_stats_del_bestspace_by_vpid (thread_p, &page_vpid);

  /* Finished. */
  log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
  is_system_op_started = false;

  /* Unfix all pages. */
  if (next_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &next_watcher);
    }
  if (prev_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &prev_watcher);
    }
  pgbuf_ordered_unfix (thread_p, &header_watcher);

  /* Page removed successfully. */
  vacuum_er_log (VACUUM_ER_LOG_HEAP, "VACUUM: Successfully remove heap page %d|%d.\n", page_vpid.volid,
		 page_vpid.pageid);
  return true;

error:
  if (is_system_op_started)
    {
      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);
    }
  if (next_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &next_watcher);
    }
  if (prev_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &prev_watcher);
    }
  if (header_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &header_watcher);
    }
  if (*page_ptr != NULL)
    {
      assert (crt_watcher.pgptr == *page_ptr);
      pgbuf_ordered_unfix_and_init (thread_p, *page_ptr, &crt_watcher);
    }
  else
    {
      assert (crt_watcher.pgptr == NULL);
    }
  /* Page was not removed. */
  return false;
}

/*
 * heap_vpid_next () - Find next page of heap
 *   return: NO_ERROR
 *   hfid(in): Object heap file identifier
 *   pgptr(in): Current page pointer
 *   next_vpid(in/out): Next volume-page identifier
 *
 * Note: Find the next page of heap file.
 */
int
heap_vpid_next (const HFID * hfid, PAGE_PTR pgptr, VPID * next_vpid)
{
  HEAP_CHAIN *chain;		/* Chain to next and prev page */
  HEAP_HDR_STATS *heap_hdr;	/* Header of heap file */
  RECDES recdes;		/* Record descriptor to page header */
  int ret = NO_ERROR;

  (void) pgbuf_check_page_ptype (NULL, pgptr, PAGE_HEAP);

  /* Get either the heap header or chain record */
  if (spage_get_record (pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, PEEK) != S_SUCCESS)
    {
      /* Unable to get header/chain record for the given page */
      VPID_SET_NULL (next_vpid);
      ret = ER_FAILED;
    }
  else
    {
      pgbuf_get_vpid (pgptr, next_vpid);
      /* Is this the header page ? */
      if (next_vpid->pageid == hfid->hpgid && next_vpid->volid == hfid->vfid.volid)
	{
	  heap_hdr = (HEAP_HDR_STATS *) recdes.data;
	  *next_vpid = heap_hdr->next_vpid;
	}
      else
	{
	  chain = (HEAP_CHAIN *) recdes.data;
	  *next_vpid = chain->next_vpid;
	}
    }

  return ret;
}

/*
 * heap_vpid_prev () - Find previous page of heap
 *   return: NO_ERROR
 *   hfid(in): Object heap file identifier
 *   pgptr(in): Current page pointer
 *   prev_vpid(in/out): Previous volume-page identifier
 *
 * Note: Find the previous page of heap file.
 */
int
heap_vpid_prev (const HFID * hfid, PAGE_PTR pgptr, VPID * prev_vpid)
{
  HEAP_CHAIN *chain;		/* Chain to next and prev page */
  RECDES recdes;		/* Record descriptor to page header */
  int ret = NO_ERROR;

  (void) pgbuf_check_page_ptype (NULL, pgptr, PAGE_HEAP);

  /* Get either the header or chain record */
  if (spage_get_record (pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, PEEK) != S_SUCCESS)
    {
      /* Unable to get header/chain record for the given page */
      VPID_SET_NULL (prev_vpid);
      ret = ER_FAILED;
    }
  else
    {
      pgbuf_get_vpid (pgptr, prev_vpid);
      /* Is this the header page ? */
      if (prev_vpid->pageid == hfid->hpgid && prev_vpid->volid == hfid->vfid.volid)
	{
	  VPID_SET_NULL (prev_vpid);
	}
      else
	{
	  chain = (HEAP_CHAIN *) recdes.data;
	  *prev_vpid = chain->prev_vpid;
	}
    }

  return ret;
}

/*
 * heap_manager_initialize () -
 *   return: NO_ERROR
 *
 * Note: Initialization process of the heap file module. Find the
 * maximum size of an object that can be inserted in the heap.
 * Objects that overpass this size are stored in overflow.
 */
int
heap_manager_initialize (void)
{
  int ret;

#define HEAP_MAX_FIRSTSLOTID_LENGTH (sizeof (HEAP_HDR_STATS))

  heap_Maxslotted_reclength = (spage_max_record_size () - HEAP_MAX_FIRSTSLOTID_LENGTH);
  heap_Slotted_overhead = spage_slot_size ();

  /* Initialize the class representation cache */
  ret = heap_chnguess_initialize ();
  if (ret != NO_ERROR)
    {
      return ret;
    }

  ret = heap_classrepr_initialize_cache ();
  if (ret != NO_ERROR)
    {
      return ret;
    }

  /* Initialize best space cache */
  ret = heap_stats_bestspace_initialize ();
  if (ret != NO_ERROR)
    {
      return ret;
    }

  ret = heap_partition_link_cache_initialize ();
  if (ret != NO_ERROR)
    {
      return ret;
    }

  /* Initialize class OID->HFID cache */
  ret = heap_initialize_hfid_table ();

  return ret;
}

/*
 * heap_manager_finalize () - Terminate the heap manager
 *   return: NO_ERROR
 * Note: Deallocate any cached structure.
 */
int
heap_manager_finalize (void)
{
  int ret;

  ret = heap_chnguess_finalize ();
  if (ret != NO_ERROR)
    {
      return ret;
    }

  ret = heap_classrepr_finalize_cache ();
  if (ret != NO_ERROR)
    {
      return ret;
    }

  ret = heap_stats_bestspace_finalize ();
  if (ret != NO_ERROR)
    {
      return ret;
    }

  heap_partition_link_cache_finalize ();

  heap_finalize_hfid_table ();

  return ret;
}

/*
 * heap_create_internal () - Create a heap file
 *   return: HFID * (hfid on success and NULL on failure)
 *   hfid(in/out): Object heap file identifier.
 *                 All fields in the identifier are set, except the volume
 *                 identifier which should have already been set by the caller.
 *   exp_npgs(in): Expected number of pages
 *   class_oid(in): OID of the class for which the heap will be created.
 *   reuse_oid(in): if true, the OIDs of deleted instances will be reused
 *
 * Note: Creates a heap file on the disk volume associated with
 * hfid->vfid->volid.
 *
 * A set of sectors is allocated to improve locality of the heap.
 * The number of sectors to allocate is estimated from the number
 * of expected pages. The maximum number of allocated sectors is
 * 25% of the total number of sectors in disk. When the number of
 * pages cannot be estimated, a negative value can be passed to
 * indicate so. In this case, no sectors are allocated. The
 * number of expected pages are not allocated at this moment,
 * they are allocated as needs arrives.
 */
static HFID *
heap_create_internal (THREAD_ENTRY * thread_p, HFID * hfid, int exp_npgs, const OID * class_oid, const bool reuse_oid)
{
  HEAP_HDR_STATS heap_hdr;	/* Heap file header */
  VPID vpid;			/* Volume and page identifiers */
  RECDES recdes;		/* Record descriptor */
  LOG_DATA_ADDR addr_hdr;	/* Address of logging data */
  INT16 slotid;
  int sp_success;
  int i;
  FILE_HEAP_DES hfdes;
  const FILE_TYPE file_type = reuse_oid ? FILE_HEAP_REUSE_SLOTS : FILE_HEAP;
  int file_created = 0;

  addr_hdr.pgptr = NULL;
  if (log_start_system_op (thread_p) == NULL)
    {
      return NULL;
    }
  /* create a file descriptor */
  if (class_oid != NULL)
    {
      hfdes.class_oid = *class_oid;
    }
  else
    {
      OID_SET_NULL (&hfdes.class_oid);
    }

  if (prm_get_bool_value (PRM_ID_DONT_REUSE_HEAP_FILE) == false)
    {
      /* 
       * Try to reuse an already mark deleted heap file
       */

      vpid.volid = hfid->vfid.volid;
      if (file_type != FILE_HEAP_REUSE_SLOTS && file_reuse_deleted (thread_p, &hfid->vfid, file_type, &hfdes) != NULL
	  && heap_get_header_page (thread_p, hfid, &vpid) != NULL)
	{
	  hfid->hpgid = vpid.pageid;
	  if (heap_reuse (thread_p, hfid, &hfdes.class_oid, reuse_oid) != NULL)
	    {
	      /* A heap has been reused */
	      assert (!HFID_IS_NULL (hfid));
	      if (heap_insert_hfid_for_class_oid (thread_p, class_oid, hfid) != NO_ERROR)
		{
		  /* Failed to cache HFID. */
		  assert_release (false);
		}
	      vacuum_log_add_dropped_file (thread_p, &hfid->vfid, class_oid, VACUUM_LOG_ADD_DROPPED_FILE_UNDO);
	      goto end;
	    }
	}

      hfid->vfid.volid = vpid.volid;
    }

  if (exp_npgs < 3)
    {
      exp_npgs = 3;
    }

  /* 
   * Create the unstructured file for the heap
   * Create the header for the heap file. The header is used to speed
   * up insertions of objects and to find some simple information about the
   * heap.
   * We do not initialize the page during the allocation since the file is
   * new, and the file is going to be removed in the event of a crash.
   */

  if (file_create_check_not_dropped (thread_p, &hfid->vfid, exp_npgs, file_type, &hfdes, &vpid, 1) == NULL)
    {
      /* Unable to create the heap file */
      goto error;
    }
  file_created = 1;

  vacuum_log_add_dropped_file (thread_p, &hfid->vfid, class_oid, VACUUM_LOG_ADD_DROPPED_FILE_UNDO);

  addr_hdr.pgptr = pgbuf_fix (thread_p, &vpid, NEW_PAGE, PGBUF_LATCH_WRITE, PGBUF_UNCONDITIONAL_LATCH);
  if (addr_hdr.pgptr == NULL)
    {
      /* something went wrong, destroy the file, and return */
      goto error;
    }

  hfid->hpgid = vpid.pageid;

  if (heap_insert_hfid_for_class_oid (thread_p, class_oid, hfid) != NO_ERROR)
    {
      /* Failed to cache HFID. */
      assert_release (false);
    }

  (void) heap_stats_del_bestspace_by_hfid (thread_p, hfid);

  (void) pgbuf_set_page_ptype (thread_p, addr_hdr.pgptr, PAGE_HEAP);

  /* Initialize header page */
  spage_initialize (thread_p, addr_hdr.pgptr, heap_get_spage_type (), HEAP_MAX_ALIGN, SAFEGUARD_RVSPACE);

  /* Now insert header */
  COPY_OID (&heap_hdr.class_oid, &hfdes.class_oid);
  VFID_SET_NULL (&heap_hdr.ovf_vfid);
  VPID_SET_NULL (&heap_hdr.next_vpid);

  heap_hdr.unfill_space = (int) ((float) DB_PAGESIZE * prm_get_float_value (PRM_ID_HF_UNFILL_FACTOR));

  heap_hdr.estimates.num_pages = 1;
  heap_hdr.estimates.num_recs = 0;
  heap_hdr.estimates.recs_sumlen = 0.0;

  heap_hdr.estimates.best[0].vpid.volid = hfid->vfid.volid;
  heap_hdr.estimates.best[0].vpid.pageid = hfid->hpgid;
  heap_hdr.estimates.best[0].freespace = spage_max_space_for_new_record (thread_p, addr_hdr.pgptr);

  heap_hdr.estimates.head = 1;
  for (i = heap_hdr.estimates.head; i < HEAP_NUM_BEST_SPACESTATS; i++)
    {
      VPID_SET_NULL (&heap_hdr.estimates.best[i].vpid);
      heap_hdr.estimates.best[i].freespace = 0;
    }

  heap_hdr.estimates.num_high_best = 1;
  heap_hdr.estimates.num_other_high_best = 0;

  heap_hdr.estimates.num_second_best = 0;
  heap_hdr.estimates.head_second_best = 0;
  heap_hdr.estimates.tail_second_best = 0;
  heap_hdr.estimates.num_substitutions = 0;

  for (i = 0; i < HEAP_NUM_BEST_SPACESTATS; i++)
    {
      VPID_SET_NULL (&heap_hdr.estimates.second_best[i]);
    }

  heap_hdr.estimates.last_vpid.volid = hfid->vfid.volid;
  heap_hdr.estimates.last_vpid.pageid = hfid->hpgid;

  heap_hdr.estimates.full_search_vpid.volid = hfid->vfid.volid;
  heap_hdr.estimates.full_search_vpid.pageid = hfid->hpgid;
  heap_hdr.has_partition_link = 0;

  recdes.area_size = recdes.length = sizeof (HEAP_HDR_STATS);
  recdes.type = REC_HOME;
  recdes.data = (char *) &heap_hdr;

  sp_success = spage_insert (thread_p, addr_hdr.pgptr, &recdes, &slotid);
  if (sp_success != SP_SUCCESS || slotid != HEAP_HEADER_AND_CHAIN_SLOTID)
    {
      /* something went wrong, destroy file and return error */
      if (sp_success != SP_SUCCESS)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNABLE_TO_CREATE_HEAP, 1,
		  fileio_get_volume_label (hfid->vfid.volid, PEEK));
	}

      /* Free the page and release the lock */
      goto error;
    }
  else
    {
      /* 
       * Don't need to log before image (undo) since file and pages of the heap
       * are deallocated during undo (abort).
       */
      addr_hdr.vfid = &hfid->vfid;
      addr_hdr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;
      log_append_redo_data (thread_p, reuse_oid ? RVHF_CREATE_HEADER_REUSE_OID : RVHF_CREATE_HEADER, &addr_hdr,
			    sizeof (heap_hdr), &heap_hdr);
      pgbuf_set_dirty (thread_p, addr_hdr.pgptr, FREE);
      addr_hdr.pgptr = NULL;
    }

end:
  log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);

  file_new_declare_as_old (thread_p, &hfid->vfid);

  addr_hdr.vfid = NULL;
  addr_hdr.pgptr = NULL;
  addr_hdr.offset = 0;
  log_append_undo_data (thread_p, RVHF_CREATE, &addr_hdr, sizeof (HFID), hfid);

  /* Already append a vacuum undo logging when file was created, but since that was included in the system operation
   * which just got committed, we need to do it again in case of rollback. */
  vacuum_log_add_dropped_file (thread_p, &hfid->vfid, class_oid, VACUUM_LOG_ADD_DROPPED_FILE_UNDO);

  LOG_CS_ENTER (thread_p);
  logpb_flush_pages_direct (thread_p);
  LOG_CS_EXIT (thread_p);

  return hfid;

error:

  if (addr_hdr.pgptr != NULL)
    {
      pgbuf_unfix_and_init (thread_p, addr_hdr.pgptr);
    }

  if (file_created)
    {
      (void) file_destroy (thread_p, &hfid->vfid);
      /* remove the file from new file cache. Since we are in a top operation, file_destroy didn't change the file as
       * OLD_FILE. */
      file_new_declare_as_old (thread_p, &hfid->vfid);
    }

  hfid->vfid.fileid = NULL_FILEID;
  hfid->hpgid = NULL_PAGEID;

  log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);
  return NULL;
}

/*
 * heap_delete_all_page_records () -
 *   return: false if nothing is deleted, otherwise true
 *   vpid(in): the vpid of the page
 *   pgptr(in): PAGE_PTR to the page
 */
static bool
heap_delete_all_page_records (THREAD_ENTRY * thread_p, const VPID * vpid, PAGE_PTR pgptr)
{
  bool something_deleted = false;
  OID oid;
  RECDES recdes;

  assert (pgptr != NULL);
  assert (vpid != NULL);

  oid.volid = vpid->volid;
  oid.pageid = vpid->pageid;
  oid.slotid = NULL_SLOTID;

  while (true)
    {
      if (spage_next_record (pgptr, &oid.slotid, &recdes, PEEK) != S_SUCCESS)
	{
	  break;
	}
      if (oid.slotid == HEAP_HEADER_AND_CHAIN_SLOTID)
	{
	  continue;
	}
      (void) spage_delete (thread_p, pgptr, oid.slotid);
      something_deleted = true;
    }

  return something_deleted;
}

/*
 * heap_reinitialize_page () -
 *   return: NO_ERROR if succeed, otherwise error code
 *   pgptr(in): PAGE_PTR to the page
 *   is_header_page(in): true if the page is the header page
 */
static int
heap_reinitialize_page (THREAD_ENTRY * thread_p, PAGE_PTR pgptr, const bool is_header_page)
{
  HEAP_CHAIN tmp_chain;
  HEAP_HDR_STATS tmp_hdr_stats;
  PGSLOTID slotid = NULL_SLOTID;
  RECDES recdes;
  int error_code = NO_ERROR;

  if (spage_get_record (pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, PEEK) != S_SUCCESS)
    {
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      error_code = ER_GENERIC_ERROR;
      goto error_exit;
    }

  if (is_header_page)
    {
      assert (recdes.length == sizeof (HEAP_HDR_STATS));
      tmp_hdr_stats = *(HEAP_HDR_STATS *) recdes.data;
      recdes.data = (char *) &tmp_hdr_stats;
      recdes.area_size = recdes.length = sizeof (tmp_hdr_stats);
      recdes.type = REC_HOME;
    }
  else
    {
      assert (recdes.length == sizeof (HEAP_CHAIN));
      tmp_chain = *(HEAP_CHAIN *) recdes.data;
      recdes.data = (char *) &tmp_chain;
      recdes.area_size = recdes.length = sizeof (tmp_chain);
      recdes.type = REC_HOME;
    }

  (void) pgbuf_set_page_ptype (thread_p, pgptr, PAGE_HEAP);

  /* Initialize header page */
  spage_initialize (thread_p, pgptr, heap_get_spage_type (), HEAP_MAX_ALIGN, SAFEGUARD_RVSPACE);

  if (spage_insert (thread_p, pgptr, &recdes, &slotid) != SP_SUCCESS || slotid != HEAP_HEADER_AND_CHAIN_SLOTID)
    {
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      error_code = ER_GENERIC_ERROR;
      goto error_exit;
    }
  else
    {
      /* All is well and the page is now empty. */
    }

  return error_code;

error_exit:
  if (error_code == NO_ERROR)
    {
      error_code = ER_GENERIC_ERROR;
    }
  return error_code;
}

/*
 * heap_reuse () - Reuse a heap
 *   return: HFID * (hfid on success and NULL on failure)
 *   hfid(in): Object heap file identifier.
 *   class_oid(in): OID of the class for which the heap will be created.
 *
 * Note: Clean the given heap file so that it can be reused.
 * Note: The heap file must have been permanently marked as deleted.
 */
static const HFID *
heap_reuse (THREAD_ENTRY * thread_p, const HFID * hfid, const OID * class_oid, const bool reuse_oid)
{
  VFID vfid;
  VPID vpid;			/* Volume and page identifiers */
  PAGE_PTR hdr_pgptr = NULL;	/* Page pointer to header page */
  PAGE_PTR pgptr = NULL;	/* Page pointer */
  LOG_DATA_ADDR addr;		/* Address of logging data */
  HEAP_HDR_STATS *heap_hdr = NULL;	/* Header of heap structure */
  HEAP_CHAIN *chain;		/* Chain to next and prev page */
  RECDES recdes;
  VPID last_vpid;
  int is_header_page;
  int npages = 0;
  int i;
  bool need_update;

  assert (class_oid != NULL);
  assert (!OID_ISNULL (class_oid));

  VPID_SET_NULL (&last_vpid);
  addr.vfid = &hfid->vfid;

  /* 
   * Read the header page.
   * We lock the header page in exclusive mode.
   */

  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;
  hdr_pgptr = pgbuf_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_WRITE, PGBUF_UNCONDITIONAL_LATCH);
  if (hdr_pgptr == NULL)
    {
      return NULL;
    }

  (void) pgbuf_check_page_ptype (thread_p, hdr_pgptr, PAGE_HEAP);

  /* 
   * Start scanning every page of the heap and removing the objects.
   * Note that, for normal heap files, the slot is not removed since we do not
   * know if the objects are pointed by some other objects in the database.
   * For reusable OID heap files we are certain there can be no references to
   * the objects so we can simply initialize the slotted page.
   */
  /* 
   * Note Because the objects of reusable OID heaps are not referenced,
   *      reusing such heaps provides no actual benefit. We might consider
   *      giving up the reuse heap mechanism for reusable OID heaps in the
   *      future.
   */

  while (!(VPID_ISNULL (&vpid)))
    {
      /* 
       * Fetch the page
       */
      pgptr = pgbuf_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_WRITE, PGBUF_UNCONDITIONAL_LATCH);
      if (pgptr == NULL)
	{
	  goto error;
	}

      (void) pgbuf_check_page_ptype (thread_p, pgptr, PAGE_HEAP);

      is_header_page = (hdr_pgptr == pgptr) ? 1 : 0;

      /* 
       * Remove all the objects in this page
       */
      if (!reuse_oid)
	{
	  (void) heap_delete_all_page_records (thread_p, &vpid, pgptr);

	  addr.pgptr = pgptr;
	  addr.offset = is_header_page;
	  log_append_redo_data (thread_p, RVHF_REUSE_PAGE, &addr, sizeof (*class_oid), class_oid);
	}
      else
	{
	  if (spage_number_of_slots (pgptr) > 1)
	    {
	      if (heap_reinitialize_page (thread_p, pgptr, is_header_page) != NO_ERROR)
		{
		  goto error;
		}
	    }

	  addr.pgptr = pgptr;
	  addr.offset = is_header_page;
	  log_append_redo_data (thread_p, RVHF_REUSE_PAGE_REUSE_OID, &addr, sizeof (*class_oid), class_oid);
	}

      if (spage_get_record (pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, PEEK) != S_SUCCESS)
	{
	  goto error;
	}
      if (recdes.data == NULL)
	{
	  goto error;
	}

      /* save new class oid in the page. it dirties the page. */
      if (is_header_page)
	{
	  heap_hdr = (HEAP_HDR_STATS *) recdes.data;
	  COPY_OID (&(heap_hdr->class_oid), class_oid);
	}
      else
	{
	  chain = (HEAP_CHAIN *) recdes.data;
	  COPY_OID (&(chain->class_oid), class_oid);
	  chain->max_mvccid = MVCCID_NULL;
	  chain->flags = 0;
	  HEAP_PAGE_SET_VACUUM_STATUS (chain, HEAP_PAGE_VACUUM_NONE);
	}

      if (npages < HEAP_NUM_BEST_SPACESTATS)
	{
	  heap_hdr->estimates.best[npages].vpid = vpid;
	  heap_hdr->estimates.best[npages].freespace =
	    spage_get_free_space_without_saving (thread_p, pgptr, &need_update);

	}

      if (prm_get_integer_value (PRM_ID_HF_MAX_BESTSPACE_ENTRIES) > 0)
	{
	  (void) heap_stats_add_bestspace (thread_p, hfid, &vpid, DB_PAGESIZE);
	}

      npages++;
      last_vpid = vpid;

      /* 
       * Find next page to scan and free the current page
       */
      if (heap_vpid_next (hfid, pgptr, &vpid) != NO_ERROR)
	{
	  goto error;
	}

      pgbuf_set_dirty (thread_p, pgptr, FREE);
      pgptr = NULL;
    }

  /* 
   * If there is an overflow file for this heap, remove it
   */
  if (heap_ovf_find_vfid (thread_p, hfid, &vfid, false, PGBUF_UNCONDITIONAL_LATCH) != NULL)
    {
      (void) file_destroy (thread_p, &vfid);
    }

  /* 
   * Reset the statistics. Set statistics for insertion back to first page
   * and reset unfill space according to new parameters
   */
  VFID_SET_NULL (&heap_hdr->ovf_vfid);
  heap_hdr->unfill_space = (int) ((float) DB_PAGESIZE * prm_get_float_value (PRM_ID_HF_UNFILL_FACTOR));
  heap_hdr->estimates.num_pages = npages;
  heap_hdr->estimates.num_recs = 0;
  heap_hdr->estimates.recs_sumlen = 0.0;

  if (npages < HEAP_NUM_BEST_SPACESTATS)
    {
      heap_hdr->estimates.num_high_best = npages;
      heap_hdr->estimates.num_other_high_best = 0;
    }
  else
    {
      heap_hdr->estimates.num_high_best = HEAP_NUM_BEST_SPACESTATS;
      heap_hdr->estimates.num_other_high_best = npages - HEAP_NUM_BEST_SPACESTATS;
    }

  heap_hdr->estimates.head = 0;
  for (i = npages; i < HEAP_NUM_BEST_SPACESTATS; i++)
    {
      VPID_SET_NULL (&heap_hdr->estimates.best[i].vpid);
      heap_hdr->estimates.best[i].freespace = 0;
    }

  heap_hdr->estimates.last_vpid = last_vpid;
  heap_hdr->has_partition_link = 0;

  addr.pgptr = hdr_pgptr;
  addr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;
  log_append_redo_data (thread_p, RVHF_STATS, &addr, sizeof (*heap_hdr), heap_hdr);
  pgbuf_set_dirty (thread_p, hdr_pgptr, FREE);
  hdr_pgptr = NULL;

  return hfid;

error:
  if (pgptr != NULL)
    {
      pgbuf_unfix_and_init (thread_p, pgptr);
    }
  if (hdr_pgptr != NULL)
    {
      pgbuf_unfix_and_init (thread_p, hdr_pgptr);
    }

  return NULL;
}

#if defined(CUBRID_DEBUG)
/*
 * heap_hfid_isvalid () -
 *   return:
 *   hfid(in):
 */
static DISK_ISVALID
heap_hfid_isvalid (HFID * hfid)
{
  DISK_ISVALID valid_pg = DISK_VALID;

  if (hfid == NULL || HFID_IS_NULL (hfid))
    {
      return DISK_INVALID;
    }

  valid_pg = disk_isvalid_page (hfid->vfid.volid, hfid->vfid.fileid);
  if (valid_pg == DISK_VALID)
    {
      valid_pg = disk_isvalid_page (hfid->vfid.volid, hfid->hpgid);
    }

  return valid_pg;
}

/*
 * heap_scanrange_isvalid () -
 *   return:
 *   scan_range(in):
 */
static DISK_ISVALID
heap_scanrange_isvalid (HEAP_SCANRANGE * scan_range)
{
  DISK_ISVALID valid_pg = DISK_INVALID;

  if (scan_range != NULL)
    {
      valid_pg = heap_hfid_isvalid (&scan_range->scan_cache.hfid);
    }

  if (valid_pg != DISK_VALID)
    {
      if (valid_pg != DISK_ERROR)
	{
	  er_log_debug (ARG_FILE_LINE, " ** SYSTEM ERROR scanrange has not been initialized");
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	}
    }

  return valid_pg;
}
#endif /* CUBRID_DEBUG */

/*
 * xheap_create () - Create a heap file
 *   return: int
 *   hfid(in/out): Object heap file identifier.
 *                 All fields in the identifier are set, except the volume
 *                 identifier which should have already been set by the caller.
 *   class_oid(in): OID of the class for which the heap will be created.
 *   reuse_oid(int):
 *
 * Note: Creates an object heap file on the disk volume associated with
 * hfid->vfid->volid.
 */
int
xheap_create (THREAD_ENTRY * thread_p, HFID * hfid, const OID * class_oid, bool reuse_oid)
{
  if (heap_create_internal (thread_p, hfid, -1, class_oid, reuse_oid) == NULL)
    {
      assert (er_errid () != NO_ERROR);
      return er_errid ();
    }
  else
    {
      return NO_ERROR;
    }
}

/*
 * xheap_destroy () - Destroy a heap file
 *   return: int
 *   hfid(in): Object heap file identifier.
 *   class_oid(in):
 *
 * Note: Destroy the heap file associated with the given heap identifier.
 */
int
xheap_destroy (THREAD_ENTRY * thread_p, const HFID * hfid, const OID * class_oid)
{
  VFID vfid;
  LOG_DATA_ADDR addr;

  vacuum_log_add_dropped_file (thread_p, &hfid->vfid, class_oid, VACUUM_LOG_ADD_DROPPED_FILE_POSTPONE);

  addr.vfid = NULL;
  addr.pgptr = NULL;
  addr.offset = -1;
  if (heap_ovf_find_vfid (thread_p, hfid, &vfid, false, PGBUF_UNCONDITIONAL_LATCH) != NULL)
    {
      log_append_postpone (thread_p, RVFL_POSTPONE_DESTROY_FILE, &addr, sizeof (vfid), &vfid);
    }

  log_append_postpone (thread_p, RVFL_POSTPONE_DESTROY_FILE, &addr, sizeof (hfid->vfid), &hfid->vfid);

  (void) heap_stats_del_bestspace_by_hfid (thread_p, hfid);

  return NO_ERROR;
}

/*
 * xheap_destroy_newly_created () - Destroy heap if it is a newly created heap
 *   return: NO_ERROR
 *   hfid(in): Object heap file identifier.
 *   class_oid(in): class OID
 *
 * Note: Destroy the heap file associated with the given heap
 * identifier if it is a newly created heap file.
 */
int
xheap_destroy_newly_created (THREAD_ENTRY * thread_p, const HFID * hfid, const OID * class_oid)
{
  VFID vfid;
  FILE_TYPE file_type;
  int ret;

  file_type = file_get_type (thread_p, &(hfid->vfid));
  if (file_type == FILE_HEAP_REUSE_SLOTS)
    {
      ret = xheap_destroy (thread_p, hfid, class_oid);
      if (ret == NO_ERROR)
	{
	  ret = heap_delete_from_partition_link_cache (thread_p, class_oid);
	}
      return ret;
    }

  vacuum_log_add_dropped_file (thread_p, &hfid->vfid, NULL, VACUUM_LOG_ADD_DROPPED_FILE_POSTPONE);

  if (heap_ovf_find_vfid (thread_p, hfid, &vfid, false, PGBUF_UNCONDITIONAL_LATCH) != NULL)
    {
      ret = file_mark_as_deleted (thread_p, &vfid, class_oid);
      if (ret != NO_ERROR)
	{
	  return ret;
	}
    }

  ret = file_mark_as_deleted (thread_p, &hfid->vfid, class_oid);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  (void) heap_stats_del_bestspace_by_hfid (thread_p, hfid);
  ret = heap_delete_from_partition_link_cache (thread_p, class_oid);

  return ret;
}

/*
 * heap_assign_address () - Assign a new location
 *   return: NO_ERROR / ER_FAILED
 *   hfid(in): Object heap file identifier
 *   class_oid(in): class identifier
 *   oid(out): Object identifier.
 *   expected_length(in): Expected length
 *
 * Note: Assign an OID to an object and reserve the expected length for
 * the object. The following rules are observed for the expected length.
 *              1. A negative value is passed when only an approximation of
 *                 the length of the object is known. This approximation is
 *                 taken as the minimal length by this module. This case is
 *                 used when the transformer module (tfcl) skips some fileds
 *                 while walking through the object to find out its length.
 *                 a) Heap manager find the average length of objects in the
 *                    heap.
 *                    If the average length > abs(expected_length)
 *                    The average length is used instead
 *              2. A zero value, heap manager uses the average length of the
 *                 objects in the heap.
 *              3. If length is larger than one page, the size of an OID is
 *                 used since the object is going to be stored in overflow
 *              4. If length is > 0 and smaller than OID_SIZE
 *                 OID_SIZE is used as the expected length.
 */
int
heap_assign_address (THREAD_ENTRY * thread_p, const HFID * hfid, OID * class_oid, OID * oid, int expected_length)
{
  HEAP_OPERATION_CONTEXT insert_context;
  RECDES recdes;
  int rc;

  if (expected_length <= 0)
    {
      recdes.length = heap_estimate_avg_length (thread_p, hfid);
      if (recdes.length > (-expected_length))
	{
	  expected_length = recdes.length;
	}
      else
	{
	  expected_length = -expected_length;
	}
    }

  /* 
   * Use the expected length only when it is larger than the size of an OID
   * and it is smaller than the maximum size of an object that can be stored
   * in the primary area (no in overflow). In any other case, use the the size
   * of an OID as the length.
   */

  recdes.length =
    ((expected_length > SSIZEOF (OID) && !heap_is_big_length (expected_length)) ? expected_length : SSIZEOF (OID));

  recdes.data = NULL;
  recdes.type = REC_ASSIGN_ADDRESS;

  /* create context */
  heap_create_insert_context (&insert_context, (HFID *) hfid, class_oid, &recdes, NULL, false);

  /* insert */
  rc = heap_insert_logical (thread_p, &insert_context);
  if (rc != NO_ERROR)
    {
      return rc;
    }

  /* get result and exit */
  COPY_OID (oid, &insert_context.res_oid);
  return NO_ERROR;
}

/*
 * heap_flush () - Flush all dirty pages where the object resides
 *   return:
 *   oid(in): Object identifier
 *
 * Note: Flush all dirty pages where the object resides.
 */
void
heap_flush (THREAD_ENTRY * thread_p, const OID * oid)
{
  VPID vpid;			/* Volume and page identifiers */
  PAGE_PTR pgptr = NULL;	/* Page pointer */
  INT16 type;
  OID forward_oid;
  RECDES forward_recdes;
  int ret = NO_ERROR;
  OID forward_rec_buffer[2];

  if (HEAP_ISVALID_OID (oid) != DISK_VALID)
    {
      return;
    }

  /* 
   * Lock and fetch the page where the object is stored
   */
  vpid.volid = oid->volid;
  vpid.pageid = oid->pageid;
  pgptr = heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, S_LOCK, NULL, NULL);
  if (pgptr == NULL)
    {
      if (er_errid () == ER_PB_BAD_PAGEID)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid, oid->slotid);
	}
      /* something went wrong, return */
      return;
    }

  (void) pgbuf_check_page_ptype (thread_p, pgptr, PAGE_HEAP);

  type = spage_get_record_type (pgptr, oid->slotid);
  if (type == REC_UNKNOWN)
    {
      goto end;
    }

  /* If this page is dirty flush it */
  (void) pgbuf_flush_with_wal (thread_p, pgptr);

  switch (type)
    {
    case REC_RELOCATION:
      /* 
       * The object stored on the page is a relocation record. The relocation
       * record is used as a map to find the actual location of the content of
       * the object.
       */

      forward_recdes.data = (char *) forward_rec_buffer;
      forward_recdes.area_size = 2 * OR_OID_SIZE;

      if (spage_get_record (pgptr, oid->slotid, &forward_recdes, COPY) != S_SUCCESS)
	{
	  /* Unable to get relocation record of the object */
	  goto end;
	}
      COPY_OID (&forward_oid, (OID *) forward_recdes.data);
      pgbuf_unfix_and_init (thread_p, pgptr);

      /* Fetch the new home page */
      vpid.volid = forward_oid.volid;
      vpid.pageid = forward_oid.pageid;

      pgptr = heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, S_LOCK, NULL, NULL);
      if (pgptr == NULL)
	{
	  if (er_errid () == ER_PB_BAD_PAGEID)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, forward_oid.volid,
		      forward_oid.pageid, forward_oid.slotid);
	    }

	  return;
	}

      (void) pgbuf_check_page_ptype (thread_p, pgptr, PAGE_HEAP);

      (void) pgbuf_flush_with_wal (thread_p, pgptr);
      break;

    case REC_BIGONE:
      /* 
       * The object stored in the heap page is a relocation_overflow record,
       * get the overflow address of the object
       */
      forward_recdes.data = (char *) forward_rec_buffer;
      forward_recdes.area_size = 2 * OR_OID_SIZE;

      if (spage_get_record (pgptr, oid->slotid, &forward_recdes, COPY) != S_SUCCESS)
	{
	  /* Unable to peek overflow address of multipage object */
	  goto end;
	}
      COPY_OID (&forward_oid, (OID *) forward_recdes.data);
      pgbuf_unfix_and_init (thread_p, pgptr);
      ret = heap_ovf_flush (thread_p, &forward_oid);
      break;

    case REC_ASSIGN_ADDRESS:
    case REC_HOME:
    case REC_NEWHOME:
    case REC_MARKDELETED:
    case REC_DELETED_WILL_REUSE:
    case REC_MVCC_NEXT_VERSION:
    default:
      break;
    }

end:
  if (pgptr != NULL)
    {
      pgbuf_unfix_and_init (thread_p, pgptr);
    }
}

/*
 * xheap_reclaim_addresses () - Reclaim addresses/OIDs and delete empty pages
 *   return: NO_ERROR
 *   hfid(in): Heap file identifier
 *   reclaim_mvcc_next_versions(in): True to reclaim REC_MVCC_NEXT_VERSION.
 *
 * Note: Reclaim the addresses (OIDs) of deleted objects of the given heap and
 *       delete all the heap pages that are left empty.
 *
 *       This function can be called:
 *    a: When there are no more references to deleted objects of the given
 *       heap. This happens during offline compactdb execution after all the
 *       classes in the schema have been processed by the process_class ()
 *       function that sets the references to deleted objects to NULL.
 *    b: When we are sure there can be no references to any object of the
 *       associated class. This happens during online compactdb execution when
 *       all the classes in the schema are checked to see if can they point to
 *       instances of the current class by checking all their atributes'
 *       domains.
 *
 *       If references to deleted objects were nulled by the current
 *       transaction some recovery problems may happen in the case of a crash
 *       since the reclaiming of the addresses is done without logging (or
 *       very little one) and thus it cannot be fully undone. Some logging is
 *       done to make sure that media recovery will not be impacted. This was
 *       done to avoid a lot of unneeded logging. Thus, if the caller was
 *       setting references to deleted objects to NULL, the caller must commit
 *       his transaction before this function is invoked.
 *
 *      This function must be run:
 *   a: offline, that is, when the user is the only one using the database
 *      system.
 *   b: online while holding an exclusive lock on the associated class.
 */
int
xheap_reclaim_addresses (THREAD_ENTRY * thread_p, const HFID * hfid, bool reclaim_mvcc_next_versions)
{
  VPID vpid;
  VPID prv_vpid;
  int best, i;
  HEAP_HDR_STATS initial_heap_hdr;
  HEAP_HDR_STATS heap_hdr;
  RECDES hdr_recdes;
  LOG_DATA_ADDR addr;
  int ret = NO_ERROR;
  int free_space;
  int npages, nrecords, rec_length;
  bool need_update;
  PGBUF_WATCHER hdr_page_watcher;
  PGBUF_WATCHER curr_page_watcher;

  PGBUF_INIT_WATCHER (&hdr_page_watcher, PGBUF_ORDERED_HEAP_HDR, hfid);
  PGBUF_INIT_WATCHER (&curr_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);

  addr.vfid = &hfid->vfid;
  addr.pgptr = NULL;
  addr.offset = 0;

  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;

  ret = pgbuf_ordered_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_WRITE, &hdr_page_watcher);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  (void) pgbuf_check_page_ptype (thread_p, hdr_page_watcher.pgptr, PAGE_HEAP);

  hdr_recdes.data = (char *) &heap_hdr;
  hdr_recdes.area_size = sizeof (heap_hdr);

  if (spage_get_record (hdr_page_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes, COPY) != S_SUCCESS
      || file_find_last_page (thread_p, &hfid->vfid, &prv_vpid) == NULL)
    {
      goto exit_on_error;
    }

  /* Copy the header to memory.. so we can log the changes */
  memcpy (&initial_heap_hdr, hdr_recdes.data, sizeof (initial_heap_hdr));

  /* 
   * Initialize best estimates
   */
  heap_hdr.estimates.num_pages = 0;
  heap_hdr.estimates.num_recs = 0;
  heap_hdr.estimates.recs_sumlen = 0.0;
  heap_hdr.estimates.num_high_best = 0;
  heap_hdr.estimates.num_other_high_best = 0;
  heap_hdr.estimates.head = 0;

  for (i = 0; i < HEAP_NUM_BEST_SPACESTATS; i++)
    {
      VPID_SET_NULL (&heap_hdr.estimates.best[i].vpid);
      heap_hdr.estimates.best[0].freespace = 0;
    }

  /* Initialize second best estimates */
  heap_hdr.estimates.num_second_best = 0;
  heap_hdr.estimates.head_second_best = 0;
  heap_hdr.estimates.tail_second_best = 0;
  heap_hdr.estimates.num_substitutions = 0;

  for (i = 0; i < HEAP_NUM_BEST_SPACESTATS; i++)
    {
      VPID_SET_NULL (&heap_hdr.estimates.second_best[i]);
    }

  /* initialize full_search_vpid */
  heap_hdr.estimates.full_search_vpid.volid = hfid->vfid.volid;
  heap_hdr.estimates.full_search_vpid.pageid = hfid->hpgid;
  heap_hdr.has_partition_link = 0;

  best = 0;

  while (!(VPID_ISNULL (&prv_vpid)))
    {
      vpid = prv_vpid;
      curr_page_watcher.pgptr =
	heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, X_LOCK, NULL, &curr_page_watcher);
      if (curr_page_watcher.pgptr == NULL)
	{
	  goto exit_on_error;
	}

      (void) pgbuf_check_page_ptype (thread_p, curr_page_watcher.pgptr, PAGE_HEAP);

      if (heap_vpid_prev (hfid, curr_page_watcher.pgptr, &prv_vpid) != NO_ERROR)
	{
	  pgbuf_ordered_unfix (thread_p, &curr_page_watcher);

	  goto exit_on_error;
	}

      /* 
       * Are there any objects in this page ?
       * Compare against > 1 since every heap page contains a header record
       * (heap header or chain).
       */

      if (spage_number_of_records (curr_page_watcher.pgptr) > 1
	  || (vpid.pageid == hfid->hpgid && vpid.volid == hfid->vfid.volid))
	{
	  if (spage_reclaim (thread_p, curr_page_watcher.pgptr, reclaim_mvcc_next_versions) == true)
	    {
	      addr.pgptr = curr_page_watcher.pgptr;
	      /* 
	       * If this function is called correctly (see the notes in the
	       * header comment about the preconditions) we can skip the
	       * logging of spage_reclaim (). Logging for REDO would add many
	       * log records for any compactdb operation and would only
	       * benefit the infrequent scenario of compactdb operations that
	       * crash right at the end. UNDO operations are not absolutely
	       * required because the deleted OIDs should be unreferenced
	       * anyway; there should be no harm in reusing them. Basically,
	       * since the call to spage_reclaim () should leave the database
	       * logically unmodified, neither REDO nor UNDO are required.
	       */
	      log_skip_logging (thread_p, &addr);
	      pgbuf_set_dirty (thread_p, curr_page_watcher.pgptr, DONT_FREE);
	    }
	}

      /* 
       * Throw away the page if it doesn't contain any object. The header of
       * the heap cannot be thrown.
       */

      if (!(vpid.pageid == hfid->hpgid && vpid.volid == hfid->vfid.volid)
	  && spage_number_of_records (curr_page_watcher.pgptr) <= 1
	  /* Is any vacuum required? */
	  && vacuum_is_mvccid_vacuumed (heap_page_get_max_mvccid (thread_p, curr_page_watcher.pgptr)))
	{
	  /* 
	   * This page can be thrown away
	   */
	  pgbuf_ordered_unfix (thread_p, &curr_page_watcher);
	  if (heap_vpid_remove (thread_p, hfid, &heap_hdr, &vpid) == NULL)
	    {
	      goto exit_on_error;
	    }
	  vacuum_er_log (VACUUM_ER_LOG_HEAP, "VACUUM: Compactdb removed page %d|%d from heap file (%d, %d|%d).\n",
			 vpid.volid, vpid.pageid, hfid->hpgid, hfid->vfid.volid, hfid->vfid.fileid);
	}
      else
	{
	  spage_collect_statistics (curr_page_watcher.pgptr, &npages, &nrecords, &rec_length);

	  heap_hdr.estimates.num_pages += npages;
	  heap_hdr.estimates.num_recs += nrecords;
	  heap_hdr.estimates.recs_sumlen += rec_length;

	  free_space = spage_get_free_space_without_saving (thread_p, curr_page_watcher.pgptr, &need_update);

	  if (free_space > HEAP_DROP_FREE_SPACE)
	    {
	      if (best < HEAP_NUM_BEST_SPACESTATS)
		{
		  heap_hdr.estimates.best[best].vpid = vpid;
		  heap_hdr.estimates.best[best].freespace = free_space;
		  best++;
		}
	      else
		{
		  heap_hdr.estimates.num_other_high_best++;
		  heap_stats_put_second_best (&heap_hdr, &vpid);
		}

	      if (prm_get_integer_value (PRM_ID_HF_MAX_BESTSPACE_ENTRIES) > 0)
		{
		  (void) heap_stats_add_bestspace (thread_p, hfid, &vpid, free_space);
		}
	    }

	  pgbuf_ordered_unfix (thread_p, &curr_page_watcher);
	}
    }

  heap_hdr.estimates.num_high_best = best;
  /* 
   * Set the rest of the statistics to NULL
   */
  for (; best < HEAP_NUM_BEST_SPACESTATS; best++)
    {
      VPID_SET_NULL (&heap_hdr.estimates.best[best].vpid);
      heap_hdr.estimates.best[best].freespace = 0;
    }

  /* Log the desired changes.. and then change the header We need to log the header changes in order to always benefit
   * from the updated statistics and in order to avoid referencing deleted pages in the statistics. */
  addr.pgptr = hdr_page_watcher.pgptr;
  addr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;
  log_append_undoredo_data (thread_p, RVHF_STATS, &addr, sizeof (HEAP_HDR_STATS), sizeof (HEAP_HDR_STATS),
			    &initial_heap_hdr, hdr_recdes.data);

  /* Now update the statistics */
  if (spage_update (thread_p, hdr_page_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes) != SP_SUCCESS)
    {
      goto exit_on_error;
    }

  pgbuf_ordered_set_dirty_and_free (thread_p, &hdr_page_watcher);

  return ret;

exit_on_error:

  if (hdr_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &hdr_page_watcher);
    }

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_ovf_find_vfid () - Find overflow file identifier
 *   return: ovf_vfid or NULL
 *   hfid(in): Object heap file identifier
 *   ovf_vfid(in/out): Overflow file identifier.
 *   docreate(in): true/false. If true and the overflow file does not
 *                 exist, it is created.
 *
 * Note: Find overflow file identifier. If the overflow file does not
 * exist, it may be created depending of the value of argument create.
 */
VFID *
heap_ovf_find_vfid (THREAD_ENTRY * thread_p, const HFID * hfid, VFID * ovf_vfid, bool docreate,
		    PGBUF_LATCH_CONDITION latch_cond)
{
  HEAP_HDR_STATS *heap_hdr;	/* Header of heap structure */
  LOG_DATA_ADDR addr_hdr;	/* Address of logging data */
  VPID vpid;			/* Page-volume identifier */
  RECDES hdr_recdes;		/* Header record descriptor */
  int mode;

  addr_hdr.vfid = &hfid->vfid;
  addr_hdr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;

  /* Read the header page */
  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;

  mode = (docreate == true ? PGBUF_LATCH_WRITE : PGBUF_LATCH_READ);
  addr_hdr.pgptr = pgbuf_fix (thread_p, &vpid, OLD_PAGE, mode, latch_cond);
  if (addr_hdr.pgptr == NULL)
    {
      /* something went wrong, return */
      return NULL;
    }

  (void) pgbuf_check_page_ptype (thread_p, addr_hdr.pgptr, PAGE_HEAP);

  /* Peek the header record */

  if (spage_get_record (addr_hdr.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes, PEEK) != S_SUCCESS)
    {
      pgbuf_unfix_and_init (thread_p, addr_hdr.pgptr);
      return NULL;
    }

  heap_hdr = (HEAP_HDR_STATS *) hdr_recdes.data;
  if (VFID_ISNULL (&heap_hdr->ovf_vfid))
    {
      if (docreate == true)
	{
	  FILE_OVF_HEAP_DES hfdes_ovf;
	  /* 
	   * Create the overflow file. Try to create the overflow file in the
	   * same volume where the heap was defined
	   */

	  /* 
	   * START A TOP SYSTEM OPERATION
	   */

	  if (log_start_system_op (thread_p) == NULL)
	    {
	      pgbuf_unfix_and_init (thread_p, addr_hdr.pgptr);
	      return NULL;
	    }


	  ovf_vfid->volid = hfid->vfid.volid;
	  /* 
	   * At least three pages since a multipage object will take at least
	   * two pages
	   */


	  /* Initialize description of overflow heap file */
	  HFID_COPY (&hfdes_ovf.hfid, hfid);

	  if (file_create (thread_p, ovf_vfid, 3, FILE_MULTIPAGE_OBJECT_HEAP, &hfdes_ovf, NULL, 0) != NULL)
	    {
	      /* Log undo, then redo */
	      log_append_undo_data (thread_p, RVHF_STATS, &addr_hdr, sizeof (*heap_hdr), heap_hdr);
	      VFID_COPY (&heap_hdr->ovf_vfid, ovf_vfid);
	      log_append_redo_data (thread_p, RVHF_STATS, &addr_hdr, sizeof (*heap_hdr), heap_hdr);
	      pgbuf_set_dirty (thread_p, addr_hdr.pgptr, DONT_FREE);

	      log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);
	      (void) file_new_declare_as_old (thread_p, ovf_vfid);
	    }
	  else
	    {
	      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);
	      ovf_vfid = NULL;
	    }
	}
      else
	{
	  ovf_vfid = NULL;
	}
    }
  else
    {
      VFID_COPY (ovf_vfid, &heap_hdr->ovf_vfid);
    }

  pgbuf_unfix_and_init (thread_p, addr_hdr.pgptr);

  return ovf_vfid;
}

/*
 * heap_ovf_insert () - Insert the content of a multipage object in overflow
 *   return: OID *(ovf_oid on success or NULL on failure)
 *   hfid(in): Object heap file identifier
 *   ovf_oid(in/out): Overflow address
 *   recdes(in): Record descriptor
 *
 * Note: Insert the content of a multipage object in overflow.
 *
 *	 In MVCC only, if mvcc_relocate_delete is not null, the object is
 *	 inserted in overflow due to delete relocation. Thus, the object
 *	 has been resized during logical deletion (new MVCC fields in header)
 *	 and the new size doesn't fit into a page anymore.
 */
static OID *
heap_ovf_insert (THREAD_ENTRY * thread_p, const HFID * hfid, OID * ovf_oid, RECDES * recdes,
		 MVCC_RELOCATE_DELETE_INFO * mvcc_relocate_delete)
{
  VFID ovf_vfid;
  VPID ovf_vpid;		/* Address of overflow insertion */

  if (heap_ovf_find_vfid (thread_p, hfid, &ovf_vfid, true, PGBUF_UNCONDITIONAL_LATCH) == NULL
      || overflow_insert (thread_p, &ovf_vfid, &ovf_vpid, recdes, NULL, mvcc_relocate_delete) == NULL)
    {
      return NULL;
    }

  ovf_oid->pageid = ovf_vpid.pageid;
  ovf_oid->volid = ovf_vpid.volid;
  ovf_oid->slotid = NULL_SLOTID;	/* Irrelevant */

  return ovf_oid;
}

/*
 * heap_ovf_update () - Update the content of a multipage object
 *   return: OID *(ovf_oid on success or NULL on failure)
 *   hfid(in): Object heap file identifier
 *   ovf_oid(in): Overflow address
 *   recdes(in): Record descriptor
 *
 * Note: Update the content of a multipage object.
 */
static const OID *
heap_ovf_update (THREAD_ENTRY * thread_p, const HFID * hfid, const OID * ovf_oid, RECDES * recdes)
{
  VFID ovf_vfid;
  VPID ovf_vpid;

  if (heap_ovf_find_vfid (thread_p, hfid, &ovf_vfid, false, PGBUF_UNCONDITIONAL_LATCH) == NULL)
    {
      return NULL;
    }

  ovf_vpid.pageid = ovf_oid->pageid;
  ovf_vpid.volid = ovf_oid->volid;

  if (overflow_update (thread_p, &ovf_vfid, &ovf_vpid, recdes) == NULL)
    {
      return NULL;
    }
  else
    {
      return ovf_oid;
    }
}

/*
 * heap_ovf_delete () - Delete the content of a multipage object
 *   return: OID *(ovf_oid on success or NULL on failure)
 *   hfid(in): Object heap file identifier
 *   ovf_oid(in): Overflow address
 *   ovf_vfid_p(in): Overflow file identifier. If given argument is NULL,
 *		     it must be obtained from heap file header.
 *
 * Note: Delete the content of a multipage object.
 */
const OID *
heap_ovf_delete (THREAD_ENTRY * thread_p, const HFID * hfid, const OID * ovf_oid, VFID * ovf_vfid_p)
{
  VFID ovf_vfid;
  VPID ovf_vpid;

  if (ovf_vfid_p == NULL || VFID_ISNULL (ovf_vfid_p))
    {
      /* Get overflow file VFID from heap file header. */
      ovf_vfid_p = (ovf_vfid_p != NULL) ? ovf_vfid_p : &ovf_vfid;
      if (heap_ovf_find_vfid (thread_p, hfid, ovf_vfid_p, false, PGBUF_UNCONDITIONAL_LATCH) == NULL)
	{
	  return NULL;
	}
    }

  ovf_vpid.pageid = ovf_oid->pageid;
  ovf_vpid.volid = ovf_oid->volid;

  if (overflow_delete (thread_p, ovf_vfid_p, &ovf_vpid) == NULL)
    {
      return NULL;
    }
  else
    {
      return ovf_oid;
    }

}

/*
 * heap_ovf_flush () - Flush all overflow dirty pages where the object resides
 *   return: NO_ERROR
 *   ovf_oid(in): Overflow address
 *
 * Note: Flush all overflow dirty pages where the object resides.
 */
static int
heap_ovf_flush (THREAD_ENTRY * thread_p, const OID * ovf_oid)
{
  VPID ovf_vpid;

  ovf_vpid.pageid = ovf_oid->pageid;
  ovf_vpid.volid = ovf_oid->volid;
  overflow_flush (thread_p, &ovf_vpid);

  return NO_ERROR;
}

/*
 * heap_ovf_get_length () - Find length of overflow object
 *   return: length
 *   ovf_oid(in): Overflow address
 *
 * Note: The length of the content of a multipage object associated
 * with the given overflow address is returned. In the case of
 * any error, -1 is returned.
 */
static int
heap_ovf_get_length (THREAD_ENTRY * thread_p, const OID * ovf_oid)
{
  VPID ovf_vpid;

  ovf_vpid.pageid = ovf_oid->pageid;
  ovf_vpid.volid = ovf_oid->volid;

  return overflow_get_length (thread_p, &ovf_vpid);
}

/*
 * heap_ovf_get () - get/retrieve the content of a multipage object from overflow
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS, S_DOESNT_FIT, S_END)
 *   ovf_oid(in): Overflow address
 *   recdes(in): Record descriptor
 *   chn(in):
 *
 * Note: The content of a multipage object associated with the given
 * overflow address(oid) is placed into the area pointed to by
 * the record descriptor. If the content of the object does not
 * fit in such an area (i.e., recdes->area_size), an error is
 * returned and a hint of its length is returned as a negative
 * value in recdes->length. The length of the retrieved object is
 * set in the the record descriptor (i.e., recdes->length).
 */
static SCAN_CODE
heap_ovf_get (THREAD_ENTRY * thread_p, const OID * ovf_oid, RECDES * recdes, int chn, MVCC_SNAPSHOT * mvcc_snapshot)
{
  VPID ovf_vpid;
  int rest_length;
  SCAN_CODE scan;

  ovf_vpid.pageid = ovf_oid->pageid;
  ovf_vpid.volid = ovf_oid->volid;

  if (chn != NULL_CHN)
    {
      /* 
       * This assumes that most of the time, we have the right cache coherency
       * number and that it is expensive to copy the overflow object to be
       * thrown most of the time. Thus, it is OK to do some extra page look up
       * when failures (it should be OK since the overflow page should be
       * already in the page buffer pool.
       */

      scan = overflow_get_nbytes (thread_p, &ovf_vpid, recdes, 0, OR_MVCC_MAX_HEADER_SIZE, &rest_length, mvcc_snapshot);
      if (scan == S_SUCCESS && chn == or_chn (recdes))
	{
	  return S_SUCCESS_CHN_UPTODATE;
	}
    }
  scan = overflow_get (thread_p, &ovf_vpid, recdes, mvcc_snapshot);

  return scan;
}

/*
 * heap_ovf_get_capacity () - Find space consumed oveflow object
 *   return: NO_ERROR
 *   ovf_oid(in): Overflow address
 *   ovf_len(out): Length of overflow object
 *   ovf_num_pages(out): Total number of overflow pages
 *   ovf_overhead(out): System overhead for overflow record
 *   ovf_free_space(out): Free space for exapnsion of the overflow rec
 *
 * Note: Find the current storage facts/capacity of given overflow rec
 */
static int
heap_ovf_get_capacity (THREAD_ENTRY * thread_p, const OID * ovf_oid, int *ovf_len, int *ovf_num_pages,
		       int *ovf_overhead, int *ovf_free_space)
{
  VPID ovf_vpid;

  ovf_vpid.pageid = ovf_oid->pageid;
  ovf_vpid.volid = ovf_oid->volid;

  return overflow_get_capacity (thread_p, &ovf_vpid, ovf_len, ovf_num_pages, ovf_overhead, ovf_free_space);
}

/*
 * heap_scancache_check_with_hfid () - Check if scancache is on provided HFID
 *				       and reinitialize it otherwise
 *   thread_p(in): thread entry
 *   hfid(in): heap file identifier to check the scancache against
 *   scan_cache(in/out): pointer to scancache pointer
 *   returns: error code or NO_ERROR
 *
 * NOTE: Function may alter the scan cache address. Caller must make sure it
 *       doesn't pass it's only reference to the object OR it is not the owner
 *       of the object.
 * NOTE: Function may alter the members of (*scan_cache).
 */
static int
heap_scancache_check_with_hfid (THREAD_ENTRY * thread_p, HFID * hfid, OID * class_oid, HEAP_SCANCACHE ** scan_cache)
{
  if (*scan_cache != NULL)
    {
      if ((*scan_cache)->debug_initpattern != HEAP_DEBUG_SCANCACHE_INITPATTERN)
	{
	  er_log_debug (ARG_FILE_LINE, "heap_insert: Your scancache is not initialized");
	  *scan_cache = NULL;
	}
      else if (!HFID_EQ (&(*scan_cache)->node.hfid, hfid) || OID_ISNULL (&(*scan_cache)->node.class_oid))
	{
	  int r;

	  /* scancache is not on our heap file, reinitialize it */
	  r = heap_scancache_reset_modify (thread_p, *scan_cache, hfid, class_oid);
	  if (r != NO_ERROR)
	    {
	      return r;
	    }
	}
    }

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_scancache_start_internal () - Start caching information for a heap scan
 *   return: NO_ERROR
 *   scan_cache(in/out): Scan cache
 *   hfid(in): Heap file identifier of the scan cache or NULL
 *             If NULL is given heap_get is the only function that can
 *             be used with the scan cache.
 *   class_oid(in): Class identifier of scan cache
 *                  For any class, NULL or NULL_OID can be given
 *   cache_last_fix_page(in): Wheater or not to cache the last fetched page
 *                            between scan objects ?
 *   is_queryscan(in):
 *   is_indexscan(in):
 *
 */
static int
heap_scancache_start_internal (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, const HFID * hfid,
			       const OID * class_oid, int cache_last_fix_page, int is_queryscan, int is_indexscan,
			       MVCC_SNAPSHOT * mvcc_snapshot)
{
  LOCK class_lock = NULL_LOCK;
  int ret = NO_ERROR;

  if (class_oid != NULL)
    {
      /* 
       * Scanning the instances of a specific class
       */
      scan_cache->node.class_oid = *class_oid;

      if (is_queryscan == true)
	{
	  /* 
	   * Acquire a lock for the heap scan so that the class is not updated
	   * during the scan of the heap. This can happen in transaction isolation
	   * levels that release the locks of the class when the class is read.
	   */
	  if (lock_scan (thread_p, class_oid, LK_UNCOND_LOCK, IS_LOCK) != LK_GRANTED)
	    {
	      goto exit_on_error;
	    }
	}
    }
  else
    {
      /* 
       * Scanning the instances of any class in the heap
       */
      OID_SET_NULL (&scan_cache->node.class_oid);
    }


  if (hfid == NULL)
    {
      HFID_SET_NULL (&scan_cache->node.hfid);
      scan_cache->node.hfid.vfid.volid = NULL_VOLID;
      scan_cache->file_type = FILE_UNKNOWN_TYPE;
    }
  else
    {
#if defined(CUBRID_DEBUG)
      DISK_ISVALID valid_file;

      valid_file = file_isvalid (&hfid->vfid);
      if (valid_file != DISK_VALID)
	{
	  if (class_oid != NULL && is_queryscan == true)
	    {
	      lock_unlock_scan (thread_p, class_oid, END_SCAN);
	    }
	  if (valid_file != DISK_ERROR)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_HEAP, 3,
		      fileio_get_volume_label (hfid->vfid.volid, PEEK), hfid->vfid.fileid, hfid->hpgid);
	    }
	  goto exit_on_error;
	}
#endif
      scan_cache->node.hfid.vfid.volid = hfid->vfid.volid;
      scan_cache->node.hfid.vfid.fileid = hfid->vfid.fileid;
      scan_cache->node.hfid.hpgid = hfid->hpgid;
      scan_cache->file_type = file_get_type (thread_p, &hfid->vfid);
      if (scan_cache->file_type == FILE_UNKNOWN_TYPE)
	{
	  goto exit_on_error;
	}
    }

  scan_cache->page_latch = S_LOCK;

  scan_cache->cache_last_fix_page = cache_last_fix_page;
  PGBUF_INIT_WATCHER (&(scan_cache->page_watcher), PGBUF_ORDERED_HEAP_NORMAL, hfid);
  scan_cache->area = NULL;
  scan_cache->area_size = -1;
  scan_cache->num_btids = 0;
  scan_cache->index_stat_info = NULL;

  scan_cache->debug_initpattern = HEAP_DEBUG_SCANCACHE_INITPATTERN;
  scan_cache->mvcc_snapshot = mvcc_snapshot;
  scan_cache->partition_list = NULL;

  return ret;

exit_on_error:

  HFID_SET_NULL (&scan_cache->node.hfid);
  scan_cache->node.hfid.vfid.volid = NULL_VOLID;
  OID_SET_NULL (&scan_cache->node.class_oid);
  scan_cache->page_latch = NULL_LOCK;
  scan_cache->cache_last_fix_page = false;
  PGBUF_INIT_WATCHER (&(scan_cache->page_watcher), PGBUF_ORDERED_RANK_UNDEFINED, PGBUF_ORDERED_NULL_HFID);
  scan_cache->area = NULL;
  scan_cache->area_size = 0;
  scan_cache->num_btids = 0;
  scan_cache->index_stat_info = NULL;
  scan_cache->file_type = FILE_UNKNOWN_TYPE;
  scan_cache->debug_initpattern = 0;
  scan_cache->mvcc_snapshot = NULL;
  scan_cache->partition_list = NULL;

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_scancache_start () - Start caching information for a heap scan
 *   return: NO_ERROR
 *   scan_cache(in/out): Scan cache
 *   hfid(in): Heap file identifier of the scan cache or NULL
 *             If NULL is given heap_get is the only function that can
 *             be used with the scan cache.
 *   class_oid(in): Class identifier of scan cache
 *                  For any class, NULL or NULL_OID can be given
 *   cache_last_fix_page(in): Wheater or not to cache the last fetched page
 *                            between scan objects ?
 *   is_indexscan(in):
 *
 */
int
heap_scancache_start (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, const HFID * hfid, const OID * class_oid,
		      int cache_last_fix_page, int is_indexscan, MVCC_SNAPSHOT * mvcc_snapshot)
{
  return heap_scancache_start_internal (thread_p, scan_cache, hfid, class_oid, cache_last_fix_page, true, is_indexscan,
					mvcc_snapshot);
}

/*
 * heap_scancache_start_modify () - Start caching information for heap
 *                                modifications
 *   return: NO_ERROR
 *   scan_cache(in/out): Scan cache
 *   hfid(in): Heap file identifier of the scan cache or NULL
 *             If NULL is given heap_get is the only function that can
 *             be used with the scan cache.
 *   class_oid(in): Class identifier of scan cache
 *                  For any class, NULL or NULL_OID can be given
 *   op_type(in):
 *
 * Note: A scancache structure is started for heap modifications.
 * The scan_cache structure is used to modify objects of the heap
 * with heap_insert, heap_update, and heap_delete. The scan structure
 * is used to cache information about the latest used page which
 * can be used by the following function to guess where to insert
 * objects, or other updates and deletes on the same page.
 * Good when we are updating things in a sequential way.
 *
 * The heap manager automatically resets the scan_cache structure
 * when it is used with a different heap. That is, the scan_cache
 * is reset with the heap and class of the insertion, update, and
 * delete. Therefore, you could pass NULLs to hfid, and class_oid
 * to this function, but that it is not recommended.
 */
int
heap_scancache_start_modify (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, const HFID * hfid,
			     const OID * class_oid, int op_type, MVCC_SNAPSHOT * mvcc_snapshot)
{
  OR_CLASSREP *classrepr = NULL;
  int classrepr_cacheindex = -1;
  int malloc_size, i;
  int ret = NO_ERROR;

  if (heap_scancache_start_internal (thread_p, scan_cache, hfid, NULL, false, false, false, mvcc_snapshot) != NO_ERROR)
    {
      goto exit_on_error;
    }

  if (class_oid != NULL)
    {
      ret = heap_scancache_reset_modify (thread_p, scan_cache, hfid, class_oid);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }
  else
    {
      scan_cache->page_latch = X_LOCK;
    }

  if (BTREE_IS_MULTI_ROW_OP (op_type) && class_oid != NULL && !OID_EQ (class_oid, oid_Root_class_oid))
    {
      /* get class representation to find the total number of indexes */
      classrepr = heap_classrepr_get (thread_p, (OID *) class_oid, NULL, NULL_REPRID, &classrepr_cacheindex);
      if (classrepr == NULL)
	{
	  goto exit_on_error;
	}
      scan_cache->num_btids = classrepr->n_indexes;

      if (scan_cache->num_btids > 0)
	{
	  /* allocate local btree statistical information structure */
	  malloc_size = sizeof (BTREE_UNIQUE_STATS) * scan_cache->num_btids;

	  if (scan_cache->index_stat_info != NULL)
	    {
	      db_private_free (thread_p, scan_cache->index_stat_info);
	    }

	  scan_cache->index_stat_info = (BTREE_UNIQUE_STATS *) db_private_alloc (thread_p, malloc_size);
	  if (scan_cache->index_stat_info == NULL)
	    {
	      ret = ER_OUT_OF_VIRTUAL_MEMORY;
	      (void) heap_classrepr_free (classrepr, &classrepr_cacheindex);
	      goto exit_on_error;
	    }
	  /* initialize the structure */
	  for (i = 0; i < scan_cache->num_btids; i++)
	    {
	      BTID_COPY (&(scan_cache->index_stat_info[i].btid), &(classrepr->indexes[i].btid));
	      scan_cache->index_stat_info[i].num_nulls = 0;
	      scan_cache->index_stat_info[i].num_keys = 0;
	      scan_cache->index_stat_info[i].num_oids = 0;
	    }
	}

      /* free class representation */
      ret = heap_classrepr_free (classrepr, &classrepr_cacheindex);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }

  /* In case of SINGLE_ROW_INSERT, SINGLE_ROW_UPDATE, SINGLE_ROW_DELETE, or SINGLE_ROW_MODIFY, the 'num_btids' and
   * 'index_stat_info' of scan cache structure have to be set as 0 and NULL, respectively. */

  return ret;

exit_on_error:

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_scancache_force_modify () -
 *   return: NO_ERROR
 *   scan_cache(in):
 */
static int
heap_scancache_force_modify (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache)
{
  if (scan_cache == NULL || scan_cache->debug_initpattern != HEAP_DEBUG_SCANCACHE_INITPATTERN)
    {
      return NO_ERROR;
    }

  /* Free fetched page */
  if (scan_cache->page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &(scan_cache->page_watcher));
    }

  return NO_ERROR;
}

/*
 * heap_scancache_reset_modify () - Reset the current caching information
 *   return: NO_ERROR
 *   scan_cache(in/out): Scan cache
 *   hfid(in): Heap file identifier of the scan cache
 *   class_oid(in): Class identifier of scan cache
 *
 * Note: Any page that has been cached under the current scan cache is
 * freed and the scancache structure is reinitialized with the
 * new information.
 */
static int
heap_scancache_reset_modify (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, const HFID * hfid,
			     const OID * class_oid)
{
  int ret;

  ret = heap_scancache_force_modify (thread_p, scan_cache);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  if (class_oid != NULL)
    {
      scan_cache->node.class_oid = *class_oid;
    }
  else
    {
      OID_SET_NULL (&scan_cache->node.class_oid);
    }

  if (!HFID_EQ (&scan_cache->node.hfid, hfid))
    {
      scan_cache->node.hfid.vfid.volid = hfid->vfid.volid;
      scan_cache->node.hfid.vfid.fileid = hfid->vfid.fileid;
      scan_cache->node.hfid.hpgid = hfid->hpgid;

      scan_cache->file_type = file_get_type (thread_p, &hfid->vfid);
      if (scan_cache->file_type == FILE_UNKNOWN_TYPE)
	{
	  ASSERT_ERROR_AND_SET (ret);
	  return ret;
	}
    }

  scan_cache->page_latch = X_LOCK;

  return ret;
}

/*
 * heap_scancache_quick_start () - Start caching information for a heap scan
 *   return: NO_ERROR
 *   scan_cache(in/out): Scan cache
 *
 * Note: This is a quick way to initialize a scancahe structure. It
 * should be used only when we would like to peek only one object
 * (heap_get). This function will cache the last fetched page by default.
 *
 *  This function was created to avoid some of the overhead
 *  associated with scancahe(e.g., find best pages, lock the heap)
 *  since we are not really scanning the heap.
 *
 *  For other needs/uses, please refer to heap_scancache_start ().
 *
 * Note: Using many scancaches with the cached_fix page option at the
 * same time should be avoided since page buffers are fixed and
 * locked for future references and there is a limit of buffers
 * in the page buffer pool. This is analogous to fetching many
 * pages at the same time. The page buffer pool is expanded when
 * needed, however, developers must pay special attention to
 * avoid this situation.
 */
int
heap_scancache_quick_start (HEAP_SCANCACHE * scan_cache)
{
  heap_scancache_quick_start_internal (scan_cache, NULL);

  scan_cache->page_latch = S_LOCK;

  return NO_ERROR;
}

/*
 * heap_scancache_quick_start_modify () - Start caching information
 *                                      for a heap modifications
 *   return: NO_ERROR
 *   scan_cache(in/out): Scan cache
 */
int
heap_scancache_quick_start_modify (HEAP_SCANCACHE * scan_cache)
{
  heap_scancache_quick_start_internal (scan_cache, NULL);

  scan_cache->page_latch = X_LOCK;

  return NO_ERROR;
}

/*
 * heap_scancache_quick_start_internal () -
 *
 *   return: NO_ERROR
 *   scan_cache(in/out): Scan cache
 */
static int
heap_scancache_quick_start_internal (HEAP_SCANCACHE * scan_cache, const HFID * hfid)
{
  HFID_SET_NULL (&scan_cache->node.hfid);
  if (hfid == NULL)
    {
      scan_cache->node.hfid.vfid.volid = NULL_VOLID;
      PGBUF_INIT_WATCHER (&(scan_cache->page_watcher), PGBUF_ORDERED_HEAP_NORMAL, PGBUF_ORDERED_NULL_HFID);
    }
  else
    {
      HFID_COPY (&scan_cache->node.hfid, hfid);
      PGBUF_INIT_WATCHER (&(scan_cache->page_watcher), PGBUF_ORDERED_HEAP_NORMAL, hfid);
    }
  OID_SET_NULL (&scan_cache->node.class_oid);
  scan_cache->page_latch = S_LOCK;
  scan_cache->cache_last_fix_page = true;
  scan_cache->area = NULL;
  scan_cache->area_size = 0;
  scan_cache->num_btids = 0;
  scan_cache->index_stat_info = NULL;
  scan_cache->file_type = FILE_UNKNOWN_TYPE;
  scan_cache->debug_initpattern = HEAP_DEBUG_SCANCACHE_INITPATTERN;
  scan_cache->mvcc_snapshot = NULL;
  scan_cache->partition_list = NULL;

  return NO_ERROR;
}

/*
 * heap_scancache_quick_end () - Stop caching information for a heap scan
 *   return: NO_ERROR
 *   scan_cache(in/out): Scan cache
 *
 * Note: Any fixed heap page on the given scan is freed and any memory
 * allocated by this scan is also freed. The scan_cache structure
 * is undefined.  This function does not update any space statistics.
 */
static int
heap_scancache_quick_end (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache)
{
  int ret = NO_ERROR;

  if (scan_cache->debug_initpattern != HEAP_DEBUG_SCANCACHE_INITPATTERN)
    {
      er_log_debug (ARG_FILE_LINE, "heap_scancache_quick_end: Your scancache is not initialized");
      ret = ER_GENERIC_ERROR;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 0);
    }
  else
    {
      if (scan_cache->index_stat_info != NULL)
	{
	  /* deallocate memory space allocated for index stat info. */
	  db_private_free_and_init (thread_p, scan_cache->index_stat_info);
	  scan_cache->num_btids = 0;
	}

      if (scan_cache->cache_last_fix_page == true)
	{
	  /* Free fetched page */
	  if (scan_cache->page_watcher.pgptr != NULL)
	    {
	      pgbuf_ordered_unfix (thread_p, &scan_cache->page_watcher);
	    }
	}

      /* Free memory */
      if (scan_cache->area)
	{
	  db_private_free_and_init (thread_p, scan_cache->area);
	}

      if (scan_cache->partition_list)
	{
	  HEAP_SCANCACHE_NODE_LIST *next_node = NULL;
	  HEAP_SCANCACHE_NODE_LIST *curr_node = NULL;

	  curr_node = scan_cache->partition_list;

	  while (curr_node != NULL)
	    {
	      next_node = curr_node->next;
	      db_private_free_and_init (thread_p, curr_node);
	      curr_node = next_node;
	    }
	}
    }

  HFID_SET_NULL (&scan_cache->node.hfid);
  scan_cache->node.hfid.vfid.volid = NULL_VOLID;
  OID_SET_NULL (&scan_cache->node.class_oid);
  scan_cache->page_latch = NULL_LOCK;
  assert (PGBUF_IS_CLEAN_WATCHER (&(scan_cache->page_watcher)));
  scan_cache->area = NULL;
  scan_cache->area_size = 0;
  scan_cache->file_type = FILE_UNKNOWN_TYPE;
  scan_cache->debug_initpattern = 0;

  return ret;
}

/*
 * heap_scancache_end_internal () -
 *   return: NO_ERROR
 *   scan_cache(in):
 *   scan_state(in):
 */
static int
heap_scancache_end_internal (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, bool scan_state)
{
  int ret = NO_ERROR;
  HEAP_SCANCACHE_NODE_LIST *p = NULL;

  if (scan_cache->debug_initpattern != HEAP_DEBUG_SCANCACHE_INITPATTERN)
    {
      er_log_debug (ARG_FILE_LINE, "heap_scancache_end_internal: Your scancache is not initialized");
      return ER_FAILED;
    }

  if (!OID_ISNULL (&scan_cache->node.class_oid))
    {
      lock_unlock_scan (thread_p, &scan_cache->node.class_oid, scan_state);
    }

  p = scan_cache->partition_list;
  while (p != NULL)
    {
      if (!OID_EQ (&p->node.class_oid, &scan_cache->node.class_oid))
	{
	  lock_unlock_scan (thread_p, &p->node.class_oid, scan_state);
	}
      p = p->next;
    }

  OID_SET_NULL (&scan_cache->node.class_oid);

  ret = heap_scancache_quick_end (thread_p, scan_cache);

  return ret;
}

/*
 * heap_scancache_end () - Stop caching information for a heap scan
 *   return: NO_ERROR
 *   scan_cache(in/out): Scan cache
 *
 * Note: Any fixed heap page on the given scan is freed and any memory
 * allocated by this scan is also freed. The scan_cache structure is undefined.
 */
int
heap_scancache_end (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache)
{
  int ret;

  ret = heap_scancache_end_internal (thread_p, scan_cache, END_SCAN);

  return NO_ERROR;
}

/*
 * heap_scancache_end_when_scan_will_resume () -
 *   return:
 *   scan_cache(in):
 */
int
heap_scancache_end_when_scan_will_resume (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache)
{
  int ret;

  ret = heap_scancache_end_internal (thread_p, scan_cache, CONTINUE_SCAN);

  return NO_ERROR;
}

/*
 * heap_scancache_end_modify () - End caching information for a heap
 *				  modification cache
 *   return:
 *   scan_cache(in/out): Scan cache
 *
 * Note: Any fixed heap page on the given scan is freed. The heap
 * best find space statistics for the heap are completely updated
 * with the ones stored in the scan cache.
 */
void
heap_scancache_end_modify (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache)
{
  int ret;

  ret = heap_scancache_force_modify (thread_p, scan_cache);
  if (ret == NO_ERROR)
    {
      ret = heap_scancache_quick_end (thread_p, scan_cache);
    }
}

/*
 * heap_get_if_diff_chn () - Get specified object of the given slotted page when
 *                       its cache coherency number is different
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS,
 *                      S_SUCCESS_CHN_UPTODATE,
 *                      S_DOESNT_FIT,
 *                      S_DOESNT_EXIST)
 *   pgptr(in): Pointer to slotted page
 *   slotid(in): Slot identifier of current record.
 *   recdes(in/out): Pointer to a record descriptor. Will be modified to
 *                   describe the desired record.
 *   ispeeking(in): Indicates whether the record is going to be copied
 *                  (like a copy) or peeked (read at the buffer).
 *   chn(in): Cache coherency number or NULL_CHN
 *
 * Note: If the given CHN is the same as the chn of the specified
 * object in the slotted page, the object may not be placed in
 * the given record descriptor. If the given CHN is NULL_CHN or
 * is not given, then the following process is followed depending
 * upon if we are peeking or not:
 * When ispeeking is PEEK, the desired record is peeked onto the
 * buffer pool. The address of the record descriptor is set
 * to the portion of the buffer pool where the record is stored.
 * For more information on peeking description, see the slotted module.
 *
 * When ispeeking is COPY, the desired record is read
 * onto the area pointed by the record descriptor. If the record
 * does not fit in such an area, the length of the record is
 * returned as a negative value in recdes->length and an error
 * condition is indicated.
 */
static SCAN_CODE
heap_get_if_diff_chn (THREAD_ENTRY * thread_p, PAGE_PTR pgptr, INT16 slotid, RECDES * recdes, int ispeeking, int chn,
		      MVCC_SNAPSHOT * mvcc_snapshot)
{
  RECDES chn_recdes;		/* Used when we need to compare the cache coherency number and we are not peeking */
  SCAN_CODE scan;
  MVCC_REC_HEADER mvcc_header;

  /* 
   * Don't retrieve the object when the object has the same cache
   * coherency number given by the caller. That is, the caller has the
   * valid cached object.
   */

  if (ispeeking == PEEK)
    {
      scan = spage_get_record (pgptr, slotid, recdes, PEEK);
      if (scan != S_SUCCESS)
	{
	  return scan;
	}

      /* For MVCC we need to obtain header and verify header */
      or_mvcc_get_header (recdes, &mvcc_header);
      if (scan == S_SUCCESS && mvcc_snapshot != NULL && mvcc_snapshot->snapshot_fnc != NULL)
	{
	  if (mvcc_snapshot->snapshot_fnc (thread_p, &mvcc_header, mvcc_snapshot) != true)
	    {
	      return S_SNAPSHOT_NOT_SATISFIED;
	    }
	}
      if (chn != NULL_CHN && (!MVCC_SHOULD_TEST_CHN (thread_p, &mvcc_header) || chn == MVCC_GET_CHN (&mvcc_header)))
	{
	  /* Test chn if MVCC is disabled for record or if delete MVCCID is invalid and the record is inserted by
	   * current transaction. */
	  /* When testing chn is not required, the result is considered up-to-date. */
	  scan = S_SUCCESS_CHN_UPTODATE;
	}
    }
  else
    {
      scan = spage_get_record (pgptr, slotid, &chn_recdes, PEEK);
      if (scan != S_SUCCESS)
	{
	  return scan;
	}

      /* For MVCC we need to obtain header and verify header */
      or_mvcc_get_header (&chn_recdes, &mvcc_header);
      if (scan == S_SUCCESS && mvcc_snapshot != NULL && mvcc_snapshot->snapshot_fnc != NULL)
	{
	  if (mvcc_snapshot->snapshot_fnc (thread_p, &mvcc_header, mvcc_snapshot) != true)
	    {
	      return S_SNAPSHOT_NOT_SATISFIED;
	    }
	}
      if (chn != NULL_CHN && (!MVCC_SHOULD_TEST_CHN (thread_p, &mvcc_header) || chn == MVCC_GET_CHN (&mvcc_header)))
	{
	  /* Test chn if MVCC is disabled for record or if delete MVCCID is invalid and the record is inserted by
	   * current transaction. */
	  /* When testing chn is not required, the result is considered up-to-date. */
	  scan = S_SUCCESS_CHN_UPTODATE;
	}

      if (scan != S_SUCCESS_CHN_UPTODATE)
	{
	  /* 
	   * Note that we could copy the recdes.data from chn_recdes.data, but
	   * I don't think it is much difference here, and we will have to deal
	   * with all not fit conditions and so on, so we decide to use
	   * spage_get_record instead.
	   */
	  scan = spage_get_record (pgptr, slotid, recdes, COPY);
	}
    }

  return scan;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * heap_get_chn () - Get the chn of the object
 *   return: chn or NULL_CHN
 *   oid(in): Object identifier
 *
 * Note: Find the cache coherency number of the object.
 */
int
heap_get_chn (THREAD_ENTRY * thread_p, const OID * oid)
{
  RECDES recdes;
  HEAP_SCANCACHE scan_cache;
  int chn;

  chn = heap_chnguess_get (thread_p, oid, LOG_FIND_THREAD_TRAN_INDEX (thread_p));

  if (chn == NULL_CHN)
    {
      heap_scancache_quick_start (&scan_cache);
      if (heap_get (thread_p, oid, &recdes, &scan_cache, PEEK, NULL_CHN) == S_SUCCESS)
	{
	  chn = or_chn (&recdes);
	}
      heap_scancache_end (thread_p, &scan_cache);
    }

  return chn;
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * heap_get () - Retrieve or peek an object
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS,
 *                      S_SUCCESS_CHN_UPTODATE,
 *                      S_DOESNT_FIT,
 *                      S_DOESNT_EXIST,
 *                      S_ERROR)
 *   oid(in): Object identifier
 *   recdes(in/out): Record descriptor
 *   scan_cache(in/out): Scan cache or NULL
 *   ispeeking(in): PEEK when the object is peeked, scan_cache cannot be NULL
 *                  COPY when the object is copied
 *   chn(in):
 *
 */
SCAN_CODE
heap_get (THREAD_ENTRY * thread_p, const OID * oid, RECDES * recdes, HEAP_SCANCACHE * scan_cache, int ispeeking,
	  int chn)
{
  return heap_get_internal (thread_p, NULL, oid, recdes, scan_cache, ispeeking, chn);
}

/*
 * heap_get_internal () - Retrieve or peek an object
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS,
 *                      S_SUCCESS_CHN_UPTODATE,
 *                      S_DOESNT_FIT,
 *                      S_DOESNT_EXIST,
 *                      S_ERROR)
 *   class_oid(out):
 *   oid(in): Object identifier
 *   recdes(in/out): Record descriptor
 *   scan_cache(in/out): Scan cache or NULL
 *   ispeeking(in): PEEK when the object is peeked, scan_cache cannot be NULL
 *                  COPY when the object is copied
 *   chn(in):
 *
 *   Note :
 *   If the type of record is REC_UNKNOWN(MARK_DELETED or DELETED_WILL_REUSE),
 *   this function returns S_DOESNT_EXIST and set warning.
 *   In this case, The caller should verify this.
 *   For example, In function scan_next_scan...
 *   If the current isolation level is uncommit_read and
 *   heap_get returns S_DOESNT_EXIST,
 *   then scan_next_scan ignores current record and continue.
 *   But if the isolation level is higher than uncommit_read,
 *   scan_next_scan returns an error.
 */
static SCAN_CODE
heap_get_internal (THREAD_ENTRY * thread_p, OID * class_oid, const OID * oid, RECDES * recdes,
		   HEAP_SCANCACHE * scan_cache, int ispeeking, int chn)
{
  VPID home_vpid, forward_vpid;
  VPID *vpidptr_incache;
  INT16 type;
  OID forward_oid;
  RECDES forward_recdes;
  SCAN_CODE scan;
  DISK_ISVALID oid_valid;
  int again_count = 0;
  int again_max = 20;
#if defined(ENABLE_SYSTEMTAP)
  OID cls_oid;
  bool is_systemtap_started = false;
#endif /* ENABLE_SYSTEMTAP */
  MVCC_SNAPSHOT *mvcc_snapshot = NULL;
  PGBUF_WATCHER home_page_watcher;
  PGBUF_WATCHER fwd_page_watcher;
  int error_code = NO_ERROR;
  OID forward_rec_buffer[2];
  bool need_buffer = false;

  PGBUF_INIT_WATCHER (&home_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, HEAP_SCAN_ORDERED_HFID (scan_cache));
  PGBUF_INIT_WATCHER (&fwd_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, HEAP_SCAN_ORDERED_HFID (scan_cache));

#if !defined (NDEBUG)
  if (scan_cache == NULL && ispeeking == PEEK)
    {
      er_log_debug (ARG_FILE_LINE, "heap_get: Using wrong interface. scan_cache cannot be NULL when peeking.");
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      assert (0);
      return S_ERROR;
    }

  if (scan_cache != NULL && scan_cache->debug_initpattern != HEAP_DEBUG_SCANCACHE_INITPATTERN)
    {
      er_log_debug (ARG_FILE_LINE, "heap_get: Your scancache is not initialized");
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      assert (0);
      return S_ERROR;
    }
#endif /* !NDEBUG */

  assert (recdes != NULL);

  if (scan_cache == NULL)
    {
      /* It is possible only in case of ispeeking == COPY */
      if (recdes->data == NULL)
	{
	  er_log_debug (ARG_FILE_LINE, "heap_get: Using wrong interface. recdes->area_size cannot be -1.");
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  return S_ERROR;
	}
    }
  else
    {
      mvcc_snapshot = scan_cache->mvcc_snapshot;
    }

  assert (scan_cache != NULL || (ispeeking == COPY && recdes->data != NULL));

  oid_valid = HEAP_ISVALID_OID (oid);
  if (oid_valid != DISK_VALID)
    {
      if (oid_valid != DISK_ERROR || er_errid () == NO_ERROR)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid, oid->slotid);
	}
      return S_DOESNT_EXIST;
    }

  home_vpid.volid = oid->volid;
  home_vpid.pageid = oid->pageid;

  /* 
   * Use previous scan page whenever possible, otherwise, deallocate the
   * page
   */

  if (scan_cache != NULL && scan_cache->cache_last_fix_page == true && scan_cache->page_watcher.pgptr != NULL)
    {
      vpidptr_incache = pgbuf_get_vpid_ptr (scan_cache->page_watcher.pgptr);
      if (VPID_EQ (&home_vpid, vpidptr_incache))
	{
	  /* We can skip the fetch operation, just switch page watcher */
	  pgbuf_replace_watcher (thread_p, &scan_cache->page_watcher, &home_page_watcher);
	}
      else
	{
	  /* Free the previous scan page and obtain a new page */
	  pgbuf_ordered_unfix (thread_p, &scan_cache->page_watcher);
	  home_page_watcher.pgptr =
	    heap_scan_pb_lock_and_fetch (thread_p, &home_vpid, OLD_PAGE, S_LOCK, scan_cache, &home_page_watcher);
	  if (home_page_watcher.pgptr == NULL)
	    {
	      if (er_errid () == ER_PB_BAD_PAGEID)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid,
			  oid->slotid);
		}

	      /* something went wrong, return */
	      scan = S_ERROR;
	      goto end;
	    }
	}
      assert (PGBUF_IS_CLEAN_WATCHER (&scan_cache->page_watcher));
    }
  else
    {
      home_page_watcher.pgptr =
	heap_scan_pb_lock_and_fetch (thread_p, &home_vpid, OLD_PAGE, S_LOCK, scan_cache, &home_page_watcher);
      if (home_page_watcher.pgptr == NULL)
	{
	  if (er_errid () == ER_PB_BAD_PAGEID)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid,
		      oid->slotid);
	    }

	  /* something went wrong, return */
	  scan = S_ERROR;
	  goto end;
	}
    }

  if (class_oid != NULL)
    {
      if (heap_get_class_oid_from_page (thread_p, home_page_watcher.pgptr, class_oid) != NO_ERROR)
	{
	  pgbuf_ordered_unfix (thread_p, &home_page_watcher);
	  scan = S_ERROR;
	  goto end;
	}

#if defined(ENABLE_SYSTEMTAP)
      COPY_OID (&cls_oid, class_oid);
      CUBRID_OBJ_READ_START (&cls_oid);
      is_systemtap_started = true;
#endif /* ENABLE_SYSTEMTAP */
    }
  else
    {
#if defined(ENABLE_SYSTEMTAP)
      if (heap_get_class_oid_from_page (thread_p, home_page_watcher.pgptr, &cls_oid) != NO_ERROR)
	{
	  pgbuf_ordered_unfix (thread_p, &home_page_watcher);
	  scan = S_ERROR;
	  goto end;
	}
      CUBRID_OBJ_READ_START (&cls_oid);
      is_systemtap_started = true;
#endif /* ENABLE_SYSTEMTAP */
    }

  type = spage_get_record_type (home_page_watcher.pgptr, oid->slotid);
  if (type == REC_UNKNOWN)
    {
      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid, oid->slotid);
      pgbuf_ordered_unfix (thread_p, &home_page_watcher);
      scan = S_DOESNT_EXIST;
      goto end;
    }

  switch (type)
    {
    case REC_RELOCATION:
      /* 
       * The record stored on the page is a relocation record, get the new
       * home of the record
       */
      forward_recdes.data = (char *) forward_rec_buffer;
      forward_recdes.area_size = 2 * OR_OID_SIZE;

      scan = spage_get_record (home_page_watcher.pgptr, oid->slotid, &forward_recdes, COPY);
      if (scan != S_SUCCESS)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_BAD_RELOCATION_RECORD, 3, oid->volid, oid->pageid,
		  oid->slotid);
	  pgbuf_ordered_unfix (thread_p, &home_page_watcher);
	  scan = S_DOESNT_EXIST;
	  goto end;
	}

      COPY_OID (&forward_oid, (OID *) forward_recdes.data);

      /* Fetch the page of relocated (forwarded) record */
      forward_vpid.volid = forward_oid.volid;
      forward_vpid.pageid = forward_oid.pageid;

      PGBUF_WATCHER_COPY_GROUP (&fwd_page_watcher, &home_page_watcher);
      /* try to fix forward page conditionally */
      error_code = pgbuf_ordered_fix (thread_p, &forward_vpid, OLD_PAGE, PGBUF_LATCH_READ, &fwd_page_watcher);
      if (error_code != NO_ERROR)
	{
	  if (error_code == ER_LK_PAGE_TIMEOUT && er_errid () == NO_ERROR)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, forward_vpid.volid,
		      forward_vpid.pageid);
	    }

	  scan = S_ERROR;
	  goto end;
	}

      assert (home_page_watcher.pgptr != NULL);
      assert (fwd_page_watcher.pgptr != NULL);

      (void) pgbuf_check_page_ptype (thread_p, fwd_page_watcher.pgptr, PAGE_HEAP);

      type = spage_get_record_type (fwd_page_watcher.pgptr, forward_oid.slotid);
      if (type == REC_MVCC_NEXT_VERSION)
	{
	  scan = S_SNAPSHOT_NOT_SATISFIED;
	  goto end;
	}
      if (type != REC_NEWHOME)
	{
	  assert (false);
	  scan = S_DOESNT_EXIST;
	  goto end;
	}

      /* NOTE that we will COPY to read the record. REC_RELOCATION should always be COPIED. */
      if ((ispeeking == COPY && recdes->data == NULL) || ispeeking == PEEK)
	{
	  assert (scan_cache != NULL);
	  /* It is guaranteed that scan_cache is not NULL. */

	  if (scan_cache->area == NULL)
	    {
	      /* Allocate an area to hold the object. Assume that the object will fit in two pages for not better
	       * estimates. */
	      scan_cache->area_size = DB_PAGESIZE * 2;
	      scan_cache->area = (char *) db_private_alloc (thread_p, scan_cache->area_size);
	      if (scan_cache->area == NULL)
		{
		  scan_cache->area_size = -1;
		  pgbuf_ordered_unfix (thread_p, &home_page_watcher);
		  pgbuf_ordered_unfix (thread_p, &fwd_page_watcher);
		  scan = S_ERROR;
		  goto end;
		}
	    }

	  recdes->data = scan_cache->area;
	  recdes->area_size = scan_cache->area_size;
	  /* The allocated space is enough to save the instance. */
	}

      ispeeking = COPY;
      scan =
	heap_get_if_diff_chn (thread_p, fwd_page_watcher.pgptr, forward_oid.slotid, recdes, ispeeking, chn,
			      mvcc_snapshot);

      if (scan_cache != NULL && scan_cache->cache_last_fix_page == true)
	{
	  /* switch to scancache page watcher */
	  pgbuf_replace_watcher (thread_p, &home_page_watcher, &scan_cache->page_watcher);
	}
      else
	{
	  pgbuf_ordered_unfix (thread_p, &home_page_watcher);
	}

      pgbuf_ordered_unfix (thread_p, &fwd_page_watcher);

      break;

    case REC_ASSIGN_ADDRESS:
      /* Object without content.. only the address has been assigned */
      if (spage_check_slot_owner (thread_p, home_page_watcher.pgptr, oid->slotid) == true)
	{
	  er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HEAP_NODATA_NEWADDRESS, 3, oid->volid, oid->pageid,
		  oid->slotid);
	  scan = S_DOESNT_EXIST;
	}
      else
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid, oid->slotid);
	  scan = S_ERROR;
	}

      if (scan_cache != NULL && scan_cache->cache_last_fix_page == true)
	{
	  /* switch to scancache page watcher */
	  pgbuf_replace_watcher (thread_p, &home_page_watcher, &scan_cache->page_watcher);
	}
      else
	{
	  pgbuf_ordered_unfix (thread_p, &home_page_watcher);
	}

      break;

    case REC_HOME:
      need_buffer = false;
      if (ispeeking == COPY && recdes->data == NULL)
	{
	  need_buffer = true;
	}
      else if (ispeeking == PEEK)
	{
	  assert (scan_cache != NULL);
	  if (scan_cache && scan_cache->cache_last_fix_page == false)
	    {
	      need_buffer = true;

	      /* This should also be COPY not PEEK, because the page is going to be unfixed. */
	      ispeeking = COPY;
	    }
	}

      if (need_buffer)
	{
	  /* It is guaranteed that scan_cache is not NULL. */
	  if (scan_cache->area == NULL)
	    {
	      /* Allocate an area to hold the object. Assume that the object will fit in two pages for not better
	       * estimates. */
	      scan_cache->area_size = DB_PAGESIZE * 2;
	      scan_cache->area = (char *) db_private_alloc (thread_p, scan_cache->area_size);
	      if (scan_cache->area == NULL)
		{
		  scan_cache->area_size = -1;
		  pgbuf_ordered_unfix (thread_p, &home_page_watcher);
		  scan = S_ERROR;
		  goto end;
		}
	    }
	  recdes->data = scan_cache->area;
	  recdes->area_size = scan_cache->area_size;
	  /* The allocated space is enough to save the instance. */
	}

      scan =
	heap_get_if_diff_chn (thread_p, home_page_watcher.pgptr, oid->slotid, recdes, ispeeking, chn, mvcc_snapshot);

      if (scan_cache != NULL && scan_cache->cache_last_fix_page == true)
	{
	  /* switch to scancache page watcher */
	  pgbuf_replace_watcher (thread_p, &home_page_watcher, &scan_cache->page_watcher);
	}
      else
	{
	  pgbuf_ordered_unfix (thread_p, &home_page_watcher);
	}
      break;

    case REC_BIGONE:
      /* Get the address of the content of the multipage object in overflow */
      forward_recdes.data = (char *) forward_rec_buffer;
      forward_recdes.area_size = 2 * OR_OID_SIZE;

      scan = spage_get_record (home_page_watcher.pgptr, oid->slotid, &forward_recdes, COPY);
      if (scan != S_SUCCESS)
	{
	  /* Unable to read overflow address of multipage object */
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_BAD_RELOCATION_RECORD, 3, oid->volid, oid->pageid,
		  oid->slotid);
	  pgbuf_ordered_unfix (thread_p, &home_page_watcher);
	  scan = S_ERROR;
	  goto end;
	}
      COPY_OID (&forward_oid, (OID *) forward_recdes.data);
      pgbuf_ordered_unfix (thread_p, &home_page_watcher);

      /* 
       * Now get the content of the multipage object.
       */

      /* Try to reuse the previously allocated area */
      if (scan_cache != NULL && (ispeeking == PEEK || recdes->data == NULL))
	{
	  if (scan_cache->area == NULL)
	    {
	      /* 
	       * Allocate an area to hold the object. Assume that the object
	       * will fit in two pages for not better estimates. We could call
	       * heap_ovf_get_length, but it may be better to just guess and
	       * realloc if needed.
	       * We could also check the estimates for average object length,
	       * but again, it may be expensive and may not be accurate
	       * for this object.
	       */
	      scan_cache->area_size = DB_PAGESIZE * 2;
	      scan_cache->area = (char *) db_private_alloc (thread_p, scan_cache->area_size);
	      if (scan_cache->area == NULL)
		{
		  scan_cache->area_size = -1;
		  scan = S_ERROR;
		  goto end;
		}
	    }
	  recdes->data = scan_cache->area;
	  recdes->area_size = scan_cache->area_size;

	  while ((scan = heap_ovf_get (thread_p, &forward_oid, recdes, chn, mvcc_snapshot)) == S_DOESNT_FIT)
	    {
	      /* 
	       * The object did not fit into such an area, reallocate a new
	       * area
	       */

	      chn = NULL_CHN;	/* To avoid checking again */

	      recdes->area_size = -recdes->length;
	      recdes->data = (char *) db_private_realloc (thread_p, scan_cache->area, recdes->area_size);
	      if (recdes->data == NULL)
		{
		  scan = S_ERROR;
		  goto end;
		}
	      scan_cache->area_size = recdes->area_size;
	      scan_cache->area = recdes->data;
	    }
	  if (scan != S_SUCCESS)
	    {
	      recdes->data = NULL;
	    }
	}
      else
	{
	  scan = heap_ovf_get (thread_p, &forward_oid, recdes, chn, mvcc_snapshot);
	}

      break;

    case REC_MVCC_NEXT_VERSION:
      /* This is not a case of error... Must be handled outside this function call. */
      if (scan_cache != NULL && scan_cache->cache_last_fix_page)
	{
	  if (home_page_watcher.pgptr != NULL)
	    {
	      /* switch page watcher to scan cache */
	      pgbuf_replace_watcher (thread_p, &home_page_watcher, &scan_cache->page_watcher);
	    }
	  else
	    {
	      assert (PGBUF_IS_CLEAN_WATCHER (&scan_cache->page_watcher));
	    }
	}
      else if (home_page_watcher.pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, &home_page_watcher);
	}
      scan = S_SNAPSHOT_NOT_SATISFIED;
      goto end;

    case REC_MARKDELETED:
    case REC_DELETED_WILL_REUSE:
    case REC_NEWHOME:
    default:
      scan = S_ERROR;
      pgbuf_ordered_unfix (thread_p, &home_page_watcher);
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_BAD_OBJECT_TYPE, 3, oid->volid, oid->pageid, oid->slotid);
      break;
    }

end:
  if (fwd_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &fwd_page_watcher);
    }
  if (home_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &home_page_watcher);
    }
#if defined(ENABLE_SYSTEMTAP)
  if (is_systemtap_started)
    {
      CUBRID_OBJ_READ_END (&cls_oid, scan);
    }
#endif /* ENABLE_SYSTEMTAP */

  return scan;
}

/*
 * heap_mvcc_get_visible () - Try to get the last updated version of an object.
 *		      For non-MVCC is a simple heap_get call, for MVCC it may
 *		      need to follow up the update chain.
 *
 * return		 : SCAN CODE
 *			   - S_SUCCESS_CHN_UPTODATE if the object is up to
 *			     date and obtaining data is not required.
 *			   - S_SUCCESS if obtaining data is required.
 *			   - S_SNAPSHOT_NOT_SATISFIED if the object is not
 *			     visible to current transaction.
 *			   - S_ERROR for errors.
 * thread_p (in)	 : Thread entry.
 * oid (in)		 : Initial OID.
 * class_oid(in/out)	 : Class OID
 * op_type(in)		 : Scan operation type
 * recdes (in)		 : Record descriptor.
 * scan_cache (in)	 : Scan cache.
 * ispeeking (in)	 : PEEK or COPY.
 * updated_oid (in)	 : Pointer to updated OID: If not null and if the
 *			   object has been updated, it will save the OID of
 *			   the updated version.
 * non_ex_handling_type (in): - LOG_ERROR_IF_DELETED: write the 
 *				ER_HEAP_UNKNOWN_OBJECT error to log
 *                            - LOG_WARNING_IF_DELETED: set only warning
 */
SCAN_CODE
heap_mvcc_get_visible (THREAD_ENTRY * thread_p, OID * oid, OID * class_oid, RECDES * recdes,
		       HEAP_SCANCACHE * scan_cache, SCAN_OPERATION_TYPE op_type, int ispeeking, int old_chn,
		       OID * updated_oid, NON_EXISTENT_HANDLING non_ex_handling_type)
{
  return heap_mvcc_lock_and_get_object_version (thread_p, oid, class_oid, recdes, scan_cache, op_type, ispeeking,
						old_chn, NULL, updated_oid, non_ex_handling_type);
}

/*
 * heap_mvcc_get_for_delete () - Get MVCC object version for delete/update.
 *
 * return	       : SCAN_CODE.
 * thread_p (in)       : Thread entry.
 * oid (in)	       : Object OID.
 * class_oid (in)      : Class OID.
 * recdes (out)	       : Record descriptor.
 * scan_cache (in)     : Heap scan cache.
 * ispeeking (in)      : PEEK or COPY.
 * old_chn (in)	       : CHN of known record data.
 * mvcc_reev_data (in) : MVCC reevaluation data.
 * update_oid (out)    : OID of selected version for delete.
 * non_ex_handling_type (in): - LOG_ERROR_IF_DELETED: write the 
 *				ER_HEAP_UNKNOWN_OBJECT error to log
 *                            - LOG_WARNING_IF_DELETED: set only warning
 */
SCAN_CODE
heap_mvcc_get_for_delete (THREAD_ENTRY * thread_p, OID * oid, OID * class_oid, RECDES * recdes,
			  HEAP_SCANCACHE * scan_cache, int ispeeking, int old_chn, MVCC_REEV_DATA * mvcc_reev_data,
			  OID * update_oid, NON_EXISTENT_HANDLING non_ex_handling_type)
{
  return heap_mvcc_lock_and_get_object_version (thread_p, oid, class_oid, recdes, scan_cache, S_DELETE, ispeeking,
						old_chn, mvcc_reev_data, update_oid, non_ex_handling_type);
}

/*
 * heap_mvcc_lock_and_get_object_version () - Generic function to obtain the
 *					      right version and lock it if
 *					      necessary.
 *
 * return	      : SCAN_CODE. Possible values:
 *		        - S_ERROR: for error cases.
 *		        - S_SUCCESS_CHN_UPTODATE: for successful case when
 *		          obtaining record data is not needed (it is already
 *		          up-to-date).
 *		        - S_SUCCESS: for successful case and record data was
 *			   obtained.
 *		        - S_DOESNT_EXIST:  if object was deleted.
 *		        - S_DOESNT_FIT: for successful case but there was not
 *			  enough space to copy all record data.
 * thread_p (in)      : Thread entry.
 * oid (in)	      : Starting version OID.
 * class_oid (in/out) : Class OID.
 * recdes (out)	      : Record descriptor.
 * scan_cache (in)    : Heap scan cache.
 * op_type (in)	      : Operation type (S_SELECT, S_SELECT_WITH_LOCK,
 *		        S_DELETE, S_UPDATE).
 * ispeeking (in)     : PEEK or COPY.
 * old_chn (in)	      : Cache coherency number for existing record data. It is
 *			used by clients to avoid resending record data when
 *			it was not updated.
 * updated_oid (out)  : OID of selected version if it is different than the
 *			original version.
 * non_ex_handling_type (in): - LOG_ERROR_IF_DELETED: write the 
 *				ER_HEAP_UNKNOWN_OBJECT error to log
 *                            - LOG_WARNING_IF_DELETED: set only warning
 *
 * NOTE: This function starts with one object version (given by OID) and must
 *	 obtain the right version depending on operation type.
 *	 S_SELECT operations are supposed to read the "visible" version of
 *	 the object.
 *	 S_SELECT_WITH_LOCK operations are supposed to read the last version
 *	 of the object and also lock it. This can be used to provide
 *	 protection when MVCC Snapshot is not enough.
 *	 S_DELETE and S_UPDATE operations are supposed to modify the object.
 *	 They must obtain X_LOCK on last version of the object. This function
 *	 works the same way for both S_DELETE and S_UPDATE operations.
 *
 *	 The oid argument of the function should point to a version at least
 *	 as old as the "visible version". Giving a newer version than the
 *	 visible version is invalid.
 *	 Most cases would give the visible version. However, client request
 *	 may come with versions older than the visible version.
 *
 * NOTE: If class has disabled MVCC, this function just gets the record data
 *	 of current object, ignoring completely MVCC versions (actually there
 *	 shouldn't be any). No locks are acquired either (it's up to the
 *	 caller to handle locks for non-MVCC classes).
 */
static SCAN_CODE
heap_mvcc_lock_and_get_object_version (THREAD_ENTRY * thread_p, const OID * oid, OID * class_oid, RECDES * recdes,
				       HEAP_SCANCACHE * scan_cache, SCAN_OPERATION_TYPE op_type, int ispeeking,
				       int old_chn, MVCC_REEV_DATA * mvcc_reev_data, OID * updated_oid,
				       NON_EXISTENT_HANDLING non_ex_handling_type)
{
  OID current_oid;		/* Current oid being processed. */
  OID forward_oid;		/* Forward OID - used for REC_RELOCATION/REC_NEWHOME, REC_BIGONE and
				 * REC_MVCC_NEXT_VERSION. */
  SPAGE_SLOT *slot_p = NULL;	/* Pointer to slot. */
  PGBUF_WATCHER home_page_watcher;	/* Page of REC_HOME, REC_BIGONE REC_RELOCATION. */
  PGBUF_WATCHER fwd_page_watcher;	/* Page of REC_NEWHOME and first overflow page. */
  OID class_oid_local;		/* Used to store class_oid if provided argument is NULL. */
  LOCK lock;			/* Lock mode depending on type of operation. */
  SCAN_CODE scan_code;		/* SCAN_CODE result. */
  MVCC_SNAPSHOT *mvcc_snapshot = NULL;	/* MVCC Snapshot used to filter invisible object versions. */
  MVCC_REC_HEADER mvcc_header;	/* Record MVCC header. */
  HEAP_MVCC_DELETE_INFO mvcc_delete_info;	/* Info structure used for delete/update. */
  MVCC_SCAN_REEV_DATA *scan_reev_data = NULL;	/* Scan re-evaluation data. */
  DB_LOGICAL ev_res;		/* Re-evaluation result. */
  RECDES temp_recdes;		/* Record descriptor used to temporary obtain record data for reevaluation. */
  INT16 type;			/* Record type. */
  bool is_version_locked = false;	/* True if current version has been locked. */
  bool is_original_oid = true;	/* True if the current object is the original object given as argument. It will be
				 * needed to verify if record data was changed and needs to be obtained. */
  bool found_visible = false;	/* Set to true when visible object is found. */
  OID current_class_oid;
  OID partition_class_oid;
  OID save_class_oid;
  HFID save_hfid;
  bool vacuum_allowed = false;
  HEAP_SCANCACHE local_scancache;
  bool is_local_scancache_started = false;

  PGBUF_INIT_WATCHER (&home_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, HEAP_SCAN_ORDERED_HFID (scan_cache));
  PGBUF_INIT_WATCHER (&fwd_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, HEAP_SCAN_ORDERED_HFID (scan_cache));

  /* save original scancache node */
  if (scan_cache != NULL)
    {
      COPY_OID (&save_class_oid, &scan_cache->node.class_oid);
      HFID_COPY (&save_hfid, &scan_cache->node.hfid);
    }

  /* Start by checking all arguments are correct. */
  if (scan_cache != NULL)
    {
      /* Get MVCC snapshot from scan_cache. */
      mvcc_snapshot = scan_cache->mvcc_snapshot;
    }

  if (mvcc_snapshot && mvcc_snapshot->snapshot_fnc == mvcc_satisfies_dirty)
    {
      vacuum_allowed = true;
    }

  if (updated_oid != NULL)
    {
      /* Initialize updated OID as NULL. If selected version is different than oid, it will be saved in updated_oid. */
      OID_SET_NULL (updated_oid);
    }

  /* Assert ispeeking, scan_cache and recdes are compatible. If ispeeking is PEEK, we must be able to keep page
   * latched. This means scan_cache must not be NULL and cache_last_fix_page must be true. If ispeeking is COPY, we
   * must have a preallocated area to copy to. This means either scan_cache is not NULL (and scan_cache->area can be
   * used) or recdes->data is not NULL (and recdes->area_size defines how much can be copied). */

  if (recdes != NULL
      && ((ispeeking == PEEK && (scan_cache == NULL || !scan_cache->cache_last_fix_page))
	  || (ispeeking == COPY && scan_cache == NULL && recdes->data == NULL)))
    {
      assert_release (false);
      return S_ERROR;
    }

  if (scan_cache == NULL)
    {
      /* we will need to lock the partitions that would be traversed for partition links. */
      if (heap_scancache_quick_start (&local_scancache) != NO_ERROR)
	{
	  return S_ERROR;
	}
      scan_cache = &local_scancache;
      is_local_scancache_started = true;
    }

  assert (scan_cache != NULL);

  /* Lock depends on type of operation. */
  if (op_type == S_SELECT)
    {
      /* Regular read. MVCC doesn't require locks and isolation is guaranteed just using snapshot. */
      lock = NULL_LOCK;
    }
  else if (op_type == S_SELECT_WITH_LOCK)
    {
      /* We need to read and lock last object version found with satisfy dirty rules. This object must be also locked. */
      /* TODO: Is lock really needed? One case this can be used is for triggers but they can also be protected by class 
       * locks.  What about users/authorizations? One option is to split into two operations: one to select dirty with
       * no locks and one to also lock.  Without locking we cannot wait to see what happens with objects being deleted
       * but not committed/aborted yet. */
      lock = S_LOCK;
    }
  else if (op_type == S_DELETE || op_type == S_UPDATE)
    {
      /* Exclusive locks. */
      lock = X_LOCK;
    }
  else
    {
      /* Unexpected/invalid case. */
      assert_release (false);
      return S_ERROR;
    }

  /* Initialize data that may be required for looping through object versions. */
  /* Start with current object. */
  COPY_OID (&current_oid, oid);
  /* Initialize MVCC delete info. */
  MVCC_SET_DELETE_INFO (&mvcc_delete_info, MVCCID_NULL, &oid_Null_oid, DELETE_RECORD_CAN_DELETE);
  /* Initialize scan_reev_data. */
  if (mvcc_reev_data != NULL && mvcc_reev_data->type == REEV_DATA_SCAN)
    {
      scan_reev_data = mvcc_reev_data->select_reev_data;
    }

  if (scan_cache != NULL && scan_cache->cache_last_fix_page && scan_cache->page_watcher.pgptr != NULL)
    {
      /* switch to local page watcher */
      pgbuf_replace_watcher (thread_p, &scan_cache->page_watcher, &home_page_watcher);
    }

  /* Always get class_oid to check MVCC is enabled for class. */
  if (class_oid == NULL)
    {
      /* Locks are required and class_oid must be obtained. It will be obtained after home_page is fixed. */
      class_oid = &class_oid_local;
      OID_SET_NULL (class_oid);
    }

  COPY_OID (&current_class_oid, class_oid);

  /* Advance to update chain until the purpose of the function is reached. */
start_current_version:

  if (fwd_page_watcher.pgptr != NULL)
    {
      /* This belong to a previous version. No longer required. */
      pgbuf_ordered_unfix (thread_p, &fwd_page_watcher);
    }

  OID_SET_NULL (&partition_class_oid);

  /* For each object version repeat: 1. Prepare for obtaining the record: get required pages, class_oid and record
   * type. 2. Get record header. 3. Depending on purpose of the call: 3.0. MVCC disabled class does not have versions
   * and are not locked in this function. Go directly to step 4.  3.1. S_SELECT - obtain visible version.  - if version 
   * not visible and has next version, go to next version and repeat steps from 1.  - if version not visible and no
   * next version => S_DOESNT_EXIST.  - if version is visible go to step 4.  3.2. S_SELECT_WITH_LOCK?, S_DELETE,
   * S_UPDATE. Last version must be obtained, which is not necessarily the visible object.  If current version <
   * visible version, get next version and repeat steps from one.  If current version is visible: - if not
   * deleted/updated and evaluation is passed, lock and go to step 4.  - if deleted/updated and has next version, go to 
   * next version and repeat steps from 1.  - if deleted/updated and no next version, return S_DOESNT_EXIST.  For
   * versions > visible versions, same steps are applied. Some checks are different though.  One example is that
   * REPEATABLE READ and SERIALIZABLE isolations cannot go further than the visible version. 4. Get record data: If
   * object version is the same and CHN is up-to-date, skip getting data and return S_SUCCESS_CHN_UPTODATE.  Otherwise, 
   * get record data. */

  /* Prepare to get record. It will obtain class_oid, record type, required pages, and forward_oid. */
  scan_code =
    heap_prepare_get_record (thread_p, &current_oid, &current_class_oid, &forward_oid, &partition_class_oid,
			     &home_page_watcher, &fwd_page_watcher, &type, PGBUF_LATCH_READ, false,
			     non_ex_handling_type);
  if (scan_code != S_SUCCESS)
    {
      /* Stop here. */
      goto end;
    }
  /* We are here only if a valid record was found: REC_HOME, REC_BIGONE, REC_RELOCATION or REC_MVCC_NEXT_VERSION. Any
   * other case should be either S_DOESNT_EXIST or S_ERROR. */
  assert (type == REC_HOME || type == REC_RELOCATION || type == REC_BIGONE || type == REC_MVCC_NEXT_VERSION);
  /* We should have required pages fixed. */
  assert (HEAP_IS_PAGE_OF_OID (home_page_watcher.pgptr, &current_oid));
  /* Check forward_page and forward_oid are set when type is REC_RELOCATION or REC_BIGONE. */
  assert (type != REC_BIGONE || type != REC_RELOCATION
	  || (!OID_ISNULL (&forward_oid) && HEAP_IS_PAGE_OF_OID (fwd_page_watcher.pgptr, &forward_oid)));

  if (heap_is_mvcc_disabled_for_class (&current_class_oid))
    {
      /* MVCC is disabled for this class. There is only one object version. Also locking for non-MVCC classes is not
       * the scope of this function. Go directly to get_record. */
      if (type == REC_MVCC_NEXT_VERSION)
	{
	  /* Unexpected since this class should not be vacuumed. */
	  assert_release (false);
	  goto error;
	}
      /* Get record directly. */
      goto get_record;
    }

  if (type == REC_MVCC_NEXT_VERSION)
    {
      if (found_visible)
	{
	  /* Is it impossible to find a vacuumed version after a visible version. Visible version means that its delete 
	   * MVCCID is active relative to current transaction. Next version insert MVCCID should be the same and again
	   * active. All following versions should actually have insert MVCCID active relative to this transaction.
	   * This means that as long as this transaction is alive, no versions newer than its visible version should be 
	   * vacuumed. This shouldn't happen. */
	  assert_release (false);
	  goto error;
	}

      /* if a partition link is present */
      if (OID_ISNULL (&partition_class_oid) == false)
	{
	  bool unfixed_watchers = false;

	  /* lock scan on new scan cache */
	  if (heap_mvcc_lock_scan_and_set_scancache_node
	      (thread_p, &partition_class_oid, scan_cache, &home_page_watcher, &fwd_page_watcher,
	       &unfixed_watchers) != LK_GRANTED)
	    {
	      assert (er_errid () != NO_ERROR);
	      goto error;
	    }
	  else if (unfixed_watchers)
	    {
	      /* we acquired lock, but we need to refix pages and read partition link again */
	      goto start_current_version;
	    }

	  COPY_OID (&current_class_oid, &partition_class_oid);
	}

      /* Advance to next version. */
      /* heap_prepare_get_record should have output next_version to forward_oid. */
      assert (!OID_ISNULL (&forward_oid));
      COPY_OID (&current_oid, &forward_oid);

      /* Not the first version. */
      is_original_oid = false;
      goto start_current_version;
    }

  /* REC_HOME, REC_RELOCATION or REC_BIGONE. */

  /* Get MVCC header. */
  scan_code =
    heap_get_mvcc_header (thread_p, &current_oid, &forward_oid, home_page_watcher.pgptr, fwd_page_watcher.pgptr, type,
			  &mvcc_header);
  if (scan_code != S_SUCCESS)
    {
      goto end;
    }

  if (!found_visible)
    {
      /* Check snapshot until visible is found. */
      if (mvcc_snapshot == NULL)
	{
	  /* Although this seems unsafe, I've seen a case that sets scan_cache->mvcc_snapshot to NULL before calling
	   * heap_mvcc_get_version_for_delete (a previous version of this function). My guess is that it already
	   * obtained visible version and it is supposed to skip visibility check. 
	   * TODO: Check whether these cases should be handled with snapshots. 
	   */
	  /* Fall through. */
	}
      else
	{
	  if (mvcc_snapshot->snapshot_fnc (thread_p, &mvcc_header, mvcc_snapshot))
	    {
	      /* Passed visibility test. */
	      /* Fall through. */
	    }
	  else
	    {
	      /* Check if there is next version. */
	      if (OID_ISNULL (&MVCC_GET_NEXT_VERSION (&mvcc_header)))
		{
		  bool is_system_class = false;

		  /* No next versions. Stop here. */
		  if (oid_is_system_class (&current_class_oid, &is_system_class) != NO_ERROR)
		    {
		      scan_code = S_DOESNT_EXIST;
		      goto end;
		    }
		  if (non_ex_handling_type == LOG_WARNING_IF_DELETED
		      || (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_DELID)
			  && MVCC_IS_REC_DELETED_BY_ME (thread_p, &mvcc_header) && is_system_class == true))
		    {
		      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, current_oid.volid,
			      current_oid.pageid, current_oid.slotid);
		    }
		  else
		    {
		      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, current_oid.volid,
			      current_oid.pageid, current_oid.slotid);
		    }
		  scan_code = S_DOESNT_EXIST;
		  goto end;
		}
	      else
		{
		  /* if a partition link is present */
		  if (MVCC_IS_HEADER_PARTITION_OID_VALID (&mvcc_header))
		    {
		      OID *partition_oid = NULL;
		      bool unfixed_watchers = false;

		      /* read partition OID */
		      partition_oid = &MVCC_GET_PARTITION_OID (&mvcc_header);

		      /* lock scan on new scan cache */
		      if (heap_mvcc_lock_scan_and_set_scancache_node
			  (thread_p, partition_oid, scan_cache, &home_page_watcher, &fwd_page_watcher,
			   &unfixed_watchers) != LK_GRANTED)
			{
			  assert (er_errid () != NO_ERROR);
			  goto error;
			}
		      else if (unfixed_watchers)
			{
			  /* we acquired lock, but we need to refix pages and read partition link again */
			  goto start_current_version;
			}

		      COPY_OID (&current_class_oid, partition_oid);
		    }

		  /* We must go to next version. */
		  COPY_OID (&current_oid, &MVCC_GET_NEXT_VERSION (&mvcc_header));
		  is_original_oid = false;
		  goto start_current_version;
		}
	    }
	}
      /* Snapshot was satisfied, thus a visible version was found. */
      found_visible = true;
      if (op_type == S_SELECT)
	{
	  /* Select visible version. */
	  /* Stop processing object versions and get record. */
	  goto get_record;
	}
      /* op_type is S_DELETE or S_UPDATE or S_SELECT_WITH_LOCK */
      if (scan_reev_data != NULL && scan_reev_data->data_filter != NULL)
	{
	  /* Evaluate record. */
	  /* At this point we have no choice but to get record in order to evaluate the object. */
	  temp_recdes.data = NULL;
	  scan_code =
	    heap_get_record_data_when_all_ready (thread_p, &current_oid, &forward_oid, home_page_watcher.pgptr,
						 fwd_page_watcher.pgptr, type, &temp_recdes, scan_cache, ispeeking);

	  if (scan_code != S_SUCCESS)
	    {
	      assert (scan_code != S_ERROR || er_errid () != NO_ERROR);
	      goto error;
	    }

	  ev_res = eval_data_filter (thread_p, &current_oid, &temp_recdes, scan_cache, scan_reev_data->data_filter);
	  ev_res =
	    update_logical_result (thread_p, ev_res, (int *) scan_reev_data->qualification, scan_reev_data->key_filter,
				   &temp_recdes, oid);
	  mvcc_reev_data->filter_result = ev_res;
	  switch (ev_res)
	    {
	    case V_ERROR:
	      /* Evaluation error. */
	      assert (er_errid () != NO_ERROR);
	      goto error;
	    case V_FALSE:
	      /* Record failed evaluation test and doesn't have to be update. Still counts as SUCCESS (caller should
	       * check evaluation result). */
	      scan_code = S_SUCCESS;
	      goto end;
	    default:
	      /* Consider evaluation passed. */
	      break;
	    }
	}
      /* No reevaluation filter or the filter was passed. */
      /* Try to lock the object. */
      if (heap_mvcc_check_and_lock_for_delete (thread_p, &current_oid, &current_class_oid, lock, vacuum_allowed,
					       &mvcc_header, &home_page_watcher, &fwd_page_watcher, &forward_oid,
					       &type, &mvcc_delete_info) != NO_ERROR)
	{
	  scan_code = S_ERROR;
	  assert (er_errid () != NO_ERROR);
	  goto error;
	}
      assert (HEAP_IS_PAGE_OF_OID (home_page_watcher.pgptr, &current_oid));
      assert (type == REC_HOME || type == REC_RELOCATION || type == REC_BIGONE
	      || (type == REC_MVCC_NEXT_VERSION && vacuum_allowed));
      assert (type == REC_HOME || type == REC_MVCC_NEXT_VERSION
	      || (!OID_ISNULL (&forward_oid) && HEAP_IS_PAGE_OF_OID (fwd_page_watcher.pgptr, &forward_oid)));

      switch (mvcc_delete_info.satisfies_delete_result)
	{
	case DELETE_RECORD_CAN_DELETE:
	  /* Row was successfully locked and prepared to delete/update. */
#if !defined (NDEBUG)
	  /* Debug checks that everything is as expected! */
	  assert (mvcc_reev_data == NULL || mvcc_reev_data->filter_result == V_TRUE);
	  assert (!MVCC_IS_HEADER_DELID_VALID (&mvcc_header));
	  assert (!MVCC_IS_HEADER_NEXT_VERSION_VALID (&mvcc_header));
	  /* Make sure the object is protected. */
	  assert (MVCC_IS_REC_INSERTED_BY_ME (thread_p, &mvcc_header)
		  || lock_get_object_lock (&current_oid, &current_class_oid,
					   LOG_FIND_THREAD_TRAN_INDEX (thread_p)) >= lock);
#endif
	  /* Get record */
	  is_version_locked = true;
	  goto get_record;

	case DELETE_RECORD_SELF_DELETED:
	  if (mvcc_snapshot != NULL)
	    {
	      /* A version deleted by me cannot be visible. */
	      /* TODO: This is one of the anomaly that can happen when NULL snapshot is provided. A version deleted by
	       * me should be considered not visible, and should try to get next version (if any). */
	      assert_release (false);
	      goto error;
	    }
	  if (!MVCC_IS_HEADER_NEXT_VERSION_VALID (&mvcc_header))
	    {
	      /* No next versions. Record is deleted and can no longer be deleted or updated. */
	      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, current_oid.volid,
		      current_oid.pageid, current_oid.slotid);
	      scan_code = S_DOESNT_EXIST;
	      goto end;
	    }
	  else
	    {
	      if (MVCC_IS_HEADER_PARTITION_OID_VALID (&mvcc_header))
		{
		  OID *partition_oid = NULL;
		  bool unfixed_watchers = false;

		  /* read new partition OID */
		  partition_oid = &MVCC_GET_PARTITION_OID (&mvcc_header);

		  /* lock scan on new scan cache */
		  if (heap_mvcc_lock_scan_and_set_scancache_node
		      (thread_p, partition_oid, scan_cache, &home_page_watcher, &fwd_page_watcher,
		       &unfixed_watchers) != LK_GRANTED)
		    {
		      assert (er_errid () != NO_ERROR);
		      goto error;
		    }
		  else if (unfixed_watchers)
		    {
		      /* we acquired lock, but we need to refix pages and read partition link again */
		      found_visible = false;
		      goto start_current_version;
		    }

		  COPY_OID (&current_class_oid, partition_oid);
		}

	      /* Go to next version. */
	      COPY_OID (&current_oid, &MVCC_GET_NEXT_VERSION (&mvcc_header));
	      is_original_oid = false;

	      if (type == REC_MVCC_NEXT_VERSION)
		{
		  /* the record has been cleaned meanwhile by VACUUM so we didn't find visible version yet */
		  found_visible = false;
		}
	      goto start_current_version;
	    }
	  /* Impossible to reach. */
	  assert_release (false);
	  goto error;

	case DELETE_RECORD_DELETED:
	  if (logtb_find_current_isolation (thread_p) > TRAN_READ_COMMITTED)
	    {
	      /* Object was already deleted by someone else. Since only READ COMMITTED isolation is allowed to
	       * "re-evaluate" newer versions than the visible one, stop here and return isolation error. */
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_MVCC_SERIALIZABLE_CONFLICT, 0);
	      goto error;
	    }

	  /* TRAN_READ_COMMITTED */
	  /* Object version was deleted. It is either current transaction the deleter or current transaction obtained
	   * the lock on object. If the object was updated and has other versions, follow the update chain. If it was
	   * just deleted, return S_DOESNT_EXIST. */
	  if (!MVCC_IS_HEADER_NEXT_VERSION_VALID (&mvcc_header))
	    {
	      /* No next versions. Record is deleted and can no longer be deleted or updated. */
	      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, current_oid.volid,
		      current_oid.pageid, current_oid.slotid);
	      scan_code = S_DOESNT_EXIST;
	      goto end;
	    }
	  else
	    {
	      /* if a partition link is present */
	      if (MVCC_IS_HEADER_PARTITION_OID_VALID (&mvcc_header))
		{
		  OID *partition_oid = NULL;
		  bool unfixed_watchers = false;

		  /* read partition OID */
		  partition_oid = &MVCC_GET_PARTITION_OID (&mvcc_header);

		  /* lock scan on new scan cache */
		  if (heap_mvcc_lock_scan_and_set_scancache_node
		      (thread_p, partition_oid, scan_cache, &home_page_watcher, &fwd_page_watcher,
		       &unfixed_watchers) != LK_GRANTED)
		    {
		      assert (er_errid () != NO_ERROR);
		      goto error;
		    }
		  else if (unfixed_watchers)
		    {
		      /* we acquired lock, but we need to refix pages and read partition link again */
		      found_visible = false;
		      goto start_current_version;
		    }

		  COPY_OID (&current_class_oid, partition_oid);
		}
	      /* Go to next version. */
	      COPY_OID (&current_oid, &MVCC_GET_NEXT_VERSION (&mvcc_header));
	      is_original_oid = false;
	      if (type == REC_MVCC_NEXT_VERSION)
		{
		  /* the record has been cleaned meanwhile by VACUUM so we didn't find visible version yet */
		  found_visible = false;
		}
	      goto start_current_version;
	    }
	  /* Impossible to reach. */
	  assert_release (false);
	  goto error;

	case DELETE_RECORD_INVISIBLE:
	  /* A visible version cannot be invisible. */
	  assert_release (false);
	  goto error;
	case DELETE_RECORD_IN_PROGRESS:
	  /* No decision can be made on an object that is in process of being deleted. Should have been blocked until
	   * its fate was decided. */
	  assert_release (false);
	  goto error;
	default:
	  /* Unexpected cases. */
	  assert_release (false);
	  goto error;
	}

      /* Impossible to reach. */
      assert_release (false);
      goto error;
    }
  else
    {
      /* Version after visible */

      /* Safe guard: select should have stopped with the visible version. */
      assert (op_type != S_SELECT);
      /* Safe guard: Isolation levels higher than READ COMMITTED should have stopped with visible version (if have
       * snapshot). */
      assert (logtb_find_current_isolation (thread_p) == TRAN_READ_COMMITTED || mvcc_snapshot == NULL);
      /* is_original_oid should be false. */
      assert (is_original_oid == false);

      assert (OID_EQ (&mvcc_delete_info.next_row_version, &current_oid));

      if (!MVCC_IS_REC_INSERTED_BY (&mvcc_header, mvcc_delete_info.row_delid))
	{
	  /* the slot was reused, since transaction id differ by our expectation - delete id of previous row from chain
	   * FIXME: Is this possible? This slot should not be vacuumed/reused since its inserted is still "active". If
	   * the answer is no, consider to leave just a debug assert. */
	  er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, current_oid.volid, current_oid.pageid,
		  current_oid.slotid);
	  scan_code = S_DOESNT_EXIST;
	  goto end;
	}

      /* Try to lock this version for update. */
      if (heap_mvcc_check_and_lock_for_delete (thread_p, &current_oid, &current_class_oid, lock, vacuum_allowed,
					       &mvcc_header, &home_page_watcher, &fwd_page_watcher, &forward_oid,
					       &type, &mvcc_delete_info) != NO_ERROR)
	{
	  assert (er_errid () != NO_ERROR);
	  goto error;
	}

      /* Check that pages are fixed and updated information is valid. */
      assert (HEAP_IS_PAGE_OF_OID (home_page_watcher.pgptr, &current_oid));
      assert (type == REC_HOME || type == REC_RELOCATION || type == REC_BIGONE
	      || (type == REC_MVCC_NEXT_VERSION && vacuum_allowed));
      assert (type == REC_HOME || type == REC_MVCC_NEXT_VERSION
	      || (!OID_ISNULL (&forward_oid) && HEAP_IS_PAGE_OF_OID (fwd_page_watcher.pgptr, &forward_oid)));

      switch (mvcc_delete_info.satisfies_delete_result)
	{
	case DELETE_RECORD_CAN_DELETE:
	  /* Object is ready to be deleted. */

	  /* Check object is not deleted and has no next version. */
	  assert (!MVCC_IS_HEADER_DELID_VALID (&mvcc_header) && !MVCC_IS_HEADER_NEXT_VERSION_VALID (&mvcc_header));

	  /* Check object is protected. */
	  assert (MVCC_IS_REC_INSERTED_BY_ME (thread_p, &mvcc_header)
		  || lock_get_object_lock (&current_oid, &current_class_oid,
					   LOG_FIND_THREAD_TRAN_INDEX (thread_p)) >= lock);

	  /* Set is_version_locked */
	  is_version_locked = true;

	  /* Check re-evaluation. */
	  if (mvcc_reev_data != NULL)
	    {
	      /* We must read record. */
	      temp_recdes.data = NULL;
	      scan_code =
		heap_get_record_data_when_all_ready (thread_p, &current_oid, &forward_oid, home_page_watcher.pgptr,
						     fwd_page_watcher.pgptr, type, &temp_recdes, scan_cache, ispeeking);
	      if (scan_code != S_SUCCESS)
		{
		  assert (scan_code != S_ERROR || er_errid () != NO_ERROR);
		  goto end;
		}

	      ev_res =
		heap_mvcc_reev_cond_and_assignment (thread_p, scan_cache, mvcc_reev_data, &mvcc_header, &current_oid,
						    &temp_recdes);
	      switch (ev_res)
		{
		case V_TRUE:
		  /* Object was locked and passed re-evaluation. Get record. */
		  goto get_record;
		case V_ERROR:
		  /* Error. */
		  assert (er_errid () != NO_ERROR);
		  goto error;
		case V_FALSE:
		case V_UNKNOWN:
		  /* Record didn't pass re-evaluation. Return S_SUCCESS and let the caller handle the case. */
		  goto end;
		default:
		  /* Unhandled. */
		  assert_release (false);
		  goto error;
		}
	    }
	  else
	    {
	      /* No reevaluation. Just get record. */
	      goto get_record;
	    }

	  /* Impossible to reach. */
	  assert_release (false);
	  goto error;

	case DELETE_RECORD_DELETED:
	case DELETE_RECORD_SELF_DELETED:
	  /* Object version was deleted. */
	  /* If object was updated, follow the update chain. If it was deleted, stop here and return S_DOESNT_EXIST. */
	  if (MVCC_IS_HEADER_NEXT_VERSION_VALID (&mvcc_header))
	    {
	      /* if a partition link is present */
	      if (MVCC_IS_HEADER_PARTITION_OID_VALID (&mvcc_header))
		{
		  OID *partition_oid = NULL;
		  bool unfixed_watchers = false;

		  /* read new partition OID */
		  partition_oid = &MVCC_GET_PARTITION_OID (&mvcc_header);

		  /* lock scan on new scan cache */
		  if (heap_mvcc_lock_scan_and_set_scancache_node
		      (thread_p, partition_oid, scan_cache, &home_page_watcher, &fwd_page_watcher,
		       &unfixed_watchers) != LK_GRANTED)
		    {
		      assert (er_errid () != NO_ERROR);
		      goto error;
		    }
		  else if (unfixed_watchers)
		    {
		      /* we acquired lock, but we need to refix pages and read partition link again */
		      goto start_current_version;
		    }

		  COPY_OID (&current_class_oid, partition_oid);
		}
	      /* advance to next version */
	      COPY_OID (&current_oid, &MVCC_GET_NEXT_VERSION (&mvcc_header));
	      is_original_oid = false;
	      goto start_current_version;
	    }
	  else
	    {
	      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, current_oid.volid,
		      current_oid.pageid, current_oid.slotid);
	      scan_code = S_DOESNT_EXIST;
	      goto end;
	    }
	  /* Impossible to reach. */
	  assert_release (false);
	  goto error;

	case DELETE_RECORD_INVISIBLE:
	  /* Inserter is still active. Shouldn't be here since the deleter of previous version (which should be the
	   * same with inserter of current version) was already verified to be committed. */
	  assert_release (false);
	  goto error;
	case DELETE_RECORD_IN_PROGRESS:
	  /* We can't take a decision if delete is still in progress. Should have waited until the fate of the object
	   * version was decided. */
	  assert_release (false);
	  goto error;
	default:
	  /* Unhandled case. */
	  assert_release (false);
	  goto error;
	}

      /* Impossible to reach. */
      assert_release (false);
      goto error;
    }

get_record:
  /* We are here because we decided which version is needed. */
  if (recdes != NULL)
    {
      /* Get the record data. */
      if (is_original_oid && MVCC_IS_CHN_UPTODATE (thread_p, &mvcc_header, old_chn))
	{
	  /* Object version didn't change and CHN is up-to-date. Don't get record data and return
	   * S_SUCCESS_CHN_UPTODATE instead. */
	  scan_code = S_SUCCESS_CHN_UPTODATE;
	  goto end;
	}
      /* Get record data. */
      /* TODO: Optimize to get data from temp_recdes when possible to avoid double get record data. */
      scan_code =
	heap_get_record_data_when_all_ready (thread_p, &current_oid, &forward_oid, home_page_watcher.pgptr,
					     fwd_page_watcher.pgptr, type, recdes, scan_cache, ispeeking);
      if (scan_code != S_SUCCESS)
	{
	  assert (scan_code != S_ERROR || er_errid () != NO_ERROR);
	  goto end;
	}
    }

  if (updated_oid != NULL && !OID_EQ (&current_oid, oid))
    {
      COPY_OID (updated_oid, &current_oid);
    }

end:
  if (home_page_watcher.pgptr != NULL)
    {
      if (scan_cache != NULL && scan_cache->cache_last_fix_page)
	{
	  /* Save the page to scan_cache. */
	  pgbuf_replace_watcher (thread_p, &home_page_watcher, &scan_cache->page_watcher);
	}
      else
	{
	  /* Unfix the page. */
	  pgbuf_ordered_unfix (thread_p, &home_page_watcher);
	}
    }
  if (fwd_page_watcher.pgptr != NULL)
    {
      /* Unfix forward page. */
      pgbuf_ordered_unfix (thread_p, &fwd_page_watcher);
    }

  /* Check if we need to release lock. If scan is not successful or if the evaluation test was not passed, unlock
   * object. What to do in case of S_DOESNT_FIT? Usually we are expecting the caller to retry getting object with a
   * larger area, in which case holding the lock would make sense. Is there any reason we should release the lock? */
  if (is_version_locked
      && ((scan_code != S_SUCCESS && scan_code != S_SUCCESS_CHN_UPTODATE && scan_code != S_DOESNT_FIT)
	  || (mvcc_reev_data != NULL && mvcc_reev_data->filter_result != V_TRUE)))
    {
      lock_unlock_object_donot_move_to_non2pl (thread_p, &current_oid, &current_class_oid, lock);
    }

  if (is_local_scancache_started == true)
    {
      heap_scancache_end (thread_p, scan_cache);
      scan_cache = NULL;
    }

  /* restore original scancache information */
  if (scan_cache != NULL)
    {
      HEAP_SCANCACHE_SET_NODE (scan_cache, &save_class_oid, &save_hfid);
    }

  COPY_OID (class_oid, &current_class_oid);

  return scan_code;

error:
  scan_code = S_ERROR;
  goto end;
}

/*
 * heap_prepare_get_record () - Prepare for obtaining/processing heap object.
 *				It may get class_oid, record_type, home page
 *				and also forward_oid and forward_page in some
 *				cases.
 *
 * return		 : SCAN_CODE: S_ERROR, S_DOESNT_EXIST and S_SUCCESS.
 * thread_p (in)	 : Thread entry.
 * oid (in)		 : Heap object OID.
 * class_oid (in/out)    : Class OID. If it is not a null pointer and its
 *			   value is NULL, class OID will be read from
 *			   page header record.
 * forward_oid (in/out)  : Forward OID to keep link in case of REC_RELOCATION,
 *			   REC_BIGONE and REC_MVCC_NEXT_VERSION.
 * partition_class_oid (out)   : only for REC_MVCC_NEXT_VERSION, class OID of
 *			   partition.
 * home_page_watcher (in/out): Heap page of given object.
 * fwd_page_watcher (in/out) : Heap page of REC_NEWHOME in case of REC_RELOCATION,
 *			   first overflow page in case of REC_BIGONE.
 * record_type (out)	 : Record type.
 * non_ex_handling_type (in): Handling type for deleted objects
 *			      - LOG_ERROR_IF_DELETED: write the 
 *				ER_HEAP_UNKNOWN_OBJECT error to log
 *                            - LOG_WARNING_IF_DELETED: set only warning
 *
 *  Note : the caller should manage the page unfix of both home and forward
 *	   pages (even in case of error, there may be pages latched).
 *	   The functions uses a multiple page latch; in some extreme cases,
 *	   if the home page was unfixed during fwd page fix, we need to recheck
 *	   the home page OID is still valid and re-PEEK the home record. We
 *	   allow this to repeat once.
 *	   For peformance:
 *	   Make sure page unfix is performed in order fwd page, then home page.
 *	   Normal fix sequence (first attempt) is home page, then fwd page; if
 *	   the fwd page is unfixed before home, another thread will attempt to
 *	   fix fwd page, after having home fix; first try (CONDITIONAL) will
 *	   fail, and wil trigger an ordered fix + UNCONDITIONAL.
 */
SCAN_CODE
heap_prepare_get_record (THREAD_ENTRY * thread_p, const OID * oid, OID * class_oid, OID * forward_oid,
			 OID * partition_class_oid, PGBUF_WATCHER * home_page_watcher, PGBUF_WATCHER * fwd_page_watcher,
			 INT16 * record_type, PGBUF_LATCH_MODE latch_mode, bool is_heap_scan,
			 NON_EXISTENT_HANDLING non_ex_handling_type)
{
  SPAGE_SLOT *slot_p = NULL;
  VPID home_vpid;
  VPID forward_vpid;
  RECDES peek_recdes;
  SCAN_CODE scan = S_SUCCESS;
  int try_count = 0;
  int try_max = 1;
  int ret;
  bool is_system_class = false;

  assert (oid != NULL);
  assert (forward_oid != NULL);
  assert (home_page_watcher != NULL);
  assert (fwd_page_watcher != NULL);

try_again:

  /* First make sure object home_page is fixed. */
  VPID_GET_FROM_OID (&home_vpid, oid);
  if (home_page_watcher->pgptr != NULL && !VPID_EQ (pgbuf_get_vpid_ptr (home_page_watcher->pgptr), &home_vpid))
    {
      pgbuf_ordered_unfix (thread_p, home_page_watcher);
    }
  if (home_page_watcher->pgptr == NULL)
    {
      ret = pgbuf_ordered_fix (thread_p, &home_vpid, OLD_PAGE, latch_mode, home_page_watcher);
      if (ret != NO_ERROR)
	{
	  if (ret == ER_PB_BAD_PAGEID)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid,
		      oid->slotid);
	    }

	  if (ret == ER_LK_PAGE_TIMEOUT && er_errid () == NO_ERROR)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, home_vpid.volid, home_vpid.pageid);
	    }
	  goto error;
	}
    }
  /* Home page was fixed. */

  /* Output class_oid. */
  if (class_oid != NULL && OID_ISNULL (class_oid))
    {
      /* Get class OID from HEAP_CHAIN. */
      scan = spage_get_record (home_page_watcher->pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &peek_recdes, PEEK);
      if (scan != S_SUCCESS)
	{
	  /* Unexpected. */
	  assert_release (false);
	  goto error;
	}
      COPY_OID (class_oid, &(((HEAP_CHAIN *) peek_recdes.data)->class_oid));
      if (OID_ISNULL (class_oid))
	{
	  /* 
	   * kludge, root class is identified with a NULL class OID but we
	   * must substitute the actual OID here - think about this.
	   */
	  COPY_OID (class_oid, oid_Root_class_oid);
	}
    }

  /* Get slot. */
  slot_p = spage_get_slot (home_page_watcher->pgptr, oid->slotid);
  if (slot_p == NULL)
    {
      /* Slot doesn't exist. */
      if (!is_heap_scan)
	{
	  /* Do not set error for heap scan and get record info. */
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid, oid->slotid);
	}
      if (record_type != NULL)
	{
	  /* Output record type as REC_UNKNOWN. */
	  *record_type = REC_UNKNOWN;
	}
      return S_DOESNT_EXIST;
    }

  /* Output record type. */
  if (record_type != NULL)
    {
      *record_type = slot_p->record_type;
    }

  if (fwd_page_watcher->pgptr != NULL && slot_p->record_type != REC_RELOCATION && slot_p->record_type != REC_BIGONE)
    {
      /* Forward page no longer required. */
      pgbuf_ordered_unfix (thread_p, fwd_page_watcher);
    }

  /* Fix required pages. */
  switch (slot_p->record_type)
    {
    case REC_RELOCATION:
      /* Need to get forward_oid and fix forward page */
      scan = spage_get_record (home_page_watcher->pgptr, oid->slotid, &peek_recdes, PEEK);
      if (scan != S_SUCCESS)
	{
	  /* Unexpected. */
	  assert_release (false);
	  goto error;
	}
      /* Output forward_oid. */
      COPY_OID (forward_oid, (OID *) peek_recdes.data);
      /* Get forward_vpid. */
      VPID_GET_FROM_OID (&forward_vpid, forward_oid);
      if (fwd_page_watcher->pgptr != NULL)
	{
	  if (VPID_EQ (pgbuf_get_vpid_ptr (fwd_page_watcher->pgptr), &forward_vpid))
	    {
	      /* Already fixed. */
	      return S_SUCCESS;
	    }
	  else
	    {
	      pgbuf_ordered_unfix (thread_p, fwd_page_watcher);
	    }
	}

      /* Try to latch forward_page. */
      PGBUF_WATCHER_COPY_GROUP (fwd_page_watcher, home_page_watcher);
      ret = pgbuf_ordered_fix (thread_p, &forward_vpid, OLD_PAGE, latch_mode, fwd_page_watcher);

      if (ret == NO_ERROR)
	{
	  /* Pages successfully fixed. */
	  if (home_page_watcher->page_was_unfixed)
	    {
	      /* Home_page/forward_page are both fixed. However, since home page was unfixed, record may have changed
	       * (record type has changed or just the relocation link). Go back and repeat steps (if nothing was
	       * changed, pages are already fixed). */
	      if (try_count++ < try_max)
		{
		  home_page_watcher->page_was_unfixed = false;
		  goto try_again;
		}
	      else
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, forward_vpid.volid,
			  forward_vpid.pageid);
		}

	      goto error;
	    }
	  return S_SUCCESS;
	}

      if (ret == ER_PB_BAD_PAGEID)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, forward_oid->volid, forward_oid->pageid,
		  forward_oid->slotid);
	}
      else if (ret == ER_LK_PAGE_TIMEOUT && er_errid () == NO_ERROR)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, forward_vpid.volid, forward_vpid.pageid);
	}

      goto error;

    case REC_BIGONE:
      /* Need to get forward_oid and forward_page (first overflow page). */
      scan = spage_get_record (home_page_watcher->pgptr, oid->slotid, &peek_recdes, PEEK);
      if (scan != S_SUCCESS)
	{
	  /* Unexpected. */
	  assert_release (false);
	  goto error;
	}
      /* Output forward_oid. */
      COPY_OID (forward_oid, (OID *) peek_recdes.data);
      /* Get forward_vpid. */
      VPID_GET_FROM_OID (&forward_vpid, forward_oid);
      if (fwd_page_watcher->pgptr != NULL)
	{
	  if (VPID_EQ (pgbuf_get_vpid_ptr (fwd_page_watcher->pgptr), &forward_vpid))
	    {
	      /* Already fixed. */
	      return S_SUCCESS;
	    }
	  else
	    {
	      pgbuf_ordered_unfix (thread_p, fwd_page_watcher);
	    }
	}

      /* Fix overflow page. Since overflow pages should be always accessed with their home pages latched, unconditional 
       * latch should work; However, we need to use the same ordered_fix approach. */
      PGBUF_WATCHER_RESET_RANK (fwd_page_watcher, PGBUF_ORDERED_HEAP_OVERFLOW);
      PGBUF_WATCHER_COPY_GROUP (fwd_page_watcher, home_page_watcher);
      ret = pgbuf_ordered_fix (thread_p, &forward_vpid, OLD_PAGE, latch_mode, fwd_page_watcher);
      if (ret == NO_ERROR)
	{
	  /* Pages successfully fixed. */
	  if (home_page_watcher->page_was_unfixed)
	    {
	      /* This is not expected. */
	      assert (false);
	      goto error;
	    }
	  return S_SUCCESS;
	}
      if (ret == ER_PB_BAD_PAGEID)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid, oid->slotid);
	}
      else if (ret == ER_LK_PAGE_TIMEOUT && er_errid () == NO_ERROR)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, forward_vpid.volid, forward_vpid.pageid);
	}

      goto error;

    case REC_ASSIGN_ADDRESS:
      /* Object without content.. only the address has been assigned */
      if (is_heap_scan)
	{
	  /* Just ignore record. */
	  return S_DOESNT_EXIST;
	}
      if (spage_check_slot_owner (thread_p, home_page_watcher->pgptr, oid->slotid))
	{
	  er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HEAP_NODATA_NEWADDRESS, 3, oid->volid, oid->pageid,
		  oid->slotid);
	  scan = S_DOESNT_EXIST;
	}
      else
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid, oid->slotid);
	  goto error;
	}
      return scan;

    case REC_HOME:
      /* Only home page is needed. */
      return S_SUCCESS;

    case REC_MVCC_NEXT_VERSION:
      /* Intermediary object version that was vacuumed. Since it only stores a link to next version, forward_page is
       * not required. */
      /* Output next_version to forward_oid in case caller wants to advance. */
      if (is_heap_scan)
	{
	  /* Just ignore record.. */
	  return S_DOESNT_EXIST;
	}
      scan = spage_get_record (home_page_watcher->pgptr, oid->slotid, &peek_recdes, PEEK);
      if (scan != S_SUCCESS)
	{
	  /* Unexpected. */
	  assert_release (false);
	  goto error;
	}
      COPY_OID (forward_oid, (OID *) peek_recdes.data);

      if (peek_recdes.length > OR_OID_SIZE && partition_class_oid != NULL)
	{
	  assert (peek_recdes.length == 2 * OR_OID_SIZE);
	  COPY_OID (partition_class_oid, (OID *) (peek_recdes.data + OR_OID_SIZE));
	}

      return S_SUCCESS;

    case REC_DELETED_WILL_REUSE:
    case REC_MARKDELETED:
      /* Vacuumed/deleted record. */
      if (is_heap_scan)
	{
	  /* Just ignore record. */
	  return S_DOESNT_EXIST;
	}
#if defined(SA_MODE)
      /* Accessing a REC_MARKDELETED record from a system class can happen in SA mode, when no MVCC operations have
       * been performed on the system class. */
      if (oid_is_system_class (class_oid, &is_system_class) != NO_ERROR)
	{
	  goto error;
	}
      if (is_system_class == true)
	{
	  er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid, oid->slotid);
	  return S_DOESNT_EXIST;
	}
#endif /* SA_MODE */

      if (OID_EQ (class_oid, oid_Root_class_oid) || OID_EQ (class_oid, oid_User_class_oid)
	  || non_ex_handling_type == LOG_WARNING_IF_DELETED)
	{
	  /* A deleted class record, corresponding to a deleted class can be accessed through catalog update operations 
	   * on another class. This is possible if a class has an attribute holding a domain that references the
	   * dropped class. Another situation is the client request for authentication, which fetches the object (an
	   * instance of db_user) using dirty version. If it has been removed, it will be found as a deleted record. */
	  er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid, oid->slotid);
	}
      else
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid, oid->slotid);
	}
      return S_DOESNT_EXIST;

    case REC_NEWHOME:
      if (is_heap_scan)
	{
	  /* Just ignore record. */
	  return S_DOESNT_EXIST;
	}
      /* REC_NEWHOME are only allowed to be accessed through REC_RELOCATION slots. */
      /* Fall through to error. */
    default:
      /* Unexpected case. */
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_BAD_OBJECT_TYPE, 3, oid->volid, oid->pageid, oid->slotid);
      goto error;
    }

  /* Impossible */
  assert_release (false);
error:
  assert (er_errid () != NO_ERROR);

  if (fwd_page_watcher->pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, fwd_page_watcher);
    }
  if (home_page_watcher->pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, home_page_watcher);
    }
  return S_ERROR;
}

/*
 * heap_get_mvcc_header () - Get record MVCC header.
 *
 * return	     : SCAN_CODE: S_SUCCESS, S_ERROR or S_DOESNT_EXIST.
 * thread_p (in)     : Thread entry.
 * oid (in)	     : Heap object OID.
 * forward_oid (in)  : Forward OID (for REC_RELOCATION and REC_BIGONE).
 * home_page (in)    : Home page (where OID is found).
 * forward_page (in) : Forward page (for REC_RELOCATION and REC_BIGONE).
 * record_type (in)  : Record type: REC_HOME, REC_RELOCATION or REC_BIGONE.
 * mvcc_header (out) : Record MVCC header.
 *
 * NOTE: This function gets MVCC header, if it has everything needed already
 *	 obtained: pages latched, forward OID (if the case), record type.
 */
SCAN_CODE
heap_get_mvcc_header (THREAD_ENTRY * thread_p, const OID * oid, const OID * forward_oid, PAGE_PTR home_page,
		      PAGE_PTR forward_page, INT16 record_type, MVCC_REC_HEADER * mvcc_header)
{
  RECDES peek_recdes;
  SCAN_CODE scan_code;

  assert (oid != NULL);
  assert (home_page != NULL);
  assert (pgbuf_get_page_id (home_page) == oid->pageid && pgbuf_get_volume_id (home_page) == oid->volid);
  assert (record_type == REC_HOME || record_type == REC_RELOCATION || record_type == REC_BIGONE);
  assert (record_type == REC_HOME
	  || (forward_oid != NULL && forward_page != NULL && pgbuf_get_page_id (forward_page) == forward_oid->pageid
	      && pgbuf_get_volume_id (forward_page) == forward_oid->volid));
  assert (mvcc_header != NULL);

  /* Get header and verify snapshot. */
  switch (record_type)
    {
    case REC_HOME:
      scan_code = spage_get_record (home_page, oid->slotid, &peek_recdes, PEEK);
      if (scan_code != S_SUCCESS)
	{
	  /* Unexpected. */
	  assert (false);
	  return S_ERROR;
	}
      if (or_mvcc_get_header (&peek_recdes, mvcc_header) != NO_ERROR)
	{
	  /* Unexpected. */
	  assert (false);
	  return S_ERROR;
	}
      return S_SUCCESS;
    case REC_BIGONE:
      assert (forward_page != NULL);
      if (heap_get_mvcc_rec_header_from_overflow (forward_page, mvcc_header, &peek_recdes) != NO_ERROR)
	{
	  /* Unexpected. */
	  assert (false);
	  return S_ERROR;
	}
      return S_SUCCESS;
    case REC_RELOCATION:
      assert (forward_oid != NULL);
      assert (forward_page != NULL);
      scan_code = spage_get_record (forward_page, forward_oid->slotid, &peek_recdes, PEEK);
      if (scan_code != S_SUCCESS)
	{
	  /* Unexpected. */
	  assert (false);
	  return S_ERROR;
	}
      if (or_mvcc_get_header (&peek_recdes, mvcc_header) != NO_ERROR)
	{
	  /* Unexpected. */
	  assert (false);
	  return S_ERROR;
	}
      return S_SUCCESS;
    default:
      /* Unexpected. */
      assert (false);
      return S_ERROR;
    }

  /* Impossible. */
  assert (false);
  return S_ERROR;
}

/*
 * heap_get_record_data_when_all_ready () - Get record data when all required
 *					    information is known. This can
 *					    work only for record types that
 *					    actually have data: REC_HOME,
 *					    REC_RELOCATION and REC_BIGONE.
 *					    Required information: home_page,
 *					    forward_oid and forward page for
 *					    REC_RELOCATION and REC_BIGONE,
 *					    and record type.
 *
 * return	      : SCAN_CODE: S_SUCCESS, S_ERROR, S_DOESNT_FIT.
 * thread_p (in)      : Thread entry.
 * oid (in)	      : OID of record to obtain.
 * forward_oid (in)   : Forward OID for REC_RELOCATION and REC_BIGONE.
 * home_page (in)     : Record home page.
 * forward_page (in)  : Record forward page.
 * record_type (in)   : Record type.
 * recdes (out)	      : Record descriptor to keep obtained data.
 * scan_cache (in)    : Heap scan cache.
 * int ispeeking (in) : PEEK or COPY.
 */
static SCAN_CODE
heap_get_record_data_when_all_ready (THREAD_ENTRY * thread_p, OID * oid, OID * forward_oid, PAGE_PTR home_page,
				     PAGE_PTR forward_page, INT16 record_type, RECDES * recdes,
				     HEAP_SCANCACHE * scan_cache, int ispeeking)
{
  SCAN_CODE scan = S_SUCCESS;

  /* We have everything set up to get record data. */

  /* Assert ispeeking, scan_cache and recdes are compatible. If ispeeking is PEEK, we must be able to keep page
   * latched. This means scan_cache must not be NULL and cache_last_fix_page must be true. If ispeeking is COPY, we
   * must have a preallocated area to copy to. This means either scan_cache is not NULL (and scan_cache->area can be
   * used) or recdes->data is not NULL (and recdes->area_size defines how much can be copied). */
  assert ((ispeeking == PEEK && scan_cache != NULL && scan_cache->cache_last_fix_page)
	  || (ispeeking == COPY && (scan_cache != NULL || recdes->data != NULL)));

  switch (record_type)
    {
    case REC_RELOCATION:
      /* Don't peek REC_RELOCATION. */
      if (scan_cache != NULL && (ispeeking == PEEK || recdes->data == NULL))
	{
	  if (scan_cache->area == NULL)
	    {
	      /* Allocate an area to hold the object. Assume that the object will fit in two pages for not better
	       * estimates. */
	      scan_cache->area_size = DB_PAGESIZE * 2;
	      scan_cache->area = (char *) db_private_alloc (thread_p, scan_cache->area_size);
	      if (scan_cache->area == NULL)
		{
		  scan_cache->area_size = -1;
		  return S_ERROR;
		}
	    }
	  recdes->data = scan_cache->area;
	  recdes->area_size = scan_cache->area_size;
	}
      return spage_get_record (forward_page, forward_oid->slotid, recdes, COPY);
    case REC_BIGONE:
      return heap_get_bigone_content (thread_p, scan_cache, ispeeking, forward_oid, recdes);
    case REC_HOME:
      if (scan_cache != NULL && ispeeking == COPY && recdes->data == NULL)
	{
	  /* It is guaranteed that scan_cache is not NULL. */
	  if (scan_cache->area == NULL)
	    {
	      /* Allocate an area to hold the object. Assume that the object will fit in two pages for not better
	       * estimates. */
	      scan_cache->area_size = DB_PAGESIZE * 2;
	      scan_cache->area = (char *) db_private_alloc (thread_p, scan_cache->area_size);
	      if (scan_cache->area == NULL)
		{
		  scan_cache->area_size = -1;
		  return S_ERROR;
		}
	    }
	  recdes->data = scan_cache->area;
	  recdes->area_size = scan_cache->area_size;
	  /* The allocated space is enough to save the instance. */
	}
      return spage_get_record (home_page, oid->slotid, recdes, ispeeking);
    default:
      break;
    }
  /* Shouldn't be here. */
  return S_ERROR;
}

/*
 * heap_get_with_class_oid () - Retrieve or peek an object and get its class oid
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS, S_DOESNT_FIT, S_DOESNT_EXIST, S_ERROR)
 *   class_oid(out): Class OID for the object
 *   oid(in): Object identifier
 *   recdes(in/out): Record descriptor
 *   scan_cache(in/out): Scan cache or NULL
 *   scan_operation_type(in): scan operation type
 *   ispeeking(in): PEEK when the object is peeked, scan_cache cannot be
 *                  NULL COPY when the object is copied
 *
 * Note: Same as heap_get, except that it will also return the class oid
 * for the object.  (see heap_get) description)
 */
SCAN_CODE
heap_get_with_class_oid (THREAD_ENTRY * thread_p, OID * class_oid, const OID * oid, RECDES * recdes,
			 HEAP_SCANCACHE * scan_cache, SCAN_OPERATION_TYPE scan_operation_type, int ispeeking,
			 OID * updated_oid, NON_EXISTENT_HANDLING non_ex_handling_type)
{
  SCAN_CODE scan;

  if (class_oid == NULL)
    {
      assert (false);
      return S_ERROR;
    }

  OID_SET_NULL (class_oid);
  scan =
    heap_mvcc_lock_and_get_object_version (thread_p, oid, class_oid, recdes, scan_cache, scan_operation_type, ispeeking,
					   NULL_CHN, NULL, updated_oid, non_ex_handling_type);
  if (scan != S_SUCCESS)
    {
      OID_SET_NULL (class_oid);
    }

  return scan;
}

/*
 * heap_next_internal () - Retrieve of peek next object.
 *
 * return		     : SCAN_CODE (Either of S_SUCCESS, S_DOESNT_FIT,
 *			       S_END, S_ERROR).
 * thread_p (in)	     : Thread entry.
 * hfid (in)		     : Heap file identifier.
 * class_oid (in)	     : Class object identifier.
 * next_oid (in/out)	     : Object identifier of current record. Will be
 *			       set to next available record or NULL_OID
 *			       when there is not one.
 * recdes (in)		     : Pointer to a record descriptor. Will be
 *			       modified to describe the new record.
 * scan_cache (in)	     : Scan cache or NULL
 * ispeeking (in)	     : PEEK when the object is peeked scan_cache can't
 *			       be NULL COPY when the object is copied.
 * cache_recordinfo (in/out) : DB_VALUE pointer array that caches record
 *			       information values.
 */
static SCAN_CODE
heap_next_internal (THREAD_ENTRY * thread_p, const HFID * hfid, OID * class_oid, OID * next_oid, RECDES * recdes,
		    HEAP_SCANCACHE * scan_cache, int ispeeking, bool reversed_direction, DB_VALUE ** cache_recordinfo)
{
  VPID vpid;
  VPID *vpidptr_incache;
  INT16 type = REC_UNKNOWN;
  OID oid;
  RECDES forward_recdes;
  SCAN_CODE scan = S_ERROR;
  int get_rec_info = cache_recordinfo != NULL;
  MVCC_SNAPSHOT *mvcc_snapshot = NULL;
  MVCC_REC_HEADER mvcc_header;
  bool check_snapshot = false;
  bool is_null_recdata;
  PGBUF_WATCHER curr_page_watcher;
  PGBUF_WATCHER old_page_watcher;

  if (scan_cache != NULL && scan_cache->mvcc_snapshot != NULL && scan_cache->mvcc_snapshot->snapshot_fnc != NULL)
    {
      mvcc_snapshot = scan_cache->mvcc_snapshot;
    }

#if defined(CUBRID_DEBUG)
  if (scan_cache == NULL && ispeeking == PEEK)
    {
      er_log_debug (ARG_FILE_LINE, "heap_next: Using wrong interface. scan_cache cannot be NULL when peeking.");
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      return S_ERROR;
    }
  if (scan_cache != NULL && scan_cache->debug_initpattern != HEAP_DEBUG_SCANCACHE_INITPATTERN)
    {
      er_log_debug (ARG_FILE_LINE, "heap_next: Your scancache is not initialized");
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      return S_ERROR;
    }
  if (scan_cache != NULL && HFID_IS_NULL (&scan_cache->hfid))
    {
      er_log_debug (ARG_FILE_LINE,
		    "heap_next: scan_cache without heap.. heap file must be given to heap_scancache_start () when"
		    " scan_cache is used with heap_first, heap_next, heap_prev heap_last");
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      return S_ERROR;
    }
#endif /* CUBRID_DEBUG */

  if (scan_cache == NULL)
    {
      /* It is possible only in case of ispeeking == COPY */
      if (recdes->data == NULL)
	{
	  er_log_debug (ARG_FILE_LINE, "heap_next: Using wrong interface. recdes->area_size cannot be -1.");
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  return S_ERROR;
	}
    }

  if (scan_cache != NULL)
    {
      hfid = &scan_cache->node.hfid;
      if (!OID_ISNULL (&scan_cache->node.class_oid))
	{
	  class_oid = &scan_cache->node.class_oid;
	}
    }

  PGBUF_INIT_WATCHER (&curr_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  PGBUF_INIT_WATCHER (&old_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);

  if (OID_ISNULL (next_oid))
    {
      if (reversed_direction)
	{
	  /* Retrieve the last record of the file. */
	  if (file_find_last_page (thread_p, &hfid->vfid, &vpid) != NULL)
	    {
	      oid.volid = vpid.volid;
	      oid.pageid = vpid.pageid;
	    }
	  else
	    {
	      oid.volid = NULL_VOLID;
	      oid.pageid = NULL_PAGEID;
	    }
	  oid.slotid = NULL_SLOTID;
	}
      else
	{
	  /* Retrieve the first object of the heap */
	  oid.volid = hfid->vfid.volid;
	  oid.pageid = hfid->hpgid;
	  oid.slotid = 0;	/* i.e., will get slot 1 */
	}
    }
  else
    {
      oid = *next_oid;
    }


  is_null_recdata = (recdes->data == NULL);

  /* Start looking for next object */
  while (true)
    {
      /* Start looking for next object in current page. If we reach the end of this page without finding a new object,
       * fetch next page and continue looking there. If no objects are found, end scanning */
      while (true)
	{
	  vpid.volid = oid.volid;
	  vpid.pageid = oid.pageid;

	  /* 
	   * Fetch the page where the object of OID is stored. Use previous
	   * scan page whenever possible, otherwise, deallocate the page.
	   */
	  if (scan_cache != NULL)
	    {
	      if (scan_cache->cache_last_fix_page == true && scan_cache->page_watcher.pgptr != NULL)
		{
		  vpidptr_incache = pgbuf_get_vpid_ptr (scan_cache->page_watcher.pgptr);
		  if (VPID_EQ (&vpid, vpidptr_incache))
		    {
		      /* replace with local watcher, scan cache watcher will be changed by called functions */
		      pgbuf_replace_watcher (thread_p, &scan_cache->page_watcher, &curr_page_watcher);
		    }
		  else
		    {
		      /* Keep previous scan page fixed until we fixed the current one */
		      pgbuf_replace_watcher (thread_p, &scan_cache->page_watcher, &old_page_watcher);
		    }
		}
	      if (curr_page_watcher.pgptr == NULL)
		{
		  curr_page_watcher.pgptr =
		    heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE_PREVENT_DEALLOC, S_LOCK, scan_cache,
						 &curr_page_watcher);
		  if (old_page_watcher.pgptr != NULL)
		    {
		      pgbuf_ordered_unfix (thread_p, &old_page_watcher);
		    }
		  if (curr_page_watcher.pgptr == NULL)
		    {
		      if (er_errid () == ER_PB_BAD_PAGEID)
			{
			  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid.volid, oid.pageid,
				  oid.slotid);
			}

		      /* something went wrong, return */
		      assert (scan_cache->page_watcher.pgptr == NULL);
		      return S_ERROR;
		    }
		}
	    }
	  else
	    {
	      curr_page_watcher.pgptr =
		heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE_PREVENT_DEALLOC, S_LOCK, NULL,
					     &curr_page_watcher);

	      if (old_page_watcher.pgptr != NULL)
		{
		  pgbuf_ordered_unfix (thread_p, &old_page_watcher);
		}
	      if (curr_page_watcher.pgptr == NULL)
		{
		  if (er_errid () == ER_PB_BAD_PAGEID)
		    {
		      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid.volid, oid.pageid,
			      oid.slotid);
		    }

		  /* something went wrong, return */
		  return S_ERROR;
		}
	    }

	  if (get_rec_info)
	    {
	      /* Getting record information means that we need to scan all slots even if they store no object. */
	      if (reversed_direction)
		{
		  scan =
		    spage_previous_record_dont_skip_empty (curr_page_watcher.pgptr, &oid.slotid, &forward_recdes, PEEK);
		}
	      else
		{
		  scan =
		    spage_next_record_dont_skip_empty (curr_page_watcher.pgptr, &oid.slotid, &forward_recdes, PEEK);
		}
	      if (oid.slotid == HEAP_HEADER_AND_CHAIN_SLOTID)
		{
		  /* skip the header */
		  scan =
		    spage_next_record_dont_skip_empty (curr_page_watcher.pgptr, &oid.slotid, &forward_recdes, PEEK);
		}
	    }
	  else
	    {
	      /* Find the next object. Skip relocated records (i.e., new_home records). This records must be accessed
	       * through the relocation record (i.e., the object). */

	      while (true)
		{
		  if (reversed_direction)
		    {
		      scan = spage_previous_record (curr_page_watcher.pgptr, &oid.slotid, &forward_recdes, PEEK);
		    }
		  else
		    {
		      scan = spage_next_record (curr_page_watcher.pgptr, &oid.slotid, &forward_recdes, PEEK);
		    }
		  if (scan != S_SUCCESS)
		    {
		      /* stop */
		      break;
		    }
		  if (oid.slotid == HEAP_HEADER_AND_CHAIN_SLOTID)
		    {
		      /* skip the header */
		      continue;
		    }
		  type = spage_get_record_type (curr_page_watcher.pgptr, oid.slotid);
		  if (type == REC_NEWHOME || type == REC_ASSIGN_ADDRESS || type == REC_UNKNOWN
		      || type == REC_MVCC_NEXT_VERSION)
		    {
		      /* skip */
		      continue;
		    }

		  if (mvcc_snapshot == NULL || type != REC_HOME)
		    {
		      /* only home records are checked against snapshot here */
		      break;
		    }

		  or_mvcc_get_header (&forward_recdes, &mvcc_header);
		  if (mvcc_snapshot->snapshot_fnc (thread_p, &mvcc_header, mvcc_snapshot) == true)
		    {
		      break;
		    }
		}
	    }

	  if (scan != S_SUCCESS)
	    {
	      if (scan == S_END)
		{
		  /* Find next page of heap and continue scanning */
		  if (reversed_direction)
		    {
		      (void) heap_vpid_prev (hfid, curr_page_watcher.pgptr, &vpid);
		    }
		  else
		    {
		      (void) heap_vpid_next (hfid, curr_page_watcher.pgptr, &vpid);
		    }
		  pgbuf_replace_watcher (thread_p, &curr_page_watcher, &old_page_watcher);
		  oid.volid = vpid.volid;
		  oid.pageid = vpid.pageid;
		  oid.slotid = -1;
		  if (oid.pageid == NULL_PAGEID)
		    {
		      /* must be last page, end scanning */
		      OID_SET_NULL (next_oid);
		      if (old_page_watcher.pgptr != NULL)
			{
			  pgbuf_ordered_unfix (thread_p, &old_page_watcher);
			}
		      return scan;
		    }
		}
	      else
		{
		  /* Error, stop scanning */
		  if (old_page_watcher.pgptr != NULL)
		    {
		      pgbuf_ordered_unfix (thread_p, &old_page_watcher);
		    }
		  pgbuf_ordered_unfix (thread_p, &curr_page_watcher);
		  return scan;
		}
	    }
	  else
	    {
	      /* found a new object */
	      break;
	    }
	}

      /* A record was found */
      if (get_rec_info)
	{
	  scan =
	    heap_get_record_info (thread_p, oid, recdes, forward_recdes, &curr_page_watcher, scan_cache, ispeeking,
				  cache_recordinfo);
	}
      else
	{
	  if (type != REC_HOME && mvcc_snapshot != NULL)
	    {
	      check_snapshot = true;
	    }
	  else
	    {
	      check_snapshot = false;
	    }

	  scan =
	    heap_scan_get_record (thread_p, oid, class_oid, recdes, forward_recdes, &curr_page_watcher, scan_cache,
				  ispeeking, check_snapshot);
	}

      if (scan == S_SUCCESS)
	{
	  /* 
	   * Make sure that the found object is an instance of the desired
	   * class. If it isn't then continue looking.
	   */
	  if (class_oid == NULL || OID_ISNULL (class_oid) || !OID_IS_ROOTOID (&oid))
	    {
	      /* stop */
	      *next_oid = oid;
	      break;
	    }
	  else
	    {
	      /* continue looking */
	      if (is_null_recdata)
		{
		  /* reset recdes->data before getting next record */
		  recdes->data = NULL;
		}
	      continue;
	    }
	}
      else if (scan == S_SNAPSHOT_NOT_SATISFIED || scan == S_DOESNT_EXIST)
	{
	  /* the record does not satisfies snapshot or was deleted - continue */
	  if (is_null_recdata)
	    {
	      /* reset recdes->data before getting next record */
	      recdes->data = NULL;
	    }
	  continue;
	}

      /* scan was not successful, stop scanning */
      break;
    }

  if (old_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &old_page_watcher);
    }

  if (curr_page_watcher.pgptr != NULL)
    {
      if (scan_cache == NULL || !scan_cache->cache_last_fix_page)
	{
	  pgbuf_ordered_unfix (thread_p, &curr_page_watcher);
	}
      else
	{
	  pgbuf_replace_watcher (thread_p, &curr_page_watcher, &scan_cache->page_watcher);
	}
    }

  return scan;
}

/*
 * heap_first () - Retrieve or peek first object of heap
 *   return: SCAN_CODE (Either of S_SUCCESS, S_DOESNT_FIT, S_END, S_ERROR)
 *   hfid(in):
 *   class_oid(in):
 *   oid(in/out): Object identifier of current record.
 *                Will be set to first available record or NULL_OID when there
 *                is not one.
 *   recdes(in/out): Pointer to a record descriptor. Will be modified to
 *                   describe the new record.
 *   scan_cache(in/out): Scan cache or NULL
 *   ispeeking(in): PEEK when the object is peeked, scan_cache cannot be NULL
 *                  COPY when the object is copied
 *
 */
SCAN_CODE
heap_first (THREAD_ENTRY * thread_p, const HFID * hfid, OID * class_oid, OID * oid, RECDES * recdes,
	    HEAP_SCANCACHE * scan_cache, int ispeeking)
{
  /* Retrieve the first record of the file */
  OID_SET_NULL (oid);
  oid->volid = hfid->vfid.volid;

  return heap_next (thread_p, hfid, class_oid, oid, recdes, scan_cache, ispeeking);
}

/*
 * heap_last () - Retrieve or peek last object of heap
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS, S_DOESNT_FIT, S_END,
 *                      S_ERROR)
 *   hfid(in):
 *   class_oid(in):
 *   oid(in/out): Object identifier of current record.
 *                Will be set to last available record or NULL_OID when there is
 *                not one.
 *   recdes(in/out): Pointer to a record descriptor. Will be modified to
 *                   describe the new record.
 *   scan_cache(in/out): Scan cache or NULL
 *   ispeeking(in): PEEK when the object is peeked, scan_cache cannot be NULL
 *                  COPY when the object is copied
 *
 */
SCAN_CODE
heap_last (THREAD_ENTRY * thread_p, const HFID * hfid, OID * class_oid, OID * oid, RECDES * recdes,
	   HEAP_SCANCACHE * scan_cache, int ispeeking)
{
  /* Retrieve the first record of the file */
  OID_SET_NULL (oid);
  oid->volid = hfid->vfid.volid;

  return heap_prev (thread_p, hfid, class_oid, oid, recdes, scan_cache, ispeeking);
}

/*
 * heap_get_alloc () - get/retrieve an object by allocating and freeing area
 *   return: NO_ERROR
 *   oid(in): Object identifier
 *   recdes(in): Record descriptor
 *
 * Note: The object associated with the given OID is copied into the
 * allocated area pointed to by the record descriptor. If the
 * object does not fit in such an area. The area is freed and a
 * new area is allocated to hold the object.
 * The caller is responsible from deallocating the area.
 *
 * Note: The area in the record descriptor is one dynamically allocated
 * with malloc and free with free_and_init.
 */
int
heap_get_alloc (THREAD_ENTRY * thread_p, const OID * oid, RECDES * recdes)
{
  SCAN_CODE scan;
  char *new_area;
  int ret = NO_ERROR;

  if (recdes->data == NULL)
    {
      recdes->area_size = DB_PAGESIZE;	/* assume that only one page is needed */
      recdes->data = (char *) malloc (recdes->area_size);
      if (recdes->data == NULL)
	{
	  ret = ER_OUT_OF_VIRTUAL_MEMORY;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1, (size_t) recdes->area_size);
	  goto exit_on_error;
	}
    }

  /* Get the object */
  while ((scan = heap_get (thread_p, oid, recdes, NULL, COPY, NULL_CHN)) != S_SUCCESS)
    {
      if (scan == S_DOESNT_FIT)
	{
	  /* Is more space needed ? */
	  new_area = (char *) realloc (recdes->data, -(recdes->length));
	  if (new_area == NULL)
	    {
	      ret = ER_OUT_OF_VIRTUAL_MEMORY;
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1, (size_t) (-(recdes->length)));
	      goto exit_on_error;
	    }
	  recdes->area_size = -recdes->length;
	  recdes->data = new_area;
	}
      else
	{
	  goto exit_on_error;
	}
    }

  return ret;

exit_on_error:

  if (recdes->data != NULL)
    {
      free_and_init (recdes->data);
      recdes->area_size = 0;
    }

  return (ret == NO_ERROR) ? ER_FAILED : ret;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * heap_cmp () - Compare heap object with current content
 *   return: int (> 0 recdes is larger,
 *                     < 0 recdes is smaller, and
 *                     = 0 same)
 *   oid(in): The object to compare
 *   recdes(in): Compare object against this content
 *
 * Note: Compare the heap object against given content in ASCII format.
 */
int
heap_cmp (THREAD_ENTRY * thread_p, const OID * oid, RECDES * recdes)
{
  HEAP_SCANCACHE scan_cache;
  RECDES peek_recdes;
  int compare;

  heap_scancache_quick_start (&scan_cache);
  if (heap_get (thread_p, oid, &peek_recdes, &scan_cache, PEEK, NULL_CHN) != S_SUCCESS)
    {
      compare = 1;
    }
  else if (recdes->length > peek_recdes.length)
    {
      compare = memcmp (recdes->data, peek_recdes.data, peek_recdes.length);
      if (compare == 0)
	{
	  compare = 1;
	}
    }
  else
    {
      compare = memcmp (recdes->data, peek_recdes.data, recdes->length);
      if (compare == 0 && recdes->length != peek_recdes.length)
	{
	  compare = -1;
	}
    }

  heap_scancache_end (thread_p, &scan_cache);

  return compare;
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * heap_scanrange_start () - Initialize a scanrange cursor
 *   return: NO_ERROR
 *   scan_range(in/out): Scan range
 *   hfid(in): Heap file identifier
 *   class_oid(in): Class identifier
 *                  For any class, NULL or NULL_OID can be given
 *
 * Note: A scanrange structure is initialized. The scanrange structure
 * is used to define a scan range (set of objects) and to cache
 * information about the latest fetched page and memory allocated
 * by the scan functions. This information is used in future
 * scans, for example, to avoid hashing for the same page in the
 * page buffer pool or defining another allocation area.
 * The caller is responsible for declaring the end of a scan
 * range so that the fixed pages and allocated memory are freed.
 * Using many scans at the same time should be avoided since page
 * buffers are fixed and locked for future references and there
 * is a limit of buffers in the page buffer pool. This is
 * analogous to fetching many pages at the same time.
 */
int
heap_scanrange_start (THREAD_ENTRY * thread_p, HEAP_SCANRANGE * scan_range, const HFID * hfid, const OID * class_oid,
		      MVCC_SNAPSHOT * mvcc_snapshot)
{
  int ret = NO_ERROR;

  /* Start the scan cache */
  ret = heap_scancache_start (thread_p, &scan_range->scan_cache, hfid, class_oid, true, false, mvcc_snapshot);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  OID_SET_NULL (&scan_range->first_oid);
  scan_range->first_oid.volid = hfid->vfid.volid;
  scan_range->last_oid = scan_range->first_oid;

  return ret;

exit_on_error:

  OID_SET_NULL (&scan_range->first_oid);
  OID_SET_NULL (&scan_range->last_oid);

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_scanrange_end () - End of a scanrange
 *   return:
 *   scan_range(in/out): Scanrange
 *
 * Note: Any fixed heap page on the given scan is freed and any memory
 * allocated by this scan is also freed. The scan_range structure is undefined.
 */
void
heap_scanrange_end (THREAD_ENTRY * thread_p, HEAP_SCANRANGE * scan_range)
{
  /* Finish the scan cache */
  heap_scancache_end (thread_p, &scan_range->scan_cache);
  OID_SET_NULL (&scan_range->first_oid);
  OID_SET_NULL (&scan_range->last_oid);
}

/*
 * heap_scanrange_to_following () - Define the following scanrange
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS, S_END, S_ERROR)
 *   scan_range(in/out): Scanrange
 *   start_oid(in): Desired OID for first element in the scanrange or NULL
 *
 * Note: The range of a scanrange is defined. The scanrange is defined
 * as follows:
 *              a: When start_oid == NULL, the first scanrange object is the
 *                 next object after the last object in the previous scanrange
 *              b: When start_oid is the same as a NULL_OID, the first object
 *                 is the first heap object.
 *              c: The first object in the scanrange is the given object.
 *              The last object in the scanrange is either the first object in
 *              the scanrange or the one after the first object which is not a
 *              relocated or multipage object.
 */
SCAN_CODE
heap_scanrange_to_following (THREAD_ENTRY * thread_p, HEAP_SCANRANGE * scan_range, OID * start_oid)
{
  SCAN_CODE scan;
  RECDES recdes;
  INT16 slotid;
  VPID *vpid;

  if (HEAP_DEBUG_ISVALID_SCANRANGE (scan_range) != DISK_VALID)
    {
      return S_ERROR;
    }

  if (start_oid != NULL)
    {
      if (OID_ISNULL (start_oid))
	{
	  /* Scanrange starts at first heap object */
	  scan =
	    heap_first (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid,
			&scan_range->first_oid, &recdes, &scan_range->scan_cache, PEEK);
	  if (scan != S_SUCCESS)
	    {
	      return scan;
	    }
	}
      else
	{
	  /* Scanrange starts with the given object */
	  scan_range->first_oid = *start_oid;
	  scan = heap_get (thread_p, &scan_range->last_oid, &recdes, &scan_range->scan_cache, PEEK, NULL_CHN);
	  if (scan != S_SUCCESS)
	    {
	      if (scan == S_DOESNT_EXIST || scan == S_SNAPSHOT_NOT_SATISFIED)
		{
		  scan =
		    heap_next (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid,
			       &scan_range->first_oid, &recdes, &scan_range->scan_cache, PEEK);
		  if (scan != S_SUCCESS)
		    {
		      return scan;
		    }
		}
	      else
		{
		  return scan;
		}
	    }
	}
    }
  else
    {
      /* 
       * Scanrange ends with the prior object after the first object in the
       * the previous scanrange
       */
      scan_range->first_oid = scan_range->last_oid;
      scan =
	heap_next (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid,
		   &scan_range->first_oid, &recdes, &scan_range->scan_cache, PEEK);
      if (scan != S_SUCCESS)
	{
	  return scan;
	}
    }


  scan_range->last_oid = scan_range->first_oid;
  if (scan_range->scan_cache.page_watcher.pgptr != NULL
      && (vpid = pgbuf_get_vpid_ptr (scan_range->scan_cache.page_watcher.pgptr)) != NULL
      && (vpid->pageid == scan_range->last_oid.pageid) && (vpid->volid == scan_range->last_oid.volid)
      && spage_get_record_type (scan_range->scan_cache.page_watcher.pgptr, scan_range->last_oid.slotid) == REC_HOME)
    {
      slotid = scan_range->last_oid.slotid;
      while (true)
	{
	  if (spage_next_record (scan_range->scan_cache.page_watcher.pgptr, &slotid, &recdes, PEEK) != S_SUCCESS
	      || spage_get_record_type (scan_range->scan_cache.page_watcher.pgptr, slotid) != REC_HOME)
	    {
	      break;
	    }
	  else
	    {
	      scan_range->last_oid.slotid = slotid;
	    }
	}
    }

  return scan;
}

/*
 * heap_scanrange_to_prior () - Define the prior scanrange
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS, S_END, S_ERROR)
 *   scan_range(in/out): Scanrange
 *   last_oid(in): Desired OID for first element in the scanrange or NULL
 *
 * Note: The range of a scanrange is defined. The scanrange is defined
 * as follows:
 *              a: When last_oid == NULL, the last scanrange object is the
 *                 prior object after the first object in the previous
 *                 scanrange.
 *              b: When last_oid is the same as a NULL_OID, the last object is
 *                 is the last heap object.
 *              c: The last object in the scanrange is the given object.
 *              The first object in the scanrange is either the last object in
 *              the scanrange or the one before the first object which is not
 *              a relocated or multipage object.
 */
SCAN_CODE
heap_scanrange_to_prior (THREAD_ENTRY * thread_p, HEAP_SCANRANGE * scan_range, OID * last_oid)
{
  SCAN_CODE scan;
  RECDES recdes;
  INT16 slotid;

  if (HEAP_DEBUG_ISVALID_SCANRANGE (scan_range) != DISK_VALID)
    {
      return S_ERROR;
    }

  if (last_oid != NULL)
    {
      if (OID_ISNULL (last_oid))
	{
	  /* Scanrange ends at last heap object */
	  scan =
	    heap_last (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid,
		       &scan_range->last_oid, &recdes, &scan_range->scan_cache, PEEK);
	  if (scan != S_SUCCESS)
	    {
	      return scan;
	    }
	}
      else
	{
	  /* Scanrange ends with the given object */
	  scan_range->last_oid = *last_oid;
	  scan = heap_get (thread_p, &scan_range->last_oid, &recdes, &scan_range->scan_cache, PEEK, NULL_CHN);
	  if (scan != S_SUCCESS)
	    {
	      if (scan == S_DOESNT_EXIST || scan == S_SNAPSHOT_NOT_SATISFIED)
		{
		  scan =
		    heap_prev (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid,
			       &scan_range->first_oid, &recdes, &scan_range->scan_cache, PEEK);
		  if (scan != S_SUCCESS)
		    {
		      return scan;
		    }
		}
	    }
	}
    }
  else
    {
      /* 
       * Scanrange ends with the prior object after the first object in the
       * the previous scanrange
       */
      scan_range->last_oid = scan_range->first_oid;
      scan =
	heap_prev (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid,
		   &scan_range->last_oid, &recdes, &scan_range->scan_cache, PEEK);
      if (scan != S_SUCCESS)
	{
	  return scan;
	}
    }

  /* 
   * Now define the first object for the scanrange. A scanrange range starts
   * when a relocated or multipage object is found or when the last object is
   * the page is found.
   */

  scan_range->first_oid = scan_range->last_oid;
  if (scan_range->scan_cache.page_watcher.pgptr != NULL)
    {
      slotid = scan_range->first_oid.slotid;
      while (true)
	{
	  if (spage_previous_record (scan_range->scan_cache.page_watcher.pgptr, &slotid, &recdes, PEEK) != S_SUCCESS
	      || slotid == HEAP_HEADER_AND_CHAIN_SLOTID
	      || spage_get_record_type (scan_range->scan_cache.page_watcher.pgptr, slotid) != REC_HOME)
	    {
	      break;
	    }
	  else
	    {
	      scan_range->first_oid.slotid = slotid;
	    }
	}
    }

  return scan;
}

/*
 * heap_scanrange_next () - Retrieve or peek next object in the scanrange
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS, S_DOESNT_FIT, S_END,
 *                      S_ERROR)
 *   next_oid(in/out): Object identifier of current record.
 *                     Will be set to next available record or NULL_OID when
 *                     there is not one.
 *   recdes(in/out): Pointer to a record descriptor. Will be modified to
 *                   describe the new record.
 *   scan_range(in/out): Scan range ... Cannot be NULL
 *   ispeeking(in): PEEK when the object is peeked,
 *                  COPY when the object is copied
 *
 */
SCAN_CODE
heap_scanrange_next (THREAD_ENTRY * thread_p, OID * next_oid, RECDES * recdes, HEAP_SCANRANGE * scan_range,
		     int ispeeking)
{
  SCAN_CODE scan;

  if (HEAP_DEBUG_ISVALID_SCANRANGE (scan_range) != DISK_VALID)
    {
      return S_ERROR;
    }

  /* 
   * If next_oid is less than the first OID in the scanrange.. get the first
   * object
   */

  if (OID_ISNULL (next_oid) || OID_LT (next_oid, &scan_range->first_oid))
    {
      /* Retrieve the first object in the scanrange */
      *next_oid = scan_range->first_oid;
      scan = heap_get (thread_p, next_oid, recdes, &scan_range->scan_cache, ispeeking, NULL_CHN);
      if (scan == S_DOESNT_EXIST || scan == S_SNAPSHOT_NOT_SATISFIED)
	{
	  scan =
	    heap_next (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid, next_oid,
		       recdes, &scan_range->scan_cache, ispeeking);
	}
      /* Make sure that we did not go overboard */
      if (scan == S_SUCCESS && OID_GT (next_oid, &scan_range->last_oid))
	{
	  OID_SET_NULL (next_oid);
	  scan = S_END;
	}
    }
  else
    {
      /* Make sure that this is not the last OID in the scanrange */
      if (OID_EQ (next_oid, &scan_range->last_oid))
	{
	  OID_SET_NULL (next_oid);
	  scan = S_END;
	}
      else
	{
	  scan =
	    heap_next (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid, next_oid,
		       recdes, &scan_range->scan_cache, ispeeking);
	  /* Make sure that we did not go overboard */
	  if (scan == S_SUCCESS && OID_GT (next_oid, &scan_range->last_oid))
	    {
	      OID_SET_NULL (next_oid);
	      scan = S_END;
	    }
	}
    }

  return scan;
}

/*
 * heap_scanrange_prev () - RETRIEVE OR PEEK NEXT OBJECT IN THE SCANRANGE
 *   return:
 * returns/side-effects: SCAN_CODE
 *              (Either of S_SUCCESS, S_DOESNT_FIT, S_END,
 *                         S_ERROR)
 *   prev_oid(in/out): Object identifier of current record.
 *                     Will be set to previous available record or NULL_OID when
 *                     there is not one.
 *   recdes(in/out): Pointer to a record descriptor. Will be modified to
 *                   describe the new record.
 *   scan_range(in/out): Scan range ... Cannot be NULL
 *   ispeeking(in): PEEK when the object is peeked,
 *                  COPY when the object is copied
 *
 */
SCAN_CODE
heap_scanrange_prev (THREAD_ENTRY * thread_p, OID * prev_oid, RECDES * recdes, HEAP_SCANRANGE * scan_range,
		     int ispeeking)
{
  SCAN_CODE scan;

  if (HEAP_DEBUG_ISVALID_SCANRANGE (scan_range) != DISK_VALID)
    {
      return S_ERROR;
    }

  if (OID_ISNULL (prev_oid) || OID_GT (prev_oid, &scan_range->last_oid))
    {
      /* Retrieve the last object in the scanrange */
      *prev_oid = scan_range->last_oid;
      scan = heap_get (thread_p, prev_oid, recdes, &scan_range->scan_cache, ispeeking, NULL_CHN);
      if (scan == S_DOESNT_EXIST || scan == S_SNAPSHOT_NOT_SATISFIED)
	{
	  scan =
	    heap_prev (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid, prev_oid,
		       recdes, &scan_range->scan_cache, ispeeking);
	}
      /* Make sure that we did not go underboard */
      if (scan == S_SUCCESS && OID_LT (prev_oid, &scan_range->last_oid))
	{
	  OID_SET_NULL (prev_oid);
	  scan = S_END;
	}
    }
  else
    {
      /* Make sure that this is not the first OID in the scanrange */
      if (OID_EQ (prev_oid, &scan_range->first_oid))
	{
	  OID_SET_NULL (prev_oid);
	  scan = S_END;
	}
      else
	{
	  scan =
	    heap_prev (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid, prev_oid,
		       recdes, &scan_range->scan_cache, ispeeking);
	  if (scan == S_SUCCESS && OID_LT (prev_oid, &scan_range->last_oid))
	    {
	      OID_SET_NULL (prev_oid);
	      scan = S_END;
	    }
	}
    }

  return scan;
}

/*
 * heap_scanrange_first () - Retrieve or peek first object in the scanrange
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS, S_DOESNT_FIT, S_END,
 *                      S_ERROR)
 *   first_oid(in/out): Object identifier.
 *                      Set to first available record or NULL_OID when there
 *                      is not one.
 *   recdes(in/out): Pointer to a record descriptor. Will be modified to
 *                   describe the new record.
 *   scan_range(in/out): Scan range ... Cannot be NULL
 *   ispeeking(in): PEEK when the object is peeked,
 *                  COPY when the object is copied
 *
 */
SCAN_CODE
heap_scanrange_first (THREAD_ENTRY * thread_p, OID * first_oid, RECDES * recdes, HEAP_SCANRANGE * scan_range,
		      int ispeeking)
{
  SCAN_CODE scan;

  if (HEAP_DEBUG_ISVALID_SCANRANGE (scan_range) != DISK_VALID)
    {
      return S_ERROR;
    }

  /* Retrieve the first object in the scanrange */
  *first_oid = scan_range->first_oid;
  scan = heap_get (thread_p, first_oid, recdes, &scan_range->scan_cache, ispeeking, NULL_CHN);
  if (scan == S_DOESNT_EXIST || scan == S_SNAPSHOT_NOT_SATISFIED)
    {
      scan =
	heap_next (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid, first_oid,
		   recdes, &scan_range->scan_cache, ispeeking);
    }
  /* Make sure that we did not go overboard */
  if (scan == S_SUCCESS && OID_GT (first_oid, &scan_range->last_oid))
    {
      OID_SET_NULL (first_oid);
      scan = S_END;
    }

  return scan;
}

/*
 * heap_scanrange_last () - Retrieve or peek last object in the scanrange
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS, S_DOESNT_FIT, S_END,
 *                      S_ERROR)
 *   last_oid(in/out): Object identifier.
 *                     Set to last available record or NULL_OID when there is
 *                     not one
 *   recdes(in/out): Pointer to a record descriptor. Will be modified to
 *                   describe the new record.
 *   scan_range(in/out): Scan range ... Cannot be NULL
 *   ispeeking(in): PEEK when the object is peeked,
 *                  COPY when the object is copied
 *
 */
SCAN_CODE
heap_scanrange_last (THREAD_ENTRY * thread_p, OID * last_oid, RECDES * recdes, HEAP_SCANRANGE * scan_range,
		     int ispeeking)
{
  SCAN_CODE scan;

  if (HEAP_DEBUG_ISVALID_SCANRANGE (scan_range) != DISK_VALID)
    {
      return S_ERROR;
    }

  /* Retrieve the last object in the scanrange */
  *last_oid = scan_range->last_oid;
  scan = heap_get (thread_p, last_oid, recdes, &scan_range->scan_cache, ispeeking, NULL_CHN);
  if (scan == S_DOESNT_EXIST || scan == S_SNAPSHOT_NOT_SATISFIED)
    {
      scan =
	heap_prev (thread_p, &scan_range->scan_cache.node.hfid, &scan_range->scan_cache.node.class_oid, last_oid,
		   recdes, &scan_range->scan_cache, ispeeking);
    }
  /* Make sure that we did not go underboard */
  if (scan == S_SUCCESS && OID_LT (last_oid, &scan_range->last_oid))
    {
      OID_SET_NULL (last_oid);
      scan = S_END;
    }

  return scan;
}

/*
 * heap_does_exist () - Does object exist?
 *   return: true/false
 *   class_oid(in): Class identifier of object or NULL
 *   oid(in): Object identifier
 *
 * Note: Check if the object associated with the given OID exist.
 * If the class of the object does not exist, the object does not
 * exist either. If the class is not given or a NULL_OID is
 * passed, the function finds the class oid.
 */
bool
heap_does_exist (THREAD_ENTRY * thread_p, OID * class_oid, const OID * oid)
{
  VPID vpid;
  OID tmp_oid;
  PGBUF_WATCHER pg_watcher;
  bool doesexist = true;
  INT16 rectype;
  bool old_check_interrupt;
  int old_wait_msec;

  PGBUF_INIT_WATCHER (&pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, PGBUF_ORDERED_NULL_HFID);

  old_check_interrupt = thread_set_check_interrupt (thread_p, false);
  old_wait_msec = xlogtb_reset_wait_msecs (thread_p, LK_INFINITE_WAIT);

  if (HEAP_ISVALID_OID (oid) != DISK_VALID)
    {
      doesexist = false;
      goto exit_on_end;
    }

  /* 
   * If the class is not NULL and it is different from the Rootclass,
   * make sure that it exist. Rootclass always exist.. not need to check
   * for it
   */
  if (class_oid != NULL && !OID_EQ (class_oid, oid_Root_class_oid) && HEAP_ISVALID_OID (class_oid) != DISK_VALID)
    {
      doesexist = false;
      goto exit_on_end;
    }

  while (doesexist)
    {
      if (oid->slotid == HEAP_HEADER_AND_CHAIN_SLOTID || oid->slotid < 0 || oid->pageid < 0 || oid->volid < 0)
	{
	  doesexist = false;
	  goto exit_on_end;
	}

      vpid.volid = oid->volid;
      vpid.pageid = oid->pageid;

      /* Fetch the page where the record is stored */

      pg_watcher.pgptr = heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, S_LOCK, NULL, &pg_watcher);
      if (pg_watcher.pgptr == NULL)
	{
	  if (er_errid () == ER_PB_BAD_PAGEID)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid->volid, oid->pageid,
		      oid->slotid);
	    }

	  /* something went wrong, give up */
	  doesexist = false;
	  goto exit_on_end;
	}

      doesexist = spage_is_slot_exist (pg_watcher.pgptr, oid->slotid);
      rectype = spage_get_record_type (pg_watcher.pgptr, oid->slotid);

      /* 
       * Check the class
       */

      if (doesexist && rectype != REC_ASSIGN_ADDRESS)
	{
	  if (class_oid == NULL)
	    {
	      class_oid = &tmp_oid;
	      OID_SET_NULL (class_oid);
	    }

	  if (OID_ISNULL (class_oid))
	    {
	      /* 
	       * Caller does not know the class of the object. Get the class
	       * identifier from disk
	       */
	      if (heap_get_class_oid_from_page (thread_p, pg_watcher.pgptr, class_oid) != NO_ERROR)
		{
		  assert_release (false);
		  doesexist = false;
		  goto exit_on_end;
		}
	      assert (!OID_ISNULL (class_oid));
	    }

	  pgbuf_ordered_unfix (thread_p, &pg_watcher);

	  /* If doesexist is true, then check its class */
	  if (!OID_IS_ROOTOID (class_oid))
	    {
	      /* 
	       * Make sure that the class exist too. Loop with this
	       */
	      oid = class_oid;
	      class_oid = oid_Root_class_oid;
	    }
	  else
	    {
	      break;
	    }
	}
      else
	{
	  break;
	}
    }

exit_on_end:

  if (pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &pg_watcher);
    }

  (void) thread_set_check_interrupt (thread_p, old_check_interrupt);
  (void) xlogtb_reset_wait_msecs (thread_p, old_wait_msec);

  return doesexist;
}

/*
 * heap_is_object_not_null () - Check if object should be considered not NULL.
 *
 * return	  : True if object is visible or too new, false if it is deleted or if errors occur.
 * thread_p (in)  : Thread entry.
 * class_oid (in) : Class OID.
 * oid (in)	  : Instance OID.
 */
bool
heap_is_object_not_null (THREAD_ENTRY * thread_p, OID * class_oid, const OID * oid)
{
  bool old_check_interrupt = thread_set_check_interrupt (thread_p, false);
  bool doesexist = false;
  HEAP_SCANCACHE scan_cache;
  SCAN_CODE scan = S_SUCCESS;
  OID local_class_oid = OID_INITIALIZER;
  MVCC_SNAPSHOT *mvcc_snapshot_ptr;
  MVCC_SNAPSHOT copy_mvcc_snapshot;
  bool is_scancache_started = false;

  /* Error will be cleared at the end of this function, so any errors previously set may be lost.
   * This function is not supposed to be called if system has errors.
   */
  assert (er_errid () == NO_ERROR);

  if (HEAP_ISVALID_OID (oid) != DISK_VALID)
    {
      goto exit_on_end;
    }

  /* 
   * If the class is not NULL and it is different from the Root class,
   * make sure that it exist. Root class always exist.. not need to check
   * for it
   */
  if (class_oid != NULL && !OID_EQ (class_oid, oid_Root_class_oid) && HEAP_ISVALID_OID (class_oid) != DISK_VALID)
    {
      goto exit_on_end;
    }
  if (class_oid == NULL)
    {
      class_oid = &local_class_oid;
    }

  if (heap_scancache_quick_start (&scan_cache) != NO_ERROR)
    {
      goto exit_on_end;
    }
  is_scancache_started = true;

  mvcc_snapshot_ptr = logtb_get_mvcc_snapshot (thread_p);
  if (mvcc_snapshot_ptr == NULL)
    {
      assert (false);
      goto exit_on_end;
    }
  /* Make a copy of snapshot. We need all MVCC information, but we also want to change the visibility function. */
  copy_mvcc_snapshot = *mvcc_snapshot_ptr;
  copy_mvcc_snapshot.snapshot_fnc = mvcc_is_not_deleted_for_snapshot;
  scan_cache.mvcc_snapshot = &copy_mvcc_snapshot;

  /* Check if object is visible (chain must be followed if version is old but has next version). If version is too
   * "new", then it means the visible version does exist somewhere in the chain previous to this version.
   * NOTE: We needed this kind of check, because sometimes the given argument could be last version (if it was
   *       locked). If we would check only visibility, this last version could be invisible and the function result
   *       could be inaccurate.
   */
  scan =
    heap_mvcc_lock_and_get_object_version (thread_p, (OID *) oid, class_oid, NULL, &scan_cache, S_SELECT, PEEK,
					   NULL_CHN, NULL, NULL, LOG_WARNING_IF_DELETED);
  if (scan != S_SUCCESS)
    {
      goto exit_on_end;
    }
  assert (!OID_ISNULL (class_oid));

  /* Check class exists. */
  doesexist = heap_does_exist (thread_p, oid_Root_class_oid, class_oid);

exit_on_end:
  (void) thread_set_check_interrupt (thread_p, old_check_interrupt);

  if (is_scancache_started)
    {
      heap_scancache_end (thread_p, &scan_cache);
    }
  /* We don't need to propagate errors from here. */
  if (er_errid () != NO_ERROR)
    {
      er_clear ();
    }

  return doesexist;
}

/*
 * heap_get_num_objects () - Count the number of objects
 *   return: number of records or -1 in case of an error
 *   hfid(in): Object heap file identifier
 *   npages(in):
 *   nobjs(in):
 *   avg_length(in):
 *
 * Note: Count the number of objects stored on the given heap.
 * This function is expensive since all pages of the heap are
 * fetched to find the number of objects.
 */
int
heap_get_num_objects (THREAD_ENTRY * thread_p, const HFID * hfid, int *npages, int *nobjs, int *avg_length)
{
  VPID vpid;			/* Page-volume identifier */
  LOG_DATA_ADDR addr_hdr;	/* Address of logging data */
  RECDES hdr_recdes;		/* Record descriptor to point to space statistics */
  HEAP_HDR_STATS *heap_hdr;	/* Heap header */
  PGBUF_WATCHER hdr_pg_watcher;

  /* 
   * Get the heap header in exclusive mode and call the synchronization to
   * update the statistics of the heap. The number of record/objects is
   * updated.
   */

  PGBUF_INIT_WATCHER (&hdr_pg_watcher, PGBUF_ORDERED_HEAP_HDR, hfid);

  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;

  addr_hdr.vfid = &hfid->vfid;
  addr_hdr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;

  if (pgbuf_ordered_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_WRITE, &hdr_pg_watcher) != NO_ERROR)
    {
      return ER_FAILED;
    }

  (void) pgbuf_check_page_ptype (thread_p, hdr_pg_watcher.pgptr, PAGE_HEAP);

  if (spage_get_record (hdr_pg_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes, PEEK) != S_SUCCESS)
    {
      pgbuf_ordered_unfix (thread_p, &hdr_pg_watcher);
      return ER_FAILED;
    }

  heap_hdr = (HEAP_HDR_STATS *) hdr_recdes.data;
  if (heap_stats_sync_bestspace (thread_p, hfid, heap_hdr, pgbuf_get_vpid_ptr (hdr_pg_watcher.pgptr), true, true) < 0)
    {
      pgbuf_ordered_unfix (thread_p, &hdr_pg_watcher);
      return ER_FAILED;
    }
  *npages = heap_hdr->estimates.num_pages;
  *nobjs = heap_hdr->estimates.num_recs;
  if (*nobjs > 0)
    {
      *avg_length = (int) ((heap_hdr->estimates.recs_sumlen / (float) *nobjs) + 0.9);
    }
  else
    {
      *avg_length = 0;
    }

  addr_hdr.pgptr = hdr_pg_watcher.pgptr;
  log_skip_logging (thread_p, &addr_hdr);
  pgbuf_ordered_set_dirty_and_free (thread_p, &hdr_pg_watcher);

  return *nobjs;
}

/*
 * heap_estimate () - Estimate the number of pages, objects, average length
 *   return: number of pages estimated or -1 in case of an error
 *   hfid(in): Object heap file identifier
 *   npages(in):
 *   nobjs(in):
 *   avg_length(in):
 *
 * Note: Estimate the number of pages, objects, and average length of objects.
 */
int
heap_estimate (THREAD_ENTRY * thread_p, const HFID * hfid, int *npages, int *nobjs, int *avg_length)
{
  VPID vpid;			/* Page-volume identifier */
  PAGE_PTR hdr_pgptr = NULL;	/* Page pointer */
  RECDES hdr_recdes;		/* Record descriptor to point to space statistics */
  HEAP_HDR_STATS *heap_hdr;	/* Heap header */

  /* 
   * Get the heap header in shared mode since it is an estimation of the
   * number of objects.
   */

  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;

  hdr_pgptr = pgbuf_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
  if (hdr_pgptr == NULL)
    {
      /* something went wrong. Unable to fetch header page */
      return ER_FAILED;
    }

  (void) pgbuf_check_page_ptype (thread_p, hdr_pgptr, PAGE_HEAP);

  if (spage_get_record (hdr_pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes, PEEK) != S_SUCCESS)
    {
      pgbuf_unfix_and_init (thread_p, hdr_pgptr);
      return ER_FAILED;
    }

  heap_hdr = (HEAP_HDR_STATS *) hdr_recdes.data;
  *npages = heap_hdr->estimates.num_pages;
  *nobjs = heap_hdr->estimates.num_recs;
  if (*nobjs > 0)
    {
      *avg_length = (int) ((heap_hdr->estimates.recs_sumlen / (float) *nobjs) + 0.9);
    }
  else
    {
      *avg_length = 0;
    }

  pgbuf_unfix_and_init (thread_p, hdr_pgptr);

  return *npages;
}

/*
 * heap_estimate_num_objects () - Estimate the number of objects
 *   return: number of records estimated or -1 in case of an error
 *   hfid(in): Object heap file identifier
 *
 * Note: Estimate the number of objects stored on the given heap.
 */
int
heap_estimate_num_objects (THREAD_ENTRY * thread_p, const HFID * hfid)
{
  int ignore_npages = -1;
  int ignore_avg_reclen = -1;
  int nobjs = -1;

  if (heap_estimate (thread_p, hfid, &ignore_npages, &nobjs, &ignore_avg_reclen) == -1)
    {
      return ER_FAILED;
    }

  return nobjs;
}

/*
 * heap_estimate_avg_length () - Estimate the average length of records
 *   return: avg length
 *   hfid(in): Object heap file identifier
 *
 * Note: Estimate the avergae length of the objects stored on the heap.
 * This function is mainly used when we are creating the OID of
 * an object of which we do not know its length. Mainly for
 * loaddb during forward references to other objects.
 */
static int
heap_estimate_avg_length (THREAD_ENTRY * thread_p, const HFID * hfid)
{
  int ignore_npages;
  int ignore_nobjs;
  int avg_reclen;

  if (heap_estimate (thread_p, hfid, &ignore_npages, &ignore_nobjs, &avg_reclen) == -1)
    {
      return ER_FAILED;
    }

  return avg_reclen;
}

/*
 * heap_get_capacity () - Find space consumed by heap
 *   return: NO_ERROR
 *   hfid(in): Object heap file identifier
 *   num_recs(in/out): Total Number of objects
 *   num_recs_relocated(in/out):
 *   num_recs_inovf(in/out):
 *   num_pages(in/out): Total number of heap pages
 *   avg_freespace(in/out): Average free space per page
 *   avg_freespace_nolast(in/out): Average free space per page without taking in
 *                                 consideration last page
 *   avg_reclength(in/out): Average object length
 *   avg_overhead(in/out): Average overhead per page
 *
 * Note: Find the current storage facts/capacity for given heap.
 */
static int
heap_get_capacity (THREAD_ENTRY * thread_p, const HFID * hfid, INT64 * num_recs, INT64 * num_recs_relocated,
		   INT64 * num_recs_inovf, INT64 * num_pages, int *avg_freespace, int *avg_freespace_nolast,
		   int *avg_reclength, int *avg_overhead)
{
  VPID vpid;			/* Page-volume identifier */
  RECDES recdes;		/* Header record descriptor */
  INT16 slotid;			/* Slot of one object */
  OID *ovf_oid;
  int last_freespace;
  int ovf_len;
  int ovf_num_pages;
  int ovf_free_space;
  int ovf_overhead;
  int j;
  INT16 type = REC_UNKNOWN;
  int ret = NO_ERROR;
  INT64 sum_freespace = 0;
  INT64 sum_reclength = 0;
  INT64 sum_overhead = 0;
  PGBUF_WATCHER pg_watcher;
  PGBUF_WATCHER old_pg_watcher;

  PGBUF_INIT_WATCHER (&pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  PGBUF_INIT_WATCHER (&old_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);

  *num_recs = 0;
  *num_pages = 0;
  *avg_freespace = 0;
  *avg_reclength = 0;
  *avg_overhead = 0;
  *num_recs_relocated = 0;
  *num_recs_inovf = 0;
  last_freespace = 0;

  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;

  while (!VPID_ISNULL (&vpid))
    {
      pg_watcher.pgptr =
	heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE_PREVENT_DEALLOC, S_LOCK, NULL, &pg_watcher);
      if (old_pg_watcher.pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
	}

      if (pg_watcher.pgptr == NULL)
	{
	  /* something went wrong, return error */
	  goto exit_on_error;
	}

      slotid = -1;
      j = spage_number_of_records (pg_watcher.pgptr);

      last_freespace = spage_get_free_space (thread_p, pg_watcher.pgptr);

      *num_pages += 1;
      sum_freespace += last_freespace;
      sum_overhead += j * spage_slot_size ();

      while ((j--) > 0)
	{
	  if (spage_next_record (pg_watcher.pgptr, &slotid, &recdes, PEEK) == S_SUCCESS)
	    {
	      if (slotid != HEAP_HEADER_AND_CHAIN_SLOTID)
		{
		  type = spage_get_record_type (pg_watcher.pgptr, slotid);
		  switch (type)
		    {
		    case REC_RELOCATION:
		    case REC_MVCC_NEXT_VERSION:
		      *num_recs_relocated += 1;
		      sum_overhead += spage_get_record_length (pg_watcher.pgptr, slotid);
		      break;
		    case REC_ASSIGN_ADDRESS:
		    case REC_HOME:
		    case REC_NEWHOME:
		      /* 
		       * Note: for newhome (relocated), we are including the length
		       *       and number of records. In the relocation record (above)
		       *       we are just adding the overhead and number of
		       *       reclocation records.
		       *       for assign address, we assume the given size.
		       */
		      *num_recs += 1;
		      sum_reclength += spage_get_record_length (pg_watcher.pgptr, slotid);
		      break;
		    case REC_BIGONE:
		      *num_recs += 1;
		      *num_recs_inovf += 1;
		      sum_overhead += spage_get_record_length (pg_watcher.pgptr, slotid);

		      ovf_oid = (OID *) recdes.data;
		      if (heap_ovf_get_capacity (thread_p, ovf_oid, &ovf_len, &ovf_num_pages, &ovf_overhead,
						 &ovf_free_space) == NO_ERROR)
			{
			  sum_reclength += ovf_len;
			  *num_pages += ovf_num_pages;
			  sum_freespace += ovf_free_space;
			  sum_overhead += ovf_overhead;
			}
		      break;
		    case REC_MARKDELETED:
		      /* 
		       * TODO Find out and document here why this is added to
		       * the overhead. The record has been deleted so its
		       * length should no longer have any meaning. Perhaps
		       * the length of the slot should have been added instead?
		       */
		      sum_overhead += spage_get_record_length (pg_watcher.pgptr, slotid);
		      break;
		    case REC_DELETED_WILL_REUSE:
		    default:
		      break;
		    }
		}
	    }
	}
      (void) heap_vpid_next (hfid, pg_watcher.pgptr, &vpid);
      pgbuf_replace_watcher (thread_p, &pg_watcher, &old_pg_watcher);
    }

  if (old_pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
    }

  assert (pg_watcher.pgptr == NULL);

  if (*num_pages > 0)
    {
      /* 
       * Don't take in consideration the last page for free space
       * considerations since the average free space will be contaminated.
       */
      *avg_freespace_nolast = ((*num_pages > 1) ? (int) ((sum_freespace - last_freespace) / (*num_pages - 1)) : 0);
      *avg_freespace = (int) (sum_freespace / *num_pages);
      *avg_overhead = (int) (sum_overhead / *num_pages);
    }

  if (*num_recs != 0)
    {
      *avg_reclength = (int) (sum_reclength / *num_recs);
    }

  return ret;

exit_on_error:

  if (old_pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
    }
  assert (pg_watcher.pgptr == NULL);

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_get_class_oid () - Get class for object. This function doesn't follow
 *			   MVCC versions. Caller must know to use right
 *			   version for this.
 *
 * return	   : Scan code.
 * thread_p (in)   : Thread entry.
 * class_oid (out) : Output class OID.
 * oid (in)	   : Object OID.
 */
SCAN_CODE
heap_get_class_oid (THREAD_ENTRY * thread_p, OID * class_oid, const OID * oid)
{
  return heap_get_with_class_oid (thread_p, class_oid, oid, NULL, NULL, S_SELECT, PEEK, NULL, LOG_ERROR_IF_DELETED);
}

/*
 * heap_get_class_oid_with_lock () - Find class oid of given instance
 *   return: OID *(class_oid on success and NULL on failure)
 *   class_oid(out): The Class oid of the instance
 *   oid(in): The Object identifier of the instance
 *   snapshot_type(in): snapshot type
 *   lock_mode(in): lock mode
 *
 * Note: Find the class identifier of the given instance.
 */
SCAN_CODE
heap_get_class_oid_with_lock (THREAD_ENTRY * thread_p, OID * class_oid, const OID * oid, SNAPSHOT_TYPE snapshot_type,
			      LOCK lock_mode, OID * updated_oid)
{
  HEAP_SCANCACHE scan_cache;
  MVCC_SNAPSHOT mvcc_snapshot_dirty;
  SCAN_OPERATION_TYPE scan_operation_type;
  SCAN_CODE scan_code = S_SUCCESS;

  assert (class_oid != NULL);
  assert (HEAP_ISVALID_OID (oid) != DISK_INVALID);

  OID_SET_NULL (class_oid);
  if (updated_oid != NULL)
    {
      OID_SET_NULL (updated_oid);
    }

  /* Handle locks. */
  if (lock_mode > NULL_LOCK && snapshot_type != SNAPSHOT_TYPE_MVCC)
    {
      if (lock_mode <= S_LOCK)
	{
	  /* S_LOCK */
	  scan_operation_type = S_SELECT_WITH_LOCK;
	}
      else
	{
	  /* X_LOCK */
	  scan_operation_type = S_DELETE;
	}
    }
  else
    {
      scan_operation_type = S_SELECT;
    }

  heap_scancache_quick_start (&scan_cache);
  if (snapshot_type == SNAPSHOT_TYPE_MVCC)
    {
      scan_cache.mvcc_snapshot = logtb_get_mvcc_snapshot (thread_p);
      if (scan_cache.mvcc_snapshot == NULL)
	{
	  ASSERT_ERROR ();
	  return S_ERROR;
	}
    }
  else if (snapshot_type == SNAPSHOT_TYPE_DIRTY)
    {
      /* get dirty version with lock */
      mvcc_snapshot_dirty.snapshot_fnc = mvcc_satisfies_dirty;
      scan_cache.mvcc_snapshot = &mvcc_snapshot_dirty;
    }

  scan_code =
    heap_mvcc_lock_and_get_object_version (thread_p, oid, class_oid, NULL, &scan_cache, scan_operation_type, PEEK,
					   NULL_CHN, NULL, updated_oid, LOG_WARNING_IF_DELETED);
  /* End scan cache before anything else. */
  heap_scancache_end (thread_p, &scan_cache);

  /* Check scan code. */
  if (scan_code != S_SUCCESS)
    {
      assert (scan_code != S_ERROR || er_errid () != NO_ERROR);
      OID_SET_NULL (class_oid);
      return scan_code;
    }
  else
    {
      assert (HEAP_ISVALID_OID (class_oid) != DISK_INVALID);
      if (lock_mode != NULL_LOCK && heap_is_mvcc_disabled_for_class (class_oid))
	{
	  /* If MVCC is disabled for class, object was not locked. Lock it now. */
	  if (lock_object (thread_p, oid, class_oid, lock_mode, LK_UNCOND_LOCK) != LK_GRANTED)
	    {
	      /* Lock failed. */
	      ASSERT_ERROR ();
	      return S_ERROR;
	    }
	}
    }

  /* Class OID obtained successfully. */
  return S_SUCCESS;
}

/*
 * heap_get_class_name () - Find classname when oid is a class
 *   return: Classname or NULL. The classname space must be
 *           released by the caller.
 *   class_oid(in): The Class Object identifier
 *
 * Note: Find the name of the given class identifier. If the passed OID
 * is not a class, it return NULL.
 *
 * Note: Classname pointer must be released by the caller using free_and_init
 */
char *
heap_get_class_name (THREAD_ENTRY * thread_p, const OID * class_oid)
{
  return heap_get_class_name_alloc_if_diff (thread_p, class_oid, NULL);
}

/*
 * heap_get_class_name_alloc_if_diff () - Get the name of given class
 *                               name is malloc when different than given name
 *   return: guess_classname when it is the real name. Don't need to free.
 *           malloc classname when different from guess_classname.
 *           Must be free by caller (free_and_init)
 *           NULL some kind of error
 *   class_oid(in): The Class Object identifier
 *   guess_classname(in): Guess name of class
 *
 * Note: Find the name of the given class identifier. If the name is
 * the same as the guessed name, the guessed name is returned.
 * Otherwise, an allocated area with the name of the class is
 * returned. If an error is found or the passed OID is not a
 * class, NULL is returned.
 */
char *
heap_get_class_name_alloc_if_diff (THREAD_ENTRY * thread_p, const OID * class_oid, char *guess_classname)
{
  char *classname = NULL;
  char *copy_classname = NULL;
  RECDES recdes;
  HEAP_SCANCACHE scan_cache;
  OID root_oid;

  heap_scancache_quick_start_root_hfid (thread_p, &scan_cache);

  if (heap_get_with_class_oid (thread_p, &root_oid, class_oid, &recdes, &scan_cache, S_SELECT, PEEK, NULL,
			       LOG_ERROR_IF_DELETED) == S_SUCCESS)
    {
      /* Make sure that this is a class */
      if (oid_is_root (&root_oid))
	{
	  classname = or_class_name (&recdes);
	  if (guess_classname == NULL || strcmp (guess_classname, classname) != 0)
	    {
	      /* 
	       * The names are different.. return a copy that must be freed.
	       */
	      copy_classname = strdup (classname);
	      if (copy_classname == NULL)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
			  (strlen (classname) + 1) * sizeof (char));
		}
	    }
	  else
	    {
	      /* 
	       * The classnames are identical
	       */
	      copy_classname = guess_classname;
	    }
	}
    }
  else
    {
      if (er_errid () == ER_HEAP_NODATA_NEWADDRESS)
	{
	  er_clear ();		/* clear ER_HEAP_NODATA_NEWADDRESS */
	}
    }

  heap_scancache_end (thread_p, &scan_cache);

  return copy_classname;
}

/*
 * heap_get_class_name_of_instance () - Find classname of given instance
 *   return: Classname or NULL. The classname space must be
 *           released by the caller.
 *   inst_oid(in): The instance object identifier
 *
 * Note: Find the class name of the class of given instance identifier.
 *
 * Note: Classname pointer must be released by the caller using free_and_init
 */
char *
heap_get_class_name_of_instance (THREAD_ENTRY * thread_p, const OID * inst_oid)
{
  char *classname = NULL;
  char *copy_classname = NULL;
  RECDES recdes;
  HEAP_SCANCACHE scan_cache;
  OID class_oid;

  heap_scancache_quick_start (&scan_cache);
  scan_cache.mvcc_snapshot = logtb_get_mvcc_snapshot (thread_p);
  if (scan_cache.mvcc_snapshot == NULL)
    {
      return NULL;
    }

  if (heap_get_with_class_oid (thread_p, &class_oid, inst_oid, NULL, &scan_cache, S_SELECT, PEEK, NULL,
			       LOG_ERROR_IF_DELETED) == S_SUCCESS)
    {
      if (heap_get (thread_p, &class_oid, &recdes, &scan_cache, PEEK, NULL_CHN) == S_SUCCESS)
	{
	  classname = or_class_name (&recdes);
	  copy_classname = (char *) malloc (strlen (classname) + 1);
	  if (copy_classname == NULL)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, (size_t) (strlen (classname) + 1));
	    }
	  else
	    {
	      strcpy (copy_classname, classname);
	    }
	}
    }

  heap_scancache_end (thread_p, &scan_cache);

  return copy_classname;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * heap_get_class_name_with_is_class () - Find if object is a class.
 * if a class, returns its name, otherwise, get the name of its class
 *   return: Classname or NULL. The classname space must be
 *           released by the caller.
 *   oid(in): The Object identifier
 *   isclass(in/out): Set to true is object is a class, otherwise is set to
 *                    false
 *
 * Note: Find if the object associated with given oid is a class.
 * If the object is a class, returns its name, otherwise, returns
 * the name of its class.
 *
 * If the object does not exist or there is another error, NULL
 * is returned as the classname.
 *
 * Note: Classname pointer must be released by the caller using free_and_init
 */
char *
heap_get_class_name_with_is_class (THREAD_ENTRY * thread_p, const OID * oid, int *isclass)
{
  char *classname = NULL;
  char *copy_classname = NULL;
  RECDES recdes;
  HEAP_SCANCACHE scan_cache;
  OID class_oid;

  *isclass = false;

  heap_scancache_quick_start (&scan_cache);
  if (heap_get_with_class_oid (thread_p, &class_oid, oid, &recdes, &scan_cache, S_SELECT, PEEK, NULL,
			       LOG_ERROR_IF_DELETED) == S_SUCCESS)
    {
      /* 
       * If oid is a class, get its name, otherwise, get the name of its class
       */
      *isclass = OID_IS_ROOTOID (&class_oid);
      if (heap_get (thread_p, ((*isclass == true) ? oid : &class_oid), &recdes, &scan_cache, PEEK, NULL_CHN) ==
	  S_SUCCESS)
	{
	  classname = or_class_name (&recdes);
	  copy_classname = (char *) malloc (strlen (classname) + 1);
	  if (copy_classname == NULL)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, (size_t) (strlen (classname) + 1));
	    }
	  else
	    {
	      strcpy (copy_classname, classname);
	    }
	}
    }

  heap_scancache_end (thread_p, &scan_cache);

  return copy_classname;
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * heap_attrinfo_start () - Initialize an attribute information structure
 *   return: NO_ERROR
 *   class_oid(in): The class identifier of the instances where values
 *                  attributes values are going to be read.
 *   requested_num_attrs(in): Number of requested attributes
 *                            If <=0 are given, it means interested on ALL.
 *   attrids(in): Array of requested attributes
 *   attr_info(in/out): The attribute information structure
 *
 * Note: Initialize an attribute information structure, so that values
 * of instances can be retrieved based on the desired attributes.
 * If the requested number of attributes is less than zero,
 * all attributes will be assumed instead. In this case
 * the attrids array should be NULL.
 *
 * The attrinfo structure is an structure where values of
 * instances can be read. For example an object is retrieved,
 * then some of its attributes are convereted to dbvalues and
 * placed in this structure.
 *
 * Note: The caller must call heap_attrinfo_end after he is done with
 * attribute information.
 */
int
heap_attrinfo_start (THREAD_ENTRY * thread_p, const OID * class_oid, int requested_num_attrs, const ATTR_ID * attrids,
		     HEAP_CACHE_ATTRINFO * attr_info)
{
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int getall;			/* Want all attribute values */
  int i;
  int ret = NO_ERROR;

  if (requested_num_attrs == 0)
    {
      /* initialize the attrinfo cache and return, there is nothing else to do */
      (void) memset (attr_info, '\0', sizeof (HEAP_CACHE_ATTRINFO));

      /* now set the num_values to -1 which indicates that this is an empty HEAP_CACHE_ATTRINFO and shouldn't be
       * operated on. */
      attr_info->num_values = -1;
      return NO_ERROR;
    }

  if (requested_num_attrs < 0)
    {
      getall = true;
    }
  else
    {
      getall = false;
    }

  /* 
   * initialize attribute information
   *
   */

  attr_info->class_oid = *class_oid;
  attr_info->last_cacheindex = -1;
  attr_info->read_cacheindex = -1;

  attr_info->last_classrepr = NULL;
  attr_info->read_classrepr = NULL;

  OID_SET_NULL (&attr_info->inst_oid);
  attr_info->inst_chn = NULL_CHN;
  attr_info->values = NULL;
  attr_info->num_values = -1;	/* initialize attr_info */

  /* 
   * Find the most recent representation of the instances of the class, and
   * cache the structure that describe the attributes of this representation.
   * At the same time find the default values of attributes, the shared
   * attribute values and the class attribute values.
   */

  attr_info->last_classrepr =
    heap_classrepr_get (thread_p, &attr_info->class_oid, NULL, NULL_REPRID, &attr_info->last_cacheindex);
  if (attr_info->last_classrepr == NULL)
    {
      goto exit_on_error;
    }

  /* 
   * If the requested attributes is < 0, get all attributes of the last
   * representation.
   */

  if (requested_num_attrs < 0)
    {
      requested_num_attrs = attr_info->last_classrepr->n_attributes;
    }
  else if (requested_num_attrs >
	   (attr_info->last_classrepr->n_attributes + attr_info->last_classrepr->n_shared_attrs +
	    attr_info->last_classrepr->n_class_attrs))
    {
      fprintf (stdout, " XXX There are not that many attributes. Num_attrs = %d, Num_requested_attrs = %d\n",
	       attr_info->last_classrepr->n_attributes, requested_num_attrs);
      requested_num_attrs =
	attr_info->last_classrepr->n_attributes + attr_info->last_classrepr->n_shared_attrs +
	attr_info->last_classrepr->n_class_attrs;
    }

  if (requested_num_attrs > 0)
    {
      attr_info->values =
	(HEAP_ATTRVALUE *) db_private_alloc (thread_p, requested_num_attrs * sizeof (*(attr_info->values)));
      if (attr_info->values == NULL)
	{
	  goto exit_on_error;
	}
    }
  else
    {
      attr_info->values = NULL;
    }

  attr_info->num_values = requested_num_attrs;

  /* 
   * Set the attribute identifier of the desired attributes in the value
   * attribute information, and indicates that the current value is
   * unitialized. That is, it has not been read, set or whatever.
   */

  for (i = 0; i < attr_info->num_values; i++)
    {
      value = &attr_info->values[i];
      if (getall == true)
	{
	  value->attrid = -1;
	}
      else
	{
	  value->attrid = *attrids++;
	}
      value->state = HEAP_UNINIT_ATTRVALUE;
      value->do_increment = 0;
      value->last_attrepr = NULL;
      value->read_attrepr = NULL;
    }

  /* 
   * Make last information to be recached for each individual attribute
   * value. Needed for WRITE and Default values
   */

  if (heap_attrinfo_recache_attrepr (attr_info, true) != NO_ERROR)
    {
      goto exit_on_error;
    }

  return ret;

exit_on_error:

  heap_attrinfo_end (thread_p, attr_info);

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

#if 0				/* TODO: remove unused */
/*
 * heap_moreattr_attrinfo () - Add another attribute to the attribute information
 *                           cache
 *   return: NO_ERROR
 *   attrid(in): The information of the attribute that will be needed
 *   attr_info(in/out): The attribute information structure
 *
 * Note: The given attribute is included as part of the reading or
 * transformation process.
 */
static int
heap_moreattr_attrinfo (int attrid, HEAP_CACHE_ATTRINFO * attr_info)
{
  HEAP_ATTRVALUE *new_values;	/* The new value attribute array */
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int i;
  int ret = NO_ERROR;

  /* 
   * If we get an empty HEAP_CACHE_ATTRINFO, this is an error.  We can
   * not add more attributes to an improperly initialized HEAP_CACHE_ATTRINFO
   * structure.
   */
  if (attr_info->num_values == -1)
    {
      return ER_FAILED;
    }

  /* 
   * Make sure that the attribute is not already included
   */
  for (i = 0; i < attr_info->num_values; i++)
    {
      value = &attr_info->values[i];
      if (value != NULL && value->attrid == attrid)
	{
	  return NO_ERROR;
	}
    }

  /* 
   * Resize the value attribute array and set the attribute identifier as
   * as part of the desired attribute list
   */
  i = (attr_info->num_values + 1) * sizeof (*(attr_info->values));

  new_values = (HEAP_ATTRVALUE *) db_private_realloc (NULL, attr_info->values, i);
  if (new_values == NULL)
    {
      goto exit_on_error;
    }

  attr_info->values = new_values;

  value = &attr_info->values[attr_info->num_values];
  value->attrid = attrid;
  value->state = HEAP_UNINIT_ATTRVALUE;
  value->last_attrepr = NULL;
  value->read_attrepr = NULL;
  attr_info->num_values++;

  /* 
   * Recache attribute representation and get default value specifications
   * for new attribute. The default values are located on the last
   * representation
   */

  if (heap_attrinfo_recache_attrepr (attr_info, true) != NO_ERROR
      || db_value_domain_init (&value->dbvalue, value->read_attrepr->type, value->read_attrepr->domain->precision,
			       value->read_attrepr->domain->scale) != NO_ERROR)
    {
      attr_info->num_values--;
      value->attrid = -1;
      goto exit_on_error;
    }

end:

  return ret;

exit_on_error:

  assert (ret != NO_ERROR);
  if (ret == NO_ERROR)
    {
      assert (er_errid () != NO_ERROR);
      ret = er_errid ();
      if (ret == NO_ERROR)
	{
	  ret = ER_FAILED;
	}
    }
  goto end;
}
#endif

/*
 * heap_attrinfo_recache_attrepr () - Recache attribute information for given attrinfo for
 *                     each attribute value
 *   return: NO_ERROR
 *   attr_info(in/out): The attribute information structure
 *   islast_reset(in): Are we resetting information for last representation.
 *
 * Note: Recache the attribute information for given representation
 * identifier of the class in attr_info for each attribute value.
 * That is, set each attribute information to point to disk
 * related attribute information for given representation
 * identifier.
 * When we are resetting information for last representation,
 * attribute values are also initialized.
 */

static int
heap_attrinfo_recache_attrepr (HEAP_CACHE_ATTRINFO * attr_info, int islast_reset)
{
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int num_found_attrs;		/* Num of found attributes */
  int srch_num_attrs;		/* Num of attributes that can be searched */
  int srch_num_shared;		/* Num of shared attrs that can be searched */
  int srch_num_class;		/* Num of class attrs that can be searched */
  OR_ATTRIBUTE *search_attrepr;	/* Information for disk attribute */
  int i, curr_attr;
  bool isattr_found;
  int ret = NO_ERROR;

  /* 
   * Initialize the value domain for dbvalues of all desired attributes
   */
  if (islast_reset == true)
    {
      srch_num_attrs = attr_info->last_classrepr->n_attributes;
    }
  else
    {
      srch_num_attrs = attr_info->read_classrepr->n_attributes;
    }

  /* shared and class attributes must always use the latest representation */
  srch_num_shared = attr_info->last_classrepr->n_shared_attrs;
  srch_num_class = attr_info->last_classrepr->n_class_attrs;

  for (num_found_attrs = 0, curr_attr = 0; curr_attr < attr_info->num_values; curr_attr++)
    {
      /* 
       * Go over the list of attributes (instance, shared, and class attrs)
       * until the desired attribute is found
       */
      isattr_found = false;
      if (islast_reset == true)
	{
	  search_attrepr = attr_info->last_classrepr->attributes;
	}
      else
	{
	  search_attrepr = attr_info->read_classrepr->attributes;
	}

      value = &attr_info->values[curr_attr];

      if (value->attrid == -1)
	{
	  /* Case that we want all attributes */
	  value->attrid = search_attrepr[curr_attr].id;
	}

      for (i = 0; isattr_found == false && i < srch_num_attrs; i++, search_attrepr++)
	{
	  /* 
	   * Is this a desired instance attribute?
	   */
	  if (value->attrid == search_attrepr->id)
	    {
	      /* 
	       * Found it.
	       * Initialize the attribute value information
	       */
	      isattr_found = true;
	      value->attr_type = HEAP_INSTANCE_ATTR;
	      if (islast_reset == true)
		{
		  value->last_attrepr = search_attrepr;
		  /* 
		   * The server does not work with DB_TYPE_OBJECT but DB_TYPE_OID
		   */
		  if (value->last_attrepr->type == DB_TYPE_OBJECT)
		    {
		      value->last_attrepr->type = DB_TYPE_OID;
		    }

		  if (value->state == HEAP_UNINIT_ATTRVALUE)
		    {
		      db_value_domain_init (&value->dbvalue, value->last_attrepr->type,
					    value->last_attrepr->domain->precision, value->last_attrepr->domain->scale);
		    }
		}
	      else
		{
		  value->read_attrepr = search_attrepr;
		  /* 
		   * The server does not work with DB_TYPE_OBJECT but DB_TYPE_OID
		   */
		  if (value->read_attrepr->type == DB_TYPE_OBJECT)
		    {
		      value->read_attrepr->type = DB_TYPE_OID;
		    }
		}

	      num_found_attrs++;
	    }
	}

      /* 
       * if the desired attribute was not found in the instance attributes,
       * look for it in the shared attributes.  We always use the last_repr
       * for shared attributes.
       */

      for (i = 0, search_attrepr = attr_info->last_classrepr->shared_attrs;
	   isattr_found == false && i < srch_num_shared; i++, search_attrepr++)
	{
	  /* 
	   * Is this a desired shared attribute?
	   */
	  if (value->attrid == search_attrepr->id)
	    {
	      /* 
	       * Found it.
	       * Initialize the attribute value information
	       */
	      isattr_found = true;
	      value->attr_type = HEAP_SHARED_ATTR;
	      value->last_attrepr = search_attrepr;
	      /* 
	       * The server does not work with DB_TYPE_OBJECT but DB_TYPE_OID
	       */
	      if (value->last_attrepr->type == DB_TYPE_OBJECT)
		{
		  value->last_attrepr->type = DB_TYPE_OID;
		}

	      if (value->state == HEAP_UNINIT_ATTRVALUE)
		{
		  db_value_domain_init (&value->dbvalue, value->last_attrepr->type,
					value->last_attrepr->domain->precision, value->last_attrepr->domain->scale);
		}
	      num_found_attrs++;
	    }
	}

      /* 
       * if the desired attribute was not found in the instance/shared atttrs,
       * look for it in the class attributes.  We always use the last_repr
       * for class attributes.
       */

      for (i = 0, search_attrepr = attr_info->last_classrepr->class_attrs; isattr_found == false && i < srch_num_class;
	   i++, search_attrepr++)
	{
	  /* 
	   * Is this a desired class attribute?
	   */

	  if (value->attrid == search_attrepr->id)
	    {
	      /* 
	       * Found it.
	       * Initialize the attribute value information
	       */
	      isattr_found = true;
	      value->attr_type = HEAP_CLASS_ATTR;
	      if (islast_reset == true)
		{
		  value->last_attrepr = search_attrepr;
		}
	      else
		{
		  value->read_attrepr = search_attrepr;
		}
	      /* 
	       * The server does not work with DB_TYPE_OBJECT but DB_TYPE_OID
	       */
	      if (value->last_attrepr->type == DB_TYPE_OBJECT)
		{
		  value->last_attrepr->type = DB_TYPE_OID;
		}

	      if (value->state == HEAP_UNINIT_ATTRVALUE)
		{
		  db_value_domain_init (&value->dbvalue, value->last_attrepr->type,
					value->last_attrepr->domain->precision, value->last_attrepr->domain->scale);
		}
	      num_found_attrs++;
	    }
	}
    }

  if (num_found_attrs != attr_info->num_values && islast_reset == true)
    {
      ret = ER_HEAP_UNKNOWN_ATTRS;
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1, attr_info->num_values - num_found_attrs);
      goto exit_on_error;
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_attrinfo_recache () - Recache attribute information for given attrinfo
 *   return: NO_ERROR
 *   reprid(in): Cache this class representation
 *   attr_info(in/out): The attribute information structure
 *
 * Note: Recache the attribute information for given representation
 * identifier of the class in attr_info. That is, set each
 * attribute information to point to disk related attribute
 * information for given representation identifier.
 */
static int
heap_attrinfo_recache (THREAD_ENTRY * thread_p, REPR_ID reprid, HEAP_CACHE_ATTRINFO * attr_info)
{
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int i;
  int ret = NO_ERROR;

  /* 
   * If we do not need to cache anything (case of only clear values and
   * disk repr structure).. return
   */

  if (attr_info->read_classrepr != NULL)
    {
      if (attr_info->read_classrepr->id == reprid)
	{
	  return NO_ERROR;
	}

      /* 
       * Do we need to free the current cached disk representation ?
       */
      if (attr_info->read_classrepr != attr_info->last_classrepr)
	{
	  ret = heap_classrepr_free (attr_info->read_classrepr, &attr_info->read_cacheindex);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }
	}
      attr_info->read_classrepr = NULL;
    }

  if (reprid == NULL_REPRID)
    {
      return NO_ERROR;
    }

  if (reprid == attr_info->last_classrepr->id)
    {
      /* 
       * Take a short cut
       */
      if (attr_info->values != NULL)
	{
	  for (i = 0; i < attr_info->num_values; i++)
	    {
	      value = &attr_info->values[i];
	      value->read_attrepr = value->last_attrepr;
	    }
	}
      attr_info->read_classrepr = attr_info->last_classrepr;
      attr_info->read_cacheindex = -1;	/* Don't need to free this one */
      return NO_ERROR;
    }

  /* 
   * Cache the desired class representation information
   */
  if (attr_info->values != NULL)
    {
      for (i = 0; i < attr_info->num_values; i++)
	{
	  value = &attr_info->values[i];
	  value->read_attrepr = NULL;
	}
    }
  attr_info->read_classrepr =
    heap_classrepr_get (thread_p, &attr_info->class_oid, NULL, reprid, &attr_info->read_cacheindex);
  if (attr_info->read_classrepr == NULL)
    {
      goto exit_on_error;
    }

  if (heap_attrinfo_recache_attrepr (attr_info, false) != NO_ERROR)
    {
      (void) heap_classrepr_free (attr_info->read_classrepr, &attr_info->read_cacheindex);
      attr_info->read_classrepr = NULL;

      goto exit_on_error;
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_attrinfo_end () - Done with attribute information structure
 *   return: void
 *   attr_info(in/out): The attribute information structure
 *
 * Note: Release any memory allocated for attribute information related
 * reading of instances.
 */
void
heap_attrinfo_end (THREAD_ENTRY * thread_p, HEAP_CACHE_ATTRINFO * attr_info)
{
  int ret = NO_ERROR;

  /* check to make sure the attr_info has been used */
  if (attr_info->num_values == -1)
    {
      return;
    }

  /* 
   * Free any attribute and class representation information
   */
  ret = heap_attrinfo_clear_dbvalues (attr_info);
  ret = heap_attrinfo_recache (thread_p, NULL_REPRID, attr_info);

  if (attr_info->last_classrepr != NULL)
    {
      ret = heap_classrepr_free (attr_info->last_classrepr, &attr_info->last_cacheindex);
      attr_info->last_classrepr = NULL;
    }

  if (attr_info->values)
    {
      db_private_free_and_init (thread_p, attr_info->values);
    }
  OID_SET_NULL (&attr_info->class_oid);

  /* 
   * Bash this so that we ensure that heap_attrinfo_end is idempotent.
   */
  attr_info->num_values = -1;

}

/*
 * heap_attrinfo_clear_dbvalues () - Clear current dbvalues of attribute
 *                                 information
 *   return: NO_ERROR
 *   attr_info(in/out): The attribute information structure
 *
 * Note: Clear any current dbvalues associated with attribute information.
 */
int
heap_attrinfo_clear_dbvalues (HEAP_CACHE_ATTRINFO * attr_info)
{
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  OR_ATTRIBUTE *attrepr;	/* Which one current repr of default one */
  int i;
  int ret = NO_ERROR;

  /* check to make sure the attr_info has been used */
  if (attr_info->num_values == -1)
    {
      return NO_ERROR;
    }

  if (attr_info->values != NULL)
    {
      for (i = 0; i < attr_info->num_values; i++)
	{
	  value = &attr_info->values[i];
	  if (value->state != HEAP_UNINIT_ATTRVALUE)
	    {
	      /* 
	       * Was the value set up from a default value or from a representation
	       * of the object
	       */
	      attrepr = ((value->read_attrepr != NULL) ? value->read_attrepr : value->last_attrepr);
	      if (attrepr != NULL)
		{
		  if (pr_clear_value (&value->dbvalue) != NO_ERROR)
		    {
		      ret = ER_FAILED;
		    }
		  value->state = HEAP_UNINIT_ATTRVALUE;
		}
	    }
	}
    }
  OID_SET_NULL (&attr_info->inst_oid);
  attr_info->inst_chn = NULL_CHN;

  return ret;
}

/*
 * heap_attrvalue_read () - Read attribute information of given attribute cache
 *                        and instance
 *   return: NO_ERROR
 *   recdes(in): Instance record descriptor
 *   value(in): Disk value attribute information
 *   attr_info(in/out): The attribute information structure
 *
 * Note: Read the dbvalue of the given value attribute information.
 */
static int
heap_attrvalue_read (RECDES * recdes, HEAP_ATTRVALUE * value, HEAP_CACHE_ATTRINFO * attr_info)
{
  OR_BUF buf;
  PR_TYPE *pr_type;		/* Primitive type array function structure */
  OR_ATTRIBUTE *volatile attrepr;
  char *disk_data = NULL;
  int disk_bound = false;
  volatile int disk_length = -1;
  int ret = NO_ERROR;

  /* Initialize disk value information */
  disk_data = NULL;
  disk_bound = false;
  disk_length = -1;

  /* 
   * Does attribute exist in this disk representation?
   */

  if (recdes == NULL || recdes->data == NULL || value->read_attrepr == NULL || value->attr_type == HEAP_SHARED_ATTR
      || value->attr_type == HEAP_CLASS_ATTR)
    {
      /* 
       * Either the attribute is a shared or class attr, or the attribute
       * does not exist in this disk representation, or we do not have
       * the disk object (recdes), get default value if any...
       */
      attrepr = value->last_attrepr;
      disk_length = value->last_attrepr->default_value.val_length;
      if (disk_length > 0)
	{
	  disk_data = (char *) value->last_attrepr->default_value.value;
	  disk_bound = true;
	}
    }
  else
    {
      attrepr = value->read_attrepr;
      /* Is it a fixed size attribute ? */
      if (value->read_attrepr->is_fixed != 0)
	{
	  /* 
	   * A fixed attribute.
	   */
	  if (!OR_FIXED_ATT_IS_UNBOUND (recdes->data, attr_info->read_classrepr->n_variable,
					attr_info->read_classrepr->fixed_length, value->read_attrepr->position))
	    {
	      /* 
	       * The fixed attribute is bound. Access its information
	       */
	      disk_data =
		((char *) recdes->data
		 + OR_FIXED_ATTRIBUTES_OFFSET_BY_OBJ (recdes->data,
						      attr_info->read_classrepr->n_variable)
		 + value->read_attrepr->location);
	      disk_length = tp_domain_disk_size (value->read_attrepr->domain);
	      disk_bound = true;
	    }
	}
      else
	{
	  /* 
	   * A variable attribute
	   */
	  if (!OR_VAR_IS_NULL (recdes->data, value->read_attrepr->location))
	    {
	      /* 
	       * The variable attribute is bound.
	       * Find its location through the variable offset attribute table.
	       */
	      disk_data = ((char *) recdes->data + OR_VAR_OFFSET (recdes->data, value->read_attrepr->location));

	      disk_bound = true;
	      switch (TP_DOMAIN_TYPE (attrepr->domain))
		{
		case DB_TYPE_ELO:	/* need real length */
		case DB_TYPE_BLOB:
		case DB_TYPE_CLOB:
		case DB_TYPE_SET:	/* it may be just a little bit fast */
		case DB_TYPE_MULTISET:
		case DB_TYPE_SEQUENCE:
		  OR_VAR_LENGTH (disk_length, recdes->data, value->read_attrepr->location,
				 attr_info->read_classrepr->n_variable);
		  break;
		default:
		  disk_length = -1;	/* remains can read without disk_length */
		}
	    }
	}
    }

  /* 
   * From now on, I should only use attrepr.. it will point to either
   * a current value or a default one
   */

  /* 
   * Clear/decache any old value
   */
  if (value->state != HEAP_UNINIT_ATTRVALUE)
    {
      (void) pr_clear_value (&value->dbvalue);
    }


  /* 
   * Now make the dbvalue according to the disk data value
   */

  if (disk_data == NULL || disk_bound == false)
    {
      /* Unbound attribute, set it to null value */
      ret = db_value_domain_init (&value->dbvalue, attrepr->type, attrepr->domain->precision, attrepr->domain->scale);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
      value->state = HEAP_READ_ATTRVALUE;
    }
  else
    {
      /* 
       * Read the value according to disk information that was found
       */
      OR_BUF_INIT2 (buf, disk_data, disk_length);
      buf.error_abort = 1;

      switch (_setjmp (buf.env))
	{
	case 0:
	  /* Do not copy the string--just use the pointer.  The pr_ routines for strings and sets have different
	   * semantics for length. A negative length value for strings means "don't copy the string, just use the
	   * pointer". For sets, don't translate the set into memory representation at this time.  It will only be
	   * translated when needed. */
	  pr_type = PR_TYPE_FROM_ID (attrepr->type);
	  if (pr_type)
	    {
	      (*(pr_type->data_readval)) (&buf, &value->dbvalue, attrepr->domain, disk_length, false, NULL, 0);
	    }
	  value->state = HEAP_READ_ATTRVALUE;
	  break;
	default:
	  /* 
	   * An error was found during the reading of the attribute value
	   */
	  (void) db_value_domain_init (&value->dbvalue, attrepr->type, attrepr->domain->precision,
				       attrepr->domain->scale);
	  value->state = HEAP_UNINIT_ATTRVALUE;
	  ret = ER_FAILED;
	  break;
	}
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_midxkey_get_value () -
 *   return:
 *   recdes(in):
 *   att(in):
 *   value(out):
 *   attr_info(in):
 */
static int
heap_midxkey_get_value (RECDES * recdes, OR_ATTRIBUTE * att, DB_VALUE * value, HEAP_CACHE_ATTRINFO * attr_info)
{
  char *disk_data = NULL;
  int disk_bound = false;
  bool found = true;		/* Does attribute(att) exist in this disk representation? */
  int i;
  int error = NO_ERROR;

  /* Initialize disk value information */
  disk_data = NULL;
  db_make_null (value);

  if (recdes != NULL && recdes->data != NULL && att != NULL)
    {
      if (or_rep_id (recdes) != attr_info->last_classrepr->id)
	{
	  found = false;
	  for (i = 0; i < attr_info->read_classrepr->n_attributes; i++)
	    {
	      if (attr_info->read_classrepr->attributes[i].id == att->id)
		{
		  att = &attr_info->read_classrepr->attributes[i];
		  found = true;
		  break;
		}
	    }
	}

      if (found == false)
	{
	  /* It means that the representation has an attribute which was created after insertion of the record. In this 
	   * case, return the default value of the attribute if it exists. */
	  if (att->default_value.val_length > 0)
	    {
	      disk_data = att->default_value.value;
	    }
	}
      else
	{
	  /* Is it a fixed size attribute ? */
	  if (att->is_fixed != 0)
	    {			/* A fixed attribute.  */
	      if (!OR_FIXED_ATT_IS_UNBOUND (recdes->data, attr_info->read_classrepr->n_variable,
					    attr_info->read_classrepr->fixed_length, att->position))
		{
		  /* The fixed attribute is bound. Access its information */
		  disk_data =
		    ((char *) recdes->data +
		     OR_FIXED_ATTRIBUTES_OFFSET_BY_OBJ (recdes->data,
							attr_info->read_classrepr->n_variable) + att->location);
		}
	    }
	  else
	    {			/* A variable attribute */
	      if (!OR_VAR_IS_NULL (recdes->data, att->location))
		{
		  /* The variable attribute is bound. Find its location through the variable offset attribute table. */
		  disk_data = ((char *) recdes->data + OR_VAR_OFFSET (recdes->data, att->location));
		}
	    }
	}
    }
  else
    {
      assert (0);
      return ER_FAILED;
    }

  if (disk_data != NULL)
    {
      OR_BUF buf;

      or_init (&buf, disk_data, -1);
      (*(att->domain->type->data_readval)) (&buf, value, att->domain, -1, false, NULL, 0);
    }

  return NO_ERROR;
}

/*
 * heap_attrinfo_read_dbvalues () - Find db_values of desired attributes of given
 *                                instance
 *   return: NO_ERROR
 *   inst_oid(in): The instance oid
 *   recdes(in): The instance Record descriptor
 *   attr_info(in/out): The attribute information structure which describe the
 *                      desired attributes
 *
 * Note: Find DB_VALUES of desired attributes of given instance.
 * The attr_info structure must have already been initialized
 * with the desired attributes.
 *
 * If the inst_oid and the recdes are NULL, then we must be
 * reading only shared and/or class attributes which are found
 * in the last representation.
 */
int
heap_attrinfo_read_dbvalues (THREAD_ENTRY * thread_p, const OID * inst_oid, RECDES * recdes,
			     HEAP_SCANCACHE * scan_cache, HEAP_CACHE_ATTRINFO * attr_info)
{
  int i;
  REPR_ID reprid;		/* The disk representation of the object */
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int ret = NO_ERROR;
  OID mvcc_updated_oid;
  bool need_last_version;

  /* check to make sure the attr_info has been used */
  if (attr_info->num_values == -1)
    {
      return NO_ERROR;
    }

  /* 
   * Make sure that we have the needed cached representation.
   */

  if (inst_oid != NULL && recdes != NULL && recdes->data != NULL)
    {
      reprid = or_rep_id (recdes);

      if (attr_info->read_classrepr == NULL || attr_info->read_classrepr->id != reprid)
	{
	  /* Get the needed representation */
	  ret = heap_attrinfo_recache (thread_p, reprid, attr_info);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }
	}
    }

  /* 
   * Go over each attribute and read it
   */

  need_last_version = false;
  for (i = 0; i < attr_info->num_values; i++)
    {
      value = &attr_info->values[i];
      ret = heap_attrvalue_read (recdes, value, attr_info);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}

      if (need_last_version == false && scan_cache != NULL && DB_VALUE_DOMAIN_TYPE (&(value->dbvalue)) == DB_TYPE_OID
	  && !DB_IS_NULL (&(value->dbvalue)))
	{
	  need_last_version = true;
	}
    }

  /* 
   * Cache the information of the instance
   */
  if (inst_oid != NULL && recdes != NULL && recdes->data != NULL)
    {
      attr_info->inst_chn = or_chn (recdes);
      attr_info->inst_oid = *inst_oid;
    }

  if (need_last_version == true)
    {
      for (i = 0; i < attr_info->num_values; i++)
	{
	  value = &attr_info->values[i];
	  if (DB_VALUE_DOMAIN_TYPE (&(value->dbvalue)) == DB_TYPE_OID && !DB_IS_NULL (&(value->dbvalue)))
	    {
	      HEAP_SCANCACHE local_scancache;

	      if (heap_scancache_quick_start (&local_scancache) != NO_ERROR)
		{
		  goto exit_on_error;
		}

	      if (scan_cache != NULL)
		{
		  local_scancache.mvcc_snapshot = scan_cache->mvcc_snapshot;
		}

	      /* scan_cache is initialized with a class and heap unrelated to referenced OIDs retrieved here; Different 
	       * scan_cache is needed here : it is likely that these OIDs do not belong to the same heap, less to same
	       * page. */
	      if (heap_mvcc_lock_and_get_object_version (thread_p, DB_GET_OID (&(value->dbvalue)), NULL, NULL,
							 &local_scancache, S_SELECT, COPY, NULL_CHN, NULL,
							 &mvcc_updated_oid, LOG_WARNING_IF_DELETED) != S_SUCCESS)
		{
		  if (er_errid () == ER_HEAP_NODATA_NEWADDRESS || er_errid () == ER_HEAP_UNKNOWN_OBJECT)
		    {
		      er_clear ();	/* clear ER_HEAP_NODATA_NEWADDRESS */
		      heap_scancache_end (thread_p, &local_scancache);
		      /* make the deleted OID NULL to avoid future operations */
		      DB_MAKE_NULL (&(value->dbvalue));
		      continue;
		    }
		  heap_scancache_end (thread_p, &local_scancache);
		  goto exit_on_error;
		}

	      if (!OID_ISNULL (&mvcc_updated_oid) && !OID_EQ (&mvcc_updated_oid, DB_GET_OID (&(value->dbvalue))))
		{
		  DB_MAKE_OID (&(value->dbvalue), &mvcc_updated_oid);
		}
	      heap_scancache_end (thread_p, &local_scancache);
	    }
	}
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

int
heap_attrinfo_read_dbvalues_without_oid (THREAD_ENTRY * thread_p, RECDES * recdes, HEAP_CACHE_ATTRINFO * attr_info)
{
  int i;
  REPR_ID reprid;		/* The disk representation of the object */
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int ret = NO_ERROR;

  /* check to make sure the attr_info has been used */
  if (attr_info->num_values == -1)
    {
      return NO_ERROR;
    }

  /* 
   * Make sure that we have the needed cached representation.
   */

  if (recdes != NULL)
    {
      reprid = or_rep_id (recdes);

      if (attr_info->read_classrepr == NULL || attr_info->read_classrepr->id != reprid)
	{
	  /* Get the needed representation */
	  ret = heap_attrinfo_recache (thread_p, reprid, attr_info);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }
	}
    }

  /* 
   * Go over each attribute and read it
   */

  for (i = 0; i < attr_info->num_values; i++)
    {
      value = &attr_info->values[i];
      ret = heap_attrvalue_read (recdes, value, attr_info);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}


/*
 * heap_attrinfo_delete_lob ()
 *   return: NO_ERROR
 *   thread_p(in):
 *   recdes(in): The instance Record descriptor
 *   attr_info(in): The attribute information structure which describe the
 *                  desired attributes
 *
 */
int
heap_attrinfo_delete_lob (THREAD_ENTRY * thread_p, RECDES * recdes, HEAP_CACHE_ATTRINFO * attr_info)
{
  int i;
  HEAP_ATTRVALUE *value;
  int ret = NO_ERROR;

  assert (attr_info != NULL);
  assert (attr_info->num_values > 0);

  /* 
   * Make sure that we have the needed cached representation.
   */

  if (recdes != NULL)
    {
      REPR_ID reprid;
      reprid = or_rep_id (recdes);
      if (attr_info->read_classrepr == NULL || attr_info->read_classrepr->id != reprid)
	{
	  /* Get the needed representation */
	  ret = heap_attrinfo_recache (thread_p, reprid, attr_info);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }
	}
    }

  /* 
   * Go over each attribute and delete the data if it's lob type
   */

  for (i = 0; i < attr_info->num_values; i++)
    {
      value = &attr_info->values[i];
      if (value->last_attrepr->type == DB_TYPE_BLOB || value->last_attrepr->type == DB_TYPE_CLOB)
	{
	  if (value->state == HEAP_UNINIT_ATTRVALUE && recdes != NULL)
	    {
	      ret = heap_attrvalue_read (recdes, value, attr_info);
	      if (ret != NO_ERROR)
		{
		  goto exit_on_error;
		}
	    }
	  if (!db_value_is_null (&value->dbvalue))
	    {
	      DB_ELO *elo;
	      assert (db_value_type (&value->dbvalue) == DB_TYPE_BLOB
		      || db_value_type (&value->dbvalue) == DB_TYPE_CLOB);
	      elo = db_get_elo (&value->dbvalue);
	      if (elo)
		{
		  ret = db_elo_delete (elo);
		}
	      value->state = HEAP_WRITTEN_ATTRVALUE;
	    }
	}
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_attrinfo_dump () - Dump value of attribute information
 *   return:
 *   attr_info(in): The attribute information structure
 *   dump_schema(in):
 *
 * Note: Dump attribute value of given attribute information.
 */
void
heap_attrinfo_dump (THREAD_ENTRY * thread_p, FILE * fp, HEAP_CACHE_ATTRINFO * attr_info, bool dump_schema)
{
  int i;
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int ret = NO_ERROR;

  /* check to make sure the attr_info has been used */
  if (attr_info->num_values == -1)
    {
      fprintf (fp, "  Empty attrinfo\n");
      return;
    }

  /* 
   * Dump attribute schema information
   */

  if (dump_schema == true)
    {
      ret = heap_classrepr_dump (thread_p, fp, &attr_info->class_oid, attr_info->read_classrepr);
    }

  for (i = 0; i < attr_info->num_values; i++)
    {
      value = &attr_info->values[i];
      fprintf (fp, "  Attrid = %d, state = %d, type = %s\n", value->attrid, value->state,
	       pr_type_name (value->read_attrepr->type));
      /* 
       * Dump the value in memory format
       */

      fprintf (fp, "  Memory_value_format:\n");
      fprintf (fp, "    value = ");
      db_value_fprint (fp, &value->dbvalue);
      fprintf (fp, "\n\n");
    }

}

/*
 * heap_attrvalue_locate () - Locate disk attribute value information
 *   return: attrvalue or NULL
 *   attrid(in): The desired attribute identifier
 *   attr_info(in/out): The attribute information structure which describe the
 *                      desired attributes
 *
 * Note: Locate the disk attribute value information of an attribute
 * information structure which have been already initialized.
 */
HEAP_ATTRVALUE *
heap_attrvalue_locate (ATTR_ID attrid, HEAP_CACHE_ATTRINFO * attr_info)
{
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int i;

  for (i = 0, value = attr_info->values; i < attr_info->num_values; i++, value++)
    {
      if (attrid == value->attrid)
	{
	  return value;
	}
    }

  return NULL;
}

/*
 * heap_locate_attribute () -
 *   return:
 *   attrid(in):
 *   attr_info(in):
 */
static OR_ATTRIBUTE *
heap_locate_attribute (ATTR_ID attrid, HEAP_CACHE_ATTRINFO * attr_info)
{
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int i;

  for (i = 0, value = attr_info->values; i < attr_info->num_values; i++, value++)
    {
      if (attrid == value->attrid)
	{
	  /* Some altered attributes might have only the last representations of them. */
	  return (value->read_attrepr != NULL) ? value->read_attrepr : value->last_attrepr;
	}
    }

  return NULL;
}

/*
 * heap_locate_last_attrepr () -
 *   return:
 *   attrid(in):
 *   attr_info(in):
 */
OR_ATTRIBUTE *
heap_locate_last_attrepr (ATTR_ID attrid, HEAP_CACHE_ATTRINFO * attr_info)
{
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int i;

  for (i = 0, value = attr_info->values; i < attr_info->num_values; i++, value++)
    {
      if (attrid == value->attrid)
	{
	  return value->last_attrepr;
	}
    }

  return NULL;
}

/*
 * heap_attrinfo_access () - Access an attribute value which has been already read
 *   return:
 *   attrid(in): The desired attribute identifier
 *   attr_info(in/out): The attribute information structure which describe the
 *                      desired attributes
 *
 * Note: Find DB_VALUE of desired attribute identifier.
 * The dbvalue attributes must have been read by now using the
 * function heap_attrinfo_read_dbvalues ()
 */
DB_VALUE *
heap_attrinfo_access (ATTR_ID attrid, HEAP_CACHE_ATTRINFO * attr_info)
{
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */

  /* check to make sure the attr_info has been used */
  if (attr_info->num_values == -1)
    {
      return NULL;
    }

  value = heap_attrvalue_locate (attrid, attr_info);
  if (value == NULL || value->state == HEAP_UNINIT_ATTRVALUE)
    {
      er_log_debug (ARG_FILE_LINE, "heap_attrinfo_access: Unknown attrid = %d", attrid);
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      return NULL;
    }

  return &value->dbvalue;
}

/*
 * heap_get_class_subclasses () - get OIDs of subclasses for a given class
 * return : error code or NO_ERROR
 * thread_p (in)  :
 * class_oid (in) : OID of the parent class
 * count (out)	  : size of the subclasses array
 * subclasses (out) : array containing OIDs of subclasses
 *
 * Note: The subclasses array is maintained as an array of OID's,
 *	 the last element in the array will satisfy the OID_ISNULL() test.
 *	 The array_size has the number of actual elements allocated in the
 *	 array which may be more than the number of slots that have non-NULL
 *	 OIDs. The function adds the subclass oids to the existing array.
 *	 If the array is not large enough, it is reallocated using realloc.
 */
int
heap_get_class_subclasses (THREAD_ENTRY * thread_p, const OID * class_oid, int *count, OID ** subclasses)
{
  HEAP_SCANCACHE scan_cache;
  RECDES recdes;
  int error = NO_ERROR;

  error = heap_scancache_quick_start_root_hfid (thread_p, &scan_cache);
  if (error != NO_ERROR)
    {
      return error;
    }

  if (heap_get (thread_p, class_oid, &recdes, &scan_cache, PEEK, NULL_CHN) != S_SUCCESS)
    {
      heap_scancache_end (thread_p, &scan_cache);
      return ER_FAILED;
    }

  error = orc_subclasses_from_record (&recdes, count, subclasses);

  heap_scancache_end (thread_p, &scan_cache);

  return error;
}

/*
 * heap_class_get_partition_info () - Get partition information for the class
 *				      identified by class_oid
 * return : error code or NO_ERROR
 * class_oid (in) : class_oid
 * partition_info (in/out) : partition information
 * class_hfid (in/out) : HFID of the partitioned class
 * repr_id  (in/out) : class representation id
 * has_partition_info (out):
 *
 * Note: This function extracts the partition information from a class OID.
 */
static int
heap_class_get_partition_info (THREAD_ENTRY * thread_p, const OID * class_oid, OR_PARTITION * partition_info,
			       HFID * class_hfid, REPR_ID * repr_id, int *has_partition_info)
{
  int error = NO_ERROR;
  RECDES recdes;
  HEAP_SCANCACHE scan_cache;
  OID root_oid;

  assert (class_oid != NULL);

  if (heap_scancache_quick_start_root_hfid (thread_p, &scan_cache) != NO_ERROR)
    {
      return ER_FAILED;
    }

  scan_cache.mvcc_snapshot = logtb_get_mvcc_snapshot (thread_p);
  if (scan_cache.mvcc_snapshot == NULL)
    {
      error = er_errid ();
      return (error == NO_ERROR ? ER_FAILED : error);
    }

  if (heap_get_with_class_oid (thread_p, &root_oid, class_oid, &recdes, &scan_cache, S_SELECT, PEEK, NULL,
			       LOG_ERROR_IF_DELETED) != S_SUCCESS)
    {
      error = ER_FAILED;
      goto cleanup;
    }

  error = or_class_get_partition_info (&recdes, partition_info, repr_id, has_partition_info);
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  if (class_hfid != NULL)
    {
      or_class_hfid (&recdes, class_hfid);
    }

cleanup:
  heap_scancache_end (thread_p, &scan_cache);

  return error;
}

/*
 * heap_get_partition_attributes () - get attribute ids for columns of
 *				      _db_partition class
 * return : error code or NO_ERROR
 * thread_p (in)      :
 * cls_oid (in)	      : _db_partition class OID
 * type_id (in/out)   : holder for the type attribute id
 * values_id (in/out) : holder for the values attribute id
 */
static int
heap_get_partition_attributes (THREAD_ENTRY * thread_p, const OID * cls_oid, ATTR_ID * type_id, ATTR_ID * values_id)
{
  int idx_cache = -1;
  RECDES recdes;
  HEAP_SCANCACHE scan;
  HEAP_CACHE_ATTRINFO attr_info;
  int error = NO_ERROR;
  int i = 0;
  const char *attr_name = NULL;
  bool is_scan_cache_started = false, is_attrinfo_started = false;

  if (type_id == NULL || values_id == NULL)
    {
      assert (false);
      error = ER_FAILED;
      goto cleanup;
    }
  *type_id = *values_id = NULL_ATTRID;

  if (heap_scancache_quick_start_root_hfid (thread_p, &scan) != NO_ERROR)
    {
      error = ER_FAILED;
      goto cleanup;
    }
  is_scan_cache_started = true;

  error = heap_attrinfo_start (thread_p, cls_oid, -1, NULL, &attr_info);
  if (error != NO_ERROR)
    {
      goto cleanup;
    }
  is_attrinfo_started = true;

  if (heap_get (thread_p, cls_oid, &recdes, &scan, PEEK, NULL_CHN) != S_SUCCESS)
    {
      error = ER_FAILED;
      goto cleanup;
    }

  for (i = 0; i < attr_info.num_values && (*type_id == NULL_ATTRID || *values_id == NULL_ATTRID); i++)
    {
      attr_name = or_get_attrname (&recdes, i);
      if (attr_name == NULL)
	{
	  error = ER_FAILED;
	  goto cleanup;
	}
      if (strcmp (attr_name, "ptype") == 0)
	{
	  *type_id = i;
	}

      if (strcmp (attr_name, "pvalues") == 0)
	{
	  *values_id = i;
	}
    }

  if (*type_id == NULL_ATTRID || *values_id == NULL_ATTRID)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      error = ER_FAILED;
    }

cleanup:
  if (is_attrinfo_started)
    {
      heap_attrinfo_end (thread_p, &attr_info);
    }
  if (is_scan_cache_started)
    {
      heap_scancache_end (thread_p, &scan);
    }
  return error;
}

/*
 * heap_get_partitions_from_subclasses () - Get partition information from a
 *					    list of subclasses
 * return : error code or NO_ERROR
 * thread_p (in)	:
 * subclasses (in)	: subclasses OIDs
 * parts_count (in/out) : number of "useful" elements in parts
 * parts (in/out)	: partitions
 *
 *  Note: Memory for the partition array must be allocated before calling this
 *  function and must be enough to store all partitions. The value from
 *  position 0 in the partitions array will contain information from the
 *  master class
 */
static int
heap_get_partitions_from_subclasses (THREAD_ENTRY * thread_p, const OID * subclasses, int *parts_count,
				     OR_PARTITION * parts)
{
  int part_idx = 0, i;
  int error = NO_ERROR;
  OR_PARTITION *part_info = NULL;
  HFID part_hfid;
  REPR_ID repr_id;
  int has_partition_info = 0;

  if (parts == NULL)
    {
      assert (false);
      error = ER_FAILED;
      goto cleanup;
    }

  /* the partition information for the master class will be set by the caller */
  part_idx = 1;

  /* loop through subclasses and load partition information if the subclass is a partition */
  for (i = 0; !OID_ISNULL (&subclasses[i]); i++)
    {
      /* Get partition information from this subclass. part_info will be the OID of the tuple from _db_partition
       * containing partition information */
      error =
	heap_class_get_partition_info (thread_p, &subclasses[i], &parts[part_idx], &part_hfid, &repr_id,
				       &has_partition_info);
      if (error != NO_ERROR)
	{
	  goto cleanup;
	}

      if (has_partition_info == 0)
	{
	  /* this is not a partition, this is a simple subclass */
	  continue;
	}

      COPY_OID (&(parts[part_idx].class_oid), &subclasses[i]);
      HFID_COPY (&(parts[part_idx].class_hfid), &part_hfid);
      parts[part_idx].rep_id = repr_id;

      part_idx++;
    }
  *parts_count = part_idx;

cleanup:
  if (error != NO_ERROR)
    {
      /* free memory for the values of partitions */
      for (i = 1; i < part_idx; i++)
	{
	  if (parts[i].values != NULL)
	    {
	      db_seq_free (parts[i].values);
	    }
	}
    }
  return error;
}

/*
 * heap_get_class_partitions () - get partitions information for a class
 * return : error code or NO_ERROR
 * thread_p (in)	:
 * class_oid (in)	: class OID
 * parts (in/out)	: partitions information
 * parts_count (in/out)	: number of partitions
 */
int
heap_get_class_partitions (THREAD_ENTRY * thread_p, const OID * class_oid, OR_PARTITION ** parts, int *parts_count)
{
  int subclasses_count = 0;
  OID *subclasses = NULL;
  OR_PARTITION part_info;
  int error = NO_ERROR;
  OR_PARTITION *partitions = NULL;
  REPR_ID class_repr_id = NULL_REPRID;
  HFID class_hfid;
  int has_partition_info = 0;

  *parts = NULL;
  *parts_count = 0;
  part_info.values = NULL;

  /* This class might have partitions and subclasses. In order to get partition information we have to: 1. Get the OIDs 
   * for all subclasses 2. Get partition information for all OIDs 3. Build information only for those subclasses which
   * are partitions */
  error =
    heap_class_get_partition_info (thread_p, class_oid, &part_info, &class_hfid, &class_repr_id, &has_partition_info);
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  if (has_partition_info == 0)
    {
      /* this class does not have partitions */
      error = NO_ERROR;
      goto cleanup;
    }

  /* Get OIDs for subclasses of class_oid. Some of them will be partitions */
  error = heap_get_class_subclasses (thread_p, class_oid, &subclasses_count, &subclasses);
  if (error != NO_ERROR)
    {
      goto cleanup;
    }
  else if (subclasses_count == 0)
    {
      /* This means that class_oid actually points to a partition and not the master class. We return NO_ERROR here
       * since there's no partition information */
      error = NO_ERROR;
      goto cleanup;
    }

  /* Allocate memory for partitions. We allocate more memory than needed here because the call to
   * heap_get_class_subclasses from above actually returned a larger count than the useful information. Also, not all
   * subclasses are necessarily partitions. */
  partitions = (OR_PARTITION *) db_private_alloc (thread_p, (subclasses_count + 1) * sizeof (OR_PARTITION));
  if (partitions == NULL)
    {
      error = ER_OUT_OF_VIRTUAL_MEMORY;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 1, (subclasses_count + 1) * sizeof (OR_PARTITION));
      goto cleanup;
    }

  error = heap_get_partitions_from_subclasses (thread_p, subclasses, parts_count, partitions);
  if (error != NO_ERROR)
    {
      ASSERT_ERROR ();
      goto cleanup;
    }

  /* fill the information for the root (partitioned class) */
  COPY_OID (&partitions[0].class_oid, class_oid);
  HFID_COPY (&partitions[0].class_hfid, &class_hfid);
  partitions[0].partition_type = part_info.partition_type;
  partitions[0].rep_id = class_repr_id;
  partitions[0].values = NULL;
  if (part_info.values != NULL)
    {
      partitions[0].values = set_copy (part_info.values);
      if (partitions[0].values == NULL)
	{
	  error = er_errid ();
	  goto cleanup;
	}
      set_free (part_info.values);
      part_info.values = NULL;
    }

  *parts = partitions;

cleanup:
  if (subclasses != NULL)
    {
      free_and_init (subclasses);
    }
  if (part_info.values != NULL)
    {
      set_free (part_info.values);
    }
  if (error != NO_ERROR && partitions != NULL)
    {
      db_private_free (thread_p, partitions);
      *parts = NULL;
      *parts_count = 0;
    }
  return error;
}

/*
 * heap_clear_partition_info () - free partitions info from heap_get_class_partitions
 * return : void
 * thread_p (in)	:
 * parts (in)		: partitions information
 * parts_count (in)	: number of partitions
 */
void
heap_clear_partition_info (THREAD_ENTRY * thread_p, OR_PARTITION * parts, int parts_count)
{
  if (parts != NULL)
    {
      int i;

      for (i = 0; i < parts_count; i++)
	{
	  if (parts[i].values != NULL)
	    {
	      db_seq_free (parts[i].values);
	    }
	}

      db_private_free (thread_p, parts);
    }
}

/*
 * heap_get_class_supers () - get OIDs of superclasses of a class
 * return : error code or NO_ERROR
 * thread_p (in)  : thread entry
 * class_oid (in) : OID of the subclass
 * super_oids (in/out) : OIDs of the superclasses
 * count (in/out)      : number of elements in super_oids
 */
int
heap_get_class_supers (THREAD_ENTRY * thread_p, const OID * class_oid, OID ** super_oids, int *count)
{
  HEAP_SCANCACHE scan_cache;
  RECDES recdes;
  int error = NO_ERROR;

  error = heap_scancache_quick_start_root_hfid (thread_p, &scan_cache);
  if (error != NO_ERROR)
    {
      return error;
    }

  if (heap_get (thread_p, class_oid, &recdes, &scan_cache, PEEK, NULL_CHN) != S_SUCCESS)
    {
      heap_scancache_end (thread_p, &scan_cache);
      return ER_FAILED;
    }

  error = orc_superclasses_from_record (&recdes, count, super_oids);

  heap_scancache_end (thread_p, &scan_cache);

  return error;
}

/*
 * heap_attrinfo_check () -
 *   return: NO_ERROR
 *   inst_oid(in): The instance oid
 *   attr_info(in): The attribute information structure which describe the
 *                  desired attributes
 */
static int
heap_attrinfo_check (const OID * inst_oid, HEAP_CACHE_ATTRINFO * attr_info)
{
  int ret = NO_ERROR;

  if (inst_oid != NULL)
    {
      /* 
       * The OIDs must be equal
       */
      if (!OID_EQ (&attr_info->inst_oid, inst_oid))
	{
	  if (!OID_ISNULL (&attr_info->inst_oid))
	    {
	      ret = ER_HEAP_WRONG_ATTRINFO;
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ret, 6, attr_info->inst_oid.volid,
		      attr_info->inst_oid.pageid, attr_info->inst_oid.slotid, inst_oid->volid, inst_oid->pageid,
		      inst_oid->slotid);
	      goto exit_on_error;
	    }

	  attr_info->inst_oid = *inst_oid;
	}
    }
  else
    {
      if (!OID_ISNULL (&attr_info->inst_oid))
	{
	  ret = ER_HEAP_WRONG_ATTRINFO;
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ret, 6, attr_info->inst_oid.volid, attr_info->inst_oid.pageid,
		  attr_info->inst_oid.slotid, NULL_VOLID, NULL_PAGEID, NULL_SLOTID);
	  goto exit_on_error;
	}
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_attrinfo_set () - Set the value of given attribute
 *   return: NO_ERROR
 *   inst_oid(in): The instance oid
 *   attrid(in): The identifier of the attribute to be set
 *   attr_val(in): The memory value of the attribute
 *   attr_info(in/out): The attribute information structure which describe the
 *                      desired attributes
 *
 * Note: Set DB_VALUE of desired attribute identifier.
 */
int
heap_attrinfo_set (const OID * inst_oid, ATTR_ID attrid, DB_VALUE * attr_val, HEAP_CACHE_ATTRINFO * attr_info)
{
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  PR_TYPE *pr_type;		/* Primitive type array function structure */
  TP_DOMAIN_STATUS dom_status;
  int ret = NO_ERROR;

  /* 
   * check to make sure the attr_info has been used, should never be empty.
   */

  if (attr_info->num_values == -1)
    {
      return ER_FAILED;
    }

  ret = heap_attrinfo_check (inst_oid, attr_info);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  value = heap_attrvalue_locate (attrid, attr_info);
  if (value == NULL)
    {
      goto exit_on_error;
    }

  pr_type = PR_TYPE_FROM_ID (value->last_attrepr->type);
  if (pr_type == NULL)
    {
      goto exit_on_error;
    }

  ret = pr_clear_value (&value->dbvalue);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  ret =
    db_value_domain_init (&value->dbvalue, value->last_attrepr->type, value->last_attrepr->domain->precision,
			  value->last_attrepr->domain->scale);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  /* 
   * As we use "writeval" to do the writing and that function gets
   * enough domain information, we can use non-exact domain matching
   * here to defer the coercion until it is written.
   */
  dom_status = tp_domain_check (value->last_attrepr->domain, attr_val, TP_EXACT_MATCH);
  if (dom_status == DOMAIN_COMPATIBLE)
    {
      /* 
       * the domains match exactly, set the value and proceed.  Copy
       * the source only if it's a set-valued thing (that's the purpose
       * of the third argument).
       */
      ret = (*(pr_type->setval)) (&value->dbvalue, attr_val, TP_IS_SET_TYPE (pr_type->id));
    }
  else
    {
      /* the domains don't match, must attempt coercion */
      dom_status = tp_value_auto_cast (attr_val, &value->dbvalue, value->last_attrepr->domain);
      if (dom_status != DOMAIN_COMPATIBLE)
	{
	  ret = tp_domain_status_er_set (dom_status, ARG_FILE_LINE, attr_val, value->last_attrepr->domain);
	  assert (er_errid () != NO_ERROR);

	  DB_MAKE_NULL (&value->dbvalue);
	}
    }

  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  value->state = HEAP_WRITTEN_ATTRVALUE;

  return ret;

exit_on_error:

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_attrinfo_set_uninitialized () - Read unitialized attributes
 *   return: NO_ERROR
 *   inst_oid(in): The instance oid
 *   recdes(in): The instance record descriptor
 *   attr_info(in/out): The attribute information structure which describe the
 *                      desired attributes
 *
 * Note: Read the db values of the unitialized attributes from the
 * given recdes. This function is used when we are ready to
 * transform an object that has been updated/inserted in the server.
 * If the object has been updated, recdes must be the old object
 * (the one on disk), so we can set the rest of the uninitialized
 * attributes from the old object.
 * If the object is a new one, recdes should be NULL, since there
 * is not an object on disk, the rest of the unitialized
 * attributes are set from default values.
 */
static int
heap_attrinfo_set_uninitialized (THREAD_ENTRY * thread_p, OID * inst_oid, RECDES * recdes,
				 HEAP_CACHE_ATTRINFO * attr_info)
{
  int i;
  REPR_ID reprid;		/* Representation of object */
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int ret = NO_ERROR;

  ret = heap_attrinfo_check (inst_oid, attr_info);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  /* 
   * Make sure that we have the needed cached representation.
   */

  if (recdes != NULL)
    {
      reprid = or_rep_id (recdes);
    }
  else
    {
      reprid = attr_info->last_classrepr->id;
    }

  if (attr_info->read_classrepr == NULL || attr_info->read_classrepr->id != reprid)
    {
      /* Get the needed representation */
      ret = heap_attrinfo_recache (thread_p, reprid, attr_info);
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }

  /* 
   * Go over the attribute values and set the ones that have not been
   * initialized
   */
  for (i = 0; i < attr_info->num_values; i++)
    {
      value = &attr_info->values[i];
      if (value->state == HEAP_UNINIT_ATTRVALUE)
	{
	  ret = heap_attrvalue_read (recdes, value, attr_info);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }
	}
      else if (value->state == HEAP_WRITTEN_ATTRVALUE
	       && (value->last_attrepr->type == DB_TYPE_BLOB || value->last_attrepr->type == DB_TYPE_CLOB))
	{
	  DB_VALUE *save;
	  save = db_value_copy (&value->dbvalue);
	  db_value_clear (&value->dbvalue);

	  /* read and delete old value */
	  ret = heap_attrvalue_read (recdes, value, attr_info);
	  if (ret != NO_ERROR)
	    {
	      goto exit_on_error;
	    }
	  if (!db_value_is_null (&value->dbvalue))
	    {
	      DB_ELO *elo;

	      assert (db_value_type (&value->dbvalue) == DB_TYPE_BLOB
		      || db_value_type (&value->dbvalue) == DB_TYPE_CLOB);
	      elo = db_get_elo (&value->dbvalue);
	      if (elo)
		{
		  ret = db_elo_delete (elo);
		}
	      db_value_clear (&value->dbvalue);
	      ret = (ret >= 0 ? NO_ERROR : ret);
	      if (ret != NO_ERROR)
		{
		  goto exit_on_error;
		}
	    }
	  value->state = HEAP_WRITTEN_ATTRVALUE;
	  db_value_clone (save, &value->dbvalue);
	  db_value_free (save);
	}
    }

  if (recdes != NULL)
    {
      attr_info->inst_chn = or_chn (recdes);
    }
  else
    {
      attr_info->inst_chn = -1;
    }

  return ret;

exit_on_error:

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_attrinfo_get_disksize () - Find the disk size needed to transform the object
 *                        represented by attr_info
 *   return: size of the object
 *   attr_info(in/out): The attribute information structure
 *
 * Note: Find the disk size needed to transform the object represented
 * by the attribute information structure.
 */
static int
heap_attrinfo_get_disksize (HEAP_CACHE_ATTRINFO * attr_info, int *offset_size_ptr)
{
  int i, size;
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */

  *offset_size_ptr = OR_BYTE_SIZE;

re_check:
  size = 0;
  for (i = 0; i < attr_info->num_values; i++)
    {
      value = &attr_info->values[i];

      if (value->last_attrepr->is_fixed != 0)
	{
	  size += tp_domain_disk_size (value->last_attrepr->domain);
	}
      else
	{
	  size += pr_data_writeval_disk_size (&value->dbvalue);
	}
    }

  size += OR_MVCC_INSERT_HEADER_SIZE;
  size += OR_VAR_TABLE_SIZE_INTERNAL (attr_info->last_classrepr->n_variable, *offset_size_ptr);
  size += OR_BOUND_BIT_BYTES (attr_info->last_classrepr->n_attributes - attr_info->last_classrepr->n_variable);

  if (*offset_size_ptr == OR_BYTE_SIZE && size > OR_MAX_BYTE)
    {
      *offset_size_ptr = OR_SHORT_SIZE;	/* 2byte */
      goto re_check;
    }
  if (*offset_size_ptr == OR_SHORT_SIZE && size > OR_MAX_SHORT)
    {
      *offset_size_ptr = BIG_VAR_OFFSET_SIZE;	/* 4byte */
      goto re_check;
    }

  return size;
}

/*
 * heap_attrinfo_transform_to_disk () - Transform to disk an attribute information
 *                               kind of instance
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS, S_DOESNT_FIT,
 *                      S_ERROR)
 *   attr_info(in/out): The attribute information structure
 *   old_recdes(in): where the object's disk format is deposited
 *   new_recdes(in):
 *
 * Note: Transform the object represented by attr_info to disk format
 */
SCAN_CODE
heap_attrinfo_transform_to_disk (THREAD_ENTRY * thread_p, HEAP_CACHE_ATTRINFO * attr_info, RECDES * old_recdes,
				 RECDES * new_recdes)
{
  return heap_attrinfo_transform_to_disk_internal (thread_p, attr_info, old_recdes, new_recdes, LOB_FLAG_INCLUDE_LOB);
}

/*
 * heap_attrinfo_transform_to_disk_except_lob () -
 *                           Transform to disk an attribute information
 *                           kind of instance. Do not create lob.
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS, S_DOESNT_FIT,
 *                      S_ERROR)
 *   attr_info(in/out): The attribute information structure
 *   old_recdes(in): where the object's disk format is deposited
 *   new_recdes(in):
 *
 * Note: Transform the object represented by attr_info to disk format
 */
SCAN_CODE
heap_attrinfo_transform_to_disk_except_lob (THREAD_ENTRY * thread_p, HEAP_CACHE_ATTRINFO * attr_info,
					    RECDES * old_recdes, RECDES * new_recdes)
{
  return heap_attrinfo_transform_to_disk_internal (thread_p, attr_info, old_recdes, new_recdes, LOB_FLAG_EXCLUDE_LOB);
}

/*
 * heap_attrinfo_transform_to_disk_internal () -
 *                         Transform to disk an attribute information
 *                         kind of instance.
 *   return: SCAN_CODE
 *           (Either of S_SUCCESS, S_DOESNT_FIT,
 *                      S_ERROR)
 *   attr_info(in/out): The attribute information structure
 *   old_recdes(in): where the object's disk format is deposited
 *   new_recdes(in):
 *   lob_create_flag(in):
 *
 * Note: Transform the object represented by attr_info to disk format
 */
static SCAN_CODE
heap_attrinfo_transform_to_disk_internal (THREAD_ENTRY * thread_p, HEAP_CACHE_ATTRINFO * attr_info, RECDES * old_recdes,
					  RECDES * new_recdes, int lob_create_flag)
{
  OR_BUF orep, *buf;
  char *ptr_bound, *ptr_varvals;
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  DB_VALUE temp_dbvalue;
  PR_TYPE *pr_type;		/* Primitive type array function structure */
  unsigned int repid_bits;
  SCAN_CODE status;
  int i;
  DB_VALUE *dbvalue = NULL;
  int expected_size, tmp;
  volatile int offset_size;
  int mvcc_wasted_space = 0, header_size;

  /* check to make sure the attr_info has been used, it should not be empty. */
  if (attr_info->num_values == -1)
    {
      return S_ERROR;
    }

  /* 
   * Get any of the values that have not been set/read
   */
  if (heap_attrinfo_set_uninitialized (thread_p, &attr_info->inst_oid, old_recdes, attr_info) != NO_ERROR)
    {
      return S_ERROR;
    }

  /* Start transforming the dbvalues into disk values for the object */
  OR_BUF_INIT2 (orep, new_recdes->data, new_recdes->area_size);
  buf = &orep;

  expected_size = heap_attrinfo_get_disksize (attr_info, &tmp);
  offset_size = tmp;

  mvcc_wasted_space = (OR_MVCC_MAX_HEADER_SIZE - OR_MVCC_INSERT_HEADER_SIZE);
  /* reserve enough space if need to add additional MVCC header info */
  expected_size += mvcc_wasted_space;

  switch (_setjmp (buf->env))
    {
    case 0:
      status = S_SUCCESS;

      /* 
       * Store the representation of the class along with bound bit
       * flag information
       */

      repid_bits = attr_info->last_classrepr->id;
      /* 
       * Do we have fixed value attributes ?
       */
      if ((attr_info->last_classrepr->n_attributes - attr_info->last_classrepr->n_variable) != 0)
	{
	  repid_bits |= OR_BOUND_BIT_FLAG;
	}

      /* offset size */
      OR_SET_VAR_OFFSET_SIZE (repid_bits, offset_size);

      /* 
       * We must increase the current value by one so that clients
       * can detect the change in object. That is, clients will need to
       * refetch the object.
       */
      attr_info->inst_chn++;
      if (!heap_is_mvcc_disabled_for_class (&(attr_info->class_oid)))
	{
	  repid_bits |= (OR_MVCC_FLAG_VALID_INSID << OR_MVCC_FLAG_SHIFT_BITS);
	  or_put_int (buf, repid_bits);
	  or_put_bigint (buf, 0);	/* MVCC insert id */
	  or_put_int (buf, 0);	/* CHN, short size */
	  header_size = OR_MVCC_INSERT_HEADER_SIZE;
	}
      else
	{
	  or_put_int (buf, repid_bits);
	  or_put_int (buf, attr_info->inst_chn);
	  header_size = OR_NON_MVCC_HEADER_SIZE;
	}

      /* 
       * Calculate the pointer address to variable offset attribute table,
       * fixed attributes, and variable attributes
       */

      ptr_bound =
	OR_GET_BOUND_BITS (buf->buffer, attr_info->last_classrepr->n_variable, attr_info->last_classrepr->fixed_length);

      /* 
       * Variable offset table is relative to the beginning of the buffer
       */

      ptr_varvals =
	ptr_bound + OR_BOUND_BIT_BYTES (attr_info->last_classrepr->n_attributes -
					attr_info->last_classrepr->n_variable);

      /* Need to make sure that the bound array is not past the allocated buffer because OR_ENABLE_BOUND_BIT() will
       * just slam the bound bit without checking the length. */

      if (ptr_varvals + mvcc_wasted_space >= buf->endptr)
	{
	  new_recdes->length = -expected_size;	/* set to negative */
	  return S_DOESNT_FIT;
	}

      for (i = 0; i < attr_info->num_values; i++)
	{
	  value = &attr_info->values[i];
	  dbvalue = &value->dbvalue;
	  pr_type = value->last_attrepr->domain->type;
	  if (pr_type == NULL)
	    {
	      return S_ERROR;
	    }

	  /* 
	   * Is this a fixed or variable attribute ?
	   */
	  if (value->last_attrepr->is_fixed != 0)
	    {
	      /* 
	       * Fixed attribute
	       * Write the fixed attributes values, if unbound, does not matter
	       * what value is stored. We need to set the appropiate bit in the
	       * bound bit array for fixed attributes. For variable attributes,
	       */
	      buf->ptr =
		buf->buffer + OR_FIXED_ATTRIBUTES_OFFSET_BY_OBJ (buf->buffer,
								 attr_info->last_classrepr->n_variable) +
		value->last_attrepr->location;
	      if (value->do_increment)
		{
		  if (qdata_increment_dbval (dbvalue, dbvalue, value->do_increment) != NO_ERROR)
		    {
		      status = S_ERROR;
		      break;
		    }
		}
	      if (dbvalue == NULL || db_value_is_null (dbvalue) == true)
		{
		  /* 
		   * This is an unbound value.
		   *  1) Set any value in the fixed array value table, so we can
		   *     advance to next attribute.
		   *  2) and set the bound bit as unbound
		   */
		  db_value_domain_init (&temp_dbvalue, value->last_attrepr->type,
					value->last_attrepr->domain->precision, value->last_attrepr->domain->scale);
		  dbvalue = &temp_dbvalue;
		  OR_CLEAR_BOUND_BIT (ptr_bound, value->last_attrepr->position);

		  /* 
		   * pad the appropriate amount, writeval needs to be modified
		   * to accept a domain so it can perform this padding.
		   */
		  or_pad (buf, tp_domain_disk_size (value->last_attrepr->domain));

		}
	      else
		{
		  /* 
		   * Write the value.
		   */
		  OR_ENABLE_BOUND_BIT (ptr_bound, value->last_attrepr->position);
		  (*(pr_type->data_writeval)) (buf, dbvalue);
		}
	    }
	  else
	    {
	      /* 
	       * Variable attribute
	       *  1) Set the offset to this value in the variable offset table
	       *  2) Set the value in the variable value portion of the disk
	       *     object (Only if the value is bound)
	       */

	      /* 
	       * Write the offset onto the variable offset table and remember
	       * the current pointer to the variable offset table
	       */

	      if (value->do_increment != 0)
		{
		  status = S_ERROR;
		  break;
		}
	      buf->ptr = (char *) (OR_VAR_ELEMENT_PTR (buf->buffer, value->last_attrepr->location));
	      /* compute the variable offsets relative to the end of the header (beginning of variable table) */
	      or_put_offset_internal (buf, CAST_BUFLEN (ptr_varvals - buf->buffer - header_size), offset_size);

	      if (dbvalue != NULL && db_value_is_null (dbvalue) != true)
		{
		  /* 
		   * Now write the value and remember the current pointer
		   * to variable value array for the next element.
		   */
		  buf->ptr = ptr_varvals;

		  if (lob_create_flag == LOB_FLAG_INCLUDE_LOB && value->state == HEAP_WRITTEN_ATTRVALUE
		      && (pr_type->id == DB_TYPE_BLOB || pr_type->id == DB_TYPE_CLOB))
		    {
		      DB_ELO dest_elo, *elo_p;
		      char *save_meta_data, *new_meta_data;
		      int error;

		      assert (db_value_type (dbvalue) == DB_TYPE_BLOB || db_value_type (dbvalue) == DB_TYPE_CLOB);

		      elo_p = db_get_elo (dbvalue);

		      if (elo_p == NULL)
			{
			  continue;
			}

		      new_meta_data = heap_get_class_name (thread_p, &(attr_info->class_oid));

		      if (new_meta_data == NULL)
			{
			  status = S_ERROR;
			  break;
			}
		      save_meta_data = elo_p->meta_data;
		      elo_p->meta_data = new_meta_data;
		      error = db_elo_copy (db_get_elo (dbvalue), &dest_elo);

		      free_and_init (elo_p->meta_data);
		      elo_p->meta_data = save_meta_data;

		      /* The purpose of HEAP_WRITTEN_LOB_ATTRVALUE is to avoid reenter this branch. In the first pass,
		       * this branch is entered and elo is copied. When BUFFER_OVERFLOW happens, we need avoid to copy
		       * elo again. Otherwize it will generate 2 copies. */
		      value->state = HEAP_WRITTEN_LOB_ATTRVALUE;

		      error = (error >= 0 ? NO_ERROR : error);
		      if (error == NO_ERROR)
			{
			  db_value_clear (dbvalue);
			  db_make_elo (dbvalue, pr_type->id, &dest_elo);
			  dbvalue->need_clear = true;
			}
		      else
			{
			  status = S_ERROR;
			  break;
			}
		    }

		  (*(pr_type->data_writeval)) (buf, dbvalue);
		  ptr_varvals = buf->ptr;
		}
	    }
	}

      if (attr_info->last_classrepr->n_variable > 0)
	{
	  /* 
	   * The last element of the variable offset table points to the end of
	   * the object. The variable offset array starts with zero, so we can
	   * just access n_variable...
	   */

	  /* Write the offset to the end of the variable attributes table */
	  buf->ptr = ((char *) (OR_VAR_ELEMENT_PTR (buf->buffer, attr_info->last_classrepr->n_variable)));
	  or_put_offset_internal (buf, CAST_BUFLEN (ptr_varvals - buf->buffer - header_size), offset_size);
	  buf->ptr = PTR_ALIGN (buf->ptr, INT_ALIGNMENT);
	}

      /* Record the length of the object */
      new_recdes->length = CAST_BUFLEN (ptr_varvals - buf->buffer);

      /* if not enough MVCC wasted space need to reallocate */
      if (ptr_varvals + mvcc_wasted_space < buf->endptr)
	{
	  break;
	}

      /* 
       * if the longjmp status was anything other than ER_TF_BUFFER_OVERFLOW,
       * it represents an error condition and er_set will have been called
       */
    case ER_TF_BUFFER_OVERFLOW:

      status = S_DOESNT_FIT;


      /* 
       * Give a hint of the needed space. The hint is given as a negative
       * value in the record descriptor length. Make sure that this length
       * is larger than the current record descriptor area.
       */

      new_recdes->length = -expected_size;	/* set to negative */

      if (new_recdes->area_size > -new_recdes->length)
	{
	  /* 
	   * This may be an error. The estimated disk size is smaller
	   * than the current record descriptor area size. For now assume
	   * at least 20% above the current area descriptor. The main problem
	   * is that heap_attrinfo_get_disksize () guess its size as much as
	   * possible
	   */
	  new_recdes->length = -(int) (new_recdes->area_size * 1.20);
	}
      break;

    default:
      status = S_ERROR;
      break;
    }

  return status;
}

/*
 * heap_attrinfo_start_refoids () - Initialize an attribute information structure
 * with attributes that may reference other objects
 *   return: NO_ERROR
 *   class_oid(in): The class identifier of the instances where values
 *                  attributes values are going to be read.
 *   attr_info(in/out): The attribute information structure
 *
 * Note: Initialize an attribute information structure with attributes
 * that may reference other objects (OIDs).
 *
 * Note: The caller must call heap_attrinfo_end after he is done with
 * attribute information.
 */

static int
heap_attrinfo_start_refoids (THREAD_ENTRY * thread_p, OID * class_oid, HEAP_CACHE_ATTRINFO * attr_info)
{
  ATTR_ID guess_attrids[HEAP_GUESS_NUM_ATTRS_REFOIDS];
  ATTR_ID *set_attrids;
  int num_found_attrs;
  OR_CLASSREP *classrepr;
  int classrepr_cacheindex = -1;
  OR_ATTRIBUTE *search_attrepr;
  int i;
  int ret = NO_ERROR;

  attr_info->num_values = -1;

  /* 
   * Find the current representation of the class, then scan all its
   * attributes finding the ones that may reference objects
   */

  classrepr = heap_classrepr_get (thread_p, class_oid, NULL, NULL_REPRID, &classrepr_cacheindex);
  if (classrepr == NULL)
    {
      return ER_FAILED;
    }

  /* 
   * Go over the list of attributes until the desired attributes (OIDs, sets)
   * are found
   */

  if (classrepr->n_attributes > HEAP_GUESS_NUM_ATTRS_REFOIDS)
    {
      set_attrids = (ATTR_ID *) malloc (classrepr->n_attributes * sizeof (ATTR_ID));
      if (set_attrids == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
		  classrepr->n_attributes * sizeof (ATTR_ID));
	  (void) heap_classrepr_free (classrepr, &classrepr_cacheindex);
	  return ER_OUT_OF_VIRTUAL_MEMORY;
	}
    }
  else
    {
      set_attrids = guess_attrids;
    }

  for (i = 0, num_found_attrs = 0; i < classrepr->n_attributes; i++)
    {
      search_attrepr = &classrepr->attributes[i];
      if (tp_domain_references_objects (search_attrepr->domain) == true)
	{
	  set_attrids[num_found_attrs++] = search_attrepr->id;
	}
    }

  ret = heap_attrinfo_start (thread_p, class_oid, num_found_attrs, set_attrids, attr_info);

  if (set_attrids != guess_attrids)
    {
      free_and_init (set_attrids);
    }

  if (ret == NO_ERROR)
    {
      ret = heap_classrepr_free (classrepr, &classrepr_cacheindex);
    }
  else
    {
      (void) heap_classrepr_free (classrepr, &classrepr_cacheindex);
    }

  return ret;
}

/*
 * heap_attrinfo_start_with_index () -
 *   return:
 *   class_oid(in):
 *   class_recdes(in):
 *   attr_info(in):
 *   idx_info(in):
 */
int
heap_attrinfo_start_with_index (THREAD_ENTRY * thread_p, OID * class_oid, RECDES * class_recdes,
				HEAP_CACHE_ATTRINFO * attr_info, HEAP_IDX_ELEMENTS_INFO * idx_info)
{
  ATTR_ID guess_attrids[HEAP_GUESS_NUM_INDEXED_ATTRS];
  ATTR_ID *set_attrids;
  int num_found_attrs;
  OR_CLASSREP *classrepr = NULL;
  int classrepr_cacheindex = -1;
  OR_ATTRIBUTE *search_attrepr;
  int i, j;
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  int *num_btids;
  OR_INDEX *indexp;

  idx_info->has_single_col = 0;
  idx_info->has_multi_col = 0;
  idx_info->num_btids = 0;

  num_btids = &idx_info->num_btids;

  set_attrids = guess_attrids;
  attr_info->num_values = -1;	/* initialize attr_info */

  classrepr = heap_classrepr_get (thread_p, class_oid, class_recdes, NULL_REPRID, &classrepr_cacheindex);
  if (classrepr == NULL)
    {
      return ER_FAILED;
    }

  if (classrepr->n_attributes > HEAP_GUESS_NUM_INDEXED_ATTRS)
    {
      set_attrids = (ATTR_ID *) malloc (classrepr->n_attributes * sizeof (ATTR_ID));
      if (set_attrids == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
		  classrepr->n_attributes * sizeof (ATTR_ID));
	  (void) heap_classrepr_free (classrepr, &classrepr_cacheindex);
	  return ER_OUT_OF_VIRTUAL_MEMORY;
	}
    }
  else
    {
      set_attrids = guess_attrids;
    }

  /* 
   * Read the number of BTID's in this class
   */
  *num_btids = classrepr->n_indexes;

  for (j = 0; j < *num_btids; j++)
    {
      indexp = &classrepr->indexes[j];
      if (indexp->n_atts == 1)
	{
	  idx_info->has_single_col = 1;
	}
      else if (indexp->n_atts > 1)
	{
	  idx_info->has_multi_col = 1;
	}
      /* check for already found both */
      if (idx_info->has_single_col && idx_info->has_multi_col)
	{
	  break;
	}
    }

  /* 
   * Go over the list of attrs until all indexed attributes (OIDs, sets)
   * are found
   */
  for (i = 0, num_found_attrs = 0, search_attrepr = classrepr->attributes; i < classrepr->n_attributes;
       i++, search_attrepr++)
    {
      if (search_attrepr->n_btids <= 0)
	{
	  continue;
	}

      if (idx_info->has_single_col)
	{
	  for (j = 0; j < *num_btids; j++)
	    {
	      indexp = &classrepr->indexes[j];
	      if (indexp->n_atts == 1 && indexp->atts[0]->id == search_attrepr->id)
		{
		  set_attrids[num_found_attrs++] = search_attrepr->id;
		  break;
		}
	    }
	}
    }				/* for (i = 0 ...) */

  if (idx_info->has_multi_col == 0 && num_found_attrs == 0)
    {
      /* initialize the attrinfo cache and return, there is nothing else to do */
      /* (void) memset(attr_info, '\0', sizeof (HEAP_CACHE_ATTRINFO)); */

      /* now set the num_values to -1 which indicates that this is an empty HEAP_CACHE_ATTRINFO and shouldn't be
       * operated on. */
      attr_info->num_values = -1;

      /* free the class representation */
      (void) heap_classrepr_free (classrepr, &classrepr_cacheindex);
    }
  else
    {				/* num_found_attrs > 0 */
      /* initialize attribute information */
      attr_info->class_oid = *class_oid;
      attr_info->last_cacheindex = classrepr_cacheindex;
      attr_info->read_cacheindex = -1;
      attr_info->last_classrepr = classrepr;
      attr_info->read_classrepr = NULL;
      OID_SET_NULL (&attr_info->inst_oid);
      attr_info->inst_chn = NULL_CHN;
      attr_info->num_values = num_found_attrs;

      if (num_found_attrs <= 0)
	{
	  attr_info->values = NULL;
	}
      else
	{
	  attr_info->values =
	    (HEAP_ATTRVALUE *) db_private_alloc (thread_p, (num_found_attrs * sizeof (HEAP_ATTRVALUE)));
	  if (attr_info->values == NULL)
	    {
	      /* free the class representation */
	      (void) heap_classrepr_free (classrepr, &classrepr_cacheindex);
	      attr_info->num_values = -1;
	      goto error;
	    }
	}

      /* 
       * Set the attribute identifier of the desired attributes in the value
       * attribute information, and indicates that the current value is
       * unitialized. That is, it has not been read, set or whatever.
       */
      for (i = 0; i < attr_info->num_values; i++)
	{
	  value = &attr_info->values[i];
	  value->attrid = set_attrids[i];
	  value->state = HEAP_UNINIT_ATTRVALUE;
	  value->last_attrepr = NULL;
	  value->read_attrepr = NULL;
	}

      /* 
       * Make last information to be recached for each individual attribute
       * value. Needed for WRITE and Default values
       */
      if (heap_attrinfo_recache_attrepr (attr_info, true) != NO_ERROR)
	{
	  /* classrepr will be freed in heap_attrinfo_end */
	  heap_attrinfo_end (thread_p, attr_info);
	  goto error;
	}
    }

  if (set_attrids != guess_attrids)
    {
      free_and_init (set_attrids);
    }

  if (num_found_attrs == 0 && idx_info->has_multi_col)
    {
      return 1;
    }
  else
    {
      return num_found_attrs;
    }

  /* **** */
error:

  if (set_attrids != guess_attrids)
    {
      free_and_init (set_attrids);
    }

  return ER_FAILED;
}

/*
 * heap_classrepr_find_index_id () - Find the indicated index ID from the class repr
 *   return: ID of desired index ot -1 if an error occurred.
 *   classrepr(in): The class representation.
 *   btid(in): The BTID of the interested index.
 *
 * Note: Locate the desired index by matching it with the passed BTID.
 * Return the ID of the index if found.
 */
int
heap_classrepr_find_index_id (OR_CLASSREP * classrepr, BTID * btid)
{
  int i;
  int id = -1;

  for (i = 0; i < classrepr->n_indexes; i++)
    {
      if (BTID_IS_EQUAL (&(classrepr->indexes[i].btid), btid))
	{
	  id = i;
	  break;
	}
    }

  return id;
}

/*
 * heap_attrinfo_start_with_btid () - Initialize an attribute information structure
 *   return: ID for the index which corresponds to the passed BTID.
 *           If an error occurred, a -1 is returned.
 *   class_oid(in): The class identifier of the instances where values
 *                  attributes values are going to be read.
 *   btid(in): The BTID of the interested index.
 *   attr_info(in/out): The attribute information structure
 *
 * Note: Initialize an attribute information structure, so that values
 * of instances can be retrieved based on the desired attributes.
 *
 * There are currently three functions which can be used to
 * initialize the attribute information structure; heap_attrinfo_start(),
 * heap_attrinfo_start_with_index() and this one.  This function determines
 * which attributes belong to the passed BTID and populate the
 * information structure on those attributes.
 *
 * The attrinfo structure is an structure where values of
 * instances can be read. For example an object is retrieved,
 * then some of its attributes are convereted to dbvalues and
 * placed in this structure.
 *
 * Note: The caller must call heap_attrinfo_end after he is done with
 * attribute information.
 */
int
heap_attrinfo_start_with_btid (THREAD_ENTRY * thread_p, OID * class_oid, BTID * btid, HEAP_CACHE_ATTRINFO * attr_info)
{
  ATTR_ID guess_attrids[HEAP_GUESS_NUM_INDEXED_ATTRS];
  ATTR_ID *set_attrids;
  OR_CLASSREP *classrepr = NULL;
  int i;
  int index_id = -1;
  int classrepr_cacheindex = -1;
  int num_found_attrs = 0;

  /* 
   *  We'll start by assuming that the number of attributes will fit into
   *  the preallocated array.
   */
  set_attrids = guess_attrids;

  attr_info->num_values = -1;	/* initialize attr_info */

  /* 
   *  Get the class representation so that we can access the indexes.
   */
  classrepr = heap_classrepr_get (thread_p, class_oid, NULL, NULL_REPRID, &classrepr_cacheindex);
  if (classrepr == NULL)
    {
      goto error;
    }

  /* 
   *  Get the index ID which corresponds to the BTID
   */
  index_id = heap_classrepr_find_index_id (classrepr, btid);
  if (index_id == -1)
    {
      goto error;
    }

  /* 
   *  Get the number of attributes associated with this index.
   *  Allocate a new attribute ID array if we have more attributes
   *  than will fit in the pre-allocated array.
   *  Fill the array with the attribute ID's
   *  Free the class representation.
   */
  num_found_attrs = classrepr->indexes[index_id].n_atts;
  if (num_found_attrs > HEAP_GUESS_NUM_INDEXED_ATTRS)
    {
      set_attrids = (ATTR_ID *) malloc (num_found_attrs * sizeof (ATTR_ID));
      if (set_attrids == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, num_found_attrs * sizeof (ATTR_ID));
	  goto error;
	}
    }

  for (i = 0; i < num_found_attrs; i++)
    {
      set_attrids[i] = classrepr->indexes[index_id].atts[i]->id;
    }

  (void) heap_classrepr_free (classrepr, &classrepr_cacheindex);
  classrepr = NULL;

  /* 
   *  Get the attribute information for the collected ID's
   */
  if (num_found_attrs > 0)
    {
      if (heap_attrinfo_start (thread_p, class_oid, num_found_attrs, set_attrids, attr_info) != NO_ERROR)
	{
	  goto error;
	}
    }

  /* 
   *  Free the attribute ID array if it was dynamically allocated
   */
  if (set_attrids != guess_attrids)
    {
      free_and_init (set_attrids);
    }

  return index_id;

  /* **** */
error:

  if (classrepr)
    {
      (void) heap_classrepr_free (classrepr, &classrepr_cacheindex);
    }

  if (set_attrids != guess_attrids)
    {
      free_and_init (set_attrids);
    }

  return ER_FAILED;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * heap_attrvalue_get_index () -
 *   return:
 *   value_index(in):
 *   attrid(in):
 *   n_btids(in):
 *   btids(in):
 *   idx_attrinfo(in):
 */
DB_VALUE *
heap_attrvalue_get_index (int value_index, ATTR_ID * attrid, int *n_btids, BTID ** btids,
			  HEAP_CACHE_ATTRINFO * idx_attrinfo)
{
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */

  /* check to make sure the idx_attrinfo has been used, it should never be empty. */
  if (idx_attrinfo->num_values == -1)
    {
      return NULL;
    }

  if (value_index > idx_attrinfo->num_values || value_index < 0)
    {
      *n_btids = 0;
      *btids = NULL;
      *attrid = NULL_ATTRID;
      return NULL;
    }
  else
    {
      value = &idx_attrinfo->values[value_index];
      *n_btids = value->last_attrepr->n_btids;
      *btids = value->last_attrepr->btids;
      *attrid = value->attrid;
      return &value->dbvalue;
    }

}
#endif

/*
 * heap_midxkey_key_get () -
 *   return:
 *   recdes(in):
 *   midxkey(in/out):
 *   index(in):
 *   attrinfo(in):
 *   func_domain(in):
 *   key_domain(out):
 */
static DB_MIDXKEY *
heap_midxkey_key_get (RECDES * recdes, DB_MIDXKEY * midxkey, OR_INDEX * index, HEAP_CACHE_ATTRINFO * attrinfo,
		      DB_VALUE * func_res, TP_DOMAIN * func_domain, TP_DOMAIN ** key_domain)
{
  char *nullmap_ptr;
  OR_ATTRIBUTE **atts;
  int num_atts, i, k;
  DB_VALUE value;
  OR_BUF buf;
  int error = NO_ERROR;
  TP_DOMAIN *set_domain = NULL;
  TP_DOMAIN *next_domain = NULL;

  num_atts = index->n_atts;
  atts = index->atts;
  if (func_res)
    {
      num_atts = index->func_index_info->attr_index_start + 1;
    }
  assert (PTR_ALIGN (midxkey->buf, INT_ALIGNMENT) == midxkey->buf);

  or_init (&buf, midxkey->buf, -1);

  nullmap_ptr = midxkey->buf;
  or_advance (&buf, pr_midxkey_init_boundbits (nullmap_ptr, num_atts));
  k = 0;
  for (i = 0; i < num_atts && k < num_atts; i++)
    {
      if (index->func_index_info && (i == index->func_index_info->col_id))
	{
	  assert (func_domain != NULL);

	  if (!db_value_is_null (func_res))
	    {
	      (*(func_domain->type->index_writeval)) (&buf, func_res);
	      OR_ENABLE_BOUND_BIT (nullmap_ptr, k);
	    }

	  if (key_domain != NULL)
	    {
	      if (k == 0)
		{
		  assert (set_domain == NULL);
		  set_domain = tp_domain_copy (func_domain, 0);
		  if (set_domain == NULL)
		    {
		      assert (false);
		      goto error;
		    }
		  next_domain = set_domain;
		}
	      else
		{
		  next_domain->next = tp_domain_copy (func_domain, 0);
		  if (next_domain->next == NULL)
		    {
		      assert (false);
		      goto error;
		    }
		  next_domain = next_domain->next;
		}
	    }

	  k++;
	}
      if (k == num_atts)
	{
	  break;
	}
      error = heap_midxkey_get_value (recdes, atts[i], &value, attrinfo);
      if (error == NO_ERROR && !db_value_is_null (&value))
	{
	  (*(atts[i]->domain->type->index_writeval)) (&buf, &value);
	  OR_ENABLE_BOUND_BIT (nullmap_ptr, k);
	}

      if (key_domain != NULL)
	{
	  if (k == 0)
	    {
	      assert (set_domain == NULL);
	      set_domain = tp_domain_copy (atts[i]->domain, 0);
	      if (set_domain == NULL)
		{
		  assert (false);
		  goto error;
		}
	      next_domain = set_domain;
	    }
	  else
	    {
	      next_domain->next = tp_domain_copy (atts[i]->domain, 0);
	      if (next_domain->next == NULL)
		{
		  assert (false);
		  goto error;
		}
	      next_domain = next_domain->next;
	    }
	}
      k++;
    }

  midxkey->size = CAST_BUFLEN (buf.ptr - buf.buffer);
  midxkey->ncolumns = num_atts;
  midxkey->domain = NULL;

  if (key_domain != NULL)
    {
      *key_domain = tp_domain_construct (DB_TYPE_MIDXKEY, (DB_OBJECT *) 0, num_atts, 0, set_domain);

      if (*key_domain)
	{
	  *key_domain = tp_domain_cache (*key_domain);
	}
      else
	{
	  assert (false);
	  goto error;
	}
    }

  return midxkey;

error:

  if (set_domain)
    {
      TP_DOMAIN *td, *next;

      for (td = set_domain, next = NULL; td != NULL; td = next)
	{
	  next = td->next;
	  tp_domain_free (td);
	}
    }

  return NULL;
}

/*
 * heap_midxkey_key_generate () -
 *   return:
 *   recdes(in):
 *   midxkey(in):
 *   att_ids(in):
 *   attrinfo(in):
 */
static DB_MIDXKEY *
heap_midxkey_key_generate (THREAD_ENTRY * thread_p, RECDES * recdes, DB_MIDXKEY * midxkey, int *att_ids,
			   HEAP_CACHE_ATTRINFO * attrinfo, DB_VALUE * func_res, int func_col_id,
			   int func_attr_index_start)
{
  char *nullmap_ptr;
  int num_vals, i, reprid, k;
  OR_ATTRIBUTE *att;
  DB_VALUE value;
  OR_BUF buf;
  int error = NO_ERROR;

  /* 
   * Make sure that we have the needed cached representation.
   */

  if (recdes != NULL)
    {
      reprid = or_rep_id (recdes);

      if (attrinfo->read_classrepr == NULL || attrinfo->read_classrepr->id != reprid)
	{
	  /* Get the needed representation */
	  if (heap_attrinfo_recache (thread_p, reprid, attrinfo) != NO_ERROR)
	    {
	      return NULL;
	    }
	}
    }

  assert (PTR_ALIGN (midxkey->buf, INT_ALIGNMENT) == midxkey->buf);

  or_init (&buf, midxkey->buf, -1);

  nullmap_ptr = midxkey->buf;

  /* On constructing index */
  num_vals = attrinfo->num_values;
  if (func_res)
    {
      num_vals = func_attr_index_start + 1;
    }
  or_advance (&buf, pr_midxkey_init_boundbits (nullmap_ptr, num_vals));
  k = 0;
  for (i = 0; i < num_vals && k < num_vals; i++)
    {
      if (i == func_col_id)
	{
	  if (!db_value_is_null (func_res))
	    {
	      TP_DOMAIN *domain = tp_domain_resolve_default (func_res->domain.general_info.type);
	      (*(domain->type->index_writeval)) (&buf, func_res);
	      OR_ENABLE_BOUND_BIT (nullmap_ptr, k);
	    }
	  k++;
	}
      if (k == num_vals)
	{
	  break;
	}
      att = heap_locate_attribute (att_ids[i], attrinfo);

      error = heap_midxkey_get_value (recdes, att, &value, attrinfo);
      if (error == NO_ERROR && !db_value_is_null (&value))
	{
	  (*(att->domain->type->index_writeval)) (&buf, &value);
	  OR_ENABLE_BOUND_BIT (nullmap_ptr, k);
	}
      k++;
    }

  midxkey->size = CAST_BUFLEN (buf.ptr - buf.buffer);
  midxkey->ncolumns = num_vals;
  midxkey->domain = NULL;
  midxkey->min_max_val.position = -1;
  midxkey->min_max_val.type = MIN_COLUMN;

  return midxkey;
}

/*
 * heap_attrinfo_generate_key () - Generate a key from the attribute information.
 *   return: Pointer to DB_VALUE containing the key.
 *   n_atts(in): Size of attribute ID array.
 *   att_ids(in): Array of attribute ID's
 *   attr_info(in): Pointer to attribute information structure.  This
 *                  structure contains the BTID's, the attributes and their
 *                  values.
 *   recdes(in):
 *   db_valuep(in): Pointer to a DB_VALUE.  This db_valuep will be used to
 *                  contain the set key in the case of multi-column B-trees.
 *                  It is ignored for single-column B-trees.
 *   buf(in):
 *
 * Note: Return a key for the specified attribute ID's
 *
 * If n_atts=1, the key will be the value of that attribute
 * and we will return a pointer to that DB_VALUE.
 *
 * If n_atts>1, the key will be a sequence of the attribute
 * values.  The set will be constructed and contained with
 * the passed DB_VALUE.  A pointer to this DB_VALUE is returned.
 *
 * It is important for the caller to deallocate this memory
 * by calling pr_clear_value().
 */
DB_VALUE *
heap_attrinfo_generate_key (THREAD_ENTRY * thread_p, int n_atts, int *att_ids, int *atts_prefix_length,
			    HEAP_CACHE_ATTRINFO * attr_info, RECDES * recdes, DB_VALUE * db_valuep, char *buf,
			    FUNCTION_INDEX_INFO * func_index_info)
{
  DB_VALUE *ret_valp;
  DB_VALUE *fi_res = NULL;
  int fi_attr_index_start = -1;
  int fi_col_id = -1;

  assert (DB_IS_NULL (db_valuep));

  if (func_index_info)
    {
      fi_attr_index_start = func_index_info->attr_index_start;
      fi_col_id = func_index_info->col_id;
      if (heap_eval_function_index (thread_p, func_index_info, n_atts, att_ids, attr_info, recdes, -1, db_valuep,
				    NULL, NULL) != NO_ERROR)
	{
	  return NULL;
	}
      fi_res = db_valuep;
    }

  /* 
   *  Multi-column index.  The key is a sequence of the attribute values.
   *  Return a pointer to the attributes DB_VALUE.
   */
  if ((n_atts > 1 && func_index_info == NULL) || (func_index_info && (func_index_info->attr_index_start + 1) > 1))
    {
      DB_MIDXKEY midxkey;
      int midxkey_size = recdes->length;

      if (func_index_info != NULL)
	{
	  /* this will allocate more than it is needed to store the key, but there is no decent way to calculate the
	   * correct size */
	  midxkey_size += OR_VALUE_ALIGNED_SIZE (fi_res);
	}

      /* Allocate storage for the buf of midxkey */
      if (midxkey_size > DBVAL_BUFSIZE)
	{
	  midxkey.buf = db_private_alloc (thread_p, midxkey_size);
	  if (midxkey.buf == NULL)
	    {
	      return NULL;
	    }
	}
      else
	{
	  midxkey.buf = buf;
	}

      if (heap_midxkey_key_generate (thread_p, recdes, &midxkey, att_ids, attr_info, fi_res, fi_col_id,
				     fi_attr_index_start) == NULL)
	{
	  return NULL;
	}

      (void) pr_clear_value (db_valuep);

      DB_MAKE_MIDXKEY (db_valuep, &midxkey);

      if (midxkey_size > DBVAL_BUFSIZE)
	{
	  db_valuep->need_clear = true;
	}

      ret_valp = db_valuep;
    }
  else
    {
      /* 
       *  Single-column index.  The key is simply the value of the attribute.
       *  Return a pointer to the attributes DB_VALUE.
       */
      if (func_index_info)
	{
	  ret_valp = db_valuep;
	  return ret_valp;
	}

      ret_valp = heap_attrinfo_access (att_ids[0], attr_info);
      if (ret_valp != NULL && atts_prefix_length && n_atts == 1)
	{
	  if (*atts_prefix_length != -1 && QSTR_IS_ANY_CHAR_OR_BIT (DB_VALUE_DOMAIN_TYPE (ret_valp)))
	    {
	      /* prefix index */
	      pr_clone_value (ret_valp, db_valuep);
	      db_string_truncate (db_valuep, *atts_prefix_length);
	      ret_valp = db_valuep;
	    }
	}
    }

  return ret_valp;
}

/*
 * heap_attrvalue_get_key () - Get B-tree key from attribute value(s)
 *   return: Pointer to DB_VALUE containing the key.
 *   btid_index(in): Index into an array of BTID's from the OR_CLASSREP
 *                   structure contained in idx_attrinfo.
 *   idx_attrinfo(in): Pointer to attribute information structure.  This
 *                     structure contains the BTID's, the attributes and their
 *                     values.
 *   recdes(in):
 *   btid(out): Pointer to a BTID.  The value of the current BTID
 *              will be returned.
 *   db_value(in): Pointer to a DB_VALUE.  This db_value will be used to
 *                 contain the set key in the case of multi-column B-trees.
 *                 It is ignored for single-column B-trees.
 *   buf(in):
 *   func_preds(in): cached function index expressions
 *   key_domain(out): domain of key
 *
 * Note: Return a B-tree key for the specified B-tree ID.
 *
 * If the specified B-tree ID is associated with a single
 * attribute the key will be the value of that attribute
 * and we will return a pointer to that DB_VALUE.
 *
 * If the BTID is associated with multiple attributes the
 * key will be a set containing the values of the attributes.
 * The set will be constructed and contained within the
 * passed DB_VALUE.  A pointer to this DB_VALUE is returned.
 * It is important for the caller to deallocate this memory
 * by calling pr_clear_value().
 */
DB_VALUE *
heap_attrvalue_get_key (THREAD_ENTRY * thread_p, int btid_index, HEAP_CACHE_ATTRINFO * idx_attrinfo, RECDES * recdes,
			BTID * btid, DB_VALUE * db_value, char *buf, FUNC_PRED_UNPACK_INFO * func_indx_pred,
			TP_DOMAIN ** key_domain)
{
  OR_INDEX *index;
  int n_atts, reprid;
  DB_VALUE *ret_val = NULL;
  DB_VALUE *fi_res = NULL;
  TP_DOMAIN *fi_domain = NULL;

  assert (DB_IS_NULL (db_value));

  /* 
   *  check to make sure the idx_attrinfo has been used, it should
   *  never be empty.
   */
  if ((idx_attrinfo->num_values == -1) || (btid_index >= idx_attrinfo->last_classrepr->n_indexes))
    {
      return NULL;
    }

  /* 
   * Make sure that we have the needed cached representation.
   */
  if (recdes != NULL)
    {
      reprid = or_rep_id (recdes);

      if (idx_attrinfo->read_classrepr == NULL || idx_attrinfo->read_classrepr->id != reprid)
	{
	  /* Get the needed representation */
	  if (heap_attrinfo_recache (thread_p, reprid, idx_attrinfo) != NO_ERROR)
	    {
	      return NULL;
	    }
	}
    }

  index = &(idx_attrinfo->last_classrepr->indexes[btid_index]);
  n_atts = index->n_atts;
  *btid = index->btid;

  /* is function index */
  if (index->func_index_info)
    {
      if (heap_eval_function_index (thread_p, NULL, -1, NULL, idx_attrinfo, recdes, btid_index, db_value,
				    func_indx_pred, &fi_domain) != NO_ERROR)
	{
	  return NULL;
	}
      fi_res = db_value;
    }

  /* 
   *  Multi-column index.  Construct the key as a sequence of attribute
   *  values.  The sequence is contained in the passed DB_VALUE.  A
   *  pointer to this DB_VALUE is returned.
   */
  if ((n_atts > 1 && recdes != NULL && index->func_index_info == NULL)
      || (index->func_index_info && (index->func_index_info->attr_index_start + 1) > 1))
    {
      DB_MIDXKEY midxkey;
      int midxkey_size = recdes->length;

      if (index->func_index_info != NULL)
	{
	  /* this will allocate more than it is needed to store the key, but there is no decent way to calculate the
	   * correct size */
	  midxkey_size += OR_VALUE_ALIGNED_SIZE (fi_res);
	}

      /* Allocate storage for the buf of midxkey */
      if (midxkey_size > DBVAL_BUFSIZE)
	{
	  midxkey.buf = db_private_alloc (thread_p, midxkey_size);
	  if (midxkey.buf == NULL)
	    {
	      return NULL;
	    }
	}
      else
	{
	  midxkey.buf = buf;
	}

      midxkey.min_max_val.position = -1;

      if (heap_midxkey_key_get (recdes, &midxkey, index, idx_attrinfo, fi_res, fi_domain, key_domain) == NULL)
	{
	  return NULL;
	}

      (void) pr_clear_value (db_value);

      DB_MAKE_MIDXKEY (db_value, &midxkey);

      if (midxkey_size > DBVAL_BUFSIZE)
	{
	  db_value->need_clear = true;
	}

      ret_val = db_value;
    }
  else
    {
      /* 
       *  Single-column index.  The key is simply the value of the attribute.
       *  Return a pointer to the attributes DB_VALUE.
       */

      PR_TYPE *pr_type = NULL;

      /* Find the matching attribute identified by the attribute ID */
      if (fi_res)
	{
	  ret_val = fi_res;
	  if (key_domain != NULL)
	    {
	      assert (fi_domain != NULL);
	      *key_domain = tp_domain_cache (fi_domain);
	    }
	  return ret_val;
	}
      ret_val = heap_attrinfo_access (index->atts[0]->id, idx_attrinfo);

      if (ret_val != NULL && index->attrs_prefix_length != NULL && index->attrs_prefix_length[0] != -1)
	{
	  if (QSTR_IS_ANY_CHAR_OR_BIT (DB_VALUE_DOMAIN_TYPE (ret_val)))
	    {
	      pr_clone_value (ret_val, db_value);
	      db_string_truncate (db_value, index->attrs_prefix_length[0]);
	      ret_val = db_value;
	    }
	}

      if (key_domain != NULL)
	{
	  if (index->attrs_prefix_length != NULL && index->attrs_prefix_length[0] != -1)
	    {
	      TP_DOMAIN *attr_dom;
	      TP_DOMAIN *prefix_dom;
	      DB_TYPE attr_type;

	      attr_type = TP_DOMAIN_TYPE (index->atts[0]->domain);

	      assert (QSTR_IS_ANY_CHAR_OR_BIT (attr_type));

	      attr_dom = index->atts[0]->domain;

	      prefix_dom =
		tp_domain_find_charbit (attr_type, TP_DOMAIN_CODESET (attr_dom), TP_DOMAIN_COLLATION (attr_dom),
					TP_DOMAIN_COLLATION_FLAG (attr_dom), attr_dom->precision, attr_dom->is_desc);

	      if (prefix_dom == NULL)
		{
		  prefix_dom = tp_domain_construct (attr_type, NULL, index->attrs_prefix_length[0], 0, NULL);
		  if (prefix_dom != NULL)
		    {
		      prefix_dom->codeset = TP_DOMAIN_CODESET (attr_dom);
		      prefix_dom->collation_id = TP_DOMAIN_COLLATION (attr_dom);
		      prefix_dom->collation_flag = TP_DOMAIN_COLLATION_FLAG (attr_dom);
		      prefix_dom->is_desc = attr_dom->is_desc;
		    }
		}

	      if (prefix_dom == NULL)
		{
		  return NULL;
		}
	      else
		{
		  *key_domain = tp_domain_cache (prefix_dom);
		}
	    }
	  else
	    {
	      *key_domain = tp_domain_cache (index->atts[0]->domain);
	    }
	}
    }

  return ret_val;
}

/*
 * heap_indexinfo_get_btid () -
 *   return:
 *   btid_index(in):
 *   attrinfo(in):
 */
BTID *
heap_indexinfo_get_btid (int btid_index, HEAP_CACHE_ATTRINFO * attrinfo)
{
  if (btid_index != -1 && btid_index < attrinfo->last_classrepr->n_indexes)
    {
      return &(attrinfo->last_classrepr->indexes[btid_index].btid);
    }
  else
    {
      return NULL;
    }
}

/*
 * heap_indexinfo_get_num_attrs () -
 *   return:
 *   btid_index(in):
 *   attrinfo(in):
 */
int
heap_indexinfo_get_num_attrs (int btid_index, HEAP_CACHE_ATTRINFO * attrinfo)
{
  if (btid_index != -1 && btid_index < attrinfo->last_classrepr->n_indexes)
    {
      return attrinfo->last_classrepr->indexes[btid_index].n_atts;
    }
  else
    {
      return 0;
    }
}

/*
 * heap_indexinfo_get_attrids () -
 *   return: NO_ERROR
 *   btid_index(in):
 *   attrinfo(in):
 *   attrids(in):
 */
int
heap_indexinfo_get_attrids (int btid_index, HEAP_CACHE_ATTRINFO * attrinfo, ATTR_ID * attrids)
{
  int i;
  int ret = NO_ERROR;

  if (btid_index != -1 && (btid_index < attrinfo->last_classrepr->n_indexes))
    {
      for (i = 0; i < attrinfo->last_classrepr->indexes[btid_index].n_atts; i++)
	{
	  attrids[i] = attrinfo->last_classrepr->indexes[btid_index].atts[i]->id;
	}
    }

  return ret;
}

/*
 * heap_indexinfo_get_attrs_prefix_length () -
 *   return: NO_ERROR
 *   btid_index(in):
 *   attrinfo(in):
 *   keys_prefix_length(in/out):
 */
int
heap_indexinfo_get_attrs_prefix_length (int btid_index, HEAP_CACHE_ATTRINFO * attrinfo, int *attrs_prefix_length,
					int len_attrs_prefix_length)
{
  int i, length = -1;
  int ret = NO_ERROR;

  if (attrs_prefix_length && len_attrs_prefix_length > 0)
    {
      for (i = 0; i < len_attrs_prefix_length; i++)
	{
	  attrs_prefix_length[i] = -1;
	}
    }

  if (btid_index != -1 && (btid_index < attrinfo->last_classrepr->n_indexes))
    {
      if (attrinfo->last_classrepr->indexes[btid_index].attrs_prefix_length && attrs_prefix_length)
	{
	  length = MIN (attrinfo->last_classrepr->indexes[btid_index].n_atts, len_attrs_prefix_length);
	  for (i = 0; i < length; i++)
	    {
	      attrs_prefix_length[i] = attrinfo->last_classrepr->indexes[btid_index].attrs_prefix_length[i];
	    }
	}
    }

  return ret;
}

/*
 * heap_get_index_with_name () - get BTID of index with name index_name
 * return : error code or NO_ERROR
 * thread_p (in) :
 * class_oid (in) : class OID
 * index_name (in): index name
 * btid (in/out)  : btid
 */
int
heap_get_index_with_name (THREAD_ENTRY * thread_p, OID * class_oid, const char *index_name, BTID * btid)
{
  OR_CLASSREP *classrep = NULL;
  OR_INDEX *indexp = NULL;
  int idx_in_cache, i;
  int error = NO_ERROR;

  BTID_SET_NULL (btid);

  /* get the class representation so that we can access the indexes */
  classrep = heap_classrepr_get (thread_p, class_oid, NULL, NULL_REPRID, &idx_in_cache);
  if (classrep == NULL)
    {
      return ER_FAILED;
    }

  for (i = 0; i < classrep->n_indexes; i++)
    {
      if (strcasecmp (classrep->indexes[i].btname, index_name) == 0)
	{
	  BTID_COPY (btid, &classrep->indexes[i].btid);
	  break;
	}
    }
  if (classrep != NULL)
    {
      error = heap_classrepr_free (classrep, &idx_in_cache);
    }
  return error;
}

/*
 * heap_get_indexinfo_of_btid () -
 *   return: NO_ERROR
 *   class_oid(in):
 *   btid(in):
 *   type(in):
 *   num_attrs(in):
 *   attr_ids(in):
 *   btnamepp(in);
 */
int
heap_get_indexinfo_of_btid (THREAD_ENTRY * thread_p, OID * class_oid, BTID * btid, BTREE_TYPE * type, int *num_attrs,
			    ATTR_ID ** attr_ids, int **attrs_prefix_length, char **btnamepp, int *func_index_col_id)
{
  OR_CLASSREP *classrepp;
  OR_INDEX *indexp;
  int idx_in_cache, i, n = 0;
  int idx;
  int ret = NO_ERROR;

  /* initial value of output parameters */
  if (num_attrs)
    {
      *num_attrs = 0;
    }

  if (attr_ids)
    {
      *attr_ids = NULL;
    }

  if (btnamepp)
    {
      *btnamepp = NULL;
    }

  if (attrs_prefix_length)
    {
      *attrs_prefix_length = NULL;
    }

  if (func_index_col_id)
    {
      *func_index_col_id = -1;
    }

  /* get the class representation so that we can access the indexes */
  classrepp = heap_classrepr_get (thread_p, class_oid, NULL, NULL_REPRID, &idx_in_cache);
  if (classrepp == NULL)
    {
      goto exit_on_error;
    }

  /* get the idx of the index which corresponds to the BTID */
  idx = heap_classrepr_find_index_id (classrepp, btid);
  if (idx < 0)
    {
      goto exit_on_error;
    }
  indexp = &classrepp->indexes[idx];

  /* get the type of this index */
  if (type)
    {
      *type = indexp->type;
    }

  /* get the number of attributes associated with this index */
  if (num_attrs)
    {
      *num_attrs = n = indexp->n_atts;
    }
  /* allocate a new attribute ID array */
  if (attr_ids)
    {
      *attr_ids = (ATTR_ID *) db_private_alloc (thread_p, n * sizeof (ATTR_ID));

      if (*attr_ids == NULL)
	{
	  goto exit_on_error;
	}

      /* fill the array with the attribute ID's */
      for (i = 0; i < n; i++)
	{
	  (*attr_ids)[i] = indexp->atts[i]->id;
	}
    }

  if (btnamepp)
    {
      *btnamepp = strdup (indexp->btname);
    }

  if (attrs_prefix_length && indexp->type == BTREE_INDEX)
    {
      *attrs_prefix_length = (int *) db_private_alloc (thread_p, n * sizeof (int));

      if (*attrs_prefix_length == NULL)
	{
	  goto exit_on_error;
	}

      for (i = 0; i < n; i++)
	{
	  if (indexp->attrs_prefix_length != NULL)
	    {
	      (*attrs_prefix_length)[i] = indexp->attrs_prefix_length[i];
	    }
	  else
	    {
	      (*attrs_prefix_length)[i] = -1;
	    }
	}
    }

  if (func_index_col_id && indexp->func_index_info)
    {
      *func_index_col_id = indexp->func_index_info->col_id;
    }

  /* free the class representation */
  ret = heap_classrepr_free (classrepp, &idx_in_cache);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }

  return ret;

exit_on_error:

  if (attr_ids && *attr_ids)
    {
      db_private_free_and_init (thread_p, *attr_ids);
    }

  if (btnamepp && *btnamepp)
    {
      free_and_init (*btnamepp);
    }

  if (attrs_prefix_length)
    {
      if (*attrs_prefix_length)
	{
	  db_private_free_and_init (thread_p, *attrs_prefix_length);
	}
      *attrs_prefix_length = NULL;
    }

  if (classrepp)
    {
      (void) heap_classrepr_free (classrepp, &idx_in_cache);
    }

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_get_referenced_by () - Find objects referenced by given object
 *   return: int (object count or -1)
 *   class_oid(in):
 *   obj_oid(in): The object identifier
 *   recdes(in): Object disk representation
 *   max_oid_cnt(in/out): Size of OID list in OIDs
 *   oid_list(in): Set to the array of referenced OIDs
 *                 (This area can be realloc, thus, it should have been
 *                 with malloc)
 *
 * Note: This function finds object identifiers referenced by the
 * given instance. If OID references are stored in the given
 * OID list. If the oid_list is not large enough to hold the
 * number of instances, the area (i.e., oid_list) is expanded
 * using realloc. The number of OID references is returned by the
 * function.
 *
 * Note: The oid_list pointer should be freed by the caller.
 * Note: Nested-sets, that is, set-of-sets inside the object are not traced.
 * Note: This function does not remove duplicate oids from the list, the
 * caller is responsible for checking and removing them if needed.
 */
int
heap_get_referenced_by (THREAD_ENTRY * thread_p, OID * class_oid, const OID * obj_oid, RECDES * recdes,
			int *max_oid_cnt, OID ** oid_list)
{
  HEAP_CACHE_ATTRINFO attr_info;
  DB_TYPE dbtype;
  HEAP_ATTRVALUE *value;	/* Disk value Attr info for a particular attr */
  DB_VALUE db_value;
  DB_SET *set;
  OID *oid_ptr;			/* iterator on oid_list */
  OID *attr_oid;
  int oid_cnt;			/* number of OIDs fetched */
  int cnt;			/* set element count */
  int new_max_oid;
  int i, j;			/* loop counters */

  /* 
   * We don't support class references in this function
   */
  if (oid_is_root (class_oid))
    {
      return 0;
    }

  if ((heap_attrinfo_start_refoids (thread_p, class_oid, &attr_info) != NO_ERROR)
      || heap_attrinfo_read_dbvalues (thread_p, obj_oid, recdes, NULL, &attr_info) != NO_ERROR)
    {
      goto error;
    }

  if (*oid_list == NULL)
    {
      *max_oid_cnt = 0;
    }
  else if (*max_oid_cnt <= 0)
    {
      /* 
       * We better release oid_list since we do not know it size. This may
       * be a bug.
       */
      free_and_init (*oid_list);
      *max_oid_cnt = 0;
    }

  /* 
   * Now start searching the attributes that may reference objects
   */
  oid_cnt = 0;
  oid_ptr = *oid_list;

  for (i = 0; i < attr_info.num_values; i++)
    {
      value = &attr_info.values[i];
      dbtype = db_value_type (&value->dbvalue);
      if (dbtype == DB_TYPE_OID && !db_value_is_null (&value->dbvalue)
	  && (attr_oid = db_get_oid (&value->dbvalue)) != NULL && !OID_ISNULL (attr_oid))
	{
	  /* 
	   * A simple attribute with reference an object (OID)
	   */
	  if (oid_cnt == *max_oid_cnt)
	    {
	      /* 
	       * We need to expand the area to deposit more OIDs.
	       * Use 50% of the current size for expansion and at least 10 OIDs
	       */
	      if (*max_oid_cnt <= 0)
		{
		  *max_oid_cnt = 0;
		  new_max_oid = attr_info.num_values;
		}
	      else
		{
		  new_max_oid = (int) (*max_oid_cnt * 1.5) + 1;
		  if (new_max_oid < attr_info.num_values)
		    {
		      new_max_oid = attr_info.num_values;
		    }
		}

	      if (new_max_oid < 10)
		{
		  new_max_oid = 10;
		}

	      oid_ptr = (OID *) realloc (*oid_list, new_max_oid * sizeof (OID));
	      if (oid_ptr == NULL)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, new_max_oid * sizeof (OID));
		  goto error;
		}

	      /* 
	       * Set the pointers and advance to current area pointer
	       */
	      *oid_list = oid_ptr;
	      oid_ptr += *max_oid_cnt;
	      *max_oid_cnt = new_max_oid;
	    }
	  *oid_ptr = *attr_oid;
	  oid_ptr++;
	  oid_cnt++;
	}
      else
	{
	  if (TP_IS_SET_TYPE (dbtype))
	    {
	      /* 
	       * A set which may or may nor reference objects (OIDs)
	       * Go through each element of the set
	       */

	      set = db_get_set (&value->dbvalue);
	      cnt = db_set_size (set);

	      for (j = 0; j < cnt; j++)
		{
		  if (db_set_get (set, j, &db_value) != NO_ERROR)
		    {
		      goto error;
		    }

		  dbtype = db_value_type (&db_value);
		  if (dbtype == DB_TYPE_OID && !db_value_is_null (&db_value)
		      && (attr_oid = db_get_oid (&db_value)) != NULL && !OID_ISNULL (attr_oid))
		    {
		      if (oid_cnt == *max_oid_cnt)
			{
			  /* 
			   * We need to expand the area to deposit more OIDs.
			   * Use 50% of the current size for expansion.
			   */
			  if (*max_oid_cnt <= 0)
			    {
			      *max_oid_cnt = 0;
			      new_max_oid = attr_info.num_values;
			    }
			  else
			    {
			      new_max_oid = (int) (*max_oid_cnt * 1.5) + 1;
			      if (new_max_oid < attr_info.num_values)
				{
				  new_max_oid = attr_info.num_values;
				}
			    }
			  if (new_max_oid < 10)
			    {
			      new_max_oid = 10;
			    }

			  oid_ptr = (OID *) realloc (*oid_list, new_max_oid * sizeof (OID));
			  if (oid_ptr == NULL)
			    {
			      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
				      new_max_oid * sizeof (OID));
			      goto error;
			    }

			  /* 
			   * Set the pointers and advance to current area pointer
			   */
			  *oid_list = oid_ptr;
			  oid_ptr += *max_oid_cnt;
			  *max_oid_cnt = new_max_oid;
			}
		      *oid_ptr = *attr_oid;
		      oid_ptr++;
		      oid_cnt++;
		    }
		}
	    }
	}
    }

  /* free object area if no OIDs were encountered */
  if (oid_cnt == 0)
    /* 
     * Unless we check whether *oid_list is NULL,
     * it may cause double-free of oid_list.
     */
    if (*oid_list != NULL)
      {
	free_and_init (*oid_list);
      }

  heap_attrinfo_end (thread_p, &attr_info);

  /* return number of OIDs fetched */
  return oid_cnt;

error:
  /* XXXXXXX */

  free_and_init (*oid_list);
  *max_oid_cnt = 0;
  heap_attrinfo_end (thread_p, &attr_info);

  return ER_FAILED;
}

/*
 * heap_prefetch () - Prefetch objects
 *   return: NO_ERROR
 *           fetch_area is set to point to fetching area
 *   class_oid(in): Class identifier for the instance oid
 *   oid(in): Object that must be fetched if its cached state is invalid
 *   prefetch(in): Prefetch structure
 *
 */
int
heap_prefetch (THREAD_ENTRY * thread_p, OID * class_oid, const OID * oid, LC_COPYAREA_DESC * prefetch)
{
  VPID vpid;
  PAGE_PTR pgptr = NULL;
  int round_length;
  INT16 right_slotid, left_slotid;
  HEAP_DIRECTION direction;
  SCAN_CODE scan;
  int ret = NO_ERROR;

  /* 
   * Prefetch other instances (i.e., neighbors) stored on the same page
   * of the given object OID. Relocated instances and instances in overflow are
   * not prefetched, nor instances that do not belong to the given class.
   * Prefetching stop once an error, such as out of space, is encountered.
   */

  vpid.volid = oid->volid;
  vpid.pageid = oid->pageid;

  pgptr = heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, S_LOCK, NULL, NULL);
  if (pgptr == NULL)
    {
      assert (er_errid () != NO_ERROR);
      ret = er_errid ();
      if (ret == ER_PB_BAD_PAGEID)
	{
	  ret = ER_HEAP_UNKNOWN_OBJECT;
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 3, oid->volid, oid->pageid, oid->slotid);
	}

      /* 
       * Problems getting the page.. forget about prefetching...
       */
      return ret;
    }

  right_slotid = oid->slotid;
  left_slotid = oid->slotid;
  direction = HEAP_DIRECTION_BOTH;

  while (direction != HEAP_DIRECTION_NONE)
    {
      /* 
       * Don't include the desired object again, forwarded instances, nor
       * instances that belong to other classes
       */

      /* Check to the right */
      if (direction == HEAP_DIRECTION_RIGHT || direction == HEAP_DIRECTION_BOTH)
	{
	  scan = spage_next_record (pgptr, &right_slotid, prefetch->recdes, COPY);
	  if (scan == S_SUCCESS && spage_get_record_type (pgptr, right_slotid) == REC_HOME)
	    {
	      prefetch->mobjs->num_objs++;
	      COPY_OID (&((*prefetch->obj)->class_oid), class_oid);
	      (*prefetch->obj)->oid.volid = oid->volid;
	      (*prefetch->obj)->oid.pageid = oid->pageid;
	      (*prefetch->obj)->oid.slotid = right_slotid;
	      OID_SET_NULL (&(*prefetch->obj)->updated_oid);
	      (*prefetch->obj)->length = prefetch->recdes->length;
	      (*prefetch->obj)->offset = *prefetch->offset;
	      (*prefetch->obj)->operation = LC_FETCH;
	      (*prefetch->obj) = LC_NEXT_ONEOBJ_PTR_IN_COPYAREA (*prefetch->obj);
	      round_length = DB_ALIGN (prefetch->recdes->length, HEAP_MAX_ALIGN);
	      *prefetch->offset += round_length;
	      prefetch->recdes->data += round_length;
	      prefetch->recdes->area_size -= (round_length + sizeof (*(*prefetch->obj)));
	    }
	  else if (scan != S_SUCCESS)
	    {
	      /* Stop prefetching objects from the right */
	      direction = ((direction == HEAP_DIRECTION_BOTH) ? HEAP_DIRECTION_LEFT : HEAP_DIRECTION_NONE);
	    }
	}

      /* Check to the left */
      if (direction == HEAP_DIRECTION_LEFT || direction == HEAP_DIRECTION_BOTH)
	{
	  scan = spage_previous_record (pgptr, &left_slotid, prefetch->recdes, COPY);
	  if (scan == S_SUCCESS && left_slotid != HEAP_HEADER_AND_CHAIN_SLOTID
	      && spage_get_record_type (pgptr, left_slotid) == REC_HOME)
	    {
	      prefetch->mobjs->num_objs++;
	      COPY_OID (&((*prefetch->obj)->class_oid), class_oid);
	      (*prefetch->obj)->oid.volid = oid->volid;
	      (*prefetch->obj)->oid.pageid = oid->pageid;
	      (*prefetch->obj)->oid.slotid = left_slotid;
	      OID_SET_NULL (&(*prefetch->obj)->updated_oid);
	      (*prefetch->obj)->length = prefetch->recdes->length;
	      (*prefetch->obj)->offset = *prefetch->offset;
	      (*prefetch->obj)->operation = LC_FETCH;
	      (*prefetch->obj) = LC_NEXT_ONEOBJ_PTR_IN_COPYAREA (*prefetch->obj);
	      round_length = DB_ALIGN (prefetch->recdes->length, HEAP_MAX_ALIGN);
	      *prefetch->offset += round_length;
	      prefetch->recdes->data += round_length;
	      prefetch->recdes->area_size -= (round_length + sizeof (*(*prefetch->obj)));
	    }
	  else if (scan != S_SUCCESS)
	    {
	      /* Stop prefetching objects from the right */
	      direction = ((direction == HEAP_DIRECTION_BOTH) ? HEAP_DIRECTION_RIGHT : HEAP_DIRECTION_NONE);
	    }
	}
    }

  pgbuf_unfix_and_init (thread_p, pgptr);

  return ret;
}

static DISK_ISVALID
heap_check_all_pages_by_heapchain (THREAD_ENTRY * thread_p, HFID * hfid, HEAP_CHKALL_RELOCOIDS * chk_objs,
				   INT32 * num_checked)
{
  VPID vpid;
  VPID *vpidptr_ofpgptr;
  INT32 npages = 0;
  DISK_ISVALID valid_pg = DISK_VALID;
  bool spg_error = false;
  PGBUF_WATCHER pg_watcher;
  PGBUF_WATCHER old_pg_watcher;

  PGBUF_INIT_WATCHER (&pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  PGBUF_INIT_WATCHER (&old_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);

  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;

  while (!VPID_ISNULL (&vpid) && valid_pg == DISK_VALID)
    {
      npages++;

      valid_pg = file_isvalid_page_partof (thread_p, &vpid, &hfid->vfid);
      if (valid_pg != DISK_VALID)
	{
	  break;
	}

      pg_watcher.pgptr =
	heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE_PREVENT_DEALLOC, S_LOCK, NULL, &pg_watcher);
      if (old_pg_watcher.pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
	}
      if (pg_watcher.pgptr == NULL)
	{
	  /* something went wrong, return */
	  valid_pg = DISK_ERROR;
	  break;
	}
#ifdef SPAGE_DEBUG
      if (spage_check (thread_p, pg_watcher.pgptr) != NO_ERROR)
	{
	  /* if spage has an error, try to go on. but, this page is corrupted. */
	  spg_error = true;
	}
#endif

      if (heap_vpid_next (hfid, pg_watcher.pgptr, &vpid) != NO_ERROR)
	{
	  pgbuf_ordered_unfix (thread_p, &pg_watcher);
	  /* something went wrong, return */
	  valid_pg = DISK_ERROR;
	  break;
	}

      vpidptr_ofpgptr = pgbuf_get_vpid_ptr (pg_watcher.pgptr);
      if (VPID_EQ (&vpid, vpidptr_ofpgptr))
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_CYCLE, 5, vpid.volid, vpid.pageid, hfid->vfid.volid,
		  hfid->vfid.fileid, hfid->hpgid);
	  VPID_SET_NULL (&vpid);
	  valid_pg = DISK_ERROR;
	}

      if (chk_objs != NULL)
	{
	  valid_pg = heap_chkreloc_next (thread_p, chk_objs, pg_watcher.pgptr);
	}

      pgbuf_replace_watcher (thread_p, &pg_watcher, &old_pg_watcher);
    }

  if (old_pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
    }
  assert (pg_watcher.pgptr == NULL);

  *num_checked = npages;
  return (spg_error == true) ? DISK_ERROR : valid_pg;
}

static DISK_ISVALID
heap_check_all_pages_by_allocset (THREAD_ENTRY * thread_p, HFID * hfid, HEAP_CHKALL_RELOCOIDS * chk_objs,
				  int *num_checked)
{
  INT32 checked = 0;
  PAGE_PTR pgptr;
  VPID vpid;
  DISK_ISVALID valid_pg = DISK_VALID;
  FILE_ALLOC_ITERATOR iter;
  FILE_ALLOC_ITERATOR *iter_p = &iter;

  /* create the iterator of heap pages */
  if (file_alloc_iterator_init (thread_p, &hfid->vfid, iter_p) == NULL)
    {
      /* heap header page should be exist */
      return DISK_ERROR;
    }

  while (iter_p)
    {
      checked++;

      if (file_alloc_iterator_get_current_page (thread_p, &iter, &vpid) == NULL)
	{
	  valid_pg = DISK_ERROR;
	  break;
	}

      assert (!VPID_ISNULL (&vpid));

      if (file_isvalid_page_partof (thread_p, &vpid, &hfid->vfid) != DISK_VALID)
	{
	  valid_pg = DISK_ERROR;
	  iter_p = file_alloc_iterator_next (thread_p, iter_p);
	  continue;
	}

      pgptr = heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, S_LOCK, NULL, NULL);
      if (pgptr == NULL)
	{
	  valid_pg = DISK_ERROR;
	  iter_p = file_alloc_iterator_next (thread_p, iter_p);
	  continue;
	}

#ifdef SPAGE_DEBUG
      if (spage_check (thread_p, pgptr) != NO_ERROR)
	{
	  valid_pg = DISK_ERROR;
	}
#endif

      (void) pgbuf_check_page_ptype (thread_p, pgptr, PAGE_HEAP);

      if (chk_objs != NULL)
	{
	  valid_pg = heap_chkreloc_next (thread_p, chk_objs, pgptr);
	}

      pgbuf_unfix_and_init (thread_p, pgptr);

      iter_p = file_alloc_iterator_next (thread_p, iter_p);
    }

  *num_checked = checked;

  return valid_pg;
}

/*
 * heap_check_all_pages () - Validate all pages known by given heap vs file manger
 *   return: DISK_INVALID, DISK_VALID, DISK_ERROR
 *   hfid(in): : Heap identifier
 *
 * Note: Verify that all pages known by the given heap are valid. That
 * is, that they are valid from the point of view of the file manager.
 */
DISK_ISVALID
heap_check_all_pages (THREAD_ENTRY * thread_p, HFID * hfid)
{
  VPID vpid;			/* Page-volume identifier */
  PAGE_PTR pgptr = NULL;	/* Page pointer */
  HEAP_HDR_STATS *heap_hdr;	/* Header of heap structure */
  RECDES hdr_recdes;		/* Header record descriptor */
  DISK_ISVALID valid_pg = DISK_VALID;
  DISK_ISVALID valid = DISK_VALID;
  DISK_ISVALID tmp_valid_pg = DISK_VALID;
  INT32 npages = 0, tmp_npages;
  int i;
  int file_numpages;
  HEAP_CHKALL_RELOCOIDS chk;
  HEAP_CHKALL_RELOCOIDS *chk_objs = &chk;


  valid_pg = heap_chkreloc_start (chk_objs);
  if (valid_pg != DISK_VALID)
    {
      chk_objs = NULL;
    }
  else
    {
      chk_objs->verify_not_vacuumed = true;
    }

  /* Scan every page of the heap to find out if they are valid */
  valid_pg = heap_check_all_pages_by_heapchain (thread_p, hfid, chk_objs, &npages);

  file_numpages = file_get_numpages (thread_p, &hfid->vfid);

  if (file_numpages != -1 && file_numpages != npages)
    {
      if (chk_objs != NULL)
	{
	  chk_objs->verify = false;
	  (void) heap_chkreloc_end (chk_objs);

	  tmp_valid_pg = heap_chkreloc_start (chk_objs);
	}

      /* 
       * Scan every page of the heap using allocset.
       * This is for getting more information of the corrupted pages.
       */
      tmp_valid_pg = heap_check_all_pages_by_allocset (thread_p, hfid, chk_objs, &tmp_npages);

      if (chk_objs != NULL)
	{
	  if (tmp_valid_pg == DISK_VALID)
	    {
	      tmp_valid_pg = heap_chkreloc_end (chk_objs);
	    }
	  else
	    {
	      chk_objs->verify = false;
	      (void) heap_chkreloc_end (chk_objs);
	    }
	}
    }
  else
    {
      if (chk_objs != NULL)
	{
	  valid_pg = heap_chkreloc_end (chk_objs);
	}
    }

  if (valid_pg == DISK_VALID)
    {
      if (npages != file_numpages)
	{
	  if (file_numpages == -1)
	    {
	      valid_pg = DISK_ERROR;
	    }
	  else
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_MISMATCH_NPAGES, 5, hfid->vfid.volid, hfid->vfid.fileid,
		      hfid->hpgid, npages, file_numpages);
	      valid_pg = DISK_INVALID;
	    }
	}

      /* 
       * Check the statistics entries in the header
       */

      /* Fetch the header page of the heap file */
      vpid.volid = hfid->vfid.volid;
      vpid.pageid = hfid->hpgid;

      pgptr = heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, S_LOCK, NULL, NULL);
      if (pgptr == NULL)
	{
	  return DISK_ERROR;
	}

      (void) pgbuf_check_page_ptype (thread_p, pgptr, PAGE_HEAP);

      if (spage_get_record (pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes, PEEK) != S_SUCCESS)
	{
	  /* Unable to peek heap header record */
	  pgbuf_unfix_and_init (thread_p, pgptr);

	  return DISK_ERROR;
	}

      heap_hdr = (HEAP_HDR_STATS *) hdr_recdes.data;
      for (i = 0; i < HEAP_NUM_BEST_SPACESTATS && valid_pg != DISK_ERROR; i++)
	{
	  if (!VPID_ISNULL (&heap_hdr->estimates.best[i].vpid))
	    {
	      valid = file_isvalid_page_partof (thread_p, &heap_hdr->estimates.best[i].vpid, &hfid->vfid);
	      if (valid != DISK_VALID)
		{
		  valid_pg = valid;
		  break;
		}
	    }
	}

#if defined(SA_MODE)
      if (prm_get_integer_value (PRM_ID_HF_MAX_BESTSPACE_ENTRIES) > 0)
	{
	  HEAP_STATS_ENTRY *ent;
	  void *last;
	  int rc;

	  rc = pthread_mutex_lock (&heap_Bestspace->bestspace_mutex);

	  last = NULL;
	  while ((ent = (HEAP_STATS_ENTRY *) mht_get2 (heap_Bestspace->hfid_ht, hfid, &last)) != NULL)
	    {
	      assert_release (!VPID_ISNULL (&ent->best.vpid));
	      if (!VPID_ISNULL (&ent->best.vpid))
		{
		  valid_pg = file_isvalid_page_partof (thread_p, &ent->best.vpid, &hfid->vfid);
		  if (valid_pg != DISK_VALID)
		    {
		      break;
		    }
		}
	      assert_release (ent->best.freespace > 0);
	    }

	  assert (mht_count (heap_Bestspace->vpid_ht) == mht_count (heap_Bestspace->hfid_ht));

	  pthread_mutex_unlock (&heap_Bestspace->bestspace_mutex);
	}
#endif

      pgbuf_unfix_and_init (thread_p, pgptr);

      /* Need to check for the overflow pages.... */
    }

  return valid_pg;
}

DISK_ISVALID
heap_check_heap_file (THREAD_ENTRY * thread_p, HFID * hfid)
{
  FILE_TYPE file_type;
  VPID vpid;
  FILE_HEAP_DES hfdes;
  DISK_ISVALID rv = DISK_VALID;

  file_type = file_get_type (thread_p, &hfid->vfid);
  if (file_type == FILE_UNKNOWN_TYPE || (file_type != FILE_HEAP && file_type != FILE_HEAP_REUSE_SLOTS))
    {
      return DISK_ERROR;
    }

  if (heap_get_header_page (thread_p, hfid, &vpid) != NULL)
    {
      hfid->hpgid = vpid.pageid;

      if ((file_get_descriptor (thread_p, &hfid->vfid, &hfdes, sizeof (FILE_HEAP_DES)) > 0)
	  && !OID_ISNULL (&hfdes.class_oid))
	{
	  assert (lock_has_lock_on_object (&hfdes.class_oid, oid_Root_class_oid, LOG_FIND_THREAD_TRAN_INDEX (thread_p),
					   IS_LOCK) == 1);
	  rv = heap_check_all_pages (thread_p, hfid);
	}
    }
  else
    {
      return DISK_ERROR;
    }

  return rv;
}

/*
 * heap_check_all_heaps () - Validate all pages of all known heap files
 *   return: DISK_INVALID, DISK_VALID, DISK_ERROR
 *
 * Note: Verify that all pages of all heap files are valid. That is,
 * that they are valid from the point of view of the file manager.
 */
DISK_ISVALID
heap_check_all_heaps (THREAD_ENTRY * thread_p)
{
  int error_code;
  HFID hfid;
  int num_files;
  int curr_num_files;
  DISK_ISVALID allvalid = DISK_VALID;
  DISK_ISVALID valid = DISK_VALID;
  FILE_TYPE file_type;
  VPID vpid;
  VFID *trk_vfid = NULL;
  FILE_HEAP_DES hfdes;
  PAGE_PTR trk_fhdr_pgptr = NULL;
  HEAP_SCANCACHE scan_cache;
  RECDES peek_recdes;
  int i;

  trk_vfid = file_get_tracker_vfid ();

  /* check file tracker */
  if (trk_vfid == NULL)
    {
      goto exit_on_error;
    }

  vpid.volid = trk_vfid->volid;
  vpid.pageid = trk_vfid->fileid;

  /* Find number of files */
  num_files = file_get_numfiles (thread_p);
  if (num_files < 0)
    {
      goto exit_on_error;
    }

  /* Go to each file, check only the heap files */
  for (i = 0; i < num_files && allvalid != DISK_ERROR; i++)
    {
      /* check # of file, check file type, get file descriptor */
      trk_fhdr_pgptr = pgbuf_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
      if (trk_fhdr_pgptr == NULL)
	{
	  goto exit_on_error;
	}

      curr_num_files = file_get_numfiles (thread_p);

      if (curr_num_files <= i)
	{
	  pgbuf_unfix_and_init (thread_p, trk_fhdr_pgptr);
	  break;
	}

      if (file_find_nthfile (thread_p, &hfid.vfid, i) != 1)
	{
	  goto exit_on_error;
	}

      file_type = file_get_type (thread_p, &hfid.vfid);
      if (file_type == FILE_UNKNOWN_TYPE)
	{
	  goto exit_on_error;
	}

      if (file_type != FILE_HEAP && file_type != FILE_HEAP_REUSE_SLOTS)
	{
	  pgbuf_unfix_and_init (thread_p, trk_fhdr_pgptr);
	  continue;
	}

      if ((file_get_descriptor (thread_p, &hfid.vfid, &hfdes, sizeof (FILE_HEAP_DES)) <= 0)
	  || OID_ISNULL (&hfdes.class_oid))
	{
	  pgbuf_unfix_and_init (thread_p, trk_fhdr_pgptr);
	  continue;
	}
      pgbuf_unfix_and_init (thread_p, trk_fhdr_pgptr);

      if (lock_object (thread_p, &hfdes.class_oid, oid_Root_class_oid, IS_LOCK, LK_UNCOND_LOCK) != LK_GRANTED)
	{
	  goto exit_on_error;
	}

      error_code = heap_scancache_quick_start_root_hfid (thread_p, &scan_cache);
      if (error_code != NO_ERROR)
	{
	  lock_unlock_object (thread_p, &hfdes.class_oid, oid_Root_class_oid, IS_LOCK, true);
	  goto exit_on_error;
	}

      /* Check heap file is really exist. It can be removed */
      if (heap_get (thread_p, &hfdes.class_oid, &peek_recdes, &scan_cache, PEEK, NULL_CHN) != S_SUCCESS)
	{
	  heap_scancache_end (thread_p, &scan_cache);
	  lock_unlock_object (thread_p, &hfdes.class_oid, oid_Root_class_oid, IS_LOCK, true);
	  continue;
	}
      heap_scancache_end (thread_p, &scan_cache);

      valid = heap_check_heap_file (thread_p, &hfid);
      if (valid != DISK_VALID)
	{
	  allvalid = valid;
	}

      lock_unlock_object (thread_p, &hfdes.class_oid, oid_Root_class_oid, IS_LOCK, true);
    }
  assert (trk_fhdr_pgptr == NULL);

  return allvalid;

exit_on_error:
  if (trk_fhdr_pgptr != NULL)
    {
      pgbuf_unfix_and_init (thread_p, trk_fhdr_pgptr);
    }

  return ((allvalid == DISK_VALID) ? DISK_ERROR : allvalid);
}

/*
 * heap_dump_hdr () - Dump heap file header
 *   return: NO_ERROR
 *   heap_hdr(in): Header structure
 */
static int
heap_dump_hdr (FILE * fp, HEAP_HDR_STATS * heap_hdr)
{
  int i, j;
  int avg_length;
  int ret = NO_ERROR;

  avg_length = ((heap_hdr->estimates.num_recs > 0)
		? (int) ((heap_hdr->estimates.recs_sumlen / (float) heap_hdr->estimates.num_recs) + 0.9) : 0);

  fprintf (fp, "CLASS_OID = %2d|%4d|%2d, ", heap_hdr->class_oid.volid, heap_hdr->class_oid.pageid,
	   heap_hdr->class_oid.slotid);
  fprintf (fp, "OVF_VFID = %4d|%4d, NEXT_VPID = %4d|%4d\n", heap_hdr->ovf_vfid.volid, heap_hdr->ovf_vfid.fileid,
	   heap_hdr->next_vpid.volid, heap_hdr->next_vpid.pageid);
  fprintf (fp, "unfill_space = %4d\n", heap_hdr->unfill_space);
  fprintf (fp, "Estimated: num_pages = %d, num_recs = %d,  avg reclength = %d\n", heap_hdr->estimates.num_pages,
	   heap_hdr->estimates.num_recs, avg_length);
  fprintf (fp, "Estimated: num high best = %d, num others(not in array) high best = %d\n",
	   heap_hdr->estimates.num_high_best, heap_hdr->estimates.num_other_high_best);
  fprintf (fp, "Hint of best set of vpids with head = %d\n", heap_hdr->estimates.head);

  for (j = 0, i = 0; i < HEAP_NUM_BEST_SPACESTATS; j++, i++)
    {
      if (j != 0 && j % 5 == 0)
	{
	  fprintf (fp, "\n");
	}
      fprintf (fp, "%4d|%4d %4d,", heap_hdr->estimates.best[i].vpid.volid, heap_hdr->estimates.best[i].vpid.pageid,
	       heap_hdr->estimates.best[i].freespace);
    }
  fprintf (fp, "\n");

  fprintf (fp, "Second best: num hints = %d, head of hints = %d, tail (next to insert) of hints = %d, num subs = %d\n",
	   heap_hdr->estimates.num_second_best, heap_hdr->estimates.head_second_best,
	   heap_hdr->estimates.tail_second_best, heap_hdr->estimates.num_substitutions);
  for (j = 0, i = 0; i < HEAP_NUM_BEST_SPACESTATS; j++, i++)
    {
      if (j != 0 && j % 5 == 0)
	{
	  fprintf (fp, "\n");
	}
      fprintf (fp, "%4d|%4d,", heap_hdr->estimates.second_best[i].volid, heap_hdr->estimates.second_best[i].pageid);
    }
  fprintf (fp, "\n");

  fprintf (fp, "Last vpid = %4d|%4d\n", heap_hdr->estimates.last_vpid.volid, heap_hdr->estimates.last_vpid.pageid);

  fprintf (fp, "Next full search vpid = %4d|%4d\n", heap_hdr->estimates.full_search_vpid.volid,
	   heap_hdr->estimates.full_search_vpid.pageid);

  return ret;
}

/*
 * heap_dump () - Dump heap file
 *   return:
 *   hfid(in): Heap file identifier
 *   dump_records(in): If true, objects are printed in ascii format, otherwise, the
 *              objects are not printed.
 *
 * Note: Dump a heap file. The objects are printed only when the value
 * of dump_records is true. This function is used for DEBUGGING PURPOSES.
 */
void
heap_dump (THREAD_ENTRY * thread_p, FILE * fp, HFID * hfid, bool dump_records)
{
  VPID vpid;			/* Page-volume identifier */
  HEAP_HDR_STATS *heap_hdr;	/* Header of heap structure */
  RECDES hdr_recdes;		/* Header record descriptor */
  VFID ovf_vfid;
  OID oid;
  HEAP_SCANCACHE scan_cache;
  HEAP_CACHE_ATTRINFO attr_info;
  RECDES peek_recdes;
  FILE_HEAP_DES hfdes;
  int ret = NO_ERROR;
  PGBUF_WATCHER pg_watcher;
  PGBUF_WATCHER old_pg_watcher;

  PGBUF_INIT_WATCHER (&pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  PGBUF_INIT_WATCHER (&old_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);

  fprintf (fp, "\n\n*** DUMPING HEAP FILE: ");
  fprintf (fp, "volid = %d, Fileid = %d, Header-pageid = %d ***\n", hfid->vfid.volid, hfid->vfid.fileid, hfid->hpgid);
  (void) file_dump_descriptor (thread_p, fp, &hfid->vfid);

  /* Fetch the header page of the heap file */

  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;
  pg_watcher.pgptr = heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, S_LOCK, NULL, &pg_watcher);
  if (pg_watcher.pgptr == NULL)
    {
      /* Unable to fetch heap header page */
      return;
    }

  /* Peek the header record to dump the estadistics */

  if (spage_get_record (pg_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes, PEEK) != S_SUCCESS)
    {
      /* Unable to peek heap header record */
      pgbuf_ordered_unfix (thread_p, &pg_watcher);
      return;
    }

  heap_hdr = (HEAP_HDR_STATS *) hdr_recdes.data;
  ret = heap_dump_hdr (fp, heap_hdr);
  if (ret != NO_ERROR)
    {
      pgbuf_ordered_unfix (thread_p, &pg_watcher);
      return;
    }

  VFID_COPY (&ovf_vfid, &heap_hdr->ovf_vfid);
  pgbuf_ordered_unfix (thread_p, &pg_watcher);

  /* now scan every page and dump it */
  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;
  while (!VPID_ISNULL (&vpid))
    {
      pg_watcher.pgptr =
	heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE_PREVENT_DEALLOC, S_LOCK, NULL, &pg_watcher);
      if (old_pg_watcher.pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
	}
      if (pg_watcher.pgptr == NULL)
	{
	  /* something went wrong, return */
	  return;
	}
      spage_dump (thread_p, fp, pg_watcher.pgptr, dump_records);
      (void) heap_vpid_next (hfid, pg_watcher.pgptr, &vpid);
      pgbuf_replace_watcher (thread_p, &pg_watcher, &old_pg_watcher);
    }

  if (old_pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
    }
  assert (pg_watcher.pgptr == NULL);

  /* Dump file table configuration */
  if (file_dump (thread_p, fp, &hfid->vfid) != NO_ERROR)
    {
      return;
    }

  if (!VFID_ISNULL (&ovf_vfid))
    {
      /* There is an overflow file for this heap file */
      fprintf (fp, "\nOVERFLOW FILE INFORMATION FOR HEAP FILE\n\n");
      if (file_dump (thread_p, fp, &ovf_vfid) != NO_ERROR)
	{
	  return;
	}
    }

  /* 
   * Dump schema definition
   */

  if (file_get_descriptor (thread_p, &hfid->vfid, &hfdes, sizeof (FILE_HEAP_DES)) > 0 && !OID_ISNULL (&hfdes.class_oid))
    {

      if (heap_attrinfo_start (thread_p, &hfdes.class_oid, -1, NULL, &attr_info) != NO_ERROR)
	{
	  return;
	}

      ret = heap_classrepr_dump (thread_p, fp, &hfdes.class_oid, attr_info.last_classrepr);
      if (ret != NO_ERROR)
	{
	  heap_attrinfo_end (thread_p, &attr_info);
	  return;
	}

      /* Dump individual Objects */
      if (dump_records == true)
	{
	  if (heap_scancache_start (thread_p, &scan_cache, hfid, NULL, true, false, NULL) != NO_ERROR)
	    {
	      /* something went wrong, return */
	      heap_attrinfo_end (thread_p, &attr_info);
	      return;
	    }

	  OID_SET_NULL (&oid);
	  oid.volid = hfid->vfid.volid;

	  while (heap_next (thread_p, hfid, NULL, &oid, &peek_recdes, &scan_cache, PEEK) == S_SUCCESS)
	    {
	      fprintf (fp, "Object-OID = %2d|%4d|%2d,\n  Length on disk = %d,\n", oid.volid, oid.pageid, oid.slotid,
		       peek_recdes.length);

	      if (heap_attrinfo_read_dbvalues (thread_p, &oid, &peek_recdes, NULL, &attr_info) != NO_ERROR)
		{
		  fprintf (fp, "  Error ... continue\n");
		  continue;
		}
	      heap_attrinfo_dump (thread_p, fp, &attr_info, false);
	    }
	  heap_scancache_end (thread_p, &scan_cache);
	}
      heap_attrinfo_end (thread_p, &attr_info);
    }

  fprintf (fp, "\n\n*** END OF DUMP FOR HEAP FILE ***\n\n");
}

/*
 * heap_dump_all () - Dump all heap files
 *   return:
 *   dump_records(in): If true, objects are printed in ascii format, otherwise, the
 *              objects are not printed.
 */
void
heap_dump_all (THREAD_ENTRY * thread_p, FILE * fp, bool dump_records)
{
  int num_files;
  HFID hfid;
  VPID vpid;
  int i;

  /* Find number of files */
  num_files = file_get_numfiles (thread_p);
  if (num_files <= 0)
    {
      return;
    }

  /* Dump each heap file */
  for (i = 0; i < num_files; i++)
    {
      FILE_TYPE file_type = FILE_UNKNOWN_TYPE;

      if (file_find_nthfile (thread_p, &hfid.vfid, i) != 1)
	{
	  break;
	}

      file_type = file_get_type (thread_p, &hfid.vfid);
      if (file_type != FILE_HEAP && file_type != FILE_HEAP_REUSE_SLOTS)
	{
	  continue;
	}

      if (heap_get_header_page (thread_p, &hfid, &vpid) != NULL)
	{
	  hfid.hpgid = vpid.pageid;
	  heap_dump (thread_p, fp, &hfid, dump_records);
	}
    }
}

/*
 * heap_dump_all_capacities () - Dump the capacities of all heap files.
 *   return:
 */
void
heap_dump_all_capacities (THREAD_ENTRY * thread_p, FILE * fp)
{
  HFID hfid;
  VPID vpid;
  int i;
  int num_files = 0;
  INT64 num_recs = 0;
  INT64 num_recs_relocated = 0;
  INT64 num_recs_inovf = 0;
  INT64 num_pages = 0;
  int avg_freespace = 0;
  int avg_freespace_nolast = 0;
  int avg_reclength = 0;
  int avg_overhead = 0;
  FILE_HEAP_DES hfdes;
  HEAP_CACHE_ATTRINFO attr_info;

  /* Find number of files */
  num_files = file_get_numfiles (thread_p);
  if (num_files <= 0)
    {
      return;
    }

  fprintf (fp, "IO_PAGESIZE = %d, DB_PAGESIZE = %d, Recv_overhead = %d\n", IO_PAGESIZE, DB_PAGESIZE,
	   IO_PAGESIZE - DB_PAGESIZE);

  /* Go to each file, check only the heap files */
  for (i = 0; i < num_files; i++)
    {
      FILE_TYPE file_type = FILE_UNKNOWN_TYPE;

      if (file_find_nthfile (thread_p, &hfid.vfid, i) != 1)
	{
	  break;
	}

      file_type = file_get_type (thread_p, &hfid.vfid);
      if (file_type != FILE_HEAP && file_type != FILE_HEAP_REUSE_SLOTS)
	{
	  continue;
	}

      if (heap_get_header_page (thread_p, &hfid, &vpid) != NULL)
	{
	  hfid.hpgid = vpid.pageid;
	  if (heap_get_capacity (thread_p, &hfid, &num_recs, &num_recs_relocated, &num_recs_inovf, &num_pages,
				 &avg_freespace, &avg_freespace_nolast, &avg_reclength, &avg_overhead) == NO_ERROR)
	    {
	      fprintf (fp,
		       "HFID:%d|%d|%d, Num_recs = %" PRId64 ", Num_reloc_recs = %" PRId64 ",\n    Num_recs_inovf = %"
		       PRId64 ", Avg_reclength = %d,\n    Num_pages = %" PRId64 ", Avg_free_space_per_page = %d,\n"
		       "    Avg_free_space_per_page_without_lastpage = %d\n    Avg_overhead_per_page = %d\n",
		       (int) hfid.vfid.volid, hfid.vfid.fileid, hfid.hpgid, num_recs, num_recs_relocated,
		       num_recs_inovf, avg_reclength, num_pages, avg_freespace, avg_freespace_nolast, avg_overhead);
	      /* 
	       * Dump schema definition
	       */
	      if (file_get_descriptor (thread_p, &hfid.vfid, &hfdes, sizeof (FILE_HEAP_DES)) > 0
		  && !OID_ISNULL (&hfdes.class_oid)
		  && heap_attrinfo_start (thread_p, &hfdes.class_oid, -1, NULL, &attr_info) == NO_ERROR)
		{
		  (void) heap_classrepr_dump (thread_p, fp, &hfdes.class_oid, attr_info.last_classrepr);
		  heap_attrinfo_end (thread_p, &attr_info);
		}
	      fprintf (fp, "\n");
	    }
	}
    }
}

/*
 * heap_estimate_num_pages_needed () - Guess the number of pages needed to store a
 *                                set of instances
 *   return: int
 *   total_nobjs(in): Number of object to insert
 *   avg_obj_size(in): Average size of object
 *   num_attrs(in): Number of attributes
 *   num_var_attrs(in): Number of variable attributes
 *
 */
INT32
heap_estimate_num_pages_needed (THREAD_ENTRY * thread_p, int total_nobjs, int avg_obj_size, int num_attrs,
				int num_var_attrs)
{
  int nobj_page;
  INT32 npages;


  avg_obj_size += (4 + 8) /* MVCC constant */ ;

  if (num_attrs > 0)
    {
      avg_obj_size += CEIL_PTVDIV (num_attrs, 32) * sizeof (int);
    }
  if (num_var_attrs > 0)
    {
      avg_obj_size += (num_var_attrs + 1) * sizeof (int);
      /* Assume max padding of 3 bytes... */
      avg_obj_size += num_var_attrs * (sizeof (int) - 1);
    }

  avg_obj_size = DB_ALIGN (avg_obj_size, HEAP_MAX_ALIGN);

  /* 
   * Find size of page available to store objects:
   * USER_SPACE_IN_PAGES = (DB_PAGESIZE * (1 - unfill_factor)
   *                        - SLOTTED PAGE HDR size overhead
   *                        - link of pages(i.e., sizeof(chain))
   *                        - slot overhead to store the link chain)
   */

  nobj_page =
    ((int) (DB_PAGESIZE * (1 - prm_get_float_value (PRM_ID_HF_UNFILL_FACTOR))) - spage_header_size ()
     - sizeof (HEAP_CHAIN) - spage_slot_size ());
  /* 
   * Find the number of objects per page
   */

  nobj_page = nobj_page / (avg_obj_size + spage_slot_size ());

  /* 
   * Find the number of pages. Add one page for file manager overhead
   */

  if (nobj_page > 0)
    {
      npages = CEIL_PTVDIV (total_nobjs, nobj_page);
      npages += file_guess_numpages_overhead (thread_p, NULL, npages);
    }
  else
    {
      /* 
       * Overflow insertion
       */
      npages = overflow_estimate_npages_needed (thread_p, total_nobjs, avg_obj_size);

      /* 
       * Find number of pages for the indirect record references (OIDs) to
       * overflow records
       */
      nobj_page =
	((int) (DB_PAGESIZE * (1 - prm_get_float_value (PRM_ID_HF_UNFILL_FACTOR))) - spage_header_size () -
	 sizeof (HEAP_CHAIN) - spage_slot_size ());
      nobj_page = nobj_page / (sizeof (OID) + spage_slot_size ());
      /* 
       * Now calculate the number of pages
       */
      nobj_page += CEIL_PTVDIV (total_nobjs, nobj_page);
      nobj_page += file_guess_numpages_overhead (thread_p, NULL, nobj_page);
      /* 
       * Add the number of overflow pages and non-heap pages
       */
      npages = npages + nobj_page;
    }

  return npages;
}

/*
 *     	Check consistency of heap from the point of view of relocation
 */

/*
 * heap_chkreloc_start () - Start validating consistency of relocated objects in
 *                        heap
 *   return: DISK_VALID, DISK_INVALID, DISK_ERROR
 *   chk(in): Structure for checking relocation objects
 *
 */
static DISK_ISVALID
heap_chkreloc_start (HEAP_CHKALL_RELOCOIDS * chk)
{
  chk->ht =
    mht_create ("Validate Relocation entries hash table", HEAP_CHK_ADD_UNFOUND_RELOCOIDS, oid_hash, oid_compare_equals);
  if (chk->ht == NULL)
    {
      chk->ht = NULL;
      chk->unfound_reloc_oids = NULL;
      chk->max_unfound_reloc = -1;
      chk->num_unfound_reloc = -1;
      return DISK_ERROR;
    }

  chk->unfound_reloc_oids = (OID *) malloc (sizeof (*chk->unfound_reloc_oids) * HEAP_CHK_ADD_UNFOUND_RELOCOIDS);
  if (chk->unfound_reloc_oids == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
	      sizeof (*chk->unfound_reloc_oids) * HEAP_CHK_ADD_UNFOUND_RELOCOIDS);

      if (chk->ht != NULL)
	{
	  mht_destroy (chk->ht);
	}

      chk->ht = NULL;
      chk->unfound_reloc_oids = NULL;
      chk->max_unfound_reloc = -1;
      chk->num_unfound_reloc = -1;
      return DISK_ERROR;
    }

  chk->max_unfound_reloc = HEAP_CHK_ADD_UNFOUND_RELOCOIDS;
  chk->num_unfound_reloc = 0;
  chk->verify = true;
  chk->verify_not_vacuumed = false;
  chk->not_vacuumed_res = DISK_VALID;

  return DISK_VALID;
}

/*
 * heap_chkreloc_end () - Finish validating consistency of relocated objects
 *                      in heap
 *   return: DISK_VALID, DISK_INVALID, DISK_ERROR
 *   chk(in): Structure for checking relocation objects
 *
 * Note: Scanning the unfound_reloc_oid list, remove those entries that
 * are also found in hash table (remove them from unfound_reloc
 * list and from hash table). At the end of the scan, if there
 * are any entries in either hash table or unfound_reloc_oid, the
 * heap is incosistent/corrupted.
 */
static DISK_ISVALID
heap_chkreloc_end (HEAP_CHKALL_RELOCOIDS * chk)
{
  HEAP_CHK_RELOCOID *forward;
  DISK_ISVALID valid_reloc = DISK_VALID;
  int i;

  if (chk->not_vacuumed_res != DISK_VALID)
    {
      valid_reloc = chk->not_vacuumed_res;
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_FOUND_NOT_VACUUMED, 0);
    }

  /* 
   * Check for any postponed unfound relocated OIDs that have not been
   * checked or found. If they are not in the hash table, it would be an
   * error. That is, we would have a relocated (content) object without an
   * object pointing to it. (relocation/home).
   */
  if (chk->verify == true)
    {
      for (i = 0; i < chk->num_unfound_reloc; i++)
	{
	  forward = (HEAP_CHK_RELOCOID *) mht_get (chk->ht, &chk->unfound_reloc_oids[i]);
	  if (forward != NULL)
	    {
	      /* 
	       * The entry was found.
	       * Remove the entry and the memory space
	       */
	      /* mht_rem() has been updated to take a function and an arg pointer that can be called on the entry
	       * before it is removed.  We may want to take advantage of that here to free the memory associated with
	       * the entry */
	      if (mht_rem (chk->ht, &chk->unfound_reloc_oids[i], NULL, NULL) != NO_ERROR)
		{
		  valid_reloc = DISK_ERROR;
		}
	      else
		{
		  free_and_init (forward);
		}
	    }
	  else
	    {
	      er_log_debug (ARG_FILE_LINE, "Unable to find relocation/home object for relocated_oid=%d|%d|%d\n",
			    (int) chk->unfound_reloc_oids[i].volid, chk->unfound_reloc_oids[i].pageid,
			    (int) chk->unfound_reloc_oids[i].slotid);
#if defined (SA_MODE)
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	      valid_reloc = DISK_INVALID;
#endif /* SA_MODE */
	    }
	}
    }

  /* 
   * If there are entries in the hash table, it would be problems. That is,
   * the relocated (content) objects were not found. That is, the home object
   * points to a dangling content object, or what it points is not a
   * relocated (newhome) object.
   */

  if (mht_count (chk->ht) > 0)
    {
      (void) mht_map (chk->ht, heap_chkreloc_print_notfound, chk);
#if defined (SA_MODE)
      valid_reloc = DISK_INVALID;
#endif /* !SA_MODE */
    }

  mht_destroy (chk->ht);
  free_and_init (chk->unfound_reloc_oids);

  return valid_reloc;
}

/*
 * heap_chkreloc_print_notfound () - Print entry that does not have a relocated entry
 *   return: NO_ERROR
 *   ignore_reloc_oid(in): Key (relocated entry to real entry) of hash table
 *   ent(in): The entry associated with key (real oid)
 *   xchk(in): Structure for checking relocation objects
 *
 * Note: Print unfound relocated record information for this home
 * record with relocation address HEAP is inconsistent.
 */
static int
heap_chkreloc_print_notfound (const void *ignore_reloc_oid, void *ent, void *xchk)
{
  HEAP_CHK_RELOCOID *forward = (HEAP_CHK_RELOCOID *) ent;
  HEAP_CHKALL_RELOCOIDS *chk = (HEAP_CHKALL_RELOCOIDS *) xchk;

  if (chk->verify == true)
    {
      er_log_debug (ARG_FILE_LINE,
		    "Unable to find relocated record with oid=%d|%d|%d for home object with oid=%d|%d|%d\n",
		    (int) forward->reloc_oid.volid, forward->reloc_oid.pageid, (int) forward->reloc_oid.slotid,
		    (int) forward->real_oid.volid, forward->real_oid.pageid, (int) forward->real_oid.slotid);
#if defined (SA_MODE)
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
#endif /* SA_MODE */
    }
  /* mht_rem() has been updated to take a function and an arg pointer that can be called on the entry before it is
   * removed.  We may want to take advantage of that here to free the memory associated with the entry */
  (void) mht_rem (chk->ht, &forward->reloc_oid, NULL, NULL);
  free_and_init (forward);

  return NO_ERROR;
}

/*
 * heap_chkreloc_next () - Verify consistency of relocation records on page heap
 *   return: DISK_VALID, DISK_INVALID, DISK_ERROR
 *   thread_p(in) : thread context
 *   chk(in): Structure for checking relocation objects
 *   pgptr(in): Page pointer
 *
 * Note: While scanning objects of given page:
 *              1: if a relocation record is found, we check if that record
 *                 has already been seen (i.e., if it is in unfound_relc
 *                 list),
 *                 if it has been seen, we remove the entry from the
 *                 unfound_relc_oid list.
 *                 if it has not been seen, we add an entry to hash table
 *                 from reloc_oid to real_oid
 *                 Note: for optimization reasons, we may not scan the
 *                 unfound_reloc if it is too long, in this case the entry is
 *                 added to hash table.
 *              2: if a newhome (relocated) record is found, we check if the
 *                 real record has already been seen (i.e., check hash table),
 *                 if it has been seen, we remove the entry from hash table
 *                 otherwise, we add an entry into the unfound_reloc list
 */

#define HEAP_CHKRELOC_UNFOUND_SHORT 5

static DISK_ISVALID
heap_chkreloc_next (THREAD_ENTRY * thread_p, HEAP_CHKALL_RELOCOIDS * chk, PAGE_PTR pgptr)
{
  HEAP_CHK_RELOCOID *forward;
  INT16 type = REC_UNKNOWN;
  RECDES recdes;
  OID oid, class_oid;
  OID *peek_oid;
  void *ptr;
  bool found;
  int i;

  if (chk->verify != true)
    {
      return DISK_VALID;
    }

  if (chk->verify_not_vacuumed && heap_get_class_oid_from_page (thread_p, pgptr, &class_oid) != NO_ERROR)
    {
      chk->not_vacuumed_res = DISK_ERROR;
      return DISK_ERROR;
    }

  oid.volid = pgbuf_get_volume_id (pgptr);
  oid.pageid = pgbuf_get_page_id (pgptr);
  oid.slotid = 0;		/* i.e., will get slot 1 */

  while (spage_next_record (pgptr, &oid.slotid, &recdes, PEEK) == S_SUCCESS)
    {
      if (oid.slotid == HEAP_HEADER_AND_CHAIN_SLOTID)
	{
	  continue;
	}
      type = spage_get_record_type (pgptr, oid.slotid);

      switch (type)
	{
	case REC_RELOCATION:
	  /* 
	   * The record stored on the page is a relocation record,
	   * get the new home for the record
	   *
	   * If we have already entries waiting to be check and the list is
	   * not that big, check them. Otherwise, wait until the end for the
	   * check since searching the list may be expensive
	   */
	  peek_oid = (OID *) recdes.data;
	  found = false;
	  if (chk->num_unfound_reloc < HEAP_CHKRELOC_UNFOUND_SHORT)
	    {
	      /* 
	       * Go a head and check since the list is very short.
	       */
	      for (i = 0; i < chk->num_unfound_reloc; i++)
		{
		  if (OID_EQ (&chk->unfound_reloc_oids[i], peek_oid))
		    {
		      /* 
		       * Remove it from the unfound list
		       */
		      if ((i + 1) != chk->num_unfound_reloc)
			{
			  chk->unfound_reloc_oids[i] = chk->unfound_reloc_oids[chk->num_unfound_reloc - 1];
			}
		      chk->num_unfound_reloc--;
		      found = true;
		      break;
		    }
		}
	    }
	  if (found == false)
	    {
	      /* 
	       * Add it to hash table
	       */
	      forward = (HEAP_CHK_RELOCOID *) malloc (sizeof (HEAP_CHK_RELOCOID));
	      if (forward == NULL)
		{
		  /* 
		   * Out of memory
		   */
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, sizeof (HEAP_CHK_RELOCOID));

		  return DISK_ERROR;
		}
	      forward->real_oid = oid;
	      forward->reloc_oid = *peek_oid;
	      if (mht_put (chk->ht, &forward->reloc_oid, forward) == NULL)
		{
		  /* 
		   * Failure in mht_put
		   */
		  return DISK_ERROR;
		}
	    }
	  break;

	case REC_BIGONE:
	  if (chk->verify_not_vacuumed)
	    {
	      MVCC_REC_HEADER rec_header;
	      PAGE_PTR overflow_page;
	      DISK_ISVALID tmp_valid;
	      VPID overflow_vpid;
	      OID *overflow_oid;

	      /* get overflow page id */
	      overflow_oid = (OID *) recdes.data;
	      overflow_vpid.volid = overflow_oid->volid;
	      overflow_vpid.pageid = overflow_oid->pageid;
	      if (VPID_ISNULL (&overflow_vpid))
		{
		  chk->not_vacuumed_res = DISK_ERROR;
		  return DISK_ERROR;
		}

	      /* fix page and get record */
	      overflow_page =
		pgbuf_fix (thread_p, &overflow_vpid, OLD_PAGE, PGBUF_LATCH_READ, PGBUF_UNCONDITIONAL_LATCH);
	      if (overflow_page == NULL)
		{
		  chk->not_vacuumed_res = DISK_ERROR;
		  return DISK_ERROR;
		}
	      if (heap_get_mvcc_rec_header_from_overflow (overflow_page, &rec_header, &recdes) != NO_ERROR)
		{
		  pgbuf_unfix_and_init (thread_p, overflow_page);
		  chk->not_vacuumed_res = DISK_ERROR;
		  return DISK_ERROR;
		}
	      pgbuf_unfix_and_init (thread_p, overflow_page);

	      /* check header */
	      tmp_valid = vacuum_check_not_vacuumed_rec_header (thread_p, &oid, &class_oid, &rec_header, -1);
	      switch (tmp_valid)
		{
		case DISK_VALID:
		  break;
		case DISK_INVALID:
		  chk->not_vacuumed_res = DISK_INVALID;
		  break;
		case DISK_ERROR:
		default:
		  chk->not_vacuumed_res = DISK_ERROR;
		  return DISK_ERROR;
		  break;
		}
	    }
	  break;

	case REC_HOME:
	  if (chk->verify_not_vacuumed)
	    {
	      DISK_ISVALID tmp_valid = vacuum_check_not_vacuumed_recdes (thread_p, &oid, &class_oid,
									 &recdes, -1);
	      switch (tmp_valid)
		{
		case DISK_VALID:
		  break;
		case DISK_INVALID:
		  chk->not_vacuumed_res = DISK_INVALID;
		  break;
		case DISK_ERROR:
		default:
		  chk->not_vacuumed_res = DISK_ERROR;
		  return DISK_ERROR;
		  break;
		}
	    }
	  break;

	case REC_NEWHOME:
	  if (chk->verify_not_vacuumed)
	    {
	      DISK_ISVALID tmp_valid = vacuum_check_not_vacuumed_recdes (thread_p, &oid, &class_oid,
									 &recdes, -1);
	      switch (tmp_valid)
		{
		case DISK_VALID:
		  break;
		case DISK_INVALID:
		  chk->not_vacuumed_res = DISK_INVALID;
		  break;
		case DISK_ERROR:
		default:
		  chk->not_vacuumed_res = DISK_ERROR;
		  return DISK_ERROR;
		  break;
		}
	    }

	  /* 
	   * Remove the object from hash table or insert the object in unfound
	   * reloc check list.
	   */
	  forward = (HEAP_CHK_RELOCOID *) mht_get (chk->ht, &oid);
	  if (forward != NULL)
	    {
	      /* 
	       * The entry was found.
	       * Remove the entry and the memory space
	       */
	      /* mht_rem() has been updated to take a function and an arg pointer that can be called on the entry
	       * before it is removed.  We may want to take advantage of that here to free the memory associated with
	       * the entry */
	      (void) mht_rem (chk->ht, &forward->reloc_oid, NULL, NULL);
	      free_and_init (forward);
	    }
	  else
	    {
	      /* 
	       * The entry is not in hash table.
	       * Add entry into unfound_reloc list
	       */
	      if (chk->max_unfound_reloc <= chk->num_unfound_reloc)
		{
		  /* 
		   * Need to realloc the area. Add 100 OIDs to it
		   */
		  i = (sizeof (*chk->unfound_reloc_oids) * (chk->max_unfound_reloc + HEAP_CHK_ADD_UNFOUND_RELOCOIDS));

		  ptr = realloc (chk->unfound_reloc_oids, i);
		  if (ptr == NULL)
		    {
		      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, (size_t) i);
		      return DISK_ERROR;
		    }
		  else
		    {
		      chk->unfound_reloc_oids = (OID *) ptr;
		      chk->max_unfound_reloc += HEAP_CHK_ADD_UNFOUND_RELOCOIDS;
		    }
		}
	      i = chk->num_unfound_reloc++;
	      chk->unfound_reloc_oids[i] = oid;
	    }
	  break;

	case REC_MARKDELETED:
	case REC_DELETED_WILL_REUSE:
	case REC_MVCC_NEXT_VERSION:
	default:
	  break;
	}
    }

  return DISK_VALID;
}

/*
 * Chn guesses for class objects at clients
 */

/*
 * Note: Currently, we do not try to guess chn of instances at clients.
 *       We are just doing it for classes.
 *
 * We do not know if the object is cached on the client side at all, we
 * are just guessing that it is still cached if it was sent to it. This is
 * almost 100% true since classes are avoided during garbage collection.

 * Caller does not know the chn when the client is fetching instances of the
 * class without knowning the class_oid. That does not imply that the
 * class object is not cached on the workspace. The client just did not
 * know the class_oid of the given fetched object. The server finds it and
 * has to decide whether or not to sent the class object. If the server does
 * not send the class object, and the client does not have it; the client will
 * request the class object (another server call)
 */

/*
 * heap_chnguess_initialize () - Initalize structure of chn guesses at clients
 *   return: NO_ERROR
 *
 * Note: Initialize structures used to cache information of CHN guess
 * at client workspaces.
 * Note: We current maintain that information only for classes.
 */
static int
heap_chnguess_initialize (void)
{
  HEAP_CHNGUESS_ENTRY *entry;
  int i;
  int ret = NO_ERROR;

  if (heap_Guesschn != NULL)
    {
      ret = heap_chnguess_finalize ();
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }

  heap_Guesschn_area.schema_change = false;
  heap_Guesschn_area.clock_hand = -1;
  heap_Guesschn_area.num_entries = HEAP_CLASSREPR_MAXCACHE;

  /* 
   * Start with at least the fude factor of clients. Make sure that every
   * bit is used.
   */
  heap_Guesschn_area.num_clients = logtb_get_number_of_total_tran_indices ();
  if (heap_Guesschn_area.num_clients < HEAP_CHNGUESS_FUDGE_MININDICES)
    {
      heap_Guesschn_area.num_clients = HEAP_CHNGUESS_FUDGE_MININDICES;
    }

  /* Make sure every single bit is used */
  heap_Guesschn_area.nbytes = HEAP_NBITS_TO_NBYTES (heap_Guesschn_area.num_clients);
  heap_Guesschn_area.num_clients = HEAP_NBYTES_TO_NBITS (heap_Guesschn_area.nbytes);

  /* Build the hash table from OID to CHN */
  heap_Guesschn_area.ht =
    mht_create ("Memory hash OID to chn at clients", HEAP_CLASSREPR_MAXCACHE, oid_hash, oid_compare_equals);
  if (heap_Guesschn_area.ht == NULL)
    {
      goto exit_on_error;
    }

  heap_Guesschn_area.entries =
    (HEAP_CHNGUESS_ENTRY *) malloc (sizeof (HEAP_CHNGUESS_ENTRY) * heap_Guesschn_area.num_entries);
  if (heap_Guesschn_area.entries == NULL)
    {
      ret = ER_OUT_OF_VIRTUAL_MEMORY;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1, sizeof (HEAP_CHNGUESS_ENTRY) * heap_Guesschn_area.num_entries);
      mht_destroy (heap_Guesschn_area.ht);
      goto exit_on_error;
    }

  heap_Guesschn_area.bitindex = (unsigned char *) malloc (heap_Guesschn_area.nbytes * heap_Guesschn_area.num_entries);
  if (heap_Guesschn_area.bitindex == NULL)
    {
      ret = ER_OUT_OF_VIRTUAL_MEMORY;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1,
	      (size_t) (heap_Guesschn_area.nbytes * heap_Guesschn_area.num_entries));
      mht_destroy (heap_Guesschn_area.ht);
      free_and_init (heap_Guesschn_area.entries);
      goto exit_on_error;
    }

  /* 
   * Initialize every entry as not recently freed
   */
  for (i = 0; i < heap_Guesschn_area.num_entries; i++)
    {
      entry = &heap_Guesschn_area.entries[i];
      entry->idx = i;
      entry->chn = NULL_CHN;
      entry->recently_accessed = false;
      OID_SET_NULL (&entry->oid);
      entry->bits = &heap_Guesschn_area.bitindex[i * heap_Guesschn_area.nbytes];
      HEAP_NBYTES_CLEARED (entry->bits, heap_Guesschn_area.nbytes);
    }
  heap_Guesschn = &heap_Guesschn_area;

  return ret;

exit_on_error:

  return (ret == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_chnguess_realloc () - More clients that currently maintained
 *   return: NO_ERROR
 *
 * Note: Expand the chn_guess structures to support at least the number
 * currently connected clients.
 */
static int
heap_chnguess_realloc (void)
{
  int i;
  unsigned char *save_bitindex;
  int save_nbytes;
  HEAP_CHNGUESS_ENTRY *entry;
  int ret = NO_ERROR;

  if (heap_Guesschn == NULL)
    {
      return heap_chnguess_initialize ();
    }

  /* 
   * Save current information, so we can copy them at a alater point
   */
  save_bitindex = heap_Guesschn_area.bitindex;
  save_nbytes = heap_Guesschn_area.nbytes;

  /* 
   * Find the number of clients that need to be supported. Avoid small
   * increases since it is undesirable to realloc again. Increase by at least
   * the fudge factor.
   */

  heap_Guesschn->num_clients += HEAP_CHNGUESS_FUDGE_MININDICES;
  i = logtb_get_number_of_total_tran_indices ();

  if (heap_Guesschn->num_clients < i)
    {
      heap_Guesschn->num_clients = i + HEAP_CHNGUESS_FUDGE_MININDICES;
    }

  /* Make sure every single bit is used */
  heap_Guesschn_area.nbytes = HEAP_NBITS_TO_NBYTES (heap_Guesschn_area.num_clients);
  heap_Guesschn_area.num_clients = HEAP_NBYTES_TO_NBITS (heap_Guesschn_area.nbytes);

  heap_Guesschn_area.bitindex = (unsigned char *) malloc (heap_Guesschn_area.nbytes * heap_Guesschn_area.num_entries);
  if (heap_Guesschn_area.bitindex == NULL)
    {
      ret = ER_OUT_OF_VIRTUAL_MEMORY;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ret, 1,
	      (size_t) (heap_Guesschn_area.nbytes * heap_Guesschn_area.num_entries));
      heap_Guesschn_area.bitindex = save_bitindex;
      heap_Guesschn_area.nbytes = save_nbytes;
      heap_Guesschn_area.num_clients = HEAP_NBYTES_TO_NBITS (save_nbytes);
      goto exit_on_error;
    }

  /* 
   * Now reset the bits for each entry
   */

  for (i = 0; i < heap_Guesschn_area.num_entries; i++)
    {
      entry = &heap_Guesschn_area.entries[i];
      entry->bits = &heap_Guesschn_area.bitindex[i * heap_Guesschn_area.nbytes];
      /* 
       * Copy the bits
       */
      memcpy (entry->bits, &save_bitindex[i * save_nbytes], save_nbytes);
      HEAP_NBYTES_CLEARED (&entry->bits[save_nbytes], heap_Guesschn_area.nbytes - save_nbytes);
    }
  /* 
   * Now throw previous storage
   */
  free_and_init (save_bitindex);

  return ret;

exit_on_error:

  return (ret == NO_ERROR && (ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_chnguess_finalize () - Finish chnguess information
 *   return: NO_ERROR
 *
 * Note: Destroy hash table and memory for entries.
 */
static int
heap_chnguess_finalize (void)
{
  int ret = NO_ERROR;

  if (heap_Guesschn == NULL)
    {
      return NO_ERROR;		/* nop */
    }

  mht_destroy (heap_Guesschn->ht);
  free_and_init (heap_Guesschn->entries);
  free_and_init (heap_Guesschn->bitindex);
  heap_Guesschn->ht = NULL;
  heap_Guesschn->schema_change = false;
  heap_Guesschn->clock_hand = 0;
  heap_Guesschn->num_entries = 0;
  heap_Guesschn->num_clients = 0;
  heap_Guesschn->nbytes = 0;

  heap_Guesschn = NULL;

  return ret;
}

/*
 * heap_stats_bestspace_initialize () - Initialize structure of best space
 *   return: NO_ERROR
 */
static int
heap_stats_bestspace_initialize (void)
{
  int ret = NO_ERROR;

  if (heap_Bestspace != NULL)
    {
      ret = heap_stats_bestspace_finalize ();
      if (ret != NO_ERROR)
	{
	  goto exit_on_error;
	}
    }

  heap_Bestspace = &heap_Bestspace_cache_area;

  pthread_mutex_init (&heap_Bestspace->bestspace_mutex, NULL);

  heap_Bestspace->num_stats_entries = 0;

  heap_Bestspace->hfid_ht =
    mht_create ("Memory hash HFID to {bestspace}", HEAP_STATS_ENTRY_MHT_EST_SIZE, heap_hash_hfid, heap_compare_hfid);
  if (heap_Bestspace->hfid_ht == NULL)
    {
      goto exit_on_error;
    }

  heap_Bestspace->vpid_ht =
    mht_create ("Memory hash VPID to {bestspace}", HEAP_STATS_ENTRY_MHT_EST_SIZE, heap_hash_vpid, heap_compare_vpid);
  if (heap_Bestspace->vpid_ht == NULL)
    {
      goto exit_on_error;
    }

  heap_Bestspace->num_alloc = 0;
  heap_Bestspace->num_free = 0;
  heap_Bestspace->free_list_count = 0;
  heap_Bestspace->free_list = NULL;

  return ret;

exit_on_error:

  return (ret == NO_ERROR) ? ER_FAILED : ret;
}

/*
 * heap_stats_bestspace_finalize () - Finish best space information
 *   return: NO_ERROR
 *
 * Note: Destroy hash table and memory for entries.
 */
static int
heap_stats_bestspace_finalize (void)
{
  HEAP_STATS_ENTRY *ent;
  int ret = NO_ERROR;

  if (heap_Bestspace == NULL)
    {
      return NO_ERROR;
    }

  if (heap_Bestspace->vpid_ht != NULL)
    {
      (void) mht_map_no_key (NULL, heap_Bestspace->vpid_ht, heap_stats_entry_free, NULL);
      while (heap_Bestspace->free_list_count > 0)
	{
	  ent = heap_Bestspace->free_list;
	  assert_release (ent != NULL);

	  heap_Bestspace->free_list = ent->next;
	  ent->next = NULL;

	  free (ent);

	  heap_Bestspace->free_list_count--;
	}
      assert_release (heap_Bestspace->free_list == NULL);
    }

  if (heap_Bestspace->vpid_ht != NULL)
    {
      mht_destroy (heap_Bestspace->vpid_ht);
      heap_Bestspace->vpid_ht = NULL;
    }

  if (heap_Bestspace->hfid_ht != NULL)
    {
      mht_destroy (heap_Bestspace->hfid_ht);
      heap_Bestspace->hfid_ht = NULL;
    }

  pthread_mutex_destroy (&heap_Bestspace->bestspace_mutex);

  heap_Bestspace = NULL;

  return ret;
}

/*
 * heap_chnguess_decache () - Decache a specific entry or all entries
 *   return: NO_ERROR
 *   oid(in): oid: class oid or NULL
 *            IF NULL implies all classes
 *
 * Note: Remove from the hash the entry associated with given oid. If
 * oid is NULL, all entries in hash are removed.
 * This function is called when a class is updated or during
 * rollback when a class was changed
 */
static int
heap_chnguess_decache (const OID * oid)
{
  HEAP_CHNGUESS_ENTRY *entry;
  int ret = NO_ERROR;

  if (heap_Guesschn == NULL)
    {
      return NO_ERROR;		/* nop */
    }

  if (oid == NULL)
    {
      (void) mht_map (heap_Guesschn->ht, heap_chnguess_remove_entry, NULL);
    }
  else
    {
      entry = (HEAP_CHNGUESS_ENTRY *) mht_get (heap_Guesschn->ht, oid);
      if (entry != NULL)
	{
	  (void) heap_chnguess_remove_entry (oid, entry, NULL);
	}
    }

  if (heap_Guesschn->schema_change == true && oid == NULL)
    {
      heap_Guesschn->schema_change = false;
    }

  return ret;
}

/*
 * heap_chnguess_remove_entry () - Remove an entry from chnguess hash table
 *   return: NO_ERROR
 *   oid_key(in): Key (oid) of chnguess table
 *   ent(in): The entry of hash table
 *   xignore(in): Extra arguments (currently ignored)
 *
 * Note: Remove from the hash the given entry. The entry is marked as
 * for immediate reuse.
 */
static int
heap_chnguess_remove_entry (const void *oid_key, void *ent, void *xignore)
{
  HEAP_CHNGUESS_ENTRY *entry = (HEAP_CHNGUESS_ENTRY *) ent;

  /* mht_rem() has been updated to take a function and an arg pointer that can be called on the entry before it is
   * removed.  We may want to take advantage of that here to free the memory associated with the entry */
  (void) mht_rem (heap_Guesschn->ht, oid_key, NULL, NULL);
  OID_SET_NULL (&entry->oid);
  entry->chn = NULL_CHN;
  entry->recently_accessed = false;
  heap_Guesschn_area.clock_hand = entry->idx;

  return NO_ERROR;
}

#if defined (CUBRID_DEBUG)
/*
 * heap_chnguess_dump () - Dump current chnguess hash table
 *   return:
 *
 * Note: Dump all valid chnguess entries.
 */
void
heap_chnguess_dump (FILE * fp)
{
  int max_tranindex, tran_index, i;
  HEAP_CHNGUESS_ENTRY *entry;

  if (heap_Guesschn != NULL)
    {
      fprintf (fp, "*** Dump of CLASS_OID to CHNGUESS at clients *** \n");
      fprintf (fp, "Schema_change = %d, clock_hand = %d,\n", heap_Guesschn->schema_change, heap_Guesschn->clock_hand);
      fprintf (fp, "Nentries = %d, Nactive_entries = %u, maxnum of clients = %d, nbytes = %d\n",
	       heap_Guesschn->num_entries, mht_count (heap_Guesschn->ht), heap_Guesschn->num_clients,
	       heap_Guesschn->nbytes);
      fprintf (fp, "Hash Table = %p, Entries = %p, Bitindex = %p\n", heap_Guesschn->ht, heap_Guesschn->entries,
	       heap_Guesschn->bitindex);

      max_tranindex = logtb_get_number_of_total_tran_indices ();
      for (i = 0; i < heap_Guesschn->num_entries; i++)
	{
	  entry = &heap_Guesschn_area.entries[i];

	  if (!OID_ISNULL (&entry->oid))
	    {
	      fprintf (fp, " \nEntry_id %d", entry->idx);
	      fprintf (fp, "OID = %2d|%4d|%2d, chn = %d, recently_free = %d,", entry->oid.volid, entry->oid.pageid,
		       entry->oid.slotid, entry->chn, entry->recently_accessed);

	      /* Dump one bit at a time */
	      for (tran_index = 0; tran_index < max_tranindex; tran_index++)
		{
		  if (tran_index % 40 == 0)
		    {
		      fprintf (fp, "\n ");
		    }
		  else if (tran_index % 10 == 0)
		    {
		      fprintf (fp, " ");
		    }
		  fprintf (fp, "%d", HEAP_BIT_GET (entry->bits, tran_index) ? 1 : 0);
		}
	      fprintf (fp, "\n");
	    }
	}
    }
}
#endif /* CUBRID_DEBUG */

/*
 * heap_chnguess_get () - Guess chn of given oid for given tran index (at client)
 *   return:
 *   oid(in): OID from where to guess chn at client workspace
 *   tran_index(in): The client transaction index
 *
 * Note: Find/guess the chn of the given OID object at the workspace of
 * given client transaction index
 */
int
heap_chnguess_get (THREAD_ENTRY * thread_p, const OID * oid, int tran_index)
{
  int chn = NULL_CHN;
  HEAP_CHNGUESS_ENTRY *entry;

  if (csect_enter (thread_p, CSECT_HEAP_CHNGUESS, INF_WAIT) != NO_ERROR)
    {
      return NULL_CHN;
    }

  if (heap_Guesschn != NULL)
    {
      if (heap_Guesschn->num_clients <= tran_index)
	{
	  if (heap_chnguess_realloc () != NO_ERROR)
	    {
	      csect_exit (thread_p, CSECT_HEAP_CHNGUESS);
	      return NULL_CHN;
	    }
	}

      /* 
       * Do we have this entry in hash table, if we do then check corresponding
       * bit for given client transaction index.
       */

      entry = (HEAP_CHNGUESS_ENTRY *) mht_get (heap_Guesschn->ht, oid);
      if (entry != NULL && HEAP_BIT_GET (entry->bits, tran_index))
	{
	  chn = entry->chn;
	}
    }

  csect_exit (thread_p, CSECT_HEAP_CHNGUESS);

  return chn;
}

/*
 * heap_chnguess_put () - Oid object is in the process of been sent to client
 *   return: chn or NULL_CHN if not cached
 *   oid(in): object oid
 *   tran_index(in): The client transaction index
 *   chn(in): cache coherency number.
 *
 * Note: Cache the information that object oid with chn has been sent
 * to client with trans_index.
 * If the function fails, it returns NULL_CHN. This failure is
 * more like a warning since the chnguess is just a caching structure.
 */
int
heap_chnguess_put (THREAD_ENTRY * thread_p, const OID * oid, int tran_index, int chn)
{
  int i;
  bool can_continue;
  HEAP_CHNGUESS_ENTRY *entry;

  if (heap_Guesschn == NULL)
    {
      return NULL_CHN;
    }

  if (csect_enter (thread_p, CSECT_HEAP_CHNGUESS, INF_WAIT) != NO_ERROR)
    {
      return NULL_CHN;
    }

  if (heap_Guesschn->num_clients <= tran_index)
    {
      if (heap_chnguess_realloc () != NO_ERROR)
	{
	  csect_exit (thread_p, CSECT_HEAP_CHNGUESS);
	  return NULL_CHN;
	}
    }

  /* 
   * Is the entry already in the chnguess hash table ?
   */
  entry = (HEAP_CHNGUESS_ENTRY *) mht_get (heap_Guesschn->ht, oid);
  if (entry != NULL)
    {
      /* 
       * If the cache coherence number is different reset all client entries
       */
      if (entry->chn != chn)
	{
	  HEAP_NBYTES_CLEARED (entry->bits, heap_Guesschn_area.nbytes);
	  entry->chn = chn;
	}
    }
  else
    {
      /* 
       * Replace one of the entries that has not been used for a while.
       * Follow clock replacement algorithm.
       */
      can_continue = true;
      while (entry == NULL && can_continue == true)
	{
	  can_continue = false;
	  for (i = 0; i < heap_Guesschn->num_entries; i++)
	    {
	      /* 
	       * Increase the clock to next entry
	       */
	      heap_Guesschn->clock_hand++;
	      if (heap_Guesschn->clock_hand >= heap_Guesschn->num_entries)
		{
		  heap_Guesschn->clock_hand = 0;
		}

	      entry = &heap_Guesschn->entries[heap_Guesschn->clock_hand];
	      if (entry->recently_accessed == true)
		{
		  /* 
		   * Set recently freed to false, so it can be replaced in next
		   * if the entry is not referenced
		   */
		  entry->recently_accessed = false;
		  entry = NULL;
		  can_continue = true;
		}
	      else
		{
		  entry->oid = *oid;
		  entry->chn = chn;
		  HEAP_NBYTES_CLEARED (entry->bits, heap_Guesschn_area.nbytes);
		  break;
		}
	    }
	}
    }

  /* 
   * Now set the desired client transaction index bit
   */
  if (entry != NULL)
    {
      HEAP_BIT_SET (entry->bits, tran_index);
      entry->recently_accessed = true;
    }
  else
    {
      chn = NULL_CHN;
    }

  csect_exit (thread_p, CSECT_HEAP_CHNGUESS);

  return chn;
}

/*
 * heap_chnguess_clear () - Clear any cached information for given client
 *                        used when client is shutdown
 *   return:
 *   tran_index(in): The client transaction index
 *
 * Note: Clear the transaction index bit for all chnguess entries.
 */
void
heap_chnguess_clear (THREAD_ENTRY * thread_p, int tran_index)
{
  int i;
  HEAP_CHNGUESS_ENTRY *entry;

  if (csect_enter (thread_p, CSECT_HEAP_CHNGUESS, INF_WAIT) != NO_ERROR)
    {
      return;
    }

  if (heap_Guesschn != NULL)
    {
      for (i = 0; i < heap_Guesschn->num_entries; i++)
	{
	  entry = &heap_Guesschn_area.entries[i];
	  if (!OID_ISNULL (&entry->oid))
	    {
	      HEAP_BIT_CLEAR (entry->bits, (unsigned int) tran_index);
	    }
	}
    }

  csect_exit (thread_p, CSECT_HEAP_CHNGUESS);

}

/*
 * Recovery functions
 */

/*
 * heap_rv_redo_newpage_internal () - Redo the statistics or a new page
 *                                    allocation for a heap file
 *   return: int
 *   rcv(in): Recovery structure
 *   reuse_oid(in): whether the heap is OID reusable
 */
static int
heap_rv_redo_newpage_internal (THREAD_ENTRY * thread_p, LOG_RCV * rcv, const bool reuse_oid)
{
  RECDES recdes;
  INT16 slotid;
  int sp_success;

  (void) pgbuf_set_page_ptype (thread_p, rcv->pgptr, PAGE_HEAP);

  /* Initialize header page */
  spage_initialize (thread_p, rcv->pgptr, heap_get_spage_type (), HEAP_MAX_ALIGN, SAFEGUARD_RVSPACE);

  /* Now insert first record (either statistics or chain record) */
  recdes.area_size = recdes.length = rcv->length;
  recdes.type = REC_HOME;
  recdes.data = (char *) rcv->data;
  sp_success = spage_insert (thread_p, rcv->pgptr, &recdes, &slotid);
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  if (sp_success != SP_SUCCESS || slotid != HEAP_HEADER_AND_CHAIN_SLOTID)
    {
      if (sp_success != SP_SUCCESS)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	}
      /* something went wrong. Unable to redo initialization of new heap page */
      assert (er_errid () != NO_ERROR);
      return er_errid ();
    }

  return NO_ERROR;
}

/*
 * heap_rv_redo_newpage () - Redo the statistics or a new page allocation for
 *                           a heap file
 *   return: int
 *   rcv(in): Recovery structure
 */
int
heap_rv_redo_newpage (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  return heap_rv_redo_newpage_internal (thread_p, rcv, false);
}

/*
 * heap_rv_redo_newpage_reuse_oid () - Redo the statistics or a new page
 *                                     allocation for a heap file with
 *                                     reusable OIDs
 *   return: int
 *   rcv(in): Recovery structure
 */
int
heap_rv_redo_newpage_reuse_oid (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  return heap_rv_redo_newpage_internal (thread_p, rcv, true);
}

/*
 * heap_rv_undoredo_pagehdr () - Recover the header of a heap page
 *                    (either statistics/chain)
 *   return: int
 *   rcv(in): Recovery structure
 *
 * Note: Recover the update of the header or a heap page. The header
 * can be the heap header or a chain header.
 */
int
heap_rv_undoredo_pagehdr (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  RECDES recdes;
  int sp_success;

  (void) pgbuf_check_page_ptype (thread_p, rcv->pgptr, PAGE_HEAP);

  recdes.area_size = recdes.length = rcv->length;
  recdes.type = REC_HOME;
  recdes.data = (char *) rcv->data;

  sp_success = spage_update (thread_p, rcv->pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes);
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  if (sp_success != SP_SUCCESS)
    {
      /* something went wrong. Unable to redo update statistics for chain */
      if (sp_success != SP_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	}
      assert (er_errid () != NO_ERROR);
      return er_errid ();
    }
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * heap_rv_dump_statistics () - Dump statistics recovery information
 *   return: int
 *   ignore_length(in): Length of Recovery Data
 *   data(in): The data being logged
 *
 * Note: Dump statistics recovery information
 */
void
heap_rv_dump_statistics (FILE * fp, int ignore_length, void *data)
{
  int ret = NO_ERROR;

  HEAP_HDR_STATS *heap_hdr;	/* Header of heap structure */

  heap_hdr = (HEAP_HDR_STATS *) data;
  ret = heap_dump_hdr (fp, heap_hdr);
}

/*
 * heap_rv_dump_chain () - Dump chain recovery information
 *   return: int
 *   ignore_length(in): Length of Recovery Data
 *   data(in): The data being logged
 */
void
heap_rv_dump_chain (FILE * fp, int ignore_length, void *data)
{
  HEAP_CHAIN *chain;

  chain = (HEAP_CHAIN *) data;
  fprintf (fp, "CLASS_OID = %2d|%4d|%2d, PREV_VPID = %2d|%4d, NEXT_VPID = %2d|%4d, MAX_MVCCID=%llu, flags=%d.\n",
	   chain->class_oid.volid, chain->class_oid.pageid, chain->class_oid.slotid, chain->prev_vpid.volid,
	   chain->prev_vpid.pageid, chain->next_vpid.volid, chain->next_vpid.pageid,
	   (unsigned long long int) chain->max_mvccid, (int) chain->flags);
}

/*
 * heap_rv_redo_insert () - Redo the insertion of an object
 *   return: int
 *   rcv(in): Recovery structure
 *
 * Note: Redo the insertion of an object at a specific location (OID).
 */
int
heap_rv_redo_insert (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  INT16 slotid;
  RECDES recdes;
  int sp_success;

  slotid = rcv->offset;
  recdes.type = *(INT16 *) (rcv->data);
  recdes.data = (char *) (rcv->data) + sizeof (recdes.type);
  recdes.area_size = recdes.length = rcv->length - sizeof (recdes.type);

  if (recdes.type == REC_ASSIGN_ADDRESS)
    {
      /* 
       * The data here isn't really the data to be inserted (because there
       * wasn't any); instead it's the number of bytes that were reserved
       * for future insertion.  Change recdes.length to reflect the number
       * of bytes to reserve, but there's no need for a valid recdes.data:
       * spage_insert_for_recovery knows to ignore it in this case.
       */
      recdes.area_size = recdes.length = *(INT16 *) recdes.data;
      recdes.data = NULL;
    }

  sp_success = spage_insert_for_recovery (thread_p, rcv->pgptr, slotid, &recdes);
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  if (sp_success != SP_SUCCESS)
    {
      /* Unable to redo insertion */
      if (sp_success != SP_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	}
      assert (er_errid () != NO_ERROR);
      return er_errid ();
    }

  return NO_ERROR;
}

/*
 * heap_mvcc_log_insert () - Log MVCC insert heap operation.
 *
 * return	 : Void.
 * thread_p (in) : Thread entry.
 * p_recdes (in) : Newly inserted record.
 * p_addr (in)	 : Log address data.
 */
static void
heap_mvcc_log_insert (THREAD_ENTRY * thread_p, RECDES * p_recdes, LOG_DATA_ADDR * p_addr)
{
#define HEAP_LOG_MVCC_INSERT_MAX_REDO_CRUMBS	    4

  int n_redo_crumbs = 0, data_copy_offset = 0, chn_offset;
  LOG_CRUMB redo_crumbs[HEAP_LOG_MVCC_INSERT_MAX_REDO_CRUMBS];
  INT32 mvcc_flags;
  HEAP_PAGE_VACUUM_STATUS vacuum_status;

  assert (p_recdes != NULL);
  assert (p_addr != NULL);

  vacuum_status = heap_page_get_vacuum_status (thread_p, p_addr->pgptr);

  /* Update chain. */
  heap_page_update_chain_after_mvcc_op (thread_p, p_addr->pgptr, logtb_get_current_mvccid (thread_p));
  if (vacuum_status != heap_page_get_vacuum_status (thread_p, p_addr->pgptr))
    {
      /* Mark status change for recovery. */
      p_addr->offset |= HEAP_RV_FLAG_VACUUM_STATUS_CHANGE;
    }

  /* Build redo crumbs */
  /* Add record type */
  redo_crumbs[n_redo_crumbs].length = sizeof (p_recdes->type);
  redo_crumbs[n_redo_crumbs++].data = &p_recdes->type;

  if (p_recdes->type != REC_BIGONE)
    {
      mvcc_flags = (INT32) OR_GET_MVCC_FLAG (p_recdes->data);
      chn_offset = OR_GET_MVCC_CHN_OFFSET (mvcc_flags);

      assert (chn_offset > 0);

      /* Add representation ID and flags field */
      redo_crumbs[n_redo_crumbs].length = OR_INT_SIZE;
      redo_crumbs[n_redo_crumbs++].data = p_recdes->data;

      /* Add CHN */
      redo_crumbs[n_redo_crumbs].length = OR_INT_SIZE;
      redo_crumbs[n_redo_crumbs++].data = p_recdes->data + chn_offset;

      /* Set data copy offset after the record header */
      data_copy_offset = OR_HEADER_SIZE (p_recdes->data);
    }

  /* Add record data - record may be skipped if the record is not big one */
  redo_crumbs[n_redo_crumbs].length = p_recdes->length - data_copy_offset;
  redo_crumbs[n_redo_crumbs++].data = p_recdes->data + data_copy_offset;

  /* Safe guard */
  assert (n_redo_crumbs <= HEAP_LOG_MVCC_INSERT_MAX_REDO_CRUMBS);

  /* Append redo crumbs; undo crumbs not necessary as the spage_delete physical operation uses the offset field of the
   * address */
  log_append_undoredo_crumbs (thread_p, RVHF_MVCC_INSERT, p_addr, 0, n_redo_crumbs, NULL, redo_crumbs);
}

/*
 * heap_rv_mvcc_redo_insert () - Redo the MVCC insertion of an object
 *   return: int
 *   rcv(in): Recovery structure
 *
 * Note: MVCC redo the insertion of an object at a specific location (OID).
 */
int
heap_rv_mvcc_redo_insert (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  INT16 slotid;
  RECDES recdes;
  int chn, sp_success;
  MVCC_REC_HEADER mvcc_rec_header;
  INT16 record_type;
  bool vacuum_status_change = false;

  assert (rcv->pgptr != NULL);
  assert (MVCCID_IS_NORMAL (rcv->mvcc_id));

  slotid = rcv->offset;
  if (slotid & HEAP_RV_FLAG_VACUUM_STATUS_CHANGE)
    {
      vacuum_status_change = true;
    }
  slotid = slotid & (~HEAP_RV_FLAG_VACUUM_STATUS_CHANGE);
  assert (slotid > 0);

  record_type = *(INT16 *) rcv->data;
  if (record_type == REC_BIGONE)
    {
      /* no data header */
      HEAP_SET_RECORD (&recdes, rcv->length - sizeof (record_type), rcv->length - sizeof (record_type), REC_BIGONE,
		       rcv->data + sizeof (record_type));
    }
  else
    {
      char data_buffer[IO_DEFAULT_PAGE_SIZE + OR_MVCC_MAX_HEADER_SIZE + MAX_ALIGNMENT];
      int repid_and_flags, offset, mvcc_flag, offset_size;

      offset = sizeof (record_type);

      repid_and_flags = OR_GET_INT (rcv->data + offset);
      offset += OR_INT_SIZE;

      chn = OR_GET_INT (rcv->data + offset);
      offset += OR_INT_SIZE;

      mvcc_flag = (char) ((repid_and_flags >> OR_MVCC_FLAG_SHIFT_BITS) & OR_MVCC_FLAG_MASK);

      assert (!(mvcc_flag & (OR_MVCC_FLAG_VALID_DELID | OR_MVCC_FLAG_VALID_LONG_CHN
			     | OR_MVCC_FLAG_VALID_NEXT_VERSION)));

      if ((repid_and_flags & OR_OFFSET_SIZE_FLAG) == OR_OFFSET_SIZE_1BYTE)
	{
	  offset_size = OR_BYTE_SIZE;
	}
      else if ((repid_and_flags & OR_OFFSET_SIZE_FLAG) == OR_OFFSET_SIZE_2BYTE)
	{
	  offset_size = OR_SHORT_SIZE;
	}
      else
	{
	  offset_size = OR_INT_SIZE;
	}

      MVCC_SET_REPID (&mvcc_rec_header, repid_and_flags & OR_MVCC_REPID_MASK);
      MVCC_SET_FLAG (&mvcc_rec_header, mvcc_flag);
      MVCC_SET_INSID (&mvcc_rec_header, rcv->mvcc_id);
      MVCC_SET_CHN (&mvcc_rec_header, chn);

      HEAP_SET_RECORD (&recdes, IO_DEFAULT_PAGE_SIZE + OR_MVCC_MAX_HEADER_SIZE, 0, record_type,
		       PTR_ALIGN (data_buffer, MAX_ALIGNMENT));
      or_mvcc_add_header (&recdes, &mvcc_rec_header, repid_and_flags & OR_BOUND_BIT_FLAG, offset_size);

      memcpy (recdes.data + recdes.length, rcv->data + offset, rcv->length - offset);
      recdes.length += (rcv->length - offset);
    }

  sp_success = spage_insert_for_recovery (thread_p, rcv->pgptr, slotid, &recdes);

  if (sp_success != SP_SUCCESS)
    {
      /* Unable to redo insertion */
      assert_release (false);
      return ER_FAILED;
    }

  heap_page_rv_chain_update (thread_p, rcv->pgptr, rcv->mvcc_id, vacuum_status_change);
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * heap_rv_undo_insert () - Undo the insertion of an object.
 *   return: int
 *   rcv(in): Recovery structure
 *
 * Note: Delete an object for recovery purposes. The OID of the object
 * is reused since the object was never committed.
 */
int
heap_rv_undo_insert (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  INT16 slotid;

  slotid = rcv->offset;
  /* Clear HEAP_RV_FLAG_VACUUM_STATUS_CHANGE */
  slotid = slotid & (~HEAP_RV_FLAG_VACUUM_STATUS_CHANGE);
  (void) spage_delete_for_recovery (thread_p, rcv->pgptr, slotid);
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * heap_rv_redo_delete () - Redo the deletion of an object
 *   return: int
 *   rcv(in): Recovery structure
 *
 * Note: Redo the deletion of an object.
 * The OID of the object is not reuse since we don't know if the object was a
 * newly created object.
 */
int
heap_rv_redo_delete (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  INT16 slotid;

  slotid = rcv->offset;
  (void) spage_delete (thread_p, rcv->pgptr, slotid);
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * heap_mvcc_log_delete () - Log normal MVCC heap delete operation (just
 *			     append delete MVCCID and next version OID).
 *
 * return		    : Void.
 * thread_p (in)	    : Thread entry.
 * undo_chn (in)	    : Record CHN before delete.
 * p_redo_next_version (in) : In case of update, the OID of next version. NULL
 *			      otherwise.
 * partition_oid	    : In case of recorded relocated to another
 *			      partition
 * p_addr (in)		    : Log address data.
 */
static void
heap_mvcc_log_delete (THREAD_ENTRY * thread_p, INT32 undo_chn, OID * p_redo_next_version, OID * partition_oid,
		      LOG_DATA_ADDR * p_addr, LOG_RCVINDEX rcvindex)
{
  char undo_data_buffer[OR_INT_SIZE + MAX_ALIGNMENT];
  char redo_data_buffer[OR_MVCCID_SIZE + 2 * OR_OID_SIZE + MAX_ALIGNMENT];
  char *undo_data_p = PTR_ALIGN (undo_data_buffer, MAX_ALIGNMENT);
  char *redo_data_p = PTR_ALIGN (redo_data_buffer, MAX_ALIGNMENT);
  char *ptr;
  int undo_data_size = 0;
  int redo_data_size = 0;
  HEAP_PAGE_VACUUM_STATUS vacuum_status;

  assert (p_addr != NULL);
  assert (rcvindex == RVHF_MVCC_DELETE_REC_HOME || rcvindex == RVHF_MVCC_DELETE_REC_NEWHOME
	  || rcvindex == RVHF_MVCC_DELETE_OVERFLOW);

  if (LOG_IS_MVCC_HEAP_OPERATION (rcvindex))
    {
      vacuum_status = heap_page_get_vacuum_status (thread_p, p_addr->pgptr);

      heap_page_update_chain_after_mvcc_op (thread_p, p_addr->pgptr, logtb_get_current_mvccid (thread_p));
      if (heap_page_get_vacuum_status (thread_p, p_addr->pgptr) != vacuum_status)
	{
	  /* Mark vacuum status change for recovery. */
	  p_addr->offset |= HEAP_RV_FLAG_VACUUM_STATUS_CHANGE;
	}
    }

  /* Prepare undo data. */
  ptr = or_pack_int (undo_data_p, (int) undo_chn);
  assert ((ptr - undo_data_buffer) <= (int) sizeof (undo_data_buffer));
  undo_data_size = OR_INT_SIZE;

  /* Prepare redo data. */
  ptr = redo_data_p;

  if (rcvindex != RVHF_MVCC_DELETE_REC_HOME)
    {
      /* MVCCID must be packed also, since it is not saved in log record structure. */
      ptr = or_pack_mvccid (ptr, logtb_get_current_mvccid (thread_p));
      redo_data_size += OR_MVCCID_SIZE;
    }

  if (p_redo_next_version != NULL && !OID_ISNULL (p_redo_next_version))
    {
      /* Pack next version. */
      ptr = or_pack_oid (ptr, p_redo_next_version);
      redo_data_size += OR_OID_SIZE;

      if (partition_oid != NULL && !OID_ISNULL (partition_oid))
	{
	  /* Pack partition OID. */
	  ptr = or_pack_oid (ptr, partition_oid);
	  redo_data_size += OR_OID_SIZE;
	}
    }
  assert ((ptr - redo_data_buffer) <= (int) sizeof (redo_data_buffer));

  /* Log append undo/redo crumbs */
  log_append_undoredo_data (thread_p, rcvindex, p_addr, undo_data_size, redo_data_size, undo_data_p, redo_data_p);
}

/*
 * heap_rv_mvcc_undo_delete () - Undo the MVCC deletion of an object
 *   return: int
 *   rcv(in): Recovery structure
 */
int
heap_rv_mvcc_undo_delete (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  INT16 slotid;
  MVCC_REC_HEADER mvcc_rec_header;
  char data_buffer[IO_MAX_PAGE_SIZE + MAX_ALIGNMENT];
  RECDES rebuild_record;
  INT32 chn;

  /* Read chn */
  chn = (INT32) OR_GET_INT (rcv->data);
  assert (rcv->length == OR_INT_SIZE);

  slotid = rcv->offset;
  slotid = slotid & (~HEAP_RV_FLAG_VACUUM_STATUS_CHANGE);
  assert (slotid > 0);

  rebuild_record.data = PTR_ALIGN (data_buffer, MAX_ALIGNMENT);
  rebuild_record.area_size = DB_PAGESIZE;
  if (spage_get_record (rcv->pgptr, slotid, &rebuild_record, COPY) != S_SUCCESS)
    {
      assert_release (false);
      return ER_FAILED;
    }
  assert (rebuild_record.type == REC_HOME || rebuild_record.type == REC_NEWHOME);

  if (or_mvcc_get_header (&rebuild_record, &mvcc_rec_header) != NO_ERROR)
    {
      assert_release (false);
      return ER_FAILED;
    }
  assert (MVCC_IS_FLAG_SET (&mvcc_rec_header, OR_MVCC_FLAG_VALID_DELID));

  /* handle DEL_ID / CHN */
  MVCC_CLEAR_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_DELID);
  MVCC_SET_CHN (&mvcc_rec_header, chn);

  /* remove OR_MVCC_FLAG_VALID_NEXT_VERSION flag even if this flag was set together with null next oid, before delete
   * execution */
  MVCC_CLEAR_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_NEXT_VERSION);
  MVCC_CLEAR_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_PARTITION_OID);

  if (or_mvcc_set_header (&rebuild_record, &mvcc_rec_header) != NO_ERROR)
    {
      assert_release (false);
      return ER_FAILED;
    }

  if (spage_update (thread_p, rcv->pgptr, slotid, &rebuild_record) != SP_SUCCESS)
    {
      assert_release (false);
      return ER_FAILED;
    }

  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
  return NO_ERROR;
}

/*
 * heap_rv_mvcc_undo_delete_overflow () - Undo MVCC delete of an overflow
 *					  record.
 *
 * return	 : Error code.
 * thread_p (in) : Thread entry.
 * rcv (in)	 : Recovery data.
 */
int
heap_rv_mvcc_undo_delete_overflow (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  INT32 chn;
  MVCC_REC_HEADER mvcc_header;

  chn = (INT32) OR_GET_INT (rcv->data);

  if (heap_get_mvcc_rec_header_from_overflow (rcv->pgptr, &mvcc_header, NULL) != NO_ERROR)
    {
      assert_release (false);
      return ER_FAILED;
    }
  assert (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_DELID));
  assert (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_NEXT_VERSION));
  assert (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_PARTITION_OID));

  /* Fixed size header. Remove flag for delete MVCCID, and set long chn. */
  MVCC_CLEAR_FLAG_BITS (&mvcc_header, OR_MVCC_FLAG_VALID_DELID);
  MVCC_SET_FLAG_BITS (&mvcc_header, OR_MVCC_FLAG_VALID_LONG_CHN);
  MVCC_SET_CHN (&mvcc_header, chn);
  MVCC_SET_NEXT_VERSION (&mvcc_header, &oid_Null_oid);
  MVCC_SET_PARTITION_OID (&mvcc_header, &oid_Null_oid);

  /* Change header. */
  if (heap_set_mvcc_rec_header_on_overflow (rcv->pgptr, &mvcc_header) != NO_ERROR)
    {
      assert_release (false);
      return ER_FAILED;
    }

  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
  return NO_ERROR;
}

/*
 * heap_rv_mvcc_redo_delete_internal () - Internal function to be used by
 *					  heap_rv_mvcc_redo_delete_home and
 *					  heap_rv_mvcc_redo_delete_newhome.
 *
 * return	      : Error code.
 * thread_p (in)      : Thread entry.
 * page (in)	      : Heap page.
 * slotid (in)	      : Recovered record slotid.
 * mvccid (in)	      : Delete MVCCID.
 * next_version (in)  : Next version.
 * partition_oid (in) : Partition OID.
 */
static int
heap_rv_mvcc_redo_delete_internal (THREAD_ENTRY * thread_p, PAGE_PTR page, PGSLOTID slotid, MVCCID mvccid,
				   OID * next_version, OID * partition_oid)
{
  RECDES rebuild_record;
  char data_buffer[IO_MAX_PAGE_SIZE + MAX_ALIGNMENT];
  MVCC_REC_HEADER mvcc_rec_header;

  assert (page != NULL);
  assert (MVCCID_IS_NORMAL (mvccid));

  rebuild_record.data = PTR_ALIGN (data_buffer, MAX_ALIGNMENT);
  rebuild_record.area_size = DB_PAGESIZE;

  /* Get record. */
  if (spage_get_record (page, slotid, &rebuild_record, COPY) != S_SUCCESS)
    {
      assert_release (false);
      return ER_FAILED;
    }

  /* Get MVCC header. */
  if (or_mvcc_get_header (&rebuild_record, &mvcc_rec_header) != NO_ERROR)
    {
      assert_release (false);
      return ER_FAILED;
    }

  /* Set delete MVCCID. */
  MVCC_SET_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_DELID);
  MVCC_SET_DELID (&mvcc_rec_header, mvccid);

  if (next_version != NULL && !OID_ISNULL (next_version))
    {
      /* Set next version. */
      MVCC_SET_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_NEXT_VERSION);
      MVCC_SET_NEXT_VERSION (&mvcc_rec_header, next_version);

      if (partition_oid != NULL && !OID_ISNULL (partition_oid))
	{
	  /* Set partition OID. */
	  MVCC_SET_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_PARTITION_OID);
	  COPY_OID (&mvcc_rec_header.partition_oid, partition_oid);
	}
    }

  /* Change header. */
  if (or_mvcc_set_header (&rebuild_record, &mvcc_rec_header) != NO_ERROR)
    {
      assert_release (false);
      return ER_FAILED;
    }

  /* Update record in page. */
  if (spage_update (thread_p, page, slotid, &rebuild_record) != SP_SUCCESS)
    {
      assert_release (false);
      return ER_FAILED;
    }

  /* Success. */
  return NO_ERROR;
}

/*
 * heap_rv_mvcc_redo_delete_home () - Redo MVCC delete of REC_HOME record.
 *
 * return	 : Error code
 * thread_p (in) : Thread entry.
 * rcv (in)	 : Recovery data.
 */
int
heap_rv_mvcc_redo_delete_home (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  int error_code = NO_ERROR;
  int offset = 0;
  PGSLOTID slotid;
  OID next_version;
  OID partition_oid;
  bool vacuum_status_change = false;

  assert (rcv->pgptr != NULL);
  assert (MVCCID_IS_NORMAL (rcv->mvcc_id));

  slotid = rcv->offset;
  if (slotid & HEAP_RV_FLAG_VACUUM_STATUS_CHANGE)
    {
      vacuum_status_change = true;
    }
  slotid = slotid & (~HEAP_RV_FLAG_VACUUM_STATUS_CHANGE);
  assert (slotid > 0);

  if (offset < rcv->length)
    {
      /* Read next version OID */
      OR_GET_OID (rcv->data + offset, &next_version);
      offset += OR_OID_SIZE;
      if (offset < rcv->length)
	{
	  /* Read partition OID */
	  OR_GET_OID (rcv->data + offset, &partition_oid);
	  offset += OR_OID_SIZE;
	}
      else
	{
	  OID_SET_NULL (&partition_oid);
	}
    }
  else
    {
      OID_SET_NULL (&next_version);
      OID_SET_NULL (&partition_oid);
    }

  assert (offset == rcv->length);

  error_code =
    heap_rv_mvcc_redo_delete_internal (thread_p, rcv->pgptr, slotid, rcv->mvcc_id, &next_version, &partition_oid);
  if (error_code != NO_ERROR)
    {
      ASSERT_ERROR ();
      return error_code;
    }
  heap_page_rv_chain_update (thread_p, rcv->pgptr, rcv->mvcc_id, vacuum_status_change);

  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
  return NO_ERROR;
}

/*
 * heap_rv_mvcc_redo_delete_overflow () - Redo MVCC delete of overflow record.
 *
 * return	 : Error code
 * thread_p (in) : Thread entry.
 * rcv (in)	 : Recovery data.
 */
int
heap_rv_mvcc_redo_delete_overflow (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  int offset = 0;
  MVCCID mvccid;
  OID next_version;
  OID partition_oid;
  MVCC_REC_HEADER mvcc_header;

  assert (rcv->pgptr != NULL);

  OR_GET_MVCCID (rcv->data + offset, &mvccid);
  offset += OR_MVCCID_SIZE;

  if (offset < rcv->length)
    {
      /* Read next version OID */
      OR_GET_OID (rcv->data + offset, &next_version);
      offset += OR_OID_SIZE;
      if (offset < rcv->length)
	{
	  /* Read partition OID */
	  OR_GET_OID (rcv->data + offset, &partition_oid);
	  offset += OR_OID_SIZE;
	}
      else
	{
	  OID_SET_NULL (&partition_oid);
	}
    }
  else
    {
      OID_SET_NULL (&next_version);
      OID_SET_NULL (&partition_oid);
    }

  assert (offset == rcv->length);

  if (heap_get_mvcc_rec_header_from_overflow (rcv->pgptr, &mvcc_header, NULL) != NO_ERROR)
    {
      assert_release (false);
      return ER_FAILED;
    }
  assert (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_INSID));
  assert (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_LONG_CHN));
  assert (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_NEXT_VERSION));
  assert (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_PARTITION_OID));

  /* Set delete MVCCID. */
  MVCC_SET_FLAG_BITS (&mvcc_header, OR_MVCC_FLAG_VALID_DELID);
  MVCC_SET_DELID (&mvcc_header, mvccid);

  /* Set next version. */
  MVCC_SET_NEXT_VERSION (&mvcc_header, &next_version);

  /* Set partition OID. */
  MVCC_SET_PARTITION_OID (&mvcc_header, &partition_oid);

  /* Update MVCC header. */
  if (heap_set_mvcc_rec_header_on_overflow (rcv->pgptr, &mvcc_header) != NO_ERROR)
    {
      assert_release (false);
      return ER_FAILED;
    }

  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
  return NO_ERROR;
}

/*
 * heap_rv_mvcc_redo_delete_newhome () - Redo MVCC delete of REC_NEWHOME
 *					 record.
 *
 * return	 : Error code
 * thread_p (in) : Thread entry.
 * rcv (in)	 : Recovery data.
 */
int
heap_rv_mvcc_redo_delete_newhome (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  int error_code = NO_ERROR;
  int offset = 0;
  MVCCID mvccid;
  OID next_version;
  OID partition_oid;

  assert (rcv->pgptr != NULL);

  OR_GET_MVCCID (rcv->data + offset, &mvccid);
  offset += OR_MVCCID_SIZE;

  if (offset < rcv->length)
    {
      /* Read next version OID */
      OR_GET_OID (rcv->data + offset, &next_version);
      offset += OR_OID_SIZE;
      if (offset < rcv->length)
	{
	  /* Read partition OID */
	  OR_GET_OID (rcv->data + offset, &partition_oid);
	  offset += OR_OID_SIZE;
	}
      else
	{
	  OID_SET_NULL (&partition_oid);
	}
    }
  else
    {
      OID_SET_NULL (&next_version);
      OID_SET_NULL (&partition_oid);
    }

  assert (offset == rcv->length);

  error_code =
    heap_rv_mvcc_redo_delete_internal (thread_p, rcv->pgptr, rcv->offset, mvccid, &next_version, &partition_oid);
  if (error_code != NO_ERROR)
    {
      ASSERT_ERROR ();
      return error_code;
    }

  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
  return NO_ERROR;
}

/*
 * heap_rv_redo_mark_reusable_slot () - Marks a deleted slot as reusable; used
 *                                      as a postponed log operation and a
 *                                      REDO function
 *   return: int
 *   rcv(in): Recovery structure
 *
 * Note: Mark (during postponed operation execution)/Redo (during recovery)
 *       the marking of a deleted slot as reusable.
 */
int
heap_rv_redo_mark_reusable_slot (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  INT16 slotid;

  slotid = rcv->offset;
  (void) spage_mark_deleted_slot_as_reusable (thread_p, rcv->pgptr, slotid);
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * heap_rv_undo_delete () - Undo the deletion of an object
 *   return: int
 *   rcv(in): Recovery structure
 */
int
heap_rv_undo_delete (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  return heap_rv_redo_insert (thread_p, rcv);
}

/*
 * heap_rv_undoredo_update () - Recover an update either for undo or redo
 *   return: int
 *   rcv(in): Recovery structure
 */
int
heap_rv_undoredo_update (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  INT16 slotid;
  RECDES recdes;
  int sp_success;

  slotid = rcv->offset;
  slotid = slotid & (~HEAP_RV_FLAG_VACUUM_STATUS_CHANGE);
  assert (slotid > 0);

  recdes.type = *(INT16 *) (rcv->data);
  recdes.data = (char *) (rcv->data) + sizeof (recdes.type);
  recdes.area_size = recdes.length = rcv->length - sizeof (recdes.type);
  if (recdes.area_size <= 0)
    {
      sp_success = SP_SUCCESS;
    }
  else
    {
      sp_success = spage_update (thread_p, rcv->pgptr, slotid, &recdes);
    }

  if (sp_success != SP_SUCCESS)
    {
      /* Unable to recover update for object */
      pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
      if (sp_success != SP_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	}
      assert (er_errid () != NO_ERROR);
      return er_errid ();
    }
  spage_update_record_type (thread_p, rcv->pgptr, slotid, recdes.type);
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * heap_rv_redo_reuse_page () - Redo the deletion of all objects in page for
 *                              reuse purposes
 *   return: int
 *   rcv(in): Recovery structure
 */
int
heap_rv_redo_reuse_page (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  VPID vpid;
  RECDES recdes;
  HEAP_CHAIN *chain;		/* Chain to next and prev page */
  int sp_success;
  const bool is_header_page = ((rcv->offset != 0) ? true : false);

  (void) pgbuf_check_page_ptype (thread_p, rcv->pgptr, PAGE_HEAP);

  vpid.volid = pgbuf_get_volume_id (rcv->pgptr);
  vpid.pageid = pgbuf_get_page_id (rcv->pgptr);

  /* We ignore the return value. It should be true (objects were deleted) except for the scenario when the redo actions 
   * are applied twice. */
  (void) heap_delete_all_page_records (thread_p, &vpid, rcv->pgptr);

  /* At here, do not consider the header of heap. Later redo the update of the header of heap at RVHF_STATS log. */
  if (!is_header_page)
    {
      sp_success = spage_get_record (rcv->pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, PEEK);
      if (sp_success != SP_SUCCESS)
	{
	  /* something went wrong. Unable to redo update class_oid */
	  if (sp_success != SP_ERROR)
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	    }
	  assert (er_errid () != NO_ERROR);
	  return er_errid ();
	}

      chain = (HEAP_CHAIN *) recdes.data;
      COPY_OID (&(chain->class_oid), (OID *) (rcv->data));
      chain->max_mvccid = MVCCID_NULL;
      chain->flags = 0;
      HEAP_PAGE_SET_VACUUM_STATUS (chain, HEAP_PAGE_VACUUM_NONE);
    }

  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * heap_rv_redo_reuse_page_reuse_oid () - Redo the deletion of all objects in
 *                                        a reusable oid heap page for reuse
 *                                        purposes
 *   return: int
 *   rcv(in): Recovery structure
 */
int
heap_rv_redo_reuse_page_reuse_oid (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  RECDES recdes;
  HEAP_CHAIN *chain;		/* Chain to next and prev page */
  int sp_success;
  const bool is_header_page = ((rcv->offset != 0) ? true : false);

  (void) heap_reinitialize_page (thread_p, rcv->pgptr, is_header_page);

  (void) pgbuf_set_page_ptype (thread_p, rcv->pgptr, PAGE_HEAP);

  /* At here, do not consider the header of heap. Later redo the update of the header of heap at RVHF_STATS log. */
  if (!is_header_page)
    {
      sp_success = spage_get_record (rcv->pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, PEEK);
      if (sp_success != SP_SUCCESS)
	{
	  /* something went wrong. Unable to redo update class_oid */
	  if (sp_success != SP_ERROR)
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	    }
	  assert (er_errid () != NO_ERROR);
	  return er_errid ();
	}

      chain = (HEAP_CHAIN *) recdes.data;
      COPY_OID (&(chain->class_oid), (OID *) (rcv->data));
      chain->max_mvccid = MVCCID_NULL;
      chain->flags = 0;
      HEAP_PAGE_SET_VACUUM_STATUS (chain, HEAP_PAGE_VACUUM_NONE);
    }

  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  return NO_ERROR;
}

/*
 * heap_rv_dump_reuse_page () - Dump reuse page
 *   return: int
 *   ignore_length(in): Length of Recovery Data
 *   ignore_data(in): The data being logged
 *
 * Note: Dump information about reuse of page.
 */
void
heap_rv_dump_reuse_page (FILE * fp, int ignore_length, void *ignore_data)
{
  fprintf (fp, "Delete all objects in page for reuse purposes of page\n");
}

/*
 * heap_rv_undo_create () - Undo the creation of an heap file
 *   return: int
 *   rcv(in): Recovery structure
 *
 * Note: The heap file is destroyed completely.
 */
int
heap_rv_undo_create (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  HFID *hfid;
  int ret;

  hfid = (HFID *) rcv->data;

  ret = file_destroy (thread_p, &hfid->vfid);

  assert (ret == NO_ERROR);

  return ((ret == NO_ERROR) ? NO_ERROR : er_errid ());
}

/*
 * heap_rv_dump_create () -
 *   return: int
 *   length_ignore(in): Length of Recovery Data
 *   data(in): The data being logged
 *
 * Note: Dump the information to undo the creation of an heap file
 */
void
heap_rv_dump_create (FILE * fp, int length_ignore, void *data)
{
  VFID *vfid;

  vfid = &((HFID *) data)->vfid;
  (void) fprintf (fp, "Undo creation of Heap vfid: %d|%d\n", vfid->volid, vfid->fileid);
}

/*
 * xheap_get_class_num_objects_pages () -
 *   return: NO_ERROR
 *   hfid(in):
 *   approximation(in):
 *   nobjs(in):
 *   npages(in):
 */
int
xheap_get_class_num_objects_pages (THREAD_ENTRY * thread_p, const HFID * hfid, int approximation, int *nobjs,
				   int *npages)
{
  int length, num;
  int ret;

  assert (!HFID_IS_NULL (hfid));

  if (approximation)
    {
      num = heap_estimate (thread_p, hfid, npages, nobjs, &length);
    }
  else
    {
      num = heap_get_num_objects (thread_p, hfid, npages, nobjs, &length);
    }

  if (num < 0)
    {
      return (((ret = er_errid ()) == NO_ERROR) ? ER_FAILED : ret);
    }

  return NO_ERROR;
}

/*
 * xheap_has_instance () -
 *   return:
 *   hfid(in):
 *   class_oid(in):
 *   has_visible_instance(in): true if we need to check for a visible record
 */
int
xheap_has_instance (THREAD_ENTRY * thread_p, const HFID * hfid, OID * class_oid, int has_visible_instance)
{
  OID oid;
  HEAP_SCANCACHE scan_cache;
  RECDES recdes;
  SCAN_CODE r;
  MVCC_SNAPSHOT *mvcc_snapshot = NULL;

  OID_SET_NULL (&oid);

  if (has_visible_instance)
    {
      mvcc_snapshot = logtb_get_mvcc_snapshot (thread_p);
      if (mvcc_snapshot == NULL)
	{
	  return ER_FAILED;
	}
    }
  if (heap_scancache_start (thread_p, &scan_cache, hfid, class_oid, true, false, mvcc_snapshot) != NO_ERROR)
    {
      return ER_FAILED;
    }

  recdes.data = NULL;
  r = heap_first (thread_p, hfid, class_oid, &oid, &recdes, &scan_cache, true);
  heap_scancache_end (thread_p, &scan_cache);

  if (r == S_ERROR)
    {
      return ER_FAILED;
    }
  else if (r == S_DOESNT_EXIST || r == S_END)
    {
      return 0;
    }
  else
    {
      return 1;
    }
}

/*
 * heap_get_class_repr_id () -
 *   return:
 *   class_oid(in):
 */
REPR_ID
heap_get_class_repr_id (THREAD_ENTRY * thread_p, OID * class_oid)
{
  OR_CLASSREP *rep;
  REPR_ID id;
  int idx_incache = -1;

  if (!class_oid || !idx_incache)
    {
      return 0;
    }

  rep = heap_classrepr_get (thread_p, class_oid, NULL, NULL_REPRID, &idx_incache);
  if (rep == NULL)
    {
      return 0;
    }

  id = rep->id;
  (void) heap_classrepr_free (rep, &idx_incache);

  return id;
}

/*
 * heap_set_autoincrement_value () -
 *   return: NO_ERROR, or ER_code
 *   attr_info(in):
 *   scan_cache(in):
 *   is_set(out): 1 if at least one autoincrement value has been set
 */
int
heap_set_autoincrement_value (THREAD_ENTRY * thread_p, HEAP_CACHE_ATTRINFO * attr_info, HEAP_SCANCACHE * scan_cache,
			      int *is_set)
{
  int i, idx_in_cache;
  char *classname = NULL;
  const char *attr_name;
  RECDES recdes;		/* Used to obtain attribute name */
  char serial_name[AUTO_INCREMENT_SERIAL_NAME_MAX_LENGTH];
  HEAP_ATTRVALUE *value;
  DB_VALUE dbvalue_numeric, *dbvalue, key_val;
  OR_ATTRIBUTE *att;
  OID serial_class_oid;
  LC_FIND_CLASSNAME status;
  OR_CLASSREP *classrep;
  BTID serial_btid;
  DB_DATA_STATUS data_stat;
  HEAP_SCANCACHE local_scan_cache;
  bool use_local_scan_cache = false;
  int ret = NO_ERROR;

  if (!attr_info || !scan_cache)
    {
      return ER_FAILED;
    }

  *is_set = 0;

  recdes.data = NULL;
  recdes.area_size = 0;

  for (i = 0; i < attr_info->num_values; i++)
    {
      value = &attr_info->values[i];
      dbvalue = &value->dbvalue;
      att = &attr_info->last_classrepr->attributes[i];

      if (att->is_autoincrement && (value->state == HEAP_UNINIT_ATTRVALUE))
	{
	  if (OID_ISNULL (&(att->auto_increment.serial_obj)))
	    {
	      memset (serial_name, '\0', sizeof (serial_name));
	      recdes.data = NULL;
	      recdes.area_size = 0;

	      if (scan_cache->cache_last_fix_page == false)
		{
		  scan_cache = &local_scan_cache;
		  (void) heap_scancache_quick_start_root_hfid (thread_p, scan_cache);
		  use_local_scan_cache = true;
		}

	      if (heap_get (thread_p, &(attr_info->class_oid), &recdes, scan_cache, PEEK, NULL_CHN) != S_SUCCESS)
		{
		  ret = ER_FAILED;
		  goto exit_on_error;
		}

	      classname = heap_get_class_name (thread_p, &(att->classoid));
	      if (classname == NULL)
		{
		  ret = ER_FAILED;
		  goto exit_on_error;
		}

	      attr_name = or_get_attrname (&recdes, att->id);
	      if (attr_name == NULL)
		{
		  ret = ER_FAILED;
		  goto exit_on_error;
		}

	      SET_AUTO_INCREMENT_SERIAL_NAME (serial_name, classname, attr_name);

	      free_and_init (classname);

	      if (db_make_varchar (&key_val, DB_MAX_IDENTIFIER_LENGTH, serial_name, strlen (serial_name),
				   LANG_SYS_CODESET, LANG_SYS_COLLATION) != NO_ERROR)
		{
		  ret = ER_FAILED;
		  goto exit_on_error;
		}

	      status = xlocator_find_class_oid (thread_p, CT_SERIAL_NAME, &serial_class_oid, NULL_LOCK);
	      if (status == LC_CLASSNAME_ERROR || status == LC_CLASSNAME_DELETED)
		{
		  ret = ER_FAILED;
		  goto exit_on_error;
		}

	      classrep = heap_classrepr_get (thread_p, &serial_class_oid, NULL, NULL_REPRID, &idx_in_cache);
	      if (classrep == NULL)
		{
		  ret = ER_FAILED;
		  goto exit_on_error;
		}

	      if (classrep->indexes)
		{
		  BTREE_SEARCH ret;
		  OID serial_oid;

		  BTID_COPY (&serial_btid, &(classrep->indexes[0].btid));
		  ret =
		    xbtree_find_unique (thread_p, &serial_btid, S_SELECT, &key_val, &serial_class_oid, &serial_oid,
					false);
		  if (heap_classrepr_free (classrep, &idx_in_cache) != NO_ERROR)
		    {
		      ret = ER_FAILED;
		      goto exit_on_error;
		    }
		  if (ret != BTREE_KEY_FOUND)
		    {
		      ret = ER_FAILED;
		      goto exit_on_error;
		    }

		  assert (!OID_ISNULL (&serial_oid));
		  ATOMIC_CAS_64 ((INT64 *) (&att->auto_increment.serial_obj), *(INT64 *) (&oid_Null_oid),
				 *(INT64 *) (&serial_oid));
		}
	      else
		{
		  (void) heap_classrepr_free (classrep, &idx_in_cache);
		  ret = ER_FAILED;
		  goto exit_on_error;
		}
	    }

	  if ((att->type == DB_TYPE_SHORT) || (att->type == DB_TYPE_INTEGER) || (att->type == DB_TYPE_BIGINT))
	    {
	      if (xserial_get_next_value (thread_p, &dbvalue_numeric, &att->auto_increment.serial_obj, 0,	/* no * cache */
					  1,	/* generate one value */
					  GENERATE_AUTO_INCREMENT, false) != NO_ERROR)
		{
		  ret = ER_FAILED;
		  goto exit_on_error;
		}

	      if (numeric_db_value_coerce_from_num (&dbvalue_numeric, dbvalue, &data_stat) != NO_ERROR)
		{
		  ret = ER_FAILED;
		  goto exit_on_error;
		}
	    }
	  else if (att->type == DB_TYPE_NUMERIC)
	    {
	      if (xserial_get_next_value (thread_p, dbvalue, &att->auto_increment.serial_obj, 0,	/* no cache */
					  1,	/* generate one value */
					  GENERATE_AUTO_INCREMENT, false) != NO_ERROR)
		{
		  ret = ER_FAILED;
		  goto exit_on_error;
		}
	    }

	  *is_set = 1;
	  value->state = HEAP_READ_ATTRVALUE;
	}
    }

  if (use_local_scan_cache)
    {
      heap_scancache_end (thread_p, scan_cache);
    }

  return ret;

exit_on_error:
  if (classname != NULL)
    {
      free_and_init (classname);
    }

  if (use_local_scan_cache)
    {
      heap_scancache_end (thread_p, scan_cache);
    }
  return ret;
}

/*
 * heap_attrinfo_set_uninitialized_global () -
 *   return: NO_ERROR
 *   inst_oid(in):
 *   recdes(in):
 *   attr_info(in):
 */
int
heap_attrinfo_set_uninitialized_global (THREAD_ENTRY * thread_p, OID * inst_oid, RECDES * recdes,
					HEAP_CACHE_ATTRINFO * attr_info)
{
  if (attr_info == NULL)
    {
      return ER_FAILED;
    }

  return heap_attrinfo_set_uninitialized (thread_p, inst_oid, recdes, attr_info);
}

/*
 * heap_get_hfid_from_class_oid () - get HFID from class oid
 *   return: error_code
 *   class_oid(in): class oid
 *   hfid(out):  the resulting hfid
 */
int
heap_get_hfid_from_class_oid (THREAD_ENTRY * thread_p, const OID * class_oid, HFID * hfid)
{
  int error_code = NO_ERROR;

  error_code = heap_get_hfid_from_cache (thread_p, class_oid, hfid);
  if (error_code != NO_ERROR)
    {
      return error_code;
    }

  return error_code;
}

/*
 * heap_compact_pages () - compact all pages from hfid of specified class OID
 *   return: error_code
 *   class_oid(out):  the class oid
 */
int
heap_compact_pages (THREAD_ENTRY * thread_p, OID * class_oid)
{
  int ret = NO_ERROR;
  VPID vpid;
  VPID next_vpid;
  LOG_DATA_ADDR addr;
  int lock_timeout = 2000;
  HFID hfid;
  PGBUF_WATCHER pg_watcher;
  PGBUF_WATCHER old_pg_watcher;

  if (class_oid == NULL)
    {
      return ER_QPROC_INVALID_PARAMETER;
    }

  if (lock_object (thread_p, class_oid, oid_Root_class_oid, IS_LOCK, LK_UNCOND_LOCK) != LK_GRANTED)
    {
      return ER_FAILED;
    }

  ret = heap_get_hfid_from_class_oid (thread_p, class_oid, &hfid);
  if (ret != NO_ERROR || HFID_IS_NULL (&hfid))
    {
      lock_unlock_object (thread_p, class_oid, oid_Root_class_oid, IS_LOCK, true);
      return ret;
    }

  PGBUF_INIT_WATCHER (&pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, &hfid);
  PGBUF_INIT_WATCHER (&old_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, &hfid);

  addr.vfid = &hfid.vfid;
  addr.pgptr = NULL;
  addr.offset = 0;

  vpid.volid = hfid.vfid.volid;
  vpid.pageid = hfid.hpgid;

  if (pgbuf_ordered_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_READ, &pg_watcher) != NO_ERROR)
    {
      lock_unlock_object (thread_p, class_oid, oid_Root_class_oid, IS_LOCK, true);
      ret = ER_FAILED;
      goto exit_on_error;
    }

  (void) pgbuf_check_page_ptype (thread_p, pg_watcher.pgptr, PAGE_HEAP);

  lock_unlock_object (thread_p, class_oid, oid_Root_class_oid, IS_LOCK, true);

  /* skip header page */
  ret = heap_vpid_next (&hfid, pg_watcher.pgptr, &next_vpid);
  if (ret != NO_ERROR)
    {
      goto exit_on_error;
    }
  pgbuf_replace_watcher (thread_p, &pg_watcher, &old_pg_watcher);

  while (!VPID_ISNULL (&next_vpid))
    {
      vpid = next_vpid;
      pg_watcher.pgptr =
	heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE_PREVENT_DEALLOC, X_LOCK, NULL, &pg_watcher);
      if (old_pg_watcher.pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
	}
      if (pg_watcher.pgptr == NULL)
	{
	  ret = ER_FAILED;
	  goto exit_on_error;
	}

      ret = heap_vpid_next (&hfid, pg_watcher.pgptr, &next_vpid);
      if (ret != NO_ERROR)
	{
	  pgbuf_ordered_unfix (thread_p, &pg_watcher);
	  goto exit_on_error;
	}

      if (spage_compact (pg_watcher.pgptr) != NO_ERROR)
	{
	  pgbuf_ordered_unfix (thread_p, &pg_watcher);
	  ret = ER_FAILED;
	  goto exit_on_error;
	}

      addr.pgptr = pg_watcher.pgptr;
      log_skip_logging (thread_p, &addr);
      pgbuf_set_dirty (thread_p, pg_watcher.pgptr, DONT_FREE);
      pgbuf_replace_watcher (thread_p, &pg_watcher, &old_pg_watcher);
    }

  if (old_pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
    }
  assert (pg_watcher.pgptr == NULL);

  return ret;

exit_on_error:

  if (pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &pg_watcher);
    }
  if (old_pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
    }

  return ret;
}

/*
 * heap_classrepr_dump_all () - dump all representations belongs to a class
 *   return: none
 *   fp(in): file pointer to print out
 *   class_oid(in): class oid to be dumped
 */
void
heap_classrepr_dump_all (THREAD_ENTRY * thread_p, FILE * fp, OID * class_oid)
{
  RECDES peek_recdes;
  HEAP_SCANCACHE scan_cache;
  OR_CLASSREP **rep_all;
  int count, i;
  char *classname;
  bool need_free_classname = false;

  classname = heap_get_class_name (thread_p, class_oid);
  if (classname == NULL)
    {
      classname = (char *) "unknown";
    }
  else
    {
      need_free_classname = true;
    }

  heap_scancache_quick_start_root_hfid (thread_p, &scan_cache);

  if (heap_get (thread_p, class_oid, &peek_recdes, &scan_cache, PEEK, NULL_CHN) == S_SUCCESS)
    {
      rep_all = or_get_all_representation (&peek_recdes, true, &count);
      fprintf (fp, "*** Dumping representations of class %s\n    Classname = %s, Class-OID = %d|%d|%d, #Repr = %d\n",
	       classname, classname, (int) class_oid->volid, class_oid->pageid, (int) class_oid->slotid, count);

      for (i = 0; i < count; i++)
	{
	  assert (rep_all[i] != NULL);
	  heap_classrepr_dump (thread_p, fp, class_oid, rep_all[i]);
	  or_free_classrep (rep_all[i]);
	}

      fprintf (fp, "\n*** End of dump.\n");
      free_and_init (rep_all);
    }

  heap_scancache_end (thread_p, &scan_cache);

  if (need_free_classname)
    {
      free_and_init (classname);
    }
}

/*
 * heap_get_btid_from_index_name () - gets the BTID of an index using its name
 *				      and OID of class
 *
 *   return: NO_ERROR, or error code
 *   thread_p(in)   : thread context
 *   p_class_oid(in): OID of class
 *   index_name(in) : name of index
 *   p_found_btid(out): the BTREE ID of index
 *
 *  Note : the 'p_found_btid' argument must be a pointer to a BTID value,
 *	   the found BTID is 'BTID_COPY-ed' into it.
 *	   Null arguments are not allowed.
 *	   If an index name is not found, the 'p_found_btid' is returned as
 *	   NULL BTID and no error is set.
 *
 */
int
heap_get_btid_from_index_name (THREAD_ENTRY * thread_p, const OID * p_class_oid, const char *index_name,
			       BTID * p_found_btid)
{
  int error = NO_ERROR;
  int classrepr_cacheindex = -1;
  int idx_cnt;
  OR_CLASSREP *classrepr = NULL;
  OR_INDEX *curr_index = NULL;

  assert (p_found_btid != NULL);
  assert (p_class_oid != NULL);
  assert (index_name != NULL);

  BTID_SET_NULL (p_found_btid);

  /* get the BTID associated from the index name : the only structure containing this info is OR_CLASSREP */

  /* get class representation */
  classrepr = heap_classrepr_get (thread_p, (OID *) p_class_oid, NULL, NULL_REPRID, &classrepr_cacheindex);

  if (classrepr == NULL)
    {
      error = er_errid ();
      if (error == NO_ERROR)
	{
	  assert (error != NO_ERROR);
	  error = ER_FAILED;
	}
      goto exit;
    }

  /* iterate through indexes looking for index name */
  for (idx_cnt = 0, curr_index = classrepr->indexes; idx_cnt < classrepr->n_indexes; idx_cnt++, curr_index++)
    {
      if (curr_index == NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_UNEXPECTED, 1, "Bad index information in class representation.");
	  error = ER_UNEXPECTED;
	  goto exit_cleanup;
	}

      if (intl_identifier_casecmp (curr_index->btname, index_name) == 0)
	{
	  BTID_COPY (p_found_btid, &(curr_index->btid));
	  break;
	}
    }

exit_cleanup:
  if (classrepr)
    {
      (void) heap_classrepr_free (classrepr, &classrepr_cacheindex);
    }

exit:
  return error;
}

/*
 * heap_object_upgrade_domain - upgrades a single attibute in an instance from
 *				the domain of current representation to the
 *				domain of the last representation.
 *
 *    return: error code , NO_ERROR if no error occured
 *    thread_p(in) : thread context
 *    upd_scancache(in): scan context
 *    attr_info(in): aatribute info structure
 *    oid(in): the oid of the object to process
 *    att_id(in): attribute id within the class (same as in schema)
 *
 *  Note : this function is used in ALTER CHANGE (with type change syntax)
 */
int
heap_object_upgrade_domain (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * upd_scancache, HEAP_CACHE_ATTRINFO * attr_info,
			    OID * oid, const ATTR_ID att_id)
{
  int i = 0, error = NO_ERROR;
  int updated_flag = 0;
  HEAP_ATTRVALUE *value = NULL;
  int force_count = 0, updated_n_attrs_id = 0;
  ATTR_ID atts_id[1] = { 0 };
  DB_VALUE orig_value;

  DB_MAKE_NULL (&orig_value);

  if (upd_scancache == NULL || attr_info == NULL || oid == NULL)
    {
      error = ER_UNEXPECTED;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 1, "Unexpected NULL arguments.");
      goto exit;
    }

  for (i = 0, value = attr_info->values; i < attr_info->num_values; i++, value++)
    {
      TP_DOMAIN *dest_dom = value->last_attrepr->domain;
      bool log_warning = false;
      int warning_code = NO_ERROR;
      DB_TYPE dest_type;
      DB_TYPE src_type = DB_VALUE_DOMAIN_TYPE (&(value->dbvalue));
      int curr_prec = 0;
      int dest_prec = 0;

      dest_type = TP_DOMAIN_TYPE (dest_dom);

      if (att_id != value->attrid)
	{
	  continue;
	}

      if (QSTR_IS_BIT (src_type))
	{
	  curr_prec = DB_GET_STRING_LENGTH (&(value->dbvalue));
	}
      else if (QSTR_IS_ANY_CHAR (src_type))
	{
	  if (TP_DOMAIN_CODESET (dest_dom) == INTL_CODESET_RAW_BYTES)
	    {
	      curr_prec = DB_GET_STRING_SIZE (&(value->dbvalue));
	    }
	  else if (!DB_IS_NULL (&(value->dbvalue)))
	    {
	      curr_prec = DB_GET_STRING_LENGTH (&(value->dbvalue));
	    }
	  else
	    {
	      curr_prec = dest_dom->precision;
	    }
	}

      dest_prec = dest_dom->precision;

      if (QSTR_IS_ANY_CHAR_OR_BIT (src_type) && QSTR_IS_ANY_CHAR_OR_BIT (dest_type))
	{
	  /* check phase of ALTER TABLE .. CHANGE should not allow changing the domains from one flavour to another : */
	  assert ((QSTR_IS_CHAR (src_type) && QSTR_IS_CHAR (dest_type))
		  || (!QSTR_IS_CHAR (src_type) && !QSTR_IS_CHAR (dest_type)));

	  assert ((QSTR_IS_NATIONAL_CHAR (src_type) && QSTR_IS_NATIONAL_CHAR (dest_type))
		  || (!QSTR_IS_NATIONAL_CHAR (src_type) && !QSTR_IS_NATIONAL_CHAR (dest_type)));

	  assert ((QSTR_IS_BIT (src_type) && QSTR_IS_BIT (dest_type))
		  || (!QSTR_IS_BIT (src_type) && !QSTR_IS_BIT (dest_type)));

	  /* check string truncation */
	  if (dest_prec < curr_prec)
	    {
	      if (prm_get_bool_value (PRM_ID_ALTER_TABLE_CHANGE_TYPE_STRICT) == true)
		{
		  error = ER_ALTER_CHANGE_TRUNC_OVERFLOW_NOT_ALLOWED;
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 0);
		  goto exit;
		}
	      else
		{
		  /* allow truncation in cast, just warning */
		  log_warning = true;
		  warning_code = ER_QPROC_SIZE_STRING_TRUNCATED;
		}
	    }
	}

      error = pr_clone_value (&(value->dbvalue), &orig_value);
      if (error != NO_ERROR)
	{
	  goto exit;
	}

      if (TP_IS_CHAR_TYPE (TP_DOMAIN_TYPE (dest_dom))
	  && !(TP_IS_CHAR_TYPE (src_type) || src_type == DB_TYPE_ENUMERATION)
	  && prm_get_bool_value (PRM_ID_ALTER_TABLE_CHANGE_TYPE_STRICT) == false)
	{
	  /* If destination is char/varchar, we need to first cast the value to a string with no precision, then to
	   * destination type with the desired precision. */
	  TP_DOMAIN *string_dom;
	  if (TP_DOMAIN_TYPE (dest_dom) == DB_TYPE_NCHAR || TP_DOMAIN_TYPE (dest_dom) == DB_TYPE_VARNCHAR)
	    {
	      string_dom = tp_domain_resolve_default (DB_TYPE_VARNCHAR);
	    }
	  else
	    {
	      string_dom = tp_domain_resolve_default (DB_TYPE_VARCHAR);
	    }
	  error = db_value_coerce (&(value->dbvalue), &(value->dbvalue), string_dom);
	}

      if (error == NO_ERROR)
	{
	  error = db_value_coerce (&(value->dbvalue), &(value->dbvalue), dest_dom);
	}
      if (error != NO_ERROR)
	{
	  bool set_default_value = false;
	  bool set_min_value = false;
	  bool set_max_value = false;

	  if (prm_get_bool_value (PRM_ID_ALTER_TABLE_CHANGE_TYPE_STRICT) == true)
	    {
	      error = ER_ALTER_CHANGE_TRUNC_OVERFLOW_NOT_ALLOWED;
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 0);
	      goto exit;
	    }

	  if (error == ER_IT_DATA_OVERFLOW)
	    {
	      int is_positive = -1;	/* -1:UNKNOWN, 0:negative, 1:positive */

	      /* determine sign of orginal value: */
	      switch (src_type)
		{
		case DB_TYPE_INTEGER:
		  is_positive = ((DB_GET_INTEGER (&value->dbvalue) >= 0) ? 1 : 0);
		  break;
		case DB_TYPE_SMALLINT:
		  is_positive = ((DB_GET_SHORT (&value->dbvalue) >= 0) ? 1 : 0);
		  break;
		case DB_TYPE_BIGINT:
		  is_positive = ((DB_GET_BIGINT (&value->dbvalue) >= 0) ? 1 : 0);
		  break;
		case DB_TYPE_FLOAT:
		  is_positive = ((DB_GET_FLOAT (&value->dbvalue) >= 0) ? 1 : 0);
		  break;
		case DB_TYPE_DOUBLE:
		  is_positive = ((DB_GET_DOUBLE (&value->dbvalue) >= 0) ? 1 : 0);
		  break;
		case DB_TYPE_NUMERIC:
		  is_positive = numeric_db_value_is_positive (&value->dbvalue);
		  break;
		case DB_TYPE_MONETARY:
		  is_positive = ((DB_GET_MONETARY (&value->dbvalue)->amount >= 0) ? 1 : 0);
		  break;

		case DB_TYPE_CHAR:
		case DB_TYPE_VARCHAR:
		case DB_TYPE_NCHAR:
		case DB_TYPE_VARNCHAR:
		  {
		    char *str = DB_PULL_STRING (&(value->dbvalue));
		    char *str_end = str + DB_GET_STRING_LENGTH (&(value->dbvalue));
		    char *p = NULL;

		    /* get the sign in the source string; look directly into the buffer string, no copy */
		    p = str;
		    while (char_isspace (*p) && p < str_end)
		      {
			p++;
		      }

		    is_positive = ((p < str_end && (*p) == '-') ? 0 : 1);
		    break;
		  }

		default:
		  is_positive = -1;
		  break;
		}

	      if (is_positive == 1)
		{
		  set_max_value = true;
		}
	      else if (is_positive == 0)
		{
		  set_min_value = true;
		}
	      else
		{
		  set_default_value = true;
		}
	    }
	  else
	    {
	      set_default_value = true;
	    }

	  log_warning = true;

	  /* the casted value will be overwriten, so a clear is needed, here */
	  pr_clear_value (&(value->dbvalue));

	  if (set_max_value)
	    {
	      /* set max value of destination domain */
	      error =
		db_value_domain_max (&(value->dbvalue), dest_type, dest_prec, dest_dom->scale, dest_dom->codeset,
				     dest_dom->collation_id, &dest_dom->enumeration);
	      if (error != NO_ERROR)
		{
		  /* this should not happen */
		  goto exit;
		}

	      warning_code = ER_ALTER_CHANGE_CAST_FAILED_SET_MAX;
	    }
	  else if (set_min_value)
	    {
	      /* set min value of destination domain */
	      error =
		db_value_domain_min (&(value->dbvalue), dest_type, dest_prec, dest_dom->scale, dest_dom->codeset,
				     dest_dom->collation_id, &dest_dom->enumeration);
	      if (error != NO_ERROR)
		{
		  /* this should not happen */
		  goto exit;
		}
	      warning_code = ER_ALTER_CHANGE_CAST_FAILED_SET_MIN;
	    }
	  else
	    {
	      assert (set_default_value == true);

	      /* set default value of destination domain */
	      error =
		db_value_domain_default (&(value->dbvalue), dest_type, dest_prec, dest_dom->scale, dest_dom->codeset,
					 dest_dom->collation_id, &dest_dom->enumeration);
	      if (error != NO_ERROR)
		{
		  /* this should not happen */
		  goto exit;
		}
	      warning_code = ER_ALTER_CHANGE_CAST_FAILED_SET_DEFAULT;
	    }
	}

      if (!DB_IS_NULL (&orig_value))
	{
	  assert (!DB_IS_NULL (&(value->dbvalue)));
	}

      if (log_warning)
	{
	  assert (warning_code != NO_ERROR);

	  /* Since we don't like to bother callers with the following warning which is just for a logging, it will be
	   * poped once it is set. */
	  er_stack_push ();

	  if (warning_code == ER_QPROC_SIZE_STRING_TRUNCATED)
	    {
	      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, warning_code, 1, "ALTER TABLE .. CHANGE");
	    }
	  else
	    {
	      er_set (ER_WARNING_SEVERITY, ARG_FILE_LINE, warning_code, 0);
	    }

	  /* forget the warning */
	  er_stack_pop ();
	}

      value->state = HEAP_WRITTEN_ATTRVALUE;
      atts_id[updated_n_attrs_id] = value->attrid;
      updated_n_attrs_id++;

      break;
    }

  /* exactly one attribute should be changed */
  assert (updated_n_attrs_id == 1);

  if (updated_n_attrs_id != 1 || attr_info->read_classrepr == NULL || attr_info->last_classrepr == NULL
      || attr_info->read_classrepr->id >= attr_info->last_classrepr->id)
    {
      error = ER_UNEXPECTED;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 1, "Incorrect attribute information.");
      goto exit;
    }

  /* the class has XCH_M_LOCK */
  error =
    locator_attribute_info_force (thread_p, &upd_scancache->node.hfid, oid, attr_info, atts_id, updated_n_attrs_id,
				  LC_FLUSH_UPDATE, SINGLE_ROW_UPDATE, upd_scancache, &force_count, false,
				  REPL_INFO_TYPE_RBR_NORMAL, DB_NOT_PARTITIONED_CLASS, NULL, NULL, NULL,
				  UPDATE_INPLACE_OLD_MVCCID, NULL, false);
  if (error != NO_ERROR)
    {
      if (error == ER_MVCC_NOT_SATISFIED_REEVALUATION)
	{
	  error = NO_ERROR;
	}

      goto exit;
    }

exit:
  pr_clear_value (&orig_value);
  return error;
}

/*
 * heap_eval_function_index - evaluate the result of the expression used in
 *			      a function index.
 *
 *    thread_p(in) : thread context
 *    func_index_info(in): function index information
 *    n_atts(in): number of attributes involved
 *    att_ids(in): attribute identifiers
 *    attr_info(in): attribute info structure
 *    recdes(in): record descriptor
 *    btid_index(in): id of the function index used
 *    func_pred_cache(in): cached function index expressions
 *    result(out): result of the function expression
 *    fi_domain(out): domain of function index (from regu_var)
 *    return: error code
 */
static int
heap_eval_function_index (THREAD_ENTRY * thread_p, FUNCTION_INDEX_INFO * func_index_info, int n_atts, int *att_ids,
			  HEAP_CACHE_ATTRINFO * attr_info, RECDES * recdes, int btid_index, DB_VALUE * result,
			  FUNC_PRED_UNPACK_INFO * func_pred_cache, TP_DOMAIN ** fi_domain)
{
  int error = NO_ERROR;
  OR_INDEX *index = NULL;
  char *expr_stream = NULL;
  int expr_stream_size = 0;
  FUNC_PRED *func_pred = NULL;
  void *unpack_info = NULL;
  DB_VALUE *res = NULL;
  int i, nr_atts;
  ATTR_ID *atts = NULL;
  bool atts_free = false, attrinfo_clear = false, attrinfo_end = false;
  HEAP_CACHE_ATTRINFO *cache_attr_info = NULL;

  if (func_index_info == NULL && btid_index > -1 && n_atts == -1)
    {
      index = &(attr_info->last_classrepr->indexes[btid_index]);
      if (func_pred_cache)
	{
	  func_pred = func_pred_cache->func_pred;
	  cache_attr_info = func_pred->cache_attrinfo;
	  nr_atts = index->n_atts;
	}
      else
	{
	  expr_stream = index->func_index_info->expr_stream;
	  expr_stream_size = index->func_index_info->expr_stream_size;
	  nr_atts = index->n_atts;
	  atts = (ATTR_ID *) malloc (nr_atts * sizeof (ATTR_ID));
	  if (atts == NULL)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, nr_atts * sizeof (ATTR_ID));
	      error = ER_FAILED;
	      goto end;
	    }
	  atts_free = true;
	  for (i = 0; i < nr_atts; i++)
	    {
	      atts[i] = index->atts[i]->id;
	    }
	  cache_attr_info = attr_info;
	}
    }
  else
    {
      /* load index case */
      expr_stream = func_index_info->expr_stream;
      expr_stream_size = func_index_info->expr_stream_size;
      nr_atts = n_atts;
      atts = att_ids;
      cache_attr_info = ((FUNC_PRED *) func_index_info->expr)->cache_attrinfo;
      func_pred = (FUNC_PRED *) func_index_info->expr;
    }

  if (func_index_info == NULL)
    {
      /* insert case, read the values */
      if (func_pred == NULL)
	{
	  if (stx_map_stream_to_func_pred (NULL, (FUNC_PRED **) (&func_pred), expr_stream, expr_stream_size,
					   &unpack_info))
	    {
	      error = ER_FAILED;
	      goto end;
	    }
	  cache_attr_info = func_pred->cache_attrinfo;

	  if (heap_attrinfo_start (thread_p, &attr_info->class_oid, nr_atts, atts, cache_attr_info) != NO_ERROR)
	    {
	      error = ER_FAILED;
	      goto end;
	    }
	  attrinfo_end = true;
	}

      if (heap_attrinfo_read_dbvalues (thread_p, &attr_info->inst_oid, recdes, NULL, cache_attr_info) != NO_ERROR)
	{
	  error = ER_FAILED;
	  goto end;
	}
      attrinfo_clear = true;
    }

  error =
    fetch_peek_dbval (thread_p, func_pred->func_regu, NULL, &cache_attr_info->class_oid, &cache_attr_info->inst_oid,
		      NULL, &res);
  if (error == NO_ERROR)
    {
      pr_clone_value (res, result);
    }

  if (fi_domain != NULL)
    {
      *fi_domain = tp_domain_cache (func_pred->func_regu->domain);
    }

end:
  if (attrinfo_clear && cache_attr_info)
    {
      heap_attrinfo_clear_dbvalues (cache_attr_info);
    }
  if (attrinfo_end && cache_attr_info)
    {
      heap_attrinfo_end (thread_p, cache_attr_info);
    }
  if (atts_free && atts)
    {
      free_and_init (atts);
    }
  if (unpack_info)
    {
      (void) qexec_clear_func_pred (thread_p, func_pred);
      stx_free_additional_buff (thread_p, unpack_info);
      stx_free_xasl_unpack_info (unpack_info);
      db_private_free_and_init (thread_p, unpack_info);
    }

  return error;
}

/*
 * heap_init_func_pred_unpack_info () - if function indexes are found,
 *			each function expression is unpacked and cached
 *			in order to be used during bulk inserts
 *			(insert ... select).
 *   return: NO_ERROR, or ER_FAILED
 *   thread_p(in): thread entry
 *   attr_info(in): heap_cache_attrinfo
 *   class_oid(in): the class oid
 *   func_indx_preds(out):
 */
int
heap_init_func_pred_unpack_info (THREAD_ENTRY * thread_p, HEAP_CACHE_ATTRINFO * attr_info, const OID * class_oid,
				 FUNC_PRED_UNPACK_INFO ** func_indx_preds)
{
  OR_FUNCTION_INDEX *fi_info = NULL;
  int n_indexes;
  int i, j;
  int *att_ids = NULL;
  int error_status = NO_ERROR;
  OR_INDEX *idx;
  FUNC_PRED_UNPACK_INFO *fi_preds = NULL;
  int *attr_info_started = NULL;
  size_t size;

  if (attr_info == NULL || class_oid == NULL || func_indx_preds == NULL)
    {
      return ER_FAILED;
    }

  *func_indx_preds = NULL;

  n_indexes = attr_info->last_classrepr->n_indexes;
  for (i = 0; i < n_indexes; i++)
    {
      idx = &(attr_info->last_classrepr->indexes[i]);
      fi_info = idx->func_index_info;
      if (fi_info)
	{
	  if (fi_preds == NULL)
	    {
	      size = n_indexes * sizeof (FUNC_PRED_UNPACK_INFO);
	      fi_preds = (FUNC_PRED_UNPACK_INFO *) db_private_alloc (thread_p, size);
	      if (!fi_preds)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, size);
		  error_status = ER_FAILED;
		  goto error;
		}
	      for (j = 0; j < n_indexes; j++)
		{
		  fi_preds[j].func_pred = NULL;
		  fi_preds[j].unpack_info = NULL;
		}

	      size = n_indexes * sizeof (int);
	      attr_info_started = (int *) db_private_alloc (thread_p, size);
	      if (attr_info_started == NULL)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, size);
		  error_status = ER_FAILED;
		  goto error;
		}
	      for (j = 0; j < n_indexes; j++)
		{
		  attr_info_started[j] = 0;
		}
	    }

	  if (stx_map_stream_to_func_pred (thread_p, (FUNC_PRED **) (&(fi_preds[i].func_pred)), fi_info->expr_stream,
					   fi_info->expr_stream_size, &(fi_preds[i].unpack_info)))
	    {
	      error_status = ER_FAILED;
	      goto error;
	    }

	  size = idx->n_atts * sizeof (ATTR_ID);
	  att_ids = (ATTR_ID *) db_private_alloc (thread_p, size);
	  if (!att_ids)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, size);
	      error_status = ER_FAILED;
	      goto error;
	    }

	  for (j = 0; j < idx->n_atts; j++)
	    {
	      att_ids[j] = idx->atts[j]->id;
	    }

	  if (heap_attrinfo_start (thread_p, class_oid, idx->n_atts, att_ids,
				   ((FUNC_PRED *) fi_preds[i].func_pred)->cache_attrinfo) != NO_ERROR)
	    {
	      error_status = ER_FAILED;
	      goto error;
	    }

	  attr_info_started[i] = 1;

	  if (att_ids)
	    {
	      db_private_free_and_init (thread_p, att_ids);
	    }
	}
    }

  if (attr_info_started != NULL)
    {
      db_private_free_and_init (thread_p, attr_info_started);
    }

  *func_indx_preds = fi_preds;

  return NO_ERROR;

error:
  if (att_ids)
    {
      db_private_free_and_init (thread_p, att_ids);
    }
  heap_free_func_pred_unpack_info (thread_p, n_indexes, fi_preds, attr_info_started);
  if (attr_info_started != NULL)
    {
      db_private_free_and_init (thread_p, attr_info_started);
    }

  return error_status;
}

/*
 * heap_free_func_pred_unpack_info () -
 *   return:
 *   thread_p(in): thread entry
 *   n_indexes(in): number of indexes
 *   func_indx_preds(in):
 *   attr_info_started(in): array of int (1 if corresponding cache_attrinfo
 *					  must be cleaned, 0 otherwise)
 *			    if null all cache_attrinfo must be cleaned
 */
void
heap_free_func_pred_unpack_info (THREAD_ENTRY * thread_p, int n_indexes, FUNC_PRED_UNPACK_INFO * func_indx_preds,
				 int *attr_info_started)
{
  int i;

  if (func_indx_preds == NULL)
    {
      return;
    }

  for (i = 0; i < n_indexes; i++)
    {
      if (func_indx_preds[i].func_pred)
	{
	  if (attr_info_started == NULL || attr_info_started[i])
	    {
	      assert (((FUNC_PRED *) func_indx_preds[i].func_pred)->cache_attrinfo);
	      (void) heap_attrinfo_end (thread_p, ((FUNC_PRED *) func_indx_preds[i].func_pred)->cache_attrinfo);
	    }
	  (void) qexec_clear_func_pred (thread_p, (FUNC_PRED *) func_indx_preds[i].func_pred);
	}

      if (func_indx_preds[i].unpack_info)
	{
	  stx_free_additional_buff (thread_p, func_indx_preds[i].unpack_info);
	  stx_free_xasl_unpack_info (func_indx_preds[i].unpack_info);
	  db_private_free_and_init (thread_p, func_indx_preds[i].unpack_info);
	}
    }
  db_private_free_and_init (thread_p, func_indx_preds);
}

/*
 * heap_header_capacity_start_scan () - start scan function for 'show heap ...'
 *   return: NO_ERROR, or ER_code
 *   thread_p(in): thread entry
 *   show_type(in):
 *   arg_values(in):
 *   arg_cnt(in):
 *   ptr(in/out): 'show heap' context
 */
int
heap_header_capacity_start_scan (THREAD_ENTRY * thread_p, int show_type, DB_VALUE ** arg_values, int arg_cnt,
				 void **ptr)
{
  int error = NO_ERROR;
  char *class_name = NULL;
  DB_CLASS_PARTITION_TYPE partition_type = DB_NOT_PARTITIONED_CLASS;
  OID class_oid;
  LC_FIND_CLASSNAME status;
  HEAP_SHOW_SCAN_CTX *ctx = NULL;
  OR_PARTITION *parts = NULL;
  int i = 0;
  int parts_count = 0;
  bool is_all = false;

  assert (arg_cnt == 2);
  assert (DB_VALUE_TYPE (arg_values[0]) == DB_TYPE_CHAR);
  assert (DB_VALUE_TYPE (arg_values[1]) == DB_TYPE_INTEGER);

  *ptr = NULL;

  class_name = db_get_string (arg_values[0]);

  partition_type = (DB_CLASS_PARTITION_TYPE) db_get_int (arg_values[1]);

  ctx = (HEAP_SHOW_SCAN_CTX *) db_private_alloc (thread_p, sizeof (HEAP_SHOW_SCAN_CTX));
  if (ctx == NULL)
    {
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      goto cleanup;
    }
  memset (ctx, 0, sizeof (HEAP_SHOW_SCAN_CTX));

  status = xlocator_find_class_oid (thread_p, class_name, &class_oid, S_LOCK);
  if (status == LC_CLASSNAME_ERROR || status == LC_CLASSNAME_DELETED)
    {
      error = ER_LC_UNKNOWN_CLASSNAME;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 1, class_name);
      goto cleanup;
    }

  is_all = (show_type == SHOWSTMT_ALL_HEAP_HEADER || show_type == SHOWSTMT_ALL_HEAP_CAPACITY);

  if (is_all && partition_type == DB_PARTITIONED_CLASS)
    {
      error = heap_get_class_partitions (thread_p, &class_oid, &parts, &parts_count);
      if (error != NO_ERROR)
	{
	  goto cleanup;
	}

      ctx->hfids = (HFID *) db_private_alloc (thread_p, parts_count * sizeof (HFID));
      if (ctx->hfids == NULL)
	{
	  assert (er_errid () != NO_ERROR);
	  error = er_errid ();
	  goto cleanup;
	}

      for (i = 0; i < parts_count; i++)
	{
	  HFID_COPY (&ctx->hfids[i], &parts[i].class_hfid);
	}

      ctx->hfids_count = parts_count;
    }
  else
    {
      ctx->hfids = (HFID *) db_private_alloc (thread_p, sizeof (HFID));
      if (ctx->hfids == NULL)
	{
	  assert (er_errid () != NO_ERROR);
	  error = er_errid ();
	  goto cleanup;
	}

      error = heap_get_hfid_from_class_oid (thread_p, &class_oid, &ctx->hfids[0]);
      if (error != NO_ERROR)
	{
	  goto cleanup;
	}

      ctx->hfids_count = 1;
    }

  *ptr = ctx;
  ctx = NULL;

cleanup:

  if (parts != NULL)
    {
      heap_clear_partition_info (thread_p, parts, parts_count);
    }

  if (ctx != NULL)
    {
      if (ctx->hfids != NULL)
	{
	  db_private_free (thread_p, ctx->hfids);
	}

      db_private_free_and_init (thread_p, ctx);
    }

  return error;
}

/*
 * heap_header_next_scan () - next scan function for
 *                            'show (all) heap header'
 *   return: NO_ERROR, or ER_code
 *   thread_p(in):
 *   cursor(in):
 *   out_values(in/out):
 *   out_cnt(in):
 *   ptr(in): 'show heap' context
 */
SCAN_CODE
heap_header_next_scan (THREAD_ENTRY * thread_p, int cursor, DB_VALUE ** out_values, int out_cnt, void *ptr)
{
  int error = NO_ERROR;
  HEAP_SHOW_SCAN_CTX *ctx = NULL;
  VPID vpid;
  HEAP_HDR_STATS *heap_hdr = NULL;
  RECDES hdr_recdes;
  int i = 0;
  int idx = 0;
  PAGE_PTR pgptr = NULL;
  HFID *hfid_p;
  char *class_name = NULL;
  int avg_length = 0;
  char buf[512] = { 0 };
  char temp[64] = { 0 };
  char *buf_p, *end;

  ctx = (HEAP_SHOW_SCAN_CTX *) ptr;

  if (cursor >= ctx->hfids_count)
    {
      return S_END;
    }

  hfid_p = &ctx->hfids[cursor];

  vpid.volid = hfid_p->vfid.volid;
  vpid.pageid = hfid_p->hpgid;

  pgptr = heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, S_LOCK, NULL, NULL);
  if (pgptr == NULL)
    {
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      goto cleanup;
    }

  if (spage_get_record (pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes, PEEK) != S_SUCCESS)
    {
      error = ER_SP_INVALID_HEADER;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 3, vpid.pageid, fileio_get_volume_label (vpid.volid, PEEK), 0);
      goto cleanup;
    }

  heap_hdr = (HEAP_HDR_STATS *) hdr_recdes.data;

  class_name = heap_get_class_name (thread_p, &(heap_hdr->class_oid));
  if (class_name == NULL)
    {
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      goto cleanup;
    }

  idx = 0;

  /* Class_name */
  error = db_make_string_copy (out_values[idx], class_name);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  /* Class_oid */
  oid_to_string (buf, sizeof (buf), &heap_hdr->class_oid);
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  /* HFID */
  db_make_int (out_values[idx], hfid_p->vfid.volid);
  idx++;

  db_make_int (out_values[idx], hfid_p->vfid.fileid);
  idx++;

  db_make_int (out_values[idx], hfid_p->hpgid);
  idx++;

  /* Overflow_vfid */
  vfid_to_string (buf, sizeof (buf), &heap_hdr->ovf_vfid);
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  /* Next_vpid */
  vpid_to_string (buf, sizeof (buf), &heap_hdr->next_vpid);
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  /* Unfill space */
  db_make_int (out_values[idx], heap_hdr->unfill_space);
  idx++;

  /* Estimated */
  db_make_bigint (out_values[idx], heap_hdr->estimates.num_pages);
  idx++;

  db_make_bigint (out_values[idx], heap_hdr->estimates.num_recs);
  idx++;

  avg_length =
    ((heap_hdr->estimates.num_recs >
      0) ? (int) ((heap_hdr->estimates.recs_sumlen / (float) heap_hdr->estimates.num_recs) + 0.9) : 0);
  db_make_int (out_values[idx], avg_length);
  idx++;

  db_make_int (out_values[idx], heap_hdr->estimates.num_high_best);
  idx++;

  db_make_int (out_values[idx], heap_hdr->estimates.num_other_high_best);
  idx++;

  db_make_int (out_values[idx], heap_hdr->estimates.head);
  idx++;

  /* Estimates_best_list */
  buf_p = buf;
  end = buf + sizeof (buf);
  for (i = 0; i < HEAP_NUM_BEST_SPACESTATS; i++)
    {
      if (i > 0)
	{
	  if (fill_string_to_buffer (&buf_p, end, ", ") == -1)
	    {
	      break;
	    }
	}

      heap_bestspace_to_string (temp, sizeof (temp), heap_hdr->estimates.best + i);
      if (fill_string_to_buffer (&buf_p, end, temp) == -1)
	{
	  break;
	}
    }

  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  db_make_int (out_values[idx], heap_hdr->estimates.num_second_best);
  idx++;

  db_make_int (out_values[idx], heap_hdr->estimates.head_second_best);
  idx++;

  db_make_int (out_values[idx], heap_hdr->estimates.tail_second_best);
  idx++;

  db_make_int (out_values[idx], heap_hdr->estimates.num_substitutions);
  idx++;

  /* Estimates_second_best */
  buf_p = buf;
  end = buf + sizeof (buf);
  for (i = 0; i < HEAP_NUM_BEST_SPACESTATS; i++)
    {
      if (i > 0)
	{
	  if (fill_string_to_buffer (&buf_p, end, ", ") == -1)
	    {
	      break;
	    }
	}

      vpid_to_string (temp, sizeof (temp), heap_hdr->estimates.second_best + i);
      if (fill_string_to_buffer (&buf_p, end, temp) == -1)
	{
	  break;
	}
    }

  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  vpid_to_string (buf, sizeof (buf), &heap_hdr->estimates.last_vpid);
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  vpid_to_string (buf, sizeof (buf), &heap_hdr->estimates.full_search_vpid);
  error = db_make_string_copy (out_values[idx], buf);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  assert (idx == out_cnt);

cleanup:

  if (pgptr != NULL)
    {
      pgbuf_unfix_and_init (thread_p, pgptr);
    }

  if (class_name != NULL)
    {
      free_and_init (class_name);
    }

  return (error == NO_ERROR) ? S_SUCCESS : S_ERROR;
}

/*
 * heap_capacity_next_scan () - next scan function for
 *                              'show (all) heap capacity'
 *   return: NO_ERROR, or ER_code
 *   thread_p(in):
 *   cursor(in):
 *   out_values(in/out):
 *   out_cnt(in):
 *   ptr(in): 'show heap' context
 */
SCAN_CODE
heap_capacity_next_scan (THREAD_ENTRY * thread_p, int cursor, DB_VALUE ** out_values, int out_cnt, void *ptr)
{
  int error = NO_ERROR;
  HEAP_SHOW_SCAN_CTX *ctx = NULL;
  HFID *hfid_p = NULL;
  FILE_HEAP_DES hfdes;
  HEAP_CACHE_ATTRINFO attr_info;
  OR_CLASSREP *repr = NULL;
  char *classname = NULL;
  char class_oid_str[64] = { 0 };
  bool is_heap_attrinfo_started = false;
  INT64 num_recs = 0;
  INT64 num_relocated_recs = 0;
  INT64 num_overflowed_recs = 0;
  INT64 num_pages = 0;
  int avg_rec_len = 0;
  int avg_free_space_per_page = 0;
  int avg_free_space_without_last_page = 0;
  int avg_overhead_per_page = 0;
  int val = 0;
  int idx = 0;

  ctx = (HEAP_SHOW_SCAN_CTX *) ptr;

  if (cursor >= ctx->hfids_count)
    {
      return S_END;
    }

  hfid_p = &ctx->hfids[cursor];

  error =
    heap_get_capacity (thread_p, hfid_p, &num_recs, &num_relocated_recs, &num_overflowed_recs, &num_pages,
		       &avg_free_space_per_page, &avg_free_space_without_last_page, &avg_rec_len,
		       &avg_overhead_per_page);
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  if (file_get_descriptor (thread_p, &hfid_p->vfid, &hfdes, sizeof (FILE_HEAP_DES)) != sizeof (FILE_HEAP_DES))
    {
      error = ER_FILE_INCONSISTENT_HEADER;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 3, hfid_p->vfid.volid, hfid_p->vfid.fileid,
	      fileio_get_volume_label (hfid_p->vfid.volid, PEEK));
      goto cleanup;
    }

  error = heap_attrinfo_start (thread_p, &hfdes.class_oid, -1, NULL, &attr_info);
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  is_heap_attrinfo_started = true;

  repr = attr_info.last_classrepr;
  if (repr == NULL)
    {
      error = ER_HEAP_UNKNOWN_OBJECT;
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 3, hfdes.class_oid.volid, hfdes.class_oid.pageid,
	      hfdes.class_oid.slotid);
      goto cleanup;
    }

  classname = heap_get_class_name (thread_p, &hfdes.class_oid);
  if (classname == NULL)
    {
      assert (er_errid () != NO_ERROR);
      error = er_errid ();
      goto cleanup;
    }

  idx = 0;

  error = db_make_string_copy (out_values[idx], classname);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  oid_to_string (class_oid_str, sizeof (class_oid_str), &hfdes.class_oid);
  error = db_make_string_copy (out_values[idx], class_oid_str);
  idx++;
  if (error != NO_ERROR)
    {
      goto cleanup;
    }

  db_make_int (out_values[idx], hfid_p->vfid.volid);
  idx++;

  db_make_int (out_values[idx], hfid_p->vfid.fileid);
  idx++;

  db_make_int (out_values[idx], hfid_p->hpgid);
  idx++;

  db_make_bigint (out_values[idx], num_recs);
  idx++;

  db_make_bigint (out_values[idx], num_relocated_recs);
  idx++;

  db_make_bigint (out_values[idx], num_overflowed_recs);
  idx++;

  db_make_bigint (out_values[idx], num_pages);
  idx++;

  db_make_int (out_values[idx], avg_rec_len);
  idx++;

  db_make_int (out_values[idx], avg_free_space_per_page);
  idx++;

  db_make_int (out_values[idx], avg_free_space_without_last_page);
  idx++;

  db_make_int (out_values[idx], avg_overhead_per_page);
  idx++;

  db_make_int (out_values[idx], repr->id);
  idx++;

  db_make_int (out_values[idx], repr->n_attributes);
  idx++;

  val = repr->n_attributes - repr->n_variable - repr->n_shared_attrs - repr->n_class_attrs;
  db_make_int (out_values[idx], val);
  idx++;

  db_make_int (out_values[idx], repr->n_variable);
  idx++;

  db_make_int (out_values[idx], repr->n_shared_attrs);
  idx++;

  db_make_int (out_values[idx], repr->n_class_attrs);
  idx++;

  db_make_int (out_values[idx], repr->fixed_length);
  idx++;

  assert (idx == out_cnt);

cleanup:

  if (classname != NULL)
    {
      free_and_init (classname);
    }

  if (is_heap_attrinfo_started)
    {
      heap_attrinfo_end (thread_p, &attr_info);
    }

  return (error == NO_ERROR) ? S_SUCCESS : S_ERROR;
}

/*
 *  heap_header_capacity_end_scan() - end scan function of
 *                                    'show (all) heap ...'
 *   return: NO_ERROR, or ER_code
 *   thread_p(in):
 *   ptr(in/out): 'show heap' context
 */
int
heap_header_capacity_end_scan (THREAD_ENTRY * thread_p, void **ptr)
{
  HEAP_SHOW_SCAN_CTX *ctx;

  ctx = (HEAP_SHOW_SCAN_CTX *) (*ptr);

  if (ctx == NULL)
    {
      return NO_ERROR;
    }

  if (ctx->hfids != NULL)
    {
      db_private_free (thread_p, ctx->hfids);
    }

  db_private_free (thread_p, ctx);
  *ptr = NULL;

  return NO_ERROR;
}

static char *
heap_bestspace_to_string (char *buf, int buf_size, const HEAP_BESTSPACE * hb)
{
  snprintf (buf, buf_size, "((%d|%d), %d)", hb->vpid.volid, hb->vpid.pageid, hb->freespace);
  buf[buf_size - 1] = '\0';

  return buf;
}

/*
 * fill_string_to_buffer () - fill string into buffer
 *
 *   -----------------------------
 *   |        buffer             |
 *   -----------------------------
 *   ^                           ^
 *   |                           |
 *   start                       end
 *
 *   return: the count of characters (not include '\0') which has been
 *           filled into buffer; -1 means error.
 *   start(in/out): After filling, start move to the '\0' position.
 *   end(in): The first unavailble position.
 *   str(in):
 */
static int
fill_string_to_buffer (char **start, char *end, const char *str)
{
  int len = strlen (str);

  if (*start + len >= end)
    {
      return -1;
    }

  memcpy (*start, str, len);
  *start += len;
  **start = '\0';

  return len;
}

/*
 * heap_get_page_info () - Obtain page information.
 *
 * return	  : SCAN_CODE.
 * thread_p (in)  : Thread entry.
 * cls_oid (in)	  : Class object identifier.
 * hfid (in)	  : Heap file identifier.
 * vpid (in)	  : Page identifier.
 * pgptr (in)	  : Pointer to the cached page.
 * page_info (in) : Pointers to DB_VALUES where page information is stored.
 */
static SCAN_CODE
heap_get_page_info (THREAD_ENTRY * thread_p, const OID * cls_oid, const HFID * hfid, const VPID * vpid,
		    const PAGE_PTR pgptr, DB_VALUE ** page_info)
{
  RECDES recdes;

  if (page_info == NULL)
    {
      /* no need to get page info */
      return S_SUCCESS;
    }

  if (spage_get_record (pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &recdes, PEEK) != S_SUCCESS)
    {
      /* Error obtaining header slot */
      return S_ERROR;
    }

  DB_MAKE_OID (page_info[HEAP_PAGE_INFO_CLASS_OID], cls_oid);

  if (hfid->hpgid == vpid->pageid && hfid->vfid.volid == vpid->volid)
    {
      HEAP_HDR_STATS *hdr_stats = (HEAP_HDR_STATS *) recdes.data;
      DB_MAKE_NULL (page_info[HEAP_PAGE_INFO_PREV_PAGE]);
      DB_MAKE_INT (page_info[HEAP_PAGE_INFO_NEXT_PAGE], hdr_stats->next_vpid.pageid);
    }
  else
    {
      HEAP_CHAIN *chain = (HEAP_CHAIN *) recdes.data;
      DB_MAKE_INT (page_info[HEAP_PAGE_INFO_PREV_PAGE], chain->prev_vpid.pageid);
      DB_MAKE_INT (page_info[HEAP_PAGE_INFO_NEXT_PAGE], chain->next_vpid.pageid);
    }

  /* Obtain information from spage header */
  return spage_get_page_header_info (pgptr, page_info);
}

/*
 * heap_page_next () - Advance to next page in chain and obtain information.
 *
 * return	       : SCAN_CODE.
 * thread_p (in)       : Thread entry.
 * class_oid (in)      : Class object identifier.
 * hfid (in)	       : Heap file identifier.
 * next_vpid (in)      : Next page identifier.
 * cache_pageinfo (in) : Pointers to DB_VALUEs where page information is
 *			 stored.
 */
SCAN_CODE
heap_page_next (THREAD_ENTRY * thread_p, const OID * class_oid, const HFID * hfid, VPID * next_vpid,
		DB_VALUE ** cache_pageinfo)
{
  PGBUF_WATCHER pg_watcher;
  PGBUF_WATCHER old_pg_watcher;
  SCAN_CODE scan = S_SUCCESS;

  PGBUF_INIT_WATCHER (&pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  PGBUF_INIT_WATCHER (&old_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);

  /* get next page */
  if (VPID_ISNULL (next_vpid))
    {
      /* set to first page */
      next_vpid->pageid = hfid->hpgid;
      next_vpid->volid = hfid->vfid.volid;
    }
  else
    {
      pg_watcher.pgptr = heap_scan_pb_lock_and_fetch (thread_p, next_vpid, OLD_PAGE, S_LOCK, NULL, &pg_watcher);
      if (pg_watcher.pgptr == NULL)
	{
	  return S_ERROR;
	}
      /* get next page */
      heap_vpid_next (hfid, pg_watcher.pgptr, next_vpid);
      if (OID_ISNULL (next_vpid))
	{
	  /* no more pages to scan */
	  pgbuf_ordered_unfix (thread_p, &pg_watcher);
	  return S_END;
	}
      pgbuf_replace_watcher (thread_p, &pg_watcher, &old_pg_watcher);
    }

  /* get page pointer to next page */
  pg_watcher.pgptr =
    heap_scan_pb_lock_and_fetch (thread_p, next_vpid, OLD_PAGE_PREVENT_DEALLOC, S_LOCK, NULL, &pg_watcher);
  if (old_pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
    }
  if (pg_watcher.pgptr == NULL)
    {
      return S_ERROR;
    }

  /* read page information and return scan code */
  scan = heap_get_page_info (thread_p, class_oid, hfid, next_vpid, pg_watcher.pgptr, cache_pageinfo);

  pgbuf_ordered_unfix (thread_p, &pg_watcher);
  return scan;
}

/*
 * heap_page_prev () - Advance to previous page in chain and obtain
 *		       information.
 *
 * return	       : SCAN_CODE.
 * thread_p (in)       : Thread entry.
 * class_oid (in)      : Class object identifier.
 * hfid (in)	       : Heap file identifier.
 * prev_vpid (in)      : Previous page identifier.
 * cache_pageinfo (in) : Pointers to DB_VALUEs where page information is
 *			 stored.
 */
SCAN_CODE
heap_page_prev (THREAD_ENTRY * thread_p, const OID * class_oid, const HFID * hfid, VPID * prev_vpid,
		DB_VALUE ** cache_pageinfo)
{
  PGBUF_WATCHER pg_watcher;
  PGBUF_WATCHER old_pg_watcher;
  SCAN_CODE scan = S_SUCCESS;

  PGBUF_INIT_WATCHER (&pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  PGBUF_INIT_WATCHER (&old_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);

  /* get next page */
  if (VPID_ISNULL (prev_vpid))
    {
      /* set to last page */
      if (file_find_last_page (thread_p, &hfid->vfid, prev_vpid) == NULL)
	{
	  return S_END;
	}
    }
  else
    {
      pg_watcher.pgptr = heap_scan_pb_lock_and_fetch (thread_p, prev_vpid, OLD_PAGE, S_LOCK, NULL, &pg_watcher);
      if (pg_watcher.pgptr == NULL)
	{
	  return S_ERROR;
	}
      /* get next page */
      heap_vpid_prev (hfid, pg_watcher.pgptr, prev_vpid);
      if (OID_ISNULL (prev_vpid))
	{
	  /* no more pages to scan */
	  return S_END;
	}
      /* get next page */
      pgbuf_replace_watcher (thread_p, &pg_watcher, &old_pg_watcher);
    }

  pg_watcher.pgptr =
    heap_scan_pb_lock_and_fetch (thread_p, prev_vpid, OLD_PAGE_PREVENT_DEALLOC, S_LOCK, NULL, &pg_watcher);
  if (old_pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
    }
  if (pg_watcher.pgptr == NULL)
    {
      pgbuf_ordered_unfix (thread_p, &pg_watcher);
      return S_ERROR;
    }

  /* read page information and return scan code */
  scan = heap_get_page_info (thread_p, class_oid, hfid, prev_vpid, pg_watcher.pgptr, cache_pageinfo);

  pgbuf_ordered_unfix (thread_p, &pg_watcher);
  return scan;
}

/*
 * heap_get_record_info () - Heap function to obtain record information and
 *			     record data.
 *
 * return	       : SCAN CODE (S_SUCCESS or S_ERROR).
 * thread_p (in)       : Thread entry.
 * oid (in)	       : Object identifier.
 * recdes (out)	       : Record descriptor (to save record data).
 * forward_recdes (in) : Record descriptor used by REC_RELOCATION & REC_BIGONE
 *			 records.
 * pgptr (in/out)      : Pointer to the page this object belongs to.
 * scan_cache (in)     : Heap scan cache.
 * ispeeking (in)      : PEEK/COPY.
 * record_info (out)   : Stores record information.
 */
static int
heap_get_record_info (THREAD_ENTRY * thread_p, const OID oid, RECDES * recdes, RECDES forward_recdes,
		      PGBUF_WATCHER * page_watcher, HEAP_SCANCACHE * scan_cache, int ispeeking, DB_VALUE ** record_info)
{
  SPAGE_SLOT *slot_p = NULL;
  SCAN_CODE scan = S_SUCCESS;
  OID *peek_oid = NULL;
  OID forward_oid;
  MVCC_REC_HEADER mvcc_header;

  assert (page_watcher != NULL);
  assert (record_info != NULL);
  assert (recdes != NULL);

  /* careful adding values in the right order */
  DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_VOLUMEID], oid.volid);
  DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_PAGEID], oid.pageid);
  DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_SLOTID], oid.slotid);

  /* get slot info */
  slot_p = spage_get_slot (page_watcher->pgptr, oid.slotid);
  if (slot_p == NULL)
    {
      assert (0);
    }
  DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_OFFSET], slot_p->offset_to_record);
  DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_LENGTH], slot_p->record_length);
  DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_REC_TYPE], slot_p->record_type);

  /* get record info */
  switch (slot_p->record_type)
    {
    case REC_NEWHOME:
    case REC_HOME:
      if (scan_cache != NULL && ispeeking == COPY && recdes->data == NULL)
	{
	  /* It is guaranteed that scan_cache is not NULL. */
	  if (scan_cache->area == NULL)
	    {
	      /* Allocate an area to hold the object. Assume that the object will fit in two pages for not better
	       * estimates. */
	      scan_cache->area_size = DB_PAGESIZE * 2;
	      scan_cache->area = (char *) db_private_alloc (thread_p, scan_cache->area_size);
	      if (scan_cache->area == NULL)
		{
		  scan_cache->area_size = -1;
		  pgbuf_ordered_unfix (thread_p, page_watcher);
		  return S_ERROR;
		}
	    }
	  recdes->data = scan_cache->area;
	  recdes->area_size = scan_cache->area_size;
	  /* The allocated space is enough to save the instance. */
	}
      if (scan_cache != NULL && scan_cache->cache_last_fix_page == true)
	{
	  scan = spage_get_record (page_watcher->pgptr, oid.slotid, recdes, ispeeking);
	  pgbuf_replace_watcher (thread_p, page_watcher, &scan_cache->page_watcher);
	}
      else
	{
	  scan = spage_get_record (page_watcher->pgptr, oid.slotid, recdes, COPY);
	  pgbuf_ordered_unfix (thread_p, page_watcher);
	}
      DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_REPRID], or_rep_id (recdes));
      DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_REPRID], or_chn (recdes));
      or_mvcc_get_header (recdes, &mvcc_header);
      DB_MAKE_BIGINT (record_info[HEAP_RECORD_INFO_T_MVCC_INSID], MVCC_GET_INSID (&mvcc_header));
      if (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_DELID))
	{
	  DB_MAKE_BIGINT (record_info[HEAP_RECORD_INFO_T_MVCC_DELID], MVCC_GET_DELID (&mvcc_header));
	  DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_CHN]);
	}
      else
	{
	  DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_CHN], OR_GET_NON_MVCC_CHN (&mvcc_header));
	  DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_MVCC_DELID]);
	}
      DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_MVCC_FLAGS], MVCC_GET_FLAG (&mvcc_header));
      if (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_NEXT_VERSION))
	{
	  DB_MAKE_OID (record_info[HEAP_RECORD_INFO_T_MVCC_NEXT_VERSION], &MVCC_GET_NEXT_VERSION (&mvcc_header));
	  if (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_PARTITION_OID))
	    {
	      DB_MAKE_OID (record_info[HEAP_RECORD_INFO_T_MVCC_PARTITION_OID], &MVCC_GET_PARTITION_OID (&mvcc_header));
	    }
	  else
	    {
	      DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_MVCC_PARTITION_OID]);
	    }
	}
      else
	{
	  DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_MVCC_NEXT_VERSION]);
	  DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_MVCC_PARTITION_OID]);
	}
      break;

    case REC_BIGONE:
      /* Get the address of the content of the multiple page object */
      COPY_OID (&forward_oid, (OID *) forward_recdes.data);
      pgbuf_ordered_unfix (thread_p, page_watcher);

      /* Now get the content of the multiple page object. */
      /* Try to reuse the previously allocated area */
      if (scan_cache != NULL && (ispeeking == PEEK || recdes->data == NULL))
	{
	  /* It is guaranteed that scan_cache is not NULL. */
	  if (scan_cache->area == NULL)
	    {
	      /* Allocate an area to hold the object. Assume that the object will fit in two pages for not better
	       * estimates. We could call heap_ovf_get_length, but it may be better to just guess and realloc if
	       * needed. We could also check the estimates for average object length, but again, it may be expensive
	       * and may not be accurate for the object. */
	      scan_cache->area_size = DB_PAGESIZE * 2;
	      scan_cache->area = (char *) db_private_alloc (thread_p, scan_cache->area_size);
	      if (scan_cache->area == NULL)
		{
		  scan_cache->area_size = -1;
		  return S_ERROR;
		}
	    }
	  recdes->data = scan_cache->area;
	  recdes->area_size = scan_cache->area_size;

	  while ((scan = heap_ovf_get (thread_p, &forward_oid, recdes, NULL_CHN, NULL)) == S_DOESNT_FIT)
	    {
	      /* The object did not fit into such an area, reallocate a new area */
	      recdes->area_size = -recdes->length;
	      recdes->data = (char *) db_private_realloc (thread_p, scan_cache->area, recdes->area_size);
	      if (recdes->data == NULL)
		{
		  return S_ERROR;
		}
	      scan_cache->area_size = recdes->area_size;
	      scan_cache->area = recdes->data;
	    }
	  if (scan != S_SUCCESS)
	    {
	      recdes->data = NULL;
	    }
	}
      else
	{
	  scan = heap_ovf_get (thread_p, &forward_oid, recdes, NULL_CHN, NULL);
	}
      if (scan != S_SUCCESS)
	{
	  return S_ERROR;
	}
      DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_REPRID], or_rep_id (recdes));
      DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_REPRID], or_chn (recdes));

      or_mvcc_get_header (recdes, &mvcc_header);
      DB_MAKE_BIGINT (record_info[HEAP_RECORD_INFO_T_MVCC_INSID], MVCC_GET_INSID (&mvcc_header));
      if (MVCC_IS_FLAG_SET (&mvcc_header, OR_MVCC_FLAG_VALID_DELID))
	{
	  DB_MAKE_BIGINT (record_info[HEAP_RECORD_INFO_T_MVCC_DELID], MVCC_GET_DELID (&mvcc_header));
	  DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_CHN]);
	}
      else
	{
	  DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_CHN], OR_GET_NON_MVCC_CHN (&mvcc_header));
	  DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_MVCC_DELID]);
	}
      DB_MAKE_INT (record_info[HEAP_RECORD_INFO_T_MVCC_FLAGS], MVCC_GET_FLAG (&mvcc_header));
      DB_MAKE_OID (record_info[HEAP_RECORD_INFO_T_MVCC_NEXT_VERSION], &MVCC_GET_NEXT_VERSION (&mvcc_header));
      DB_MAKE_OID (record_info[HEAP_RECORD_INFO_T_MVCC_PARTITION_OID], &MVCC_GET_PARTITION_OID (&mvcc_header));
      break;
    case REC_RELOCATION:
    case REC_MVCC_NEXT_VERSION:
    case REC_MARKDELETED:
    case REC_DELETED_WILL_REUSE:
    case REC_ASSIGN_ADDRESS:
    case REC_UNKNOWN:
    default:
      DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_REPRID]);
      DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_CHN]);
      DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_MVCC_INSID]);
      DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_MVCC_DELID]);
      DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_MVCC_FLAGS]);
      if (slot_p->record_type == REC_MVCC_NEXT_VERSION)
	{
	  peek_oid = (OID *) forward_recdes.data;
	  DB_MAKE_OID (record_info[HEAP_RECORD_INFO_T_MVCC_NEXT_VERSION], peek_oid);
	  if (forward_recdes.length > OR_OID_SIZE)
	    {
	      peek_oid = (OID *) (forward_recdes.data + OR_OID_SIZE);
	      DB_MAKE_OID (record_info[HEAP_RECORD_INFO_T_MVCC_PARTITION_OID], peek_oid);
	    }
	  else
	    {
	      DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_MVCC_PARTITION_OID]);
	    }
	}
      else
	{
	  DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_MVCC_NEXT_VERSION]);
	  DB_MAKE_NULL (record_info[HEAP_RECORD_INFO_T_MVCC_PARTITION_OID]);
	}

      recdes->area_size = -1;
      recdes->data = NULL;
      if (scan_cache != NULL && scan_cache->cache_last_fix_page)
	{
	  assert (PGBUF_IS_CLEAN_WATCHER (&(scan_cache->page_watcher)));
	  if (page_watcher->pgptr != NULL)
	    {
	      pgbuf_replace_watcher (thread_p, page_watcher, &scan_cache->page_watcher);
	    }
	}
      else if (page_watcher->pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, page_watcher);
	}
      break;
    }

  return scan;
}

/*
 * heap_get_record () - Get heap record during a heap scan.
 *
 * return	       : Scan code.
 * thread_p (in)       : Thread entry.
 * oid (in)	       : Object to be obtained.
 * recdes (in)	       : Record descriptor to keep object data.
 * forward_recdes (in) : Record descriptor used for REC_BIGONE and
 *			 REC_RELOCATION. -- legacy
 * pgptr (in)	       : Pointer to home page.
 * scan_cache (in)     : Scan cache.
 * ispeeking (in)      : Peek record or copy.
 * type (in)	       : Record type.
 * check_snapshot (in) : true, if snapshot function must be checked
 *
 * NOTE: This is called from heap_next_internal.
 */
static int
heap_scan_get_record (THREAD_ENTRY * thread_p, const OID oid, OID * class_oid, RECDES * recdes, RECDES forward_recdes,
		      PGBUF_WATCHER * home_pg_watcher, HEAP_SCANCACHE * scan_cache, int ispeeking, bool check_snapshot)
{
  OID forward_oid = OID_INITIALIZER;
  SCAN_CODE scan = S_SUCCESS;
  MVCC_SNAPSHOT *mvcc_snapshot = NULL;
  MVCC_REC_HEADER mvcc_header = MVCC_REC_HEADER_INITIALIZER;
  PGBUF_WATCHER fwd_pg_watcher;
  INT16 type;

  assert (recdes != NULL);
  assert ((check_snapshot == false)
	  || (scan_cache != NULL && scan_cache->mvcc_snapshot != NULL
	      && scan_cache->mvcc_snapshot->snapshot_fnc != NULL));
  assert (scan_cache != NULL || ispeeking == COPY);
  assert (home_pg_watcher != NULL);
  assert (home_pg_watcher->pgptr != NULL);

  PGBUF_INIT_WATCHER (&fwd_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, HEAP_SCAN_ORDERED_HFID (scan_cache));

  scan =
    heap_prepare_get_record (thread_p, &oid, class_oid, &forward_oid, NULL, home_pg_watcher, &fwd_pg_watcher, &type,
			     PGBUF_LATCH_READ, true, LOG_ERROR_IF_DELETED);
  if (scan != S_SUCCESS)
    {
      goto exit;
    }
  assert (type == REC_HOME || type == REC_BIGONE || type == REC_RELOCATION);
  assert (type == REC_HOME || (!OID_ISNULL (&forward_oid) && fwd_pg_watcher.pgptr != NULL));

  if (scan_cache != NULL && scan_cache->mvcc_snapshot != NULL && scan_cache->mvcc_snapshot->snapshot_fnc != NULL
      && check_snapshot)
    {
      mvcc_snapshot = scan_cache->mvcc_snapshot;
    }
  if (mvcc_snapshot != NULL)
    {
      scan =
	heap_get_mvcc_header (thread_p, &oid, &forward_oid, home_pg_watcher->pgptr, fwd_pg_watcher.pgptr, type,
			      &mvcc_header);
      if (scan != S_SUCCESS)
	{
	  goto exit;
	}
      if (mvcc_snapshot->snapshot_fnc (thread_p, &mvcc_header, mvcc_snapshot) == false)
	{
	  scan = S_SNAPSHOT_NOT_SATISFIED;
	  goto exit;
	}
    }
  assert (scan == S_SUCCESS);
  scan =
    heap_get_record_data_when_all_ready (thread_p, (OID *) & oid, &forward_oid, home_pg_watcher->pgptr,
					 fwd_pg_watcher.pgptr, type, recdes, scan_cache, ispeeking);
  /* Fall through to exit. */

exit:

  if (fwd_pg_watcher.pgptr != NULL)
    {
      /* Unfix forward page. */
      pgbuf_ordered_unfix (thread_p, &fwd_pg_watcher);
    }
  if (scan == S_SUCCESS && scan_cache != NULL && scan_cache->cache_last_fix_page)
    {
      /* Record was successfully obtained and scan is fixed. Save home page (or NULL if it had to be unfixed) to
       * scan_cache. */
      pgbuf_replace_watcher (thread_p, home_pg_watcher, &scan_cache->page_watcher);
      assert (home_pg_watcher->pgptr == NULL);
    }
  else if (home_pg_watcher->pgptr)
    {
      /* Unfix home page. */
      pgbuf_ordered_unfix (thread_p, home_pg_watcher);
    }
  return scan;
}

/*
 * heap_next () - Retrieve or peek next object
 *   return: SCAN_CODE (Either of S_SUCCESS, S_DOESNT_FIT, S_END, S_ERROR)
 *   hfid(in):
 *   class_oid(in):
 *   next_oid(in/out): Object identifier of current record.
 *                     Will be set to next available record or NULL_OID when
 *                     there is not one.
 *   recdes(in/out): Pointer to a record descriptor. Will be modified to
 *                   describe the new record.
 *   scan_cache(in/out): Scan cache or NULL
 *   ispeeking(in): PEEK when the object is peeked, scan_cache cannot be NULL
 *                  COPY when the object is copied
 *
 */
SCAN_CODE
heap_next (THREAD_ENTRY * thread_p, const HFID * hfid, OID * class_oid, OID * next_oid, RECDES * recdes,
	   HEAP_SCANCACHE * scan_cache, int ispeeking)
{
  return heap_next_internal (thread_p, hfid, class_oid, next_oid, recdes, scan_cache, ispeeking, false, NULL);
}

/*
 * heap_next_record_info () - Retrieve or peek next object.
 *
 * return		     : SCAN_CODE.
 * thread_p (in)	     : Thread entry.
 * hfid (in)		     : Heap file identifier.
 * class_oid (in)	     : Class Object identifier.
 * next_oid (in/out)	     : Current object identifier. Will store the next
 *			       scanned object identifier.
 * recdes (in)		     : Record descriptor.
 * scan_cache (in)	     : Scan cache.
 * ispeeking (in)	     : PEEK/COPY.
 * cache_recordinfo (in/out) : DB_VALUE pointer array that caches record
 *			       information values.
 *
 * NOTE: This function is similar to heap next. The difference is that all
 *	 slots are scanned in their order in the heap file and along with
 *	 record data also information about that record is obtained.
 */
SCAN_CODE
heap_next_record_info (THREAD_ENTRY * thread_p, const HFID * hfid, OID * class_oid, OID * next_oid, RECDES * recdes,
		       HEAP_SCANCACHE * scan_cache, int ispeeking, DB_VALUE ** cache_recordinfo)
{
  return heap_next_internal (thread_p, hfid, class_oid, next_oid, recdes, scan_cache, ispeeking, false,
			     cache_recordinfo);
}

/*
 * heap_prev () - Retrieve or peek next object
 *   return: SCAN_CODE (Either of S_SUCCESS, S_DOESNT_FIT, S_END, S_ERROR)
 *   hfid(in):
 *   class_oid(in):
 *   next_oid(in/out): Object identifier of current record.
 *                     Will be set to next available record or NULL_OID when
 *                     there is not one.
 *   recdes(in/out): Pointer to a record descriptor. Will be modified to
 *                   describe the new record.
 *   scan_cache(in/out): Scan cache or NULL
 *   ispeeking(in): PEEK when the object is peeked, scan_cache cannot be NULL
 *                  COPY when the object is copied
 *
 */
SCAN_CODE
heap_prev (THREAD_ENTRY * thread_p, const HFID * hfid, OID * class_oid, OID * next_oid, RECDES * recdes,
	   HEAP_SCANCACHE * scan_cache, int ispeeking)
{
  return heap_next_internal (thread_p, hfid, class_oid, next_oid, recdes, scan_cache, ispeeking, true, NULL);
}

/*
 * heap_prev_record_info () - Retrieve or peek next object.
 *
 * return		     : SCAN_CODE.
 * thread_p (in)	     : Thread entry.
 * hfid (in)		     : Heap file identifier.
 * class_oid (in)	     : Class Object identifier.
 * prev_oid (in/out)	     : Current object identifier. Will store the
 *			       previous scanned object identifier.
 * recdes (in)		     : Record descriptor.
 * scan_cache (in)	     : Scan cache.
 * ispeeking (in)	     : PEEK/COPY.
 * cache_recordinfo (in/out) : DB_VALUE pointer array that caches record
 *			       information values
 *
 * NOTE: This function is similar to heap next. The difference is that all
 *	 slots are scanned in their order in the heap file and along with
 *	 record data also information about that record is obtained.
 */
SCAN_CODE
heap_prev_record_info (THREAD_ENTRY * thread_p, const HFID * hfid, OID * class_oid, OID * next_oid, RECDES * recdes,
		       HEAP_SCANCACHE * scan_cache, int ispeeking, DB_VALUE ** cache_recordinfo)
{
  return heap_next_internal (thread_p, hfid, class_oid, next_oid, recdes, scan_cache, ispeeking, true,
			     cache_recordinfo);
}

/*
 * heap_mvcc_check_and_lock_for_delete () - lock a row if it can be deleted
 *
 *   return: error code
 *   thread_p(in): thread entry
 *   oid(in): oid
 *   class_oid(in): class oid
 *   lock(in): lock to acquire
 *   vacuum_allowed(in): true, if VACUUM can clean the current OID
 *	while current thread is waiting for unconditional lock or latch
 *   recdes_header(in/out): MVCC record header
 *   pgptr(in): page where the record reside, already latched for write
 *   forward_pgptr(in): forward page, needed in case of REC_BIGONE records
 *   forward_oid(out): forward OID
 *   mvcc_delete_info(in/out): MVCC delete info
 *
 *  Note: The function preserve the acquired lock only if the row can be
 *    deleted. If vacuum allowed, and the type of the record is
 *    REC_MVCC_NEXT_VERSION when transaction resume after unconditional locking,
 *    recdes_header is filled with informations that may be used by caller
 *    to advance to the next version.
 */
static int
heap_mvcc_check_and_lock_for_delete (THREAD_ENTRY * thread_p, OID * oid, OID * class_oid, LOCK lock,
				     bool vacuum_allowed, MVCC_REC_HEADER * recdes_header,
				     PGBUF_WATCHER * home_page_watcher, PGBUF_WATCHER * fwd_page_watcher,
				     OID * forward_oid, INT16 * record_type, HEAP_MVCC_DELETE_INFO * mvcc_delete_info)
{
  MVCC_SATISFIES_DELETE_RESULT satisfies_delete_result;
  MVCCID del_mvccid = MVCCID_NULL;
  bool record_locked = false;
  OID curr_row_version = { NULL_PAGEID, NULL_SLOTID, NULL_VOLID };
  bool ignore_record = false;
  INT16 rec_type_local;
  OID partition_class_oid;

  assert (recdes_header != NULL);
  assert (home_page_watcher != NULL);
  assert (home_page_watcher->pgptr != NULL);
  assert (mvcc_delete_info != NULL);

  if (record_type == NULL)
    {
      /* Use local variable if the argument is not provided. */
      record_type = &rec_type_local;
    }

try_again:

  /* Check satisfies delete. */
  satisfies_delete_result = mvcc_satisfies_delete (thread_p, recdes_header);
  if (satisfies_delete_result == DELETE_RECORD_INVISIBLE)
    {
      /* Record is too "new" and cannot be seen (the inserter is still considered active). Should be handled before
       * calling this function. */
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_MVCC_ROW_INVALID_FOR_DELETE, 3, oid->volid, oid->pageid,
	      oid->slotid);
      goto error;
    }
  else if (satisfies_delete_result == DELETE_RECORD_IN_PROGRESS || satisfies_delete_result == DELETE_RECORD_CAN_DELETE)
    {
      /* Object must be locked. */
      if (record_locked == true || lock == NULL_LOCK)
	{
	  /* The lock has been already acquired */
	  assert (satisfies_delete_result == DELETE_RECORD_CAN_DELETE);
	  goto end;
	}

      if (satisfies_delete_result == DELETE_RECORD_IN_PROGRESS)
	{
	  /* Safe guard */
	  assert (MVCC_IS_FLAG_SET (recdes_header, OR_MVCC_FLAG_VALID_DELID));
	  del_mvccid = MVCC_GET_DELID (recdes_header);
	  assert (MVCCID_IS_VALID (del_mvccid));
	}
      else
	{
	  /* Safe guard */
	  assert (!MVCC_IS_FLAG_SET (recdes_header, OR_MVCC_FLAG_VALID_DELID)
		  || !MVCCID_IS_VALID (MVCC_GET_DELID (recdes_header)));
	}

      if (satisfies_delete_result == DELETE_RECORD_CAN_DELETE)
	{
	  if (lock_object (thread_p, oid, class_oid, lock, LK_COND_LOCK) == LK_GRANTED)
	    {
	      /* In case of DELETE_RECORD_CAN_DELETE, most likely a conditional will be successful. However it is still 
	       * possible that two or more transactions are racing for this lock and only one will get it. In case of
	       * DELETE_RECORD_IN_PROGRESS, it is very unlikely that the deleter transaction, which was just checked as 
	       * being active, commits until a conditional lock is requested. The overhead of trying conditional lock
	       * may be a bad trade-off. */
	      record_locked = true;
	      goto end;
	    }
	}

      /* DELETE_RECORD_IN_PROGRESS or conditional lock failed. */
      /* Unfix the pages. */
      pgbuf_ordered_unfix (thread_p, home_page_watcher);
      if (fwd_page_watcher != NULL && fwd_page_watcher->pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, fwd_page_watcher);
	}

      /* Lock the object unconditionally. */
      if (lock_object (thread_p, oid, class_oid, lock, LK_UNCOND_LOCK) != LK_GRANTED)
	{
	  goto error;
	}

      record_locked = true;

      /* Prepare for getting record again. Since pages have been unlatched, others may have changed them (even our
       * record). */
      OID_SET_NULL (&partition_class_oid);
      if (heap_prepare_get_record (thread_p, oid, class_oid, forward_oid, &partition_class_oid, home_page_watcher,
				   fwd_page_watcher, record_type, PGBUF_LATCH_READ, false,
				   LOG_WARNING_IF_DELETED) != S_SUCCESS)
	{
	  goto error;
	}
      /* Make sure everything is as expected. */
      assert (*record_type == REC_HOME || *record_type == REC_RELOCATION || *record_type == REC_BIGONE
	      || *record_type == REC_MVCC_NEXT_VERSION);

      if (*record_type == REC_MVCC_NEXT_VERSION)
	{
	  if (vacuum_allowed)
	    {
	      /* 
	       * if visible version was not previously obtained (MVCC snapshot
	       * was not applied), it means that the current version may be
	       * cleaned by VACUUM while current transaction is waiting on lock
	       * or page latch acquisition.
	       */

	      /* release acquired lock */
	      lock_unlock_object_donot_move_to_non2pl (thread_p, oid, class_oid, lock);

	      /* 
	       * Use MVCC header to store information about next version. These
	       * infos may be used by the caller to advance to the next version.
	       */
	      recdes_header->mvcc_flag = OR_MVCC_FLAG_VALID_NEXT_VERSION;
	      COPY_OID (&recdes_header->next_version, forward_oid);
	      if (!OID_ISNULL (&partition_class_oid))
		{
		  recdes_header->mvcc_flag |= OR_MVCC_FLAG_VALID_PARTITION_OID;
		  COPY_OID (&recdes_header->partition_oid, &partition_class_oid);
		}

	      /* 
	       * store saved DELID, satisfies result for current row,
	       * set next row OID, lock needed for next row update
	       */
	      MVCC_SET_DELETE_INFO (mvcc_delete_info, del_mvccid, forward_oid, DELETE_RECORD_DELETED);
	      return NO_ERROR;
	    }

	  /* This must be called on a version at least as new as the visible version. Even if the version is deleted,
	   * it still cannot be considered for vacuum, since its deleter is still active relative to current
	   * transaction. */
	  assert_release (false);
	  goto error;
	}

      /* Check page pointer and oid match. */
      assert (HEAP_IS_PAGE_OF_OID (home_page_watcher->pgptr, oid));
      /* Make sure that forward_oid and forward page are obtained if rec_type is REC_BIGONE or REC_RELOCATION. */
      assert (*record_type == REC_HOME
	      || (!OID_ISNULL (forward_oid) && HEAP_IS_PAGE_OF_OID (fwd_page_watcher->pgptr, forward_oid)));

      /* Get MVCC header */
      if (heap_get_mvcc_header (thread_p, oid, forward_oid, home_page_watcher->pgptr, fwd_page_watcher->pgptr,
				*record_type, recdes_header) != S_SUCCESS)
	{
	  goto error;
	}

      if (MVCC_IS_FLAG_SET (recdes_header, OR_MVCC_FLAG_VALID_DELID))
	{
	  if (satisfies_delete_result == DELETE_RECORD_IN_PROGRESS
	      && !MVCCID_IS_EQUAL (MVCC_GET_DELID (recdes_header), del_mvccid))
	    {
	      /* Other transaction updated the row while sleeping, check whether the row satisfies delete again. */
	      /* TODO: Even if other transaction updated the row, can't we consider that record was deleted? Since we
	       * obtained the lock, the deleter must have committed (or otherwise should still hold lock).  Maybe we
	       * can add a debug assert here that the new deleter is no longer active and avoid trying everything
	       * again. */
	      goto try_again;
	    }
	  satisfies_delete_result = DELETE_RECORD_DELETED;
	}
      else
	{
	  satisfies_delete_result = DELETE_RECORD_CAN_DELETE;
	}
    }

end:
  if (satisfies_delete_result == DELETE_RECORD_DELETED || satisfies_delete_result == DELETE_RECORD_SELF_DELETED)
    {
      /* If satisfies_delete_result == DELETE_RECORD_SELF_DELETED should we release lock? */
      if (record_locked == true)
	{
	  /* release acquired lock */
	  assert (satisfies_delete_result == DELETE_RECORD_DELETED);
	  lock_unlock_object_donot_move_to_non2pl (thread_p, oid, class_oid, lock);
	  record_locked = false;
	}

      /* store DELID, satisfies result for current row set next row OID, lock needed for next row update */
      MVCC_SET_DELETE_INFO (mvcc_delete_info, MVCC_GET_DELID (recdes_header), &MVCC_GET_NEXT_VERSION (recdes_header),
			    satisfies_delete_result);
    }
  else if (satisfies_delete_result == DELETE_RECORD_CAN_DELETE)
    {
      /* there is no next row, set satisfies result for current row */
      MVCC_SET_DELETE_INFO (mvcc_delete_info, MVCCID_NULL, (OID *) & oid_Null_oid, satisfies_delete_result);
    }
  else
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_MVCC_ROW_INVALID_FOR_DELETE, 3, oid->volid, oid->pageid,
	      oid->slotid);
      goto error;
    }

  return NO_ERROR;

error:
  if (home_page_watcher->pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, home_page_watcher);
    }
  if (fwd_page_watcher != NULL && fwd_page_watcher->pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, fwd_page_watcher);
    }

  if (record_locked == true)
    {
      lock_unlock_object (thread_p, oid, class_oid, lock, true);
    }

  return (er_errid () == NO_ERROR) ? ER_FAILED : er_errid ();
}

/*
 * heap_scancache_start_chain_update  () - start new scan cache for MVCC
 *					  chain update
 *   return: error code
 *   thread_p(in): thread entry
 *   new_scan_cache(in/out): the new scan cache
 *   old_scan_cache(in/out): the old scan cache
 *   next_row_version(in): next row version
 *
 * Note: This function start a new scan cache used to update
 *	MVCC chain. The old and new scan cache may share common data.
 */
int
heap_scancache_start_chain_update (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * new_scan_cache,
				   HEAP_SCANCACHE * old_scan_cache, OID * next_row_version)
{
  assert (new_scan_cache != NULL);
  assert (old_scan_cache != NULL);

  /* update scan cache */

  /* start a local cache that is suitable for our needs */
  if (heap_scancache_start (thread_p, new_scan_cache, &old_scan_cache->node.hfid, &old_scan_cache->node.class_oid,
			    true, false, NULL) != NO_ERROR)
    {
      /* TODO - er_set */
      return ER_FAILED;
    }

  /* use current_scan_cache->area in order to alloc only once */
  new_scan_cache->area = old_scan_cache->area;
  new_scan_cache->area_size = old_scan_cache->area_size;
  /* set pgptr if is the case */
  if (old_scan_cache->page_watcher.pgptr != NULL)
    {
      /* handle this case, just to be sure */
      VPID *vpidptr_incache;
      VPID vpid;

      vpid.volid = next_row_version->volid;
      vpid.pageid = next_row_version->pageid;
      vpidptr_incache = pgbuf_get_vpid_ptr (old_scan_cache->page_watcher.pgptr);
      if (VPID_EQ (&vpid, vpidptr_incache))
	{
	  pgbuf_replace_watcher (thread_p, &old_scan_cache->page_watcher, &new_scan_cache->page_watcher);
	}
      else
	{
	  /* Free the previous scan page */
	  pgbuf_ordered_unfix (thread_p, &old_scan_cache->page_watcher);
	}
    }

  return NO_ERROR;
}

/*
 * heap_get_mvcc_rec_header_from_overflow () - Get record header from overflow
 *					       page.
 *
 * return :
 * PAGE_PTR ovf_page (in) : overflow page pointer
 * MVCC_REC_HEADER * mvcc_header (in/out) : MVCC record header
 * recdes(in/out): if not NULL then receives first overflow page
 */
int
heap_get_mvcc_rec_header_from_overflow (PAGE_PTR ovf_page, MVCC_REC_HEADER * mvcc_header, RECDES * peek_recdes)
{
  RECDES ovf_recdes;

  assert (ovf_page != NULL);
  assert (mvcc_header != NULL);

  if (peek_recdes == NULL)
    {
      peek_recdes = &ovf_recdes;
    }
  peek_recdes->data = overflow_get_first_page_data (ovf_page);
  peek_recdes->length = OR_MVCC_MAX_HEADER_SIZE;

  return or_mvcc_get_header (peek_recdes, mvcc_header);
}

/*
 * heap_set_mvcc_rec_header_on_overflow () - Updates MVCC record header on
 *					     overflow page data.
 *
 * return	    : Void.
 * ovf_page (in)    : First overflow page.
 * mvcc_header (in) : MVCC Record header.
 */
int
heap_set_mvcc_rec_header_on_overflow (PAGE_PTR ovf_page, MVCC_REC_HEADER * mvcc_header)
{
  RECDES ovf_recdes;

  assert (ovf_page != NULL);
  assert (mvcc_header != NULL);

  ovf_recdes.data = overflow_get_first_page_data (ovf_page);
  ovf_recdes.area_size = ovf_recdes.length = OR_HEADER_SIZE (ovf_recdes.data);
  /* Safe guard */
  assert (ovf_recdes.length == OR_MVCC_MAX_HEADER_SIZE);

  /* Make sure the header has maximum size for overflow records */
  if (!MVCC_IS_FLAG_SET (mvcc_header, OR_MVCC_FLAG_VALID_INSID))
    {
      /* Add MVCCID_ALL_VISIBLE for insert MVCCID */
      MVCC_SET_FLAG_BITS (mvcc_header, OR_MVCC_FLAG_VALID_INSID);
      MVCC_SET_INSID (mvcc_header, MVCCID_ALL_VISIBLE);
    }
  if (!MVCC_IS_FLAG_SET (mvcc_header, OR_MVCC_FLAG_VALID_DELID)
      && !MVCC_IS_FLAG_SET (mvcc_header, OR_MVCC_FLAG_VALID_LONG_CHN))
    {
      /* Set 8 bytes for CHN */
      MVCC_SET_FLAG_BITS (mvcc_header, OR_MVCC_FLAG_VALID_LONG_CHN);
    }
  /* Safe guard */
  assert (or_mvcc_header_size_from_flags (MVCC_GET_FLAG (mvcc_header)) == OR_MVCC_MAX_HEADER_SIZE);
  return or_mvcc_set_header (&ovf_recdes, mvcc_header);
}

/*
 * heap_get_bigone_content () - get content of a big record
 *
 * return	    : scan code.
 * thread_p (in)    :
 * scan_cache (in)  : Scan cache
 * ispeeking(in)    : 0 if the content will be copied.
 * forward_oid(in)  : content oid.
 * recdes(in/out)   : record descriptor that will contain its content
 */
SCAN_CODE
heap_get_bigone_content (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, int ispeeking, OID * forward_oid,
			 RECDES * recdes)
{
  SCAN_CODE scan = S_SUCCESS;

  /* Try to reuse the previously allocated area No need to check the snapshot since was already checked */
  if (scan_cache != NULL && (ispeeking == PEEK || recdes->data == NULL))
    {
      if (scan_cache->area == NULL)
	{
	  /* 
	   * Allocate an area to hold the object. Assume that the object
	   * will fit in two pages for not better estimates. We could call
	   * heap_ovf_get_length, but it may be better to just guess and
	   * realloc if needed.
	   * We could also check the estimates for average object length,
	   * but again, it may be expensive and may not be accurate
	   * for this object.
	   */
	  scan_cache->area_size = DB_PAGESIZE * 2;
	  scan_cache->area = (char *) db_private_alloc (thread_p, scan_cache->area_size);
	  if (scan_cache->area == NULL)
	    {
	      scan_cache->area_size = -1;
	      return S_ERROR;
	    }
	}
      recdes->data = scan_cache->area;
      recdes->area_size = scan_cache->area_size;

      while ((scan = heap_ovf_get (thread_p, forward_oid, recdes, NULL_CHN, NULL)) == S_DOESNT_FIT)
	{
	  /* 
	   * The object did not fit into such an area, reallocate a new
	   * area
	   */

	  recdes->area_size = -recdes->length;
	  recdes->data = (char *) db_private_realloc (thread_p, scan_cache->area, recdes->area_size);
	  if (recdes->data == NULL)
	    {
	      return S_ERROR;
	    }
	  scan_cache->area_size = recdes->area_size;
	  scan_cache->area = recdes->data;
	}
      if (scan != S_SUCCESS)
	{
	  recdes->data = NULL;
	}
    }
  else
    {
      scan = heap_ovf_get (thread_p, forward_oid, recdes, NULL_CHN, NULL);
    }

  return scan;
}

/*
 * mvcc_reevaluate_filters () - reevaluates key range, key filter and data
 *				filter predicates
 *   return: result of reevaluation
 *   thread_p(in): thread entry
 *   mvcc_reev_data(in): The structure that contains data needed for
 *			 reevaluation
 *   oid(in) : The record that was modified by other transactions and is
 *	       involved in filters.
 *   recdes(in): Record descriptor that will contain the record
 */
static DB_LOGICAL
mvcc_reevaluate_filters (THREAD_ENTRY * thread_p, MVCC_SCAN_REEV_DATA * mvcc_reev_data, OID * oid, RECDES * recdes)
{
  FILTER_INFO *filter;
  DB_LOGICAL ev_res = V_TRUE;

  filter = mvcc_reev_data->range_filter;
  if (filter != NULL && filter->scan_pred != NULL && filter->scan_pred->pred_expr != NULL)
    {
      if (heap_attrinfo_read_dbvalues (thread_p, oid, recdes, NULL, filter->scan_attrs->attr_cache) != NO_ERROR)
	{
	  return V_ERROR;
	}
      ev_res = (*filter->scan_pred->pr_eval_fnc) (thread_p, filter->scan_pred->pred_expr, filter->val_descr, oid);
      ev_res = update_logical_result (thread_p, ev_res, NULL, NULL, NULL, NULL);
      if (ev_res != V_TRUE)
	{
	  goto end;
	}
    }

  filter = mvcc_reev_data->key_filter;
  if (filter != NULL && filter->scan_pred != NULL && filter->scan_pred->pred_expr != NULL)
    {
      if (heap_attrinfo_read_dbvalues (thread_p, oid, recdes, NULL, filter->scan_attrs->attr_cache) != NO_ERROR)
	{
	  return V_ERROR;
	}
      ev_res = (*filter->scan_pred->pr_eval_fnc) (thread_p, filter->scan_pred->pred_expr, filter->val_descr, oid);
      ev_res = update_logical_result (thread_p, ev_res, NULL, NULL, NULL, NULL);
      if (ev_res != V_TRUE)
	{
	  goto end;
	}
    }

  filter = mvcc_reev_data->data_filter;
  if (filter != NULL && filter->scan_pred != NULL && filter->scan_pred->pred_expr != NULL)
    {
      ev_res = eval_data_filter (thread_p, oid, recdes, NULL, filter);
      ev_res =
	update_logical_result (thread_p, ev_res, (int *) mvcc_reev_data->qualification, mvcc_reev_data->key_filter,
			       recdes, oid);
    }

end:
  return ev_res;
}

/*
 * heap_mvcc_reeval_scan_filters () - reevaluates conditions for a scan table
 *				    at update/delete stage of an UPDATE/DELETE
 *				    statement
 *   return: result of reevaluation
 *   thread_p(in): thread entry
 *   oid(in) : The OID of the latest version of record that triggered
 *	       reevaluation
 *   scan_cache(in): scan_cache
 *   recdes(in): Record descriptor of the record to be updated/deleted.
 *   mvcc_cond_reeval(in): The structure that contains data needed for
 *			   reevaluation
 *   is_upddel(in): true if current scan is updated/deleted when reevaluation
 *		    occured.
 *
 *  Note: The current transaction already acquired X-LOCK on oid parameter
 *	    before calling this function. If the condition returns false then
 *	    the lock must be released by the caller.
 *	  The function reevaluates entire condition: key range, key filter and
 *	    data filter.
 */
static DB_LOGICAL
heap_mvcc_reeval_scan_filters (THREAD_ENTRY * thread_p, OID * oid, HEAP_SCANCACHE * scan_cache, RECDES * recdes,
			       UPDDEL_MVCC_COND_REEVAL * mvcc_cond_reeval, bool is_upddel)
{
  OID *cls_oid = NULL, *oid_inst = NULL;
  MVCC_SCAN_REEV_DATA scan_reev;
  RECDES temp_recdes, *recdesp = NULL;
  HEAP_SCANCACHE local_scan_cache;
  bool scan_cache_inited = false;
  SCAN_CODE scan_code;
  DB_LOGICAL ev_res = V_TRUE;

  cls_oid = &mvcc_cond_reeval->cls_oid;
  if (!is_upddel)
    {
      /* the class is different than the class to be updated/deleted, so use the latest version of row */
      recdesp = &temp_recdes;
      oid_inst = oid;
      if (heap_scancache_quick_start_with_class_hfid (thread_p, &local_scan_cache, &scan_cache->node.hfid) != NO_ERROR)
	{
	  ev_res = V_ERROR;
	  goto end;
	}
      scan_cache_inited = true;
      scan_code = heap_get (thread_p, oid_inst, recdesp, &local_scan_cache, PEEK, NULL_CHN);
      if (scan_code != S_SUCCESS)
	{
	  ev_res = V_ERROR;
	  goto end;
	}
    }
  else
    {
      /* the class to be updated/deleted */
      recdesp = recdes;
      oid_inst = mvcc_cond_reeval->inst_oid;
    }

  if (mvcc_cond_reeval->rest_attrs->num_attrs != 0)
    {
      if (heap_attrinfo_read_dbvalues (thread_p, oid_inst, recdesp, NULL, mvcc_cond_reeval->rest_attrs->attr_cache) !=
	  NO_ERROR)
	{
	  ev_res = V_ERROR;
	  goto end;
	}

      if (fetch_val_list (thread_p, mvcc_cond_reeval->rest_regu_list, NULL, cls_oid, oid_inst, NULL, PEEK) != NO_ERROR)
	{
	  ev_res = V_ERROR;
	  goto end;
	}
    }

  if (mvcc_cond_reeval->range_filter.scan_pred != NULL || mvcc_cond_reeval->key_filter.scan_pred != NULL
      || mvcc_cond_reeval->data_filter.scan_pred != NULL)
    {
      /* evaluate conditions */
      scan_reev.range_filter = &mvcc_cond_reeval->range_filter;
      scan_reev.key_filter = &mvcc_cond_reeval->key_filter;
      scan_reev.data_filter = &mvcc_cond_reeval->data_filter;
      scan_reev.qualification = &mvcc_cond_reeval->qualification;
      ev_res = mvcc_reevaluate_filters (thread_p, &scan_reev, oid_inst, recdesp);
    }

end:
  if (scan_cache_inited)
    {
      heap_scancache_end (thread_p, &local_scan_cache);
    }

  return ev_res;
}

/*
 * heap_mvcc_reev_cond_assigns () - reevaluates conditions and assignments
 *				    at update/delete stage of an UPDATE/DELETE
 *				    statement
 *   return: result of reevaluation
 *   thread_p(in): thread entry
 *   class_oid(in): OID of the class that triggered reevaluation
 *   oid(in) : The OID of the latest version of record that triggered
 *	       reevaluation
 *   scan_cache(in): scan_cache
 *   recdes(in): Record descriptor that will contain the updated/deleted record
 *   mvcc_reev_data(in): The structure that contains data needed for
 *			 reevaluation
 *
 *  Note: the current transaction already acquired X-LOCK on oid parameter
 *	    before calling this function. If the condition returns false then
 *	    the lock must be released by the caller.
 *	  The function reevaluates entire condition: key range, key filter and
 *	    data filter.
 *	  This function allocates memory for recdes and deallocates it if it
 *	    was already allocated for previous reevaluated record. After last
 *	    reevaluation this memory must be deallocated by one of its callers
 *	    (e.g. qexec_execute_update).
 */
static DB_LOGICAL
heap_mvcc_reev_cond_assigns (THREAD_ENTRY * thread_p, OID * class_oid, OID * oid, HEAP_SCANCACHE * scan_cache,
			     RECDES * recdes, MVCC_UPDDEL_REEV_DATA * mvcc_reev_data)
{
  DB_LOGICAL ev_res = V_TRUE;
  UPDDEL_MVCC_COND_REEVAL *mvcc_cond_reeval = NULL;
  int idx;

  /* reevaluate condition for each class involved into */
  for (mvcc_cond_reeval = mvcc_reev_data->mvcc_cond_reev_list; mvcc_cond_reeval != NULL;
       mvcc_cond_reeval = mvcc_cond_reeval->next)
    {
      ev_res =
	heap_mvcc_reeval_scan_filters (thread_p, oid, scan_cache, recdes, mvcc_cond_reeval,
				       mvcc_reev_data->curr_upddel == mvcc_cond_reeval);
      if (ev_res != V_TRUE)
	{
	  goto end;
	}
    }

  if (mvcc_reev_data->new_recdes == NULL)
    {
      /* Seems that the caller wants to reevaluate only the condition */
      goto end;
    }

  /* reload data from classes involved only in right side of assignments (not in condition) */
  if (mvcc_reev_data->curr_extra_assign_reev != NULL)
    {
      for (idx = 0; idx < mvcc_reev_data->curr_extra_assign_cnt; idx++)
	{
	  mvcc_cond_reeval = mvcc_reev_data->curr_extra_assign_reev[idx];
	  ev_res = heap_mvcc_reeval_scan_filters (thread_p, oid, scan_cache, recdes, mvcc_cond_reeval, false);
	  if (ev_res != V_TRUE)
	    {
	      goto end;
	    }
	}
    }

  /* after reevaluation perform assignments */
  if (mvcc_reev_data->curr_assigns != NULL)
    {
      UPDATE_MVCC_REEV_ASSIGNMENT *assign = mvcc_reev_data->curr_assigns;
      int rc;
      DB_VALUE *dbval = NULL;

      if (heap_attrinfo_clear_dbvalues (mvcc_reev_data->curr_attrinfo) != NO_ERROR)
	{
	  ev_res = V_ERROR;
	  goto end;
	}
      for (; assign != NULL; assign = assign->next)
	{
	  if (assign->constant != NULL)
	    {
	      rc = heap_attrinfo_set (oid, assign->att_id, assign->constant, mvcc_reev_data->curr_attrinfo);
	    }
	  else
	    {
	      if (fetch_peek_dbval (thread_p, assign->regu_right, mvcc_reev_data->vd, (OID *) class_oid, oid, NULL,
				    &dbval) != NO_ERROR)
		{
		  ev_res = V_ERROR;
		  goto end;
		}
	      rc = heap_attrinfo_set (oid, assign->att_id, dbval, mvcc_reev_data->curr_attrinfo);
	    }
	  if (rc != NO_ERROR)
	    {
	      ev_res = V_ERROR;
	      goto end;
	    }
	}

      /* TO DO - reuse already allocated area */
      if (mvcc_reev_data->copyarea != NULL)
	{
	  locator_free_copy_area (mvcc_reev_data->copyarea);
	  mvcc_reev_data->new_recdes->data = NULL;
	  mvcc_reev_data->new_recdes->area_size = 0;
	}
      mvcc_reev_data->copyarea =
	locator_allocate_copy_area_by_attr_info (thread_p, mvcc_reev_data->curr_attrinfo, recdes,
						 mvcc_reev_data->new_recdes, -1, LOB_FLAG_INCLUDE_LOB);
      if (mvcc_reev_data->copyarea == NULL)
	{
	  ev_res = V_ERROR;
	  goto end;
	}
    }

end:

  return ev_res;
}

/*
 * heap_is_mvcc_disabled_for_class () - MVCC is disabled for root class and
 *					db_serial, db_partition.
 *
 * return	  : True if MVCC is disabled for class.
 * thread_p (in)  : Thread entry.
 * class_oid (in) : Class OID.
 */
bool
heap_is_mvcc_disabled_for_class (const OID * class_oid)
{
  if (OID_ISNULL (class_oid) || OID_IS_ROOTOID (class_oid))
    {
      /* MVCC is disabled for root class */
      return true;
    }

  if (oid_is_serial (class_oid))
    {
      return true;
    }

  if (oid_check_cached_class_oid (OID_CACHE_COLLATION_CLASS_ID, class_oid))
    {
      return true;
    }

  if (oid_check_cached_class_oid (OID_CACHE_HA_APPLY_INFO_CLASS_ID, class_oid))
    {
      return true;
    }

  return false;
}

/*
 * heap_get_class_oid_from_page () - Gets heap page owner class OID.
 *
 * return	   : Error code.
 * thread_p (in)   : Thread entry.
 * page_p (in)	   : Heap page.
 * class_oid (out) : Class identifier.
 */
int
heap_get_class_oid_from_page (THREAD_ENTRY * thread_p, PAGE_PTR page_p, OID * class_oid)
{
  RECDES chain_recdes;
  HEAP_CHAIN *chain;

  if (spage_get_record (page_p, HEAP_HEADER_AND_CHAIN_SLOTID, &chain_recdes, PEEK) != S_SUCCESS)
    {
      assert (0);
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      return ER_FAILED;
    }

  chain = (HEAP_CHAIN *) chain_recdes.data;
  COPY_OID (class_oid, &(chain->class_oid));

  /* 
   * kludge, root class is identified with a NULL class OID but we must
   * substitute the actual OID here - think about this
   */
  if (OID_ISNULL (class_oid))
    {
      /* root class class oid, substitute with global */
      COPY_OID (class_oid, oid_Root_class_oid);
    }
  return NO_ERROR;
}

/*
 * heap_mvcc_log_home_change_on_delete () - Log the change of record in home
 *					    page when MVCC delete does not
 *					    change a REC_HOME to REC_HOME.
 *
 * return	   : Void.
 * thread_p (in)   : Thread entry.
 * old_recdes (in) : NULL or a REC_RELOCATION record.
 * new_recdes (in) : Record including delete info (MVCCID and next version).
 * p_addr (in)	   : Log data address.
 */
static void
heap_mvcc_log_home_change_on_delete (THREAD_ENTRY * thread_p, RECDES * old_recdes, RECDES * new_recdes,
				     LOG_DATA_ADDR * p_addr)
{
  HEAP_PAGE_VACUUM_STATUS vacuum_status = heap_page_get_vacuum_status (thread_p, p_addr->pgptr);

  /* REC_RELOCATION type record was brought back to home page or REC_HOME has been converted to
   * REC_RELOCATION/REC_BIGONE. */

  /* Update heap chain for vacuum. */
  heap_page_update_chain_after_mvcc_op (thread_p, p_addr->pgptr, logtb_get_current_mvccid (thread_p));
  if (heap_page_get_vacuum_status (thread_p, p_addr->pgptr) != vacuum_status)
    {
      /* Mark vacuum status change for recovery. */
      p_addr->offset |= HEAP_RV_FLAG_VACUUM_STATUS_CHANGE;
    }
  log_append_undoredo_recdes (thread_p, RVHF_MVCC_DELETE_MODIFY_HOME, p_addr, old_recdes, new_recdes);
}

/*
 * heap_mvcc_log_home_no_change_on_delete () - Update page chain for vacuum
 *					       and notify vacuum even when
 *					       home page is not changed.
 *					       Used by delete of
 *					       REC_RELOCATION and REC_BIGONE.
 *
 * return	 : Void.
 * thread_p (in) : Thread entry.
 * p_addr (in)	 : Data address for logging.
 */
static void
heap_mvcc_log_home_no_change_on_delete (THREAD_ENTRY * thread_p, LOG_DATA_ADDR * p_addr)
{
  HEAP_PAGE_VACUUM_STATUS vacuum_status = heap_page_get_vacuum_status (thread_p, p_addr->pgptr);
  heap_page_update_chain_after_mvcc_op (thread_p, p_addr->pgptr, logtb_get_current_mvccid (thread_p));
  if (vacuum_status != heap_page_get_vacuum_status (thread_p, p_addr->pgptr))
    {
      /* Mark vacuum status change for recovery. */
      p_addr->offset |= HEAP_RV_FLAG_VACUUM_STATUS_CHANGE;
    }
  log_append_undoredo_data (thread_p, RVHF_MVCC_DELETE_NO_MODIFY_HOME, p_addr, 0, 0, NULL, NULL);
}

/*
 * heap_rv_undoredo_update_and_update_chain () - Redo update record as part of
 *						 MVCC delete operation.
 *   return: int
 *   rcv(in): Recovery structure
 */
int
heap_rv_undoredo_update_and_update_chain (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  int error_code = NO_ERROR;
  bool vacuum_status_change = false;
  PGSLOTID slotid;

  assert (rcv->pgptr != NULL);
  assert (MVCCID_IS_NORMAL (rcv->mvcc_id));

  slotid = rcv->offset;
  if (slotid & HEAP_RV_FLAG_VACUUM_STATUS_CHANGE)
    {
      vacuum_status_change = true;
    }
  slotid = slotid & (~HEAP_RV_FLAG_VACUUM_STATUS_CHANGE);
  assert (slotid > 0);

  error_code = heap_rv_undoredo_update (thread_p, rcv);
  if (error_code != NO_ERROR)
    {
      ASSERT_ERROR ();
      return error_code;
    }

  heap_page_rv_chain_update (thread_p, rcv->pgptr, rcv->mvcc_id, vacuum_status_change);
  /* Page was already marked as dirty */
  return NO_ERROR;
}

/*
 * heap_attrinfo_check_unique_index () - check whether exists an unique index on
 *					specified attributes
 *   return: true, if there is an index containing specified attributes
 *   thread_p(in): thread entry
 *   attr_info(in): attribute info
 *   att_id(in): attribute ids
 *   n_att_id(in): count attributes
 */
bool
heap_attrinfo_check_unique_index (THREAD_ENTRY * thread_p, HEAP_CACHE_ATTRINFO * attr_info, ATTR_ID * att_id,
				  int n_att_id)
{
  OR_INDEX *index;
  int num_btids, i, j, k;

  if (attr_info == NULL || att_id == NULL)
    {
      return false;
    }

  num_btids = attr_info->last_classrepr->n_indexes;
  for (i = 0; i < num_btids; i++)
    {
      index = &(attr_info->last_classrepr->indexes[i]);
      if (btree_is_unique_type (index->type))
	{
	  for (j = 0; j < n_att_id; j++)
	    {
	      for (k = 0; k < index->n_atts; k++)
		{
		  if (att_id[j] == (ATTR_ID) (index->atts[k]->id))
		    {		/* the index key_type has updated attr */
		      return true;
		    }
		}
	    }
	}
    }

  return false;
}

/*
 * heap_mvcc_reev_cond_and_assignment () -
 *
 *   return: DB_LOGICAL
 *   thread_p(in): thread entry
 *   scan_cache(in):
 *   mvcc_reev_data_p(in/out):
 *   mvcc_header_p(in):
 *   curr_row_version_oid_p(in):
 *   recdes(in):
 */
/* TODO: We need to reevaluate relation between primary key * and foreign key. */
static DB_LOGICAL
heap_mvcc_reev_cond_and_assignment (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache,
				    MVCC_REEV_DATA * mvcc_reev_data_p, MVCC_REC_HEADER * mvcc_header_p,
				    OID * curr_row_version_oid_p, RECDES * recdes)
{
  bool ev_res = V_TRUE;

  if (mvcc_reev_data_p == NULL)
    {
      return ev_res;
    }

  assert (mvcc_header_p != NULL && curr_row_version_oid_p != NULL);

  ev_res = V_TRUE;
  if (!MVCC_IS_REC_INSERTED_BY_ME (thread_p, mvcc_header_p))
    {
      if (mvcc_reev_data_p->primary_key != NULL)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_MVCC_ROW_INVALID_FOR_DELETE, 3, curr_row_version_oid_p->volid,
		  curr_row_version_oid_p->pageid, curr_row_version_oid_p->slotid);
	  ev_res = V_ERROR;
	}
      else
	{
	  switch (mvcc_reev_data_p->type)
	    {
	    case REEV_DATA_SCAN:
	      ev_res =
		mvcc_reevaluate_filters (thread_p, mvcc_reev_data_p->select_reev_data, curr_row_version_oid_p, recdes);
	      mvcc_reev_data_p->filter_result = ev_res;
	      break;

	    case REEV_DATA_UPDDEL:
	      ev_res =
		heap_mvcc_reev_cond_assigns (thread_p, &scan_cache->node.class_oid, curr_row_version_oid_p, scan_cache,
					     recdes, mvcc_reev_data_p->upddel_reev_data);
	      mvcc_reev_data_p->filter_result = ev_res;
	      break;

	    default:
	      break;
	    }
	}
    }
  else
    {
      mvcc_reev_data_p->filter_result = V_TRUE;
    }

  return ev_res;
}

/*
 * heap_get_from_page_no_snapshot () -
 *
 *   return: SCAN_CODE
 *   thread_p(in): thread entry
 *   scan_cache(in):
 *   mvcc_reev_data_p(in):
 *   mvcc_header_p(in/out):
 *   curr_row_version_oid_p(in):
 *   recdes(in):
 *   ispeeking(in):
 */
static SCAN_CODE
heap_get_from_page_no_snapshot (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, MVCC_REEV_DATA * mvcc_reev_data_p,
				MVCC_REC_HEADER * mvcc_header_p, OID * curr_row_version_oid_p, RECDES * recdes,
				int ispeeking)
{
  MVCC_SNAPSHOT *saved_snapshot;
  int saved_cache_last_fix_page;
  SCAN_CODE scan;

  assert (scan_cache != NULL);
  assert (scan_cache->page_watcher.pgptr != NULL);
  assert (curr_row_version_oid_p != NULL);
  assert (mvcc_header_p != NULL);
  assert (recdes != NULL);

  saved_snapshot = scan_cache->mvcc_snapshot;
  scan_cache->mvcc_snapshot = NULL;

  saved_cache_last_fix_page = scan_cache->cache_last_fix_page;
  scan_cache->cache_last_fix_page = true;

  scan = heap_get (thread_p, curr_row_version_oid_p, recdes, scan_cache, ispeeking, NULL_CHN);

  if (mvcc_reev_data_p != NULL)
    {
      or_mvcc_get_header (recdes, mvcc_header_p);
    }

  scan_cache->cache_last_fix_page = saved_cache_last_fix_page;
  scan_cache->mvcc_snapshot = saved_snapshot;

  return scan;
}

/*
 * heap_get_mvcc_record_header () -
 *
 *   return: SCAN_CODE
 *   thread_p(in): thread entry
 *   mvcc_header_p(in/out):
 *   mvcc_snapshot_p(in):
 *   pgptr(in):
 *   slotid(in):
 */
static SCAN_CODE
heap_get_mvcc_record_header (THREAD_ENTRY * thread_p, MVCC_REC_HEADER * mvcc_header_p, MVCC_SNAPSHOT * mvcc_snapshot_p,
			     PAGE_PTR pgptr, PGSLOTID slotid)
{
  RECDES recdes;
  SCAN_CODE scan;

  assert (pgptr != NULL);
  assert (mvcc_header_p != NULL);

  scan = spage_get_record_mvcc (pgptr, slotid, &recdes, PEEK);
  if (scan == S_SUCCESS)
    {
      or_mvcc_get_header (&recdes, mvcc_header_p);
      if (mvcc_snapshot_p != NULL && mvcc_snapshot_p->snapshot_fnc != NULL)
	{
	  if (mvcc_snapshot_p->snapshot_fnc (thread_p, mvcc_header_p, mvcc_snapshot_p) == false)
	    {
	      scan = S_SNAPSHOT_NOT_SATISFIED;
	    }
	}
    }

  return scan;
}

/*
 * heap_ovf_get_mvcc_record_header () -
 *
 *   return: SCAN_CODE
 *   thread_p(in): thread entry
 *   mvcc_header_p(in/out):
 *   mvcc_snapshot_p(in):
 *   ovf_first_page(in):
 */
static SCAN_CODE
heap_ovf_get_mvcc_record_header (THREAD_ENTRY * thread_p, MVCC_REC_HEADER * mvcc_header_p,
				 MVCC_SNAPSHOT * mvcc_snapshot_p, PAGE_PTR ovf_first_pgptr)
{
  RECDES recdes;
  SCAN_CODE scan;

  assert (mvcc_header_p != NULL);
  assert (ovf_first_pgptr != NULL);

  recdes.data = overflow_get_first_page_data (ovf_first_pgptr);
  recdes.length = OR_HEADER_SIZE (recdes.data);
  or_mvcc_get_header (&recdes, mvcc_header_p);

  scan = S_SUCCESS;
  if (mvcc_snapshot_p != NULL)
    {
      if (mvcc_snapshot_p->snapshot_fnc (thread_p, mvcc_header_p, mvcc_snapshot_p) != true)
	{
	  scan = S_SNAPSHOT_NOT_SATISFIED;
	}
    }

  return scan;
}

#if defined(ENABLE_UNUSED_FUNCTION)
/*
 * heap_try_fetch_header_page () -
 *                  try to fetch header page, having home page already fetched
 *
 *   return: error code
 *   thread_p(in): thread entry
 *   home_pgptr_p(out):
 *   home_vpid_p(in):
 *   oid_p(in):
 *   hdr_pgptr_p(out):
 *   hdr_vpid_p(in):
 *   scan_cache(in):
 *   again_count_p(in/out):
 *   again_max(in):
 */
/* TODO - fix er_clear */
STATIC_INLINE int
heap_try_fetch_header_page (THREAD_ENTRY * thread_p, PAGE_PTR * home_pgptr_p, const VPID * home_vpid_p,
			    const OID * oid_p, PAGE_PTR * hdr_pgptr_p, const VPID * hdr_vpid_p,
			    HEAP_SCANCACHE * scan_cache, int *again_count_p, int again_max)
{
  int error_code = NO_ERROR;

  *hdr_pgptr_p = pgbuf_fix (thread_p, hdr_vpid_p, OLD_PAGE, PGBUF_LATCH_WRITE, PGBUF_CONDITIONAL_LATCH);
  if (*hdr_pgptr_p != NULL)
    {
      return NO_ERROR;
    }

  pgbuf_unfix_and_init (thread_p, *home_pgptr_p);
  *hdr_pgptr_p = heap_scan_pb_lock_and_fetch (thread_p, hdr_vpid_p, OLD_PAGE, X_LOCK, scan_cache, NULL);
  if (*hdr_pgptr_p == NULL)
    {
      error_code = er_errid ();
      if (error_code == ER_PB_BAD_PAGEID)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, hdr_vpid_p->volid, hdr_vpid_p->pageid,
		  0);
	  error_code = ER_HEAP_UNKNOWN_OBJECT;
	}
    }
  else
    {
      *home_pgptr_p = pgbuf_fix (thread_p, home_vpid_p, OLD_PAGE, PGBUF_LATCH_WRITE, PGBUF_CONDITIONAL_LATCH);
      if (*home_pgptr_p == NULL)
	{
	  pgbuf_unfix_and_init (thread_p, *hdr_pgptr_p);
	  if ((*again_count_p)++ >= again_max)
	    {
	      error_code = er_errid ();
	      if (error_code == ER_PB_BAD_PAGEID)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid_p->volid, oid_p->pageid,
			  oid_p->slotid);
		  error_code = ER_HEAP_UNKNOWN_OBJECT;
		}
	      else if (error_code == NO_ERROR)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, home_vpid_p->volid,
			  home_vpid_p->pageid);
		  error_code = ER_PAGE_LATCH_ABORTED;
		}
	    }
	}
    }

  return error_code;
}

/*
 * heap_try_fetch_forward_page () -
 *                  try to fetch forward page, having home page already fetched
 *
 *   return: error code
 *   thread_p(in): thread entry
 *   home_pgptr_p(out):
 *   home_vpid_p(in):
 *   oid_p(in):
 *   fwd_pgptr_p(out):
 *   fwd_vpid_p(in):
 *   fwd_oid_p(in):
 *   scan_cache(in):
 *   again_count_p(in/out):
 *   again_max(in):
 */
STATIC_INLINE int
heap_try_fetch_forward_page (THREAD_ENTRY * thread_p, PAGE_PTR * home_pgptr_p, const VPID * home_vpid_p,
			     const OID * oid_p, PAGE_PTR * fwd_pgptr_p, const VPID * fwd_vpid_p, const OID * fwd_oid_p,
			     HEAP_SCANCACHE * scan_cache, int *again_count_p, int again_max)
{
  int error_code = NO_ERROR;

  *fwd_pgptr_p = pgbuf_fix (thread_p, fwd_vpid_p, OLD_PAGE, PGBUF_LATCH_WRITE, PGBUF_CONDITIONAL_LATCH);
  if (*fwd_pgptr_p != NULL)
    {
      return NO_ERROR;
    }

  pgbuf_unfix_and_init (thread_p, *home_pgptr_p);
  *fwd_pgptr_p = heap_scan_pb_lock_and_fetch (thread_p, fwd_vpid_p, OLD_PAGE, X_LOCK, scan_cache, NULL);
  if (*fwd_pgptr_p == NULL)
    {
      error_code = er_errid ();
      if (error_code == ER_PB_BAD_PAGEID)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, fwd_oid_p->volid, fwd_oid_p->pageid,
		  fwd_oid_p->slotid);
	  error_code = ER_HEAP_UNKNOWN_OBJECT;
	}
    }
  else
    {
      *home_pgptr_p = pgbuf_fix (thread_p, home_vpid_p, OLD_PAGE, PGBUF_LATCH_WRITE, PGBUF_CONDITIONAL_LATCH);
      if (*home_pgptr_p == NULL)
	{
	  pgbuf_unfix_and_init (thread_p, *fwd_pgptr_p);
	  if ((*again_count_p)++ >= again_max)
	    {
	      error_code = er_errid ();
	      if (error_code == ER_PB_BAD_PAGEID)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid_p->volid, oid_p->pageid,
			  oid_p->slotid);
		  error_code = ER_HEAP_UNKNOWN_OBJECT;
		}
	      else if (error_code == NO_ERROR)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, home_vpid_p->volid,
			  home_vpid_p->pageid);
		  error_code = ER_PAGE_LATCH_ABORTED;
		}
	    }
	}
    }

  return error_code;
}

/*
 * heap_try_fetch_header_with_forward_page () -
 *       try to fetch header and forward page, having home page already fetched
 *
 *   return: error code
 *   thread_p(in): thread entry
 *   home_pgptr_p(out):
 *   home_vpid_p(in):
 *   oid_p(in):
 *   hdr_pgptr_p(out):
 *   hdr_vpid_p(in):
 *   fwd_pgptr_p(out):
 *   fwd_vpid_p(in):
 *   fwd_oid_p(in):
 *   scan_cache(in):
 *   again_count_p(in/out):
 *   again_max(in):
 */
STATIC_INLINE int
heap_try_fetch_header_with_forward_page (THREAD_ENTRY * thread_p, PAGE_PTR * home_pgptr_p, const VPID * home_vpid_p,
					 const OID * oid_p, PAGE_PTR * hdr_pgptr_p, const VPID * hdr_vpid_p,
					 PAGE_PTR * fwd_pgptr_p, const VPID * fwd_vpid_p, const OID * fwd_oid_p,
					 HEAP_SCANCACHE * scan_cache, int *again_count_p, int again_max)
{
  int error_code = NO_ERROR;

  *hdr_pgptr_p = pgbuf_fix (thread_p, hdr_vpid_p, OLD_PAGE, PGBUF_LATCH_WRITE, PGBUF_CONDITIONAL_LATCH);
  if (*hdr_pgptr_p != NULL)
    {
      return NO_ERROR;
    }

  pgbuf_unfix_and_init (thread_p, *home_pgptr_p);
  pgbuf_unfix_and_init (thread_p, *fwd_pgptr_p);
  *hdr_pgptr_p = heap_scan_pb_lock_and_fetch (thread_p, hdr_vpid_p, OLD_PAGE, X_LOCK, scan_cache, NULL);
  if (*hdr_pgptr_p == NULL)
    {
      error_code = er_errid ();
      if (error_code == ER_PB_BAD_PAGEID)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, hdr_vpid_p->volid, hdr_vpid_p->pageid,
		  0);
	  error_code = ER_HEAP_UNKNOWN_OBJECT;
	}
    }
  else
    {
      *home_pgptr_p = pgbuf_fix (thread_p, home_vpid_p, OLD_PAGE, PGBUF_LATCH_WRITE, PGBUF_CONDITIONAL_LATCH);
      if (*home_pgptr_p == NULL)
	{
	  pgbuf_unfix_and_init (thread_p, *hdr_pgptr_p);
	  if ((*again_count_p)++ >= again_max)
	    {
	      error_code = er_errid ();
	      if (error_code == ER_PB_BAD_PAGEID)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid_p->volid, oid_p->pageid,
			  oid_p->slotid);
		  error_code = ER_HEAP_UNKNOWN_OBJECT;
		}
	      else if (error_code == NO_ERROR)
		{
		  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, home_vpid_p->volid,
			  home_vpid_p->pageid);
		  error_code = ER_PAGE_LATCH_ABORTED;
		}
	    }
	}
      else
	{
	  *fwd_pgptr_p = pgbuf_fix (thread_p, fwd_vpid_p, OLD_PAGE, PGBUF_LATCH_WRITE, PGBUF_CONDITIONAL_LATCH);
	  if (*fwd_pgptr_p == NULL)
	    {
	      pgbuf_unfix_and_init (thread_p, *hdr_pgptr_p);
	      pgbuf_unfix_and_init (thread_p, *home_pgptr_p);
	      if ((*again_count_p)++ >= again_max)
		{
		  error_code = er_errid ();
		  if (error_code == ER_PB_BAD_PAGEID)
		    {
		      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, fwd_oid_p->volid,
			      fwd_oid_p->pageid, fwd_oid_p->slotid);
		      error_code = ER_HEAP_UNKNOWN_OBJECT;
		    }
		  else if (er_errid () == NO_ERROR)
		    {
		      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, fwd_vpid_p->volid,
			      fwd_vpid_p->pageid);
		    }
		}
	    }
	}
    }

  return error_code;
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * heap_get_header_page () -
 *   return: pageid or NULL_PAGEID
 *   btid(in): Heap file identifier
 *   header_vpid(out):
 *
 * Note: get the page identifier of the first allocated page of the given file.
 */
VPID *
heap_get_header_page (THREAD_ENTRY * thread_p, HFID * hfid, VPID * header_vpid)
{
  VPID *vpid;

  assert (!VFID_ISNULL (&hfid->vfid));

  vpid = file_get_first_alloc_vpid (thread_p, &hfid->vfid, header_vpid);

  assert (!VPID_ISNULL (header_vpid));

  return vpid;
}

/*
 * heap_scancache_quick_start_root_hfid () - Start caching information for a
 *					     heap scan on root hfid
 *   return: NO_ERROR
 *   thread_p(in):
 *   scan_cache(in/out): Scan cache
 *
 * Note: this is similar to heap_scancache_quick_start, except it sets the
 *	 HFID of root in the scan_cache (otherwise remains NULL).
 *	 This should be used to avoid inconsistency when using ordered fix.
 */
int
heap_scancache_quick_start_root_hfid (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache)
{
  HFID root_hfid;

  (void) boot_find_root_heap (&root_hfid);
  (void) heap_scancache_quick_start_internal (scan_cache, &root_hfid);
  scan_cache->page_latch = S_LOCK;

  return NO_ERROR;
}


/*
 * heap_scancache_quick_start_with_class_oid () - Start caching information for
 *						   a heap scan on a class.
 *
 *   return: NO_ERROR
 *   thread_p(in):
 *   scan_cache(in/out): Scan cache
 *   class_oid(in): class
 *
 * Note: this is similar to heap_scancache_quick_start, except it sets the
 *	 HFID of class in the scan_cache (otherwise remains NULL).
 *	 This should be used to avoid inconsistency when using ordered fix.
 *	 This has a page latch overhead on top of heap_scancache_quick_start.
 *
 */
int
heap_scancache_quick_start_with_class_oid (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, OID * class_oid)
{
  HFID class_hfid;

  heap_get_hfid_from_class_oid (thread_p, class_oid, &class_hfid);
  (void) heap_scancache_quick_start_with_class_hfid (thread_p, scan_cache, &class_hfid);
  scan_cache->page_latch = S_LOCK;

  return NO_ERROR;
}

/*
 * heap_scancache_quick_start_with_class_hfid () - Start caching information for
 *						   a heap scan on a class.
 *
 *   return: NO_ERROR
 *   thread_p(in):
 *   scan_cache(in/out): Scan cache
 *   class_oid(in): class
 *
 * Note: this is similar to heap_scancache_quick_start, except it sets the
 *	 HFID of class in the scan_cache (otherwise remains NULL).
 *	 This should be used to avoid inconsistency when using ordered fix.
 *
 */
int
heap_scancache_quick_start_with_class_hfid (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, const HFID * hfid)
{
  (void) heap_scancache_quick_start_internal (scan_cache, hfid);
  scan_cache->page_latch = S_LOCK;

  return NO_ERROR;
}


/*
 * heap_scancache_quick_start_modify_with_class_oid () -
 *			Start caching information for a heap scan on class.
 *
 *   return: NO_ERROR
 *   thread_p(in):
 *   scan_cache(in/out): Scan cache
 *   class_oid(in): class
 *
 * Note: this is similar to heap_scancache_quick_start_modify, except it sets
 *	 the HFID of class in the scan_cache (otherwise remains NULL).
 *	 This should be used to avoid inconsistency when using ordered fix.
 *	 This has a page latch overhead on top of heap_scancache_quick_start.
 */
int
heap_scancache_quick_start_modify_with_class_oid (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, OID * class_oid)
{
  HFID class_hfid;

  heap_get_hfid_from_class_oid (thread_p, class_oid, &class_hfid);
  (void) heap_scancache_quick_start_internal (scan_cache, &class_hfid);
  scan_cache->page_latch = X_LOCK;

  return NO_ERROR;
}

/*
 * heap_mvcc_lock_object () - lock the last version of the object, if the
 *			      object is not deleted 
 *
 *   return: error code
 *   thread_p(in): thread entry
 *   oid_p(in):	OID
 *   class_oid_p(in): class OID
 *   lock_mode(in): lock mode
 *   snapshot_type(in): snapshot type
 *   locked_oid(out): locked oid
 *
 * NOTE: Class must be known and MVCC enabled.
 */
SCAN_CODE
heap_mvcc_lock_object (THREAD_ENTRY * thread_p, OID * oid, OID * class_oid, LOCK lock_mode, SNAPSHOT_TYPE snapshot_type,
		       OID * locked_oid)
{
  HEAP_SCANCACHE scan_cache;
  MVCC_SNAPSHOT mvcc_snapshot_dirty;
  SCAN_OPERATION_TYPE scan_operation_type = S_SELECT;
  SCAN_CODE scan_code;
  bool scan_cache_started = false;

  assert (oid != NULL);
  assert (class_oid != NULL && !heap_is_mvcc_disabled_for_class (class_oid));

  OID_SET_NULL (locked_oid);
  if (lock_mode == NULL_LOCK)
    {
      return S_SUCCESS;
    }

  if (lock_mode <= S_LOCK)
    {
      /* S_LOCK */
      scan_operation_type = S_SELECT_WITH_LOCK;
    }
  else
    {
      /* X_LOCK */
      scan_operation_type = S_DELETE;
    }

  heap_scancache_quick_start (&scan_cache);
  scan_cache_started = true;

  if (snapshot_type == SNAPSHOT_TYPE_MVCC)
    {
      scan_cache.mvcc_snapshot = logtb_get_mvcc_snapshot (thread_p);
      if (scan_cache.mvcc_snapshot == NULL)
	{
	  goto exit_on_error;
	}
    }
  else if (snapshot_type == SNAPSHOT_TYPE_DIRTY)
    {
      /* get dirty version with lock */
      mvcc_snapshot_dirty.snapshot_fnc = mvcc_satisfies_dirty;
      scan_cache.mvcc_snapshot = &mvcc_snapshot_dirty;
    }

  scan_code =
    heap_mvcc_lock_and_get_object_version (thread_p, oid, NULL, NULL, &scan_cache, scan_operation_type, true, NULL_CHN,
					   NULL, locked_oid, LOG_ERROR_IF_DELETED);
  if (scan_code == S_ERROR)
    {
      ASSERT_ERROR ();
      goto exit_on_error;
    }

  heap_scancache_end (thread_p, &scan_cache);
  return scan_code;

exit_on_error:
  if (scan_cache_started)
    {
      heap_scancache_end (thread_p, &scan_cache);
    }
  return S_ERROR;
}

/*
 * heap_mvcc_lock_scan_and_set_scancache_node () - lock a scan for partition
 *					hopping.
 *					Sets the curent node of the scancache
 *					with the given OID. If the lock was not
 *					previously acquired, the node is added
 *					to the scancache's partition list.
 *   thread_p(in): thread entry
 *   partition_oid_p(in): partition OID
 *   home_page_watcher_p(in): home page
 *   fwd_page_watcher_p(in): forward page
 *   unfixed_watchers(out): true if home/fwd page were unfixed for uncond lock
 *   returns: lock result
 */
static int
heap_mvcc_lock_scan_and_set_scancache_node (THREAD_ENTRY * thread_p, OID * partition_oid_p, HEAP_SCANCACHE * scan_cache,
					    PGBUF_WATCHER * home_page_watcher_p, PGBUF_WATCHER * fwd_page_watcher_p,
					    bool * unfixed_watchers)
{
  LOCK curr_lock = NULL_LOCK;
  int rc;
  HEAP_SCANCACHE_NODE_LIST *p = NULL;

  assert (partition_oid_p != NULL);
  assert (!OID_ISNULL (partition_oid_p));
  assert (home_page_watcher_p != NULL);
  assert (fwd_page_watcher_p != NULL);

  if (scan_cache == NULL)
    {
      return LK_GRANTED;
    }
  assert (scan_cache != NULL);

  p = scan_cache->partition_list;
  while (p != NULL)
    {
      if (OID_EQ (partition_oid_p, &p->node.class_oid))
	{
	  HEAP_SCANCACHE_SET_NODE (scan_cache, &p->node.class_oid, &p->node.hfid);
	  /* no need to acquire lock */
	  return LK_GRANTED;
	}
      p = p->next;
    }

  if (home_page_watcher_p->pgptr == NULL && fwd_page_watcher_p->pgptr == NULL)
    {
      /* pages not fixed, we can just go ahead and do an unconditional lock */
      goto unconditional_lock;
    }

  /* try a conditional lock */
  rc = lock_scan (thread_p, partition_oid_p, LK_COND_LOCK, IS_LOCK);
  if (rc != LK_NOTGRANTED_DUE_TIMEOUT)
    {
      /* could be granted, error, aborted ...  */
      if (rc == LK_GRANTED)
	{
	  if (heap_scancache_add_partition_node (thread_p, scan_cache, partition_oid_p) != NO_ERROR)
	    {
	      lock_unlock_scan (thread_p, partition_oid_p, END_SCAN);
	      return LK_NOTGRANTED;
	    }
	}
      return rc;
    }

  /* conditional lock failed; unfix pages and try unconditional */
  if (home_page_watcher_p->pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, home_page_watcher_p);
    }
  if (fwd_page_watcher_p->pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, fwd_page_watcher_p);
    }
  if (unfixed_watchers != NULL)
    {
      *unfixed_watchers = true;
    }

unconditional_lock:
  /* unconditional lock */
  rc = lock_scan (thread_p, partition_oid_p, LK_UNCOND_LOCK, IS_LOCK);
  if (rc == LK_GRANTED)
    {
      if (heap_scancache_add_partition_node (thread_p, scan_cache, partition_oid_p) != NO_ERROR)
	{
	  lock_unlock_scan (thread_p, partition_oid_p, END_SCAN);
	  return LK_NOTGRANTED;
	}
    }

  return rc;
}

/*
 * heap_mvcc_find_next_valid_version () - finds the next valid version,
 *				  excluding the versions that assume moving into
 *				  a partition found in the OID list
 *
 * return : error code
 *
 * thread_p (in)      :
 * oid (in)	      :
 * class_oid (in/out) :
 * oid_list (in)      : partition OID list
 * no_oids (in)	      : number of OIDs in the list
 * next_version (out) : next valid version
 * next_version_class_oid (out): the class OID of the next version object.
 */
static SCAN_CODE
heap_mvcc_find_next_valid_version (THREAD_ENTRY * thread_p, const OID * oid, OID * class_oid, OID * oid_list,
				   int no_oids, OID * next_version, OID * next_version_class_oid)
{
  OID current_oid;		/* Current oid being processed. */
  OID forward_oid;		/* Forward OID - used for REC_RELOCATION/REC_NEWHOME, REC_BIGONE and
				 * REC_MVCC_NEXT_VERSION. */
  SCAN_CODE scan_code;		/* SCAN_CODE result. */
  MVCC_REC_HEADER mvcc_header;	/* Record MVCC header. */
  INT16 type;			/* Record type. */
  OID current_class_oid;
  OID partition_oid;
  int i = 0;
  PGBUF_WATCHER home_page_watcher;
  PGBUF_WATCHER fwd_page_watcher;
  HFID hfid;
  bool is_valid_partition = true;

  PGBUF_INIT_WATCHER (&home_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, PGBUF_ORDERED_NULL_HFID);
  PGBUF_INIT_WATCHER (&fwd_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, PGBUF_ORDERED_NULL_HFID);

  /* Initialize data that may be required for looping through object versions. */
  /* Start with current object. */
  COPY_OID (&current_oid, oid);
  COPY_OID (&current_class_oid, class_oid);

  /* Advance to update chain until the purpose of the function is reached. */
start_current_version:

  if (home_page_watcher.pgptr != NULL)
    {
      /* This belong to a previous version. No longer required. */
      pgbuf_ordered_unfix (thread_p, &home_page_watcher);
    }
  if (fwd_page_watcher.pgptr != NULL)
    {
      /* This belong to a previous version. No longer required. */
      pgbuf_ordered_unfix (thread_p, &fwd_page_watcher);
    }

  OID_SET_NULL (&partition_oid);

  if (heap_get_hfid_from_class_oid (thread_p, &current_class_oid, &hfid) != NO_ERROR)
    {
      scan_code = S_ERROR;
      goto end;
    }

  PGBUF_INIT_WATCHER (&home_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, &hfid);
  PGBUF_INIT_WATCHER (&fwd_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, &hfid);

  /* Prepare to get record. It will obtain class_oid, record type, required pages, and forward_oid. */
  scan_code =
    heap_prepare_get_record (thread_p, &current_oid, &current_class_oid, &forward_oid, &partition_oid,
			     &home_page_watcher, &fwd_page_watcher, &type, PGBUF_LATCH_READ, false,
			     LOG_WARNING_IF_DELETED);
  if (scan_code != S_SUCCESS)
    {
      /* next version might be deleted. This case is expected and removal of partition links must still continue */
      if (er_errid () == ER_HEAP_UNKNOWN_OBJECT && (type == REC_MARKDELETED || type == REC_DELETED_WILL_REUSE))
	{
	  er_clear ();
	  scan_code = S_SUCCESS;
	  OID_SET_NULL (next_version);
	  OID_SET_NULL (next_version_class_oid);
	}
      /* Stop here. */
      goto end;
    }

  /* We are here only if a valid record was found: REC_HOME, REC_BIGONE, REC_RELOCATION or REC_MVCC_NEXT_VERSION. Any
   * other case should be either S_DOESNT_EXIST or S_ERROR. */
  assert (type == REC_HOME || type == REC_RELOCATION || type == REC_BIGONE || type == REC_MVCC_NEXT_VERSION);
  /* We should have required pages fixed. */
  assert (HEAP_IS_PAGE_OF_OID (home_page_watcher.pgptr, &current_oid));
  /* Check forward_page and forward_oid are set when type is REC_RELOCATION or REC_BIGONE. */
  assert ((type != REC_BIGONE && type != REC_RELOCATION)
	  || (!OID_ISNULL (&forward_oid) && HEAP_IS_PAGE_OF_OID (fwd_page_watcher.pgptr, &forward_oid)));

  if (heap_is_mvcc_disabled_for_class (&current_class_oid))
    {
      /* MVCC is disabled for this class. There is only one object version. Also locking for non-MVCC classes is not
       * the scope of this function. Go directly to get_record. */
      if (type == REC_MVCC_NEXT_VERSION)
	{
	  /* Unexpected since this class should not have link. */
	  assert (0);
	  goto error;
	}
    }

  if (type == REC_MVCC_NEXT_VERSION)
    {
      /* Advance to next version. */
      /* heap_prepare_get_record should have output next_version to forward_oid. */
      if (OID_ISNULL (&forward_oid))
	{
	  OID_SET_NULL (next_version);
	  OID_SET_NULL (next_version_class_oid);
	  goto end;
	}
      assert (!OID_ISNULL (&forward_oid));
      COPY_OID (&current_oid, &forward_oid);

      if (OID_ISNULL (&partition_oid) == false)
	{
	  for (i = 0; i < no_oids; i++)
	    {
	      if (OID_EQ (&partition_oid, &oid_list[i]))
		{
		  break;
		}
	    }
	  if (i == no_oids)
	    {
	      /* find the valid next version. Stop */
	      COPY_OID (next_version, &current_oid);
	      COPY_OID (next_version_class_oid, &partition_oid);
	    }
	  else
	    {
	      /* need to filter this partition and follow the link more */
	      COPY_OID (&current_class_oid, &partition_oid);

	      /* remember that we are now scanning an invalid partition. The next versions in the chain that are from
	       * this class will also be invalid. */
	      is_valid_partition = false;
	      goto start_current_version;
	    }
	}
      else
	{
	  /* Next version is a regular OID, not a partition link. The next version object is in the same class as the
	   * current object. */
	  if (is_valid_partition == false)
	    {
	      /* We are dealing with an invalid partition, so continue to follow the chain */
	      goto start_current_version;
	    }
	  else
	    {
	      /* Also return its class OID */
	      COPY_OID (next_version, &current_oid);
	      COPY_OID (next_version_class_oid, &current_class_oid);
	    }
	}
    }
  else
    {
      /* REC_HOME, REC_RELOCATION or REC_BIGONE. */
      assert (type == REC_HOME || type == REC_RELOCATION || type == REC_BIGONE);

      /* Get MVCC header. */
      scan_code =
	heap_get_mvcc_header (thread_p, &current_oid, &forward_oid, home_page_watcher.pgptr, fwd_page_watcher.pgptr,
			      type, &mvcc_header);
      if (scan_code != S_SUCCESS)
	{
	  goto end;
	}

      /* Check if there is next version. */
      if (OID_ISNULL (&MVCC_GET_NEXT_VERSION (&mvcc_header)))
	{
	  OID_SET_NULL (next_version);
	  OID_SET_NULL (next_version_class_oid);
	}
      else
	{
	  /* We must go to next version. */
	  COPY_OID (&current_oid, &MVCC_GET_NEXT_VERSION (&mvcc_header));

	  if (MVCC_IS_HEADER_PARTITION_OID_VALID (&mvcc_header))
	    {
	      OID *partition_oid = NULL;

	      partition_oid = &MVCC_GET_PARTITION_OID (&mvcc_header);
	      for (i = 0; i < no_oids; i++)
		{
		  if (OID_EQ (partition_oid, &oid_list[i]))
		    {
		      break;
		    }
		}

	      if (i == no_oids)
		{
		  /* find the valid next version. Stop */
		  COPY_OID (next_version, &current_oid);
		  COPY_OID (next_version_class_oid, partition_oid);
		}
	      else
		{
		  /* need to filter this partition and follow the link more */
		  COPY_OID (&current_class_oid, partition_oid);

		  /* remember that we are now scanning an invalid partition. The next versions in the chain that are
		   * from this class will also be invalid. */
		  is_valid_partition = false;
		  goto start_current_version;
		}
	    }
	  else
	    {
	      /* Next version is a regular OID, not a partition link. The next version object is in the same class as
	       * the current object. */
	      if (is_valid_partition == false)
		{
		  /* We are dealing with an invalid partition, so continue to follow the chain */
		  goto start_current_version;
		}
	      else
		{
		  /* Also return its class OID */
		  COPY_OID (next_version, &current_oid);
		  COPY_OID (next_version_class_oid, &current_class_oid);
		}
	    }
	}
    }

end:
  if (home_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &home_page_watcher);
    }

  if (fwd_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &fwd_page_watcher);
    }

  COPY_OID (class_oid, &current_class_oid);

  return scan_code;

error:
  scan_code = S_ERROR;
  goto end;
}

/*
 * heap_mvcc_cleanup_partition_link () - 
 *
 * return : error code
 *
 * thread_p (in)      :
 * oid (in)	      :
 * class_oid (in)     :
 * oid_list (in)      : partition OID list
 * no_oids (in)	      : number of OIDs in the list
 */
static SCAN_CODE
heap_mvcc_cleanup_partition_link (THREAD_ENTRY * thread_p, HFID * hfid, const OID * oid, OID * class_oid,
				  PGBUF_WATCHER * home_page_watcher, OID * oid_list, int no_oids)
{
  OID forward_oid;		/* Forward OID - used for REC_RELOCATION/REC_NEWHOME, REC_BIGONE and
				 * REC_MVCC_NEXT_VERSION. */
  OID partition_oid;
  SCAN_CODE scan_code = S_SUCCESS;	/* SCAN_CODE result. */
  MVCC_REC_HEADER mvcc_header;	/* Record MVCC header. */
  INT16 type;			/* Record type. */
  int i;
  RECDES recdes;
  PGBUF_WATCHER fwd_page_watcher;
  LOG_DATA_ADDR home_addr;
  LOG_DATA_ADDR forward_addr;
  LOG_DATA_ADDR *addr_p;
  OID save_next_version;
  OID save_partition_oid;
  OID next_version;
  OID next_partition_oid;
  char buffer[IO_MAX_PAGE_SIZE + MAX_ALIGNMENT];

  PGBUF_INIT_WATCHER (&fwd_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid);
  OID_SET_NULL (&partition_oid);

retry:
  /* Prepare to get record. It will obtain class_oid, record type, required pages, and forward_oid. */
  scan_code =
    heap_prepare_get_record (thread_p, oid, class_oid, &forward_oid, &partition_oid, home_page_watcher,
			     &fwd_page_watcher, &type, PGBUF_LATCH_WRITE, false, LOG_WARNING_IF_DELETED);
  if (scan_code != S_SUCCESS)
    {
      /* Stop here. */
      goto end;
    }

  /* forget the flags previously set */
  home_page_watcher->page_was_unfixed = false;
  fwd_page_watcher.page_was_unfixed = false;

  /* we need to check if the record is valid. Althought the caller has done that, pages might have been unfixed, we
   * need to recheck */
  if (type != REC_HOME && type != REC_BIGONE && type != REC_RELOCATION)
    {
      goto end;
    }

  /* We should have required pages fixed. */
  assert (HEAP_IS_PAGE_OF_OID (home_page_watcher->pgptr, oid));
  /* Check forward_page and forward_oid are set when type is REC_RELOCATION or REC_BIGONE. */
  assert ((type != REC_BIGONE && type != REC_RELOCATION)
	  || (!OID_ISNULL (&forward_oid) && HEAP_IS_PAGE_OF_OID (fwd_page_watcher.pgptr, &forward_oid)));

  /* Get MVCC header. */
  scan_code =
    heap_get_mvcc_header (thread_p, oid, &forward_oid, home_page_watcher->pgptr, fwd_page_watcher.pgptr, type,
			  &mvcc_header);
  if (scan_code != S_SUCCESS)
    {
      goto end;
    }
  if (OID_ISNULL (&partition_oid) && !MVCC_IS_HEADER_PARTITION_OID_VALID (&mvcc_header))
    {
      /* no partition link present */
      goto end;
    }

  /* a partition link has been found. Save the current values */
  if (OID_ISNULL (&partition_oid) && MVCC_IS_HEADER_PARTITION_OID_VALID (&mvcc_header))
    {
      COPY_OID (&partition_oid, &MVCC_GET_PARTITION_OID (&mvcc_header));
      COPY_OID (&save_next_version, &MVCC_GET_NEXT_VERSION (&mvcc_header));
      COPY_OID (&save_partition_oid, &MVCC_GET_PARTITION_OID (&mvcc_header));
    }

  /* search the partition class OID in the list of dropped partitions */
  for (i = 0; i < no_oids; i++)
    {
      if (OID_EQ (&partition_oid, &oid_list[i]))
	{
	  break;
	}
    }

  /* the partition link should not be removed */
  if (i == no_oids)
    {
      goto end;
    }

  /* The partition link is valid for removal. Save the partition link to be removed, according to record type */

  /* invalid version - search for the next valid version */
  if (heap_mvcc_find_next_valid_version (thread_p, &save_next_version, &save_partition_oid, oid_list, no_oids,
					 &next_version, &next_partition_oid) != S_SUCCESS)
    {
      scan_code = S_ERROR;
      goto error;
    }

  if (home_page_watcher->page_was_unfixed || fwd_page_watcher.page_was_unfixed)
    {
      home_page_watcher->page_was_unfixed = false;
      fwd_page_watcher.page_was_unfixed = false;
      goto retry;
    }

  /* If the next valid version is in the same class as the current OID, we need not store this as a partition link. */
  if (OID_EQ (&next_partition_oid, class_oid))
    {
      OID_SET_NULL (&next_partition_oid);
    }

  /* set logging addresses */
  LOG_SET_DATA_ADDR (&home_addr, home_page_watcher->pgptr, &(hfid->vfid), oid->slotid);
  if (fwd_page_watcher.pgptr != NULL)
    {
      LOG_SET_DATA_ADDR (&forward_addr, fwd_page_watcher.pgptr, &(hfid->vfid), forward_oid.slotid);
    }
  else
    {
      LOG_SET_DATA_ADDR (&forward_addr, NULL, NULL, 0);
    }

  if (type == REC_BIGONE || type == REC_RELOCATION)
    {
      /* use forward_addr for logging */
      addr_p = &forward_addr;
    }
  else
    {
      /* use home_addr for logging */
      addr_p = &home_addr;
    }

  /* cleanup the partition link, according to record type. The record's length will always either decrease */

  if (!OID_ISNULL (&next_version))
    {
      /* update the next version in MVCC header */
      MVCC_SET_NEXT_VERSION (&mvcc_header, &next_version);

      if (!OID_ISNULL (&next_partition_oid))
	{
	  /* update the partition link in MVCC header */
	  COPY_OID (&(mvcc_header.partition_oid), &next_partition_oid);
	}
      else
	{
	  MVCC_CLEAR_FLAG_BITS (&mvcc_header, OR_MVCC_FLAG_VALID_PARTITION_OID);
	}
    }
  else
    {
      /* no next version present */
      MVCC_CLEAR_FLAG_BITS (&mvcc_header, OR_MVCC_FLAG_VALID_NEXT_VERSION);
      MVCC_CLEAR_FLAG_BITS (&mvcc_header, OR_MVCC_FLAG_VALID_PARTITION_OID);
    }


  switch (type)
    {
    case REC_BIGONE:
      /* fill up to maximum size */
      HEAP_MVCC_SET_HEADER_MAXIMUM_SIZE (&mvcc_header);

      if (heap_set_mvcc_rec_header_on_overflow (fwd_page_watcher.pgptr, &mvcc_header) != NO_ERROR)
	{
	  scan_code = S_ERROR;
	  goto error;
	}
      heap_mvcc_log_remove_partition_link (thread_p, &save_next_version, &save_partition_oid, &next_version,
					   &next_partition_oid, 1, addr_p);
      pgbuf_set_dirty (thread_p, fwd_page_watcher.pgptr, DONT_FREE);
      break;

    case REC_HOME:
      recdes.area_size = DB_PAGESIZE;
      recdes.data = (char *) PTR_ALIGN (buffer, MAX_ALIGNMENT);
      scan_code = spage_get_record (home_page_watcher->pgptr, oid->slotid, &recdes, COPY);
      if (scan_code != S_SUCCESS)
	{
	  goto end;
	}
      recdes.area_size = recdes.length;
      or_mvcc_set_header (&recdes, &mvcc_header);
      if (spage_update (thread_p, home_page_watcher->pgptr, oid->slotid, &recdes) != SP_SUCCESS)
	{
	  scan_code = S_ERROR;
	  goto error;
	}
      heap_mvcc_log_remove_partition_link (thread_p, &save_next_version, &save_partition_oid, &next_version,
					   &next_partition_oid, 0, addr_p);
      pgbuf_set_dirty (thread_p, home_page_watcher->pgptr, DONT_FREE);
      break;

    case REC_RELOCATION:

      /* peek the forward record */
      recdes.area_size = DB_PAGESIZE;
      recdes.data = (char *) PTR_ALIGN (buffer, MAX_ALIGNMENT);
      scan_code = spage_get_record (fwd_page_watcher.pgptr, forward_oid.slotid, &recdes, COPY);
      if (scan_code != S_SUCCESS)
	{
	  goto end;
	}
      recdes.area_size = recdes.length;
      or_mvcc_set_header (&recdes, &mvcc_header);
      if (spage_update (thread_p, fwd_page_watcher.pgptr, forward_oid.slotid, &recdes) != SP_SUCCESS)
	{
	  scan_code = S_ERROR;
	  goto error;
	}
      heap_mvcc_log_remove_partition_link (thread_p, &save_next_version, &save_partition_oid, &next_version,
					   &next_partition_oid, 0, addr_p);
      pgbuf_set_dirty (thread_p, fwd_page_watcher.pgptr, DONT_FREE);
      break;

    default:
      assert (0);
      scan_code = S_ERROR;
      break;
    }

end:
  if (fwd_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &fwd_page_watcher);
    }

  return scan_code;

error:
  scan_code = S_ERROR;
  goto end;
}

/*
 * heap_remove_partition_links () - remove the invalid partition links, that
 *			    refer dropped partitions in the oid_list
 *
 * return : error code
 *
 * thread_p (in)      :
 * class_oid (in)     :
 * oid_list (in)      : partition OID list
 * no_oids (in)	      : number of OIDs in the list
 */
int
heap_remove_partition_links (THREAD_ENTRY * thread_p, OID * class_oid, OID * oid_list, int no_oids)
{
  int error = NO_ERROR;
  int i = 0, j = 0;
  RECDES recdes;
  HFID hfid, class_hfid;
  OID inst_oid;
  OR_PARTITION part_info;
  REPR_ID class_repr_id;
  int subclasses_count = 0;
  OID *subclasses = NULL;
  SCAN_CODE scan = S_SUCCESS;
  VPID vpid;
  PGBUF_WATCHER curr_page_watcher;
  PGBUF_WATCHER old_page_watcher;
  PGSLOTID slotid;
  bool is_scan_end = false;
  int has_partition_info;
  int has_partition_link = 0;

  PGBUF_INIT_WATCHER (&curr_page_watcher, PGBUF_ORDERED_RANK_UNDEFINED, PGBUF_ORDERED_NULL_HFID);
  PGBUF_INIT_WATCHER (&old_page_watcher, PGBUF_ORDERED_RANK_UNDEFINED, PGBUF_ORDERED_NULL_HFID);
  part_info.values = NULL;

  error =
    heap_class_get_partition_info (thread_p, class_oid, &part_info, &class_hfid, &class_repr_id, &has_partition_info);
  if (error != NO_ERROR)
    {
      goto exit;
    }

  if (has_partition_info == 0)
    {
      /* this class does not have partitions */
      error = NO_ERROR;
      goto exit;
    }

  /* Get OIDs for subclasses of class_oid. Some of them will be partitions */
  error = heap_get_class_subclasses (thread_p, class_oid, &subclasses_count, &subclasses);
  if (error != NO_ERROR)
    {
      goto exit;
    }
  else if (subclasses_count == 0)
    {
      /* This means that class_oid actually points to a partition and not the master class. We return NO_ERROR here
       * since there's no partition information */
      error = NO_ERROR;
      goto exit;
    }

  for (i = 0; i < subclasses_count; i++)
    {
      if (OID_ISNULL (&subclasses[i]))
	{
	  goto exit;
	}

      /* search the partition class OID in the list of dropped partitions */
      for (j = 0; j < no_oids; j++)
	{
	  if (OID_EQ (&subclasses[i], &oid_list[j]))
	    {
	      break;
	    }
	}
      if (j < no_oids)
	{
	  /* partition is in the list of dropped partitions, so it will be removed anyway. No need to scan it. */
	  continue;
	}

      /* read values of the single record in heap */
      error = heap_get_hfid_from_class_oid (thread_p, &subclasses[i], &hfid);
      if (error != NO_ERROR || HFID_IS_NULL (&hfid))
	{
	  error = ER_FAILED;
	  goto exit;
	}

      error = heap_get_partition_link_flag (thread_p, &hfid, &has_partition_link);
      if (error != NO_ERROR)
	{
	  goto exit;
	}

      if (has_partition_link == 0)
	{
	  /* skip this partition, it does not have partition links */
	  continue;
	}

      PGBUF_INIT_WATCHER (&curr_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, &hfid);
      PGBUF_INIT_WATCHER (&old_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, &hfid);

      /* start with first OID of the first page */
      vpid.volid = hfid.vfid.volid;
      vpid.pageid = hfid.hpgid;
      is_scan_end = false;
      while (!is_scan_end)
	{
	  if (curr_page_watcher.pgptr == NULL)
	    {
	      error =
		pgbuf_ordered_fix (thread_p, &vpid, OLD_PAGE_PREVENT_DEALLOC, PGBUF_LATCH_WRITE, &curr_page_watcher);
	      if (old_page_watcher.pgptr != NULL)
		{
		  pgbuf_ordered_unfix (thread_p, &old_page_watcher);
		}
	      if (error != NO_ERROR)
		{
		  if (error == ER_LK_PAGE_TIMEOUT && er_errid () == NO_ERROR)
		    {
		      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, vpid.volid, vpid.pageid);
		    }
		  goto exit;
		}
	    }

	  slotid = 0;
	  while (true)
	    {
	      /* get next record */
	      scan = spage_next_record (curr_page_watcher.pgptr, &slotid, &recdes, PEEK);
	      if (scan == S_ERROR)
		{
		  error = ER_FAILED;
		  goto exit;
		}
	      else if (scan == S_END)
		{
		  /* move to next page */
		  error = heap_vpid_next (&hfid, curr_page_watcher.pgptr, &vpid);
		  if (error != NO_ERROR)
		    {
		      goto exit;
		    }

		  /* keep latch on current page until the next page is fixed */
		  pgbuf_replace_watcher (thread_p, &curr_page_watcher, &old_page_watcher);

		  if (VPID_ISNULL (&vpid))
		    {
		      /* no more pages in the current heap file */
		      is_scan_end = true;
		    }
		  break;
		}

	      if (slotid == HEAP_HEADER_AND_CHAIN_SLOTID)
		{
		  /* skip the header */
		  continue;
		}

	      if (recdes.type != REC_HOME && recdes.type != REC_BIGONE && recdes.type != REC_RELOCATION)
		{
		  continue;
		}

	      /* find and cleanup the partition link is needed */
	      inst_oid.pageid = vpid.pageid;
	      inst_oid.volid = vpid.volid;
	      inst_oid.slotid = slotid;
	      if (heap_mvcc_cleanup_partition_link (thread_p, &hfid, &inst_oid, &subclasses[i], &curr_page_watcher,
						    oid_list, no_oids) != S_SUCCESS)
		{
		  error = ER_FAILED;
		  goto exit;
		}
	    }
	}
      if (old_page_watcher.pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, &old_page_watcher);
	}
    }

exit:
  if (curr_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &curr_page_watcher);
    }
  if (old_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &old_page_watcher);
    }
  if (subclasses != NULL)
    {
      free_and_init (subclasses);
    }
  if (has_partition_info != 0)
    {
      if (part_info.values != NULL)
	{
	  set_free (part_info.values);
	}
    }
  return error;
}

/*
 * heap_mvcc_remove_partition_link () - Log removal of partition links
 *
 * return		    : Void.
 * thread_p (in)	    : Thread entry.
 * partition_oid	    : partition link
 * p_addr (in)		    : Log address data.
 */
static void
heap_mvcc_log_remove_partition_link (THREAD_ENTRY * thread_p, OID * old_next_version, OID * old_partition_oid,
				     OID * new_next_version, OID * new_partition_oid, int is_bigone,
				     LOG_DATA_ADDR * p_addr)
{
#define HEAP_LOG_MVCC_REMOVE_PARTITION_LINK_MAX_UNDO_CRUMBS 3
#define HEAP_LOG_MVCC_REMOVE_PARTITION_LINK_MAX_REDO_CRUMBS 3

  int n_undo_crumbs = 0, n_redo_crumbs = 0;
  LOG_CRUMB undo_crumbs[HEAP_LOG_MVCC_REMOVE_PARTITION_LINK_MAX_UNDO_CRUMBS];
  LOG_CRUMB redo_crumbs[HEAP_LOG_MVCC_REMOVE_PARTITION_LINK_MAX_REDO_CRUMBS];

  /* Build undo crumbs */
  undo_crumbs[n_undo_crumbs].length = OR_INT_SIZE;
  undo_crumbs[n_undo_crumbs++].data = &is_bigone;

  undo_crumbs[n_undo_crumbs].length = OR_OID_SIZE;
  undo_crumbs[n_undo_crumbs++].data = old_next_version;

  undo_crumbs[n_undo_crumbs].length = OR_OID_SIZE;
  undo_crumbs[n_undo_crumbs++].data = old_partition_oid;

  /* Safe guard */
  assert (n_undo_crumbs == HEAP_LOG_MVCC_REMOVE_PARTITION_LINK_MAX_UNDO_CRUMBS);

  /* Build redo crumbs */
  redo_crumbs[n_redo_crumbs].length = OR_INT_SIZE;
  redo_crumbs[n_redo_crumbs++].data = &is_bigone;

  if (new_next_version != NULL && !OID_ISNULL (new_next_version))
    {
      redo_crumbs[n_redo_crumbs].length = OR_OID_SIZE;
      redo_crumbs[n_redo_crumbs++].data = new_next_version;

      if (new_partition_oid != NULL && !OID_ISNULL (new_partition_oid))
	{
	  redo_crumbs[n_redo_crumbs].length = OR_OID_SIZE;
	  redo_crumbs[n_redo_crumbs++].data = new_partition_oid;
	}
    }

  /* Safe guard */
  assert (n_redo_crumbs <= HEAP_LOG_MVCC_REMOVE_PARTITION_LINK_MAX_REDO_CRUMBS);

  /* Log append undo/redo crumbs */
  log_append_undoredo_crumbs (thread_p, RVHF_MVCC_REMOVE_PARTITION_LINK, p_addr, n_undo_crumbs, n_redo_crumbs,
			      undo_crumbs, redo_crumbs);
}

/*
 * heap_rv_mvcc_undo_remove_partition_link () - Undo the cleanup of a
 *					partition link.
 *   return: int
 *   rcv(in): Recovery structure
 */
int
heap_rv_mvcc_undo_remove_partition_link (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  RECDES peek_recdes;
  INT16 slotid;
  bool is_bigone = false;
  char *data = NULL;
  MVCC_REC_HEADER mvcc_rec_header;
  int old_mvcc_header_size;
  int offset = 0;
  char data_buffer[IO_MAX_PAGE_SIZE + MAX_ALIGNMENT];
  RECDES recdes;
  int sp_success;
  OID next_version;
  OID partition_oid;
  OID rec_buffer[2];

  assert (rcv->length == 2 * OR_OID_SIZE + OR_INT_SIZE);

  /* Read bigone */
  is_bigone = (bool) OR_GET_INT (rcv->data + offset);
  offset += OR_INT_SIZE;

  /* Read next version */
  COPY_OID (&next_version, (OID *) (rcv->data + offset));
  offset += OR_OID_SIZE;

  assert (!OID_ISNULL (&next_version));

  /* Read partition OID */
  COPY_OID (&partition_oid, (OID *) (rcv->data + offset));
  offset += OR_OID_SIZE;

  assert (!OID_ISNULL (&partition_oid));
  assert (offset == rcv->length);

  if (is_bigone == false)
    {
      assert (rcv->offset >= 0);
      slotid = rcv->offset;
      if (spage_get_record (rcv->pgptr, slotid, &peek_recdes, PEEK) != S_SUCCESS)
	{
	  return ER_FAILED;
	}
      if (peek_recdes.type != REC_MVCC_NEXT_VERSION)
	{
	  if (or_mvcc_get_header (&peek_recdes, &mvcc_rec_header) != NO_ERROR)
	    {
	      return ER_FAILED;
	    }
	}
    }
  else
    {
      if (heap_get_mvcc_rec_header_from_overflow (rcv->pgptr, &mvcc_rec_header, &peek_recdes) != NO_ERROR)
	{
	  return ER_FAILED;
	}
    }

  if (peek_recdes.type == REC_MVCC_NEXT_VERSION)
    {
      assert (peek_recdes.length == 2 * OR_OID_SIZE);

      COPY_OID (&rec_buffer[0], &next_version);
      COPY_OID (&rec_buffer[1], &partition_oid);

      recdes.type = peek_recdes.type;
      recdes.area_size = 2 * OR_OID_SIZE;
      recdes.length = 2 * OR_OID_SIZE;
      recdes.data = (char *) rec_buffer;

      sp_success = spage_update (thread_p, rcv->pgptr, slotid, &recdes);
      if (sp_success != SP_SUCCESS)
	{
	  /* Unable to recover update for object */
	  if (sp_success != SP_ERROR)
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	    }
	  return er_errid ();
	}

      pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
      return NO_ERROR;
    }

  assert (peek_recdes.type != REC_MVCC_NEXT_VERSION);

  old_mvcc_header_size = or_mvcc_header_size_from_flags (mvcc_rec_header.mvcc_flag);

  MVCC_SET_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_NEXT_VERSION);
  MVCC_SET_NEXT_VERSION (&mvcc_rec_header, &next_version);

  MVCC_SET_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_PARTITION_OID);
  COPY_OID (&(mvcc_rec_header.partition_oid), &partition_oid);

  if (is_bigone)
    {
      if (heap_set_mvcc_rec_header_on_overflow (rcv->pgptr, &mvcc_rec_header) != NO_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  return ER_FAILED;
	}
    }
  else
    {
      HEAP_SET_RECORD (&recdes, DB_PAGESIZE, 0, peek_recdes.type, PTR_ALIGN (data_buffer, MAX_ALIGNMENT));

      if (or_mvcc_add_header (&recdes, &mvcc_rec_header, OR_GET_BOUND_BIT_FLAG (peek_recdes.data),
			      OR_GET_OFFSET_SIZE (peek_recdes.data)) != NO_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  return ER_FAILED;
	}
      memcpy (recdes.data + recdes.length, peek_recdes.data + old_mvcc_header_size,
	      peek_recdes.length - old_mvcc_header_size);
      recdes.length += (peek_recdes.length - old_mvcc_header_size);

      sp_success = spage_update (thread_p, rcv->pgptr, slotid, &recdes);
      if (sp_success != SP_SUCCESS)
	{
	  /* Unable to recover update for object */
	  if (sp_success != SP_ERROR)
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	    }
	  return er_errid ();
	}
    }

  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
  return NO_ERROR;
}

/*
 * heap_rv_mvcc_redo_remove_partition_link () - Redo the cleanup of a
 *					partition link.
 *   return: int
 *   rcv(in): Recovery structure
 */
int
heap_rv_mvcc_redo_remove_partition_link (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  RECDES peek_recdes;
  INT16 slotid;
  bool is_bigone = false;
  char *data = NULL;
  MVCC_REC_HEADER mvcc_rec_header;
  int old_mvcc_header_size;
  int offset = 0;
  char data_buffer[IO_MAX_PAGE_SIZE + MAX_ALIGNMENT];
  RECDES recdes;
  int sp_success;
  OID next_version;
  OID partition_oid;
  OID rec_buffer[2];

  /* Read bigone */
  is_bigone = (bool) OR_GET_INT (rcv->data + offset);
  offset += OR_INT_SIZE;

  if (offset < rcv->length)
    {
      /* Read next version */
      COPY_OID (&next_version, (OID *) rcv->data + offset);
      offset += OR_OID_SIZE;

      if (offset < rcv->length)
	{
	  /* Read partition OID */
	  COPY_OID (&partition_oid, (OID *) rcv->data + offset);
	  offset += OR_OID_SIZE;
	}
      else
	{
	  OID_SET_NULL (&partition_oid);
	}
    }
  else
    {
      OID_SET_NULL (&next_version);
      OID_SET_NULL (&partition_oid);
    }

  assert (offset == rcv->length);

  if (is_bigone == false)
    {
      assert (rcv->offset >= 0);
      slotid = rcv->offset;
      if (spage_get_record (rcv->pgptr, slotid, &peek_recdes, PEEK) != S_SUCCESS)
	{
	  return ER_FAILED;
	}

      if (peek_recdes.type != REC_MVCC_NEXT_VERSION)
	{
	  if (or_mvcc_get_header (&peek_recdes, &mvcc_rec_header) != NO_ERROR)
	    {
	      return ER_FAILED;
	    }
	}
    }
  else
    {
      if (heap_get_mvcc_rec_header_from_overflow (rcv->pgptr, &mvcc_rec_header, &peek_recdes) != NO_ERROR)
	{
	  return ER_FAILED;
	}
    }

  if (peek_recdes.type == REC_MVCC_NEXT_VERSION)
    {
      assert (peek_recdes.length == 2 * OR_OID_SIZE);

      COPY_OID (&rec_buffer[0], &next_version);
      COPY_OID (&rec_buffer[1], &partition_oid);

      recdes.type = peek_recdes.type;
      recdes.area_size = 2 * OR_OID_SIZE;
      recdes.length = 2 * OR_OID_SIZE;
      recdes.data = (char *) rec_buffer;

      sp_success = spage_update (thread_p, rcv->pgptr, slotid, &recdes);
      if (sp_success != SP_SUCCESS)
	{
	  /* Unable to recover update for object */
	  if (sp_success != SP_ERROR)
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	    }
	  return er_errid ();
	}

      pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
      return NO_ERROR;
    }

  assert (peek_recdes.type != REC_MVCC_NEXT_VERSION);

  old_mvcc_header_size = or_mvcc_header_size_from_flags (mvcc_rec_header.mvcc_flag);

  if (OID_ISNULL (&next_version) == false)
    {
      MVCC_SET_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_NEXT_VERSION);
      MVCC_SET_NEXT_VERSION (&mvcc_rec_header, &next_version);
      if (OID_ISNULL (&partition_oid) == false)
	{
	  MVCC_SET_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_PARTITION_OID);
	  COPY_OID (&mvcc_rec_header.partition_oid, &partition_oid);
	}
      else
	{
	  MVCC_CLEAR_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_PARTITION_OID);
	}
    }
  else
    {
      MVCC_CLEAR_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_NEXT_VERSION);
      MVCC_CLEAR_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_PARTITION_OID);
    }

  if (is_bigone)
    {
      /* fill up to maximum header size */
      HEAP_MVCC_SET_HEADER_MAXIMUM_SIZE (&mvcc_rec_header);

      if (heap_set_mvcc_rec_header_on_overflow (rcv->pgptr, &mvcc_rec_header) != NO_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  return ER_FAILED;
	}
    }
  else
    {
      HEAP_SET_RECORD (&recdes, DB_PAGESIZE, 0, peek_recdes.type, PTR_ALIGN (data_buffer, MAX_ALIGNMENT));

      if (or_mvcc_add_header (&recdes, &mvcc_rec_header, OR_GET_BOUND_BIT_FLAG (peek_recdes.data),
			      OR_GET_OFFSET_SIZE (peek_recdes.data)) != NO_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	  return ER_FAILED;
	}
      memcpy (recdes.data + recdes.length, peek_recdes.data + old_mvcc_header_size,
	      peek_recdes.length - old_mvcc_header_size);
      recdes.length += (peek_recdes.length - old_mvcc_header_size);

      sp_success = spage_update (thread_p, rcv->pgptr, slotid, &recdes);
      if (sp_success != SP_SUCCESS)
	{
	  /* Unable to recover update for object */
	  if (sp_success != SP_ERROR)
	    {
	      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	    }
	  return er_errid ();
	}
    }

  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
  return NO_ERROR;
}

/*
 * heap_link_watchers () - link page watchers of a child operation to it's
 *			   parent
 *   child(in): child operation context
 *   parent(in): parent operation context
 *
 * NOTE: Sometimes, parts of a heap operation are executed in a parent heap
 *       operation, skipping the fixing of pages and location of records.
 *       Since page watchers are identified by address, we must use a single
 *       location for them, and reference it everywhere.
 */
static void
heap_link_watchers (HEAP_OPERATION_CONTEXT * child, HEAP_OPERATION_CONTEXT * parent)
{
  assert (child != NULL);
  assert (parent != NULL);

  child->header_page_watcher_p = &parent->header_page_watcher;
  child->forward_page_watcher_p = &parent->forward_page_watcher;
  child->overflow_page_watcher_p = &parent->overflow_page_watcher;
  child->home_page_watcher_p = &parent->home_page_watcher;
}

/*
 * heap_unfix_watchers () - unfix context pages
 *   thread_p(in): thread entry
 *   context(in): operation context
 *
 * NOTE: This function only unfixes physical watchers. Calling this in a child
 *       operation that was linked to the parent with heap_link_watchers will
 *       have no effect on the fixed pages.
 */
static void
heap_unfix_watchers (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context)
{
  assert (context != NULL);

  /* unfix pages */
  if (context->home_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &context->home_page_watcher);
    }
  if (context->overflow_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &context->overflow_page_watcher);
    }
  if (context->header_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &context->header_page_watcher);
    }
  if (context->forward_page_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &context->forward_page_watcher);
    }
}

/*
 * heap_clear_operation_context () - clear a heap operation context
 *   context(in): the context
 *   hfid_p(in): heap file identifier
 */
static void
heap_clear_operation_context (HEAP_OPERATION_CONTEXT * context, HFID * hfid_p)
{
  assert (context != NULL);
  assert (hfid_p != NULL);

  /* keep hfid */
  HFID_COPY (&context->hfid, hfid_p);

  /* initialize watchers to HFID */
  PGBUF_INIT_WATCHER (&context->home_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid_p);
  PGBUF_INIT_WATCHER (&context->forward_page_watcher, PGBUF_ORDERED_HEAP_NORMAL, hfid_p);
  PGBUF_INIT_WATCHER (&context->overflow_page_watcher, PGBUF_ORDERED_HEAP_OVERFLOW, hfid_p);
  PGBUF_INIT_WATCHER (&context->header_page_watcher, PGBUF_ORDERED_HEAP_HDR, hfid_p);

  /* by default link physical watchers to usage watchers on same context */
  heap_link_watchers (context, context);

  /* nullify everything else */
  context->type = HEAP_OPERATION_NONE;
  context->update_in_place = UPDATE_INPLACE_NONE;
  OID_SET_NULL (&context->oid);
  OID_SET_NULL (&context->next_version);
  OID_SET_NULL (&context->partition_link);
  OID_SET_NULL (&context->class_oid);
  context->recdes_p = NULL;
  context->scan_cache_p = NULL;
  context->flags = 0;

  context->map_recdes.data = NULL;
  context->map_recdes.length = 0;
  context->map_recdes.area_size = 0;
  context->map_recdes.type = REC_UNKNOWN;

  OID_SET_NULL (&context->ovf_oid[0]);
  OID_SET_NULL (&context->ovf_oid[1]);

  context->home_recdes.data = NULL;
  context->home_recdes.length = 0;
  context->home_recdes.area_size = 0;
  context->home_recdes.type = REC_UNKNOWN;

  context->record_type = REC_UNKNOWN;
  context->file_type = -1;
  OID_SET_NULL (&context->res_oid);
  context->is_logical_old = false;
  context->is_redistribute_insert_with_delid = false;

  context->time_track = NULL;
}

/*
 * heap_mark_class_as_modified () - add to transaction's modified class list
 *                                  and cache/decache coherency number
 *   thread_p(in): thread entry
 *   oid_p(in): class OID
 *   chn(in): coherency number (required iff decache == false)
 *   decache(in): (false => cache, true => decache)
 */
static int
heap_mark_class_as_modified (THREAD_ENTRY * thread_p, OID * oid_p, int chn, bool decache)
{
  char *classname = NULL;

  assert (oid_p != NULL);

  if (heap_Guesschn == NULL || HFID_IS_NULL (&(heap_Classrepr->rootclass_hfid)))
    {
      /* nothing to do */
      return NO_ERROR;
    }

  classname = heap_get_class_name (thread_p, oid_p);
  if (classname == NULL)
    {
      return ER_FAILED;
    }
  if (log_add_to_modified_class_list (thread_p, classname, oid_p) != NO_ERROR)
    {
      free_and_init (classname);
      return ER_FAILED;
    }

  free_and_init (classname);

  if (csect_enter (thread_p, CSECT_HEAP_CHNGUESS, INF_WAIT) != NO_ERROR)
    {
      return ER_FAILED;
    }
  heap_Guesschn->schema_change = true;

  if (decache)
    {
      (void) heap_chnguess_decache (oid_p);
    }
  else
    {
      (void) heap_chnguess_put (thread_p, oid_p, LOG_FIND_THREAD_TRAN_INDEX (thread_p), chn);
    }

  csect_exit (thread_p, CSECT_HEAP_CHNGUESS);

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_get_file_type () - get the file type from a heap operation context
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   returns: file type
 */
static FILE_TYPE
heap_get_file_type (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context)
{
  if (context->scan_cache_p != NULL)
    {
      assert (HFID_EQ (&context->hfid, &context->scan_cache_p->node.hfid));
      assert (context->scan_cache_p->file_type != FILE_UNKNOWN_TYPE);

      return context->scan_cache_p->file_type;
    }
  else
    {
      return file_get_type (thread_p, &context->hfid.vfid);
    }
}

/*
 * heap_is_valid_oid () - check if provided OID is valid
 *   oid_p(in): object identifier
 *   returns: error code or NO_ERROR
 */
static int
heap_is_valid_oid (OID * oid_p)
{
  DISK_ISVALID oid_valid = HEAP_ISVALID_OID (oid_p);
  if (oid_valid != DISK_VALID)
    {
      if (oid_valid != DISK_ERROR)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, oid_p->volid, oid_p->pageid,
		  oid_p->slotid);
	}
      return ER_FAILED;
    }
  else
    {
      return NO_ERROR;
    }
}

/*
 * heap_fix_header_page () - fix header page for a heap operation context
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   returns: error code or NO_ERROR
 */
static int
heap_fix_header_page (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context)
{
  VPID header_vpid;
  int rc;

  assert (context != NULL);
  assert (context->header_page_watcher_p != NULL);

  if (context->header_page_watcher_p->pgptr != NULL)
    {
      /* already fixed */
      return NO_ERROR;
    }

  /* fix header page */
  header_vpid.volid = context->hfid.vfid.volid;
  header_vpid.pageid = context->hfid.hpgid;

  /* fix page */
  rc = pgbuf_ordered_fix (thread_p, &header_vpid, OLD_PAGE, PGBUF_LATCH_WRITE, context->header_page_watcher_p);
  if (rc != NO_ERROR)
    {
      if (rc == ER_LK_PAGE_TIMEOUT && er_errid () == NO_ERROR)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, header_vpid.volid, header_vpid.pageid);
	  rc = ER_PAGE_LATCH_ABORTED;
	}
      return rc;
    }

  /* check page type */
  (void) pgbuf_check_page_ptype (thread_p, context->header_page_watcher_p->pgptr, PAGE_HEAP);

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_fix_forward_page () - fix forward page for a heap operation context
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   forward_oid_hint(in): location of forward object (if known)
 *   returns: error code or NO_ERROR
 *
 * NOTE: If forward_oid_hint is provided, this function will fix it's page. If
 *       not, the function will treat the context's home_recdes as a forwarding
 *       record descriptor and read the identifier from it.
 */
static int
heap_fix_forward_page (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, OID * forward_oid_hint)
{
  VPID forward_vpid;
  OID forward_oid;
  int rc;

  assert (context != NULL);
  assert (context->forward_page_watcher_p != NULL);

  if (context->forward_page_watcher_p->pgptr != NULL)
    {
      /* already fixed */
      return NO_ERROR;
    }

  if (forward_oid_hint == NULL)
    {
      assert (context->home_recdes.data != NULL);

      /* cast home record as forward oid if no hint is provided */
      forward_oid = *((OID *) context->home_recdes.data);
    }
  else
    {
      /* oid is provided, use it */
      COPY_OID (&forward_oid, forward_oid_hint);
    }

  /* prepare VPID */
  forward_vpid.pageid = forward_oid.pageid;
  forward_vpid.volid = forward_oid.volid;

  /* fix forward page */
  PGBUF_WATCHER_COPY_GROUP (context->forward_page_watcher_p, context->home_page_watcher_p);
  rc = pgbuf_ordered_fix (thread_p, &forward_vpid, OLD_PAGE, PGBUF_LATCH_WRITE, context->forward_page_watcher_p);
  if (rc != NO_ERROR)
    {
      if (rc == ER_LK_PAGE_TIMEOUT && er_errid () == NO_ERROR)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, forward_vpid.volid, forward_vpid.pageid);
	}
      return ER_FAILED;
    }
  (void) pgbuf_check_page_ptype (thread_p, context->forward_page_watcher_p->pgptr, PAGE_HEAP);

#if defined(CUBRID_DEBUG)
  if (spage_get_record_type (context->forward_page_watcher_p->pgptr, forward_oid.slotid) != REC_NEWHOME)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_BAD_OBJECT_TYPE, 3, forward_oid.volid, forward_oid.pageid,
	      forward_oid.slotid);
      return ER_FAILED;
    }
#endif

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_build_forwarding_recdes () - build a record descriptor for pointing to
 *                                   a forward object
 *   recdes_p(in): record descriptor to build into
 *   rec_type(in): type of record
 *   oid_buffer(in): OID buffer to use (should be able to hold 2 OIDs)
 *   reserve_extra_oid(in): if true, will reserve space for 2 OIDs
 */
static void
heap_build_forwarding_recdes (RECDES * recdes_p, INT16 rec_type, OID * oid_buffer_p, bool reserve_extra_oid)
{
  assert (recdes_p != NULL);
  assert (oid_buffer_p != NULL);

  recdes_p->type = rec_type;
  recdes_p->data = (char *) oid_buffer_p;

  if (reserve_extra_oid)
    {
      recdes_p->length = 2 * sizeof (OID);
      recdes_p->area_size = 2 * sizeof (OID);
      OID_SET_NULL (&oid_buffer_p[1]);
    }
  else
    {
      recdes_p->length = sizeof (OID);
      recdes_p->area_size = sizeof (OID);
    }
}

/*
 * heap_insert_adjust_recdes_header () - adjust record header for insert
 *                                       operation
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   recdes_p(in): record descriptor to adjust
 *   is_mvcc_op(in): specifies type of operation (MVCC/non-MVCC)
 *   returns: error code or NO_ERROR
 *
 * NOTE: For MVCC operation, it will add an insert_id to the header. For
 *       non-MVCC operations, it will clear all flags.
 * NOTE: The function will alter the provided record descriptor data area.
 */
static int
heap_insert_adjust_recdes_header (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, RECDES * recdes_p,
				  bool is_mvcc_op, bool is_mvcc_class)
{
  MVCC_REC_HEADER mvcc_rec_header;
  MVCCID mvcc_id;
  int record_size = recdes_p->length;
  bool insert_from_reorganize = false;

  /* read MVCC header from record */
  if (or_mvcc_get_header (recdes_p, &mvcc_rec_header) != NO_ERROR)
    {
      return ER_FAILED;
    }

  insert_from_reorganize = (context->type == HEAP_OPERATION_INSERT
			    && context->update_in_place == UPDATE_INPLACE_OLD_MVCCID);

  if (insert_from_reorganize == true && MVCC_IS_FLAG_SET (&mvcc_rec_header, OR_MVCC_FLAG_VALID_DELID))
    {
      context->is_redistribute_insert_with_delid = true;
    }

  if ((context->type != HEAP_OPERATION_UPDATE || context->update_in_place != UPDATE_INPLACE_OLD_MVCCID)
      && insert_from_reorganize == false)
    {
      if (is_mvcc_op)
	{
	  assert (is_mvcc_class);

	  /* get MVCC id */
	  mvcc_id = logtb_get_current_mvccid (thread_p);

	  /* set MVCC insertid if necessary */
	  if (!MVCC_IS_FLAG_SET (&mvcc_rec_header, OR_MVCC_FLAG_VALID_INSID))
	    {
	      MVCC_SET_FLAG (&mvcc_rec_header, OR_MVCC_FLAG_VALID_INSID);
	      record_size += OR_MVCCID_SIZE;
	    }
	  MVCC_SET_INSID (&mvcc_rec_header, mvcc_id);
	}
      else
	{
	  int curr_header_size, new_header_size;

	  /* strip MVCC information */
	  curr_header_size = or_mvcc_header_size_from_flags (mvcc_rec_header.mvcc_flag);
	  MVCC_CLEAR_ALL_FLAG_BITS (&mvcc_rec_header);
	  new_header_size = or_mvcc_header_size_from_flags (mvcc_rec_header.mvcc_flag);

	  /* compute new record size */
	  record_size -= (curr_header_size - new_header_size);
	}
    }

  if (is_mvcc_op && insert_from_reorganize && !heap_is_big_length (record_size)
      && MVCC_IS_FLAG_SET (&mvcc_rec_header, OR_MVCC_FLAG_VALID_NEXT_VERSION))
    {
      MVCC_CLEAR_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_NEXT_VERSION);
      MVCC_CLEAR_FLAG_BITS (&mvcc_rec_header, OR_MVCC_FLAG_VALID_PARTITION_OID);
    }

  if (is_mvcc_class && heap_is_big_length (record_size))
    {
      /* for multipage records, set MVCC header size to maximum size */
      HEAP_MVCC_SET_HEADER_MAXIMUM_SIZE (&mvcc_rec_header);
    }

  /* write the header back to the record */
  if (or_mvcc_set_header (recdes_p, &mvcc_rec_header) != NO_ERROR)
    {
      return ER_FAILED;
    }

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_insert_handle_multipage_record () - handle a multipage object for insert
 *   thread_p(in): thread entry
 *   context(in): operation context
 *
 * NOTE: In case of multipage records, this function will perform the overflow
 *       insertion and provide a forwarding record descriptor in map_recdes.
 *       recdes_p will point to the map_recdes structure for insertion in home
 *       page.
 */
static int
heap_insert_handle_multipage_record (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context)
{
  bool use_bigone_maxsize = false;

  assert (context != NULL);
  assert (context->type == HEAP_OPERATION_INSERT);
  assert (context->recdes_p != NULL);

  /* check for big record */
  if (!heap_is_big_length (context->recdes_p->length))
    {
      return NO_ERROR;
    }

  /* insert overflow record */
  if (heap_ovf_insert (thread_p, &context->hfid, &context->ovf_oid[0], context->recdes_p, NULL) == NULL)
    {
      return ER_FAILED;
    }

  use_bigone_maxsize = HEAP_OP_CONTEXT_IS_FLAG_SET (context->flags, HEAP_OP_CONTEXT_FLAG_BIGONE_MAXSIZE);

  /* Add a map record to point to the record in overflow */
  /* NOTE: MVCC information is held in overflow record */
  heap_build_forwarding_recdes (&context->map_recdes, REC_BIGONE, context->ovf_oid, use_bigone_maxsize);

  /* use map_recdes for page insertion */
  context->recdes_p = &context->map_recdes;

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_get_insert_location_with_lock () - get a page (and possibly and slot)
 *				    for insert and lock the OID
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   home_hint_p(in): if not null, will try to find and lock a slot in hinted page
 *   returns: error code or NO_ERROR
 *
 * NOTE: For all operations, this function will find a suitable page, put it
 *       in context->home_page_watcher, find a suitable slot, lock it and
 *       put the exact insert location in context->res_oid.
 * NOTE: If a home hint is present, the function will search for a free and
 *       lockable slot ONLY in the hinted page. If no hint is present, it will
 *       find the page on it's own.
 */
static int
heap_get_insert_location_with_lock (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context,
				    PGBUF_WATCHER * home_hint_p)
{
  int slot_count, slot_id, lk_result;
  LOCK lock;
  int error_code = NO_ERROR;

  /* check input */
  assert (context != NULL);
  assert (context->type == HEAP_OPERATION_INSERT);
  assert (context->recdes_p != NULL);

  if (home_hint_p == NULL)
    {
      /* find and fix page for insert */
      if (heap_stats_find_best_page (thread_p, &context->hfid, context->recdes_p->length,
				     (context->recdes_p->type != REC_NEWHOME), context->recdes_p->length,
				     context->scan_cache_p, context->home_page_watcher_p) == NULL)
	{
	  ASSERT_ERROR_AND_SET (error_code);
	  return error_code;
	}
    }
  else
    {
      assert (home_hint_p->pgptr != NULL);

      /* check page for space and use hinted page as insert page */
      if (spage_max_space_for_new_record (thread_p, home_hint_p->pgptr) < context->recdes_p->length)
	{
	  return ER_SP_NOSPACE_IN_PAGE;
	}
      context->home_page_watcher_p = home_hint_p;
    }
  assert (context->home_page_watcher_p->pgptr != NULL);

  /* partially populate output OID */
  context->res_oid.volid = pgbuf_get_volume_id (context->home_page_watcher_p->pgptr);
  context->res_oid.pageid = pgbuf_get_page_id (context->home_page_watcher_p->pgptr);

  /* 
   * Find a slot that is lockable and lock it
   */
  /* determine lock type */
  if (OID_IS_ROOTOID (&context->class_oid))
    {
      /* class creation */
      lock = SCH_M_LOCK;
    }
  else
    {
      /* instance */
      lock = X_LOCK;
    }

  /* retrieve number of slots in page */
  slot_count = spage_number_of_slots (context->home_page_watcher_p->pgptr);

  /* find REC_DELETED_WILL_REUSE slot or add new slot */
  /* slot_id == slot_count means add new slot */
  for (slot_id = 0; slot_id <= slot_count; slot_id++)
    {
      slot_id = spage_find_free_slot (context->home_page_watcher_p->pgptr, NULL, slot_id);
      context->res_oid.slotid = slot_id;

      /* lock the object to be inserted conditionally */
      lk_result = lock_object (thread_p, &context->res_oid, &context->class_oid, lock, LK_COND_LOCK);
      if (lk_result == LK_GRANTED)
	{
	  /* successfully locked! */
	  return NO_ERROR;
	}
      else if (lk_result != LK_NOTGRANTED_DUE_TIMEOUT)
	{
#if !defined(NDEBUG)
	  if (lk_result == LK_NOTGRANTED_DUE_ABORTED)
	    {
	      LOG_TDES *tdes = LOG_FIND_CURRENT_TDES (thread_p);
	      assert (tdes->tran_abort_reason == TRAN_ABORT_DUE_ROLLBACK_ON_ESCALATION);
	    }
	  else
	    {
	      assert (false);	/* unknown locking error */
	    }
#endif
	  break;		/* go to error case */
	}
    }

  /* either lock error or no slot was found in page (which should not happen) */
  OID_SET_NULL (&context->res_oid);
  if (context->home_page_watcher_p != home_hint_p)
    {
      pgbuf_ordered_unfix (thread_p, context->home_page_watcher_p);
    }
  else
    {
      context->home_page_watcher_p = NULL;
    }
  assert (false);
  return ER_FAILED;
}


/*
 * heap_find_location_and_insert_rec_newhome  () - find location in a heap page
 *				    and then insert context->record
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   returns: error code or NO_ERROR
 *
 * NOTE: This function will find a suitable page, put it in
 *	  context->home_page_watcher, insert context->recdes_p into that page
 *	  and put recdes location into context->res_oid.
 *	 Currently, this function is called only for REC_NEWHOME records, when
 *	  lock acquisition is not required.
 *	 The caller must log the inserted data.
 */
static int
heap_find_location_and_insert_rec_newhome (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context)
{
  SCAN_CODE sp_success;
  int error_code = NO_ERROR;

  /* check input */
  assert (context != NULL);
  assert (context->type == HEAP_OPERATION_INSERT);
  assert (context->recdes_p != NULL);
  assert (context->recdes_p->type == REC_NEWHOME);

#if defined(CUBRID_DEBUG)
  if (heap_is_big_length (context->recdes_p->length))
    {
      er_log_debug (ARG_FILE_LINE,
		    "heap_insert_internal: This function does not accept"
		    " objects longer than %d. An object of %d was given\n", heap_Maxslotted_reclength, recdes->length);
      return ER_FAILED;
    }
#endif

  if (heap_stats_find_best_page (thread_p, &context->hfid, context->recdes_p->length, false, context->recdes_p->length,
				 context->scan_cache_p, context->home_page_watcher_p) == NULL)
    {
      ASSERT_ERROR_AND_SET (error_code);
      return error_code;
    }

#if !defined(NDEBUG)
  if (context->scan_cache_p != NULL)
    {
      OID heap_class_oid;

      assert (heap_get_class_oid_from_page (thread_p, context->home_page_watcher_p->pgptr, &heap_class_oid) ==
	      NO_ERROR);

      assert (OID_EQ (&heap_class_oid, &context->scan_cache_p->node.class_oid));
    }
#endif

  assert (context->home_page_watcher_p->pgptr != NULL);
  (void) pgbuf_check_page_ptype (thread_p, context->home_page_watcher_p->pgptr, PAGE_HEAP);

  sp_success =
    spage_insert (thread_p, context->home_page_watcher_p->pgptr, context->recdes_p, &context->res_oid.slotid);
  if (sp_success == SP_SUCCESS)
    {
      context->res_oid.volid = pgbuf_get_volume_id (context->home_page_watcher_p->pgptr);
      context->res_oid.pageid = pgbuf_get_page_id (context->home_page_watcher_p->pgptr);

      return NO_ERROR;
    }
  else
    {
      assert (false);
      if (sp_success != SP_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	}
      OID_SET_NULL (&context->res_oid);
      pgbuf_ordered_unfix (thread_p, context->home_page_watcher_p);
      return ER_FAILED;
    }
}

/*
 * heap_insert_newhome () - will find an insert location for a REC_NEWHOME
 *                          record and will insert it there
 *   thread_p(in): thread entry
 *   parent_context(in): the context of the parent operation
 *   recdes_p(in): record descriptor of newhome record
 *   out_oid_p(in): pointer to an OID object to be populated with the result
 *                  OID of the insert
 *   returns: error code or NO_ERROR
 *
 * NOTE: This function works ONLY in an MVCC operation. It will create a new
 *       context for the insert operation.
 */
static int
heap_insert_newhome (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * parent_context, RECDES * recdes_p,
		     OID * out_oid_p)
{
  HEAP_OPERATION_CONTEXT ins_context;
  int error_code = NO_ERROR;

  /* check input */
  assert (recdes_p != NULL);
  assert (parent_context != NULL);
  assert (parent_context->type == HEAP_OPERATION_DELETE || parent_context->type == HEAP_OPERATION_UPDATE);

  /* build insert context */
  heap_create_insert_context (&ins_context, &parent_context->hfid, &parent_context->class_oid, recdes_p, NULL, false);

  /* physical insertion */
  error_code = heap_find_location_and_insert_rec_newhome (thread_p, &ins_context);
  if (error_code != NO_ERROR)
    {
      ASSERT_ERROR ();
      return error_code;
    }

  HEAP_PERF_TRACK_EXECUTE (thread_p, parent_context);

  /* log operation */

  /* This is a relocation of existing record, be it deleted or updated. Vacuum is not supposed to be notified since he
   * never check REC_NEWHOME type records. An MVCC type logging is not required here, a simple RVHF_INSERT will do. */
  heap_log_insert_physical (thread_p, ins_context.home_page_watcher_p->pgptr, &ins_context.hfid.vfid,
			    &ins_context.res_oid, ins_context.recdes_p, false, false);

  HEAP_PERF_TRACK_LOGGING (thread_p, parent_context);

  /* advertise insert location */
  if (out_oid_p != NULL)
    {
      COPY_OID (out_oid_p, &ins_context.res_oid);
    }

  /* mark insert page as dirty and unfix it */
  pgbuf_set_dirty (thread_p, ins_context.home_page_watcher_p->pgptr, DONT_FREE);

  /* unfix all pages of insert context */
  heap_unfix_watchers (thread_p, &ins_context);

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_insert_newver () - insert a new version of an object
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   home_hint(in): if provided, will try to insert in hinted page, and will
 *                  fall back to another page if not possible
 *   do_logging(in): if true, will insert logging information
 *
 * NOTE: This operation only makes sense in MVCC
 */
static int
heap_insert_newver (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, PGBUF_WATCHER * home_hint)
{
  int rc = NO_ERROR;

  assert (context != NULL);
  assert (context->type == HEAP_OPERATION_INSERT);
  assert (context->recdes_p != NULL);

  /* fix header page if REC_BIGONE */
  if (heap_is_big_length (context->recdes_p->length))
    {
      rc = heap_fix_header_page (thread_p, context);
      if (rc != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  return rc;
	}
    }

  /* handle overflow case of new record version */
  rc = heap_insert_handle_multipage_record (thread_p, context);
  if (rc != NO_ERROR)
    {
      ASSERT_ERROR ();
      return rc;
    }

  /* if we get a page hint, try to get the insert location in it */
  if (home_hint != NULL)
    {
      rc = heap_get_insert_location_with_lock (thread_p, context, home_hint);
      if (rc != NO_ERROR && rc != ER_SP_NOSPACE_IN_PAGE)
	{
	  ASSERT_ERROR ();
	  return rc;
	}
    }

  /* if we can't insert in hinted page, get a fresh location */
  if (context->home_page_watcher_p->pgptr == NULL)
    {
      assert (home_hint == NULL || rc == ER_SP_NOSPACE_IN_PAGE);

      /* fix header page */
      rc = heap_fix_header_page (thread_p, context);
      if (rc != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  return rc;
	}

      /* get new insert location */
      rc = heap_get_insert_location_with_lock (thread_p, context, NULL);
      if (rc != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  return rc;
	}
    }

  /* physically insert new version's home */
  rc = heap_insert_physical (thread_p, context);
  if (rc != NO_ERROR)
    {
      ASSERT_ERROR ();
      return rc;
    }

  HEAP_PERF_TRACK_EXECUTE (thread_p, context);

  /* log operation */
  heap_log_insert_physical (thread_p, context->home_page_watcher_p->pgptr, &context->hfid.vfid, &context->res_oid,
			    context->recdes_p, true, false);

  HEAP_PERF_TRACK_LOGGING (thread_p, context);

  mnt_heap_new_ver_inserts (thread_p);

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_insert_physical () - physical insert into heap page
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   is_mvcc_op(in): MVCC or non-MVCC operation
 *
 * NOTE: This fuction should receive a fixed page and a location in res_oid,
 *       where the context->recdes_p will go in.
 */
static int
heap_insert_physical (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context)
{
  /* check input */
  assert (context != NULL);
  assert (context->type == HEAP_OPERATION_INSERT);
  assert (context->recdes_p != NULL);
  assert (context->home_page_watcher_p->pgptr != NULL);

  /* assume we have the exact location for insert as well as a fixed page */
  assert (context->res_oid.volid != NULL_VOLID);
  assert (context->res_oid.pageid != NULL_PAGEID);
  assert (context->res_oid.slotid != NULL_SLOTID);

#if defined(CUBRID_DEBUG)
  /* function should have received map record if input record was multipage */
  if (heap_is_big_length (context->recdes_p->length))
    {
      er_log_debug (ARG_FILE_LINE,
		    "heap_insert_internal: This function does not accept"
		    " objects longer than %d. An object of %d was given\n", heap_Maxslotted_reclength, recdes->length);
      return ER_FAILED;
    }

  /* check we're inserting in a page of desired class */
  if (!OID_ISNULL (&context->class_oid))
    {
      OID heap_class_oid;
      int rc;

      rc = heap_get_class_oid_from_page (thread_p, context->home_page_watcher_p->pgptr, &heap_class_oid);
      assert (rc == NO_ERROR);
      assert (OID_EQ (&heap_class_oid, &context->class_oid));
    }
#endif

  /* physical insertion */
  if (spage_insert_at (thread_p, context->home_page_watcher_p->pgptr, context->res_oid.slotid, context->recdes_p) !=
      SP_SUCCESS)
    {
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      OID_SET_NULL (&context->res_oid);
      return ER_FAILED;
    }

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_log_insert_physical () - add logging information for physical insertion
 *   thread_p(in): thread entry
 *   page_p(in): page where insert was performed
 *   vfid_p(in): virtual file id
 *   oid_p(in): newly inserted object id
 *   recdes_p(in): record descriptor of inserted record
 *   is_mvcc_op(in): specifies type of operation (MVCC/non-MVCC)
 *   is_redistribute_op(in): whether the insertion is due to partition
 *			     redistribute operation and has a valid delid
 */
static void
heap_log_insert_physical (THREAD_ENTRY * thread_p, PAGE_PTR page_p, VFID * vfid_p, OID * oid_p, RECDES * recdes_p,
			  bool is_mvcc_op, bool is_redistribute_op)
{
  LOG_DATA_ADDR log_addr;

  /* populate address field */
  log_addr.vfid = vfid_p;
  log_addr.offset = oid_p->slotid;
  log_addr.pgptr = page_p;

  if (is_mvcc_op)
    {
      if (is_redistribute_op)
	{
	  /* this is actually a deleted record, inserted due to a PARTITION reorganize operation. Log this operation
	   * separately */
	  heap_mvcc_log_redistribute (thread_p, recdes_p, &log_addr);
	}
      else
	{
	  /* MVCC logging */
	  heap_mvcc_log_insert (thread_p, recdes_p, &log_addr);
	}
    }
  else
    {
      INT16 bytes_reserved;
      RECDES temp_recdes;

      if (recdes_p->type == REC_ASSIGN_ADDRESS)
	{
	  /* special case for REC_ASSIGN */
	  temp_recdes.type = recdes_p->type;
	  temp_recdes.area_size = sizeof (bytes_reserved);
	  temp_recdes.length = sizeof (bytes_reserved);
	  bytes_reserved = (INT16) recdes_p->length;
	  temp_recdes.data = (char *) &bytes_reserved;
	  log_append_undoredo_recdes (thread_p, RVHF_INSERT, &log_addr, NULL, &temp_recdes);
	}
      else if (recdes_p->type == REC_NEWHOME)
	{
	  /* we don't want replication for REC_NEWHOME; in any other respect RVHF_INSERT_NEWHOME is the same as
	   * RVHF_INSERT */
	  log_append_undoredo_recdes (thread_p, RVHF_INSERT_NEWHOME, &log_addr, NULL, recdes_p);
	}
      else
	{
	  log_append_undoredo_recdes (thread_p, RVHF_INSERT, &log_addr, NULL, recdes_p);
	}
    }
}

/*
 * heap_delete_adjust_recdes_header () - adjust record header for insert
 *                                       operation
 *   recdes_p(in): home record descriptor
 *   mvcc_id(in): MVCC identifier
 *   next_version_p(in): next version OID
 *   partition_oid_p(in): partition link OID
 *
 * NOTE: Only applicable for MVCC operations.
 */
void
heap_delete_adjust_header (MVCC_REC_HEADER * header_p, MVCCID mvcc_id, OID * next_version_p, OID * partition_oid_p,
			   int record_size)
{
  assert (header_p != NULL);

  /* put delete MVCCID */
  if (!MVCC_IS_FLAG_SET (header_p, OR_MVCC_FLAG_VALID_DELID))
    {
      if (!MVCC_IS_FLAG_SET (header_p, OR_MVCC_FLAG_VALID_LONG_CHN))
	{
	  /* the record contains short CHN, we need to extend to bigint */
	  record_size += OR_INT_SIZE;
	}
      else
	{
	  /* we already have long CHN */
	  MVCC_CLEAR_FLAG_BITS (header_p, OR_MVCC_FLAG_VALID_LONG_CHN);
	}
    }
  else
    {
      assert (!MVCCID_IS_VALID (MVCC_GET_DELID (header_p)));
    }
  MVCC_SET_FLAG_BITS (header_p, OR_MVCC_FLAG_VALID_DELID);
  MVCC_SET_DELID (header_p, mvcc_id);

  /* put next version (if applicable) */
  if (next_version_p != NULL && !OID_ISNULL (next_version_p))
    {
      if (!MVCC_IS_FLAG_SET (header_p, OR_MVCC_FLAG_VALID_NEXT_VERSION))
	{
	  MVCC_SET_FLAG_BITS (header_p, OR_MVCC_FLAG_VALID_NEXT_VERSION);
	  record_size += OR_OID_SIZE;
	}
      MVCC_SET_NEXT_VERSION (header_p, next_version_p);

      /* put partition link (if applicable) */
      if (partition_oid_p != NULL && !OID_ISNULL (partition_oid_p))
	{
	  if (!MVCC_IS_FLAG_SET (header_p, OR_MVCC_FLAG_VALID_PARTITION_OID))
	    {
	      MVCC_SET_FLAG_BITS (header_p, OR_MVCC_FLAG_VALID_PARTITION_OID);
	      record_size += OR_OID_SIZE;
	    }
	  MVCC_SET_PARTITION_OID (header_p, partition_oid_p);
	}
    }

  /* treat overflow */
  if (heap_is_big_length (record_size))
    {
      /* overflow records have maximum MVCC header size */
      HEAP_MVCC_SET_HEADER_MAXIMUM_SIZE (header_p);
    }
}

/*
 * heap_delete_adjust_recdes_header () - adjust record header of record
 *                                       descriptor for insert operation
 *   recdes_p(in): home record descriptor
 *   mvcc_id(in): MVCC identifier
 *   next_version_p(in): next version OID
 *   partition_oid_p(in): partition link OID
 *   returns: an MVCC record header adjusted for delete operation
 *
 * NOTE: Only applicable for MVCC operations.
 */
MVCC_REC_HEADER
heap_delete_adjust_recdes_header (RECDES * recdes_p, MVCCID mvcc_id, OID * next_version_p, OID * partition_oid_p)
{
  MVCC_REC_HEADER rec_header;

  assert (recdes_p != NULL);

  /* fetch MVCC header from record descriptor */
  if (or_mvcc_get_header (recdes_p, &rec_header) != NO_ERROR)
    {
      assert (false);
    }

  /* adjust this header */
  heap_delete_adjust_header (&rec_header, mvcc_id, next_version_p, partition_oid_p, recdes_p->length);

  /* return it for further use */
  return rec_header;
}

/*
 * heap_get_delete_location () - find the desired object and fix the page
 *   thread_p(in): thread entry
 *   context(in): delete operation context
 *   return: error code or NO_ERROR
 */
static int
heap_get_record_location (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context)
{
  VPID vpid;

  /* check input */
  assert (context != NULL);
  assert (!OID_ISNULL (&context->oid));
  assert (!HFID_IS_NULL (&context->hfid));

  /* get vpid from object */
  vpid.pageid = context->oid.pageid;
  vpid.volid = context->oid.volid;

  /* first try to retrieve cached fixed page from scancache */
  if (context->scan_cache_p != NULL && context->scan_cache_p->page_watcher.pgptr != NULL
      && context->scan_cache_p->cache_last_fix_page == true)
    {
      VPID *vpid_incache_p = pgbuf_get_vpid_ptr (context->scan_cache_p->page_watcher.pgptr);

      if (VPID_EQ (&vpid, vpid_incache_p))
	{
	  /* we can get it from the scancache */
	  pgbuf_replace_watcher (thread_p, &context->scan_cache_p->page_watcher, context->home_page_watcher_p);
	}
      else
	{
	  /* last scancache fixed page is not desired page */
	  pgbuf_ordered_unfix (thread_p, &context->scan_cache_p->page_watcher);
	}
      assert (context->scan_cache_p->page_watcher.pgptr == NULL);
    }

  /* if scancache page was not suitable, fix desired page */
  if (context->home_page_watcher_p->pgptr == NULL)
    {
      (void) heap_scan_pb_lock_and_fetch (thread_p, &vpid, OLD_PAGE, X_LOCK, context->scan_cache_p,
					  context->home_page_watcher_p);
      if (context->home_page_watcher_p->pgptr == NULL)
	{
	  int rc;

	  if (er_errid () == ER_PB_BAD_PAGEID)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, context->oid.volid,
		      context->oid.pageid, context->oid.slotid);
	    }

	  /* something went wrong, return */
	  ASSERT_ERROR_AND_SET (rc);
	  return rc;
	}
    }

#if !defined(NDEBUG)
  if (context->scan_cache_p != NULL)
    {
      OID heap_class_oid;

      assert (heap_get_class_oid_from_page (thread_p, context->home_page_watcher_p->pgptr, &heap_class_oid) ==
	      NO_ERROR);
      assert ((OID_EQ (&heap_class_oid, &context->scan_cache_p->node.class_oid))
	      || (OID_ISNULL (&context->scan_cache_p->node.class_oid)
		  && spage_get_record_type (context->home_page_watcher_p->pgptr,
					    context->oid.slotid) == REC_ASSIGN_ADDRESS));
    }
#endif

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_delete_bigone () - delete a REC_BIGONE record
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   is_mvcc_op(in): specifies type of operation (MVCC/non-MVCC)
 */
static int
heap_delete_bigone (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op)
{
  OID overflow_oid;
  int rc;

  /* check input */
  assert (context != NULL);
  assert (context->type == HEAP_OPERATION_DELETE);
  assert (context->home_recdes.data != NULL);
  assert (context->home_page_watcher_p != NULL);
  assert (context->home_page_watcher_p->pgptr != NULL);
  assert (context->overflow_page_watcher_p != NULL);
  assert (context->overflow_page_watcher_p->pgptr == NULL);

  /* MVCC info is in overflow page, we only keep and OID in home */
  overflow_oid = *((OID *) context->home_recdes.data);

  /* reset overflow watcher rank */
  PGBUF_WATCHER_RESET_RANK (context->overflow_page_watcher_p, PGBUF_ORDERED_HEAP_OVERFLOW);

  if (is_mvcc_op)
    {
      MVCC_REC_HEADER overflow_header;
      VPID overflow_vpid;
      LOG_DATA_ADDR log_addr;
      MVCCID mvcc_id = logtb_get_current_mvccid (thread_p);

      /* fix overflow page */
      overflow_vpid.pageid = overflow_oid.pageid;
      overflow_vpid.volid = overflow_oid.volid;
      PGBUF_WATCHER_COPY_GROUP (context->overflow_page_watcher_p, context->home_page_watcher_p);
      rc = pgbuf_ordered_fix (thread_p, &overflow_vpid, OLD_PAGE, PGBUF_LATCH_WRITE, context->overflow_page_watcher_p);
      if (rc != NO_ERROR)
	{
	  if (rc == ER_LK_PAGE_TIMEOUT && er_errid () == NO_ERROR)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_PAGE_LATCH_ABORTED, 2, overflow_vpid.volid,
		      overflow_vpid.pageid);
	    }
	  return rc;
	}

      /* check overflow page type */
      (void) pgbuf_check_page_ptype (thread_p, context->overflow_page_watcher_p->pgptr, PAGE_OVERFLOW);

      /* fetch header from overflow */
      if (heap_get_mvcc_rec_header_from_overflow (context->overflow_page_watcher_p->pgptr, &overflow_header, NULL) !=
	  NO_ERROR)
	{
	  return ER_FAILED;
	}
      assert (or_mvcc_header_size_from_flags (overflow_header.mvcc_flag) == OR_MVCC_MAX_HEADER_SIZE);

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      /* log operation */
      log_addr.pgptr = context->overflow_page_watcher_p->pgptr;
      log_addr.vfid = &context->hfid.vfid;
      log_addr.offset = overflow_oid.slotid;
      heap_mvcc_log_delete (thread_p, MVCC_GET_CHN (&overflow_header), &context->next_version, &context->partition_link,
			    &log_addr, RVHF_MVCC_DELETE_OVERFLOW);

      HEAP_PERF_TRACK_LOGGING (thread_p, context);

      /* adjust header; provide zero length, we don't care to make header max size since it's already done */
      heap_delete_adjust_header (&overflow_header, mvcc_id, &context->next_version, &context->partition_link, 0);

      /* write header to overflow */
      rc = heap_set_mvcc_rec_header_on_overflow (context->overflow_page_watcher_p->pgptr, &overflow_header);
      if (rc != NO_ERROR)
	{
	  return rc;
	}

      /* set page as dirty */
      pgbuf_set_dirty (thread_p, context->overflow_page_watcher_p->pgptr, DONT_FREE);

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      /* Home record is not changed, but page max MVCCID and vacuum status have to change. Also vacuum needs to be
       * vacuum with the location of home record (REC_RELOCATION). */
      log_addr.vfid = &context->hfid.vfid;
      log_addr.pgptr = context->home_page_watcher_p->pgptr;
      log_addr.offset = context->oid.slotid;
      heap_mvcc_log_home_no_change_on_delete (thread_p, &log_addr);

      pgbuf_set_dirty (thread_p, context->home_page_watcher_p->pgptr, DONT_FREE);

      HEAP_PERF_TRACK_LOGGING (thread_p, context);

      mnt_heap_big_mvcc_deletes (thread_p);
    }
  else
    {
      bool is_reusable = heap_is_reusable_oid (context->file_type);

      /* fix header page */
      rc = heap_fix_header_page (thread_p, context);
      if (rc != NO_ERROR)
	{
	  return rc;
	}

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      if (context->home_page_watcher_p->page_was_unfixed)
	{
	  /* 
	   * Need to get the record again, since record may have changed
	   * by other transactions (INSID removed by VACUUM, page compact).
	   * The object was already locked, so the record size may be the
	   * same or smaller (INSID removed by VACUUM). 
	   */
	  int is_peeking = (context->home_recdes.area_size >= context->home_recdes.length) ? COPY : PEEK;
	  if (spage_get_record (context->home_page_watcher_p->pgptr, context->oid.slotid, &context->home_recdes,
				is_peeking) != S_SUCCESS)
	    {
	      return ER_FAILED;
	    }
	}

      /* log operation */
      heap_log_delete_physical (thread_p, context->home_page_watcher_p->pgptr, &context->hfid.vfid, &context->oid,
				&context->home_recdes, is_reusable);

      HEAP_PERF_TRACK_LOGGING (thread_p, context);

      /* physical deletion of home record */
      rc = heap_delete_physical (thread_p, &context->hfid, context->home_page_watcher_p->pgptr, &context->oid);
      if (rc != NO_ERROR)
	{
	  return rc;
	}

      /* physical deletion of overflow record */
      if (heap_ovf_delete (thread_p, &context->hfid, &overflow_oid, NULL) == NULL)
	{
	  return ER_FAILED;
	}

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      mnt_heap_big_deletes (thread_p);
    }

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_delete_relocation () - delete a REC_RELOCATION record
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   is_mvcc_op(in): specifies type of operation (MVCC/non-MVCC)
 *   returns: error code or NO_ERROR
 */
static int
heap_delete_relocation (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op)
{
  RECDES forward_recdes;
  OID forward_oid;
  int rc;

  /* check input */
  assert (context != NULL);
  assert (context->type == HEAP_OPERATION_DELETE);
  assert (context->record_type == REC_RELOCATION);
  assert (context->home_page_watcher_p != NULL);
  assert (context->home_page_watcher_p->pgptr != NULL);
  assert (context->forward_page_watcher_p != NULL);

  /* get forward oid */
  forward_oid = *((OID *) context->home_recdes.data);

  /* fix forward page */
  if (heap_fix_forward_page (thread_p, context, &forward_oid) != NO_ERROR)
    {
      return ER_FAILED;
    }

  /* get forward record */
  if (spage_get_record (context->forward_page_watcher_p->pgptr, forward_oid.slotid, &forward_recdes, PEEK) != S_SUCCESS)
    {
      return ER_FAILED;
    }

  HEAP_PERF_TRACK_PREPARE (thread_p, context);

  if (is_mvcc_op)
    {
      RECDES new_forward_recdes, new_home_recdes;
      MVCC_REC_HEADER forward_rec_header, new_forward_rec_header;
      MVCCID mvcc_id = logtb_get_current_mvccid (thread_p);
      char buffer[IO_DEFAULT_PAGE_SIZE + OR_MVCC_MAX_HEADER_SIZE + MAX_ALIGNMENT];
      OID new_forward_oid[2];
      int adjusted_size, forward_rec_header_size;
      bool fits_in_home, fits_in_forward;
      bool update_old_home = false;
      bool update_old_forward = false;
      bool remove_old_forward = false;

      /* compute new record size (we'll do the real adjustments once we know if we need the header page) */
      if (or_mvcc_get_header (&forward_recdes, &forward_rec_header) != NO_ERROR)
	{
	  return ER_FAILED;
	}
      forward_rec_header_size = or_mvcc_header_size_from_flags (forward_rec_header.mvcc_flag);
      adjusted_size = forward_recdes.length;
      if (!MVCC_IS_FLAG_SET (&forward_rec_header, OR_MVCC_FLAG_VALID_DELID)
	  && !MVCC_IS_FLAG_SET (&forward_rec_header, OR_MVCC_FLAG_VALID_LONG_CHN))
	{
	  /* extend short CHN to MVCCID */
	  adjusted_size += OR_INT_SIZE;
	}
      if (!OID_ISNULL (&context->next_version)
	  && !MVCC_IS_FLAG_SET (&forward_rec_header, OR_MVCC_FLAG_VALID_NEXT_VERSION))
	{
	  /* next version */
	  adjusted_size += OR_OID_SIZE;
	}
      if (!OID_ISNULL (&context->partition_link)
	  && !MVCC_IS_FLAG_SET (&forward_rec_header, OR_MVCC_FLAG_VALID_PARTITION_OID))
	{
	  /* next version */
	  adjusted_size += OR_OID_SIZE;
	}
      if (heap_is_big_length (adjusted_size))
	{
	  /* not exactly necessary, but we'll be able to compare sizes */
	  adjusted_size = forward_recdes.length - forward_rec_header_size + OR_MVCC_MAX_HEADER_SIZE;
	}

      /* fix header if necessary */
      fits_in_home =
	spage_is_updatable (thread_p, context->home_page_watcher_p->pgptr, context->oid.slotid, adjusted_size);
      fits_in_forward =
	spage_is_updatable (thread_p, context->forward_page_watcher_p->pgptr, forward_oid.slotid, adjusted_size);
      if (heap_is_big_length (adjusted_size) || (!fits_in_forward && !fits_in_home))
	{
	  /* fix header page */
	  rc = heap_fix_header_page (thread_p, context);
	  if (rc != NO_ERROR)
	    {
	      return ER_FAILED;
	    }

	  if (context->forward_page_watcher_p->page_was_unfixed)
	    {
	      /* re-peek forward record descriptor; forward page may have been unfixed by previous pgbuf_ordered_fix()
	       * call */
	      if (spage_get_record (context->forward_page_watcher_p->pgptr, forward_oid.slotid, &forward_recdes, PEEK)
		  != S_SUCCESS)
		{
		  return ER_FAILED;
		}
	    }
	}

      /* get adjusted forward record header */
      new_forward_rec_header =
	heap_delete_adjust_recdes_header (&forward_recdes, mvcc_id, &context->next_version, &context->partition_link);

      /* build new record */
      new_forward_recdes.data = PTR_ALIGN (buffer, MAX_ALIGNMENT);
      new_forward_recdes.area_size = IO_DEFAULT_PAGE_SIZE + OR_MVCC_MAX_HEADER_SIZE + MAX_ALIGNMENT;
      new_forward_recdes.type = REC_UNKNOWN;	/* to be filled in */
      new_forward_recdes.length = 0;

      or_mvcc_add_header (&new_forward_recdes, &new_forward_rec_header, OR_GET_BOUND_BIT_FLAG (forward_recdes.data),
			  OR_GET_OFFSET_SIZE (forward_recdes.data));

      memcpy (new_forward_recdes.data + new_forward_recdes.length, forward_recdes.data + forward_rec_header_size,
	      forward_recdes.length - forward_rec_header_size);
      new_forward_recdes.length += forward_recdes.length - forward_rec_header_size;

      assert (new_forward_recdes.length == adjusted_size);

      /* determine what operations on home/forward pages are necessary and execute extra operations for each case */
      if (heap_is_big_length (adjusted_size))
	{
	  /* insert new overflow record */
	  if (heap_ovf_insert (thread_p, &context->hfid, &new_forward_oid[0], &new_forward_recdes, NULL) == NULL)
	    {
	      return ER_FAILED;
	    }

	  /* home record descriptor will be an overflow OID and will be placed in original home page */
	  heap_build_forwarding_recdes (&new_home_recdes, REC_BIGONE, new_forward_oid,
					!OID_ISNULL (&context->partition_link));

	  /* remove old forward record */
	  remove_old_forward = true;
	  update_old_home = true;

	  mnt_heap_rel_to_big_deletes (thread_p);
	}
      else if (fits_in_home)
	{
	  /* updated forward record fits in home page */
	  new_home_recdes = new_forward_recdes;
	  new_home_recdes.type = REC_HOME;

	  /* clear forward rebuild_record (just to be safe) */
	  new_forward_recdes.area_size = 0;
	  new_forward_recdes.length = 0;
	  new_forward_recdes.type = REC_UNKNOWN;
	  new_forward_recdes.data = NULL;

	  /* remove old forward record */
	  remove_old_forward = true;
	  update_old_home = true;

	  mnt_heap_rel_to_home_deletes (thread_p);
	}
      else if (fits_in_forward)
	{
	  /* updated forward record fits in old forward page */
	  new_forward_recdes.type = REC_NEWHOME;

	  /* home record will not be touched */
	  update_old_forward = true;

	  mnt_heap_rel_mvcc_deletes (thread_p);
	}
      else
	{
	  /* doesn't fit in either home or forward page */
	  /* insert a new forward record */
	  new_forward_recdes.type = REC_NEWHOME;
	  rc = heap_insert_newhome (thread_p, context, &new_forward_recdes, &new_forward_oid[0]);
	  if (rc != NO_ERROR)
	    {
	      return rc;
	    }

	  /* new home record will be a REC_RELOCATION and will be placed in the original home page */
	  heap_build_forwarding_recdes (&new_home_recdes, REC_RELOCATION, new_forward_oid,
					!OID_ISNULL (&context->partition_link));

	  /* remove old forward record */
	  remove_old_forward = true;
	  update_old_home = true;

	  mnt_heap_rel_to_rel_deletes (thread_p);
	}

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      /* 
       * Update old home record (if necessary)
       */
      if (update_old_home)
	{
	  LOG_DATA_ADDR home_addr;

	  if (context->home_page_watcher_p->page_was_unfixed)
	    {
	      /* 
	       * Need to get the record again, since record may have changed
	       * by other transactions (INSID removed by VACUUM, page compact).
	       * The object was already locked, so the record size may be the
	       * same or smaller (INSID removed by VACUUM). 
	       */
	      int is_peeking = (context->home_recdes.area_size >= context->home_recdes.length) ? COPY : PEEK;
	      if (spage_get_record (context->home_page_watcher_p->pgptr, context->oid.slotid, &context->home_recdes,
				    is_peeking) != S_SUCCESS)
		{
		  return ER_FAILED;
		}
	    }

	  /* log operation */
	  home_addr.vfid = &context->hfid.vfid;
	  home_addr.pgptr = context->home_page_watcher_p->pgptr;
	  home_addr.offset = context->oid.slotid;

	  heap_mvcc_log_home_change_on_delete (thread_p, &context->home_recdes, &new_home_recdes, &home_addr);

	  HEAP_PERF_TRACK_LOGGING (thread_p, context);

	  /* update home record */
	  rc =
	    heap_update_physical (thread_p, context->home_page_watcher_p->pgptr, context->oid.slotid, &new_home_recdes);
	  if (rc != NO_ERROR)
	    {
	      return rc;
	    }

	  HEAP_PERF_TRACK_EXECUTE (thread_p, context);
	}
      else
	{
	  /* Home record is not changed, but page max MVCCID and vacuum status have to change. Also vacuum needs to be
	   * vacuum with the location of home record (REC_BIGONE). */
	  LOG_DATA_ADDR home_addr;

	  /* log operation */
	  home_addr.vfid = &context->hfid.vfid;
	  home_addr.pgptr = context->home_page_watcher_p->pgptr;
	  home_addr.offset = context->oid.slotid;
	  heap_mvcc_log_home_no_change_on_delete (thread_p, &home_addr);
	  pgbuf_set_dirty (thread_p, context->home_page_watcher_p->pgptr, DONT_FREE);

	  HEAP_PERF_TRACK_LOGGING (thread_p, context);
	}

      /* 
       * Update old forward record (if necessary)
       */
      if (update_old_forward)
	{
	  LOG_DATA_ADDR forward_addr;
	  INT32 chn;

	  /* log operation */
	  forward_addr.vfid = &context->hfid.vfid;
	  forward_addr.pgptr = context->forward_page_watcher_p->pgptr;
	  forward_addr.offset = forward_oid.slotid;
	  chn = MVCC_GET_CHN (&forward_rec_header);
	  heap_mvcc_log_delete (thread_p, chn, &context->next_version, &context->partition_link, &forward_addr,
				RVHF_MVCC_DELETE_REC_NEWHOME);

	  HEAP_PERF_TRACK_LOGGING (thread_p, context);

	  /* physical update of forward record */
	  rc =
	    heap_update_physical (thread_p, context->forward_page_watcher_p->pgptr, forward_oid.slotid,
				  &new_forward_recdes);
	  if (rc != NO_ERROR)
	    {
	      return rc;
	    }

	  HEAP_PERF_TRACK_EXECUTE (thread_p, context);
	}

      /* 
       * Delete old forward record (if necessary)
       */
      if (remove_old_forward)
	{
	  LOG_DATA_ADDR forward_addr;

	  /* re-peek forward record descriptor; forward page may have been unfixed by previous pgbuf_ordered_fix() call 
	   */
	  if (context->forward_page_watcher_p->page_was_unfixed)
	    {
	      if (spage_get_record (context->forward_page_watcher_p->pgptr, forward_oid.slotid, &forward_recdes, PEEK)
		  != S_SUCCESS)
		{
		  return ER_FAILED;
		}
	    }

	  /* operation logging */
	  forward_addr.vfid = &context->hfid.vfid;
	  forward_addr.pgptr = context->forward_page_watcher_p->pgptr;
	  forward_addr.offset = forward_oid.slotid;

	  log_append_undoredo_recdes (thread_p, RVHF_DELETE, &forward_addr, &forward_recdes, NULL);
	  if (heap_is_reusable_oid (context->file_type))
	    {
	      log_append_postpone (thread_p, RVHF_MARK_REUSABLE_SLOT, &forward_addr, 0, NULL);
	    }

	  HEAP_PERF_TRACK_LOGGING (thread_p, context);

	  /* physical removal of forward record */
	  rc = heap_delete_physical (thread_p, &context->hfid, context->forward_page_watcher_p->pgptr, &forward_oid);
	  if (rc != NO_ERROR)
	    {
	      return rc;
	    }

	  HEAP_PERF_TRACK_EXECUTE (thread_p, context);
	}
    }
  else
    {
      bool is_reusable = heap_is_reusable_oid (context->file_type);

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      if (context->home_page_watcher_p->page_was_unfixed)
	{
	  /* 
	   * Need to get the record again, since record may have changed
	   * by other transactions (INSID removed by VACUUM, page compact).
	   * The object was already locked, so the record size may be the
	   * same or smaller (INSID removed by VACUUM). 
	   */
	  int is_peeking = (context->home_recdes.area_size >= context->home_recdes.length) ? COPY : PEEK;
	  if (spage_get_record (context->home_page_watcher_p->pgptr, context->oid.slotid, &context->home_recdes,
				is_peeking) != S_SUCCESS)
	    {
	      return ER_FAILED;
	    }
	}
      /* 
       * Delete home record
       */

      heap_log_delete_physical (thread_p, context->home_page_watcher_p->pgptr, &context->hfid.vfid, &context->oid,
				&context->home_recdes, is_reusable);

      HEAP_PERF_TRACK_LOGGING (thread_p, context);

      /* physical deletion of home record */
      rc = heap_delete_physical (thread_p, &context->hfid, context->home_page_watcher_p->pgptr, &context->oid);
      if (rc != NO_ERROR)
	{
	  return rc;
	}

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      if (context->forward_page_watcher_p->page_was_unfixed)
	{
	  /* re-peek forward record descriptor; forward page may have been unfixed by previous pgbuf_ordered_fix() call 
	   */
	  if (spage_get_record (context->forward_page_watcher_p->pgptr, forward_oid.slotid, &forward_recdes, PEEK) !=
	      S_SUCCESS)
	    {
	      return ER_FAILED;
	    }
	}
      /* 
       * Delete forward record
       */
      /* 
       * It should be safe to mark the new home slot as reusable regardless
       * of the heap type (reusable OID or not) as the relocated record
       * should not be referenced anywhere in the database.
       */
      heap_log_delete_physical (thread_p, context->forward_page_watcher_p->pgptr, &context->hfid.vfid, &forward_oid,
				&forward_recdes, true);

      HEAP_PERF_TRACK_LOGGING (thread_p, context);

      /* physical deletion of forward record */
      rc = heap_delete_physical (thread_p, &context->hfid, context->forward_page_watcher_p->pgptr, &forward_oid);
      if (rc != NO_ERROR)
	{
	  return rc;
	}

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      mnt_heap_rel_deletes (thread_p);
    }

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_delete_home () - delete a REC_HOME (or REC_ASSIGN_ADDRESS) record
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   is_mvcc_op(in): specifies type of operation (MVCC/non-MVCC)
 *   returns: error code or NO_ERROR
 */
static int
heap_delete_home (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op)
{
  int error_code = NO_ERROR;

  /* check input */
  assert (context != NULL);
  assert (context->record_type == REC_HOME || context->record_type == REC_ASSIGN_ADDRESS);
  assert (context->type == HEAP_OPERATION_DELETE);
  assert (context->home_page_watcher_p != NULL);
  assert (context->home_page_watcher_p->pgptr != NULL);

  if (context->home_page_watcher_p->page_was_unfixed)
    {
      /* 
       * Need to get the record again, since record may have changed
       * by other transactions (INSID removed by VACUUM, page compact).
       * The object was already locked, so the record size may be the
       * same or smaller (INSID removed by VACUUM). 
       */
      int is_peeking = (context->home_recdes.area_size >= context->home_recdes.length) ? COPY : PEEK;
      if (spage_get_record (context->home_page_watcher_p->pgptr, context->oid.slotid, &context->home_recdes, is_peeking)
	  != S_SUCCESS)
	{
	  assert (false);
	  return ER_FAILED;
	}
    }

  /* operation */
  if (is_mvcc_op)
    {
      MVCC_REC_HEADER record_header, built_rec_header;
      RECDES built_recdes;
      RECDES forwarding_recdes;
      RECDES *home_page_updated_recdes;
      OID forward_oid[2];
      MVCCID mvcc_id = logtb_get_current_mvccid (thread_p);
      char data_buffer[IO_DEFAULT_PAGE_SIZE + OR_MVCC_MAX_HEADER_SIZE + MAX_ALIGNMENT];
      int header_size;
      INT32 chn;

      /* get home record header size */
      error_code = or_mvcc_get_header (&context->home_recdes, &record_header);
      if (error_code != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  return error_code;
	}
      header_size = or_mvcc_header_size_from_flags (record_header.mvcc_flag);
      chn = MVCC_GET_CHN (&record_header);

      /* build an adjusted record header */
      built_rec_header =
	heap_delete_adjust_recdes_header (&context->home_recdes, mvcc_id, &context->next_version,
					  &context->partition_link);

      /* build new record descriptor */
      built_recdes.data = PTR_ALIGN (data_buffer, MAX_ALIGNMENT);
      built_recdes.area_size = IO_DEFAULT_PAGE_SIZE + OR_MVCC_MAX_HEADER_SIZE;
      built_recdes.length = 0;

      /* add data to record descriptor */
      or_mvcc_add_header (&built_recdes, &built_rec_header, OR_GET_BOUND_BIT_FLAG (context->home_recdes.data),
			  OR_GET_OFFSET_SIZE (context->home_recdes.data));
      memcpy (built_recdes.data + built_recdes.length, context->home_recdes.data + header_size,
	      context->home_recdes.length - header_size);
      built_recdes.length += (context->home_recdes.length - header_size);

      /* determine type */
      if (heap_is_big_length (built_recdes.length))
	{
	  built_recdes.type = REC_BIGONE;
	}
      else if (!spage_is_updatable (thread_p, context->home_page_watcher_p->pgptr, context->oid.slotid,
				    built_recdes.length))
	{
	  built_recdes.type = REC_NEWHOME;
	}
      else
	{
	  built_recdes.type = REC_HOME;
	}

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      /* check whether relocation is necessary */
      if (built_recdes.type == REC_BIGONE || built_recdes.type == REC_NEWHOME)
	{
	  /* 
	   * Relocation necessary
	   */
	  MVCC_RELOCATE_DELETE_INFO mvcc_relocate_delete;
	  LOG_DATA_ADDR rec_address;

	  /* populate relocation info structure */
	  mvcc_relocate_delete.mvcc_delete_oid = &context->oid;
	  mvcc_relocate_delete.next_version = NULL;

	  /* insertion of built record */
	  if (built_recdes.type == REC_BIGONE)
	    {
	      /* new record is overflow record - REC_BIGONE case */
	      forwarding_recdes.type = REC_BIGONE;
	      if (heap_ovf_insert (thread_p, &context->hfid, &forward_oid[0], &built_recdes, &mvcc_relocate_delete) ==
		  NULL)
		{
		  ASSERT_ERROR_AND_SET (error_code);
		  return error_code;
		}

	      mnt_heap_home_to_big_deletes (thread_p);
	    }
	  else
	    {
	      /* new record is relocated - REC_NEWHOME case */
	      forwarding_recdes.type = REC_RELOCATION;

	      /* insert NEWHOME record */
	      error_code = heap_insert_newhome (thread_p, context, &built_recdes, &forward_oid[0]);
	      if (error_code != NO_ERROR)
		{
		  ASSERT_ERROR ();
		  return error_code;
		}

	      mnt_heap_home_to_rel_deletes (thread_p);
	    }

	  /* build forwarding rebuild_record */
	  heap_build_forwarding_recdes (&forwarding_recdes, forwarding_recdes.type, forward_oid,
					!OID_ISNULL (&context->partition_link));

	  HEAP_PERF_TRACK_EXECUTE (thread_p, context);

	  if (context->home_page_watcher_p->page_was_unfixed)
	    {
	      /* 
	       * Need to get the record again, since record may have changed
	       * by other transactions (INSID removed by VACUUM, page compact).
	       * The object was already locked, so the record size may be the
	       * same or smaller (INSID removed by VACUUM). 
	       */
	      int is_peeking = (context->home_recdes.area_size >= context->home_recdes.length) ? COPY : PEEK;
	      if (spage_get_record (context->home_page_watcher_p->pgptr, context->oid.slotid, &context->home_recdes,
				    is_peeking) != S_SUCCESS)
		{
		  assert (false);
		  return ER_FAILED;
		}
	    }

	  /* log relocation */
	  rec_address.pgptr = context->home_page_watcher_p->pgptr;
	  rec_address.vfid = &context->hfid.vfid;
	  rec_address.offset = context->oid.slotid;
	  heap_mvcc_log_home_change_on_delete (thread_p, &context->home_recdes, &forwarding_recdes, &rec_address);

	  HEAP_PERF_TRACK_LOGGING (thread_p, context);

	  /* we'll update the home page with the forwarding record */
	  home_page_updated_recdes = &forwarding_recdes;
	}
      else
	{
	  LOG_DATA_ADDR rec_address;

	  /* 
	   * No relocation, can be updated in place
	   */

	  rec_address.pgptr = context->home_page_watcher_p->pgptr;
	  rec_address.vfid = &context->hfid.vfid;
	  rec_address.offset = context->oid.slotid;
	  heap_mvcc_log_delete (thread_p, chn, &context->next_version, &context->partition_link, &rec_address,
				RVHF_MVCC_DELETE_REC_HOME);

	  HEAP_PERF_TRACK_LOGGING (thread_p, context);

	  /* we'll update the home page with the built record, since it fits in home page */
	  home_page_updated_recdes = &built_recdes;

	  mnt_heap_home_mvcc_deletes (thread_p);
	}

      /* update home page and check operation result */
      error_code =
	heap_update_physical (thread_p, context->home_page_watcher_p->pgptr, context->oid.slotid,
			      home_page_updated_recdes);
      if (error_code != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  return error_code;
	}

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);
    }
  else
    {
      bool is_reusable = heap_is_reusable_oid (context->file_type);

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      /* log operation */
      heap_log_delete_physical (thread_p, context->home_page_watcher_p->pgptr, &context->hfid.vfid, &context->oid,
				&context->home_recdes, is_reusable);

      HEAP_PERF_TRACK_LOGGING (thread_p, context);

      /* physical deletion */
      error_code = heap_delete_physical (thread_p, &context->hfid, context->home_page_watcher_p->pgptr, &context->oid);

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      mnt_heap_home_deletes (thread_p);

      assert (error_code == NO_ERROR || er_errid () != NO_ERROR);
      return error_code;
    }

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_delete_physical () - physical deletion of a record
 *   thread_p(in): thread entry
 *   hfid_p(in): heap file identifier where record is located
 *   page_p(in): page where record is stored
 *   oid_p(in): object identifier of record
 */
static int
heap_delete_physical (THREAD_ENTRY * thread_p, HFID * hfid_p, PAGE_PTR page_p, OID * oid_p)
{
  int free_space;

  /* check input */
  assert (hfid_p != NULL);
  assert (page_p != NULL);
  assert (oid_p != NULL);
  assert (oid_p->slotid != NULL_SLOTID);

  /* save old freespace */
  free_space = spage_get_free_space_without_saving (thread_p, page_p, NULL);

  /* physical deletion */
  if (spage_delete (thread_p, page_p, oid_p->slotid) == NULL_SLOTID)
    {
      return ER_FAILED;
    }

  /* update statistics */
  heap_stats_update (thread_p, page_p, hfid_p, free_space);

  /* mark page as dirty */
  pgbuf_set_dirty (thread_p, page_p, DONT_FREE);

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_log_delete_physical () - log physical deletion
 *   thread_p(in): thread entry
 *   page_p(in): page pointer
 *   vfid_p(in): virtual file identifier
 *   oid_p(in): object identifier of deleted record
 *   recdes_p(in): record descriptor of deleted record
 *   mark_reusable(in): if true, will mark the slot as reusable
 */
static void
heap_log_delete_physical (THREAD_ENTRY * thread_p, PAGE_PTR page_p, VFID * vfid_p, OID * oid_p, RECDES * recdes_p,
			  bool mark_reusable)
{
  LOG_DATA_ADDR log_addr;

  /* check input */
  assert (page_p != NULL);
  assert (vfid_p != NULL);
  assert (oid_p != NULL);
  assert (recdes_p != NULL);

  /* populate address */
  log_addr.offset = oid_p->slotid;
  log_addr.pgptr = page_p;
  log_addr.vfid = vfid_p;

  if (recdes_p->type == REC_ASSIGN_ADDRESS)
    {
      /* special case for REC_ASSIGN */
      RECDES temp_recdes;
      INT16 bytes_reserved;

      temp_recdes.type = recdes_p->type;
      temp_recdes.area_size = sizeof (bytes_reserved);
      temp_recdes.length = sizeof (bytes_reserved);
      bytes_reserved = (INT16) recdes_p->length;
      temp_recdes.data = (char *) &bytes_reserved;

      log_append_undoredo_recdes (thread_p, RVHF_DELETE, &log_addr, &temp_recdes, NULL);
    }
  else
    {
      /* log record descriptor */
      log_append_undoredo_recdes (thread_p, RVHF_DELETE, &log_addr, recdes_p, NULL);
    }

  /* log postponed operation */
  if (mark_reusable)
    {
      log_append_postpone (thread_p, RVHF_MARK_REUSABLE_SLOT, &log_addr, 0, NULL);
    }
}

/*
 * heap_update_bigone () - update a REC_BIGONE record
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   is_mvcc_op(in): type of operation (MVCC/non-MVCC)
 */
static int
heap_update_bigone (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op)
{
  OID overflow_oid;

  assert (context != NULL);
  assert (context->type == HEAP_OPERATION_UPDATE);
  assert (context->recdes_p != NULL);
  assert (context->home_page_watcher_p != NULL);
  assert (context->home_page_watcher_p->pgptr != NULL);
  assert (context->overflow_page_watcher_p != NULL);

  /* read OID of overflow record */
  overflow_oid = *((OID *) context->home_recdes.data);

  /* update */
  if (is_mvcc_op && !HEAP_IS_UPDATE_INPLACE (context->update_in_place))
    {
      HEAP_OPERATION_CONTEXT delete_context, insert_context;

      /* 
       * Insert new version
       */
      /* prepare insert context */
      heap_create_insert_context (&insert_context, &context->hfid, &context->class_oid, context->recdes_p, NULL, false);
      COPY_OID (&insert_context.partition_link, &context->partition_link);
      insert_context.flags = context->flags;
      insert_context.header_page_watcher_p = &context->header_page_watcher;
      insert_context.time_track = context->time_track;

      /* insert new version */
      context->recdes_p->type = REC_HOME;
      if (heap_insert_newver (thread_p, &insert_context, context->home_page_watcher_p) != NO_ERROR)
	{
	  return ER_FAILED;
	}

      /* unfix new version's home */
      heap_unfix_watchers (thread_p, &insert_context);

      /* keep new location */
      assert (!OID_ISNULL (&insert_context.res_oid));
      COPY_OID (&context->res_oid, &insert_context.res_oid);

      /* 
       * Delete old version and link it to new version
       * NOTE: new version OID is kept in insert_context.oid
       */
      /* prepare delete context */
      heap_create_delete_context (&delete_context, &context->hfid, &context->oid, &context->class_oid, NULL);
      delete_context.record_type = context->record_type;	/* skip extra work */
      delete_context.file_type = context->file_type;	/* skip extra work */
      delete_context.home_recdes = context->home_recdes;	/* skip extra work */
      COPY_OID (&delete_context.next_version, &insert_context.res_oid);
      COPY_OID (&delete_context.partition_link, &context->partition_link);
      heap_link_watchers (&delete_context, context);
      delete_context.time_track = context->time_track;

      /* delete old record */
      if (heap_delete_bigone (thread_p, &delete_context, true) != NO_ERROR)
	{
	  return ER_FAILED;
	}
    }
  else
    {
      /* fix header page */
      if (heap_fix_header_page (thread_p, context) != NO_ERROR)
	{
	  return ER_FAILED;
	}

      if (heap_is_big_length (context->recdes_p->length))
	{
	  /* overflow -> overflow update */
	  if (heap_ovf_update (thread_p, &context->hfid, &overflow_oid, context->recdes_p) == NULL)
	    {
	      return ER_FAILED;
	    }
	}
      else
	{
	  RECDES old_home_recdes;
	  OID old_overflow_oid[2];

	  if (context->home_page_watcher_p->page_was_unfixed)
	    {
	      /* 
	       * Need to get the record again, since record may have changed
	       * by other transactions (INSID removed by VACUUM, page compact).
	       * The object was already locked, so the record size may be the
	       * same or smaller (INSID removed by VACUUM). 
	       */
	      int is_peeking = (context->home_recdes.area_size >= context->home_recdes.length) ? COPY : PEEK;
	      if (spage_get_record (context->home_page_watcher_p->pgptr, context->oid.slotid, &context->home_recdes,
				    is_peeking) != S_SUCCESS)
		{
		  return ER_FAILED;
		}
	    }

	  /* keep a copy of old home recdes for logging purposes */
	  old_home_recdes = context->home_recdes;
	  old_home_recdes.data = (char *) &old_overflow_oid[0];
	  old_overflow_oid[0] = overflow_oid;
	  OID_SET_NULL (&old_overflow_oid[1]);

	  /* overflow -> non-overflow update */
	  if (spage_update (thread_p, context->home_page_watcher_p->pgptr, context->oid.slotid, context->recdes_p) ==
	      SP_SUCCESS)
	    {
	      /* yaay! new record fits in home page; update it's type in the page */
	      context->recdes_p->type = REC_HOME;
	      spage_update_record_type (thread_p, context->home_page_watcher_p->pgptr, context->oid.slotid, REC_HOME);

	      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

	      /* log operation */
	      heap_log_update_physical (thread_p, context->home_page_watcher_p->pgptr, &context->hfid.vfid,
					&context->oid, &old_home_recdes, context->recdes_p, is_mvcc_op);

	      HEAP_PERF_TRACK_LOGGING (thread_p, context);
	    }
	  else
	    {
	      RECDES forwarding_recdes;
	      OID newhome_oid[2];

	      /* insert new home */
	      context->recdes_p->type = REC_NEWHOME;
	      if (heap_insert_newhome (thread_p, context, context->recdes_p, &newhome_oid[0]) != NO_ERROR)
		{
		  return ER_FAILED;
		}

	      /* prepare record descriptor */
	      heap_build_forwarding_recdes (&forwarding_recdes, REC_RELOCATION, newhome_oid,
					    !OID_ISNULL (&context->partition_link));

	      /* update home */
	      if (heap_update_physical (thread_p, context->home_page_watcher_p->pgptr, context->oid.slotid,
					&forwarding_recdes) != NO_ERROR)
		{
		  return ER_FAILED;
		}

	      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

	      /* log operation */
	      heap_log_update_physical (thread_p, context->home_page_watcher_p->pgptr, &context->hfid.vfid,
					&context->oid, &old_home_recdes, &forwarding_recdes, is_mvcc_op);

	      HEAP_PERF_TRACK_LOGGING (thread_p, context);
	    }

	  /* remove old overflow record */
	  if (heap_ovf_delete (thread_p, &context->hfid, &overflow_oid, NULL) == NULL)
	    {
	      return ER_FAILED;
	    }

	  HEAP_PERF_TRACK_EXECUTE (thread_p, context);

	  /* dirty home page */
	  pgbuf_set_dirty (thread_p, context->home_page_watcher_p->pgptr, DONT_FREE);

	  /* location did not change */
	  COPY_OID (&context->res_oid, &context->oid);
	}

      mnt_heap_big_updates (thread_p);
    }

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_update_relocation () - update a REC_RELOCATION/REC_NEWHOME combo
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   is_mvcc_op(in): type of operation (MVCC/non-MVCC)
 */
static int
heap_update_relocation (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op)
{
  RECDES forward_recdes;
  char forward_recdes_buffer[IO_MAX_PAGE_SIZE + MAX_ALIGNMENT];
  OID forward_oid;
  int rc;

  assert (context != NULL);
  assert (context->recdes_p != NULL);
  assert (context->type == HEAP_OPERATION_UPDATE);
  assert (context->home_page_watcher_p != NULL);
  assert (context->home_page_watcher_p->pgptr != NULL);
  assert (context->forward_page_watcher_p != NULL);

  /* get forward oid */
  forward_oid = *((OID *) context->home_recdes.data);

  /* fix forward page */
  if (heap_fix_forward_page (thread_p, context, &forward_oid) != NO_ERROR)
    {
      return ER_FAILED;
    }

  /* get forward record */
  forward_recdes.area_size = DB_PAGESIZE;
  forward_recdes.data = PTR_ALIGN (forward_recdes_buffer, MAX_ALIGNMENT);
  if (spage_get_record (context->forward_page_watcher_p->pgptr, forward_oid.slotid, &forward_recdes, COPY) != S_SUCCESS)
    {
      assert (false);
      return ER_FAILED;
    }

  HEAP_PERF_TRACK_PREPARE (thread_p, context);

  if (is_mvcc_op && !HEAP_IS_UPDATE_INPLACE (context->update_in_place))
    {
      /* 
       * TO BE OPTIMIZED IN THE FUTURE. You can use the disabled function
       * 'heap_mvcc_update_relocated_record()' as a guideline.
       */
      HEAP_OPERATION_CONTEXT delete_context, insert_context;

      /* 
       * Insert new version
       */
      /* set up insertion context */
      context->recdes_p->type = REC_HOME;
      heap_create_insert_context (&insert_context, &context->hfid, &context->class_oid, context->recdes_p, NULL, false);
      COPY_OID (&insert_context.partition_link, &context->partition_link);
      insert_context.flags = context->flags;
      insert_context.header_page_watcher_p = &context->header_page_watcher;
      insert_context.time_track = context->time_track;

      /* insert new version */
      if (heap_insert_newver (thread_p, &insert_context, context->forward_page_watcher_p) != NO_ERROR)
	{
	  return ER_FAILED;
	}

      /* unfix new version's pages */
      heap_unfix_watchers (thread_p, &insert_context);

      /* keep new location */
      assert (!OID_ISNULL (&insert_context.res_oid));
      COPY_OID (&context->res_oid, &insert_context.res_oid);

      /* 
       * Delete old version
       */
      /* set up delete context */
      heap_create_delete_context (&delete_context, &context->hfid, &context->oid, &context->class_oid, NULL);
      delete_context.record_type = context->record_type;	/* skip extra work */
      delete_context.file_type = context->file_type;	/* skip extra work */
      delete_context.home_recdes = context->home_recdes;	/* skip extra work */
      COPY_OID (&delete_context.next_version, &insert_context.res_oid);
      COPY_OID (&delete_context.partition_link, &context->partition_link);
      heap_link_watchers (&delete_context, context);
      delete_context.time_track = context->time_track;

      /* delete old record */
      if (heap_delete_relocation (thread_p, &delete_context, true) != NO_ERROR)
	{
	  return ER_FAILED;
	}
    }
  else
    {
      RECDES new_home_recdes;
      OID new_forward_oid[2];
      bool fits_in_home, fits_in_forward;
      bool update_old_home = false;
      bool update_old_forward = false;
      bool remove_old_forward = false;

      /* fix header if necessary */
      fits_in_home =
	spage_is_updatable (thread_p, context->home_page_watcher_p->pgptr, context->oid.slotid,
			    context->recdes_p->length);
      fits_in_forward =
	spage_is_updatable (thread_p, context->forward_page_watcher_p->pgptr, forward_oid.slotid,
			    context->recdes_p->length);
      if (heap_is_big_length (context->recdes_p->length) || (!fits_in_forward && !fits_in_home))
	{
	  /* fix header page */
	  rc = heap_fix_header_page (thread_p, context);
	  if (rc != NO_ERROR)
	    {
	      return ER_FAILED;
	    }
	}

      /* determine what operations on home/forward pages are necessary and execute extra operations for each case */
      if (heap_is_big_length (context->recdes_p->length))
	{
	  /* insert new overflow record */
	  if (heap_ovf_insert (thread_p, &context->hfid, &new_forward_oid[0], context->recdes_p, NULL) == NULL)
	    {
	      return ER_FAILED;
	    }

	  /* home record descriptor will be an overflow OID and will be placed in original home page */
	  heap_build_forwarding_recdes (&new_home_recdes, REC_BIGONE, new_forward_oid,
					!OID_ISNULL (&context->partition_link));

	  /* remove old forward record */
	  remove_old_forward = true;
	  update_old_home = true;

	  mnt_heap_rel_to_big_updates (thread_p);
	}
      else if (!fits_in_forward && !fits_in_home)
	{
	  /* insert a new forward record */
	  context->recdes_p->type = REC_NEWHOME;
	  rc = heap_insert_newhome (thread_p, context, context->recdes_p, &new_forward_oid[0]);
	  if (rc != NO_ERROR)
	    {
	      return rc;
	    }

	  /* new home record will be a REC_RELOCATION and will be placed in the original home page */
	  heap_build_forwarding_recdes (&new_home_recdes, REC_RELOCATION, new_forward_oid,
					!OID_ISNULL (&context->partition_link));

	  /* remove old forward record */
	  remove_old_forward = true;
	  update_old_home = true;

	  mnt_heap_rel_to_rel_updates (thread_p);
	}
      else if (fits_in_home)
	{
	  /* updated forward record fits in home page */
	  context->recdes_p->type = REC_HOME;
	  new_home_recdes = *context->recdes_p;

	  /* remove old forward record */
	  remove_old_forward = true;
	  update_old_home = true;

	  mnt_heap_rel_to_home_updates (thread_p);
	}
      else if (fits_in_forward)
	{
	  /* updated forward record fits in old forward page */
	  context->recdes_p->type = REC_NEWHOME;

	  /* home record will not be touched */
	  update_old_forward = true;

	  mnt_heap_rel_updates (thread_p);
	}
      else
	{
	  /* impossible case */
	  assert (false);
	  return ER_FAILED;
	}

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      /* 
       * Update old home record (if necessary)
       */
      if (update_old_home)
	{
	  if (context->home_page_watcher_p->page_was_unfixed)
	    {
	      /* 
	       * Need to get the record again, since record may have changed
	       * by other transactions (INSID removed by VACUUM, page compact).
	       * The object was already locked, so the record size may be the
	       * same or smaller (INSID removed by VACUUM). 
	       */
	      int is_peeking = (context->home_recdes.area_size >= context->home_recdes.length) ? COPY : PEEK;
	      if (spage_get_record (context->home_page_watcher_p->pgptr, context->oid.slotid, &context->home_recdes,
				    is_peeking) != S_SUCCESS)
		{
		  return ER_FAILED;
		}
	    }
	  /* log operation */
	  heap_log_update_physical (thread_p, context->home_page_watcher_p->pgptr, &context->hfid.vfid, &context->oid,
				    &context->home_recdes, &new_home_recdes, is_mvcc_op);

	  HEAP_PERF_TRACK_LOGGING (thread_p, context);

	  /* update home record */
	  rc =
	    heap_update_physical (thread_p, context->home_page_watcher_p->pgptr, context->oid.slotid, &new_home_recdes);
	  if (rc != NO_ERROR)
	    {
	      return rc;
	    }

	  HEAP_PERF_TRACK_EXECUTE (thread_p, context);
	}

      if (update_old_forward || remove_old_forward)
	{
	  if (context->forward_page_watcher_p->page_was_unfixed)
	    {
	      if (spage_get_record (context->forward_page_watcher_p->pgptr, forward_oid.slotid, &forward_recdes, COPY)
		  != S_SUCCESS)
		{
		  return ER_FAILED;
		}
	      /* re-peek forward record descriptor; forward page may have been unfixed by previous pgbuf_ordered_fix()
	       * call */
	    }
	}
      /* 
       * Update old forward record (if necessary)
       */
      if (update_old_forward)
	{
	  /* log operation */
	  heap_log_update_physical (thread_p, context->forward_page_watcher_p->pgptr, &context->hfid.vfid, &forward_oid,
				    &forward_recdes, context->recdes_p, is_mvcc_op);

	  HEAP_PERF_TRACK_LOGGING (thread_p, context);

	  /* physical update of forward record */
	  rc =
	    heap_update_physical (thread_p, context->forward_page_watcher_p->pgptr, forward_oid.slotid,
				  context->recdes_p);
	  if (rc != NO_ERROR)
	    {
	      return rc;
	    }

	  HEAP_PERF_TRACK_EXECUTE (thread_p, context);
	}

      /* 
       * Delete old forward record (if necessary)
       */
      if (remove_old_forward)
	{
	  bool mark_reusable = heap_is_reusable_oid (context->file_type);

	  /* operation logging */
	  heap_log_delete_physical (thread_p, context->forward_page_watcher_p->pgptr, &context->hfid.vfid, &forward_oid,
				    &forward_recdes, mark_reusable);

	  HEAP_PERF_TRACK_LOGGING (thread_p, context);

	  /* physical removal of forward record */
	  rc = heap_delete_physical (thread_p, &context->hfid, context->forward_page_watcher_p->pgptr, &forward_oid);
	  if (rc != NO_ERROR)
	    {
	      return rc;
	    }

	  HEAP_PERF_TRACK_EXECUTE (thread_p, context);
	}

      /* location did not change */
      COPY_OID (&context->res_oid, &context->oid);
    }

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_update_home () - update a REC_HOME record
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   is_mvcc_op(in): type of operation (MVCC/non-MVCC)
 */
static int
heap_update_home (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context, bool is_mvcc_op)
{
  int error_code = NO_ERROR;

  assert (context != NULL);
  assert (context->recdes_p != NULL);
  assert (context->type == HEAP_OPERATION_UPDATE);
  assert (context->home_page_watcher_p != NULL);
  assert (context->home_page_watcher_p->pgptr != NULL);
  assert (context->forward_page_watcher_p != NULL);

  if (!HEAP_IS_UPDATE_INPLACE (context->update_in_place) && context->home_recdes.type == REC_ASSIGN_ADDRESS)
    {
      /* updating a REC_ASSIGN_ADDRESS should be done as a non-mvcc operation */
      assert (false);
#if defined(CUBRID_DEBUG)
      er_log_debug (ARG_FILE_LINE,
		    "heap_update_home: ** SYSTEM_ERROR ** update"
		    " mvcc update was attempted on REC_ASSIGN_ADDRESS home record");
#endif
      return ER_FAILED;
    }

  if (is_mvcc_op && !HEAP_IS_UPDATE_INPLACE (context->update_in_place))
    {
      HEAP_OPERATION_CONTEXT delete_context, insert_context;
      MVCCID mvcc_id = logtb_get_current_mvccid (thread_p);

      /* 
       * Insert new version
       */
      /* prepare insert context */
      context->recdes_p->type = REC_HOME;
      heap_create_insert_context (&insert_context, &context->hfid, &context->class_oid, context->recdes_p, NULL, false);
      COPY_OID (&insert_context.partition_link, &context->partition_link);
      insert_context.flags = context->flags;
      insert_context.header_page_watcher_p = &context->header_page_watcher;
      insert_context.time_track = context->time_track;

      /* insert new version */
      error_code = heap_insert_newver (thread_p, &insert_context, context->home_page_watcher_p);
      if (error_code != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  return error_code;
	}

      /* unfix new version's pages */
      heap_unfix_watchers (thread_p, &insert_context);

      /* keep new location */
      assert (!OID_ISNULL (&insert_context.res_oid));
      COPY_OID (&context->res_oid, &insert_context.res_oid);

      /* 
       * Delete old version and link it to new version
       * NOTE: new version OID is kept in insert_context.oid
       */
      /* prepare delete context */
      heap_create_delete_context (&delete_context, &context->hfid, &context->oid, &context->class_oid, NULL);
      delete_context.record_type = context->record_type;	/* skip extra work */
      delete_context.file_type = context->file_type;	/* skip extra work */
      delete_context.home_recdes = context->home_recdes;	/* skip extra work */
      COPY_OID (&delete_context.next_version, &insert_context.res_oid);
      COPY_OID (&delete_context.partition_link, &context->partition_link);
      heap_link_watchers (&delete_context, context);
      delete_context.time_track = context->time_track;

      /* delete old record */
      error_code = heap_delete_home (thread_p, &delete_context, true);
      if (error_code != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  return error_code;
	}
    }
  else
    {
      RECDES forwarding_recdes;
      RECDES *home_page_updated_recdes_p = NULL;
      OID forward_oid[2];

      if (heap_is_big_length (context->recdes_p->length))
	{
	  /* fix header page */
	  error_code = heap_fix_header_page (thread_p, context);
	  if (error_code != NO_ERROR)
	    {
	      ASSERT_ERROR ();
	      return error_code;
	    }

	  /* insert new overflow record */
	  if (heap_ovf_insert (thread_p, &context->hfid, &forward_oid[0], context->recdes_p, NULL) == NULL)
	    {
	      ASSERT_ERROR_AND_SET (error_code);
	      return error_code;
	    }

	  /* forwarding record is REC_BIGONE */
	  heap_build_forwarding_recdes (&forwarding_recdes, REC_BIGONE, forward_oid,
					!OID_ISNULL (&context->partition_link));

	  /* we'll be updating home with forwarding record */
	  home_page_updated_recdes_p = &forwarding_recdes;

	  mnt_heap_home_to_big_updates (thread_p);
	}
      else if (!spage_is_updatable (thread_p, context->home_page_watcher_p->pgptr, context->oid.slotid,
				    context->recdes_p->length))
	{
	  /* fix header page */
	  error_code = heap_fix_header_page (thread_p, context);
	  if (error_code != NO_ERROR)
	    {
	      ASSERT_ERROR ();
	      return error_code;
	    }

	  /* insert new home record */
	  context->recdes_p->type = REC_NEWHOME;
	  error_code = heap_insert_newhome (thread_p, context, context->recdes_p, &forward_oid[0]);
	  if (error_code != NO_ERROR)
	    {
	      ASSERT_ERROR ();
	      return error_code;
	    }

	  /* forwarding record is REC_RELOCATION */
	  heap_build_forwarding_recdes (&forwarding_recdes, REC_RELOCATION, forward_oid,
					!OID_ISNULL (&context->partition_link));

	  /* we'll be updating home with forwarding record */
	  home_page_updated_recdes_p = &forwarding_recdes;

	  mnt_heap_home_to_rel_updates (thread_p);
	}
      else
	{
	  context->recdes_p->type = REC_HOME;

	  /* updated record fits in home page */
	  home_page_updated_recdes_p = context->recdes_p;

	  mnt_heap_home_updates (thread_p);
	}

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      if ((home_page_updated_recdes_p->type == REC_RELOCATION || home_page_updated_recdes_p->type == REC_BIGONE)
	  && context->home_page_watcher_p->page_was_unfixed)
	{
	  /* 
	   * Need to get the record again, since record may have changed
	   * by other transactions (INSID removed by VACUUM, page compact).
	   * The object was already locked, so the record size may be the
	   * same or smaller (INSID removed by VACUUM). 
	   */
	  int is_peeking = (context->home_recdes.area_size >= context->home_recdes.length) ? COPY : PEEK;
	  if (spage_get_record (context->home_page_watcher_p->pgptr, context->oid.slotid, &context->home_recdes,
				is_peeking) != S_SUCCESS)
	    {
	      assert (false);
	      return ER_FAILED;
	    }
	}

      /* log operation */
      heap_log_update_physical (thread_p, context->home_page_watcher_p->pgptr, &context->hfid.vfid, &context->oid,
				&context->home_recdes, home_page_updated_recdes_p, is_mvcc_op);

      HEAP_PERF_TRACK_LOGGING (thread_p, context);

      /* physical update of home record */
      error_code =
	heap_update_physical (thread_p, context->home_page_watcher_p->pgptr, context->oid.slotid,
			      home_page_updated_recdes_p);
      if (error_code != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  return error_code;
	}

      HEAP_PERF_TRACK_EXECUTE (thread_p, context);

      /* location did not change */
      COPY_OID (&context->res_oid, &context->oid);
    }

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_update_physical () - physically update a record
 *   thread_p(in): thread entry
 *   page_p(in): page where record is stored
 *   slot_id(in): slot where record is stored within page
 *   recdes_p(in): record descriptor of updated record
 *   returns: error code or NO_ERROR
 */
static int
heap_update_physical (THREAD_ENTRY * thread_p, PAGE_PTR page_p, short slot_id, RECDES * recdes_p)
{
  SCAN_CODE scancode;
  INT16 old_record_type;
  int error_code = NO_ERROR;

  /* check input */
  assert (page_p != NULL);
  assert (recdes_p != NULL);
  assert (slot_id != NULL_SLOTID);

  /* retrieve current record type */
  old_record_type = spage_get_record_type (page_p, slot_id);

  /* update home page and check operation result */
  scancode = spage_update (thread_p, page_p, slot_id, recdes_p);
  if (scancode != SP_SUCCESS)
    {
      /* 
       * This is likely a system error since we have already checked
       * for space.
       */
      assert (false);
      if (scancode != SP_ERROR)
	{
	  er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
	}

#if defined(CUBRID_DEBUG)
      er_log_debug (ARG_FILE_LINE,
		    "heap_update_physical: ** SYSTEM_ERROR ** update operation failed even when have already checked"
		    " for space");
#endif

      return ER_FAILED;
    }

  /* Reflect record type change */
  if (old_record_type != recdes_p->type)
    {
      spage_update_record_type (thread_p, page_p, slot_id, recdes_p->type);
    }

  /* mark as dirty */
  pgbuf_set_dirty (thread_p, page_p, DONT_FREE);

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_log_update_physical () - log a physical update
 *   thread_p(in): thread entry
 *   page_p(in): updated page
 *   vfid_p(in): virtual file id
 *   oid_p(in): object id
 *   old_recdes_p(in): old record
 *   new_recdes_p(in): new record
 *   log_rule(in): type of logging (MVCC/non-MVCC)
 */
static void
heap_log_update_physical (THREAD_ENTRY * thread_p, PAGE_PTR page_p, VFID * vfid_p, OID * oid_p, RECDES * old_recdes_p,
			  RECDES * new_recdes_p, bool is_mvcc_op)
{
  LOG_DATA_ADDR address;

  /* build address */
  address.offset = oid_p->slotid;
  address.pgptr = page_p;
  address.vfid = vfid_p;

  /* actual logging */
  if (is_mvcc_op && old_recdes_p->type == REC_ASSIGN_ADDRESS)
    {
      /* This physical update of record in home page. In most cases, vacuum doesn't need to be notified of this update
       * (since it was already notified when object was first inserted). One exception is when REC_ASSIGN_ADDRESS
       * content is updated. When REC_ASSIGN_ADDRESS was inserted, vacuum is not notified. The actual insert of record
       * data is done through physical update, but the record still has MVCC information. Update page chain header and
       * notify vacuum of this change. */
      HEAP_PAGE_VACUUM_STATUS vacuum_status = heap_page_get_vacuum_status (thread_p, page_p);
      heap_page_update_chain_after_mvcc_op (thread_p, page_p, logtb_get_current_mvccid (thread_p));
      if (heap_page_get_vacuum_status (thread_p, page_p) != vacuum_status)
	{
	  /* Mark vacuum status change for recovery. */
	  address.offset |= HEAP_RV_FLAG_VACUUM_STATUS_CHANGE;
	}
      log_append_undoredo_recdes (thread_p, RVHF_UPDATE_NOTIFY_VACUUM, &address, old_recdes_p, new_recdes_p);
    }
  else
    {
      log_append_undoredo_recdes (thread_p, RVHF_UPDATE, &address, old_recdes_p, new_recdes_p);
    }
}

/*
 * heap_create_insert_context () - create an insertion context
 *   context(in): context to set up
 *   hfid_p(in): heap file identifier
 *   class_oid_p(in): class OID
 *   recdes_p(in): record descriptor to insert
 *   scancache_p(in): scan cache to use (optional)
 *   bigone_max_size(in): use maximum record size
 *			      (reserve space for an extra OID) for REC_BIGONE
 */
void
heap_create_insert_context (HEAP_OPERATION_CONTEXT * context, HFID * hfid_p, OID * class_oid_p, RECDES * recdes_p,
			    HEAP_SCANCACHE * scancache_p, bool bigone_max_size)
{
  assert (context != NULL);
  assert (hfid_p != NULL);
  assert (recdes_p != NULL);

  heap_clear_operation_context (context, hfid_p);
  if (class_oid_p != NULL)
    {
      COPY_OID (&context->class_oid, class_oid_p);
    }
  context->recdes_p = recdes_p;
  context->scan_cache_p = scancache_p;
  context->type = HEAP_OPERATION_INSERT;
  if (bigone_max_size == true)
    {
      HEAP_OP_CONTEXT_SET_FLAG (context->flags, HEAP_OP_CONTEXT_FLAG_BIGONE_MAXSIZE);
    }
}

/*
 * heap_create_delete_context () - create a deletion context
 *   context(in): context to set up
 *   hfid_p(in): heap file identifier
 *   oid(in): identifier of object to delete
 *   class_oid_p(in): class OID
 *   scancache_p(in): scan cache to use (optional)
 */
void
heap_create_delete_context (HEAP_OPERATION_CONTEXT * context, HFID * hfid_p, OID * oid_p, OID * class_oid_p,
			    HEAP_SCANCACHE * scancache_p)
{
  assert (context != NULL);
  assert (hfid_p != NULL);
  assert (oid_p != NULL);
  assert (class_oid_p != NULL);

  heap_clear_operation_context (context, hfid_p);
  COPY_OID (&context->oid, oid_p);
  COPY_OID (&context->class_oid, class_oid_p);
  context->scan_cache_p = scancache_p;
  context->type = HEAP_OPERATION_DELETE;
}

/*
 * heap_create_update_context () - create an update operation context
 *   context(in): context to set up
 *   hfid_p(in): heap file identifier
 *   oid(in): identifier of object to delete
 *   class_oid_p(in): class OID
 *   recdes_p(in): updated record to write
 *   scancache_p(in): scan cache to use (optional)
 *   in_place(in): specifies if the "in place" type of the update operation
 *   bigone_max_size(in): use maximum record size
 *			      (reserve space for an extra OID) for REC_BIGONE 
 */
void
heap_create_update_context (HEAP_OPERATION_CONTEXT * context, HFID * hfid_p, OID * oid_p, OID * class_oid_p,
			    RECDES * recdes_p, HEAP_SCANCACHE * scancache_p, UPDATE_INPLACE_STYLE in_place,
			    bool bigone_max_size)
{
  assert (context != NULL);
  assert (hfid_p != NULL);
  assert (oid_p != NULL);
  assert (class_oid_p != NULL);
  assert (recdes_p != NULL);

  heap_clear_operation_context (context, hfid_p);
  COPY_OID (&context->oid, oid_p);
  COPY_OID (&context->class_oid, class_oid_p);
  context->recdes_p = recdes_p;
  context->scan_cache_p = scancache_p;
  context->type = HEAP_OPERATION_UPDATE;
  context->update_in_place = in_place;
  if (bigone_max_size == true)
    {
      HEAP_OP_CONTEXT_SET_FLAG (context->flags, HEAP_OP_CONTEXT_FLAG_BIGONE_MAXSIZE);
    }
}

/*
 * heap_insert_logical () - Insert an object onto heap
 *   context(in/out): operation context
 *   return: error code or NO_ERROR
 *
 * Note: Insert an object onto the given file heap. The object is
 * inserted using the following algorithm:
 *              1: If the object cannot be inserted in a single page, it is
 *                 inserted in overflow as a multipage object. An overflow
 *                 relocation record is created in the heap as an address map
 *                 to the actual content of the object (the overflow address).
 *              2: If the object can be inserted in the last allocated page
 *                 without overpassing the reserved space on the page, the
 *                 object is placed on this page.
 *              3: If the object can be inserted in the hinted page without
 *                 overpassing the reserved space on the page, the object is
 *       	   placed on this page.
 *              4: The object is inserted in a newly allocated page. Don't
 *                 about reserve space here.
 *
 * NOTE-1: The class object was already IX-locked during compile time
 *         under normal situation.
 *         However, with prepare-execute-commit-execute-... scenario,
 *         the class object is not properly IX-locked since the previous
 *         commit released the entire acquired locks including IX-lock.
 *         So we have to make it sure the class object is IX-locked at this
 *         moment.
 */
int
heap_insert_logical (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context)
{
  bool is_mvcc_op;
  int rc = NO_ERROR;
  PERF_UTIME_TRACKER time_track;

  /* check required input */
  assert (context != NULL);
  assert (context->type == HEAP_OPERATION_INSERT);
  assert (context->recdes_p != NULL);
  assert (!HFID_IS_NULL (&context->hfid));

  context->time_track = &time_track;
  HEAP_PERF_START (thread_p, context);

  /* check scancache */
  if (heap_scancache_check_with_hfid (thread_p, &context->hfid, &context->class_oid, &context->scan_cache_p) !=
      NO_ERROR)
    {
      return ER_FAILED;
    }

  /* 
   * Determine type of operation
   */
#if defined (SERVER_MODE)
  if (heap_is_mvcc_disabled_for_class (&context->class_oid) || context->recdes_p->type == REC_ASSIGN_ADDRESS)
    {
      is_mvcc_op = false;
    }
  else
    {
      is_mvcc_op = true;
    }
#else /* SERVER_MODE */
  is_mvcc_op = false;
#endif /* SERVER_MODE */

  /* 
   * Record header adjustments
   */
  if (!OID_ISNULL (&context->class_oid) && !OID_IS_ROOTOID (&context->class_oid)
      && context->recdes_p->type != REC_ASSIGN_ADDRESS)
    {
      bool is_mvcc_class = !heap_is_mvcc_disabled_for_class (&context->class_oid);
      if (heap_insert_adjust_recdes_header (thread_p, context, context->recdes_p, is_mvcc_op, is_mvcc_class) !=
	  NO_ERROR)
	{
	  return ER_FAILED;
	}
    }

#if defined(ENABLE_SYSTEMTAP)
  CUBRID_OBJ_INSERT_START (&context->class_oid);
#endif /* ENABLE_SYSTEMTAP */

  /* 
   * Handle multipage object
   */
  if (heap_insert_handle_multipage_record (thread_p, context) != NO_ERROR)
    {
      rc = ER_FAILED;
      goto error;
    }

  /* 
   * Locking
   */
  /* make sure we have IX_LOCK on class see [NOTE-1] */
  if (lock_object (thread_p, &context->class_oid, oid_Root_class_oid, IX_LOCK, LK_UNCOND_LOCK) != LK_GRANTED)
    {
      return ER_FAILED;
    }

  /* get insert location (includes locking) */
  if (heap_get_insert_location_with_lock (thread_p, context, NULL) != NO_ERROR)
    {
      return ER_FAILED;
    }

  HEAP_PERF_TRACK_PREPARE (thread_p, context);

  /* 
   * Physical insertion
   */
  if (heap_insert_physical (thread_p, context) != NO_ERROR)
    {
      rc = ER_FAILED;
      goto error;
    }

  HEAP_PERF_TRACK_EXECUTE (thread_p, context);

  /* 
   * Operation logging
   */
  heap_log_insert_physical (thread_p, context->home_page_watcher_p->pgptr, &context->hfid.vfid, &context->res_oid,
			    context->recdes_p, is_mvcc_op, context->is_redistribute_insert_with_delid);

  HEAP_PERF_TRACK_LOGGING (thread_p, context);

  if (context->update_in_place == UPDATE_INPLACE_OLD_MVCCID)
    {
      MVCC_REC_HEADER mvcc_rec_header;
      MVCCID mvcc_insid, mvcc_delid;
      bool mutex_acquired = false, has_insid = false, has_delid = false;

      or_mvcc_get_header (context->recdes_p, &mvcc_rec_header);
      has_insid = MVCC_IS_FLAG_SET (&mvcc_rec_header, OR_MVCC_FLAG_VALID_INSID);
      has_delid = MVCC_IS_FLAG_SET (&mvcc_rec_header, OR_MVCC_FLAG_VALID_DELID);
      if (has_insid || has_delid)
	{
	  if (has_insid)
	    {
	      mvcc_insid = MVCC_GET_INSID (&mvcc_rec_header);
	      if (MVCC_ID_PRECEDES (log_Gl.hdr.last_block_newest_mvccid, mvcc_insid)
		  || MVCC_ID_PRECEDES (mvcc_insid, log_Gl.hdr.last_block_oldest_mvccid))
		{
		  /* we have a chance to update global newest/oldest */
		  (void) pthread_mutex_lock (&log_Gl.prior_info.prior_lsa_mutex);
		  mutex_acquired = true;
		  /* need to check again, maybe another thread has update last_block_newest_mvccid or
		   * last_block_oldest_mvccid meanwhile */
		  if (MVCC_ID_PRECEDES (log_Gl.hdr.last_block_newest_mvccid, mvcc_insid))
		    {
		      /* A newer MVCCID was found */
		      log_Gl.hdr.last_block_newest_mvccid = mvcc_insid;
		    }
		  else if (MVCC_ID_PRECEDES (mvcc_insid, log_Gl.hdr.last_block_oldest_mvccid))
		    {
		      /* An older MVCCID was found */
		      log_Gl.hdr.last_block_oldest_mvccid = mvcc_insid;
		    }
		}
	    }

	  if (has_delid)
	    {
	      mvcc_delid = MVCC_GET_DELID (&mvcc_rec_header);
	      if (MVCC_ID_PRECEDES (log_Gl.hdr.last_block_newest_mvccid, mvcc_delid)
		  || MVCC_ID_PRECEDES (mvcc_delid, log_Gl.hdr.last_block_oldest_mvccid))
		{
		  if (mutex_acquired == false)
		    {
		      (void) pthread_mutex_lock (&log_Gl.prior_info.prior_lsa_mutex);
		      mutex_acquired = true;
		    }
		  if (MVCC_ID_PRECEDES (log_Gl.hdr.last_block_newest_mvccid, mvcc_delid))
		    {
		      /* A newer MVCCID was found */
		      log_Gl.hdr.last_block_newest_mvccid = mvcc_delid;
		    }
		  else if (MVCC_ID_PRECEDES (mvcc_delid, log_Gl.hdr.last_block_oldest_mvccid))
		    {
		      /* An older MVCCID was found */
		      log_Gl.hdr.last_block_oldest_mvccid = mvcc_delid;
		    }
		}
	    }

	  if (mutex_acquired == true)
	    {
	      pthread_mutex_unlock (&log_Gl.prior_info.prior_lsa_mutex);
	    }
	}
    }

  /* mark insert page as dirty */
  pgbuf_set_dirty (thread_p, context->home_page_watcher_p->pgptr, DONT_FREE);

  /* 
   * Page unfix or caching
   */
  if (context->scan_cache_p != NULL && context->home_page_watcher_p == &context->home_page_watcher
      && context->scan_cache_p->cache_last_fix_page == true)
    {
      /* cache */
      assert (context->home_page_watcher_p->pgptr != NULL);
      pgbuf_replace_watcher (thread_p, context->home_page_watcher_p, &context->scan_cache_p->page_watcher);
    }
  else
    {
      /* unfix */
      pgbuf_ordered_unfix (thread_p, context->home_page_watcher_p);
    }

  /* unfix other pages */
  heap_unfix_watchers (thread_p, context);

  /* 
   * Class creation case
   */
  if (context->recdes_p->type != REC_ASSIGN_ADDRESS && HFID_EQ ((&context->hfid), &(heap_Classrepr->rootclass_hfid)))
    {
      if (heap_mark_class_as_modified (thread_p, &context->res_oid, or_chn (context->recdes_p), false) != NO_ERROR)
	{
	  rc = ER_FAILED;
	  goto error;
	}
    }

  if (context->recdes_p->type == REC_HOME)
    {
      mnt_heap_home_inserts (thread_p);
    }
  else if (context->recdes_p->type == REC_BIGONE)
    {
      mnt_heap_big_inserts (thread_p);
    }
  else
    {
      mnt_heap_assign_inserts (thread_p);
    }

error:

#if defined(ENABLE_SYSTEMTAP)
  CUBRID_OBJ_INSERT_END (&context->class_oid, (rc < 0));
#endif /* ENABLE_SYSTEMTAP */

  /* all ok */
  return rc;
}

/*
 * heap_delete_logical () - Delete an object from heap file
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   return: error code or NO_ERROR
 *
 * Note: Delete the object associated with the given OID from the given
 * heap file. If the object has been relocated or stored in
 * overflow, both the relocation and the relocated record are deleted.
 */
int
heap_delete_logical (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context)
{
  bool is_mvcc_op;
  int rc = NO_ERROR;
  PERF_UTIME_TRACKER time_track;

  /* 
   * Check input
   */
  assert (context != NULL);
  assert (context->type == HEAP_OPERATION_DELETE);
  assert (!HFID_IS_NULL (&context->hfid));
  assert (!OID_ISNULL (&context->oid));

  context->time_track = &time_track;
  HEAP_PERF_START (thread_p, context);

  /* check input OID validity */
  if (heap_is_valid_oid (&context->oid) != NO_ERROR)
    {
      return ER_FAILED;
    }

  /* check scancache */
  if (heap_scancache_check_with_hfid (thread_p, &context->hfid, &context->class_oid, &context->scan_cache_p) !=
      NO_ERROR)
    {
      return ER_FAILED;
    }

  /* check file type */
  context->file_type = heap_get_file_type (thread_p, context);
  if (context->file_type != FILE_HEAP && context->file_type != FILE_HEAP_REUSE_SLOTS)
    {
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      return ER_FAILED;
    }

  /* 
   * Class deletion case
   */
  if (HFID_EQ (&context->hfid, &(heap_Classrepr->rootclass_hfid)))
    {
      if (heap_mark_class_as_modified (thread_p, &context->oid, NULL_CHN, true) != NO_ERROR)
	{
	  return ER_FAILED;
	}
    }

  /* 
   * Determine type of operation
   */
#if defined (SERVER_MODE)
  if (heap_is_mvcc_disabled_for_class (&context->class_oid))
    {
      is_mvcc_op = false;
    }
  else
    {
      is_mvcc_op = true;
    }
#else /* SERVER_MODE */
  is_mvcc_op = false;
#endif /* SERVER_MODE */

#if defined(ENABLE_SYSTEMTAP)
  CUBRID_OBJ_DELETE_START (&context->class_oid);
#endif /* ENABLE_SYSTEMTAP */

  /* 
   * Fetch object's page and check record type
   */
  if (heap_get_record_location (thread_p, context) != NO_ERROR)
    {
      rc = ER_FAILED;
      goto error;
    }

  context->record_type = spage_get_record_type (context->home_page_watcher_p->pgptr, context->oid.slotid);
  if (context->record_type == REC_UNKNOWN)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, context->oid.volid, context->oid.pageid,
	      context->oid.slotid);
      rc = ER_FAILED;
      goto error;
    }

  /* fetch record to be deleted */
  context->home_recdes.area_size = DB_PAGESIZE;
  context->home_recdes.data = PTR_ALIGN (context->home_recdes_buffer, MAX_ALIGNMENT);
  if (spage_get_record (context->home_page_watcher_p->pgptr, context->oid.slotid, &context->home_recdes, COPY) !=
      S_SUCCESS)
    {
      rc = ER_FAILED;
      goto error;
    }

  HEAP_PERF_TRACK_PREPARE (thread_p, context);

  /* 
   * Physical deletion and logging
   */
  switch (context->record_type)
    {
    case REC_BIGONE:
      rc = heap_delete_bigone (thread_p, context, is_mvcc_op);
      break;

    case REC_RELOCATION:
      rc = heap_delete_relocation (thread_p, context, is_mvcc_op);
      break;

    case REC_HOME:
    case REC_ASSIGN_ADDRESS:
      rc = heap_delete_home (thread_p, context, is_mvcc_op);
      break;

    default:
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_BAD_OBJECT_TYPE, 3, context->oid.volid, context->oid.pageid,
	      context->oid.slotid);
      rc = ER_FAILED;
      goto error;
    }

  if (!OID_ISNULL (&context->partition_link))
    {
      if (heap_get_partition_link_from_cache (thread_p, &context->class_oid) != NO_ERROR)
	{
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_BAD_OBJECT_TYPE, 3, context->oid.volid, context->oid.pageid,
		  context->oid.slotid);
	  rc = ER_FAILED;
	  goto error;
	}
    }

error:

  /* unfix or keep home page */
  if (context->scan_cache_p != NULL && context->home_page_watcher_p == &context->home_page_watcher
      && context->scan_cache_p->cache_last_fix_page == true)
    {
      pgbuf_replace_watcher (thread_p, context->home_page_watcher_p, &context->scan_cache_p->page_watcher);
    }
  else
    {
      if (context->home_page_watcher_p->pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, context->home_page_watcher_p);
	}
    }

  /* unfix pages */
  heap_unfix_watchers (thread_p, context);

#if defined(ENABLE_SYSTEMTAP)
  CUBRID_OBJ_DELETE_END (&context->class_oid, (rc != NO_ERROR));
#endif /* ENABLE_SYSTEMTAP */

  return rc;
}

/*
 * heap_partition_link_entry_alloc() - allocate a new structure for
 *		  the partition link cache
 *   returns: new pointer or NULL on error
 */
static void *
heap_partition_link_entry_alloc (void)
{
  HEAP_PARTITION_LINK_CACHE_ENTRY *new_entry =
    (HEAP_PARTITION_LINK_CACHE_ENTRY *) malloc (sizeof (HEAP_PARTITION_LINK_CACHE_ENTRY));
  if (new_entry == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, sizeof (HEAP_PARTITION_LINK_CACHE_ENTRY));
      return NULL;
    }

  pthread_mutex_init (&new_entry->mutex, NULL);

  return (void *) new_entry;
}

/*
 * heap_partition_link_entry_free () - free a partition link cache
 *   returns: error code or NO_ERROR
 *   entry(in): entry to free (HEAP_HFID_TABLE_ENTRY)
 */
static int
heap_partition_link_entry_free (void *entry)
{
  if (entry != NULL)
    {
      pthread_mutex_destroy (&((HEAP_PARTITION_LINK_CACHE_ENTRY *) entry)->mutex);
      free (entry);
      return NO_ERROR;
    }
  else
    {
      return ER_FAILED;
    }
}

/*
 * heap_partition_link_entry_init () - initialize a partition link cache entry
 *   returns: error code or NO_ERROR
 *   entry(in): partition link cache entry
 */
static int
heap_partition_link_entry_init (void *entry)
{
  HEAP_PARTITION_LINK_CACHE_ENTRY *entry_p = (HEAP_PARTITION_LINK_CACHE_ENTRY *) entry;

  if (entry_p == NULL)
    {
      return ER_FAILED;
    }

  /* initialize fields */
  OID_SET_NULL (&entry_p->class_oid);
  entry_p->partition_link_flag = 0;

  return NO_ERROR;
}

/*
 * heap_partition_link_entry_key_copy () - copy a partition link key
 *   returns: error code or NO_ERROR
 *   src(in): source
 *   dest(in): destination
 */
static int
heap_partition_link_entry_key_copy (void *src, void *dest)
{
  if (src == NULL || dest == NULL)
    {
      return ER_FAILED;
    }

  COPY_OID ((OID *) dest, (OID *) src);

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_partition_link_entry_key_hash () - hashing function for the
 *				    partition link hash table
 *   return: int
 *   key(in): Session key
 *   hash_table_size(in): Memory Hash Table Size
 *
 * Note: Generate a hash number for the given key for the given hash table
 *	 size.
 */
static unsigned int
heap_partition_link_entry_key_hash (void *key, int hash_table_size)
{
  return ((int) OID_PSEUDO_KEY ((OID *) key)) % hash_table_size;
}

/*
 * heap_partition_link_entry_key_compare () - Compare two global unique
 *					     statistics keys (OIDs)
 *   return: int (true or false)
 *   k1  (in) : First OID key
 *   k2 (in) : Second OID key
 */
static int
heap_partition_link_entry_key_compare (void *k1, void *k2)
{
  OID *key1, *key2;

  key1 = (OID *) k1;
  key2 = (OID *) k2;

  if (k1 == NULL || k2 == NULL)
    {
      /* should not happen */
      assert (false);
      return 0;
    }

  if (OID_EQ (key1, key2))
    {
      /* equal */
      return 0;
    }
  else
    {
      /* not equal */
      return 1;
    }
}

/*
 * heap_partition_link_cache_initialize () - Creates and initializes global
 *                            structure for global partition link hash table
 *   return: error code
 *   thread_p  (in) : 
 */
int
heap_partition_link_cache_initialize (void)
{
  int ret = NO_ERROR;
  LF_ENTRY_DESCRIPTOR *edesc = NULL;

  if (heap_Partition_link_cache != NULL)
    {
      return NO_ERROR;
    }

  edesc = &heap_Partition_link_cache_area.partition_link_descriptor;

  edesc->of_local_next = offsetof (HEAP_PARTITION_LINK_CACHE_ENTRY, stack);
  edesc->of_next = offsetof (HEAP_PARTITION_LINK_CACHE_ENTRY, next);
  edesc->of_del_tran_id = offsetof (HEAP_PARTITION_LINK_CACHE_ENTRY, del_id);
  edesc->of_key = offsetof (HEAP_PARTITION_LINK_CACHE_ENTRY, class_oid);
  edesc->of_mutex = offsetof (HEAP_PARTITION_LINK_CACHE_ENTRY, mutex);
  edesc->mutex_flags = LF_EM_FLAG_LOCK_ON_FIND | LF_EM_FLAG_LOCK_ON_DELETE | LF_EM_FLAG_UNLOCK_AFTER_DELETE;
  edesc->f_alloc = heap_partition_link_entry_alloc;
  edesc->f_free = heap_partition_link_entry_free;
  edesc->f_init = heap_partition_link_entry_init;
  edesc->f_uninit = NULL;
  edesc->f_key_copy = heap_partition_link_entry_key_copy;
  edesc->f_key_cmp = heap_partition_link_entry_key_compare;
  edesc->f_hash = heap_partition_link_entry_key_hash;
  edesc->f_duplicate = NULL;

  /* initialize freelist */
  ret = lf_freelist_init (&heap_Partition_link_cache_area.partition_link_free_list, 1, 100, edesc, &partition_link_Ts);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  /* initialize hash table */
  ret =
    lf_hash_init (&heap_Partition_link_cache_area.partition_link_hash,
		  &heap_Partition_link_cache_area.partition_link_free_list, HEAP_PARTITION_LINK_HASH_SIZE, edesc);
  if (ret != NO_ERROR)
    {
      lf_hash_destroy (&heap_Partition_link_cache_area.partition_link_hash);
      return ret;
    }

  heap_Partition_link_cache = &heap_Partition_link_cache_area;

  return ret;
}

/*
 * heap_partition_link_cache_finalize () - Finalize partition link hash table
 *   return: error code
 *   thread_p  (in) : 
 */
void
heap_partition_link_cache_finalize (void)
{
  if (heap_Partition_link_cache != NULL)
    {
      /* destroy hash and freelist */
      lf_hash_destroy (&heap_Partition_link_cache->partition_link_hash);
      lf_freelist_destroy (&heap_Partition_link_cache->partition_link_free_list);

      heap_Partition_link_cache = NULL;
    }
}

/*
 * heap_delete_from_partition_link_cache () - deletes the entry associated with
 *					the given class OID from the partition
 *					link hash
 *   return: error code
 *   thread_p  (in) :
 *   class_oid (in) : the class OID for which the entry will be deleted
 */
static int
heap_delete_from_partition_link_cache (THREAD_ENTRY * thread_p, const OID * class_oid)
{
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_PARTITION_LINK_HASH);
  int error = NO_ERROR;

  error = lf_hash_delete (t_entry, &heap_Partition_link_cache->partition_link_hash, (void *) class_oid, NULL);

  return error;
}

/*
 * heap_get_partition_link_from_cache() - returns the partition link flag of the
 *			      class with the given class OID. If the entry does
 *			      not exist, it will be inserted and the flag is
 *			      updated on disk
 *   return: error code
 *   thread_p  (in) :
 *   class OID (in) : the class OID for which the entry will be returned 
 */
static int
heap_get_partition_link_from_cache (THREAD_ENTRY * thread_p, OID * class_oid)
{
  int error_code = NO_ERROR;
  HFID hfid;
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_PARTITION_LINK_HASH);
  HEAP_PARTITION_LINK_CACHE_ENTRY *entry = NULL;

  assert (class_oid != NULL);

  error_code =
    lf_hash_find_or_insert (t_entry, &heap_Partition_link_cache->partition_link_hash, class_oid, (void **) &entry);

  if (error_code != NO_ERROR)
    {
      return error_code;
    }
  assert (entry != NULL);

  if (entry->partition_link_flag == 0)
    {
      /* entry has been inserted, we need to update the flag */
      error_code = heap_get_hfid_from_class_oid (thread_p, class_oid, &hfid);
      if (error_code != NO_ERROR)
	{
	  return error_code;
	}

      /* mark that this partition has at least one partition flag */
      error_code = heap_update_partition_link_flag (thread_p, &hfid, 1);
      if (error_code != NO_ERROR)
	{
	  return error_code;
	}
      entry->partition_link_flag = 1;
    }
  pthread_mutex_unlock (&entry->mutex);

  return NO_ERROR;
}

/*
 * heap_update_partition_link_flag () - Update the partition link flag for a
 *				      given HFID. This persistent flag marks if
 *				      the class has at least one partition link
 *
 *   return: error code
 *   hfid(in): Object heap file identifier
 *   set_flag(in): value of the flag (0 or 1)
 *
 */
static int
heap_update_partition_link_flag (THREAD_ENTRY * thread_p, HFID * hfid, int set_flag)
{
  VPID vpid;			/* Volume and page identifiers */
  LOG_DATA_ADDR addr_hdr;	/* Address of logging data */
  RECDES hdr_recdes;		/* Record descriptor to point to space statistics */
  HEAP_HDR_STATS *heap_hdr;	/* Heap header */
  PGBUF_WATCHER hdr_page_watcher;
  int save_partition_link_flag;

  PGBUF_INIT_WATCHER (&hdr_page_watcher, PGBUF_ORDERED_HEAP_HDR, hfid);

  /* 
   * Get the heap header in exclusive mode since it is going to be changed.
   *
   * Note: to avoid any possibilities of deadlocks, I should not have any locks
   *       on the heap at this moment.
   *       That is, we must assume that locking the header of the heap in
   *       exclusive mode, the rest of the heap is locked.
   */

  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;

  addr_hdr.vfid = &hfid->vfid;
  addr_hdr.offset = HEAP_HEADER_AND_CHAIN_SLOTID;

  if (log_start_system_op (thread_p) == NULL)
    {
      return ER_FAILED;
    }

  if (pgbuf_ordered_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_WRITE, &hdr_page_watcher) != NO_ERROR)
    {
      /* something went wrong. Unable to fetch header page */
      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);
      return ER_FAILED;
    }

  (void) pgbuf_check_page_ptype (thread_p, hdr_page_watcher.pgptr, PAGE_HEAP);

  if (spage_get_record (hdr_page_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes, PEEK) != S_SUCCESS)
    {
      pgbuf_ordered_unfix (thread_p, &hdr_page_watcher);
      log_end_system_op (thread_p, LOG_RESULT_TOPOP_ABORT);
      return ER_FAILED;
    }

  heap_hdr = (HEAP_HDR_STATS *) hdr_recdes.data;
  addr_hdr.pgptr = hdr_page_watcher.pgptr;

  /* update the partition link flag in the header page */
  save_partition_link_flag = heap_hdr->has_partition_link;
  heap_hdr->has_partition_link = set_flag;

  log_append_undoredo_data (thread_p, RVHF_PARTITION_LINK_FLAG, &addr_hdr, sizeof (int), sizeof (int),
			    &save_partition_link_flag, &set_flag);
  pgbuf_ordered_set_dirty_and_free (thread_p, &hdr_page_watcher);
  log_end_system_op (thread_p, LOG_RESULT_TOPOP_COMMIT);

  return NO_ERROR;
}

/*
 * heap_get_partition_link_flag () - Get the partition link flag for a
 *				      given HFID. This persistent flag marks if
 *				      the class has at least one partition link
 *
 *   return: error code
 *   hfid(in): Object heap file identifier
 *   partition_link(out): value of the flag (0 or 1)
 *
 */
static int
heap_get_partition_link_flag (THREAD_ENTRY * thread_p, HFID * hfid, int *partition_link)
{
  VPID vpid;			/* Volume and page identifiers */
  RECDES hdr_recdes;		/* Record descriptor to point to space statistics */
  HEAP_HDR_STATS *heap_hdr;	/* Heap header */
  PGBUF_WATCHER hdr_page_watcher;

  assert (partition_link != NULL);

  *partition_link = 0;
  PGBUF_INIT_WATCHER (&hdr_page_watcher, PGBUF_ORDERED_HEAP_HDR, hfid);

  vpid.volid = hfid->vfid.volid;
  vpid.pageid = hfid->hpgid;

  if (pgbuf_ordered_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_READ, &hdr_page_watcher) != NO_ERROR)
    {
      /* something went wrong. Unable to fetch header page */
      return ER_FAILED;
    }

  (void) pgbuf_check_page_ptype (thread_p, hdr_page_watcher.pgptr, PAGE_HEAP);

  if (spage_get_record (hdr_page_watcher.pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes, PEEK) != S_SUCCESS)
    {
      pgbuf_ordered_unfix (thread_p, &hdr_page_watcher);
      return ER_FAILED;
    }

  heap_hdr = (HEAP_HDR_STATS *) hdr_recdes.data;

  /* read the partition link flag in the header page */
  *partition_link = heap_hdr->has_partition_link;

  pgbuf_ordered_unfix (thread_p, &hdr_page_watcher);

  return NO_ERROR;
}

/*
 * heap_rv_undoredo_partition_link_flag () - Undo/Redo the set of a partition
 *					    link flag.
 *   return: int
 *   rcv(in): Recovery structure
 */
int
heap_rv_undoredo_partition_link_flag (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  int offset = 0;
  int set_flag;
  RECDES hdr_recdes;
  HEAP_HDR_STATS *heap_hdr = NULL;

  assert (rcv->length == OR_INT_SIZE);

  set_flag = OR_GET_INT (rcv->data + offset);
  offset += OR_INT_SIZE;

  assert (offset == rcv->length);

  if (spage_get_record (rcv->pgptr, HEAP_HEADER_AND_CHAIN_SLOTID, &hdr_recdes, PEEK) != S_SUCCESS)
    {
      return ER_FAILED;
    }
  heap_hdr = (HEAP_HDR_STATS *) hdr_recdes.data;
  heap_hdr->has_partition_link = set_flag;

  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
  return NO_ERROR;
}

/*
 * heap_update_logical () - update a record in a heap file
 *   thread_p(in): thread entry
 *   context(in): operation context
 *   return: error code or NO_ERROR
 */
extern int
heap_update_logical (THREAD_ENTRY * thread_p, HEAP_OPERATION_CONTEXT * context)
{
  bool is_mvcc_op;
  int rc = NO_ERROR;
  PERF_UTIME_TRACKER time_track;

  /* 
   * Check input
   */
  assert (context != NULL);
  assert (context->type == HEAP_OPERATION_UPDATE);
  assert (!OID_ISNULL (&context->oid));
  assert (!OID_ISNULL (&context->class_oid));

  context->time_track = &time_track;
  HEAP_PERF_START (thread_p, context);

  /* check scancache */
  rc = heap_scancache_check_with_hfid (thread_p, &context->hfid, &context->class_oid, &context->scan_cache_p);
  if (rc != NO_ERROR)
    {
      ASSERT_ERROR ();
      return rc;
    }

  /* check file type */
  context->file_type = heap_get_file_type (thread_p, context);
  if (context->file_type != FILE_HEAP && context->file_type != FILE_HEAP_REUSE_SLOTS)
    {
      er_set (ER_FATAL_ERROR_SEVERITY, ARG_FILE_LINE, ER_GENERIC_ERROR, 0);
      return ER_GENERIC_ERROR;
    }

  /* get heap file identifier from scancache if none was provided */
  if (HFID_IS_NULL (&context->hfid))
    {
      if (context->scan_cache_p != NULL)
	{
	  HFID_COPY (&context->hfid, &context->scan_cache_p->node.hfid);
	}
      else
	{
	  er_log_debug (ARG_FILE_LINE, "heap_update: Bad interface a heap is needed");
	  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_HEAP, 3, "", NULL_FILEID, NULL_PAGEID);
	  assert (false);
	  return ER_HEAP_UNKNOWN_HEAP;
	}
    }

  /* check provided object identifier */
  rc = heap_is_valid_oid (&context->oid);
  if (rc != NO_ERROR)
    {
      ASSERT_ERROR ();
      return rc;
    }

  /* by default, consider it old */
  context->is_logical_old = true;

  /* 
   * Determine type of operation
   */
#if defined (SERVER_MODE)
  if (heap_is_mvcc_disabled_for_class (&context->class_oid))
    {
      is_mvcc_op = false;
    }
  else
    {
      is_mvcc_op = true;
    }
#else /* SERVER_MODE */
  is_mvcc_op = false;
#endif /* SERVER_MODE */

#if defined(ENABLE_SYSTEMTAP)
  CUBRID_OBJ_UPDATE_START (&context->class_oid);
#endif /* ENABLE_SYSTEMTAP */

  /* 
   * Get location
   */
  rc = heap_get_record_location (thread_p, context);
  if (rc != NO_ERROR)
    {
      ASSERT_ERROR ();
      goto exit;
    }

  /* 
   * Adjust new record header
   */
  if (!OID_ISNULL (&context->class_oid) && !OID_IS_ROOTOID (&context->class_oid))
    {
      bool is_mvcc_class = !heap_is_mvcc_disabled_for_class (&context->class_oid);
      rc = heap_insert_adjust_recdes_header (thread_p, context, context->recdes_p, is_mvcc_op, is_mvcc_class);
      if (rc != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  goto exit;
	}
    }

  /* decache guessed representation */
  HEAP_MAYNEED_DECACHE_GUESSED_LASTREPRS (&context->oid, &context->hfid);

  /* 
   * Fetch record
   */
  context->record_type = spage_get_record_type (context->home_page_watcher_p->pgptr, context->oid.slotid);
  if (context->record_type == REC_UNKNOWN)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_UNKNOWN_OBJECT, 3, context->oid.volid, context->oid.pageid,
	      context->oid.slotid);
      rc = ER_HEAP_UNKNOWN_OBJECT;
      goto exit;
    }

  context->home_recdes.area_size = DB_PAGESIZE;
  context->home_recdes.data = PTR_ALIGN (context->home_recdes_buffer, MAX_ALIGNMENT);
  if (spage_get_record (context->home_page_watcher_p->pgptr, context->oid.slotid, &context->home_recdes, COPY) !=
      S_SUCCESS)
    {
      rc = ER_FAILED;
      goto exit;
    }

  HEAP_PERF_TRACK_PREPARE (thread_p, context);

  /* 
   * Update record
   */
  switch (context->record_type)
    {
    case REC_RELOCATION:
      rc = heap_update_relocation (thread_p, context, is_mvcc_op);
      break;

    case REC_BIGONE:
      rc = heap_update_bigone (thread_p, context, is_mvcc_op);
      break;

    case REC_ASSIGN_ADDRESS:
      /* it's not an old record, it was inserted in this transaction */
      context->is_logical_old = false;
      /* fall trough */
    case REC_HOME:
      rc = heap_update_home (thread_p, context, is_mvcc_op);
      break;

    default:
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_HEAP_BAD_OBJECT_TYPE, 3, context->oid.volid, context->oid.pageid,
	      context->oid.slotid);
      rc = ER_HEAP_BAD_OBJECT_TYPE;
      goto exit;
    }

  /* check return code of operation */
  if (rc != NO_ERROR)
    {
      ASSERT_ERROR ();
      goto exit;
    }

  /* 
   * Class update case
   */
  if (HFID_EQ ((&context->hfid), &(heap_Classrepr->rootclass_hfid)))
    {
      rc = heap_mark_class_as_modified (thread_p, &context->oid, or_chn (context->recdes_p), false);
      if (rc != NO_ERROR)
	{
	  ASSERT_ERROR ();
	  goto exit;
	}
    }

exit:

  /* unfix or cache home page */
  if (context->home_page_watcher_p->pgptr != NULL && context->home_page_watcher_p == &context->home_page_watcher)
    {
      if (context->scan_cache_p != NULL && context->scan_cache_p->cache_last_fix_page)
	{
	  pgbuf_replace_watcher (thread_p, context->home_page_watcher_p, &context->scan_cache_p->page_watcher);
	}
      else
	{
	  pgbuf_ordered_unfix (thread_p, context->home_page_watcher_p);
	}
    }

  /* unfix pages */
  heap_unfix_watchers (thread_p, context);

#if defined(ENABLE_SYSTEMTAP)
  CUBRID_OBJ_UPDATE_END (&context->class_oid, (rc != NO_ERROR));
#endif /* ENABLE_SYSTEMTAP */

  return rc;
}

/*
 * heap_get_hfid_from_class_record () - get HFID from class record for the
 *				      given OID.
 *   return: error_code
 *   class_oid(in): class oid
 *   hfid(out):  the resulting hfid
 */
static int
heap_get_hfid_from_class_record (THREAD_ENTRY * thread_p, const OID * class_oid, HFID * hfid)
{
  int error_code = NO_ERROR;
  RECDES recdes;
  HEAP_SCANCACHE scan_cache;

  if (class_oid == NULL || hfid == NULL)
    {
      return ER_FAILED;
    }

  (void) heap_scancache_quick_start_root_hfid (thread_p, &scan_cache);

  if (heap_get (thread_p, class_oid, &recdes, &scan_cache, PEEK, NULL_CHN) != S_SUCCESS)
    {
      heap_scancache_end (thread_p, &scan_cache);
      return ER_FAILED;
    }

  or_class_hfid (&recdes, hfid);

  error_code = heap_scancache_end (thread_p, &scan_cache);
  if (error_code != NO_ERROR)
    {
      return error_code;
    }

  return error_code;
}

/*
 * heap_hfid_table_entry_alloc() - allocate a new structure for
 *		  the class OID->HFID hash
 *   returns: new pointer or NULL on error
 */
static void *
heap_hfid_table_entry_alloc (void)
{
  HEAP_HFID_TABLE_ENTRY *new_entry = (HEAP_HFID_TABLE_ENTRY *) malloc (sizeof (HEAP_HFID_TABLE_ENTRY));
  return (void *) new_entry;
}

/*
 * logtb_global_unique_stat_free () - free a hfid_table entry
 *   returns: error code or NO_ERROR
 *   entry(in): entry to free (HEAP_HFID_TABLE_ENTRY)
 */
static int
heap_hfid_table_entry_free (void *entry)
{
  if (entry != NULL)
    {
      free (entry);
      return NO_ERROR;
    }
  else
    {
      return ER_FAILED;
    }
}

/*
 * heap_hfid_table_entry_init () - initialize a hfid_table entry
 *   returns: error code or NO_ERROR
 *   entry(in): hfid_table entry
 */
static int
heap_hfid_table_entry_init (void *entry)
{
  HEAP_HFID_TABLE_ENTRY *entry_p = (HEAP_HFID_TABLE_ENTRY *) entry;

  if (entry_p == NULL)
    {
      return ER_FAILED;
    }

  /* initialize fields */
  OID_SET_NULL (&entry_p->class_oid);
  entry_p->hfid.vfid.fileid = NULL_FILEID;
  entry_p->hfid.vfid.volid = NULL_VOLID;
  entry_p->hfid.hpgid = NULL_PAGEID;

  return NO_ERROR;
}

/*
 * heap_hfid_table_entry_key_copy () - copy a hfid_table key
 *   returns: error code or NO_ERROR
 *   src(in): source
 *   dest(in): destination
 */
static int
heap_hfid_table_entry_key_copy (void *src, void *dest)
{
  if (src == NULL || dest == NULL)
    {
      return ER_FAILED;
    }

  COPY_OID ((OID *) dest, (OID *) src);

  /* all ok */
  return NO_ERROR;
}

/*
 * heap_hfid_table_entry_key_hash () - hashing function for the class OID->HFID
 *				    hash table
 *   return: int
 *   key(in): Session key
 *   hash_table_size(in): Memory Hash Table Size
 *
 * Note: Generate a hash number for the given key for the given hash table
 *	 size.
 */
static unsigned int
heap_hfid_table_entry_key_hash (void *key, int hash_table_size)
{
  return ((int) OID_PSEUDO_KEY ((OID *) key)) % hash_table_size;
}

/*
 * heap_hfid_table_entry_key_compare () - Compare two global unique
 *					     statistics keys (OIDs)
 *   return: int (true or false)
 *   k1  (in) : First OID key
 *   k2 (in) : Second OID key
 */
static int
heap_hfid_table_entry_key_compare (void *k1, void *k2)
{
  OID *key1, *key2;

  key1 = (OID *) k1;
  key2 = (OID *) k2;

  if (k1 == NULL || k2 == NULL)
    {
      /* should not happen */
      assert (false);
      return 0;
    }

  if (OID_EQ (key1, key2))
    {
      /* equal */
      return 0;
    }
  else
    {
      /* not equal */
      return 1;
    }
}

/*
 * heap_initialize_hfid_table () - Creates and initializes global structure
 *				    for global class OID->HFID hash table
 *   return: error code
 *   thread_p  (in) : 
 */
int
heap_initialize_hfid_table (void)
{
  int ret = NO_ERROR;
  LF_ENTRY_DESCRIPTOR *edesc = NULL;

  if (heap_Hfid_table != NULL)
    {
      return NO_ERROR;
    }

  edesc = &heap_Hfid_table_area.hfid_hash_descriptor;

  edesc->of_local_next = offsetof (HEAP_HFID_TABLE_ENTRY, stack);
  edesc->of_next = offsetof (HEAP_HFID_TABLE_ENTRY, next);
  edesc->of_del_tran_id = offsetof (HEAP_HFID_TABLE_ENTRY, del_id);
  edesc->of_key = offsetof (HEAP_HFID_TABLE_ENTRY, class_oid);
  edesc->of_mutex = 0;
  edesc->mutex_flags = LF_EM_NOT_USING_MUTEX;
  edesc->f_alloc = heap_hfid_table_entry_alloc;
  edesc->f_free = heap_hfid_table_entry_free;
  edesc->f_init = heap_hfid_table_entry_init;
  edesc->f_uninit = NULL;
  edesc->f_key_copy = heap_hfid_table_entry_key_copy;
  edesc->f_key_cmp = heap_hfid_table_entry_key_compare;
  edesc->f_hash = heap_hfid_table_entry_key_hash;
  edesc->f_duplicate = NULL;

  /* initialize freelist */
  ret = lf_freelist_init (&heap_Hfid_table_area.hfid_hash_freelist, 1, 100, edesc, &hfid_table_Ts);
  if (ret != NO_ERROR)
    {
      return ret;
    }

  /* initialize hash table */
  ret =
    lf_hash_init (&heap_Hfid_table_area.hfid_hash, &heap_Hfid_table_area.hfid_hash_freelist, HEAP_HFID_HASH_SIZE,
		  edesc);
  if (ret != NO_ERROR)
    {
      lf_hash_destroy (&heap_Hfid_table_area.hfid_hash);
      return ret;
    }

  heap_Hfid_table = &heap_Hfid_table_area;

  return ret;
}

/*
 * heap_finalize_hfid_table () - Finalize class OID->HFID hash table
 *   return: error code
 *   thread_p  (in) : 
 */
void
heap_finalize_hfid_table (void)
{
  if (heap_Hfid_table != NULL)
    {
      /* destroy hash and freelist */
      lf_hash_destroy (&heap_Hfid_table->hfid_hash);
      lf_freelist_destroy (&heap_Hfid_table->hfid_hash_freelist);

      heap_Hfid_table = NULL;
    }
}

/*
 * heap_delete_hfid_from_cache () - deletes the entry associated with
 *					the given class OID from the hfid table
 *   return: error code
 *   thread_p  (in) :
 *   class_oid (in) : the class OID for which the entry will be deleted
 */
int
heap_delete_hfid_from_cache (THREAD_ENTRY * thread_p, OID * class_oid)
{
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_HFID_TABLE);
  int error = NO_ERROR;

  error = lf_hash_delete (t_entry, &heap_Hfid_table->hfid_hash, class_oid, NULL);
  if (error != NO_ERROR)
    {
      return error;
    }

  return NO_ERROR;
}

/*
 * heap_vacuum_all_objects () - Vacuum all objects in heap.
 *
 * return		 : Error code.
 * thread_p (in)	 : Thread entry.
 * upd_scancache(in)	 : Update scan cache
 * threshold_mvccid(in)  : Threshold MVCCID
 */
int
heap_vacuum_all_objects (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * upd_scancache, MVCCID threshold_mvccid)
{
  PGBUF_WATCHER pg_watcher;
  PGBUF_WATCHER old_pg_watcher;
  VPID next_vpid, vpid;
  VACUUM_WORKER worker;
  int max_num_slots, i;
  OID temp_oid;
  bool reusable;
  int error_code = NO_ERROR;

  assert (upd_scancache != NULL);
  PGBUF_INIT_WATCHER (&pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, &upd_scancache->node.hfid);
  PGBUF_INIT_WATCHER (&old_pg_watcher, PGBUF_ORDERED_HEAP_NORMAL, &upd_scancache->node.hfid);
  memset (&worker, 0, sizeof (worker));
  max_num_slots = IO_MAX_PAGE_SIZE / sizeof (SPAGE_SLOT);
  worker.heap_objects = (VACUUM_HEAP_OBJECT *) malloc (max_num_slots * sizeof (VACUUM_HEAP_OBJECT));
  if (worker.heap_objects == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
	      max_num_slots * sizeof (VACUUM_HEAP_OBJECT));
      error_code = ER_OUT_OF_VIRTUAL_MEMORY;
      goto exit;
    }
  worker.heap_objects_capacity = max_num_slots;
  worker.n_heap_objects = 0;

  next_vpid.volid = upd_scancache->node.hfid.vfid.volid;
  next_vpid.pageid = upd_scancache->node.hfid.hpgid;
  for (i = 0; i < max_num_slots; i++)
    {
      VFID_COPY (&worker.heap_objects[i].vfid, &upd_scancache->node.hfid.vfid);
    }

  reusable = heap_is_reusable_oid (upd_scancache->file_type);
  while (!VPID_ISNULL (&next_vpid))
    {
      vpid = next_vpid;
      error_code = pgbuf_ordered_fix (thread_p, &vpid, OLD_PAGE, PGBUF_LATCH_WRITE, &pg_watcher);
      if (error_code != NO_ERROR)
	{
	  goto exit;
	}

      (void) pgbuf_check_page_ptype (thread_p, pg_watcher.pgptr, PAGE_HEAP);

      if (old_pg_watcher.pgptr != NULL)
	{
	  pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
	}

      error_code = heap_vpid_next (&upd_scancache->node.hfid, pg_watcher.pgptr, &next_vpid);
      if (error_code != NO_ERROR)
	{
	  assert (false);
	  goto exit;
	}

      temp_oid.volid = vpid.volid;
      temp_oid.pageid = vpid.pageid;
      worker.n_heap_objects = spage_number_of_slots (pg_watcher.pgptr) - 1;
      if (worker.n_heap_objects > 0)
	{
	  for (i = 1; i <= worker.n_heap_objects; i++)
	    {
	      temp_oid.slotid = i;
	      COPY_OID (&worker.heap_objects[i - 1].oid, &temp_oid);
	    }

	  error_code =
	    vacuum_heap_page (thread_p, worker.heap_objects, worker.n_heap_objects, threshold_mvccid, reusable, false);
	  if (error_code != NO_ERROR)
	    {
	      goto exit;
	    }
	}

      pgbuf_replace_watcher (thread_p, &pg_watcher, &old_pg_watcher);
    }

exit:
  if (pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &pg_watcher);
    }
  if (old_pg_watcher.pgptr != NULL)
    {
      pgbuf_ordered_unfix (thread_p, &old_pg_watcher);
    }

  if (worker.heap_objects != NULL)
    {
      free_and_init (worker.heap_objects);
    }
  return error_code;
}

/*
 * heap_insert_hfid_for_class_oid () - Cache HFID for class object.
 *
 * return	  : Error code.
 * thread_p (in)  : Thread entry.
 * class_oid (in) : Class OID.
 * hfid (in)	  : Heap file ID.
 */
static int
heap_insert_hfid_for_class_oid (THREAD_ENTRY * thread_p, const OID * class_oid, HFID * hfid)
{
  int error_code = NO_ERROR;
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_HFID_TABLE);
  HEAP_HFID_TABLE_ENTRY *entry = NULL;

  assert (hfid != NULL && !HFID_IS_NULL (hfid));

  if (class_oid == NULL || OID_ISNULL (class_oid))
    {
      /* We can't cache it. */
      return NO_ERROR;
    }

  error_code = lf_hash_find_or_insert (t_entry, &heap_Hfid_table->hfid_hash, (void *) class_oid, (void **) &entry);
  if (error_code != NO_ERROR)
    {
      return error_code;
    }
  assert (entry != NULL);
  assert (entry->hfid.hpgid == NULL_PAGEID);

  HFID_COPY (&entry->hfid, hfid);
  (void) lf_tran_end (t_entry);

  /* Successfully cached. */
  return NO_ERROR;
}

/*
 * heap_get_hfid_from_cache () - returns the HFID of the
 *			      class with the given class OID
 *   return: error code
 *   thread_p  (in) :
 *   class OID (in) : the class OID for which the entry will be returned
 *   hfid      (out): 
 *
 *   Note: if the entry is not found, one will be inserted and the HFID is
 *	retrieved from the class record.
 */
static int
heap_get_hfid_from_cache (THREAD_ENTRY * thread_p, const OID * class_oid, HFID * hfid)
{
  int error_code = NO_ERROR;
  LF_TRAN_ENTRY *t_entry = thread_get_tran_entry (thread_p, THREAD_TS_HFID_TABLE);
  HEAP_HFID_TABLE_ENTRY *entry = NULL;

  assert (class_oid != NULL);

  error_code = lf_hash_find_or_insert (t_entry, &heap_Hfid_table->hfid_hash, (void *) class_oid, (void **) &entry);

  if (error_code != NO_ERROR)
    {
      return error_code;
    }
  assert (entry != NULL);

  if (entry->hfid.hpgid == NULL_PAGEID || entry->hfid.vfid.fileid == NULL_FILEID
      || entry->hfid.vfid.volid == NULL_VOLID)
    {
      /* this is either a newly inserted entry or one with incomplete information that is currently being filled by
       * another transaction. We need to retrieve the HFID from the class record. We do not care that we are
       * overwriting the information, since it must be always the same (the HFID never changes for the same class OID). */
      error_code = heap_get_hfid_from_class_record (thread_p, class_oid, &entry->hfid);
    }

  if (error_code == NO_ERROR)
    {
      HFID_COPY (hfid, &entry->hfid);
    }
  else
    {
      HFID_SET_NULL (hfid);
    }

  (void) lf_tran_end (t_entry);
  return error_code;
}

/*
 * heap_page_update_chain_after_mvcc_op () - Update max MVCCID and vacuum
 *					     status in heap page chain after
 *					     an MVCC op is executed.
 *
 * return	  : Void.
 * thread_p (in)  : Thread entry.
 * heap_page (in) : Heap page.
 * mvccid (in)	  : MVCC op MVCCID.
 */
static void
heap_page_update_chain_after_mvcc_op (THREAD_ENTRY * thread_p, PAGE_PTR heap_page, MVCCID mvccid)
{
  HEAP_CHAIN *chain;
  RECDES chain_recdes;
  HEAP_PAGE_VACUUM_STATUS vacuum_status;

  assert (heap_page != NULL);
  assert (MVCCID_IS_NORMAL (mvccid));

  /* Two actions are being done here: 1. Update vacuum status.  - HEAP_PAGE_VACUUM_NONE + 1 mvcc op =>
   * HEAP_PAGE_VACUUM_ONCE - HEAP_PAGE_VACUUM_ONCE + 1 mvcc op => HEAP_PAGE_VACUUM_UNKNOWN (because future becomes
   * unpredictable).  - HEAP_PAGE_VACUUM_UNKNOWN + 1 mvcc op can we tell that page is vacuumed? =>
   * HEAP_PAGE_VACUUM_ONCE we don't know that page is vacuumed? => HEAP_PAGE_VACUUM_UNKNOWN 2. Update max MVCCID if
   * new MVCCID is bigger. */

  /* Get heap chain. */
  if (spage_get_record (heap_page, HEAP_HEADER_AND_CHAIN_SLOTID, &chain_recdes, PEEK) != S_SUCCESS)
    {
      assert_release (false);
      return;
    }
  if (chain_recdes.length != sizeof (HEAP_CHAIN))
    {
      /* Heap header page. Do nothing. */
      assert (chain_recdes.length == sizeof (HEAP_HDR_STATS));
      return;
    }
  chain = (HEAP_CHAIN *) chain_recdes.data;

  /* Update vacuum status. */
  vacuum_status = HEAP_PAGE_GET_VACUUM_STATUS (chain);
  switch (vacuum_status)
    {
    case HEAP_PAGE_VACUUM_NONE:
      /* Change status to one vacuum. */
      assert (MVCC_ID_PRECEDES (chain->max_mvccid, mvccid));
      HEAP_PAGE_SET_VACUUM_STATUS (chain, HEAP_PAGE_VACUUM_ONCE);
      vacuum_er_log (VACUUM_ER_LOG_HEAP,
		     "VACUUM: Changed vacuum status for page %d|%d, lsa=%lld|%d from no vacuum to vacuum once.\n",
		     pgbuf_get_volume_id (heap_page), pgbuf_get_page_id (heap_page),
		     (long long int) pgbuf_get_lsa (heap_page)->pageid, (int) pgbuf_get_lsa (heap_page)->offset);
      break;

    case HEAP_PAGE_VACUUM_ONCE:
      /* Change status to unknown number of vacuums. */
      HEAP_PAGE_SET_VACUUM_STATUS (chain, HEAP_PAGE_VACUUM_UNKNOWN);
      vacuum_er_log (VACUUM_ER_LOG_HEAP,
		     "VACUUM: Changed vacuum status for page %d|%d, lsa=%lld|%d from vacuum once to unknown.\n",
		     pgbuf_get_volume_id (heap_page), pgbuf_get_page_id (heap_page),
		     (long long int) pgbuf_get_lsa (heap_page)->pageid, (int) pgbuf_get_lsa (heap_page)->offset);
      break;

    case HEAP_PAGE_VACUUM_UNKNOWN:
      /* Was page completely vacuumed? We can tell if current max_mvccid precedes vacuum data's oldest mvccid. */
      if (vacuum_is_mvccid_vacuumed (chain->max_mvccid))
	{
	  /* Now page must be vacuumed once, due to new MVCC op. */
	  HEAP_PAGE_SET_VACUUM_STATUS (chain, HEAP_PAGE_VACUUM_ONCE);
	  vacuum_er_log (VACUUM_ER_LOG_HEAP,
			 "VACUUM: Changed vacuum status for page %d|%d, lsa=%lld|%d from unknown to vacuum once.\n ",
			 pgbuf_get_volume_id (heap_page), pgbuf_get_page_id (heap_page),
			 (long long int) pgbuf_get_lsa (heap_page)->pageid, (int) pgbuf_get_lsa (heap_page)->offset);
	}
      else
	{
	  /* Status remains the same. Number of vacuums needed still cannot be predicted. */
	  vacuum_er_log (VACUUM_ER_LOG_HEAP, "VACUUM: Vacuum status for page %d|%d, %lld|%d remains unknown.\n ",
			 pgbuf_get_volume_id (heap_page), pgbuf_get_page_id (heap_page),
			 (long long int) pgbuf_get_lsa (heap_page)->pageid, (int) pgbuf_get_lsa (heap_page)->offset);
	}
      break;
    default:
      assert_release (false);
      break;
    }

  /* Update max_mvccid. */
  if (MVCC_ID_PRECEDES (chain->max_mvccid, mvccid))
    {
      vacuum_er_log (VACUUM_ER_LOG_HEAP, "VACUUM: Update max MVCCID for page %d|%d from %llu to %llu.\n",
		     pgbuf_get_volume_id (heap_page), pgbuf_get_page_id (heap_page),
		     (unsigned long long int) chain->max_mvccid, (unsigned long long int) mvccid);
      chain->max_mvccid = mvccid;
    }
}

/*
 * heap_page_rv_vacuum_status_change () - Applies vacuum status change for
 *					  recovery.
 *
 * return	  : Void.
 * thread_p (in)  : Thread entry.
 * heap_page (in) : Heap page.
 */
static void
heap_page_rv_chain_update (THREAD_ENTRY * thread_p, PAGE_PTR heap_page, MVCCID mvccid, bool vacuum_status_change)
{
  HEAP_CHAIN *chain;
  RECDES chain_recdes;
  HEAP_PAGE_VACUUM_STATUS vacuum_status;

  assert (heap_page != NULL);

  /* Possible transitions (see heap_page_update_chain_after_mvcc_op): - HEAP_PAGE_VACUUM_NONE => HEAP_PAGE_VACUUM_ONCE.
   * - HEAP_PAGE_VACUUM_ONCE => HEAP_PAGE_VACUUM_UNKNOWN. - HEAP_PAGE_VACUUM_UNKNOWN => HEAP_PAGE_VACUUM_ONCE. */

  /* Get heap chain. */
  if (spage_get_record (heap_page, HEAP_HEADER_AND_CHAIN_SLOTID, &chain_recdes, PEEK) != S_SUCCESS)
    {
      assert_release (false);
      return;
    }
  if (chain_recdes.length != sizeof (HEAP_CHAIN))
    {
      /* Header page. Don't change chain. */
      return;
    }
  chain = (HEAP_CHAIN *) chain_recdes.data;

  if (vacuum_status_change)
    {
      /* Change status. */
      vacuum_status = HEAP_PAGE_GET_VACUUM_STATUS (chain);
      switch (vacuum_status)
	{
	case HEAP_PAGE_VACUUM_NONE:
	case HEAP_PAGE_VACUUM_UNKNOWN:
	  HEAP_PAGE_SET_VACUUM_STATUS (chain, HEAP_PAGE_VACUUM_ONCE);

	  vacuum_er_log (VACUUM_ER_LOG_HEAP | VACUUM_ER_LOG_RECOVERY,
			 "VACUUM: Change heap page %d|%d, lsa=%lld|%d, status from %s to once.\n",
			 pgbuf_get_volume_id (heap_page), pgbuf_get_page_id (heap_page),
			 (long long int) pgbuf_get_lsa (heap_page)->pageid, (int) pgbuf_get_lsa (heap_page)->offset,
			 vacuum_status == HEAP_PAGE_VACUUM_NONE ? "none" : "unknown");
	  break;
	case HEAP_PAGE_VACUUM_ONCE:
	  HEAP_PAGE_SET_VACUUM_STATUS (chain, HEAP_PAGE_VACUUM_UNKNOWN);

	  vacuum_er_log (VACUUM_ER_LOG_HEAP | VACUUM_ER_LOG_RECOVERY,
			 "VACUUM: Change heap page %d|%d, lsa=%lld|%d, status from once to unknown.\n",
			 pgbuf_get_volume_id (heap_page), pgbuf_get_page_id (heap_page),
			 (long long int) pgbuf_get_lsa (heap_page)->pageid, (int) pgbuf_get_lsa (heap_page)->offset);
	  break;
	}
    }
  if (MVCC_ID_PRECEDES (chain->max_mvccid, mvccid))
    {
      chain->max_mvccid = mvccid;
    }
}

/*
 * heap_page_set_vacuum_status_none () - Change vacuum status from one vacuum
 *					 required to none.
 *
 * return	  : Void.
 * thread_p (in)  : Thread entry.
 * heap_page (in) : Heap page.
 */
void
heap_page_set_vacuum_status_none (THREAD_ENTRY * thread_p, PAGE_PTR heap_page)
{
  HEAP_CHAIN *chain;
  RECDES chain_recdes;

  assert (heap_page != NULL);

  /* Updating vacuum status: - HEAP_PAGE_VACUUM_NONE => Vacuum is not expected. Fail. - HEAP_PAGE_VACUUM_ONCE + 1
   * vacuum => HEAP_PAGE_VACUUM_NONE. - HEAP_PAGE_VACUUM_UNKNOWN + 1 vacuum => HEAP_PAGE_VACUUM_UNKNOWN.  Number of
   * vacuums expected is unknown and remains that way. */

  /* Get heap chain. */
  if (spage_get_record (heap_page, HEAP_HEADER_AND_CHAIN_SLOTID, &chain_recdes, PEEK) != S_SUCCESS)
    {
      assert_release (false);
      return;
    }
  if (chain_recdes.length != sizeof (HEAP_CHAIN))
    {
      /* Heap header page. */
      /* Should never be here. */
      assert_release (false);
      return;
    }
  chain = (HEAP_CHAIN *) chain_recdes.data;

  assert (HEAP_PAGE_GET_VACUUM_STATUS (chain) == HEAP_PAGE_VACUUM_ONCE);

  /* Update vacuum status. */
  HEAP_PAGE_SET_VACUUM_STATUS (chain, HEAP_PAGE_VACUUM_NONE);

  vacuum_er_log (VACUUM_ER_LOG_HEAP, "VACUUM: Changed vacuum status for page %d|%d from vacuum once to no vacuum.\n",
		 pgbuf_get_volume_id (heap_page), pgbuf_get_page_id (heap_page));
}

/*
 * heap_page_get_max_mvccid () - Get max MVCCID of heap page.
 *
 * return	  : Max MVCCID.
 * thread_p (in)  : Thread entry.
 * heap_page (in) : Heap page.
 */
MVCCID
heap_page_get_max_mvccid (THREAD_ENTRY * thread_p, PAGE_PTR heap_page)
{
  HEAP_CHAIN *chain;
  RECDES chain_recdes;

  assert (heap_page != NULL);

  /* Get heap chain. */
  if (spage_get_record (heap_page, HEAP_HEADER_AND_CHAIN_SLOTID, &chain_recdes, PEEK) != S_SUCCESS)
    {
      assert_release (false);
      return MVCCID_NULL;
    }
  if (chain_recdes.length != sizeof (HEAP_CHAIN))
    {
      /* Heap header page. */
      assert (chain_recdes.length == sizeof (HEAP_HDR_STATS));
      return MVCCID_NULL;
    }
  chain = (HEAP_CHAIN *) chain_recdes.data;

  return chain->max_mvccid;
}

/*
 * heap_page_get_vacuum_status () - Get heap page vacuum status.
 *
 * return	  : Vacuum status.
 * thread_p (in)  : Thread entry.
 * heap_page (in) : Heap page.
 */
HEAP_PAGE_VACUUM_STATUS
heap_page_get_vacuum_status (THREAD_ENTRY * thread_p, PAGE_PTR heap_page)
{
  HEAP_CHAIN *chain;
  RECDES chain_recdes;

  assert (heap_page != NULL);

  /* Get heap chain. */
  if (spage_get_record (heap_page, HEAP_HEADER_AND_CHAIN_SLOTID, &chain_recdes, PEEK) != S_SUCCESS)
    {
      assert_release (false);
      return HEAP_PAGE_VACUUM_UNKNOWN;
    }
  if (chain_recdes.length != sizeof (HEAP_CHAIN))
    {
      /* Heap header page. */
      assert (chain_recdes.length == sizeof (HEAP_HDR_STATS));
      return HEAP_PAGE_VACUUM_UNKNOWN;
    }
  chain = (HEAP_CHAIN *) chain_recdes.data;

  return HEAP_PAGE_GET_VACUUM_STATUS (chain);
}

/*
 * heap_rv_nop () - Heap recovery no op function.
 *
 * return	 : NO_ERROR.
 * thread_p (in) : Thread entry.
 * rcv (in)	 : Recovery data.
 */
int
heap_rv_nop (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  assert (rcv->pgptr != NULL);
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
  return NO_ERROR;
}

/*
 * heap_rv_update_chain_after_mvcc_op () - Redo update of page chain after
 *					   an MVCC operation (used for
 *					   operations that are not changing
 *					   
 *
 * return	 : NO_ERROR
 * thread_p (in) : Thread entry.
 * rcv (in)	 : Recovery data.
 */
int
heap_rv_update_chain_after_mvcc_op (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  bool vacuum_status_change = false;

  assert (rcv->pgptr != NULL);
  assert (MVCCID_IS_NORMAL (rcv->mvcc_id));

  vacuum_status_change = (rcv->offset & HEAP_RV_FLAG_VACUUM_STATUS_CHANGE) != 0;
  heap_page_rv_chain_update (thread_p, rcv->pgptr, rcv->mvcc_id, vacuum_status_change);
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);
  return NO_ERROR;
}

/*
 * heap_rv_remove_flags_from_offset () - Remove flags from recovery offset.
 *
 * return	 : Offset without flags.
 * offset (in)	 : Offset with flags.
 */
INT16
heap_rv_remove_flags_from_offset (INT16 offset)
{
  return offset & (~HEAP_RV_FLAG_VACUUM_STATUS_CHANGE);
}

/*
 * heap_should_try_update_stat () - checks if an heap update statistics is
 *				    indicated
 *					   
 *
 * return	 : NO_ERROR
 * thread_p (in) : Thread entry.
 * rcv (in)	 : Recovery data.
 */
bool
heap_should_try_update_stat (const int current_freespace, const int prev_freespace)
{
  if (current_freespace > prev_freespace && current_freespace > HEAP_DROP_FREE_SPACE
      && prev_freespace < HEAP_DROP_FREE_SPACE)
    {
      return true;
    }
  return false;
}

/*
 * heap_scancache_add_partition_node () - add a new partition information to
 *				      to the scan_cache's partition list.
 *				      Also sets the current node of the
 *				      scancache to this newly inserted node.
 * 
 * return		: error code
 * thread_p (in)	:
 * scan_cache (in)	:
 * partition_oid (in)   :
 */
static int
heap_scancache_add_partition_node (THREAD_ENTRY * thread_p, HEAP_SCANCACHE * scan_cache, OID * partition_oid)
{
  HFID hfid;
  HEAP_SCANCACHE_NODE_LIST *new_ = NULL;

  assert (scan_cache != NULL);

  if (heap_get_hfid_from_class_oid (thread_p, partition_oid, &hfid) != NO_ERROR)
    {
      return ER_FAILED;
    }

  new_ = (HEAP_SCANCACHE_NODE_LIST *) db_private_alloc (thread_p, sizeof (HEAP_SCANCACHE_NODE_LIST));
  if (new_ == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1, sizeof (HEAP_SCANCACHE_NODE_LIST));
      return ER_OUT_OF_VIRTUAL_MEMORY;
    }

  COPY_OID (&new_->node.class_oid, partition_oid);
  HFID_COPY (&new_->node.hfid, &hfid);
  if (scan_cache->partition_list == NULL)
    {
      new_->next = NULL;
      scan_cache->partition_list = new_;
    }
  else
    {
      new_->next = scan_cache->partition_list;
      scan_cache->partition_list = new_;
    }

  /* set the new node as the current node */
  HEAP_SCANCACHE_SET_NODE (scan_cache, partition_oid, &hfid);

  return NO_ERROR;
}

/*
 * heap_mvcc_log_redistribute () - Log partition redistribute data
 *
 * return	 : Void.
 * thread_p (in) : Thread entry.
 * p_recdes (in) : Newly inserted record.
 * p_addr (in)	 : Log address data.
 */
static void
heap_mvcc_log_redistribute (THREAD_ENTRY * thread_p, RECDES * p_recdes, LOG_DATA_ADDR * p_addr)
{
#define HEAP_LOG_MVCC_REDISTRIBUTE_MAX_REDO_CRUMBS	    4

  int n_redo_crumbs = 0, data_copy_offset = 0;
  LOG_CRUMB redo_crumbs[HEAP_LOG_MVCC_REDISTRIBUTE_MAX_REDO_CRUMBS];
  MVCCID delid;
  MVCC_REC_HEADER mvcc_rec_header;
  HEAP_PAGE_VACUUM_STATUS vacuum_status;

  assert (p_recdes != NULL);
  assert (p_addr != NULL);

  vacuum_status = heap_page_get_vacuum_status (thread_p, p_addr->pgptr);

  /* Update chain. */
  heap_page_update_chain_after_mvcc_op (thread_p, p_addr->pgptr, logtb_get_current_mvccid (thread_p));
  if (vacuum_status != heap_page_get_vacuum_status (thread_p, p_addr->pgptr))
    {
      /* Mark status change for recovery. */
      p_addr->offset |= HEAP_RV_FLAG_VACUUM_STATUS_CHANGE;
    }

  /* Build redo crumbs */
  /* Add record type */
  redo_crumbs[n_redo_crumbs].length = sizeof (p_recdes->type);
  redo_crumbs[n_redo_crumbs++].data = &p_recdes->type;

  if (p_recdes->type != REC_BIGONE)
    {
      or_mvcc_get_header (p_recdes, &mvcc_rec_header);
      assert (MVCC_IS_FLAG_SET (&mvcc_rec_header, OR_MVCC_FLAG_VALID_DELID));
      assert (!MVCC_IS_FLAG_SET (&mvcc_rec_header, OR_MVCC_FLAG_VALID_NEXT_VERSION)
	      && !MVCC_IS_FLAG_SET (&mvcc_rec_header, OR_MVCC_FLAG_VALID_PARTITION_OID));

      /* Add representation ID and flags field */
      redo_crumbs[n_redo_crumbs].length = OR_INT_SIZE;
      redo_crumbs[n_redo_crumbs++].data = p_recdes->data;

      redo_crumbs[n_redo_crumbs].length = OR_MVCCID_SIZE;
      redo_crumbs[n_redo_crumbs++].data = &delid;

      /* Set data copy offset after the record header */
      data_copy_offset = OR_HEADER_SIZE (p_recdes->data);
    }

  /* Add record data - record may be skipped if the record is not big one */
  redo_crumbs[n_redo_crumbs].length = p_recdes->length - data_copy_offset;
  redo_crumbs[n_redo_crumbs++].data = p_recdes->data + data_copy_offset;

  /* Safe guard */
  assert (n_redo_crumbs <= HEAP_LOG_MVCC_REDISTRIBUTE_MAX_REDO_CRUMBS);

  /* Append redo crumbs; undo crumbs not necessary as the spage_delete physical operation uses the offset field of the
   * address */
  log_append_undoredo_crumbs (thread_p, RVHF_MVCC_REDISTRIBUTE, p_addr, 0, n_redo_crumbs, NULL, redo_crumbs);
}

/*
 * heap_rv_mvcc_redo_redistribute () - Redo the MVCC redistribute partition data
 *   return: int
 *   rcv(in): Recovery structure
 *
 */
int
heap_rv_mvcc_redo_redistribute (THREAD_ENTRY * thread_p, LOG_RCV * rcv)
{
  INT16 slotid;
  RECDES recdes;
  int sp_success;
  MVCCID delid;
  MVCC_REC_HEADER mvcc_rec_header;
  INT16 record_type;
  bool vacuum_status_change = false;

  assert (rcv->pgptr != NULL);

  slotid = rcv->offset;
  if (slotid & HEAP_RV_FLAG_VACUUM_STATUS_CHANGE)
    {
      vacuum_status_change = true;
    }
  slotid = slotid & (~HEAP_RV_FLAG_VACUUM_STATUS_CHANGE);
  assert (slotid > 0);

  record_type = *(INT16 *) rcv->data;
  if (record_type == REC_BIGONE)
    {
      /* no data header */
      HEAP_SET_RECORD (&recdes, rcv->length - sizeof (record_type), rcv->length - sizeof (record_type), REC_BIGONE,
		       rcv->data + sizeof (record_type));
    }
  else
    {
      char data_buffer[IO_DEFAULT_PAGE_SIZE + OR_MVCC_MAX_HEADER_SIZE + MAX_ALIGNMENT];
      int repid_and_flags, offset, mvcc_flag, offset_size;

      offset = sizeof (record_type);

      repid_and_flags = OR_GET_INT (rcv->data + offset);
      offset += OR_INT_SIZE;

      OR_GET_MVCCID (rcv->data + offset, &delid);
      offset += OR_MVCCID_SIZE;

      mvcc_flag = (char) ((repid_and_flags >> OR_MVCC_FLAG_SHIFT_BITS) & OR_MVCC_FLAG_MASK);

      assert (!(mvcc_flag & (OR_MVCC_FLAG_VALID_LONG_CHN | OR_MVCC_FLAG_VALID_NEXT_VERSION)));

      if ((repid_and_flags & OR_OFFSET_SIZE_FLAG) == OR_OFFSET_SIZE_1BYTE)
	{
	  offset_size = OR_BYTE_SIZE;
	}
      else if ((repid_and_flags & OR_OFFSET_SIZE_FLAG) == OR_OFFSET_SIZE_2BYTE)
	{
	  offset_size = OR_SHORT_SIZE;
	}
      else
	{
	  offset_size = OR_INT_SIZE;
	}

      MVCC_SET_REPID (&mvcc_rec_header, repid_and_flags & OR_MVCC_REPID_MASK);
      MVCC_SET_FLAG (&mvcc_rec_header, mvcc_flag);
      MVCC_SET_INSID (&mvcc_rec_header, rcv->mvcc_id);
      MVCC_SET_DELID (&mvcc_rec_header, delid);

      HEAP_SET_RECORD (&recdes, IO_DEFAULT_PAGE_SIZE + OR_MVCC_MAX_HEADER_SIZE, 0, record_type,
		       PTR_ALIGN (data_buffer, MAX_ALIGNMENT));
      or_mvcc_add_header (&recdes, &mvcc_rec_header, repid_and_flags & OR_BOUND_BIT_FLAG, offset_size);

      memcpy (recdes.data + recdes.length, rcv->data + offset, rcv->length - offset);
      recdes.length += (rcv->length - offset);
    }

  sp_success = spage_insert_for_recovery (thread_p, rcv->pgptr, slotid, &recdes);

  if (sp_success != SP_SUCCESS)
    {
      /* Unable to redo insertion */
      assert_release (false);
      return ER_FAILED;
    }

  heap_page_rv_chain_update (thread_p, rcv->pgptr, rcv->mvcc_id, vacuum_status_change);
  pgbuf_set_dirty (thread_p, rcv->pgptr, DONT_FREE);

  return NO_ERROR;
}
