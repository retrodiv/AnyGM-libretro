/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "software3d_test_fixture.h"


void free_extension_fixture(GmlVM *vm){
  if(!vm) return;
  gml_colgrid_invalidate(vm);
  free(vm->cg_off);
  free(vm->cg_items);
  free(vm->cg_overlay);
  free(vm->inst);
  vm->cg_off=NULL; vm->cg_items=NULL; vm->cg_overlay=NULL; vm->inst=NULL;
}


void store_u32le(uint8_t *dst,uint32_t value){
  dst[0]=(uint8_t)value;
  dst[1]=(uint8_t)(value>>8);
  dst[2]=(uint8_t)(value>>16);
  dst[3]=(uint8_t)(value>>24);
}


size_t classic_information_record(uint8_t *dst,size_t capacity){
  const char caption[]="Information";
  const char text[]="{\\rtf1\\ansi\\pard\\qc\\b\\fs32 Generic information\\par\\b0\\fs24 Neutral \\ul underlined\\ulnone  fixture text\\par}";
  size_t need=8+4+sizeof(caption)-1+8*4+8+4+sizeof(text)-1;
  if(!dst || capacity<need) return 0;
  size_t at=0;
#define INFO_U32(v) do{ store_u32le(dst+at,(uint32_t)(v)); at+=4; }while(0)
  INFO_U32(0xFF000018u); INFO_U32(1);
  INFO_U32(sizeof(caption)-1); memcpy(dst+at,caption,sizeof(caption)-1); at+=sizeof(caption)-1;
  INFO_U32((uint32_t)-1); INFO_U32((uint32_t)-1); INFO_U32(600); INFO_U32(400);
  INFO_U32(1); INFO_U32(1); INFO_U32(0); INFO_U32(1);
  memset(dst+at,0,8); at+=8;
  INFO_U32(sizeof(text)-1); memcpy(dst+at,text,sizeof(text)-1); at+=sizeof(text)-1;
#undef INFO_U32
  return at;
}


static void add_named_asset_chunk(GmlWin *win,uint8_t *data,const char *tag,uint32_t base,
                                  uint32_t name_offset,int versioned){
  GmlChunk *chunk=&win->chunks[win->n_chunks++];
  memcpy(chunk->name,tag,4); chunk->name[4]=0;
  chunk->off=base; chunk->size=80;
  if(versioned){
    store_u32le(data+base,1);
    store_u32le(data+base+4,1);
    store_u32le(data+base+8,base+16);
  }else{
    store_u32le(data+base,1);
    store_u32le(data+base+4,base+16);
  }
  store_u32le(data+base+16,name_offset);
}


int asset_lookup_fixture(void){
  uint8_t data[1024]={0};
  char *names[]={
    "neutral_object", "neutral_sprite", "neutral_sound", "neutral_room",
    "neutral_path", "neutral_script", "neutral_font", "neutral_timeline",
    "neutral_shader", "neutral_sequence", "neutral_curve", "neutral_particles",
    "neutral_tileset"
  };
  uint32_t offsets[13];
  for(int i=0;i<13;i++) offsets[i]=800u+(uint32_t)i*4u;
  GmlWin win={0}; GmlVM vm={0}; GmlRender render={0};
  GmlSprite sprite={0}; GmlObject object={0}; GmlTimeline timeline={0};
  win.data=data; win.size=sizeof(data); win.strs=names; win.str_charoff=offsets; win.n_strs=13;
  add_named_asset_chunk(&win,data,"SOND",  0,offsets[2],0);
  add_named_asset_chunk(&win,data,"ROOM", 80,offsets[3],0);
  add_named_asset_chunk(&win,data,"PATH",160,offsets[4],0);
  add_named_asset_chunk(&win,data,"SCPT",240,offsets[5],0);
  add_named_asset_chunk(&win,data,"FONT",320,offsets[6],0);
  add_named_asset_chunk(&win,data,"SHDR",400,offsets[8],0);
  add_named_asset_chunk(&win,data,"SEQN",480,offsets[9],1);
  add_named_asset_chunk(&win,data,"ACRV",560,offsets[10],1);
  add_named_asset_chunk(&win,data,"PSYS",640,offsets[11],1);
  add_named_asset_chunk(&win,data,"BGND",720,offsets[12],0);
  sprite.name=names[1]; render.spr=&sprite; render.n_spr=1;
  object.name=names[0]; timeline.name=names[7];
  vm.win=&win; vm.render=&render; vm.objects=&object; vm.n_objects=1;
  vm.timelines=&timeline; vm.n_timelines=1;
  static const int expected_type[]={0,1,2,3,4,5,6,7,8,9,10,11,13};
  for(int i=0;i<13;i++){
    GmlVal name=vstr(names[i]);
    GmlVal index=call_values(&vm,"asset_get_index",&name,1);
    GmlVal type=call_values(&vm,"asset_get_type",&name,1);
    if(index.t!=V_REAL || index.d!=0 || type.t!=V_REAL || type.d!=expected_type[i]){
      fprintf(stderr,"asset lookup fixture mismatch at %d: index=%g type=%g\n",i,index.d,type.d);
      return 0;
    }
  }
  GmlVal absent=vstr("neutral_absent");
  if(call_values(&vm,"asset_get_index",&absent,1).d!=-1 ||
     call_values(&vm,"asset_get_type",&absent,1).d!=-1){
    fprintf(stderr,"asset lookup fixture missing-resource mismatch\n");
    return 0;
  }
  return 1;
}


