/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson,
    copyright (c) 2011-2012 johannes hanika

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/history.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/mipmap_cache.h"
#include "common/tags.h"
#include "common/utility.h"
#include "common/collection.h"
#include "control/control.h"
#include "develop/develop.h"

void dt_history_item_free(gpointer data)
{
  dt_history_item_t *item = (dt_history_item_t *)data;
  g_free(item->op);
  g_free(item->name);
  item->op = NULL;
  item->name = NULL;
  g_free(item);
}

static void remove_preset_flag(const int imgid)
{
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  // clear flag
  image->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;

  // write through to sql+xmp
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

static void _dt_history_cleanup_multi_instance()
{
  /* as we let the user decide which history item to copy, we can end with some gaps in multi-instance
     numbering.
     for ex., if user decide to not copy the 2nd instance of a module which as 3 instances.
     let's clean-up the history multi-instance. What we want to do is have a unique multi_priority value for
     each iop.
     Furthermore this value must start to 0 and increment one by one for each multi-instance of the same
     module. On
     SQLite there is no notion of ROW_NUMBER, so we use rather resource consuming SQL statement, but as an
     history has
     never a huge number of items that's not a real issue.

     We only do this for the given imgid and only for num>minnum, that is we only handle new history items
     just copied.
  */
  typedef struct _history_item_t
  {
    int num;
    char op[1024];
    int mi;
    int new_mi;
  } _history_item_t;

  // we first reload all the newly added history item
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT num, operation, multi_priority FROM "
                                                             "memory.style_items ORDER BY "
                                                             "operation, multi_priority",
                              -1, &stmt, NULL);
  GList *hitems = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *op = (const char *)sqlite3_column_text(stmt, 1);
    GList *modules = darktable.iop;
    while(modules)
    {
      dt_iop_module_so_t *find_op = (dt_iop_module_so_t *)(modules->data);
      if(!strcmp(find_op->op, op))
      {
        break;
      }
      modules = g_list_next(modules);
    }
    if(modules && (((dt_iop_module_so_t *)(modules->data))->flags() & IOP_FLAGS_ONE_INSTANCE))
    {
      // the current module is a single-instance one, so there's no point in trying
      // to mess up our multi_priority value
      continue;
    }

    _history_item_t *hi = (_history_item_t *)calloc(1, sizeof(_history_item_t));
    hi->num = sqlite3_column_int(stmt, 0);
    snprintf(hi->op, sizeof(hi->op), "%s", sqlite3_column_text(stmt, 1));
    hi->mi = sqlite3_column_int(stmt, 2);
    hi->new_mi = -5; // means : not changed atm
    hitems = g_list_append(hitems, hi);
  }
  sqlite3_finalize(stmt);

  // then we change the multi-priority to be sure to have a correct numbering
  char op[1024] = "";
  int c_mi = 0;
  int nb_change = 0;
  GList *items = g_list_first(hitems);
  while(items)
  {
    _history_item_t *hi = (_history_item_t *)(items->data);
    if(strcmp(op, hi->op) != 0)
    {
      g_strlcpy(op, hi->op, sizeof(op));
      c_mi = 0;
    }
    if(hi->mi != c_mi) nb_change++;
    hi->new_mi = c_mi;
    c_mi++;
    items = g_list_next(items);
  }

  if(nb_change == 0)
  {
    // everything is ok, nothing to change
    g_list_free_full(hitems, free);
    return;
  }

  // and we update the history items
  char *req = NULL;
  req = dt_util_dstrcat(req, "%s", "UPDATE memory.style_items SET multi_priority = CASE num ");
  items = g_list_first(hitems);
  while(items)
  {
    _history_item_t *hi = (_history_item_t *)(items->data);
    if(hi->mi != hi->new_mi)
    {
      req = dt_util_dstrcat(req, "WHEN %d THEN %d ", hi->num, hi->new_mi);
    }
    items = g_list_next(items);
  }
  req = dt_util_dstrcat(req, "%s", "else multi_priority end");
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), req, -1, &stmt, NULL);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_free(req);
  g_list_free_full(hitems, free);
}

