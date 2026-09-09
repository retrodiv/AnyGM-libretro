/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Collision query geometry, candidate indexing, and builtin adaptation. */
#include "gml_builtin_internal.h"
#include "anygm_compatibility.h"
#include "gml_render.h"
#include "anygm_host.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* mask bbox (world coords) of an instance placed at (atx,aty), from its sprite margins. */
static int inst_mask_sprite_index(GmlInstance *in){
  return in->mask_index>=0 ? (int)in->mask_index : (int)in->sprite_index;
}
int target_matches_instance(GmlVM *vm, GmlInstance *self, GmlInstance *o, int target){
  if(!o || !o->active || o->marked) return 0;
  if(target==IT_NOONE) return 0;
  if(target==IT_ALL) return 1;
  if(target==IT_SELF) return o==self;
  if(target==IT_OTHER) return o==vm->cur_other;
  if(target>=100000) return (int)o->id==target;
  return target>=0 && gml_object_is(vm,o->obj,target);
}
int inst_bbox(GmlVM *vm, GmlInstance *in, double atx, double aty,
                     double *l, double *t, double *r, double *b){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 0;
  int si=inst_mask_sprite_index(in); GmlRenderSpriteMetrics sprite;
  if(!gml_render_sprite_metrics(R,si,&sprite) ||
     sprite.collision_right<sprite.collision_left ||
     sprite.collision_bottom<sprite.collision_top) return 0;
  double xs=in->image_xscale, ys=in->image_yscale;
  if(fabs(xs)<1e-9 || fabs(ys)<1e-9) return 0;
  if(anygm_policy_round_collision_bounds(vm->win)){
    /* Rounded-bound modes build the inclusive bottom/right corner as top-left plus scaled size
     * minus one, rotate those pixel coordinates, then round each edge to the nearest integer. */
    double x0=(sprite.collision_left-sprite.origin_x)*xs;
    double y0=(sprite.collision_top-sprite.origin_y)*ys;
    double x1=x0+(sprite.collision_right+1.0-sprite.collision_left)*xs-1.0;
    double y1=y0+(sprite.collision_bottom+1.0-sprite.collision_top)*ys-1.0;
    double ang=in->image_angle*M_PI/180.0, c=cos(ang), sn=sin(ang);
    double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
    double corners[4][2]={{x0,y0},{x1,y0},{x0,y1},{x1,y1}};
    for(int i=0;i<4;i++){
      double wx=atx + corners[i][0]*c + corners[i][1]*sn;
      double wy=aty - corners[i][0]*sn + corners[i][1]*c;
      if(wx<minx) minx=wx;
      if(wx>maxx) maxx=wx;
      if(wy<miny) miny=wy;
      if(wy>maxy) maxy=wy;
    }
    *l=gm_round(minx); *t=gm_round(miny); *r=gm_round(maxx); *b=gm_round(maxy);
    return 1;
  }
  if(in->image_angle==0){
    /* unrotated fast path — the overwhelming majority of collision candidates (terrain) sit at
     * angle 0; the generic path costs a cos+sin per candidate per query (many trig calls per frame in a
     * large level: every place_meeting ground probe scans the instance list). */
    double px0=(sprite.collision_left-sprite.origin_x)*xs;
    double px1=(sprite.collision_right+1.0-sprite.origin_x)*xs;
    double py0=(sprite.collision_top-sprite.origin_y)*ys;
    double py1=(sprite.collision_bottom+1.0-sprite.origin_y)*ys;
    double minx = px0<px1? px0:px1, maxx = px0<px1? px1:px0;
    double miny = py0<py1? py0:py1, maxy = py0<py1? py1:py0;
    /* Round both edges at the instance position. The far edge is inclusive, so
     * subtract one after rounding its exclusive coordinate. */
    *l=gm_round(atx+minx); *t=gm_round(aty+miny); *r=gm_round(atx+maxx)-1.0; *b=gm_round(aty+maxy)-1.0; return 1;
  }
  double ang=in->image_angle*M_PI/180.0, c=cos(ang), sn=sin(ang);
  double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
  double x0=sprite.collision_left, x1=sprite.collision_right+1.0;
  double y0=sprite.collision_top, y1=sprite.collision_bottom+1.0;
  double corners[4][2]={{x0,y0},{x1,y0},{x0,y1},{x1,y1}};
  for(int i=0;i<4;i++){
    double px=(corners[i][0]-sprite.origin_x)*xs;
    double py=(corners[i][1]-sprite.origin_y)*ys;
    double wx=atx + px*c + py*sn;
    double wy=aty - px*sn + py*c;
    if(wx<minx) minx=wx;
    if(wx>maxx) maxx=wx;
    if(wy<miny) miny=wy;
    if(wy>maxy) maxy=wy;
  }
  *l=gm_round(minx); *t=gm_round(miny); *r=gm_round(maxx)-1.0; *b=gm_round(maxy)-1.0; return 1;
}
double point_to_instance_distance(GmlVM *vm,GmlInstance *in,double x,double y){
  double l,t,r,b;
  if(!inst_bbox(vm,in,in->x,in->y,&l,&t,&r,&b)) return hypot(x-in->x,y-in->y);
  double dx=x<l?x-l:(x>r?x-r:0.0);
  double dy=y<t?y-t:(y>b?y-b:0.0);
  return hypot(dx,dy);
}
static double instance_to_instance_distance(GmlVM *vm,GmlInstance *a,GmlInstance *b){
  double al,at,ar,ab,bl,bt,br,bb;
  if(!inst_bbox(vm,a,a->x,a->y,&al,&at,&ar,&ab) ||
     !inst_bbox(vm,b,b->x,b->y,&bl,&bt,&br,&bb))
    return hypot(a->x-b->x,a->y-b->y);
  double dx=al>br?al-br:(bl>ar?bl-ar:0.0);
  double dy=at>bb?at-bb:(bt>ab?bt-ab:0.0);
  return hypot(dx,dy);
}
double distance_to_target(GmlVM *vm,GmlInstance *self,int target){
  if(!self) return 0.0;
  /* GM returns this sentinel when the target does not exist (including an explicit reference
   * to self). A negative result is especially harmful because every ordinary upper-bound
   * proximity test then succeeds. */
  double best=1000000.0;
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *other=&vm->inst[i];
    if(other==self || !target_matches_instance(vm,self,other,target)) continue;
    double d=instance_to_instance_distance(vm,self,other);
    if(d<best) best=d;
    if(target>=100000 || target==IT_OTHER) break;
  }
  return best;
}
static int bbox_overlap(double l1,double t1,double r1,double b1, double l2,double t2,double r2,double b2){
  return l1<=r2 && l2<=r1 && t1<=b2 && t2<=b1;
}
static int mask_hit_world(GmlRender *R, GmlInstance *in,
                          const GmlRenderSpriteMetrics *metrics, int sprite,
                          double atx, double aty, int round_origin, int wx, int wy){
  double xs=in->image_xscale, ys=in->image_yscale;
  if(fabs(xs)<1e-9 || fabs(ys)<1e-9) return 0;
  /* Classic formats sample precise masks relative to the integer instance origin while retaining
   * fractional movement. Other rounded-bound modes only round the bounding box. */
  if(round_origin){ atx=gm_round(atx); aty=gm_round(aty); }
  if(in->image_angle==0){   /* unrotated fast path: the generic one pays cos+sin PER PIXEL */
    int lx=(int)floor(((double)wx-atx)/xs + metrics->origin_x);
    int ly=(int)floor(((double)wy-aty)/ys + metrics->origin_y);
    return gml_sprite_collision(R,sprite,(int)in->image_index,lx,ly);
  }
  double ang=in->image_angle*M_PI/180.0, c=cos(ang), sn=sin(ang);
  double rx=(double)wx-atx, ry=(double)wy-aty;
  double sxr=rx*c - ry*sn, syr=rx*sn + ry*c;
  int lx=(int)floor(sxr/xs + metrics->origin_x);
  int ly=(int)floor(syr/ys + metrics->origin_y);
  return gml_sprite_collision(R,sprite,(int)in->image_index,lx,ly);
}
/* Per-pixel (precise) mask overlap of self placed at (sx,sy) vs another
 * instance at its own position, scanned over the bbox intersection. This
 * matters for shaped collision masks such as diagonal slopes. Falls back to
 * "overlap" if a sprite is missing. */
static int masks_overlap(GmlVM *vm, GmlInstance *self, double sx, double sy, GmlInstance *o){
  GmlRender *R=(GmlRender*)vm->render; if(!R) return 1;
  int ss=inst_mask_sprite_index(self), os=inst_mask_sprite_index(o); if(ss<0||os<0) return 1;
  GmlRenderSpriteMetrics sp,op;
  if(!gml_render_sprite_metrics(R,ss,&sp) ||
     !gml_render_sprite_metrics(R,os,&op)) return 1;
  int round_origin=vm->win && anygm_policy_uses_classic_runtime(vm->win);
  double sl,st,sr,sb,ol,ot,orr,ob;
  if(!inst_bbox(vm,self,sx,sy,&sl,&st,&sr,&sb)) return 0;
  if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) return 0;
  /* Rectangles answer from their inclusive bounds, not a second inverse pixel sample that
   * loses a shared edge at fractional coordinates. Precise and rotated pairs retain sampling. */
  if(sp.collision_box && op.collision_box && self->image_angle==0 && o->image_angle==0)
    return bbox_overlap(sl,st,sr,sb,ol,ot,orr,ob);
  int x0=(int)floor(fmax(sl,ol)), x1=(int)ceil(fmin(sr,orr)+1.0);
  int y0=(int)floor(fmax(st,ot)), y1=(int)ceil(fmin(sb,ob)+1.0);
  /* GM bbox coordinates are inclusive. A common grounding probe is place_meeting(x,y+1,...),
   * where the moved mask's bottom row exactly equals the wall's top row. The bbox precheck
   * treats that as overlap, so the mask scan must include that single edge row/column too. */
  if(x1==x0) x1++;
  if(y1==y0) y1++;
  for(int wy=y0; wy<y1; wy++) for(int wx=x0; wx<x1; wx++){
    if(!mask_hit_world(R,self,&sp,ss,sx,sy,round_origin,wx,wy)) continue;
    if( mask_hit_world(R,o,&op,os,o->x,o->y,round_origin,wx,wy)) return 1;
  }
  return 0;
}
/* ---- collision-candidate grid ----------------------------------------------------------------
 * place_meeting and place_free queries can issue many probes per frame. The grid buckets every
 * instance bounding box once per frame, lazily on the first
 * query); a query then tests only the instances overlapping its probe cells PLUS the "overlay":
 * instances whose bbox inputs were written after the build (choke-point hooks: the GML built-in
 * setter and every C-side motion site) and instances created after it. Grid entries are only
 * CANDIDATES — the exact test always uses live data — so a stale entry can produce a redundant
 * check, never a wrong hit; a missed move cannot produce a miss because the mover is in the
 * overlay. GML_NO_COLGRID=1 reverts to the linear scan; GML_DBG_GRIDCHECK=1 runs both paths and
 * reports any divergence. */
