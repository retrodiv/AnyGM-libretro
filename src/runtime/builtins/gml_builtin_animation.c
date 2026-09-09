/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Animation-curve lookup and ordered animation/platform fallback dispatch. */
#include "gml_builtin_internal.h"
#include "gml_render.h"
#include "anygm_host.h"

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ACRV curve records contain inline channels and points. Evaluate linearly between knots and clamp at the ends. */
static float acrv_f32(const uint8_t *d, uint32_t o){ float f; memcpy(&f,d+o,4); return f; }
static int acrv_bounds(GmlVM *vm,size_t *begin,size_t *end){
  if(!vm || !vm->win || !vm->win->data) return 0;
  const GmlChunk *chunk=gml_chunk(vm->win,"ACRV");
  if(!chunk || chunk->off>vm->win->size || chunk->size<8 ||
     chunk->size>vm->win->size-chunk->off) return 0;
  *begin=chunk->off; *end=*begin+chunk->size;
  return 1;
}
static uint32_t acrv_curve_ptr(GmlVM *vm, int idx){
  size_t begin,end;
  if(idx<0 || !acrv_bounds(vm,&begin,&end)) return 0;
  const uint8_t *d=vm->win->data; uint32_t n=u32(d,begin+4);
  if(n>(end-begin-8)/4 || (uint32_t)idx>=n) return 0;
  uint32_t cp=u32(d,begin+8+(size_t)idx*4);
  if(cp<begin+8+(size_t)n*4 || cp>end || end-cp<12) return 0;
  if(u32(d,cp+8)>(end-cp-12)/16) return 0;
  return cp;
}
static int acrv_channel_span(GmlVM *vm,size_t position,size_t end,size_t *next){
  if(position>end || end-position<16) return 0;
  uint32_t count=u32(vm->win->data,position+12);
  if(count>(end-position-16)/24) return 0;
  *next=position+16+(size_t)count*24;
  return 1;
}
static uint32_t acrv_channel_ptr(GmlVM *vm, int curve, int ch){
  uint32_t cp=acrv_curve_ptr(vm,curve); if(!cp) return 0;
  const uint8_t *d=vm->win->data; uint32_t nch=u32(d,cp+8);
  if(ch<0||(uint32_t)ch>=nch) return 0;
  size_t begin,end,p=(size_t)cp+12,next;
  if(!acrv_bounds(vm,&begin,&end)) return 0;
  for(int i=0;i<=ch;i++){
    if(!acrv_channel_span(vm,p,end,&next)) return 0;
    if(i==ch) return (uint32_t)p;
    p=next;
  }
  return 0;
}
static int acrv_channel_named(GmlVM *vm,int curve,const char *name){
  uint32_t cp=acrv_curve_ptr(vm,curve);
  size_t begin,end;
  if(!cp || !name || !acrv_bounds(vm,&begin,&end)) return -1;
  const uint8_t *data=vm->win->data;
  uint32_t count=u32(data,cp+8);
  size_t position=(size_t)cp+12,length=strlen(name);
  for(uint32_t i=0;i<count;i++){
    size_t next;
    if(!acrv_channel_span(vm,position,end,&next)) return -1;
    uint32_t pointer=u32(data,position);
    if(pointer<4 || pointer>=vm->win->size) return -1;
    uint32_t bytes=u32(data,pointer-4);
    if(bytes>=vm->win->size-pointer || data[(size_t)pointer+bytes]!=0) return -1;
    if(bytes==length && !memcmp(data+pointer,name,length)) return (int)i;
    position=next;
  }
  return -1;
}
static GmlInstance *acrv_struct(GmlVM *vm,GmlVal value){
  if(!vm || value.t!=V_REAL || !GML_IS_STRUCT_ID(value.d) ||
     floor(value.d)!=value.d) return NULL;
  return gml_struct_find(vm,(unsigned)value.d);
}
static GmlVal acrv_channel_index(GmlVM *vm,GmlVal *args,int count){
  int index=-1;
  if(args && count==2 && args[0].t==V_REAL && args[1].t==V_STR && args[1].s){
    double id=args[0].d;
    GmlInstance *curve=acrv_struct(vm,args[0]);
    if(curve){
      GmlVal *channels=gml_varmap_get(&curve->vars,"channels");
      int length=channels?gml_val_array_length(*channels):0;
      for(int i=0;i<length;i++){
        GmlInstance *channel=acrv_struct(vm,gml_arr_get(*channels,i));
        GmlVal *name=channel?gml_varmap_get(&channel->vars,"name"):NULL;
        if(name && name->t==V_STR && name->s && !strcmp(name->s,args[1].s)){
          index=i; break;
        }
      }
    }else if(isfinite(id) && id>=0 && id<=INT_MAX && floor(id)==id)
      index=acrv_channel_named(vm,(int)id,args[1].s);
  }
  if(index>=0) return vreal(index);
  /* Invalid queries have no usable index. Full language exception transport is not available. */
  anygm_host_logf(vm?vm->host:NULL,ANYGM_LOG_ERROR,
                 "animcurve_get_channel_index: invalid curve or channel query\n");
  return vundef();
}
#define GML_ACRV_TAG 0x52000000
static double acrv_evaluate(GmlVM *vm, int curve, int ch, double x){
  uint32_t p=acrv_channel_ptr(vm,curve,ch); if(!p) return 0;
  const uint8_t *d=vm->win->data; uint32_t npts=u32(d,p+12);
  if(!npts) return 0;
  uint32_t pt=p+16;
  double x0=acrv_f32(d,pt), y0=acrv_f32(d,pt+4);
  if(npts==1 || x<=x0) return y0;
  for(uint32_t i=1;i<npts;i++){
    double x1=acrv_f32(d,pt+i*24), y1=acrv_f32(d,pt+i*24+4);
    if(x<=x1){ double t=(x1>x0)?(x-x0)/(x1-x0):0; return y0+(y1-y0)*t; }
    x0=x1; y0=y1;
  }
  return acrv_f32(d,pt+(npts-1)*24+4);   /* past the last knot */
}
static void inst_store_val(GmlInstance *in, const char *key, GmlVal v){
  if(!in || !key) return;
  GmlVal *p=gml_varmap_get(&in->vars,key);
  if(p){ *p=v; return; }
  char *owned=strdup(key);
  if(!owned) return;
  *gml_varmap_put(&in->vars,owned)=v;
}
static void inst_store_real(GmlInstance *in, const char *key, double v){
  inst_store_val(in,key,vreal(v));
}
static void inst_store_string(GmlInstance *in, const char *key, const char *s){
  char *copy=strdup(s?s:"");
  if(!copy) return;
  inst_store_val(in,key,vstr(copy));
}
static GmlVal inst_lookup(GmlInstance *in, const char *key){
  GmlVal *p=(in&&key)?gml_varmap_get(&in->vars,key):NULL;
  return p?*p:vundef();
}
static int skel_key(char *out, size_t cap, const char *kind, const char *name, const char *field){
  int n=snprintf(out,cap,"__skel_%s:%s:%s",kind?kind:"",name?name:"",field?field:"");
  return n>=0 && (size_t)n<cap;
}
static double skel_bone_num(GmlInstance *in, const char *kind, const char *bone, const char *field, double def){
  char key[384];
  if(!skel_key(key,sizeof(key),kind,bone,field)) return def;
  GmlVal v=inst_lookup(in,key);
  if(v.t==V_UNDEF) return def;
  return N(&v,1,0);
}
static const char *skel_bone_str(GmlInstance *in, const char *kind, const char *bone, const char *field, const char *def){
  char key[384];
  if(!skel_key(key,sizeof(key),kind,bone,field)) return def;
  GmlVal v=inst_lookup(in,key);
  return v.t==V_STR ? (v.s?v.s:"") : def;
}
static int skel_bone_has(GmlInstance *in, const char *kind, const char *bone, const char *field){
  char key[384];
  return skel_key(key,sizeof(key),kind,bone,field) && inst_lookup(in,key).t!=V_UNDEF;
}
static void skel_store_map_num(GmlVM *vm, GmlInstance *in, int mapid,
                               const char *kind, const char *bone, const char *field, double def){
  int ok=0;
  GmlVal mv=ds_map_lookup_s(vm,mapid,field,&ok);
  double v=ok?N(&mv,1,0):skel_bone_num(in,kind,bone,field,def);
  char key[384];
  if(skel_key(key,sizeof(key),kind,bone,field)) inst_store_real(in,key,v);
}
static void skel_store_map_str(GmlVM *vm, GmlInstance *in, int mapid,
                               const char *kind, const char *bone, const char *field, const char *def){
  int ok=0;
  GmlVal mv=ds_map_lookup_s(vm,mapid,field,&ok);
  const char *v=(ok && mv.t==V_STR) ? (mv.s?mv.s:"") : skel_bone_str(in,kind,bone,field,def);
  char key[384];
  if(skel_key(key,sizeof(key),kind,bone,field)) inst_store_string(in,key,v);
}
static double skel_bone_setup(GmlVM *vm,GmlInstance *in,const char *kind,
                              const char *bone,const char *field,double fallback){
  if(skel_bone_has(in,kind,bone,field))
    return skel_bone_num(in,kind,bone,field,fallback);
  GmlRender *render=vm?(GmlRender*)vm->render:NULL;
  double value=fallback;
  if(render && gml_render_skeleton_bone_setup(
       render,(int)in->sprite_index,bone,field,&value)) return value;
  return fallback;
}
static void skel_bone_map_put_base(GmlVM *vm, GmlInstance *in, int mapid,
                                   const char *kind, const char *bone){
  ds_map_put(vm,mapid,vstr("x"),vreal(skel_bone_setup(vm,in,kind,bone,"x",0)),1);
  ds_map_put(vm,mapid,vstr("y"),vreal(skel_bone_setup(vm,in,kind,bone,"y",0)),1);
  ds_map_put(vm,mapid,vstr("angle"),vreal(skel_bone_setup(vm,in,kind,bone,"angle",0)),1);
  ds_map_put(vm,mapid,vstr("xscale"),vreal(skel_bone_setup(vm,in,kind,bone,"xscale",1)),1);
  ds_map_put(vm,mapid,vstr("yscale"),vreal(skel_bone_setup(vm,in,kind,bone,"yscale",1)),1);
  ds_map_put(vm,mapid,vstr("parent"),vstr(skel_bone_str(in,kind,bone,"parent","")),1);
}
GmlVal builtin_skeleton(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlInstance *in=vm?vm->cur_self:NULL;
  GmlBuiltinState *state=builtin_state_ensure(vm);
  if(builtin_setting(vm,"GML_LOG_SKEL")){
    if(!state || state->skeleton_log_count<256){
      
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[skel] f%ld self=%u %s",vm->frame,in?in->id:0,nm);
      for(int i=0;i<n && i<3;i++){
        if(a[i].t==V_STR) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"%s\"%s\"",i?" ":" ",a[i].s?a[i].s:"");
        else anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"%s%g",i?" ":" ",N(a,n,i));
      }
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"\n");
    }
    if(state) state->skeleton_log_count++;
  }
  if(!strcmp(nm,"skeleton_animation_set")){
    if(in){
      const char *next=S(vm,a,n,0);
      GmlVal current=inst_lookup(in,"__skel_animation");
      if(current.t!=V_STR || strcmp(current.s?current.s:"",next))
        inst_store_real(in,"__skel_time",0);
      inst_store_string(in,"__skel_animation",next);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_animation_get")){
    GmlVal v=inst_lookup(in,"__skel_animation");
    return v.t==V_STR ? vstr(v.s?v.s:"") : vstr("");
  }
  if(!strcmp(nm,"skeleton_animation_mix")){
    if(in){
      char key[384];
      int k=snprintf(key,sizeof(key),"__skel_mix:%s:%s",S(vm,a,n,0),S(vm,a,n,1));
      if(k>=0 && (size_t)k<sizeof(key)) inst_store_real(in,key,N(a,n,2));
    }
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_skin_set")){
    if(in) inst_store_string(in,"__skel_skin",S(vm,a,n,0));
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_skin_get")){
    GmlVal v=inst_lookup(in,"__skel_skin");
    return v.t==V_STR ? vstr(v.s?v.s:"") : vstr("");
  }
  if(!strcmp(nm,"skeleton_attachment_set")){
    if(in){
      char key[384];
      int k=snprintf(key,sizeof(key),"__skel_attachment:%s",S(vm,a,n,0));
      if(k>=0 && (size_t)k<sizeof(key)) inst_store_string(in,key,S(vm,a,n,1));
    }
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_attachment_get")){
    char key[384];
    int k=snprintf(key,sizeof(key),"__skel_attachment:%s",S(vm,a,n,0));
    GmlVal v=(k>=0 && (size_t)k<sizeof(key))?inst_lookup(in,key):vundef();
    return v.t==V_STR ? vstr(v.s?v.s:"") : vstr("");
  }
  if(!strcmp(nm,"skeleton_bone_data_get")){
    if(in && n>=2) skel_bone_map_put_base(vm,in,(int)N(a,n,1),"bone_data",S(vm,a,n,0));
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_bone_data_set")){
    if(in && n>=2){
      int mapid=(int)N(a,n,1);
      const char *bone=S(vm,a,n,0);
      skel_store_map_num(vm,in,mapid,"bone_data",bone,"x",0);
      skel_store_map_num(vm,in,mapid,"bone_data",bone,"y",0);
      skel_store_map_num(vm,in,mapid,"bone_data",bone,"angle",0);
      skel_store_map_num(vm,in,mapid,"bone_data",bone,"xscale",1);
      skel_store_map_num(vm,in,mapid,"bone_data",bone,"yscale",1);
      skel_store_map_str(vm,in,mapid,"bone_data",bone,"parent","");
    }
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_bone_state_get")){
    if(in && n>=2){
      int mapid=(int)N(a,n,1);
      const char *bone=S(vm,a,n,0);
      if(!skel_bone_has(in,"bone_state",bone,"angle") &&
         !skel_bone_has(in,"bone_state",bone,"x") &&
         !skel_bone_has(in,"bone_state",bone,"y"))
        return vreal(0);
      double x=skel_bone_num(in,"bone_state",bone,"x",skel_bone_num(in,"bone_data",bone,"x",0));
      double y=skel_bone_num(in,"bone_state",bone,"y",skel_bone_num(in,"bone_data",bone,"y",0));
      double angle=skel_bone_num(in,"bone_state",bone,"angle",skel_bone_num(in,"bone_data",bone,"angle",0));
      double xs=skel_bone_num(in,"bone_state",bone,"xscale",skel_bone_num(in,"bone_data",bone,"xscale",1));
      double ys=skel_bone_num(in,"bone_state",bone,"yscale",skel_bone_num(in,"bone_data",bone,"yscale",1));
      ds_map_put(vm,mapid,vstr("x"),vreal(x),1);
      ds_map_put(vm,mapid,vstr("y"),vreal(y),1);
      ds_map_put(vm,mapid,vstr("angle"),vreal(angle),1);
      ds_map_put(vm,mapid,vstr("xscale"),vreal(xs),1);
      ds_map_put(vm,mapid,vstr("yscale"),vreal(ys),1);
      ds_map_put(vm,mapid,vstr("worldX"),vreal(x),1);
      ds_map_put(vm,mapid,vstr("worldY"),vreal(y),1);
      ds_map_put(vm,mapid,vstr("worldAngleX"),vreal(angle),1);
      ds_map_put(vm,mapid,vstr("worldAngleY"),vreal(angle),1);
      ds_map_put(vm,mapid,vstr("worldScaleX"),vreal(fabs(xs)),1);
      ds_map_put(vm,mapid,vstr("worldScaleY"),vreal(fabs(ys)),1);
      ds_map_put(vm,mapid,vstr("appliedAngle"),vreal(angle),1);
      ds_map_put(vm,mapid,vstr("parent"),vstr(skel_bone_str(in,"bone_state",bone,"parent",
        skel_bone_str(in,"bone_data",bone,"parent",""))),1);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"skeleton_bone_state_set")){
    if(in && n>=2){
      int mapid=(int)N(a,n,1);
      const char *bone=S(vm,a,n,0);
      skel_store_map_num(vm,in,mapid,"bone_state",bone,"x",skel_bone_num(in,"bone_data",bone,"x",0));
      skel_store_map_num(vm,in,mapid,"bone_state",bone,"y",skel_bone_num(in,"bone_data",bone,"y",0));
      skel_store_map_num(vm,in,mapid,"bone_state",bone,"angle",skel_bone_num(in,"bone_data",bone,"angle",0));
      skel_store_map_num(vm,in,mapid,"bone_state",bone,"xscale",skel_bone_num(in,"bone_data",bone,"xscale",1));
      skel_store_map_num(vm,in,mapid,"bone_state",bone,"yscale",skel_bone_num(in,"bone_data",bone,"yscale",1));
      skel_store_map_str(vm,in,mapid,"bone_state",bone,"parent",skel_bone_str(in,"bone_data",bone,"parent",""));
    }
    return vreal(0);
  }
  return vreal(0);
}

