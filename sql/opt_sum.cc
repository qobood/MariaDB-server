/* Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (c) 2008, 2021, MariaDB Corporation.

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


/**
  @file

  Optimising of MIN(), MAX() and COUNT(*) queries without 'group by' clause
  by replacing the aggregate expression with a constant.  

  Given a table with a compound key on columns (a,b,c), the following
  types of queries are optimised (assuming the table handler supports
  the required methods)

  @verbatim
  SELECT COUNT(*) FROM t1[,t2,t3,...]
  SELECT MIN(b) FROM t1 WHERE a=const
  SELECT MAX(c) FROM t1 WHERE a=const AND b=const
  SELECT MAX(b) FROM t1 WHERE a=const AND b<const
  SELECT MIN(b) FROM t1 WHERE a=const AND b>const
  SELECT MIN(b) FROM t1 WHERE a=const AND b BETWEEN const AND const
  SELECT MAX(b) FROM t1 WHERE a=const AND b BETWEEN const AND const
  @endverbatim

  Instead of '<' one can use '<=', '>', '>=' and '=' as well.
  Instead of 'a=const' the condition 'a IS NULL' can be used.

  If all selected fields are replaced then we will also remove all
  involved tables and return the answer without any join. Thus, the
  following query will be replaced with a row of two constants:
  @verbatim
  SELECT MAX(b), MIN(d) FROM t1,t2 
    WHERE a=const AND b<const AND d>const
  @endverbatim
  (assuming a index for column d of table t2 is defined)
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "key.h"                                // key_cmp_if_same
#include "sql_select.h"

static bool find_key_for_maxmin(bool max_fl, TABLE_REF *ref, Field* field,
                                COND *cond, uint *range_fl,
                                uint *key_prefix_length, bool *reverse);
static int reckey_in_range(bool max_fl, TABLE_REF *ref, Field* field,
                            COND *cond, uint range_fl, uint prefix_len);
static int maxmin_in_range(bool max_fl, Field* field, COND *cond);


/*
  Get exact count of rows in all tables

  SYNOPSIS
    get_exact_records()
    tables		List of tables

  NOTES
    When this is called, we know all table handlers supports HA_HAS_RECORDS
    or HA_STATS_RECORDS_IS_EXACT

  RETURN
    ULONGLONG_MAX	Error: Could not calculate number of rows
    #			Multiplication of number of rows in all tables
*/

static ulonglong get_exact_record_count(List<TABLE_LIST> &tables)
{
  ulonglong count= 1;
  TABLE_LIST *tl;
  List_iterator<TABLE_LIST> ti(tables);
  while ((tl= ti++))
  {
    ha_rows tmp= tl->table->file->records();
    if (tmp == HA_POS_ERROR)
      return ULONGLONG_MAX;
    count*= tmp;
  }
  return count;
}


/**
  Use index to read MIN(field) value.
  
  @param table      Table object
  @param ref        Reference to the structure where we store the key value
  @item_field       Field used in MIN()
  @range_fl         Whether range endpoint is strict less than
  @prefix_len       Length of common key part for the range
  @reverse          Whether key part used is reverse descending index
  
  @retval
    0               No errors
    HA_ERR_...      Otherwise
*/

