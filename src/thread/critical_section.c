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
 * critical_section.c - critical section support
 */

#ident "$Id$"

#include "config.h"

#include <stdio.h>
#include <assert.h>
#if !defined(WINDOWS)
#include <sys/time.h>
#endif /* !WINDOWS */

#include "porting.h"
#include "critical_section.h"
#include "connection_defs.h"
#include "thread.h"
#include "connection_error.h"
#include "perf_monitor.h"
#include "system_parameter.h"
#include "tsc_timer.h"
#include "show_scan.h"
#include "numeric_opfunc.h"

#undef csect_initialize_critical_section
#undef csect_finalize_critical_section
#undef csect_enter
#undef csect_enter_as_reader
#undef csect_exit
#undef csect_enter_critical_section
#undef csect_exit_critical_section

#define TOTAL_AND_MAX_TIMEVAL(total, max, elapsed) \
do { \
  (total).tv_sec += elapsed.tv_sec; \
  (total).tv_usec += elapsed.tv_usec; \
  (total).tv_sec += (total).tv_usec / 1000000; \
  (total).tv_usec %= 1000000; \
  if (((max).tv_sec < elapsed.tv_sec) \
      || ((max).tv_sec == elapsed.tv_sec \
          && (max).tv_usec < elapsed.tv_usec)) \
    { \
      (max).tv_sec = elapsed.tv_sec; \
      (max).tv_usec = elapsed.tv_usec; \
    } \
} while (0)

/* define critical section array */
CSS_CRITICAL_SECTION css_Csect_array[CRITICAL_SECTION_COUNT];

static const char *css_Csect_name[] = {
  "ER_LOG_FILE",
  "ER_MSG_CACHE",
  "WFG",
  "LOG",
  "LOCATOR_CLASSNAME_TABLE",
  "FILE_NEWFILE",
  "QPROC_QUERY_TABLE",
  "QPROC_XASL_CACHE",
  "QPROC_LIST_CACHE",
  "BOOT_SR_DBPARM",
  "DISK_REFRESH_GOODVOL",
  "CNV_FMT_LEXER",
  "HEAP_CHNGUESS",
  "TRAN_TABLE",
  "CT_OID_TABLE",
  "HA_SERVER_STATE",
  "COMPACTDB_ONE_INSTANCE",
  "ACL",
  "QPROC_FILTER_PRED_CACHE",
  "PARTITION_CACHE",
  "EVENT_LOG_FILE",
  "CONN_ACTIVE",
  "CONN_FREE",
  "TEMPFILE_CACHE",
  "LOG_PB",
  "LOG_ARCHIVE",
  "ACCESS_STATUS",
  "MVCC_ACTIVE_TRANS"
};

const char *css_Csect_name_conn = "CONN_ENTRY";
const char *css_Csect_name_tdes = "TDES";

static int csect_initialize_entry (int cs_index);
static int csect_finalize_entry (int cs_index);
static int csect_wait_on_writer_queue (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr, int timeout,
				       struct timespec *to);
static int csect_wait_on_promoter_queue (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr, int timeout,
					 struct timespec *to);
static int csect_wakeup_waiting_writer (CSS_CRITICAL_SECTION * cs_ptr);
static int csect_wakeup_waiting_promoter (CSS_CRITICAL_SECTION * cs_ptr);
static int csect_demote_critical_section (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr, int wait_secs);
static int csect_promote_critical_section (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr, int wait_secs);
static int csect_check_own_critical_section (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr);

/*
 * csect_initialize_critical_section() - initialize critical section
 *   return: 0 if success, or error code
 *   cs_ptr(in): critical section
 */
int
csect_initialize_critical_section (CSS_CRITICAL_SECTION * cs_ptr)
{
  int error_code = NO_ERROR;
  pthread_mutexattr_t mattr;

  assert (cs_ptr != NULL);

  cs_ptr->cs_index = -1;
  cs_ptr->name = NULL;

  error_code = pthread_mutexattr_init (&mattr);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEXATTR_INIT, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEXATTR_INIT;
    }

#ifdef CHECK_MUTEX
  error_code = pthread_mutexattr_settype (&mattr, PTHREAD_MUTEX_ERRORCHECK);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEXATTR_SETTYPE, 0);
      return ER_CSS_PTHREAD_MUTEXATTR_SETTYPE;
    }
#endif /* CHECK_MUTEX */

  error_code = pthread_mutex_init (&cs_ptr->lock, &mattr);

  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_INIT, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_INIT;
    }

  error_code = pthread_cond_init (&cs_ptr->readers_ok, NULL);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_INIT, 0);
      assert (0);
      return ER_CSS_PTHREAD_COND_INIT;
    }

  cs_ptr->rwlock = 0;
  cs_ptr->owner = ((pthread_t) 0);
  cs_ptr->tran_index = -1;
  cs_ptr->waiting_readers = 0;
  cs_ptr->waiting_writers = 0;
  cs_ptr->waiting_writers_queue = NULL;
  cs_ptr->waiting_promoters_queue = NULL;

  cs_ptr->total_enter = 0;
  cs_ptr->total_nwaits = 0;

  cs_ptr->max_wait.tv_sec = 0;
  cs_ptr->max_wait.tv_usec = 0;
  cs_ptr->total_wait.tv_sec = 0;
  cs_ptr->total_wait.tv_usec = 0;

  error_code = pthread_mutexattr_destroy (&mattr);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEXATTR_DESTROY, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEXATTR_DESTROY;
    }

  return NO_ERROR;
}

/*
 * csect_initialize_entry() - initialize critical section entry
 *   return: 0 if success, or error code
 *   cs_index(in): critical section entry index
 */
static int
csect_initialize_entry (int cs_index)
{
  int error_code = NO_ERROR;
  CSS_CRITICAL_SECTION *cs_ptr;

  assert (cs_index >= 0);
  assert (cs_index < CRITICAL_SECTION_COUNT);

  cs_ptr = &css_Csect_array[cs_index];
  error_code = csect_initialize_critical_section (cs_ptr);
  if (error_code == NO_ERROR)
    {
      cs_ptr->cs_index = cs_index;
      cs_ptr->name = css_Csect_name[cs_index];
    }

  return error_code;
}

/*
 * csect_finalize_critical_section() - free critical section
 *   return: 0 if success, or error code
 *   cs_ptr(in): critical section
 */
