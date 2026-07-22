/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Particle system implementation (part_type / part_system / part_emitter).
 *
 * Each VM owns its pools. They update once per runtime step and draw via the existing sprite
 * blitter (gml_draw_sprite_ext) or a small clipped square for shape/pixel types. Nothing here touches
 * the core render blit paths, so content that never calls a part_* function is wholly unaffected.
 *
 * Particle pools are VM state and participate in save/load/rewind. They still reset on fresh VM start,
 * which prevents a game that re-creates its systems every room from leaking pool slots. Motion
 * follows GM: velocity from (speed,direction) + gravity, with per-frame *_incr on speed/dir/size/orient;
 * colour and alpha interpolate over the particle's life (1/2/3-key). Randomness uses a private LCG —
 * now serialized with the particle pools for deterministic rewind. */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <limits.h>
#include "gml_render.h"
#include "anygm_compatibility.h"
#include "gml_particle.h"
#include "gml_vm.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PT_MAX 256
#define PS_MAX 64
#define PE_MAX 128
#define DEG2RAD(d) ((d)*M_PI/180.0)

typedef struct {
  int used;
  int sprite, spr_animate, spr_stretch, spr_random;
  int shape;                                  /* pt_shape_* (unused for custom sprites) */
  double sz_min, sz_max, sz_incr, sz_wig;
  double xscale, yscale;
  double sp_min, sp_max, sp_incr, sp_wig;
  double dir_min, dir_max, dir_incr, dir_wig;
  double grav_amt, grav_dir;
  double life_min, life_max;
  uint32_t col[3]; int ncol;                  /* colour keys over life (GM order: BBGGRR) */
  int color_mode;                             /* 0=key colours, 1=random RGB, 2=random mix, 3=random HSV */
  uint32_t mix_a, mix_b;
  double cmin[3], cmax[3];                    /* RGB or HSV component ranges */
  double alpha[3]; int nalpha;
  double ori_min, ori_max, ori_incr, ori_wig; int ori_rel;
  int additive;
  int step_number, step_type;
  int death_number, death_type;
} PType;

typedef struct {
  double x, y, speed, dir, grav_amt, grav_dir;
  double life, life0, size, size_incr, ori, ori_incr;
  int type;
  int has_col; uint32_t col_over;   /* part_particles_create_color override (else use the type's colour) */
  int random_start;                 /* shared phase used by all four classic wiggle waves */
} Part;

typedef struct {
  int used, auto_update, auto_draw;
  double depth;
  double px, py;
  Part *parts; int n, cap;
} PSys;

typedef struct {
  int used, sys;
  double xmin, xmax, ymin, ymax;
  int shape, dist;
  int stream_type, stream_number;
} PEmit;

struct GmlParticleState {
  PType type[PT_MAX];
  PSys system[PS_MAX];
  PEmit emitter[PE_MAX];
  int effect_system[2];
  int effect_type[2][12][3];
  int effect_explosion_core[2][3];
  GmlVM *vm;
  uint32_t prng;
  uint8_t explosion_shape_mask[64*64];
  uint8_t glint_shape_mask[3][64*64];
  uint8_t snow_shape_mask[64*64];
  uint8_t ring_shape_mask[64*64];
  unsigned glint_shape_mask_ready;
  int explosion_shape_mask_ready;
  int snow_shape_mask_ready;
  int ring_shape_mask_ready;
};

#define g_pt (state->type)
#define g_ps (state->system)
#define g_pe (state->emitter)
#define g_effect_sys (state->effect_system)
#define g_effect_type (state->effect_type)
#define g_effect_explosion_core (state->effect_explosion_core)
#define g_particle_vm (state->vm)
#define g_prng (state->prng)
#define g_explosion_shape_mask (state->explosion_shape_mask)
#define g_glint_shape_mask (state->glint_shape_mask)
#define g_snow_shape_mask (state->snow_shape_mask)
#define g_ring_shape_mask (state->ring_shape_mask)
#define g_glint_shape_mask_ready (state->glint_shape_mask_ready)
#define g_explosion_shape_mask_ready (state->explosion_shape_mask_ready)
#define g_snow_shape_mask_ready (state->snow_shape_mask_ready)
#define g_ring_shape_mask_ready (state->ring_shape_mask_ready)

GmlParticleState *gml_particle_state_create(GmlVM *vm){
  GmlParticleState *state=calloc(1,sizeof(*state));
  if(!state) return NULL;
  state->vm=vm;
  state->prng=0x2545F491u;
  return state;
}
void gml_particle_state_destroy(GmlParticleState *state){
  if(!state) return;
  gml_part_reset_all(state);
  free(state);
}
static double prnd(GmlParticleState *state){
  if(g_particle_vm && anygm_policy_uses_classic_runtime(g_particle_vm->win))
    return gml_rng_value(g_particle_vm);
  g_prng = g_prng*1664525u + 1013904223u;
  return ((g_prng>>8) & 0xFFFFFF)/(double)0x1000000;
}
/* A fixed particle property is assigned directly; only a real interval samples the RNG. */
static double particle_range(GmlParticleState *state,double a,double b){ return b>a ? a+prnd(state)*(b-a) : a; }
static int clamp255(double v){ if(v<0) return 0; if(v>255) return 255; return (int)(v+0.5); }
static uint32_t rgb_col(int r,int g,int b){ return ((uint32_t)clamp255(b)<<16)|((uint32_t)clamp255(g)<<8)|(uint32_t)clamp255(r); }
static uint32_t mix_col(uint32_t a, uint32_t b, double t){
  if(t<0)t=0; if(t>1)t=1;
  double ar=a&0xFF, ag=(a>>8)&0xFF, ab=(a>>16)&0xFF;
  double br=b&0xFF, bg=(b>>8)&0xFF, bb=(b>>16)&0xFF;
  return rgb_col((int)(ar+(br-ar)*t+0.5),(int)(ag+(bg-ag)*t+0.5),(int)(ab+(bb-ab)*t+0.5));
}
static uint32_t hsv_col(double h,double s,double v){
  h=fmod(h,256.0); if(h<0) h+=256.0;
  s=clamp255(s)/255.0; v=clamp255(v)/255.0;
  double hp=h*(6.0/256.0), c=v*s, x=c*(1.0-fabs(fmod(hp,2.0)-1.0)), m=v-c;
  double r=0,g=0,b=0;
  if(hp<1){ r=c; g=x; }
  else if(hp<2){ r=x; g=c; }
  else if(hp<3){ g=c; b=x; }
  else if(hp<4){ g=x; b=c; }
  else if(hp<5){ r=x; b=c; }
  else { r=c; b=x; }
  return rgb_col((int)((r+m)*255.0+0.5),(int)((g+m)*255.0+0.5),(int)((b+m)*255.0+0.5));
}

static PType *pt(GmlParticleState *state,int id){ int i=id-1; return (state && i>=0 && i<PT_MAX && g_pt[i].used) ? &g_pt[i] : NULL; }
static PSys  *ps(GmlParticleState *state,int id){ int i=id-1; return (state && i>=0 && i<PS_MAX && g_ps[i].used) ? &g_ps[i] : NULL; }
static PEmit *pe(GmlParticleState *state,int id){ int i=id-1; return (state && i>=0 && i<PE_MAX && g_pe[i].used) ? &g_pe[i] : NULL; }

void gml_part_reset_all(GmlParticleState *state){
  if(!state) return;
  for(int i=0;i<PS_MAX;i++){ free(g_ps[i].parts); }
  memset(g_pt,0,sizeof g_pt); memset(g_ps,0,sizeof g_ps); memset(g_pe,0,sizeof g_pe);
  memset(g_effect_sys,0,sizeof g_effect_sys); memset(g_effect_type,0,sizeof g_effect_type);
  memset(g_effect_explosion_core,0,sizeof g_effect_explosion_core);
  g_prng=0x2545F491u;
}

int gml_part_type_create(GmlParticleState *state){
  if(!state) return 0;
  for(int i=0;i<PT_MAX;i++) if(!g_pt[i].used){
    PType *t=&g_pt[i]; memset(t,0,sizeof *t); t->used=1;
    t->sprite=-1; t->sz_min=t->sz_max=1; t->xscale=t->yscale=1;
    t->life_min=t->life_max=100; t->col[0]=0xFFFFFF; t->ncol=1; t->alpha[0]=1; t->nalpha=1;
    return i+1;
  }
  return 0;
}
int gml_part_type_exists(GmlParticleState *state,int id){ return pt(state,id)!=NULL; }
void gml_part_type_destroy(GmlParticleState *state,int id){ PType *t=pt(state,id); if(t){ t->used=0; } }
void gml_part_type_clear(GmlParticleState *state,int id){ PType *t=pt(state,id); if(t){ int u=t->used; memset(t,0,sizeof *t); t->used=u;
  t->sprite=-1; t->sz_min=t->sz_max=1; t->xscale=t->yscale=1; t->life_min=t->life_max=100; t->col[0]=0xFFFFFF; t->ncol=1; t->alpha[0]=1; t->nalpha=1; } }

