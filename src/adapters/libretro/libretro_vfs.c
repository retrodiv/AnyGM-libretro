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
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef struct LibretroFile {
  struct retro_vfs_file_handle *vfs_handle;
  FILE *stdio_handle;
} LibretroFile;

typedef struct LibretroMapping {
  const void *data;
  size_t size;
#ifdef _WIN32
  HANDLE handle;
#endif
} LibretroMapping;

typedef struct LibretroDirectory {
  struct retro_vfs_dir_handle *vfs_handle;
#ifdef _WIN32
  intptr_t find_handle;
  struct _finddata_t find_data;
  int first_pending;
#else
  DIR *stdio_handle;
  /* Kept so an entry whose kind readdir does not know can be stat'ed by its full path. */
  char stdio_path[2048];
#endif
} LibretroDirectory;

/* Libretro VFS paths always use forward slashes, including on Windows. Frontends
 * may reject native separators, so normalize only at the adapter boundary and
 * leave the portable runtime's path namespace unchanged. */
static const char *frontend_path(const char *path,char *normalized,size_t capacity){
  if(!path || !normalized || !capacity) return NULL;
  size_t length=strlen(path);
  if(length>=capacity) return NULL;
  for(size_t i=0;i<=length;i++) normalized[i]=path[i]=='\\'?'/' : path[i];
  return normalized;
}

/* Mapping is an optional host optimization for large immutable files. Virtual or non-native
 * frontend paths fail this callback and remain on the ordinary VFS-backed path. */
static void host_file_unmap(void *userdata,void *handle,const void *data,size_t size){
  (void)userdata;
  (void)data;
  (void)size;
  LibretroMapping *mapping=handle;
  if(!mapping) return;
#ifdef _WIN32
  if(mapping->data) UnmapViewOfFile(mapping->data);
  if(mapping->handle) CloseHandle(mapping->handle);
#else
  if(mapping->data && mapping->size)
    munmap((void *)(uintptr_t)mapping->data,mapping->size);
#endif
  free(mapping);
}

