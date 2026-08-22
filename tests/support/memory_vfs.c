/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "memory_vfs.h"

#include <stdlib.h>
#include <string.h>

typedef struct MemoryFile {
  AnygmMemoryVfs *memory;
  AnygmMemoryVfsNode *node;
  size_t position;
} MemoryFile;

typedef struct MemoryDirectory {
  AnygmMemoryVfs *memory;
  char path[1024];
  size_t index;
} MemoryDirectory;

static AnygmMemoryVfsNode *memory_find(AnygmMemoryVfs *memory,const char *path){
  if(!memory || !path) return NULL;
  for(size_t index=0;index<memory->node_count;index++)
    if(!strcmp(memory->nodes[index].path,path)) return &memory->nodes[index];
  return NULL;
}

static AnygmMemoryVfsNode *memory_add(AnygmMemoryVfs *memory,const char *path,int directory){
  AnygmMemoryVfsNode *node=memory_find(memory,path);
  if(node) return node->directory==directory?node:NULL;
  if(!memory || !path || !path[0] || strlen(path)>=sizeof memory->nodes[0].path ||
     memory->node_count>=sizeof memory->nodes/sizeof memory->nodes[0]) return NULL;
  node=&memory->nodes[memory->node_count++];
  memset(node,0,sizeof *node);
  memcpy(node->path,path,strlen(path)+1u);
  node->directory=directory;
  return node;
}

int anygm_memory_vfs_add_file(AnygmMemoryVfs *memory,const char *path,
                              const void *data,size_t size){
  AnygmMemoryVfsNode *node=memory_add(memory,path,0);
  if(!node || (!data && size)) return 0;
  uint8_t *copy=malloc(size?size:1u);
  if(!copy) return 0;
  if(size) memcpy(copy,data,size);
  free(node->data);
  node->data=copy;
  node->size=node->capacity=size;
  return 1;
}

static void *memory_open(void *userdata,const char *path,AnygmFileMode mode){
  AnygmMemoryVfs *memory=userdata;
  AnygmMemoryVfsNode *node=memory_find(memory,path);
  if(mode&ANYGM_FILE_WRITE){
    if(!node) node=memory_add(memory,path,0);
    if(!node || node->directory) return NULL;
    if(mode&ANYGM_FILE_TRUNCATE) node->size=0;
  }else if(!node || node->directory) return NULL;
  MemoryFile *file=calloc(1,sizeof *file);
  if(!file) return NULL;
  file->memory=memory;
  file->node=node;
  return file;
}

static size_t memory_read(void *userdata,void *opaque,void *data,size_t size){
  AnygmMemoryVfs *memory=userdata;
  MemoryFile *file=opaque;
  if(!file || file->memory!=memory || (!data && size)) return 0;
  if(size>memory->max_read_request) memory->max_read_request=size;
  if(memory->guard_active && !strcmp(file->node->path,memory->guarded_path) &&
     ((uint64_t)file->position<memory->guard_begin ||
      (uint64_t)file->position>memory->guard_end ||
      (uint64_t)size>memory->guard_end-(uint64_t)file->position)){
    memory->read_violation=1;
    return 0;
  }
  size_t remaining=file->node->size-file->position;
  if(size>remaining) size=remaining;
  if(size) memcpy(data,file->node->data+file->position,size);
  file->position+=size;
  return size;
}

static size_t memory_write(void *userdata,void *opaque,const void *data,size_t size){
  AnygmMemoryVfs *memory=userdata;
  MemoryFile *file=opaque;
  if(!file || file->memory!=memory || (!data && size) || size>SIZE_MAX-file->position) return 0;
  size_t needed=file->position+size;
  if(needed>file->node->capacity){
    size_t capacity=file->node->capacity?file->node->capacity:64u;
    while(capacity<needed){
      if(capacity>SIZE_MAX/2u) return 0;
      capacity*=2u;
    }
    uint8_t *grown=realloc(file->node->data,capacity);
    if(!grown) return 0;
    file->node->data=grown;
    file->node->capacity=capacity;
  }
  if(size) memcpy(file->node->data+file->position,data,size);
  file->position=needed;
  if(file->node->size<needed) file->node->size=needed;
  return size;
}

static int64_t memory_seek(void *userdata,void *opaque,int64_t offset,AnygmSeekOrigin origin){
  MemoryFile *file=opaque;
  if(!file || file->memory!=userdata) return -1;
  int64_t base=origin==ANYGM_SEEK_START?0:
               origin==ANYGM_SEEK_CURRENT?(int64_t)file->position:
               origin==ANYGM_SEEK_END?(int64_t)file->node->size:-1;
  if(base<0 || offset< -base || (uint64_t)(base+offset)>file->node->size) return -1;
  file->position=(size_t)(base+offset);
  return base+offset;
}

static AnygmResult memory_flush(void *userdata,void *opaque){
  MemoryFile *file=opaque;
  return file && file->memory==userdata?ANYGM_OK:ANYGM_ERROR_IO;
}

static void memory_close(void *userdata,void *opaque){
  MemoryFile *file=opaque;
  if(file && file->memory==userdata) free(file);
}

static AnygmResult memory_stat(void *userdata,const char *path,AnygmFileInfo *info){
  AnygmMemoryVfsNode *node=memory_find(userdata,path);
  if(!node || !info) return ANYGM_ERROR_IO;
  info->flags|=ANYGM_FILE_INFO_EXISTS|
    (node->directory?ANYGM_FILE_INFO_DIRECTORY:ANYGM_FILE_INFO_REGULAR);
  info->size=node->size;
  return ANYGM_OK;
}

