/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_package_internal.h"

#include "anygm_host.h"
#include "anygm_vfs.h"
#include "gml_image_codec.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ref_texture_find_frame(const RefTextureLayout *r, const char *name, int frame){
  if(!r || !name) return -1;
  for(int i=0;i<r->n_frames;i++){
    if(r->frames[i].frame==frame &&
       r->frames[i].sprite_name &&
       !strcmp(r->frames[i].sprite_name,name)){
      return i;
    }
  }
  return -1;
}

static int total_sprite_frames(const GmlcProject *p){
  int n=0;
  for(int i=0;i<p->n_sprites;i++) n += p->sprites[i].n_frames;
  return n;
}

static int total_texture_pages(const GmlcProject *p){
  return total_sprite_frames(p) + p->n_fonts;
}

#define GMLC_ATLAS_BASE_DIM 2048
#define GMLC_ATLAS_MAX_DIM 8192
#define GMLC_ATLAS_BORDER 2

typedef struct {
  int idx, w, h, xoff, yoff;
  uint64_t hash;
} TextureRequest;

typedef struct {
  int x, y, w, h;
} PackRect;

typedef struct {
  PackRect *rects;
  int n, cap;
} PackPage;

/* Ordering has to be total, not merely descending by size. The sort is not stable, so leaving
 * same-sized images comparing equal let each C library place them in its own order; the atlases
 * were then packed differently and the generated payload differed between platforms built from
 * one source. A payload that is not byte-identical everywhere cannot be cached, compared, or
 * carried by a save state across builds, so the request's own index settles every tie. */
static int texture_request_cmp(const void *a, const void *b){
  const TextureRequest *ra=(const TextureRequest*)a;
  const TextureRequest *rb=(const TextureRequest*)b;
  int aa=ra->w*ra->h, ab=rb->w*rb->h;
  if(aa!=ab) return ab-aa;
  if(ra->h!=rb->h) return rb->h-ra->h;
  if(ra->w!=rb->w) return rb->w-ra->w;
  return ra->idx<rb->idx?-1:ra->idx>rb->idx;
}

static unsigned char *load_project_rgba(const GmlcProject *project, const char *path,
                                        int *width, int *height, int *components){
  const GmlcMemoryFile *memory=gmlc_project_find_memory_file(project,path);
  if(memory){
    if(memory->kind==GMLC_MEMORY_RGBA){
      if(memory->width<=0 || memory->height<=0 ||
         (size_t)memory->width>SIZE_MAX/(size_t)memory->height/4u ||
         memory->size<(size_t)memory->width*(size_t)memory->height*4u) return NULL;
      unsigned char *rgba=(unsigned char*)malloc(memory->size?memory->size:1u);
      if(!rgba) return NULL;
      memcpy(rgba,memory->data,memory->size);
      *width=memory->width; *height=memory->height;
      if(components) *components=4;
      return rgba;
    }
    if(memory->kind==GMLC_MEMORY_BLOB){
      GmlMediaBuffer image={0};
      if(gml_image_decode_rgba(memory->data,memory->size,&image,
                               width,height,components))
        return image.data;
    }
    return NULL;
  }
  if(!project || !path) return NULL;
  uint8_t *encoded=NULL;
  size_t encoded_size=0;
  if(!anygm_vfs_read_all(project->host,path,&encoded,&encoded_size,(size_t)INT_MAX))
    return NULL;
  GmlMediaBuffer image={0};
  int decoded=gml_image_decode_rgba(encoded,encoded_size,&image,
                                    width,height,components);
  free(encoded);
  return decoded?image.data:NULL;
}

static int texture_alpha_bounds(const GmlcProject *project, const char *path,
                                int canvas_w, int canvas_h,
                                int *out_x, int *out_y, int *out_w, int *out_h,
                                uint64_t *out_hash){
  int w=0,h=0,comp=0;
  unsigned char *rgba=load_project_rgba(project,path,&w,&h,&comp);
  if(!rgba) return 0;
  int minx=w, miny=h, maxx=-1, maxy=-1;
  for(int y=0;y<h;y++) for(int x=0;x<w;x++){
    if(rgba[((size_t)y*(size_t)w+(size_t)x)*4u+3u]){
      if(x<minx) minx=x;
      if(y<miny) miny=y;
      if(x>maxx) maxx=x;
      if(y>maxy) maxy=y;
    }
  }
  if(maxx<0){
    *out_x=0; *out_y=0; *out_w=1; *out_h=1;
  } else {
    *out_x=minx; *out_y=miny; *out_w=maxx-minx+1; *out_h=maxy-miny+1;
  }
  if(*out_x<0) *out_x=0;
  if(*out_y<0) *out_y=0;
  if(*out_w<1) *out_w=1;
  if(*out_h<1) *out_h=1;
  if(canvas_w>0 && *out_x+*out_w>canvas_w) *out_w=canvas_w-*out_x;
  if(canvas_h>0 && *out_y+*out_h>canvas_h) *out_h=canvas_h-*out_y;
  if(*out_w<1) *out_w=1;
  if(*out_h<1) *out_h=1;
  if(out_hash){
    uint64_t hash=1469598103934665603ull;
    int hash_x=*out_x, hash_y=*out_y;
    int hash_x1=*out_x+*out_w, hash_y1=*out_y+*out_h;
    /* A classic linear-filtered sprite can sample the transparent texel immediately outside
     * its alpha crop.  Include the copied atlas fringe in duplicate detection so two frames with
     * identical visible pixels but different hidden RGB do not incorrectly share one placement. */
    if(project->classic_version){
      hash_x-=GMLC_ATLAS_BORDER; hash_y-=GMLC_ATLAS_BORDER;
      hash_x1+=GMLC_ATLAS_BORDER; hash_y1+=GMLC_ATLAS_BORDER;
      if(hash_x<0) hash_x=0;
      if(hash_y<0) hash_y=0;
      if(hash_x1>w) hash_x1=w;
      if(hash_y1>h) hash_y1=h;
    }
    for(int y=hash_y;y<hash_y1;y++){
      const unsigned char *row=rgba+((size_t)y*(size_t)w+(size_t)hash_x)*4u;
      for(int x=hash_x;x<hash_x1;x++){
        for(int c=0;c<4;c++){
          hash ^= (uint64_t)row[(size_t)(x-hash_x)*4u+(size_t)c];
          hash *= 1099511628211ull;
        }
      }
    }
    *out_hash=hash;
  }
  free(rgba);
  return 1;
}

