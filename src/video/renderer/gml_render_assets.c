/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Renderer-owned asset lookup and record operations. */
#include "gml_render_cpu_backend.h"
#include "gml_render_internal.h"
#include "gml_image_codec.h"
#include "gmlc_json.h"
#include "anygm_host.h"
#include "anygm_vfs.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gml_render_sprite_metrics(const GmlRender *R,int sprite,
                              GmlRenderSpriteMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!R || sprite<0 || sprite>=R->n_spr) return 0;
  const GmlSprite *source=&R->spr[sprite];
  if(metrics){
    metrics->width=source->w; metrics->height=source->h;
    metrics->origin_x=source->originx; metrics->origin_y=source->originy;
    metrics->frame_count=source->n_frames;
    metrics->collision_left=source->ml; metrics->collision_top=source->mt;
    metrics->collision_right=source->mr; metrics->collision_bottom=source->mb;
    metrics->collision_box=source->collision_kind==1;
    metrics->playback_speed=source->playback_speed;
    metrics->playback_speed_type=source->playback_speed_type;
    metrics->playback_speed_valid=source->playback_speed_valid;
    metrics->name=source->name;
  }
  return 1;
}

/* Information queries stream values and never borrow or decode image pixels. */
int gml_render_sprite_information(const GmlRender *render,int sprite,
                                   GmlRenderInfoSink sink,void *context){
  if(!sink || !render || sprite<0 || sprite>=render->n_spr ||
     (!render->spr[sprite].n_frames && !render->spr[sprite].spine)) return 0;
  const GmlSprite *source=&render->spr[sprite];
#define INFO(kind,key,number,text) do { if(!sink(context,key,kind,number,text)) return 0; } while(0)
#define INFO_NUM(key,value) INFO(GML_RENDER_INFO_NUMBER,key,value,NULL)
#define INFO_END() INFO(GML_RENDER_INFO_END,NULL,0,NULL)
  INFO(GML_RENDER_INFO_OBJECT,NULL,0,NULL);
  INFO_NUM("width",source->w); INFO_NUM("height",source->h);
  INFO_NUM("xoffset",source->originx); INFO_NUM("yoffset",source->originy);
  INFO_NUM("bbox_left",source->ml); INFO_NUM("bbox_top",source->mt);
  INFO_NUM("bbox_right",source->mr); INFO_NUM("bbox_bottom",source->mb);
  INFO(GML_RENDER_INFO_STRING,"name",0,source->name?source->name:"");
  INFO_NUM("transparent",(source->metadata_flags&GML_SPRITE_META_TRANSPARENT)!=0);
  INFO_NUM("smooth",(source->metadata_flags&GML_SPRITE_META_SMOOTH)!=0);
  INFO_NUM("preload",(source->metadata_flags&GML_SPRITE_META_PRELOAD)!=0);
  int type=(int)((source->metadata_flags>>GML_SPRITE_META_TYPE_SHIFT)&3u);
  INFO_NUM("type",type);
  INFO_NUM("num_subimages",source->n_frames);
  INFO_NUM("frame_speed",source->playback_speed_valid?source->playback_speed:1);
  INFO_NUM("frame_type",source->playback_speed_type);
  INFO_NUM("use_mask",source->collision_kind!=1 &&
    !(source->metadata_flags&GML_SPRITE_META_ROTATED));
  INFO_NUM("num_masks",source->mask_count);
  INFO_NUM("rotated_bounds",(source->metadata_flags&GML_SPRITE_META_ROTATED)!=0);
  if((source->metadata_flags&GML_SPRITE_META_NINESLICE) || source->ns_enabled){
    INFO(GML_RENDER_INFO_OBJECT,"nineslice",0,NULL);
    INFO_NUM("enabled",source->ns_enabled);
    INFO_NUM("left",source->ns_l); INFO_NUM("right",source->ns_r);
    INFO_NUM("top",source->ns_t); INFO_NUM("bottom",source->ns_b);
    INFO(GML_RENDER_INFO_ARRAY,"tilemode",5,NULL);
    for(int i=0;i<5;i++) INFO_NUM(NULL,source->ns_tile[i]);
    INFO_END(); INFO_END();
  }else INFO(GML_RENDER_INFO_UNDEFINED,"nineslice",0,NULL);
  if(type==0){
    INFO(GML_RENDER_INFO_ARRAY,"frames",source->n_frames,NULL);
    for(int i=0;i<source->n_frames;i++){
      const GmlTpag *page=NULL;
      if(!source->runtime_rgba && source->frame && source->frame[i]>=0 &&
         source->frame[i]<render->n_tpag) page=&render->tpag[source->frame[i]];
      INFO(GML_RENDER_INFO_OBJECT,NULL,0,NULL);
      INFO_NUM("x",page?page->sx:0); INFO_NUM("y",page?page->sy:0);
      INFO_NUM("w",page?page->sw:source->w); INFO_NUM("h",page?page->sh:source->h);
      INFO_NUM("x_offset",page?page->tx:0); INFO_NUM("y_offset",page?page->ty:0);
      INFO_NUM("crop_width",page?page->tw:source->w);
      INFO_NUM("crop_height",page?page->th:source->h);
      INFO_NUM("original_width",page?page->bw:source->w);
      INFO_NUM("original_height",page?page->bh:source->h);
      int texture=page?gml_render_atlas_texture_handle(render,page->atlas):
        gml_render_sprite_texture_handle(sprite,i);
      INFO_NUM("texture",texture);
      INFO_END();
    }
    INFO_END();
  }
  /* This interface does not expose decoded timeline or skeletal collections.
   * Undefined does not assert that authored messages or stretched frames are absent. */
  INFO(GML_RENDER_INFO_UNDEFINED,"messages",0,NULL);
  INFO(GML_RENDER_INFO_UNDEFINED,"frame_info",0,NULL);
  INFO_END();
#undef INFO_END
#undef INFO_NUM
#undef INFO
  return 1;
}

