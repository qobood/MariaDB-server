/* Copyright (c) 2007, 2013, Oracle and/or its affiliates. All rights reserved.

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

#include "mariadb.h"
#include "sql_priv.h"
#include "mysqld.h"
#include "sql_audit.h"

extern int initialize_audit_plugin(void *plugin);
extern int finalize_audit_plugin(void *plugin);

#ifndef EMBEDDED_LIBRARY

struct st_mysql_event_generic
{
  unsigned long event_class_mask[MYSQL_AUDIT_CLASS_MASK_SIZE];
  unsigned int event_class;
  const void *event;
};

unsigned long mysql_global_audit_mask[MYSQL_AUDIT_CLASS_MASK_SIZE];

static mysql_mutex_t LOCK_audit_mask;


static inline
void set_audit_mask(unsigned long *mask, uint event_class)
{
  mask[0]= 1;
  mask[0]<<= event_class;
}

static inline
void add_audit_mask(unsigned long *mask, const unsigned long *rhs)
{
  mask[0]|= rhs[0];
}

static inline
bool check_audit_mask(const unsigned long *lhs,
                      const unsigned long *rhs)
{
  return !(lhs[0] & rhs[0]);
}


/**
  Acquire and lock any additional audit plugins as required
  
  @param[in] thd
  @param[in] plugin
  @param[in] arg

  @retval FALSE Always  
*/

static my_bool acquire_plugins(THD *thd, plugin_ref plugin, void *arg)
{
  ulong *event_class_mask= (ulong*) arg;
  st_mysql_audit *data= plugin_data(plugin, struct st_mysql_audit *);

  /* Check if this plugin is interested in the event */
  if (check_audit_mask(data->class_mask, event_class_mask))
    return 0;

  /*
    Check if this plugin may already be registered. This will fail to
    acquire a newly installed plugin on a specific corner case where
    one or more event classes already in use by the calling thread
    are an event class of which the audit plugin has interest.
  */
  if (!check_audit_mask(data->class_mask, thd->audit_class_mask))
    return 0;
  
  /* Check if we need to initialize the array of acquired plugins */
  if (unlikely(!thd->audit_class_plugins.buffer))
  {
    /* specify some reasonable initialization defaults */
    my_init_dynamic_array(PSI_INSTRUMENT_ME, &thd->audit_class_plugins,
                          sizeof(plugin_ref), 16, 16, MYF(0));
  }
  
  /* lock the plugin and add it to the list */
  plugin= my_plugin_lock(NULL, plugin);
  insert_dynamic(&thd->audit_class_plugins, (uchar*) &plugin);

  return 0;
}


/**
  @brief Acquire audit plugins

  @param[in]   thd              MySQL thread handle
  @param[in]   event_class      Audit event class

  @details Ensure that audit plugins interested in given event
  class are locked by current thread.
*/
void mysql_audit_acquire_plugins(THD *thd, ulong *event_class_mask)
{
  DBUG_ENTER("mysql_audit_acquire_plugins");
  DBUG_ASSERT(thd);
  if (check_audit_mask(thd->audit_class_mask, event_class_mask))
  {
    plugin_foreach(thd, acquire_plugins, MYSQL_AUDIT_PLUGIN, event_class_mask);
    add_audit_mask(thd->audit_class_mask, event_class_mask);
    thd->audit_plugin_version= global_plugin_version;
  }
  DBUG_VOID_RETURN;
}


/**
  Check if there were changes in the state of plugins
  so we need to do the mysql_audit_release asap.

  @param[in] thd

*/

my_bool mysql_audit_release_required(THD *thd)
{
  return thd && (thd->audit_plugin_version != global_plugin_version);
}


/**
  Release any resources associated with the current thd.
  
  @param[in] thd

*/

void mysql_audit_release(THD *thd)
{
  plugin_ref *plugins, *plugins_last;
  
  if (!thd || !(thd->audit_class_plugins.elements))
    return;
  
  plugins= (plugin_ref*) thd->audit_class_plugins.buffer;
  plugins_last= plugins + thd->audit_class_plugins.elements;
  for (; plugins < plugins_last; plugins++)
  {
    st_mysql_audit *data= plugin_data(*plugins, struct st_mysql_audit *);
	
    /* Check to see if the plugin has a release method */
    if (!(data->release_thd))
      continue;

    /* Tell the plugin to release its resources */
    data->release_thd(thd);
  }

  /* Now we actually unlock the plugins */  
  plugin_unlock_list(NULL, (plugin_ref*) thd->audit_class_plugins.buffer,
                     thd->audit_class_plugins.elements);
  
  /* Reset the state of thread values */
  reset_dynamic(&thd->audit_class_plugins);
  bzero(thd->audit_class_mask, sizeof(thd->audit_class_mask));
  thd->audit_plugin_version= -1;
}


