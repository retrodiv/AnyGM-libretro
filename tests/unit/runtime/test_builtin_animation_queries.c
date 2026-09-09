/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_builtin.h"
#include "anygm_test_runner.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition,const char *message){
  if(!condition) fprintf(stderr,"%s\n",message);
  return condition;
}
static int real_is(GmlVal value,double number){
  return value.t==V_REAL && value.d==number;
}
static void put32(unsigned char *data,size_t offset,uint32_t value){
  for(int i=0;i<4;i++) data[offset+i]=(unsigned char)(value>>(8*i));
}
static void put_float(unsigned char *data,size_t offset,float value){
  uint32_t bits; memcpy(&bits,&value,4); put32(data,offset,bits);
}
static void put_name(unsigned char *data,size_t offset,const char *name){
  put32(data,offset-4,(uint32_t)strlen(name));
  memcpy(data+offset,name,strlen(name)+1);
}
static void asset_fixture(GmlWin *win,unsigned char *data){
  memset(data,0,512); memset(win,0,sizeof *win);
  win->data=data; win->size=512; win->n_chunks=1;
  memcpy(win->chunks[0].name,"ACRV",4);
  win->chunks[0].off=16; win->chunks[0].size=240;
  put32(data,16,1); put32(data,20,2);
  put32(data,24,40); put32(data,28,200);
  put32(data,40,320); put32(data,48,3);
  put32(data,52,352); put32(data,64,1);
  put_float(data,68,0); put_float(data,72,10);
  put32(data,92,384); put32(data,104,2);
  put_float(data,108,0); put_float(data,112,20);
  put_float(data,132,1); put_float(data,136,40);
  put32(data,156,416); put32(data,168,0);
  put32(data,200,448); put32(data,208,1);
  put32(data,212,416); put32(data,224,1);
  put_float(data,228,0); put_float(data,232,90);
  put_name(data,320,"curve_a"); put_name(data,352,"first");
  put_name(data,384,"middle"); put_name(data,416,"last");
  put_name(data,448,"curve_b");
}
static GmlVal query(GmlVM *vm,GmlVal curve,const char *name,int cached,int *ok){
  const char *function="animcurve_get_channel_index";
  GmlVal args[]={curve,vstr(name)};
  if(!cached) return gml_builtin_call(vm,function,args,2);
  int id=gml_builtin_fast_id(vm,function);
  *ok &= expect(id>=0,"channel index query must have a cached ID");
  return id>=0?gml_builtin_call_fast_id(vm,id,function,args,2):vundef();
}
static int asset_names_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    unsigned char data[512],original[512]; GmlWin win;
    asset_fixture(&win,data); memcpy(original,data,sizeof data);
    GmlVM vm={.win=&win};
    const char *names[]={"first","middle","last"};
    for(int i=0;i<3;i++)
      ok &= expect(real_is(query(&vm,vreal(0),names[i],cached,&ok),i),
                   "channel index follows channel order, not point count");
    ok &= expect(real_is(query(&vm,vreal(1),"last",cached,&ok),0),
                 "channel names are scoped to the selected curve");
    GmlVal args[]={vreal(0),vstr("middle")};
    GmlVal channel=gml_builtin_call(&vm,"animcurve_get_channel",args,2);
    args[1]=vreal(1);
    GmlVal by_index=gml_builtin_call(&vm,"animcurve_get_channel",args,2);
    ok &= expect(real_is(channel,by_index.d),"existing named and indexed channels agree");
    GmlVal evaluate[]={channel,vreal(0.25)};
    ok &= expect(real_is(gml_builtin_call(&vm,"animcurve_channel_evaluate",evaluate,2),25),
                 "existing interpolation remains unchanged");
    ok &= expect(!memcmp(data,original,sizeof data),"queries never mutate immutable curve bytes");
    gml_vm_free(&vm);
  }
  return ok;
}
static GmlVal make_struct(GmlVM *vm){
  GmlInstance *instance=gml_struct_new(vm);
  return instance?vreal(instance->id):vundef();
}
static void set_field(GmlVM *vm,GmlVal structure,const char *name,GmlVal value){
  GmlVal args[]={structure,vstr(name),value};
  gml_builtin_call(vm,"variable_struct_set",args,3);
}
static int struct_names_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    GmlVal curve=make_struct(&vm),other=make_struct(&vm);
    GmlVal first=make_struct(&vm),last=make_struct(&vm);
    GmlVal channels=gml_arr_new(2,vundef()),single=gml_arr_new(1,vundef());
    set_field(&vm,first,"name",vstr("first")); set_field(&vm,last,"name",vstr("last"));
    gml_arr_set(channels,0,first); gml_arr_set(channels,1,last); gml_arr_set(single,0,last);
    set_field(&vm,curve,"channels",channels); set_field(&vm,other,"channels",single);
    ok &= expect(real_is(query(&vm,curve,"last",cached,&ok),1) &&
                 real_is(query(&vm,other,"last",cached,&ok),0),
                 "struct queries use each curve's actual ordered channel array");
    set_field(&vm,last,"name",vstr("renamed"));
    gml_arr_set(channels,0,last); gml_arr_set(channels,1,first);
    ok &= expect(real_is(query(&vm,curve,"renamed",cached,&ok),0) &&
                 real_is(query(&vm,curve,"first",cached,&ok),1),
                 "queries observe live channel names and reordering without a stale cache");
    ok &= expect(real_is(gml_arr_get(channels,0),last.d) &&
                 real_is(gml_arr_get(channels,1),first.d),"queries leave channel references unchanged");
    gml_vm_free(&vm);
  }
  return ok;
}
/* Explicit defensive policy: invalid input has no valid channel index. */
static int invalid_arguments_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    unsigned char data[512]; GmlWin win; asset_fixture(&win,data);
    GmlVM vm={.win=&win};
    GmlVal invalid[]={vreal(-1),vreal(2),vreal(0.5),vreal(NAN),vreal(INFINITY),
                      vreal(1e100),vstr("0"),vundef(),vreal(GML_STRUCT_ID_BASE)};
    for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++)
      ok &= expect(query(&vm,invalid[i],"first",cached,&ok).t==V_UNDEF,
                   "invalid handles do not alias an existing curve");
    ok &= expect(query(&vm,vreal(0),"missing",cached,&ok).t==V_UNDEF &&
                 query(&vm,vreal(0),"Middle",cached,&ok).t==V_UNDEF,
                 "absent or differently cased names do not silently select channel zero");
    GmlVal empty=make_struct(&vm);
    ok &= expect(query(&vm,empty,"first",cached,&ok).t==V_UNDEF,"missing channel array is invalid");
    set_field(&vm,empty,"channels",vreal(12));
    ok &= expect(query(&vm,empty,"first",cached,&ok).t==V_UNDEF,"channel array must be an array");
    GmlVal channels=gml_arr_new(1,vreal(0)); set_field(&vm,empty,"channels",channels);
    ok &= expect(query(&vm,empty,"first",cached,&ok).t==V_UNDEF,"channel entry must be a live struct");
    GmlVal channel=make_struct(&vm); gml_arr_set(channels,0,channel);
    ok &= expect(query(&vm,empty,"first",cached,&ok).t==V_UNDEF,"channel name must exist");
    set_field(&vm,channel,"name",vreal(1));
    ok &= expect(query(&vm,empty,"first",cached,&ok).t==V_UNDEF,"channel name must be a string");
    ok &= expect(gml_builtin_call(&vm,"animcurve_get_channel_index",NULL,0).t==V_UNDEF,
                 "missing arguments do not read argument memory");
    GmlVal wrong_name[]={vreal(0),vreal(0)};
    ok &= expect(gml_builtin_call(&vm,"animcurve_get_channel_index",wrong_name,2).t==V_UNDEF,
                 "numeric channel names are not converted into indices");
    gml_vm_free(&vm);
  }
  return ok;
}
static int malformed_assets_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++) for(int defect=0;defect<10;defect++){
    unsigned char data[512]; GmlWin win; asset_fixture(&win,data);
    GmlVM vm={.win=&win};
    switch(defect){
      case 0: win.n_chunks=0; break;
      case 1: win.chunks[0].size=7; break;
      case 2: put32(data,20,UINT32_MAX); break;
      case 3: put32(data,24,UINT32_MAX); break;
      case 4: put32(data,48,UINT32_MAX); break;
      case 5: put32(data,64,UINT32_MAX); break;
      case 6: put32(data,92,2); break;
      case 7: put32(data,380,UINT32_MAX); break;
      case 8: win.size=120; break;
      case 9: data[390]='!'; break;
    }
    ok &= expect(query(&vm,vreal(0),"middle",cached,&ok).t==V_UNDEF,
                 "malformed curve, channel, point or string spans are rejected");
    gml_vm_free(&vm);
  }
  GmlVM empty={0};
  ok &= expect(query(&empty,vreal(0),"first",0,&ok).t==V_UNDEF,"absent content has no curve");
  gml_vm_free(&empty);
  return ok;
}
/* Reproduce both struct-setup paths without invoking any animation builtin. This
 * control separates pre-existing setter lifetime from a read-only query's work. */
