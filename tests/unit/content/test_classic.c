/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_classic.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

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
  int encrypted = version == 701 || version == 702;
  if((unsigned)h.version != version ||
     (!encrypted && (h.game_id != 0x12345678u || memcmp(h.guid, data + 12, 16))) ||
     (encrypted && h.game_id != 0)){
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



static int expect_gm7_decode(void){
  unsigned char plain[64] = {0};
  put_u32le(plain, GMLC_CLASSIC_MAGIC);
  put_u32le(plain + 4, 701);
  for(size_t i = 8; i < sizeof(plain); ++i) plain[i] = (unsigned char)(i * 3 + 1);
  size_t encoded_size = 0;
  unsigned char *encoded = encode_gm7(plain, sizeof(plain), &encoded_size);
  if(!encoded) return 0;
  uint8_t *decoded = NULL;
  size_t decoded_size = 0;
  char err[128];
  int ok = (0 /* This operation is unavailable. */) &&
           decoded_size == sizeof(plain) && !memcmp(decoded, plain, sizeof(plain));
  if(!ok) fprintf(stderr, "GM7 decode fixture failed: %s\n", err);
  free(decoded);
  free(encoded);
  return ok;
}

typedef struct {
  unsigned char data[1024];
  size_t size;
} Fixture;

static void fixture_u32(Fixture *f, unsigned value){
  put_u32le(f->data + f->size, value);
  f->size += 4;
}

static void fixture_zero(Fixture *f, size_t count){
  memset(f->data + f->size, 0, count);
  f->size += count;
}

static void fixture_string(Fixture *f, const char *text){
  size_t length = text ? strlen(text) : 0;
  fixture_u32(f, (unsigned)length);
  if(length){
    memcpy(f->data + f->size, text, length);
    f->size += length;
  }
}

static void fixture_compressed(Fixture *f, const unsigned char *raw, int raw_size){
  int compressed_size = 0;
  unsigned char *compressed = stbi_zlib_compress((unsigned char*)raw, raw_size, &compressed_size, 8);
  if(!compressed || compressed_size <= 0 || f->size + 4 + (size_t)compressed_size > sizeof(f->data)) abort();
  fixture_u32(f, (unsigned)compressed_size);
  memcpy(f->data + f->size, compressed, (size_t)compressed_size);
  f->size += (size_t)compressed_size;
  STBIW_FREE(compressed);
}

static Fixture inventory_fixture(unsigned container_version){
  Fixture f = {{0}, 0};
  fixture_u32(&f, GMLC_CLASSIC_MAGIC);
  fixture_u32(&f, container_version);
  fixture_u32(&f, 7);
  fixture_zero(&f, 16);
  fixture_u32(&f, 800); /* settings version */
  fixture_u32(&f, 0);   /* compressed settings */
  fixture_u32(&f, 800); fixture_u32(&f, 0); fixture_zero(&f, 8); /* triggers */
  fixture_u32(&f, 800); fixture_u32(&f, 0); fixture_zero(&f, 8); /* constants */
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    fixture_u32(&f, 800);
    fixture_u32(&f, type);
    for(unsigned slot = 0; slot < type; ++slot) fixture_u32(&f, 0);
  }
  fixture_u32(&f, 100123);
  fixture_u32(&f, 1000456);
  return f;
}

static int expect_inventory(unsigned version){
  Fixture f = inventory_fixture(version);
  GmlcClassicInventory in;
  char err[128];
  if(!gmlc_classic_inventory(f.data, f.size, &in, err, sizeof(err))){
    fprintf(stderr, "inventory %u failed: %s\n", version, err);
    return 0;
  }
  if(in.payload_end != f.size || in.last_instance_id != 100123 || in.last_tile_id != 1000456)
    return 0;
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type)
    if(in.resource_slots[type] != type) return 0;
  return 1;
}

