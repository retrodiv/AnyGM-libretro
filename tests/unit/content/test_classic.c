/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_classic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u32le(unsigned char *p, unsigned value){
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
  p[2] = (unsigned char)(value >> 16);
  p[3] = (unsigned char)(value >> 24);
}

static int expect_header(unsigned version){
  unsigned char data[28] = {0};
  put_u32le(data, GMLC_CLASSIC_MAGIC);
  put_u32le(data + 4, version);
  put_u32le(data + 8, 0x12345678u);
  for(int i = 0; i < 16; ++i) data[12 + i] = (unsigned char)(0xa0 + i);
  GmlcClassicHeader h;
  char err[128];
  if(!gmlc_classic_probe(data, sizeof(data), &h, err, sizeof(err))){
    fprintf(stderr, "probe %u failed: %s\n", version, err);
    return 0;
  }
  if((unsigned)h.version != version || h.game_id != 0x12345678u ||
     memcmp(h.guid, data + 12, 16)){
    fprintf(stderr, "probe %u returned incorrect fields\n", version);
    return 0;
  }
  return 1;
}

static int expect_rejected(unsigned magic, unsigned version, size_t size){
  unsigned char data[28] = {0};
  put_u32le(data, magic);
  put_u32le(data + 4, version);
  GmlcClassicHeader h;
  char err[128];
  return !gmlc_classic_probe(data, size, &h, err, sizeof(err)) && err[0];
}

int main(int argc, char **argv){
  const unsigned versions[] = {600, 701, 702, 800, 810};
  int passed = 0, failed = 0;
  for(size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i){
    if(expect_header(versions[i])) ++passed; else ++failed;
  }
  if(expect_rejected(0, 800, 28)) ++passed; else ++failed;
  if(expect_rejected(GMLC_CLASSIC_MAGIC, 999, 28)) ++passed; else ++failed;
  if(expect_rejected(GMLC_CLASSIC_MAGIC, 800, 27)) ++passed; else ++failed;

  for(int i = 1; i < argc; ++i){
    GmlcClassicHeader h;
    char err[512];
    if(!gmlc_classic_probe_file(argv[i], &h, err, sizeof(err))){
      fprintf(stderr, "%s: %s\n", argv[i], err);
      ++failed;
      continue;
    }
    printf("%s\t%u\t%s\n", argv[i], (unsigned)h.version,
           gmlc_classic_version_name(h.version));
    ++passed;
  }
  printf("classic probe: passed=%d failed=%d\n", passed, failed);
  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
