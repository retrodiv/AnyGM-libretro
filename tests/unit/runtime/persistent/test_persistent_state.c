/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_particle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int expect_vm_state_graph_case(void){
  GmlWin win={0}; GmlVM vm={0};
  vm.win=&win; vm.room_index=vm.pending_room=-1; vm.next_creation_seq=1;
  vm.particles=gml_particle_state_create(&vm);
  gml_vm_software3d_reset(&vm);
  GmlVal root=gml_arr_new(4,vreal(0)), child=gml_arr_new(2,vreal(7));
  GmlVal equal=gml_arr_new(2,vreal(7)), empty=gml_arr_new(0,vreal(0));
  GmlVal null_array=vreal(0); null_array.t=V_ARR;
  gml_arr_set(root,0,root); /* self cycle */
  gml_arr_set(root,1,child); gml_arr_set(child,0,root); /* mutual cycle */
  gml_arr_set(root,2,child); gml_arr_set(root,3,equal);
  *gml_varmap_put(&vm.globals,"graph")=root;
  *gml_varmap_put(&vm.globals,"empty")=empty;
  *gml_varmap_put(&vm.globals,"null_array")=null_array;
  vm.script_args[0]=root; vm.script_argc=1;
  vm.code_static=calloc(1,sizeof(*vm.code_static));
  vm.code_static_init=calloc(1,1); vm.code_static_count=1;
  if(!vm.particles || !vm.code_static || !vm.code_static_init){ gml_vm_free(&vm); return 0; }
  *gml_varmap_put(&vm.code_static[0],"alias")=child;
  vm.code_static_init[0]=1;
  size_t size=gml_vm_state_size(&vm),written=0,used=0;
  uint8_t *bytes=malloc(size?size:1), *again=malloc(size?size:1);
  int ok=size && bytes && again && gml_vm_state_save(&vm,bytes,size,&written) && written==size;
  for(int pass=0;pass<3 && ok;pass++){
    ok=gml_vm_state_load(&vm,bytes,size,&used) && used==size;
    GmlVal *restored=gml_varmap_get(&vm.globals,"graph");
    GmlVal *nil=gml_varmap_get(&vm.globals,"null_array");
    GmlVal *zero=gml_varmap_get(&vm.globals,"empty");
    GmlVal *alias=gml_varmap_get(&vm.code_static[0],"alias");
    GmlVal nested=restored?gml_arr_get(*restored,1):vreal(0);
    ok=ok && restored && nil && zero && alias &&
       nil->t==V_ARR && !nil->arr && zero->t==V_ARR && zero->arr &&
       gml_val_array_length(*zero)==0 && vm.script_args[0].arr==restored->arr &&
       gml_arr_get(*restored,0).arr==restored->arr &&
       gml_arr_get(nested,0).arr==restored->arr &&
       gml_arr_get(*restored,2).arr==nested.arr && alias->arr==nested.arr &&
       gml_arr_get(*restored,3).arr!=nested.arr &&
       gml_vm_state_size(&vm)==size &&
       gml_vm_state_save(&vm,again,size,&written) && written==size && !memcmp(bytes,again,size);
    if(ok){
      gml_arr_set(nested,1,vreal(91));
      ok=gml_arr_get(*alias,1).d==91 &&
         gml_arr_get(gml_arr_get(*restored,2),1).d==91 &&
         gml_arr_get(gml_arr_get(*restored,3),1).d==7;
    }
  }
  if(!ok) fputs("array graph identity, cycles or canonical replay failed\n",stderr);
  free(bytes); free(again); gml_vm_free(&vm);
  return ok;
}

int expect_vm_state_array_depth_case(void){
  GmlWin win={0}; GmlVM vm={0}; vm.win=&win;
  vm.particles=gml_particle_state_create(&vm); gml_vm_software3d_reset(&vm);
  GmlVal root=gml_arr_new(1,vreal(0)), tail=root;
  *gml_varmap_put(&vm.globals,"chain")=root;
  for(int i=1;i<64;i++){
    GmlVal child=gml_arr_new(1,vreal(0)); gml_arr_set(tail,0,child); tail=child;
  }
  gml_arr_set(tail,0,root); /* a back reference does not consume another definition depth */
  size_t size=gml_vm_state_size(&vm),written=0,used=0;
  void *bytes=malloc(size?size:1);
  int ok=size && bytes && gml_vm_state_save(&vm,bytes,size,&written) && written==size &&
         gml_vm_state_load(&vm,bytes,size,&used) && used==size;
  GmlVal *restored=gml_varmap_get(&vm.globals,"chain");
  tail=restored?*restored:vreal(0);
  for(int i=1;i<64;i++) tail=gml_arr_get(tail,0);
  ok=ok && restored && gml_arr_get(tail,0).arr==restored->arr;
  GmlVal extra=gml_arr_new(1,vreal(1)); gml_arr_set(tail,0,extra);
  ok=ok && gml_vm_state_size(&vm)==0 && !gml_vm_state_save(&vm,bytes,size,&written);
  if(!ok) fputs("array depth bounds must reject instead of truncating values\n",stderr);
  free(bytes); gml_vm_free(&vm);
  return ok;
}


