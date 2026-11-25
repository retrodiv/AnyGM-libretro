/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_builtin.h"
#include "gml_builtin_internal.h"
#include "anygm_vfs.h"
#include "gml_image_codec.h"
#include "gml_particle.h"
#include "stdio_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#endif

static int ini_number_matches(GmlVM *vm,const char *section,const char *key,double expected){
  GmlVal args[]={vstr(section),vstr(key),vreal(-17)};
  GmlVal value=gml_builtin_call(vm,"ini_read_real",args,3);
  return value.t==V_REAL && value.d==expected;
}

static void ini_write_number(GmlVM *vm,const char *section,const char *key,double value){
  GmlVal args[]={vstr(section),vstr(key),vreal(value)};
  (void)gml_builtin_call(vm,"ini_write_real",args,3);
}

int expect_buffer_resize_cursor_case(void){
  GmlWin win={0}; GmlVM vm={0}; vm.win=&win;
  vm.particles=gml_particle_state_create(&vm);
  gml_vm_software3d_reset(&vm);
  GmlVal create[]={vreal(1),vreal(1),vreal(1)};
  GmlVal buffer=gml_builtin_call(&vm,"buffer_create",create,3);
  GmlVal write[]={buffer,vreal(11),vstr("first")};
  (void)gml_builtin_call(&vm,"buffer_write",write,3);
  GmlVal resize[]={buffer,vreal(1)};
  (void)gml_builtin_call(&vm,"buffer_resize",resize,2);
  GmlVal cursor=gml_builtin_call(&vm,"buffer_tell",&buffer,1);
  int ok=cursor.t==V_REAL && cursor.d==0;
  write[2]=vstr("second");
  (void)gml_builtin_call(&vm,"buffer_write",write,3);
  GmlVal seek[]={buffer,vreal(0),vreal(0)};
  (void)gml_builtin_call(&vm,"buffer_seek",seek,3);
  GmlVal read[]={buffer,vreal(11)};
  GmlVal text=gml_builtin_call(&vm,"buffer_read",read,2);
  ok=ok && text.t==V_STR && text.s && !strcmp(text.s,"second");
  if(!ok) fprintf(stderr,"resized buffer cursor=%.0f, reused text=%s\n",cursor.d,text.t==V_STR?text.s:"<non-string>");
  gml_values_release_owned(&text,1);
  resize[1]=vreal(32);
  (void)gml_builtin_call(&vm,"buffer_resize",resize,2);
  cursor=gml_builtin_call(&vm,"buffer_tell",&buffer,1);
  ok=ok && cursor.t==V_REAL && cursor.d==7;
  resize[1]=vreal(4);
  (void)gml_builtin_call(&vm,"buffer_resize",resize,2);
  cursor=gml_builtin_call(&vm,"buffer_tell",&buffer,1);
  ok=ok && cursor.t==V_REAL && cursor.d==0;
  (void)gml_builtin_call(&vm,"buffer_delete",&buffer,1);
  gml_vm_free(&vm);
  return ok;
}

int expect_ini_close_text_case(void){
  GmlWin win={0}; GmlVM vm={0}; vm.win=&win;
  vm.particles=gml_particle_state_create(&vm);
  gml_vm_software3d_reset(&vm);
  GmlVal source=vstr("");
  (void)gml_builtin_call(&vm,"ini_open_from_string",&source,1);
  ini_write_number(&vm,"progress","level",7);
  GmlVal args[]={vstr("progress"),vstr("name"),vstr("authored text")};
  (void)gml_builtin_call(&vm,"ini_write_string",args,3);
  GmlVal text=gml_builtin_call(&vm,"ini_close",NULL,0);
  int ok=text.t==V_STR && text.s && strstr(text.s,"[progress]") &&
    strstr(text.s,"level=7") && strstr(text.s,"name=authored text");
  if(ok){
    (void)gml_builtin_call(&vm,"ini_open_from_string",&text,1);
    ok=ini_number_matches(&vm,"progress","level",7);
    GmlVal copied=gml_builtin_call(&vm,"ini_read_string",args,3);
    ok=ok && copied.t==V_STR && copied.s && !strcmp(copied.s,"authored text");
    if(copied.t==V_STR && copied.d!=0) free((void*)copied.s);
  }
  if(text.t==V_STR && text.d!=0) free((void*)text.s);
  GmlVal closed=gml_builtin_call(&vm,"ini_close",NULL,0);
  gml_values_release_owned(&closed,1);
  source=vstr("");
  (void)gml_builtin_call(&vm,"ini_open_from_string",&source,1);
  text=gml_builtin_call(&vm,"ini_close",NULL,0);
  ok=ok && text.t==V_STR && text.s && !text.s[0];
  gml_values_release_owned(&text,1);
  (void)gml_builtin_call(&vm,"ini_open_from_string",&source,1);
  char long_text[2049];
  memset(long_text,'q',sizeof long_text-1); long_text[sizeof long_text-1]=0;
  args[2]=vstr(long_text);
  (void)gml_builtin_call(&vm,"ini_write_string",args,3);
  text=gml_builtin_call(&vm,"ini_close",NULL,0);
  ok=ok && text.t==V_STR && text.s && strstr(text.s,long_text);
  if(text.t==V_STR){
    (void)gml_builtin_call(&vm,"ini_open_from_string",&text,1);
    GmlVal copied=gml_builtin_call(&vm,"ini_read_string",args,3);
    ok=ok && copied.t==V_STR && copied.s && !strcmp(copied.s,long_text);
    gml_values_release_owned(&copied,1);
  }
  gml_values_release_owned(&text,1);
  gml_vm_free(&vm);
  if(!ok) fprintf(stderr,"INI close lost the authored in-memory document\n");
  return ok;
}