static int get_index_min_value(TABLE *table, TABLE_REF *ref,
                               Item_field *item_field, uint range_fl,
                               uint prefix_len, bool reverse)
{
  int error;
  
  if (!ref->key_length)
  {
    error= reverse ? table->file->ha_index_last(table->record[0]) :
                     table->file->ha_index_first(table->record[0]);
  }
  else 
  {
    /*
      Use index to replace MIN/MAX functions with their values
      according to the following rules:

      1) Insert the minimum non-null values where the WHERE clause still
         matches, or
      2) a NULL value if there are only NULL values for key_part_k.
      3) Fail, producing a row of nulls

      Implementation: Read the smallest value using the search key. If
      the interval is open, read the next value after the search
      key. If read fails, and we're looking for a MIN() value for a
      nullable column, test if there is an exact match for the key.
    */
    if (!(range_fl & NEAR_MIN))
      /* 
         Closed interval: Either The MIN argument is non-nullable, or
         we have a >= predicate for the MIN argument.
      */
      error= table->file->ha_index_read_map(table->record[0],
                                            ref->key_buff,
                                            make_prev_keypart_map(ref->key_parts),
                                            reverse ?
                                            HA_READ_PREFIX_LAST_OR_PREV :
                                            HA_READ_KEY_OR_NEXT);
    else
    {
      /*
        Open interval: There are two cases:
        1) We have only MIN() and the argument column is nullable, or
        2) there is a > predicate on it, nullability is irrelevant.
        We need to scan the next bigger record first.
        Open interval is not used if the search key involves the last keypart,
        and it would not work.
      */
      DBUG_ASSERT(prefix_len < ref->key_length);
      error= table->file->ha_index_read_map(table->record[0],
                                            ref->key_buff,
                                            make_prev_keypart_map(ref->key_parts),
                                            reverse ? HA_READ_BEFORE_KEY :
                                            HA_READ_AFTER_KEY);
      /* 
         If the found record is outside the group formed by the search
         prefix, or there is no such record at all, check if all
         records in that group have NULL in the MIN argument
         column. If that is the case return that NULL.

         Check if case 1 from above holds. If it does, we should read
         the skipped tuple.
      */
      if (item_field->field->real_maybe_null() &&
          ref->key_buff[prefix_len] == 1 &&
          /*
            Last keypart (i.e. the argument to MIN) is set to NULL by
            find_key_for_maxmin only if all other keyparts are bound
            to constants in a conjunction of equalities. Hence, we
            can detect this by checking only if the last keypart is
            NULL.
          */
          (error == HA_ERR_KEY_NOT_FOUND ||
           key_cmp_if_same(table, ref->key_buff, ref->key, prefix_len)))
      {
        DBUG_ASSERT(item_field->field->real_maybe_null());
        error= table->file->ha_index_read_map(table->record[0],
                                              ref->key_buff,
                                              make_prev_keypart_map(ref->key_parts),
                                              HA_READ_KEY_EXACT);
      }
    }
  }
  return error;
}


/**
  Use index to read MAX(field) value.
  
  @param table      Table object
  @param ref        Reference to the structure where we store the key value
  @range_fl         Whether range endpoint is strict greater than
  @reverse          Whether the key part used is reverse descending index
  
  @retval
    0               No errors
    HA_ERR_...      Otherwise
*/

static int get_index_max_value(TABLE *table, TABLE_REF *ref, uint range_fl,
                               bool reverse)
{
  if (ref->key_length)
  {
    return table->file->ha_index_read_map(table->record[0], ref->key_buff,
                                          make_prev_keypart_map(ref->key_parts),
                                          range_fl & NEAR_MAX ?
                                          (reverse ? HA_READ_AFTER_KEY :
                                           HA_READ_BEFORE_KEY) :
                                          (reverse ? HA_READ_KEY_OR_NEXT :
                                           HA_READ_PREFIX_LAST_OR_PREV));
  }
  else
  {
    return reverse ? table->file->ha_index_first(table->record[0]) :
                     table->file->ha_index_last(table->record[0]);
  }
}



/**
  Substitutes constants for some COUNT(), MIN() and MAX() functions.

  @param thd                   thread handler
  @param tables                list of leaves of join table tree
  @param all_fields            All fields to be returned
  @param conds                 WHERE clause

  @note
    This function is only called for queries with aggregate functions and no
    GROUP BY part. This means that the result set shall contain a single
    row only

  @retval
    0                    no errors
  @retval
    1                    if all items were resolved
  @retval
    HA_ERR_KEY_NOT_FOUND on impossible conditions
  @retval
    HA_ERR_... if a deadlock or a lock wait timeout happens, for example
  @retval
    ER_...     e.g. ER_SUBQUERY_NO_1_ROW
*/