static int pack_page_add(PackPage *p, PackRect r){
  if(r.w<=0 || r.h<=0) return 1;
  if(p->n>=p->cap){
    int nc=p->cap?p->cap*2:64;
    PackRect *nr=(PackRect*)realloc(p->rects,(size_t)nc*sizeof(*nr));
    if(!nr) return 0;
    p->rects=nr; p->cap=nc;
  }
  p->rects[p->n++]=r;
  return 1;
}

static int pack_rect_contains(PackRect a, PackRect b){
  return b.x>=a.x && b.y>=a.y && b.x+b.w<=a.x+a.w && b.y+b.h<=a.y+a.h;
}

static void pack_page_prune(PackPage *p){
  for(int i=0;i<p->n;i++){
    for(int j=i+1;j<p->n;j++){
      if(pack_rect_contains(p->rects[i],p->rects[j])){
        memmove(&p->rects[j],&p->rects[j+1],(size_t)(p->n-j-1)*sizeof(*p->rects));
        p->n--; j--;
      } else if(pack_rect_contains(p->rects[j],p->rects[i])){
        memmove(&p->rects[i],&p->rects[i+1],(size_t)(p->n-i-1)*sizeof(*p->rects));
        p->n--; i--; break;
      }
    }
  }
}

static int pack_page_split(PackPage *p, PackRect used){
  for(int i=0;i<p->n;i++){
    PackRect fr=p->rects[i];
    if(used.x>=fr.x+fr.w || used.x+used.w<=fr.x || used.y>=fr.y+fr.h || used.y+used.h<=fr.y)
      continue;
    memmove(&p->rects[i],&p->rects[i+1],(size_t)(p->n-i-1)*sizeof(*p->rects));
    p->n--; i--;
    if(used.x>fr.x && !pack_page_add(p,(PackRect){fr.x,fr.y,used.x-fr.x,fr.h})) return 0;
    if(used.x+used.w<fr.x+fr.w && !pack_page_add(p,(PackRect){used.x+used.w,fr.y,fr.x+fr.w-(used.x+used.w),fr.h})) return 0;
    if(used.y>fr.y && !pack_page_add(p,(PackRect){fr.x,fr.y,fr.w,used.y-fr.y})) return 0;
    if(used.y+used.h<fr.y+fr.h && !pack_page_add(p,(PackRect){fr.x,used.y+used.h,fr.w,fr.y+fr.h-(used.y+used.h)})) return 0;
  }
  pack_page_prune(p);
  return 1;
}

static int pack_page_place(PackPage *p, int w, int h, int *out_x, int *out_y){
  int rw=w+GMLC_ATLAS_BORDER*2, rh=h+GMLC_ATLAS_BORDER*2;
  int best=-1, best_score=INT_MAX, best_y=INT_MAX, best_x=INT_MAX;
  for(int i=0;i<p->n;i++){
    PackRect fr=p->rects[i];
    if(rw>fr.w || rh>fr.h) continue;
    int score=fr.w*fr.h-rw*rh;
    if(score<best_score || (score==best_score && (fr.y<best_y || (fr.y==best_y && fr.x<best_x)))){
      best=i; best_score=score; best_y=fr.y; best_x=fr.x;
    }
  }
  if(best<0) return 0;
  PackRect fr=p->rects[best];
  PackRect used={fr.x,fr.y,rw,rh};
  *out_x=fr.x+GMLC_ATLAS_BORDER; *out_y=fr.y+GMLC_ATLAS_BORDER;
  return pack_page_split(p,used);
}

