/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Existing runtime sprite/background image and collision-mask lifecycle. */
#include "gml_render_internal.h"
#include "gml_render_surfaces_internal.h"
#include "gml_image_codec.h"
#include "anygm_host.h"
#include "anygm_vfs.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sprite_backup(GmlSprite *s){
  if(s->base_valid) return;
  s->base_valid=1;
  s->base_originx=s->originx; s->base_originy=s->originy;
  s->base_w=s->w; s->base_h=s->h; s->base_n_frames=s->n_frames;
  s->base_ml=s->ml; s->base_mr=s->mr; s->base_mt=s->mt; s->base_mb=s->mb;
  s->base_mask=s->mask; s->base_mask_rowb=s->mask_rowb; s->base_mask_count=s->mask_count;
  s->base_collision_kind=s->collision_kind; s->base_collision_tolerance=s->collision_tolerance;
}
static void sprite_restore_base(GmlSprite *s){
  if(!s->base_valid) return;
  s->originx=s->base_originx; s->originy=s->base_originy;
  s->w=s->base_w; s->h=s->base_h; s->n_frames=s->base_n_frames;
  s->ml=s->base_ml; s->mr=s->base_mr; s->mt=s->base_mt; s->mb=s->base_mb;
  s->mask=s->base_mask; s->mask_rowb=s->base_mask_rowb; s->mask_count=s->base_mask_count;
  s->collision_kind=s->base_collision_kind; s->collision_tolerance=s->base_collision_tolerance;
  s->base_valid=0;
}
int gml_render_runtime_sprite_scene_source_view(GmlRender *r,GmlSprite *sprite,int frame,
                                                GmlRenderResourceSource *source){
  if(source) memset(source,0,sizeof *source);
  if(!r || !sprite || !source || !sprite->runtime_rgba ||
     sprite->w<=0 || sprite->h<=0 ||
     sprite->w>4096 || sprite->h>4096 ||
     sprite->n_frames<=0 || sprite->n_frames>4096) return 0;
  uint64_t bytes=(uint64_t)(uint32_t)sprite->w*(uint32_t)sprite->h*4u;
  if(bytes*(uint32_t)sprite->n_frames>SIZE_MAX) return 0;
  int sub=gml_render_runtime_frame_index(sprite,frame);
  if(!sprite->runtime_scene_incarnation){
    sprite->runtime_scene_incarnation=gml_render_scene_incarnation_allocate();
    if(!sprite->runtime_scene_incarnation) return 0;
  }
  /* A CPU-owned frame is immutable between the fenced mutations below. Keep
   * each subframe distinct from packaged pages and from every replaced buffer. */
  source->image=(GmlRenderImageRef){GML_RENDER_RESOURCE_ATLAS,
    UINT32_C(0x80000000)|(uint32_t)sub,sprite->runtime_scene_incarnation,1u};
  source->width=(uint32_t)sprite->w;source->height=(uint32_t)sprite->h;
  source->pixel_format=GML_RENDER_PIXEL_RGBA8888;
  source->stride_bytes=(uint32_t)sprite->w*4u;
  source->byte_capacity=(size_t)bytes;source->bytes=runtime_frame_rgba(sprite,sub);
  source->borrowed_immutable=1;
  sprite->runtime_scene_used=1;
  return 1;
}

int gml_render_runtime_sprite_scene_retire(GmlRender *r,GmlSprite *sprite){
  if(!r || !sprite) return 0;
  if(!sprite->runtime_scene_used) return 1;
  /* Flush borrowed views before freeing bytes. A refused prefix leaves the
   * old allocation alive; this is submission, not a synchronous CPU read. */
  if(!gml_render_application_scene_flush(r)) return 0;
  const GmlRenderDeviceServices *device=gml_render_device(r);
  if(device && device->retire)
    for(int sub=0;sub<sprite->n_frames;sub++)
      device->retire(device->context,(GmlRenderImageRef){GML_RENDER_RESOURCE_ATLAS,
        UINT32_C(0x80000000)|(uint32_t)sub,sprite->runtime_scene_incarnation,1u});
  sprite->runtime_scene_used=0;
  sprite->runtime_scene_incarnation=0;
  return 1;
}

