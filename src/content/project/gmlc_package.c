/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_package.h"
#include "gmlc_bytecode.h"
#include "gml_win.h"
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>

typedef struct {
  uint8_t *data;
  size_t len, cap;
} Buf;

typedef struct {
  char **items;
  uint32_t *char_off;
  int n, cap;
} StrTab;

typedef struct {
  uint32_t pos;
  int sid;
} StrPatch;

typedef struct {
  uint32_t pos;
  int frame;
} FramePatch;

typedef struct {
  uint32_t pos;
  int font;
} FontPatch;

typedef struct {
  char *name;
  GmlcRefKind kind;
  uint32_t instr_abs;
  uint32_t ref_abs;
  uint32_t high_bits;
  int code_index;
  int inst;
} CodeRef;

typedef struct {
  uint32_t rel_pos;
  uint32_t blob_off;
} CodeBlobPatch;

typedef struct {
  int sid;
} CodeNameRef;

typedef struct {
  uint16_t sx, sy, sw, sh;
  uint16_t xoff, yoff;
  uint16_t atlas;
} TexturePlacement;

typedef struct {
  Buf b;
  Buf code_data;
  StrTab strs;
  StrPatch *patches;
  int n_patches, cap_patches;
  FramePatch *frame_patches;
  int n_frame_patches, cap_frame_patches;
  FontPatch *font_patches;
  int n_font_patches, cap_font_patches;
  uint32_t *frame_tpag_ptr;
  uint32_t *font_tpag_ptr;
  int n_frames;
  int n_texture_pages;
  TexturePlacement *texture_place;
  int n_texture_items;
  int n_atlas_pages;
  CodeRef *code_refs;
  int n_code_refs, cap_code_refs;
  CodeBlobPatch *code_blob_patches;
  int n_code_blob_patches, cap_code_blob_patches;
  CodeNameRef *code_name_refs;
  int n_code_name_refs, cap_code_name_refs;
  uint32_t code_blob_base;
  int code_data_emitted;
  int code_blobs_finalized;
  int refs_patched;
  int compiled_code;
  int placeholder_code;
} Pkg;

static int reserve(Buf *b, size_t n){
  if(b->len+n<=b->cap) return 1;
  size_t nc=b->cap?b->cap*2:4096;
  while(b->len+n>nc) nc*=2;
  uint8_t *nd=(uint8_t*)realloc(b->data,nc);
  if(!nd) return 0;
  b->data=nd; b->cap=nc;
  return 1;
}

static int wbytes(Buf *b, const void *p, size_t n){
  if(n==0) return 1;
  if(!reserve(b,n)) return 0;
  memcpy(b->data+b->len,p,n);
  b->len+=n;
  return 1;
}

static int wu8(Buf *b, uint8_t v){ return wbytes(b,&v,1); }
static int wu16(Buf *b, uint16_t v){ uint8_t x[2]={(uint8_t)v,(uint8_t)(v>>8)}; return wbytes(b,x,2); }
static int wu32(Buf *b, uint32_t v){ uint8_t x[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)}; return wbytes(b,x,4); }
static int wu64(Buf *b, uint64_t v){ return wu32(b,(uint32_t)v) && wu32(b,(uint32_t)(v>>32)); }
static int wi32(Buf *b, int32_t v){ return wu32(b,(uint32_t)v); }
static int wf32(Buf *b, float f){ uint32_t u; memcpy(&u,&f,4); return wu32(b,u); }
static int zfill(Buf *b, size_t n){ if(!reserve(b,n)) return 0; memset(b->data+b->len,0,n); b->len+=n; return 1; }
static void patch32(Buf *b, size_t pos, uint32_t v){ b->data[pos]=(uint8_t)v; b->data[pos+1]=(uint8_t)(v>>8); b->data[pos+2]=(uint8_t)(v>>16); b->data[pos+3]=(uint8_t)(v>>24); }
static void patch64(Buf *b, size_t pos, uint64_t v){ patch32(b,pos,(uint32_t)v); patch32(b,pos+4,(uint32_t)(v>>32)); }

static int intern(Pkg *p, const char *s){
  if(!s) s="";
  for(int i=0;i<p->strs.n;i++) if(!strcmp(p->strs.items[i],s)) return i;
  if(p->strs.n>=p->strs.cap){
    int nc=p->strs.cap?p->strs.cap*2:128;
    char **ni=(char**)realloc(p->strs.items,(size_t)nc*sizeof(*ni));
    uint32_t *no=(uint32_t*)realloc(p->strs.char_off,(size_t)nc*sizeof(*no));
    if(!ni || !no){ free(ni); free(no); return -1; }
    p->strs.items=ni; p->strs.char_off=no; p->strs.cap=nc;
  }
  p->strs.items[p->strs.n]=gmlc_strdup(s);
  p->strs.char_off[p->strs.n]=0;
  return p->strs.items[p->strs.n] ? p->strs.n++ : -1;
}