static int build_texture_layout(Pkg *pkg, const GmlcProject *p){
  int n=total_texture_pages(p);
  pkg->atlas_dim=GMLC_ATLAS_BASE_DIM;
  pkg->n_texture_items=n;
  pkg->texture_place=(TexturePlacement*)calloc((size_t)(n?n:1),sizeof(*pkg->texture_place));
  if(!pkg->texture_place) return 0;
  if(n<=0){
    pkg->n_atlas_pages=0;
    return 1;
  }
  if(pkg->ref_tex.enabled){
    int idx=0;
    for(int i=0;i<p->n_sprites;i++){
      const GmlcSprite *sp=&p->sprites[i];
      for(int f=0;f<sp->n_frames;f++,idx++){
        int found=ref_texture_find_frame(&pkg->ref_tex,sp->name,f);
        if(found<0) return 0;
        pkg->texture_place[idx]=pkg->ref_tex.frames[found].place;
      }
    }
    if(p->n_fonts>0) return 0;
    pkg->n_atlas_pages=1;
    return 1;
  }
  TextureRequest *req=(TextureRequest*)calloc((size_t)n,sizeof(*req));
  PackPage *pages=NULL;
  if(!req) return 0;
  int idx=0;
  for(int i=0;i<p->n_sprites;i++){
    const GmlcSprite *sp=&p->sprites[i];
    for(int f=0;f<sp->n_frames;f++,idx++){
      int cw=sp->width>0?sp->width:1;
      int ch=sp->height>0?sp->height:1;
      int xoff=0, yoff=0, w=cw, h=ch;
      uint64_t hash=0;
      const char *path=(sp->frame_paths && sp->frame_paths[f]) ? sp->frame_paths[f] : NULL;
      if(!texture_alpha_bounds(p,path,cw,ch,&xoff,&yoff,&w,&h,&hash)){
        free(req);
        return 0;
      }
      int required=(w>h?w:h)+GMLC_ATLAS_BORDER*2;
      while(pkg->atlas_dim<required && pkg->atlas_dim<GMLC_ATLAS_MAX_DIM) pkg->atlas_dim*=2;
      if(required>pkg->atlas_dim){ free(req); return 0; }
      req[idx]=(TextureRequest){idx,w,h,xoff,yoff,hash};
    }
  }
  for(int i=0;i<p->n_fonts;i++,idx++){
    const GmlcFont *f=&p->fonts[i];
    int w=f->width>0?f->width:1;
    int h=f->height>0?f->height:1;
    int required=(w>h?w:h)+GMLC_ATLAS_BORDER*2;
    while(pkg->atlas_dim<required && pkg->atlas_dim<GMLC_ATLAS_MAX_DIM) pkg->atlas_dim*=2;
    if(required>pkg->atlas_dim){ free(req); return 0; }
    req[idx]=(TextureRequest){idx,w,h,0,0,0};
  }
  qsort(req,(size_t)n,sizeof(*req),texture_request_cmp);
  int n_pages=0, cap_pages=0;
  for(int ri=0;ri<n;ri++){
    TextureRequest *r=&req[ri];
    int dup=-1;
    if(r->hash){
      for(int pi=0;pi<ri;pi++){
        if(req[pi].hash==r->hash && req[pi].w==r->w && req[pi].h==r->h){
          dup=req[pi].idx;
          break;
        }
      }
    }
    if(dup>=0){
      TexturePlacement *tp=&pkg->texture_place[r->idx];
      const TexturePlacement *dp=&pkg->texture_place[dup];
      tp->sx=dp->sx; tp->sy=dp->sy;
      tp->sw=(uint16_t)r->w; tp->sh=(uint16_t)r->h;
      tp->xoff=(uint16_t)r->xoff; tp->yoff=(uint16_t)r->yoff;
      tp->atlas=dp->atlas;
      continue;
    }
    int px=0, py=0, page=-1;
    for(int pi=0;pi<n_pages;pi++){
      if(pack_page_place(&pages[pi],r->w,r->h,&px,&py)){ page=pi; break; }
    }
    if(page<0){
      if(n_pages>=cap_pages){
        int nc=cap_pages?cap_pages*2:2;
        PackPage *np=(PackPage*)realloc(pages,(size_t)nc*sizeof(*np));
        if(!np){ free(req); return 0; }
        pages=np;
        memset(&pages[cap_pages],0,(size_t)(nc-cap_pages)*sizeof(*pages));
        cap_pages=nc;
      }
      page=n_pages++;
      if(!pack_page_add(&pages[page],(PackRect){0,0,pkg->atlas_dim,pkg->atlas_dim}) ||
         !pack_page_place(&pages[page],r->w,r->h,&px,&py)){
        for(int i=0;i<n_pages;i++) free(pages[i].rects);
        free(pages); free(req);
        return 0;
      }
    }
    TexturePlacement *tp=&pkg->texture_place[r->idx];
    tp->sx=(uint16_t)px; tp->sy=(uint16_t)py;
    tp->sw=(uint16_t)r->w; tp->sh=(uint16_t)r->h;
    tp->xoff=(uint16_t)r->xoff; tp->yoff=(uint16_t)r->yoff;
    tp->atlas=(uint16_t)page;
  }
  pkg->n_atlas_pages=n_pages;
  for(int i=0;i<n_pages;i++) free(pages[i].rects);
  free(pages);
  free(req);
  return 1;
}

static int global_frame_index(const GmlcProject *p, int sprite, int frame){
  int n=0;
  for(int i=0;i<sprite;i++) n += p->sprites[i].n_frames;
  return n+frame;
}

/* Classic precise sprites before revision 800 use a mask per subimage.
 * Shaped sprites retain one mask derived from their shared bounding box. */
static int sprite_masks_are_per_subimage(const GmlcProject *project,const GmlcSprite *sprite){
  if(sprite->sep_masks) return 1;
  return project && project->classic_version && project->classic_version<800 &&
         sprite->col_kind==0;
}

static uint8_t *load_sprite_mask_alpha(const GmlcProject *project,const GmlcSprite *sprite,
                                       int mask_index,char *err,size_t errcap){
  size_t pixels=(size_t)sprite->width*(size_t)sprite->height;
  uint8_t *alpha=(uint8_t*)calloc(pixels?pixels:1u,1u);
  if(!alpha){ snprintf(err,errcap,"out of memory while reading sprite mask images"); return NULL; }
  /* Revision 800 shared precise masks combine subimage alpha planes. For an
   * earlier shaped sprite, the final subimage supplies shared automatic bounds. */
  int per_subimage=sprite_masks_are_per_subimage(project,sprite);
  int first_frame=per_subimage?mask_index:
                  (project && project->classic_version && project->classic_version<800?
                   sprite->n_frames-1:0);
  int end_frame=per_subimage?mask_index+1:sprite->n_frames;
  for(int frame=first_frame;frame<end_frame;frame++){
    int width=0,height=0,components=0;
    const char *path=(sprite->frame_paths && sprite->frame_paths[frame])?
                     sprite->frame_paths[frame]:NULL;
    unsigned char *rgba=load_project_rgba(project,path,&width,&height,&components);
    if(!rgba){
      free(alpha); snprintf(err,errcap,"%s: sprite mask image read failed",path?path:"<missing>");
      return NULL;
    }
    int mask_width=width<sprite->width?width:sprite->width;
    int mask_height=height<sprite->height?height:sprite->height;
    for(int y=0;y<mask_height;y++) for(int x=0;x<mask_width;x++){
      uint8_t value=rgba[((size_t)y*(size_t)width+(size_t)x)*4u+3u];
      if(value>alpha[(size_t)y*(size_t)sprite->width+(size_t)x])
        alpha[(size_t)y*(size_t)sprite->width+(size_t)x]=value;
    }
    free(rgba);
  }
  return alpha;
}

