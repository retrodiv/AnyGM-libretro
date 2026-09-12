/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LibretroAdapter g_libretro;
static const char *option="On";
static const char *invalid_entry;
static int refuse_delete;
static unsigned opened,closed,removed;
static int failures;
static const char root[]="memory://test/AnyGM-cache";
static struct { const char *relative; int directory; int exists; } nodes[]={
  {"",1,1},{"current",1,1},{"current/.hidden",0,1},{"current/nested",1,1},
  {"current/nested/data",0,1},{"other",1,1},{"other/data",0,1},{".marker",0,1}
};
struct retro_vfs_dir_handle { unsigned node,cursor,current; };

void libretro_log(enum retro_log_level level,const char *format,...){ (void)level; (void)format; }
const char *libretro_options_value(const char *key){ (void)key; return option; }

static void require(int condition,const char *message){
  if(!condition){ fprintf(stderr,"cache VFS: %s\n",message); failures++; }
}
static int node_for(const char *path){
  size_t length=strlen(root);
  if(strncmp(path,root,length) || (path[length] && path[length]!='/')) return -1;
  const char *relative=path+length+(path[length]=='/');
  for(unsigned i=0;i<sizeof nodes/sizeof *nodes;i++)
    if(nodes[i].exists && !strcmp(relative,nodes[i].relative)) return (int)i;
  return -1;
}
static int32_t fake_stat(const char *path,int32_t *size){
  if(size) *size=0;
  int node=node_for(path);
  return node<0?0:RETRO_VFS_STAT_IS_VALID|
      (nodes[node].directory?RETRO_VFS_STAT_IS_DIRECTORY:0);
}
static struct retro_vfs_dir_handle *fake_open(const char *path,bool hidden){
  require(hidden,"cleanup requests hidden VFS entries");
  int node=node_for(path);
  if(node<0 || !nodes[node].directory) return NULL;
  struct retro_vfs_dir_handle *directory=calloc(1,sizeof *directory);
  if(directory){ directory->node=(unsigned)node; opened++; }
  return directory;
}
static bool fake_read(struct retro_vfs_dir_handle *directory){
  if(invalid_entry) return directory->cursor++==0;
  const char *parent=nodes[directory->node].relative;
  size_t length=strlen(parent);
  while(++directory->cursor<sizeof nodes/sizeof *nodes){
    unsigned index=directory->cursor;
    const char *name=nodes[index].relative;
    if(!nodes[index].exists || strncmp(name,parent,length)) continue;
    if(length){ if(name[length]!='/') continue; name+=length+1; }
    if(!name[0] || strchr(name,'/')) continue;
    directory->current=index;
    return true;
  }
  return false;
}
static const char *fake_name(struct retro_vfs_dir_handle *directory){
  if(invalid_entry) return invalid_entry;
  const char *name=nodes[directory->current].relative;
  const char *slash=strrchr(name,'/');
  return slash?slash+1:name;
}
static bool fake_is_dir(struct retro_vfs_dir_handle *directory){
  return nodes[directory->current].directory!=0;
}
static int fake_close(struct retro_vfs_dir_handle *directory){
  closed++; free(directory); return 0;
}
static int fake_remove(const char *path){
  if(refuse_delete) return -1;
  int node=node_for(path);
  if(node<=0) return -1;
  nodes[node].exists=0; removed++; return 0;
}
static struct retro_vfs_interface vfs={
  .stat=fake_stat,.opendir=fake_open,.readdir=fake_read,.dirent_get_name=fake_name,
  .dirent_is_dir=fake_is_dir,.closedir=fake_close,.remove=fake_remove
};
static void reset(void){
  memset(&g_libretro,0,sizeof g_libretro);
  snprintf(g_libretro.cache_directory,sizeof g_libretro.cache_directory,"%s",root);
  g_libretro.vfs=&vfs;
  for(unsigned i=0;i<sizeof nodes/sizeof *nodes;i++) nodes[i].exists=1;
  opened=closed=removed=0; refuse_delete=0; invalid_entry=NULL;
}

int main(void){
  reset();
  require(libretro_vfs_clear_cache(root,true),"global URI cleanup succeeds through the VFS");
  require(nodes[0].exists && removed==7 && opened==closed,"root retained, all children removed, handles closed");
  reset();
  require(libretro_vfs_clear_cache("memory://test/AnyGM-cache/current",false),"current URI subtree clears");
  require(!nodes[1].exists && nodes[5].exists && nodes[7].exists && removed==4,
          "current cleanup preserves other entries");
  reset(); refuse_delete=1;
  require(!libretro_cache_begin() && !g_libretro.cache_tracking && removed==0 && opened==closed,
          "global deletion failure prevents preparation and closes directories");
  reset(); invalid_entry="../outside";
  require(!libretro_vfs_clear_cache(root,true) && !removed && opened==closed,
          "parent traversal from a VFS entry is rejected");
  invalid_entry="other\\outside";
  require(!libretro_vfs_clear_cache(root,true) && !removed && opened==closed,
          "native separators in a VFS entry are rejected");
  require(!libretro_vfs_clear_cache("memory://test/AnyGM-cache/../outside",false),"traversal target refused");
  require(!libretro_vfs_clear_cache("memory://test/AnyGM-cache-other",false),"prefix collision refused");
  require(!libretro_vfs_clear_cache(root,false),"unload cannot delete the entire cache root");
  reset(); option="Off";
  require(libretro_cache_begin(),"disabled global policy starts tracking without deleting");
  libretro_cache_track("memory://test/AnyGM-cache/current/data");
  libretro_cache_track("memory://test/AnyGM-cache/current/nested/data");
  libretro_cache_track("memory://test/AnyGM-cache\\current\\other");
  libretro_cache_track("memory://test/AnyGM-cache-other/file");
  libretro_cache_track("memory://test/AnyGM-cache/../save");
  require(g_libretro.cache_path_count==1 && !removed,"tracking deduplicates separator spellings and excludes other roots");
  option="On"; libretro_cache_end();
  require(!nodes[1].exists && nodes[5].exists && !g_libretro.cache_paths && !g_libretro.cache_tracking,
          "unload cleans recorded entries through the VFS and releases tracking");
  if(failures) return 1;
  puts("libretro cache VFS: ok");
  return 0;
}
