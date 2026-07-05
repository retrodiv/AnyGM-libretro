/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_render.c — atlas/TPAG/sprite decode + software blitter. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "gml_render.h"
#include "gm_qoi.h"
#include "bzip2/bzlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

static uint32_t u32(const uint8_t *d, uint32_t o){
  return (uint32_t)d[o]|(uint32_t)d[o+1]<<8|(uint32_t)d[o+2]<<16|(uint32_t)d[o+3]<<24;
}
static uint16_t u16(const uint8_t *d, uint32_t o){ return (uint16_t)(d[o]|d[o+1]<<8); }

/* ---- atlas (TXTR) ---- */
/* Decode PNG, fioq, or a bzip2-compressed 2zoq container into RGBA pixels. */
static uint8_t *decode_texture_blob(const uint8_t *blob, size_t avail, size_t chunk_end,
                                    int *ow, int *oh){
  if(avail<4) return NULL;
  if(blob[0]==0x89 && blob[1]=='P' && blob[2]=='N' && blob[3]=='G'){
    int w,h,ch; uint8_t *px=stbi_load_from_memory(blob,(int)avail,&w,&h,&ch,4);
    if(px){ *ow=w; *oh=h; } return px;
  }
  if(!memcmp(blob,"fioq",4)) return gm_qoi_decode(blob,avail,ow,oh);
  if(!memcmp(blob,"2zoq",4)){
    /* "2zoq": magic(4) + w(u16) + h(u16) + decompressed_len(u32) + bzip2 stream */
    if(avail<12) return NULL;
    uint32_t dlen=u32(blob,8);
    if(dlen<12 || dlen>64u*1024*1024) return NULL;
    char *dec=malloc(dlen); if(!dec) return NULL;
    unsigned int declen=dlen;
    unsigned int srclen=(unsigned int)(chunk_end>(size_t)(blob+12)? chunk_end-(size_t)(blob+12) : 0);
    int rc=BZ2_bzBuffToBuffDecompress(dec,&declen,(char*)(blob+12),srclen,0,0);
    uint8_t *px=NULL;
    if((rc==BZ_OK||rc==BZ_OUTBUFF_FULL) && declen>=12) px=gm_qoi_decode((uint8_t*)dec,declen,ow,oh);
    free(dec); return px;
  }
  return NULL;
}
static void parse_txtr(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"TXTR"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  size_t chunk_end=(size_t)(d + c->off + c->size);
  r->atlas=calloc(n?n:1,sizeof(GmlAtlas));
  r->n_atlas=(int)n;
  for(uint32_t i=0;i<n;i++){
    uint32_t entry=u32(d,c->off+4+i*4);
    /* Locate the texture data through a candidate pointer in its record. */
    uint32_t blob=0;
    /* Scan record fields and select a pointer to recognized PNG, fioq or 2zoq data. */
    for(uint32_t off=4; off<=32; off+=4){ uint32_t v=u32(d,entry+off);
      if((size_t)v+4<=r->win->size){ const uint8_t *m=d+v;
        if((m[0]==0x89&&m[1]=='P'&&m[2]=='N'&&m[3]=='G')||!memcmp(m,"fioq",4)||!memcmp(m,"2zoq",4)){ blob=v; break; } } }
    if(!blob || (size_t)blob>=r->win->size) continue;
    int w=0,h=0;
    uint8_t *px=decode_texture_blob(d+blob, r->win->size-blob, chunk_end, &w,&h);
    if(px){ r->atlas[i].px=px; r->atlas[i].w=w; r->atlas[i].h=h; }
    if(px && getenv("GML_DUMP_ATLAS")){ char fn[64]; snprintf(fn,sizeof fn,"builds/_atlas%u.ppm",i);
      FILE*f=fopen(fn,"wb"); if(f){ fprintf(f,"P6\n%d %d\n255\n",w,h);
        for(int q=0;q<w*h;q++) fwrite(px+q*4,1,3,f); fclose(f);
        fprintf(stderr,"[atlas] dumped %s (%dx%d)\n",fn,w,h); } }
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
  r->n_spr=(int)n; r->spr=calloc(n,sizeof(GmlSprite)); r->spr_cap=(int)n; r->spr_has_free=0;
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    GmlSprite *s=&r->spr[i];
    s->name=gml_str_by_ptr(r->win,u32(d,p));
    s->w=(int)u32(d,p+4); s->h=(int)u32(d,p+8);
    s->ml=(int)u32(d,p+12); s->mr=(int)u32(d,p+16); s->mb=(int)u32(d,p+20); s->mt=(int)u32(d,p+24);
    s->collision_kind=0; s->collision_tolerance=63; /* preserve the old alpha>=64 fallback */
    /* margins(16) transparent/smooth/preload(12) bboxmode(4) sepmasks(4) -> originX/Y */
    /* GMS1 sprite header: ...,BBoxMode(40),SepMasks(44),OriginX(48),OriginY(52),frameList(56) */
    s->originx=(int)u32(d,p+48); s->originy=(int)u32(d,p+52);
    uint32_t list=p+56;             /* GMS1: SimpleList<TextureEntry> here (count + pointers) */
    /* A -1 marker at +56 selects the versioned sprite layout. For simple sprites,
     * the texture list follows playback fields and optional sequence/nine-slice offsets. */
    if(u32(d,p+56)==0xFFFFFFFFu){
      uint32_t sver=u32(d,p+60), stype=u32(d,p+64);
      if(stype!=0){ s->n_frames=0; s->frame=calloc(1,sizeof(int)); continue; }  /* SWF/Spine: no simple list */
      uint32_t fl=76;                         /* after PlaybackSpeed(+68)+PlaybackSpeedType(+72) */
      if(sver>=2) fl+=4;                       /* SequenceOffset */
      if(sver>=3) fl+=4;                       /* NineSliceOffset */
      list=p+fl;
    }
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
  if(lx<0||ly<0||lx>=s->w||ly>=s->h) return 0;
  if(lx<s->ml||lx>s->mr||ly<s->mt||ly>s->mb) return 0;
  if(s->collision_kind==1) return 1;
  if(s->collision_kind==2){
    double hw=(s->mr-s->ml+1)/2.0, hh=(s->mb-s->mt+1)/2.0;
    if(hw<=0||hh<=0) return 0;
    double cx=s->ml+hw-0.5, cy=s->mt+hh-0.5;
    double dx=(lx-cx)/hw, dy=(ly-cy)/hh;
    return dx*dx+dy*dy<=1.0;
  }
  if(s->collision_kind==3){
    double hw=(s->mr-s->ml+1)/2.0, hh=(s->mb-s->mt+1)/2.0;
    if(hw<=0||hh<=0) return 0;
    double cx=s->ml+hw-0.5, cy=s->mt+hh-0.5;
    return fabs((lx-cx)/hw)+fabs((ly-cy)/hh)<=1.0;
  }
  if(s->runtime_rgba) return gml_sprite_alpha(r,sprite,frame,lx,ly)>s->collision_tolerance;
  if(!s->mask||s->mask_count<=0) return gml_sprite_alpha(r,sprite,frame,lx,ly)>=64;
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
	    if(r->win->bytecode>=17 && p+64<c->off+c->size){
	      int ver=(int)u32(d,p+20), tw=(int)u32(d,p+24), th=(int)u32(d,p+28);
	      int bx=(int)u32(d,p+32), by=(int)u32(d,p+36), cols=(int)u32(d,p+40);
	      int items=(int)u32(d,p+44), count=(int)u32(d,p+48);
	      uint64_t id_bytes=(uint64_t)items*(uint64_t)count*4u;
	      if(ver>0 && tw>0 && th>0 && tw<=4096 && th<=4096 && bx>=0 && by>=0 &&
	         cols>0 && cols<=4096 && items>0 && items<=1024 && count>0 &&
	         id_bytes<=4000000u && p+64+id_bytes<=c->off+c->size){
	        r->bg[i].tile_w=tw; r->bg[i].tile_h=th;
	        r->bg[i].tile_border_x=bx; r->bg[i].tile_border_y=by;
	        r->bg[i].tile_columns=cols; r->bg[i].tile_items_per_tile=items; r->bg[i].tile_count=count;
	        r->bg[i].tile_ids=d+p+64;
	      }
	    }
	  }
	}