static void sprite_mask_bounds(const GmlcSprite *sprite,const uint8_t *alpha,int tolerance,
                               int *left,int *top,int *right,int *bottom){
  if(sprite->bbox_mode==1){
    *left=*top=0; *right=sprite->width-1; *bottom=sprite->height-1; return;
  }
  if(sprite->bbox_mode==2){
    *left=sprite->bbox_left; *top=sprite->bbox_top;
    *right=sprite->bbox_right; *bottom=sprite->bbox_bottom; return;
  }
  int found=0;
  *left=sprite->width-1; *right=0; *top=sprite->height-1; *bottom=0;
  for(int y=0;y<sprite->height;y++) for(int x=0;x<sprite->width;x++){
    if(alpha[(size_t)y*(size_t)sprite->width+(size_t)x]<=tolerance) continue;
    if(!found){ *left=*right=x; *top=*bottom=y; found=1; }
    else {
      if(x<*left) *left=x;
      if(x>*right) *right=x;
      if(y<*top) *top=y;
      if(y>*bottom) *bottom=y;
    }
  }
}

static int write_sprite_masks(Pkg *pkg, const GmlcProject *project,
                              const GmlcSprite *sp, char *err, size_t errcap){
  if(sp->n_frames<=0 || sp->width<=0 || sp->height<=0){
    wu32(&pkg->b,0);
    return 1;
  }
  int rowb=(sp->width+7)/8;
  if(rowb<=0 || sp->height<=0){
    wu32(&pkg->b,0);
    return 1;
  }
  int mask_count=sprite_masks_are_per_subimage(project,sp) ? sp->n_frames : 1;
  if(mask_count<=0) mask_count=1;
  size_t mask_bytes=(size_t)rowb*(size_t)sp->height;
  if(sp->collision_mask_data && sp->collision_mask_stride==mask_bytes &&
     sp->collision_mask_count>0){
    mask_count=sp->collision_mask_count;
    wu32(&pkg->b,(uint32_t)mask_count);
    size_t total=mask_bytes*(size_t)mask_count;
    if(!wbytes(&pkg->b,sp->collision_mask_data,total)){
      snprintf(err,errcap,"out of memory while writing sprite mask");
      return 0;
    }
    while(total%4u){ if(!wu8(&pkg->b,0)) return 0; total++; }
    return 1;
  }
  wu32(&pkg->b,(uint32_t)mask_count);
  int tol=sp->col_tolerance;
  if(tol<0) tol=0;
  if(tol>255) tol=255;
  for(int f=0;f<mask_count;f++){
    uint8_t *mask=(uint8_t*)calloc(mask_bytes?mask_bytes:1,1);
    if(!mask){
      snprintf(err,errcap,"out of memory while writing sprite mask");
      return 0;
    }
    uint8_t *alpha=load_sprite_mask_alpha(project,sp,f,err,errcap);
    if(!alpha){ free(mask); return 0; }
    int left=0,top=0,right=0,bottom=0;
    sprite_mask_bounds(sp,alpha,tol,&left,&top,&right,&bottom);
    for(int y=0;y<sp->height;y++) for(int x=0;x<sp->width;x++){
        int inside=x>=left && x<=right && y>=top && y<=bottom;
        int solid=0;
        if(inside && sp->col_kind==1) solid=1;
        else if(inside && (sp->col_kind==2 || sp->col_kind==3)){
          double hw=(right-left+1)/2.0;
          double hh=(bottom-top+1)/2.0;
          if(hw>0.0 && hh>0.0){
            double cx=left+hw-0.5, cy=top+hh-0.5;
            double dx=fabs((x-cx)/hw), dy=fabs((y-cy)/hh);
            solid=sp->col_kind==2 ? dx*dx+dy*dy<=1.0 : dx+dy<=1.0;
          }
        } else if(inside){
          solid=alpha[(size_t)y*(size_t)sp->width+(size_t)x]>tol;
        }
        if(solid) mask[(size_t)y*(size_t)rowb+(size_t)x/8u] |= (uint8_t)(1u<<(7-(x&7)));
    }
    free(alpha);
    if(!wbytes(&pkg->b,mask,mask_bytes)){
      free(mask);
      snprintf(err,errcap,"out of memory while writing sprite mask");
      return 0;
    }
    free(mask);
  }
  size_t total=(size_t)rowb*(size_t)sp->height*(size_t)mask_count;
  while(total % 4){ if(!wu8(&pkg->b,0)) return 0; total++; }
  return 1;
}