int gml_render_sprite_collision_metrics(const GmlRender *r,int sprite,
                                        GmlRenderSpriteMetrics *metrics){
  if(!gml_render_sprite_metrics(r,sprite,metrics)) return 0;
  if(metrics && r->spr[sprite].bbox_mode==1){
    metrics->collision_left=metrics->collision_top=0;
    metrics->collision_right=metrics->width-1;
    metrics->collision_bottom=metrics->height-1;
  }
  return 1;
}

int gml_render_sprite_set_playback(GmlRender *R,int sprite,
                                   double speed,int speed_type){
  if(!R || sprite<0 || sprite>=R->n_spr) return 0;
  R->spr[sprite].playback_speed=(float)speed;
  R->spr[sprite].playback_speed_type=speed_type==0?0:1;
  R->spr[sprite].playback_speed_valid=1;
  return 1;
}

int gml_render_sprite_texture_handle(int sprite,int image){
  if(sprite<0) return -1;
  return (int)(GML_TEX_SPR_TAG|((sprite&0xFFFF)<<10)|(image&0x3FF));
}
int gml_render_atlas_texture_handle(const GmlRender *r,int atlas){
  return r && atlas>=0 && atlas<r->n_atlas && atlas<=0x00FFFFFF
    ?(int)(GML_TEX_ATLAS_TAG|(uint32_t)atlas):-1;
}

int gml_render_surface_texture_handle(int surface){
  if(surface<0) return -1;
  return (int)(GML_TEX_SURF_TAG|((unsigned)surface&0xFFFFu));
}