void gml_part_type_sprite(GmlParticleState *state,int id,int spr,int animate,int stretch,int random){ PType *t=pt(state,id); if(t){ t->sprite=spr; t->spr_animate=animate; t->spr_stretch=stretch; t->spr_random=random; } }
void gml_part_type_shape(GmlParticleState *state,int id,int shape){ PType *t=pt(state,id); if(t) t->shape=shape; }
void gml_part_type_size(GmlParticleState *state,int id,double mn,double mx,double incr,double wig){ PType *t=pt(state,id); if(t){ t->sz_min=mn; t->sz_max=mx; t->sz_incr=incr; t->sz_wig=wig; } }
void gml_part_type_scale(GmlParticleState *state,int id,double xs,double ys){ PType *t=pt(state,id); if(t){ t->xscale=xs; t->yscale=ys; } }
void gml_part_type_speed(GmlParticleState *state,int id,double mn,double mx,double incr,double wig){ PType *t=pt(state,id); if(t){ t->sp_min=mn; t->sp_max=mx; t->sp_incr=incr; t->sp_wig=wig; } }
void gml_part_type_direction(GmlParticleState *state,int id,double mn,double mx,double incr,double wig){ PType *t=pt(state,id); if(t){ t->dir_min=mn; t->dir_max=mx; t->dir_incr=incr; t->dir_wig=wig; } }
void gml_part_type_gravity(GmlParticleState *state,int id,double amt,double dir){ PType *t=pt(state,id); if(t){ t->grav_amt=amt; t->grav_dir=dir; } }
void gml_part_type_life(GmlParticleState *state,int id,double mn,double mx){ PType *t=pt(state,id); if(t){ t->life_min=mn; t->life_max=mx; } }
void gml_part_type_step(GmlParticleState *state,int id,int number,int type){ PType *t=pt(state,id); if(t){ t->step_number=number; t->step_type=type; } }
void gml_part_type_death(GmlParticleState *state,int id,int number,int type){ PType *t=pt(state,id); if(t){ t->death_number=number; t->death_type=type; } }
void gml_part_type_orientation(GmlParticleState *state,int id,double mn,double mx,double incr,double wig,int rel){ PType *t=pt(state,id); if(t){ t->ori_min=mn; t->ori_max=mx; t->ori_incr=incr; t->ori_wig=wig; t->ori_rel=rel; } }
void gml_part_type_color(GmlParticleState *state,int id,int ncol,uint32_t c1,uint32_t c2,uint32_t c3){ PType *t=pt(state,id); if(t){ t->color_mode=0; t->ncol=ncol<1?1:(ncol>3?3:ncol); t->col[0]=c1; t->col[1]=c2; t->col[2]=c3; } }
void gml_part_type_color_rgb(GmlParticleState *state,int id,double rmin,double rmax,double gmin,double gmax,double bmin,double bmax){
  PType *t=pt(state,id); if(t){ t->color_mode=1; t->cmin[0]=rmin; t->cmax[0]=rmax; t->cmin[1]=gmin; t->cmax[1]=gmax; t->cmin[2]=bmin; t->cmax[2]=bmax; }
}
void gml_part_type_color_mix(GmlParticleState *state,int id,uint32_t c1,uint32_t c2){ PType *t=pt(state,id); if(t){ t->color_mode=2; t->mix_a=c1; t->mix_b=c2; } }
void gml_part_type_color_hsv(GmlParticleState *state,int id,double hmin,double hmax,double smin,double smax,double vmin,double vmax){
  PType *t=pt(state,id); if(t){ t->color_mode=3; t->cmin[0]=hmin; t->cmax[0]=hmax; t->cmin[1]=smin; t->cmax[1]=smax; t->cmin[2]=vmin; t->cmax[2]=vmax; }
}
void gml_part_type_alpha(GmlParticleState *state,int id,int na,double a1,double a2,double a3){ PType *t=pt(state,id); if(t){ t->nalpha=na<1?1:(na>3?3:na); t->alpha[0]=a1; t->alpha[1]=a2; t->alpha[2]=a3; } }
void gml_part_type_blend(GmlParticleState *state,int id,int additive){ PType *t=pt(state,id); if(t) t->additive=additive; }

int gml_part_system_create(GmlParticleState *state){
  if(!state) return 0;
  for(int i=0;i<PS_MAX;i++) if(!g_ps[i].used){ PSys *s=&g_ps[i]; memset(s,0,sizeof *s); s->used=1; s->auto_update=1; s->auto_draw=1;
    return i+1; }
  return 0;
}
int gml_part_system_exists(GmlParticleState *state,int id){ return ps(state,id)!=NULL; }
void gml_part_system_destroy(GmlParticleState *state,int id){ PSys *s=ps(state,id); if(s){ free(s->parts); memset(s,0,sizeof *s); } }
void gml_part_system_clear(GmlParticleState *state,int id){ PSys *s=ps(state,id); if(s) s->n=0; }
void gml_part_system_position(GmlParticleState *state,int id,double x,double y){ PSys *s=ps(state,id); if(s){ s->px=x; s->py=y; } }
void gml_part_system_automatic_update(GmlParticleState *state,int id,int on){ PSys *s=ps(state,id); if(s) s->auto_update=on?1:0; }
void gml_part_system_automatic_draw(GmlParticleState *state,int id,int on){ PSys *s=ps(state,id); if(s) s->auto_draw=on?1:0; }
void gml_part_system_depth(GmlParticleState *state,int id,double depth){ PSys *s=ps(state,id); if(s) s->depth=depth; }
int gml_part_system_count(GmlParticleState *state,int id){ PSys *s=ps(state,id); return s? s->n : 0; }
int gml_part_system_auto_draw_nth(GmlParticleState *state,int nth,int *id,double *depth){
  if(!state || nth<0) return 0;
  for(int i=0;i<PS_MAX;i++) if(g_ps[i].used && g_ps[i].auto_draw){
    if(nth--==0){
      if(id) *id=i+1;
      if(depth) *depth=g_ps[i].depth;
      return 1;
    }
  }
  return 0;
}

static void sys_spawn(GmlParticleState *state,PSys *s,double x,double y,int type,int number,int col){
  PType *t=pt(state,type); if(!t || number<=0) return;
  if(number>4000) number=4000;
  for(int k=0;k<number;k++){
    if(s->n>=s->cap){ int nc=s->cap? s->cap*2:64; Part *np=realloc(s->parts,(size_t)nc*sizeof(Part)); if(!np) return; s->parts=np; s->cap=nc; }
    Part *p=&s->parts[s->n++]; memset(p,0,sizeof *p);
    p->x=x; p->y=y; p->type=type;
    p->speed=particle_range(state,t->sp_min,t->sp_max);
    p->dir=particle_range(state,t->dir_min,t->dir_max);
    p->ori=particle_range(state,t->ori_min,t->ori_max); p->ori_incr=t->ori_incr;
    p->life=p->life0=floor(particle_range(state,t->life_min,t->life_max)+0.5); if(p->life<1) p->life=p->life0=1;
    if(t->color_mode==1){ p->has_col=1; p->col_over=rgb_col((int)floor(particle_range(state,t->cmin[0],t->cmax[0])+0.5),(int)floor(particle_range(state,t->cmin[1],t->cmax[1])+0.5),(int)floor(particle_range(state,t->cmin[2],t->cmax[2])+0.5)); }
    else if(t->color_mode==2){ p->has_col=1; p->col_over=mix_col(t->mix_a,t->mix_b,prnd(state)); }
    else if(t->color_mode==3){ p->has_col=1; p->col_over=hsv_col(particle_range(state,t->cmin[0],t->cmax[0]),particle_range(state,t->cmin[1],t->cmax[1]),particle_range(state,t->cmin[2],t->cmax[2])); }
    if(col>=0){ p->has_col=1; p->col_over=(uint32_t)col; }
    p->size=particle_range(state,t->sz_min,t->sz_max); p->size_incr=t->sz_incr;
    if(t->sprite>=0 && t->spr_random) (void)prnd(state);
    p->random_start=(int)floor(prnd(state)*100001.0); /* inclusive classic irandom(100000) */
    p->grav_amt=t->grav_amt; p->grav_dir=t->grav_dir;
  }
}
void gml_part_particles_create(GmlParticleState *state,int sysid,double x,double y,int type,int number){ PSys *s=ps(state,sysid); if(s) sys_spawn(state,s,x+s->px,y+s->py,type,number,-1); }
void gml_part_particles_create_color(GmlParticleState *state,int sysid,double x,double y,int type,uint32_t col,int number){ PSys *s=ps(state,sysid); if(s) sys_spawn(state,s,x+s->px,y+s->py,type,number,(int)(col&0xFFFFFF)); }

static void effect_room_metrics(GmlParticleState *state,int *width,int *height,int *speed){
  *width=640; *height=480; *speed=30;
  if(!g_particle_vm) return;
  GmlRoom room;
  if(g_particle_vm->win && gml_room_get(g_particle_vm->win,g_particle_vm->room_index,&room)==0){
    if(room.width>0) *width=room.width;
    if(room.height>0) *height=room.height;
    if(room.speed>0) *speed=room.speed;
  } else if(g_particle_vm->render){
    GmlRender *render=(GmlRender*)g_particle_vm->render;
    if(render->fbw>0) *width=render->fbw;
    if(render->fbh>0) *height=render->fbh;
  }
}