void gml_colgrid_invalidate(GmlVM *vm){ if(vm) vm->cg_built_frame=-1; }
void gml_colgrid_touch(GmlVM *vm, GmlInstance *in){
  if(!vm || !in) return;
  
  if(vm->cg_built_frame!=vm->frame) return;                    /* no valid build to patch */
  if(in<vm->inst || in>=vm->inst+vm->inst_cap){ vm->cg_built_frame=-1; return; }  /* foreign VM */
  if(in->cg_touch==vm->cg_gen) return;                          /* already in the overlay */
  in->cg_touch=vm->cg_gen;
  if(vm->cg_overlay_n>=vm->cg_overlay_cap){
    int nc=vm->cg_overlay_cap? vm->cg_overlay_cap*2 : 256;
    int *p=realloc(vm->cg_overlay,nc*sizeof(int)); if(!p){ vm->cg_built_frame=-1; return; }
    vm->cg_overlay=p; vm->cg_overlay_cap=nc;
  }
  vm->cg_overlay[vm->cg_overlay_n++]=(int)(in-vm->inst);
}
static void cg_cell_range(GmlVM *vm, double l, double t, double r, double b,
                          int *cx0,int *cy0,int *cx1,int *cy1){
  /* Clamp both ends of both axes. Content can place instances entirely outside the grid. If only
   * the lower bound is clamped, the degenerate-range repair can move an endpoint out of range and
   * make the fill pass write through an invalid offset. Out-of-grid boxes collapse
   * to the border cells, which out-of-grid probes also clamp to, so they still meet. */
  int x0=(int)floor((l-vm->cg_ox)/vm->cg_cell), y0=(int)floor((t-vm->cg_oy)/vm->cg_cell);
  int x1=(int)floor((r-vm->cg_ox)/vm->cg_cell), y1=(int)floor((b-vm->cg_oy)/vm->cg_cell);
  if(x0<0) x0=0;
  if(x0>=vm->cg_cw) x0=vm->cg_cw-1;
  if(y0<0) y0=0;
  if(y0>=vm->cg_ch) y0=vm->cg_ch-1;
  if(x1<0) x1=0;
  if(x1>=vm->cg_cw) x1=vm->cg_cw-1;
  if(y1<0) y1=0;
  if(y1>=vm->cg_ch) y1=vm->cg_ch-1;
  if(x1<x0) x1=x0;
  if(y1<y0) y1=y0;
  *cx0=x0;*cy0=y0;*cx1=x1;*cy1=y1;
}
static int cg_build(GmlVM *vm){
  
  GmlRoom rm; double rw=2048,rh=2048;
  if(gml_vm_room_get(vm,vm->room_index,&rm)==0){ rw=rm.width; rh=rm.height; }
  vm->cg_ox=-512; vm->cg_oy=-512;
  double span=fmax(rw,rh)+1024;
  vm->cg_cell=64; while(span/vm->cg_cell>128) vm->cg_cell*=2;
  vm->cg_cw=(int)(span/vm->cg_cell)+1; vm->cg_ch=(int)(span/vm->cg_cell)+1;
  int ncell=vm->cg_cw*vm->cg_ch;
  if(!vm->cg_off || vm->cg_off_cap<ncell+1){ free(vm->cg_off);
    vm->cg_off=malloc((ncell+1)*sizeof(int)); vm->cg_off_cap=ncell+1;
    if(!vm->cg_off){ vm->cg_off_cap=0; return 0; } }
  memset(vm->cg_off,0,(ncell+1)*sizeof(int));
  int n=vm->inst_count;
  /* pass 1: count cell spans (skip dead slots; deactivated stay in — they may re-activate
   * mid-frame and the query filters on live flags anyway) */
  for(int i=0;i<n;i++){ GmlInstance *o=&vm->inst[i];
    if(!o->active && !o->deactivated) continue;
    double l,t,r,b; if(!inst_bbox(vm,o,o->x,o->y,&l,&t,&r,&b)) continue;
    int x0,y0,x1,y1; cg_cell_range(vm,l,t,r,b,&x0,&y0,&x1,&y1);
    for(int cy=y0;cy<=y1;cy++) for(int cx=x0;cx<=x1;cx++) vm->cg_off[cy*vm->cg_cw+cx+1]++;
  }
  for(int c=0;c<ncell;c++) vm->cg_off[c+1]+=vm->cg_off[c];
  int total=vm->cg_off[ncell];
  if(total>vm->cg_items_cap){ free(vm->cg_items);
    vm->cg_items=malloc((total>256?total:256)*sizeof(int));
    vm->cg_items_cap=total>256?total:256;
    if(!vm->cg_items){ free(vm->cg_off); vm->cg_off=NULL; vm->cg_off_cap=0; return 0; } }
  /* pass 2: fill (cursor = shifted offsets) */
  int *cur=malloc((ncell?ncell:1)*sizeof(int)); if(!cur) return 0;
  memcpy(cur,vm->cg_off,ncell*sizeof(int));
  for(int i=0;i<n;i++){ GmlInstance *o=&vm->inst[i];
    o->cg_touch=0; o->cg_visit=0;
    if(!o->active && !o->deactivated) continue;
    double l,t,r,b; if(!inst_bbox(vm,o,o->x,o->y,&l,&t,&r,&b)) continue;
    int x0,y0,x1,y1; cg_cell_range(vm,l,t,r,b,&x0,&y0,&x1,&y1);
    for(int cy=y0;cy<=y1;cy++) for(int cx=x0;cx<=x1;cx++) vm->cg_items[cur[cy*vm->cg_cw+cx]++]=i;
  }
  free(cur);
  vm->cg_overlay_n=0;
  vm->cg_gen++; if(vm->cg_gen<=0) vm->cg_gen=1;
  vm->cg_built_frame=vm->frame;
  return 1;
}
/* family-alive shortcut: a query targeting an object class with NO living family member can
 * only return "nothing" — skip the scan. Only for plain object targets (not all/other/id).
 * Under GML_DBG_GRIDCHECK the skipped scan still runs and screams if it finds anything. */
static int obj_family_absent(GmlVM *vm, int obj){
  return obj>=0 && obj<vm->n_objects && vm->obj_alive && vm->obj_alive[obj]==0;
}
static int cg_ensure(GmlVM *vm){
  
  if(vm->cg_built_frame==vm->frame){
    /* Self-tuning: the overlay (instances touched since the build) is appended to EVERY query's
     * candidates regardless of location. A bomb blast turns ~150 debris into per-frame movers,
     * so each of their many pixel-step probes paid the whole overlay again. Once it outgrows a
     * small bound, re-snapshot the grid mid-frame (~tens of µs, correct by construction: it
     * captures live positions) and the overlay drains back to zero. */
    if(vm->cg_overlay_n > 96) return cg_build(vm);
    return 1;
  }
  return cg_build(vm);
}
/* Collect the candidate slot indices (grid cells overlapping the probe + the touched overlay),
 * deduped and sorted ascending so callers preserve the linear scan's first-match-by-slot-order
 * semantics. Returns count, or -1 when the grid is unusable (caller falls back to linear). */
