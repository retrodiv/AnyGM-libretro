/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_render.c — atlas/TPAG/sprite decode + software blitter. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#include "gml_render.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o]|(uint32_t)d[o+1]<<8|(uint32_t)d[o+2]<<16|(uint32_t)d[o+3]<<24;
}
static uint16_t u16(const uint8_t *d, uint32_t o){ return (uint16_t)(d[o]|d[o+1]<<8); }

/* ---- atlas (TXTR) ---- */
static void parse_txtr(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"TXTR"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  for(uint32_t i=0;i<n && i<16;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    /* GMS1 EmbeddedTexture: Scaled(u32), pointer-to-blob(u32). Blob = PNG. */
    uint32_t blob=u32(d,p+4);
    int w,h,ch;
    int avail=(int)(r->win->size - blob);
    uint8_t *px=stbi_load_from_memory(d+blob, avail, &w,&h,&ch, 4);
    if(px){ r->atlas[i].px=px; r->atlas[i].w=w; r->atlas[i].h=h; }
    r->n_atlas=i+1;
    if(getenv("GML_DUMP_ATLAS")){ char fn[64]; snprintf(fn,sizeof fn,"builds/_atlas%u.ppm",i);
      FILE*f=fopen(fn,"wb"); if(f){ fprintf(f,"P6\n%d %d\n255\n",w,h);
        for(int q=0;q<w*h;q++) fwrite(px+q*4,1,3,f); fclose(f);
        fprintf(stderr,"[atlas] dumped %s (%dx%d ch=%d)\n",fn,w,h,ch); } }
  }
}

/* ---- TPAG ---- */
static uint32_t *g_tpag_ptr; /* parallel: file offset of each tpag, for sprite frame mapping */
static void parse_tpag(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"TPAG"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_tpag=(int)n; r->tpag=calloc(n,sizeof(GmlTpag)); g_tpag_ptr=calloc(n,sizeof(uint32_t));
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4); g_tpag_ptr[i]=p;
    GmlTpag *t=&r->tpag[i];
    t->sx=u16(d,p); t->sy=u16(d,p+2); t->sw=u16(d,p+4); t->sh=u16(d,p+6);
    t->tx=u16(d,p+8); t->ty=u16(d,p+10); t->bw=u16(d,p+16); t->bh=u16(d,p+18);
    t->atlas=(int16_t)u16(d,p+20);
  }
}
static int tpag_index_for_ptr(GmlRender *r, uint32_t ptr){
  for(int i=0;i<r->n_tpag;i++) if(g_tpag_ptr[i]==ptr) return i;
  return -1;
}

/* ---- SPRT ---- */
static void parse_sprt(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"SPRT"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_spr=(int)n; r->spr=calloc(n,sizeof(GmlSprite));
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    GmlSprite *s=&r->spr[i];
    s->name=gml_str_by_ptr(r->win,u32(d,p));
    s->w=(int)u32(d,p+4); s->h=(int)u32(d,p+8);
    s->ml=(int)u32(d,p+12); s->mr=(int)u32(d,p+16); s->mb=(int)u32(d,p+20); s->mt=(int)u32(d,p+24);
    /* margins(16) transparent/smooth/preload(12) bboxmode(4) sepmasks(4) -> originX/Y */
    /* GMS1 sprite header: ...,BBoxMode(40),SepMasks(44),OriginX(48),OriginY(52),frameList(56) */
    s->originx=(int)u32(d,p+48); s->originy=(int)u32(d,p+52);
    uint32_t list=p+56;             /* SimpleList<TextureEntry>: count + pointers */
    uint32_t fn=u32(d,list);
    if(fn>10000) fn=0;              /* guard against special-type sprites */
    s->n_frames=(int)fn; s->frame=calloc(fn?fn:1,sizeof(int));
    for(uint32_t f=0;f<fn;f++){
      uint32_t tptr=u32(d,list+4+f*4);
      s->frame[f]=tpag_index_for_ptr(r,tptr);
    }
    /* SPRT collision masks (after the frame list): count + count×(rowbytes·height) of 1bpp data.
     * GameMaker collides with THESE, not the visible sprite alpha (which can be decorative). */
    uint32_t maskoff=list+4+fn*4; uint32_t mc=u32(d,maskoff);
    if(mc>0 && mc<100000 && s->w>0 && s->h>0){
      s->mask_count=(int)mc; s->mask_rowb=(s->w+7)/8; s->mask=d+maskoff+4;
    }
  }
}
/* whether sprite's COLLISION MASK is solid at sprite-local (lx,ly). Falls back to the visible
 * alpha if a sprite has no mask. */