void gml_effect_create(GmlParticleState *state,int above,int kind,double x,double y,int size,uint32_t color){
  if(!state) return;
  int layer=above?1:0; if(kind<0)kind=0; if(kind>11)kind=11; if(size<0)size=0; if(size>2)size=2;
  if(!g_effect_sys[layer]){
    g_effect_sys[layer]=gml_part_system_create(state);
    gml_part_system_depth(state,g_effect_sys[layer],above?-100000.0:100000.0);
  }
  int type=g_effect_type[layer][kind][size];
  if(!type){
    type=gml_part_type_create(state); if(!type) return;
    g_effect_type[layer][kind][size]=type;
    double scale=size==0?.8:(size==1?1.6:2.8);
    gml_part_type_shape(state,type,0);
    gml_part_type_size(state,type,scale,scale*1.8,kind==6||kind==7?-.03:.02,0);
    gml_part_type_life(state,type,kind==4||kind==5?35:18,kind==4||kind==5?60:34);
    gml_part_type_alpha(state,type,3,0.0,0.9,0.0);
    gml_part_type_direction(state,type,0,360,0,0);
    if(kind==4||kind==5){ gml_part_type_speed(state,type,.2*scale,1.0*scale,-.01,0); gml_part_type_gravity(state,type,.025,90); }
    else if(kind==10){ gml_part_type_speed(state,type,4*scale,7*scale,0,0); gml_part_type_direction(state,type,250,290,0,0); }
    else if(kind==11){ gml_part_type_speed(state,type,.3*scale,1.2*scale,0,0); gml_part_type_direction(state,type,240,300,0,0); }
    else gml_part_type_speed(state,type,.5*scale,2.5*scale,-.03,0);
  }
  if(kind==0){
    PType *burst=pt(state,type); if(!burst) return;
    int width,height,speed; effect_room_metrics(state,&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double initial_size[3]={.1,.3,.4};
    static const double size_growth[3]={.05,.10,.20};
    static const double initial_speed[3]={2,4,7};
    static const double speed_decay[3]={-.10,-.18,-.20};
    static const double life_min[3]={10,12,15};
    static const double life_max[3]={15,17,20};
    burst->sprite=-1; burst->shape=10;
    burst->sz_min=burst->sz_max=initial_size[size]; burst->sz_incr=size_growth[size]*cadence; burst->sz_wig=0;
    burst->xscale=burst->yscale=1;
    burst->sp_min=burst->sp_max=initial_speed[size]*cadence; burst->sp_incr=speed_decay[size]*cadence; burst->sp_wig=0;
    burst->dir_min=0; burst->dir_max=360; burst->dir_incr=burst->dir_wig=0;
    burst->grav_amt=0; burst->grav_dir=270;
    burst->life_min=floor(life_min[size]/cadence+.5); burst->life_max=floor(life_max[size]/cadence+.5);
    burst->alpha[0]=.6; burst->alpha[1]=.3; burst->alpha[2]=0; burst->nalpha=3;
    burst->ori_min=0; burst->ori_max=360; burst->ori_incr=burst->ori_wig=0; burst->ori_rel=0;
    burst->additive=0;

    int core_id=g_effect_explosion_core[layer][size];
    if(!core_id){ core_id=gml_part_type_create(state); g_effect_explosion_core[layer][size]=core_id; }
    PType *core=pt(state,core_id); if(!core) return;
    static const double core_growth[3]={.10,.20,.40};
    static const double core_life[3]={15,17,20};
    core->sprite=-1; core->shape=10;
    core->sz_min=core->sz_max=initial_size[size]; core->sz_incr=core_growth[size]*cadence; core->sz_wig=0;
    core->xscale=core->yscale=1;
    core->sp_min=core->sp_max=core->sp_incr=core->sp_wig=0;
    core->dir_min=core->dir_max=core->dir_incr=core->dir_wig=0;
    core->grav_amt=0; core->grav_dir=270;
    core->life_min=core->life_max=floor(core_life[size]/cadence+.5);
    core->alpha[0]=.8; core->alpha[1]=.4; core->alpha[2]=0; core->nalpha=3;
    core->ori_min=0; core->ori_max=360; core->ori_incr=core->ori_wig=0; core->ori_rel=0;
    core->additive=0;
    gml_part_particles_create_color(state,g_effect_sys[layer],x,y,type,color,20);
    gml_part_particles_create_color(state,g_effect_sys[layer],x,y,core_id,0,1);
    return;
  }
  if(kind==1 || kind==2){
    PType *wave=pt(state,type); if(!wave) return;
    int width,height,speed; effect_room_metrics(state,&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double growth[2][3]={{.15,.25,.40},{.20,.35,.60}};
    static const double life_min[3]={10,13,18};
    static const double life_max[3]={12,15,20};
    wave->sprite=-1; wave->shape=6;
    wave->sz_min=wave->sz_max=0; wave->sz_incr=growth[kind-1][size]*cadence; wave->sz_wig=0;
    wave->xscale=1; wave->yscale=kind==2?.5:1;
    wave->sp_min=wave->sp_max=wave->sp_incr=wave->sp_wig=0;
    wave->dir_min=wave->dir_max=wave->dir_incr=wave->dir_wig=0;
    wave->grav_amt=0; wave->grav_dir=270;
    wave->life_min=floor(life_min[size]/cadence+.5);
    wave->life_max=floor(life_max[size]/cadence+.5);
    wave->alpha[0]=1; wave->alpha[1]=.5; wave->alpha[2]=0; wave->nalpha=3;
    wave->ori_min=wave->ori_max=wave->ori_incr=wave->ori_wig=0; wave->ori_rel=0;
    wave->additive=0;
    gml_part_particles_create_color(state,g_effect_sys[layer],x,y,type,color,1);
    return;
  }
  if(kind==3){
    PType *firework=pt(state,type); if(!firework) return;
    int width,height,speed; effect_room_metrics(state,&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double max_speed[3]={3,6,8};
    static const double life_min[3]={15,20,30};
    static const double life_max[3]={25,30,40};
    static const double gravity[3]={.10,.15,.17};
    static const int count[3]={75,150,250};
    firework->sprite=-1; firework->shape=8;
    firework->sz_min=.1; firework->sz_max=.2; firework->sz_incr=firework->sz_wig=0;
    firework->xscale=firework->yscale=1;
    firework->sp_min=.5*cadence; firework->sp_max=max_speed[size]*cadence;
    firework->sp_incr=firework->sp_wig=0;
    firework->dir_min=0; firework->dir_max=360; firework->dir_incr=firework->dir_wig=0;
    firework->grav_amt=gravity[size]; firework->grav_dir=270;
    firework->life_min=floor(life_min[size]/cadence+.5);
    firework->life_max=floor(life_max[size]/cadence+.5);
    firework->alpha[0]=1; firework->alpha[1]=.7; firework->alpha[2]=.4; firework->nalpha=3;
    firework->ori_min=firework->ori_max=firework->ori_incr=firework->ori_wig=0;
    firework->ori_rel=0; firework->additive=0;
    gml_part_particles_create_color(state,g_effect_sys[layer],x,y,type,color,count[size]);
    return;
  }
  if(kind==4 || kind==5){
    PType *smoke=pt(state,type); if(!smoke) return;
    int width,height,speed; effect_room_metrics(state,&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double min_size[3]={.2,.4,.4};
    static const double max_size[3]={.4,.7,1.0};
    static const double lifetime[3]={25,30,50};
    static const double rise_min[3]={3,5,6};
    static const double rise_max[3]={4,6,7};
    static const int count[3]={6,11,16};
    static const int spread[3]={10,30,60};
    smoke->sprite=-1; smoke->shape=10;
    smoke->sz_min=min_size[size]; smoke->sz_max=max_size[size];
    smoke->sz_incr=-.01*cadence; smoke->sz_wig=0;
    smoke->xscale=smoke->yscale=1;
    if(kind==5){
      smoke->sp_min=rise_min[size]*cadence; smoke->sp_max=rise_max[size]*cadence;
      smoke->dir_min=smoke->dir_max=90;
    } else {
      smoke->sp_min=smoke->sp_max=0;
      smoke->dir_min=smoke->dir_max=0;
    }
    smoke->sp_incr=smoke->sp_wig=0;
    smoke->dir_incr=smoke->dir_wig=0;
    smoke->grav_amt=0; smoke->grav_dir=270;
    smoke->life_min=smoke->life_max=fmax(1.0,floor(lifetime[size]/cadence+.5));
    smoke->alpha[0]=.4; smoke->alpha[1]=.2; smoke->alpha[2]=0; smoke->nalpha=3;
    smoke->ori_min=smoke->ori_max=smoke->ori_incr=smoke->ori_wig=0; smoke->ori_rel=0;
    smoke->additive=0;
    PSys *system=ps(state,g_effect_sys[layer]);
    int half=spread[size]/2;
    for(int i=0;system && i<count[size];i++){
      double dx=floor(prnd(state)*spread[size])-half;
      double dy=floor(prnd(state)*spread[size])-half;
      sys_spawn(state,system,x+dx,y+dy,type,1,(int)(color&0xFFFFFF));
    }
    return;
  }
  if(kind==6 || kind==7 || kind==8){
    PType *flash=pt(state,type); if(!flash) return;
    int width,height,speed; effect_room_metrics(state,&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double initial_size[3]={.4,.75,1.2};
    static const double shrink[3]={-.02,-.03,-.04};
    static const double lifetime[3]={20,25,30};
    flash->sprite=-1; flash->shape=kind==6?4:(kind==7?9:8);
    flash->sz_min=flash->sz_max=initial_size[size]; flash->sz_incr=shrink[size]*cadence; flash->sz_wig=0;
    flash->xscale=flash->yscale=1;
    flash->sp_min=flash->sp_max=flash->sp_incr=flash->sp_wig=0;
    flash->dir_min=flash->dir_max=flash->dir_incr=flash->dir_wig=0;
    flash->grav_amt=0; flash->grav_dir=270;
    flash->life_min=flash->life_max=floor(lifetime[size]/cadence+.5);
    flash->alpha[0]=flash->alpha[1]=flash->alpha[2]=1; flash->nalpha=3;
    flash->ori_min=0; flash->ori_max=360; flash->ori_incr=flash->ori_wig=0; flash->ori_rel=0;
    flash->additive=0;
    gml_part_particles_create_color(state,g_effect_sys[layer],x,y,type,color,1);
    return;
  }
  if(kind==9){
    PType *cloud=pt(state,type); if(!cloud) return;
    int width,height,speed; effect_room_metrics(state,&width,&height,&speed);
    (void)width; (void)height;
    double cadence=fmax(30.0/speed,1.0);
    static const double cloud_size[3]={2,4,8};
    cloud->sprite=-1; cloud->shape=10;
    cloud->sz_min=cloud->sz_max=cloud_size[size]; cloud->sz_incr=cloud->sz_wig=0;
    cloud->xscale=1; cloud->yscale=.5;
    cloud->sp_min=cloud->sp_max=cloud->sp_incr=cloud->sp_wig=0;
    cloud->dir_min=cloud->dir_max=cloud->dir_incr=cloud->dir_wig=0;
    cloud->grav_amt=0; cloud->grav_dir=270;
    cloud->life_min=cloud->life_max=fmax(1.0,floor(100.0/cadence+.5));
    cloud->alpha[0]=0; cloud->alpha[1]=.3; cloud->alpha[2]=0; cloud->nalpha=3;
    cloud->ori_min=cloud->ori_max=cloud->ori_incr=cloud->ori_wig=0; cloud->ori_rel=0;
    cloud->additive=0;
    gml_part_particles_create_color(state,g_effect_sys[layer],x,y,type,color,1);
    return;
  }
  if(kind==10){
    PType *rain=pt(state,type); if(!rain) return;
    int width,height,speed; effect_room_metrics(state,&width,&height,&speed);
    double cadence=fmax(30.0/speed,1.0);
    rain->shape=3;
    rain->sz_min=.2; rain->sz_max=.3; rain->sz_incr=rain->sz_wig=0;
    rain->sp_min=rain->sp_max=7*cadence; rain->sp_incr=rain->sp_wig=0;
    rain->dir_min=rain->dir_max=260; rain->dir_incr=rain->dir_wig=0;
    rain->ori_min=rain->ori_max=rain->ori_incr=rain->ori_wig=0; rain->ori_rel=1;
    rain->alpha[0]=rain->alpha[1]=rain->alpha[2]=.4; rain->nalpha=3;
    rain->life_min=rain->life_max=fmax(1.0,floor(.2*height/cadence+.5));
    int number=size==0?2:(size==1?5:9);
    PSys *system=ps(state,g_effect_sys[layer]);
    for(int i=0;system && i<number;i++){
      double spawn_x=prnd(state)*width*1.2;
      double spawn_y=-30+floor(prnd(state)*20);
      sys_spawn(state,system,spawn_x,spawn_y,type,1,(int)(color&0xFFFFFF));
    }
    return;
  }
  if(kind==11){
    PType *snow=pt(state,type); if(!snow) return;
    int width,height,speed; effect_room_metrics(state,&width,&height,&speed);
    double cadence=fmax(30.0/speed,1.0);
    snow->sprite=-1; snow->shape=13;
    snow->sz_min=.1; snow->sz_max=.25; snow->sz_incr=snow->sz_wig=0;
    snow->xscale=snow->yscale=1;
    snow->sp_min=2.5*cadence; snow->sp_max=3.0*cadence;
    snow->sp_incr=snow->sp_wig=0;
    snow->dir_min=240; snow->dir_max=300; snow->dir_incr=0; snow->dir_wig=20;
    snow->grav_amt=0; snow->grav_dir=270;
    snow->life_min=snow->life_max=fmax(1.0,floor(.5*height/cadence+.5));
    snow->alpha[0]=snow->alpha[1]=snow->alpha[2]=.6; snow->nalpha=3;
    snow->ori_min=0; snow->ori_max=360; snow->ori_incr=snow->ori_wig=0;
    snow->ori_rel=0; snow->additive=0;
    int number=size==0?1:(size==1?3:7);
    PSys *system=ps(state,g_effect_sys[layer]);
    for(int i=0;system && i<number;i++){
      double spawn_x=prnd(state)*width*1.2-60;
      double spawn_y=floor(prnd(state)*20)-30;
      sys_spawn(state,system,spawn_x,spawn_y,type,1,(int)(color&0xFFFFFF));
    }
    return;
  }
}

int gml_part_emitter_create(GmlParticleState *state,int sysid){ (void)sysid; if(!state) return 0; for(int i=0;i<PE_MAX;i++) if(!g_pe[i].used){ memset(&g_pe[i],0,sizeof g_pe[i]); g_pe[i].used=1; g_pe[i].sys=sysid; return i+1; } return 0; }
int gml_part_emitter_exists(GmlParticleState *state,int sysid,int em){ PEmit *e=pe(state,em); return e && (sysid<=0 || e->sys==sysid); }
void gml_part_emitter_destroy(GmlParticleState *state,int em){ PEmit *e=pe(state,em); if(e) e->used=0; }
void gml_part_emitter_destroy_all(GmlParticleState *state,int sysid){ if(!state) return; for(int i=0;i<PE_MAX;i++) if(g_pe[i].used && (sysid<=0 || g_pe[i].sys==sysid)) g_pe[i].used=0; }
void gml_part_emitter_clear(GmlParticleState *state,int sysid,int em){ PEmit *e=pe(state,em); if(e && (sysid<=0 || e->sys==sysid)){ int used=e->used, sys=e->sys; memset(e,0,sizeof(*e)); e->used=used; e->sys=sys; } }
void gml_part_emitter_region(GmlParticleState *state,int sysid,int em,double xmin,double xmax,double ymin,double ymax,int shape,int dist){ (void)sysid; PEmit *e=pe(state,em); if(e){ e->xmin=xmin; e->xmax=xmax; e->ymin=ymin; e->ymax=ymax; e->shape=shape; e->dist=dist; } }
static void emit_point(GmlParticleState *state,PEmit *e,double *ox,double *oy){
  /* Sample normalized coordinates even for a zero-area region: the classic emitter advances both
   * axes before mapping them into the bounds. Diamond/ellipse still use the bounding rectangle. */
  double nx=prnd(state),ny=prnd(state);
  *ox=e->xmin+nx*(e->xmax-e->xmin);
  *oy=e->ymin+ny*(e->ymax-e->ymin);
}
static void emitter_burst(GmlParticleState *state,PSys *s,PEmit *e,int type,int number){
  if(!s||!e||number<=0) return;
  if(number>4000) number=4000;
  for(int k=0;k<number;k++){ double x,y; emit_point(state,e,&x,&y); sys_spawn(state,s,x+s->px,y+s->py,type,1,-1); }
}
void gml_part_emitter_burst(GmlParticleState *state,int sysid,int em,int type,int number){ PSys *s=ps(state,sysid); PEmit *e=pe(state,em); emitter_burst(state,s,e,type,number); }
void gml_part_emitter_stream(GmlParticleState *state,int sysid,int em,int type,int number){ PEmit *e=pe(state,em); if(e && (sysid<=0 || e->sys==sysid)){ e->sys=sysid; e->stream_type=type; e->stream_number=number; } }

static void emit_streams(GmlParticleState *state,int sysid,PSys *s){
  for(int i=0;i<PE_MAX;i++){
    PEmit *e=&g_pe[i];
    if(!e->used || e->sys!=sysid || e->stream_number==0) continue;
    if(e->stream_number>0) emitter_burst(state,s,e,e->stream_type,e->stream_number);
    else { int den=-e->stream_number; if(den>0 && prnd(state) < 1.0/(double)den) emitter_burst(state,s,e,e->stream_type,1); }
  }
}

static double particle_wiggle(long long tick,int period,int quarter){
  int step=(int)(tick%period); if(step<0) step+=period;
  double factor=step/(double)quarter;
  if(factor>2.0) factor=4.0-factor;
  return factor-1.0;
}

/* Positive child counts are exact. A negative count means
 * one child with probability 1/abs(number) on each eligible update. */
static void spawn_child_rule(GmlParticleState *state,PSys *s,double x,double y,int type,int number){
  if(number>0) sys_spawn(state,s,x,y,type,number,-1);
  else if(number<0){
    int den=number==INT_MIN ? INT_MAX : -number;
    if(den>0 && prnd(state)<1.0/(double)den) sys_spawn(state,s,x,y,type,1,-1);
  }
}

static void update_sys(GmlParticleState *state,int sysid,PSys *s){
  /* Child particles are appended while their parent is being advanced so RNG
   * consumption retains established order. Only the population present at entry
   * is updated; newborn particles are drawn once at age zero and begin moving
   * on the following step. */
  int initial_n=s->n;
  for(int i=0;i<initial_n;i++){
    Part *p=&s->parts[i]; PType *t=pt(state,p->type);
    if(t){
      p->speed += t->sp_incr; if(p->speed<0) p->speed=0;
      p->dir += t->dir_incr; p->ori += p->ori_incr;
    }
    /* Step increments affect this step's velocity; gravity then adjusts its vector. */
    double vx=p->speed*cos(DEG2RAD(p->dir)), vy=-p->speed*sin(DEG2RAD(p->dir));
    if(p->grav_amt!=0){ vx += p->grav_amt*cos(DEG2RAD(p->grav_dir)); vy += -p->grav_amt*sin(DEG2RAD(p->grav_dir));
      p->speed=hypot(vx,vy); p->dir = atan2(-vy,vx)*180.0/M_PI; }
    double move_speed=p->speed,move_dir=p->dir;
    if(t){
      long long timer=(long long)floor(p->life0-p->life)+1;
      if(t->dir_wig!=0)
        move_dir += t->dir_wig*particle_wiggle(timer+(long long)p->random_start*3,24,6);
      if(t->sp_wig!=0)
        move_speed += t->sp_wig*particle_wiggle(timer+(long long)p->random_start*4,20,5);
    }
    p->x += move_speed*cos(DEG2RAD(move_dir));
    p->y += -move_speed*sin(DEG2RAD(move_dir));
    if(t) p->size += p->size_incr;
    if(p->size<0) p->size=0;
    p->life -= 1;
    double child_x=p->x, child_y=p->y;
    int step_number=t?t->step_number:0, step_type=t?t->step_type:0;
    int death_number=t?t->death_number:0, death_type=t?t->death_type:0;
    int dead=p->life<=0;
    if(step_number) spawn_child_rule(state,s,child_x,child_y,step_type,step_number);
    if(dead && death_number) spawn_child_rule(state,s,child_x,child_y,death_type,death_number);
    if(dead) s->parts[i].type=0; /* reacquire after a possible realloc */
  }
  /* Compact dead parents without pulling newborn particles into the range
   * that was updated this frame. */
  int write=0;
  for(int i=0;i<initial_n;i++) if(s->parts[i].type!=0){
    if(write!=i) s->parts[write]=s->parts[i];
    write++;
  }
  int newborn=s->n-initial_n;
  if(newborn>0 && write!=initial_n)
    memmove(s->parts+write,s->parts+initial_n,(size_t)newborn*sizeof(Part));
  s->n=write+newborn;
  /* Stream particles are born after the current population advances. They are therefore drawn at
   * their initial position/alpha once and only start ageing on the following particle update. */
  emit_streams(state,sysid,s);
}
void gml_part_system_update(GmlParticleState *state,int id){ PSys *s=ps(state,id); if(s) update_sys(state,id,s); }
void gml_part_update_all(GmlParticleState *state){ if(!state) return; for(int i=0;i<PS_MAX;i++) if(g_ps[i].used && g_ps[i].auto_update) update_sys(state,i+1,&g_ps[i]); }

/* interpolate a channel over the particle's age (0 at birth → 1 at death) across up to 3 keys. */
static double keyf(double age, int nk, double k0, double k1, double k2){
  if(nk<=1) return k0;
  if(nk==2) return k0 + (k1-k0)*age;
  if(age<0.5) return k0 + (k1-k0)*(age*2.0);
  return k1 + (k2-k1)*((age-0.5)*2.0);
}
static uint32_t keyc(double age, PType *t){
  if(t->ncol<=1) return t->col[0];
  double r0=t->col[0]&0xFF,g0=(t->col[0]>>8)&0xFF,b0=(t->col[0]>>16)&0xFF;   /* GM BBGGRR */
  double r1=t->col[1]&0xFF,g1=(t->col[1]>>8)&0xFF,b1=(t->col[1]>>16)&0xFF;
  double r2=t->col[2]&0xFF,g2=(t->col[2]>>8)&0xFF,b2=(t->col[2]>>16)&0xFF;
  double r=keyf(age,t->ncol,r0,r1,r2), g=keyf(age,t->ncol,g0,g1,g2), b=keyf(age,t->ncol,b0,b1,b2);
  return ((uint32_t)(b+0.5)<<16)|((uint32_t)(g+0.5)<<8)|(uint32_t)(r+0.5);
}

/* plot a filled clipped square for shape/pixel particles (no custom sprite) */
static void plot_square(GmlRender *r, int cx, int cy, int half, uint32_t col, double a){
  if(!r||!r->fb||a<=0) return; if(a>1) a=1;
  int br=col&0xFF, bg=(col>>8)&0xFF, bb=(col>>16)&0xFF;   /* GM BBGGRR → r,g,b */
  int x0=cx-half, x1=cx+half+1, y0=cy-half, y1=cy+half+1;
  if(x0<0)x0=0; if(y0<0)y0=0; if(x1>r->fbw)x1=r->fbw; if(y1>r->fbh)y1=r->fbh;
  if(x1<=x0 || y1<=y0) return;
  int ia=(int)(a*255+0.5), iia=255-ia;
  uint32_t src=0xFF000000u|((uint32_t)br<<16)|((uint32_t)bg<<8)|(uint32_t)bb;
  if(ia>=255 || !r->alphablend){
    int n=x1-x0;
    for(int y=y0;y<y1;y++){ uint32_t *dp=r->fb+(size_t)y*r->fbw+x0;
      for(int x=0;x<n;x++) dp[x]=src; }
    return;
  }
  int pixels=(x1-x0)*(y1-y0);
  if(pixels>=64){
    uint8_t lr[256], lg[256], lb[256], la[256];
    for(int d=0; d<256; d++){
      lr[d]=(uint8_t)((br*ia+d*iia)/255);
      lg[d]=(uint8_t)((bg*ia+d*iia)/255);
      lb[d]=(uint8_t)((bb*ia+d*iia)/255);
      la[d]=(uint8_t)(ia+(d*iia)/255);
    }
    for(int y=y0;y<y1;y++){ uint32_t *dp=r->fb+(size_t)y*r->fbw;
      for(int x=x0;x<x1;x++){ uint32_t dv=dp[x];
        dp[x]=((uint32_t)la[(dv>>24)&0xFF]<<24)|((uint32_t)lr[(dv>>16)&0xFF]<<16)|
              ((uint32_t)lg[(dv>>8)&0xFF]<<8)|(uint32_t)lb[dv&0xFF]; } }
    return;
  }
  for(int y=y0;y<y1;y++){ uint32_t *dp=r->fb+(size_t)y*r->fbw;
    for(int x=x0;x<x1;x++){ uint32_t dv=dp[x];
      int dr=(dv>>16)&0xFF,dg=(dv>>8)&0xFF,db=dv&0xFF;
      int da=(dv>>24)&0xFF,oa=ia+(da*iia)/255;
      dp[x]=((uint32_t)oa<<24)|(((br*ia+dr*iia)/255)<<16)|
            (((bg*ia+dg*iia)/255)<<8)|((bb*ia+db*iia)/255); } }
}

static double sample_ring_shape(GmlParticleState *state,double x,double y){
  if(!g_ring_shape_mask_ready){
    /* Classic particle shapes are filtered from a 64x64 cell.  Build the soft ring
     * procedurally, then sample that cell below so small particles retain the same
     * filtered edge and thickness instead of turning into scale-dependent vectors. */
    for(int py=0;py<64;py++) for(int px=0;px<64;px++){
      double nx=(px-31.5)/32.0,ny=(py-31.5)/32.0;
      double radial=sqrt(nx*nx+ny*ny);
      double coverage=(1.0-fabs(radial-0.795)/0.085)*0.90;
      if(coverage<0.0) coverage=0.0;
      if(coverage>1.0) coverage=1.0;
      g_ring_shape_mask[py*64+px]=(uint8_t)(coverage*255.0+0.5);
    }
    g_ring_shape_mask_ready=1;
  }
  double sx=x+31.5,sy=y+31.5;
  int ix=(int)floor(sx),iy=(int)floor(sy);
  if(ix<0 || iy<0 || ix>=63 || iy>=63) return 0.0;
  double fx=sx-ix,fy=sy-iy;
  double a=g_ring_shape_mask[iy*64+ix]*(1.0-fx)+g_ring_shape_mask[iy*64+ix+1]*fx;
  double b=g_ring_shape_mask[(iy+1)*64+ix]*(1.0-fx)+g_ring_shape_mask[(iy+1)*64+ix+1]*fx;
  return (a*(1.0-fy)+b*fy)/255.0;
}

static void plot_circle_shape(GmlParticleState *state,GmlRender *r,double cx,double cy,double xs,double ys,
                              uint32_t color,double alpha,int hollow){
  double rx=32.0*fabs(xs),ry=32.0*fabs(ys);
  if(!r || rx<0.25 || ry<0.25 || alpha<=0) return;
  int x0=(int)floor(cx-rx),x1=(int)ceil(cx+rx);
  int y0=(int)floor(cy-ry),y1=(int)ceil(cy+ry);
  if(x0<0)x0=0; if(y0<0)y0=0; if(x1>=r->fbw)x1=r->fbw-1; if(y1>=r->fbh)y1=r->fbh-1;
  for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
    double nx=(x+0.5-cx)/rx,ny=(y+0.5-cy)/ry,d=sqrt(nx*nx+ny*ny);
    double coverage;
    if(hollow) coverage=sample_ring_shape(state,nx*32.0,ny*32.0);
    else coverage=(1.0-d)*fmin(rx,ry);
    if(coverage<=0) continue;
    if(coverage>1) coverage=1;
    plot_square(r,x,y,0,color,alpha*coverage);
  }
}

/* Shape 3 is a slender soft-edged bar inside the standard 64x64 particle cell.
 * Evaluate its coverage procedurally. The long and short edge ramps are
 * separable, which also lets the exact-2x path evaluate real half-pixel
 * samples rather than filtering a rasterized line. */
static double sample_line_shape(double x,double y){
  double horizontal=fmin((x+29.0)/5.0,(28.0-x)/5.0);
  double vertical=fmin((y+5.75)/2.5,(4.75-y)/2.5);
  if(horizontal<=0 || vertical<=0) return 0;
  if(horizontal>1) horizontal=1;
  if(vertical>1) vertical=1;
  return horizontal*vertical;
}

static double sample_line_shape_texture(double x,double y,int interpolate){
  if(!interpolate)
    return sample_line_shape(floor(x+.5),floor(y+.5));
  double x0=floor(x),y0=floor(y),fx=x-x0,fy=y-y0;
  double a=sample_line_shape(x0,y0),b=sample_line_shape(x0+1.0,y0);
  double c=sample_line_shape(x0,y0+1.0),d=sample_line_shape(x0+1.0,y0+1.0);
  return (a+(b-a)*fx)*(1.0-fy)+(c+(d-c)*fx)*fy;
}

static void plot_line_shape_plane(GmlRender *r,uint32_t *plane,
                                  double cx,double cy,double xs,double ys,
                                  double co,double si,double ex,double ey,
                                  double sample_x,double sample_y,
                                  uint32_t color,double alpha){
  if(!r || !plane || fabs(xs)<1.0/128.0 || fabs(ys)<1.0/128.0 || alpha<=0) return;
  int x0=(int)floor(cx-ex-sample_x),x1=(int)ceil(cx+ex-sample_x);
  int y0=(int)floor(cy-ey-sample_y),y1=(int)ceil(cy+ey-sample_y);
  if(x0<0)x0=0; if(y0<0)y0=0;
  if(x1>=r->fbw)x1=r->fbw-1; if(y1>=r->fbh)y1=r->fbh-1;
  uint32_t *saved_fb=r->fb;
  r->fb=plane;
  for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
    double dx=x+sample_x-cx,dy=y+sample_y-cy;
    double lx=co*dx-si*dy,ly=si*dx+co*dy;
    /* The historical sprite quad applies its half-pixel correction before
     * scale and rotation.  Convert the screen sample back to a virtual source
     * texel, then use the active nearest/linear texture filter. */
    double source_x=(lx+.5)/xs-.5,source_y=(ly+.5)/ys-.5;
    double coverage=sample_line_shape_texture(source_x,source_y,r->interp);
    if(coverage>0) plot_square(r,x,y,0,color,alpha*coverage);
  }
  r->fb=saved_fb;
}

static void plot_line_shape(GmlRender *r,double cx,double cy,double xs,double ys,double angle,
                            uint32_t color,double alpha){
  if(!r || !r->fb || fabs(xs)<1.0/128.0 || fabs(ys)<1.0/128.0 || alpha<=0) return;
  double rad=DEG2RAD(angle),co=cos(rad),si=sin(rad);
  double ex=fabs(co)*29.0*fabs(xs)+fabs(si)*6.0*fabs(ys)+1.0;
  double ey=fabs(si)*29.0*fabs(xs)+fabs(co)*6.0*fabs(ys)+1.0;
  uint32_t *base=r->fb;
  plot_line_shape_plane(r,base,cx,cy,xs,ys,co,si,ex,ey,0,0,color,alpha);
  if(r->target_sp==0 && base==r->base_fb &&
     r->classic_interp_phase[0] && r->classic_interp_phase[1] &&
     r->classic_interp_phase[2]){
    plot_line_shape_plane(r,r->classic_interp_phase[0],cx,cy,xs,ys,co,si,ex,ey,.5,0,color,alpha);
    plot_line_shape_plane(r,r->classic_interp_phase[1],cx,cy,xs,ys,co,si,ex,ey,0,.5,color,alpha);
    plot_line_shape_plane(r,r->classic_interp_phase[2],cx,cy,xs,ys,co,si,ex,ey,.5,.5,color,alpha);
  }
}

/* The textured classic shapes occupy a 64x64 cell.  Generate the soft,
 * irregular explosion field from an analytic function at startup rather than
 * shipping a precomputed bitmap.  Keeping the field in a small mask also avoids
 * evaluating trigonometry for every live particle and every frame. Each VM owns
 * its generated masks so initialization and interleaving are instance-safe. */

static double unit_clamp(double v){ return v<0?0:(v>1?1:v); }
static double smooth_unit(double v){ v=unit_clamp(v); return v*v*(3.0-2.0*v); }

static double point_segment_distance(double px,double py,double ax,double ay,double bx,double by){
  double dx=bx-ax,dy=by-ay,den=dx*dx+dy*dy;
  double t=den>0?((px-ax)*dx+(py-ay)*dy)/den:0;
  if(t<0)t=0; else if(t>1)t=1;
  return hypot(px-(ax+t*dx),py-(ay+t*dy));
}

/* Generate the classic glint family from geometry rather than bundling
 * precomputed textures.  All masks keep the transparent 64x64 particle cell:
 * a faceted five-point star, a soft radial flare and a fine multi-ray spark. */
static void prepare_glint_shape_mask(GmlParticleState *state,int shape){
  int slot=shape==4?0:(shape==8?1:2);
  unsigned bit=1u<<slot;
  if(g_glint_shape_mask_ready&bit) return;
  uint8_t *mask=g_glint_shape_mask[slot];
  for(int y=0;y<64;y++) for(int x=0;x<64;x++){
    double px=x+.5-32.0,py=y+.5-32.0;
    double radius=hypot(px,py),angle=atan2(py,px);
    double coverage=0;
    if(shape==4){
      double vx[10],vy[10];
      int inside=0;
      double edge=64;
      for(int i=0;i<10;i++){
        double a=-M_PI*.5+i*M_PI/5.0;
        double r=(i&1)?12.0:28.0;
        vx[i]=cos(a)*r; vy[i]=sin(a)*r;
      }
      for(int i=0,j=9;i<10;j=i++){
        if(((vy[i]>py)!=(vy[j]>py)) &&
           px<(vx[j]-vx[i])*(py-vy[i])/(vy[j]-vy[i])+vx[i]) inside=!inside;
        double d=point_segment_distance(px,py,vx[i],vy[i],vx[j],vy[j]);
        if(d<edge) edge=d;
      }
      double antialias=smooth_unit((inside?edge:-edge)+.75);
      double facets=.58+.42*(.5+.5*cos(angle*5.0+M_PI*.5));
      double centre=.72+.28*smooth_unit(radius/15.0);
      coverage=antialias*facets*centre;
    } else if(shape==8){
      double glow=exp(-(radius*radius)/(2.0*11.5*11.5))*.72;
      double core=smooth_unit((4.7-radius)*.55+.5);
      double major=exp(-pow(radius*fabs(sin(angle*4.0))/.85,2.0))
                  *smooth_unit((30.0-radius)/8.0);
      double minor=exp(-pow(radius*fabs(sin(angle*8.0))/.65,2.0))
                  *smooth_unit((23.0-radius)/7.0)*.45;
      double envelope=smooth_unit((31.5-radius)/3.0);
      coverage=fmax(core,fmax(glow,fmax(major*.70,minor)))*envelope;
    } else {
      double core=exp(-(radius*radius)/(2.0*3.2*3.2));
      double long_rays=exp(-pow(radius*fabs(sin(angle*8.0))/.42,2.0))
                      *smooth_unit((31.0-radius)/9.0);
      double fine_rays=exp(-pow(radius*fabs(sin(angle*12.0))/.32,2.0))
                      *smooth_unit((25.0-radius)/8.0)*.65;
      double irregular=.72+.28*(.5+.5*sin(angle*37.0+1.1));
      coverage=fmax(core,fmax(long_rays,fine_rays))*irregular;
    }
    mask[y*64+x]=(uint8_t)(255.0*unit_clamp(coverage)+.5);
  }
  g_glint_shape_mask_ready|=bit;
}

static void prepare_explosion_shape_mask(GmlParticleState *state){
  if(g_explosion_shape_mask_ready) return;
  for(int y=0;y<64;y++) for(int x=0;x<64;x++){
    double nx=(x+.5-32.0)/32.0,ny=(y+.5-32.0)/32.0;
    double radius=hypot(nx,ny),angle=atan2(ny,nx);
    double rim=.84 + .065*sin(angle*5.0+.4) + .045*sin(angle*9.0-1.1)
                    + .025*sin(angle*17.0+.8);
    double edge=smooth_unit((rim-radius)*4.0+.5);
    double grain=.80 + .12*sin(nx*13.0+ny*7.0+.6)
                       *sin(nx*5.0-ny*17.0-.3)
                       + .08*cos(nx*21.0+ny*11.0);
    double centre=.82+.18*smooth_unit(radius*3.0);
    g_explosion_shape_mask[y*64+x]=(uint8_t)(255.0*unit_clamp(edge*grain*centre*.92)+.5);
  }
  g_explosion_shape_mask_ready=1;
}

/* The classic snow cell is a soft, filled six-lobed flake rather than a
 * branching line drawing.  A radial boundary keeps the generated mask
 * symmetric under rotation while the broad edge reproduces its soft halo. */
static void prepare_snow_shape_mask(GmlParticleState *state){
  if(g_snow_shape_mask_ready) return;
  for(int y=0;y<64;y++) for(int x=0;x<64;x++){
    double px=x+.5-32.0,py=y+.5-32.0;
    double radius=hypot(px,py),angle=atan2(py,px);
    double boundary=23.0-3.0*cos(angle*6.0);
    double coverage=smooth_unit((boundary-radius)*.07+.5);
    g_snow_shape_mask[y*64+x]=(uint8_t)(255.0*coverage+.5);
  }
  g_snow_shape_mask_ready=1;
}

static double sample_explosion_shape(GmlParticleState *state,double x,double y){
  prepare_explosion_shape_mask(state);
  double tx=x+31.5,ty=y+31.5;
  if(tx<0 || ty<0 || tx>63 || ty>63) return 0;
  int x0=(int)floor(tx),y0=(int)floor(ty);
  int x1=x0<63?x0+1:x0,y1=y0<63?y0+1:y0;
  double fx=tx-x0,fy=ty-y0;
  double a=g_explosion_shape_mask[y0*64+x0];
  double b=g_explosion_shape_mask[y0*64+x1];
  double c=g_explosion_shape_mask[y1*64+x0];
  double d=g_explosion_shape_mask[y1*64+x1];
  return ((a+(b-a)*fx)*(1.0-fy)+(c+(d-c)*fx)*fy)/255.0;
}

static double sample_glint_shape(GmlParticleState *state,int shape,double x,double y){
  prepare_glint_shape_mask(state,shape);
  int slot=shape==4?0:(shape==8?1:2);
  const uint8_t *mask=g_glint_shape_mask[slot];
  double tx=x+31.5,ty=y+31.5;
  if(tx<0 || ty<0 || tx>63 || ty>63) return 0;
  int x0=(int)floor(tx),y0=(int)floor(ty);
  int x1=x0<63?x0+1:x0,y1=y0<63?y0+1:y0;
  double fx=tx-x0,fy=ty-y0;
  double a=mask[y0*64+x0],b=mask[y0*64+x1];
  double c=mask[y1*64+x0],d=mask[y1*64+x1];
  return ((a+(b-a)*fx)*(1.0-fy)+(c+(d-c)*fx)*fy)/255.0;
}

static double sample_snow_shape(GmlParticleState *state,double x,double y){
  prepare_snow_shape_mask(state);
  double tx=x+31.5,ty=y+31.5;
  if(tx<0 || ty<0 || tx>63 || ty>63) return 0;
  int x0=(int)floor(tx),y0=(int)floor(ty);
  int x1=x0<63?x0+1:x0,y1=y0<63?y0+1:y0;
  double fx=tx-x0,fy=ty-y0;
  double a=g_snow_shape_mask[y0*64+x0],b=g_snow_shape_mask[y0*64+x1];
  double c=g_snow_shape_mask[y1*64+x0],d=g_snow_shape_mask[y1*64+x1];
  return ((a+(b-a)*fx)*(1.0-fy)+(c+(d-c)*fx)*fy)/255.0;
}

static void plot_explosion_shape(GmlParticleState *state,GmlRender *r,double cx,double cy,double xs,double ys,
                                 double angle,uint32_t color,double alpha){
  if(!r || !r->fb || fabs(xs)<1.0/128.0 || fabs(ys)<1.0/128.0 || alpha<=0) return;
  double rad=DEG2RAD(angle),co=cos(rad),si=sin(rad);
  double sx=32.0*fabs(xs),sy=32.0*fabs(ys);
  double ex=fabs(co)*sx+fabs(si)*sy,ey=fabs(si)*sx+fabs(co)*sy;
  int x0=(int)floor(cx-ex),x1=(int)ceil(cx+ex);
  int y0=(int)floor(cy-ey),y1=(int)ceil(cy+ey);
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>=r->fbw) x1=r->fbw-1;
  if(y1>=r->fbh) y1=r->fbh-1;
  for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
    double dx=x+.5-cx,dy=y+.5-cy;
    double lx=co*dx-si*dy,ly=si*dx+co*dy;
    double coverage=sample_explosion_shape(state,lx/xs,ly/ys);
    if(coverage>0) plot_square(r,x,y,0,color,alpha*coverage);
  }
}

static void plot_glint_shape(GmlParticleState *state,GmlRender *r,int shape,double cx,double cy,double xs,double ys,
                             double angle,uint32_t color,double alpha){
  if(!r || !r->fb || fabs(xs)<1.0/128.0 || fabs(ys)<1.0/128.0 || alpha<=0) return;
  double rad=DEG2RAD(angle),co=cos(rad),si=sin(rad);
  double sx=32.0*fabs(xs),sy=32.0*fabs(ys);
  double ex=fabs(co)*sx+fabs(si)*sy,ey=fabs(si)*sx+fabs(co)*sy;
  int x0=(int)floor(cx-ex),x1=(int)ceil(cx+ex);
  int y0=(int)floor(cy-ey),y1=(int)ceil(cy+ey);
  if(x0<0)x0=0;
  if(y0<0)y0=0;
  if(x1>=r->fbw)x1=r->fbw-1;
  if(y1>=r->fbh)y1=r->fbh-1;
  for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
    double dx=x+.5-cx,dy=y+.5-cy;
    double lx=co*dx-si*dy,ly=si*dx+co*dy;
    double coverage=sample_glint_shape(state,shape,lx/xs,ly/ys);
    if(coverage>0) plot_square(r,x,y,0,color,alpha*coverage);
  }
}