int
csect_finalize_critical_section (CSS_CRITICAL_SECTION * cs_ptr)
{
  int error_code = NO_ERROR;

  error_code = pthread_mutex_destroy (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_DESTROY, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_DESTROY;
    }

  error_code = pthread_cond_destroy (&cs_ptr->readers_ok);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_DESTROY, 0);
      assert (0);
      return ER_CSS_PTHREAD_COND_DESTROY;
    }

  cs_ptr->rwlock = 0;
  cs_ptr->owner = ((pthread_t) 0);
  cs_ptr->tran_index = -1;
  cs_ptr->waiting_readers = 0;
  cs_ptr->waiting_writers = 0;
  cs_ptr->waiting_writers_queue = NULL;
  cs_ptr->waiting_promoters_queue = NULL;

  cs_ptr->total_enter = 0;
  cs_ptr->total_nwaits = 0;

  cs_ptr->max_wait.tv_sec = 0;
  cs_ptr->max_wait.tv_usec = 0;
  cs_ptr->total_wait.tv_sec = 0;
  cs_ptr->total_wait.tv_usec = 0;

  return NO_ERROR;
}

/*
 * csect_finalize_entry() - free critical section entry
 *   return: 0 if success, or error code
 *   cs_index(in): critical section entry index
 */
static int
csect_finalize_entry (int cs_index)
{
  int error_code = NO_ERROR;
  CSS_CRITICAL_SECTION *cs_ptr;

  assert (cs_index >= 0);
  assert (cs_index < CRITICAL_SECTION_COUNT);

  cs_ptr = &css_Csect_array[cs_index];
  assert (cs_ptr->cs_index == cs_index);

  error_code = csect_finalize_critical_section (cs_ptr);
  if (error_code == NO_ERROR)
    {
      cs_ptr->cs_index = -1;
      cs_ptr->name = NULL;
    }

  return error_code;
}

/*
 * csect_initialize() - initialize all the critical section lock structures
 *   return: 0 if success, or error code
 */
int
csect_initialize (void)
{
  int i, error_code = NO_ERROR;

  for (i = 0; i < CRITICAL_SECTION_COUNT; i++)
    {
      error_code = csect_initialize_entry (i);
      if (error_code != NO_ERROR)
	{
	  break;
	}
    }

  return error_code;
}

/*
 * csect_finalize() - free all the critical section lock structures
 *   return: 0 if success, or error code
 */
int
csect_finalize (void)
{
  int i, error_code = NO_ERROR;

  for (i = 0; i < CRITICAL_SECTION_COUNT; i++)
    {
      error_code = csect_finalize_entry (i);
      if (error_code != NO_ERROR)
	{
	  break;
	}
    }

  return error_code;
}

static int
csect_wait_on_writer_queue (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr, int timeout, struct timespec *to)
{
  THREAD_ENTRY *prev_thread_p = NULL;
  int err = NO_ERROR;

  thread_p->next_wait_thrd = NULL;

  if (cs_ptr->waiting_writers_queue == NULL)
    {
      /* nobody is waiting. */
      cs_ptr->waiting_writers_queue = thread_p;
    }
  else
    {
      /* waits on the rear of the queue */
      prev_thread_p = cs_ptr->waiting_writers_queue;
      while (prev_thread_p->next_wait_thrd != NULL)
	{
	  assert (prev_thread_p != thread_p);

	  prev_thread_p = prev_thread_p->next_wait_thrd;
	}

      assert (prev_thread_p != thread_p);
      prev_thread_p->next_wait_thrd = thread_p;
    }

  while (true)
    {
      err = thread_suspend_with_other_mutex (thread_p, &cs_ptr->lock, timeout, to, THREAD_CSECT_WRITER_SUSPENDED);

      if (DOES_THREAD_RESUME_DUE_TO_SHUTDOWN (thread_p))
	{
	  /* check if i'm in the queue */
	  prev_thread_p = cs_ptr->waiting_writers_queue;
	  while (prev_thread_p != NULL)
	    {
	      if (prev_thread_p == thread_p)
		{
		  break;
		}
	      prev_thread_p = prev_thread_p->next_wait_thrd;
	    }

	  if (prev_thread_p != NULL)
	    {
	      continue;
	    }

	  /* In case I wake up by shutdown thread and there's not me in the writers Q, I proceed anyway assuming that
	   * followings occurred in order. 1. Critical section holder wakes me up after removing me from the writers
	   * Q.(mutex lock is not released yet). 2. I wake up and then wait for the mutex to be released. 3. The
	   * shutdown thread wakes me up by a server shutdown command.  (resume_status is changed to
	   * THREAD_RESUME_DUE_TO_INTERRUPT) 4. Critical section holder releases the mutex lock. 5. I wake up with
	   * holding the mutex. Currently, resume_status of mine is THREAD_RESUME_DUE_TO_INTERRUPT and there's not me
	   * in the writers Q. */
	}
      else if (thread_p->resume_status != THREAD_CSECT_WRITER_RESUMED)
	{
	  assert (0);
	}

      break;
    }

  return err;
}

static int
csect_wait_on_promoter_queue (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr, int timeout, struct timespec *to)
{
  THREAD_ENTRY *prev_thread_p = NULL;
  int err = NO_ERROR;

  thread_p->next_wait_thrd = NULL;

  if (cs_ptr->waiting_promoters_queue == NULL)
    {
      /* nobody is waiting. */
      cs_ptr->waiting_promoters_queue = thread_p;
    }
  else
    {
      /* waits on the rear of the queue */
      prev_thread_p = cs_ptr->waiting_promoters_queue;
      while (prev_thread_p->next_wait_thrd != NULL)
	{
	  assert (prev_thread_p != thread_p);

	  prev_thread_p = prev_thread_p->next_wait_thrd;
	}

      assert (prev_thread_p != thread_p);
      prev_thread_p->next_wait_thrd = thread_p;
    }

  while (1)
    {
      err = thread_suspend_with_other_mutex (thread_p, &cs_ptr->lock, timeout, to, THREAD_CSECT_PROMOTER_SUSPENDED);

      if (DOES_THREAD_RESUME_DUE_TO_SHUTDOWN (thread_p))
	{
	  /* check if i'm in the queue */
	  prev_thread_p = cs_ptr->waiting_promoters_queue;
	  while (prev_thread_p != NULL)
	    {
	      if (prev_thread_p == thread_p)
		{
		  break;
		}
	      prev_thread_p = prev_thread_p->next_wait_thrd;
	    }

	  if (prev_thread_p != NULL)
	    {
	      continue;
	    }

	  /* In case I wake up by shutdown thread and there's not me in the promoters Q, I proceed anyway assuming that 
	   * followings occurred in order. 1. Critical section holder wakes me up after removing me from the promoters 
	   * Q.(mutex lock is not released yet). 2. I wake up and then wait for the mutex to be released. 3. The
	   * shutdown thread wakes me up by a server shutdown command.  (resume_status is changed to
	   * THREAD_RESUME_DUE_TO_INTERRUPT) 4. Critical section holder releases the mutex lock. 5. I wake up with
	   * holding the mutex. Currently, resume_status of mine is THREAD_RESUME_DUE_TO_INTERRUPT and there's not me
	   * in the promoters Q. */
	}
      else if (thread_p->resume_status != THREAD_CSECT_PROMOTER_RESUMED)
	{
	  assert (0);
	}

      break;
    }

  return err;
}

