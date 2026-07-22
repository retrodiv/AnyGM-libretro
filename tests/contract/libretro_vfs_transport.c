/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "libretro_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct retro_vfs_file_handle {
  size_t position;
};

static const uint8_t content[]={'F','O','R','M',1,2,3,4};
static struct retro_vfs_file_handle stream;
static int invalid_path_seen;
static int invalid_end_seek_seen;

LibretroAdapter g_libretro;

static int valid_path(const char *path){
  int valid=path && !strcmp(path,"C:/content/data.win");
  if(!valid) invalid_path_seen=1;
  return valid;
}

static struct retro_vfs_file_handle *fake_open(const char *path,unsigned mode,unsigned hints){
  (void)hints;
  if(!valid_path(path) || mode!=RETRO_VFS_FILE_ACCESS_READ) return NULL;
  stream.position=0;
  return &stream;
}

static int fake_close(struct retro_vfs_file_handle *file){ return file==&stream?0:-1; }
static int64_t fake_size(struct retro_vfs_file_handle *file){
  return file==&stream?(int64_t)sizeof content:-1;
}
static int64_t fake_tell(struct retro_vfs_file_handle *file){
  return file==&stream?(int64_t)file->position:-1;
}
static int64_t fake_seek(struct retro_vfs_file_handle *file,int64_t offset,int origin){
  if(file!=&stream) return -1;
  if(origin==RETRO_VFS_SEEK_POSITION_END && offset>=0){
    invalid_end_seek_seen=1;
    return -1;
  }
  int64_t base=origin==RETRO_VFS_SEEK_POSITION_START?0:
               origin==RETRO_VFS_SEEK_POSITION_CURRENT?(int64_t)file->position:
               origin==RETRO_VFS_SEEK_POSITION_END?(int64_t)sizeof content:-1;
  if(base<0 || offset< -base || offset>(int64_t)sizeof content-base) return -1;
  file->position=(size_t)(base+offset);
  return 0;
}
static int64_t fake_read(struct retro_vfs_file_handle *file,void *data,uint64_t size){
  if(file!=&stream || (!data&&size)) return -1;
  size_t remaining=sizeof content-file->position;
  size_t amount=size<remaining?(size_t)size:remaining;
  if(amount) memcpy(data,content+file->position,amount);
  file->position+=amount;
  return (int64_t)amount;
}
static int fake_stat(const char *path,int32_t *size){
  if(!valid_path(path)) return 0;
  if(size) *size=(int32_t)sizeof content;
  return RETRO_VFS_STAT_IS_VALID;
}

static struct retro_vfs_interface fake_vfs={
  .open=fake_open,
  .close=fake_close,
  .size=fake_size,
  .tell=fake_tell,
  .seek=fake_seek,
  .read=fake_read,
  .stat=fake_stat
};

static bool environment_callback(unsigned command,void *data){
  if(command!=RETRO_ENVIRONMENT_GET_VFS_INTERFACE || !data) return false;
  struct retro_vfs_interface_info *request=data;
  if(request->required_interface_version!=3) return false;
  request->iface=&fake_vfs;
  return true;
}

int main(void){
  memset(&g_libretro,0,sizeof g_libretro);
  g_libretro.environment=environment_callback;
  libretro_vfs_request();
  AnygmHostServices services;
  memset(&services,0,sizeof services);
  services.struct_size=sizeof services;
  libretro_vfs_services_init(&services);

  AnygmFileInfo info;
  memset(&info,0,sizeof info);
  info.struct_size=sizeof info;
  if(services.file_stat(NULL,"C:\\content\\data.win",&info)!=ANYGM_OK ||
     !(info.flags&ANYGM_FILE_INFO_REGULAR) || info.size!=sizeof content){
    fprintf(stderr,"libretro VFS stat transport failed\n");
    return 1;
  }
  void *file=services.file_open(NULL,"C:\\content\\data.win",ANYGM_FILE_READ);
  uint8_t copy[sizeof content];
  if(!file || services.file_seek(NULL,file,0,ANYGM_SEEK_END)!=(int64_t)sizeof content ||
     services.file_seek(NULL,file,0,ANYGM_SEEK_START)!=0 ||
     services.file_read(NULL,file,copy,sizeof copy)!=sizeof copy ||
     memcmp(copy,content,sizeof copy)){
    fprintf(stderr,"libretro VFS file transport failed\n");
    return 1;
  }
  services.file_close(NULL,file);
  if(invalid_path_seen || invalid_end_seek_seen){
    fprintf(stderr,"libretro VFS boundary normalization failed\n");
    return 1;
  }
  puts("libretro VFS transport: ok");
  return 0;
}