static int add_str_patch(Pkg *p, uint32_t pos, int sid){
  if(p->n_patches>=p->cap_patches){
    int nc=p->cap_patches?p->cap_patches*2:128;
    StrPatch *np=(StrPatch*)realloc(p->patches,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->patches=np; p->cap_patches=nc;
  }
  p->patches[p->n_patches].pos=pos;
  p->patches[p->n_patches].sid=sid;
  p->n_patches++;
  return 1;
}

static int add_frame_patch(Pkg *p, uint32_t pos, int frame){
  if(p->n_frame_patches>=p->cap_frame_patches){
    int nc=p->cap_frame_patches?p->cap_frame_patches*2:128;
    FramePatch *np=(FramePatch*)realloc(p->frame_patches,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->frame_patches=np; p->cap_frame_patches=nc;
  }
  p->frame_patches[p->n_frame_patches].pos=pos;
  p->frame_patches[p->n_frame_patches].frame=frame;
  p->n_frame_patches++;
  return 1;
}

static int add_font_patch(Pkg *p, uint32_t pos, int font){
  if(p->n_font_patches>=p->cap_font_patches){
    int nc=p->cap_font_patches?p->cap_font_patches*2:16;
    FontPatch *np=(FontPatch*)realloc(p->font_patches,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->font_patches=np; p->cap_font_patches=nc;
  }
  p->font_patches[p->n_font_patches].pos=pos;
  p->font_patches[p->n_font_patches].font=font;
  p->n_font_patches++;
  return 1;
}

static int add_code_ref(Pkg *p, const GmlcRefSite *src, uint32_t code_start, int code_index){
  if(p->n_code_refs>=p->cap_code_refs){
    int nc=p->cap_code_refs?p->cap_code_refs*2:256;
    CodeRef *nr=(CodeRef*)realloc(p->code_refs,(size_t)nc*sizeof(*nr));
    if(!nr) return 0;
    p->code_refs=nr; p->cap_code_refs=nc;
  }
  CodeRef *r=&p->code_refs[p->n_code_refs++];
  r->name=gmlc_strdup(src->name?src->name:"");
  r->kind=src->kind;
  r->instr_abs=code_start+src->instr_off;
  r->ref_abs=code_start+src->ref_off;
  r->high_bits=src->high_bits;
  r->code_index=code_index;
  r->inst=src->inst;
  return r->name!=NULL;
}

static int add_code_blob_patch(Pkg *p, uint32_t rel_pos, uint32_t blob_off){
  if(p->n_code_blob_patches>=p->cap_code_blob_patches){
    int nc=p->cap_code_blob_patches?p->cap_code_blob_patches*2:128;
    CodeBlobPatch *np=(CodeBlobPatch*)realloc(p->code_blob_patches,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->code_blob_patches=np; p->cap_code_blob_patches=nc;
  }
  p->code_blob_patches[p->n_code_blob_patches].rel_pos=rel_pos;
  p->code_blob_patches[p->n_code_blob_patches].blob_off=blob_off;
  p->n_code_blob_patches++;
  return 1;
}

static int add_code_name_ref(Pkg *p, int sid){
  if(p->n_code_name_refs>=p->cap_code_name_refs){
    int nc=p->cap_code_name_refs?p->cap_code_name_refs*2:128;
    CodeNameRef *np=(CodeNameRef*)realloc(p->code_name_refs,(size_t)nc*sizeof(*np));
    if(!np) return 0;
    p->code_name_refs=np; p->cap_code_name_refs=nc;
  }
  p->code_name_refs[p->n_code_name_refs++].sid=sid;
  return 1;
}

static int emit_code_data_prefix(Pkg *p){
  if(p->code_data_emitted) return 1;
  if(p->b.len>UINT32_MAX) return 0;
  p->code_blob_base=(uint32_t)p->b.len;
  if(p->code_data.len && !wbytes(&p->b,p->code_data.data,p->code_data.len)) return 0;
  for(int i=0;i<p->n_code_refs;i++){
    p->code_refs[i].instr_abs+=p->code_blob_base;
    p->code_refs[i].ref_abs+=p->code_blob_base;
  }
  p->code_data_emitted=1;
  return 1;
}

static int finalize_code_blobs(Pkg *p){
  if(p->code_blobs_finalized) return 1;
  if(!p->code_data_emitted && !emit_code_data_prefix(p)) return 0;
  uint32_t blob_base=p->code_blob_base;
  for(int i=0;i<p->n_code_blob_patches;i++){
    const CodeBlobPatch *bp=&p->code_blob_patches[i];
    uint32_t blob_abs=blob_base+bp->blob_off;
    int64_t rel=(int64_t)blob_abs-(int64_t)bp->rel_pos;
    if(rel<INT32_MIN || rel>INT32_MAX) return 0;
    patch32(&p->b,bp->rel_pos,(uint32_t)(int32_t)rel);
  }
  p->code_blobs_finalized=1;
  return 1;
}

static int same_ref_name(const CodeRef *a, const CodeRef *b){
  return a->kind==b->kind && a->name && b->name && !strcmp(a->name,b->name);
}

typedef struct {
  int index;
  uint32_t addr;
} RefOrder;

static int cmp_ref_order(const void *A, const void *B){
  const RefOrder *a=(const RefOrder*)A, *b=(const RefOrder*)B;
  return a->addr<b->addr?-1:(a->addr>b->addr?1:0);
}

static int patch_ref_chains(Pkg *p){
  if(p->refs_patched) return 1;
  for(int i=0;i<p->n_code_refs;i++){
    int seen=0;
    for(int j=0;j<i;j++) if(same_ref_name(&p->code_refs[i],&p->code_refs[j])){ seen=1; break; }
    if(seen) continue;
    int count=0;
    for(int j=i;j<p->n_code_refs;j++) if(same_ref_name(&p->code_refs[i],&p->code_refs[j])) count++;
    RefOrder *ord=(RefOrder*)malloc((size_t)(count?count:1)*sizeof(*ord));
    if(!ord) return 0;
    int n=0;
    for(int j=i;j<p->n_code_refs;j++) if(same_ref_name(&p->code_refs[i],&p->code_refs[j])){
      ord[n].index=j;
      ord[n].addr=p->code_refs[j].instr_abs;
      n++;
    }
    qsort(ord,(size_t)n,sizeof(*ord),cmp_ref_order);
    for(int k=0;k<n;k++){
      CodeRef *r=&p->code_refs[ord[k].index];
      uint32_t delta=0;
      if(k+1<n){
        uint32_t next=p->code_refs[ord[k+1].index].instr_abs;
        if(next<r->instr_abs || next-r->instr_abs>0x07FFFFFFu){ free(ord); return 0; }
        delta=next-r->instr_abs;
      } else {
        int sid=intern(p,r->name?r->name:"");
        if(sid<0 || sid>0x07FFFFFF){ free(ord); return 0; }
        delta=(uint32_t)sid;
      }
      patch32(&p->b,r->ref_abs,(r->high_bits & 0xF8000000u) | delta);
    }
    free(ord);
  }
  p->refs_patched=1;
  return 1;
}

static int ref_group_count(const Pkg *p, GmlcRefKind kind){
  int n=0;
  for(int i=0;i<p->n_code_refs;i++){
    if(p->code_refs[i].kind!=kind) continue;
    int seen=0;
    for(int j=0;j<i;j++) if(p->code_refs[j].kind==kind && same_ref_name(&p->code_refs[i],&p->code_refs[j])){ seen=1; break; }
    if(!seen) n++;
  }
  return n;
}

static int ref_group_stats(const Pkg *p, int idx, uint32_t *occ, uint32_t *first_addr){
  const CodeRef *base=&p->code_refs[idx];
  uint32_t n=0, first=0xFFFFFFFFu;
  for(int i=0;i<p->n_code_refs;i++){
    if(!same_ref_name(base,&p->code_refs[i])) continue;
    n++;
    if(p->code_refs[i].instr_abs<first) first=p->code_refs[i].instr_abs;
  }
  *occ=n;
  *first_addr=first==0xFFFFFFFFu?0:first;
  return 1;
}

static int wstrptr(Pkg *p, int sid){
  uint32_t pos=(uint32_t)p->b.len;
  if(!wu32(&p->b,0)) return 0;
  return add_str_patch(p,pos,sid);
}

static size_t chunk_begin(Pkg *p, const char name[4]){
  wbytes(&p->b,name,4);
  size_t szpos=p->b.len;
  wu32(&p->b,0);
  return szpos;
}

static void chunk_end(Pkg *p, size_t szpos){
  patch32(&p->b,szpos,(uint32_t)(p->b.len-(szpos+4)));
}

static int empty_list_chunk(Pkg *p, const char name[4]){
  size_t s=chunk_begin(p,name);
  if(!wu32(&p->b,0)) return 0;
  chunk_end(p,s);
  return 1;
}

static int total_sprite_frames(const GmlcProject *p){
  int n=0;
  for(int i=0;i<p->n_sprites;i++) n += p->sprites[i].n_frames;
  return n;
}

static int total_texture_pages(const GmlcProject *p){
  return total_sprite_frames(p) + p->n_fonts;
}

#define GMLC_ATLAS_DIM 2048
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

static int texture_request_cmp(const void *a, const void *b){
  const TextureRequest *ra=(const TextureRequest*)a;
  const TextureRequest *rb=(const TextureRequest*)b;
  int aa=ra->w*ra->h, ab=rb->w*rb->h;
  if(aa!=ab) return ab-aa;
  if(ra->h!=rb->h) return rb->h-ra->h;
  return rb->w-ra->w;
}

static int texture_alpha_bounds(const char *path, int canvas_w, int canvas_h,
                                int *out_x, int *out_y, int *out_w, int *out_h,
                                uint64_t *out_hash){
  int w=0,h=0,comp=0;
  unsigned char *rgba=path?stbi_load(path,&w,&h,&comp,4):NULL;
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
    for(int y=*out_y;y<*out_y+*out_h && y<h;y++){
      const unsigned char *row=rgba+((size_t)y*(size_t)w+(size_t)*out_x)*4u;
      for(int x=0;x<*out_w && *out_x+x<w;x++){
        for(int c=0;c<4;c++){
          hash ^= (uint64_t)row[(size_t)x*4u+(size_t)c];
          hash *= 1099511628211ull;
        }
      }
    }
    *out_hash=hash;
  }
  stbi_image_free(rgba);
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
  pkg->n_texture_items=n;
  pkg->texture_place=(TexturePlacement*)calloc((size_t)(n?n:1),sizeof(*pkg->texture_place));
  if(!pkg->texture_place) return 0;
  if(n<=0){
    pkg->n_atlas_pages=0;
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
      if(!texture_alpha_bounds(path,cw,ch,&xoff,&yoff,&w,&h,&hash)){
        free(req);
        return 0;
      }
      if(w+GMLC_ATLAS_BORDER*2>GMLC_ATLAS_DIM || h+GMLC_ATLAS_BORDER*2>GMLC_ATLAS_DIM){ free(req); return 0; }
      req[idx]=(TextureRequest){idx,w,h,xoff,yoff,hash};
    }
  }
  for(int i=0;i<p->n_fonts;i++,idx++){
    const GmlcFont *f=&p->fonts[i];
    int w=f->width>0?f->width:1;
    int h=f->height>0?f->height:1;
    if(w+GMLC_ATLAS_BORDER*2>GMLC_ATLAS_DIM || h+GMLC_ATLAS_BORDER*2>GMLC_ATLAS_DIM){ free(req); return 0; }
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
      if(!pack_page_add(&pages[page],(PackRect){0,0,GMLC_ATLAS_DIM,GMLC_ATLAS_DIM}) ||
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

static int write_sprite_masks(Pkg *pkg, const GmlcSprite *sp, char *err, size_t errcap){
  if(sp->n_frames<=0 || sp->width<=0 || sp->height<=0){
    wu32(&pkg->b,0);
    return 1;
  }
  int rowb=(sp->width+7)/8;
  if(rowb<=0 || sp->height<=0){
    wu32(&pkg->b,0);
    return 1;
  }
  int mask_count=sp->sep_masks ? sp->n_frames : 1;
  if(mask_count<=0) mask_count=1;
  wu32(&pkg->b,(uint32_t)mask_count);
  int tol=sp->col_tolerance;
  if(tol<0) tol=0;
  if(tol>255) tol=255;
  for(int f=0;f<mask_count;f++){
    int w=0,h=0,comp=0;
    const char *path=(sp->frame_paths && sp->frame_paths[f]) ? sp->frame_paths[f] : NULL;
    unsigned char *rgba=path?stbi_load(path,&w,&h,&comp,4):NULL;
    if(!rgba){
      snprintf(err,errcap,"%s: sprite mask image read failed",path?path:"<missing>");
      return 0;
    }
    size_t mask_bytes=(size_t)rowb*(size_t)sp->height;
    uint8_t *mask=(uint8_t*)calloc(mask_bytes?mask_bytes:1,1);
    if(!mask){
      stbi_image_free(rgba);
      snprintf(err,errcap,"out of memory while writing sprite mask");
      return 0;
    }
    int mw=w<sp->width?w:sp->width;
    int mh=h<sp->height?h:sp->height;
    for(int y=0;y<mh;y++) for(int x=0;x<mw;x++){
      unsigned char a=rgba[((size_t)y*(size_t)w+(size_t)x)*4u+3u];
      if(a>tol) mask[(size_t)y*(size_t)rowb+(size_t)x/8u] |= (uint8_t)(1u<<(7-(x&7)));
    }
    stbi_image_free(rgba);
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

static int write_sprt(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  size_t s=chunk_begin(pkg,"SPRT");
  uint32_t n=(uint32_t)p->n_sprites;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcSprite *sp=&p->sprites[i];
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
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
    wu32(&pkg->b,(uint32_t)(sp->sep_masks?1:0));
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
      add_frame_patch(pkg,pos,global_frame_index(p,(int)i,f));
    }
    if(!write_sprite_masks(pkg,sp,err,errcap)) return 0;
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_bgnd(Pkg *pkg, const GmlcProject *p){
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
    for(int id=0;id<ts->tile_count;id++) wi32(&pkg->b,id>0?id-1:0);
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_tpag(Pkg *pkg, const GmlcProject *p){
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

static int read_blob(const char *path, uint8_t **out, size_t *out_len){
  FILE *f=fopen(path,"rb");
  if(!f) return 0;
  fseek(f,0,SEEK_END);
  long sz=ftell(f);
  rewind(f);
  if(sz<=0){ fclose(f); return 0; }
  uint8_t *buf=(uint8_t*)malloc((size_t)sz);
  if(!buf){ fclose(f); return 0; }
  if(fread(buf,1,(size_t)sz,f)!=(size_t)sz){ fclose(f); free(buf); return 0; }
  fclose(f);
  *out=buf; *out_len=(size_t)sz;
  return 1;
}

typedef struct {
  Buf b;
  int ok;
} PngOut;

static void png_out_write(void *ctx, void *data, int size){
  PngOut *out=(PngOut*)ctx;
  if(!out || !out->ok || size<=0) return;
  if(!wbytes(&out->b,data,(size_t)size)) out->ok=0;
}

static int atlas_copy_png(uint8_t *atlas, int atlas_dim, const TexturePlacement *tp, const char *path, char *err, size_t errcap){
  int w=0,h=0,comp=0;
  unsigned char *rgba=path?stbi_load(path,&w,&h,&comp,4):NULL;
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
    for(int x=-GMLC_ATLAS_BORDER;x<cw+GMLC_ATLAS_BORDER;x++){
      int dx=(int)tp->sx+x;
      if(dx<0 || dx>=atlas_dim) continue;
      int sx=x<0?0:(x>=cw?cw-1:x);
      uint8_t *dst=atlas+((size_t)dy*(size_t)atlas_dim+(size_t)dx)*4u;
      const uint8_t *src=rgba+(((size_t)src_y+(size_t)sy)*(size_t)w+(size_t)src_x+(size_t)sx)*4u;
      memcpy(dst,src,4);
    }
  }
  stbi_image_free(rgba);
  return 1;
}

static int write_atlas_blob(Pkg *pkg, const GmlcProject *p, int page, size_t blob_pos, char *err, size_t errcap){
  size_t pixels_len=(size_t)GMLC_ATLAS_DIM*(size_t)GMLC_ATLAS_DIM*4u;
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
      if(!atlas_copy_png(pixels,GMLC_ATLAS_DIM,tp,path,err,errcap)){
        free(pixels);
        return 0;
      }
    }
  }
  for(int i=0;i<p->n_fonts;i++,idx++){
    const TexturePlacement *tp=&pkg->texture_place[idx];
    if(tp->atlas!=(uint16_t)page) continue;
    if(!atlas_copy_png(pixels,GMLC_ATLAS_DIM,tp,p->fonts[i].png_path,err,errcap)){
      free(pixels);
      return 0;
    }
  }
  PngOut out;
  memset(&out,0,sizeof(out));
  out.ok=1;
  stbi_write_png_compression_level=8;
  int wrote=stbi_write_png_to_func(png_out_write,&out,GMLC_ATLAS_DIM,GMLC_ATLAS_DIM,4,pixels,GMLC_ATLAS_DIM*4);
  free(pixels);
  if(!wrote || !out.ok || out.b.len==0){
    free(out.b.data);
    snprintf(err,errcap,"texture atlas PNG encode failed");
    return 0;
  }
  while(pkg->b.len % 0x80) wu8(&pkg->b,0);
  patch32(&pkg->b,blob_pos,(uint32_t)pkg->b.len);
  int ok=wbytes(&pkg->b,out.b.data,out.b.len);
  free(out.b.data);
  if(!ok){
    snprintf(err,errcap,"out of memory while writing texture atlas");
    return 0;
  }
  while(pkg->b.len % 4) wu8(&pkg->b,0);
  return 1;
}

static int write_txtr(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  int nf=pkg->n_atlas_pages;
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
    if(!write_atlas_blob(pkg,p,i,blob_patch[i],err,errcap)){
      free(blob_patch);
      return 0;
    }
  }
  free(blob_patch);
  chunk_end(pkg,s);
  return 1;
}

static int total_object_events(const GmlcProject *p){
  int n=0;
  for(int i=0;i<p->n_objects;i++) n += p->objects[i].n_events;
  return n;
}

static int room_has_creation_code(const GmlcRoom *r){
  return r && r->creation_code_path && *r->creation_code_path;
}

static int room_code_count(const GmlcProject *p){
  int n=0;
  for(int i=0;i<p->n_rooms;i++) if(room_has_creation_code(&p->rooms[i])) n++;
  return n;
}

static int script_code_index(const GmlcProject *p, int script_index){
  return room_code_count(p) + script_index;
}

static int room_instance_creation_code_count(const GmlcProject *p){
  int n=0;
  for(int ri=0;ri<p->n_rooms;ri++){
    const GmlcRoom *r=&p->rooms[ri];
    for(int ii=0;ii<r->n_instances;ii++){
      if(r->instances[ii].creation_code_path && *r->instances[ii].creation_code_path) n++;
    }
  }
  return n;
}

static int room_instance_creation_code_base(const GmlcProject *p){
  return room_code_count(p) + p->n_scripts + total_object_events(p);
}

static int room_instance_creation_code_index(const GmlcProject *p, int room_index, int inst_index){
  if(room_index<0 || room_index>=p->n_rooms) return -1;
  const GmlcRoom *target=&p->rooms[room_index];
  if(inst_index<0 || inst_index>=target->n_instances) return -1;
  const GmlcRoomInstance *in=&target->instances[inst_index];
  if(!in->creation_code_path || !*in->creation_code_path) return -1;
  int idx=room_instance_creation_code_base(p);
  for(int ri=0;ri<room_index;ri++){
    const GmlcRoom *r=&p->rooms[ri];
    for(int ii=0;ii<r->n_instances;ii++)
      if(r->instances[ii].creation_code_path && *r->instances[ii].creation_code_path) idx++;
  }
  for(int ii=0;ii<inst_index;ii++)
    if(target->instances[ii].creation_code_path && *target->instances[ii].creation_code_path) idx++;
  return idx;
}

static void id_token(char *dst, size_t cap, const char *src){
  if(!dst || cap==0) return;
  size_t j=0;
  if(src){
    for(size_t i=0;src[i] && j+1<cap;i++) dst[j++]=(src[i]=='-')?'_':src[i];
  }
  dst[j]=0;
}

static const char *event_suffix(const GmlcObjectEvent *ev, char *buf, size_t cap){
  switch(ev->event_type){
    case 0: snprintf(buf,cap,"Create_0"); break;
    case 1: snprintf(buf,cap,"Destroy_0"); break;
    case 2: snprintf(buf,cap,"Alarm_%d",ev->event_number); break;
    case 3: snprintf(buf,cap,"Step_%d",ev->event_number); break;
    case 4:
      if(ev->collision_id && *ev->collision_id && strcmp(ev->collision_id,"00000000-0000-0000-0000-000000000000")){
        char idbuf[64];
        id_token(idbuf,sizeof(idbuf),ev->collision_id);
        snprintf(buf,cap,"Collision_%s",idbuf);
      }
      else snprintf(buf,cap,"Collision_%d",ev->collision_object_id);
      break;
    case 5: snprintf(buf,cap,"Keyboard_%d",ev->event_number); break;
    case 6: snprintf(buf,cap,"Mouse_%d",ev->event_number); break;
    case 7: snprintf(buf,cap,"Other_%d",ev->event_number); break;
    case 8: snprintf(buf,cap,"Draw_%d",ev->event_number); break;
    case 9: snprintf(buf,cap,"KeyPress_%d",ev->event_number); break;
    case 10: snprintf(buf,cap,"KeyRelease_%d",ev->event_number); break;
    default: snprintf(buf,cap,"Other_%d",ev->event_number); break;
  }
  return buf;
}

static char *object_event_code_name(const GmlcObject *obj, const GmlcObjectEvent *ev){
  char suffix[96];
  event_suffix(ev,suffix,sizeof(suffix));
  size_t n=strlen(obj->name?obj->name:"")+strlen(suffix)+16;
  char *out=(char*)malloc(n);
  if(out) snprintf(out,n,"gml_Object_%s_%s",obj->name?obj->name:"",suffix);
  return out;
}

static char *script_code_name(const GmlcScript *script){
  size_t n=strlen(script->name?script->name:"")+12;
  char *out=(char*)malloc(n);
  if(out) snprintf(out,n,"gml_Script_%s",script->name?script->name:"");
  return out;
}

static int write_sond(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"SOND");
  uint32_t n=(uint32_t)p->n_sounds;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcSound *snd=&p->sounds[i];
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    int sid_name=intern(pkg,snd->name);
    int sid_file=intern(pkg,snd->name);
    wstrptr(pkg,sid_name);
    wu32(&pkg->b,0x65);
    wu32(&pkg->b,0);
    wstrptr(pkg,sid_file);
    wu32(&pkg->b,0);
    wf32(&pkg->b,snd->volume);
    wf32(&pkg->b,snd->pitch==0.0f?1.0f:snd->pitch);
    wi32(&pkg->b,0);
    wi32(&pkg->b,(int32_t)i);
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_scpt(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"SCPT");
  uint32_t n=(uint32_t)p->n_scripts;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcScript *sc=&p->scripts[i];
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,sc->name);
    wstrptr(pkg,sid);
    wi32(&pkg->b,script_code_index(p,(int)i));
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_shdr(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"SHDR");
  uint32_t n=(uint32_t)p->n_shaders;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcShader *sh=&p->shaders[i];
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,sh->name?sh->name:"");
    int vsid=intern(pkg,sh->vertex_source?sh->vertex_source:"");
    int fsid=intern(pkg,sh->fragment_source?sh->fragment_source:"");
    if(sid<0 || vsid<0 || fsid<0) return 0;
    wstrptr(pkg,sid);
    wu32(&pkg->b,0x80000001u);
    wstrptr(pkg,vsid);
    wstrptr(pkg,fsid);
    wstrptr(pkg,vsid);
    wstrptr(pkg,fsid);
    wstrptr(pkg,vsid);
    wstrptr(pkg,fsid);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wi32(&pkg->b,2);
    for(int j=0;j<6;j++){
      wu32(&pkg->b,0);
      wu32(&pkg->b,0);
    }
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_font(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"FONT");
  uint32_t n=(uint32_t)p->n_fonts;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcFont *font=&p->fonts[i];
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,font->name?font->name:"");
    wstrptr(pkg,sid);
    wu32(&pkg->b,0);
    wi32(&pkg->b,font->em_size>0?font->em_size:12);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    uint32_t tex_pos=(uint32_t)pkg->b.len;
    wu32(&pkg->b,0);
    if(!add_font_patch(pkg,tex_pos,(int)i)) return 0;
    wf32(&pkg->b,1.0f);
    wf32(&pkg->b,1.0f);
    wu32(&pkg->b,(uint32_t)font->n_glyphs);
    size_t gtable=pkg->b.len;
    zfill(&pkg->b,(size_t)font->n_glyphs*4);
    for(int g=0;g<font->n_glyphs;g++){
      const GmlcFontGlyph *gl=&font->glyphs[g];
      patch32(&pkg->b,gtable+(size_t)g*4,(uint32_t)pkg->b.len);
      wu16(&pkg->b,(uint16_t)gl->ch);
      wu16(&pkg->b,(uint16_t)gl->x);
      wu16(&pkg->b,(uint16_t)gl->y);
      wu16(&pkg->b,(uint16_t)gl->w);
      wu16(&pkg->b,(uint16_t)gl->h);
      wu16(&pkg->b,(uint16_t)gl->shift);
      wu16(&pkg->b,(uint16_t)gl->offset);
    }
  }
  for(uint16_t i=0;i<0x80;i++) wu16(&pkg->b,i);
  for(uint16_t i=0;i<0x80;i++) wu16(&pkg->b,0x3f);
  chunk_end(pkg,s);
  return 1;
}