int gml_colgrid_collect(GmlVM *vm, double l, double t, double r, double b, int **out){
  /* Candidates are gathered into a per-slot BITSET and emitted by scanning set bits upward:
   * ascending slot order (the linear scan's event/return order) and dedupe come for free.
   * The first version qsort()ed the list per query — with an exploding bomb (~150 collision
   * sources x ~320 candidates each) the comparator-callback sort alone burned more than the
   * pruning saved. */
  if(gml_colgrid_mode(vm)==0) return -1;
  if(!cg_ensure(vm)) return -1;
  int words=(vm->inst_count+63)/64;
  if(words>vm->cg_candidate_bits_words){
    uint64_t *p=realloc(vm->cg_candidate_bits,(size_t)words*sizeof(*p));
    if(!p) return -1;
    vm->cg_candidate_bits=p; vm->cg_candidate_bits_words=words;
  }
  memset(vm->cg_candidate_bits,0,(size_t)words*sizeof(*vm->cg_candidate_bits));
  int x0,y0,x1,y1; cg_cell_range(vm,l,t,r,b,&x0,&y0,&x1,&y1);
  for(int cy=y0;cy<=y1;cy++) for(int cx=x0;cx<=x1;cx++){
    int c=cy*vm->cg_cw+cx;
    for(int k=vm->cg_off[c];k<vm->cg_off[c+1];k++){
      int i=vm->cg_items[k];
      if(i<vm->inst_count) vm->cg_candidate_bits[i>>6] |= 1ull<<(i&63);
    }
  }
  for(int k2=0;k2<vm->cg_overlay_n;k2++){
    int i=vm->cg_overlay[k2];
    if(i<vm->inst_count) vm->cg_candidate_bits[i>>6] |= 1ull<<(i&63);
  }
  int n=0;
  for(int w=0;w<words;w++){
    uint64_t m=vm->cg_candidate_bits[w];
    while(m){
      int bit=__builtin_ctzll(m); m&=m-1;
      int i=(w<<6)|bit;
      if(n>=vm->cg_candidate_cap){
        int nc=vm->cg_candidate_cap?vm->cg_candidate_cap*2:256;
        int *p=realloc(vm->cg_candidate,(size_t)nc*sizeof(*p)); if(!p) return -1;
        vm->cg_candidate=p; vm->cg_candidate_cap=nc;
      }
      vm->cg_candidate[n++]=i;
    }
  }
  *out=vm->cg_candidate;
  return n;
}
/* solid=1 -> test only solid instances (place_free); else test instances matching obj. */
static GmlInstance *collision_instance_at_linear(GmlVM *vm, double x, double y, int obj, int solid_only){
  GmlInstance *self=vm->cur_self; if(!self) return 0;
  double sl,st,sr,sb; if(!inst_bbox(vm,self,x,y,&sl,&st,&sr,&sb)) return 0;
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
    if(o==self || !o->active || o->marked) continue;
    if(solid_only){ if(o->solid<0.5) continue; }
    else if(!target_matches_instance(vm,self,o,obj)) continue;
    double ol,ot,orr,ob; if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) continue;
    if(bbox_overlap(sl,st,sr,sb, ol,ot,orr,ob) && masks_overlap(vm,self,x,y,o)) return o;
  }
  return 0;
}
static int cg_candidate_hit(GmlVM *vm, GmlInstance *self, GmlInstance *o,
                            double x, double y, int obj, int solid_only,
                            double sl,double st,double sr,double sb){
  if(o==self || !o->active || o->marked) return 0;
  if(solid_only){ if(o->solid<0.5) return 0; }
  else if(!target_matches_instance(vm,self,o,obj)) return 0;
  double ol,ot,orr,ob; if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) return 0;
  return bbox_overlap(sl,st,sr,sb, ol,ot,orr,ob) && masks_overlap(vm,self,x,y,o);
}
typedef struct { int idx; double d2; } GmlColListHit;
static int col_list_cmp(const void *aa, const void *bb){
  const GmlColListHit *a=(const GmlColListHit*)aa, *b=(const GmlColListHit*)bb;
  if(a->d2 < b->d2) return -1;
  if(a->d2 > b->d2) return 1;
  return a->idx - b->idx;
}
static int col_list_add(GmlVM *vm, GmlDSList *list, int ordered,
                        GmlColListHit **hits, int *nhit, int *cap,
                        int idx, double x, double y){
  if(!ordered){
    if(list) ds_list_push(list,vreal((double)vm->inst[idx].id));
    (*nhit)++;
    return 1;
  }
  if(*nhit>=*cap){
    int nc=*cap?*cap*2:16;
    GmlColListHit *p=realloc(*hits,(size_t)nc*sizeof(*p));
    if(!p) return 0;
    *hits=p; *cap=nc;
  }
  double dx=vm->inst[idx].x-x, dy=vm->inst[idx].y-y;
  (*hits)[*nhit]=(GmlColListHit){ idx, dx*dx+dy*dy };
  (*nhit)++;
  return 1;
}
int collision_instance_list_at(GmlVM *vm, double x, double y, int obj, GmlDSList *list, int ordered){
  if(obj_family_absent(vm,obj)) return 0;
  GmlInstance *self=vm->cur_self; if(!self) return 0;
  double sl,st,sr,sb; if(!inst_bbox(vm,self,x,y,&sl,&st,&sr,&sb)) return 0;
  GmlColListHit *hits=NULL; int nhit=0, cap=0;
  int *cand=NULL, cn=-1;
  if(gml_colgrid_mode(vm)!=0)
    cn=gml_colgrid_collect(vm,sl,st,sr,sb,&cand);
  if(cn>=0){
    for(int c=0;c<cn;c++){
      int i=cand[c]; if(i<0 || i>=vm->inst_count) continue;
      if(cg_candidate_hit(vm,self,&vm->inst[i],x,y,obj,0,sl,st,sr,sb) &&
         !col_list_add(vm,list,ordered,&hits,&nhit,&cap,i,x,y)) break;
    }
  } else {
    for(int i=0;i<vm->inst_count;i++){
      if(cg_candidate_hit(vm,self,&vm->inst[i],x,y,obj,0,sl,st,sr,sb) &&
         !col_list_add(vm,list,ordered,&hits,&nhit,&cap,i,x,y)) break;
    }
  }
  if(ordered && hits){
    qsort(hits,(size_t)nhit,sizeof(*hits),col_list_cmp);
    if(list) for(int i=0;i<nhit;i++) ds_list_push(list,vreal((double)vm->inst[hits[i].idx].id));
  }
  free(hits);
  return nhit;
}
GmlInstance *collision_instance_at(GmlVM *vm, double x, double y, int obj, int solid_only){
  int cg_mode=gml_colgrid_mode(vm);   /* 0 = linear, 1 = grid, 2 = grid plus verification */
  if(!solid_only && obj_family_absent(vm,obj)){
    if(cg_mode==2){ GmlInstance *lin=collision_instance_at_linear(vm,x,y,obj,solid_only);
      if(lin){  anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gridcheck] MISMATCH alive-count f%ld obj=%d found id=%u\n",vm->frame,obj,lin->id); return lin; } }
    return NULL;
  }
  if(cg_mode==0) return collision_instance_at_linear(vm,x,y,obj,solid_only);
  GmlInstance *self=vm->cur_self; if(!self) return 0;
  
  if(vm->cg_built_frame!=vm->frame){
    if(!cg_build(vm)) return collision_instance_at_linear(vm,x,y,obj,solid_only);
  }
  double sl,st,sr,sb; if(!inst_bbox(vm,self,x,y,&sl,&st,&sr,&sb)) return 0;
  /* result keeps the LINEAR scan's semantics: lowest slot index wins (instance_place returns
   * "an instance" — keep it deterministic and identical to the old order). */
  int best=-1;
  int x0,y0,x1,y1; cg_cell_range(vm,sl,st,sr,sb,&x0,&y0,&x1,&y1);
  int vgen=++vm->cg_vgen; if(vm->cg_vgen<=0) vgen=vm->cg_vgen=1;
  for(int cy=y0;cy<=y1;cy++) for(int cx=x0;cx<=x1;cx++){
    int c=cy*vm->cg_cw+cx;
    for(int k=vm->cg_off[c];k<vm->cg_off[c+1];k++){
      int i=vm->cg_items[k]; GmlInstance *o=&vm->inst[i];
      if(o->cg_visit==vgen) continue;
      o->cg_visit=vgen;
      if(best>=0 && i>=best) continue;
      if(cg_candidate_hit(vm,self,o,x,y,obj,solid_only,sl,st,sr,sb)) best=i;
    }
  }
  for(int k2=0;k2<vm->cg_overlay_n;k2++){
    int i=vm->cg_overlay[k2]; GmlInstance *o=&vm->inst[i];
    if(o->cg_visit==vgen) continue;
    o->cg_visit=vgen;
    if(best>=0 && i>=best) continue;
    if(cg_candidate_hit(vm,self,o,x,y,obj,solid_only,sl,st,sr,sb)) best=i;
  }
  GmlInstance *res = best>=0 ? &vm->inst[best] : NULL;
  if(cg_mode==2){
    GmlInstance *lin=collision_instance_at_linear(vm,x,y,obj,solid_only);
    if(lin!=res){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gridcheck] MISMATCH f%ld probe=(%.2f,%.2f) obj=%d solid=%d grid=%d linear=%d\n",
        vm->frame,x,y,obj,solid_only,best,lin?(int)(lin-vm->inst):-1);
      res=lin; }   /* trust the reference path when verifying */
  }
  if(res && builtin_setting(vm,"GML_LOG_COL_HIT")){
    const char *self_name=(self->obj>=0&&self->obj<vm->n_objects)?vm->objects[self->obj].name:"?";
    const char *hit_name=(res->obj>=0&&res->obj<vm->n_objects)?vm->objects[res->obj].name:"?";
    const char *target_name=(obj>=0&&obj<vm->n_objects)?vm->objects[obj].name:"?";
    anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[col-hit] %s/%u probe=(%.1f,%.1f) target=%s/%d -> %s/%u @ (%.1f,%.1f)\n",
      self_name?self_name:"?",self->id,x,y,target_name?target_name:"?",obj,
      hit_name?hit_name:"?",res->id,res->x,res->y);
  }
  return res;
}
int collision_at(GmlVM *vm, double x, double y, int obj, int solid_only){
  return collision_instance_at(vm,x,y,obj,solid_only)!=NULL;
}
/* Classic bounce queries leave the instance at its pre-contact position.  The basic
 * variant reflects axis contacts and falls back to a diagonal reflection; the advanced
 * variant samples the free directions on both sides in 10-degree steps. */
