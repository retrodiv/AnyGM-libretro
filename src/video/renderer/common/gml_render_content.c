/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing TPAG, sprite, and background record initialisation. */
#include "gml_render_content.h"
#include "gml_render_internal.h"
#include "anygm_compatibility.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- TPAG ---- */
static void parse_tpag(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"TPAG"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_tpag=(int)n; r->tpag=calloc(n,sizeof(GmlTpag)); r->tpag_ptr=calloc(n,sizeof(uint32_t));
  if(!r->tpag || !r->tpag_ptr){ free(r->tpag); free(r->tpag_ptr); r->tpag=NULL; r->tpag_ptr=NULL; r->n_tpag=0; return; }
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4); r->tpag_ptr[i]=p;
    GmlTpag *t=&r->tpag[i];
    t->sx=u16(d,p); t->sy=u16(d,p+2); t->sw=u16(d,p+4); t->sh=u16(d,p+6);
    t->tx=u16(d,p+8); t->ty=u16(d,p+10); t->tw=u16(d,p+12); t->th=u16(d,p+14);
    t->bw=u16(d,p+16); t->bh=u16(d,p+18);
    t->atlas=(int16_t)u16(d,p+20);
  }
}
int tpag_index_for_ptr(GmlRender *r, uint32_t ptr){
  for(int i=0;i<r->n_tpag;i++) if(r->tpag_ptr[i]==ptr) return i;
  return -1;
}
/* ---- SPRT ---- */
static void parse_sprt(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"SPRT"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_spr=(int)n; r->spr=calloc(n,sizeof(GmlSprite)); r->spr_cap=(int)n; r->spr_has_free=0;
  if(!r->spr){ r->n_spr=0; r->spr_cap=0; return; }
  for(uint32_t i=0;i<n;i++){
    uint32_t p=u32(d,c->off+4+i*4);
    if(!p) continue;
    GmlSprite *s=&r->spr[i];
    s->name=gml_str_by_ptr(r->win,u32(d,p));
    s->playback_speed=1.0f; s->playback_speed_type=1; s->playback_speed_valid=0;
    s->w=(int)u32(d,p+4); s->h=(int)u32(d,p+8);
    s->ml=(int)u32(d,p+12); s->mr=(int)u32(d,p+16); s->mb=(int)u32(d,p+20); s->mt=(int)u32(d,p+24);
    /* The native mask type selects an axis-aligned box (0) or a precise plane (1).
     * It is not a separate-subimage flag: one shared plane can still be precise.
     * Other native types retain their existing sampled geometry. */
    s->collision_kind=u32(d,p+44)==0?1:0;
    s->collision_tolerance=63; /* preserve the old alpha>=64 fallback */
    /* Header: ...,BBoxMode(40),MaskType(44),OriginX(48),OriginY(52),frameList(56). */
    s->originx=(int)u32(d,p+48); s->originy=(int)u32(d,p+52);
    uint32_t list=p+56;             /* GMS1: SimpleList<TextureEntry> here (count + pointers) */
    /* GMS2 sprite: a -1 marker at +56, then SVersion(+60), SpriteType(+64), and for a normal sprite
     * PlaybackSpeed(+68 float)+PlaybackSpeedType(+72), plus SequenceOffset (SVersion>=2) and
     * NineSliceOffset (SVersion>=3) — the texture list only starts after all that. Reading +56 as the
     * frame count (=-1) would zero the frame list and blank every sprite. */
    if(u32(d,p+56)==0xFFFFFFFFu){
      uint32_t sver=u32(d,p+60), stype=u32(d,p+64);
      float playback; memcpy(&playback,d+p+68,sizeof playback);
      uint32_t playback_type=u32(d,p+72);
      if(isfinite(playback) && playback>=0.0f && playback_type<=1){
        s->playback_speed=playback;
        s->playback_speed_type=(int)playback_type;
        s->playback_speed_valid=1;
      }
      if(stype!=0){
        s->n_frames=0;
        s->frame=calloc(1,sizeof(int));
        if(stype==2){
          uint32_t header=p+76+(sver>=2?4:0)+(sver>=3?4:0);
          /* Some packages place a texture-page list before the skeletal record. */
          if(header+12<=r->win->size){
            uint32_t count=u32(d,header),candidate=header+4+count*4u;
            if(count>0 && count<=16 && candidate+20<=r->win->size &&
               u32(d,candidate)>=1 && u32(d,candidate)<=3)
              header=candidate;
          }
          (void)gml_render_parse_spine(r,s,p,header);
        }
        continue;
      }
      uint32_t fl=76;                         /* after PlaybackSpeed(+68)+PlaybackSpeedType(+72) */
      if(sver>=2) fl+=4;                       /* SequenceOffset */
      if(sver>=3){                             /* NineSliceOffset */
        uint32_t nso=u32(d,p+80);
        if(nso && nso+40<=r->win->size && u32(d,nso+16)){
          /* NineSlice layout: Left,Top,Right,Bottom (i32), Enabled, TileModes[5]
           * (left,top,right,bottom,center). GM keeps the borders at native size when the
           * sprite draws scaled — stretching them uniformly deforms UI boards/bubbles. */
          s->ns_l=(int)(int32_t)u32(d,nso); s->ns_t=(int)(int32_t)u32(d,nso+4);
          s->ns_r=(int)(int32_t)u32(d,nso+8); s->ns_b=(int)(int32_t)u32(d,nso+12);
          for(int k=0;k<5;k++) s->ns_tile[k]=(int)(int32_t)u32(d,nso+20+4u*k);
          if(s->ns_l>=0 && s->ns_t>=0 && s->ns_r>=0 && s->ns_b>=0 &&
             s->ns_l+s->ns_r<=s->w && s->ns_t+s->ns_b<=s->h)
            s->ns_enabled=1;
        }
        fl+=4;
      }
      list=p+fl;
    }
    uint32_t fn=u32(d,list);
    if(fn>10000) fn=0;              /* guard against special-type sprites */
    s->n_frames=(int)fn; s->frame=calloc(fn?fn:1,sizeof(int));
    if(!s->frame){ s->n_frames=0; continue; }
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
/* ---- BGND ---- */
static void parse_bgnd(GmlRender *r){
  const GmlChunk *c=gml_chunk(r->win,"BGND"); if(!c) return;
  const uint8_t *d=r->win->data; uint32_t n=u32(d,c->off);
  r->n_bg=(int)n; r->bg=calloc(n,sizeof(GmlBg));
  if(!r->bg){ r->n_bg=0; return; }
  for(uint32_t i=0;i<n;i++){
	    uint32_t p=u32(d,c->off+4+i*4);
	    /* name, transparent, smooth, preload, texture(TPAG ptr) */
	    uint32_t tptr=u32(d,p+16);
	    r->bg[i].tpag=tpag_index_for_ptr(r,tptr);
	    r->bg[i].name=gml_str_by_ptr(r->win,u32(d,p));
	    r->bg[i].transparent=(int)(u32(d,p+4)!=0);
	    r->bg[i].smooth=(int)(u32(d,p+8)!=0);
	    r->bg[i].preload=(int)(u32(d,p+12)!=0);
	    if(anygm_policy_has_modern_layer_semantics(r->win) && p+64<c->off+c->size){
	      int ver=(int)u32(d,p+20), tw=(int)u32(d,p+24), th=(int)u32(d,p+28);
          /* The separated-border layout inserts separationX/Y before the output-border fields.
           * Validate both layouts structurally. Interpreting an older record as the new layout
           * makes its exported-sprite slot become ItemsPerTile (normally zero); a new record has
           * a complete count*frames table at +72. */
          int obx=(int)u32(d,p+32), oby=(int)u32(d,p+36), ocols=(int)u32(d,p+40);
          int oitems=(int)u32(d,p+44), ocount=(int)u32(d,p+48);
          uint64_t obytes=(uint64_t)(uint32_t)oitems*(uint64_t)(uint32_t)ocount*4u;
          int old_ok=ver>0 && tw>0 && th>0 && tw<=4096 && th<=4096 && obx>=0 && oby>=0 &&
                     ocols>0 && ocols<=4096 && oitems>0 && oitems<=1024 && ocount>0 &&
                     obytes<=4000000u && (uint64_t)p+64u+obytes<=(uint64_t)c->off+c->size;
          int nsep_x=(int)u32(d,p+32), nsep_y=(int)u32(d,p+36);
          int nbx=(int)u32(d,p+40), nby=(int)u32(d,p+44), ncols=(int)u32(d,p+48);
          int nitems=(int)u32(d,p+52), ncount=(int)u32(d,p+56);
          uint64_t nbytes=(uint64_t)(uint32_t)nitems*(uint64_t)(uint32_t)ncount*4u;
          int new_ok=ver>0 && tw>0 && th>0 && tw<=4096 && th<=4096 &&
                     nsep_x>=0 && nsep_x<=4096 && nsep_y>=0 && nsep_y<=4096 &&
                     nbx>=0 && nby>=0 && nbx<=4096 && nby<=4096 &&
                     ncols>0 && ncols<=4096 && nitems>0 && nitems<=1024 && ncount>0 &&
                     nbytes<=4000000u && (uint64_t)p+72u+nbytes<=(uint64_t)c->off+c->size;
          int modern=new_ok && (!old_ok || (ncount>ocount && ncols>ocols));
          if(old_ok || modern){
            int bx=modern?nbx:obx, by=modern?nby:oby, cols=modern?ncols:ocols;
            int items=modern?nitems:oitems, count=modern?ncount:ocount;
            r->bg[i].tile_w=tw; r->bg[i].tile_h=th;
            r->bg[i].tile_border_x=bx; r->bg[i].tile_border_y=by;
            r->bg[i].tile_separation_x=modern?nsep_x:0;
            r->bg[i].tile_separation_y=modern?nsep_y:0;
            r->bg[i].tile_columns=cols; r->bg[i].tile_items_per_tile=items; r->bg[i].tile_count=count;
            {
              uint32_t frame_off=p+(modern?64:56);
              r->bg[i].tile_frame_length_us=
                (uint64_t)u32(d,frame_off)|((uint64_t)u32(d,frame_off+4)<<32);
            }
            r->bg[i].tile_ids=d+p+(modern?72:64);
          }
	    }
	  }
	}

void gml_render_content_init(GmlRender *r){
  parse_tpag(r); parse_sprt(r); parse_bgnd(r);
}