static int write_audo(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  size_t s=chunk_begin(pkg,"AUDO");
  uint32_t n=(uint32_t)p->n_sounds;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    const GmlcSound *snd=&p->sounds[i];
    uint8_t *blob=NULL; size_t blen=0;
    if(!snd->data_path || !read_blob(snd->data_path,&blob,&blen)){
      snprintf(err,errcap,"%s: sound blob read failed",snd->data_path?snd->data_path:"<missing>");
      return 0;
    }
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    wu32(&pkg->b,(uint32_t)blen);
    wbytes(&pkg->b,blob,blen);
    if(i+1<n) while(pkg->b.len % 4) wu8(&pkg->b,0);
    free(blob);
  }
  chunk_end(pkg,s);
  return 1;
}

static int fixed_zero_chunk(Pkg *p, const char name[4], size_t n){
  size_t s=chunk_begin(p,name);
  if(!zfill(&p->b,n)) return 0;
  chunk_end(p,s);
  return 1;
}

static int write_agrp(Pkg *p){
  int sid=intern(p,"audiogroup_default");
  if(sid<0) return 0;
  size_t s=chunk_begin(p,"AGRP");
  wu32(&p->b,1);
  size_t table=p->b.len;
  wu32(&p->b,0);
  patch32(&p->b,table,(uint32_t)p->b.len);
  wstrptr(p,sid);
  chunk_end(p,s);
  return 1;
}