static int
csect_wakeup_waiting_writer (CSS_CRITICAL_SECTION * cs_ptr)
{
  THREAD_ENTRY *waiting_thread_p = NULL;
  int error_code = NO_ERROR;

  waiting_thread_p = cs_ptr->waiting_writers_queue;

  if (waiting_thread_p != NULL)
    {
      cs_ptr->waiting_writers_queue = waiting_thread_p->next_wait_thrd;
      waiting_thread_p->next_wait_thrd = NULL;

      error_code = thread_wakeup (waiting_thread_p, THREAD_CSECT_WRITER_RESUMED);
    }

  return error_code;
}

static int
csect_wakeup_waiting_promoter (CSS_CRITICAL_SECTION * cs_ptr)
{
  THREAD_ENTRY *waiting_thread_p = NULL;
  int error_code = NO_ERROR;

  waiting_thread_p = cs_ptr->waiting_promoters_queue;

  if (waiting_thread_p != NULL)
    {
      cs_ptr->waiting_promoters_queue = waiting_thread_p->next_wait_thrd;
      waiting_thread_p->next_wait_thrd = NULL;

      error_code = thread_wakeup (waiting_thread_p, THREAD_CSECT_PROMOTER_RESUMED);
    }

  return error_code;
}

/*
 * csect_enter_critical_section() - lock critical section
 *   return: 0 if success, or error code
 *   cs_ptr(in): critical section
 *   wait_secs(in): timeout second
 */
int
csect_enter_critical_section (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr, int wait_secs)
{
  int error_code = NO_ERROR, r;
#if defined (EnableThreadMonitoring)
  TSC_TICKS start_tick, end_tick;
  TSCTIMEVAL tv_diff;
#endif

  TSC_TICKS wait_start_tick, wait_end_tick;
  TSCTIMEVAL wait_tv_diff;

  assert (cs_ptr != NULL);

  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

#if !defined (NDEBUG)
  thread_rc_track_meter (thread_p, __FILE__, __LINE__, THREAD_TRACK_CSECT_ENTER_AS_WRITER, &(cs_ptr->cs_index), RC_CS,
			 MGR_DEF);
#endif /* NDEBUG */

  cs_ptr->total_enter++;

#if defined (EnableThreadMonitoring)
  if (0 < prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD))
    {
      tsc_getticks (&start_tick);
    }
#endif

  error_code = pthread_mutex_lock (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_LOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_LOCK;
    }

  while (cs_ptr->rwlock != 0 || cs_ptr->owner != ((pthread_t) 0))
    {
      if (cs_ptr->rwlock < 0 && cs_ptr->owner == thread_p->tid)
	{
	  /* 
	   * I am holding the csect, and reenter it again as writer.
	   * Note that rwlock will be decremented.
	   */
	  break;
	}
      else
	{
	  if (wait_secs == INF_WAIT)
	    {
	      cs_ptr->waiting_writers++;
	      cs_ptr->total_nwaits++;

	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_start_tick);
		}

	      error_code = csect_wait_on_writer_queue (thread_p, cs_ptr, INF_WAIT, NULL);
	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_end_tick);
		  tsc_elapsed_time_usec (&wait_tv_diff, wait_end_tick, wait_start_tick);
		  TSC_ADD_TIMEVAL (thread_p->event_stats.cs_waits, wait_tv_diff);
		}

	      cs_ptr->waiting_writers--;
	      if (error_code != NO_ERROR)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		    }
		  assert (0);
		  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_WAIT, 0);
		  return ER_CSS_PTHREAD_COND_WAIT;
		}
	      if (cs_ptr->owner != ((pthread_t) 0) && cs_ptr->waiting_writers > 0)
		{
		  /* 
		   * There's one waiting to be promoted.
		   * Note that 'owner' was not reset while demoting.
		   * I have to yield to the waiter
		   */
		  error_code = csect_wakeup_waiting_promoter (cs_ptr);
		  if (error_code != NO_ERROR)
		    {
		      r = pthread_mutex_unlock (&cs_ptr->lock);
		      if (r != NO_ERROR)
			{
			  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
			  assert (0);
			  return ER_CSS_PTHREAD_MUTEX_UNLOCK;
			}
		      assert (0);
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_SIGNAL, 0);
		      return ER_CSS_PTHREAD_COND_SIGNAL;
		    }
		  continue;
		}
	    }
	  else if (wait_secs > 0)
	    {
	      struct timespec to;
	      to.tv_sec = time (NULL) + wait_secs;
	      to.tv_nsec = 0;

	      cs_ptr->waiting_writers++;

	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_start_tick);
		}

	      error_code = csect_wait_on_writer_queue (thread_p, cs_ptr, NOT_WAIT, &to);
	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_end_tick);
		  tsc_elapsed_time_usec (&wait_tv_diff, wait_end_tick, wait_start_tick);
		  TSC_ADD_TIMEVAL (thread_p->event_stats.cs_waits, wait_tv_diff);
		}

	      cs_ptr->waiting_writers--;
	      if (error_code != NO_ERROR)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		    }
		  if (error_code != ER_CSS_PTHREAD_COND_WAIT)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_WAIT, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_COND_WAIT;
		    }

		  return error_code;
		}
	    }
	  else
	    {
	      error_code = pthread_mutex_unlock (&cs_ptr->lock);
	      if (error_code != NO_ERROR)
		{
		  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		  assert (0);
		  return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		}
	      return ETIMEDOUT;
	    }
	}
    }

  /* rwlock will be < 0. It denotes that a writer owns the csect. */
  cs_ptr->rwlock--;

  /* record that I am the writer of the csect. */
  cs_ptr->owner = thread_p->tid;
  cs_ptr->tran_index = thread_p->tran_index;

#if defined (EnableThreadMonitoring)
  if (0 < prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD))
    {
      tsc_getticks (&end_tick);
      tsc_elapsed_time_usec (&tv_diff, end_tick, start_tick);
      TOTAL_AND_MAX_TIMEVAL (cs_ptr->total_wait, cs_ptr->max_wait, tv_diff);
    }
#endif

  error_code = pthread_mutex_unlock (&cs_ptr->lock);

  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
    }