static void _history_rebuild_multi_priority_append(const int dest_imgid)
{
  sqlite3_stmt *stmt = NULL;

  // we have to shift the multi_priority on history for the copied entries
  // go through memory.style_items and shift one at the time
  // we can't do it in a single statment because single-instance modules
  // can't be duplicated
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT DISTINCT operation FROM memory.style_items",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *op_old = (const char *)sqlite3_column_text(stmt, 0);

    // if the module is a single-instance, do nothing
    GList *modules = darktable.iop;
    while(modules)
    {
      dt_iop_module_so_t *find_op = (dt_iop_module_so_t *)(modules->data);
      if(!strcmp(find_op->op, op_old))
      {
        break;
      }
      modules = g_list_next(modules);
    }
    if(modules && (((dt_iop_module_so_t *)(modules->data))->flags() & IOP_FLAGS_ONE_INSTANCE)) continue;

    sqlite3_stmt *stmt2 = NULL;

    // shift the priority on history
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE main.history SET multi_priority = multi_priority + "
                                "(SELECT IFNULL(MAX(multi_priority), -1)+1 "
                                "FROM memory.style_items "
                                "WHERE memory.style_items.operation = main.history.operation) "
                                "WHERE imgid = ?1 AND operation = ?2",
                                -1, &stmt2, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, dest_imgid);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 2, op_old, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt2);
    sqlite3_finalize(stmt2);
  }
  sqlite3_finalize(stmt);
}

void dt_history_rebuild_multi_priority_merge(const int dest_imgid)
{
  sqlite3_stmt *stmt = NULL;

  char operation_prev[257] = { 0 };
  int multi_priority_next = -1;

  // first make as if copied items will be appended to history
  // we'll merge it in the next step
  _history_rebuild_multi_priority_append(dest_imgid);

  // select the last entry in history for each operation that we are about to copy
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT MAX(num), operation, multi_priority, multi_name FROM "
      "main.history WHERE imgid = ?1 AND "
      "EXISTS (SELECT * FROM memory.style_items WHERE main.history.operation=memory.style_items.operation) "
      "GROUP BY operation, multi_priority "
      "ORDER BY operation, multi_priority",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *op_old = (const char *)sqlite3_column_text(stmt, 1);
    const int multi_priority_old = sqlite3_column_int(stmt, 2);
    const char *multi_name_old = (const char *)sqlite3_column_text(stmt, 3);

    sqlite3_stmt *stmt2 = NULL;

    // if the module is a single-instance, do nothing
    GList *modules = darktable.iop;
    while(modules)
    {
      dt_iop_module_so_t *find_op = (dt_iop_module_so_t *)(modules->data);
      if(!strcmp(find_op->op, op_old))
      {
        break;
      }
      modules = g_list_next(modules);
    }
    if(modules && (((dt_iop_module_so_t *)(modules->data))->flags() & IOP_FLAGS_ONE_INSTANCE)) continue;

    // we start a new operation, get the next priority
    if(strcmp(op_old, operation_prev) != 0)
    {
      snprintf(operation_prev, sizeof(operation_prev), "%s", op_old);

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT MAX(multi_priority) FROM memory.style_items "
                                  "WHERE operation=?1",
                                  -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 1, op_old, -1, SQLITE_TRANSIENT);
      if(sqlite3_step(stmt2) == SQLITE_ROW)
        multi_priority_next = sqlite3_column_int(stmt2, 0);
      else
        multi_priority_next = -1;
      sqlite3_finalize(stmt2);
    }

    // if this (operation, multi_name) exists on memory.style_items it should be replaced on dest_imgid
    // we also check that it hasn't been used to replace another instance already
    int multi_priority_new = -1;
    int num_new = -1;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT num, operation, multi_name, multi_priority FROM memory.style_items "
                                "WHERE operation=?1 AND multi_name=?2 AND num >= 0",
                                -1, &stmt2, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 1, op_old, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 2, multi_name_old, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(stmt2) == SQLITE_ROW)
    {
      num_new = sqlite3_column_int(stmt2, 0);
      multi_priority_new = sqlite3_column_int(stmt2, 3);
    }
    sqlite3_finalize(stmt2);

    if(multi_priority_new >= 0)
    {
      // if this (operation, multi_name) exists in memory.style_items it should replace the one in history
      // so we update the multi_priority in history with the new one from memory.style_items
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE main.history SET multi_priority = ?1"
                                  "WHERE imgid=?2 AND operation=?3 AND multi_priority=?4",
                                  -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, multi_priority_new);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 2, dest_imgid);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 3, op_old, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 4, multi_priority_old);
      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);

      // and flag this instance as used
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "UPDATE memory.style_items SET num = -1 "
                                                                 "WHERE num = ?1",
                                  -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, num_new);
      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);
    }
    else
    {
      // if this (operation, multi_name) do not exists in memory.style_items it should be
      // pushed back in the pipe, so copied operation are last in the pipe
      // we shift the multi_priority in history
      multi_priority_next++;

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE main.history SET multi_priority = ?4 "
                                  "WHERE imgid=?1 AND operation=?2 AND multi_priority=?3",
                                  -1, &stmt2, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 1, dest_imgid);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 2, op_old, -1, SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 3, multi_priority_old);
      DT_DEBUG_SQLITE3_BIND_INT(stmt2, 4, multi_priority_next);
      sqlite3_step(stmt2);
      sqlite3_finalize(stmt2);
    }
  }
  sqlite3_finalize(stmt);
}