int opt_sum_query(THD *thd,
                  List<TABLE_LIST> &tables, List<Item> &all_fields, COND *conds)
{
  List_iterator_fast<Item> it(all_fields);
  List_iterator<TABLE_LIST> ti(tables);
  TABLE_LIST *tl;
  int const_result= 1;
  bool recalc_const_item= 0;
  ulonglong count= 1;
  bool is_exact_count= TRUE, maybe_exact_count= TRUE;
  table_map removed_tables= 0, outer_tables= 0, used_tables= 0;
  table_map where_tables= 0;
  Item *item;
  int error= 0;
  DBUG_ENTER("opt_sum_query");

  thd->lex->current_select->min_max_opt_list.empty();

  if (conds)
    where_tables= conds->used_tables();

  /*
    Analyze outer join dependencies, and, if possible, compute the number
    of returned rows.
  */
  while ((tl= ti++))
  {
    TABLE_LIST *embedded;
    for (embedded= tl ; embedded; embedded= embedded->embedding)
    {
      if (embedded->on_expr)
        break;
    }
    if (embedded)
    /* Don't replace expression on a table that is part of an outer join */
    {
      outer_tables|= tl->table->map;

      /*
        We can't optimise LEFT JOIN in cases where the WHERE condition
        restricts the table that is used, like in:
          SELECT MAX(t1.a) FROM t1 LEFT JOIN t2 join-condition
          WHERE t2.field IS NULL;
      */
      if (tl->table->map & where_tables)
        DBUG_RETURN(0);
    }
    else
      used_tables|= tl->table->map;

    /*
      If the storage manager of 'tl' gives exact row count as part of
      statistics (cheap), compute the total number of rows. If there are
      no outer table dependencies, this count may be used as the real count.
      Schema tables are filled after this function is invoked, so we can't
      get row count 
    */
    if (!(tl->table->file->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT) ||
        tl->schema_table)
    {
      maybe_exact_count&= MY_TEST(!tl->schema_table &&
                                  (tl->table->file->ha_table_flags() &
                                   HA_HAS_RECORDS));
      is_exact_count= FALSE;
      count= 1;                                 // ensure count != 0
    }
    else if (tl->is_materialized_derived() || 
             tl->jtbm_subselect)
    {
      /*
        Can't remove a derived table as it's number of rows is just an
        estimate.
      */
      DBUG_RETURN(0);
    }
    else
    {
      error= tl->table->file->info(HA_STATUS_VARIABLE | HA_STATUS_NO_LOCK);
      if (unlikely(error))
      {
        tl->table->file->print_error(error, MYF(ME_FATAL));
        DBUG_RETURN(error);
      }
      count*= tl->table->file->stats.records;
    }
  }

  /*
    Iterate through all items in the SELECT clause and replace
    COUNT(), MIN() and MAX() with constants (if possible).
  */

  while ((item= it++))
  {
    if (item->type() == Item::SUM_FUNC_ITEM)
    {
      Item_sum *item_sum= (((Item_sum*) item));
      switch (item_sum->sum_func()) {
      case Item_sum::COUNT_FUNC:
        /*
          If the expr in COUNT(expr) can never be null we can change this
          to the number of rows in the tables if this number is exact and
          there are no outer joins.
        */
        if (!conds && !((Item_sum_count*) item)->get_arg(0)->maybe_null() &&
            !outer_tables && maybe_exact_count &&
            ((item->used_tables() & OUTER_REF_TABLE_BIT) == 0))
        {
          if (!is_exact_count)
          {
            if ((count= get_exact_record_count(tables)) == ULONGLONG_MAX)
            {
              /* Error from handler in counting rows. Don't optimize count() */
              const_result= 0;
              continue;
            }
            is_exact_count= 1;                  // count is now exact
          }
          ((Item_sum_count*) item)->make_const((longlong) count);
          recalc_const_item= 1;
        }
        else
          const_result= 0;
        break;
      case Item_sum::MIN_FUNC:
      case Item_sum::MAX_FUNC:
      {
        int is_max= MY_TEST(item_sum->sum_func() == Item_sum::MAX_FUNC);
        /*
          If MIN/MAX(expr) is the first part of a key or if all previous
          parts of the key is found in the COND, then we can use
          indexes to find the key.
        */
        Item *expr=item_sum->get_arg(0);
        if (((expr->used_tables() & OUTER_REF_TABLE_BIT) == 0) &&
            expr->real_item()->type() == Item::FIELD_ITEM)
        {
          uchar key_buff[MAX_KEY_LENGTH];
          TABLE_REF ref;
          uint range_fl, prefix_len;
          bool reverse= false;

          ref.key_buff= key_buff;
          Item_field *item_field= (Item_field*) (expr->real_item());
          TABLE *table= item_field->field->table;

          /* 
            Look for a partial key that can be used for optimization.
            If we succeed, ref.key_length will contain the length of
            this key, while prefix_len will contain the length of 
            the beginning of this key without field used in MIN/MAX(). 
            Type of range for the key part for this field will be
            returned in range_fl.
          */
          if (table->file->inited || (outer_tables & table->map) ||
              !find_key_for_maxmin(is_max, &ref, item_field->field, conds,
                                   &range_fl, &prefix_len, &reverse))
          {
            const_result= 0;
            break;
          }
          longlong info_limit= 1;
          error= 0;

          table->file->info_push(INFO_KIND_FORCE_LIMIT_BEGIN, &info_limit);
          if (!table->const_table)
          {
            if (likely(!(error= table->file->ha_index_init((uint) ref.key,
                                                           1))))
              error= (is_max ?
                      get_index_max_value(table, &ref, range_fl, reverse) :
                      get_index_min_value(table, &ref, item_field, range_fl,
                                          prefix_len, reverse));
          }
          /* Verify that the read tuple indeed matches the search key */
	  if (!error &&
              reckey_in_range(is_max, &ref, item_field->field,
                              conds, range_fl, prefix_len))
	    error= HA_ERR_KEY_NOT_FOUND;
          if (!table->const_table)
          {
            table->file->ha_end_keyread();
            table->file->ha_index_end();
          }
          table->file->info_push(INFO_KIND_FORCE_LIMIT_END, NULL);
          if (error)
	  {
	    if (error == HA_ERR_KEY_NOT_FOUND || error == HA_ERR_END_OF_FILE)
	      DBUG_RETURN(HA_ERR_KEY_NOT_FOUND); // No rows matching WHERE
	    /* HA_ERR_LOCK_DEADLOCK or some other error */
 	    table->file->print_error(error, MYF(0));
            DBUG_RETURN(error);
	  }
          removed_tables|= table->map;
        }
        else if (!expr->const_item() || !is_exact_count || conds)
        {
          /*
            The optimization is not applicable in both cases:
            (a) 'expr' is a non-constant expression. Then we can't
            replace 'expr' by a constant.
            (b) 'expr' is a constant. According to ANSI, MIN/MAX must return
            NULL if the query does not return any rows. Thus, if we are not
            able to determine if the query returns any rows, we can't apply
            the optimization and replace MIN/MAX with a constant.
            (c) there is a WHERE clause. The WHERE conditions may result in
            an empty result, but the clause cannot be taken into account here.
          */
          const_result= 0;
          break;
        }
        item_sum->set_aggregator(thd,
                                 item_sum->has_with_distinct() ?
                                 Aggregator::DISTINCT_AGGREGATOR :
                                 Aggregator::SIMPLE_AGGREGATOR);
        /*
          If count == 0 (so is_exact_count == TRUE) and
          there're no outer joins, set to NULL,
          otherwise set to the constant value.
        */
        if (!count && !outer_tables)
        {
          item_sum->aggregator_clear();
        }
        else
        {
          item_sum->reset_and_add();
          /*
            Save a reference to the item for possible rollback
            of the min/max optimizations for this select
          */
	  thd->lex->current_select->min_max_opt_list.push_back(item_sum);
        }
        item_sum->make_const();
        recalc_const_item= 1;
        break;
      }
      default:
        const_result= 0;
        break;
      }
    }
    else if (const_result)
    {
      if (recalc_const_item)
        item->update_used_tables();
      if (!item->const_item() && item->type() != Item::WINDOW_FUNC_ITEM)
        const_result= 0;
    }
  }

  if (unlikely(thd->is_error()))
    DBUG_RETURN(thd->get_stmt_da()->sql_errno());

  /*
    If we have a where clause, we can only ignore searching in the
    tables if MIN/MAX optimisation replaced all used tables
    We do not use replaced values in case of:
    SELECT MIN(key) FROM table_1, empty_table
    removed_tables is != 0 if we have used MIN() or MAX().
  */
  if (removed_tables && used_tables != removed_tables)
    const_result= 0;                            // We didn't remove all tables
  DBUG_RETURN(const_result);
}