#if defined (EnableThreadMonitoring)
  if (MONITOR_WAITING_THREAD (elapsed_time))
    {
      if (cs_ptr->cs_index > 0)
	{
	  er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE, ER_MNT_WAITING_THREAD, 2, cs_ptr->name,
		  prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD));
	}
      er_log_debug (ARG_FILE_LINE,
		    "csect_enter_critical_section_as_reader: %6d.%06d"
		    " %s total_enter %d total_nwaits %d max_wait %d.%06d total_wait %d.06d\n", elapsed_time.tv_sec,
		    elapsed_time.tv_usec, cs_ptr->name, cs_ptr->total_enter, cs_ptr->total_nwaits,
		    cs_ptr->max_wait.tv_sec, cs_ptr->max_wait.tv_usec, cs_ptr->total_wait.tv_sec,
		    cs_ptr->total_wait.tv_usec);
    }
#endif

  return NO_ERROR;
}

/*
 * csect_enter() - lock out other threads from concurrent execution
 * 		through a critical section of code
 *   return: 0 if success, or error code
 *   cs_index(in): identifier of the section to lock
 *   wait_secs(in): timeout second
 *
 * Note: locks the critical section, or suspends the thread until the critical
 *       section has been freed by another thread
 */
int
csect_enter (THREAD_ENTRY * thread_p, int cs_index, int wait_secs)
{
  CSS_CRITICAL_SECTION *cs_ptr;

  assert (cs_index >= 0);
  assert (cs_index < CRITICAL_SECTION_COUNT);

  cs_ptr = &css_Csect_array[cs_index];
#if defined (SERVER_MODE)
  assert (cs_ptr->cs_index == cs_index);
#endif

  return csect_enter_critical_section (thread_p, cs_ptr, wait_secs);
}

/*
 * csect_enter_critical_section_as_reader () - acquire a read lock
 *   return: 0 if success, or error code
 *   cs_ptr(in): critical section
 *   wait_secs(in): timeout second
 */
int
csect_enter_critical_section_as_reader (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr, int wait_secs)
{
  int error_code = NO_ERROR, r;
#if defined (EnableThreadMonitoring)
  TSC_TICKS start_tick, end_tick;
  TSCTIMEVAL tv_diff;
#endif
  TSC_TICKS wait_start_tick, wait_end_tick;
  TSCTIMEVAL wait_tv_diff;

  assert (cs_ptr != NULL);

  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

#if !defined (NDEBUG)
  thread_rc_track_meter (thread_p, __FILE__, __LINE__, THREAD_TRACK_CSECT_ENTER_AS_READER, &(cs_ptr->cs_index), RC_CS,
			 MGR_DEF);
#endif /* NDEBUG */

  cs_ptr->total_enter++;

#if defined (EnableThreadMonitoring)
  if (0 < prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD))
    {
      tsc_getticks (&start_tick);
    }
#endif

  error_code = pthread_mutex_lock (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_LOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_LOCK;
    }

  if (cs_ptr->rwlock < 0 && cs_ptr->owner == thread_p->tid)
    {
      /* writer reenters the csect as a reader. treat as writer. */
      cs_ptr->rwlock--;
    }
  else
    {
      /* reader can enter this csect without waiting writer(s) when the csect had been demoted by the other */
      while (cs_ptr->rwlock < 0 || (cs_ptr->waiting_writers > 0 && cs_ptr->owner == ((pthread_t) 0)))
	{
	  /* reader should wait writer(s). */
	  if (wait_secs == INF_WAIT)
	    {
	      cs_ptr->waiting_readers++;
	      cs_ptr->total_nwaits++;
	      thread_p->resume_status = THREAD_CSECT_READER_SUSPENDED;

	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_start_tick);
		}

	      error_code = pthread_cond_wait (&cs_ptr->readers_ok, &cs_ptr->lock);
	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_end_tick);
		  tsc_elapsed_time_usec (&wait_tv_diff, wait_end_tick, wait_start_tick);
		  TSC_ADD_TIMEVAL (thread_p->event_stats.cs_waits, wait_tv_diff);
		}

	      cs_ptr->waiting_readers--;

	      if (error_code != NO_ERROR)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_LOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_LOCK;
		    }
		  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_WAIT, 0);
		  assert (0);
		  return ER_CSS_PTHREAD_COND_WAIT;
		}
	      if (thread_p->resume_status == THREAD_RESUME_DUE_TO_INTERRUPT)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		    }
		  continue;
		}
	    }
	  else if (wait_secs > 0)
	    {
	      struct timespec to;
	      to.tv_sec = time (NULL) + wait_secs;
	      to.tv_nsec = 0;

	      cs_ptr->waiting_readers++;
	      thread_p->resume_status = THREAD_CSECT_READER_SUSPENDED;

	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_start_tick);
		}

	      error_code = pthread_cond_timedwait (&cs_ptr->readers_ok, &cs_ptr->lock, &to);
	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_end_tick);
		  tsc_elapsed_time_usec (&wait_tv_diff, wait_end_tick, wait_start_tick);
		  TSC_ADD_TIMEVAL (thread_p->event_stats.cs_waits, wait_tv_diff);
		}

	      cs_ptr->waiting_readers--;

	      if (error_code != 0)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		    }
		  if (error_code != ETIMEDOUT)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_WAIT, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_COND_WAIT;
		    }
		  return error_code;
		}
	      if (thread_p->resume_status == THREAD_RESUME_DUE_TO_INTERRUPT)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		    }
		  continue;
		}
	    }
	  else
	    {
	      error_code = pthread_mutex_unlock (&cs_ptr->lock);
	      if (error_code != NO_ERROR)
		{
		  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		  assert (0);
		  return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		}
	      return ETIMEDOUT;
	    }
	}

      /* rwlock will be > 0. record that a reader enters the csect. */
      cs_ptr->rwlock++;
    }

#if defined (EnableThreadMonitoring)
  if (0 < prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD))
    {
      tsc_getticks (&end_tick);
      tsc_elapsed_time_usec (&tv_diff, end_tick, start_tick);
      TOTAL_AND_MAX_TIMEVAL (cs_ptr->total_wait, cs_ptr->max_wait, tv_diff);
    }
#endif

  error_code = pthread_mutex_unlock (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
    }

#if defined (EnableThreadMonitoring)
  if (MONITOR_WAITING_THREAD (elapsed_time))
    {
      if (cs_ptr->cs_index > 0)
	{
	  er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE, ER_MNT_WAITING_THREAD, 2, cs_ptr->name,
		  prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD));
	}
      er_log_debug (ARG_FILE_LINE,
		    "csect_enter_critical_section: %6d.%06d %s total_enter %d total_nwaits %d max_wait %d.%06d"
		    " total_wait %d.06d\n", elapsed_time.tv_sec, elapsed_time.tv_usec, cs_ptr->name,
		    cs_ptr->total_enter, cs_ptr->total_nwaits, cs_ptr->max_wait.tv_sec, cs_ptr->max_wait.tv_usec,
		    cs_ptr->total_wait.tv_sec, cs_ptr->total_wait.tv_usec);
    }
