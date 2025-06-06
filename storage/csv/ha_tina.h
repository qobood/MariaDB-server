/* Copyright (c) 2003, 2011, Oracle and/or its affiliates. All rights reserved.

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

#include <sys/types.h>
#include <sys/stat.h>
#include <my_dir.h>
#include "transparent_file.h"

#define DEFAULT_CHAIN_LENGTH 512
/*
  Version for file format.
  1 - Initial Version. That is, the version when the metafile was introduced.
*/

#define TINA_VERSION 1

typedef struct st_tina_share {
  char *table_name;
  char data_file_name[FN_REFLEN];
  uint table_name_length, use_count;
  /*
    Below flag is needed to make log tables work with concurrent insert.
    For more details see comment to ha_tina::update_status.
  */
  my_bool is_log_table;
  /*
    Here we save the length of the file for readers. This is updated by
    inserts, updates and deletes. The var is initialized along with the
    share initialization.
  */
  my_off_t saved_data_file_length;
  mysql_mutex_t mutex;
  THR_LOCK lock;
  bool update_file_opened;
  bool tina_write_opened;
  File meta_file;           /* Meta file we use */
  File tina_write_filedes;  /* File handler for readers */
  bool crashed;             /* Meta file is crashed */
  ha_rows rows_recorded;    /* Number of rows in tables */
  uint data_file_version;   /* Version of the data file used */
} TINA_SHARE;

struct tina_set {
  my_off_t begin;
  my_off_t end;
};

class ha_tina final : public handler
{
  THR_LOCK_DATA lock;      /* MySQL lock */
  TINA_SHARE *share;       /* Shared lock info */
  my_off_t current_position;  /* Current position in the file during a file scan */
  my_off_t next_position;     /* Next position in the file scan */
  my_off_t local_saved_data_file_length; /* save position for reads */
  my_off_t temp_file_length;
  uchar byte_buffer[IO_SIZE];
  Transparent_file *file_buff;
  File data_file;                   /* File handler for readers */
  File update_temp_file;
  String buffer;
  /*
    The chain contains "holes" in the file, occurred because of
    deletes/updates. It is used in rnd_end() to get rid of them
    in the end of the query.
  */
  tina_set chain_buffer[DEFAULT_CHAIN_LENGTH];
  tina_set *chain;
  tina_set *chain_ptr;
  uchar chain_alloced;
  uint32 chain_size;
  uint local_data_file_version;  /* Saved version of the data file used */
  bool records_is_known, found_end_of_file;
  MEM_ROOT blobroot;

private:
  int curr_lock_type;

  bool get_write_pos(my_off_t *end_pos, tina_set *closest_hole);
  int open_update_temp_file_if_needed();
  int init_tina_writer();
  int init_data_file();

public:
  ha_tina(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_tina()
  {
    if (chain_alloced)
      my_free(chain);
    if (file_buff)
      delete file_buff;
    free_root(&blobroot, MYF(0));
  }
  ulonglong table_flags() const override
  {
    return (HA_NO_TRANSACTIONS | HA_REC_NOT_IN_SEQ | HA_NO_AUTO_INCREMENT |
            HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE | HA_CAN_EXPORT |
            HA_CAN_REPAIR | HA_SLOW_RND_POS);
  }
  ulong index_flags(uint idx, uint part, bool all_parts) const override
  {
    /*
      We will never have indexes so this will never be called(AKA we return
      zero)
    */
    return 0;
  }
  uint max_record_length() const { return HA_MAX_REC_LENGTH; }
  uint max_keys()          const { return 0; }
  uint max_key_parts()     const { return 0; }
  uint max_key_length()    const { return 0; }
  /*
     Called in test_quick_select to determine if indexes should be used.
   */
  IO_AND_CPU_COST scan_time() override
  {
    return
    { (double) ((share->saved_data_file_length + IO_SIZE-1))/ IO_SIZE,
        (stats.records+stats.deleted) * ROW_NEXT_FIND_COST };
  }
  /* The next method will never be called */
  virtual bool fast_key_read() { return 1;}
  /* 
    TODO: return actual upper bound of number of records in the table.
    (e.g. save number of records seen on full table scan and/or use file size
    as upper bound)
  */
  ha_rows estimate_rows_upper_bound() override { return HA_POS_ERROR; }

  int open(const char *name, int mode, uint open_options) override;
  int close(void) override;
  int write_row(const uchar * buf) override;
  int update_row(const uchar * old_data, const uchar * new_data) override;
  int delete_row(const uchar * buf) override;
  int rnd_init(bool scan=1) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar * buf, uchar *pos) override;
  bool check_and_repair(THD *thd) override;
  int check(THD* thd, HA_CHECK_OPT* check_opt) override;
  bool is_crashed() const override;
  int rnd_end() override;
  int repair(THD* thd, HA_CHECK_OPT* check_opt) override;
  /* This is required for SQL layer to know that we support autorepair */
  bool auto_repair(int error) const override
  { return error == HA_ERR_CRASHED_ON_USAGE; }
  bool auto_repair() const { return 1; }
  void position(const uchar *record) override;
  int info(uint) override;
  int reset() override;
  int extra(enum ha_extra_function operation) override;
  int delete_all_rows(void) override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info) override;
  bool check_if_incompatible_data(HA_CREATE_INFO *info,
                                  uint table_changes) override;

  int external_lock(THD *thd, int lock_type) override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
      enum thr_lock_type lock_type) override;

  /*
    These functions used to get/update status of the handler.
    Needed to enable concurrent inserts.
  */
  void get_status();
  void update_status();

  /* The following methods were added just for TINA */
  int encode_quote(const uchar *buf);
  int find_current_row(uchar *buf);
  int chain_append();
};