/*
  Check if both item1 and item2 are strings, and item1 has fewer characters 
  than item2.
*/

static bool check_item1_shorter_item2(Item *item1, Item *item2)
{
  if (item1->cmp_type() == STRING_RESULT &&
      item2->cmp_type() == STRING_RESULT)
  {
    int len1= item1->max_length / item1->collation.collation->mbmaxlen;
    int len2= item2->max_length / item2->collation.collation->mbmaxlen;
    return len1 < len2;
  }
  return false;  /* When the check is not applicable, it means "not bigger" */
}


/**
  Test if the predicate compares a field with constants.

  @param func_item        Predicate item
  @param[out] args        Here we store the field followed by constants
  @param[out] inv_order   Is set to 1 if the predicate is of the form
                          'const op field'

  @retval
    0      func_item is a simple predicate: a field is compared with a constant
           whose length does not exceed the max length of the field values  
  @retval
    1        Otherwise
*/

bool simple_pred(Item_func *func_item, Item **args, bool *inv_order)
{
  Item *item;
  *inv_order= 0;
  switch (func_item->argument_count()) {
  case 0:
    /* MULT_EQUAL_FUNC */
    {
      Item_equal *item_equal= (Item_equal *) func_item;
      if (!(args[1]= item_equal->get_const()))
        return 0;
      Item_equal_fields_iterator it(*item_equal);
      if (!(item= it++))
        return 0;
      args[0]= item->real_item();
      if (check_item1_shorter_item2(args[0], args[1]))
        return 0;
      if (it++)
        return 0;
    }
    break;
  case 1:
    /* field IS NULL */
    item= func_item->arguments()[0]->real_item();
    if (item->type() != Item::FIELD_ITEM)
      return 0;
    args[0]= item;
    break;
  case 2:
    /* 'field op const' or 'const op field' */
    item= func_item->arguments()[0]->real_item();
    if (item->type() == Item::FIELD_ITEM)
    {
      args[0]= item;
      item= func_item->arguments()[1]->real_item();
      if (!item->const_item())
        return 0;
      args[1]= item;
    }
    else if (item->const_item())
    {
      args[1]= item;
      item= func_item->arguments()[1]->real_item();
      if (item->type() != Item::FIELD_ITEM)
        return 0;
      args[0]= item;
      *inv_order= 1;
    }
    else
      return 0;
    if (check_item1_shorter_item2(args[0], args[1]))
      return 0;
    break;
  case 3:
    /* field BETWEEN const AND const */
    item= func_item->arguments()[0]->real_item();
    if (item->type() == Item::FIELD_ITEM)
    {
      args[0]= item;
      for (int i= 1 ; i <= 2; i++)
      {
        item= func_item->arguments()[i]->real_item();
        if (!item->const_item())
          return 0;
        args[i]= item;
        if (check_item1_shorter_item2(args[0], args[i]))
          return 0;
      }
    }
    else
      return 0;
  }
  return 1;
}