static int write_optn(Pkg *p){
  size_t s=chunk_begin(p,"OPTN");
  wu32(&p->b,0x80000000u);
  wu32(&p->b,2);
  wu64(&p->b,0x0000000000480214ull);
  wi32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,1);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,0);
  wu32(&p->b,255);
  wu32(&p->b,0);
  chunk_end(p,s);
  return 1;
}

static int write_embi(Pkg *p){
  size_t s=chunk_begin(p,"EMBI");
  wu32(&p->b,1);
  wu32(&p->b,0);
  chunk_end(p,s);
  return 1;
}

/* GEN8 ends with a "random UID" block: a first-random word, four more 32-bit words the
 * GameMaker IDE fills to make an exported data.win look unique, then a frame-time float, a
 * flag byte and a 16-byte pad. This runtime writes a data.win only for its own loader, which
 * reads the header fields and the room order and never these words (nor the +92 timestamp),
 * so a fixed zero block keeps the chunk well-formed, the same size and fully deterministic. */
static int write_gen8_uid_block(Buf *b){
  for(int i=0;i<5;i++) if(!wu64(b,0)) return 0;   /* first-random word + four UID words */
  if(!wf32(b,60.0f) || !wu8(b,1) || !zfill(b,16)) return 0;
  return 1;
}
static char *identifier_from_name(const char *name){
  if(!name || !*name) return gmlc_strdup("source_project");
  size_t n=strlen(name);
  char *out=(char*)malloc(n+1);
  if(!out) return NULL;
  for(size_t i=0;i<n;i++){
    unsigned char ch=(unsigned char)name[i];
    out[i]=(isalnum(ch) || ch=='_') ? (char)ch : '_';
  }
  out[n]=0;
  if(!isalpha((unsigned char)out[0]) && out[0]!='_') out[0]='_';
  return out;
}