static int sprite_set_runtime_rgba(GmlRender *r,GmlSprite *s, uint8_t *rgba, int w, int h, int frames, int xorig, int yorig, int extra){
  if(!gml_render_runtime_sprite_scene_retire(r,s)) return 0;
  if(frames<1) frames=1;
  if(!extra) sprite_backup(s);
  gml_render_sprite_cache_free(s);
  gml_render_sprite_state_cache_clear(s);
  free(s->runtime_rgba);
  free(s->runtime_mask);
  free(s->runtime_row_min);
  free(s->runtime_row_max);
  free(s->runtime_source_path);
  s->runtime_rgba=rgba;
  s->runtime_mask=NULL;
  s->runtime_row_min=NULL;
  s->runtime_row_max=NULL;
  s->runtime_source_path=NULL;
  s->runtime_source_imgnum=0;
  s->runtime_source_removeback=0;
  s->runtime_owned=1;
  s->runtime_extra=extra;
  s->runtime_opaque=1;
  s->runtime_scene_binary=0;
  s->w=w; s->h=h; s->originx=xorig; s->originy=yorig;
  s->n_frames=frames;
  s->ml=0; s->mt=0; s->mr=w>0?w-1:0; s->mb=h>0?h-1:0;
  s->mask=NULL; s->mask_rowb=0; s->mask_count=0;
  s->collision_kind=0; s->collision_tolerance=63;
  size_t rows=(size_t)frames*(size_t)h;
  s->runtime_row_min=malloc(rows*sizeof(int));
  s->runtime_row_max=malloc(rows*sizeof(int));
  if(s->runtime_row_min && s->runtime_row_max){
    s->runtime_scene_binary=1;
    for(int f=0; f<frames; f++) for(int yy=0; yy<h; yy++){
      int mn=w, mx=-1;
      const uint8_t *sp=rgba+((size_t)f*(size_t)w*(size_t)h+(size_t)yy*(size_t)w)*4;
      for(int xx=0; xx<w; xx++, sp+=4){
        if(sp[3]!=255) s->runtime_opaque=0;
        if(sp[3]!=0 && sp[3]!=255) s->runtime_scene_binary=0;
        if(sp[3]){
          if(xx<mn) mn=xx;
          if(xx>mx) mx=xx;
        }
      }
      s->runtime_row_min[(size_t)f*(size_t)h+(size_t)yy]=mn;
      s->runtime_row_max[(size_t)f*(size_t)h+(size_t)yy]=mx;
    }
  } else {
    free(s->runtime_row_min); free(s->runtime_row_max);
    s->runtime_row_min=NULL; s->runtime_row_max=NULL;
  }
  return 1;
}
static void sprite_set_runtime_source(GmlSprite *s, const char *path, int imgnum, int removeback){
  if(!s) return;
  free(s->runtime_source_path);
  s->runtime_source_path=path?strdup(path):NULL;
  s->runtime_source_imgnum=imgnum;
  s->runtime_source_removeback=removeback?1:0;
}

void gml_render_apply_removeback_rgba(uint8_t *rgba, int w, int h, int removeback){
  if(!removeback || !rgba || w<=0 || h<=0) return;
  const uint8_t *key=rgba+(size_t)(h-1)*(size_t)w*4u;
  uint8_t rr=key[0], gg=key[1], bb=key[2];
  for(int i=0;i<w*h;i++){
    uint8_t *p=rgba+i*4;
    if(p[0]==rr && p[1]==gg && p[2]==bb) p[3]=0;
  }
}

int gml_sprite_append_from_rgba_frames(GmlRender *r, uint8_t *rgba, int w, int h, int frames, int xorig, int yorig, const char *name){
  if(!rgba || w<=0 || h<=0 || frames<=0) return -1;
  if(render_setting(r,"GML_LOG_SPRGEN") && ++r->generated_sprite_log_count%2000==0)
    anygm_host_logf(r && r->win ? r->win->host : NULL,ANYGM_LOG_DEBUG,"[sprgen] %ld sprites appended (n_spr=%d)\n",r->generated_sprite_log_count,r->n_spr);
  int id=-1;
  /* Reuse a freed runtime slot only if one exists (spr_has_free); scanning all sprites on every
   * append is O(n) and turns bulk generation into
   * O(n²). Grow the array by doubling (spr_cap) instead of +1 for the same reason. */
  if(r->spr_has_free){
    int base=r->base_n_spr>0?r->base_n_spr:0;
    for(int i=base;i<r->n_spr;i++){
      if(r->spr[i].runtime_extra && !r->spr[i].runtime_rgba && r->spr[i].n_frames<=0){ id=i; break; }
    }
    if(id<0) r->spr_has_free=0;   /* none left; don't rescan until another is freed */
  }
  if(id<0){
    if(r->n_spr>=r->spr_cap){
      int nc=r->spr_cap>0?r->spr_cap*2:64;
      GmlSprite *ns=realloc(r->spr,(size_t)nc*sizeof(GmlSprite));
      if(!ns){ free(rgba); return -1; }
      r->spr=ns; r->spr_cap=nc;
    }
    id=r->n_spr++;
  }
  GmlSprite *s=&r->spr[id];
  memset(s,0,sizeof(*s));
  /* GameMaker gives every runtime-created sprite an addressable asset name.  Reusing a shared
   * placeholder makes sprite_get_name() return the same string for every surface sprite, so a
   * later asset_get_index(sprite_get_name(id)) aliases all of them to the first generated cell.
   * This pattern is used by runtime atlas splitters, among other things. */
  if(name) s->owned_name=strdup(name);
  else {
    char generated[64];
    snprintf(generated,sizeof generated,"__gml_runtime_sprite_%d",id);
    s->owned_name=strdup(generated);
  }
  s->name=s->owned_name?s->owned_name:"<runtime-sprite>";
  r->spr_name_gen++;
  if(!sprite_set_runtime_rgba(r,s,rgba,w,h,frames,xorig,yorig,1)){
    free(rgba); return -1;
  }
  return id;
}
int gml_sprite_append_from_rgba(GmlRender *r, uint8_t *rgba, int w, int h, int xorig, int yorig, const char *name){
  return gml_sprite_append_from_rgba_frames(r,rgba,w,h,1,xorig,yorig,name);
}
static uint8_t *sprite_copy_rgba(GmlRender *r,int sprite,
                                 int *width,int *height,int *frame_count){
  if(!r || sprite<0 || sprite>=r->n_spr) return NULL;
  GmlSprite *s=&r->spr[sprite];
  if(s->w<=0 || s->h<=0 || s->n_frames<=0) return NULL;
  int w=s->w, h=s->h, frames=s->n_frames;
  size_t pixels=(size_t)w*(size_t)h*(size_t)frames;
  if(pixels==0 || pixels>SIZE_MAX/4) return NULL;
  uint8_t *rgba=calloc(pixels,4);
  if(!rgba) return NULL;

  for(int f=0; f<frames; f++){
    uint8_t *dst=rgba+(size_t)f*(size_t)w*(size_t)h*4;
    if(s->runtime_rgba){
      const uint8_t *fr=runtime_frame_rgba(s,f);
      if(fr) memcpy(dst,fr,(size_t)w*(size_t)h*4);
      continue;
    }
    int ti=s->frame ? s->frame[f] : -1;
    if(ti<0 || ti>=r->n_tpag) continue;
    GmlTpag *t=&r->tpag[ti];
    if(t->atlas<0 || t->atlas>=r->n_atlas) continue;
    GmlAtlas *a=&r->atlas[t->atlas];
    if(!atlas_pixels(r,t->atlas) || !a->px) continue;
    for(int yy=0; yy<h; yy++) for(int xx=0; xx<w; xx++){
      int ix=xx-t->tx, iy=yy-t->ty;
      if(ix<0 || iy<0 || ix>=t->sw || iy>=t->sh) continue;
      int ax=t->sx+ix, ay=t->sy+iy;
      if(ax<0 || ay<0 || ax>=a->w || ay>=a->h) continue;
      const uint8_t *sp=a->px+((size_t)ay*(size_t)a->w+(size_t)ax)*4;
      uint8_t *dp=dst+((size_t)yy*(size_t)w+(size_t)xx)*4;
      dp[0]=sp[0]; dp[1]=sp[1]; dp[2]=sp[2]; dp[3]=sp[3];
    }
  }
  if(width) *width=w;
  if(height) *height=h;
  if(frame_count) *frame_count=frames;
  return rgba;
}