void classic_move_bounce(GmlVM *vm, GmlInstance *s, int all, int advanced){
  int target=all?IT_ALL:0, solid_only=!all;
  double bx=s->x, by=s->y;
  int bounced=collision_at(vm,bx,by,target,solid_only);
  if(bounced){ bx=s->xprevious; by=s->yprevious; }

  if(advanced){
    double start=gm_round(s->direction/10.0)*10.0;
    double clockwise=start, counterclockwise=start;
    for(int step=0;step<36;step++){
      clockwise-=10.0;
      double radians=clockwise*M_PI/180.0;
      if(!collision_at(vm,bx+s->speed*cos(radians),by-s->speed*sin(radians),
                       target,solid_only)) break;
      bounced=1;
    }
    for(int step=0;step<36;step++){
      counterclockwise+=10.0;
      double radians=counterclockwise*M_PI/180.0;
      if(!collision_at(vm,bx+s->speed*cos(radians),by-s->speed*sin(radians),
                       target,solid_only)) break;
      bounced=1;
    }
    if(bounced){
      s->direction=clockwise+counterclockwise+180.0-start;
      motion_from_speed_direction(vm,s);
    }
  } else {
    double hs=s->hspeed, vs=s->vspeed;
    int horizontal=collision_at(vm,bx+hs,by,target,solid_only);
    int vertical=collision_at(vm,bx,by+vs,target,solid_only);
    if(horizontal) s->hspeed=-hs;
    if(vertical) s->vspeed=-vs;
    if(!horizontal && !vertical &&
       collision_at(vm,bx+hs,by+vs,target,solid_only)){
      s->hspeed=-hs;
      s->vspeed=-vs;
      horizontal=vertical=1;
    }
    if(horizontal||vertical) motion_from_components(s);
  }

  if(s->x!=bx || s->y!=by){
    s->x=bx;
    s->y=by;
    gml_colgrid_touch(vm,s);
  }
}
/* Keep the distance-limited step shared; collision selection changes only its veto. */
static int linear_step_move(GmlVM *vm,GmlInstance *self,double tx,double ty,
                            double step,int target,int solid_only){
  if(!self || !isfinite(tx) || !isfinite(ty) || !isfinite(step) || step<0)
    return 0;
  double dx=tx-self->x,dy=ty-self->y,distance=hypot(dx,dy);
  if(!isfinite(distance)) return 0;
  if(distance==0) return 1;
  if(step==0) return 0;
  int arrived=distance<=step || distance<1e-9;
  double x=arrived?tx:self->x+dx/distance*step;
  double y=arrived?ty:self->y+dy/distance*step;
  if(collision_at(vm,x,y,target,solid_only)) return 0;
  self->x=x;
  self->y=y;
  gml_colgrid_touch(vm,self);
  return arrived;
}

static double potential_dir_norm(double d){
  d=fmod(d,360.0); return d<0?d+360.0:d;
}
/* Potential motion keeps the current heading within the configured turn cone,
 * probes farther along each candidate ray, then commits exactly one step. */
int potential_step_move(GmlVM *vm, GmlInstance *s, double tx, double ty,
                               double amount, int target, int solid_only){
  double ox=s->x, oy=s->y, dx=tx-ox, dy=ty-oy, distance=hypot(dx,dy);
  if(distance<1e-12) return 1;
  double desired=potential_dir_norm(-atan2(dy,dx)*180.0/M_PI);
  if(distance<=amount){
    if(collision_at(vm,tx,ty,target,solid_only)) return 0;
    s->x=tx; s->y=ty; s->direction=desired; gml_colgrid_touch(vm,s); return 1;
  }
  double current=potential_dir_norm(s->direction);
  double rotate=fabs(vm->potential_rotate_step);
  int attempts=0;
  for(int ring=0;attempts<4096;ring++){
    double mag=ring*rotate;
    if(ring>0 && (rotate<=0 || mag>=180.0)) break;
    int sides=ring?2:1;
    for(int side=0;side<sides && attempts<4096;side++,attempts++){
      double offset=ring?(side? -mag:mag):0;
      double candidate=potential_dir_norm(desired-offset);
      double change=potential_dir_norm(candidate-current);
      double maxrot=vm->potential_max_rotation;
      if(change>maxrot && change<360.0-maxrot) continue;
      double rad=candidate*M_PI/180.0;
      double ux=cos(rad), uy=-sin(rad);
      double ax=ox+ux*amount*vm->potential_check_distance;
      double ay=oy+uy*amount*vm->potential_check_distance;
      if(collision_at(vm,ax,ay,target,solid_only)) continue;
      double nx=ox+ux*amount, ny=oy+uy*amount;
      if(collision_at(vm,nx,ny,target,solid_only)) continue;
      s->x=nx; s->y=ny; s->direction=candidate; gml_colgrid_touch(vm,s); return 0;
    }
    if(rotate<=0) break;
  }
  if(vm->potential_rotate_on_spot)
    s->direction=potential_dir_norm(current+vm->potential_max_rotation);
  return 0;
}
int instance_region_hit(GmlVM *vm, GmlInstance *o, double rx, double ry, double rw, double rh){
  double rl=rx, rt=ry, rr=rx+rw, rb=ry+rh;
  if(rr<rl){ double t=rl; rl=rr; rr=t; }
  if(rb<rt){ double t=rt; rt=rb; rb=t; }
  double l,t,r,b;
  if(inst_bbox(vm,o,o->x,o->y,&l,&t,&r,&b))
    return bbox_overlap(l,t,r,b,rl,rt,rr,rb);
  return o->x>=rl && o->x<=rr && o->y>=rt && o->y<=rb;
}
void snap_contact_axis(GmlVM *vm, GmlInstance *s, double dx, double dy){
  double nx=s->x, ny=s->y;
  if(fabs(dx)<1e-9 && fabs(dy)>0.999999)
    ny = dy>0 ? floor(s->y+1e-9) : ceil(s->y-1e-9);
  else if(fabs(dy)<1e-9 && fabs(dx)>0.999999)
    nx = dx>0 ? floor(s->x+1e-9) : ceil(s->x-1e-9);
  else
    return;
  if(!collision_at(vm,nx,ny,0,1)){ s->x=nx; s->y=ny; gml_colgrid_touch(vm,s); }
}
int resolve_landing_overlap(GmlVM *vm, GmlInstance *s, int md){
  if(s->vspeed<=0) return 0;
  double ox=s->x, oy=s->y;
  int limit=md + (int)ceil(fabs(s->vspeed)) + 2;
  if(limit<1) limit=1;
  for(int k=0;k<=limit;k++){
    if(!collision_at(vm,s->x,s->y,0,1)){
      double free_y=s->y, hit_y=s->y+1.0;
      for(int i=0;i<10;i++){
        double mid=(free_y+hit_y)*0.5;
        if(collision_at(vm,s->x,mid,0,1)) hit_y=mid;
        else free_y=mid;
      }
      s->y=free_y; gml_colgrid_touch(vm,s);
      return 1;
    }
    s->y-=1.0; gml_colgrid_touch(vm,s);
  }
  s->x=ox; s->y=oy; gml_colgrid_touch(vm,s);
  return 0;
}