/**
  Initialize thd variables used by Audit
  
  @param[in] thd

*/

void mysql_audit_init_thd(THD *thd)
{
  bzero(&thd->audit_class_plugins, sizeof(thd->audit_class_plugins));
  bzero(thd->audit_class_mask, sizeof(thd->audit_class_mask));
}


/**
  Free thd variables used by Audit
  
  @param[in] thd
  @param[in] plugin
  @param[in] arg

  @retval FALSE Always  
*/

void mysql_audit_free_thd(THD *thd)
{
  mysql_audit_release(thd);
  DBUG_ASSERT(thd->audit_class_plugins.elements == 0);
  delete_dynamic(&thd->audit_class_plugins);
}

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key key_LOCK_audit_mask;

static PSI_mutex_info all_audit_mutexes[]=
{
  { &key_LOCK_audit_mask, "LOCK_audit_mask", PSI_FLAG_GLOBAL}
};

static void init_audit_psi_keys(void)
{
  const char* category= "sql";
  int count;

  if (PSI_server == NULL)
    return;

  count= array_elements(all_audit_mutexes);
  PSI_server->register_mutex(category, all_audit_mutexes, count);
}
#endif /* HAVE_PSI_INTERFACE */

/**
  Initialize Audit global variables
*/

void mysql_audit_initialize()
{
#ifdef HAVE_PSI_INTERFACE
  init_audit_psi_keys();
#endif

  mysql_mutex_init(key_LOCK_audit_mask, &LOCK_audit_mask, MY_MUTEX_INIT_FAST);
  bzero(mysql_global_audit_mask, sizeof(mysql_global_audit_mask));
}


/**
  Finalize Audit global variables  
*/

void mysql_audit_finalize()
{
  mysql_mutex_destroy(&LOCK_audit_mask);
}


/**
  Initialize an Audit plug-in

  @param[in] plugin

  @retval FALSE  OK
  @retval TRUE   There was an error.
*/

int initialize_audit_plugin(void *plugin_)
{
  st_plugin_int *plugin= static_cast<st_plugin_int *>(plugin_);
  st_mysql_audit *data= (st_mysql_audit*) plugin->plugin->info;

  if (!data->event_notify || !data->class_mask[0])
  {
    sql_print_error("Plugin '%s' has invalid data.",
                    plugin->name.str);
    return 1;
  }

  if (plugin->plugin->init && plugin->plugin->init(NULL))
  {
    sql_print_error("Plugin '%s' init function returned error.",
                    plugin->name.str);
    return 1;
  }

  /* Make the interface info more easily accessible */
  plugin->data= plugin->plugin->info;

  /* Add the bits the plugin is interested in to the global mask */
  mysql_mutex_lock(&LOCK_audit_mask);
  add_audit_mask(mysql_global_audit_mask, data->class_mask);
  mysql_mutex_unlock(&LOCK_audit_mask);

  /*
    Pre-acquire the newly installed audit plugin for events that
    may potentially occur further during INSTALL PLUGIN.

    When audit event is triggered, audit subsystem acquires interested
    plugins by walking through plugin list. Evidently plugin list
    iterator protects plugin list by acquiring LOCK_plugin, see
    plugin_foreach_with_mask().

    On the other hand [UN]INSTALL PLUGIN is acquiring LOCK_plugin
    rather for a long time.

    When audit event is triggered during [UN]INSTALL PLUGIN, plugin
    list iterator acquires the same lock (within the same thread)
    second time.

    This hack should be removed when LOCK_plugin is fixed so it
    protects only what it supposed to protect.

    See also mysql_install_plugin() and mysql_uninstall_plugin()
  */
  THD *thd= current_thd;
  if (thd)
  {
    acquire_plugins(thd, plugin_int_to_ref(plugin), data->class_mask);
    add_audit_mask(thd->audit_class_mask, data->class_mask);
  }

  return 0;
}