int gml_render_background_metrics(const GmlRender *R,int background,
                                  GmlRenderBackgroundMetrics *metrics){
  if(metrics){ memset(metrics,0,sizeof(*metrics)); metrics->texture_page=-1; metrics->atlas=-1; }
  if(!R || background<0 || background>=R->n_bg) return 0;
  const GmlBg *source=&R->bg[background];
  if(metrics){
    metrics->tile_width=source->tile_w; metrics->tile_height=source->tile_h;
    metrics->tile_border_x=source->tile_border_x;
    metrics->tile_border_y=source->tile_border_y;
    metrics->tile_separation_x=source->tile_separation_x;
    metrics->tile_separation_y=source->tile_separation_y;
    metrics->tile_columns=source->tile_columns;
    metrics->transparent=source->transparent;
    metrics->smooth=source->smooth;
    metrics->preload=source->preload;
    metrics->name=source->name;
    if(source->tpag>=0 && source->tpag<R->n_tpag){
      const GmlTpag *page=&R->tpag[source->tpag];
      metrics->texture_page=source->tpag; metrics->atlas=page->atlas;
      metrics->packed_x=page->sx; metrics->packed_y=page->sy;
      metrics->packed_width=page->sw; metrics->packed_height=page->sh;
      metrics->trim_x=page->tx; metrics->trim_y=page->ty;
      metrics->declared_width=page->bw; metrics->declared_height=page->bh;
      metrics->logical_width=page->bw?page->bw:page->sw;
      metrics->logical_height=page->bh?page->bh:page->sh;
      if(page->atlas>=0 && page->atlas<R->n_atlas){
        const GmlAtlas *atlas=&R->atlas[page->atlas];
        metrics->content_texture=atlas->blob!=0 || atlas->external_blob!=NULL;
      }
    }
  }
  return 1;
}

int gml_render_background_texture_handle(const GmlRender *R,int background){
  return R && background>=0 && background<R->n_bg
    ? (int)(GML_TEX_BG_TAG|((unsigned)background&0x00FFFFFFu)) : -1;
}

int gml_render_background_tile_animation_frame(const GmlRender *R,int background,
                                                double elapsed_seconds){
  if(!R || background<0 || background>=R->n_bg || !isfinite(elapsed_seconds) ||
     elapsed_seconds<=0.0) return 0;
  const GmlBg *source=&R->bg[background];
  if(source->tile_items_per_tile<=1 || !source->tile_frame_length_us) return 0;
  double frame_seconds=(double)source->tile_frame_length_us/1000000.0;
  double period=frame_seconds*source->tile_items_per_tile;
  if(!isfinite(frame_seconds) || frame_seconds<=0.0 ||
     !isfinite(period) || period<=0.0) return 0;
  double within=fmod(elapsed_seconds,period);
  if(within<0.0) within+=period;
  int frame=(int)floor(within/frame_seconds);
  return frame>=source->tile_items_per_tile?source->tile_items_per_tile-1:frame;
}

int gml_render_background_tile_source_index(const GmlRender *R,int background,
                                             int tile_index,int animation_frame){
  if(!R || background<0 || background>=R->n_bg || tile_index<0) return -1;
  const GmlBg *source=&R->bg[background];
  if(source->tile_ids && source->tile_items_per_tile>0 && tile_index<source->tile_count){
    int frame=animation_frame%source->tile_items_per_tile;
    if(frame<0) frame+=source->tile_items_per_tile;
    const uint8_t *id=source->tile_ids+
      ((size_t)tile_index*(size_t)source->tile_items_per_tile+(size_t)frame)*4u;
    return (int)((uint32_t)id[0]|((uint32_t)id[1]<<8)|
                 ((uint32_t)id[2]<<16)|((uint32_t)id[3]<<24));
  }
  return source->tile_ids?tile_index:tile_index-1;
}