int gml_sprite_collision(GmlRender *r, int sprite, int frame, int lx, int ly){
  if(sprite<0||sprite>=r->n_spr) return 0;
  GmlSprite *s=&r->spr[sprite];
  if(!s->mask||s->mask_count<=0) return gml_sprite_alpha(r,sprite,frame,lx,ly)>=64;
  if(lx<0||ly<0||lx>=s->w||ly>=s->h) return 0;
  int mi=(s->mask_count>1 && frame>=0 && frame<s->mask_count)? frame : 0;
  const uint8_t *m=s->mask + (size_t)mi*s->mask_rowb*s->h;
  return (m[(size_t)ly*s->mask_rowb + lx/8] >> (7-(lx%8))) & 1;
}

/* ---- BGND ---- */
static void parse_bgnd(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"BGND"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_bg=(int)n; r->bg=calloc(n,sizeof(GmlBg));
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    /* name, transparent, smooth, preload, texture(TPAG ptr) */
    uint32_t tptr=u32(d,p+16);
    r->bg[i].tpag=tpag_index_for_ptr(r,tptr);
  }
}

int gml_render_init(GmlRender *r, GmlWin *win){
  memset(r,0,sizeof(*r)); r->win=win; r->color=0xFFFFFF; r->alpha=1; r->app_draw_enable=1;
  parse_txtr(r); parse_tpag(r); parse_sprt(r); parse_bgnd(r);
  return 0;
}
void gml_render_free(GmlRender *r){
  for(int i=0;i<r->n_atlas;i++) stbi_image_free(r->atlas[i].px);
  for(int i=0;i<r->n_spr;i++) free(r->spr[i].frame);
  free(r->spr); free(r->tpag); free(r->bg); free(g_tpag_ptr); g_tpag_ptr=0;
}
void gml_render_begin(GmlRender *r, uint32_t *fb, int w, int h, double cx, double cy){
  r->fb=fb; r->fbw=w; r->fbh=h; r->cam_x=cx; r->cam_y=cy;
}
int gml_sprite_frames(GmlRender *r, int sprite){
  return (sprite>=0 && sprite<r->n_spr)? r->spr[sprite].n_frames : 0;
}
/* alpha (0-255) of a sprite frame at SPRITE-LOCAL pixel (lx,ly) in [0,w)x[0,h); 0 outside the
 * trimmed image. Used for per-pixel (precise) collision masks. */
int gml_sprite_alpha(GmlRender *r, int sprite, int frame, int lx, int ly){
  if(sprite<0||sprite>=r->n_spr) return 0;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return 0;
  int sub=((frame%s->n_frames)+s->n_frames)%s->n_frames;
  int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag) return 0;
  GmlTpag *t=&r->tpag[ti];
  int ix=lx - t->tx, iy=ly - t->ty;                 /* into the trimmed sub-image */
  if(ix<0||iy<0||ix>=t->sw||iy>=t->sh) return 0;
  if(t->atlas<0||t->atlas>=r->n_atlas) return 0;
  GmlAtlas *a=&r->atlas[t->atlas]; if(!a->px) return 0;
  int ax=t->sx+ix, ay=t->sy+iy;
  if(ax<0||ay<0||ax>=a->w||ay>=a->h) return 0;
  return a->px[((size_t)ay*a->w+ax)*4+3];
}

/* blit one TPAG sub-rect; nearest-neighbour scale; per-pixel alpha; blend multiply.
 * Handles negative xscale/yscale (horizontal/vertical mirror): the caller's (dx,dy) is the anchor
 * edge for the scale sign, and we walk the destination outward (right/down for +, left/up for −)
 * while sampling the source left-to-right/top-to-bottom. rotation not yet handled (rot ignored). */
