/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "synthetic_content.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t package_digest(const uint8_t *data,size_t size){
  uint64_t hash=UINT64_C(14695981039346656037);
  for(size_t i=0;i<size;i++){
    hash^=data[i];
    hash*=UINT64_C(1099511628211);
  }
  return hash;
}

static int write_bytes(const char *path,const uint8_t *data,size_t size){
  FILE *file=fopen(path,"wb");
  int ok=file && fwrite(data,1,size,file)==size;
  if(file && fclose(file)!=0) ok=0;
  return ok;
}

int main(int argc,char **argv){
  AnygmSyntheticContent first={0}, second={0};
  uint8_t *first_bytes=NULL, *second_bytes=NULL;
  size_t first_size=0, second_size=0;
  int ok=anygm_synthetic_content_create(&first)
      && anygm_synthetic_content_create(&second)
      && anygm_synthetic_content_read(&first,&first_bytes,&first_size)
      && anygm_synthetic_content_read(&second,&second_bytes,&second_size);
  if(!ok){
    fputs("could not create structural package fixtures\n",stderr);
  }else if(first_size!=second_size ||
           memcmp(first_bytes,second_bytes,first_size)!=0){
    fprintf(stderr,
            "structural package output is not byte-identical: %zu versus %zu bytes\n",
            first_size,second_size);
    ok=0;
  }

  uint64_t digest=ok?package_digest(first_bytes,first_size):0;
  if(ok && (first_size!=2968 ||
            digest!=UINT64_C(0x78208d57266661e0))){
    fprintf(stderr,
            "structural package golden changed: %zu bytes, fnv64=%016" PRIx64 "\n",
            first_size,digest);
    ok=0;
  }
  if(ok && argc==2 && !write_bytes(argv[1],first_bytes,first_size)){
    fprintf(stderr,"could not retain structural package fixture at %s\n",argv[1]);
    ok=0;
  }else if(argc>2){
    fputs("usage: test_package [output-path]\n",stderr);
    ok=0;
  }

  free(first_bytes);
  free(second_bytes);
  anygm_synthetic_content_destroy(&first);
  anygm_synthetic_content_destroy(&second);
  if(ok)
    printf("structural package bytes: ok (%zu bytes, fnv64=%016" PRIx64 ")\n",
           first_size,digest);
  return ok?0:1;
}
