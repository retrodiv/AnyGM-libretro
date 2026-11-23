/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* After room warming, idle atlas workers decode pages nothing has referenced yet, so a page first
 * drawn later in a room is already decoded instead of stalling that frame. A host that declares
 * memory scarce by offering reclaimable copies keeps the on-demand behaviour. */
#include "gml_render_internal.h"
#include "gml_win.h"
#include "anygm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PAGES 6
#define TEX_W 4
#define TEX_H 4

static void store_u32le(uint8_t *at,uint32_t value){
  at[0]=(uint8_t)value;at[1]=(uint8_t)(value>>8);at[2]=(uint8_t)(value>>16);at[3]=(uint8_t)(value>>24);
}

/* One flat colour per page: COLOR, then RUN_8 over the remaining pixels. */
static size_t build_page(uint8_t *out,uint8_t shade){
  memcpy(out,"fioq",4);
  out[4]=TEX_W;out[5]=0;out[6]=TEX_H;out[7]=0;
  store_u32le(out+8,0);
  size_t at=12;
  out[at++]=0xff;out[at++]=shade;out[at++]=(uint8_t)(255u-shade);out[at++]=0x40;out[at++]=0xff;
  out[at++]=(uint8_t)(0x40|(TEX_W*TEX_H-2));
  return at;
}

static void *reclaimable_copy(void *userdata,const void *source,size_t size){
  (void)userdata;(void)source;(void)size;
  return NULL;
}

static int build(GmlWin *win,GmlRender *render,uint8_t *data,size_t capacity){
  memset(data,0,capacity);
  store_u32le(data,PAGES);
  size_t blob=64u+PAGES*16u;
  for(uint32_t page=0;page<PAGES;page++){
    uint32_t record=64u+page*16u;
    store_u32le(data+4u+page*4u,record);
    store_u32le(data+record+8u,(uint32_t)blob);
    blob+=build_page(data+blob,(uint8_t)(page*40u));
  }
  memcpy(win->chunks[0].name,"TXTR",4);
  win->chunks[0].off=0;
  win->chunks[0].size=(uint32_t)blob;
  win->n_chunks=1;
  win->data=data;
  win->size=capacity;
  render->win=win;
  parse_txtr(render);
  return render->n_atlas==PAGES;
}

static int decoded_pages(GmlRender *render){
  int count=0;
  for(int page=0;page<render->n_atlas;page++)
    if(__atomic_load_n(&render->atlas[page].px,__ATOMIC_ACQUIRE)) count++;
  return count;
}

static void release(GmlRender *render){
  atlas_pool_free(render);
  for(int page=0;page<render->n_atlas;page++)
    if(render->atlas[page].px) gml_render_atlas_pixels_release(render,&render->atlas[page]);
  free(render->atlas);
}

int main(void){
  static uint8_t data[4096];
  int ok=1;
  {
    GmlWin win;GmlRender render;
    memset(&win,0,sizeof win);memset(&render,0,sizeof render);
    if(!build(&win,&render,data,sizeof data)){ fprintf(stderr,"fixture pages were not parsed\n"); return 1; }
    gml_render_atlas_fill(&render);
    /* Workers own the timing; the outcome is that every page becomes decoded unreferenced. */
    for(int wait=0;wait<5000 && decoded_pages(&render)<PAGES;wait++){
      struct timespec pause={0,1000000};
      nanosleep(&pause,NULL);
    }
    if(decoded_pages(&render)!=PAGES){
      fprintf(stderr,"idle fill decoded %d of %d unreferenced pages\n",decoded_pages(&render),PAGES);
      ok=0;
    }
    for(int page=0;page<render.n_atlas && ok;page++){
      const uint8_t *px=render.atlas[page].px;
      for(int pixel=0;pixel<TEX_W*TEX_H && ok;pixel++)
        if(px[pixel*4]!=(uint8_t)(page*40) || px[pixel*4+1]!=(uint8_t)(255-page*40) ||
           px[pixel*4+3]!=0xff){
          fprintf(stderr,"page %d pixel %d decoded to the wrong colour\n",page,pixel);
          ok=0;
        }
    }
    release(&render);
  }
  {
    AnygmHostServices host;
    GmlWin win;GmlRender render;
    memset(&host,0,sizeof host);memset(&win,0,sizeof win);memset(&render,0,sizeof render);
    host.reclaimable_copy=reclaimable_copy;
    win.host=&host;
    if(!build(&win,&render,data,sizeof data)){ fprintf(stderr,"fixture pages were not parsed\n"); return 1; }
    gml_render_atlas_fill(&render);
    if(render.prefetch || decoded_pages(&render)){
      fprintf(stderr,"a host declaring memory scarce still started a speculative fill\n");
      ok=0;
    }
    release(&render);
  }
  if(ok) printf("renderer atlas fill: unreferenced pages decode ahead of use\n");
  return ok?0:1;
}