int expect_vm_state_case(void){
  GmlWin win={0};
  uint8_t content_string[]="canonical content string";
  char runtime_string[]="canonical content string";
  char *strings[]={(char *)content_string};
  uint32_t string_offsets[]={0};
  win.data=content_string;
  win.size=sizeof content_string;
  win.strs=strings;
  win.str_charoff=string_offsets;
  win.n_strs=1;
  GmlVM vm={0};
  vm.win=&win;
  vm.particles=gml_particle_state_create(&vm);
  if(!vm.particles || !gml_vm_software3d_ensure(&vm)) return 0;
  gml_vm_software3d_reset(&vm);
  *gml_varmap_put(&vm.globals,"state_value")=vreal(37);
  *gml_varmap_put(&vm.globals,"runtime_text")=vstr(runtime_string);
  GmlVal ordinary=gml_arr_new(2,vreal(0));
  GmlVal ordinary_child=gml_arr_new(2,vreal(0));
  gml_arr_set(ordinary_child,0,vreal(11));
  gml_arr_set(ordinary_child,1,vreal(12));
  gml_arr_set(ordinary,0,ordinary_child);
  *gml_varmap_put(&vm.globals,"ordinary_nested_array")=ordinary;
  GmlVal indexed_2d=gml_arr_new(0,vreal(0));
  gml_arr_set_2d(indexed_2d,0,1,vreal(21));
  gml_arr_set_2d(indexed_2d,1,0,vreal(22));
  *gml_varmap_put(&vm.globals,"indexed_2d_array")=indexed_2d;
  GmlRtLayer *runtime_layer=gml_rt_layer_new(&vm);
  GmlRtElem *runtime_element=gml_rt_elem_new(&vm);
  if(!runtime_layer || !runtime_element) return 0;
  runtime_element->layer=runtime_layer->id;
  int expected_rt_next_id=vm.rt_next_id;
  size_t size=gml_vm_state_size(&vm),first_size=0,second_size=0,used=0;
  void *first=malloc(size?size:1);
  void *second=malloc(size?size:1);
  int ok=first && second &&
    gml_vm_state_save(&vm,first,size,&first_size) && first_size==size &&
    gml_vm_state_save(&vm,second,size,&second_size) && second_size==first_size &&
    !memcmp(first,second,size);
  *gml_varmap_put(&vm.globals,"state_value")=vreal(-1);
  if(ok) ok=gml_vm_state_load(&vm,first,size,&used) && used==size;
  GmlVal *value=gml_varmap_get(&vm.globals,"state_value");
  GmlVal *text=gml_varmap_get(&vm.globals,"runtime_text");
  GmlVal *restored_ordinary=gml_varmap_get(&vm.globals,"ordinary_nested_array");
  GmlVal *restored_indexed_2d=gml_varmap_get(&vm.globals,"indexed_2d_array");
  GmlVal ordinary_first=restored_ordinary
    ? gml_arr_get(*restored_ordinary,0) : vreal(0);
  GmlVal indexed_flat=restored_indexed_2d
    ? gml_arr_get_2d(*restored_indexed_2d,0,1) : vreal(0);
  ok=ok && value && value->t==V_REAL && value->d==37 &&
    text && text->t==V_STR && !strcmp(text->s,runtime_string) &&
    restored_ordinary && restored_ordinary->t==V_ARR &&
    restored_ordinary->arr && !((GmlArr *)restored_ordinary->arr)->nested_2d &&
    ordinary_first.t==V_ARR && gml_val_array_length(ordinary_first)==2 &&
    gml_arr_get(ordinary_first,0).d==11 &&
    restored_indexed_2d && restored_indexed_2d->t==V_ARR &&
    restored_indexed_2d->arr && ((GmlArr *)restored_indexed_2d->arr)->nested_2d &&
    indexed_flat.t==V_REAL && indexed_flat.d==21 &&
    vm.rt_next_id==expected_rt_next_id;
  second_size=0;
  if(ok) ok=gml_vm_state_save(&vm,second,size,&second_size) &&
    second_size==first_size && !memcmp(first,second,first_size);
  vm.structs_last_gc_frame=vm.frame;
  GmlInstance *temporary=gml_struct_new(&vm);
  unsigned temporary_id=temporary?temporary->id:0;
  if(ok) ok=temporary && gml_vm_state_size(&vm)>0 &&
    !gml_struct_find(&vm,temporary_id);
  if(!ok){
    size_t common=first_size<second_size?first_size:second_size;
    size_t first_difference=0;
    while(first_difference<common &&
          ((const uint8_t *)first)[first_difference]==
          ((const uint8_t *)second)[first_difference])
      first_difference++;
    fprintf(stderr,
      "canonical VM state fixture failed: measured=%zu first=%zu restored=%zu "
      "loaded=%zu first-difference=%zu\n",
      size,first_size,second_size,used,first_difference);
    size_t context_end=first_difference+32<common?first_difference+32:common;
    fputs("  first:   ",stderr);
    for(size_t offset=first_difference;offset<context_end;offset++)
      fprintf(stderr,"%02x",((const uint8_t *)first)[offset]);
    fputs("\n  restored:",stderr);
    for(size_t offset=first_difference;offset<context_end;offset++)
      fprintf(stderr,"%02x",((const uint8_t *)second)[offset]);
    fputc('\n',stderr);
  }
  free(second);
  free(first);
  gml_vm_free(&vm);
  free(win.str_hix);
  return ok;
}