static Fixture manifest_fixture(unsigned container_version){
  Fixture f = {{0}, 0};
  fixture_u32(&f, GMLC_CLASSIC_MAGIC); fixture_u32(&f, container_version);
  fixture_u32(&f, 9); fixture_zero(&f, 16);
  fixture_u32(&f, 800); fixture_u32(&f, 0);
  fixture_u32(&f, 800); fixture_u32(&f, 0); fixture_zero(&f, 8);
  fixture_u32(&f, 800); fixture_u32(&f, 0); fixture_zero(&f, 8);
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    fixture_u32(&f, 800); fixture_u32(&f, 1);
    if(type == GMLC_CLASSIC_SCRIPT){
      unsigned char raw[128] = {0};
      size_t n = 0;
      put_u32le(raw + n, 1); n += 4;
      const char name[] = "resource_script";
      put_u32le(raw + n, sizeof(name) - 1); n += 4;
      memcpy(raw + n, name, sizeof(name) - 1); n += sizeof(name) - 1;
      n += 8;
      put_u32le(raw + n, 800); n += 4;
      const char source[] = "return 7;";
      put_u32le(raw + n, sizeof(source) - 1); n += 4;
      memcpy(raw + n, source, sizeof(source) - 1); n += sizeof(source) - 1;
      fixture_compressed(&f, raw, (int)n);
    } else {
      const unsigned char absent[4] = {0, 0, 0, 0};
      fixture_compressed(&f, absent, sizeof(absent));
    }
  }
  fixture_u32(&f, 100001); fixture_u32(&f, 1000001);
  return f;
}

static int expect_manifest(void){
  Fixture f = manifest_fixture(800);
  GmlcClassicManifest manifest;
  char err[128];
  if(!gmlc_classic_manifest(f.data, f.size, &manifest, err, sizeof(err))){
    fprintf(stderr, "manifest failed: %s\n", err);
    return 0;
  }
  int ok = manifest.existing[GMLC_CLASSIC_SCRIPT] == 1 &&
           manifest.slots[GMLC_CLASSIC_SCRIPT][0].exists &&
           manifest.slots[GMLC_CLASSIC_SCRIPT][0].version == 800 &&
           !strcmp(manifest.slots[GMLC_CLASSIC_SCRIPT][0].name, "resource_script") &&
           !strcmp(manifest.slots[GMLC_CLASSIC_SCRIPT][0].source, "return 7;");
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type)
    if(type != GMLC_CLASSIC_SCRIPT && manifest.existing[type]) ok = 0;
  gmlc_classic_manifest_free(&manifest);
  return ok;
}

static int expect_manifest_810(void){
  Fixture f = manifest_fixture(810);
  GmlcClassicManifest manifest;
  char err[128];
  if(!gmlc_classic_manifest(f.data, f.size, &manifest, err, sizeof(err))){
    fprintf(stderr, "manifest 810 failed: %s\n", err);
    return 0;
  }
  int ok = manifest.inventory.header.version == GMLC_CLASSIC_GM81 &&
           manifest.existing[GMLC_CLASSIC_SCRIPT] == 1;
  gmlc_classic_manifest_free(&manifest);
  return ok;
}

static Fixture legacy_fixture(unsigned container_version){
  Fixture f = {{0}, 0};
  int gm7 = container_version == 701 || container_version == 702;
  fixture_u32(&f, GMLC_CLASSIC_MAGIC); fixture_u32(&f, container_version);
  fixture_u32(&f, 42); fixture_zero(&f, 16);
  fixture_u32(&f, gm7 ? 702 : 600);
  for(unsigned i = 0; i < (gm7 ? 22u : 20u); ++i) fixture_u32(&f, 0);
  fixture_u32(&f, 0); /* loading bar */
  fixture_u32(&f, 0); /* custom loading image */
  fixture_u32(&f, 0); fixture_u32(&f, 255); fixture_u32(&f, 1);
  fixture_u32(&f, 0); /* icon blob */
  for(unsigned i = 0; i < 4; ++i) fixture_u32(&f, 0);
  fixture_string(&f, "");
  if(gm7) fixture_string(&f, "100"); else fixture_u32(&f, 100);
  fixture_zero(&f, 8);
  fixture_string(&f, "");
  fixture_u32(&f, 0); /* constants */
  if(gm7){
    for(unsigned i = 0; i < 4; ++i) fixture_u32(&f, i == 0 ? 1 : 0);
    for(unsigned i = 0; i < 4; ++i) fixture_string(&f, "");
  } else {
    fixture_u32(&f, 0); /* includes */
    fixture_u32(&f, 0); fixture_u32(&f, 0); fixture_u32(&f, 0);
  }
  const unsigned section_versions[GMLC_CLASSIC_RESOURCE_TYPES] =
    {400, 400, 400, 420, 400, 540, 500, 400, 420};
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    fixture_u32(&f, section_versions[type]);
    fixture_u32(&f, 0);
  }
  fixture_u32(&f, 100000); fixture_u32(&f, 1000000);
  return f;
}