int expect_ini_key_lookup_case(void){
  static const char first_collision[]="section-619323c6";
  static const char second_collision[]="section-110c83b1";
  static const char text[]="[duplicate]\nvalue=1\nvalue=9\n"
    "[section-619323c6]\nvalue=21\n[section-110c83b1]\nvalue=42\n"
    "[Case]\nkey=7\n[case]\nkey=8\n";
  GmlWin win={0}; GmlVM vm={0}; vm.win=&win;
  vm.particles=gml_particle_state_create(&vm);
  gml_vm_software3d_reset(&vm);
  GmlVal source=vstr(text);
  (void)gml_builtin_call(&vm,"ini_open_from_string",&source,1);
  if(!vm.particles || !vm.builtins){ gml_vm_free(&vm); return 0; }
  /* These authored names collide in the tuple filter. Exact names must still
   * select the correct entry, including the forward write and reverse read. */
  int ok=vm.builtins->ini_n==6 &&
    builtin_state_ini_key_hash(first_collision,"value")==
      builtin_state_ini_key_hash(second_collision,"value") &&
    vm.builtins->ini_kv[2].key_hash==vm.builtins->ini_kv[3].key_hash &&
    ini_number_matches(&vm,first_collision,"value",21) &&
    ini_number_matches(&vm,second_collision,"value",42) &&
    ini_number_matches(&vm,"Case","key",7) && ini_number_matches(&vm,"case","key",8);
  ini_write_number(&vm,second_collision,"value",84);
  ini_write_number(&vm,"duplicate","value",4);
  ok=ok && ini_number_matches(&vm,first_collision,"value",21) &&
    ini_number_matches(&vm,second_collision,"value",84) &&
    vm.builtins->ini_kv[0].val==4 && ini_number_matches(&vm,"duplicate","value",9);
  GmlVal strings[]={vstr(first_collision),vstr("value"),vstr("written")};
  (void)gml_builtin_call(&vm,"ini_write_string",strings,3);
  GmlVal copied=gml_builtin_call(&vm,"ini_read_string",strings,3);
  ok=ok && copied.t==V_STR && copied.s && !strcmp(copied.s,"written") &&
    ini_number_matches(&vm,second_collision,"value",84);
  if(copied.t==V_STR && copied.d!=0) free((void*)copied.s);
  GmlVal duplicate[]={vstr("duplicate"),vstr("value")};
  (void)gml_builtin_call(&vm,"ini_key_delete",duplicate,2);
  ok=ok && vm.builtins->ini_n==5 && ini_number_matches(&vm,"duplicate","value",9);
  (void)gml_builtin_call(&vm,"ini_section_delete",duplicate,1);
  GmlVal exists=gml_builtin_call(&vm,"ini_key_exists",duplicate,2);
  ok=ok && vm.builtins->ini_n==4 && exists.t==V_REAL && exists.d==0 &&
    ini_number_matches(&vm,"duplicate","value",-17);
  /* Different tuple boundaries and a sizeable table exercise append, update
   * and missing-key lookup without a platform-dependent timing expectation. */
  ini_write_number(&vm,"a","bc",11);
  ini_write_number(&vm,"ab","c",22);
  for(unsigned i=0;i<3000u;i++){
    char section[32],key[32];
    snprintf(section,sizeof section,"chunk%u",i/7u);
    snprintf(key,sizeof key,"item%u",i);
    ini_write_number(&vm,section,key,(double)i+1.0);
  }
  ini_write_number(&vm,"chunk0","item0",41);
  ok=ok && vm.builtins->ini_n==3006 && ini_number_matches(&vm,"a","bc",11) &&
    ini_number_matches(&vm,"ab","c",22) && ini_number_matches(&vm,"chunk0","item0",41) &&
    ini_number_matches(&vm,"chunk428","item2999",3000);
  size_t size=gml_vm_state_size(&vm),written=0,used=0;
  void *before=malloc(size?size:1),*after=malloc(size?size:1);
  ok=ok && size && before && after &&
    gml_vm_state_save(&vm,before,size,&written) && written==size;
  if(ok){
    /* Derived lookup data must never become state bytes. A restore rebuilds it
     * from the ordered values before any read, update or delete can consume it. */
    for(int i=0;i<vm.builtins->ini_n;i++) vm.builtins->ini_kv[i].key_hash^=UINT32_C(0x01010101);
    ok=gml_vm_state_save(&vm,after,size,&written) && written==size &&
       !memcmp(before,after,size) && gml_vm_state_load(&vm,before,size,&used) && used==size;
  }
  if(ok){
    for(int i=0;i<vm.builtins->ini_n;i++){
      const GmlBuiltinIniEntry *entry=&vm.builtins->ini_kv[i];
      ok=ok && entry->key_hash==builtin_state_ini_key_hash(entry->section,entry->key);
    }
    ok=ok && ini_number_matches(&vm,"chunk428","item2999",3000) &&
      ini_number_matches(&vm,second_collision,"value",84) &&
      gml_vm_state_size(&vm)==size && gml_vm_state_save(&vm,after,size,&written) &&
      written==size && !memcmp(before,after,size);
    ini_write_number(&vm,second_collision,"value",168);
    ok=ok && ini_number_matches(&vm,second_collision,"value",168);
    (void)gml_builtin_call(&vm,"ini_key_delete",strings,2);
    ok=ok && ini_number_matches(&vm,second_collision,"value",168);
  }
  if(!ok) fprintf(stderr,"INI tuple lookup, duplicate order or derived-state rebuild changed\n");
  free(before); free(after); gml_vm_free(&vm);
  return ok;
}