/* first active instance of `obj` whose mask covers world point (px,py), or NULL. */
static int point_hits_instance_prec(GmlVM *vm, GmlInstance *o, double px, double py, int obj, GmlInstance *skip, int precise){
  GmlRender *R=(GmlRender*)vm->render;
  if(o==skip) return 0;
  if(!target_matches_instance(vm,vm->cur_self,o,obj)) return 0;
  double ol,ot,orr,ob; if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) return 0;
  if(px<ol||px>orr||py<ot||py>ob) return 0;
  if(!precise) return 1;
  /* bbox hit; refine with the per-pixel mask when available */
  if(R){ int si=inst_mask_sprite_index(o);
    GmlRenderSpriteMetrics sprite;
    if(gml_render_sprite_metrics(R,si,&sprite)){
      if(!mask_hit_world(R,o,&sprite,si,o->x,o->y,vm->win&&anygm_policy_uses_classic_runtime(vm->win),
                         (int)floor(px),(int)floor(py))) return 0; } }
  return 1;
}
static int point_hits_instance(GmlVM *vm, GmlInstance *o, double px, double py, int obj, GmlInstance *skip){
  return point_hits_instance_prec(vm,o,px,py,obj,skip,1);
}
static GmlInstance *instance_at_point_ex_linear(GmlVM *vm, double px, double py, int obj, GmlInstance *skip){
  for(int i=0;i<vm->inst_count;i++){ GmlInstance *o=&vm->inst[i];
    if(point_hits_instance(vm,o,px,py,obj,skip)) return o; }
  return NULL;
}
static GmlInstance *instance_at_point_ex(GmlVM *vm, double px, double py, int obj, GmlInstance *skip){
  int mode=gml_colgrid_mode(vm); int *cand=NULL; int n;
  if(obj_family_absent(vm,obj)){
    if(mode==2){ GmlInstance *lin=instance_at_point_ex_linear(vm,px,py,obj,skip);
      if(lin){  anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gridcheck] MISMATCH alive-count(point) f%ld obj=%d id=%u\n",vm->frame,obj,lin->id); return lin; } }
    return NULL;
  }
  if(mode==0 || (n=gml_colgrid_collect(vm,px,py,px,py,&cand))<0)
    return instance_at_point_ex_linear(vm,px,py,obj,skip);
  GmlInstance *res=NULL;
  for(int k=0;k<n;k++){ GmlInstance *o=&vm->inst[cand[k]];
    if(point_hits_instance(vm,o,px,py,obj,skip)){ res=o; break; } }
  if(mode==2){ GmlInstance *lin=instance_at_point_ex_linear(vm,px,py,obj,skip);
    if(lin!=res){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gridcheck] MISMATCH point f%ld (%.2f,%.2f) obj=%d grid=%d linear=%d\n",
        vm->frame,px,py,obj,res?(int)(res-vm->inst):-1,lin?(int)(lin-vm->inst):-1);
      res=lin; } }
  return res;
}
GmlInstance *instance_at_point(GmlVM *vm, double px, double py, int obj){
  return instance_at_point_ex(vm,px,py,obj,NULL);
}
static int line_hits_instance(GmlVM *vm, GmlInstance *o,
                              double x1, double y1, double x2, double y2,
                              int obj, GmlInstance *skip, int precise, int steps,
                              double *first_d2){
  if(!o || !o->active || o->marked) return 0;
  for(int k=0;k<=steps;k++){
    double t=(double)k/(double)steps;
    double px=x1+(x2-x1)*t, py=y1+(y2-y1)*t;
    if(point_hits_instance_prec(vm,o,px,py,obj,skip,precise)){
      if(first_d2){
        double dx=px-x1, dy=py-y1;
        *first_d2=dx*dx+dy*dy;
      }
      return 1;
    }
  }
  return 0;
}
GmlInstance *collision_line_query(GmlVM *vm, double x1, double y1, double x2, double y2,
                                         int obj, int precise, int notme){
  int steps=(int)ceil(fmax(fabs(x2-x1),fabs(y2-y1))); if(steps<1) steps=1;
  GmlInstance *skip=notme?vm->cur_self:NULL;
  if(obj_family_absent(vm,obj) && gml_colgrid_mode(vm)!=2) return NULL;
  int *cand=NULL; int cn=-1;
  if(gml_colgrid_mode(vm)!=0)
    cn=gml_colgrid_collect(vm, fmin(x1,x2), fmin(y1,y2), fmax(x1,x2), fmax(y1,y2), &cand);
  GmlInstance *hit=NULL;
  for(int k=0;k<=steps && !hit;k++){
    double t=(double)k/(double)steps, px=x1+(x2-x1)*t, py=y1+(y2-y1)*t;
    if(cn>=0){
      for(int c=0;c<cn;c++){ GmlInstance *o=&vm->inst[cand[c]];
        if(point_hits_instance_prec(vm,o,px,py,obj,skip,precise)){ hit=o; break; } }
    } else {
      for(int i=0;i<vm->inst_count;i++){
        GmlInstance *o=&vm->inst[i];
        if(point_hits_instance_prec(vm,o,px,py,obj,skip,precise)){ hit=o; break; }
      }
    }
  }
  if(gml_colgrid_mode(vm)==2){
    GmlInstance *lin=NULL;
    for(int k=0;k<=steps && !lin;k++){
      double t=(double)k/(double)steps, px=x1+(x2-x1)*t, py=y1+(y2-y1)*t;
      for(int i=0;i<vm->inst_count;i++){
        GmlInstance *o=&vm->inst[i];
        if(point_hits_instance_prec(vm,o,px,py,obj,skip,precise)){ lin=o; break; }
      }
    }
    if(lin!=hit){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gridcheck] MISMATCH line f%ld (%.1f,%.1f)-(%.1f,%.1f) obj=%d grid=%d linear=%d\n",
        vm->frame,x1,y1,x2,y2,obj,hit?(int)(hit-vm->inst):-1,lin?(int)(lin-vm->inst):-1);
      hit=lin;
    }
  }
  return hit;
}
int collision_line_list_query(GmlVM *vm, double x1, double y1, double x2, double y2,
                                     int obj, int precise, int notme, GmlDSList *list, int ordered){
  int steps=(int)ceil(fmax(fabs(x2-x1),fabs(y2-y1))); if(steps<1) steps=1;
  GmlInstance *skip=notme?vm->cur_self:NULL;
  if(obj_family_absent(vm,obj) && gml_colgrid_mode(vm)!=2) return 0;
  int *cand=NULL; int cn=-1;
  if(gml_colgrid_mode(vm)!=0)
    cn=gml_colgrid_collect(vm, fmin(x1,x2), fmin(y1,y2), fmax(x1,x2), fmax(y1,y2), &cand);
  GmlColListHit *hits=NULL; int nhit=0, cap=0;
  if(cn>=0){
    for(int c=0;c<cn;c++){
      int i=cand[c]; if(i<0 || i>=vm->inst_count) continue;
      double d2=0;
      if(line_hits_instance(vm,&vm->inst[i],x1,y1,x2,y2,obj,skip,precise,steps,&d2)){
        if(!col_list_add(vm,ordered?NULL:list,ordered,&hits,&nhit,&cap,i,x1,y1)) break;
        if(ordered && nhit>0) hits[nhit-1].d2=d2;
      }
    }
  } else {
    for(int i=0;i<vm->inst_count;i++){
      double d2=0;
      if(line_hits_instance(vm,&vm->inst[i],x1,y1,x2,y2,obj,skip,precise,steps,&d2)){
        if(!col_list_add(vm,ordered?NULL:list,ordered,&hits,&nhit,&cap,i,x1,y1)) break;
        if(ordered && nhit>0) hits[nhit-1].d2=d2;
      }
    }
  }
  if(ordered && hits){
    qsort(hits,(size_t)nhit,sizeof(*hits),col_list_cmp);
    if(list) for(int i=0;i<nhit;i++) ds_list_push(list,vreal((double)vm->inst[hits[i].idx].id));
  }
  free(hits);
  return nhit;
}
static int shape_hits_instance(GmlVM *vm, GmlInstance *o, int kind, double *p, int obj,
                               GmlInstance *skip, int precise,
                               double sl,double st,double sr,double sb){
  GmlRender *R=(GmlRender*)vm->render;
  if(o==skip || !target_matches_instance(vm,vm->cur_self,o,obj)) return 0;
  double ol,ot,orr,ob; if(!inst_bbox(vm,o,o->x,o->y,&ol,&ot,&orr,&ob)) return 0;
  if(!bbox_overlap(sl,st,sr,sb,ol,ot,orr,ob)) return 0;
  int x0=(int)floor(fmax(sl,ol)), x1=(int)ceil(fmin(sr,orr)+1.0);
  int y0=(int)floor(fmax(st,ot)), y1=(int)ceil(fmin(sb,ob)+1.0);
  if(x1==x0) x1++;
  if(y1==y0) y1++;
  for(int wy=y0; wy<y1; wy++) for(int wx=x0; wx<x1; wx++){
    if(kind==2){ double dx=wx-p[0], dy=wy-p[1]; if(dx*dx+dy*dy>p[2]*p[2]) continue; }
    /* An ellipse inscribed in the rectangle the caller gave, which is what collision_ellipse
     * means: centre at the midpoint, radii at half the extent, and a point inside when the
     * normalised distance is at most one. A zero-width or zero-height rectangle has no inside,
     * and dividing by it would answer "everything" rather than "nothing". */
    if(kind==3){
      double cx=(p[0]+p[2])*0.5, cy=(p[1]+p[3])*0.5;
      double rx=fabs(p[2]-p[0])*0.5, ry=fabs(p[3]-p[1])*0.5;
      if(rx<=0.0 || ry<=0.0) continue;
      double nx=(wx-cx)/rx, ny=(wy-cy)/ry;
      if(nx*nx+ny*ny>1.0) continue;
    }
    if(precise && R){ int si=inst_mask_sprite_index(o);
      GmlRenderSpriteMetrics sprite;
      if(gml_render_sprite_metrics(R,si,&sprite) &&
         !mask_hit_world(R,o,&sprite,si,o->x,o->y,
                         vm->win&&anygm_policy_uses_classic_runtime(vm->win),wx,wy)) continue; }
    return 1;
  }
  return 0;
}
static void shape_bounds(int kind, double *p, double *sl,double *st,double *sr,double *sb){
  if(kind==0){ *sl=p[0]; *st=p[1]; *sr=p[0]; *sb=p[1]; }
  /* A rectangle and an inscribed ellipse are bounded by the same rectangle: the ellipse is
   * narrowed by the per-point test above, not here. */
  else if(kind==1||kind==3){ *sl=fmin(p[0],p[2]); *st=fmin(p[1],p[3]); *sr=fmax(p[0],p[2]); *sb=fmax(p[1],p[3]); }
  else { *sl=p[0]-p[2]; *st=p[1]-p[2]; *sr=p[0]+p[2]; *sb=p[1]+p[2]; }
}
static GmlInstance *collision_shape_linear(GmlVM *vm, int kind, double *p, int obj,
                                            int precise, int notme){
  GmlInstance *skip=notme?vm->cur_self:NULL;
  double sl,st,sr,sb; shape_bounds(kind,p,&sl,&st,&sr,&sb);
  for(int i=0;i<vm->inst_count;i++)
    if(shape_hits_instance(vm,&vm->inst[i],kind,p,obj,skip,precise,sl,st,sr,sb)) return &vm->inst[i];
  return NULL;
}
GmlInstance *collision_shape(GmlVM *vm, int kind, double *p, int obj,
                                    int precise, int notme){
  GmlInstance *skip=notme?vm->cur_self:NULL;
  double sl,st,sr,sb; shape_bounds(kind,p,&sl,&st,&sr,&sb);
  int mode=gml_colgrid_mode(vm); int *cand=NULL; int n;
  if(obj_family_absent(vm,obj)){
    if(mode==2){ GmlInstance *lin=collision_shape_linear(vm,kind,p,obj,precise,notme);
      if(lin){  anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gridcheck] MISMATCH alive-count(shape) f%ld obj=%d id=%u\n",vm->frame,obj,lin->id); return lin; } }
    return NULL;
  }
  if(mode==0 || (n=gml_colgrid_collect(vm,sl,st,sr,sb,&cand))<0)
    return collision_shape_linear(vm,kind,p,obj,precise,notme);
  GmlInstance *res=NULL;
  for(int k=0;k<n;k++){ GmlInstance *o=&vm->inst[cand[k]];
    if(shape_hits_instance(vm,o,kind,p,obj,skip,precise,sl,st,sr,sb)){ res=o; break; } }
  if(mode==2){ GmlInstance *lin=collision_shape_linear(vm,kind,p,obj,precise,notme);
    if(lin!=res){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[gridcheck] MISMATCH shape f%ld kind=%d obj=%d grid=%d linear=%d\n",
        vm->frame,kind,obj,res?(int)(res-vm->inst):-1,lin?(int)(lin-vm->inst):-1);
      res=lin; } }
  return res;
}
static int collision_target_value_matches(GmlVM *vm, GmlInstance *instance,
                                          GmlVal target, int depth){
  if(target.t==V_ARR && target.arr && depth<32){
    GmlArr *array=target.arr;
    for(int i=0;i<array->len;i++)
      if(collision_target_value_matches(vm,instance,array->data[i],depth+1)) return 1;
    return 0;
  }
  if(target.t!=V_REAL) return 0;
  return target_matches_instance(vm,vm->cur_self,instance,(int)target.d);
}
static GmlInstance *collision_shape_value_linear(GmlVM *vm, int kind, double *p,
                                                  GmlVal target, int precise, int notme){
  GmlInstance *skip=notme?vm->cur_self:NULL;
  double sl,st,sr,sb; shape_bounds(kind,p,&sl,&st,&sr,&sb);
  for(int i=0;i<vm->inst_count;i++){
    GmlInstance *instance=&vm->inst[i];
    if(!collision_target_value_matches(vm,instance,target,0)) continue;
    if(shape_hits_instance(vm,instance,kind,p,IT_ALL,skip,precise,sl,st,sr,sb)) return instance;
  }
  return NULL;
}
static GmlInstance *collision_shape_value(GmlVM *vm, int kind, double *p,
                                          GmlVal target, int precise, int notme){
  if(target.t==V_REAL) return collision_shape(vm,kind,p,(int)target.d,precise,notme);
  if(target.t!=V_ARR || !target.arr) return NULL;
  GmlInstance *skip=notme?vm->cur_self:NULL;
  double sl,st,sr,sb; shape_bounds(kind,p,&sl,&st,&sr,&sb);
  int mode=gml_colgrid_mode(vm), *cand=NULL;
  int count=mode==0?-1:gml_colgrid_collect(vm,sl,st,sr,sb,&cand);
  if(count<0) return collision_shape_value_linear(vm,kind,p,target,precise,notme);
  GmlInstance *result=NULL;
  for(int k=0;k<count;k++){
    int i=cand[k]; if(i<0 || i>=vm->inst_count) continue;
    GmlInstance *instance=&vm->inst[i];
    if(!collision_target_value_matches(vm,instance,target,0)) continue;
    if(shape_hits_instance(vm,instance,kind,p,IT_ALL,skip,precise,sl,st,sr,sb)){
      result=instance; break;
    }
  }
  if(mode==2){
    GmlInstance *linear=collision_shape_value_linear(vm,kind,p,target,precise,notme);
    if(linear!=result){
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
        "[gridcheck] MISMATCH shape-array f%ld kind=%d grid=%d linear=%d\n",
        vm->frame,kind,result?(int)(result-vm->inst):-1,linear?(int)(linear-vm->inst):-1);
      result=linear;
    }
  }
  return result;
}
int collision_shape_list_query(GmlVM *vm, int kind, double *p, GmlVal target,
                                      int precise, int notme, GmlDSList *list, int ordered){
  GmlInstance *skip=notme?vm->cur_self:NULL;
  double sl,st,sr,sb; shape_bounds(kind,p,&sl,&st,&sr,&sb);
  double centre_x=(sl+sr)*0.5, centre_y=(st+sb)*0.5;
  int *cand=NULL, count=-1;
  if(gml_colgrid_mode(vm)!=0) count=gml_colgrid_collect(vm,sl,st,sr,sb,&cand);
  GmlColListHit *hits=NULL; int found=0, cap=0;
  int iterations=count>=0?count:vm->inst_count;
  for(int k=0;k<iterations;k++){
    int i=count>=0?cand[k]:k;
    if(i<0 || i>=vm->inst_count) continue;
    GmlInstance *instance=&vm->inst[i];
    if(!collision_target_value_matches(vm,instance,target,0)) continue;
    if(!shape_hits_instance(vm,instance,kind,p,IT_ALL,skip,precise,sl,st,sr,sb)) continue;
    if(!col_list_add(vm,ordered?NULL:list,ordered,&hits,&found,&cap,i,centre_x,centre_y)) break;
  }
  if(ordered && hits){
    qsort(hits,(size_t)found,sizeof(*hits),col_list_cmp);
    if(list) for(int i=0;i<found;i++)
      ds_list_push(list,vreal((double)vm->inst[hits[i].idx].id));
  }
  free(hits);
  return found;
}