static int write_gen8(Pkg *pkg, const GmlcProject *p){
  const char *display_name=p->name?p->name:"source_project";
  char *identifier=identifier_from_name(display_name);
  if(!identifier) return 0;
  int sid_name=intern(pkg,display_name);
  int sid_cfg=intern(pkg,"default");
  int sid_id=intern(pkg,identifier);
  free(identifier);
  if(sid_name<0 || sid_cfg<0 || sid_id<0) return 0;
  uint32_t dw=640, dh=480;
  if(p->n_rooms>0){
    dw=(uint32_t)(p->rooms[0].port_w>0?p->rooms[0].port_w:p->rooms[0].width);
    dh=(uint32_t)(p->rooms[0].port_h>0?p->rooms[0].port_h:p->rooms[0].height);
  }
  const uint8_t bytecode_version=15;
  const uint32_t game_id=0;
  const uint32_t info_flags=0x000000B2u;
  const uint64_t timestamp=0;
  size_t s=chunk_begin(pkg,"GEN8");
  size_t base=pkg->b.len;
  zfill(&pkg->b,128);
  pkg->b.data[base+0]=1;
  pkg->b.data[base+1]=bytecode_version;
  add_str_patch(pkg,(uint32_t)(base+4),sid_name);
  add_str_patch(pkg,(uint32_t)(base+8),sid_cfg);
  patch32(&pkg->b,base+12,(uint32_t)(p->next_instance_id>100000?p->next_instance_id:100000));
  patch32(&pkg->b,base+16,10000000);
  patch32(&pkg->b,base+20,game_id);
  add_str_patch(pkg,(uint32_t)(base+40),sid_id);
  patch32(&pkg->b,base+44,2);
  patch32(&pkg->b,base+48,0);
  patch32(&pkg->b,base+52,0);
  patch32(&pkg->b,base+56,0);
  patch32(&pkg->b,base+60,(uint32_t)dw);
  patch32(&pkg->b,base+64,(uint32_t)dh);
  patch32(&pkg->b,base+68,info_flags);
  patch64(&pkg->b,base+92,timestamp);
  add_str_patch(pkg,(uint32_t)(base+100),sid_name);
  patch32(&pkg->b,base+124,6502);
  wu32(&pkg->b,(uint32_t)p->n_rooms);
  for(int i=0;i<p->n_rooms;i++) wu32(&pkg->b,(uint32_t)i);
  if(!write_gen8_uid_block(&pkg->b)) return 0;
  while((pkg->b.len-base)&3) wu8(&pkg->b,0);
  chunk_end(pkg,s);
  return 1;
}

static int object_event_code_index(const GmlcProject *p, int obj_index, int event_index){
  if(obj_index<0 || obj_index>=p->n_objects) return -1;
  const GmlcObject *obj=&p->objects[obj_index];
  if(event_index<0 || event_index>=obj->n_events) return -1;
  int idx=room_code_count(p) + p->n_scripts;
  for(int i=0;i<obj_index;i++) idx += p->objects[i].n_events;
  return idx + event_index;
}

static int write_objt_event_action(Pkg *pkg, int code_index, int empty_sid){
  wu32(&pkg->b,1);
  wu32(&pkg->b,603);
  wu32(&pkg->b,7);
  wu32(&pkg->b,0);
  wu32(&pkg->b,0);
  wu32(&pkg->b,1);
  wu32(&pkg->b,2);
  wstrptr(pkg,empty_sid);
  wi32(&pkg->b,code_index);
  wu32(&pkg->b,1);
  wi32(&pkg->b,-1);
  wu32(&pkg->b,0);
  wu32(&pkg->b,0);
  wu32(&pkg->b,0);
  return 1;
}

static int write_objt_event(Pkg *pkg, const GmlcObjectEvent *ev,
                            int code_index, int empty_sid){
  uint32_t subtype=(uint32_t)ev->event_number;
  if(ev->event_type==4 && ev->collision_object_id>=0)
    subtype=(uint32_t)ev->collision_object_id;
  wu32(&pkg->b,subtype);
  wu32(&pkg->b,1);
  size_t table=pkg->b.len;
  wu32(&pkg->b,0);
  patch32(&pkg->b,table,(uint32_t)pkg->b.len);
  return write_objt_event_action(pkg,code_index,empty_sid);
}

static int write_objt_events(Pkg *pkg, const GmlcProject *p,
                             int obj_index, int empty_sid){
  const int event_type_count=13;
  const GmlcObject *obj=&p->objects[obj_index];
  wu32(&pkg->b,(uint32_t)event_type_count);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)event_type_count*4);
  for(int t=0;t<event_type_count;t++){
    patch32(&pkg->b,table+(size_t)t*4,(uint32_t)pkg->b.len);
    int count=0;
    for(int ei=0;ei<obj->n_events;ei++) if(obj->events[ei].event_type==t) count++;
    wu32(&pkg->b,(uint32_t)count);
    size_t subtable=pkg->b.len;
    zfill(&pkg->b,(size_t)count*4);
    int wi=0;
    for(int ei=0;ei<obj->n_events;ei++){
      if(obj->events[ei].event_type!=t) continue;
      patch32(&pkg->b,subtable+(size_t)wi*4,(uint32_t)pkg->b.len);
      if(!write_objt_event(pkg,&obj->events[ei],
                           object_event_code_index(p,obj_index,ei),
                           empty_sid)) return 0;
      wi++;
    }
  }
  return 1;
}

static int write_objt(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"OBJT");
  uint32_t n=(uint32_t)p->n_objects;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  int empty_sid=intern(pkg,"");
  if(empty_sid<0) return 0;
  for(uint32_t i=0;i<n;i++){
    patch32(&pkg->b,table+i*4,(uint32_t)pkg->b.len);
    const GmlcObject *o=&p->objects[i];
    int sid=intern(pkg,o->name);
    if(sid<0) return 0;
    wstrptr(pkg,sid);
    wi32(&pkg->b,o->sprite_id);
    wu32(&pkg->b,(uint32_t)(o->visible?1:0));
    wu32(&pkg->b,(uint32_t)(o->solid?1:0));
    wi32(&pkg->b,0);
    wu32(&pkg->b,(uint32_t)(o->persistent?1:0));
    wi32(&pkg->b,o->parent_id>=0?o->parent_id:-100);
    wi32(&pkg->b,o->mask_id);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,1);
    wf32(&pkg->b,0.5f);
    wf32(&pkg->b,0.1f);
    wu32(&pkg->b,0);
    wf32(&pkg->b,0.1f);
    wf32(&pkg->b,0.1f);
    wi32(&pkg->b,0);
    wf32(&pkg->b,0.2f);
    wu32(&pkg->b,1);
    wu32(&pkg->b,0);
    if(!write_objt_events(pkg,p,(int)i,empty_sid)) return 0;
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_room_views(Pkg *pkg, const GmlcRoom *r, uint32_t *out_ptr){
  *out_ptr=(uint32_t)pkg->b.len;
  wu32(&pkg->b,8);
  size_t table=pkg->b.len;
  zfill(&pkg->b,8*4);
  for(int i=0;i<8;i++){
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    int visible=(i==0 && r->view_enabled)?1:0;
    wu32(&pkg->b,(uint32_t)visible);
    wi32(&pkg->b,0); wi32(&pkg->b,0);
    wi32(&pkg->b,r->view_w>0?r->view_w:r->width);
    wi32(&pkg->b,r->view_h>0?r->view_h:r->height);
    wi32(&pkg->b,0); wi32(&pkg->b,0);
    wi32(&pkg->b,r->port_w>0?r->port_w:r->width);
    wi32(&pkg->b,r->port_h>0?r->port_h:r->height);
    wi32(&pkg->b,32); wi32(&pkg->b,32);
    wi32(&pkg->b,-1); wi32(&pkg->b,-1);
    wi32(&pkg->b,-1);
  }
  return 1;
}

static int write_room_instances(Pkg *pkg, const GmlcProject *p, int room_index, const GmlcRoom *r, uint32_t *out_ptr){
  *out_ptr=(uint32_t)pkg->b.len;
  wu32(&pkg->b,(uint32_t)r->n_instances);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)r->n_instances*4);
  for(int i=0;i<r->n_instances;i++){
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    const GmlcRoomInstance *in=&r->instances[i];
    wi32(&pkg->b,in->x);
    wi32(&pkg->b,in->y);
    wi32(&pkg->b,in->object_id);
    wu32(&pkg->b,(uint32_t)in->instance_id);
    wi32(&pkg->b,room_instance_creation_code_index(p,room_index,i));
    wf32(&pkg->b,in->sx==0.0f?1.0f:in->sx);
    wf32(&pkg->b,in->sy==0.0f?1.0f:in->sy);
    wu32(&pkg->b,in->color?in->color:0xFFFFFFFFu);
    wf32(&pkg->b,in->rotation);
  }
  return 1;
}

static int write_room_empty_list(Pkg *pkg, uint32_t *out_ptr){
  *out_ptr=(uint32_t)pkg->b.len;
  return wu32(&pkg->b,0);
}

static int count_room_tile_cells(const GmlcProject *p, const GmlcRoom *r){
  int n=0;
  for(int i=0;i<r->n_layers;i++){
    const GmlcRoomLayer *ly=&r->layers[i];
    if(ly->type!=4 || !ly->tile_data || ly->tile_cols<=0 || ly->tile_rows<=0) continue;
    if(ly->tile_tileset_id<0 || ly->tile_tileset_id>=p->n_tilesets) continue;
    int cells=ly->tile_cols*ly->tile_rows;
    for(int c=0;c<cells;c++) if((ly->tile_data[c]&0x7FFFFu)!=0) n++;
  }
  return n;
}

