/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

typedef struct LibretroFile {
  struct retro_vfs_file_handle *vfs_handle;
  FILE *stdio_handle;
} LibretroFile;

typedef struct LibretroDirectory {
  struct retro_vfs_dir_handle *vfs_handle;
#ifdef _WIN32
  intptr_t find_handle;
  struct _finddata_t find_data;
  int first_pending;
#else
  DIR *stdio_handle;
#endif
} LibretroDirectory;

void libretro_vfs_request(void){
  g_libretro.vfs=NULL;
  if(!g_libretro.environment) return;
  struct retro_vfs_interface_info request;
  memset(&request,0,sizeof request);
  request.required_interface_version=3;
  if(g_libretro.environment(RETRO_ENVIRONMENT_GET_VFS_INTERFACE,&request))
    g_libretro.vfs=request.iface;
}

static unsigned vfs_mode(AnygmFileMode mode){
  unsigned result=0;
  if(mode&ANYGM_FILE_READ) result|=RETRO_VFS_FILE_ACCESS_READ;
  if(mode&ANYGM_FILE_WRITE) result|=RETRO_VFS_FILE_ACCESS_WRITE;
  if((mode&ANYGM_FILE_WRITE) && !(mode&ANYGM_FILE_TRUNCATE))
    result|=RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;
  return result;
}

static const char *stdio_mode(AnygmFileMode mode){
  int read=(mode&ANYGM_FILE_READ)!=0;
  int write=(mode&ANYGM_FILE_WRITE)!=0;
  int truncate=(mode&ANYGM_FILE_TRUNCATE)!=0;
  if(read&&write) return truncate?"w+b":"r+b";
  if(write) return truncate?"wb":"r+b";
  return "rb";
}

static void *host_file_open(void *userdata,const char *path,AnygmFileMode mode){
  (void)userdata;
  if(!path || !(mode&(ANYGM_FILE_READ|ANYGM_FILE_WRITE))) return NULL;
  LibretroFile *file=calloc(1,sizeof *file);
  if(!file) return NULL;
  if(g_libretro.vfs && g_libretro.vfs->open)
    file->vfs_handle=g_libretro.vfs->open(path,vfs_mode(mode),RETRO_VFS_FILE_ACCESS_HINT_NONE);
  else {
    file->stdio_handle=fopen(path,stdio_mode(mode));
    if(!file->stdio_handle && (mode&ANYGM_FILE_CREATE) && (mode&ANYGM_FILE_WRITE) &&
       !(mode&ANYGM_FILE_TRUNCATE))
      file->stdio_handle=fopen(path,(mode&ANYGM_FILE_READ)?"w+b":"wb");
  }
  if(!file->vfs_handle && !file->stdio_handle){ free(file); return NULL; }
  return file;
}

static size_t host_file_read(void *userdata,void *handle,void *data,size_t size){
  (void)userdata;
  LibretroFile *file=handle;
  if(!file || (!data&&size)) return 0;
  if(file->vfs_handle){
    int64_t read=g_libretro.vfs->read(file->vfs_handle,data,(uint64_t)size);
    return read>0?(size_t)read:0;
  }
  return fread(data,1,size,file->stdio_handle);
}

static size_t host_file_write(void *userdata,void *handle,const void *data,size_t size){
  (void)userdata;
  LibretroFile *file=handle;
  if(!file || (!data&&size)) return 0;
  if(file->vfs_handle){
    int64_t written=g_libretro.vfs->write(file->vfs_handle,data,(uint64_t)size);
    return written>0?(size_t)written:0;
  }
  return fwrite(data,1,size,file->stdio_handle);
}

static int64_t host_file_seek(void *userdata,void *handle,int64_t offset,AnygmSeekOrigin origin){
  (void)userdata;
  LibretroFile *file=handle;
  if(!file) return -1;
  if(file->vfs_handle)
    return g_libretro.vfs->seek(file->vfs_handle,offset,(int)origin);
#ifdef _WIN32
  return _fseeki64(file->stdio_handle,offset,(int)origin)==0?_ftelli64(file->stdio_handle):-1;
#else
  return fseeko(file->stdio_handle,(off_t)offset,(int)origin)==0?(int64_t)ftello(file->stdio_handle):-1;
#endif
}