static void blit(GmlRender *r, GmlTpag *t, double dx, double dy, double xs, double ys,
                 uint32_t blend, double alpha){
  if(t->atlas<0 || t->atlas>=r->n_atlas) return;
  GmlAtlas *a=&r->atlas[t->atlas]; if(!a->px) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;   /* GM clamps draw alpha to [0,1] */
  double axs=fabs(xs), ays=fabs(ys); if(axs<=0||ays<=0) return;
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;  /* GM blend = BBGGRR */
  /* round-half-up (not floor): GM rasterizes a sprite quad via the GPU, so a fractional position
   * snaps to the nearest pixel. Integer positions (bg tiles) are unaffected; fractional ones (the
   * player/enemies at x.5+) land on the same pixel as the original instead of 1px to the left. */
  int x0=(int)floor(dx+0.5), y0=(int)floor(dy+0.5);
  int w=(int)lround(t->sw*axs), h=(int)lround(t->sh*ays);
  int flipx=(xs<0), flipy=(ys<0);
  for(int yy=0; yy<h; yy++){
    int py = flipy ? (y0-yy) : (y0+yy); if(py<0||py>=r->fbh) continue;
    int ly=(int)((yy+0.5)/ays); if(ly<0||ly>=t->sh) continue;
    int sy=t->sy+ly;
    for(int xx=0; xx<w; xx++){
      int px = flipx ? (x0-xx) : (x0+xx); if(px<0||px>=r->fbw) continue;
      int lx=(int)((xx+0.5)/axs); if(lx<0||lx>=t->sw) continue;
      int sx=t->sx+lx;
      uint8_t *sp=a->px + ((size_t)sy*a->w+sx)*4;
      double sa=(sp[3]/255.0)*alpha; if(sa<=0) continue;
      uint32_t *dp=&r->fb[(size_t)py*r->fbw+px];
      int dr=(*dp>>16)&0xFF, dg=(*dp>>8)&0xFF, db=*dp&0xFF;
      int sr=sp[0]*bR/255, sg=sp[1]*bG/255, sb=sp[2]*bB/255;
      /* clamp each channel to [0,255]: a blend>255 or a (legitimately clamped) alpha can still push
       * sr*sa over 255, and packing an out-of-range byte would corrupt the neighbouring channel. */
      int or_=(int)(sr*sa+dr*(1-sa)); if(or_>255) or_=255; else if(or_<0) or_=0;
      int og=(int)(sg*sa+dg*(1-sa)); if(og>255) og=255; else if(og<0) og=0;
      int ob=(int)(sb*sa+db*(1-sa)); if(ob>255) ob=255; else if(ob<0) ob=0;
      *dp=(or_<<16)|(og<<8)|ob;
    }
  }
}
static void blit_rotated(GmlRender *r, GmlSprite *spr, GmlTpag *t, double x, double y,
                         double xs, double ys, double rot, uint32_t blend, double alpha){
  if(t->atlas<0 || t->atlas>=r->n_atlas) return;
  GmlAtlas *a=&r->atlas[t->atlas]; if(!a->px) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(xs==0||ys==0) return;
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;
  double ang=rot*M_PI/180.0, c=cos(ang), sn=sin(ang);
  double ax=x-r->cam_x, ay=y-r->cam_y;
  double minx=1e30,miny=1e30,maxx=-1e30,maxy=-1e30;
  double corners[4][2]={{t->tx,t->ty},{t->tx+t->sw,t->ty},{t->tx,t->ty+t->sh},{t->tx+t->sw,t->ty+t->sh}};
  for(int i=0;i<4;i++){
    double px=(corners[i][0]-spr->originx)*xs, py=(corners[i][1]-spr->originy)*ys;
    double dx=ax + px*c + py*sn, dy=ay - px*sn + py*c;
    if(dx<minx) minx=dx;
    if(dx>maxx) maxx=dx;
    if(dy<miny) miny=dy;
    if(dy>maxy) maxy=dy;
  }
  int x0=(int)floor(minx)-1, x1=(int)ceil(maxx)+1;
  int y0=(int)floor(miny)-1, y1=(int)ceil(maxy)+1;
  if(x0<0) x0=0;
  if(y0<0) y0=0;
  if(x1>r->fbw) x1=r->fbw;
  if(y1>r->fbh) y1=r->fbh;
  for(int py=y0; py<y1; py++) for(int px=x0; px<x1; px++){
    double rx=px+0.5-ax, ry=py+0.5-ay;
    double sxr=rx*c - ry*sn, syr=rx*sn + ry*c;
    double lx=sxr/xs + spr->originx, ly=syr/ys + spr->originy;
    int ix=(int)floor(lx-t->tx), iy=(int)floor(ly-t->ty);
    if(ix<0||iy<0||ix>=t->sw||iy>=t->sh) continue;
    int sx=t->sx+ix, sy=t->sy+iy;
    if(sx<0||sy<0||sx>=a->w||sy>=a->h) continue;
    uint8_t *sp=a->px + ((size_t)sy*a->w+sx)*4;
    double sa=(sp[3]/255.0)*alpha; if(sa<=0) continue;
    uint32_t *dp=&r->fb[(size_t)py*r->fbw+px];
    int dr=(*dp>>16)&0xFF, dg=(*dp>>8)&0xFF, db=*dp&0xFF;
    int sr=sp[0]*bR/255, sg=sp[1]*bG/255, sb=sp[2]*bB/255;
    int or_=(int)(sr*sa+dr*(1-sa)); if(or_>255) or_=255; else if(or_<0) or_=0;
    int og=(int)(sg*sa+dg*(1-sa)); if(og>255) og=255; else if(og<0) og=0;
    int ob=(int)(sb*sa+db*(1-sa)); if(ob>255) ob=255; else if(ob<0) ob=0;
    *dp=(or_<<16)|(og<<8)|ob;
  }
}