static int write_room_tiles(Pkg *pkg, const GmlcProject *p, const GmlcRoom *r, uint32_t *out_ptr){
  int n=count_room_tile_cells(p,r);
  *out_ptr=(uint32_t)pkg->b.len;
  wu32(&pkg->b,(uint32_t)n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  int ti=0;
  for(int li=0;li<r->n_layers;li++){
    const GmlcRoomLayer *ly=&r->layers[li];
    if(ly->type!=4 || !ly->tile_data || ly->tile_cols<=0 || ly->tile_rows<=0) continue;
    if(ly->tile_tileset_id<0 || ly->tile_tileset_id>=p->n_tilesets) continue;
    const GmlcTileset *ts=&p->tilesets[ly->tile_tileset_id];
    int tw=ts->tile_width>0?ts->tile_width:16;
    int th=ts->tile_height>0?ts->tile_height:16;
    int cols=ts->columns>0?ts->columns:1;
    int pitch_x=tw + 2*ts->border_x;
    int pitch_y=th + 2*ts->border_y;
    if(pitch_x<=0) pitch_x=tw;
    if(pitch_y<=0) pitch_y=th;
    for(int y=0;y<ly->tile_rows;y++) for(int x=0;x<ly->tile_cols;x++){
      uint32_t datum=ly->tile_data[(size_t)y*(size_t)ly->tile_cols+(size_t)x];
      int idx=(int)(datum&0x7FFFFu);
      if(idx<=0) continue;
      int src_idx=idx-1;
      patch32(&pkg->b,table+(size_t)ti*4,(uint32_t)pkg->b.len);
      ti++;
      wi32(&pkg->b,(int)ly->x + x*tw);
      wi32(&pkg->b,(int)ly->y + y*th);
      wi32(&pkg->b,ly->tile_tileset_id);
      wi32(&pkg->b,(src_idx%cols)*pitch_x + ts->border_x);
      wi32(&pkg->b,(src_idx/cols)*pitch_y + ts->border_y);
      wi32(&pkg->b,tw);
      wi32(&pkg->b,th);
      wi32(&pkg->b,ly->depth);
    }
  }
  return 1;
}

static int write_room_layer_list(Pkg *pkg, const GmlcRoom *r, uint32_t *out_ptr){
  *out_ptr=(uint32_t)pkg->b.len;
  wu32(&pkg->b,(uint32_t)r->n_layers);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)r->n_layers*4);
  for(int i=0;i<r->n_layers;i++){
    const GmlcRoomLayer *ly=&r->layers[i];
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    int sid=intern(pkg,ly->name?ly->name:"");
    wstrptr(pkg,sid);
    wi32(&pkg->b,ly->layer_id);
    wi32(&pkg->b,ly->type);
    wi32(&pkg->b,ly->depth);
    wf32(&pkg->b,ly->x);
    wf32(&pkg->b,ly->y);
    wf32(&pkg->b,ly->hspeed);
    wf32(&pkg->b,ly->vspeed);
    wu32(&pkg->b,(uint32_t)(ly->visible?1:0));
    if(ly->type==1){
      wu32(&pkg->b,(uint32_t)(ly->visible?1:0));
      wu32(&pkg->b,0);
      wi32(&pkg->b,ly->bg_sprite_id);
      wu32(&pkg->b,(uint32_t)(ly->bg_htiled?1:0));
      wu32(&pkg->b,(uint32_t)(ly->bg_vtiled?1:0));
      wu32(&pkg->b,(uint32_t)(ly->bg_stretch?1:0));
      wu32(&pkg->b,ly->bg_color);
      wf32(&pkg->b,ly->bg_frame);
      wf32(&pkg->b,ly->bg_speed);
      wu32(&pkg->b,0);
    } else if(ly->type==2){
      wu32(&pkg->b,(uint32_t)ly->n_instance_ids);
      for(int k=0;k<ly->n_instance_ids;k++) wu32(&pkg->b,ly->instance_ids[k]);
    } else if(ly->type==3){
      size_t tiles_pos=pkg->b.len; wu32(&pkg->b,0);
      size_t sprites_pos=pkg->b.len; wu32(&pkg->b,0);
      patch32(&pkg->b,tiles_pos,(uint32_t)pkg->b.len);
      wu32(&pkg->b,0);
      patch32(&pkg->b,sprites_pos,(uint32_t)pkg->b.len);
      wu32(&pkg->b,(uint32_t)ly->n_assets);
      size_t stable=pkg->b.len;
      zfill(&pkg->b,(size_t)ly->n_assets*4);
      for(int a=0;a<ly->n_assets;a++){
        const GmlcRoomAsset *ra=&ly->assets[a];
        patch32(&pkg->b,stable+(size_t)a*4,(uint32_t)pkg->b.len);
        int asid=intern(pkg,ra->name?ra->name:"");
        wstrptr(pkg,asid);
        wi32(&pkg->b,ra->sprite_id);
        wi32(&pkg->b,ra->x);
        wi32(&pkg->b,ra->y);
        wf32(&pkg->b,ra->sx==0.0f?1.0f:ra->sx);
        wf32(&pkg->b,ra->sy==0.0f?1.0f:ra->sy);
        wu32(&pkg->b,ra->color?ra->color:0xFFFFFFFFu);
        wf32(&pkg->b,ra->speed);
        wu32(&pkg->b,0);
        wf32(&pkg->b,ra->frame);
        wf32(&pkg->b,ra->rotation);
      }
    } else if(ly->type==4){
      wi32(&pkg->b,ly->tile_tileset_id);
      wi32(&pkg->b,ly->tile_cols);
      wi32(&pkg->b,ly->tile_rows);
      int cells=ly->tile_cols>0 && ly->tile_rows>0 ? ly->tile_cols*ly->tile_rows : 0;
      for(int c=0;c<cells;c++) wu32(&pkg->b,ly->tile_data?ly->tile_data[c]:0);
    }
  }
  return 1;
}

static int write_room(Pkg *pkg, const GmlcProject *p, const GmlcRoom *r, int room_index, uint32_t *record_ptr){
  *record_ptr=(uint32_t)pkg->b.len;
  int sid=intern(pkg,r->name);
  wstrptr(pkg,sid);
  wu32(&pkg->b,0);
  wu32(&pkg->b,(uint32_t)r->width);
  wu32(&pkg->b,(uint32_t)r->height);
  wu32(&pkg->b,(uint32_t)(r->speed>0?r->speed:60));
  wi32(&pkg->b,0);
  wu32(&pkg->b,0xFF000000u);
  wu32(&pkg->b,1);
  wi32(&pkg->b,0);
  wi32(&pkg->b,0);
  size_t bg_pos=pkg->b.len; wu32(&pkg->b,0);
  size_t view_pos=pkg->b.len; wu32(&pkg->b,0);
  size_t obj_pos=pkg->b.len; wu32(&pkg->b,0);
  size_t tile_pos=pkg->b.len; wu32(&pkg->b,0);
  wu32(&pkg->b,1);
  zfill(&pkg->b,24);
  wf32(&pkg->b,0.1f);
  size_t layer_pos=pkg->b.len; wu32(&pkg->b,0);
  uint32_t bg=0, view=0, obj=0, tile=0, layers=0;
  write_room_empty_list(pkg,&bg);
  write_room_views(pkg,r,&view);
  write_room_instances(pkg,p,room_index,r,&obj);
  write_room_tiles(pkg,p,r,&tile);
  write_room_layer_list(pkg,r,&layers);
  patch32(&pkg->b,bg_pos,bg);
  patch32(&pkg->b,view_pos,view);
  patch32(&pkg->b,obj_pos,obj);
  patch32(&pkg->b,tile_pos,tile);
  patch32(&pkg->b,layer_pos,layers);
  return 1;
}

static int write_room_chunk(Pkg *pkg, const GmlcProject *p){
  size_t s=chunk_begin(pkg,"ROOM");
  uint32_t n=(uint32_t)p->n_rooms;
  wu32(&pkg->b,n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)n*4);
  for(uint32_t i=0;i<n;i++){
    uint32_t ptr=0;
    write_room(pkg,p,&p->rooms[i],(int)i,&ptr);
    patch32(&pkg->b,table+i*4,ptr);
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_strg(Pkg *pkg){
  size_t s=chunk_begin(pkg,"STRG");
  wu32(&pkg->b,(uint32_t)pkg->strs.n);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)pkg->strs.n*4);
  for(int i=0;i<pkg->strs.n;i++){
    uint32_t len=(uint32_t)strlen(pkg->strs.items[i]);
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    wu32(&pkg->b,len);
    pkg->strs.char_off[i]=(uint32_t)pkg->b.len;
    wbytes(&pkg->b,pkg->strs.items[i],len);
    wu8(&pkg->b,0);
  }
  while(pkg->b.len % 0x80) wu8(&pkg->b,0);
  chunk_end(pkg,s);
  for(int i=0;i<pkg->n_patches;i++){
    StrPatch *sp=&pkg->patches[i];
    if(sp->sid>=0 && sp->sid<pkg->strs.n) patch32(&pkg->b,sp->pos,pkg->strs.char_off[sp->sid]);
  }
  return 1;
}