static int expect_legacy_manifest(unsigned version){
  Fixture plain = legacy_fixture(version);
  const unsigned char *data = plain.data;
  size_t size = plain.size;
  unsigned char *encoded = NULL;
  if(version == 701 || version == 702){
    encoded = encode_gm7(plain.data, plain.size, &size);
    if(!encoded) return 0;
    data = encoded;
  }
  GmlcClassicManifest manifest;
  char err[128];
  int ok = gmlc_classic_manifest(data, size, &manifest, err, sizeof(err));
  if(!ok) fprintf(stderr, "legacy manifest %u failed: %s\n", version, err);
  if(ok){
    ok = manifest.inventory.settings_version == (version == 600 ? 600u : 702u) &&
         manifest.inventory.last_instance_id == 100000 &&
         manifest.inventory.last_tile_id == 1000000;
    gmlc_classic_manifest_free(&manifest);
  }
  free(encoded);
  return ok;
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

  if(expect_inventory(800)) ++passed; else ++failed;
  if(expect_inventory(810)) ++passed; else ++failed;
  if(expect_manifest()) ++passed; else ++failed;
  if(expect_manifest_810()) ++passed; else ++failed;
  if(expect_gm7_decode()) ++passed; else ++failed;
  if(expect_legacy_manifest(600)) ++passed; else ++failed;
  if(expect_legacy_manifest(701)) ++passed; else ++failed;

  for(int i = 1; i < argc; ++i){
    GmlcClassicInventory in;
    GmlcClassicHeader h;
    char err[512];
    if(!gmlc_classic_probe_file(argv[i], &h, err, sizeof(err))){
      fprintf(stderr, "%s: %s\n", argv[i], err);
      ++failed;
      continue;
    }
    if(gmlc_classic_inventory_file(argv[i], &in, err, sizeof(err))){
      GmlcClassicManifest manifest;
      if(!gmlc_classic_manifest_file(argv[i], &manifest, err, sizeof(err))){
        fprintf(stderr, "%s: %s\n", argv[i], err);
        ++failed;
        continue;
      }
      printf("%s\t%u\t%s\tsettings=%u sounds=%u/%u sprites=%u/%u backgrounds=%u/%u paths=%u/%u scripts=%u/%u fonts=%u/%u timelines=%u/%u objects=%u/%u rooms=%u/%u\n",
             argv[i], (unsigned)h.version, gmlc_classic_version_name(h.version), in.settings_version,
             manifest.existing[GMLC_CLASSIC_SOUND], in.resource_slots[GMLC_CLASSIC_SOUND],
             manifest.existing[GMLC_CLASSIC_SPRITE], in.resource_slots[GMLC_CLASSIC_SPRITE],
             manifest.existing[GMLC_CLASSIC_BACKGROUND], in.resource_slots[GMLC_CLASSIC_BACKGROUND],
             manifest.existing[GMLC_CLASSIC_PATH], in.resource_slots[GMLC_CLASSIC_PATH],
             manifest.existing[GMLC_CLASSIC_SCRIPT], in.resource_slots[GMLC_CLASSIC_SCRIPT],
             manifest.existing[GMLC_CLASSIC_FONT], in.resource_slots[GMLC_CLASSIC_FONT],
             manifest.existing[GMLC_CLASSIC_TIMELINE], in.resource_slots[GMLC_CLASSIC_TIMELINE],
             manifest.existing[GMLC_CLASSIC_OBJECT], in.resource_slots[GMLC_CLASSIC_OBJECT],
             manifest.existing[GMLC_CLASSIC_ROOM], in.resource_slots[GMLC_CLASSIC_ROOM]);
      gmlc_classic_manifest_free(&manifest);
    } else {
      printf("%s\t%u\t%s\n", argv[i], (unsigned)h.version,
             gmlc_classic_version_name(h.version));
    }
    ++passed;
  }
  printf("classic probe: passed=%d failed=%d\n", passed, failed);
  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