int expect_file_sandbox(GmlVM *vm,const char *save_dir){
  char overlay[80],written[80],ini[80],large_ini[80],winsep_dir[80],winsep_name[160];
  char folded_dir[80],folded_name[160];
  snprintf(overlay,sizeof overlay,"gml-overlay-%ld.txt",(long)getpid());
  snprintf(written,sizeof written,"gml-written-%ld.txt",(long)getpid());
  snprintf(ini,sizeof ini,"gml-default-%ld.ini",(long)getpid());
  snprintf(large_ini,sizeof large_ini,"gml-large-%ld.ini",(long)getpid());
  snprintf(winsep_dir,sizeof winsep_dir,"gml-winsep-%ld",(long)getpid());
  snprintf(winsep_name,sizeof winsep_name,"%s\\payload.txt",winsep_dir);
  snprintf(folded_dir,sizeof folded_dir,"gml-Folded-%ld",(long)getpid());
  snprintf(folded_name,sizeof folded_name,"gml-folded-%ld/visual.png",(long)getpid());
  char content_overlay[640],save_overlay[640],content_written[640],save_written[640];
  char content_ini[640],save_ini[640],content_large_ini[640],save_large_ini[640];
  char content_winsep_dir[640],content_winsep_file[768],content_literal_backslash[768];
  char absolute_winsep_name[768],save_winsep_dir[640],save_winsep_file[768];
  char save_literal_backslash[640];
  char content_folded_dir[640],content_folded_file[768];
  char observed[128]={0};
  snprintf(content_overlay,sizeof content_overlay,"%s/%s",vm->win->content_dir,overlay);
  snprintf(save_overlay,sizeof save_overlay,"%s/%s",save_dir,overlay);
  snprintf(content_written,sizeof content_written,"%s/%s",vm->win->content_dir,written);
  snprintf(save_written,sizeof save_written,"%s/%s",save_dir,written);
  snprintf(content_ini,sizeof content_ini,"%s/%s",vm->win->content_dir,ini);
  snprintf(save_ini,sizeof save_ini,"%s/%s",save_dir,ini);
  snprintf(content_large_ini,sizeof content_large_ini,"%s/%s",vm->win->content_dir,large_ini);
  snprintf(save_large_ini,sizeof save_large_ini,"%s/%s",save_dir,large_ini);
  snprintf(content_winsep_dir,sizeof content_winsep_dir,"%s/%s",vm->win->content_dir,winsep_dir);
  snprintf(content_winsep_file,sizeof content_winsep_file,"%s/payload.txt",content_winsep_dir);
  snprintf(content_literal_backslash,sizeof content_literal_backslash,"%s/%s",
           vm->win->content_dir,winsep_name);
  snprintf(absolute_winsep_name,sizeof absolute_winsep_name,"%s/%s",
           vm->win->content_dir,winsep_name);
  snprintf(save_winsep_dir,sizeof save_winsep_dir,"%s/%s",save_dir,winsep_dir);
  snprintf(save_winsep_file,sizeof save_winsep_file,"%s/payload.txt",save_winsep_dir);
  snprintf(save_literal_backslash,sizeof save_literal_backslash,"%s/%s",save_dir,winsep_name);
  snprintf(content_folded_dir,sizeof content_folded_dir,"%s/%s",
           vm->win->content_dir,folded_dir);
  snprintf(content_folded_file,sizeof content_folded_file,"%s/Visual.PNG",
           content_folded_dir);
  unlink(content_overlay); unlink(save_overlay); unlink(content_written); unlink(save_written);
  unlink(content_ini); unlink(save_ini); unlink(content_large_ini); unlink(save_large_ini);
  unlink(content_winsep_file); unlink(content_literal_backslash); rmdir(content_winsep_dir);
  unlink(save_winsep_file); unlink(save_literal_backslash); rmdir(save_winsep_dir);
  unlink(content_folded_file); rmdir(content_folded_dir);
  if(!fixture_write_text(content_overlay,"program") || !fixture_write_text(save_overlay,"save") ||
     !fixture_write_text(content_ini,"[fixture]\nvalue=7\nlocalized=0,65\n") ||
     !fixture_write_large_ini(content_large_ini)) return 0;
  if(mkdir(content_winsep_dir,0700)!=0 ||
     !fixture_write_text(content_winsep_file,"installed-portable")) return 0;
  if(mkdir(content_folded_dir,0700)!=0 ||
     !fixture_write_text(content_folded_file,"case-folded-read")) return 0;

  GmlVal name=vstr(overlay);
  GmlVal handle=gml_builtin_call(vm,"file_text_open_read",&name,1);
  GmlVal line=gml_builtin_call(vm,"file_text_readln",&handle,1);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  int ok=handle.t==V_REAL && handle.d>0 && line.t==V_STR && line.s && !strcmp(line.s,"save");
  if(line.t==V_STR && line.d!=0) free((void*)line.s);
  unlink(save_overlay);
  handle=gml_builtin_call(vm,"file_text_open_read",&name,1);
  line=gml_builtin_call(vm,"file_text_readln",&handle,1);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  ok=ok && line.t==V_STR && line.s && !strcmp(line.s,"program");
  if(line.t==V_STR && line.d!=0) free((void*)line.s);

  /* Installed assets retain Windows path semantics on case-sensitive hosts, including every
   * directory component rather than only the final filename. */
  name=vstr(folded_name);
  handle=gml_builtin_call(vm,"file_text_open_read",&name,1);
  line=gml_builtin_call(vm,"file_text_readln",&handle,1);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  ok=ok && line.t==V_STR && line.s && !strcmp(line.s,"case-folded-read");
  if(line.t==V_STR && line.d!=0) free((void*)line.s);

  /* working_directory concatenation produces an absolute bundle path. It must retain the same
   * save-over-bundle read overlay rather than bypassing the sandbox. */
  if(!fixture_write_text(save_overlay,"absolute-save")) ok=0;
  name=vstr(content_overlay);
  handle=gml_builtin_call(vm,"file_text_open_read",&name,1);
  line=gml_builtin_call(vm,"file_text_readln",&handle,1);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  ok=ok && line.t==V_STR && line.s && !strcmp(line.s,"absolute-save");
  if(line.t==V_STR && line.d!=0) free((void*)line.s);
  unlink(save_overlay);

  /* Studio content also concatenates Windows-separated asset paths onto an absolute
   * working_directory. The suffix must still resolve inside the installed bundle. */
  name=vstr(absolute_winsep_name);
  handle=gml_builtin_call(vm,"file_text_open_read",&name,1);
  line=gml_builtin_call(vm,"file_text_readln",&handle,1);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  ok=ok && line.t==V_STR && line.s && !strcmp(line.s,"installed-portable") &&
     !fixture_read_text(content_literal_backslash,observed,sizeof observed);
  if(line.t==V_STR && line.d!=0) free((void*)line.s);

  name=vstr(written);
  handle=gml_builtin_call(vm,"file_text_open_write",&name,1);
  GmlVal write_args[2]={handle,vstr("sandbox")};
  (void)gml_builtin_call(vm,"file_text_write_string",write_args,2);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  observed[0]=0;
  ok=ok && !fixture_read_text(content_written,observed,sizeof observed) &&
     fixture_read_text(save_written,observed,sizeof observed) && !strcmp(observed,"sandbox");
  unlink(save_written);
  name=vstr(content_written);
  handle=gml_builtin_call(vm,"file_text_open_write",&name,1);
  GmlVal absolute_write_args[2]={handle,vstr("absolute-sandbox")};
  (void)gml_builtin_call(vm,"file_text_write_string",absolute_write_args,2);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  observed[0]=0;
  ok=ok && !fixture_read_text(content_written,observed,sizeof observed) &&
     fixture_read_text(save_written,observed,sizeof observed) && !strcmp(observed,"absolute-sandbox");

  /* Classic projects use Windows separators for nested save paths.  On Unix those separators
   * must address directories, never become literal characters in a filename. */
  if(mkdir(save_winsep_dir,0700)!=0) ok=0;
  name=vstr(winsep_name);
  handle=gml_builtin_call(vm,"file_text_open_write",&name,1);
  GmlVal winsep_write_args[2]={handle,vstr("portable")};
  (void)gml_builtin_call(vm,"file_text_write_string",winsep_write_args,2);
  (void)gml_builtin_call(vm,"file_text_close",&handle,1);
  observed[0]=0;
  ok=ok && fixture_read_text(save_winsep_file,observed,sizeof observed) &&
     !strcmp(observed,"portable") && !fixture_read_text(save_literal_backslash,observed,sizeof observed);
  GmlVal exists=gml_builtin_call(vm,"file_exists",&name,1);
  ok=ok && exists.t==V_REAL && exists.d==1;

  name=vstr(ini);
  (void)gml_builtin_call(vm,"ini_open",&name,1);
  GmlVal read_args[3]={vstr("fixture"),vstr("value"),vreal(-1)};
  GmlVal initial=gml_builtin_call(vm,"ini_read_real",read_args,3);
  GmlVal localized_args[3]={vstr("fixture"),vstr("localized"),vreal(-1)};
  GmlVal localized=gml_builtin_call(vm,"ini_read_real",localized_args,3);
  GmlVal missing_args[3]={vstr("fixture"),vstr("missing"),vstr("2.5")};
  GmlVal string_default=gml_builtin_call(vm,"ini_read_real",missing_args,3);
  GmlVal ini_write_args[3]={vstr("fixture"),vstr("value"),vreal(11)};
  (void)gml_builtin_call(vm,"ini_write_real",ini_write_args,3);
  GmlVal serialized=gml_builtin_call(vm,"ini_close",NULL,0);
  char installed[128]={0},saved[128]={0};
  ok=ok && initial.t==V_REAL && initial.d==7 &&
     localized.t==V_REAL && localized.d==0.65 &&
     string_default.t==V_REAL && string_default.d==2.5 &&
     fixture_read_text(content_ini,installed,sizeof installed) && strstr(installed,"value=7") &&
     fixture_read_text(save_ini,saved,sizeof saved) && strstr(saved,"value=11") &&
     serialized.t==V_STR && serialized.s && !strcmp(saved,serialized.s);
  gml_values_release_owned(&serialized,1);

  /* Localization INIs routinely exceed the old 256-entry settings limit and quote
   * string values.  A late value must be unquoted and survive the save overlay. */
  name=vstr(large_ini);
  (void)gml_builtin_call(vm,"ini_open",&name,1);
  GmlVal late_args[3]={vstr("localization"),vstr("key_599"),vstr("missing")};
  GmlVal late=gml_builtin_call(vm,"ini_read_string",late_args,3);
  ok=ok && late.t==V_STR && late.s && !strcmp(late.s,"value_599") &&
     gml_builtin_ini_entry_count(vm)==600;
  if(late.t==V_STR && late.d!=0) free((void*)late.s);
  serialized=gml_builtin_call(vm,"ini_close",NULL,0);
  gml_values_release_owned(&serialized,1);
  (void)gml_builtin_call(vm,"ini_open",&name,1);
  late=gml_builtin_call(vm,"ini_read_string",late_args,3);
  ok=ok && late.t==V_STR && late.s && !strcmp(late.s,"value_599");
  if(late.t==V_STR && late.d!=0) free((void*)late.s);
  serialized=gml_builtin_call(vm,"ini_close",NULL,0);
  gml_values_release_owned(&serialized,1);

  unlink(content_overlay); unlink(save_overlay); unlink(content_written); unlink(save_written);
  unlink(content_ini); unlink(save_ini); unlink(content_large_ini); unlink(save_large_ini);
  unlink(content_winsep_file); unlink(content_literal_backslash); rmdir(content_winsep_dir);
  unlink(save_winsep_file); unlink(save_literal_backslash); rmdir(save_winsep_dir);
  unlink(content_folded_file); rmdir(content_folded_dir);
  if(!ok) fprintf(stderr,"read overlay / writable sandbox fixture failed\n");
  return ok;
}