#endif

  return NO_ERROR;
}

/*
 * csect_enter_as_reader() - acquire a read lock
 *   return: 0 if success, or error code
 *   cs_index(in): identifier of the section to lock
 *   wait_secs(in): timeout second
 *
 * Note: Multiple readers go if there are no writers.
 */
int
csect_enter_as_reader (THREAD_ENTRY * thread_p, int cs_index, int wait_secs)
{
  CSS_CRITICAL_SECTION *cs_ptr;

  assert (cs_index >= 0);
  assert (cs_index < CRITICAL_SECTION_COUNT);

  cs_ptr = &css_Csect_array[cs_index];
#if defined (SERVER_MODE)
  assert (cs_ptr->cs_index == cs_index);
#endif

  return csect_enter_critical_section_as_reader (thread_p, cs_ptr, wait_secs);
}

/*
 * csect_demote_critical_section () - acquire a read lock when it has write lock
 *   return: 0 if success, or error code
 *   cs_ptr(in): critical section
 *   wait_secs(in): timeout second
 *
 * Note: Always successful because I have the write lock.
 */
static int
csect_demote_critical_section (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr, int wait_secs)
{
  int error_code = NO_ERROR, r;
#if defined (EnableThreadMonitoring)
  TSC_TICKS start_tick, end_tick;
  TSCTIMEVAL tv_diff;
#endif
  TSC_TICKS wait_start_tick, wait_end_tick;
  TSCTIMEVAL wait_tv_diff;

  assert (cs_ptr != NULL);

  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

#if !defined (NDEBUG)
  thread_rc_track_meter (thread_p, __FILE__, __LINE__, THREAD_TRACK_CSECT_DEMOTE, &(cs_ptr->cs_index), RC_CS, MGR_DEF);
#endif /* NDEBUG */

  cs_ptr->total_enter++;

#if defined (EnableThreadMonitoring)
  if (0 < prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD))
    {
      tsc_getticks (&start_tick);
    }
#endif

  error_code = pthread_mutex_lock (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_LOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_LOCK;
    }

  if (cs_ptr->rwlock < 0 && cs_ptr->owner == thread_p->tid)
    {
      /* 
       * I have write lock. I was entered before as a writer.
       * Every others are waiting on either 'reader_ok', if it is waiting as
       * a reader, or 'writers_queue' with 'waiting_writers++', if waiting as
       * a writer.
       */

      cs_ptr->rwlock++;		/* releasing */
      if (cs_ptr->rwlock < 0)
	{
	  /* 
	   * In the middle of an outer critical section, it is not possible
	   * to be a reader. Treat as same as csect_enter_critical_section_as_reader().
	   */
	  cs_ptr->rwlock--;	/* entering as a writer */
	}
      else
	{
	  /* rwlock == 0 */
	  cs_ptr->rwlock++;	/* entering as a reader */
#if 0
	  cs_ptr->owner = (pthread_t) 0;
	  cs_ptr->tran_index = -1;
#endif
	}
    }
  else
    {
      /* 
       * I don't have write lock. Act like a normal reader request.
       */
      while (cs_ptr->rwlock < 0 || cs_ptr->waiting_writers > 0)
	{
	  /* reader should wait writer(s). */
	  if (wait_secs == INF_WAIT)
	    {
	      cs_ptr->waiting_readers++;
	      cs_ptr->total_nwaits++;
	      thread_p->resume_status = THREAD_CSECT_READER_SUSPENDED;

	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_start_tick);
		}

	      error_code = pthread_cond_wait (&cs_ptr->readers_ok, &cs_ptr->lock);
	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_end_tick);
		  tsc_elapsed_time_usec (&wait_tv_diff, wait_end_tick, wait_start_tick);
		  TSC_ADD_TIMEVAL (thread_p->event_stats.cs_waits, wait_tv_diff);
		}

	      cs_ptr->waiting_readers--;

	      if (error_code != NO_ERROR)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_LOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_LOCK;
		    }
		  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_WAIT, 0);
		  assert (0);
		  return ER_CSS_PTHREAD_COND_WAIT;
		}
	      if (thread_p->resume_status == THREAD_RESUME_DUE_TO_INTERRUPT)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		    }
		  continue;
		}
	    }
	  else if (wait_secs > 0)
	    {
	      struct timespec to;
	      to.tv_sec = time (NULL) + wait_secs;
	      to.tv_nsec = 0;

	      cs_ptr->waiting_readers++;
	      thread_p->resume_status = THREAD_CSECT_READER_SUSPENDED;

	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_start_tick);
		}

	      error_code = pthread_cond_timedwait (&cs_ptr->readers_ok, &cs_ptr->lock, &to);
	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_end_tick);
		  tsc_elapsed_time_usec (&wait_tv_diff, wait_end_tick, wait_start_tick);
		  TSC_ADD_TIMEVAL (thread_p->event_stats.cs_waits, wait_tv_diff);
		}

	      cs_ptr->waiting_readers--;

	      if (error_code != 0)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		    }
		  if (error_code != ETIMEDOUT)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_WAIT, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_COND_WAIT;
		    }
		  return error_code;
		}
	      if (thread_p->resume_status == THREAD_RESUME_DUE_TO_INTERRUPT)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		    }
		  continue;
		}
	    }
	  else
	    {
	      error_code = pthread_mutex_unlock (&cs_ptr->lock);
	      if (error_code != NO_ERROR)
		{
		  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		  assert (0);
		  return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		}
	      return ETIMEDOUT;
	    }
	}

      /* rwlock will be > 0. record that a reader enters the csect. */
      cs_ptr->rwlock++;
    }

#if defined (EnableThreadMonitoring)
  if (0 < prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD))
    {
      tsc_getticks (&end_tick);
      tsc_elapsed_time_usec (&tv_diff, end_tick, start_tick);
      TOTAL_AND_MAX_TIMEVAL (cs_ptr->total_wait, cs_ptr->max_wait, tv_diff);
    }
#endif

  /* Someone can wait for being reader. Wakeup all readers. */
  error_code = pthread_cond_broadcast (&cs_ptr->readers_ok);
  if (error_code != NO_ERROR)
    {
      r = pthread_mutex_unlock (&cs_ptr->lock);
      if (r != NO_ERROR)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
	  assert (0);
	  return ER_CSS_PTHREAD_MUTEX_UNLOCK;
	}

      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_BROADCAST, 0);
      assert (0);
      return ER_CSS_PTHREAD_COND_BROADCAST;
    }

  error_code = pthread_mutex_unlock (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
    }