GmlVal gml_builtin_try_collision_planning(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- mp_grid: motion-planning grids (A*). Transient AI aids are owned by this VM and are not
   * serialized because the runtime rebuilds them for each room or level. ---- */
  if(!strcmp(nm,"mp_grid_create")||!strcmp(nm,"mp_grid_destroy")||
     !strcmp(nm,"mp_grid_path")||!strcmp(nm,"mp_grid_add_instances")||
     !strncmp(nm,"mp_grid_",8)){
    GmlBuiltinState *state=builtin_state_ensure(vm);
    if(!state) return vreal(-1);
    GmlMpGrid *mp=state->mp_grid;
    const char *sub=nm+8;
    if(!strcmp(sub,"create")){
      int hc=(int)N(a,n,2), vc=(int)N(a,n,3);
      if(hc<1||vc<1||(long)hc*vc>GML_MP_GRID_MAX_CELLS) return vreal(-1);
      for(int i=0;i<GML_MP_GRID_MAX;i++) if(!mp[i].live){
        mp[i].live=1; mp[i].left=N(a,n,0); mp[i].top=N(a,n,1);
        mp[i].hc=hc; mp[i].vc=vc; mp[i].cw=(int)N(a,n,4); mp[i].ch=(int)N(a,n,5);
        if(mp[i].cw<1) mp[i].cw=1;
        if(mp[i].ch<1) mp[i].ch=1;
        free(mp[i].cell); mp[i].cell=calloc((size_t)hc*vc,1);
        return vreal(i);
      }
      return vreal(-1);
    }
    int gi=(int)N(a,n,0);
    GmlMpGrid *g = (gi>=0 && gi<GML_MP_GRID_MAX && mp[gi].live) ? &mp[gi] : NULL;
    if(!g) return vreal(0);
    if(!strcmp(sub,"destroy")){ free(g->cell); memset(g,0,sizeof *g); return vreal(0); }
    if(!strcmp(sub,"clear_all")){ memset(g->cell,0,(size_t)g->hc*g->vc); return vreal(0); }
    if(!strcmp(sub,"add_cell")||!strcmp(sub,"clear_cell")){
      int h=(int)N(a,n,1), v=(int)N(a,n,2);
      if(h>=0&&h<g->hc&&v>=0&&v<g->vc) g->cell[v*g->hc+h]=(sub[0]=='a');
      return vreal(0); }
    if(!strcmp(sub,"get_cell")){ int h=(int)N(a,n,1), v=(int)N(a,n,2);
      return vreal((h>=0&&h<g->hc&&v>=0&&v<g->vc)? -(double)g->cell[v*g->hc+h] : -1); }
    if(!strcmp(sub,"add_rectangle")||!strcmp(sub,"clear_rectangle")){
      int h0=(int)floor((N(a,n,1)-g->left)/g->cw), v0=(int)floor((N(a,n,2)-g->top)/g->ch);
      int h1=(int)floor((N(a,n,3)-g->left)/g->cw), v1=(int)floor((N(a,n,4)-g->top)/g->ch);
      if(h0>h1){int t=h0;h0=h1;h1=t;} if(v0>v1){int t=v0;v0=v1;v1=t;}
      uint8_t val = (sub[0]=='a');
      for(int v=v0;v<=v1;v++) for(int h=h0;h<=h1;h++)
        if(h>=0&&h<g->hc&&v>=0&&v<g->vc) g->cell[v*g->hc+h]=val;
      return vreal(0); }
    if(!strcmp(sub,"add_instances")){
      int obj=(int)N(a,n,1);
      for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
        if(!o->active||o->marked||o->deactivated) continue;
        if(!target_matches_instance(vm,vm->cur_self,o,obj)) continue;
        double l,t,r,b; if(!inst_bbox(vm,o,o->x,o->y,&l,&t,&r,&b)) continue;
        int h0=(int)floor((l-g->left)/g->cw), v0=(int)floor((t-g->top)/g->ch);
        int h1=(int)floor((r-g->left)/g->cw), v1=(int)floor((b-g->top)/g->ch);
        for(int v=v0;v<=v1;v++) for(int h=h0;h<=h1;h++)
          if(h>=0&&h<g->hc&&v>=0&&v<g->vc) g->cell[v*g->hc+h]=1;
      }
      return vreal(0); }
    if(!strcmp(sub,"path")){
      /* mp_grid_path(grid, path, x0, y0, x1, y1, allowdiag) -> bool; fills the path with a route */
      int pi=(int)N(a,n,1), diag=(int)N(a,n,6)!=0;
      double x0=N(a,n,2), y0=N(a,n,3), x1=N(a,n,4), y1=N(a,n,5);
      if(pi<0||pi>=vm->n_paths) return vreal(0);
      int sh=(int)floor((x0-g->left)/g->cw), sv=(int)floor((y0-g->top)/g->ch);
      int gh=(int)floor((x1-g->left)/g->cw), gv=(int)floor((y1-g->top)/g->ch);
      if(vm->paths[pi].deleted) return vreal(0);
      if(sh<0||sh>=g->hc||sv<0||sv>=g->vc||gh<0||gh>=g->hc||gv<0||gv>=g->vc) return vreal(0);
      if(g->cell[sv*g->hc+sh]||g->cell[gv*g->hc+gh]) return vreal(0);
      int nc=g->hc*g->vc;
      int *prev=malloc((size_t)nc*sizeof(int));
      int *cost=malloc((size_t)nc*sizeof(int));
      int *heap=malloc((size_t)(nc+1)*sizeof(int));   /* binary heap of cell ids keyed by f */
      int *fsco=malloc((size_t)nc*sizeof(int));
      if(!prev||!cost||!heap||!fsco){ free(prev);free(cost);free(heap);free(fsco); return vreal(0); }
      for(int i=0;i<nc;i++){ prev[i]=-1; cost[i]=INT_MAX; }
      int start=sv*g->hc+sh, goal=gv*g->hc+gh, hn=0;
      #define MPH(c) (diag ? 10*((abs((c)%g->hc-gh)>abs((c)/g->hc-gv))?abs((c)%g->hc-gh):abs((c)/g->hc-gv)) \
                           : 10*(abs((c)%g->hc-gh)+abs((c)/g->hc-gv)))
      cost[start]=0; fsco[start]=MPH(start); heap[++hn]=start;
      int found=0;
      while(hn>0){
        int cur=heap[1];                             /* pop-min */
        heap[1]=heap[hn--];
        for(int i2=1;;){ int lch=2*i2, rch=lch+1, sm=i2;
          if(lch<=hn && fsco[heap[lch]]<fsco[heap[sm]]) sm=lch;
          if(rch<=hn && fsco[heap[rch]]<fsco[heap[sm]]) sm=rch;
          if(sm==i2) break;
          int t=heap[i2]; heap[i2]=heap[sm]; heap[sm]=t; i2=sm; }
        if(cur==goal){ found=1; break; }
        int ch2=cur%g->hc, cv2=cur/g->hc;
        static const int DX[8]={1,-1,0,0,1,1,-1,-1}, DY[8]={0,0,1,-1,1,-1,1,-1};
        int ndirs = diag?8:4;
        for(int k=0;k<ndirs;k++){
          int nh=ch2+DX[k], nv2=cv2+DY[k];
          if(nh<0||nh>=g->hc||nv2<0||nv2>=g->vc) continue;
          if(g->cell[nv2*g->hc+nh]) continue;
          if(k>=4 && (g->cell[cv2*g->hc+nh]||g->cell[nv2*g->hc+ch2])) continue;  /* no corner cutting */
          int nb=nv2*g->hc+nh, w=(k>=4)?14:10;
          if(cost[cur]+w < cost[nb]){
            cost[nb]=cost[cur]+w; prev[nb]=cur; fsco[nb]=cost[nb]+MPH(nb);
            heap[++hn]=nb;                            /* push (dupes ok: worse copies skipped) */
            for(int i2=hn; i2>1 && fsco[heap[i2]]<fsco[heap[i2/2]]; i2/=2){
              int t=heap[i2]; heap[i2]=heap[i2/2]; heap[i2/2]=t; }
          }
        }
      }
      #undef MPH
      if(found){
        int rn=0; for(int c=goal;c>=0;c=prev[c]) rn++;
        GmlPathControl *pts=calloc((size_t)rn>0?(size_t)rn:1,sizeof(*pts));
        if(!pts){ free(prev); free(cost); free(heap); free(fsco); return vreal(0); }
        int idx=rn-1;
        for(int c=goal;c>=0;c=prev[c],idx--){
          pts[idx].x=g->left+(c%g->hc+0.5)*g->cw;
          pts[idx].y=g->top+(c/g->hc+0.5)*g->ch;
          pts[idx].sp=100;
        }
        pts[0].x=x0; pts[0].y=y0;                     /* preserve exact endpoints */
        pts[rn-1].x=x1; pts[rn-1].y=y1;
        found=gml_path_replace(vm,pi,pts,rn,0,0,vm->paths[pi].precision);
        free(pts);
      }
      free(prev); free(cost); free(heap); free(fsco);
      return vreal(found);
    }
    if(!strcmp(sub,"draw")) return vreal(0);
    return vreal(0);
  }
  /* runtime paths (path_add / mp_grid_path targets). Appended to vm->paths; indices stay stable. */
  if(!strcmp(nm,"path_assign")){
    double destination=N(a,n,0), source=N(a,n,1);
    if(n>=2 && isfinite(destination) && destination>=0 && destination<vm->n_paths &&
       isfinite(source) && source>=0 && source<vm->n_paths)
      gml_path_assign(vm,(int)destination,(int)source);
    return vreal(0);
  }
  if(!strcmp(nm,"path_duplicate")){
    double source=N(a,n,0);
    if(n<1 || !isfinite(source) || source<0 || source>=vm->n_paths) return vreal(-1);
    return vreal(gml_path_duplicate(vm,(int)source));
  }
  if(!strcmp(nm,"path_get_speed")){
    double index=N(a,n,0), position=N(a,n,1);
    if(n<2 || !isfinite(index) || index<0 || index>=vm->n_paths) return vreal(0);
    return vreal(gml_path_speed_public(vm,(int)index,position));
  }
  if(!strcmp(nm,"path_add")) return vreal(gml_path_add(vm));
  if(!strncmp(nm,"path_",5)){
    double index=N(a,n,0);
    int pi=n && isfinite(index) && index>=0 && index<vm->n_paths?(int)index:-1;
    GmlPath *p=pi>=0 && !vm->paths[pi].deleted?&vm->paths[pi]:NULL;
    if(!strcmp(nm,"path_get_name")) return vstr(p && p->name?p->name:"");
    if(!strcmp(nm,"path_exists")) return vreal(p!=NULL);
    if(!strcmp(nm,"path_add_point")){
      if(p && n>=4) gml_path_edit_point(vm,pi,p->control_count,GML_PATH_POINT_INSERT,
                                      (GmlPathControl){N(a,n,1),N(a,n,2),N(a,n,3)});
      return vreal(0);
    }
    if(!strcmp(nm,"path_change_point") || !strcmp(nm,"path_insert_point") ||
       !strcmp(nm,"path_delete_point")){
      int operation=!strcmp(nm,"path_insert_point")?GML_PATH_POINT_INSERT:
                    (!strcmp(nm,"path_delete_point")?GML_PATH_POINT_DELETE:GML_PATH_POINT_CHANGE);
      double point=N(a,n,1);
      if(p && n>=(operation==GML_PATH_POINT_DELETE?2:5) && isfinite(point) &&
         point>=0 && point<=p->control_count)
        gml_path_edit_point(vm,pi,(int)point,operation,
                           (GmlPathControl){N(a,n,2),N(a,n,3),N(a,n,4)});
      return vreal(0);
    }
    if(!strcmp(nm,"path_set_closed") || !strcmp(nm,"path_set_kind") ||
       !strcmp(nm,"path_set_precision")){
      double value=N(a,n,1);
      if(p && n>=2 && isfinite(value)){
        int kind=p->kind,closed=p->closed,precision=p->precision;
        if(!strcmp(nm,"path_set_closed")) closed=value!=0;
        else if(!strcmp(nm,"path_set_kind")){ if(value<0 || value>1) return vreal(0); kind=(int)value; }
        else { if(value<1 || value>8) return vreal(0); precision=(int)value; }
        gml_path_replace(vm,pi,p->controls,p->control_count,kind,closed,precision);
      }
      return vreal(0);
    }
    if(!strcmp(nm,"path_clear_points") || !strcmp(nm,"path_delete")){
      if(p && gml_path_replace(vm,pi,NULL,0,p->kind,p->closed,p->precision) &&
         !strcmp(nm,"path_delete")) p->deleted=1;
      return vreal(0);
    }
    if(!strcmp(nm,"path_append")){
      double source=N(a,n,1);
      if(p && n>=2 && isfinite(source) && source>=0 && source<vm->n_paths)
        gml_path_append(vm,pi,(int)source);
      return vreal(0);
    }
    if(!strcmp(nm,"path_reverse")){ if(p) gml_path_reverse(vm,pi); return vreal(0); }
    if(!strcmp(nm,"path_mirror")){ if(p) gml_path_transform(vm,pi,-1,1,0); return vreal(0); }
    if(!strcmp(nm,"path_flip")){ if(p) gml_path_transform(vm,pi,1,-1,0); return vreal(0); }
    if(!strcmp(nm,"path_rescale")){
      if(p && n>=3) gml_path_transform(vm,pi,N(a,n,1),N(a,n,2),0);
      return vreal(0);
    }
    if(!strcmp(nm,"path_rotate")){
      if(p && n>=2) gml_path_transform(vm,pi,1,1,N(a,n,1));
      return vreal(0);
    }
    if(!strcmp(nm,"path_shift")){
      if(p && n>=3) gml_path_shift(vm,pi,N(a,n,1),N(a,n,2));
      return vreal(0);
    }
    if(!strcmp(nm,"path_get_number")) return vreal(p?p->control_count:0);
    if(!strcmp(nm,"path_get_closed")) return vreal(p?p->closed:0);
    if(!strcmp(nm,"path_get_kind")) return vreal(p?p->kind:0);
    if(!strcmp(nm,"path_get_precision")) return vreal(p?p->precision:0);
    if(!strcmp(nm,"path_get_length")) return vreal(p?p->len:0);
    if(!strcmp(nm,"path_get_x") || !strcmp(nm,"path_get_y")){
      double t=N(a,n,1),px=0,py=0;
      if(p && n>=2 && isfinite(t)) gml_path_eval_public(vm,pi,t,&px,&py);
      return vreal(nm[9]=='x'?px:py);
    }
    if(!strcmp(nm,"path_get_point_x") || !strcmp(nm,"path_get_point_y") ||
       !strcmp(nm,"path_get_point_speed")){
      double point=N(a,n,1);
      if(!p || n<2 || !isfinite(point) || point<0 || point>=p->control_count) return vreal(0);
      const GmlPathControl *value=&p->controls[(int)point];
      return vreal(nm[15]=='x'?value->x:(nm[15]=='y'?value->y:value->sp));
    }
  }
  if(!strcmp(nm,"mp_linear_step")){
    if(n<4) return vreal(0);
    int all=N(a,n,3)!=0;
    return vreal(linear_step_move(vm,vm->cur_self,N(a,n,0),N(a,n,1),N(a,n,2),
                                 IT_ALL,!all));
  }
  if(!strcmp(nm,"mp_linear_step_object")){
    double target=N(a,n,3);
    if(n<4 || !isfinite(target) || target<INT_MIN || target>INT_MAX) return vreal(0);
    return vreal(linear_step_move(vm,vm->cur_self,N(a,n,0),N(a,n,1),N(a,n,2),
                                 (int)target,0));
  }
  if(!strcmp(nm,"mp_potential_settings")){
    vm->potential_max_rotation=N(a,n,0); vm->potential_rotate_step=N(a,n,1);
    vm->potential_check_distance=N(a,n,2); vm->potential_rotate_on_spot=N(a,n,3)!=0;
    return vreal(0);
  }
  if(!strcmp(nm,"mp_potential_step")){
    GmlInstance*s=vm->cur_self; if(!s) return vreal(0);
    int all=N(a,n,3)!=0;
    return vreal(potential_step_move(vm,s,N(a,n,0),N(a,n,1),N(a,n,2),all?IT_ALL:0,!all));
  }
  if(!strcmp(nm,"mp_potential_step_object")){
    GmlInstance*s=vm->cur_self; if(!s) return vreal(0);
    return vreal(potential_step_move(vm,s,N(a,n,0),N(a,n,1),N(a,n,2),(int)N(a,n,3),0));
  }
  if(!strcmp(nm,"action_potential_step")){
    GmlInstance*s=vm->cur_self; if(!s) return vreal(0);
    double tx=N(a,n,0), ty=N(a,n,1); if(vm->action_relative){ tx+=s->x; ty+=s->y; }
    int all=N(a,n,3)!=0;
    return vreal(potential_step_move(vm,s,tx,ty,N(a,n,2),all?IT_ALL:0,!all));
  }
  /* instance_nearest/furthest(x,y,obj): id of the nearest/furthest instance of obj (noone=-4). */
  if(!strcmp(nm,"instance_nearest")||!strcmp(nm,"instance_furthest")){
    int far=!strcmp(nm,"instance_furthest"); double px=N(a,n,0),py=N(a,n,1); int obj=(int)N(a,n,2);
    double best=far?-1:1e18; int bid=-4;
    for(int i=0;i<vm->inst_count;i++){ GmlInstance*o=&vm->inst[i];
      if(!target_matches_instance(vm,vm->cur_self,o,obj)) continue;
      double d=hypot(o->x-px,o->y-py); if((far&&d>best)||(!far&&d<best)){ best=d; bid=(int)o->id; } }
    return vreal(bid); }

  return gml_builtin_continue_values_strings(vm,nm,a,n);
}

