/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_vm.c — normalized GML bytecode interpreter. See gml_vm.h. */
#include "gml_vm.h"
#include "gml_vm_internal.h"
#include "gml_value_internal.h"
#include "anygm_compatibility.h"
#include "gml_builtin.h"
#include "gml_render.h"
#include "gml_audio.h"
#include "gml_particle.h"
#include "anygm_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <ctype.h>

int gml_real_compare_epsilon(double lhs, double rhs, int cmp, double epsilon){
  int order;
  if(isnan(lhs) || isnan(rhs)) return cmp==CMP_NEQ;
  if(epsilon<0.0) epsilon=0.0;
  double delta=lhs-rhs;
  order=fabs(delta)<=epsilon ? 0 : (delta<0.0 ? -1 : 1);
  switch(cmp){
    case CMP_LT: return order<0;
    case CMP_LTE: return order<=0;
    case CMP_EQ: return order==0;
    case CMP_NEQ: return order!=0;
    case CMP_GTE: return order>=0;
    case CMP_GT: return order>0;
    default: return 0;
  }
}
int gml_real_compare(double lhs, double rhs, int cmp, int classic){
  return gml_real_compare_epsilon(lhs,rhs,cmp,classic?1e-13:1e-5);
}

/* Generic cheat/debug primitive used by the host cheat interface and development probe.
 * Applies one command line to the VM:
 *   "room=N"      one-shot: queue a transition to room index N (like room_goto(N))
 *   "name=V"      set global 'name'[0] = V   (numeric V)
 *   "name[i]=V"   set global 'name'[i] = V
 * Returns 1 for a "sticky" global write the caller should re-apply every frame (to freeze it),
 * 0 for a one-shot (room warp) or a parse failure. */
#define GML_CHEAT_NAMECH(c) (((c)>='a'&&(c)<='z')||((c)>='A'&&(c)<='Z')||((c)>='0'&&(c)<='9')||(c)=='_')
int gml_cheat_apply(GmlVM *vm, const char *code){
  if(!vm || !code) return 0;
  while(*code==' '||*code=='\t') code++;
  const char *n=code; while(*code && GML_CHEAT_NAMECH(*code)) code++;
  int nlen=(int)(code-n);
  if(nlen<=0 || nlen>=64) return 0;
  char name[64]; memcpy(name,n,(size_t)nlen); name[nlen]=0;
  int idx=0;
  if(*code=='['){ code++; idx=atoi(code); while(*code && *code!=']') code++; if(*code==']') code++; }
  while(*code==' ') code++;
  if(*code!='=') return 0;
  code++; while(*code==' ') code++;
  double val=atof(code);
  if(!strcmp(name,"room")){
    int target=(int)val;
    gml_vm_warm_audio_for_room(vm,target);
    vm->pending_room=target;
    return 0; }   /* one-shot warp (room index) */
  if(idx<0) idx=0;
  gml_set_global_arr(vm,name,idx,val);
  return 1;   /* sticky: re-apply each frame to freeze */
}

/* The input API stores keyboard remapping as physical/source VK -> logical/destination VK.
 * The host exposes physical state by VK, so aggregate every source that currently maps
 * to the requested logical key. Aggregating current/previous state before testing an edge is
 * important when several physical keys map to the same logical key. */