int expect_file_sandbox_case(void){
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  char content_root[]="/tmp/gml-content-root-XXXXXX";
  char save_root[]="/tmp/gml-save-case-XXXXXX";
  if(!mkdtemp(content_root)) return 0;
  if(!mkdtemp(save_root)){
    rmdir(content_root);
    return 0;
  }
  GmlWin win={0};
  snprintf(win.content_dir,sizeof win.content_dir,"%s",content_root);
  snprintf(win.save_dir,sizeof win.save_dir,"%s",save_root);
  GmlVM vm={0};
  vm.win=&win;
  vm.host=&services;
  int ok=expect_file_sandbox(&vm,save_root);
  gml_vm_free(&vm);
  rmdir(save_root);
  rmdir(content_root);
  return ok;
}

typedef struct FileLookupCounters {
  AnygmHostServices base;
  unsigned stats;
  unsigned directory_opens;
  const char *unavailable_directory;
  void *unavailable_handle;
  int fail_directory_open;
  int omit_hidden_entries;
} FileLookupCounters;

static AnygmResult counted_file_stat(void *userdata,const char *path,AnygmFileInfo *info){
  FileLookupCounters *counts=userdata;
  counts->stats++;
  return counts->base.file_stat(counts->base.userdata,path,info);
}

static void *counted_directory_open(void *userdata,const char *path){
  FileLookupCounters *counts=userdata;
  counts->directory_opens++;
  size_t length=strlen(path);
  while(length && path[length-1]=='/') length--;
  int unavailable=counts->unavailable_directory &&
    strlen(counts->unavailable_directory)==length &&
    !strncmp(path,counts->unavailable_directory,length);
  if(unavailable && counts->fail_directory_open) return NULL;
  void *handle=counts->base.directory_open(counts->base.userdata,path);
  if(unavailable) counts->unavailable_handle=handle;
  return handle;
}

