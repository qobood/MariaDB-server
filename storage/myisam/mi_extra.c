/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "myisamdef.h"

static void mi_extra_keyflag(MI_INFO *info, enum ha_extra_function function);


/*
  Set options and buffers to optimize table handling

  SYNOPSIS
    mi_extra()
    info	open table
    function	operation
    extra_arg	Pointer to extra argument (normally pointer to ulong)
    		Used when function is one of:
		HA_EXTRA_WRITE_CACHE
		HA_EXTRA_CACHE
  RETURN VALUES
    0  ok
    #  error
*/

int mi_extra(MI_INFO *info, enum ha_extra_function function, void *extra_arg)
{
  int error=0, error2;
  ulong cache_size;
  MYISAM_SHARE *share=info->s;
  DBUG_ENTER("mi_extra");
  DBUG_PRINT("enter",("function: %d",(int) function));

  switch (function) {
  case HA_EXTRA_RESET_STATE:		/* Reset state (don't free buffers) */
    info->lastinx= 0;			/* Use first index as def */
    info->last_search_keypage=info->lastpos= HA_OFFSET_ERROR;
    info->page_changed=1;
					/* Next/prev gives first/last */
    if (info->opt_flag & READ_CACHE_USED)
    {
      reinit_io_cache(&info->rec_cache,READ_CACHE,0,
		      (pbool) (info->lock_type != F_UNLCK),
                      (pbool) MY_TEST(info->update & HA_STATE_ROW_CHANGED)
		      );
    }
    info->update= ((info->update & HA_STATE_CHANGED) | HA_STATE_NEXT_FOUND |
		   HA_STATE_PREV_FOUND);
    break;
  case HA_EXTRA_CACHE:
    if (info->lock_type == F_UNLCK &&
	(share->options & HA_OPTION_PACK_RECORD))
    {
      error=1;			/* Not possibly if not locked */
      my_errno=EACCES;
      break;
    }
    if (info->s->file_map) /* Don't use cache if mmap */
      break;
#if defined(HAVE_MMAP) && defined(HAVE_MADVISE) && !defined(HAVE_valgrind)
    if ((share->options & HA_OPTION_COMPRESS_RECORD))
    {
      mysql_mutex_lock(&share->intern_lock);
      if (_mi_memmap_file(info))
      {
	/* We don't nead MADV_SEQUENTIAL if small file */
	madvise((char*) share->file_map, share->state.state.data_file_length,
		share->state.state.data_file_length <= RECORD_CACHE_SIZE*16 ?
		MADV_RANDOM : MADV_SEQUENTIAL);
        mysql_mutex_unlock(&share->intern_lock);
	break;
      }
      mysql_mutex_unlock(&share->intern_lock);
    }
#endif
    if (info->opt_flag & WRITE_CACHE_USED)
    {
      info->opt_flag&= ~WRITE_CACHE_USED;
      if ((error=end_io_cache(&info->rec_cache)))
	break;
    }
    if (!(info->opt_flag &
	  (READ_CACHE_USED | WRITE_CACHE_USED | MEMMAP_USED)))
    {
      cache_size= (extra_arg ? *(ulong*) extra_arg :
		   my_default_record_cache_size);
      if (!(init_io_cache(&info->rec_cache,info->dfile,
			 (uint) MY_MIN(info->state->data_file_length+1,
				    cache_size),
			  READ_CACHE,0L,(pbool) (info->lock_type != F_UNLCK),
			  MYF(share->write_flag & MY_WAIT_IF_FULL))))
      {
	info->opt_flag|=READ_CACHE_USED;
	info->update&= ~HA_STATE_ROW_CHANGED;
      }
      if (share->concurrent_insert)
	info->rec_cache.end_of_file=info->state->data_file_length;
    }
    break;
  case HA_EXTRA_REINIT_CACHE:
    if (info->opt_flag & READ_CACHE_USED)
    {
      reinit_io_cache(&info->rec_cache,READ_CACHE,info->nextpos,
		      (pbool) (info->lock_type != F_UNLCK),
                      (pbool) MY_TEST(info->update & HA_STATE_ROW_CHANGED));
      info->update&= ~HA_STATE_ROW_CHANGED;
      if (share->concurrent_insert)
	info->rec_cache.end_of_file=info->state->data_file_length;
    }
    break;
  case HA_EXTRA_WRITE_CACHE:
    if (info->lock_type == F_UNLCK)
    {
      error=1;			/* Not possibly if not locked */
      break;
    }

    cache_size= (extra_arg ? *(ulong*) extra_arg :
		 my_default_record_cache_size);
    if (!(info->opt_flag &
	  (READ_CACHE_USED | WRITE_CACHE_USED | OPT_NO_ROWS)) &&
	!share->state.header.uniques)
      if (!(init_io_cache(&info->rec_cache,info->dfile, cache_size,
			 WRITE_CACHE,info->state->data_file_length,
			  (pbool) (info->lock_type != F_UNLCK),
			  MYF(share->write_flag & MY_WAIT_IF_FULL))))
      {
	info->opt_flag|=WRITE_CACHE_USED;
	info->update&= ~(HA_STATE_ROW_CHANGED |
			 HA_STATE_WRITE_AT_END |
			 HA_STATE_EXTEND_BLOCK);
      }
    break;
  case HA_EXTRA_PREPARE_FOR_UPDATE:
    if (info->s->data_file_type != DYNAMIC_RECORD)
      break;
    /* Remove read/write cache if dynamic rows */
    /* fall through */
  case HA_EXTRA_NO_CACHE:
    if (info->opt_flag & (READ_CACHE_USED | WRITE_CACHE_USED))
    {
      info->opt_flag&= ~(READ_CACHE_USED | WRITE_CACHE_USED);
      error=end_io_cache(&info->rec_cache);
      /* Sergei will insert full text index caching here */
    }
#if defined(HAVE_MMAP) && defined(HAVE_MADVISE)  && !defined(HAVE_valgrind)
    if (info->opt_flag & MEMMAP_USED)
      madvise((char*) share->file_map, share->state.state.data_file_length,
              MADV_RANDOM);
#endif
    break;
  case HA_EXTRA_FLUSH_CACHE:
    if (info->opt_flag & WRITE_CACHE_USED)
    {
      if ((error=flush_io_cache(&info->rec_cache)))
      {
        mi_print_error(info->s, HA_ERR_CRASHED);
	mi_mark_crashed(info);			/* Fatal error found */
      }
    }
    break;
  case HA_EXTRA_NO_READCHECK:
    info->opt_flag&= ~READ_CHECK_USED;		/* No readcheck */
    break;
  case HA_EXTRA_READCHECK:
    info->opt_flag|= READ_CHECK_USED;
    break;
  case HA_EXTRA_KEYREAD:			/* Read only keys to record */
  case HA_EXTRA_REMEMBER_POS:
    info->opt_flag |= REMEMBER_OLD_POS;
    bmove((uchar*) info->lastkey+share->base.max_key_length*2,
	  (uchar*) info->lastkey,info->lastkey_length);
    info->save_update=	info->update;
    info->save_lastinx= info->lastinx;
    info->save_lastpos= info->lastpos;
    info->save_lastkey_length=info->lastkey_length;
    if (function == HA_EXTRA_REMEMBER_POS)
      break;
    /* fall through */
  case HA_EXTRA_KEYREAD_CHANGE_POS:
    info->opt_flag |= KEY_READ_USED;
    info->read_record=_mi_read_key_record;
    break;
  case HA_EXTRA_NO_KEYREAD:
  case HA_EXTRA_RESTORE_POS:
    if (info->opt_flag & REMEMBER_OLD_POS)
    {
      bmove((uchar*) info->lastkey,
	    (uchar*) info->lastkey+share->base.max_key_length*2,
	    info->save_lastkey_length);
      info->update=	info->save_update | HA_STATE_WRITTEN;
      info->lastinx=	info->save_lastinx;
      info->lastpos=	info->save_lastpos;
      info->lastkey_length=info->save_lastkey_length;
    }
    info->read_record=	share->read_record;
    info->opt_flag&= ~(KEY_READ_USED | REMEMBER_OLD_POS);
    break;
  case HA_EXTRA_NO_USER_CHANGE: /* Database is locked preventing changes */
    info->lock_type= F_EXTRA_LCK; /* Simulate as locked */
    break;
  case HA_EXTRA_WAIT_LOCK:
    info->lock_wait=0;
    break;
  case HA_EXTRA_NO_WAIT_LOCK:
    info->lock_wait= MY_SHORT_WAIT;
    break;
  case HA_EXTRA_NO_KEYS:
    if (info->lock_type == F_UNLCK)
    {
      error=1;					/* Not possibly if not lock */
      break;
    }
    if (mi_is_any_key_active(share->state.key_map))
    {
      if (share->state.key_map != *(ulonglong*)extra_arg)
        info->update|= HA_STATE_CHANGED;
      share->state.key_map= *(ulonglong*)extra_arg;

      if (!share->changed)
      {
	share->state.changed|= STATE_CHANGED | STATE_NOT_ANALYZED;
	share->changed=1;			/* Update on close */
	if (!share->global_changed)
	{
	  share->global_changed=1;
	  share->state.open_count++;
	}
      }
      share->state.state= *info->state;
      error=mi_state_info_write(share->kfile,&share->state,1 | 2);
    }
    break;
  case HA_EXTRA_FORCE_REOPEN:
    mysql_mutex_lock(&THR_LOCK_myisam);
    share->last_version= 0L;			/* Impossible version */
    mysql_mutex_unlock(&THR_LOCK_myisam);
    break;
  case HA_EXTRA_PREPARE_FOR_DROP:
    /* Signals about intent to delete this table */
    share->deleting= TRUE;
    share->global_changed= FALSE;     /* force writing changed flag */
    _mi_mark_file_changed(info);
    if (share->temporary)
      break;
    /* fall through */
  case HA_EXTRA_PREPARE_FOR_RENAME:
    DBUG_ASSERT(!share->temporary);
    mysql_mutex_lock(&THR_LOCK_myisam);
    share->last_version= 0L;			/* Impossible version */
    mysql_mutex_lock(&share->intern_lock);
    /* Flush pages that we don't need anymore */
    if (flush_key_blocks(share->key_cache, share->kfile,
                         &share->dirty_part_map,
			 (function == HA_EXTRA_PREPARE_FOR_DROP ?
                          FLUSH_IGNORE_CHANGED : FLUSH_RELEASE)))
    {
      error=my_errno;
      share->changed=1;
      mi_print_error(info->s, HA_ERR_CRASHED);
      mi_mark_crashed(info);			/* Fatal error found */
    }
    mysql_mutex_unlock(&share->intern_lock);
    mysql_mutex_unlock(&THR_LOCK_myisam);
    break;
  case HA_EXTRA_END_ALTER_COPY:
  case HA_EXTRA_FLUSH:
    mysql_mutex_lock(&share->intern_lock);
    if (!share->temporary)
    {
      if (flush_key_blocks(share->key_cache, share->kfile, &share->dirty_part_map,
                           FLUSH_KEEP))
        error= my_errno;
      /*
        Check if this is the primary table holding the state of the table
        If there are multiple usage of the same table, only one is holding
        the true state.
      */
      if (info->state == &info->save_state)
      {
        info->s->state.state= *info->state;
        if (info->opt_flag & WRITE_CACHE_USED)
        {
          if ((error2= my_b_flush_io_cache(&info->rec_cache, 0)))
            error= error2;
        }
        if ((error2= mi_state_info_write(share->kfile, &share->state, 1)))
          error= error2;
      }
    }
    /* Tell mi_lock_database() that we locked the intern_lock mutex */
    info->intern_lock_locked= 1;
    _mi_decrement_open_count(info);
    info->intern_lock_locked= 0;
    if (share->not_flushed)
    {
      share->not_flushed=0;
      if (mysql_file_sync(share->kfile, MYF(0)))
	error= my_errno;
      if (mysql_file_sync(info->dfile, MYF(0)))
	error= my_errno;
      if (error)
      {
	share->changed=1;
        mi_print_error(info->s, HA_ERR_CRASHED);
	mi_mark_crashed(info);			/* Fatal error found */
      }
    }
    if (share->base.blobs)
      mi_alloc_rec_buff(info, -1, &info->rec_buff);
    mysql_mutex_unlock(&share->intern_lock);
    break;
  case HA_EXTRA_NORMAL:				/* These aren't in use */
    info->quick_mode=0;
    break;
  case HA_EXTRA_QUICK:
    info->quick_mode=1;
    break;
  case HA_EXTRA_NO_ROWS:
    if (!share->state.header.uniques)
      info->opt_flag|= OPT_NO_ROWS;
    break;
  case HA_EXTRA_PRELOAD_BUFFER_SIZE:
    info->preload_buff_size= *((ulong *) extra_arg); 
    break;
  case HA_EXTRA_CHANGE_KEY_TO_UNIQUE:
  case HA_EXTRA_CHANGE_KEY_TO_DUP:
    mi_extra_keyflag(info, function);
    break;
  case HA_EXTRA_MMAP:
#if defined(HAVE_MMAP) && !defined(HAVE_valgrind)
    mysql_mutex_lock(&share->intern_lock);
    /*
      Memory map the data file if it is not already mapped. It is safe
      to memory map a file while other threads are using file I/O on it.
      Assigning a new address to a function pointer is an atomic
      operation. intern_lock prevents that two or more mappings are done
      at the same time.
    */
    if (!share->file_map)
    {
      if (mi_dynmap_file(info, share->state.state.data_file_length))
      {
        DBUG_PRINT("warning",("mmap failed: errno: %d",errno));
        error= my_errno= errno;
      }
    }
    mysql_mutex_unlock(&share->intern_lock);
#endif
    break;
  case HA_EXTRA_MARK_AS_LOG_TABLE:
    mysql_mutex_lock(&share->intern_lock);
    share->is_log_table= TRUE;
    mysql_mutex_unlock(&share->intern_lock);
    break;
  case HA_EXTRA_DETACH_CHILD: /* When used with MERGE tables */
    info->open_flag&=     ~HA_OPEN_MERGE_TABLE;
    info->lock.priority&= ~THR_LOCK_MERGE_PRIV;
    break;
    
  case HA_EXTRA_KEY_CACHE:
  case HA_EXTRA_NO_KEY_CACHE:
  default:
    break;
  }
  {
    char tmp[1];
    tmp[0]=function;
    myisam_log_command(MI_LOG_EXTRA,info,(uchar*) tmp,1,error);
  }
  DBUG_RETURN(error);
} /* mi_extra */