static uint8_t *sprite_copy_collision_mask(const GmlSprite *sprite){
  if(!sprite || !sprite->mask || sprite->mask_rowb<=0 ||
     sprite->mask_count<=0 || sprite->h<=0) return NULL;
  size_t bytes=(size_t)sprite->mask_rowb*(size_t)sprite->h*(size_t)sprite->mask_count;
  uint8_t *copy=malloc(bytes);
  if(copy) memcpy(copy,sprite->mask,bytes);
  return copy;
}

int gml_sprite_duplicate(GmlRender *r, int sprite){
  int w=0,h=0,frames=0;
  uint8_t *rgba=sprite_copy_rgba(r,sprite,&w,&h,&frames);
  if(!rgba) return -1;
  GmlSprite *s=&r->spr[sprite];
  uint8_t *mask=sprite_copy_collision_mask(s);
  if(s->mask && !mask){ free(rgba); return -1; }

  int id=gml_sprite_append_from_rgba_frames(r,rgba,w,h,frames,s->originx,s->originy,
                                            s->name?s->name:"<sprite-copy>");
  if(id>=0 && id<r->n_spr){
    GmlSprite *src=&r->spr[sprite];
    GmlSprite *dst=&r->spr[id];
    dst->ml=src->ml; dst->mt=src->mt; dst->mr=src->mr; dst->mb=src->mb;
    dst->runtime_mask=mask;
    dst->mask=mask;
    dst->mask_rowb=src->mask_rowb;
    dst->mask_count=src->mask_count;
    dst->collision_kind=src->collision_kind;
    dst->collision_tolerance=src->collision_tolerance;
    dst->ns_enabled=src->ns_enabled;
    dst->ns_l=src->ns_l; dst->ns_t=src->ns_t; dst->ns_r=src->ns_r; dst->ns_b=src->ns_b;
    for(int i=0;i<5;i++) dst->ns_tile[i]=src->ns_tile[i];
  } else free(mask);
  return id;
}

int gml_sprite_assign(GmlRender *r,int destination,int source){
  if(!r || destination<0 || destination>=r->n_spr ||
     source<0 || source>=r->n_spr) return 0;
  if(destination==source) return gml_sprite_exists(r,source);
  int width=0,height=0,frames=0;
  uint8_t *rgba=sprite_copy_rgba(r,source,&width,&height,&frames);
  if(!rgba) return 0;
  GmlSprite *src=&r->spr[source];
  GmlSprite *dst=&r->spr[destination];
  uint8_t *mask=sprite_copy_collision_mask(src);
  if(src->mask && !mask){ free(rgba); return 0; }
  int extra=dst->runtime_extra;
  if(!sprite_set_runtime_rgba(r,dst,rgba,width,height,frames,src->originx,src->originy,extra)){
    free(mask);free(rgba);return 0;
  }
  dst->ml=src->ml; dst->mt=src->mt; dst->mr=src->mr; dst->mb=src->mb;
  /* sprite_assign replaces the whole sprite asset.  Its independently authored collision plane
   * follows the visible frames; falling back to copied-frame alpha changes precise collisions. */
  dst->runtime_mask=mask;
  dst->mask=mask;
  dst->mask_rowb=src->mask_rowb;
  dst->mask_count=src->mask_count;
  dst->collision_kind=src->collision_kind;
  dst->collision_tolerance=src->collision_tolerance;
  dst->playback_speed=src->playback_speed;
  dst->playback_speed_type=src->playback_speed_type;
  dst->playback_speed_valid=src->playback_speed_valid;
  dst->ns_enabled=src->ns_enabled;
  dst->ns_l=src->ns_l; dst->ns_t=src->ns_t; dst->ns_r=src->ns_r; dst->ns_b=src->ns_b;
  for(int index=0;index<5;index++) dst->ns_tile[index]=src->ns_tile[index];
  return 1;
}