static AnygmResult counted_directory_read(void *userdata,void *handle,AnygmDirectoryEntry *entry){
  FileLookupCounters *counts=userdata;
  if(handle==counts->unavailable_handle) return ANYGM_ERROR_IO;
  AnygmResult result;
  do {
    result=counts->base.directory_read(counts->base.userdata,handle,entry);
  } while(result==ANYGM_OK && counts->omit_hidden_entries && entry->name[0]=='.');
  return result;
}

static void counted_directory_close(void *userdata,void *handle){
  FileLookupCounters *counts=userdata;
  if(handle==counts->unavailable_handle) counts->unavailable_handle=NULL;
  counts->base.directory_close(counts->base.userdata,handle);
}

int expect_file_lookup_probes_case(void){
  FileLookupCounters counts={0};
  counts.base.struct_size=sizeof counts.base;
  counts.base.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&counts.base);
  AnygmHostServices services=counts.base;
  services.userdata=&counts;
  services.file_stat=counted_file_stat;
  services.directory_open=counted_directory_open;
  char content_root[]="/tmp/gml-lookup-content-XXXXXX";
  char save_root[]="/tmp/gml-lookup-save-XXXXXX";
  if(!mkdtemp(content_root)) return 0;
  if(!mkdtemp(save_root)){ rmdir(content_root); return 0; }
  char save_file[640],content_file[640],later_file[640];
  snprintf(save_file,sizeof save_file,"%s/probe.txt",save_root);
  snprintf(content_file,sizeof content_file,"%s/MiXeD.TXT",content_root);
  snprintf(later_file,sizeof later_file,"%s/later.txt",save_root);
  GmlWin win={0};
  snprintf(win.content_dir,sizeof win.content_dir,"%s",content_root);
  snprintf(win.save_dir,sizeof win.save_dir,"%s",save_root);
  GmlVM vm={0};
  vm.win=&win;vm.host=&services;
  int ok=fixture_write_text(save_file,"save") && fixture_write_text(content_file,"content");
  GmlVal name=vstr("probe.txt");
  GmlVal exists=gml_builtin_call(&vm,"file_exists",&name,1);
  int exact=exists.t==V_REAL && exists.d==1 && counts.stats==1 && !counts.directory_opens;
  if(!exact) fprintf(stderr,"exact lookup probes: stats=%u directory_opens=%u\n",
                    counts.stats,counts.directory_opens);
  ok=ok && exact;
  counts.stats=counts.directory_opens=0;
  name=vstr("later.txt");
  exists=gml_builtin_call(&vm,"file_exists",&name,1);
  int missing=exists.t==V_REAL && exists.d==0 && counts.stats<=2 && counts.directory_opens<=2;
  if(!missing) fprintf(stderr,"missing lookup probes: stats=%u directory_opens=%u\n",
                      counts.stats,counts.directory_opens);
  ok=ok && missing;
  /* A later operation must observe a file created after a failed lookup. */
  ok=fixture_write_text(later_file,"new") && ok;
  counts.stats=counts.directory_opens=0;
  exists=gml_builtin_call(&vm,"file_exists",&name,1);
  ok=ok && exists.t==V_REAL && exists.d==1 && counts.stats==1 && !counts.directory_opens;
  counts.stats=counts.directory_opens=0;
  name=vstr("mixed.txt");
  exists=gml_builtin_call(&vm,"file_exists",&name,1);
  ok=ok && exists.t==V_REAL && exists.d==1;
  name=vstr(save_root);
  GmlVal directory=gml_builtin_call(&vm,"directory_exists",&name,1);
  ok=ok && directory.t==V_REAL && directory.d==1;
  name=vstr(save_file);
  directory=gml_builtin_call(&vm,"directory_exists",&name,1);
  ok=ok && directory.t==V_REAL && directory.d==0;
  gml_vm_free(&vm);
  unlink(save_file);unlink(content_file);unlink(later_file);
  rmdir(save_root);rmdir(content_root);
  return ok;
}