static void plot_snow_shape(GmlParticleState *state,GmlRender *r,double cx,double cy,double xs,double ys,
                            double angle,uint32_t color,double alpha){
  if(!r || !r->fb || fabs(xs)<1.0/128.0 || fabs(ys)<1.0/128.0 || alpha<=0) return;
  double rad=DEG2RAD(angle),co=cos(rad),si=sin(rad);
  double sx=32.0*fabs(xs),sy=32.0*fabs(ys);
  double ex=fabs(co)*sx+fabs(si)*sy,ey=fabs(si)*sx+fabs(co)*sy;
  int x0=(int)floor(cx-ex),x1=(int)ceil(cx+ex);
  int y0=(int)floor(cy-ey),y1=(int)ceil(cy+ey);
  if(x0<0)x0=0;
  if(y0<0)y0=0;
  if(x1>=r->fbw)x1=r->fbw-1;
  if(y1>=r->fbh)y1=r->fbh-1;
  for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++){
    double dx=x+.5-cx,dy=y+.5-cy;
    double lx=co*dx-si*dy,ly=si*dx+co*dy;
    double coverage=sample_snow_shape(state,lx/xs,ly/ys);
    if(coverage>0) plot_square(r,x,y,0,color,alpha*coverage);
  }
}

void gml_part_system_drawit(GmlParticleState *state,GmlRender *r,int id){
  PSys *s=ps(state,id); if(!s||!r) return;
  for(int i=0;i<s->n;i++){ Part *p=&s->parts[i]; PType *t=pt(state,p->type); if(!t) continue;
    double age = p->life0>0 ? (p->life0-p->life)/p->life0 : 0; if(age<0)age=0; if(age>1)age=1;
    long long timer=(long long)floor(p->life0-p->life);
    double draw_size=p->size;
    double draw_ori=p->ori;
    if(t->sz_wig!=0) draw_size += t->sz_wig*particle_wiggle(timer+p->random_start,16,4);
    if(t->ori_wig!=0) draw_ori += t->ori_wig*particle_wiggle(timer+(long long)p->random_start*2,16,4);
    if(t->sprite>=0){
      int frames=gml_sprite_frames(r,t->sprite); int sub = (t->spr_animate&&frames>0)? (int)(age*frames)%frames : 0;
      double xs=draw_size*t->xscale, ys=draw_size*t->yscale;
      if(t->sprite<r->n_spr && frames>0){
        GmlSprite *spr=&r->spr[t->sprite];
        double ax=fabs(xs), ay=fabs(ys);
        double rx=fmax((double)spr->originx,(double)(spr->w-spr->originx))*ax;
        double ry=fmax((double)spr->originy,(double)(spr->h-spr->originy))*ay;
        double rad=hypot(rx,ry) + 2.0;
        double sx=p->x-r->cam_x, sy=p->y-r->cam_y;
        if(sx+rad<0 || sy+rad<0 || sx-rad>=r->fbw || sy-rad>=r->fbh) continue;
      }
      double alpha=keyf(age,t->nalpha,t->alpha[0],t->alpha[1],t->alpha[2]);
      if(alpha<=0) continue;
      uint32_t col = p->has_col ? p->col_over : keyc(age,t);
      /* gml_draw_sprite_ext applies the camera itself → pass world (x,y), NOT camera-relative. */
      gml_draw_sprite_ext(r,t->sprite,sub, p->x, p->y,
                          xs, ys, draw_ori+(t->ori_rel?p->dir:0), col, alpha);
    } else {
      int half=(int)(draw_size)+0; if(half<0)half=0; if(half>64)half=64;
      int cx=(int)(p->x - r->cam_x), cy=(int)(p->y - r->cam_y);
      double shape_rx=(t->shape==1||t->shape==5||t->shape==6||t->shape==7)?
        32.0*fabs(draw_size*t->xscale):half;
      double shape_ry=(t->shape==1||t->shape==5||t->shape==6||t->shape==7)?
        32.0*fabs(draw_size*t->yscale):half;
      if(t->shape==3 || t->shape==4 || t->shape==8 || t->shape==9 || t->shape==10 || t->shape==13){
        shape_rx=shape_ry=32.0*hypot(draw_size*t->xscale,draw_size*t->yscale);
      }
      if(cx+shape_rx<0 || cy+shape_ry<0 || cx-shape_rx>=r->fbw || cy-shape_ry>=r->fbh) continue;
      double alpha=keyf(age,t->nalpha,t->alpha[0],t->alpha[1],t->alpha[2]);
      if(alpha<=0) continue;
      uint32_t col = p->has_col ? p->col_over : keyc(age,t);
      gml_render_maybe_prepare_draw(r);
      if(t->shape==1 || t->shape==5 || t->shape==6 || t->shape==7){
        plot_circle_shape(state,r,p->x-r->cam_x,p->y-r->cam_y,
                          draw_size*t->xscale,draw_size*t->yscale,col,alpha,t->shape==5||t->shape==6);
      } else if(t->shape==3){
        double angle=draw_ori+(t->ori_rel?p->dir:0);
        plot_line_shape(r,p->x-r->cam_x,p->y-r->cam_y,
                        draw_size*t->xscale,draw_size*t->yscale,angle,col,alpha);
      } else if(t->shape==4 || t->shape==8 || t->shape==9){
        double angle=draw_ori+(t->ori_rel?p->dir:0);
        plot_glint_shape(state,r,t->shape,p->x-r->cam_x,p->y-r->cam_y,
                         draw_size*t->xscale,draw_size*t->yscale,angle,col,alpha);
      } else if(t->shape==10){
        double angle=draw_ori+(t->ori_rel?p->dir:0);
        plot_explosion_shape(state,r,p->x-r->cam_x,p->y-r->cam_y,
                             draw_size*t->xscale,draw_size*t->yscale,angle,col,alpha);
      } else if(t->shape==13){
        double angle=draw_ori+(t->ori_rel?p->dir:0);
        plot_snow_shape(state,r,p->x-r->cam_x,p->y-r->cam_y,
                        draw_size*t->xscale,draw_size*t->yscale,angle,col,alpha);
      } else if(!gml_d3_draw_rectangle_2d(r,p->x-half,p->y-half,p->x+half+1,p->y+half+1,col,alpha,0))
        plot_square(r,cx,cy,half,col,alpha);
    }
  }
}
void gml_part_system_draw_all(GmlParticleState *state,GmlRender *r){ if(!state) return; for(int i=0;i<PS_MAX;i++) if(g_ps[i].used && g_ps[i].auto_draw) gml_part_system_drawit(state,r,i+1); }