/* FONT records reference a TPAG page and glyph rectangles with advance and bearing.
 * Parse these records from the loaded container. Resource fonts occupy the initial
 * font indices; dynamically added sprite fonts follow them. */
static void parse_font(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"FONT"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  int nf=(int)n; if(nf>GML_MAX_FONTS) nf=GML_MAX_FONTS;
  for(int i=0;i<nf;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    GmlFont *f=&r->fonts[i];
    memset(f,0,sizeof(*f));
    for(int k=0;k<256;k++) f->glyph_by_char[k]=-1;
    f->real=1;
    /* Interpret EmSize as an integer or floating-point field using the checks below. */
    int em_is_float=0;
    { uint32_t rawem=u32(d,p+8); float fem; memcpy(&fem,&rawem,4);
      if(fem<0){ f->line_height=(int)(0.5f-fem); em_is_float=1; }
      else if(fem>0 && fem<512.0f && rawem>0x30000000u){ f->line_height=(int)(fem+0.5f); em_is_float=1; }
      else f->line_height=(int)rawem; }
    uint32_t texptr=u32(d,p+28);                    /* glyph-page TPAG record */
    int tsx=u16(d,texptr), tsy=u16(d,texptr+2), tatlas=(int16_t)u16(d,texptr+20);
    f->atlas=tatlas;
    /* glyph table position: +40 (bc14-16) or +48 (GMS2.2.6+/2.3 insert AscenderOffset and
     * Ascender after the scales). Detect per record: the count must be sane and be followed
     * by an in-file pointer list. */
    uint32_t goff=40;
    { uint32_t c40=u32(d,p+40), c48=u32(d,p+48);
      uint32_t q40=(c40>0&&c40<100000)?u32(d,p+44):0, q48=(c48>0&&c48<100000)?u32(d,p+52):0;
      int ok40 = q40>p && q40+14<=r->win->size;
      int ok48 = q48>p && q48+14<=r->win->size;
      if(!ok40 && ok48) goff=48;
      else if(ok40 && ok48){
        /* both look plausible (rare): prefer the one whose first glyph has a sane char code */
        if(u16(d,q40)>0x2FFF && u16(d,q48)<=0x2FFF) goff=48;
      } }
    uint32_t gc=u32(d,p+goff);
    if(gc>100000) gc=0;                             /* guard */
    f->glyphs=calloc(gc?gc:1,sizeof(GmlGlyph));
    f->n_glyphs=(int)gc;
    for(uint32_t g=0;g<gc && f->glyphs;g++){
      uint32_t q=u32(d,p+goff+4+g*4);               /* pointer to glyph record */
      GmlGlyph *gl=&f->glyphs[g];
      gl->ch=u16(d,q);
      gl->sx=tsx+(int)u16(d,q+2); gl->sy=tsy+(int)u16(d,q+4);   /* absolute atlas coords */
      gl->w=(int)u16(d,q+6); gl->h=(int)u16(d,q+8);
      gl->shift=(int16_t)u16(d,q+10);               /* advance */
      gl->offset=(int16_t)u16(d,q+12);              /* left bearing */
      if(gl->ch<256) f->glyph_by_char[gl->ch]=(int)g;
    }
    /* For floating-point EmSize, increase line height to the tallest glyph when needed. */
    if(em_is_float){ int mh=0;
      for(int g2=0;g2<f->n_glyphs;g2++) if(f->glyphs[g2].h>mh) mh=f->glyphs[g2].h;
      if(mh>f->line_height) f->line_height=mh; }
    if(getenv("GML_LOG_FONT"))
      fprintf(stderr,"[font] real id=%d name=%s em=%d atlas=%d glyphs=%d\n",
        i, gml_str_by_ptr(r->win,u32(d,p)), f->line_height, f->atlas, f->n_glyphs);
  }
  if(r->n_fonts<nf) r->n_fonts=nf;                  /* sprite fonts number after real fonts */
}