void dt_history_delete_on_image(int32_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.history WHERE imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET history_end = 0 WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.mask WHERE imgid = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  remove_preset_flag(imgid);

  /* if current image in develop reload history */
  if(dt_dev_is_current_image(darktable.develop, imgid)) dt_dev_reload_history_items(darktable.develop);

  /* make sure mipmaps are recomputed */
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);

  /* remove darktable|style|* tags */
  dt_tag_detach_by_string("darktable|style%", imgid);
}

void dt_history_delete_on_selection()
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    dt_history_delete_on_image(imgid);
    dt_image_set_aspect_ratio(imgid);
  }
  sqlite3_finalize(stmt);
}

int dt_history_load_and_apply(int imgid, gchar *filename, int history_only)
{
  int res = 0;
  dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(img)
  {
    if(dt_exif_xmp_read(img, filename, history_only)) return 1;

    /* if current image in develop reload history */
    if(dt_dev_is_current_image(darktable.develop, imgid)) dt_dev_reload_history_items(darktable.develop);

    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
    dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
  }
  return res;
}

int dt_history_load_and_apply_on_selection(gchar *filename)
{
  int res = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    if(dt_history_load_and_apply(imgid, filename, 1)) res = 1;
  }
  sqlite3_finalize(stmt);
  return res;
}

int dt_history_copy_and_paste_on_image(int32_t imgid, int32_t dest_imgid, gboolean merge, GList *ops)
{
  sqlite3_stmt *stmt;
  if(imgid == dest_imgid) return 1;

  if(imgid == -1)
  {
    dt_control_log(_("you need to copy history from an image before you paste it onto another"));
    return 1;
  }

  // be sure the current history is written before pasting some other history data
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM) dt_dev_write_history(darktable.develop);

  /* if merge onto history stack, lets find history offest in destination image */
  int32_t offs = 0;
  if(merge)
  {
    /* apply on top of history stack */
    // first trim the stack to get rid of whatever is above the selected entry
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM main.history WHERE imgid = ?1 AND num >= (SELECT history_end "
                                "FROM main.images WHERE id = imgid)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT IFNULL(MAX(num), -1)+1 FROM main.history WHERE imgid = ?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW) offs = sqlite3_column_int(stmt, 0);
  }
  else
  {
    /* replace history stack */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.history WHERE imgid = ?1", -1,
                                &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);

  /* delete all items from the temp styles_items, this table is used only to get a ROWNUM of the results */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.style_items", NULL, NULL, NULL);

  /* copy history items from styles onto temp table */

  //  prepare SQL request
  if(merge && !ops)
  {
    // the user has selected copy all and append
    // select only the last entry in history for each (operation, multi_priority)
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "INSERT INTO memory.style_items (num, module, operation, op_params, enabled, blendop_params, "
        "blendop_version, multi_name, multi_priority) SELECT MAX(num) AS max_num, module, operation, "
        "op_params, enabled, blendop_params, blendop_version, multi_name, multi_priority FROM "
        "main.history WHERE imgid = ?1"
        "GROUP BY operation, multi_priority "
        "ORDER BY max_num",
        -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    // in any other case we select all items in history or only the ones selected by the user
    char req[2048];
    g_strlcpy(req, "INSERT INTO memory.style_items (num, module, operation, op_params, enabled, blendop_params, "
                   "blendop_version, multi_name, multi_priority) SELECT num, module, operation, "
                   "op_params, enabled, blendop_params, blendop_version, multi_name, multi_priority FROM "
                   "main.history WHERE imgid = ?1",
              sizeof(req));

    //  Add ops selection if any format: ... and num in (val1, val2)
    if(ops)
    {
      GList *l = ops;
      int first = 1;
      g_strlcat(req, " AND num IN (", sizeof(req));

      while(l)
      {
        unsigned int value = GPOINTER_TO_UINT(l->data);
        char v[30];

        if(!first) g_strlcat(req, ",", sizeof(req));
        snprintf(v, sizeof(v), "%u", value);
        g_strlcat(req, v, sizeof(req));
        first = 0;
        l = g_list_next(l);
      }
      g_strlcat(req, ")", sizeof(req));
    }

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), req, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  if(merge)
  {
    _dt_history_cleanup_multi_instance();
    dt_history_rebuild_multi_priority_merge(dest_imgid);
  }

  /* copy the history items into the history of the dest image */
  /* note: rowid starts at 1 while num has to start at 0! */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.history "
                              "(imgid,num,module,operation,op_params,enabled,blendop_params,blendop_"
                              "version,multi_priority,multi_name) SELECT "
                              "?1,?2+rowid-1,module,operation,op_params,enabled,blendop_params,blendop_"
                              "version,multi_priority,multi_name FROM memory.style_items",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, offs);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // we have to copy masks too
  // what to do with existing masks ?
  if(merge)
  {
    // there's very little chance that we will have same shapes id.
    // but we may want to handle this case anyway
    // and it's not trivial at all !
  }
  else
  {
    // let's remove all existing shapes
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.mask WHERE imgid = ?1", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  // let's copy now
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT INTO main.mask (imgid, formid, form, name, version, points, points_count, source) SELECT "
      "?1, formid, form, name, version, points, points_count, source FROM main.mask WHERE imgid = ?2",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // always make the whole stack active
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET history_end = (SELECT MAX(num) + 1 FROM main.history "
                              "WHERE imgid = ?1) WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  /* if current image in develop reload history */
  if(dt_dev_is_current_image(darktable.develop, dest_imgid))
  {
    dt_dev_reload_history_items(darktable.develop);
    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
  }

  /* update xmp file */
  dt_image_synch_xmp(dest_imgid);

  dt_mipmap_cache_remove(darktable.mipmap_cache, dest_imgid);

  /* update the aspect ratio if the current sorting is based on aspect ratio, otherwise the aspect ratio will be
     recalculated when the mimpap will be recreated */
  if (darktable.collection->params.sort == DT_COLLECTION_SORT_ASPECT_RATIO)
    dt_image_set_aspect_ratio(dest_imgid);

  return 0;
}

GList *dt_history_get_items(int32_t imgid, gboolean enabled)
{
  GList *result = NULL;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT num, operation, enabled, multi_name FROM main.history WHERE imgid=?1 AND "
                              "num IN (SELECT MAX(num) FROM main.history hst2 WHERE hst2.imgid=?1 AND "
                              "hst2.operation=main.history.operation GROUP BY multi_priority) ORDER BY num DESC",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char name[512] = { 0 };
    const int is_active = sqlite3_column_int(stmt, 2);

    if(enabled == FALSE || is_active)
    {
      dt_history_item_t *item = g_malloc(sizeof(dt_history_item_t));
      item->num = sqlite3_column_int(stmt, 0);
      char *mname = NULL;
      mname = g_strdup((gchar *)sqlite3_column_text(stmt, 3));
      if(enabled)
      {
        if(strcmp(mname, "0") == 0)
          g_snprintf(name, sizeof(name), "%s",
                     dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)));
        else
          g_snprintf(name, sizeof(name), "%s %s",
                     dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)),
                     (char *)sqlite3_column_text(stmt, 3));
      }
      else
      {
        if(strcmp(mname, "0") == 0)
          g_snprintf(name, sizeof(name), "%s (%s)",
                     dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)),
                     (is_active != 0) ? _("on") : _("off"));
        g_snprintf(name, sizeof(name), "%s %s (%s)",
                   dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 1)),
                   (char *)sqlite3_column_text(stmt, 3), (is_active != 0) ? _("on") : _("off"));
      }
      item->name = g_strdup(name);
      item->op = g_strdup((gchar *)sqlite3_column_text(stmt, 1));
      result = g_list_append(result, item);

      g_free(mname);
    }
  }
  sqlite3_finalize(stmt);
  return result;
}