static AnygmResult host_file_flush(void *userdata,void *handle){
  (void)userdata;
  LibretroFile *file=handle;
  if(!file) return ANYGM_ERROR_INVALID_ARGUMENT;
  int result=file->vfs_handle?g_libretro.vfs->flush(file->vfs_handle):fflush(file->stdio_handle);
  return result==0?ANYGM_OK:ANYGM_ERROR_IO;
}

static void host_file_close(void *userdata,void *handle){
  (void)userdata;
  LibretroFile *file=handle;
  if(!file) return;
  if(file->vfs_handle) g_libretro.vfs->close(file->vfs_handle);
  if(file->stdio_handle) fclose(file->stdio_handle);
  free(file);
}

static AnygmResult host_file_stat(void *userdata,const char *path,AnygmFileInfo *info){
  (void)userdata;
  if(!path || !info || info->struct_size<sizeof *info) return ANYGM_ERROR_INVALID_ARGUMENT;
  memset((char *)info+sizeof info->struct_size,0,sizeof *info-sizeof info->struct_size);
  info->struct_size=sizeof *info;
  if(g_libretro.vfs && g_libretro.vfs->stat){
    int32_t short_size=0;
    int flags=g_libretro.vfs->stat(path,&short_size);
    if(!(flags&RETRO_VFS_STAT_IS_VALID)) return ANYGM_ERROR_IO;
    info->flags=ANYGM_FILE_INFO_EXISTS;
    if(flags&RETRO_VFS_STAT_IS_DIRECTORY) info->flags|=ANYGM_FILE_INFO_DIRECTORY;
    else info->flags|=ANYGM_FILE_INFO_REGULAR;
    if(!(flags&RETRO_VFS_STAT_IS_DIRECTORY) && g_libretro.vfs->open && g_libretro.vfs->size){
      struct retro_vfs_file_handle *file=
          g_libretro.vfs->open(path,RETRO_VFS_FILE_ACCESS_READ,RETRO_VFS_FILE_ACCESS_HINT_NONE);
      if(file){ int64_t size=g_libretro.vfs->size(file); g_libretro.vfs->close(file);
        if(size>=0) info->size=(uint64_t)size; }
    }
    return ANYGM_OK;
  }
  struct stat status;
  if(stat(path,&status)!=0) return ANYGM_ERROR_IO;
  info->flags=ANYGM_FILE_INFO_EXISTS;
  if(S_ISDIR(status.st_mode)) info->flags|=ANYGM_FILE_INFO_DIRECTORY;
  if(S_ISREG(status.st_mode)) info->flags|=ANYGM_FILE_INFO_REGULAR;
  if(status.st_size>=0) info->size=(uint64_t)status.st_size;
  return ANYGM_OK;
}

static AnygmResult host_directory_create(void *userdata,const char *path){
  (void)userdata;
  if(!path) return ANYGM_ERROR_INVALID_ARGUMENT;
  int result;
  if(g_libretro.vfs && g_libretro.vfs->mkdir) result=g_libretro.vfs->mkdir(path);
#ifdef _WIN32
  else result=_mkdir(path);
#else
  else result=mkdir(path,0755);
#endif
  return result==0 || result==-2 || errno==EEXIST?ANYGM_OK:ANYGM_ERROR_IO;
}

static AnygmResult host_path_rename(void *userdata,const char *from,const char *to){
  (void)userdata;
  if(!from || !to) return ANYGM_ERROR_INVALID_ARGUMENT;
  int result=g_libretro.vfs&&g_libretro.vfs->rename?
      g_libretro.vfs->rename(from,to):rename(from,to);
  return result==0?ANYGM_OK:ANYGM_ERROR_IO;
}

