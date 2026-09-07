/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "content_path.h"

#include <string.h>

/* Shared unchanged member policy for archives, anchors and external resources. */
int anygm_content_member_normalize(char *name,size_t raw_size){
  if(!name || !raw_size || raw_size>ANYGM_CONTENT_MAX_MEMBER_PATH ||
     memchr(name,0,raw_size)) return 0;
  name[raw_size]=0;
  for(size_t i=0;i<raw_size;i++){
    unsigned char c=(unsigned char)name[i];
    if(c=='\\') name[i]='/';
    else if(c<0x20u || c==0x7fu || c==':') return 0;
  }
  if(name[0]=='/') return 0;
  size_t segment=0;
  for(size_t i=0;i<=raw_size;i++){
    if(i<raw_size && name[i]!='/') continue;
    size_t length=i-segment;
    int final_directory=(i==raw_size && i>0 && name[i-1]=='/');
    if((!length && !final_directory) ||
       (length==1 && name[segment]=='.') ||
       (length==2 && name[segment]=='.' && name[segment+1]=='.')) return 0;
    segment=i+1;
  }
  return 1;
}
