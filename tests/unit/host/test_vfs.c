/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm_vfs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MemoryHost {
  const uint8_t *input;
  size_t input_size;
  size_t position;
  uint8_t output[32];
  size_t output_size;
  size_t write_limit;
  int stop_read_early;
  int overreport_read;
  unsigned opens;
  unsigned closes;
  unsigned flushes;
  unsigned creates;
  unsigned renames;
  unsigned removes;
} MemoryHost;

static int fail(const char *message){
  fprintf(stderr,"VFS contract: %s\n",message);
  return 1;
}

static void *memory_open(void *userdata,const char *path,AnygmFileMode mode){
  MemoryHost *host=userdata;
  if(!path || strncmp(path,"root/",5) || strstr(path,"..") || path[0]=='/') return NULL;
  if((mode&ANYGM_FILE_READ) && strcmp(path,"root/input")) return NULL;
  if((mode&ANYGM_FILE_WRITE) && strcmp(path,"root/output")) return NULL;
  host->position=0;
  host->opens++;
  if(mode&ANYGM_FILE_TRUNCATE) host->output_size=0;
  return host;
}

static size_t memory_read(void *userdata,void *file,void *data,size_t size){
  MemoryHost *host=userdata;
  (void)file;
  if(host->overreport_read){ host->overreport_read=0; return size+1; }
  if(host->stop_read_early && host->position>=host->input_size/2) return 0;
  size_t remaining=host->input_size-host->position;
  if(host->stop_read_early && remaining>host->input_size/2-host->position)
    remaining=host->input_size/2-host->position;
  if(size>remaining) size=remaining;
  if(size) memcpy(data,host->input+host->position,size);
  host->position+=size;
  return size;
}

static size_t memory_write(void *userdata,void *file,const void *data,size_t size){
  MemoryHost *host=userdata;
  (void)file;
  size_t allowed=sizeof host->output-host->output_size;
  if(host->write_limit){
    if(host->output_size>=host->write_limit) allowed=0;
    else if(allowed>host->write_limit-host->output_size)
      allowed=host->write_limit-host->output_size;
  }
  if(size>allowed) size=allowed;
  if(size) memcpy(host->output+host->output_size,data,size);
  host->output_size+=size;
  return size;
}

static int64_t memory_seek(void *userdata,void *file,int64_t offset,AnygmSeekOrigin origin){
  MemoryHost *host=userdata;
  (void)file;
  int64_t base=origin==ANYGM_SEEK_START?0:
               origin==ANYGM_SEEK_CURRENT?(int64_t)host->position:
               origin==ANYGM_SEEK_END?(int64_t)host->input_size:-1;
  if(base<0 || offset < -base || (uint64_t)(base+offset)>host->input_size) return -1;
  host->position=(size_t)(base+offset);
  return (int64_t)host->position;
}

static AnygmResult memory_flush(void *userdata,void *file){
  MemoryHost *host=userdata;
  (void)file;
  host->flushes++;
  return ANYGM_OK;
}

static void memory_close(void *userdata,void *file){
  MemoryHost *host=userdata;
  (void)file;
  host->closes++;
}

static AnygmResult memory_create(void *userdata,const char *path){
  MemoryHost *host=userdata;
  if(!path || (strcmp(path,"root")&&strncmp(path,"root/",5)) || strstr(path,".."))
    return ANYGM_ERROR_IO;
  host->creates++;
  return ANYGM_OK;
}

static AnygmResult memory_rename(void *userdata,const char *from,const char *to){
  MemoryHost *host=userdata;
  if(strcmp(from,"root/temporary")||strcmp(to,"root/final")) return ANYGM_ERROR_IO;
  host->renames++;
  return ANYGM_OK;
}

static AnygmResult memory_remove(void *userdata,const char *path){
  MemoryHost *host=userdata;
  if(strcmp(path,"root/temporary")) return ANYGM_ERROR_IO;
  host->removes++;
  return ANYGM_OK;
}


/* A bundle authored where filenames ignore case names its files with whatever spelling it likes.
 * Read on a filesystem that does not ignore case, the exact name resolves to nothing, and a state
 * that recorded one of those names would be unreadable over a capital letter. The resolver has to
 * recover the spelling the filesystem actually holds, one component at a time. */
typedef struct DirectoryHost {
  const char *const *names;
  size_t count;
  const char *listing_path;
  size_t listing_position;
} DirectoryHost;

static const char *const DIRECTORY_ENTRIES[]={"root","Data","Player.PNG"};

static AnygmResult directory_stat(void *userdata,const char *path,AnygmFileInfo *info){
  (void)userdata;
  if(!path || !info) return ANYGM_ERROR_INVALID_ARGUMENT;
  if(strcmp(path,"root") && strcmp(path,"root/Data") && strcmp(path,"root/Data/Player.PNG"))
    return ANYGM_ERROR_IO;
  info->flags|=ANYGM_FILE_INFO_EXISTS;
  return ANYGM_OK;
}

static void *directory_open(void *userdata,const char *path){
  DirectoryHost *state=userdata;
  if(!state || !path) return NULL;
  if(strcmp(path,".") && strcmp(path,"root") && strcmp(path,"root/Data")) return NULL;
  state->listing_path=!strcmp(path,".")?"":path;
  state->listing_position=0;
  return state;
}