/**
  Check whether a condition matches a key to get {MAX|MIN}(field):.

   For the index specified by the keyinfo parameter and an index that
   contains the field as its component (field_part), the function
   checks whether 

   - the condition cond is a conjunction, 
   - all of its conjuncts refer to columns of the same table, and
   - each conjunct is on one of the following forms:
     - f_i = const_i or const_i = f_i or f_i IS NULL,
       where f_i is part of the index
     - field {<|<=|>=|>|=} const
     - const {<|<=|>=|>|=} field
     - field BETWEEN const_1 AND const_2

   As a side-effect, the key value to be used for looking up the MIN/MAX value
   is actually stored inside the Field object. An interesting feature is that
   the function will find the most restrictive endpoint by over-eager
   evaluation of the @c WHERE condition. It continually stores the current
   endpoint inside the Field object. For a query such as

   @code
   SELECT MIN(a) FROM t1 WHERE a > 3 AND a > 5;
   @endcode

   the algorithm will recurse over the conjuction, storing first a 3 in the
   field. In the next recursive invocation the expression a > 5 is evaluated
   as 3 > 5 (Due to the dual nature of Field objects as value carriers and
   field identifiers), which will obviously fail, leading to 5 being stored in
   the Field object.
   
   @param[in]     max_fl         Set to true if we are optimizing MAX(),
                                 false means we are optimizing %MIN()
   @param[in, out] ref           Reference to the structure where the function 
                                 stores the key value
   @param[in]     keyinfo        Reference to the key info
   @param[in]     field_part     Pointer to the key part for the field
   @param[in]     cond           WHERE condition
   @param[in,out] key_part_used  Map of matchings parts. The function will output
                                 the set of key parts actually being matched in 
                                 this set, yet it relies on the caller to 
                                 initialize the value to zero. This is due 
                                 to the fact that this value is passed 
                                 recursively.
   @param[in,out] range_fl       Says whether endpoints use strict greater/less 
                                 than.
   @param[out]    prefix_len     Length of common key part for the range
                                 where MAX/MIN is searched for

  @retval
    false    Index can't be used.
  @retval
    true     We can use the index to get MIN/MAX value
*/