int gml_render_tileset_layout(const GmlRender *R,int background,
                              int fallback_width,int fallback_height,
                              GmlRenderTilesetLayout *layout){
  GmlRenderBackgroundMetrics metrics;
  if(!layout || !gml_render_background_metrics(R,background,&metrics) ||
     metrics.texture_page<0) return 0;
  int width=metrics.tile_width>0?metrics.tile_width:fallback_width;
  int height=metrics.tile_height>0?metrics.tile_height:fallback_height;
  int64_t pitch_x=(int64_t)width+2LL*metrics.tile_border_x+metrics.tile_separation_x;
  int64_t pitch_y=(int64_t)height+2LL*metrics.tile_border_y+metrics.tile_separation_y;
  if(width<=0 || height<=0 || pitch_x<=0 || pitch_x>INT_MAX ||
     pitch_y<=0 || pitch_y>INT_MAX || metrics.tile_border_x<0 || metrics.tile_border_y<0 ||
     metrics.logical_width<=0 || metrics.logical_height<=0) return 0;
  int columns=metrics.tile_columns>0?metrics.tile_columns:metrics.logical_width/(int)pitch_x;
  if(columns<=0) return 0;
  *layout=(GmlRenderTilesetLayout){background,width,height,
    metrics.tile_border_x,metrics.tile_border_y,(int)pitch_x,(int)pitch_y,columns,
    metrics.logical_width,metrics.logical_height};
  return 1;
}

int gml_render_tileset_source(const GmlRender *R,const GmlRenderTilesetLayout *layout,
                              uint32_t datum,int frame,GmlRenderTileSource *source){
  int index=(int)(datum&0x7ffffu);
  if(!R || !layout || !source || index==0 || layout->background<0 ||
     layout->background>=R->n_bg || layout->columns<=0) return 0;
  int mapped=gml_render_background_tile_source_index(R,layout->background,index,frame);
  /* An animated empty frame is not atlas cell zero. Unmapped legacy layouts,
   * however, legitimately use that cell for their first nonempty tile. */
  if(mapped<0 || (mapped==0 && R->bg[layout->background].tile_ids)) return 0;
  int64_t x=(int64_t)(mapped%layout->columns)*layout->pitch_x+layout->border_x;
  int64_t y=(int64_t)(mapped/layout->columns)*layout->pitch_y+layout->border_y;
  if(x<0 || y<0 || x>=layout->source_width || y>=layout->source_height) return 0;
  int width=layout->width,height=layout->height;
  if(width>layout->source_width-x) width=(int)(layout->source_width-x);
  if(height>layout->source_height-y) height=(int)(layout->source_height-y);
  if(width<=0 || height<=0) return 0;
  *source=(GmlRenderTileSource){(int)x,(int)y,width,height};
  return 1;
}

int gml_render_font_metrics(const GmlRender *R,int font,
                            GmlRenderFontMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!R) return 0;
  const GmlFont *source;
  if(font<0) source=&R->default_font;
  else {
    if(font>=R->n_fonts) return 0;
    source=&R->fonts[font];
  }
  if(metrics){
    metrics->line_height=source->line_height;
    metrics->ascender=source->ascender;
    metrics->ascender_offset=source->ascender_offset;
    metrics->sdf_spread=source->sdf_spread;
    metrics->bold=source->bold;
    metrics->italic=source->italic;
    metrics->sprite=source->sprite;
    metrics->first=source->first;
    metrics->proportional=source->prop;
    metrics->separation=source->sep;
    metrics->sprite_backed=!source->real;
    metrics->glyph_count=source->n_glyphs;
  }
  return 1;
}

int gml_render_font_glyph_metrics(const GmlRender *R,int font,int glyph,
                                  GmlRenderFontGlyphMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  if(!R || font<0 || font>=R->n_fonts || glyph<0 ||
     glyph>=R->fonts[font].n_glyphs || !R->fonts[font].glyphs) return 0;
  const GmlGlyph *source=&R->fonts[font].glyphs[glyph];
  if(metrics){
    metrics->character=source->ch;
    metrics->x=source->sx; metrics->y=source->sy;
    metrics->width=source->w; metrics->height=source->h;
    metrics->shift=source->shift; metrics->offset=source->offset;
  }
  return 1;
}