int pushref_function_fixture(void){
  /* caller: pushref(callback), callv(), ret; callback: push 73, ret. The deliberately plain raw
   * reference proves that the FUNC occurrence, rather than its low bits, makes it callable. */
  GmlWin win={0}; GmlVM vm={0};
  win.bytecode=17;
  win.size=24;
  win.owns=1;
  win.data=calloc(win.size,1);
  win.n_code=2;
  win.code=calloc((size_t)win.n_code,sizeof(*win.code));
  win.n_refs=1;
  win.ref_addr=calloc(1,sizeof(*win.ref_addr));
  win.ref_name=calloc(1,sizeof(*win.ref_name));
  if(!win.data || !win.code || !win.ref_addr || !win.ref_name){
    gml_win_free(&win);
    fprintf(stderr,"pushref function fixture allocation failed\n");
    return 0;
  }
  store_u32le(win.data+0,0xFF02FFF5u);  /* break.i32 -11 */
  store_u32le(win.data+4,32u);          /* raw reference payload */
  store_u32le(win.data+8,0x99050000u);  /* callv.v 0 */
  store_u32le(win.data+12,0x9C050000u); /* ret.v */
  store_u32le(win.data+16,0x84000049u); /* push.e 73 */
  store_u32le(win.data+20,0x9C050000u); /* ret.v */
  win.code[0]=(GmlCode){.name="gml_Script_neutral_caller",.start=0,.length=16};
  win.code[1]=(GmlCode){.name="gml_Script_neutral_callback",.start=16,.length=8};
  win.ref_addr[0]=4;
  win.ref_name[0]=win.code[1].name;
  if(gml_vm_init(&vm,&win,NULL)){
    gml_win_free(&win);
    fprintf(stderr,"pushref VM initialization failed\n");
    return 0;
  }
  GmlVal result=gml_vm_run_code(&vm,0,NULL,NULL,NULL,0);
  int ok=result.t==V_REAL && result.d==73;
  if(!ok) fprintf(stderr,"pushref function dispatch mismatch: %.17g\n",result.d);
  gml_vm_free(&vm);
  gml_win_free(&win);
  return ok;
}


