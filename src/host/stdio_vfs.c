/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "stdio_vfs.h"
#include "gml_win.h"

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

typedef struct AnygmStdioDirectory {
#ifdef _WIN32
  intptr_t find_handle;
  struct _finddata_t find_data;
  int first_pending;
#else
  DIR *handle;
#endif
} AnygmStdioDirectory;

static const char *open_mode(AnygmFileMode mode){
  int read=(mode&ANYGM_FILE_READ)!=0;
  int write=(mode&ANYGM_FILE_WRITE)!=0;
  int truncate=(mode&ANYGM_FILE_TRUNCATE)!=0;
  if(read&&write) return truncate?"w+b":"r+b";
  if(write) return truncate?"wb":"r+b";
  return "rb";
}

static void *stdio_open(void *userdata,const char *path,AnygmFileMode mode){
  (void)userdata;
  if(!path || !(mode&(ANYGM_FILE_READ|ANYGM_FILE_WRITE))) return NULL;
  FILE *file=fopen(path,open_mode(mode));
  if(!file && (mode&ANYGM_FILE_CREATE) && (mode&ANYGM_FILE_WRITE) &&
     !(mode&ANYGM_FILE_TRUNCATE))
    file=fopen(path,(mode&ANYGM_FILE_READ)?"w+b":"wb");
  return file;
}

static size_t stdio_read(void *userdata,void *file,void *data,size_t size){
  (void)userdata;
  return file?fread(data,1,size,(FILE *)file):0;
}

static size_t stdio_write(void *userdata,void *file,const void *data,size_t size){
  (void)userdata;
  return file?fwrite(data,1,size,(FILE *)file):0;
}

static int64_t stdio_seek(void *userdata,void *file,int64_t offset,AnygmSeekOrigin origin){
  (void)userdata;
  if(!file) return -1;
#ifdef _WIN32
  return _fseeki64((FILE *)file,offset,(int)origin)==0?_ftelli64((FILE *)file):-1;
#else
  return fseeko((FILE *)file,(off_t)offset,(int)origin)==0?(int64_t)ftello((FILE *)file):-1;
#endif
}

static AnygmResult stdio_flush(void *userdata,void *file){
  (void)userdata;
  return file&&fflush((FILE *)file)==0?ANYGM_OK:ANYGM_ERROR_IO;
}

static void stdio_close(void *userdata,void *file){
  (void)userdata;
  if(file) fclose((FILE *)file);
}

static AnygmResult stdio_stat(void *userdata,const char *path,AnygmFileInfo *info){
  (void)userdata;
  if(!path || !info || info->struct_size<sizeof *info) return ANYGM_ERROR_INVALID_ARGUMENT;
  struct stat status;
  if(stat(path,&status)!=0) return ANYGM_ERROR_IO;
  memset(info,0,sizeof *info);
  info->struct_size=sizeof *info;
  info->flags=ANYGM_FILE_INFO_EXISTS;
  if(S_ISREG(status.st_mode)) info->flags|=ANYGM_FILE_INFO_REGULAR;
  if(S_ISDIR(status.st_mode)) info->flags|=ANYGM_FILE_INFO_DIRECTORY;
  if(status.st_size>=0) info->size=(uint64_t)status.st_size;
  return ANYGM_OK;
}

static AnygmResult stdio_mkdir(void *userdata,const char *path){
  (void)userdata;
#ifdef _WIN32
  int result=_mkdir(path);
#else
  int result=mkdir(path,0755);
#endif
  return result==0 || errno==EEXIST?ANYGM_OK:ANYGM_ERROR_IO;
}

static AnygmResult stdio_rename(void *userdata,const char *from,const char *to){
  (void)userdata;
  return from&&to&&rename(from,to)==0?ANYGM_OK:ANYGM_ERROR_IO;
}

static AnygmResult stdio_remove(void *userdata,const char *path){
  (void)userdata;
  return path&&remove(path)==0?ANYGM_OK:ANYGM_ERROR_IO;
}