static AnygmResult memory_mkdir(void *userdata,const char *path){
  return memory_add(userdata,path,1)?ANYGM_OK:ANYGM_ERROR_IO;
}

static AnygmResult memory_remove(void *userdata,const char *path){
  AnygmMemoryVfs *memory=userdata;
  AnygmMemoryVfsNode *node=memory_find(memory,path);
  if(!node) return ANYGM_OK;
  if(node->directory){
    size_t length=strlen(path);
    for(size_t index=0;index<memory->node_count;index++)
      if(&memory->nodes[index]!=node && !strncmp(memory->nodes[index].path,path,length) &&
         memory->nodes[index].path[length]=='/') return ANYGM_ERROR_IO;
  }
  size_t index=(size_t)(node-memory->nodes);
  free(node->data);
  memory->nodes[index]=memory->nodes[--memory->node_count];
  memset(&memory->nodes[memory->node_count],0,sizeof memory->nodes[0]);
  return ANYGM_OK;
}

static AnygmResult memory_rename(void *userdata,const char *from,const char *to){
  AnygmMemoryVfs *memory=userdata;
  AnygmMemoryVfsNode *node=memory_find(memory,from);
  if(!node || memory_find(memory,to)) return ANYGM_ERROR_IO;
  size_t from_size=strlen(from),to_size=strlen(to);
  for(size_t index=0;index<memory->node_count;index++){
    char *path=memory->nodes[index].path;
    if(strcmp(path,from) && (strncmp(path,from,from_size) || path[from_size]!='/')) continue;
    const char *suffix=path+from_size;
    if(to_size+strlen(suffix)>=sizeof memory->nodes[index].path) return ANYGM_ERROR_IO;
  }
  for(size_t index=0;index<memory->node_count;index++){
    char *path=memory->nodes[index].path;
    if(strcmp(path,from) && (strncmp(path,from,from_size) || path[from_size]!='/')) continue;
    char renamed[1024];
    memcpy(renamed,to,to_size);
    memcpy(renamed+to_size,path+from_size,strlen(path+from_size)+1u);
    memcpy(path,renamed,strlen(renamed)+1u);
  }
  return ANYGM_OK;
}

static void *memory_directory_open(void *userdata,const char *path){
  AnygmMemoryVfsNode *node=memory_find(userdata,path);
  if(!node || !node->directory) return NULL;
  MemoryDirectory *directory=calloc(1,sizeof *directory);
  if(!directory) return NULL;
  directory->memory=userdata;
  memcpy(directory->path,path,strlen(path)+1u);
  return directory;
}

static AnygmResult memory_directory_read(void *userdata,void *opaque,AnygmDirectoryEntry *entry){
  AnygmMemoryVfs *memory=userdata;
  MemoryDirectory *directory=opaque;
  if(!directory || directory->memory!=memory || !entry) return ANYGM_ERROR_IO;
  size_t prefix=strlen(directory->path);
  while(directory->index<memory->node_count){
    AnygmMemoryVfsNode *node=&memory->nodes[directory->index++];
    if(strncmp(node->path,directory->path,prefix) || node->path[prefix]!='/' ||
       strchr(node->path+prefix+1u,'/')) continue;
    const char *name=node->path+prefix+1u;
    if(strlen(name)>=sizeof entry->name) return ANYGM_ERROR_IO;
    memcpy(entry->name,name,strlen(name)+1u);
    entry->flags=node->directory?ANYGM_FILE_INFO_DIRECTORY:ANYGM_FILE_INFO_REGULAR;
    return ANYGM_OK;
  }
  return ANYGM_ERROR_IO;
}

static void memory_directory_close(void *userdata,void *opaque){
  MemoryDirectory *directory=opaque;
  if(directory && directory->memory==userdata) free(directory);
}

void anygm_memory_vfs_init(AnygmMemoryVfs *memory,AnygmHostServices *services){
  if(!memory || !services) return;
  memset(memory,0,sizeof *memory);
  memset(services,0,sizeof *services);
  memory_add(memory,"mem",1);
  services->struct_size=sizeof *services;
  services->abi_version=ANYGM_HOST_SERVICES_VERSION;
  services->userdata=memory;
  services->file_open=memory_open;
  services->file_read=memory_read;
  services->file_write=memory_write;
  services->file_seek=memory_seek;
  services->file_flush=memory_flush;
  services->file_close=memory_close;
  services->file_stat=memory_stat;
  services->directory_create=memory_mkdir;
  services->path_remove=memory_remove;
  services->path_rename=memory_rename;
  services->directory_open=memory_directory_open;
  services->directory_read=memory_directory_read;
  services->directory_close=memory_directory_close;
}

void anygm_memory_vfs_guard_reads(AnygmMemoryVfs *memory,const char *path,
                                  uint64_t begin,uint64_t size){
  if(!memory || !path || begin>UINT64_MAX-size) return;
  memcpy(memory->guarded_path,path,strlen(path)+1u);
  memory->guard_begin=begin;
  memory->guard_end=begin+size;
  memory->guard_active=1;
  memory->read_violation=0;
  memory->max_read_request=0;
}

void anygm_memory_vfs_destroy(AnygmMemoryVfs *memory){
  if(!memory) return;
  for(size_t index=0;index<memory->node_count;index++) free(memory->nodes[index].data);
  memset(memory,0,sizeof *memory);
}