typedef struct {
  int sid;
  uint32_t size;
  uint32_t blob_off;
} CodeEntryPlan;

static int prepare_code_blob(Pkg *pkg, int sid, int code_index, GmlcCodeBlob *blob, CodeEntryPlan *entry, char *err, size_t errcap){
  for(int i=0;i<blob->n_strings;i++){
    GmlcStringSite *s=&blob->strings[i];
    if(s->payload_off+4>blob->size){
      snprintf(err,errcap,"string literal patch outside code blob");
      return 0;
    }
    int sid_lit=intern(pkg,s->value?s->value:"");
    if(sid_lit<0) return 0;
    blob->data[s->payload_off]=(uint8_t)sid_lit;
    blob->data[s->payload_off+1]=(uint8_t)(sid_lit>>8);
    blob->data[s->payload_off+2]=(uint8_t)(sid_lit>>16);
    blob->data[s->payload_off+3]=(uint8_t)(sid_lit>>24);
  }
  if(!add_code_name_ref(pkg,sid)) return 0;
  uint32_t code_start=(uint32_t)pkg->code_data.len;
  entry->sid=sid;
  entry->size=(uint32_t)blob->size;
  entry->blob_off=code_start;
  if(!wbytes(&pkg->code_data,blob->data,blob->size)) return 0;
  for(int i=0;i<blob->n_refs;i++) if(!add_code_ref(pkg,&blob->refs[i],code_start,code_index)) return 0;
  return 1;
}

static int write_code_entry_header(Pkg *pkg, const CodeEntryPlan *entry){
  wstrptr(pkg,entry->sid);
  wu32(&pkg->b,entry->size);
  wu32(&pkg->b,0);
  uint32_t rel_pos=(uint32_t)pkg->b.len;
  wi32(&pkg->b,0);
  wu32(&pkg->b,0);
  return add_code_blob_patch(pkg,rel_pos,entry->blob_off);
}

static int compile_code_blob(Pkg *pkg, const GmlcProject *p, const GmlcFunctionRegistry *funcs, int script_index, const char *path, GmlcCodeBlob *blob){
  memset(blob,0,sizeof(*blob));
  if(path && *path){
    char berr[512]={0};
    if(gmlc_bytecode_compile_source_ex(p,funcs,script_index,path,blob,berr,sizeof(berr))){
      if(blob->is_placeholder){
        pkg->placeholder_code++;
        if(blob->diagnostic)
          fprintf(stderr,"source_to_win: code placeholder: %s: %s\n",path,blob->diagnostic);
      } else {
        pkg->compiled_code++;
      }
      return 1;
    }
    if(!gmlc_bytecode_emit_empty(blob)) return 0;
    blob->diagnostic=gmlc_strdup(berr[0]?berr:"source read failed");
    pkg->placeholder_code++;
    fprintf(stderr,"source_to_win: code placeholder: %s: %s\n",path,blob->diagnostic?blob->diagnostic:"source read failed");
    return 1;
  }
  if(!gmlc_bytecode_emit_empty(blob)) return 0;
  pkg->placeholder_code++;
  return 1;
}

static int write_compiled_code_entry(Pkg *pkg, const GmlcProject *p, const GmlcFunctionRegistry *funcs, int script_index, CodeEntryPlan *entries, int *ci, int sid, const char *path, char *err, size_t errcap){
  GmlcCodeBlob blob;
  if(!compile_code_blob(pkg,p,funcs,script_index,path,&blob)) return 0;
  int ok=prepare_code_blob(pkg,sid,*ci,&blob,&entries[*ci],err,errcap);
  if(ok) (*ci)++;
  gmlc_bytecode_free(&blob);
  return ok;
}

static char *function_code_name(const GmlcFunctionDef *def){
  if(def->name && *def->name){
    size_t n=strlen(def->name)+12;
    char *out=(char*)malloc(n);
    if(out) snprintf(out,n,"gml_Script_%s",def->name);
    return out;
  }
  char tmp[64];
  snprintf(tmp,sizeof(tmp),"gml_Function_%d",def->code_index);
  return gmlc_strdup(tmp);
}

static int write_function_code_entry(Pkg *pkg, const GmlcProject *p, const GmlcFunctionRegistry *funcs, CodeEntryPlan *entries, int *ci, const GmlcFunctionDef *def, char *err, size_t errcap){
  if(*ci!=def->code_index){
    snprintf(err,errcap,"function code index mismatch");
    return 0;
  }
  char *name=function_code_name(def);
  if(!name) return 0;
  int sid=intern(pkg,name);
  free(name);
  GmlcCodeBlob blob;
  char berr[512]={0};
  if(!gmlc_bytecode_compile_function_body(p,funcs,def,&blob,berr,sizeof(berr))){
    if(!gmlc_bytecode_emit_empty(&blob)) return 0;
    blob.diagnostic=gmlc_strdup(berr[0]?berr:"function source read failed");
    pkg->placeholder_code++;
    fprintf(stderr,"source_to_win: code placeholder: function %d: %s\n",def->code_index,blob.diagnostic?blob.diagnostic:"function source read failed");
  } else if(blob.is_placeholder){
    pkg->placeholder_code++;
    if(blob.diagnostic)
      fprintf(stderr,"source_to_win: code placeholder: function %d: %s\n",def->code_index,blob.diagnostic);
  } else {
    pkg->compiled_code++;
  }
  int ok=prepare_code_blob(pkg,sid,*ci,&blob,&entries[*ci],err,errcap);
  if(ok) (*ci)++;
  gmlc_bytecode_free(&blob);
  return ok;
}

static int write_code(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap){
  size_t s=chunk_begin(pkg,"CODE");
  int base_count=room_code_count(p) + p->n_scripts + total_object_events(p) + room_instance_creation_code_count(p);
  GmlcFunctionRegistry funcs;
  if(!gmlc_bytecode_collect_functions(p,base_count,&funcs,err,errcap)) return 0;
  int count=base_count + gmlc_function_registry_extra_count(&funcs);
  wu32(&pkg->b,(uint32_t)count);
  size_t table=pkg->b.len;
  zfill(&pkg->b,(size_t)count*4);
  CodeEntryPlan *entries=(CodeEntryPlan*)calloc((size_t)(count?count:1),sizeof(*entries));
  if(!entries){
    gmlc_function_registry_free(&funcs);
    snprintf(err,errcap,"out of memory while writing code entries");
    return 0;
  }
  int ci=0;
  int ok=0;
  for(int i=0;i<p->n_rooms;i++){
    if(!room_has_creation_code(&p->rooms[i])) continue;
    char name[64];
    snprintf(name,sizeof(name),"gml_RoomCC_%d",i);
    int sid=intern(pkg,name);
    if(!write_compiled_code_entry(pkg,p,&funcs,-1,entries,&ci,sid,p->rooms[i].creation_code_path,err,errcap)) goto done;
  }
  for(int i=0;i<p->n_scripts;i++){
    char *name=script_code_name(&p->scripts[i]);
    int nsid=intern(pkg,name?name:"");
    free(name);
    if(!write_compiled_code_entry(pkg,p,&funcs,i,entries,&ci,nsid,p->scripts[i].source_path,err,errcap)) goto done;
  }
  for(int oi=0;oi<p->n_objects;oi++){
    const GmlcObject *obj=&p->objects[oi];
    for(int ei=0;ei<obj->n_events;ei++){
      char *name=object_event_code_name(obj,&obj->events[ei]);
      int nsid=intern(pkg,name?name:"");
      free(name);
      if(!write_compiled_code_entry(pkg,p,&funcs,-1,entries,&ci,nsid,obj->events[ei].source_path,err,errcap)) goto done;
    }
  }
  for(int ri=0;ri<p->n_rooms;ri++){
    const GmlcRoom *r=&p->rooms[ri];
    for(int ii=0;ii<r->n_instances;ii++){
      const GmlcRoomInstance *in=&r->instances[ii];
      if(!in->creation_code_path || !*in->creation_code_path) continue;
      char name[96];
      snprintf(name,sizeof(name),"gml_RoomInstanceCC_%d_%d",ri,ii);
      int sid=intern(pkg,name);
      if(!write_compiled_code_entry(pkg,p,&funcs,-1,entries,&ci,sid,in->creation_code_path,err,errcap)) goto done;
    }
  }
  for(int i=0;i<funcs.n_defs;i++){
    if(funcs.defs[i].is_script_wrapper) continue;
    if(!write_function_code_entry(pkg,p,&funcs,entries,&ci,&funcs.defs[i],err,errcap)) goto done;
  }
  if(ci!=count){
    snprintf(err,errcap,"code entry count mismatch");
    goto done;
  }
  if(!emit_code_data_prefix(pkg)){
    snprintf(err,errcap,"code blob write failed");
    goto done;
  }
  for(int i=0;i<count;i++){
    patch32(&pkg->b,table+(size_t)i*4,(uint32_t)pkg->b.len);
    if(!write_code_entry_header(pkg,&entries[i])){
      snprintf(err,errcap,"code entry header write failed");
      goto done;
    }
  }
  if(!finalize_code_blobs(pkg)){
    snprintf(err,errcap,"code blob layout patch failed");
    goto done;
  }
  if(!patch_ref_chains(pkg)){
    snprintf(err,errcap,"reference chain patch failed");
    goto done;
  }
  chunk_end(pkg,s);
  ok=1;
done:
  free(entries);
  gmlc_function_registry_free(&funcs);
  return ok;
}