static void keyboard_source_state(GmlVM *vm,int source, int *cur, int *prev){
  int c=gml_input_key(vm,source,0);
  int pressed=gml_input_key(vm,source,1);
  int released=gml_input_key(vm,source,2);
  int p=released ? 1 : (pressed ? 0 : c);
  if(cur) *cur=c;
  if(prev) *prev=p;
}
void gml_keyboard_unset_map(GmlVM *vm){
  if(!vm) return;
  for(int i=0;i<256;i++) vm->key_map[i]=(int16_t)i;
}
void gml_keyboard_set_map(GmlVM *vm, int source, int destination){
  if(!vm || source<0 || source>255 || destination < -1 || destination>255) return;
  vm->key_map[source]=(int16_t)destination;
  if(anygm_host_development_setting(vm->host,"GML_LOG_KEYMAP")){
    
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[keymap] f%ld source=%d destination=%d\n",vm->frame,source,destination);
  }
}
int gml_keyboard_get_map(GmlVM *vm, int source){
  if(!vm || source<0 || source>255) return source;
  return vm->key_map[source];
}
int gml_keyboard_check(GmlVM *vm, int vk, int edge){
  if(!vm) return 0;
  if(edge<0 || edge>2) edge=0;
  int any_cur=0, any_prev=0;
  if(vk==0 || vk==1){
    for(int source=2;source<256;source++){
      if(vm->key_map[source]<2) continue; /* disabled and sentinel destinations are not keys */
      int cur=0,prev=0;
      keyboard_source_state(vm,source,&cur,&prev);
      any_cur|=cur; any_prev|=prev;
    }
    if(vk==0){ any_cur=!any_cur; any_prev=!any_prev; }
  } else {
    if(vk<0 || vk>255) return 0;
    for(int source=2;source<256;source++) if(vm->key_map[source]==vk){
      int cur=0,prev=0;
      keyboard_source_state(vm,source,&cur,&prev);
      any_cur|=cur; any_prev|=prev;
    }
  }
  return edge==1 ? (any_cur && !any_prev) : edge==2 ? (!any_cur && any_prev) : any_cur;
}
void gml_keyboard_clear(GmlVM *vm, int logical_vk){
  if(!vm) return;
  if(logical_vk==0) return;
  for(int source=2;source<256;source++)
    if((logical_vk==1 && vm->key_map[source]>=2) || vm->key_map[source]==logical_vk)
      gml_input_key_clear(vm,source);
}

GmlSoftware3D *gml_vm_software3d_ensure(GmlVM *vm){
  if(!vm) return NULL;
  if(!vm->software3d) vm->software3d=gml_software3d_create();
  if(vm->software3d && vm->render)
    gml_render_bind_software3d((GmlRender*)vm->render,vm->software3d);
  return vm->software3d;
}
void gml_vm_software3d_reset(GmlVM *vm){
  gml_software3d_reset(gml_vm_software3d_ensure(vm));
}
void gml_vm_software3d_state_get(
  GmlVM *vm,
  int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT],
  double values[GML_SOFTWARE3D_STATE_VALUE_COUNT],
  uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT]){
  gml_software3d_state_get(gml_vm_software3d_ensure(vm),flags,values,colors);
}
void gml_vm_software3d_state_set(
  GmlVM *vm,
  const int flags[GML_SOFTWARE3D_STATE_FLAG_COUNT],
  const double values[GML_SOFTWARE3D_STATE_VALUE_COUNT],
  const uint32_t colors[GML_SOFTWARE3D_STATE_COLOR_COUNT]){
  gml_software3d_state_set(gml_vm_software3d_ensure(vm),flags,values,colors);
}

void gml_vm_set_launch_parameters(GmlVM *vm,const char *executable,const char *parameters){
  if(!vm) return;
  snprintf(vm->parameter_executable,sizeof vm->parameter_executable,"%s",
           executable?executable:"");
  memset(vm->parameter_value,0,sizeof vm->parameter_value);
  vm->parameter_count=0;
  const char *cursor=parameters?parameters:"";
  while(*cursor && vm->parameter_count<GML_PARAMETER_COUNT_MAX){
    while(isspace((unsigned char)*cursor)) cursor++;
    if(!*cursor) break;
    char *output=vm->parameter_value[vm->parameter_count];
    size_t written=0;
    int quote=0;
    while(*cursor){
      unsigned char c=(unsigned char)*cursor;
      if(!quote && isspace(c)) break;
      cursor++;
      if((c=='\'' || c=='"')){
        if(!quote){ quote=c; continue; }
        if(quote==c){ quote=0; continue; }
      }
      if(c=='\\' && *cursor && ((quote && *cursor==quote) || *cursor=='\\'))
        c=(unsigned char)*cursor++;
      if(written+1<GML_PARAMETER_TEXT_MAX) output[written++]=(char)c;
    }
    output[written]='\0';
    vm->parameter_count++;
    while(isspace((unsigned char)*cursor)) cursor++;
  }
}