static AnygmResult host_path_remove(void *userdata,const char *path){
  (void)userdata;
  if(!path) return ANYGM_ERROR_INVALID_ARGUMENT;
  int result=g_libretro.vfs&&g_libretro.vfs->remove?
      g_libretro.vfs->remove(path):remove(path);
  return result==0?ANYGM_OK:ANYGM_ERROR_IO;
}

static void *host_directory_open(void *userdata,const char *path){
  (void)userdata;
  if(!path) return NULL;
  LibretroDirectory *directory=calloc(1,sizeof *directory);
  if(!directory) return NULL;
#ifdef _WIN32
  directory->find_handle=-1;
#endif
  if(g_libretro.vfs && g_libretro.vfs->opendir)
    directory->vfs_handle=g_libretro.vfs->opendir(path,false);
#ifdef _WIN32
  else {
    char pattern[1024];
    if(snprintf(pattern,sizeof pattern,"%s\\*",path)<(int)sizeof pattern){
      directory->find_handle=_findfirst(pattern,&directory->find_data);
      directory->first_pending=directory->find_handle!=-1;
    }
  }
  if(!directory->vfs_handle && directory->find_handle==-1){ free(directory); return NULL; }
#else
  else directory->stdio_handle=opendir(path);
  if(!directory->vfs_handle && !directory->stdio_handle){ free(directory); return NULL; }
#endif
  return directory;
}

static AnygmResult host_directory_read(void *userdata,void *handle,AnygmDirectoryEntry *entry){
  (void)userdata;
  LibretroDirectory *directory=handle;
  if(!directory || !entry || entry->struct_size<sizeof *entry) return ANYGM_ERROR_INVALID_ARGUMENT;
  const char *name=NULL;
  int is_directory=0;
  if(directory->vfs_handle){
    if(!g_libretro.vfs->readdir(directory->vfs_handle)) return ANYGM_RESULT_END;
    name=g_libretro.vfs->dirent_get_name(directory->vfs_handle);
    is_directory=g_libretro.vfs->dirent_is_dir(directory->vfs_handle);
  }
#ifdef _WIN32
  else {
    if(directory->first_pending) directory->first_pending=0;
    else if(_findnext(directory->find_handle,&directory->find_data)!=0) return ANYGM_RESULT_END;
    name=directory->find_data.name;
    is_directory=(directory->find_data.attrib&_A_SUBDIR)!=0;
  }
#else
  else {
    struct dirent *item=readdir(directory->stdio_handle);
    if(!item) return ANYGM_RESULT_END;
    name=item->d_name;
    is_directory=item->d_type==DT_DIR;
  }
#endif
  if(!name) return ANYGM_ERROR_IO;
  memset(entry,0,sizeof *entry);
  entry->struct_size=sizeof *entry;
  entry->flags=ANYGM_FILE_INFO_EXISTS|
      (is_directory?ANYGM_FILE_INFO_DIRECTORY:ANYGM_FILE_INFO_REGULAR);
  snprintf(entry->name,sizeof entry->name,"%s",name);
  return ANYGM_OK;
}

static void host_directory_close(void *userdata,void *handle){
  (void)userdata;
  LibretroDirectory *directory=handle;
  if(!directory) return;
  if(directory->vfs_handle) g_libretro.vfs->closedir(directory->vfs_handle);
#ifdef _WIN32
  if(directory->find_handle!=-1) _findclose(directory->find_handle);
#else
  if(directory->stdio_handle) closedir(directory->stdio_handle);
#endif
  free(directory);
}

void libretro_vfs_services_init(AnygmHostServices *services){
  services->file_open=host_file_open;
  services->file_read=host_file_read;
  services->file_write=host_file_write;
  services->file_seek=host_file_seek;
  services->file_flush=host_file_flush;
  services->file_close=host_file_close;
  services->file_stat=host_file_stat;
  services->directory_create=host_directory_create;
  services->path_rename=host_path_rename;
  services->path_remove=host_path_remove;
  services->directory_open=host_directory_open;
  services->directory_read=host_directory_read;
  services->directory_close=host_directory_close;
}
