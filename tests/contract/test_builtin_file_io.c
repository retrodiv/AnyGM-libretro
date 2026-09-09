/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "anygm_test_runner.h"
#include "memory_vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  AnygmMemoryVfs memory; /* first member also serves ordinary memory-VFS callbacks */
  AnygmHostServices base, host;
  size_t read_limit, read_total, max_requested;
  int short_eof, fail_seek, fail_flush, fail_truncate, overread, shorter_extent, fail_rewind;
  int opened, closed;
} FileFixture;
static void *fixture_open(void *userdata,const char *path,AnygmFileMode mode){
  FileFixture *f=userdata;
  if(f->fail_truncate && (mode&ANYGM_FILE_TRUNCATE)) return NULL;
  void *file=f->base.file_open(&f->memory,path,mode);
  if(file) f->opened++;
  return file;
}
static void fixture_close(void *userdata,void *file){
  FileFixture *f=userdata;
  f->closed++;
  f->base.file_close(&f->memory,file);
}
static size_t fixture_read(void *userdata,void *file,void *data,size_t size){
  FileFixture *f=userdata;
  if(size>f->max_requested) f->max_requested=size;
  if(f->overread) return size+1u;
  if(f->short_eof && f->read_total>=2) return 0;
  if(f->read_limit && size>f->read_limit) size=f->read_limit;
  size_t count=f->base.file_read(&f->memory,file,data,size);
  f->read_total+=count;
  return count;
}
static int64_t fixture_seek(void *userdata,void *file,int64_t offset,AnygmSeekOrigin origin){
  FileFixture *f=userdata;
  if(f->fail_seek || (f->fail_rewind && origin==ANYGM_SEEK_START)) return -1;
  int64_t result=f->base.file_seek(&f->memory,file,offset,origin);
  return f->shorter_extent && origin==ANYGM_SEEK_END && result>0?result-1:result;
}
static AnygmResult fixture_flush(void *userdata,void *file){
  FileFixture *f=userdata;
  return f->fail_flush?ANYGM_ERROR_IO:f->base.file_flush(&f->memory,file);
}
static void fixture_init(FileFixture *f,GmlVM *vm){
  memset(f,0,sizeof *f); memset(vm,0,sizeof *vm);
  anygm_memory_vfs_init(&f->memory,&f->base);
  f->host=f->base; f->host.userdata=f;
  f->host.file_open=fixture_open; f->host.file_close=fixture_close;
  f->host.file_read=fixture_read; f->host.file_seek=fixture_seek;
  f->host.file_flush=fixture_flush; vm->host=&f->host;
}
static int fixture_destroy(FileFixture *f,GmlVM *vm){
  gml_vm_free(vm);
  int balanced=f->opened==f->closed;
  anygm_memory_vfs_destroy(&f->memory);
  return balanced;
}
static GmlVal invoke(GmlVM *vm,const char *name,GmlVal *a,int n,int cached,int *ok){
  if(!cached) return gml_builtin_call(vm,name,a,n);
  int id=gml_builtin_fast_id(vm,name);
  if(id<0){ *ok=0; return vundef(); }
  return gml_builtin_call_fast_id(vm,id,name,a,n);
}
static int string_is(GmlVal value,const char *expected){
  int ok=value.t==V_STR && value.s && !strcmp(value.s,expected);
  if(!ok) fprintf(stderr,"file digest mismatch: expected %s\n",expected);
  if(value.t==V_STR && value.d!=0) free((void *)value.s);
  else gml_values_release(&value,1);
  return ok;
}
static int real_is(GmlVal value,double expected){ return value.t==V_REAL && value.d==expected; }
static int digest_case(void){
  const char *names[]={"md5_file","sha1_file"};
  const char *hashes[][3]={
    {"d41d8cd98f00b204e9800998ecf8427e","900150983cd24fb0d6963f7d28e17f72","5eb36102d6213f03aeacf0f2ebaf2a40"},
    {"da39a3ee5e6b4b0d3255bfef95601890afd80709","a9993e364706816aba3e25717850c26c9cd0d89d","f91d14501059c2fa96a99aa2f8af6488ad672088"}
  };
  int ok=1;
  for(int cached=0;cached<2;cached++){
    FileFixture f; GmlVM vm; GmlWin win={0};
    fixture_init(&f,&vm);
    snprintf(win.content_dir,sizeof win.content_dir,"/content");
    snprintf(win.save_dir,sizeof win.save_dir,"/save"); vm.win=&win;
    unsigned char *bytes=malloc(131077);
    if(!bytes) return 0;
    for(int i=0;i<131077;i++) bytes[i]=(unsigned char)(i%251);
    ok &= anygm_memory_vfs_add_file(&f.memory,"/content/input.bin","different",9);
    ok &= anygm_memory_vfs_add_file(&f.memory,"/save/input.bin","abc",3);
    ok &= anygm_memory_vfs_add_file(&f.memory,"/content/empty.bin",NULL,0);
    ok &= anygm_memory_vfs_add_file(&f.memory,"/content/binary.bin",bytes,131077);
    free(bytes);
    const char *paths[]={"empty.bin","input.bin","binary.bin"};
    for(int algorithm=0;algorithm<2;algorithm++){
      for(int input=0;input<3;input++){
        GmlVal path=vstr(paths[input]);
        f.read_limit=7; f.read_total=0;
        ok &= string_is(invoke(&vm,names[algorithm],&path,1,cached,&ok),hashes[algorithm][input]);
      }
    }
    ok &= f.max_requested<=65536 && f.opened==f.closed;
    vm.win=NULL;
    ok &= fixture_destroy(&f,&vm);
  }
  return ok;
}
static int digest_failure_case(void){
  int ok=1;
  const char *names[]={"md5_file","sha1_file"};
  for(int fault=0;fault<9;fault++){
    FileFixture f; GmlVM vm; fixture_init(&f,&vm);
    anygm_memory_vfs_add_file(&f.memory,"/input.bin","abc",3);
    f.read_limit=1; f.short_eof=fault==0; f.fail_seek=fault==1;
    if(fault==2) f.host.file_read=NULL;
    if(fault==4) f.host.file_seek=NULL;
    f.overread=fault==5; f.shorter_extent=fault==6; f.fail_rewind=fault==7;
    if(fault==8) f.host.file_open=NULL;
    GmlVal path=vstr(fault==3?"/missing.bin":"/input.bin");
    for(int algorithm=0;algorithm<2;algorithm++){
      f.read_total=0;
      ok &= string_is(invoke(&vm,names[algorithm],&path,1,1,&ok),"");
    }
    ok &= fixture_destroy(&f,&vm);
  }
  return ok;
}
static int rewrite_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    FileFixture f; GmlVM vm; fixture_init(&f,&vm);
    anygm_memory_vfs_add_file(&f.memory,"/input.bin","abcdef",6);
    GmlVal args[]={vstr("/input.bin"),vreal(2)};
    GmlVal file=gml_builtin_call(&vm,"file_bin_open",args,2);
    GmlVal seek[]={file,vreal(3)};
    gml_builtin_call(&vm,"file_bin_seek",seek,2);
    invoke(&vm,"file_bin_rewrite",&file,1,cached,&ok);
    ok &= real_is(gml_builtin_call(&vm,"file_bin_size",&file,1),0) &&
          real_is(gml_builtin_call(&vm,"file_bin_position",&file,1),0);
    GmlVal write[]={file,vreal(255)};
    gml_builtin_call(&vm,"file_bin_write_byte",write,2);
    ok &= real_is(gml_builtin_call(&vm,"file_bin_size",&file,1),1);
    seek[1]=vreal(0); gml_builtin_call(&vm,"file_bin_seek",seek,2);
    ok &= real_is(gml_builtin_call(&vm,"file_bin_read_byte",&file,1),255);
    ok &= fixture_destroy(&f,&vm);
  }
  return ok;
}
static int rewrite_failure_case(void){
  int ok=1;
  for(int fault=0;fault<5;fault++){
    FileFixture f; GmlVM vm; fixture_init(&f,&vm);
    anygm_memory_vfs_add_file(&f.memory,"/input.bin","abc",3);
    GmlVal args[]={vstr("/input.bin"),vreal(fault==0?0:2)};
    GmlVal file=gml_builtin_call(&vm,"file_bin_open",args,2);
    GmlVal seek[]={file,vreal(1)};
    gml_builtin_call(&vm,"file_bin_seek",seek,2);
    f.fail_flush=fault==1; f.fail_truncate=fault==2;
    if(fault==3) f.host.file_flush=NULL;
    if(fault==4) f.host.file_open=NULL;
    invoke(&vm,"file_bin_rewrite",&file,1,1,&ok);
    ok &= real_is(gml_builtin_call(&vm,"file_bin_position",&file,1),1) &&
          real_is(gml_builtin_call(&vm,"file_bin_size",&file,1),3) &&
          real_is(gml_builtin_call(&vm,"file_bin_read_byte",&file,1),'b');
    ok &= fixture_destroy(&f,&vm);
  }
  return ok;
}
int main(void){
  static const AnygmTestCase cases[]={
    {"digest_vectors_overlay_and_short_reads",digest_case},
    {"digest_injected_failures",digest_failure_case},
    {"rewrite_handle_and_cursor",rewrite_case},
    {"rewrite_authority_and_failures",rewrite_failure_case}
  };
  const AnygmTestGroup group={"builtin_file_io",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,NULL,&result);
  printf("builtin file I/O: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
