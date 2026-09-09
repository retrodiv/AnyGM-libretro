/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef ANYGM_FONT_TEST_FIXTURE_H
#define ANYGM_FONT_TEST_FIXTURE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One authored rectangular glyph in a bounded TrueType container. */
static void font_fixture_put_u16(uint8_t *data, size_t offset, unsigned value){
  data[offset]=(uint8_t)(value>>8);
  data[offset+1]=(uint8_t)value;
}

static void font_fixture_put_u32(uint8_t *data, size_t offset, uint32_t value){
  data[offset]=(uint8_t)(value>>24);
  data[offset+1]=(uint8_t)(value>>16);
  data[offset+2]=(uint8_t)(value>>8);
  data[offset+3]=(uint8_t)value;
}

static void font_fixture_put_table(uint8_t *data, size_t directory, const char tag[4],
                      uint32_t offset, uint32_t length){
  memcpy(data+directory,tag,4);
  font_fixture_put_u32(data,directory+8,offset);
  font_fixture_put_u32(data,directory+12,length);
}

static size_t font_fixture_build(uint8_t data[552]){
  enum {
    DIRECTORY=12,
    CMAP=124,
    GLYF=400,
    HEAD=436,
    HHEA=492,
    HMTX=528,
    LOCA=536,
    MAXP=544
  };
  memset(data,0,552);
  font_fixture_put_u32(data,0,0x00010000u);
  font_fixture_put_u16(data,4,7);
  font_fixture_put_u16(data,6,64);
  font_fixture_put_u16(data,8,2);
  font_fixture_put_u16(data,10,48);
  font_fixture_put_table(data,DIRECTORY+0u*16u,"cmap",CMAP,274);
  font_fixture_put_table(data,DIRECTORY+1u*16u,"glyf",GLYF,34);
  font_fixture_put_table(data,DIRECTORY+2u*16u,"head",HEAD,54);
  font_fixture_put_table(data,DIRECTORY+3u*16u,"hhea",HHEA,36);
  font_fixture_put_table(data,DIRECTORY+4u*16u,"hmtx",HMTX,8);
  font_fixture_put_table(data,DIRECTORY+5u*16u,"loca",LOCA,6);
  font_fixture_put_table(data,DIRECTORY+6u*16u,"maxp",MAXP,6);

  font_fixture_put_u16(data,CMAP,0);
  font_fixture_put_u16(data,CMAP+2,1);
  font_fixture_put_u16(data,CMAP+4,3);
  font_fixture_put_u16(data,CMAP+6,1);
  font_fixture_put_u32(data,CMAP+8,12);
  font_fixture_put_u16(data,CMAP+12,0);
  font_fixture_put_u16(data,CMAP+14,262);
  font_fixture_put_u16(data,CMAP+16,0);
  data[CMAP+18+65]=1;

  font_fixture_put_u16(data,GLYF,1);
  font_fixture_put_u16(data,GLYF+2,0);
  font_fixture_put_u16(data,GLYF+4,0);
  font_fixture_put_u16(data,GLYF+6,500);
  font_fixture_put_u16(data,GLYF+8,700);
  font_fixture_put_u16(data,GLYF+10,3);
  font_fixture_put_u16(data,GLYF+12,0);
  data[GLYF+14]=1;
  data[GLYF+15]=1;
  data[GLYF+16]=1;
  data[GLYF+17]=1;
  font_fixture_put_u16(data,GLYF+18,0);
  font_fixture_put_u16(data,GLYF+20,500);
  font_fixture_put_u16(data,GLYF+22,0);
  font_fixture_put_u16(data,GLYF+24,(unsigned)-500);
  font_fixture_put_u16(data,GLYF+26,0);
  font_fixture_put_u16(data,GLYF+28,0);
  font_fixture_put_u16(data,GLYF+30,700);
  font_fixture_put_u16(data,GLYF+32,0);

  font_fixture_put_u32(data,HEAD,0x00010000u);
  font_fixture_put_u32(data,HEAD+4,0x00010000u);
  font_fixture_put_u32(data,HEAD+12,0x5f0f3cf5u);
  font_fixture_put_u16(data,HEAD+18,1000);
  font_fixture_put_u16(data,HEAD+36,0);
  font_fixture_put_u16(data,HEAD+38,0);
  font_fixture_put_u16(data,HEAD+40,500);
  font_fixture_put_u16(data,HEAD+42,700);
  font_fixture_put_u16(data,HEAD+46,8);
  font_fixture_put_u16(data,HEAD+48,2);
  font_fixture_put_u16(data,HEAD+50,0);
  font_fixture_put_u16(data,HEAD+52,0);

  font_fixture_put_u32(data,HHEA,0x00010000u);
  font_fixture_put_u16(data,HHEA+4,800);
  font_fixture_put_u16(data,HHEA+6,(unsigned)-200);
  font_fixture_put_u16(data,HHEA+8,200);
  font_fixture_put_u16(data,HHEA+10,600);
  font_fixture_put_u16(data,HHEA+16,500);
  font_fixture_put_u16(data,HHEA+18,1);
  font_fixture_put_u16(data,HHEA+34,2);

  font_fixture_put_u16(data,HMTX,500);
  font_fixture_put_u16(data,HMTX+2,0);
  font_fixture_put_u16(data,HMTX+4,600);
  font_fixture_put_u16(data,HMTX+6,0);
  font_fixture_put_u16(data,LOCA,0);
  font_fixture_put_u16(data,LOCA+2,0);
  font_fixture_put_u16(data,LOCA+4,17);
  font_fixture_put_u32(data,MAXP,0x00010000u);
  font_fixture_put_u16(data,MAXP+4,2);
  return 552;
}

#endif