int expect_missing_path_components_case(void){
  FileLookupCounters counts={0};
  counts.base.struct_size=sizeof counts.base;
  counts.base.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&counts.base);
  AnygmHostServices services=counts.base;
  services.userdata=&counts;
  services.file_stat=counted_file_stat;
  services.directory_open=counted_directory_open;
  services.directory_read=counted_directory_read;
  services.directory_close=counted_directory_close;
  char content_root[]="/tmp/gml-components-content-XXXXXX";
  char save_root[]="/tmp/gml-components-save-XXXXXX";
  if(!mkdtemp(content_root)) return 0;
  if(!mkdtemp(save_root)){ rmdir(content_root); return 0; }
  char mixed_dir[640],child[768],flat[640],fallback[768],shadow_flat[640];
  char hidden_dir[640],hidden_child[768];
  snprintf(mixed_dir,sizeof mixed_dir,"%s/MiXeD",content_root);
  snprintf(child,sizeof child,"%s/Child.TXT",mixed_dir);
  snprintf(flat,sizeof flat,"%s/missing@deeper@file.txt",content_root);
  snprintf(fallback,sizeof fallback,"%s/MiXeD/absent/deeper.txt",content_root);
  snprintf(shadow_flat,sizeof shadow_flat,"%s/mixed@child.txt",save_root);
  snprintf(hidden_dir,sizeof hidden_dir,"%s/.HiDdEn",content_root);
  snprintf(hidden_child,sizeof hidden_child,"%s/Child.TXT",hidden_dir);
  GmlWin win={0};
  snprintf(win.content_dir,sizeof win.content_dir,"%s",content_root);
  snprintf(win.save_dir,sizeof win.save_dir,"%s",save_root);
  GmlVM vm={0};vm.win=&win;vm.host=&services;
  int ok=mkdir(mixed_dir,0700)==0 && fixture_write_text(child,"child") &&
    mkdir(hidden_dir,0700)==0 && fixture_write_text(hidden_child,"hidden-child");
  GmlVal name=vstr("missing/deeper/file.txt");
  GmlVal exists=gml_builtin_call(&vm,"file_exists",&name,1);
  int missing=exists.t==V_REAL && exists.d==0 && counts.directory_opens<=4;
  if(!missing) fprintf(stderr,"missing components: directory_opens=%u\n",counts.directory_opens);
  ok=ok && missing;
  counts.directory_opens=0;
  name=vstr("mixed/absent/deeper.txt");
  exists=gml_builtin_call(&vm,"file_exists",&name,1);
  missing=exists.t==V_REAL && exists.d==0 && counts.directory_opens<=5;
  if(!missing) fprintf(stderr,"folded missing components: directory_opens=%u\n",counts.directory_opens);
  ok=ok && missing;
  /* Keep the corrected prefix and complete unresolved suffix for callers that open the path. */
  char *resolved=resolve_read_path(&vm,name.s);
  ok=ok && resolved && !strcmp(resolved,fallback);
  free(resolved);
  /* The normal roots still precede the flattened included-file fallback, and a new
   * operation must see an exported file created after the miss. */
  ok=fixture_write_text(flat,"flat") && ok;
  name=vstr("missing/deeper/file.txt");
  exists=gml_builtin_call(&vm,"file_exists",&name,1);
  ok=ok && exists.t==V_REAL && exists.d==1;
  name=vstr("missing\\deeper\\file.txt");
  exists=gml_builtin_call(&vm,"file_exists",&name,1);
  ok=ok && exists.t==V_REAL && exists.d==1;
  ok=fixture_write_text(shadow_flat,"shadow") && ok;
  resolved=resolve_read_path(&vm,"mixed/child.txt");
  ok=ok && resolved && !strcmp(resolved,child);
  free(resolved);
  /* Some directory services omit special entries. Neither spelling proves absence. */
  counts.omit_hidden_entries=1;
  name=vstr("./mixed/child.txt");
  exists=gml_builtin_call(&vm,"file_exists",&name,1);
  ok=ok && exists.t==V_REAL && exists.d==1;
  name=vstr("MiXeD/../mixed/child.txt");
  exists=gml_builtin_call(&vm,"file_exists",&name,1);
  ok=ok && exists.t==V_REAL && exists.d==1;
  /* Filtered enumeration can hide an exact directory that is still traversable. */
  name=vstr(".HiDdEn/child.txt");
  exists=gml_builtin_call(&vm,"file_exists",&name,1);
  int hidden=exists.t==V_REAL && exists.d==1;
  if(!hidden) fprintf(stderr,"filtered hidden directory lost its case-folded child\n");
  ok=hidden && ok;
  /* A failed enumeration is unknown. The exact first component can still be traversed,
   * allowing the next directory to repair the final filename's case. */
  counts.omit_hidden_entries=0;
  counts.unavailable_directory=content_root;
  name=vstr("MiXeD/child.txt");
  exists=gml_builtin_call(&vm,"file_exists",&name,1);
  ok=ok && exists.t==V_REAL && exists.d==1;
  counts.fail_directory_open=1;
  exists=gml_builtin_call(&vm,"file_exists",&name,1);
  ok=ok && exists.t==V_REAL && exists.d==1;
  gml_vm_free(&vm);
  unlink(child);unlink(flat);unlink(shadow_flat);unlink(hidden_child);
  rmdir(mixed_dir);rmdir(hidden_dir);rmdir(save_root);rmdir(content_root);
  return ok;
}