GmlVal gml_builtin_try_collision(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- collision ---- */
  if(!strcmp(nm,"place_meeting")){ int r=collision_at(vm,N(a,n,0),N(a,n,1),(int)N(a,n,2),0);
    if(log_col_on(vm)){ GmlInstance*cs=vm->cur_self; const char*cn=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
      const char*tn=((int)N(a,n,2)>=0&&(int)N(a,n,2)<vm->n_objects)?vm->objects[(int)N(a,n,2)].name:"?";
      if(log_col_match(vm,cn)) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[col] %s place_meeting(%.0f,%.0f,%s)=%d\n",cn,N(a,n,0),N(a,n,1),tn,r); }
    return vreal(r); }
  if(!strcmp(nm,"place_free")){ int r=!collision_at(vm,N(a,n,0),N(a,n,1),0,1);
    if(log_col_on(vm)){ GmlInstance*cs=vm->cur_self; const char*cn=(cs&&cs->obj>=0&&cs->obj<vm->n_objects)?vm->objects[cs->obj].name:"?";
      if(log_col_match(vm,cn)) anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[col] %s place_free(%.0f,%.0f)=%d\n",cn,N(a,n,0),N(a,n,1),r); }
    return vreal(r); }
  if(!strcmp(nm,"place_empty")){
    int target=n>=3?(int)N(a,n,2):IT_ALL;
    return vreal(!collision_at(vm,N(a,n,0),N(a,n,1),target,0));
  }
  if(!strcmp(nm,"collision_point")){ double p[4]={N(a,n,0),N(a,n,1),0,0}; GmlInstance *o=collision_shape_value(vm,0,p,n>2?a[2]:vreal(IT_NOONE),N(a,n,3)>=0.5,(int)N(a,n,4)); return vreal(o?(double)o->id:-4); }
  /* position_meeting(x,y,obj): is the point (x,y) inside any instance of obj? (bool; checks all). */
  if(!strcmp(nm,"position_meeting")){ double p[4]={N(a,n,0),N(a,n,1),0,0}; return vreal(collision_shape(vm,0,p,(int)N(a,n,2),1,0)!=NULL); }
  if(!strcmp(nm,"collision_rectangle")){ double p[4]={N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3)}; GmlInstance *o=collision_shape_value(vm,1,p,n>4?a[4]:vreal(IT_NOONE),N(a,n,5)>=0.5,(int)N(a,n,6)); return vreal(o?(double)o->id:-4); }
  if(!strcmp(nm,"collision_rectangle_list") || !strcmp(nm,"collision_ellipse_list")){ double p[4]={N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3)};
    int kind=!strcmp(nm,"collision_ellipse_list")?3:1;
    if(kind==3){
      if(n<9 || !isfinite(N(a,n,7)) || N(a,n,7)<0 || N(a,n,7)>INT_MAX) return vreal(0);
      for(int i=0;i<4;i++)
        if(!isfinite(p[i]) || p[i]<INT_MIN || p[i]>INT_MAX-1.0) return vreal(0);
    }
    GmlDSList *list=ds_list_slot_repair(vm,(int)N(a,n,7));
    return vreal(collision_shape_list_query(vm,kind,p,n>4?a[4]:vreal(IT_NOONE),
      N(a,n,5)>=0.5,kind==3?N(a,n,6)>=0.5:(int)N(a,n,6),list,N(a,n,8)>=0.5)); }
  if(!strcmp(nm,"rectangle_in_rectangle")){
    double ax1=N(a,n,0), ay1=N(a,n,1), ax2=N(a,n,2), ay2=N(a,n,3);
    double bx1=N(a,n,4), by1=N(a,n,5), bx2=N(a,n,6), by2=N(a,n,7);
    if(ax1>ax2){ double t=ax1; ax1=ax2; ax2=t; }
    if(ay1>ay2){ double t=ay1; ay1=ay2; ay2=t; }
    if(bx1>bx2){ double t=bx1; bx1=bx2; bx2=t; }
    if(by1>by2){ double t=by1; by1=by2; by2=t; }
    return vreal(!(ax2<bx1 || bx2<ax1 || ay2<by1 || by2<ay1));
  }
  /* Ellipse queries use rectangle-coordinate order and the shared shape search. */
  if(!strcmp(nm,"collision_ellipse")){ double p[4]={N(a,n,0),N(a,n,1),N(a,n,2),N(a,n,3)}; GmlInstance *o=collision_shape_value(vm,3,p,n>4?a[4]:vreal(IT_NOONE),N(a,n,5)>=0.5,(int)N(a,n,6)); return vreal(o?(double)o->id:-4); }
  if(!strcmp(nm,"collision_circle")){ double p[3]={N(a,n,0),N(a,n,1),N(a,n,2)}; GmlInstance *o=collision_shape_value(vm,2,p,n>3?a[3]:vreal(IT_NOONE),N(a,n,4)>=0.5,(int)N(a,n,5)); return vreal(o?(double)o->id:-4); }
  if(!strcmp(nm,"collision_circle_list")){ double p[3]={N(a,n,0),N(a,n,1),N(a,n,2)};
    GmlDSList *list=ds_list_slot_repair(vm,(int)N(a,n,6));
    return vreal(collision_shape_list_query(vm,2,p,n>3?a[3]:vreal(IT_NOONE),
      N(a,n,4)>=0.5,(int)N(a,n,5),list,N(a,n,7)>=0.5)); }
  if(!strcmp(nm,"move_contact_solid")||!strcmp(nm,"move_contact")){ GmlInstance *s=vm->cur_self; if(!s) return vreal(0);
    int solid_only=!strcmp(nm,"move_contact_solid");
    int target=solid_only?0:IT_ALL;
    int classic=vm->win && anygm_policy_uses_classic_runtime(vm->win);
    double dir=N(a,n,0), md=n>=2?N(a,n,1):1000; if(md<=0) md=1000; else if(classic) md=gm_round(md);
    double dx=cos(dir*M_PI/180.0), dy=-sin(dir*M_PI/180.0);
    if(collision_at(vm,s->x,s->y,target,solid_only)){
      if(classic) return vreal(0);
      if(resolve_landing_overlap(vm,s,(int)md)) return vreal(0);
      for(int k=0;k<(int)md;k++){ if(!collision_at(vm,s->x,s->y,target,solid_only)) break; s->x-=dx; s->y-=dy; }
      gml_colgrid_touch(vm,s);
      snap_contact_axis(vm,s,dx,dy);
      return vreal(0);
    }
    for(int k=0;k<(int)md;k++){ if(collision_at(vm,s->x+dx,s->y+dy,target,solid_only)) break; s->x+=dx; s->y+=dy; }
    gml_colgrid_touch(vm,s);
    return vreal(0); }
  return gml_builtin_try_values_math(vm,nm,a,n);
}
