/*
 * metadata.c : GeeXboX uShare CDS Metadata DB.
 * Originally developped for the GeeXboX project.
 * Copyright (C) 2005-2007 Benjamin Zores <ben@geexbox.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdbool.h>

#include "mime.h"
#include "metadata.h"
#include "util_iconv.h"
#include "content.h"
#include "gettext.h"
#include "trace.h"

#ifdef HAVE_FAM
#include "ufam.h"
#endif /* HAVE_FAM */

#define DEFAULT_CONTAINER_VALUE 10
#define DLNA_CLASS_FOLDER (DLNA_CLASS_COLLECTION + 1)
struct {
  uint32_t id;
  char *name;
} ushare_containers[] = {
  [DLNA_CLASS_UNKNOWN] = { .id = DLNA_CLASS_UNKNOWN + DEFAULT_CONTAINER_VALUE, .name = "others" },
  [DLNA_CLASS_IMAGE] = { .id = DLNA_CLASS_IMAGE + DEFAULT_CONTAINER_VALUE, .name = "image" },
  [DLNA_CLASS_AUDIO] = { .id = DLNA_CLASS_AUDIO + DEFAULT_CONTAINER_VALUE, .name = "music" },
  [DLNA_CLASS_AV] = { .id = DLNA_CLASS_AV + DEFAULT_CONTAINER_VALUE, .name = "video" },
  [DLNA_CLASS_COLLECTION] = { .id = DLNA_CLASS_AUDIO + DEFAULT_CONTAINER_VALUE, .name = "music" },
  [DLNA_CLASS_FOLDER] = { .id = DLNA_CLASS_FOLDER + DEFAULT_CONTAINER_VALUE, .name = "folders" },
};

static void
add_virtualfolder (dlna_vfs_t *vfs, char *name, dlna_item_t *item)
{
  dlna_media_class_t id;

  id = dlna_item_type (item);

  dlna_vfs_add_container (vfs, ushare_containers[id].name, ushare_containers[id].id, 0);
  dlna_vfs_add_resource (vfs, name, item, ushare_containers[id].id);
}

static void
parse_container (dlna_t *dlna, dlna_vfs_t *vfs, char *dir, uint32_t id)
{
  struct dirent **namelist;
  int n, i;
  
  n = scandir (dir, &namelist, 0, alphasort);
  if (n < 0)
  {
    perror ("scandir");
    return;
  }

  for (i = 0; i < n; i++)
  {
    uint32_t cid;
    struct stat st;
    char *fullpath = NULL;

    if (namelist[i]->d_name[0] == '.')
    {
      free (namelist[i]);
      continue;
    }

    fullpath = malloc (strlen (dir) + strlen (namelist[i]->d_name) + 2);
    sprintf (fullpath, "%s/%s", dir, namelist[i]->d_name);

    if (stat (fullpath, &st) < 0)
    {
      free (namelist[i]);
      free (fullpath);
      continue;
    }

    if (!id)
      cid = ushare_containers[DLNA_CLASS_FOLDER].id;
    else
      cid = id;
    if (S_ISDIR (st.st_mode))
    {
      cid = dlna_vfs_add_container (vfs, basename (fullpath), 0, cid);
      parse_container (dlna, vfs, fullpath, cid);
    }
    else
    {
      dlna_item_t *item;

      item = dlna_item_new (dlna, fullpath);
      if (item)
      {
        add_virtualfolder (vfs, basename (fullpath), item);
        dlna_vfs_add_resource (vfs, basename (fullpath), item, cid);
      }
    }
    
    free (namelist[i]);
    free (fullpath);
  }
  free (namelist);
}

void
build_metadata_list (ushare_t *ut)
{
  int i;
  
  log_info (_("Building Metadata List ...\n"));

  ushare_containers[DLNA_CLASS_FOLDER].id = dlna_vfs_add_container (ut->vfs, 
                                ushare_containers[DLNA_CLASS_FOLDER].name, 
                                ushare_containers[DLNA_CLASS_FOLDER].id, 0);
  /* add files from content directory */
  for (i = 0 ; i < ut->contentlist->count ; i++)
  {
    log_info (_("Looking for files in content directory : %s\n"),
              ut->contentlist->content[i]);

    parse_container (ut->dlna, ut->vfs, ut->contentlist->content[i], 0);
  }
}

void
free_metadata_list (ushare_t *ut)
{
  dlna_vfs_remove_item_by_id (ut->vfs, 0);
}