int member_function_self_fixture(void){
  /* A member field call keeps its receiver below the function value. The callback must observe
   * that receiver as self even when the field contains a plain function value rather than an
   * explicitly bound method struct. */
  GmlWin win={0}; GmlVM vm={0};
  win.bytecode=17;
  win.size=44;
  win.owns=1;
  win.data=calloc(win.size,1);
  win.n_code=2;
  win.code=calloc((size_t)win.n_code,sizeof(*win.code));
  win.n_refs=2;
  win.ref_addr=calloc((size_t)win.n_refs,sizeof(*win.ref_addr));
  win.ref_name=calloc((size_t)win.n_refs,sizeof(*win.ref_name));
  if(!win.data || !win.code || !win.ref_addr || !win.ref_name){
    gml_win_free(&win);
    fprintf(stderr,"member function fixture allocation failed\n");
    return 0;
  }
  win.code[0]=(GmlCode){.name="gml_Script_neutral_member_caller",.start=0,.length=32};
  win.code[1]=(GmlCode){.name="gml_Script_neutral_member_callback",.start=32,.length=12};
  win.ref_addr[0]=20; win.ref_name[0]="neutral_callback";
  win.ref_addr[1]=36; win.ref_name[1]="neutral_marker";
  if(gml_vm_init(&vm,&win,NULL)){
    gml_win_free(&win);
    fprintf(stderr,"member function VM initialization failed\n");
    return 0;
  }
  GmlInstance *receiver=gml_struct_new(&vm);
  if(!receiver){
    gml_vm_free(&vm); gml_win_free(&win);
    fprintf(stderr,"member function fixture receiver allocation failed\n");
    return 0;
  }
  *gml_varmap_put(&receiver->vars,"neutral_callback")=vreal((double)(GML_FUNCVAL_TAG|1));
  *gml_varmap_put(&receiver->vars,"neutral_marker")=vreal(73);
  store_u32le(win.data+0,0xC0020000u);  /* push.i32 receiver */
  store_u32le(win.data+4,receiver->id);
  store_u32le(win.data+8,0x86058800u);  /* dup-swap receiver (zero args) */
  store_u32le(win.data+12,0x86050000u); /* retain receiver for StackTop field read */
  store_u32le(win.data+16,0xC005FFF7u); /* push.v stack.neutral_callback */
  store_u32le(win.data+20,0xA0000000u);
  store_u32le(win.data+24,0x99050000u); /* callv.v 0 */
  store_u32le(win.data+28,0x9C050000u); /* ret.v */
  store_u32le(win.data+32,0xC005FFFFu); /* push.v self.neutral_marker */
  store_u32le(win.data+36,0xA0000000u);
  store_u32le(win.data+40,0x9C050000u); /* ret.v */
  GmlVal result=gml_vm_run_code(&vm,0,NULL,NULL,NULL,0);
  int ok=result.t==V_REAL && result.d==73;
  if(!ok) fprintf(stderr,"member function self dispatch mismatch: %.17g\n",result.d);
  gml_vm_free(&vm);
  gml_win_free(&win);
  return ok;
}


int member_function_argument_fixture(void){
  /* The implicit-self member form keeps arguments below @@This@@ and the function value:
   * [arg0, receiver, callback]. Verify that callv removes the receiver before collecting args. */
  GmlWin win={0}; GmlVM vm={0};
  win.bytecode=17;
  win.size=40;
  win.owns=1;
  win.data=calloc(win.size,1);
  win.n_code=2;
  win.code=calloc((size_t)win.n_code,sizeof(*win.code));
  win.n_refs=3;
  win.ref_addr=calloc((size_t)win.n_refs,sizeof(*win.ref_addr));
  win.ref_name=calloc((size_t)win.n_refs,sizeof(*win.ref_name));
  if(!win.data || !win.code || !win.ref_addr || !win.ref_name){
    gml_win_free(&win);
    fprintf(stderr,"member function argument fixture allocation failed\n");
    return 0;
  }
  win.code[0]=(GmlCode){.name="gml_Script_neutral_member_argument_caller",.start=0,.length=28};
  win.code[1]=(GmlCode){.name="gml_Script_neutral_member_argument_callback",.start=28,.length=12};
  win.ref_addr[0]=8;  win.ref_name[0]="@@This@@";
  win.ref_addr[1]=16; win.ref_name[1]="neutral_callback";
  win.ref_addr[2]=32; win.ref_name[2]="argument0";
  if(gml_vm_init(&vm,&win,NULL)){
    gml_win_free(&win);
    fprintf(stderr,"member argument VM initialization failed\n");
    return 0;
  }
  GmlInstance *receiver=gml_struct_new(&vm);
  if(!receiver){
    gml_vm_free(&vm); gml_win_free(&win);
    fprintf(stderr,"member function argument fixture receiver allocation failed\n");
    return 0;
  }
  *gml_varmap_put(&receiver->vars,"neutral_callback")=vreal((double)(GML_FUNCVAL_TAG|1));
  store_u32le(win.data+0,0x84000005u);  /* push.e 5 (arg0) */
  store_u32le(win.data+4,0xD9020000u);  /* call @@This@@(0) */
  store_u32le(win.data+8,0u);
  store_u32le(win.data+12,0xC005FFFAu); /* push.v builtin.neutral_callback */
  store_u32le(win.data+16,0xA0000000u);
  store_u32le(win.data+20,0x99050001u); /* callv.v 1 */
  store_u32le(win.data+24,0x9C050000u); /* ret.v */
  store_u32le(win.data+28,0xC305FFFAu); /* push.v builtin.argument0 */
  store_u32le(win.data+32,0xA0000000u);
  store_u32le(win.data+36,0x9C050000u); /* ret.v */
  GmlVal result=gml_vm_run_code(&vm,0,receiver,receiver,NULL,0);
  int ok=result.t==V_REAL && result.d==5;
  if(!ok) fprintf(stderr,"member function argument dispatch mismatch: %.17g\n",result.d);
  gml_vm_free(&vm);
  gml_win_free(&win);
  return ok;
}