static int sprite_read_rgba(GmlRender *r, GmlSprite *s, int frame, int x, int y, uint8_t out[4]){
  memset(out,0,4);
  if(!r||!s||x<0||y<0||x>=s->w||y>=s->h||s->n_frames<=0) return 0;
  int f=((frame%s->n_frames)+s->n_frames)%s->n_frames;
  if(s->runtime_rgba){ const uint8_t *rgba=runtime_frame_rgba(s,f); if(!rgba)return 0;
    memcpy(out,rgba+((size_t)y*s->w+x)*4,4); return 1; }
  int ti=s->frame?s->frame[f]:-1; if(ti<0||ti>=r->n_tpag)return 0;
  GmlTpag *t=&r->tpag[ti]; int ix=x-t->tx,iy=y-t->ty;
  if(ix<0||iy<0||ix>=t->sw||iy>=t->sh||t->atlas<0||t->atlas>=r->n_atlas)return 0;
  GmlAtlas *atlas=&r->atlas[t->atlas]; if(!atlas_pixels(r,t->atlas)||!atlas->px)return 0;
  int ax=t->sx+ix,ay=t->sy+iy; if(ax<0||ay<0||ax>=atlas->w||ay>=atlas->h)return 0;
  memcpy(out,atlas->px+((size_t)ay*atlas->w+ax)*4,4); return 1;
}

int gml_sprite_set_alpha_from_sprite(GmlRender *r, int sprite, int alpha_sprite){
  if(!r||sprite<0||sprite>=r->n_spr||alpha_sprite<0||alpha_sprite>=r->n_spr)return 0;
  GmlSprite *dst=&r->spr[sprite],*src=&r->spr[alpha_sprite];
  if(dst->w<=0||dst->h<=0||dst->n_frames<=0||src->n_frames<=0)return 0;
  size_t pixels=(size_t)dst->w*dst->h*dst->n_frames; if(!pixels||pixels>SIZE_MAX/4)return 0;
  uint8_t *rgba=malloc(pixels*4); if(!rgba)return 0;
  for(int f=0;f<dst->n_frames;f++) for(int y=0;y<dst->h;y++) for(int x=0;x<dst->w;x++){
    uint8_t dc[4],sc[4]; sprite_read_rgba(r,dst,f,x,y,dc); sprite_read_rgba(r,src,f,x,y,sc);
    uint8_t *p=rgba+(((size_t)f*dst->h+y)*dst->w+x)*4;
    p[0]=dc[0];p[1]=dc[1];p[2]=dc[2];p[3]=(uint8_t)(((int)sc[0]+sc[1]+sc[2])/3);
  }
  int w=dst->w,h=dst->h,frames=dst->n_frames,xorig=dst->originx,yorig=dst->originy,extra=dst->runtime_extra;
  if(!sprite_set_runtime_rgba(r,dst,rgba,w,h,frames,xorig,yorig,extra)){
    free(rgba);return 0;
  }
  return 1;
}
void gml_sprite_set_offset(GmlRender *r, int sprite, int xorig, int yorig){
  if(!r || sprite<0 || sprite>=r->n_spr) return;
  r->spr[sprite].originx=xorig;
  r->spr[sprite].originy=yorig;
}
/* sprite_add(file, imgnum, removeback, smooth, xorig, yorig): load a loose image as a runtime
 * sprite. GM treats the image as a horizontal strip of imgnum equal frames. removeback keys
 * out the bottom-left pixel's color, GM8-style. Returns -1 when the file can't be decoded. */
