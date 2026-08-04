/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_builtin.h"
#include "stdio_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


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
  (void)gml_builtin_call(vm,"ini_close",NULL,0);
  char installed[128]={0},saved[128]={0};
  ok=ok && initial.t==V_REAL && initial.d==7 &&
     localized.t==V_REAL && localized.d==0.65 &&
     string_default.t==V_REAL && string_default.d==2.5 &&
     fixture_read_text(content_ini,installed,sizeof installed) && strstr(installed,"value=7") &&
     fixture_read_text(save_ini,saved,sizeof saved) && strstr(saved,"value=11");

  /* Localization INIs routinely exceed the old 256-entry settings limit and quote
   * string values.  A late value must be unquoted and survive the save overlay. */
  name=vstr(large_ini);
  (void)gml_builtin_call(vm,"ini_open",&name,1);
  GmlVal late_args[3]={vstr("localization"),vstr("key_599"),vstr("missing")};
  GmlVal late=gml_builtin_call(vm,"ini_read_string",late_args,3);
  ok=ok && late.t==V_STR && late.s && !strcmp(late.s,"value_599") &&
     gml_builtin_ini_entry_count(vm)==600;
  if(late.t==V_STR && late.d!=0) free((void*)late.s);
  (void)gml_builtin_call(vm,"ini_close",NULL,0);
  (void)gml_builtin_call(vm,"ini_open",&name,1);
  late=gml_builtin_call(vm,"ini_read_string",late_args,3);
  ok=ok && late.t==V_STR && late.s && !strcmp(late.s,"value_599");
  if(late.t==V_STR && late.d!=0) free((void*)late.s);
  (void)gml_builtin_call(vm,"ini_close",NULL,0);

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