/**
  Performs a bitwise OR of the installed plugins event class masks

  @param[in] thd
  @param[in] plugin
  @param[in] arg

  @retval FALSE  always
*/
static my_bool calc_class_mask(THD *thd, plugin_ref plugin, void *arg)
{
  st_mysql_audit *data= plugin_data(plugin, struct st_mysql_audit *);
  if ((data= plugin_data(plugin, struct st_mysql_audit *)))
    add_audit_mask((unsigned long *) arg, data->class_mask);
  return 0;
}


/**
  Finalize an Audit plug-in
  
  @param[in] plugin

  @retval FALSE  OK
  @retval TRUE   There was an error.
*/
int finalize_audit_plugin(void *plugin_)
{
  st_plugin_int *plugin= static_cast<st_plugin_int *>(plugin_);
  int deinit_status= 0;
  unsigned long event_class_mask[MYSQL_AUDIT_CLASS_MASK_SIZE];
  
  if (plugin->plugin->deinit)
    deinit_status= plugin->plugin->deinit(NULL);
  
  plugin->data= NULL;
  bzero(&event_class_mask, sizeof(event_class_mask));

  /* Iterate through all the installed plugins to create new mask */

  /*
    LOCK_audit_mask/LOCK_plugin order is not fixed, but serialized with table
    lock on mysql.plugin.
  */
  mysql_mutex_lock(&LOCK_audit_mask);
  plugin_foreach(current_thd, calc_class_mask, MYSQL_AUDIT_PLUGIN,
                 &event_class_mask);

  /* Set the global audit mask */
  bmove(mysql_global_audit_mask, event_class_mask, sizeof(event_class_mask));
  mysql_mutex_unlock(&LOCK_audit_mask);

  return deinit_status;
}


/**
  Dispatches an event by invoking the plugin's event_notify method.  

  @param[in] thd
  @param[in] plugin
  @param[in] arg

  @retval FALSE  always
*/

static my_bool plugins_dispatch(THD *thd, plugin_ref plugin, void *arg)
{
  const struct st_mysql_event_generic *event_generic=
    (const struct st_mysql_event_generic *) arg;
  st_mysql_audit *data= plugin_data(plugin, struct st_mysql_audit *);

  /* Check to see if the plugin is interested in this event */
  if (!check_audit_mask(data->class_mask, event_generic->event_class_mask))
    data->event_notify(thd, event_generic->event_class, event_generic->event);

  return 0;
}


/**
  Distributes an audit event to plug-ins

  @param[in] thd
  @param[in] event_class
  @param[in] event
*/

void mysql_audit_notify(THD *thd, uint event_class, const void *event)
{
  struct st_mysql_event_generic event_generic;
  event_generic.event_class= event_class;
  event_generic.event= event;
  set_audit_mask(event_generic.event_class_mask, event_class);
  /*
    Check if we are doing a slow global dispatch. This event occurs when
    thd == NULL as it is not associated with any particular thread.
  */
  if (unlikely(!thd))
  {
    plugin_foreach(thd, plugins_dispatch, MYSQL_AUDIT_PLUGIN, &event_generic);
  }
  else
  {
    plugin_ref *plugins, *plugins_last;

    mysql_audit_acquire_plugins(thd, event_generic.event_class_mask);

    /* Use the cached set of audit plugins */
    plugins= (plugin_ref*) thd->audit_class_plugins.buffer;
    plugins_last= plugins + thd->audit_class_plugins.elements;

    for (; plugins < plugins_last; plugins++)
      plugins_dispatch(thd, *plugins, &event_generic);
  }
}


#else /* EMBEDDED_LIBRARY */


void mysql_audit_acquire_plugins(THD *thd, ulong *event_class_mask)
{
}


void mysql_audit_initialize()
{
}


void mysql_audit_finalize()
{
}


int initialize_audit_plugin(void *plugin)
{
  return 1;
}


int finalize_audit_plugin(void *plugin)
{
  return 0;
}


void mysql_audit_release(THD *thd)
{
}

void mysql_audit_init_thd(THD *thd)
{
}

void mysql_audit_free_thd(THD *thd)
{
}

#endif /* EMBEDDED_LIBRARY */