char *dt_history_get_items_as_string(int32_t imgid)
{
  GList *items = NULL;
  const char *onoff[2] = { _("off"), _("on") };
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT operation, enabled, multi_name FROM main.history WHERE imgid=?1 ORDER BY num DESC", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  // collect all the entries in the history from the db
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *name = NULL, *multi_name = NULL;
    const char *mn = (char *)sqlite3_column_text(stmt, 2);
    if(mn && *mn && g_strcmp0(mn, " ") != 0 && g_strcmp0(mn, "0") != 0)
      multi_name = g_strconcat(" ", sqlite3_column_text(stmt, 2), NULL);
    name = g_strconcat(dt_iop_get_localized_name((char *)sqlite3_column_text(stmt, 0)),
                       multi_name ? multi_name : "", " (",
                       (sqlite3_column_int(stmt, 1) == 0) ? onoff[0] : onoff[1], ")", NULL);
    items = g_list_append(items, name);
    g_free(multi_name);
  }
  sqlite3_finalize(stmt);
  char *result = dt_util_glist_to_str("\n", items);
  g_list_free_full(items, g_free);
  return result;
}

int dt_history_copy_and_paste_on_selection(int32_t imgid, gboolean merge, GList *ops)
{
  if(imgid < 0) return 1;

  int res = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM main.selected_images WHERE imgid != ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    do
    {
      /* get imgid of selected image */
      int32_t dest_imgid = sqlite3_column_int(stmt, 0);

      /* paste history stack onto image id */
      dt_history_copy_and_paste_on_image(imgid, dest_imgid, merge, ops);

    } while(sqlite3_step(stmt) == SQLITE_ROW);
  }
  else
    res = 1;

  sqlite3_finalize(stmt);
  return res;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