static int struct_setup_control_case(void){
  for(int repeat=0;repeat<2;repeat++){
    GmlVM vm={0};
    GmlVal curve=make_struct(&vm),other=make_struct(&vm);
    GmlVal first=make_struct(&vm),last=make_struct(&vm);
    GmlVal channels=gml_arr_new(2,vundef()),single=gml_arr_new(1,vundef());
    set_field(&vm,first,"name",vstr("first")); set_field(&vm,last,"name",vstr("last"));
    gml_arr_set(channels,0,first); gml_arr_set(channels,1,last); gml_arr_set(single,0,last);
    set_field(&vm,curve,"channels",channels); set_field(&vm,other,"channels",single);
    set_field(&vm,last,"name",vstr("renamed"));
    gml_arr_set(channels,0,last); gml_arr_set(channels,1,first);
    gml_vm_free(&vm);
    GmlVM invalid={0};
    GmlVal empty=make_struct(&invalid);
    set_field(&invalid,empty,"channels",vreal(12));
    channels=gml_arr_new(1,vreal(0)); set_field(&invalid,empty,"channels",channels);
    GmlVal channel=make_struct(&invalid); gml_arr_set(channels,0,channel);
    set_field(&invalid,channel,"name",vreal(1));
    gml_vm_free(&invalid);
  }
  return 1;
}
/* Borrowed literal fields isolate query behavior from dynamic setter ownership.
 * The setter-driven cases and query-free lifetime control remain above. */
