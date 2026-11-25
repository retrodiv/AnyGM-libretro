/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_router.h"
#include "memory_vfs.h"

int LLVMFuzzerTestOneInput(const uint8_t *data,size_t size){
  static const char *const names[]={
    "/synthetic.zip","/synthetic.exe","/synthetic.gmk","/synthetic.anygm",
    "/synthetic.win","/synthetic.yyp","/synthetic.apk","/synthetic.port"
  };
  if(!size || size>65536u) return 0;
  const char *path=names[data[0]%(sizeof names/sizeof *names)];
  AnygmMemoryVfs vfs;
  AnygmHostServices services;
  anygm_memory_vfs_init(&vfs,&services);
  if(anygm_memory_vfs_add_file(&vfs,path,data+1,size-1)){
    AnygmContentRouter router={.host=&services,.cache_directory="/cache"};
    char payload[1024],assets[1024];
    if(anygm_content_resolve_path(&router,path,payload,sizeof payload,assets,sizeof assets,
         NULL,0)==ANYGM_CONTENT_RESOLVE_OK){
      GmlWin win;
      if(anygm_content_load_win(&router,&win,payload,assets,NULL,0)) gml_win_free(&win);
    }
  }
  anygm_memory_vfs_destroy(&vfs);
  return 0;
}