#if defined(__linux__)
int expect_stdio_directory_errors_case(void){
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  char root[]="/tmp/gml-directory-error-XXXXXX";
  if(!mkdtemp(root)) return 0;
  void *handle=services.directory_open(services.userdata,root);
  if(!handle){ rmdir(root); return 0; }
  /* Invalidate only the newly opened synthetic directory's descriptor, without
   * depending on the provider's private handle layout. No stream read preceded this. */
  DIR *descriptors=opendir("/proc/self/fd");
  int descriptor=-1,matches=0;
  struct dirent *item;
  while(descriptors && (item=readdir(descriptors))){
    char *end=NULL;
    long number=strtol(item->d_name,&end,10);
    if(!end || *end || number<0 || number>INT_MAX) continue;
    char link[128],target[640];
    snprintf(link,sizeof link,"/proc/self/fd/%ld",number);
    ssize_t size=readlink(link,target,sizeof target-1);
    if(size<0) continue;
    target[size]=0;
    if(!strcmp(target,root)){ descriptor=(int)number;matches++; }
  }
  if(descriptors) closedir(descriptors);
  int ok=matches==1 && descriptor>=0 && close(descriptor)==0;
  if(ok){
    AnygmDirectoryEntry entry={.struct_size=sizeof entry};
    AnygmResult result=services.directory_read(services.userdata,handle,&entry);
    if(result!=ANYGM_ERROR_IO){
      fprintf(stderr,"stdio directory read error: got=%d expected=%d\n",result,ANYGM_ERROR_IO);
      ok=0;
    }
  }
  services.directory_close(services.userdata,handle);
  handle=services.directory_open(services.userdata,root);
  if(handle){
    AnygmDirectoryEntry entry={.struct_size=sizeof entry};
    AnygmResult result;
    do {
      errno=EIO; /* A stale caller errno must not turn actual EOF into an error. */
      result=services.directory_read(services.userdata,handle,&entry);
    } while(result==ANYGM_OK);
    ok=(result==ANYGM_RESULT_END) && ok;
    services.directory_close(services.userdata,handle);
  } else ok=0;
  rmdir(root);
  return ok;
}
#endif