static void *stdio_directory_open(void *userdata,const char *path){
  (void)userdata;
  if(!path) return NULL;
  AnygmStdioDirectory *directory=calloc(1,sizeof *directory);
  if(!directory) return NULL;
#ifdef _WIN32
  char pattern[1024];
  directory->find_handle=-1;
  if(snprintf(pattern,sizeof pattern,"%s\\*",path)>=(int)sizeof pattern){ free(directory); return NULL; }
  directory->find_handle=_findfirst(pattern,&directory->find_data);
  if(directory->find_handle==-1){ free(directory); return NULL; }
  directory->first_pending=1;
#else
  directory->handle=opendir(path);
  if(!directory->handle){ free(directory); return NULL; }
#endif
  return directory;
}

static AnygmResult stdio_directory_read(void *userdata,void *handle,AnygmDirectoryEntry *entry){
  (void)userdata;
  AnygmStdioDirectory *directory=handle;
  if(!directory || !entry || entry->struct_size<sizeof *entry) return ANYGM_ERROR_INVALID_ARGUMENT;
  const char *name=NULL;
  int is_directory=0;
#ifdef _WIN32
  if(directory->first_pending) directory->first_pending=0;
  else if(_findnext(directory->find_handle,&directory->find_data)!=0) return ANYGM_RESULT_END;
  name=directory->find_data.name;
  is_directory=(directory->find_data.attrib&_A_SUBDIR)!=0;
#else
  struct dirent *item=readdir(directory->handle);
  if(!item) return ANYGM_RESULT_END;
  name=item->d_name;
  is_directory=item->d_type==DT_DIR;
#endif
  memset(entry,0,sizeof *entry);
  entry->struct_size=sizeof *entry;
  entry->flags=ANYGM_FILE_INFO_EXISTS|
      (is_directory?ANYGM_FILE_INFO_DIRECTORY:ANYGM_FILE_INFO_REGULAR);
  snprintf(entry->name,sizeof entry->name,"%s",name);
  return ANYGM_OK;
}

static void stdio_directory_close(void *userdata,void *handle){
  (void)userdata;
  AnygmStdioDirectory *directory=handle;
  if(!directory) return;
#ifdef _WIN32
  if(directory->find_handle!=-1) _findclose(directory->find_handle);
#else
  if(directory->handle) closedir(directory->handle);
#endif
  free(directory);
}

static const char *stdio_development_setting(void *userdata,const char *name){
  (void)userdata;
  return name?getenv(name):NULL;
}

static const AnygmHostServices stdio_host_services={
  .struct_size=sizeof(AnygmHostServices),
  .abi_version=ANYGM_HOST_SERVICES_VERSION,
  .file_open=stdio_open,
  .file_read=stdio_read,
  .file_write=stdio_write,
  .file_seek=stdio_seek,
  .file_flush=stdio_flush,
  .file_close=stdio_close,
  .file_stat=stdio_stat,
  .directory_create=stdio_mkdir,
  .path_rename=stdio_rename,
  .path_remove=stdio_remove,
  .directory_open=stdio_directory_open,
  .directory_read=stdio_directory_read,
  .directory_close=stdio_directory_close,
  .development_setting=stdio_development_setting
};

void anygm_stdio_vfs_services_init(AnygmHostServices *services){
  if(!services) return;
  services->file_open=stdio_open;
  services->file_read=stdio_read;
  services->file_write=stdio_write;
  services->file_seek=stdio_seek;
  services->file_flush=stdio_flush;
  services->file_close=stdio_close;
  services->file_stat=stdio_stat;
  services->directory_create=stdio_mkdir;
  services->path_rename=stdio_rename;
  services->path_remove=stdio_remove;
  services->directory_open=stdio_directory_open;
  services->directory_read=stdio_directory_read;
  services->directory_close=stdio_directory_close;
}

int anygm_stdio_load_win(struct GmlWin *win,const char *path){
  return gml_win_load_host(win,&stdio_host_services,path);
}