void gml_draw_sprite_ext(GmlRender *r, int sprite, int subimg, double x, double y,
                         double xs, double ys, double rot, uint32_t blend, double alpha){
  if(sprite<0||sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  int sub = s->n_frames? ((subimg%s->n_frames)+s->n_frames)%s->n_frames : 0;
  int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti];
  if(getenv("GML_LOG_SPR"))
    fprintf(stderr,"[spr] %s subimg=%d sub=%d tpag=%d sx,sy=%d,%d sw,sh=%d,%d atlas=%d x=%.0f y=%.0f blend=%06X a=%.2f\n",
      s->name?s->name:"?",subimg,sub,ti,t->sx,t->sy,t->sw,t->sh,t->atlas,x,y,(unsigned)(blend&0xffffff),alpha);
  /* draw at (x - origin)*scale + target offset, minus camera */
  double dx = x - s->originx*xs + t->tx*xs - r->cam_x;
  double dy = y - s->originy*ys + t->ty*ys - r->cam_y;
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  if(fabs(rr)<0.001 || fabs(rr-360.0)<0.001) blit(r,t,dx,dy,xs,ys,blend,alpha);
  else blit_rotated(r,s,t,x,y,xs,ys,rr,blend,alpha);
}
void gml_draw_sprite(GmlRender *r, int sprite, int subimg, double x, double y){
  gml_draw_sprite_ext(r,sprite,subimg,x,y,1,1,0,0xFFFFFF,1);
}
/* draw_sprite_tiled_ext: repeat a sprite frame to fill both screen axes, anchored at (x,y). */
void gml_draw_sprite_tiled_ext(GmlRender *r, int sprite, int subimg, double x, double y,
                               double xs, double ys, uint32_t blend, double alpha){
  if(sprite<0||sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  int sub=((subimg%s->n_frames)+s->n_frames)%s->n_frames;
  int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti]; double bw=t->sw*fabs(xs), bh=t->sh*fabs(ys); if(bw<=0||bh<=0) return;
  double ax=floor(x - s->originx*xs + t->tx*xs - r->cam_x);
  double ay=floor(y - s->originy*ys + t->ty*ys - r->cam_y);
  double x0=fmod(ax,bw); if(x0>0) x0-=bw;
  double y0=fmod(ay,bh); if(y0>0) y0-=bh;
  for(double yy=y0; yy<r->fbh; yy+=bh)
    for(double xx=x0; xx<r->fbw; xx+=bw)
      blit(r,t, xx, yy, xs,ys, blend, alpha);
}

void gml_draw_background(GmlRender *r, int bg, double x, double y){
  if(bg<0||bg>=r->n_bg) return; int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti];
  blit(r,t, x - r->cam_x + t->tx, y - r->cam_y + t->ty, 1,1, 0xFFFFFF, 1);
}
/* draw_background_tiled: repeat the background image along the layer's tiled axes, anchored at (x,y).
 * htiled/vtiled come from the room layer (background_htiled[]/vtiled[]): a layer that only tiles
 * horizontally (e.g. a ground strip) must draw a single row, not fill the screen vertically. */