static bool matching_cond(bool max_fl, TABLE_REF *ref, KEY *keyinfo, 
                          KEY_PART_INFO *field_part, COND *cond,
                          key_part_map *key_part_used, uint *range_fl,
                          uint *prefix_len)
{
  DBUG_ENTER("matching_cond");
  if (!cond)
    DBUG_RETURN(TRUE);
  Field *field= field_part->field;
  table_map cond_used_tables= cond->used_tables();
  if (cond_used_tables & OUTER_REF_TABLE_BIT)
  { 
    DBUG_RETURN(FALSE);
  } 
  if (!(cond_used_tables & field->table->map) &&
      MY_TEST(cond_used_tables & ~PSEUDO_TABLE_BITS))
  {
    /* Condition doesn't restrict the used table */
    DBUG_RETURN(!cond->const_item());
  }
  else if (cond->is_expensive())
    DBUG_RETURN(FALSE);
  if (cond->type() == Item::COND_ITEM)
  {
    if (((Item_cond*) cond)->functype() == Item_func::COND_OR_FUNC)
      DBUG_RETURN(FALSE);

    /* AND */
    List_iterator_fast<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    while ((item= li++))
    {
      if (!matching_cond(max_fl, ref, keyinfo, field_part, item,
                         key_part_used, range_fl, prefix_len))
        DBUG_RETURN(FALSE);
    }
    DBUG_RETURN(TRUE);
  }

  if (cond->type() != Item::FUNC_ITEM)
    DBUG_RETURN(FALSE);                                 // Not operator, can't optimize

  bool eq_type= 0;                            // =, <=> or IS NULL
  bool is_null_safe_eq= FALSE;                // The operator is NULL safe, e.g. <=> 
  bool noeq_type= 0;                          // < or >  
  bool less_fl= 0;                            // < or <= 
  bool is_null= 0;                            // IS NULL
  bool between= 0;                            // BETWEEN ... AND ... 

  switch (((Item_func*) cond)->functype()) {
  case Item_func::ISNULL_FUNC:
    is_null= 1;     /* fall through */
  case Item_func::EQ_FUNC:
    eq_type= TRUE;
    break;
  case Item_func::EQUAL_FUNC:
    eq_type= is_null_safe_eq= TRUE;
    break;
  case Item_func::LT_FUNC:
    noeq_type= 1;   /* fall through */
  case Item_func::LE_FUNC:
    less_fl= 1;      
    break;
  case Item_func::GT_FUNC:
    noeq_type= 1;   /* fall through */
  case Item_func::GE_FUNC:
    break;
  case Item_func::BETWEEN:
    if (((Item_func_between*) cond)->negated)
      DBUG_RETURN(FALSE);
    between= 1;
    break;
  case Item_func::MULT_EQUAL_FUNC:
    eq_type= 1;
    break;
  default:
    DBUG_RETURN(FALSE);                                        // Can't optimize function
  }
  
  Item *args[3];
  bool inv;

  /* Test if this is a comparison of a field and constant */
  if (!simple_pred((Item_func*) cond, args, &inv))
    DBUG_RETURN(FALSE);

  if (!is_null_safe_eq && !is_null &&
      (args[1]->is_null() || (between && args[2]->is_null())))
    DBUG_RETURN(FALSE);

  if (inv && !eq_type)
    less_fl= 1-less_fl;                         // Convert '<' -> '>' (etc)

  /* Check if field is part of the tested partial key */
  uchar *key_ptr= ref->key_buff;
  KEY_PART_INFO *part;
  for (part= keyinfo->key_part; ; key_ptr+= part++->store_length)

  {
    if (part > field_part)
      DBUG_RETURN(FALSE);                     // Field is beyond the tested parts
    if (part->field->eq(((Item_field*) args[0])->field))
      break;                        // Found a part of the key for the field
  }

  bool is_field_part= part == field_part;
  if (!(is_field_part || eq_type))
    DBUG_RETURN(FALSE);

  key_part_map org_key_part_used= *key_part_used;
  if (eq_type || between || max_fl == less_fl)
  {
    uint length= (uint)(key_ptr-ref->key_buff)+part->store_length;
    if (ref->key_length < length)
    {
    /* Ultimately ref->key_length will contain the length of the search key */
      ref->key_length= length;      
      ref->key_parts= (uint)(part - keyinfo->key_part) + 1;
    }
    if (!*prefix_len && part+1 == field_part)       
      *prefix_len= length;
    if (is_field_part && eq_type)
      *prefix_len= ref->key_length;
  
    *key_part_used|= (key_part_map) 1 << (part - keyinfo->key_part);
  }

  if (org_key_part_used == *key_part_used &&
    /*
      The current search key is not being extended with a new key part.  This
      means that the a condition is added a key part for which there was a
      previous condition. We can only overwrite such key parts in some special
      cases, e.g. a > 2 AND a > 1 (here range_fl must be set to something). In
      all other cases the WHERE condition is always false anyway.
    */
      (eq_type || *range_fl == 0))
      DBUG_RETURN(FALSE);

  if (org_key_part_used != *key_part_used ||
      (is_field_part && 
       (between || eq_type || max_fl == less_fl) && !cond->val_bool()))
  {
    /*
      It's the first predicate for this part or a predicate of the
      following form  that moves upper/lower bounds for max/min values:
      - field BETWEEN const AND const
      - field = const 
      - field {<|<=} const, when searching for MAX
      - field {>|>=} const, when searching for MIN
    */

    if (is_null || (is_null_safe_eq && args[1]->is_null()))
    {
      /*
        If we have a non-nullable index, we cannot use it,
        since set_null will be ignored, and we will compare uninitialized data.
      */
      if (!part->field->real_maybe_null())
        DBUG_RETURN(FALSE);
      part->field->set_null();
      *key_ptr= (uchar) 1;
    }
    else
    {
      /* Update endpoints for MAX/MIN, see function comment. */
      Item *value= args[between && max_fl ? 2 : 1];
      value->save_in_field_no_warnings(part->field, 1);
      if (part->null_bit) 
        *key_ptr++= (uchar) MY_TEST(part->field->is_null());
      part->field->get_key_image(key_ptr, part->length, Field::itRAW);
    }
    if (is_field_part)
    {
      if (between || eq_type)
      {
        *range_fl&= ~(NO_MAX_RANGE | NO_MIN_RANGE);
        *range_fl&= ~(max_fl ? NEAR_MAX : NEAR_MIN);
      }
      else
      {
        *range_fl&= ~(max_fl ? NO_MAX_RANGE : NO_MIN_RANGE);
        if (noeq_type)
          *range_fl|=  (max_fl ? NEAR_MAX : NEAR_MIN);
        else
          *range_fl&= ~(max_fl ? NEAR_MAX : NEAR_MIN);
      }
    }
  }
  else if (eq_type)
  {
    if ((!is_null && !cond->val_bool()) ||
        (is_null && !MY_TEST(part->field->is_null())))
     DBUG_RETURN(FALSE);                       // Impossible test
  }
  else if (is_field_part)
    *range_fl&= ~(max_fl ? NO_MIN_RANGE : NO_MAX_RANGE);
  DBUG_RETURN(TRUE);  
}