static int borrowed_struct_fields_case(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    GmlVM vm={0};
    GmlVal curve=make_struct(&vm),first=make_struct(&vm),last=make_struct(&vm);
    GmlInstance *c=gml_struct_find(&vm,(unsigned)curve.d);
    GmlInstance *a=gml_struct_find(&vm,(unsigned)first.d);
    GmlInstance *b=gml_struct_find(&vm,(unsigned)last.d);
    if(!c || !a || !b){ gml_vm_free(&vm); return 0; }
    *gml_varmap_put(&a->vars,"name")=vstr("first");
    *gml_varmap_put(&b->vars,"name")=vstr("last");
    GmlVal channels=gml_arr_new(2,vundef());
    gml_arr_set(channels,0,first); gml_arr_set(channels,1,last);
    *gml_varmap_put(&c->vars,"channels")=channels;
    ok &= expect(real_is(query(&vm,curve,"last",cached,&ok),1),
                 "borrowed fields retain the same nonzero channel index");
    *gml_varmap_get(&b->vars,"name")=vstr("renamed");
    gml_arr_set(channels,0,last); gml_arr_set(channels,1,first);
    ok &= expect(real_is(query(&vm,curve,"renamed",cached,&ok),0) &&
                 real_is(query(&vm,curve,"first",cached,&ok),1),
                 "query-only controls observe live fields without setter allocation");
    gml_vm_free(&vm);
  }
  return ok;
}
int main(int argc,char **argv){
  static const AnygmTestCase cases[]={
    {"asset_channel_names",asset_names_case}, {"struct_channel_names",struct_names_case},
    {"invalid_arguments",invalid_arguments_case}, {"malformed_assets",malformed_assets_case},
    {"struct_setup_control",struct_setup_control_case},
    {"borrowed_struct_fields",borrowed_struct_fields_case}
  };
  static const AnygmTestGroup group={"animation_queries",cases,sizeof cases/sizeof cases[0]};
  const char *filter=NULL;
  if(argc==3 && !strcmp(argv[1],"--case")) filter=argv[2];
  else if(argc!=1) return EXIT_FAILURE;
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,filter,&result);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
