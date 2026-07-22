/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm_vfs.h"

#include <stdlib.h>
#include <string.h>

int anygm_vfs_can_read(const AnygmHostServices *host){
  return host&&host->file_open&&host->file_read&&host->file_close;
}

int anygm_vfs_can_write(const AnygmHostServices *host){
  return host&&host->file_open&&host->file_write&&host->file_close;
}

int anygm_vfs_read_all(const AnygmHostServices *host,const char *path,
                       uint8_t **data,size_t *size,size_t limit){
  if(data) *data=NULL;
  if(size) *size=0;
  if(!data || !size || !path || !anygm_vfs_can_read(host)) return 0;
  void *file=host->file_open(host->userdata,path,ANYGM_FILE_READ);
  if(!file) return 0;
  size_t capacity=0;
  size_t exact_size=0;
  int exact_size_known=0;
  if(host->file_seek){
    int64_t end=host->file_seek(host->userdata,file,0,ANYGM_SEEK_END);
    int64_t reset=host->file_seek(host->userdata,file,0,ANYGM_SEEK_START);
    if(end<0 || reset<0){ host->file_close(host->userdata,file); return 0; }
    if((uint64_t)end<=SIZE_MAX && (!limit || (uint64_t)end<=limit)){
      capacity=(size_t)end;
      exact_size=capacity;
      exact_size_known=1;
    }
  }
  if(!capacity) capacity=exact_size_known?1u:65536u;
  if(limit&&capacity>limit) capacity=limit;
  uint8_t *buffer=malloc(capacity?capacity:1);
  if(!buffer){ host->file_close(host->userdata,file); return 0; }
  size_t used=0;
  for(;;){
    if(used==capacity){
      if(exact_size_known) break;
      if(limit&&capacity>=limit){ free(buffer); host->file_close(host->userdata,file); return 0; }
      size_t next=capacity<1024*1024?capacity*2:capacity+capacity/2;
      if(next<capacity || (limit&&next>limit)) next=limit;
      if(next<=capacity){ free(buffer); host->file_close(host->userdata,file); return 0; }
      uint8_t *grown=realloc(buffer,next);
      if(!grown){ free(buffer); host->file_close(host->userdata,file); return 0; }
      buffer=grown; capacity=next;
    }
    size_t count=host->file_read(host->userdata,file,buffer+used,capacity-used);
    if(!count) break;
    if(count>capacity-used){ free(buffer); host->file_close(host->userdata,file); return 0; }
    used+=count;
  }
  int exact_ok=1;
  if(exact_size_known){
    uint8_t extra=0;
    size_t extra_count=host->file_read(host->userdata,file,&extra,1);
    exact_ok=used==exact_size && extra_count==0;
  }
  host->file_close(host->userdata,file);
  if(!exact_ok){ free(buffer); return 0; }
  if(!used){
    *data=buffer;
    *size=0;
    return 1;
  }
  uint8_t *exact=realloc(buffer,used);
  *data=exact?exact:buffer;
  *size=used;
  return 1;
}

int anygm_vfs_read_prefix(const AnygmHostServices *host,const char *path,
                          void *data,size_t capacity,size_t *size){
  if(size) *size=0;
  if(!size || !path || (!data&&capacity) || !anygm_vfs_can_read(host)) return 0;
  void *file=host->file_open(host->userdata,path,ANYGM_FILE_READ);
  if(!file) return 0;
  size_t used=0;
  while(used<capacity){
    size_t count=host->file_read(host->userdata,file,(uint8_t *)data+used,capacity-used);
    if(!count) break;
    if(count>capacity-used){ host->file_close(host->userdata,file); return 0; }
    used+=count;
  }
  host->file_close(host->userdata,file);
  *size=used;
  return 1;
}

int anygm_vfs_write_all(const AnygmHostServices *host,const char *path,
                        const void *data,size_t size){
  if(!path || (!data&&size) || !anygm_vfs_can_write(host)) return 0;
  void *file=host->file_open(host->userdata,path,
                             ANYGM_FILE_WRITE|ANYGM_FILE_CREATE|ANYGM_FILE_TRUNCATE);
  if(!file) return 0;
  size_t written=0;
  while(written<size){
    size_t count=host->file_write(host->userdata,file,(const uint8_t *)data+written,size-written);
    if(!count || count>size-written) break;
    written+=count;
  }
  int ok=written==size;
  if(ok && host->file_flush)
    ok=host->file_flush(host->userdata,file)==ANYGM_OK;
  host->file_close(host->userdata,file);
  return ok;
}

int anygm_vfs_copy(const AnygmHostServices *host,const char *source,const char *destination){
  if(!source || !destination || !anygm_vfs_can_read(host) || !anygm_vfs_can_write(host)) return 0;
  void *input=host->file_open(host->userdata,source,ANYGM_FILE_READ);
  if(!input) return 0;
  void *output=host->file_open(host->userdata,destination,
                              ANYGM_FILE_WRITE|ANYGM_FILE_CREATE|ANYGM_FILE_TRUNCATE);
  if(!output){ host->file_close(host->userdata,input); return 0; }
  uint8_t buffer[16384];
  int ok=1;
  for(;;){
    size_t count=host->file_read(host->userdata,input,buffer,sizeof buffer);
    if(!count) break;
    size_t written=0;
    while(written<count){
      size_t step=host->file_write(host->userdata,output,buffer+written,count-written);
      if(!step || step>count-written){ ok=0; break; }
      written+=step;
    }
    if(!ok) break;
  }
  if(ok && host->file_flush) ok=host->file_flush(host->userdata,output)==ANYGM_OK;
  host->file_close(host->userdata,input);
  host->file_close(host->userdata,output);
  return ok;
}

int anygm_vfs_stat(const AnygmHostServices *host,const char *path,AnygmFileInfo *info){
  if(!host || !host->file_stat || !path || !info) return 0;
  memset(info,0,sizeof *info);
  info->struct_size=sizeof *info;
  return host->file_stat(host->userdata,path,info)==ANYGM_OK &&
         (info->flags&ANYGM_FILE_INFO_EXISTS)!=0;
}

int anygm_vfs_mkdirs(const AnygmHostServices *host,const char *path){
  if(!host || !host->directory_create || !path || !path[0]) return 0;
  size_t length=strlen(path);
  if(length>=4096) return 0;
  char copy[4096];
  memcpy(copy,path,length+1);
  for(char *cursor=copy+1;*cursor;cursor++){
    if(*cursor!='/'&&*cursor!='\\') continue;
    char saved=*cursor;
    *cursor=0;
    if(copy[0] && !(copy[1]==':'&&copy[2]==0))
      host->directory_create(host->userdata,copy);
    *cursor=saved;
  }
  if(host->directory_create(host->userdata,copy)!=ANYGM_OK){
    AnygmFileInfo info;
    return anygm_vfs_stat(host,copy,&info) && (info.flags&ANYGM_FILE_INFO_DIRECTORY);
  }
  return 1;
}

int anygm_vfs_publish(const AnygmHostServices *host,const char *temporary,const char *destination){
  return host&&host->path_rename&&temporary&&destination&&
         host->path_rename(host->userdata,temporary,destination)==ANYGM_OK;
}

void anygm_vfs_remove(const AnygmHostServices *host,const char *path){
  if(host&&host->path_remove&&path) host->path_remove(host->userdata,path);
}