static uint8_t *runtime_image_load(GmlRender *r,const char *path,
                                   int *width,int *height,int *components){
  if(!r || !r->win || !path) return NULL;
  uint8_t *encoded=NULL;
  size_t encoded_size=0;
  if(!anygm_vfs_read_all(r->win->host,path,&encoded,&encoded_size,(size_t)INT_MAX))
    return NULL;
  GmlMediaBuffer image={0};
  int decoded=gml_image_decode_rgba(encoded,encoded_size,&image,
                                    width,height,components);
  free(encoded);
  return decoded?image.data:NULL;
}
static uint8_t *runtime_image_load_frames(
    GmlRender *r,const char *path,int *width,int *height,
    int *frames,int *components){
  if(!r || !r->win || !path) return NULL;
  uint8_t *encoded=NULL;
  size_t encoded_size=0;
  if(!anygm_vfs_read_all(r->win->host,path,&encoded,&encoded_size,(size_t)INT_MAX))
    return NULL;
  GmlMediaBuffer image={0};
  int decoded=gml_image_decode_rgba_frames(
      encoded,encoded_size,&image,width,height,frames,components);
  free(encoded);
  return decoded?image.data:NULL;
}
static uint8_t *runtime_sprite_image_load(
    GmlRender *r,const char *path,int requested_frames,int removeback,
    int *frame_width,int *height,int *frame_count){
  int width=0,decoded_height=0,decoded_frames=0,components=0;
  uint8_t *decoded=runtime_image_load_frames(
      r,path,&width,&decoded_height,&decoded_frames,&components);
  (void)components;
  if(!decoded || width<=0 || decoded_height<=0 || decoded_frames<=0){
    free(decoded);
    return NULL;
  }
  int frames=1;
  int width_per_frame=width;
  uint8_t *rgba=decoded;
  if(decoded_frames>1){
    /* An animated image states its own subimage count. The requested count describes how to cut a
     * single-image strip; applying it to an animated file would collapse the animation to its first
     * frame whenever the caller requests one image. */
    frames=decoded_frames;
  } else {
    frames=requested_frames>0?requested_frames:1;
    if(frames>width || width%frames) frames=1;
    width_per_frame=width/frames;
    if(frames>1){
      if((size_t)frames*(size_t)width_per_frame>
         SIZE_MAX/(size_t)decoded_height/4u){
        free(decoded);
        return NULL;
      }
      rgba=malloc((size_t)frames*(size_t)width_per_frame*
                  (size_t)decoded_height*4u);
      if(!rgba){
        free(decoded);
        return NULL;
      }
      for(int frame=0;frame<frames;frame++)
        for(int y=0;y<decoded_height;y++)
          memcpy(rgba+((size_t)frame*decoded_height+y)*width_per_frame*4u,
                 decoded+((size_t)y*width+frame*width_per_frame)*4u,
                 (size_t)width_per_frame*4u);
      free(decoded);
    }
  }
  if(removeback){
    uint8_t *key=rgba+((size_t)(decoded_height-1)*width_per_frame)*4u;
    uint8_t red=key[0],green=key[1],blue=key[2];
    size_t pixels=(size_t)frames*(size_t)width_per_frame*
                  (size_t)decoded_height;
    for(size_t index=0;index<pixels;index++){
      uint8_t *pixel=rgba+index*4u;
      if(pixel[0]==red && pixel[1]==green && pixel[2]==blue)
        pixel[3]=0;
    }
  }
  if(frame_width) *frame_width=width_per_frame;
  if(height) *height=decoded_height;
  if(frame_count) *frame_count=frames;
  return rgba;
}

int gml_background_replace_from_rgba(GmlRender *r,int background,
                                     uint8_t *rgba,int width,int height){
  if(!r || background<0 || background>=r->n_bg || !rgba ||
     width<=0 || height<=0 || width>4096 || height>4096) return 0;
  int page_index=r->bg[background].tpag;
  int add_page=page_index<0 || page_index>=r->n_tpag;
  GmlAtlas *atlases=malloc((size_t)(r->n_atlas+1)*sizeof(*atlases));
  GmlTpag *pages=NULL;
  uint32_t *page_pointers=NULL;
  if(!atlases) return 0;
  if(r->n_atlas>0 && r->atlas)
    memcpy(atlases,r->atlas,(size_t)r->n_atlas*sizeof(*atlases));
  if(add_page){
    pages=malloc((size_t)(r->n_tpag+1)*sizeof(*pages));
    page_pointers=calloc((size_t)r->n_tpag+1,sizeof(*page_pointers));
    if(!pages || !page_pointers){
      free(atlases);
      free(pages);
      free(page_pointers);
      return 0;
    }
    if(r->n_tpag>0 && r->tpag)
      memcpy(pages,r->tpag,(size_t)r->n_tpag*sizeof(*pages));
    if(r->n_tpag>0 && r->tpag_ptr)
      memcpy(page_pointers,r->tpag_ptr,(size_t)r->n_tpag*sizeof(*page_pointers));
  }

  int atlas_index=r->n_atlas;
  memset(&atlases[atlas_index],0,sizeof(atlases[atlas_index]));
  atlases[atlas_index].px=rgba;
  atlases[atlas_index].w=width;
  atlases[atlas_index].h=height;
  atlases[atlas_index].decode_attempted=1;
  free(r->atlas);
  r->atlas=atlases;
  r->n_atlas++;

  if(add_page){
    page_index=r->n_tpag;
    memset(&pages[page_index],0,sizeof(pages[page_index]));
    free(r->tpag);
    free(r->tpag_ptr);
    r->tpag=pages;
    r->tpag_ptr=page_pointers;
    r->n_tpag++;
    r->bg[background].tpag=page_index;
  } else {
    gml_render_texture_page_cache_clear(r,&r->tpag[page_index]);
  }
  GmlTpag *page=&r->tpag[page_index];
  page->sx=page->sy=page->tx=page->ty=0;
  page->sw=page->bw=width;
  page->sh=page->bh=height;
  page->atlas=atlas_index;
  gml_render_interpolated_subrect_cache_clear(r);
  return 1;
}

int gml_background_replace_from_file(GmlRender *r,int background,const char *path,
                                     int removeback,int smooth){
  (void)smooth;
  if(!r || !path || !*path) return 0;
  int width=0,height=0,components=0;
  uint8_t *rgba=runtime_image_load(r,path,&width,&height,&components);
  (void)components;
  if(!rgba) return 0;
  gml_render_apply_removeback_rgba(rgba,width,height,removeback);
  if(gml_background_replace_from_rgba(r,background,rgba,width,height)) return 1;
  free(rgba);
  return 0;
}