static int write_vari(Pkg *pkg){
  if(!patch_ref_chains(pkg)) return 0;
  size_t s=chunk_begin(pkg,"VARI");
  zfill(&pkg->b,12);
  int sid=intern(pkg,"prototype");
  if(sid<0) return 0;
  wstrptr(pkg,sid); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,UINT32_MAX);
  sid=intern(pkg,"@@array@@");
  if(sid<0) return 0;
  wstrptr(pkg,sid); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,UINT32_MAX);
  int arguments_sid=intern(pkg,"arguments");
  if(arguments_sid<0) return 0;
  for(int i=0;i<pkg->n_code_name_refs;i++){
    wstrptr(pkg,arguments_sid); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,UINT32_MAX);
  }
  for(int i=0;i<pkg->n_code_refs;i++){
    if(pkg->code_refs[i].kind!=GMLC_REF_VARI) continue;
    int seen=0;
    for(int j=0;j<i;j++) if(pkg->code_refs[j].kind==GMLC_REF_VARI && same_ref_name(&pkg->code_refs[i],&pkg->code_refs[j])){ seen=1; break; }
    if(seen) continue;
    uint32_t occ=0, first=0;
    ref_group_stats(pkg,i,&occ,&first);
    int sid=intern(pkg,pkg->code_refs[i].name);
    wstrptr(pkg,sid);
    wu32(&pkg->b,0);
    wu32(&pkg->b,0);
    wu32(&pkg->b,occ);
    wu32(&pkg->b,first);
  }
  for(int i=0;i<pkg->n_code_refs;i++){
    if(pkg->code_refs[i].kind!=GMLC_REF_VARI) continue;
    int seen=0;
    for(int j=0;j<i;j++) if(pkg->code_refs[j].kind==GMLC_REF_VARI && same_ref_name(&pkg->code_refs[i],&pkg->code_refs[j])){ seen=1; break; }
    if(seen) continue;
    if(pkg->code_refs[i].name && !strncmp(pkg->code_refs[i].name,"argument",8)) continue;
    int groups=0;
    int has_nonlocal=0;
    for(int j=i;j<pkg->n_code_refs;j++){
      if(!same_ref_name(&pkg->code_refs[i],&pkg->code_refs[j])) continue;
      if(pkg->code_refs[j].inst!=IT_LOCAL){
        has_nonlocal=1;
        continue;
      }
      int group_seen=0;
      for(int k=i;k<j;k++){
        if(same_ref_name(&pkg->code_refs[i],&pkg->code_refs[k]) &&
           pkg->code_refs[k].inst==IT_LOCAL &&
           pkg->code_refs[k].code_index==pkg->code_refs[j].code_index){
          group_seen=1;
          break;
        }
      }
      if(!group_seen) groups++;
    }
    if(groups<=0) continue;
    if(has_nonlocal) groups++;
    int dup_sid=intern(pkg,pkg->code_refs[i].name);
    if(dup_sid<0) return 0;
    for(int g=1;g<groups;g++){
      wstrptr(pkg,dup_sid); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,0); wu32(&pkg->b,UINT32_MAX);
    }
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_func(Pkg *pkg){
  if(!patch_ref_chains(pkg)) return 0;
  size_t s=chunk_begin(pkg,"FUNC");
  wu32(&pkg->b,(uint32_t)ref_group_count(pkg,GMLC_REF_FUNC));
  for(int i=0;i<pkg->n_code_refs;i++){
    if(pkg->code_refs[i].kind!=GMLC_REF_FUNC) continue;
    int seen=0;
    for(int j=0;j<i;j++) if(pkg->code_refs[j].kind==GMLC_REF_FUNC && same_ref_name(&pkg->code_refs[i],&pkg->code_refs[j])){ seen=1; break; }
    if(seen) continue;
    uint32_t occ=0, first=0;
    ref_group_stats(pkg,i,&occ,&first);
    int sid=intern(pkg,pkg->code_refs[i].name);
    wstrptr(pkg,sid);
    wu32(&pkg->b,occ);
    wu32(&pkg->b,first);
  }
  int arguments_sid=intern(pkg,"arguments");
  if(arguments_sid<0) return 0;
  wu32(&pkg->b,(uint32_t)pkg->n_code_name_refs);
  for(int i=0;i<pkg->n_code_name_refs;i++){
    wu32(&pkg->b,1);
    wstrptr(pkg,pkg->code_name_refs[i].sid);
    wu32(&pkg->b,0);
    wstrptr(pkg,arguments_sid);
  }
  chunk_end(pkg,s);
  return 1;
}

static int write_file(const char *path, const uint8_t *data, size_t len, char *err, size_t errcap){
  FILE *f=fopen(path,"wb");
  if(!f){ snprintf(err,errcap,"%s: open for write failed",path); return 0; }
  if(fwrite(data,1,len,f)!=len){ fclose(f); snprintf(err,errcap,"%s: write failed",path); return 0; }
  fclose(f);
  return 1;
}

int gmlc_package_write_structural(const GmlcProject *p, const char *out_path, char *err, size_t errcap){
  Pkg pkg;
  memset(&pkg,0,sizeof(pkg));
  wbytes(&pkg.b,"FORM",4);
  size_t form_size_pos=pkg.b.len;
  wu32(&pkg.b,0);
  if(!write_gen8(&pkg,p) ||
     !write_optn(&pkg) ||
     !fixed_zero_chunk(&pkg,"LANG",12) ||
     !empty_list_chunk(&pkg,"EXTN") ||
     !write_sond(&pkg,p) ||
     !write_agrp(&pkg) ||
     !write_sprt(&pkg,p,err,errcap) ||
     !write_bgnd(&pkg,p) ||
     !empty_list_chunk(&pkg,"PATH") ||
     !write_scpt(&pkg,p) ||
     !empty_list_chunk(&pkg,"GLOB") ||
     !write_shdr(&pkg,p) ||
     !write_font(&pkg,p) ||
     !empty_list_chunk(&pkg,"TMLN") ||
     !write_objt(&pkg,p) ||
     !write_room_chunk(&pkg,p) ||
     !fixed_zero_chunk(&pkg,"DAFL",0) ||
     !write_embi(&pkg) ||
     !write_tpag(&pkg,p) ||
     !write_code(&pkg,p,err,errcap) ||
     !write_vari(&pkg) ||
     !write_func(&pkg) ||
     !write_strg(&pkg) ||
     !write_txtr(&pkg,p,err,errcap) ||
     !write_audo(&pkg,p,err,errcap)){
    if(!err[0]) snprintf(err,errcap,"out of memory while writing package");
    free(pkg.b.data);
    free(pkg.code_data.data);
    for(int i=0;i<pkg.strs.n;i++) free(pkg.strs.items[i]);
    free(pkg.strs.items); free(pkg.strs.char_off); free(pkg.patches);
    free(pkg.frame_patches); free(pkg.font_patches);
    free(pkg.frame_tpag_ptr); free(pkg.font_tpag_ptr);
    free(pkg.texture_place);
    free(pkg.code_blob_patches);
    free(pkg.code_name_refs);
    for(int i=0;i<pkg.n_code_refs;i++) free(pkg.code_refs[i].name);
    free(pkg.code_refs);
    return 0;
  }
  patch32(&pkg.b,form_size_pos,(uint32_t)(pkg.b.len-8));
  int ok=write_file(out_path,pkg.b.data,pkg.b.len,err,errcap);
  free(pkg.b.data);
  free(pkg.code_data.data);
  for(int i=0;i<pkg.strs.n;i++) free(pkg.strs.items[i]);
  free(pkg.strs.items); free(pkg.strs.char_off); free(pkg.patches);
  free(pkg.frame_patches); free(pkg.font_patches);
  free(pkg.frame_tpag_ptr); free(pkg.font_tpag_ptr);
  free(pkg.texture_place);
  free(pkg.code_blob_patches);
  free(pkg.code_name_refs);
  for(int i=0;i<pkg.n_code_refs;i++) free(pkg.code_refs[i].name);
  free(pkg.code_refs);
  return ok;
}
