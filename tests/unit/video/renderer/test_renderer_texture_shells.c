/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Verify both compressed texture shells with authored synthetic pixels. The optional
 * length field precedes the bzip2 stream; if it is absent, "BZh" begins at that offset.
 * Mistaking the stream magic for a length would reject the atlas before decoding pixels. */
#include "gml_render_internal.h"
#include "gml_win.h"
#include "gm_qoi.h"
#include "bzlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEX_W 4
#define TEX_H 4
#define RED   0xC0u
#define GREEN 0x30u
#define BLUE  0x60u

static void store_u32le(uint8_t *at, uint32_t value){
  at[0]=(uint8_t)(value&0xff);       at[1]=(uint8_t)((value>>8)&0xff);
  at[2]=(uint8_t)((value>>16)&0xff); at[3]=(uint8_t)((value>>24)&0xff);
}
static void store_u16le(uint8_t *at, uint16_t value){
  at[0]=(uint8_t)(value&0xff); at[1]=(uint8_t)((value>>8)&0xff);
}

/* A "fioq" stream of one flat colour: set it with COLOR, then repeat it with RUN_8. */
static size_t build_fioq(uint8_t *out, size_t cap){
  if(cap < 12+5+1) return 0;
  memcpy(out,"fioq",4);
  store_u16le(out+4,TEX_W);
  store_u16le(out+6,TEX_H);
  store_u32le(out+8,0);              /* the decoder starts reading ops at 12 */
  size_t p=12;
  out[p++]=0xff;                     /* COLOR with all four channels present */
  out[p++]=RED; out[p++]=GREEN; out[p++]=BLUE; out[p++]=0xff;
  out[p++]=(uint8_t)(0x40 | (TEX_W*TEX_H - 1 - 1));   /* RUN_8: the remaining pixels */
  return p;
}

/* Wrap a bzip2 stream in a "2zoq" shell, with the length field or without it. */
static size_t build_2zoq(uint8_t *out, size_t cap, const uint8_t *bz, size_t bzlen,
                         size_t raw_length, int with_length){
  size_t head = with_length ? 12u : 8u;
  if(cap < head+bzlen) return 0;
  memcpy(out,"2zoq",4);
  store_u16le(out+4,TEX_W);
  store_u16le(out+6,TEX_H);
  if(with_length) store_u32le(out+8,(uint32_t)raw_length);
  memcpy(out+head,bz,bzlen);
  return head+bzlen;
}

static int decodes(const char *label, const uint8_t *shell, size_t shell_len){
  /* One atlas, one texture, and the renderer asked for its pixels — the same call the draw path
   * makes when a sprite first needs the page. */
  static uint8_t data[65536];
  memset(data,0,sizeof data);
  GmlWin win; memset(&win,0,sizeof win);
  GmlRender render; memset(&render,0,sizeof render);

  size_t blob_at = 256;
  if(blob_at + shell_len > sizeof data){ fprintf(stderr,"%s: fixture too large\n",label); return 0; }
  store_u32le(data+0,1);             /* TXTR count */
  store_u32le(data+4,16);            /* pointer to the one texture record */
  store_u32le(data+16+8,(uint32_t)blob_at);   /* the record's blob pointer, at +8 within it */
  memcpy(data+blob_at,shell,shell_len);

  win.data=data;
  win.size=sizeof data;
  memcpy(win.chunks[0].name,"TXTR",4);
  win.chunks[0].off=0;
  win.chunks[0].size=(uint32_t)(blob_at+shell_len);
  win.n_chunks=1;
  render.win=&win;

  parse_txtr(&render);
  if(render.n_atlas!=1){
    fprintf(stderr,"%s: expected one atlas, got %d\n",label,render.n_atlas);
    return 0;
  }
  uint8_t *px=atlas_pixels(&render,0);
  int ok=1;
  if(!px){
    fprintf(stderr,"%s: the atlas decoded to nothing, so every sprite on it draws empty\n",label);
    ok=0;
  } else if(render.atlas[0].w!=TEX_W || render.atlas[0].h!=TEX_H){
    fprintf(stderr,"%s: decoded %dx%d, expected %dx%d\n",
            label,render.atlas[0].w,render.atlas[0].h,TEX_W,TEX_H);
    ok=0;
  } else {
    for(int i=0;i<TEX_W*TEX_H && ok;i++){
      const uint8_t *p=px+(size_t)i*4;
      if(p[0]!=RED || p[1]!=GREEN || p[2]!=BLUE || p[3]!=0xff){
        fprintf(stderr,"%s: pixel %d is %02x%02x%02x%02x, expected %02x%02x%02x ff\n",
                label,i,p[0],p[1],p[2],p[3],RED,GREEN,BLUE);
        ok=0;
      }
    }
  }
  free(render.atlas);
  return ok;
}

int main(void){
  uint8_t raw[256];
  size_t rawlen=build_fioq(raw,sizeof raw);
  if(!rawlen){ fprintf(stderr,"could not build the fixture stream\n"); return 1; }

  char packed[4096];
  unsigned int packedlen=(unsigned int)sizeof packed;
  int rc=BZ2_bzBuffToBuffCompress(packed,&packedlen,(char*)raw,(unsigned int)rawlen,9,0,30);
  if(rc!=BZ_OK){ fprintf(stderr,"could not compress the fixture: bzip2 rc=%d\n",rc); return 1; }
  if(memcmp(packed,"BZh",3)!=0){ fprintf(stderr,"the fixture is not a bzip2 stream\n"); return 1; }

  uint8_t shell[8192];
  int ok=1;

  size_t with=build_2zoq(shell,sizeof shell,(uint8_t*)packed,packedlen,rawlen,1);
  ok &= with && decodes("2zoq carrying its decompressed length",shell,with);

  size_t without=build_2zoq(shell,sizeof shell,(uint8_t*)packed,packedlen,rawlen,0);
  ok &= without && decodes("2zoq starting its stream straight after the height",shell,without);

  if(ok) printf("renderer texture shells: both 2zoq layouts decode\n");
  return ok?0:1;
}