#if defined (EnableThreadMonitoring)
  if (MONITOR_WAITING_THREAD (elapsed_time))
    {
      if (cs_ptr->cs_index > 0)
	{
	  er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE, ER_MNT_WAITING_THREAD, 2, cs_ptr->name,
		  prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD));
	}
      er_log_debug (ARG_FILE_LINE,
		    "csect_demote_critical_section: %6d.%06d %s total_enter %d total_nwaits %d max_wait %d.%06d"
		    " total_wait %d.06d\n", elapsed_time.tv_sec, elapsed_time.tv_usec, cs_ptr->name,
		    cs_ptr->total_enter, cs_ptr->total_nwaits, cs_ptr->max_wait.tv_sec, cs_ptr->max_wait.tv_usec,
		    cs_ptr->total_wait.tv_sec, cs_ptr->total_wait.tv_usec);
    }
#endif

  return NO_ERROR;
}

/*
 * csect_demote () - acquire a read lock when it has write lock
 *   return: 0 if success, or error code
 *   cs_index(in): identifier of the section to lock
 *   wait_secs(in): timeout second
 *
 * Note: Always successful because I have the write lock.
 */
int
csect_demote (THREAD_ENTRY * thread_p, int cs_index, int wait_secs)
{
  CSS_CRITICAL_SECTION *cs_ptr;

  assert (cs_index >= 0);
  assert (cs_index < CRITICAL_SECTION_COUNT);

  cs_ptr = &css_Csect_array[cs_index];
  return csect_demote_critical_section (thread_p, cs_ptr, wait_secs);
}

/*
 * csect_promote_critical_section () - acquire a write lock when it has read lock
 *   return: 0 if success, or error code
 *   cs_ptr(in): critical section
 *   wait_secs(in): timeout second
 *
 * Note: Always successful because I have the write lock.
 */
static int
csect_promote_critical_section (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr, int wait_secs)
{
  int error_code = NO_ERROR, r;
#if defined (EnableThreadMonitoring)
  TSC_TICKS start_tick, end_tick;
  TSCTIMEVAL tv_diff;
#endif
  TSC_TICKS wait_start_tick, wait_end_tick;
  TSCTIMEVAL wait_tv_diff;

  assert (cs_ptr != NULL);

  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

#if !defined (NDEBUG)
  thread_rc_track_meter (thread_p, __FILE__, __LINE__, THREAD_TRACK_CSECT_PROMOTE, &(cs_ptr->cs_index), RC_CS, MGR_DEF);
#endif /* NDEBUG */

  cs_ptr->total_enter++;

#if defined (EnableThreadMonitoring)
  if (0 < prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD))
    {
      tsc_getticks (&start_tick);
    }
#endif

  error_code = pthread_mutex_lock (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_LOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_LOCK;
    }

  if (cs_ptr->rwlock > 0)
    {
      /* 
       * I am a reader so that no writer is in this csect but reader(s) could be.
       * All writers are waiting on 'writers_queue' with 'waiting_writers++'.
       */
      cs_ptr->rwlock--;		/* releasing */
    }
  else
    {
      cs_ptr->rwlock++;		/* releasing */
      /* 
       * I don't have read lock. Act like a normal writer request.
       */
    }
  while (cs_ptr->rwlock != 0)
    {
      /* There's another readers. So I have to wait as a writer. */
      if (cs_ptr->rwlock < 0 && cs_ptr->owner == thread_p->tid)
	{
	  /* 
	   * I am holding the csect, and reenter it again as writer.
	   * Note that rwlock will be decremented.
	   */
	  break;
	}
      else
	{
	  if (wait_secs == INF_WAIT)
	    {
	      cs_ptr->waiting_writers++;
	      cs_ptr->total_nwaits++;

	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_start_tick);
		}

	      error_code = csect_wait_on_promoter_queue (thread_p, cs_ptr, INF_WAIT, NULL);
	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_end_tick);
		  tsc_elapsed_time_usec (&wait_tv_diff, wait_end_tick, wait_start_tick);
		  TSC_ADD_TIMEVAL (thread_p->event_stats.cs_waits, wait_tv_diff);
		}

	      cs_ptr->waiting_writers--;
	      if (error_code != NO_ERROR)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		    }
		  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_WAIT, 0);
		  assert (0);
		  return ER_CSS_PTHREAD_COND_WAIT;
		}
	    }
	  else if (wait_secs > 0)
	    {
	      struct timespec to;
	      to.tv_sec = time (NULL) + wait_secs;
	      to.tv_nsec = 0;

	      cs_ptr->waiting_writers++;

	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_start_tick);
		}

	      error_code = csect_wait_on_promoter_queue (thread_p, cs_ptr, NOT_WAIT, &to);
	      if (thread_p->event_stats.trace_slow_query == true)
		{
		  tsc_getticks (&wait_end_tick);
		  tsc_elapsed_time_usec (&wait_tv_diff, wait_end_tick, wait_start_tick);
		  TSC_ADD_TIMEVAL (thread_p->event_stats.cs_waits, wait_tv_diff);
		}

	      cs_ptr->waiting_writers--;
	      if (error_code != NO_ERROR)
		{
		  r = pthread_mutex_unlock (&cs_ptr->lock);
		  if (r != NO_ERROR)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		    }
		  if (error_code != ER_CSS_PTHREAD_COND_WAIT && error_code != ER_CSS_PTHREAD_COND_TIMEDOUT)
		    {
		      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_WAIT, 0);
		      assert (0);
		      return ER_CSS_PTHREAD_COND_WAIT;
		    }
		  return error_code;
		}
	    }
	  else
	    {
	      error_code = pthread_mutex_unlock (&cs_ptr->lock);
	      if (error_code != NO_ERROR)
		{
		  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
		  assert (0);
		  return ER_CSS_PTHREAD_MUTEX_UNLOCK;
		}
	      return ETIMEDOUT;
	    }
	}
    }

  /* rwlock will be < 0. It denotes that a writer owns the csect. */
  cs_ptr->rwlock--;
  /* record that I am the writer of the csect. */
  cs_ptr->owner = thread_p->tid;
  cs_ptr->tran_index = thread_p->tran_index;

#if defined (EnableThreadMonitoring)
  if (0 < prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD))
    {
      tsc_getticks (&end_tick);
      tsc_elapsed_time_usec (&tv_diff, end_tick, start_tick);
      TOTAL_AND_MAX_TIMEVAL (cs_ptr->total_wait, cs_ptr->max_wait, tv_diff);
    }
#endif

  error_code = pthread_mutex_unlock (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
    }

