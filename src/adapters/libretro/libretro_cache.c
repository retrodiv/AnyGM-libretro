/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <stdlib.h>
#include <string.h>

static bool cache_option_enabled(const char *key){
  const char *value=libretro_options_value(key);
  return !value || strcmp(value,"Off");
}

/* Observe the actual VFS paths instead of predicting the loader's content hashes. A warm
 * read, a transformed input, nested extraction and a renamed staging tree all belong to the
 * same loaded session. Only immediate children of the explicit cache root may be owned. */
void libretro_cache_track(const char *path){
  if(!g_libretro.cache_tracking || !path || !g_libretro.cache_directory[0]) return;
  const char *root=g_libretro.cache_directory;
  size_t length=strlen(root);
  while(length && (root[length-1]=='/' || root[length-1]=='\\')) length--;
  for(size_t i=0;i<length;i++){
    if(!path[i]) return;
    char a=path[i]=='\\'?'/':path[i],b=root[i]=='\\'?'/':root[i];
    if(a!=b) return;
  }
  if(path[length]!='/' && path[length]!='\\') return;
  const char *child=path+length+1;
  size_t child_length=strcspn(child,"/\\");
  if(!child_length || (child_length==1 && child[0]=='.') ||
     (child_length==2 && child[0]=='.' && child[1]=='.') ||
     memchr(child,':',child_length)) return;
  size_t size=length+1+child_length;
  for(size_t i=0;i<g_libretro.cache_path_count;i++){
    const char *known=g_libretro.cache_paths[i];
    if(strlen(known)==size && !memcmp(known,root,length) &&
       !memcmp(known+length+1,child,child_length)) return;
  }
  if(g_libretro.cache_path_count==g_libretro.cache_path_capacity){
    size_t capacity=g_libretro.cache_path_capacity?g_libretro.cache_path_capacity*2:8;
    if(capacity>4096) goto failed;
    char **paths=realloc(g_libretro.cache_paths,capacity*sizeof *paths);
    if(!paths) goto failed;
    g_libretro.cache_paths=paths;
    g_libretro.cache_path_capacity=capacity;
  }
  char *owned=malloc(size+1);
  if(!owned) goto failed;
  memcpy(owned,root,length);
  owned[length]='/';
  memcpy(owned+length+1,child,child_length);
  owned[size]=0;
  g_libretro.cache_paths[g_libretro.cache_path_count++]=owned;
  return;
failed:
  if(!g_libretro.cache_tracking_failed)
    libretro_log(RETRO_LOG_ERROR,"Cannot record every content cache directory for cleanup\n");
  g_libretro.cache_tracking_failed=true;
}

bool libretro_cache_begin(void){
  g_libretro.cache_tracking=false;
  g_libretro.cache_tracking_failed=false;
  if(g_libretro.cache_directory[0] && cache_option_enabled("anygm_clear_all_caches")){
    if(!libretro_vfs_clear_cache(g_libretro.cache_directory,true)){
      libretro_log(RETRO_LOG_ERROR,"Cannot clear all game caches before loading: %s\n",
                   g_libretro.cache_directory);
      if(g_libretro.environment){
        struct retro_message message={
          "AnyGM could not clear its cache. Check directory permissions or turn off "
          "Clear all game caches on load.",360};
        g_libretro.environment(RETRO_ENVIRONMENT_SET_MESSAGE,&message);
      }
      return false;
    }
    libretro_log(RETRO_LOG_INFO,"All game caches cleared: %s\n",g_libretro.cache_directory);
  }
  g_libretro.cache_tracking=true;
  return true;
}

/* The caller has already unloaded the runtime, including its mappings and open assets.
 * Query the frontend now: changing this option in a paused menu must take effect on close. */
void libretro_cache_end(void){
  g_libretro.cache_tracking=false;
  bool clear=cache_option_enabled("anygm_clear_game_cache");
  for(size_t i=0;i<g_libretro.cache_path_count;i++){
    char *path=g_libretro.cache_paths[i];
    if(clear){
      bool ok=libretro_vfs_clear_cache(path,false);
      libretro_log(ok?RETRO_LOG_INFO:RETRO_LOG_WARN,
                   ok?"Current game cache cleared: %s\n":"Cannot clear current game cache: %s\n",
                   path);
    }
    free(path);
  }
  free(g_libretro.cache_paths);
  g_libretro.cache_paths=NULL;
  g_libretro.cache_path_count=g_libretro.cache_path_capacity=0;
  g_libretro.cache_tracking_failed=false;
}