/* ---- recognized display post-processes: none retained in this revision ---- */

/* ---- real FONT-chunk font helpers ---- */
static GmlGlyph *real_glyph(GmlFont *f, unsigned cp){
  if(cp<256){ int gi=f->glyph_by_char[cp]; return gi>=0? &f->glyphs[gi] : NULL; }
  for(int i=0;i<f->n_glyphs;i++) if(f->glyphs[i].ch==cp) return &f->glyphs[i];
  return NULL;
}
/* advance width of one line (up to '#'/NUL), '\#' counts as a literal '#'. */
static int real_line_width(GmlFont *f, const char *p, const char **end){
  int w=0;
  while(*p && *p!='#'){
    unsigned cp=text_next_cp(&p);
    GmlGlyph *g=real_glyph(f,cp);
    if(g) w+=g->shift;
  }
  *end=p; return w;
}
/* Draw glyph atlas rectangles top-aligned and advance the pen by each shift.
 * Rotation changes pen positions; glyph rectangles remain axis-aligned. */
static void draw_text_real(GmlRender *r, GmlFont *f, double x, double y, const char *str,
                           double xs, double ys, double ca, double sa, int use_rot,
                           uint32_t blend, double alpha){
  int lh=f->line_height>0? f->line_height:12;
  int nlines=1; for(const char *q=str;*q;q++){ if(*q=='\\'&&q[1]=='#'){q++;continue;} if(*q=='#') nlines++; }
  double base_y=0;
  if(r->valign==1) base_y=-(nlines*lh)/2.0; else if(r->valign==2) base_y=-nlines*lh;
  const char *p=str;
  for(int li=0; *p || li==0; li++){
    const char *end; int lw=real_line_width(f,p,&end);
    double cx=0;
    if(r->halign==1) cx=-lw/2.0; else if(r->halign==2) cx=-lw;
    while(p<end){
      unsigned cp=text_next_cp(&p);
      GmlGlyph *g=real_glyph(f,cp);
      if(g && g->w>0 && g->h>0){
        GmlTpag gt={ .sx=g->sx,.sy=g->sy,.sw=g->w,.sh=g->h,.tx=0,.ty=0,.bw=g->w,.bh=g->h,.atlas=f->atlas };
        double dx=(cx+g->offset)*xs, dy=base_y*ys;
        if(use_rot) blit(r,&gt, x + dx*ca + dy*sa - r->cam_x, y - dx*sa + dy*ca - r->cam_y, xs,ys, blend, alpha);
        else        blit(r,&gt, x + dx - r->cam_x, y + dy - r->cam_y, xs,ys, blend, alpha);
      }
      if(g) cx += g->shift;
    }
    base_y += lh;
    if(*end=='#') p=end+1; else break;
  }
}