enum { GML_PARTICLE_STATE_SCHEMA=1 };
#define GML_PARTICLE_STATE_MAGIC UINT32_C(0x53545041)
typedef struct { uint8_t *data; size_t cap, pos; int ok; } PartW;
typedef struct { const uint8_t *data; size_t cap, pos; int ok; } PartR;

static void pw_raw(PartW *w, const void *p, size_t n){
  if(n>SIZE_MAX-w->pos){ w->ok=0; w->pos=SIZE_MAX; return; }
  if(w->data){
    if(w->pos<=w->cap && n<=w->cap-w->pos) memcpy(w->data+w->pos,p,n);
    else w->ok=0;
  }
  w->pos+=n;
}
static void pr_raw(PartR *r, void *p, size_t n){
  if(n>SIZE_MAX-r->pos){ memset(p,0,n); r->ok=0; r->pos=SIZE_MAX; return; }
  if(r->pos<=r->cap && n<=r->cap-r->pos) memcpy(p,r->data+r->pos,n);
  else { memset(p,0,n); r->ok=0; }
  r->pos+=n;
}
static void pw_u32(PartW *w, uint32_t v){
  uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
  pw_raw(w,b,sizeof b);
}
static void pw_i32(PartW *w, int v){ pw_u32(w,(uint32_t)(int32_t)v); }
static void pw_d(PartW *w, double v){
  uint64_t bits=0; uint8_t b[8]; memcpy(&bits,&v,sizeof bits);
  for(unsigned i=0;i<8;i++) b[i]=(uint8_t)(bits>>(i*8));
  pw_raw(w,b,sizeof b);
}
static uint32_t pr_u32(PartR *r){
  uint8_t b[4]={0}; pr_raw(r,b,sizeof b);
  return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static int pr_i32(PartR *r){ return (int)(int32_t)pr_u32(r); }
static double pr_d(PartR *r){
  uint8_t b[8]={0}; uint64_t bits=0; pr_raw(r,b,sizeof b);
  for(unsigned i=0;i<8;i++) bits|=(uint64_t)b[i]<<(i*8);
  double v=0; memcpy(&v,&bits,sizeof v); return v;
}

static void part_state_write(GmlParticleState *state,PartW *w){
  pw_u32(w,GML_PARTICLE_STATE_MAGIC); /* APTS */
  pw_u32(w,GML_PARTICLE_STATE_SCHEMA);
  pw_u32(w,g_prng);
  int nt=0; for(int i=0;i<PT_MAX;i++) if(g_pt[i].used) nt++;
  pw_i32(w,nt);
  for(int i=0;i<PT_MAX;i++) if(g_pt[i].used){ pw_i32(w,i+1); pw_raw(w,&g_pt[i],sizeof(g_pt[i])); }
  int ns=0; for(int i=0;i<PS_MAX;i++) if(g_ps[i].used) ns++;
  pw_i32(w,ns);
  for(int i=0;i<PS_MAX;i++) if(g_ps[i].used){
    PSys *s=&g_ps[i];
    pw_i32(w,i+1); pw_i32(w,s->auto_update); pw_i32(w,s->auto_draw);
    pw_d(w,s->depth); pw_d(w,s->px); pw_d(w,s->py);
    pw_i32(w,s->n);
    if(s->n>0) pw_raw(w,s->parts,(size_t)s->n*sizeof(Part));
  }
  int ne=0; for(int i=0;i<PE_MAX;i++) if(g_pe[i].used) ne++;
  pw_i32(w,ne);
  for(int i=0;i<PE_MAX;i++) if(g_pe[i].used){ pw_i32(w,i+1); pw_raw(w,&g_pe[i],sizeof(g_pe[i])); }
  for(int layer=0;layer<2;layer++) pw_i32(w,g_effect_sys[layer]);
  for(int layer=0;layer<2;layer++) for(int kind=0;kind<12;kind++) for(int size=0;size<3;size++)
    pw_i32(w,g_effect_type[layer][kind][size]);
  for(int layer=0;layer<2;layer++) for(int size=0;size<3;size++)
    pw_i32(w,g_effect_explosion_core[layer][size]);
}

size_t gml_part_state_size(GmlParticleState *state){
  if(!state) return 0;
  PartW w={0}; w.ok=1; part_state_write(state,&w); return w.pos;
}
int gml_part_state_save(GmlParticleState *state,void *data,size_t len,size_t *written){
  if(!state) return 0;
  PartW w={(uint8_t*)data,len,0,1};
  part_state_write(state,&w);
  if(written) *written=w.pos;
  return w.ok && w.pos<=len;
}
int gml_part_state_load(GmlParticleState *state,const void *data,size_t len,size_t *used){
  if(!state) return 0;
  PartR r={(const uint8_t*)data,len,0,1};
  uint32_t magic=pr_u32(&r);
  uint32_t schema=pr_u32(&r);
  if(magic!=GML_PARTICLE_STATE_MAGIC || schema!=GML_PARTICLE_STATE_SCHEMA){
    if(used) *used=r.pos; return 0;
  }
  gml_part_reset_all(state);
  g_prng=pr_u32(&r);
  int nt=pr_i32(&r);
  if(nt<0 || nt>PT_MAX) r.ok=0;
  for(int k=0;k<nt;k++){
    int id=pr_i32(&r);
    PType tmp; memset(&tmp,0,sizeof(tmp));
    pr_raw(&r,&tmp,sizeof(tmp));
    if(id>=1 && id<=PT_MAX){ g_pt[id-1]=tmp; g_pt[id-1].used=1; }
  }
  int ns=pr_i32(&r);
  if(ns<0 || ns>PS_MAX) r.ok=0;
  for(int k=0;k<ns;k++){
    int id=pr_i32(&r);
    int au=pr_i32(&r), ad=pr_i32(&r);
    double depth=pr_d(&r), px=pr_d(&r), py=pr_d(&r);
    int n=pr_i32(&r);
    if(n<0 || n>200000){ r.ok=0; n=0; }
    size_t bytes=(size_t)n*sizeof(Part);
    Part *parts=n?calloc((size_t)n,sizeof(Part)):NULL;
    if(n && !parts){
      r.ok=0;
      if(r.pos+bytes<=r.cap) r.pos+=bytes; else { r.pos+=bytes; r.ok=0; }
      n=0;
    } else if(n) pr_raw(&r,parts,bytes);
    if(id>=1 && id<=PS_MAX){
      PSys *s=&g_ps[id-1]; memset(s,0,sizeof(*s));
      s->used=1; s->auto_update=au?1:0; s->auto_draw=ad?1:0;
      s->depth=depth; s->px=px; s->py=py; s->parts=parts; s->n=s->cap=n;
      parts=NULL;
    }
    free(parts);
  }
  int ne=pr_i32(&r);
  if(ne<0 || ne>PE_MAX) r.ok=0;
  for(int k=0;k<ne;k++){
    int id=pr_i32(&r);
    PEmit tmp; memset(&tmp,0,sizeof(tmp));
    pr_raw(&r,&tmp,sizeof(tmp));
    if(id>=1 && id<=PE_MAX){ g_pe[id-1]=tmp; g_pe[id-1].used=1; }
  }
  for(int layer=0;layer<2;layer++){
    int id=pr_i32(&r);
    if(id<0 || id>PS_MAX || (id && !g_ps[id-1].used)){ r.ok=0; id=0; }
    g_effect_sys[layer]=id;
  }
  for(int layer=0;layer<2;layer++) for(int kind=0;kind<12;kind++) for(int size=0;size<3;size++){
    int id=pr_i32(&r);
    if(id<0 || id>PT_MAX || (id && !g_pt[id-1].used)){ r.ok=0; id=0; }
    g_effect_type[layer][kind][size]=id;
  }
  for(int layer=0;layer<2;layer++) for(int size=0;size<3;size++){
    int id=pr_i32(&r);
    if(id<0 || id>PT_MAX || (id && !g_pt[id-1].used)){ r.ok=0; id=0; }
    g_effect_explosion_core[layer][size]=id;
  }
  if(used) *used=r.pos;
  return r.ok && r.pos<=len;
}
