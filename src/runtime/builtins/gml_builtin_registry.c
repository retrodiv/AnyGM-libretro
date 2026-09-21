/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Exact builtin ID resolution and direct cached dispatch. */
#include "gml_builtin_internal.h"
#include "gml_builtin_registry.h"
#include "gml_builtin_registry_index.h"
#include "anygm_host.h"
#include "gml_particle.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/*
 * Preserve the established unconditional per-dispatch context ensure as the
 * same direct VM operation. Exact lookup changes routing only; it does not
 * move graphics-context lifetime into the registry.
 */
#define graphics_state_for_vm gml_vm_software3d_ensure

#if defined(__GNUC__) || defined(__clang__)
/*
 * Keep the two moved hot entries on the code-layout characteristics verified
 * by the paired 101-sample boundary probe. These local aliases annotate the
 * unchanged definitions below and do not alter their C implementation text.
 */
#define gml_builtin_fast_id \
  __attribute__((aligned(64))) gml_builtin_fast_id
#define gml_builtin_call_fast_id \
  __attribute__((hot)) gml_builtin_call_fast_id
#endif

typedef struct GmlBuiltinRegistryEntry {
  const char *name;
  unsigned short id;
  unsigned char owner;
  unsigned char stage;
  unsigned char cache;
} GmlBuiltinRegistryEntry;