/**
  Check whether we can get value for {max|min}(field) by using a key.

     If where-condition is not a conjunction of 0 or more conjuct the
     function returns false, otherwise it checks whether there is an
     index including field as its k-th component/part such that:

     -# for each previous component f_i there is one and only one conjunct
        of the form: f_i= const_i or const_i= f_i or f_i is null
     -# references to field occur only in conjucts of the form:
        field {<|<=|>=|>|=} const or const {<|<=|>=|>|=} field or 
        field BETWEEN const1 AND const2
     -# all references to the columns from the same table as column field
        occur only in conjucts mentioned above.
     -# each of k first components the index is not partial, i.e. is not
        defined on a fixed length proper prefix of the field.

     If such an index exists the function through the ref parameter
     returns the key value to find max/min for the field using the index,
     the length of first (k-1) components of the key and flags saying
     how to apply the key for the search max/min value.
     (if we have a condition field = const, prefix_len contains the length
     of the whole search key)

  @param[in]     max_fl      0 for MIN(field) / 1 for MAX(field)
  @param[in,out] ref         Reference to the structure we store the key value
  @param[in]     field       Field used inside MIN() / MAX()
  @param[in]     cond        WHERE condition
  @param[out]    range_fl    Bit flags for how to search if key is ok
  @param[out]    prefix_len  Length of prefix for the search range
  @param[out]    reverse     Whether the key part used is descending index

  @note
    This function may set field->table->key_read to true,
    which must be reset after index is used!
    (This can only happen when function returns 1)

  @retval
    0   Index can not be used to optimize MIN(field)/MAX(field)
  @retval
    1   Can use key to optimize MIN()/MAX().
    In this case ref, range_fl and prefix_len are updated
*/
      
