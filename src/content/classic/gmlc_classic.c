/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_classic.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static uint32_t read_u32le(const uint8_t *p){
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static int known_version(uint32_t version){
  return version == GMLC_CLASSIC_GM6 || version == GMLC_CLASSIC_GM7 ||
         version == GMLC_CLASSIC_GM7_ALT || version == GMLC_CLASSIC_GM8 ||
         version == GMLC_CLASSIC_GM81;
}

int gmlc_classic_probe(const void *data, size_t size, GmlcClassicHeader *out,
                       char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!data || !out){
    if(err && errcap) snprintf(err, errcap, "classic project: invalid probe arguments");
    return 0;
  }
  if(size < 28){
    if(err && errcap) snprintf(err, errcap, "classic project: truncated common header (%zu bytes)", size);
    return 0;
  }
  const uint8_t *p = (const uint8_t*)data;
  uint32_t magic = read_u32le(p);
  uint32_t version = read_u32le(p + 4);
  if(magic != GMLC_CLASSIC_MAGIC){
    if(err && errcap) snprintf(err, errcap, "classic project: bad magic %u", magic);
    return 0;
  }
  if(!known_version(version)){
    if(err && errcap) snprintf(err, errcap, "classic project: unsupported container version %u", version);
    return 0;
  }
  memset(out, 0, sizeof(*out));
  out->version = (GmlcClassicVersion)version;
  out->game_id = read_u32le(p + 8);
  memcpy(out->guid, p + 12, sizeof(out->guid));
  return 1;
}

int gmlc_classic_probe_file(const char *path, GmlcClassicHeader *out,
                            char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!path || !out){
    if(err && errcap) snprintf(err, errcap, "classic project: invalid file probe arguments");
    return 0;
  }
  FILE *f = fopen(path, "rb");
  if(!f){
    if(err && errcap) snprintf(err, errcap, "classic project: cannot open %s: %s", path, strerror(errno));
    return 0;
  }
  uint8_t header[28];
  size_t got = fread(header, 1, sizeof(header), f);
  int io_error = ferror(f);
  fclose(f);
  if(io_error){
    if(err && errcap) snprintf(err, errcap, "classic project: cannot read %s", path);
    return 0;
  }
  return gmlc_classic_probe(header, got, out, err, errcap);
}

const char *gmlc_classic_version_name(GmlcClassicVersion version){
  switch(version){
    case GMLC_CLASSIC_GM6: return "GameMaker 6";
    case GMLC_CLASSIC_GM7:
    case GMLC_CLASSIC_GM7_ALT: return "GameMaker 7";
    case GMLC_CLASSIC_GM8: return "GameMaker 8";
    case GMLC_CLASSIC_GM81: return "GameMaker 8.1";
    default: return "unknown classic GameMaker";
  }
}