int write_sprt(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  size_t s=chunk_begin(pkg,"SPRT");
  uint32_t n=(uint32_t)gmlc_project_runtime_sprite_count(p);
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(int i=0;i<p->n_sprites;i++){
    const GmlcSprite *sp=&p->sprites[i];
    if(sp->runtime_id<0) continue;
    patch32(&pkg->b,table+(size_t)sp->runtime_id*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,sp->name);
    wstrptr(pkg,sid);
    wu32(&pkg->b,(uint32_t)sp->width);
    wu32(&pkg->b,(uint32_t)sp->height);
    wi32(&pkg->b,sp->bbox_left);
    wi32(&pkg->b,sp->bbox_right);
    wi32(&pkg->b,sp->bbox_bottom);
    wi32(&pkg->b,sp->bbox_top);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,1);
    wi32(&pkg->b,sp->bbox_mode);
    wu32(&pkg->b,(uint32_t)(sprite_masks_are_per_subimage(p,sp)?1:0));
    wi32(&pkg->b,sp->xorig);
    wi32(&pkg->b,sp->yorig);
    wi32(&pkg->b,-1);
    wu32(&pkg->b,1);
    wu32(&pkg->b,0);
    wf32(&pkg->b,15.0f);
    wu32(&pkg->b,0);
    wu32(&pkg->b,(uint32_t)sp->n_frames);
    for(int f=0;f<sp->n_frames;f++){
      uint32_t pos=(uint32_t)pkg->b.len;
      wu32(&pkg->b,0);
      add_frame_patch(pkg,pos,global_frame_index(p,i,f));
    }
    if(!write_sprite_masks(pkg,p,sp,err,errcap)) return 0;
  }
  chunk_end(pkg,s);
  return 1;
}

int write_bgnd(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"BGND");
  uint32_t n=(uint32_t)p->n_tilesets;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcTileset *ts=&p->tilesets[i];
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    size_t rec=pkg->b.len;
    int sid=intern(pkg,ts->name?ts->name:"");
    wstrptr(pkg,sid);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,1);
    uint32_t tex_pos=(uint32_t)pkg->b.len;
    wu32(&pkg->b,0);
    if(ts->sprite_id>=0 && ts->sprite_id<p->n_sprites && p->sprites[ts->sprite_id].n_frames>0){
      if(!add_frame_patch(pkg,tex_pos,global_frame_index(p,ts->sprite_id,0))) return 0;
    }
    wu32(&pkg->b,1);
    wi32(&pkg->b,ts->tile_width>0?ts->tile_width:16);
    wi32(&pkg->b,ts->tile_height>0?ts->tile_height:16);
    wi32(&pkg->b,ts->border_x);
    wi32(&pkg->b,ts->border_y);
    wi32(&pkg->b,ts->columns);
    wi32(&pkg->b,1);
    wi32(&pkg->b,ts->tile_count);
    while(pkg->b.len<rec+64) wu8(&pkg->b,0);
    for(int id=0;id<ts->tile_count;id++) wi32(&pkg->b,id);
  }
  chunk_end(pkg,s);
  return 1;
}

int write_path(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"PATH");
  uint32_t n=(uint32_t)p->n_paths;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcPath *path=&p->paths[i];
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,path->name?path->name:"");
    if(sid<0) return 0;
    wstrptr(pkg,sid);
    wi32(&pkg->b,path->kind);
    wu32(&pkg->b,(uint32_t)(path->closed?1:0));
    wi32(&pkg->b,path->precision);
    wu32(&pkg->b,(uint32_t)path->n_points);
    for(int point=0;point<path->n_points;point++){
      wf32(&pkg->b,path->points[point].x);
      wf32(&pkg->b,path->points[point].y);
      wf32(&pkg->b,path->points[point].speed);
    }
  }
  chunk_end(pkg,s);
  return 1;
}

int write_tpag(Pkg *pkg, const GmlcProject *p){
  pkg->n_frames=total_sprite_frames(p);
  pkg->n_texture_pages=total_texture_pages(p);
  if(!build_texture_layout(pkg,p)) return 0;
  pkg->frame_tpag_ptr=(uint32_t*)calloc((size_t)(pkg->n_frames?pkg->n_frames:1),sizeof(uint32_t));
  pkg->font_tpag_ptr=(uint32_t*)calloc((size_t)(p->n_fonts?p->n_fonts:1),sizeof(uint32_t));
  if(!pkg->frame_tpag_ptr || !pkg->font_tpag_ptr) return 0;
  size_t s=chunk_begin(pkg,"TPAG");
  wu32(&pkg->b,(uint32_t)pkg->n_texture_pages);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)pkg->n_texture_pages*4);
  int gf=0;
  for(int i=0;i<p->n_sprites;i++){
    const GmlcSprite *sp=&p->sprites[i];
    for(int f=0;f<sp->n_frames;f++,gf++){
      uint32_t rec=(uint32_t)pkg->b.len;
      pkg->frame_tpag_ptr[gf]=rec;
      patch32(&pkg->b,table+(size_t)gf*4,rec);
      const TexturePlacement *tp=&pkg->texture_place[gf];
      wu16(&pkg->b,tp->sx); wu16(&pkg->b,tp->sy);
      wu16(&pkg->b,tp->sw); wu16(&pkg->b,tp->sh);
      wu16(&pkg->b,tp->xoff); wu16(&pkg->b,tp->yoff);
      wu16(&pkg->b,tp->sw); wu16(&pkg->b,tp->sh);
      wu16(&pkg->b,(uint16_t)sp->width); wu16(&pkg->b,(uint16_t)sp->height);
      wu16(&pkg->b,tp->atlas);
    }
  }
  for(int i=0;i<p->n_fonts;i++){
    const GmlcFont *f=&p->fonts[i];
    uint32_t rec=(uint32_t)pkg->b.len;
    pkg->font_tpag_ptr[i]=rec;
    patch32(&pkg->b,table+(size_t)(pkg->n_frames+i)*4,rec);
    const TexturePlacement *tp=&pkg->texture_place[pkg->n_frames+i];
    wu16(&pkg->b,tp->sx); wu16(&pkg->b,tp->sy);
    wu16(&pkg->b,tp->sw); wu16(&pkg->b,tp->sh);
    wu16(&pkg->b,tp->xoff); wu16(&pkg->b,tp->yoff);
    wu16(&pkg->b,tp->sw); wu16(&pkg->b,tp->sh);
    wu16(&pkg->b,(uint16_t)f->width); wu16(&pkg->b,(uint16_t)f->height);
    wu16(&pkg->b,tp->atlas);
  }
  while(pkg->b.len % 4) wu8(&pkg->b,0);
  chunk_end(pkg,s);
  for(int i=0;i<pkg->n_frame_patches;i++){
    FramePatch *fp=&pkg->frame_patches[i];
    if(fp->frame>=0 && fp->frame<pkg->n_frames) patch32(&pkg->b,fp->pos,pkg->frame_tpag_ptr[fp->frame]);
  }
  for(int i=0;i<pkg->n_font_patches;i++){
    FontPatch *fp=&pkg->font_patches[i];
    if(fp->font>=0 && fp->font<p->n_fonts) patch32(&pkg->b,fp->pos,pkg->font_tpag_ptr[fp->font]);
  }
  return 1;
}