static void do_bg_tiled_ext(GmlRender *r, int bg, double x, double y, double xs, double ys,
                            uint32_t color, double alpha, int htiled, int vtiled){
  if(bg<0||bg>=r->n_bg) return; { const char*sb=getenv("GML_SKIP_BG"); if(sb&&atoi(sb)==bg) return; }
  int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti]; double bw=t->sw*xs, bh=t->sh*ys; if(bw<=0||bh<=0) return;
  /* apply the texture-page target offset (tx,ty): GM places a background's cropped content at this
   * offset inside its logical (bounding) image — e.g. a tree background sits at y=64 so the sky shows above
   * it. gml_draw_background already does this; the tiled path must too, or the canopy draws too high. */
  double sx=floor(x + t->tx*xs - r->cam_x), sy=floor(y + t->ty*ys - r->cam_y);
  double x0,xend,y0,yend;
  if(htiled){ x0=fmod(sx,bw); if(x0>0) x0-=bw; xend=r->fbw; } else { x0=sx; xend=sx+1; }
  if(vtiled){ y0=fmod(sy,bh); if(y0>0) y0-=bh; yend=r->fbh; } else { y0=sy; yend=sy+1; }
  for(double yy=y0; yy<yend; yy+=bh)
    for(double xx=x0; xx<xend; xx+=bw)
      blit(r,t, xx, yy, xs,ys, color, alpha);
}
/* paint a bg layer immediately, in the order a parallax object issues it (index 0..7). */
static void bg_emit(GmlRender *r, int bgdef, double x, double y, double xs, double ys,
                    uint32_t color, double alpha, int htiled, int vtiled){
  do_bg_tiled_ext(r,bgdef,x,y,xs,ys,color,alpha,htiled,vtiled);
}
void gml_draw_background_tiled(GmlRender *r, int bg, double x, double y, int htiled, int vtiled){
  bg_emit(r,bg,x,y,1,1,0xFFFFFF,1,htiled,vtiled);
}
/* draw_background[_tiled]_ext: as above + xscale/yscale + blend colour + alpha. a parallax object
 * draws layers 1..n with these (e.g. a tree background at background_alpha). */
void gml_draw_background_ext(GmlRender *r, int bg, double x, double y, double xs, double ys,
                            uint32_t color, double alpha){
  if(bg<0||bg>=r->n_bg) return; int ti=r->bg[bg].tpag; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti];
  blit(r,t, x - r->cam_x + t->tx*xs, y - r->cam_y + t->ty*ys, xs,ys, color, alpha);
}
void gml_draw_background_tiled_ext(GmlRender *r, int bg, double x, double y, double xs, double ys,
                                  uint32_t color, double alpha, int htiled, int vtiled){
  bg_emit(r,bg,x,y,xs,ys,color,alpha,htiled,vtiled);
}

/* draw a room's background layers (from the room Background pointer-list).
 * want_fg: 0 = backgrounds (behind instances), 1 = foregrounds (in front). */
void gml_draw_room_backgrounds(GmlRender *r, uint32_t bg_ptr, int want_fg){
  if(!bg_ptr) return;
  const uint8_t *d=r->win->data; uint32_t cnt=u32(d,bg_ptr);
  for(uint32_t i=0;i<cnt;i++){
    uint32_t p=u32(d,bg_ptr+4+i*4);
    int enabled=(int)u32(d,p), fg=(int)u32(d,p+4), bgdef=(int)u32(d,p+8);
    int x=(int)u32(d,p+12), y=(int)u32(d,p+16), tilex=(int)u32(d,p+20), tiley=(int)u32(d,p+24);
    if(!enabled || fg!=want_fg) continue;
    if(bgdef<0||bgdef>=r->n_bg) continue;
    int ti=r->bg[bgdef].tpag; if(ti<0||ti>=r->n_tpag) continue;
    GmlTpag *t=&r->tpag[ti]; int bw=t->sw, bh=t->sh; if(bw<=0||bh<=0) continue;
    int x0=x, y0=y;
    if(tilex){ x0 = x % bw; if(x0>0) x0-=bw; }
    if(tiley){ y0 = y % bh; if(y0>0) y0-=bh; }
    for(int yy=y0; yy < (tiley? r->fbh : y0+1); yy+=bh){
      for(int xx=x0; xx < (tilex? r->fbw : x0+1); xx+=bw){
        blit(r,t, xx, yy, 1,1, 0xFFFFFF, 1);
      }
      if(!tilex) {}
    }
  }
}