int gml_text_width(GmlRender *r, const char *str){
  if(r->font<0||r->font>=r->n_fonts||!str) return 0;
  GmlFont *f=&r->fonts[r->font];
  int best=0; const char *p=str;
  for(;;){
    const char *end; int w = f->real ? real_line_width(f,p,&end) : line_width(r,f,p,&end);
    if(w>best) best=w;
    if(*end!='#') break;
    p=end+1;
  }
  return best;
}
int gml_text_height(GmlRender *r, const char *str){
  if(r->font<0||r->font>=r->n_fonts) return 0;
  GmlFont *f=&r->fonts[r->font];
  int lh, nlines=1;
  if(f->real) lh=f->line_height>0?f->line_height:12;
  else { GmlSprite *s=&r->spr[f->sprite]; lh=s->h>0?s->h:8; }
  if(str) for(const char *p=str;*p;p++){ if(*p=='\\'&&p[1]=='#'){p++;continue;} if(*p=='#') nlines++; }
  return lh*nlines;
}
void gml_draw_text_transformed(GmlRender *r, double x, double y, const char *str,
                               double xs, double ys, double rot, uint32_t blend, double alpha){
  if(r->font<0 && str && str[0] && getenv("GML_LOG_FONT")) fprintf(stderr,"[font] draw_text font=%d str=\"%.30s\" — INVISIBLE (no default font)\n",r->font,str);
  if(r->font<0||r->font>=r->n_fonts||!str) return;
  if(alpha>1) alpha=1; else if(alpha<0) alpha=0;
  if(xs==0||ys==0||alpha<=0) return;
  double rr=fmod(rot,360.0); if(rr<0) rr+=360.0;
  double ang=rr*M_PI/180.0, ca=cos(ang), sa=sin(ang);
  int use_rot = fabs(rr)>0.001 && fabs(rr-360.0)>0.001;
  if(getenv("GML_LOG_TEXT")) fprintf(stderr,"[text] x=%.0f y=%.0f font=%d halign=%d valign=%d scale=(%.2f,%.2f) rot=%.1f col=%06X a=%.2f \"%s\"\n",
    x,y,r->font,r->halign,r->valign,xs,ys,rr,(unsigned)(blend&0xffffff),alpha,str);
  GmlFont *f=&r->fonts[r->font];
  if(f->real){ draw_text_real(r,f,x,y,str,xs,ys,ca,sa,use_rot,blend,alpha); return; }
  if(f->sprite<0 || f->sprite>=r->n_spr) return;
  GmlSprite *s=&r->spr[f->sprite];
  if(s->n_frames<=0) return;
  /* line height = sprite cell height (GM uses the sprite's declared h, not the
   * glyph's trimmed sub-rect sh — otherwise text lines collapse when glyphs are cropped). */
  int lh=s->h; if(lh<=0) lh=8;
  /* count lines for valign */
  int nlines=1; for(const char *p=str;*p;p++){ if(*p=='\\'&&p[1]=='#'){p++;continue;} if(*p=='#') nlines++; }
  double base_y=0;
  if(r->valign==1) base_y=-(nlines*lh)/2.0; else if(r->valign==2) base_y=-nlines*lh;
  const char *p=str;
  for(int li=0; *p || li==0; li++){
    const char *end; int lw=line_width(r,f,p,&end);
    double base_x=0;
    if(r->halign==1) base_x=-lw/2.0; else if(r->halign==2) base_x=-lw;
    double cx=base_x;
    while(p<end){
      unsigned cp=text_next_cp(&p);
      int fr=glyph_frame(f,cp);
      if(fr>=0 && fr<s->n_frames){
        if(s->runtime_rgba){
          const uint8_t *fr_rgba=runtime_frame_rgba(s,fr);
          if(fr_rgba){
            if(use_rot){
              double gx=x + cx*xs*ca + base_y*ys*sa;
              double gy=y - cx*xs*sa + base_y*ys*ca;
              blit_rgba_sprite(r,fr_rgba,s->w,s->h,gx,gy,xs,ys,rr,0,0,blend,alpha,1);
            } else {
              blit_rgba_sprite(r,fr_rgba,s->w,s->h,x+cx*xs,y+base_y*ys,xs,ys,0,0,0,blend,alpha,1);
            }
          }
        } else if(s->frame){ int ti=s->frame[fr];
          if(ti>=0 && ti<r->n_tpag){ GmlTpag *t=&r->tpag[ti];
          if(use_rot){
            double ox=(cx+s->originx)*xs;
            double oy=(base_y+s->originy)*ys;
            double gx=x + ox*ca + oy*sa;
            double gy=y - ox*sa + oy*ca;
            blit_rotated(r,s,t,gx,gy,xs,ys,rr,blend,alpha);
          } else {
            blit(r,t, x + cx*xs - r->cam_x + t->tx*xs,
              y + base_y*ys - r->cam_y + t->ty*ys, xs,ys, blend, alpha);
          } } }
        }
      cx += glyph_w(r,f,fr)+f->sep;
    }
    base_y += lh;
    if(*end=='#') p=end+1; else break;
  }
}
void gml_draw_text(GmlRender *r, double x, double y, const char *str){
  if(!r) return;
  gml_draw_text_transformed(r,x,y,str,1,1,0,r->color,r->alpha);
}
/* Wrap between words at pixel width w (-1 disables wrapping).
 * sep selects line separation; -1 selects the font default. */