typedef struct {
  size_t off, size;
} RefChunk;

static uint16_t ref_rd16(const uint8_t *p){
  return (uint16_t)p[0] | ((uint16_t)p[1]<<8);
}

static uint32_t ref_rd32(const uint8_t *p){
  return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static uint32_t ref_rd32be(const uint8_t *p){
  return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | (uint32_t)p[3];
}

static int ref_has(size_t len, size_t off, size_t n){
  return off<=len && n<=len-off;
}

static int ref_find_chunk(const uint8_t *data, size_t len, const char name[4], RefChunk *out){
  if(!data || len<8 || memcmp(data,"FORM",4)) return 0;
  size_t pos=8;
  while(pos+8<=len){
    uint32_t sz=ref_rd32(data+pos+4);
    size_t body=pos+8;
    if(!ref_has(len,body,(size_t)sz)) return 0;
    if(!memcmp(data+pos,name,4)){
      out->off=body;
      out->size=(size_t)sz;
      return 1;
    }
    pos=body+(size_t)sz;
    if(pos&1) pos++;
  }
  return 0;
}

static char *ref_read_string(const uint8_t *data, size_t len, RefChunk strg, uint32_t ptr){
  size_t p=(size_t)ptr;
  if(p<4 || !ref_has(len,p-4,4)) return NULL;
  uint32_t slen=ref_rd32(data+p-4);
  if(slen>0x100000u) return NULL;
  if(p<strg.off || p>strg.off+strg.size || (size_t)slen>strg.off+strg.size-p) return NULL;
  if(!ref_has(len,p,(size_t)slen)) return NULL;
  char *s=(char*)malloc((size_t)slen+1);
  if(!s) return NULL;
  memcpy(s,data+p,(size_t)slen);
  s[slen]=0;
  return s;
}

void free_ref_texture_layout(RefTextureLayout *r){
  if(!r) return;
  for(int i=0;i<r->n_frames;i++) free(r->frames[i].sprite_name);
  free(r->frames);
  free(r->png);
  memset(r,0,sizeof(*r));
}

static int ref_add_texture_frame(RefTextureLayout *r, const char *name, int frame, TexturePlacement place){
  if(r->n_frames>=r->cap_frames){
    int nc=r->cap_frames?r->cap_frames*2:128;
    RefTextureFrame *nf=(RefTextureFrame*)realloc(r->frames,(size_t)nc*sizeof(*nf));
    if(!nf) return 0;
    r->frames=nf;
    r->cap_frames=nc;
  }
  RefTextureFrame *f=&r->frames[r->n_frames++];
  f->sprite_name=gmlc_strdup(name?name:"");
  f->frame=frame;
  f->place=place;
  return f->sprite_name!=NULL;
}

static int ref_png_len(const uint8_t *data, size_t len, size_t off, size_t *out_len){
  static const uint8_t sig[8]={137,80,78,71,13,10,26,10};
  if(!ref_has(len,off,8) || memcmp(data+off,sig,8)) return 0;
  size_t pos=off+8;
  while(ref_has(len,pos,12)){
    uint32_t chunk_len=ref_rd32be(data+pos);
    const uint8_t *type=data+pos+4;
    size_t payload=pos+8;
    if(!ref_has(len,payload,(size_t)chunk_len+4)) return 0;
    pos=payload+(size_t)chunk_len+4;
    if(!memcmp(type,"IEND",4)){
      *out_len=pos-off;
      return 1;
    }
  }
  return 0;
}

static int validate_reference_texture_layout(const RefTextureLayout *r, const GmlcProject *p, char *err, size_t errcap){
  if(p->n_fonts>0){
    snprintf(err,errcap,"reference texture layout does not include generated font pages");
    return 0;
  }
  for(int i=0;i<p->n_sprites;i++){
    const GmlcSprite *sp=&p->sprites[i];
    for(int f=0;f<sp->n_frames;f++){
      int idx=ref_texture_find_frame(r,sp->name,f);
      if(idx<0){
        snprintf(err,errcap,"reference texture layout is missing a source sprite frame");
        return 0;
      }
      if(r->frames[idx].place.atlas!=0){
        snprintf(err,errcap,"reference texture layout uses multiple texture pages");
        return 0;
      }
    }
  }
  return 1;
}

int load_reference_texture_layout(Pkg *pkg, const GmlcProject *p, const char *path, char *err, size_t errcap){
  uint8_t *data=NULL;
  size_t len=0;
  RefTextureLayout tmp;
  memset(&tmp,0,sizeof(tmp));
  if(!read_blob(NULL,path,&data,&len)){
    snprintf(err,errcap,"%s: reference package read failed",path?path:"<missing>");
    return 0;
  }
  RefChunk strg, sprt, tpag, txtr;
  if(!ref_find_chunk(data,len,"STRG",&strg) ||
     !ref_find_chunk(data,len,"SPRT",&sprt) ||
     !ref_find_chunk(data,len,"TPAG",&tpag) ||
     !ref_find_chunk(data,len,"TXTR",&txtr)){
    snprintf(err,errcap,"%s: reference package is missing texture metadata",path);
    goto fail;
  }
  if(!ref_has(len,sprt.off,4)){
    snprintf(err,errcap,"%s: invalid reference sprite table",path);
    goto fail;
  }
  uint32_t ns=ref_rd32(data+sprt.off);
  if(ns>100000u || !ref_has(len,sprt.off+4,(size_t)ns*4)){
    snprintf(err,errcap,"%s: invalid reference sprite count",path);
    goto fail;
  }
  for(uint32_t i=0;i<ns;i++){
    uint32_t rec=ref_rd32(data+sprt.off+4+(size_t)i*4);
    if(!ref_has(len,(size_t)rec,80)){
      snprintf(err,errcap,"%s: invalid reference sprite record",path);
      goto fail;
    }
    char *name=ref_read_string(data,len,strg,ref_rd32(data+rec));
    if(!name){
      snprintf(err,errcap,"%s: invalid reference sprite name",path);
      goto fail;
    }
    uint32_t frames=ref_rd32(data+rec+76);
    if(frames>100000u || !ref_has(len,(size_t)rec+80,(size_t)frames*4)){
      free(name);
      snprintf(err,errcap,"%s: invalid reference frame table",path);
      goto fail;
    }
    for(uint32_t f=0;f<frames;f++){
      uint32_t tr=ref_rd32(data+rec+80+(size_t)f*4);
      if((size_t)tr<tpag.off || !ref_has(len,(size_t)tr,22) || (size_t)tr+22>tpag.off+tpag.size){
        free(name);
        snprintf(err,errcap,"%s: invalid reference texture page record",path);
        goto fail;
      }
      TexturePlacement place;
      memset(&place,0,sizeof(place));
      place.sx=ref_rd16(data+tr+0);
      place.sy=ref_rd16(data+tr+2);
      place.sw=ref_rd16(data+tr+4);
      place.sh=ref_rd16(data+tr+6);
      place.xoff=ref_rd16(data+tr+8);
      place.yoff=ref_rd16(data+tr+10);
      place.atlas=ref_rd16(data+tr+20);
      if(!ref_add_texture_frame(&tmp,name,(int)f,place)){
        free(name);
        snprintf(err,errcap,"out of memory while reading reference texture layout");
        goto fail;
      }
    }
    free(name);
  }
  if(!ref_has(len,txtr.off,4)){
    snprintf(err,errcap,"%s: invalid reference texture table",path);
    goto fail;
  }
  uint32_t nt=ref_rd32(data+txtr.off);
  if(nt!=1u || !ref_has(len,txtr.off+4,(size_t)nt*4)){
    snprintf(err,errcap,"%s: reference texture layout uses multiple texture pages",path);
    goto fail;
  }
  uint32_t tex_rec=ref_rd32(data+txtr.off+4);
  if(!ref_has(len,(size_t)tex_rec,8) || ref_rd32(data+tex_rec)!=1u){
    snprintf(err,errcap,"%s: invalid reference texture record",path);
    goto fail;
  }
  uint32_t png_ptr=ref_rd32(data+tex_rec+4);
  size_t png_len=0;
  if(!ref_png_len(data,len,(size_t)png_ptr,&png_len)){
    snprintf(err,errcap,"%s: invalid reference texture PNG",path);
    goto fail;
  }
  tmp.png=(uint8_t*)malloc(png_len);
  if(!tmp.png){
    snprintf(err,errcap,"out of memory while reading reference texture PNG");
    goto fail;
  }
  memcpy(tmp.png,data+png_ptr,png_len);
  tmp.png_len=png_len;
  tmp.enabled=1;
  if(!validate_reference_texture_layout(&tmp,p,err,errcap)) goto fail;
  free_ref_texture_layout(&pkg->ref_tex);
  pkg->ref_tex=tmp;
  memset(&tmp,0,sizeof(tmp));
  free(data);
  return 1;

fail:
  free_ref_texture_layout(&tmp);
  free(data);
  return 0;
}

static int atlas_copy_png(const GmlcProject *project, uint8_t *atlas, int atlas_dim,
                          const TexturePlacement *tp, const char *path,
                          char *err, size_t errcap){
  int w=0,h=0,comp=0;
  unsigned char *rgba=load_project_rgba(project,path,&w,&h,&comp);
  if(!rgba){
    snprintf(err,errcap,"%s: texture image read failed",path?path:"<missing>");
    return 0;
  }
  int src_x=(int)tp->xoff, src_y=(int)tp->yoff;
  int cw=(int)tp->sw;
  int ch=(int)tp->sh;
  if(src_x<0) src_x=0;
  if(src_y<0) src_y=0;
  if(src_x>=w) src_x=0;
  if(src_y>=h) src_y=0;
  if(src_x+cw>w) cw=w-src_x;
  if(src_y+ch>h) ch=h-src_y;
  if(cw<0) cw=0;
  if(ch<0) ch=0;
  for(int y=-GMLC_ATLAS_BORDER;y<ch+GMLC_ATLAS_BORDER;y++){
    int dy=(int)tp->sy+y;
    if(dy<0 || dy>=atlas_dim) continue;
    int sy=y<0?0:(y>=ch?ch-1:y);
    int sample_y=src_y+sy;
    if(project->classic_version){
      sample_y=src_y+y;
      if(sample_y<0) sample_y=0; else if(sample_y>=h) sample_y=h-1;
    }
    for(int x=-GMLC_ATLAS_BORDER;x<cw+GMLC_ATLAS_BORDER;x++){
      int dx=(int)tp->sx+x;
      if(dx<0 || dx>=atlas_dim) continue;
      int sx=x<0?0:(x>=cw?cw-1:x);
      int sample_x=src_x+sx;
      if(project->classic_version){
        sample_x=src_x+x;
        if(sample_x<0) sample_x=0; else if(sample_x>=w) sample_x=w-1;
      }
      uint8_t *dst=atlas+((size_t)dy*(size_t)atlas_dim+(size_t)dx)*4u;
      const uint8_t *src=rgba+((size_t)sample_y*(size_t)w+(size_t)sample_x)*4u;
      memcpy(dst,src,4);
    }
  }
  free(rgba);
  return 1;
}

static int write_atlas_blob(Pkg *pkg, const GmlcProject *p, int page, size_t blob_pos, char *err, size_t errcap){
  int atlas_dim=pkg->atlas_dim>0?pkg->atlas_dim:GMLC_ATLAS_BASE_DIM;
  size_t pixels_len=(size_t)atlas_dim*(size_t)atlas_dim*4u;
  uint8_t *pixels=(uint8_t*)calloc(pixels_len?pixels_len:1,1);
  if(!pixels){
    snprintf(err,errcap,"out of memory while building texture atlas");
    return 0;
  }
  int idx=0;
  for(int i=0;i<p->n_sprites;i++){
    const GmlcSprite *sp=&p->sprites[i];
    for(int f=0;f<sp->n_frames;f++,idx++){
      const TexturePlacement *tp=&pkg->texture_place[idx];
      if(tp->atlas!=(uint16_t)page) continue;
      const char *path=(sp->frame_paths && sp->frame_paths[f]) ? sp->frame_paths[f] : NULL;
      if(!atlas_copy_png(p,pixels,atlas_dim,tp,path,err,errcap)){
        free(pixels);
        return 0;
      }
    }
  }
  for(int i=0;i<p->n_fonts;i++,idx++){
    const TexturePlacement *tp=&pkg->texture_place[idx];
    if(tp->atlas!=(uint16_t)page) continue;
    if(!atlas_copy_png(p,pixels,atlas_dim,tp,p->fonts[i].png_path,err,errcap)){
      free(pixels);
      return 0;
    }
  }
  /* The bundled encoder's high-quality search is superlinear on large, mostly transparent
   * software atlases. Level 1 preserves identical decoded pixels and keeps content builds bounded. */
  GmlMediaBuffer out={0};
  int wrote=gml_image_encode_png(pixels,atlas_dim,atlas_dim,4,atlas_dim*4,&out);
  free(pixels);
  if(!wrote || out.size==0){
    gml_media_buffer_release(&out);
    snprintf(err,errcap,"texture atlas PNG encode failed");
    return 0;
  }
  while(pkg->b.len % 0x80) wu8(&pkg->b,0);
  patch32(&pkg->b,blob_pos,(uint32_t)pkg->b.len);
  int ok=wbytes(&pkg->b,out.data,out.size);
  gml_media_buffer_release(&out);
  if(!ok){
    snprintf(err,errcap,"out of memory while writing texture atlas");
    return 0;
  }
  while(pkg->b.len % 4) wu8(&pkg->b,0);
  return 1;
}

static int write_reference_atlas_blob(Pkg *pkg, size_t blob_pos, char *err, size_t errcap){
  if(!pkg->ref_tex.png || pkg->ref_tex.png_len==0){
    snprintf(err,errcap,"reference texture PNG is empty");
    return 0;
  }
  while(pkg->b.len % 0x80) wu8(&pkg->b,0);
  patch32(&pkg->b,blob_pos,(uint32_t)pkg->b.len);
  if(!wbytes(&pkg->b,pkg->ref_tex.png,pkg->ref_tex.png_len)){
    snprintf(err,errcap,"out of memory while writing reference texture atlas");
    return 0;
  }
  while(pkg->b.len % 4) wu8(&pkg->b,0);
  return 1;
}

int write_txtr(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  int nf=pkg->ref_tex.enabled ? 1 : pkg->n_atlas_pages;
  size_t s=chunk_begin(pkg,"TXTR");
  wu32(&pkg->b,(uint32_t)nf);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)nf*4);
  size_t *blob_patch=(size_t*)calloc((size_t)(nf?nf:1),sizeof(size_t));
  if(!blob_patch) return 0;
  for(int i=0;i<nf;i++){
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    wu32(&pkg->b,1);
    blob_patch[i]=pkg->b.len;
    wu32(&pkg->b,0);
  }
  for(int i=0;i<nf;i++){
    int ok=pkg->ref_tex.enabled
      ? write_reference_atlas_blob(pkg,blob_patch[i],err,errcap)
      : write_atlas_blob(pkg,p,i,blob_patch[i],err,errcap);
    if(!ok){
      free(blob_patch);
      return 0;
    }
  }
  free(blob_patch);
  chunk_end(pkg,s);
  return 1;
}