int gml_vm_init_launch(GmlVM *vm,GmlWin *win,const AnygmHostServices *host,
                       const char *program_directory,const char *executable,
                       const char *parameters){
  memset(vm,0,sizeof(*vm));
  /* Attach services before startup bytecode runs. Startup is part of VM initialization and may
   * legitimately query clocks, entropy, or other host capabilities. */
  vm->host=host;
  vm->diagnostics.hot_builtin_profile=-1;
  vm->diagnostics.pc_offset=-1;
  vm->diagnostics.function_value_debug=-1;
  vm->diagnostics.push_reference_debug=-1;
  vm->diagnostics.event_time_enabled=-1;
  vm->diagnostics.event_profile_last_frame=-1;
  vm->diagnostics.audio_room_warm_disabled=-1;
  vm->diagnostics.audio_room_warm_debug=-1;
  vm->diagnostics.collision_candidate_debug=-1;
  vm->diagnostics.collision_candidate_last_frame=-1;
  vm->diagnostics.collision_event_time_enabled=-1;
  vm->diagnostics.rng_call_logging=-1;
  vm->diagnostics.state_variable_debug=-1;
  vm->diagnostics.state_variable_load_debug=-1;
  vm->particles=gml_particle_state_create(vm);
  if(!vm->particles) return 1;
  vm->software3d=gml_software3d_create();
  if(!vm->software3d){
    gml_particle_state_destroy(vm->particles); vm->particles=NULL;
    return 1;
  }
  vm->builtins=gml_builtin_state_create(vm);
  if(!vm->builtins){
    gml_software3d_destroy(vm->software3d); vm->software3d=NULL;
    gml_particle_state_destroy(vm->particles); vm->particles=NULL;
    return 1;
  }
  gml_keyboard_unset_map(vm);
  gml_vm_software3d_reset(vm);
  vm->cg_built_frame=-1;   /* memset leaves 0, which would collide with vm->frame==0 at boot */
  gml_vm_instances_reset_caches(vm); /* cache slots belong to this VM */
  vm->win=win; vm->pending_room=-1; vm->room_index=-1; vm->next_id=100000; vm->rng_state=0;
  vm->time_sample_frame=-1;
  snprintf(vm->working_directory,sizeof vm->working_directory,"%s/",win?win->content_dir:"");
  snprintf(vm->program_directory,sizeof vm->program_directory,"%s/",
           program_directory&&program_directory[0]?program_directory:(win?win->content_dir:""));
  gml_vm_set_launch_parameters(vm,executable,parameters);
  vm->cur_code_index=-1;
  vm->code_static_count=(win && win->n_code>0)?win->n_code:0;
  vm->code_static=vm->code_static_count?calloc((size_t)vm->code_static_count,sizeof(*vm->code_static)):NULL;
  vm->code_static_init=vm->code_static_count?calloc((size_t)vm->code_static_count,1):NULL;
  if(vm->code_static_count && (!vm->code_static || !vm->code_static_init)){
    free(vm->code_static); free(vm->code_static_init);
    vm->code_static=NULL; vm->code_static_init=NULL; vm->code_static_count=0;
    gml_builtin_state_destroy(vm->builtins); vm->builtins=NULL;
    gml_software3d_destroy(vm->software3d); vm->software3d=NULL;
    gml_particle_state_destroy(vm->particles); vm->particles=NULL;
    return 1;
  }
  snprintf(vm->os_language,sizeof vm->os_language,"en");
  snprintf(vm->os_region,sizeof vm->os_region,"us");
  snprintf(vm->language_tag,sizeof vm->language_tag,"en-US");
  vm->rng_classic_state=0;
  vm->math_epsilon=anygm_policy_default_comparison_epsilon(win);
  vm->potential_max_rotation=30; vm->potential_rotate_step=10;
  vm->potential_check_distance=3; vm->potential_rotate_on_spot=1;
  /* A negative cursor_sprite selects the platform cursor. Frontends may hide that cursor in
   * fullscreen, but an authored non-negative sprite is rendered by the core at presentation time. */
  gml_set_global_scalar(vm,"cursor_sprite",-1);
  /* GM6-8 exposes these as writable built-in variables. Keeping the defaults in the ordinary
   * global map lets compiled source read/write them without a presentation-specific lookup. */
  if(win && anygm_policy_uses_classic_runtime(win)){
    gml_set_global_scalar(vm,"transition_kind",0);
    gml_set_global_scalar(vm,"transition_steps",80);
  }
  vm->room_state_count=gml_room_count(win);
  vm->room_stored=calloc((size_t)(vm->room_state_count>0?vm->room_state_count:1),1);
  /* GM assigns dynamic instance ids ABOVE every room-placed id in the project. Seeding the
   * counter per-room lets an early dynamic (often persistent) instance take an id that a LATER
   * room uses for a placed instance — two live instances then share an id and every id lookup
   * (with(), StackTop writes, and End-of-Path event refetch) can resolve to the wrong one. Seed the
   * dynamic range above every placed id in the complete project. */
  { const GmlChunk *rc=gml_chunk(win,"ROOM");
    if(rc){ const uint8_t *d=win->data; uint32_t nr=gml_vm_read_u32_le(d,rc->off);
      for(uint32_t ri=0;ri<nr;ri++){
        uint32_t rp=gml_vm_read_u32_le(d,rc->off+4+ri*4); if(!rp) continue;
        GmlRoom r; if(gml_room_get(win,(int)ri,&r)!=0 || !r.obj_ptr) continue;
        uint32_t cnt=gml_vm_read_u32_le(d,r.obj_ptr);
        for(uint32_t i=0;i<cnt;i++){
          uint32_t ip=gml_vm_read_u32_le(d,r.obj_ptr+4+i*4); if(!ip) continue;
          uint32_t rid=gml_vm_read_u32_le(d,ip+12);
          if(rid>=vm->next_id && rid<0x40000000u) vm->next_id=rid+1;
        }
      }
    } }
  { /* Optional initial seed for deterministic cross-run fidelity captures. Games that call
     * randomize() still use GML_RANDOMIZE_SEED at that call site; without this variable the
     * runtime uses the normal default seed. */
    const char *fixed=anygm_host_development_setting(vm->host,"GML_RNG_SEED");
    uint32_t seed=fixed ? (uint32_t)strtoll(fixed,NULL,10) : 0u;
    gml_rng_seed(vm,seed);
  }
  gml_vm_instances_parse_objects(vm);
  gml_vm_rooms_init(vm);
  gml_vm_instances_parse_boundary_events(vm);
  free(vm->obj_alive); vm->obj_alive=calloc(vm->n_objects?vm->n_objects:1,sizeof(int));
  free(vm->obj_head); vm->obj_head=malloc((vm->n_objects?vm->n_objects:1)*sizeof(int));
  if(vm->obj_head) for(int o=0;o<vm->n_objects;o++) vm->obj_head[o]=-1;
  free(vm->inst_next); free(vm->inst_prev);
  { int pc=vm->inst_cap>0?vm->inst_cap:16384;
    vm->inst_next=malloc(pc*sizeof(int)); vm->inst_prev=malloc(pc*sizeof(int));
    if(vm->inst_next&&vm->inst_prev) for(int i=0;i<pc;i++){ vm->inst_next[i]=vm->inst_prev[i]=-1; }
    else { free(vm->obj_head); vm->obj_head=NULL; } }
  gml_vm_instances_parse_dispatch_events(vm);
  vm->col_pair_cache_cap=16384;
  vm->col_pair_cache=calloc((size_t)vm->col_pair_cache_cap,sizeof(GmlColPairCache));
  vm->event_cache_cap=32768;
  vm->event_cache=calloc((size_t)vm->event_cache_cap,sizeof(GmlEventCache));
  vm->inst_cap=16384; vm->inst=calloc(vm->inst_cap,sizeof(GmlInstance)); /* fixed pool: never realloc-move */
  /* Classic action libraries can carry creation code executed once before the
   * first room. The compiler emits one reserved CODE entry only when such code
   * exists; ordinary packages take this fast miss. */
  {
    int ci=gml_code_index_by_name(win,"gml_GlobalScript___gmlc_classic_startup");
    if(ci>=0){
      GmlInstance scratch;
      memset(&scratch,0,sizeof(scratch));
      scratch.active=1; scratch.obj=-1; scratch.id=0;
      scratch.image_xscale=scratch.image_yscale=1; scratch.image_alpha=1;
      scratch.sprite_index=-1; scratch.mask_index=-1; scratch.path_index=-1;
      scratch.timeline_index=-1; scratch.timeline_speed=1;
      for(int a=0;a<GML_ALARMS;a++) scratch.alarm[a]=-1;
      GmlVal result=gml_vm_run_code(vm,ci,&scratch,NULL,NULL,0);
      if(result.t==V_STR && result.d!=0) free((char*)result.s);
      gml_varmap_free_ex(&scratch.vars,0);
    }
  }
  return 0;
}