int gml_sprite_add_file(GmlRender *r, const char *path, int imgnum, int removeback, int xorig, int yorig){
  if(!r || !path) return -1;
  int fw=0,H=0,frames=0;
  uint8_t *rgba=runtime_sprite_image_load(
      r,path,imgnum,removeback,&fw,&H,&frames);
  if(!rgba) return -1;
  int id=gml_sprite_append_from_rgba_frames(r,rgba,fw,H,frames,xorig,yorig,path);
  if(id>=0 && id<r->n_spr) sprite_set_runtime_source(&r->spr[id],path,imgnum,removeback);
  return id;
}
int gml_sprite_replace_from_rgba_frames(GmlRender *r, int sprite, uint8_t *rgba, int w, int h, int frames, int xorig, int yorig){
  if(sprite<0 || sprite>=r->n_spr || !rgba || w<=0 || h<=0 || frames<=0) return 0;
  return sprite_set_runtime_rgba(r,&r->spr[sprite],rgba,w,h,frames,xorig,yorig,0);
}
int gml_sprite_replace_from_rgba(GmlRender *r, int sprite, uint8_t *rgba, int w, int h, int xorig, int yorig){
  return gml_sprite_replace_from_rgba_frames(r,sprite,rgba,w,h,1,xorig,yorig);
}
void gml_sprite_delete(GmlRender *r, int sprite){
  if(!r || sprite<0 || sprite>=r->n_spr) return;
  int base=r->base_n_spr>0?r->base_n_spr:0;
  if(sprite<base) return;
  GmlSprite *s=&r->spr[sprite];
  if(!s->runtime_extra) return;
  if(!gml_render_runtime_sprite_scene_retire(r,s)) return;
  gml_render_sprite_cache_free(s);
  gml_render_sprite_state_cache_clear(s);
  free(s->runtime_rgba);
  free(s->runtime_mask);
  free(s->runtime_row_min);
  free(s->runtime_row_max);
  free(s->owned_name);
  free(s->runtime_source_path);
  free(s->frame);
  memset(s,0,sizeof(*s));
  s->runtime_extra=1;
  r->spr_has_free=1;   /* a reusable freed slot now exists */
  r->spr_name_gen++;   /* the deleted sprite's name is gone */
  while(r->n_spr>base){
    GmlSprite *last=&r->spr[r->n_spr-1];
    if(!(last->runtime_extra && !last->runtime_rgba && last->n_frames<=0)) break;
    r->n_spr--;
  }
}
static void sprite_auto_bbox(GmlRender *r, int sprite, int tolerance, int *ml, int *mt, int *mr, int *mb){
  GmlSprite *s=&r->spr[sprite];
  int l=s->w, t=s->h, rr=-1, bb=-1;
  int frames=s->n_frames>0?s->n_frames:1;
  for(int f=0; f<frames; f++) for(int y=0; y<s->h; y++) for(int x=0; x<s->w; x++){
    if(gml_sprite_alpha(r,sprite,f,x,y)<=tolerance) continue;
    if(x<l) l=x;
    if(x>rr) rr=x;
    if(y<t) t=y;
    if(y>bb) bb=y;
  }
  if(rr<l || bb<t){ *ml=0; *mt=0; *mr=-1; *mb=-1; return; }
  *ml=l; *mt=t; *mr=rr; *mb=bb;
}
static int clamp_i(int v, int lo, int hi){
  return v<lo?lo:(v>hi?hi:v);
}
int gml_sprite_collision_mask(GmlRender *r, int sprite, int sepmasks, int bboxmode,
                              int bbleft, int bbtop, int bbright, int bbbottom,
                              int kind, int tolerance){
  (void)sepmasks;
  if(!r || sprite<0 || sprite>=r->n_spr) return 0;
  GmlSprite *s=&r->spr[sprite];
  if(!s->runtime_rgba || s->w<=0 || s->h<=0 || s->n_frames<=0) return 0;
  if(tolerance<0) tolerance=0;
  if(tolerance>255) tolerance=255;
  if(kind<0 || kind>3) kind=0;
  if(bboxmode==0){
    sprite_auto_bbox(r,sprite,tolerance,&s->ml,&s->mt,&s->mr,&s->mb);
  } else if(bboxmode==2){
    s->ml=clamp_i(bbleft,0,s->w-1);
    s->mt=clamp_i(bbtop,0,s->h-1);
    s->mr=clamp_i(bbright,0,s->w-1);
    s->mb=clamp_i(bbbottom,0,s->h-1);
  } else {
    s->ml=0; s->mt=0; s->mr=s->w-1; s->mb=s->h-1;
  }
  free(s->runtime_mask); s->runtime_mask=NULL;
  s->mask=NULL; s->mask_rowb=0; s->mask_count=0;
  s->collision_kind=kind;
  s->collision_tolerance=tolerance;
  return 1;
}
void gml_render_clear_runtime_sprites(GmlRender *r){
  for(int i=0;i<r->n_spr;i++)
    if(!gml_render_runtime_sprite_scene_retire(r,&r->spr[i])) return;
  int base=r->base_n_spr>0?r->base_n_spr:0;
  if(base>r->n_spr) base=r->n_spr;
  for(int i=0;i<base;i++){
    GmlSprite *s=&r->spr[i];
    gml_render_sprite_cache_free(s);
    gml_render_sprite_state_cache_clear(s);
    free(s->runtime_rgba); free(s->runtime_mask); free(s->runtime_row_min); free(s->runtime_row_max); free(s->runtime_source_path);
    s->runtime_rgba=NULL; s->runtime_mask=NULL; s->runtime_row_min=NULL; s->runtime_row_max=NULL; s->runtime_source_path=NULL; s->runtime_owned=0; s->runtime_extra=0;
    s->runtime_opaque=0;
    s->runtime_scene_binary=0;
    s->runtime_source_imgnum=0; s->runtime_source_removeback=0;
    sprite_restore_base(s);
  }
  for(int i=base;i<r->n_spr;i++){
    gml_render_sprite_state_cache_clear(&r->spr[i]);
    free(r->spr[i].runtime_rgba);
    free(r->spr[i].runtime_mask);
    free(r->spr[i].runtime_row_min);
    free(r->spr[i].runtime_row_max);
    free(r->spr[i].owned_name);
    free(r->spr[i].runtime_source_path);
    free(r->spr[i].frame);
  }
  r->n_spr=base;
}
static uint8_t *surface_region_rgba(GmlRender *r,int surf,int x,int y,int w,int h,
                                     int removeback){
  /* Preparing an active target may enqueue a device clear. Do that before the
   * observation barrier, so the returned CPU view includes that clear. */
  if(r && surf>=0 && (gml_surface_get_target(r)==surf ||
                      (surf==0 && r->fb==r->app_surface)))
    gml_render_maybe_prepare_draw(r);
  int sw=0, sh=0; const uint32_t *src=surface_pixels_observe(r,surf,&sw,&sh);
  if(!src || w<=0 || h<=0 || (size_t)w>SIZE_MAX/4u/(size_t)h) return NULL;
  uint8_t *rgba=calloc((size_t)w*h*4,1);
  if(!rgba) return NULL;
  for(int yy=0; yy<h; yy++) for(int xx=0; xx<w; xx++){
    int64_t sx=(int64_t)x+xx, sy=(int64_t)y+yy;
    uint8_t *dp=rgba+((size_t)yy*w+xx)*4;
    if(sx>=0 && sy>=0 && sx<sw && sy<sh){
      uint32_t p=src[(size_t)sy*sw+sx];
      dp[0]=(uint8_t)((p>>16)&0xff);
      dp[1]=(uint8_t)((p>>8)&0xff);
      dp[2]=(uint8_t)(p&0xff);
      dp[3]=(uint8_t)(p>>24);
    }
  }
  gml_render_apply_removeback_rgba(rgba,w,h,removeback);
  return rgba;
}
int gml_sprite_create_from_surface(GmlRender *r, int surf, int x, int y, int w, int h,
                                   int removeback, int smooth, int xorig, int yorig){
  (void)smooth;
  uint8_t *rgba=surface_region_rgba(r,surf,x,y,w,h,removeback);
  if(!rgba) return -1;
  return gml_sprite_append_from_rgba_frames(r,rgba,w,h,1,xorig,yorig,NULL);
}
int gml_sprite_add_from_surface(GmlRender *r,int sprite,int surf,int x,int y,int w,int h,
                                 int removeback,int smooth){
  (void)smooth;
  if(!r || sprite<0 || sprite>=r->n_spr || w<=0 || h<=0 || w>4096 || h>4096)
    return 0;
  GmlSprite *s=&r->spr[sprite];
  /* Surface appends operate on created or duplicated assets, never the packaged source. */
  if(!s->runtime_extra || !s->runtime_rgba || s->w<=0 || s->h<=0 ||
     s->w>4096 || s->h>4096 || s->n_frames<=0 || s->n_frames>=4096) return 0;
  size_t frame_bytes=(size_t)s->w*(size_t)s->h*4u;
  if(frame_bytes>SIZE_MAX/(size_t)(s->n_frames+1)) return 0;
  uint8_t *region=surface_region_rgba(r,surf,x,y,w,h,removeback);
  if(!region) return 0;
  uint8_t *rgba=malloc(frame_bytes*(size_t)(s->n_frames+1));
  uint8_t *mask=sprite_copy_collision_mask(s);
  int rowb=s->mask_rowb,masks=s->mask_count;
  if(!s->mask && s->collision_kind==0){
    /* An implicit alpha mask must become independent of the enlarged image.
     * Adding a subimage does not rebuild the sprite's existing collision mask. */
    rowb=(s->w+7)/8;masks=s->n_frames;
    mask=calloc((size_t)rowb*(size_t)s->h*(size_t)masks,1);
    if(mask) for(int frame=0;frame<masks;frame++)
      for(int yy=0;yy<s->h;yy++) for(int xx=0;xx<s->w;xx++)
        if(gml_sprite_collision(r,sprite,frame,xx,yy))
          mask[((size_t)frame*(size_t)s->h+(size_t)yy)*(size_t)rowb+(size_t)xx/8u]
            |=(uint8_t)(1u<<(7-(xx%8)));
  }
  if(!rgba || (masks && !mask)){
    free(mask);free(rgba);free(region);return 0;
  }
  memcpy(rgba,s->runtime_rgba,frame_bytes*(size_t)s->n_frames);
  uint8_t *last=rgba+frame_bytes*(size_t)s->n_frames;
  /* A differently sized region is normalized to the existing logical frame.
   * This changes resource bytes; device draw filtering remains the device's job. */
  for(int yy=0;yy<s->h;yy++) for(int xx=0;xx<s->w;xx++){
    size_t sx=(size_t)xx*(size_t)w/(size_t)s->w;
    size_t sy=(size_t)yy*(size_t)h/(size_t)s->h;
    memcpy(last+((size_t)yy*(size_t)s->w+(size_t)xx)*4u,
           region+(sy*(size_t)w+sx)*4u,4u);
  }
  free(region);
  int ml=s->ml,mt=s->mt,mr=s->mr,mb=s->mb;
  int kind=s->collision_kind;
  int tolerance=s->collision_tolerance;
  if(!sprite_set_runtime_rgba(r,s,rgba,s->w,s->h,s->n_frames+1,
                              s->originx,s->originy,s->runtime_extra)){
    free(mask);free(rgba);return 0;
  }
  s->ml=ml;s->mt=mt;s->mr=mr;s->mb=mb;
  s->runtime_mask=mask;s->mask=mask;s->mask_rowb=rowb;s->mask_count=masks;
  s->collision_kind=kind;s->collision_tolerance=tolerance;
  return 1;
}
int gml_sprite_replace_from_file(GmlRender *r, int sprite, const char *path, int imgnumb,
                                 int removeback, int smooth, int xorig, int yorig){
  (void)smooth;
  if(sprite<0 || sprite>=r->n_spr || !path || !*path) return 0;
  int w=0,h=0,frames=0;
  uint8_t *rgba=runtime_sprite_image_load(
      r,path,imgnumb,removeback,&w,&h,&frames);
  if(!rgba) return 0;
  int ok=gml_sprite_replace_from_rgba_frames(r,sprite,rgba,w,h,frames,xorig,yorig);
  if(ok) sprite_set_runtime_source(&r->spr[sprite],path,imgnumb,removeback);
  if(!ok) free(rgba);
  return ok;
}