static bool find_key_for_maxmin(bool max_fl, TABLE_REF *ref,
                                Field* field, COND *cond,
                                uint *range_fl, uint *prefix_len,
                                bool *reverse)
{
  if (!(field->flags & PART_KEY_FLAG))
    return FALSE;                               // Not key field

  DBUG_ENTER("find_key_for_maxmin");

  TABLE *table= field->table;
  uint idx= 0;

  KEY *keyinfo,*keyinfo_end;
  for (keyinfo= table->key_info, keyinfo_end= keyinfo+table->s->keys ;
       keyinfo != keyinfo_end;
       keyinfo++,idx++)
  {
    KEY_PART_INFO *part,*part_end;
    key_part_map key_part_to_use= 0;
    /*
      Perform a check if index is not disabled by ALTER TABLE
      or IGNORE INDEX.
    */
    if (!table->keys_in_use_for_query.is_set(idx))
      continue;
    uint jdx= 0;
    *prefix_len= 0;
    part_end= keyinfo->key_part+table->actual_n_key_parts(keyinfo);
    for (part= keyinfo->key_part ;
         part != part_end ;
         part++, jdx++, key_part_to_use= (key_part_to_use << 1) | 1)
    {
      if (!(table->file->index_flags(idx, jdx, 0) & HA_READ_ORDER))
        DBUG_RETURN(FALSE);

      /* Check whether the index component is partial */
      Field *part_field= table->field[part->fieldnr-1];
      if ((part_field->flags & BLOB_FLAG) ||
          part->length < part_field->key_length())
        break;

      if (field->eq(part->field))
      {
        ref->key= idx;
        ref->key_length= 0;
        ref->key_parts= 0;
        key_part_map key_part_used= 0;
        *range_fl= NO_MIN_RANGE | NO_MAX_RANGE;
        if (matching_cond(max_fl, ref, keyinfo, part, cond,
                          &key_part_used, range_fl, prefix_len) &&
            !(key_part_to_use & ~key_part_used))
        {
          if (!max_fl && key_part_used == key_part_to_use && part->null_bit)
          {
            /*
              The query is on this form:

              SELECT MIN(key_part_k) 
              FROM t1 
              WHERE key_part_1 = const and ... and key_part_k-1 = const

              If key_part_k is nullable, we want to find the first matching row
              where key_part_k is not null. The key buffer is now {const, ...,
              NULL}. This will be passed to the handler along with a flag
              indicating open interval. If a tuple is read that does not match
              these search criteria, an attempt will be made to read an exact
              match for the key buffer.
            */
            /* Set the first byte of key_part_k to 1, that means NULL */
            ref->key_buff[ref->key_length]= 1;
            ref->key_length+= part->store_length;
            ref->key_parts++;
            DBUG_ASSERT(ref->key_parts == jdx+1);
            *range_fl&= ~NO_MIN_RANGE;
            *range_fl|= NEAR_MIN; // Open interval
          }
          /*
            The following test is false when the key in the key tree is
            converted (for example to upper case)
          */
          if (field->part_of_key.is_set(idx))
            table->file->ha_start_keyread(idx);
          *reverse= part->key_part_flag & HA_REVERSE_SORT ? true : false;
          DBUG_RETURN(TRUE);
        }
      }
    }
  }
  DBUG_RETURN(FALSE);
}


/**
  Check whether found key is in range specified by conditions.

  @param[in] max_fl         0 for MIN(field) / 1 for MAX(field)
  @param[in] ref            Reference to the key value and info
  @param[in] field          Field used the MIN/MAX expression
  @param[in] cond           WHERE condition
  @param[in] range_fl       Says whether there is a condition to to be checked
  @param[in] prefix_len     Length of the constant part of the key

  @retval
    0        ok
  @retval
    1        WHERE was not true for the found row
*/

static int reckey_in_range(bool max_fl, TABLE_REF *ref, Field* field,
                            COND *cond, uint range_fl, uint prefix_len)
{
  if (key_cmp_if_same(field->table, ref->key_buff, ref->key, prefix_len))
    return 1;
  if (!cond || (range_fl & (max_fl ? NO_MIN_RANGE : NO_MAX_RANGE)))
    return 0;
  return maxmin_in_range(max_fl, field, cond);
}


/**
  Check whether {MAX|MIN}(field) is in range specified by conditions.

  @param[in] max_fl          0 for MIN(field) / 1 for MAX(field)
  @param[in] field           Field used the MIN/MAX expression
  @param[in] cond            WHERE condition

  @retval
    0        ok
  @retval
    1        WHERE was not true for the found row
*/

static int maxmin_in_range(bool max_fl, Field* field, COND *cond)
{
  /* If AND/OR condition */
  if (cond->type() == Item::COND_ITEM)
  {
    List_iterator_fast<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    while ((item= li++))
    {
      if (maxmin_in_range(max_fl, field, item))
        return 1;
    }
    return 0;
  }

  if (cond->used_tables() != field->table->map)
    return 0;
  bool less_fl= 0;
  switch (((Item_func*) cond)->functype()) {
  case Item_func::BETWEEN:
    return cond->val_bool() == false;           // Return 1 if WHERE is false
  case Item_func::LT_FUNC:
  case Item_func::LE_FUNC:
    less_fl= 1;
    /* fall through */
  case Item_func::GT_FUNC:
  case Item_func::GE_FUNC:
  {
    Item *item= ((Item_func*) cond)->arguments()[1];
    /* In case of 'const op item' we have to swap the operator */
    if (!item->const_item())
      less_fl= 1-less_fl;
    /*
      We only have to check the expression if we are using an expression like
      SELECT MAX(b) FROM t1 WHERE a=const AND b>const
      not for
      SELECT MAX(b) FROM t1 WHERE a=const AND b<const
    */
    if (max_fl != less_fl)
      return cond->val_bool() == false;         // Return 1 if WHERE is false
    return 0;
  }
  default:
    break;                                      // Ignore
  }
  return 0;
}