int gml_vm_init(GmlVM *vm,GmlWin *win,const AnygmHostServices *host){
  return gml_vm_init_launch(vm,win,host,NULL,NULL,NULL);
}
static void gml_vm_release_builtin_value(void *userdata,GmlVal value){
  gml_val_free((GmlValueFreeContext *)userdata,value);
}

void gml_vm_free(GmlVM *vm){
  GML_VM_DIAGNOSTIC_DESTROY(vm);
  gml_colgrid_invalidate(vm);
  gml_vm_instances_reset_caches(vm);
  if(vm->obj_desc){ for(int i=0;i<vm->n_objects;i++) free(vm->obj_desc[i]); }
  free(vm->obj_desc); vm->obj_desc=NULL; free(vm->obj_desc_n); vm->obj_desc_n=NULL;
  free(vm->obj_alive); vm->obj_alive=NULL;
  free(vm->obj_head); vm->obj_head=NULL; free(vm->inst_next); vm->inst_next=NULL; free(vm->inst_prev); vm->inst_prev=NULL;
  free(vm->event_ord); vm->event_ord=NULL; vm->event_ord_cap=0;
  free(vm->special_var_hash); vm->special_var_hash=NULL; vm->special_var_bloom=0;
  free(vm->step_free); vm->step_free=NULL;
  vm->step_free_n=vm->step_free_pos=vm->step_free_cap=0;
  free(vm->cg_off); vm->cg_off=NULL; vm->cg_off_cap=0;
  free(vm->cg_items); vm->cg_items=NULL; vm->cg_items_cap=0;
  free(vm->cg_overlay); vm->cg_overlay=NULL; vm->cg_overlay_cap=vm->cg_overlay_n=0;
  free(vm->cg_candidate); vm->cg_candidate=NULL; vm->cg_candidate_cap=0;
  free(vm->cg_candidate_bits); vm->cg_candidate_bits=NULL; vm->cg_candidate_bits_words=0;
  free(vm->draw_ord); vm->draw_ord=NULL; vm->draw_ord_cap=0;
  gml_vm_frame_cleanup(vm);
  GmlValueFreeContext free_context={0};
  gml_value_free_context_begin(&free_context);
  gml_varmap_free_with_context(&vm->globals,0,&free_context);
  for(int i=0;i<vm->code_static_count;i++) gml_varmap_free_with_context(&vm->code_static[i],0,&free_context);
  for(int i=0;i<vm->inst_count;i++) gml_varmap_free_with_context(&vm->inst[i].vars,0,&free_context);
  for(int i=0;i<vm->n_structs;i++) if(vm->structs[i]) gml_varmap_free_with_context(&vm->structs[i]->vars,0,&free_context);
  gml_builtin_state_take_owned_values(vm->builtins,
                                      gml_vm_release_builtin_value,
                                      &free_context);
  gml_value_free_context_end(&free_context);
  free(vm->code_static); free(vm->code_static_init);
  vm->code_static=NULL; vm->code_static_init=NULL; vm->code_static_count=0;
  free(vm->state_sort_slots); vm->state_sort_slots=NULL; vm->state_sort_slots_capacity=0;
  for(int i=0;i<vm->n_structs;i++) free(vm->structs[i]);
  free(vm->structs); free(vm->struct_gen); free(vm->struct_free);
  free(vm->inst);
  for(int i=0;i<vm->n_objects;i++) free(vm->objects[i].events);
  for(int i=0;i<vm->n_paths;i++) free(vm->paths[i].pts);
  free(vm->paths);
  for(int i=0;i<vm->n_timelines;i++){
    free(vm->timelines[i].moments);
    free(vm->timelines[i].owned_name);
  }
  free(vm->timelines);
  free(vm->objects); free(vm->col_events); free(vm->col_pair_cache); free(vm->event_cache);
  gml_vm_rooms_clear_tilemaps(vm);
  free(vm->rtl); free(vm->rte); free(vm->view_ovr); free(vm->tilemaps);
  free(vm->room_stored); vm->room_stored=NULL; vm->room_state_count=0;
  free(vm->audio_room_warm_scan); vm->audio_room_warm_scan=NULL; vm->audio_room_warm_scan_n=0;
  gml_particle_state_destroy(vm->particles); vm->particles=NULL;
  gml_builtin_state_destroy(vm->builtins); vm->builtins=NULL;
  if(vm->render) gml_render_bind_software3d((GmlRender*)vm->render,NULL);
  gml_software3d_destroy(vm->software3d); vm->software3d=NULL;
}