#if defined (EnableThreadMonitoring)
  if (MONITOR_WAITING_THREAD (elapsed_time))
    {
      if (cs_ptr->cs_index > 0)
	{
	  er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE, ER_MNT_WAITING_THREAD, 2, cs_ptr->name,
		  prm_get_integer_value (PRM_ID_MNT_WAITING_THREAD));
	}
      er_log_debug (ARG_FILE_LINE,
		    "csect_promote_critical_section: %6d.%06d %s total_enter %d total_nwaits %d max_wait %d.%06d"
		    " total_wait %d.06d\n", elapsed_time.tv_sec, elapsed_time.tv_usec, cs_ptr->name,
		    cs_ptr->total_enter, cs_ptr->total_nwaits, cs_ptr->max_wait.tv_sec, cs_ptr->max_wait.tv_usec,
		    cs_ptr->total_wait.tv_sec, cs_ptr->total_wait.tv_usec);
    }
#endif

  return NO_ERROR;
}

/*
 * csect_promote () - acquire a write lock when it has read lock
 *   return: 0 if success, or error code
 *   cs_index(in): identifier of the section to lock
 *   wait_secs(in): timeout second
 *
 * Note: Always successful because I have the write lock.
 */
int
csect_promote (THREAD_ENTRY * thread_p, int cs_index, int wait_secs)
{
  CSS_CRITICAL_SECTION *cs_ptr;

  assert (cs_index >= 0);
  assert (cs_index < CRITICAL_SECTION_COUNT);

  cs_ptr = &css_Csect_array[cs_index];
  return csect_promote_critical_section (thread_p, cs_ptr, wait_secs);
}

/*
 * csect_exit_critical_section() - unlock critical section
 *   return:  0 if success, or error code
 *   cs_ptr(in): critical section
 */
int
csect_exit_critical_section (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr)
{
  int error_code = NO_ERROR;
  bool ww, wr, wp;

  assert (cs_ptr != NULL);

  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

#if !defined (NDEBUG)
  thread_rc_track_meter (thread_p, __FILE__, __LINE__, THREAD_TRACK_CSECT_EXIT, &(cs_ptr->cs_index), RC_CS, MGR_DEF);
#endif /* NDEBUG */

  error_code = pthread_mutex_lock (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_LOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_LOCK;
    }

  if (cs_ptr->rwlock < 0)
    {				/* rwlock < 0 if locked for writing */
      cs_ptr->rwlock++;
      if (cs_ptr->rwlock < 0)
	{
	  /* in the middle of an outer critical section */
	  error_code = pthread_mutex_unlock (&cs_ptr->lock);
	  if (error_code != NO_ERROR)
	    {
	      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
	      assert (0);
	      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
	    }
	  return NO_ERROR;
	}
      else
	{
	  assert (cs_ptr->rwlock == 0);
	  cs_ptr->owner = ((pthread_t) 0);
	  cs_ptr->tran_index = -1;
	}
    }
  else if (cs_ptr->rwlock > 0)
    {
      cs_ptr->rwlock--;
    }
  else
    {
      /* cs_ptr->rwlock == 0 */
      error_code = pthread_mutex_unlock (&cs_ptr->lock);
      if (error_code != NO_ERROR)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
	  assert (0);
	  return ER_CSS_PTHREAD_MUTEX_UNLOCK;
	}

      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CS_UNLOCKED_BEFORE, 0);
      assert (0);
      return ER_CS_UNLOCKED_BEFORE;
    }

  /* 
   * Keep flags that show if there are waiting readers or writers
   * so that we can wake them up outside the monitor lock.
   */
  ww = (cs_ptr->waiting_writers > 0 && cs_ptr->rwlock == 0 && cs_ptr->owner == ((pthread_t) 0));
  wp = (cs_ptr->waiting_writers > 0 && cs_ptr->rwlock == 0 && cs_ptr->owner != ((pthread_t) 0));
  wr = (cs_ptr->waiting_writers == 0);

  /* wakeup a waiting writer first. Otherwise wakeup all readers. */
  if (wp == true)
    {
      error_code = csect_wakeup_waiting_promoter (cs_ptr);
      if (error_code != NO_ERROR)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_SIGNAL, 0);
	  pthread_mutex_unlock (&cs_ptr->lock);
	  assert (0);
	  return ER_CSS_PTHREAD_COND_SIGNAL;
	}
    }
  else if (ww == true)
    {
      error_code = csect_wakeup_waiting_writer (cs_ptr);
      if (error_code != NO_ERROR)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_SIGNAL, 0);
	  pthread_mutex_unlock (&cs_ptr->lock);
	  assert (0);
	  return ER_CSS_PTHREAD_COND_SIGNAL;
	}
    }
  else
    {
      error_code = pthread_cond_broadcast (&cs_ptr->readers_ok);
      if (error_code != NO_ERROR)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_COND_BROADCAST, 0);
	  pthread_mutex_unlock (&cs_ptr->lock);
	  assert (0);
	  return ER_CSS_PTHREAD_COND_BROADCAST;
	}
    }

  error_code = pthread_mutex_unlock (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
    }

  return NO_ERROR;
}

/*
 * csect_exit() - free a lock that prevents other threads from
 *             concurrent execution through a critical section of code
 *   return: 0 if success, or error code
 *   cs_index(in): identifier of the section to unlock
 *
 * Note: unlocks the critical section, which may restart another thread that
 *       is suspended and waiting for the critical section.
 */
int
csect_exit (THREAD_ENTRY * thread_p, int cs_index)
{
  CSS_CRITICAL_SECTION *cs_ptr;

  assert (cs_index >= 0);
  assert (cs_index < CRITICAL_SECTION_COUNT);

  cs_ptr = &css_Csect_array[cs_index];
#if defined (SERVER_MODE)
  assert (cs_ptr->cs_index == cs_index);
#endif

  return csect_exit_critical_section (thread_p, cs_ptr);
}

/*
 * csect_dump_statistics() - dump critical section statistics
 *   return: void
 */
void
csect_dump_statistics (FILE * fp)
{
  CSS_CRITICAL_SECTION *cs_ptr;
  int r = NO_ERROR, i;

  fprintf (fp, "             CS Name    |Total Enter|Total Wait |   Max Wait    |  Total wait\n");

  for (i = 0; i < CRITICAL_SECTION_COUNT; i++)
    {
      cs_ptr = &css_Csect_array[i];
      fprintf (fp, "%23s |%10d |%10d | %6ld.%06ld | %6ld.%06ld\n", cs_ptr->name, cs_ptr->total_enter,
	       cs_ptr->total_nwaits, cs_ptr->max_wait.tv_sec, cs_ptr->max_wait.tv_usec, cs_ptr->total_wait.tv_sec,
	       cs_ptr->total_wait.tv_usec);

      cs_ptr->total_enter = 0;
      cs_ptr->total_nwaits = 0;
      cs_ptr->max_wait.tv_sec = 0;
      cs_ptr->max_wait.tv_usec = 0;

      cs_ptr->total_wait.tv_sec = 0;
      cs_ptr->total_wait.tv_usec = 0;
    }
}