static void *host_file_map(void *userdata,const char *path,const void **data,size_t *size){
  (void)userdata;
  if(data) *data=NULL;
  if(size) *size=0;
  if(!path || !path[0] || !data || !size) return NULL;
  const void *mapped=NULL;
  size_t length=0;
#ifdef _WIN32
  HANDLE file=CreateFileA(path,GENERIC_READ,
                          FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                          NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
  if(file==INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER file_size;
  if(!GetFileSizeEx(file,&file_size) || file_size.QuadPart<=0 ||
     (uint64_t)file_size.QuadPart>(uint64_t)SIZE_MAX){
    CloseHandle(file);
    return NULL;
  }
  HANDLE native_mapping=CreateFileMappingA(file,NULL,PAGE_READONLY,0,0,NULL);
  CloseHandle(file);
  if(!native_mapping) return NULL;
  mapped=MapViewOfFile(native_mapping,FILE_MAP_READ,0,0,0);
  if(!mapped){ CloseHandle(native_mapping); return NULL; }
  length=(size_t)file_size.QuadPart;
#else
  int file=open(path,O_RDONLY);
  if(file<0) return NULL;
  struct stat status;
  if(fstat(file,&status)!=0 || status.st_size<=0 ||
     (uint64_t)status.st_size>(uint64_t)SIZE_MAX){
    close(file);
    return NULL;
  }
  length=(size_t)status.st_size;
  mapped=mmap(NULL,length,PROT_READ,MAP_PRIVATE,file,0);
  close(file);
  if(mapped==MAP_FAILED) return NULL;
#endif
  LibretroMapping *mapping=calloc(1,sizeof *mapping);
  if(!mapping){
#ifdef _WIN32
    UnmapViewOfFile(mapped);
    CloseHandle(native_mapping);
#else
    munmap((void *)(uintptr_t)mapped,length);
#endif
    return NULL;
  }
  mapping->data=mapped;
  mapping->size=length;
#ifdef _WIN32
  mapping->handle=native_mapping;
#endif
  *data=mapped;
  *size=length;
  return mapping;
}

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
  if(g_libretro.vfs && g_libretro.vfs->open){
    char normalized[4096];
    const char *vfs_path=frontend_path(path,normalized,sizeof normalized);
    if(vfs_path)
      file->vfs_handle=g_libretro.vfs->open(vfs_path,vfs_mode(mode),
                                           RETRO_VFS_FILE_ACCESS_HINT_NONE);
  }
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

static int64_t frontend_seek(struct retro_vfs_file_handle *file,int64_t offset,int origin){
  int64_t result=g_libretro.vfs->seek(file,offset,origin);
  if(result<0) return -1;
  /* Some established frontends return a status code here despite the libretro
   * contract specifying the new position. Prefer the separately negotiated
   * tell operation so the host-facing AnyGM contract remains unambiguous. */
  if(g_libretro.vfs->tell){
    int64_t position=g_libretro.vfs->tell(file);
    if(position>=0) return position;
  }
  return result;
}

static int64_t host_file_seek(void *userdata,void *handle,int64_t offset,AnygmSeekOrigin origin){
  (void)userdata;
  LibretroFile *file=handle;
  if(!file) return -1;
  if(file->vfs_handle){
    int seek_position=origin==ANYGM_SEEK_START?RETRO_VFS_SEEK_POSITION_START:
                      origin==ANYGM_SEEK_CURRENT?RETRO_VFS_SEEK_POSITION_CURRENT:
                      origin==ANYGM_SEEK_END?RETRO_VFS_SEEK_POSITION_END:-1;
    if(seek_position<0) return -1;
    /* Libretro requires end-relative offsets to be negative. AnyGM follows the
     * usual stdio contract where zero means the exact end, so translate through
     * the VFS size operation and seek from the start. */
    if(origin==ANYGM_SEEK_END){
      if(!g_libretro.vfs->size) return -1;
      int64_t size=g_libretro.vfs->size(file->vfs_handle);
      if(size<0 || (offset>0 && size>INT64_MAX-offset) ||
         (offset<0 && size<INT64_MIN-offset)) return -1;
      int64_t result=frontend_seek(file->vfs_handle,size+offset,
                                   RETRO_VFS_SEEK_POSITION_START);
      return result;
    }
    return frontend_seek(file->vfs_handle,offset,seek_position);
  }
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
    char normalized[4096];
    const char *vfs_path=frontend_path(path,normalized,sizeof normalized);
    if(!vfs_path) return ANYGM_ERROR_IO;
    int32_t short_size=0;
    int flags=g_libretro.vfs->stat(vfs_path,&short_size);
    if(!(flags&RETRO_VFS_STAT_IS_VALID)) return ANYGM_ERROR_IO;
    info->flags=ANYGM_FILE_INFO_EXISTS;
    if(flags&RETRO_VFS_STAT_IS_DIRECTORY) info->flags|=ANYGM_FILE_INFO_DIRECTORY;
    else info->flags|=ANYGM_FILE_INFO_REGULAR;
    if(!(flags&RETRO_VFS_STAT_IS_DIRECTORY) && g_libretro.vfs->open && g_libretro.vfs->size){
      struct retro_vfs_file_handle *file=
          g_libretro.vfs->open(vfs_path,RETRO_VFS_FILE_ACCESS_READ,
                               RETRO_VFS_FILE_ACCESS_HINT_NONE);
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
  /* A value left over from an unrelated call would otherwise satisfy the EEXIST test below. */
  errno=0;
  if(g_libretro.vfs && g_libretro.vfs->mkdir){
    char normalized[4096];
    const char *vfs_path=frontend_path(path,normalized,sizeof normalized);
    result=vfs_path?g_libretro.vfs->mkdir(vfs_path):-1;
  }
#ifdef _WIN32
  else result=_mkdir(path);
#else
  else result=mkdir(path,0755);
#endif
  /* -2 is the frontend VFS spelling of "already there". errno only means anything on the stdio
   * branch: the frontend's mkdir is not required to set it, so consulting it after a VFS failure
   * reads a value from whatever ran last. */
  if(result==0 || result==-2) return ANYGM_OK;
  int used_vfs=g_libretro.vfs && g_libretro.vfs->mkdir;
  return (!used_vfs && errno==EEXIST)?ANYGM_OK:ANYGM_ERROR_IO;
}

static AnygmResult host_path_rename(void *userdata,const char *from,const char *to){
  (void)userdata;
  if(!from || !to) return ANYGM_ERROR_INVALID_ARGUMENT;
  int result;
  if(g_libretro.vfs&&g_libretro.vfs->rename){
    char normalized_from[4096],normalized_to[4096];
    const char *vfs_from=frontend_path(from,normalized_from,sizeof normalized_from);
    const char *vfs_to=frontend_path(to,normalized_to,sizeof normalized_to);
    result=vfs_from&&vfs_to?g_libretro.vfs->rename(vfs_from,vfs_to):-1;
  } else result=rename(from,to);
  return result==0?ANYGM_OK:ANYGM_ERROR_IO;
}

static AnygmResult host_path_remove(void *userdata,const char *path){
  (void)userdata;
  if(!path) return ANYGM_ERROR_INVALID_ARGUMENT;
  int result;
  if(g_libretro.vfs&&g_libretro.vfs->remove){
    char normalized[4096];
    const char *vfs_path=frontend_path(path,normalized,sizeof normalized);
    result=vfs_path?g_libretro.vfs->remove(vfs_path):-1;
  } else result=remove(path);
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
  {
    char normalized[4096];
    const char *vfs_path=frontend_path(path,normalized,sizeof normalized);
    if(vfs_path) directory->vfs_handle=g_libretro.vfs->opendir(vfs_path,false);
  }
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
  else {
    directory->stdio_handle=opendir(path);
    if(directory->stdio_handle){
      int written=snprintf(directory->stdio_path,sizeof directory->stdio_path,"%s",path);
      /* A path too long to keep is a path the d_type fallback below cannot use; the walk still
       * works, it just has nothing better than d_type to answer with. */
      if(written<0 || (size_t)written>=sizeof directory->stdio_path) directory->stdio_path[0]=0;
    }
  }
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
    /* d_type is optional. XFS, several FUSE filesystems and some Android volumes answer
     * DT_UNKNOWN for everything, and reading that as "not a directory" makes every directory
     * on such a volume look like a file. */
    if(item->d_type!=DT_UNKNOWN) is_directory=item->d_type==DT_DIR;
    else if(directory->stdio_path[0]){
      char full[3072];
      struct stat info;
      int written=snprintf(full,sizeof full,"%s/%s",directory->stdio_path,item->d_name);
      if(written>0 && (size_t)written<sizeof full && stat(full,&info)==0)
        is_directory=S_ISDIR(info.st_mode);
    }
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
  services->file_map=host_file_map;
  services->file_unmap=host_file_unmap;
  services->file_stat=host_file_stat;
  services->directory_create=host_directory_create;
  services->path_rename=host_path_rename;
  services->path_remove=host_path_remove;
  services->directory_open=host_directory_open;
  services->directory_read=host_directory_read;
  services->directory_close=host_directory_close;
}