/* A TPAG item may declare a logical target larger than its stored texel rectangle. Materialize
 * only those reduced-resolution sprites as runtime RGBA frames at the logical size, preserving
 * the specialized texel blits for other items. */
void gml_render_materialize_scaled_pages(GmlRender *r){
  if(!r || !r->spr || !r->tpag) return;
  for(int i=0;i<r->n_spr;i++){
    GmlSprite *s=&r->spr[i];
    if(s->runtime_rgba || !s->frame || s->n_frames<=0 || s->w<=0 || s->h<=0) continue;
    int scaled=0;
    for(int f=0;f<s->n_frames && !scaled;f++){
      int ti=s->frame[f];
      if(ti<0 || ti>=r->n_tpag) continue;
      GmlTpag *t=&r->tpag[ti];
      /* A smaller target may represent ordinary trim or padding. Materialize only when the
       * logical target is larger than the stored texel rectangle. */
      if(t->tw>t->sw || t->th>t->sh) scaled=1;
    }
    if(!scaled) continue;
    size_t frame_px=(size_t)s->w*(size_t)s->h;
    if(frame_px==0 || frame_px>(size_t)64*1024*1024) continue;
    uint8_t *rgba=calloc((size_t)s->n_frames*frame_px,4);
    if(!rgba) continue;
    int ok=1;
    for(int f=0;f<s->n_frames && ok;f++){
      int ti=s->frame[f];
      if(ti<0 || ti>=r->n_tpag) continue;
      GmlTpag *t=&r->tpag[ti];
      if(t->atlas<0 || t->atlas>=r->n_atlas || !atlas_pixels(r,t->atlas)){ ok=0; break; }
      GmlAtlas *a=&r->atlas[t->atlas];
      int tw=t->tw>0?t->tw:t->sw, th=t->th>0?t->th:t->sh;
      if(t->sw<=0 || t->sh<=0 || tw<=0 || th<=0) continue;
      uint8_t *dst_frame=rgba+(size_t)f*frame_px*4;
      for(int y=0;y<th;y++){
        int dy=t->ty+y;
        if(dy<0 || dy>=s->h) continue;
        int sy=t->sy+(int)(((int64_t)y*t->sh)/th);
        if(sy<0 || sy>=a->h) continue;
        const uint8_t *srow=a->px+(size_t)sy*a->w*4;
        for(int x=0;x<tw;x++){
          int dx=t->tx+x;
          if(dx<0 || dx>=s->w) continue;
          int sx=t->sx+(int)(((int64_t)x*t->sw)/tw);
          if(sx<0 || sx>=a->w) continue;
          uint8_t *dp=dst_frame+((size_t)dy*s->w+dx)*4;
          const uint8_t *sp=srow+(size_t)sx*4;
          dp[0]=sp[0]; dp[1]=sp[1]; dp[2]=sp[2]; dp[3]=sp[3];
        }
      }
    }
    if(!ok){ free(rgba); continue; }
    if(!sprite_set_runtime_rgba(r,s,rgba,s->w,s->h,s->n_frames,s->originx,s->originy,0))
      free(rgba);
  }
}