void gml_draw_text_ext(GmlRender *r, double x, double y, const char *str, double sep, double w){
  if(!r || !str || r->font<0 || r->font>=r->n_fonts){ return; }
  char wrapped[2048]; size_t o=0;
  if(w>0){
    char word[256]; size_t wl=0;
    char line[1024]; size_t ll=0; line[0]=0;
    const char *p=str;
    for(;;){
      char c=*p;
      int end_word = (c==' '||c=='#'||c==0||(c=='\\'&&p[1]=='#'));
      if(!end_word){ if(wl<sizeof(word)-1) word[wl++]=c; p++; continue; }
      word[wl]=0;
      if(wl){
        char probe[1300];
        if(ll) snprintf(probe,sizeof probe,"%s %s",line,word);
        else snprintf(probe,sizeof probe,"%s",word);
        if(ll && gml_text_width(r,probe)>(int)w){
          /* flush current line */
          for(size_t k=0;k<ll && o<sizeof(wrapped)-2;k++) wrapped[o++]=line[k];
          wrapped[o++]='#';
          snprintf(line,sizeof line,"%s",word); ll=strlen(line);
        } else { snprintf(line,sizeof line,"%s",probe); ll=strlen(line); }
        wl=0;
      }
      if(c=='#'){ for(size_t k=0;k<ll && o<sizeof(wrapped)-2;k++) wrapped[o++]=line[k];
        wrapped[o++]='#'; ll=0; line[0]=0; }
      if(c==0) break;
      p += (c=='\\')?2:1;
    }
    for(size_t k=0;k<ll && o<sizeof(wrapped)-1;k++) wrapped[o++]=line[k];
    wrapped[o]=0;
    str=wrapped;
  }
  if(sep<=0){ gml_draw_text_transformed(r,x,y,str,1,1,0,r->color,r->alpha); return; }
  /* custom line separation: draw line by line at y + i*sep */
  int nlines=1; for(const char *q=str;*q;q++){ if(*q=='\\'&&q[1]=='#'){q++;continue;} if(*q=='#') nlines++; }
  double y0=y;
  if(r->valign==1) y0=y-(nlines*sep)/2.0; else if(r->valign==2) y0=y-nlines*sep;
  int sv=r->valign; r->valign=0;
  const char *p=str; int li=0;
  char lbuf[1024];
  while(1){
    size_t k=0;
    while(*p && !( *p=='#' && (p==str || p[-1]!='\\') ) && k<sizeof(lbuf)-1) lbuf[k++]=*p++;
    lbuf[k]=0;
    gml_draw_text_transformed(r,x,y0+li*sep,lbuf,1,1,0,r->color,r->alpha);
    if(*p!='#') break;
    p++; li++;
  }
  r->valign=sv;
}