/*
 * csect_check_own() - check if current thread is critical section owner
 *   return: true if cs's owner is me, false if not
 *   cs_index(in): css_Csect_array's index
 */
int
csect_check_own (THREAD_ENTRY * thread_p, int cs_index)
{
  CSS_CRITICAL_SECTION *cs_ptr;
  int error_code = NO_ERROR;

  assert (cs_index >= 0);
  assert (cs_index < CRITICAL_SECTION_COUNT);

  cs_ptr = &css_Csect_array[cs_index];

  return csect_check_own_critical_section (thread_p, cs_ptr);
}

/*
 * csect_check_own_critical_section() - check if current thread is critical section owner
 *   return: true if cs's owner is me, false if not
 *   cs_ptr(in): critical section
 */
static int
csect_check_own_critical_section (THREAD_ENTRY * thread_p, CSS_CRITICAL_SECTION * cs_ptr)
{
  int error_code = NO_ERROR, return_code;

  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  error_code = pthread_mutex_lock (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_LOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_LOCK;
    }

  if (cs_ptr->rwlock < 0 && cs_ptr->owner == thread_p->tid)
    {
      /* has the write lock */
      return_code = 1;
    }
  else if (cs_ptr->rwlock > 0)
    {
      /* has the read lock */
      return_code = 2;
    }
  else
    {
      return_code = 0;
    }

  error_code = pthread_mutex_unlock (&cs_ptr->lock);
  if (error_code != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_PTHREAD_MUTEX_LOCK, 0);
      assert (0);
      return ER_CSS_PTHREAD_MUTEX_LOCK;
    }

  return return_code;
}

/*
 * csect_start_scan () -  start scan function for
 *                        show global critical sections
 *   return: NO_ERROR, or ER_code
 *
 *   thread_p(in):
 *   show_type(in):
 *   arg_values(in):
 *   arg_cnt(in):
 *   ptr(in/out): 
 */
int
csect_start_scan (THREAD_ENTRY * thread_p, int show_type, DB_VALUE ** arg_values, int arg_cnt, void **ptr)
{
  SHOWSTMT_ARRAY_CONTEXT *ctx = NULL;
  int i, idx, error = NO_ERROR;
  DB_VALUE *vals = NULL;
  CSS_CRITICAL_SECTION *cs_ptr;
  char buf[256] = { 0 };
  double msec;
  DB_VALUE db_val;
  DB_DATA_STATUS data_status;
  pthread_t owner_tid;
  int ival;
  THREAD_ENTRY *thread_entry = NULL;
  int num_cols = 12;

  *ptr = NULL;
  ctx = showstmt_alloc_array_context (thread_p, CRITICAL_SECTION_COUNT, num_cols);
  if (ctx == NULL)
    {
      error = er_errid ();
      goto exit_on_error;
    }

  for (i = 0; i < CRITICAL_SECTION_COUNT; i++)
    {
      idx = 0;
      vals = showstmt_alloc_tuple_in_context (thread_p, ctx);
      if (vals == NULL)
	{
	  error = er_errid ();
	  goto exit_on_error;
	}

      cs_ptr = &css_Csect_array[i];

      /* The index of the critical section */
      db_make_int (&vals[idx], cs_ptr->cs_index);
      idx++;

      /* The name of the critical section */
      db_make_string (&vals[idx], cs_ptr->name);
      idx++;

      /* 'N readers', '1 writer', 'none' */
      ival = cs_ptr->rwlock;
      if (ival > 0)
	{
	  snprintf (buf, sizeof (buf), "%d readers", ival);
	}
      else if (ival < 0)
	{
	  snprintf (buf, sizeof (buf), "1 writer");
	}
      else
	{
	  snprintf (buf, sizeof (buf), "none");
	}

      error = db_make_string_copy (&vals[idx], buf);
      idx++;
      if (error != NO_ERROR)
	{
	  goto exit_on_error;
	}

      /* The number of waiting readers */
      db_make_int (&vals[idx], cs_ptr->waiting_readers);
      idx++;

      /* The number of waiting writers */
      db_make_int (&vals[idx], cs_ptr->waiting_writers);
      idx++;

      /* The thread index of CS owner writer, NULL if no owner */
      owner_tid = cs_ptr->owner;
      if (owner_tid == 0)
	{
	  db_make_null (&vals[idx]);
	}
      else
	{
	  thread_entry = thread_find_entry_by_tid (owner_tid);
	  if (thread_entry != NULL)
	    {
	      db_make_bigint (&vals[idx], thread_entry->index);
	    }
	  else
	    {
	      db_make_null (&vals[idx]);
	    }
	}
      idx++;

      /* Transaction id of CS owner writer, NULL if no owner */
      ival = cs_ptr->tran_index;
      if (ival == -1)
	{
	  db_make_null (&vals[idx]);
	}
      else
	{
	  db_make_int (&vals[idx], ival);
	}
      idx++;

      /* Total count of enters */
      db_make_bigint (&vals[idx], cs_ptr->total_enter);
      idx++;

      /* Total count of waiters */
      db_make_bigint (&vals[idx], cs_ptr->total_nwaits);
      idx++;

      /* The thread index of waiting promoter, NULL if no waiting promoter */
      thread_entry = cs_ptr->waiting_promoters_queue;
      if (thread_entry != NULL)
	{
	  db_make_int (&vals[idx], thread_entry->index);
	}
      else
	{
	  db_make_null (&vals[idx]);
	}
      idx++;

      /* Maximum waiting time (millisecond) */
      msec = cs_ptr->max_wait.tv_sec * 1000 + cs_ptr->max_wait.tv_usec / 1000.0;
      db_make_double (&db_val, msec);
      db_value_domain_init (&vals[idx], DB_TYPE_NUMERIC, 10, 3);
      error = numeric_db_value_coerce_to_num (&db_val, &vals[idx], &data_status);
      idx++;
      if (error != NO_ERROR)
	{
	  goto exit_on_error;
	}

      /* Total waiting time (millisecond) */
      msec = cs_ptr->total_wait.tv_sec * 1000 + cs_ptr->total_wait.tv_usec / 1000.0;
      db_make_double (&db_val, msec);
      db_value_domain_init (&vals[idx], DB_TYPE_NUMERIC, 10, 3);
      error = numeric_db_value_coerce_to_num (&db_val, &vals[idx], &data_status);
      idx++;
      if (error != NO_ERROR)
	{
	  goto exit_on_error;
	}

      assert (idx == num_cols);
    }

  *ptr = ctx;
  return NO_ERROR;

exit_on_error:

  if (ctx != NULL)
    {
      showstmt_free_array_context (thread_p, ctx);
    }

  return error;
}