/* a room's tile layer: each tile blits a sub-rect (src_x,src_y,w,h) of a background/tileset
 * image to a room position, drawn back-to-front by tile depth. The room tile list is a
 * pointer-list (count + pointers), each record: x,y,bgdef,srcx,srcy,w,h,depth,id,sx,sy,color. */
typedef struct { int x,y,def,sx,sy,w,h,depth; } GmlTileRec;
static int cmp_tile_depth(const void *a, const void *b){
  const GmlTileRec *ta=a,*tb=b;
  return ta->depth>tb->depth? -1 : (ta->depth<tb->depth? 1 : 0);   /* high depth first (behind) */
}
/* blit a single tile: a sub-rect (sx,sy,w,h) of background/tileset `def` at room (x,y). */
void gml_draw_tile(GmlRender *r, int def, int sx, int sy, int w, int h, double x, double y){
  if(def<0||def>=r->n_bg) return; int ti=r->bg[def].tpag; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *bt=&r->tpag[ti]; GmlTpag tt=*bt;
  tt.sx=bt->sx+sx; tt.sy=bt->sy+sy; tt.sw=w; tt.sh=h;
  blit(r,&tt, x - r->cam_x, y - r->cam_y, 1,1, 0xFFFFFF, 1);
}
void gml_draw_room_tiles(GmlRender *r, uint32_t tile_ptr){
  if(!tile_ptr) return;
  const uint8_t *d=r->win->data; uint32_t cnt=u32(d,tile_ptr);
  if(cnt==0 || cnt>100000) return;
  GmlTileRec *tiles=malloc((size_t)cnt*sizeof(GmlTileRec)); if(!tiles) return; int m=0;
  for(uint32_t i=0;i<cnt;i++){
    uint32_t p=u32(d,tile_ptr+4+i*4);
    GmlTileRec t;
    t.x=(int)u32(d,p);    t.y=(int)u32(d,p+4);  t.def=(int)u32(d,p+8);
    t.sx=(int)u32(d,p+12);t.sy=(int)u32(d,p+16);t.w=(int)u32(d,p+20);
    t.h=(int)u32(d,p+24); t.depth=(int)u32(d,p+28);
    tiles[m++]=t;
  }
  qsort(tiles,m,sizeof(GmlTileRec),cmp_tile_depth);
  for(int i=0;i<m;i++){
    GmlTileRec *t=&tiles[i];
    if(t->def<0 || t->def>=r->n_bg) continue;
    int ti=r->bg[t->def].tpag; if(ti<0 || ti>=r->n_tpag) continue;
    GmlTpag *bt=&r->tpag[ti];
    GmlTpag tt=*bt;                                  /* sub-rect of the tileset image */
    tt.sx=bt->sx + t->sx; tt.sy=bt->sy + t->sy; tt.sw=t->w; tt.sh=t->h;
    blit(r,&tt, t->x - r->cam_x, t->y - r->cam_y, 1,1, 0xFFFFFF, 1);
  }
  free(tiles);
}

/* draw_sprite_stretched: blit a sprite frame stretched into the screen rect (dx,dy,dw,dh), box-
 * averaging the source per dest pixel (matches the GPU's filtered down-stretch). Screen-space. */