static AnygmResult directory_read(void *userdata,void *directory,AnygmDirectoryEntry *entry){
  DirectoryHost *state=userdata;
  (void)directory;
  if(!state || !entry) return ANYGM_ERROR_INVALID_ARGUMENT;
  /* Each directory in the fixture holds exactly the next name of the path. */
  size_t index=!state->listing_path[0]?0:(!strcmp(state->listing_path,"root")?1:2);
  if(state->listing_position) return ANYGM_ERROR_IO;
  state->listing_position=1;
  snprintf(entry->name,sizeof entry->name,"%s",DIRECTORY_ENTRIES[index]);
  return ANYGM_OK;
}

static void directory_close(void *userdata,void *directory){
  (void)userdata;
  (void)directory;
}

static int check_casefold_resolution(void){
  DirectoryHost state={0};
  AnygmHostServices host={0};
  char resolved[256];
  host.struct_size=sizeof host;
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  host.userdata=&state;
  host.file_stat=directory_stat;
  host.directory_open=directory_open;
  host.directory_read=directory_read;
  host.directory_close=directory_close;

  if(!anygm_vfs_resolve_casefold(&host,"root/data/player.png",resolved,sizeof resolved) ||
     strcmp(resolved,"root/Data/Player.PNG"))
    return fail("a path spelled for a case-insensitive filesystem was not recovered");
  if(!anygm_vfs_resolve_casefold(&host,"root/Data/Player.PNG",resolved,sizeof resolved) ||
     strcmp(resolved,"root/Data/Player.PNG"))
    return fail("a path that already resolves was rewritten");
  if(!anygm_vfs_resolve_casefold(&host,"root/data/missing.png",resolved,sizeof resolved) ||
     strcmp(resolved,"root/Data/missing.png"))
    return fail("an unresolvable component did not keep the name the content asked for");

  AnygmHostServices without_directories={0};
  without_directories.struct_size=sizeof without_directories;
  without_directories.abi_version=ANYGM_HOST_SERVICES_VERSION;
  if(!anygm_vfs_resolve_casefold(&without_directories,"root/data/player.png",
                                 resolved,sizeof resolved) ||
     strcmp(resolved,"root/data/player.png"))
    return fail("a host without directory services lost the requested path");
  if(anygm_vfs_resolve_casefold(&host,"root/data/player.png",resolved,8))
    return fail("a path longer than the buffer was reported as resolved");
  return 0;
}

int main(void){
  static const uint8_t input[]={0,1,2,3,4,5,6,7,8,9};
  MemoryHost memory={0};
  memory.input=input;
  memory.input_size=sizeof input;
  AnygmHostServices host={0};
  host.struct_size=sizeof host;
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  host.userdata=&memory;
  host.file_open=memory_open;
  host.file_read=memory_read;
  host.file_write=memory_write;
  host.file_seek=memory_seek;
  host.file_flush=memory_flush;
  host.file_close=memory_close;
  host.directory_create=memory_create;
  host.path_rename=memory_rename;
  host.path_remove=memory_remove;

  uint8_t *data=NULL;
  size_t size=0;
  if(!anygm_vfs_can_read(&host)||!anygm_vfs_can_write(&host)||
     !anygm_vfs_read_all(&host,"root/input",&data,&size,sizeof input)||
     size!=sizeof input||memcmp(data,input,sizeof input)) return fail("bounded read failed");
  free(data);

  if(anygm_vfs_read_all(&host,"outside/input",&data,&size,sizeof input) ||
     anygm_vfs_read_all(&host,"root/../input",&data,&size,sizeof input))
    return fail("host root rejection fell back to another filesystem");
  if(anygm_vfs_read_all(&host,"root/input",&data,&size,sizeof input-1))
    return fail("read limit was ignored");

  memory.stop_read_early=1;
  if(anygm_vfs_read_all(&host,"root/input",&data,&size,sizeof input))
    return fail("short exact read was accepted");
  memory.stop_read_early=0;
  memory.overreport_read=1;
  if(anygm_vfs_read_all(&host,"root/input",&data,&size,sizeof input))
    return fail("overreported read was accepted");

  static const uint8_t output[]={9,8,7,6};
  if(!anygm_vfs_write_all(&host,"root/output",output,sizeof output)||
     memory.output_size!=sizeof output||memcmp(memory.output,output,sizeof output)||
     memory.flushes!=1) return fail("complete write failed");
  memory.write_limit=2;
  if(anygm_vfs_write_all(&host,"root/output",output,sizeof output))
    return fail("short write was accepted");
  memory.write_limit=0;

  if(!anygm_vfs_mkdirs(&host,"root/cache/derived")||memory.creates<2)
    return fail("directory creation did not use host services");
  if(!anygm_vfs_publish(&host,"root/temporary","root/final")||memory.renames!=1)
    return fail("publication did not use host rename");
  anygm_vfs_remove(&host,"root/temporary");
  if(memory.removes!=1) return fail("removal did not use host service");

  AnygmHostServices unavailable={0};
  if(anygm_vfs_can_read(&unavailable)||anygm_vfs_can_write(&unavailable)||
     anygm_vfs_read_all(&unavailable,"root/input",&data,&size,sizeof input)||
     anygm_vfs_write_all(&unavailable,"root/output",output,sizeof output)||
     anygm_vfs_mkdirs(&unavailable,"root/cache"))
    return fail("missing capabilities activated a fallback");

  if(memory.opens!=memory.closes) return fail("file handles were not balanced");
  if(check_casefold_resolution()) return 1;
  puts("VFS capability contract: ok");
  return 0;
}
