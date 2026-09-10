/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Delayed calls share scheduling while keeping their handles and lifetime private. */
#include "gml_builtin.h"
#include "gmlc_package.h"
#include "stdio_vfs.h"
#include "anygm_test_runner.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <io.h>
#define fixture_unlink _unlink
#define fixture_rmdir _rmdir
#else
#include <unistd.h>
#define fixture_unlink unlink
#define fixture_rmdir rmdir
#endif

typedef struct {
  GmlVM vm;
  GmlWin win;
  AnygmHostServices host;
  char directory[160], package[192], script[3][192];
  GmlVal callback[3];
} Fixture;

static int expect(int condition,const char *message){
  if(!condition) fprintf(stderr,"delayed call: %s\n",message);
  return condition;
}
static int real_is(GmlVal value,double number){return value.t==V_REAL && value.d==number;}
static void set_global(Fixture *f,const char *name,GmlVal value){
  *gml_varmap_put(&f->vm.globals,name)=value;
}
static double number(Fixture *f,const char *name){return gml_global_num(&f->vm,name);}
static void fixture_free(Fixture *f){
  gml_vm_free(&f->vm); gml_win_free(&f->win);
  for(int i=0;i<3;i++) if(f->script[i][0]) fixture_unlink(f->script[i]);
  if(f->package[0]) fixture_unlink(f->package);
  if(f->directory[0]) fixture_rmdir(f->directory);
}
static int fixture_init(Fixture *f){
  memset(f,0,sizeof(*f));
#ifdef _WIN32
  int created=0;
  for(int i=0;i<256 && !created;i++){
    snprintf(f->directory,sizeof f->directory,"build/delayed-call-%ld-%d",(long)_getpid(),i);
    created=_mkdir(f->directory)==0;
  }
  if(!created){ f->directory[0]=0; return 0; }
#else
  snprintf(f->directory,sizeof f->directory,"build/delayed-call-XXXXXX");
  if(!mkdtemp(f->directory)){ f->directory[0]=0; return 0; }
#endif
  const char *names[]={"first_callback","second_callback","third_callback"};
  const char *sources[]={
    "global.hits += 1; global.trace = global.trace * 10 + 1; "
    "global.argc = argument_count; "
    "if(global.mode == 1) call_cancel(global.cancel); "
    "if(global.mode == 2) global.child = call_later(1,1,global.second); "
    "if(global.mode == 3) amount += 5;",
    "global.hits += 1; global.trace = global.trace * 10 + 2;",
    "global.hits += 1; global.trace = global.trace * 10 + 3;"
  };
  GmlcScript scripts[3]={0};
  for(int i=0;i<3;i++){
    snprintf(f->script[i],sizeof f->script[i],"%s/callback-%d.gml",f->directory,i);
    FILE *file=fopen(f->script[i],"wb");
    if(!file) return 0;
    size_t len=strlen(sources[i]); int written=fwrite(sources[i],1,len,file)==len;
    if(fclose(file)!=0 || !written) return 0;
    scripts[i].id=scripts[i].name=(char *)names[i]; scripts[i].source_path=f->script[i];
  }
  f->host.struct_size=sizeof f->host; f->host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&f->host);
  GmlcProject project={0}; GmlcRoom room={0}; int order=0;
  project.host=&f->host; project.name=(char *)"delayed-callback-fixture";
  project.scripts=scripts; project.n_scripts=project.cap_scripts=3;
  project.rooms=&room; project.n_rooms=project.cap_rooms=1;
  project.room_order=&order; project.n_room_order=1;
  room.name=room.id=(char *)"neutral_room"; room.width=room.height=16; room.speed=10;
  snprintf(f->package,sizeof f->package,"%s/data.win",f->directory);
  char error[256]={0};
  if(!gmlc_package_write_structural(&project,f->package,error,sizeof error)){
    fprintf(stderr,"delayed-call fixture: %s\n",error); return 0;
  }
  if(gml_win_load_host(&f->win,&f->host,f->package) || gml_vm_init(&f->vm,&f->win,&f->host)) return 0;
  f->vm.room_index=0;
  for(int i=0;i<3;i++){
    char name[80]; snprintf(name,sizeof name,"gml_Script_%s",names[i]);
    int code=gml_code_index_by_name(&f->win,name);
    if(code<0) return 0;
    f->callback[i]=vreal(GML_FUNCVAL_TAG|code);
  }
  set_global(f,"hits",vreal(0)); set_global(f,"trace",vreal(0));
  set_global(f,"mode",vreal(0)); set_global(f,"argc",vreal(-1));
  set_global(f,"second",f->callback[1]);
  return 1;
}
static GmlVal call(Fixture *f,const char *name,GmlVal *args,int count,int cached,int *ok){
  if(!cached) return gml_builtin_call(&f->vm,name,args,count);
  int id=gml_builtin_fast_id(&f->vm,name);
  *ok &= expect(id>=0,"operation has an exact cached identity");
  return id>=0?gml_builtin_call_fast_id(&f->vm,id,name,args,count):vundef();
}
static GmlVal later(Fixture *f,double period,int units,int callback,int loop,int cached,int *ok){
  GmlVal args[]={vreal(period),vreal(units),f->callback[callback],vreal(loop)};
  GmlVal result=call(f,"call_later",args,loop<0?3:4,cached,ok);
  *ok &= expect(result.t==V_REAL && result.d>=3,"creation returns a live delayed-call identity");
  return result;
}
static GmlVal ordinary(Fixture *f,int parent,int callback){
  GmlVal args[]={vreal(parent),vreal(1),vreal(1),f->callback[callback]};
  GmlVal id=gml_builtin_call(&f->vm,"time_source_create",args,4);
  gml_builtin_call(&f->vm,"time_source_start",&id,1); return id;
}
static void visit(void *context,GmlVal value){(void)value; (*(int *)context)++;}
static int root_count(Fixture *f){
  int count=0; gml_builtin_state_visit_values(f->vm.builtins,visit,&count); return count;
}
static int basic_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    Fixture f; if(!fixture_init(&f)){fixture_free(&f); return 0;}
    later(&f,2,1,0,-1,cached,&ok);
    gml_time_sources_tick(&f.vm);
    ok &= expect(number(&f,"hits")==0 && root_count(&f)==2,"one-shot waits with one retained callback root pair");
    gml_time_sources_tick(&f.vm); gml_time_sources_tick(&f.vm);
    ok &= expect(number(&f,"hits")==1 && number(&f,"argc")==0 && root_count(&f)==0,
                 "one-shot fires once without user arguments and releases its slot");
    fixture_free(&f);
  }
  return ok;
}
static int periods_case(void){
  const double periods[]={0,-3,0.5,2.9,3}; const int frames[]={1,1,1,2,3}; int ok=1;
  for(int i=0;i<5;i++){
    Fixture f; if(!fixture_init(&f)){fixture_free(&f); return 0;}
    later(&f,periods[i],1,0,0,0,&ok);
    for(int frame=1;frame<=5;frame++){
      gml_time_sources_tick(&f.vm);
      ok &= expect(number(&f,"hits")== (frame>=frames[i]),"frame periods floor and clamp at one");
    }
    fixture_free(&f);
  }
  Fixture f; if(!fixture_init(&f)){fixture_free(&f); return 0;}
  later(&f,0.2,0,0,0,0,&ok);
  gml_time_sources_tick(&f.vm); ok &= expect(number(&f,"hits")==0,"seconds wait before their deadline");
  gml_time_sources_tick(&f.vm); ok &= expect(number(&f,"hits")==1,"seconds expire on an exact deterministic deadline");
  fixture_free(&f); return ok;
}
static int visibility_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    Fixture f; if(!fixture_init(&f)){fixture_free(&f); return 0;}
    GmlVal hidden=later(&f,1,1,0,0,cached,&ok), visible=ordinary(&f,0,1);
    ok &= expect(real_is(gml_builtin_call(&f.vm,"time_source_exists",&hidden,1),0),"hidden handle is absent from ordinary lookup");
    const char *getters[]={"time_source_get_parent","time_source_get_period","time_source_get_state",
      "time_source_get_reps_completed","time_source_get_reps_remaining","time_source_get_units",
      "time_source_get_time_remaining","time_source_get_children"};
    for(size_t i=0;i<sizeof getters/sizeof getters[0];i++)
      ok &= expect(gml_builtin_call(&f.vm,getters[i],&hidden,1).t==V_UNDEF,"ordinary queries cannot inspect a hidden handle");
    for(int parent=0;parent<=1;parent++){
      GmlVal id=vreal(parent), children=gml_builtin_call(&f.vm,"time_source_get_children",&id,1);
      ok &= expect(gml_val_array_length(children)==(parent==0),"hidden handle is absent from public children");
      gml_values_release_owned(&children,1);
    }
    GmlVal child_args[]={hidden,vreal(1),vreal(1),f.callback[2]};
    ok &= expect(gml_builtin_call(&f.vm,"time_source_create",child_args,4).t==V_UNDEF,"hidden timer cannot parent a public timer");
    gml_builtin_call(&f.vm,"time_source_stop",&hidden,1);
    gml_builtin_call(&f.vm,"time_source_destroy",&hidden,1);
    call(&f,"call_cancel",&visible,1,cached,&ok);
    gml_time_sources_tick(&f.vm);
    ok &= expect(number(&f,"trace")==21 && root_count(&f)==2,"cross-domain operations cannot cancel the other timer kind");
    fixture_free(&f);
  }
  return ok;
}
static int cancellation_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    Fixture f; if(!fixture_init(&f)){fixture_free(&f); return 0;}
    GmlVal cancelled=later(&f,1,1,1,0,cached,&ok);
    call(&f,"call_cancel",&cancelled,1,cached,&ok); gml_time_sources_tick(&f.vm);
    ok &= expect(number(&f,"hits")==0 && root_count(&f)==0,"cancellation removes pending work");
    set_global(&f,"mode",vreal(1));
    GmlVal self=later(&f,2,1,0,1,cached,&ok); set_global(&f,"cancel",self);
    for(int i=0;i<6;i++) gml_time_sources_tick(&f.vm);
    ok &= expect(number(&f,"hits")==1 && root_count(&f)==0,"loop can cancel itself without another callback");
    set_global(&f,"hits",vreal(0)); set_global(&f,"trace",vreal(0));
    later(&f,1,1,0,0,cached,&ok);
    GmlVal sibling=later(&f,1,1,1,0,cached,&ok); set_global(&f,"cancel",sibling);
    gml_time_sources_tick(&f.vm);
    ok &= expect(number(&f,"trace")==1 && root_count(&f)==0,"callback cancels a later due sibling");
    fixture_free(&f);
  }
  return ok;
}
static int ordering_case(void){
  Fixture f; int ok=1; if(!fixture_init(&f)){fixture_free(&f); return 0;}
  ordinary(&f,1,2); later(&f,1,1,1,0,0,&ok); ordinary(&f,0,0);
  gml_time_sources_tick(&f.vm);
  ok &= expect(number(&f,"trace")==123,"global, hidden and game callback phases are ordered");
  set_global(&f,"hits",vreal(0)); set_global(&f,"trace",vreal(0)); set_global(&f,"mode",vreal(2));
  later(&f,1,1,0,0,0,&ok); gml_time_sources_tick(&f.vm);
  ok &= expect(number(&f,"trace")==1,"callback-created hidden work is not due immediately");
  gml_time_sources_tick(&f.vm); ok &= expect(number(&f,"trace")==12,"new hidden work joins the next tick");
  fixture_free(&f); return ok;
}
static int pause_and_binding_case(void){
  Fixture f; int ok=1; if(!fixture_init(&f)){fixture_free(&f); return 0;}
  ordinary(&f,1,1); later(&f,1,1,0,0,0,&ok); GmlVal game=vreal(1);
  gml_builtin_call(&f.vm,"time_source_pause",&game,1); gml_time_sources_tick(&f.vm);
  ok &= expect(number(&f,"trace")==1,"game pause does not pause delayed calls");
  gml_builtin_call(&f.vm,"time_source_resume",&game,1); gml_time_sources_tick(&f.vm);
  ok &= expect(number(&f,"trace")==12,"resumed game timer remains usable");
  GmlInstance *receiver=gml_struct_new(&f.vm);
  if(!receiver){fixture_free(&f); return 0;}
  *gml_varmap_put(&receiver->vars,"amount")=vreal(7);
  GmlVal args[]={vreal(receiver->id),f.callback[0]};
  f.callback[0]=gml_builtin_call(&f.vm,"method",args,2); set_global(&f,"mode",vreal(3));
  later(&f,1,1,0,0,0,&ok); gml_time_sources_tick(&f.vm);
  GmlVal *amount=gml_varmap_get(&receiver->vars,"amount");
  ok &= expect(amount && real_is(*amount,12),"delayed callback preserves its bound receiver");
  fixture_free(&f); return ok;
}
static int restore_case(void){
  Fixture f; int ok=1; if(!fixture_init(&f)){fixture_free(&f); return 0;}
  GmlVal id=later(&f,3,1,0,1,0,&ok); gml_time_sources_tick(&f.vm);
  size_t size=gml_vm_state_size(&f.vm),written=0,used=0;
  unsigned char *bytes=malloc(size?size:1),*again=malloc(size?size:1);
  ok &= expect(bytes && again && gml_vm_state_save(&f.vm,bytes,size,&written) && written==size,"pending hidden timer saves in current VM state");
  call(&f,"call_cancel",&id,1,0,&ok);
  if(ok) ok &= expect(gml_vm_state_load(&f.vm,bytes,size,&used) && used==size,"pending hidden timer restores");
  if(ok){
    size_t repeated=0;
    ok &= expect(gml_vm_state_save(&f.vm,again,size,&repeated) && repeated==size && !memcmp(bytes,again,size),"same-run timer state is canonical");
    ok &= expect(real_is(gml_builtin_call(&f.vm,"time_source_exists",&id,1),0),"restored delayed timer remains hidden");
    gml_time_sources_tick(&f.vm); ok &= expect(number(&f,"hits")==0,"restore preserves residual delay");
    gml_time_sources_tick(&f.vm); ok &= expect(number(&f,"hits")==1,"restored loop fires at its original deadline");
    call(&f,"call_cancel",&id,1,0,&ok); ok &= expect(root_count(&f)==0,"restored handle remains cancellable");
  }
  free(bytes); free(again); fixture_free(&f); return ok;
}
static int bounds_and_reuse_case(void){
  Fixture f,other; int ok=1;
  if(!fixture_init(&f)){fixture_free(&f); return 0;}
  if(!fixture_init(&other)){fixture_free(&other); fixture_free(&f); return 0;}
  GmlVal id=later(&other,1,1,0,0,0,&ok);
  const double bad[]={NAN,INFINITY,-INFINITY,2147483648.0,-1};
  for(size_t i=0;i<sizeof bad/sizeof bad[0];i++){
    GmlVal handle=vreal(bad[i]); call(&f,"call_cancel",&handle,1,1,&ok);
  }
  GmlVal args[]={vreal(1),vreal(1),f.callback[0],vreal(0)};
  for(int count=0;count<3;count++) ok &= expect(real_is(call(&f,"call_later",args,count,1,&ok),-1),"missing arguments reject without retaining work");
  args[2]=vundef(); ok &= expect(real_is(call(&f,"call_later",args,4,1,&ok),-1),"invalid callback is rejected");
  args[2]=f.callback[0]; args[1]=vreal(3);
  ok &= expect(real_is(call(&f,"call_later",args,4,1,&ok),-1),"invalid units reject without a source");
  double previous=-1;
  for(int i=0;i<1100;i++){
    GmlVal current=later(&f,1,1,0,0,0,&ok);
    ok &= expect(current.t==V_REAL && current.d>previous,"reclaimed slots do not recycle exposed handles");
    previous=current.d; gml_time_sources_tick(&f.vm); set_global(&f,"trace",vreal(0));
  }
  ok &= expect(number(&f,"hits")==1100 && root_count(&f)==0 && number(&other,"hits")==0,"one-shot cleanup avoids pool exhaustion and is engine-local");
  call(&f,"call_cancel",&id,1,0,&ok); gml_time_sources_tick(&other.vm);
  ok &= expect(number(&other,"hits")==1,"cross-engine handle cannot cancel another engine");
  fixture_free(&f); fixture_free(&other); return ok;
}
int main(int argc,char **argv){
  const AnygmTestCase cases[]={{"basic",basic_case},{"periods",periods_case},{"visibility",visibility_case},
    {"cancellation",cancellation_case},{"ordering",ordering_case},{"pause_and_binding",pause_and_binding_case},
    {"restore",restore_case},{"bounds_and_reuse",bounds_and_reuse_case}};
  const AnygmTestGroup group={"delayed_calls",cases,sizeof cases/sizeof cases[0]};
  const char *filter=argc==3 && !strcmp(argv[1],"--case")?argv[2]:NULL; AnygmTestResult result={0};
  int ok=anygm_test_run_groups(&group,1,filter,&result);
  printf("delayed calls: %d passed, %d failed\n",result.passed,result.failed);
  return ok?0:1;
}