int gml_render_font_texture_handle(const GmlRender *R,int font){
  if(!R || font<0 || font>=R->n_fonts) return -1;
  const GmlFont *source=&R->fonts[font];
  if(source->real && source->atlas>=0 && source->atlas<R->n_atlas)
    return (int)(GML_TEX_FONT_TAG|((unsigned)font&0x00FFFFFFu));
  if(source->sprite>=0) return gml_render_sprite_texture_handle(source->sprite,0);
  return -1;
}

int gml_render_font_exists(const GmlRender *R,int font){
  if(!R || font<0 || font>=R->n_fonts) return 0;
  const GmlFont *source=&R->fonts[font];
  return source->real || source->sprite>=0;
}

int gml_render_texture_metrics(GmlRender *R,int handle,
                               GmlRenderTextureMetrics *metrics){
  if(metrics) memset(metrics,0,sizeof(*metrics));
  GmlRenderCpuBackendTextureView view;
  if(!metrics || !gml_render_cpu_backend_texture_view(R,handle,0,&view)) return 0;
  uint32_t kind=(uint32_t)handle&GML_TEX_KIND_MASK;
  if(kind==GML_TEX_SPR_TAG) metrics->kind=GML_RENDER_TEXTURE_SPRITE;
  else if(kind==GML_TEX_SURF_TAG) metrics->kind=GML_RENDER_TEXTURE_SURFACE;
  else if(kind==GML_TEX_BG_TAG) metrics->kind=GML_RENDER_TEXTURE_BACKGROUND;
  else if(kind==GML_TEX_FONT_TAG) metrics->kind=GML_RENDER_TEXTURE_FONT;
  else if(kind==GML_TEX_ATLAS_TAG) metrics->kind=GML_RENDER_TEXTURE_ATLAS;
  else return 0;
  metrics->runtime=view.runtime;
  metrics->atlas_backed=view.pixel_kind==GML_RENDER_CPU_BACKEND_PIXELS_RGBA &&
    !view.runtime;
  metrics->logical_width=view.logical_width;
  metrics->logical_height=view.logical_height;
  metrics->trim_x=view.trim_x;
  metrics->trim_y=view.trim_y;
  metrics->width=view.width;
  metrics->height=view.height;
  metrics->source_x=view.source_x;
  metrics->source_y=view.source_y;
  metrics->full_width=view.full_width;
  metrics->full_height=view.full_height;
  return 1;
}

int gml_sprite_exists(GmlRender *r, int sprite){
  return r && sprite>=0 && sprite<r->n_spr &&
         (r->spr[sprite].n_frames>0 || r->spr[sprite].spine);
}
int gml_sprite_frames(GmlRender *r, int sprite){
  return r && sprite>=0 && sprite<r->n_spr ? r->spr[sprite].n_frames : 0;
}

int gml_render_sprite_frame_rect(GmlRender *r,int sprite,int frame,int *atlas,int *sx,int *sy,
                                 int *w,int *h){
  return gml_render_sprite_frame_rect_full(r,sprite,frame,atlas,sx,sy,w,h,NULL,NULL);
}

/* The sprite frame's own texture-page rectangle, or a refusal for a runtime-built sprite. */
int gml_render_sprite_frame_rect_full(GmlRender *r,int sprite,int frame,int *atlas,int *sx,int *sy,
                                      int *w,int *h,int *tx,int *ty){
  GmlSprite *s;
  int ti;
  if(!r || sprite<0 || sprite>=r->n_spr) return 0;
  s=&r->spr[sprite];
  if(s->runtime_rgba || !s->frame || s->n_frames<=0) return 0;
  if(frame<0 || frame>=s->n_frames) frame=0;
  ti=s->frame[frame];
  if(ti<0 || ti>=r->n_tpag) return 0;
  {
    GmlTpag *t=&r->tpag[ti];
    if(t->atlas<0 || t->atlas>=r->n_atlas || t->sw<=0 || t->sh<=0) return 0;
    *atlas=t->atlas; *sx=t->sx; *sy=t->sy; *w=t->sw; *h=t->sh;
    if(tx) *tx=t->tx;
    if(ty) *ty=t->ty;
    return 1;
  }
}