void mi_set_index_cond_func(MI_INFO *info, index_cond_func_t func,
                            void *func_arg)
{
  info->index_cond_func= func;
  info->index_cond_func_arg= func_arg;
  info->has_cond_pushdown= (info->index_cond_func || info->rowid_filter_func);
}

void mi_set_rowid_filter_func(MI_INFO *info,
                              rowid_filter_func_t check_func,
                              void *func_arg)
{
  info->rowid_filter_func= check_func;
  info->rowid_filter_func_arg= func_arg;
  info->has_cond_pushdown= (info->index_cond_func || info->rowid_filter_func);
}

/*
    Start/Stop Inserting Duplicates Into a Table, WL#1648.
 */
static void mi_extra_keyflag(MI_INFO *info, enum ha_extra_function function)
{
  uint  idx;

  for (idx= 0; idx< info->s->base.keys; idx++)
  {
    switch (function) {
    case HA_EXTRA_CHANGE_KEY_TO_UNIQUE:
      info->s->keyinfo[idx].flag|= HA_NOSAME;
      break;
    case HA_EXTRA_CHANGE_KEY_TO_DUP:
      info->s->keyinfo[idx].flag&= ~(HA_NOSAME);
      break;
    default:
      break;
    }
  }
}


int mi_reset(MI_INFO *info)
{
  int error= 0;
  MYISAM_SHARE *share=info->s;
  DBUG_ENTER("mi_reset");
  /*
    Free buffers and reset the following flags:
    EXTRA_CACHE, EXTRA_WRITE_CACHE, EXTRA_KEYREAD, EXTRA_QUICK

    If the row buffer cache is large (for dynamic tables), reduce it
    to save memory.
  */
  if (info->opt_flag & (READ_CACHE_USED | WRITE_CACHE_USED))
  {
    info->opt_flag&= ~(READ_CACHE_USED | WRITE_CACHE_USED);
    error= end_io_cache(&info->rec_cache);
  }
  if (share->base.blobs)
    mi_alloc_rec_buff(info, -1, &info->rec_buff);
#if defined(HAVE_MMAP) && defined(HAVE_MADVISE)
  if (info->opt_flag & MEMMAP_USED)
    madvise((char*) share->file_map, share->state.state.data_file_length,
            MADV_RANDOM);
#endif
  info->opt_flag&= ~(KEY_READ_USED | REMEMBER_OLD_POS);
  info->quick_mode=0;
  info->lastinx= 0;			/* Use first index as def */
  info->last_search_keypage= info->lastpos= HA_OFFSET_ERROR;
  info->page_changed= 1;
  info->update= ((info->update & HA_STATE_CHANGED) | HA_STATE_NEXT_FOUND |
                 HA_STATE_PREV_FOUND);
  DBUG_RETURN(error);
}

my_bool mi_killed_standalone(MI_INFO *info __attribute__((unused)))
{
  return 0;
}