int expect_portable_extension_io(void){
  AnygmHostServices services={0};
  services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&services);
  char content_root[]="/tmp/gml-extension-content-XXXXXX";
  char save_root[]="/tmp/gml-extension-save-XXXXXX";
  if(!mkdtemp(content_root)) return 0;
  if(!mkdtemp(save_root)){ rmdir(content_root); return 0; }
  char outside[]="/tmp/gml-extension-outside-XXXXXX";
  int outside_fd=mkstemp(outside);
  if(outside_fd<0){ rmdir(save_root); rmdir(content_root); return 0; }
  close(outside_fd);
  if(!fixture_write_text(outside,"outside")){
    unlink(outside); rmdir(save_root); rmdir(content_root); return 0;
  }
  GmlWin win={0};
  snprintf(win.content_dir,sizeof win.content_dir,"%s",content_root);
  snprintf(win.save_dir,sizeof win.save_dir,"%s",save_root);
  GmlVM vm={0};
  vm.win=&win;
  vm.host=&services;

  GmlVal save_args[]={vstr("note.txt"),vstr("portable-data")};
  GmlVal saved=gml_builtin_call(&vm,"string_save_ns",save_args,2);
  GmlVal filename=vstr("note.txt");
  GmlVal loaded=gml_builtin_call(&vm,"string_load_ns",&filename,1);
  GmlVal copy_args[]={filename,vstr("copy.txt")};
  GmlVal copied=gml_builtin_call(&vm,"file_copy_ns",copy_args,2);
  char copied_path[512],observed[64]={0};
  snprintf(copied_path,sizeof copied_path,"%s/copy.txt",save_root);
  int ok=saved.t==V_REAL && saved.d==1 && loaded.t==V_STR && loaded.s &&
    !strcmp(loaded.s,"portable-data") && copied.t==V_REAL && copied.d==1 &&
    fixture_read_text(copied_path,observed,sizeof observed) &&
    !strcmp(observed,"portable-data");
  if(loaded.t==V_STR && loaded.d!=0) free((void*)loaded.s);

  GmlVal outside_name=vstr(outside);
  GmlVal outside_loaded=gml_builtin_call(&vm,"string_load_ns",&outside_name,1);
  GmlVal outside_save_args[]={outside_name,vstr("changed")};
  GmlVal outside_saved=gml_builtin_call(&vm,"string_save_ns",outside_save_args,2);
  GmlVal traversal_args[]={vstr("../escape.txt"),vstr("changed")};
  GmlVal traversal_saved=gml_builtin_call(&vm,"string_save_ns",traversal_args,2);
  memset(observed,0,sizeof observed);
  ok=ok && outside_loaded.t==V_STR && outside_loaded.s && !outside_loaded.s[0] &&
    outside_saved.t==V_REAL && outside_saved.d==0 &&
    traversal_saved.t==V_REAL && traversal_saved.d==0 &&
    fixture_read_text(outside,observed,sizeof observed) && !strcmp(observed,"outside");
  if(outside_loaded.t==V_STR && outside_loaded.d!=0) free((void*)outside_loaded.s);

  GmlVal joined=vstr("nested/../note.txt");
  GmlVal regular_match=gml_builtin_call(&vm,"file_exists",&joined,1);
  GmlVal strict_match=gml_builtin_call(&vm,"file_exists_ns",&joined,1);
  ok=ok && regular_match.t==V_REAL && regular_match.d==1 &&
    strict_match.t==V_REAL && strict_match.d==0;

  char tree_path[512],tree_file[768],copied_tree_file[768],moved_tree[512];
  snprintf(tree_path,sizeof tree_path,"%s/tree",content_root);
  snprintf(tree_file,sizeof tree_file,"%s/a.txt",tree_path);
  snprintf(copied_tree_file,sizeof copied_tree_file,"%s/tree-copy/a.txt",save_root);
  snprintf(moved_tree,sizeof moved_tree,"%s/tree-moved",save_root);
  if(mkdir(tree_path,0700)!=0 || !fixture_write_text(tree_file,"tree-data")) ok=0;
  GmlVal directory_copy_args[]={vstr("tree"),vstr("tree-copy")};
  GmlVal directory_copy=gml_builtin_call(&vm,"directory_copy_ns",directory_copy_args,2);
  GmlVal copied_directory=vstr("tree-copy");
  GmlVal directory_exists=gml_builtin_call(&vm,"directory_exists_ns",&copied_directory,1);
  memset(observed,0,sizeof observed);
  int tree_copied=fixture_read_text(copied_tree_file,observed,sizeof observed) &&
    !strcmp(observed,"tree-data");
  GmlVal directory_move_args[]={vstr("tree-copy"),vstr("tree-moved")};
  GmlVal directory_moved=gml_builtin_call(&vm,"directory_move_ns",directory_move_args,2);
  GmlVal moved_directory=vstr("tree-moved");
  GmlVal directory_deleted=gml_builtin_call(&vm,"directory_delete_ns",&moved_directory,1);
  ok=ok && directory_copy.t==V_REAL && directory_copy.d==1 &&
    directory_exists.t==V_REAL && directory_exists.d==1 &&
    tree_copied && directory_moved.t==V_REAL && directory_moved.d==1 &&
    directory_deleted.t==V_REAL && directory_deleted.d==1 && access(moved_tree,F_OK)!=0;

  const uint8_t pixels[]={255,0,0,255, 0,255,0,255};
  GmlMediaBuffer png={0};
  char source_png[512],saved_png[512];
  snprintf(source_png,sizeof source_png,"%s/mask.png",content_root);
  snprintf(saved_png,sizeof saved_png,"%s/mask.png",save_root);
  if(!gml_image_encode_png(pixels,2,1,4,8,&png)) ok=0;
  FILE *file=png.data?fopen(source_png,"wb"):NULL;
  if(!file || fwrite(png.data,1,png.size,file)!=png.size) ok=0;
  if(file && fclose(file)!=0) ok=0;
  gml_media_buffer_release(&png);
  GmlVal color_args[]={vstr("mask.png"),vreal(255)};
  GmlVal keyed=gml_builtin_call(&vm,"MakeColorTransparent",color_args,2);
  uint8_t *encoded=NULL;
  size_t encoded_size=0;
  GmlMediaBuffer decoded={0};
  int width=0,height=0,components=0;
  ok=ok && keyed.t==V_REAL && keyed.d==1 &&
    anygm_vfs_read_all(&services,saved_png,&encoded,&encoded_size,1024u*1024u) &&
    gml_image_decode_rgba(encoded,encoded_size,&decoded,&width,&height,&components) &&
    width==2 && height==1 && decoded.size>=8 && decoded.data[3]==0 && decoded.data[7]==255;
  free(encoded);
  gml_media_buffer_release(&decoded);

  gml_vm_free(&vm);
  unlink(source_png);
  unlink(saved_png);
  unlink(outside);
  unlink(tree_file);
  rmdir(tree_path);
  char note_path[512];
  snprintf(note_path,sizeof note_path,"%s/note.txt",save_root);
  unlink(note_path); unlink(copied_path);
  rmdir(save_root); rmdir(content_root);
  if(!ok) fprintf(stderr,
    "portable filesystem/color-key extension fixture failed "
    "save=%.0f copy=%.0f outside_save=%.0f traversal=%.0f "
    "dir_copy=%.0f dir_exists=%.0f dir_move=%.0f dir_delete=%.0f key=%.0f\n",
    saved.t==V_REAL?saved.d:-1.0,copied.t==V_REAL?copied.d:-1.0,
    outside_saved.t==V_REAL?outside_saved.d:-1.0,
    traversal_saved.t==V_REAL?traversal_saved.d:-1.0,
    directory_copy.t==V_REAL?directory_copy.d:-1.0,
    directory_exists.t==V_REAL?directory_exists.d:-1.0,
    directory_moved.t==V_REAL?directory_moved.d:-1.0,
    directory_deleted.t==V_REAL?directory_deleted.d:-1.0,
    keyed.t==V_REAL?keyed.d:-1.0);
  return ok;
}