void gml_draw_sprite_stretched(GmlRender *r, int sprite, int frame, double dx, double dy, double dw, double dh, uint32_t blend, double alpha){
  if(sprite<0||sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[sprite]; if(s->n_frames<=0) return;
  int sub=((frame%s->n_frames)+s->n_frames)%s->n_frames;
  int ti=s->frame[sub]; if(ti<0||ti>=r->n_tpag) return;
  GmlTpag *t=&r->tpag[ti]; if(t->atlas<0||t->atlas>=r->n_atlas) return;
  GmlAtlas *a=&r->atlas[t->atlas]; if(!a->px) return;
  int sw = s->w>0? s->w : t->sw, sh = s->h>0? s->h : t->sh;
  int x0=(int)floor(dx), y0=(int)floor(dy), W=(int)lround(dw), H=(int)lround(dh);
  if(sw<=0||sh<=0||W<=0||H<=0) return;
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;
  for(int py=0; py<H; py++){ int ty_=y0+py; if(ty_<0||ty_>=r->fbh) continue;
    int sy0=(py*sh)/H, sy1=((py+1)*sh)/H; if(sy1<=sy0) sy1=sy0+1;
    for(int px=0; px<W; px++){ int tx_=x0+px; if(tx_<0||tx_>=r->fbw) continue;
      int sx0=(px*sw)/W, sx1=((px+1)*sw)/W; if(sx1<=sx0) sx1=sx0+1;
      int R=0,G=0,B=0,A=0,n=0;
      for(int sy=sy0;sy<sy1;sy++){ int iy=sy - t->ty; if(iy<0||iy>=t->sh) continue;
        for(int sx=sx0;sx<sx1;sx++){ int ix=sx - t->tx; if(ix<0||ix>=t->sw) continue;
          uint8_t *sp=a->px + ((size_t)(t->sy+iy)*a->w + (t->sx+ix))*4;
          R+=sp[0]; G+=sp[1]; B+=sp[2]; A+=sp[3]; n++; } }
      if(!n) continue;
      double oa=((A/(double)n)/255.0)*alpha; if(oa<=0) continue;
      uint32_t *dp=&r->fb[(size_t)ty_*r->fbw+tx_];
      int dr=(*dp>>16)&0xFF, dg=(*dp>>8)&0xFF, db=*dp&0xFF;
      int sr=(int)(R/(double)n)*bR/255, sg=(int)(G/(double)n)*bG/255, sb=(int)(B/(double)n)*bB/255;
      *dp = ((int)(sr*oa+dr*(1-oa))<<16) | ((int)(sg*oa+dg*(1-oa))<<8) | (int)(sb*oa+db*(1-oa));
    }
  }
}
/* draw_surface_stretched[_ext]: blit the application_surface (the game render) into (dx,dy,dw,dh)
 * of the screen, box-averaged, with blend+alpha. an overlay object uses this to composite the game. */
void gml_draw_surface_stretched(GmlRender *r, double dx, double dy, double dw, double dh, uint32_t blend, double alpha){
  if(!r->app_surface) return;
  int sw=r->fbw, sh=r->fbh;   /* the app_surface is the same native size as the screen */
  int x0=(int)floor(dx), y0=(int)floor(dy), W=(int)lround(dw), H=(int)lround(dh);
  if(W<=0||H<=0) return;
  int bR=blend&0xFF, bG=(blend>>8)&0xFF, bB=(blend>>16)&0xFF;
  for(int py=0; py<H; py++){ int ty_=y0+py; if(ty_<0||ty_>=r->fbh) continue;
    int sy0=(py*sh)/H, sy1=((py+1)*sh)/H; if(sy1<=sy0) sy1=sy0+1;
    for(int px=0; px<W; px++){ int tx_=x0+px; if(tx_<0||tx_>=r->fbw) continue;
      int sx0=(px*sw)/W, sx1=((px+1)*sw)/W; if(sx1<=sx0) sx1=sx0+1;
      int R=0,G=0,B=0,n=0;
      for(int sy=sy0;sy<sy1;sy++) for(int sx=sx0;sx<sx1;sx++){
        uint32_t s=r->app_surface[(size_t)sy*sw+sx]; R+=(s>>16)&0xFF; G+=(s>>8)&0xFF; B+=s&0xFF; n++; }
      if(!n) continue;
      int sr=(R/n)*bR/255, sg=(G/n)*bG/255, sb=(B/n)*bB/255;
      uint32_t *dp=&r->fb[(size_t)ty_*r->fbw+tx_];
      if(alpha>=1.0){ *dp=(sr<<16)|(sg<<8)|sb; }
      else { int dr=(*dp>>16)&0xFF, dg=(*dp>>8)&0xFF, db=*dp&0xFF;
        *dp=((int)(sr*alpha+dr*(1-alpha))<<16)|((int)(sg*alpha+dg*(1-alpha))<<8)|(int)(sb*alpha+db*(1-alpha)); }
    }
  }
}

int gml_font_add_sprite(GmlRender *r, int sprite, int first, int prop, int sep){
  if(r->n_fonts>=GML_MAX_FONTS) return -1;
  int id=r->n_fonts++;
  r->fonts[id]=(GmlFont){sprite,first,prop,sep};
  if(getenv("GML_LOG_TEXT")){ GmlSprite *s=&r->spr[sprite];
    int spfr=(int)' '-first; int spw=-1;
    if(spfr>=0&&spfr<s->n_frames){ int ti=s->frame[spfr]; if(ti>=0&&ti<r->n_tpag) spw=r->tpag[ti].sw; }
    fprintf(stderr,"[font] id=%d spr=%d(%s) first=%d prop=%d sep=%d nframes=%d cell=%dx%d space_sw=%d\n",
      id,sprite,s->name?s->name:"?",first,prop,sep,s->n_frames,s->w,s->h,spw); }
  return id;
}

/* glyph advance width for char index within a sprite font. Characters outside the font's
 * glyph range (notably the space, when `first` > 32) still advance the cursor: GM lays them
 * out at the fixed cell width (these sprite fonts are non-proportional), so a space is a blank
 * cell, not zero-width. */
static int glyph_w(GmlRender *r, GmlFont *f, int frame){
  GmlSprite *s=&r->spr[f->sprite];
  if(frame<0||frame>=s->n_frames) return s->w;        /* space / out-of-range: blank cell */
  int ti=s->frame[frame]; if(ti<0||ti>=r->n_tpag) return s->w;
  return f->prop ? r->tpag[ti].sw : (s->w?s->w:r->tpag[ti].sw);
}

/* width of one line (up to '#'/NUL), counting '\#' as a literal '#'. */
static int line_width(GmlRender *r, GmlFont *f, const char *p, const char **end){
  int w=0; for(; *p && *p!='#'; p++){
    if(*p=='\\' && p[1]=='#') p++;            /* escaped '#' -> literal */
    w += glyph_w(r,f,(unsigned char)*p - f->first) + f->sep;
  }
  if(w>0) w-=f->sep; *end=p; return w;
}
void gml_draw_text(GmlRender *r, double x, double y, const char *str){
  if(r->font<0||r->font>=r->n_fonts||!str) return;
  if(getenv("GML_LOG_TEXT")) fprintf(stderr,"[text] x=%.0f y=%.0f font=%d halign=%d valign=%d col=%06X a=%.2f \"%s\"\n",
    x,y,r->font,r->halign,r->valign,(unsigned)(r->color&0xffffff),r->alpha,str);
  GmlFont *f=&r->fonts[r->font]; GmlSprite *s=&r->spr[f->sprite];
  /* line height = sprite cell height (GM uses the sprite's declared h, not the
   * glyph's trimmed sub-rect sh — otherwise text lines collapse when glyphs are cropped). */
  int lh=s->h; if(lh<=0) lh=8;
  /* count lines for valign */
  int nlines=1; for(const char *p=str;*p;p++){ if(*p=='\\'&&p[1]=='#'){p++;continue;} if(*p=='#') nlines++; }
  double cy=y;
  if(r->valign==1) cy=y-(nlines*lh)/2.0; else if(r->valign==2) cy=y-nlines*lh;
  const char *p=str;
  for(int li=0; *p || li==0; li++){
    const char *end; int lw=line_width(r,f,p,&end);
    double cx=x;
    if(r->halign==1) cx=x-lw/2.0; else if(r->halign==2) cx=x-lw;
    for(; p<end; p++){
      char c=*p; if(c=='\\'&&p[1]=='#'){ c='#'; p++; }
      int fr=(unsigned char)c - f->first;
      if(fr>=0 && fr<s->n_frames){ int ti=s->frame[fr];
        if(ti>=0 && ti<r->n_tpag){ GmlTpag *t=&r->tpag[ti];
          blit(r,t, cx - r->cam_x + t->tx, cy - r->cam_y + t->ty, 1,1, r->color, r->alpha); } }
      cx += glyph_w(r,f,fr)+f->sep;
    }
    cy += lh;
    if(*end=='#') p=end+1; else break;
  }
}