GmlVal gml_builtin_try_animation(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* animation curves: get_channel hands out a tagged handle; evaluate interpolates the knots. */
  if(!strcmp(nm,"animcurve_exists")) return vreal(acrv_curve_ptr(vm,(int)N(a,n,0))!=0);
  if(!strcmp(nm,"animcurve_get")) return vreal(N(a,n,0));   /* asset ref passes through */
  if(!strcmp(nm,"animcurve_get_channel_index")) return acrv_channel_index(vm,a,n);
  if(!strcmp(nm,"animcurve_get_channel")){
    int curve=(int)N(a,n,0), ch=0;
    if(n>=2 && a[1].t==V_STR){                    /* select channel by name */
      ch=acrv_channel_named(vm,curve,a[1].s);
      if(ch<0) ch=0;
    } else ch=(int)N(a,n,1);
    if(!acrv_channel_ptr(vm,curve,ch)) return vreal(-1);
    return vreal((double)(GML_ACRV_TAG | ((curve&0xFFF)<<8) | (ch&0xFF))); }
  if(!strcmp(nm,"animcurve_channel_evaluate")){
    int h=(int)N(a,n,0);
    if((h & 0x7F000000)!=GML_ACRV_TAG) return vreal(0);
    return vreal(acrv_evaluate(vm,(h>>8)&0xFFF,h&0xFF,N(a,n,1))); }
  /* The VM performs mark/sweep at safe frame boundaries. An explicit request therefore needs no
   * mid-bytecode mutation; acknowledging it preserves API behavior without risking live roots. */
  if(!strcmp(nm,"gc_collect") || !strcmp(nm,"gc_enable")) return vreal(0);
  if(!strcmp(nm,"gc_is_enabled")) return vreal(1);
  if(!strncmp(nm,"shader_",7)) return vreal(0);
  if(!strcmp(nm,"steam_current_game_language")){
    /* Report the language the core resolved (the Language core option / frontend GET_LANGUAGE,
     * the same source os_get_language reads), mapped to the lowercase names this call returns.
     * Locales without a distinct name here answer english, which is also the no-option default. */
    const char *iso=vm->os_language[0]?vm->os_language:"en";
    static const struct { const char *iso, *name; } lang[]={
      {"en","english"},{"es","spanish"},{"fr","french"},{"de","german"},{"it","italian"},
      {"ja","japanese"},{"ko","koreana"},{"zh","schinese"},{"pt","portuguese"},{"ru","russian"},
      {"nl","dutch"},{"pl","polish"},{"cs","czech"},{"fi","finnish"},{"sv","swedish"},
      {"tr","turkish"},{"uk","ukrainian"},{"el","greek"},{"ar","arabic"},{"vi","vietnamese"} };
    for(int i=0;i<(int)(sizeof lang/sizeof lang[0]);i++)
      if(!strcmp(iso,lang[i].iso)) return vstr(lang[i].name);
    return vstr("english"); }
  if(!strcmp(nm,"steam_initialised")||!strcmp(nm,"steam_initialized")) return vreal(0);
  if(!strncmp(nm,"psn_",4)) return vreal(0);
  return gml_builtin_try_physics(vm,nm,a,n);
}