#define GML_BUILTIN_REGISTRY_ENTRY(value,id,name,owner,stage,cache) \
  {name,BID_##id,GML_BUILTIN_OWNER_##owner,GML_BUILTIN_STAGE_##stage, \
   GML_BUILTIN_CACHE_##cache},
#define GML_BUILTIN_REGISTRY_ALIAS(id,name,owner,stage,cache) \
  {name,BID_##id,GML_BUILTIN_OWNER_##owner,GML_BUILTIN_STAGE_##stage, \
   GML_BUILTIN_CACHE_##cache},
static const GmlBuiltinRegistryEntry gml_builtin_exact_registry[]={
  GML_BUILTIN_EXACT_REGISTRY(
    GML_BUILTIN_REGISTRY_ENTRY,GML_BUILTIN_REGISTRY_ALIAS
  )
};
#undef GML_BUILTIN_REGISTRY_ALIAS
#undef GML_BUILTIN_REGISTRY_ENTRY

_Static_assert(
  sizeof(gml_builtin_exact_registry)/sizeof(gml_builtin_exact_registry[0])==
    GML_BUILTIN_REGISTRY_ENTRY_COUNT,
  "generated exact builtin index differs from the canonical registry"
);
_Static_assert(
  (GML_BUILTIN_REGISTRY_SLOT_COUNT&
   (GML_BUILTIN_REGISTRY_SLOT_COUNT-1u))==0,
  "exact builtin hash table must have a power-of-two slot count"
);

#define GML_BUILTIN_STAGE_ENTRY(value,id,name,owner,stage,cache) \
  [BID_##id]=GML_BUILTIN_STAGE_##stage,
#define GML_BUILTIN_STAGE_ALIAS(id,name,owner,stage,cache)
static const unsigned char gml_builtin_id_stage[GML_BUILTIN_ID_LIMIT]={
  GML_BUILTIN_EXACT_REGISTRY(
    GML_BUILTIN_STAGE_ENTRY,GML_BUILTIN_STAGE_ALIAS
  )
  GML_BUILTIN_DYNAMIC_REGISTRY(GML_BUILTIN_STAGE_ENTRY)
};
#undef GML_BUILTIN_STAGE_ALIAS
#undef GML_BUILTIN_STAGE_ENTRY

static uint32_t gml_builtin_name_hash(const char *name){
  uint32_t value=UINT32_C(2166136261);
  const unsigned char *cursor=(const unsigned char*)name;
  while(cursor[0]){
    value^=cursor[0];
    cursor++;
    value*=UINT32_C(16777619);
  }
  return value;
}

static const GmlBuiltinRegistryEntry *gml_builtin_exact_entry(
  const char *name
){
  unsigned slot=gml_builtin_name_hash(name)&
                (GML_BUILTIN_REGISTRY_SLOT_COUNT-1u);
  for(unsigned probe=0;probe<GML_BUILTIN_REGISTRY_SLOT_COUNT;probe++){
    unsigned entry=gml_builtin_registry_slots[slot];
    if(!entry) return NULL;
    const GmlBuiltinRegistryEntry *candidate=
      &gml_builtin_exact_registry[entry-1u];
    if(!strcmp(candidate->name,name)) return candidate;
    slot=(slot+1u)&(GML_BUILTIN_REGISTRY_SLOT_COUNT-1u);
  }
  return NULL;
}

/* A payload's own script shadows a builtin of the same name. The reference runner binds a call to
 * the content's own script first and consults an extension or a builtin only when the payload has
 * no such script, so a builtin that answers in its place silently replaces the content's logic
 * with the core's: a payload script that asks every voice to stop was measured never running,
 * because a same-named core feature claimed the name and did something else entirely. The answer
 * is per payload and per name, so one code-table probe is paid once for each builtin the payload
 * actually redefines. */
static const GmlWin *gml_builtin_shadow_win=NULL;
static signed char gml_builtin_shadow[GML_BUILTIN_ID_LIMIT];

static int gml_builtin_shadowed_by_payload(GmlVM *vm,int id,const char *name){
  if(!vm || !vm->win || !name || id<=0 || id>=GML_BUILTIN_ID_LIMIT) return 0;
  if(gml_builtin_shadow_win!=vm->win){
    gml_builtin_shadow_win=vm->win;
    memset(gml_builtin_shadow,-1,sizeof gml_builtin_shadow);
  }
  if(gml_builtin_shadow[id]<0){
    char code_name[192];
    int written=snprintf(code_name,sizeof code_name,"gml_Script_%s",name);
    gml_builtin_shadow[id]=(written>0 && written<(int)sizeof code_name &&
                            gml_code_index_by_name(vm->win,code_name)>=0)?1:0;
  }
  return gml_builtin_shadow[id];
}

/* The payload's own code index for a name it redefines, or -1 when the builtin is the only
 * definition. Callers use this to send the call to the payload's script instead of answering it
 * themselves. */
int gml_builtin_payload_shadow_script(GmlVM *vm,const char *nm){
  const GmlBuiltinRegistryEntry *entry=gml_builtin_exact_entry(nm);
  if(!entry || !gml_builtin_shadowed_by_payload(vm,entry->id,nm)) return -1;
  char code_name[192];
  int written=snprintf(code_name,sizeof code_name,"gml_Script_%s",nm);
  if(written<=0 || written>=(int)sizeof code_name) return -1;
  return gml_code_index_by_name(vm->win,code_name);
}

int gml_builtin_fast_id(GmlVM *vm,const char *nm){
  if(!nm || !*nm) return -1;
  const GmlBuiltinRegistryEntry *entry=gml_builtin_exact_entry(nm);
  if(entry){
    if(gml_builtin_shadowed_by_payload(vm,entry->id,nm)) return -1;
    switch((GmlBuiltinCachePolicy)entry->cache){
      case GML_BUILTIN_CACHE_ALWAYS:
        return entry->id;
      case GML_BUILTIN_CACHE_DS_LOG_DISABLED:
        return log_ds_on(vm)?-1:entry->id;
      case GML_BUILTIN_CACHE_NEVER:
        return -1;
    }
    return -1;
  }
  /* Prefix and dynamic fallbacks deliberately remain outside the exact table. */
  if(nm[0]=='f' && !strncmp(nm,"fmod_",5)) return BID_FMOD_PREFIX;
  if(nm[0]=='g' && !strncmp(nm,"gamepad_",8)) return BID_INPUT_KBGP;
  return -1;
}

static GmlVal gml_builtin_call_registered_stage(
  GmlVM *vm,GmlBuiltinDispatchStage stage,
  const char *nm,GmlVal *a,int n
){
  switch(stage){
    case GML_BUILTIN_STAGE_COLLISION:
      return gml_builtin_try_collision(vm,nm,a,n);
    case GML_BUILTIN_STAGE_VALUES_MATH:
      return gml_builtin_try_values_math(vm,nm,a,n);
    case GML_BUILTIN_STAGE_COLLISION_PLANNING:
      return gml_builtin_try_collision_planning(vm,nm,a,n);
    case GML_BUILTIN_STAGE_VALUES_STRINGS:
      return gml_builtin_try_values_strings(vm,nm,a,n);
    case GML_BUILTIN_STAGE_IO_INI:
      return gml_builtin_try_io_ini(vm,nm,a,n);
    case GML_BUILTIN_STAGE_INSTANCES_ROOMS:
      return gml_builtin_try_instances_rooms(vm,nm,a,n);
    case GML_BUILTIN_STAGE_ACTIONS:
      return gml_builtin_try_actions(vm,nm,a,n);
    case GML_BUILTIN_STAGE_INSTANCES:
      return gml_builtin_try_instances(vm,nm,a,n);
    case GML_BUILTIN_STAGE_VALUES_LANGUAGE:
      return gml_builtin_try_values_language(vm,nm,a,n);
    case GML_BUILTIN_STAGE_INSTANCES_DESTROY:
      return gml_builtin_try_instances_destroy(vm,nm,a,n);
    case GML_BUILTIN_STAGE_ACTIONS_LEGACY:
      return gml_builtin_try_actions_legacy(vm,nm,a,n);
    case GML_BUILTIN_STAGE_INSTANCES_QUERIES:
      return gml_builtin_try_instances_queries(vm,nm,a,n);
    case GML_BUILTIN_STAGE_PARTICLES:
      return gml_builtin_try_particles(vm,nm,a,n);
    case GML_BUILTIN_STAGE_DRAW:
      return gml_builtin_try_draw(vm,nm,a,n);
    case GML_BUILTIN_STAGE_INPUT:
      return gml_builtin_try_input(vm,nm,a,n);
    case GML_BUILTIN_STAGE_IO:
      return gml_builtin_try_io(vm,nm,a,n);
    case GML_BUILTIN_STAGE_INSTANCES_SCRIPTS:
      return gml_builtin_try_instances_scripts(vm,nm,a,n);
    case GML_BUILTIN_STAGE_INSTANCES_EVENTS:
      return gml_builtin_try_instances_scripts(vm,nm,a,n);
    case GML_BUILTIN_STAGE_AUDIO:
      return gml_builtin_try_audio(vm,nm,a,n);
    case GML_BUILTIN_STAGE_INSTANCES_PATHS:
      return gml_builtin_try_instances_paths(vm,nm,a,n);
    case GML_BUILTIN_STAGE_INSTANCES_TIMELINES:
      return gml_builtin_try_instances_paths(vm,nm,a,n);
    case GML_BUILTIN_STAGE_LAYERS_EARLY:
      return gml_builtin_try_layers_early(vm,nm,a,n);
    case GML_BUILTIN_STAGE_PLATFORM:
      return gml_builtin_try_platform(vm,nm,a,n);
    case GML_BUILTIN_STAGE_DS:
      return gml_builtin_try_ds(vm,nm,a,n);
    case GML_BUILTIN_STAGE_JSON:
      return gml_builtin_try_json(vm,nm,a,n);
    case GML_BUILTIN_STAGE_VALUES_VARIABLES:
      return gml_builtin_try_values_variables(vm,nm,a,n);
    case GML_BUILTIN_STAGE_DRAW_3D:
      return gml_builtin_try_draw_3d(vm,nm,a,n);
    case GML_BUILTIN_STAGE_ANIMATION:
      return gml_builtin_try_animation(vm,nm,a,n);
    case GML_BUILTIN_STAGE_PHYSICS:
      return gml_builtin_try_physics(vm,nm,a,n);
    case GML_BUILTIN_STAGE_PLATFORM_EXTENSIONS:
      return gml_builtin_try_platform_extensions(vm,nm,a,n);
    case GML_BUILTIN_STAGE_PLATFORM_NOOPS:
      return gml_builtin_try_platform_noops(vm,nm,a,n);
    case GML_BUILTIN_STAGE_LAYERS_LATE:
      return gml_builtin_try_layers(vm,nm,a,n);
    case GML_BUILTIN_STAGE_PLATFORM_TAIL:
      return gml_builtin_try_platform_tail(vm,nm,a,n);
    case GML_BUILTIN_STAGE_FACADE:
    case GML_BUILTIN_STAGE_COUNT:
      return builtin_call_impl(vm,nm,a,n);
  }
  return builtin_call_impl(vm,nm,a,n);
}

static GmlVal gml_builtin_call_fast_id_impl(GmlVM *vm, int id, const char *nm, GmlVal *a, int n);
static GmlVal gml_builtin_call_fast_id_original(GmlVM *vm, int id, const char *nm, GmlVal *a, int n);

/* Which shape of draw a builtin name is, for the shader accounting. */
/* A name that begins with draw_ but sets state rather than putting anything on the target. These names do not represent target-writing draws. */
static int shader_account_is_draw(const char *nm){
  static const char *not_draws[]={"draw_set","draw_get","draw_enable","draw_clear","draw_flush",
                                  "draw_primitive_begin","draw_vertex","draw_texture_flush"};
  for(size_t i=0;i<sizeof not_draws/sizeof not_draws[0];i++)
    if(!strncmp(nm,not_draws[i],strlen(not_draws[i]))) return 0;
  return 1;
}

static int shader_account_kind(const char *nm){
  if(!strncmp(nm,"draw_surface",12)) return GML_RENDER_SHADER_DRAW_SURFACE;
  if(!strncmp(nm,"draw_sprite",11) || !strcmp(nm,"draw_self")) return GML_RENDER_SHADER_DRAW_SPRITE;
  if(!strncmp(nm,"draw_rectangle",14)) return GML_RENDER_SHADER_DRAW_RECT;
  if(!strncmp(nm,"draw_text",9)) return GML_RENDER_SHADER_DRAW_TEXT;
  return GML_RENDER_SHADER_DRAW_OTHER;
}

/* A draw made with a content program active is accounted by whether it reached the device, which
 * the executed counter answers: it moves only when a program ran there. */
static int shader_account_begin(GmlVM *vm,const char *nm,GmlRender **render,int *kind,uint32_t *mark){
  GmlRender *r;
  if(!vm || !nm || strncmp(nm,"draw_",5) || !shader_account_is_draw(nm)) return 0;
  if(!builtin_setting(vm,"GML_SHADER_ACCOUNT")) return 0;
  r=(GmlRender*)vm->render;
  if(!r || gml_render_shader_current(r)<0) return 0;
  *render=r;
  *kind=shader_account_kind(nm);
  *mark=gml_render_shader_executed_count(r);
  return 1;
}

GmlVal gml_builtin_call_fast_id(GmlVM *vm, int id, const char *nm, GmlVal *a, int n){
  GmlRender *account_render=NULL;
  int account_kind=0;
  uint32_t account_mark=0;
  int account=shader_account_begin(vm,nm,&account_render,&account_kind,&account_mark);
  if(account){
    int shader=gml_render_shader_current(account_render);
    GmlVal answer=gml_builtin_call_fast_id_original(vm,id,nm,a,n);
    gml_render_shader_account(account_render,shader,account_kind,
                              gml_render_shader_executed_count(account_render)!=account_mark);
    return answer;
  }
  return gml_builtin_call_fast_id_original(vm,id,nm,a,n);
}

static GmlVal gml_builtin_call_fast_id_original(GmlVM *vm, int id, const char *nm, GmlVal *a, int n){
  if(!gml_builtin_state_ensure(vm)) return vundef();
  GmlRender *R=(GmlRender*)vm->render;
  (void)graphics_state_for_vm(vm);
  if(builtin_setting(vm,"GML_LOG_CALLS_IN_TARGET")){
    /* Cover the direct cached-dispatch path while a surface target is active. */
    int tgt_=R?gml_surface_get_target(R):-1;
    if(tgt_>0){
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[call] target=%d %s/%d\n",tgt_,nm?nm:"?",n);
    }
  }
  if(R && nm && builtin_setting(vm,"GML_LOG_SURF_DELTA") && strstr(nm,"draw")){
    /* Scan only selected drawing calls while a surface target is active. */
    int before_=gml_surface_target_lit(R);
    if(before_>=0){
      GmlVal out_=gml_builtin_call_fast_id_impl(vm,id,nm,a,n);
      int after_=gml_surface_target_lit(R);
      if(after_!=before_){
        /* Report the final two arguments alongside the colour-coverage delta. */
        double a1_=n>=2?N(a,n,n-2):0.0, a0_=n>=1?N(a,n,n-1):0.0;
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
          "[delta] %s/%d lit %d -> %d (%+d) arg[%d]=%.4f arg[%d]=%.4f\n",
          nm,n,before_,after_,after_-before_,n-2,a1_,n-1,a0_);
      }
      return out_;
    }
  }
  return gml_builtin_call_fast_id_impl(vm,id,nm,a,n);
}
static GmlVal gml_builtin_call_fast_id_impl(GmlVM *vm, int id, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  switch(id){
    /* the bodies below mirror their generic-chain handlers exactly; keep both in sync */
    case BID_FMOD_PREFIX:
      return builtin_fmod(vm,nm,a,n);
    case BID_INPUT_KBGP:{
      GmlVal v; if(builtin_input_kbgp(vm,nm,a,n,&v)) return v;
      return builtin_call_impl(vm,nm,a,n); }   /* unreachable for the ids we hand out; safety net */
    case BID_EVENT_INHERITED:
      gml_event_inherited(vm); return vreal(0);
    case BID_DS_LIST_CLEAR:
      gml_ds_list_clear_direct(vm,(int)N(a,n,0)); return vreal(0);
    case BID_GPU_SET_TEXFILTER:
      builtin_set_interpolation(R,N(a,n,
        ((!strcmp(nm,"gpu_set_texfilter_ext") ||
          !strcmp(nm,"texture_set_interpolation_ext")) && n>1)?1:0)!=0.0);
      return vreal(0);
    case BID_WINDOW_HAS_FOCUS:
      return vreal(1);
    case BID_SPRITE_EXISTS:{
      int spr=(int)N(a,n,0); return vreal(R&&gml_sprite_exists(R,spr)); }
    case BID_SPRITE_GET_WIDTH:{
      GmlRenderSpriteMetrics sprite;
      return vreal(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)?sprite.width:0); }
    case BID_SPRITE_GET_HEIGHT:{
      GmlRenderSpriteMetrics sprite;
      return vreal(gml_render_sprite_metrics(R,(int)N(a,n,0),&sprite)?sprite.height:0); }
    case BID_DS_MAP_FIND_VALUE:{
      return gml_ds_map_find_value_direct(vm,(int)N(a,n,0),n>=2?a[1]:vundef(),n>=2); }
    case BID_DS_MAP_FIND_NEXT:
      return gml_ds_map_find_next_direct(vm,(int)N(a,n,0),
                                         n>=2?a[1]:vundef(),n>=2);
    case BID_DS_MAP_FIND_PREVIOUS:
      return gml_ds_map_find_previous_direct(vm,(int)N(a,n,0),
                                             n>=2?a[1]:vundef(),n>=2);
    case BID_DS_MAP_EXISTS:
      return vreal(gml_ds_map_exists_direct(vm,(int)N(a,n,0),
                                            n>=2?a[1]:vundef(),n>=2));
    case BID_DS_MAP_SIZE:
      return vreal(gml_ds_map_size_direct(vm,(int)N(a,n,0)));
    case BID_DS_MAP_EMPTY:
      return vreal(gml_ds_map_empty_direct(vm,(int)N(a,n,0)));
    case BID_DS_MAP_FIND_FIRST:
      return gml_ds_map_find_first_direct(vm,(int)N(a,n,0));
    case BID_DS_MAP_FIND_LAST:
      return gml_ds_map_find_last_direct(vm,(int)N(a,n,0));
    case BID_DS_LIST_FIND_VALUE:
      return gml_ds_list_find_value_direct(vm,(int)N(a,n,0),
                                           (int)N(a,n,1));
    case BID_DS_LIST_SIZE:
      return vreal(gml_ds_list_size_direct(vm,(int)N(a,n,0)));
    case BID_IS_ARRAY:
      return vreal(n>0 && a[0].t==V_ARR);
    case BID_IS_UNDEFINED:
      return vreal(n>0 && a[0].t==V_UNDEF);
    case BID_IS_STRING:
      return vreal(n>0 && a[0].t==V_STR);
    case BID_IS_REAL:
      return vreal(n>0 && a[0].t==V_REAL);
    case BID_STRING_ORD_AT:{
      const unsigned char*s=(const unsigned char*)S(vm,a,n,0); int idx=(int)N(a,n,1), len=(int)strlen((const char*)s);
      return vreal((idx>=1&&idx<=len)?s[idx-1]:0); }
    case BID_STRING_CHAR_AT:{
      const char*s=S(vm,a,n,0); int idx=(int)N(a,n,1), len=(int)strlen(s);
      if(idx<1||idx>len) return vstr("");
      return vstr_owned(dup_n(s+idx-1,1)); }
    case BID_STRING_LENGTH:
      return vreal((double)strlen(S(vm,a,n,0)));
    case BID_ORD:{
      const unsigned char*s=(const unsigned char*)S(vm,a,n,0); return vreal(s[0]); }
    case BID_FILE_TEXT_EOF:{
      int i=vm_file_slot(vm,(int)N(a,n,0)); if(i<0) return vreal(1);
      int c=vm_file_getc(vm,i); if(c==EOF) return vreal(1); vm_file_ungetc(vm,i,c); return vreal(0); }
    case BID_FILE_TEXT_OPEN_READ:
      return builtin_file_text_open_read(vm,a,n);
    case BID_FILE_TEXT_READ_STRING:
      return builtin_file_text_read_string(vm,a,n);
    case BID_FILE_TEXT_READLN:
      return builtin_file_text_readln(vm,a,n);
    case BID_INI_OPEN:
      return builtin_ini_open_file(vm,a,n);
    case BID_ARRAY_LENGTH:
    case BID_ARRAY_LENGTH_1D:{
      int len_=n>0?gml_val_array_length(a[0]):0;
      const char *lenset_=builtin_setting(vm,"GML_LOG_ARR_LENGTH");
      /* The host setting selects the minimum answer to report. */
      int lenmin_=lenset_?atoi(lenset_):0;
      if(lenmin_<=0) lenmin_=64;
      if(lenset_ && len_>=lenmin_){
        /* Report the answer and array metadata alongside the current code entry. */
        const GmlArr *A_=(n>0 && a[0].t==V_ARR)?(const GmlArr*)a[0].arr:NULL;
        const char *where_=(vm&&vm->win&&vm->cur_code_index>=0&&vm->cur_code_index<vm->win->n_code&&
                            vm->win->code[vm->cur_code_index].name)
                           ?vm->win->code[vm->cur_code_index].name:"";
        anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_DEBUG,
          "[arrlen] %d (len=%d is_2d=%d height2d=%d) in %s\n",
          len_,A_?A_->len:-1,A_?A_->is_2d:-1,A_?A_->height2d:-1,where_);
      }
      return vreal(len_); }
    case BID_ARRAY_GET:
      return n>1?gml_arr_get(a[0],(int)N(a,n,1)):vreal(0);
    case BID_ARRAY_SET:
      if(n>2) gml_arr_set(a[0],(int)N(a,n,1),a[2]);
      return vreal(0);
    case BID_ARRAY_CREATE:{
      int sz=n>0?(int)N(a,n,0):0;
      GmlVal fill=n>1?a[1]:vreal(0);
      return gml_arr_new(sz,fill); }
    case BID_ARRAY_PUSH:
      for(int i=1;i<n;i++) gml_arr_push(a[0],a[i]);
      return vreal(0);
    case BID_ARRAY_POP:
      return n>0?gml_arr_pop(a[0]):vreal(0);
    case BID_ARRAY_RESIZE:
      if(n>1) gml_arr_resize(a[0],(int)N(a,n,1));
      return vreal(0);
    case BID_ARRAY_COPY:
      if(n>4) gml_arr_copy(a[0],(int)N(a,n,1),a[2],(int)N(a,n,3),(int)N(a,n,4));
      return vreal(0);
    case BID_ARRAY_HEIGHT_2D:
      return vreal(n>0?gml_val_array_height_2d(a[0]):0);
    case BID_ARRAY_LENGTH_2D:
      return vreal(n>0?gml_val_array_length_2d(a[0],(int)N(a,n,1)):0);
    case BID_DRAW_SPRITE:
      if(R) gml_draw_sprite(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3));
      return vreal(0);
    case BID_DRAW_SPRITE_EXT:
      if(R) gml_draw_sprite_ext(R,(int)N(a,n,0),gml_draw_subimg(vm,N(a,n,1)),N(a,n,2),N(a,n,3),
        N(a,n,4),N(a,n,5),N(a,n,6),NU32(a,n,7),N(a,n,8));
      return vreal(0);
    case BID_DRAW_SELF:{
      if(!strcmp(nm,"draw_full_sprite") && n!=0) return vreal(0);
      GmlInstance*s=vm->cur_self;
      if(R&&s) gml_draw_sprite_ext(R,(int)s->sprite_index,
        (int)s->image_index,s->x,s->y,s->image_xscale,s->image_yscale,s->image_angle,(uint32_t)s->image_blend,
        !strcmp(nm,"draw_full_sprite")?1:s->image_alpha);
      return vreal(0); }
    case BID_DRAW_SHADOW:
      if(!strcmp(nm,"draw_shadow")){
        if(n==0) draw_legacy_sprite_shadow(vm,4,.5);
      } else if(!strcmp(nm,"draw_shadow_ext") && n==2){
        draw_legacy_sprite_shadow(vm,N(a,n,0),N(a,n,1));
      }
      return vreal(0);
    case BID_LEGACY_DEPTH_BY_Y:
      if(n==0 && vm->cur_self) vm->cur_self->depth=-(vm->cur_self->y/100.0);
      return vreal(0);
    case BID_LEGACY_CREATE:
      if(n==3) (void)gml_instance_create(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2));
      return vreal(0);
    case BID_LEGACY_MOVE_RPG:
      legacy_move_rpg(vm,a,n);
      return vreal(0);
    case BID_LEGACY_DIRECTION_RPG:
      legacy_direction_rpg(vm,a,n);
      return vreal(0);
    case BID_LEGACY_FRICTION_PLATFORM:
      legacy_friction_platform(vm,a,n);
      return vreal(0);
    case BID_LEGACY_DESTROY_SELF:
      legacy_destroy_self(vm,n);
      return vreal(0);
    case BID_DRAW_SURFACE:
      if(R){ int s=(int)N(a,n,0);
        GmlRenderDrawState draw=builtin_draw_state(R);
        GmlRenderTargetMetrics target=builtin_target_metrics(R);
        if(s>0 && s==(int)gml_global_arr(vm,"view_surface_id",0) && N(a,n,1)==0 && N(a,n,2)==0)
          gml_draw_surface_stretched(R,s,
            gml_render_gui_logical_x(R,target.camera_x),gml_render_gui_logical_y(R,target.camera_y),
            gml_render_gui_logical_width(R),gml_render_gui_logical_height(R),0xFFFFFF,draw.alpha);
        else
          gml_draw_surface_stretched(R,s,N(a,n,1),N(a,n,2),gml_surface_width(R,s),gml_surface_height(R,s),0xFFFFFF,draw.alpha);
      }
      return vreal(0);
    case BID_DRAW_SURFACE_EXT:
      if(R) gml_draw_surface_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),
                                N(a,n,5),NU32(a,n,6),N(a,n,7));
      return vreal(0);
    case BID_DRAW_SURFACE_STRETCHED:
      if(R){ GmlRenderDrawState draw=builtin_draw_state(R);
        gml_draw_surface_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),0xFFFFFF,draw.alpha); }
      return vreal(0);
    case BID_DRAW_SURFACE_STRETCHED_EXT:
      if(R) gml_draw_surface_stretched(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),NU32(a,n,5),N(a,n,6));
      return vreal(0);
    case BID_DRAW_SURFACE_PART_EXT:
      if(R) gml_draw_surface_part_ext(R,(int)N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),N(a,n,8),NU32(a,n,9),N(a,n,10));
      return vreal(0);
    case BID_DRAW_RECTANGLE_COLOR:
    case BID_DRAW_RECTANGLE_COLOUR:
      if(R){ int outline=(int)N(a,n,8);
        GmlRenderTargetMetrics target=builtin_target_metrics(R);
        int x1=(int)floor(draw_gui_x(R,N(a,n,0))-target.camera_x), y1=(int)floor(draw_gui_y(R,N(a,n,1))-target.camera_y);
        int x2=(int)ceil(draw_gui_x(R,N(a,n,2))-target.camera_x), y2=(int)ceil(draw_gui_y(R,N(a,n,3))-target.camera_y);
        gml_render_primitive_rectangle_color(R,x1,y1,x2,y2,NU32(a,n,4),NU32(a,n,5),NU32(a,n,6),NU32(a,n,7),outline);
      }
      return vreal(0);
    case BID_DRAW_RECTANGLE:
      if(R){
        GmlRenderTargetMetrics target=builtin_target_metrics(R);
        GmlRenderDrawState draw=builtin_draw_state(R);
        uint32_t color=draw.color;
        int outline=(int)N(a,n,4);
        (void)builtin_classic_hollow_rectangle(vm,n,&color,&outline);
        int x1=(int)floor(draw_gui_x(R,N(a,n,0))-target.camera_x), y1=(int)floor(draw_gui_y(R,N(a,n,1))-target.camera_y);
        int x2=(int)ceil(draw_gui_x(R,N(a,n,2))-target.camera_x), y2=(int)ceil(draw_gui_y(R,N(a,n,3))-target.camera_y);
        gml_render_primitive_rectangle(R,x1,y1,x2,y2,color,outline);
      }
      return vreal(0);
    case BID_DRAW_SET_COLOR:
      builtin_set_draw_color(R,NU32(a,n,0));
      return vreal(0);
    case BID_DRAW_TEXT:
      if(R) gml_draw_text(R,N(a,n,0),N(a,n,1),S(vm,a,n,2));
      return vreal(0);
    case BID_DRAW_TEXT_EXT:
      if(R) gml_draw_text_ext(R,N(a,n,0),N(a,n,1),S(vm,a,n,2),N(a,n,3),N(a,n,4));
      return vreal(0);
    case BID_DRAW_TEXT_SPRITE:
      if(R) gml_draw_text_sprite(R,N(a,n,0),N(a,n,1),S(vm,a,n,2),N(a,n,3),N(a,n,4),
                                 (int)N(a,n,5),(int)N(a,n,6),N(a,n,7));
      return vreal(0);
    case BID_DRAW_TEXT_EXT_TRANSFORMED_COLOUR:
    case BID_DRAW_TEXT_EXT_TRANSFORMED_COLOR:
      if(R) gml_draw_text_ext_transformed(R,N(a,n,0),N(a,n,1),S(vm,a,n,2),N(a,n,3),N(a,n,4),N(a,n,5),N(a,n,6),N(a,n,7),NU32(a,n,8),N(a,n,12));
      return vreal(0);
    case BID_DRAW_SET_ALPHA:
      builtin_set_draw_alpha(R,N(a,n,0));
      return vreal(0);
    case BID_DRAW_SET_FONT:
      builtin_set_draw_font(vm,R,(int)N(a,n,0));
      return vreal(0);
    case BID_DRAW_SET_HALIGN:
      builtin_set_draw_halign(R,(int)N(a,n,0));
      return vreal(0);
    case BID_DRAW_SET_VALIGN:
      builtin_set_draw_valign(R,(int)N(a,n,0));
      return vreal(0);
    case BID_GPU_SET_BLENDENABLE:
      builtin_set_alpha_blend(R,N(a,n,0)>=0.5);
      return vreal(0);
    case BID_GPU_SET_BLENDMODE:
      if(builtin_setting(vm,"GML_DBG_BM"))
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
          "[bm] f%ld gpu_set_blendmode(%d)\n",vm->frame,(int)N(a,n,0));
      builtin_set_blendmode(R,(int)N(a,n,0));
      return vreal(0);
    case BID_GPU_SET_BLENDMODE_EXT:
      builtin_set_blendmode_ext(vm,R,(int)N(a,n,0),(int)N(a,n,1));
      return vreal(0);
    case BID_SHADER_SET:
      gml_render_shader_set_current(R,(int)N(a,n,0));
      if(builtin_setting(vm,"GML_LOG_SHADER")){ 
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[shader] f%ld cached shader_set(%d)\n",vm->frame,(int)N(a,n,0)); }
      return vreal(0);
    case BID_SHADER_RESET:
      gml_render_shader_set_current(R,-1);
      if(builtin_setting(vm,"GML_LOG_SHADER")){ 
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[shader] f%ld cached shader_reset()\n",vm->frame); }
      return vreal(0);
    case BID_SHADER_GET_UNIFORM:{
      int sh=(int)N(a,n,0); const char *un=S(vm,a,n,1);
      return vreal(gml_shader_get_uniform(R,sh,un)); }
    case BID_SHADER_SET_UNIFORM_F:
    case BID_SHADER_SET_UNIFORM_F_ARRAY:
      if(R) gml_shader_set_uniform_f(R,(int)N(a,n,0),a,n);
      return vreal(0);
    case BID_PART_SYSTEM_DRAWIT:
    case BID_PART_SYSTEM_DRAWIT_EXT:
      if(R) gml_part_system_drawit(vm->particles,R,(int)N(a,n,0));
      return vreal(0);
    case BID_PLACE_MEETING:{
      int r=collision_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),0);
      if(log_col_on(vm)){ GmlInstance*cs=vm->cur_self; const char*cn=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
        const char*tn=((int)N(a,n,2)>=0&&(int)N(a,n,2)<vm->n_objects)?vm->objects[(int)N(a,n,2)].name:"?";
        if(log_col_match(vm,cn)) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[col] %s place_meeting(%.0f,%.0f,%s)=%d\n",cn,N(a,n,0),N(a,n,1),tn,r); }
      return vreal(r); }
    case BID_INSTANCE_EXISTS:
      return vreal(gml_instance_number(vm,(int)N(a,n,0))>0);
    case BID_INSTANCE_NUMBER:
      return vreal(gml_instance_number(vm,(int)N(a,n,0)));
    case BID_INSTANCE_PLACE_LIST:{
      GmlDSList *l=ds_list_slot_repair(vm,(int)N(a,n,3));
      int r=collision_instance_list_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),l,N(a,n,4)>=0.5);
      if(log_col_on(vm)){ GmlInstance*cs=vm->cur_self; const char*cnm=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
        const char*tn=((int)N(a,n,2)>=0&&(int)N(a,n,2)<vm->n_objects)?vm->objects[(int)N(a,n,2)].name:"?";
        if(log_col_match(vm,cnm)) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[col] %s instance_place_list(%.0f,%.0f,%s)=%d\n",cnm,N(a,n,0),N(a,n,1),tn,r); }
      return vreal(r); }
    case BID_SIN:
      return vreal(sin(N(a,n,0)));
    case BID_DISTANCE_TO_OBJECT:{
      return vreal(distance_to_target(vm,vm->cur_self,(int)N(a,n,0))); }
    case BID_FLOOR:
      return vreal(floor(N(a,n,0)));
    case BID_FRAC:{
      double v=N(a,n,0); return vreal(v-floor(v)); }
    case BID_ABS:
      return vreal(fabs(N(a,n,0)));
    case BID_MIN:{
      if(n<=0) return vreal(0);
      double v=N(a,n,0);
      for(int i=1;i<n;i++){ double x=N(a,n,i); if(x<v) v=x; }
      return vreal(v); }
    case BID_MAX:{
      if(n<=0) return vreal(0);
      double v=N(a,n,0);
      for(int i=1;i<n;i++){ double x=N(a,n,i); if(x>v) v=x; }
      return vreal(v); }
    case BID_CLAMP:{
      double x=N(a,n,0), lo=N(a,n,1), hi=N(a,n,2);
      if(x<lo)x=lo;
      if(x>hi)x=hi;
      return vreal(x); }
    case BID_LENGTHDIR_X:
      return vreal(N(a,n,0)*cos(N(a,n,1)*M_PI/180.0));
    case BID_LENGTHDIR_Y:
      return vreal(-N(a,n,0)*sin(N(a,n,1)*M_PI/180.0));
    case BID_POINT_DISTANCE:
      return vreal(hypot(N(a,n,2)-N(a,n,0),N(a,n,3)-N(a,n,1)));
    case BID_POINT_DIRECTION:{
      double dx=N(a,n,2)-N(a,n,0), dy=N(a,n,3)-N(a,n,1);
      double r=atan2(-dy,dx)*180.0/M_PI; if(r<0)r+=360; return vreal(r); }
    case BID_KEYBOARD_CHECK:
      return vreal(gml_keyboard_check(vm,(int)N(a,n,0),0));
    case BID_KEYBOARD_CHECK_DIRECT:
      return vreal(gml_input_key(vm,(int)N(a,n,0),0));
    case BID_KEYBOARD_CHECK_PRESSED:
      return vreal(gml_keyboard_check(vm,(int)N(a,n,0),1));
    case BID_KEYBOARD_CHECK_RELEASED:
      return vreal(gml_keyboard_check(vm,(int)N(a,n,0),2));
    case BID_KEYBOARD_CLEAR:
      gml_keyboard_clear(vm,(int)N(a,n,0));
      return vreal(0);
    case BID_KEYBOARD_KEY_PRESS:
      gml_input_key_press(vm,(int)N(a,n,0));
      return vreal(0);
    case BID_KEYBOARD_KEY_RELEASE:
      gml_input_key_release(vm,(int)N(a,n,0));
      return vreal(0);
    case BID_GAMEPAD_BUTTON_CHECK:
      if(gp_debug_on(vm)){ 
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gp] f%ld gamepad_button_check n=%d a0=%.0f a1=%.0f -> %d\n",vm->frame,n,N(a,n,0),N(a,n,1),gml_input_gamepad(vm,(int)N(a,n,0),(int)N(a,n,1),0)); }
      return vreal(gml_input_gamepad(vm,(int)N(a,n,0),(int)N(a,n,1),0));
    case BID_GAMEPAD_BUTTON_VALUE:
      if(gp_debug_on(vm)){ 
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gp] f%ld gamepad_button_value n=%d a0=%.0f a1=%.0f -> %d\n",vm->frame,n,N(a,n,0),N(a,n,1),gml_input_gamepad(vm,(int)N(a,n,0),(int)N(a,n,1),0)); }
      return vreal(gml_input_gamepad(vm,(int)N(a,n,0),(int)N(a,n,1),0) ? 1.0 : 0.0);
    case BID_GAMEPAD_BUTTON_CHECK_PRESSED:
      if(gp_debug_on(vm)){ 
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gp] f%ld gamepad_button_check_pressed n=%d a0=%.0f a1=%.0f -> %d\n",vm->frame,n,N(a,n,0),N(a,n,1),gml_input_gamepad(vm,(int)N(a,n,0),(int)N(a,n,1),1)); }
      return vreal(gml_input_gamepad(vm,(int)N(a,n,0),(int)N(a,n,1),1));
    case BID_GAMEPAD_BUTTON_CHECK_RELEASED:
      if(gp_debug_on(vm)){ 
        anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gp] f%ld gamepad_button_check_released n=%d a0=%.0f a1=%.0f -> %d\n",vm->frame,n,N(a,n,0),N(a,n,1),gml_input_gamepad(vm,(int)N(a,n,0),(int)N(a,n,1),2)); }
      return vreal(gml_input_gamepad(vm,(int)N(a,n,0),(int)N(a,n,1),2));
    case BID_GAMEPAD_IS_CONNECTED:
      return vreal(gml_input_gamepad_connected(vm,(int)N(a,n,0)));
    case BID_GAMEPAD_IS_SUPPORTED:
      return vreal(1);
    case BID_GAMEPAD_GET_DEVICE_COUNT:
      return vreal(gml_input_gamepad_device_count(vm));
    case BID_GAMEPAD_BUTTON_COUNT:
      return vreal(16);
    case BID_GAMEPAD_AXIS_COUNT:
      return vreal(4);
    case BID_GAMEPAD_SET_AXIS_DEADZONE:
      gp_deadzone_set(vm,(int)N(a,n,0),N(a,n,1));
      return vreal(0);
    case BID_GAMEPAD_SET_VIBRATION:
      gml_input_gamepad_set_vibration(vm,(int)N(a,n,0),N(a,n,1),N(a,n,2));
      return vreal(0);
    case BID_GAMEPAD_AXIS_VALUE:
      return vreal(gp_axis_value_filtered(vm,(int)N(a,n,0),(int)N(a,n,1)));
    case BID_MOUSE_CHECK_BUTTON:
      return vreal(mouse_btn_check(vm,(int)N(a,n,0),0));
    case BID_MOUSE_CHECK_BUTTON_PRESSED:
      return vreal(mouse_btn_check(vm,(int)N(a,n,0),1));
    case BID_MOUSE_CHECK_BUTTON_RELEASED:
      return vreal(mouse_btn_check(vm,(int)N(a,n,0),2));
    case BID_DEVICE_MOUSE_CHECK_BUTTON:
      return vreal(mouse_btn_check(vm,(int)N(a,n,1),0));
    case BID_DEVICE_MOUSE_CHECK_BUTTON_PRESSED:
      return vreal(mouse_btn_check(vm,(int)N(a,n,1),1));
    case BID_DEVICE_MOUSE_CHECK_BUTTON_RELEASED:
      return vreal(mouse_btn_check(vm,(int)N(a,n,1),2));
    case BID_DEVICE_MOUSE_X:{
      double v; gml_input_mouse(vm,&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_DEVICE_MOUSE_Y:{
      double v; gml_input_mouse(vm,NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_DEVICE_MOUSE_X_TO_GUI:{
      double v; gml_input_mouse(vm,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_DEVICE_MOUSE_Y_TO_GUI:{
      double v; gml_input_mouse(vm,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_DEVICE_MOUSE_RAW_X:{
      double v; gml_input_mouse(vm,NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_DEVICE_MOUSE_RAW_Y:{
      double v; gml_input_mouse(vm,NULL,NULL,NULL,NULL,NULL,&v,NULL,NULL,NULL,NULL); return vreal(v); }
    case BID_WINDOW_MOUSE_SET:
      return vreal(0);
    case BID_DISPLAY_GET_WIDTH:
      return vreal(display_size(vm,R,0));
    case BID_DISPLAY_GET_HEIGHT:
      return vreal(display_size(vm,R,1));
    case BID_WINDOW_GET_WIDTH:
      return vreal(presentation_size(vm,R,0));
    case BID_WINDOW_GET_HEIGHT:
      return vreal(presentation_size(vm,R,1));
    case BID_DISPLAY_GET_GUI_WIDTH:
      { GmlRenderTargetMetrics target=builtin_target_metrics(R);
        return vreal(vm->gui_w>0?vm->gui_w:(target.width>0?target.width:(vm->win&&vm->win->disp_w?(int)vm->win->disp_w:288))); }
    case BID_DISPLAY_GET_GUI_HEIGHT:
      { GmlRenderTargetMetrics target=builtin_target_metrics(R);
        return vreal(vm->gui_h>0?vm->gui_h:(target.height>0?target.height:(vm->win&&vm->win->disp_h?(int)vm->win->disp_h:216))); }
    case BID_SURFACE_EXISTS:
      return vreal(R?gml_surface_exists(R,(int)N(a,n,0)):0);
    case BID_SURFACE_CREATE:
      return vreal(R?gml_surface_create(R,(int)N(a,n,0),(int)N(a,n,1)):-1);
    case BID_SURFACE_FREE:
      if(R) gml_surface_free(R,(int)N(a,n,0));
      return vreal(0);
    case BID_SURFACE_GET_TEXTURE:{
      int sid=(int)N(a,n,0);
      return vreal((R&&gml_surface_exists(R,sid))?
        gml_render_surface_texture_handle(sid):-1); }
    case BID_SURFACE_GET_WIDTH:
      return vreal(R?gml_surface_width(R,(int)N(a,n,0)):0);
    case BID_SURFACE_GET_HEIGHT:
      return vreal(R?gml_surface_height(R,(int)N(a,n,0)):0);
    case BID_SURFACE_SET_TARGET:
      if(builtin_setting(vm,"GML_LOG_SURF")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[surf] set_target %d\n",(int)N(a,n,0));
      return vreal(R?gml_surface_set_target(R,(int)N(a,n,0)):0);
    case BID_SURFACE_RESET_TARGET:
      if(builtin_setting(vm,"GML_LOG_SURF")) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[surf] reset_target\n");
      if(R) gml_surface_reset_target(R);
      return vreal(0);
    case BID_TEXTURE_GET_TEXEL_WIDTH:{
      double tw; return vreal(texture_info(R,(int)N(a,n,0),NULL,NULL,&tw,NULL)?tw:0); }
    case BID_TEXTURE_GET_TEXEL_HEIGHT:{
      double th; return vreal(texture_info(R,(int)N(a,n,0),NULL,NULL,NULL,&th)?th:0); }
    case BID_STRING_WIDTH:
      return vreal(R?gml_text_width(R,S(vm,a,n,0)):(int)strlen(S(vm,a,n,0))*8);
    case BID_STRING_HEIGHT:
      return vreal(R?gml_text_height(R,S(vm,a,n,0)):8);
    default:
      if(id>0 && id<GML_BUILTIN_ID_LIMIT)
        return gml_builtin_call_registered_stage(
          vm,(GmlBuiltinDispatchStage)gml_builtin_id_stage[id],nm,a,n
        );
      return builtin_call_impl(vm,nm,a,n);
  }
}
