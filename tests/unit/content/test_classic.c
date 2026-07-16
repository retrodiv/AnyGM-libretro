/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_classic.h"
#include "gmlc_classic_import.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_GIF
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

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
  unsigned char data[16384];
  size_t size;
} Fixture;

static void fixture_compressed(Fixture *f, const unsigned char *raw, int raw_size);

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



static int write_fixture_file(const char *path, const Fixture *fixture, size_t limit){
  FILE *file=fopen(path,"wb");
  size_t size=fixture->size<limit ? fixture->size : limit;
  int ok=file && fwrite(fixture->data,1,size,file)==size;
  if(file && fclose(file)!=0) ok=0;
  return ok;
}

static int expect_extension_alias_import(void){
  const char *dir="tmp/classic_extension_fixture";
#ifdef _WIN32
  _mkdir("tmp"); _mkdir(dir);
#else
  mkdir("tmp",0777); mkdir(dir,0777);
#endif
  const char *paths[]={
    "tmp/classic_extension_fixture/base.gex",
    "tmp/classic_extension_fixture/secondary.GEX",
    "tmp/classic_extension_fixture/conflict.gex",
    "tmp/classic_extension_fixture/unrelated.gex",
    "tmp/classic_extension_fixture/truncated.gex",
    "tmp/classic_extension_fixture/embedded.gex"
  };
  Fixture fixtures[]={
    extension_fixture("fixtureextension","fixture_route","fixture_target",
                      "#define fixture_target\nreturn 1;\n","extension_truth","1"),
    extension_fixture("Fixture Extension","fixture_action","fixture_target",
                      "#define fixture_target\nreturn 2;\n","project_truth","9"),
    extension_fixture("Fixture Extension","fixture_route","alternate_target",
                      "#define alternate_target\nreturn 3;\n",NULL,NULL),
    extension_fixture("Different Extension","unrelated_action","fixture_target",
                      "#define fixture_target\nreturn 4;\n","unrelated_constant","9"),
    extension_fixture("Fixture Extension","broken_action","fixture_target",
                      "#define fixture_target\nreturn 5;\n","broken_constant","5"),
    extension_fixture("Fixture Extension","embedded_action","embedded_target",
                      "#define helper_target\nreturn 6;\n#define embedded_target\nreturn 37;\n",
                      NULL,NULL)
  };
  int files_ok=1;
  for(int i=0;i<6;i++)
    files_ok &= write_fixture_file(paths[i],&fixtures[i],
                                   i==4 ? fixtures[i].size-3u : SIZE_MAX);
  uint64_t dependency_before=0,dependency_repeat=0,dependency_after=0;
  int dependency_ok=files_ok &&
    gmlc_classic_extension_dependency_hash(dir,123u,&dependency_before) &&
    gmlc_classic_extension_dependency_hash(dir,123u,&dependency_repeat) &&
    dependency_before==dependency_repeat;

  char *extension_names[]={(char*)"Fixture Extension"};
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.extension_names=extension_names;
  manifest.extension_count=1;
  GmlcProject project;
  gmlc_project_init(&project);
  project.prefer_memory_files=1;
  project.scripts=(GmlcScript*)calloc(2,sizeof(*project.scripts));
  project.n_scripts=project.cap_scripts=2;
  if(project.scripts){
    project.scripts[0].name=gmlc_strdup("fixture_target");
    project.scripts[1].name=gmlc_strdup("alternate_target");
  }
  project.constants=(GmlcProjectConstant*)calloc(1,sizeof(*project.constants));
  project.n_constants=project.cap_constants=project.constants ? 1 : 0;
  if(project.constants){
    project.constants[0].name=gmlc_strdup("project_truth");
    project.constants[0].expression=gmlc_strdup("42");
  }
  char err[256]={0};
  int imported=files_ok && project.scripts && project.scripts[0].name && project.scripts[1].name &&
    project.constants && project.constants[0].name && project.constants[0].expression &&
    gmlc_classic_import_extension_aliases(&manifest,&project,dir,err,sizeof(err));
  int found_action=0, found_ambiguous=0, found_unrelated=0, found_broken=0;
  int found_embedded=0, embedded_script=0, found_extension_constant=0;
  int found_project_constant=0, found_unrelated_constant=0, found_broken_constant=0;
  for(int i=0;i<project.n_function_aliases;i++){
    GmlcFunctionAlias *alias=&project.function_aliases[i];
    if(!strcmp(alias->public_name,"fixture_action") &&
       !strcmp(alias->target_name,"fixture_target") && !alias->ambiguous) found_action=1;
    if(!strcmp(alias->public_name,"fixture_route") && alias->ambiguous) found_ambiguous=1;
    if(!strcmp(alias->public_name,"unrelated_action")) found_unrelated=1;
    if(!strcmp(alias->public_name,"broken_action")) found_broken=1;
    if(!strcmp(alias->public_name,"embedded_action") &&
       !strcmp(alias->target_name,"embedded_target") && !alias->ambiguous) found_embedded=1;
  }
  for(int i=0;i<project.n_scripts;i++){
    GmlcScript *script=&project.scripts[i];
    if(!script->name || strcmp(script->name,"embedded_target")) continue;
    char *source=gmlc_project_read_source(&project,script->source_path);
    embedded_script=source && !strcmp(source,"return 37;\n");
    free(source);
  }
  for(int i=0;i<project.n_constants;i++){
    GmlcProjectConstant *constant=&project.constants[i];
    if(!strcmp(constant->name,"extension_truth") &&
       !strcmp(constant->expression,"1")) found_extension_constant=1;
    if(!strcmp(constant->name,"project_truth") &&
       !strcmp(constant->expression,"42")) found_project_constant=1;
    if(!strcmp(constant->name,"unrelated_constant")) found_unrelated_constant=1;
    if(!strcmp(constant->name,"broken_constant")) found_broken_constant=1;
  }
  FILE *changed=fopen(paths[5],"ab");
  int changed_ok=changed && fputc(0x5a,changed)!=EOF;
  if(changed && fclose(changed)!=0) changed_ok=0;
  dependency_ok = dependency_ok && changed_ok &&
    gmlc_classic_extension_dependency_hash(dir,123u,&dependency_after) &&
    dependency_after!=dependency_before;
  int ok=imported && !err[0] && project.n_function_aliases==3 && project.n_scripts==3 &&
         found_action && found_ambiguous && !found_unrelated && !found_broken &&
         found_embedded && embedded_script && found_extension_constant && found_project_constant &&
         !found_unrelated_constant && !found_broken_constant && project.n_constants==2 &&
         dependency_ok;
  if(!ok) fprintf(stderr,"extension alias fixture failed: aliases=%d error=%s\n",
                  project.n_function_aliases,err);
  gmlc_project_free(&project);
  for(int i=0;i<6;i++) remove(paths[i]);
#ifdef _WIN32
  _rmdir(dir);
#else
  rmdir(dir);
#endif
  return ok;
}

static void fixture_double(Fixture *f, double value){
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  for(unsigned i = 0; i < 8; ++i) f->data[f->size++] = (unsigned char)(bits >> (i * 8));
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

static Fixture game_information_fixture(void){
  Fixture f={{0},0};
  fixture_u32(&f,0xFF000018u); fixture_u32(&f,1);
  fixture_string(&f,"Information");
  fixture_u32(&f,(unsigned)-1); fixture_u32(&f,(unsigned)-1);
  fixture_u32(&f,600); fixture_u32(&f,400);
  fixture_u32(&f,1); fixture_u32(&f,1); fixture_u32(&f,0); fixture_u32(&f,1);
  fixture_double(&f,40000.0);
  fixture_string(&f,"{\\rtf1\\ansi\\pard\\qc\\b\\fs32 Generic information\\par\\b0\\fs24 Neutral fixture text\\par}");
  return f;
}

static void fixture_legacy_bmp_image(Fixture *f){
  unsigned char bmp[62] = {0};
  bmp[0]='B'; bmp[1]='M';
  put_u32le(bmp+2,sizeof(bmp)); put_u32le(bmp+10,54); put_u32le(bmp+14,40);
  put_u32le(bmp+18,2); put_u32le(bmp+22,1);
  bmp[26]=1; bmp[28]=24; put_u32le(bmp+34,8);
  /* Bottom-up BGR row: magenta transparency key, then RGB(1,2,3), plus padding. */
  bmp[54]=255; bmp[55]=0; bmp[56]=255;
  bmp[57]=3; bmp[58]=2; bmp[59]=1;
  fixture_u32(f,10);
  fixture_compressed(f,bmp,sizeof(bmp));
}

static Fixture inventory_fixture(unsigned container_version){
  Fixture f = {{0}, 0};
  fixture_u32(&f, GMLC_CLASSIC_MAGIC);
  fixture_u32(&f, container_version);
  fixture_u32(&f, 7);
  fixture_zero(&f, 16);
  fixture_u32(&f, 800); /* settings version */
  unsigned char settings[35 * 4] = {0};
  put_u32le(settings + 4, 1);      /* interpolation */
  put_u32le(settings + 16, 200);   /* fixed two-times scaling */
  put_u32le(settings + 34 * 4, 1); /* Create runs before room-instance code */
  fixture_compressed(&f, settings, sizeof(settings));
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
  if(in.payload_end != f.size || in.last_instance_id != 100123 || in.last_tile_id != 1000456 ||
     in.settings.interpolate != 1 || in.settings.scaling != 200 ||
     in.settings.swap_creation_events != 1)
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
  fixture_u32(&f, 800); fixture_u32(&f, 1);
  { Fixture trigger={{0},0};
    fixture_u32(&trigger,1); fixture_u32(&trigger,800);
    fixture_string(&trigger,"fixture_trigger"); fixture_string(&trigger,"global.ready");
    fixture_u32(&trigger,1); fixture_string(&trigger,"fixture_trigger_constant");
    fixture_compressed(&f,trigger.data,(int)trigger.size); }
  fixture_zero(&f, 8);
  fixture_u32(&f, 800); fixture_u32(&f, 2);
  fixture_string(&f,"fixture_number"); fixture_string(&f,"6*7");
  fixture_string(&f,"fixture_text"); fixture_string(&f,"\"ready\"");
  fixture_zero(&f, 8);
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
  fixture_u32(&f, 800); fixture_u32(&f, 1); /* included files */
  { Fixture included={{0},0}; const unsigned char contents[]={4,5,6,7};
    fixture_zero(&included,8); fixture_u32(&included,800);
    fixture_string(&included,"fixture.dat"); fixture_string(&included,"source/fixture.dat");
    fixture_u32(&included,1); fixture_u32(&included,sizeof(contents)); fixture_u32(&included,1);
    fixture_u32(&included,sizeof(contents)); memcpy(included.data+included.size,contents,sizeof(contents)); included.size+=sizeof(contents);
    fixture_u32(&included,2); fixture_string(&included,"");
    fixture_u32(&included,1); fixture_u32(&included,0); fixture_u32(&included,0);
    fixture_compressed(&f,included.data,(int)included.size); }
  fixture_u32(&f, 700); fixture_u32(&f, 1); /* extensions */
  fixture_string(&f,"fixture_extension");
  fixture_u32(&f, 800); /* game information */
  { Fixture information=game_information_fixture();
    fixture_compressed(&f,information.data,(int)information.size); }
  fixture_u32(&f, 500); fixture_u32(&f, 1); /* library code */
  fixture_string(&f,"global.fixture_started = 1;");
  fixture_u32(&f, 700); fixture_u32(&f, 0); /* executable rooms */
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
           !strcmp(manifest.slots[GMLC_CLASSIC_SCRIPT][0].source, "return 7;") &&
           manifest.trigger_def_count==1 && manifest.trigger_defs[0].exists &&
           !strcmp(manifest.trigger_defs[0].condition,"global.ready") &&
           !strcmp(manifest.trigger_defs[0].constant_name,"fixture_trigger_constant") &&
           manifest.trigger_defs[0].moment==1 &&
           manifest.constant_def_count==2 &&
           !strcmp(manifest.constant_defs[0].name,"fixture_number") &&
           !strcmp(manifest.constant_defs[0].value,"6*7") &&
           manifest.included_file_count==1 && manifest.included_files[0].data_size==4 &&
           !strcmp(manifest.included_files[0].file_name,"fixture.dat") &&
           manifest.included_files[0].export_mode==2 &&
           manifest.extension_count==1 &&
           !strcmp(manifest.extension_names[0],"fixture_extension") &&
           manifest.library_creation_code_count==1 &&
           !strcmp(manifest.library_creation_code[0],"global.fixture_started = 1;");
  GmlcClassicBlob decoded={0};
  Fixture expected_information=game_information_fixture();
  if(ok) ok=gmlc_classic_game_information_decode(&manifest.game_information,&decoded,
                                                  err,sizeof(err));
  if(ok) ok=decoded.size==expected_information.size &&
            !memcmp(decoded.data,expected_information.data,expected_information.size);
  free(decoded.data);
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type)
    if(type != GMLC_CLASSIC_SCRIPT && manifest.existing[type]) ok = 0;
  if(!ok) fprintf(stderr,"manifest assertions failed: %s\n",err);
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

static int build_executable_fixture(Fixture *executable){
  Fixture decoded={{0},0};
  fixture_u32(&decoded,0); /* leading junk */
  fixture_u32(&decoded,1); fixture_u32(&decoded,0x13572468); fixture_zero(&decoded,16);
  fixture_u32(&decoded,700); fixture_u32(&decoded,1); /* extensions */
  fixture_u32(&decoded,700); fixture_string(&decoded,"fixture_executable_extension"); fixture_string(&decoded,"fixture_folder");
  fixture_u32(&decoded,1); fixture_u32(&decoded,700); fixture_string(&decoded,"fixture.bin");
  fixture_u32(&decoded,4); fixture_string(&decoded,"global.fixture_extension_started = 1;"); fixture_string(&decoded,"");
  fixture_u32(&decoded,0); fixture_u32(&decoded,1); /* functions, constants */
  fixture_u32(&decoded,700); fixture_string(&decoded,"fixture_extension_constant"); fixture_string(&decoded,"7*6");
  fixture_u32(&decoded,4); fixture_u32(&decoded,0); 
  fixture_u32(&decoded,800); fixture_u32(&decoded,1); /* triggers */
  { Fixture trigger={{0},0};
    fixture_u32(&trigger,1); fixture_u32(&trigger,800);
    fixture_string(&trigger,"fixture_executable_trigger"); fixture_string(&trigger,"global.ready");
    fixture_u32(&trigger,2); fixture_string(&trigger,"fixture_executable_trigger_constant");
    fixture_compressed(&decoded,trigger.data,(int)trigger.size); }
  fixture_u32(&decoded,800); fixture_u32(&decoded,1); /* constants */
  fixture_string(&decoded,"fixture_executable_constant"); fixture_string(&decoded,"21*2");
  for(unsigned type=0;type<GMLC_CLASSIC_RESOURCE_TYPES;type++){
    fixture_u32(&decoded,800);
    if(type==GMLC_CLASSIC_SCRIPT){
      Fixture script={{0},0};
      fixture_u32(&decoded,1);
      fixture_u32(&script,1);
      fixture_string(&script,"fixture_script");
      fixture_u32(&script,800);
      fixture_string(&script,"exit;");
      fixture_compressed(&decoded,script.data,(int)script.size);
    } else if(type==GMLC_CLASSIC_BACKGROUND){
      Fixture background={{0},0}; const unsigned char bgra[]={3,2,1,255};
      fixture_u32(&decoded,1);
      fixture_u32(&background,1); fixture_string(&background,"fixture_executable_background");
      fixture_u32(&background,800); fixture_u32(&background,800);
      fixture_u32(&background,1); fixture_u32(&background,1); fixture_u32(&background,sizeof(bgra));
      memcpy(background.data+background.size,bgra,sizeof(bgra)); background.size+=sizeof(bgra);
      fixture_compressed(&decoded,background.data,(int)background.size);
    } else if(type==GMLC_CLASSIC_FONT){
      Fixture font={{0},0};
      fixture_u32(&decoded,1);
      fixture_u32(&font,1); fixture_string(&font,"fixture_compiled_font"); fixture_u32(&font,800);
      fixture_string(&font,"fixture face"); fixture_u32(&font,10); fixture_u32(&font,1);
      fixture_u32(&font,0); fixture_u32(&font,65); fixture_u32(&font,66);
      for(unsigned entry=0;entry<256u*6u;entry++){
        unsigned value=0;
        if(entry==65u*6u+2u || entry==65u*6u+3u) value=1;
        else if(entry==65u*6u+4u) value=3;
        else if(entry==66u*6u) value=1;
        else if(entry==66u*6u+2u || entry==66u*6u+3u) value=1;
        else if(entry==66u*6u+4u) value=4;
        else if(entry==66u*6u+5u) value=(unsigned)-1;
        fixture_u32(&font,value);
      }
      fixture_u32(&font,2); fixture_u32(&font,1); fixture_u32(&font,2);
      font.data[font.size++]=17; font.data[font.size++]=231;
      fixture_compressed(&decoded,font.data,(int)font.size);
    } else if(type==GMLC_CLASSIC_ROOM){
      Fixture room={{0},0};
      fixture_u32(&decoded,1);
      fixture_u32(&room,1); fixture_string(&room,"fixture_executable_room"); fixture_u32(&room,800);
      fixture_string(&room,""); fixture_u32(&room,320); fixture_u32(&room,240);
      fixture_u32(&room,30); fixture_u32(&room,0); fixture_u32(&room,0); fixture_u32(&room,1);
      fixture_string(&room,""); fixture_u32(&room,0);
      fixture_u32(&room,0); fixture_u32(&room,0);
      fixture_u32(&room,1); fixture_u32(&room,10); fixture_u32(&room,20);
      fixture_u32(&room,0); fixture_u32(&room,100001); fixture_u32(&room,1);
      fixture_u32(&room,0);
      fixture_compressed(&decoded,room.data,(int)room.size);
    } else fixture_u32(&decoded,0);
  }
  fixture_u32(&decoded,100000); fixture_u32(&decoded,1000000);
  fixture_u32(&decoded,800); fixture_u32(&decoded,1); /* includes */
  { Fixture included={{0},0}; const unsigned char contents[]={10,11};
    fixture_u32(&included,800); fixture_string(&included,"executable.dat"); fixture_string(&included,"");
    fixture_u32(&included,1); fixture_u32(&included,sizeof(contents)); fixture_u32(&included,1);
    fixture_u32(&included,sizeof(contents)); memcpy(included.data+included.size,contents,sizeof(contents)); included.size+=sizeof(contents);
    fixture_u32(&included,1); fixture_string(&included,"");
    fixture_u32(&included,1); fixture_u32(&included,0); fixture_u32(&included,1);
    fixture_compressed(&decoded,included.data,(int)included.size); }
  fixture_u32(&decoded,800); /* help */
  { Fixture information=game_information_fixture();
    fixture_u32(&decoded,(unsigned)information.size);
    memcpy(decoded.data+decoded.size,information.data,information.size);
    decoded.size+=information.size; }
  fixture_u32(&decoded,500); fixture_u32(&decoded,1); /* library code */
  fixture_string(&decoded,"global.fixture_executable_started = 1;");
  fixture_u32(&decoded,700); fixture_u32(&decoded,1); fixture_u32(&decoded,0); /* room order */

  memset(executable,0,sizeof(*executable));
  executable->data[0]='M'; executable->data[1]='Z'; executable->size=16;
  fixture_u32(executable,20); /* embedded-data self pointer */
  fixture_u32(executable,GMLC_CLASSIC_MAGIC); fixture_u32(executable,800);
  fixture_u32(executable,0); fixture_u32(executable,800);
  fixture_u32(executable,0); /* settings */
  fixture_u32(executable,0); fixture_u32(executable,0); /* wrapper strings */
  fixture_u32(executable,0); fixture_u32(executable,0); /* junk counts */
  unsigned char table[256];
  for(unsigned i=0;i<256;i++){
    table[i]=(unsigned char)(i*73u+41u);
    executable->data[executable->size++]=table[i];
  }
  fixture_u32(executable,(unsigned)decoded.size);
  unsigned char encoded_data[sizeof(decoded.data)];
  memcpy(encoded_data,decoded.data,decoded.size);
  for(size_t i=0;i<decoded.size;i++){
    size_t offset=table[i&255u];
    size_t other=i>offset?i-offset:0;
    unsigned char swap=encoded_data[i]; encoded_data[i]=encoded_data[other]; encoded_data[other]=swap;
  }
  unsigned char previous=encoded_data[0];
  executable->data[executable->size++]=previous;
  for(size_t i=1;i<decoded.size;i++){
    previous=table[(unsigned char)(encoded_data[i]+previous+(unsigned char)i)];
    executable->data[executable->size++]=previous;
  }
  return 1;
}

static int expect_executable_manifest_variant(const Fixture *executable){
  GmlcClassicManifest manifest;
  char err[256]={0};
  int ok=gmlc_classic_manifest(executable->data,executable->size,&manifest,err,sizeof(err));
  if(!ok) fprintf(stderr,"executable manifest failed: %s\n",err);
  if(ok){
    ok=manifest.inventory.header.version==GMLC_CLASSIC_GM8 &&
       manifest.inventory.header.game_id==0x13572468 && manifest.room_order_count==1 &&
       manifest.room_order[0]==0 &&
       manifest.extension_count==1 && !strcmp(manifest.extension_names[0],"fixture_executable_extension") &&
       manifest.existing[GMLC_CLASSIC_SCRIPT]==1 &&
       manifest.existing[GMLC_CLASSIC_BACKGROUND]==1 &&
       manifest.existing[GMLC_CLASSIC_FONT]==1 &&
       manifest.existing[GMLC_CLASSIC_ROOM]==1 &&
       !strcmp(manifest.slots[GMLC_CLASSIC_SCRIPT][0].source,"exit;") &&
       manifest.trigger_def_count==1 && manifest.constant_def_count==2 &&
       !strcmp(manifest.constant_defs[0].value,"7*6") && !strcmp(manifest.constant_defs[1].value,"21*2") &&
       manifest.included_file_count==1 && manifest.included_files[0].data_size==2 &&
       !strcmp(manifest.included_files[0].file_name,"executable.dat") &&
       manifest.library_creation_code_count==2 && manifest.game_information.size>0;
    if(ok){
      GmlcClassicBlob information={0};
      Fixture expected=game_information_fixture();
      ok=gmlc_classic_game_information_decode(&manifest.game_information,&information,
                                               err,sizeof(err)) &&
         information.size==expected.size &&
         !memcmp(information.data,expected.data,expected.size);
      free(information.data);
    }
    if(ok){
      GmlcProject project;
      gmlc_project_init(&project);
      project.prefer_memory_files=1;
      ok=gmlc_classic_import_fonts(&manifest,&project,"tmp",err,sizeof(err));
      if(ok) ok=project.n_fonts==1 && project.fonts[0].n_glyphs==2 &&
        project.fonts[0].em_size==1 && project.fonts[0].glyphs[0].ch==65 &&
        project.fonts[0].glyphs[0].x==0 && project.fonts[0].glyphs[0].w==1 &&
        project.fonts[0].glyphs[0].shift==3 && project.fonts[0].glyphs[0].offset==0 &&
        project.fonts[0].glyphs[1].ch==66 && project.fonts[0].glyphs[1].x==1 &&
        project.fonts[0].glyphs[1].shift==4 && project.fonts[0].glyphs[1].offset==-1 &&
        project.n_memory_files==1 && project.memory_files[0].kind==GMLC_MEMORY_RGBA &&
        project.memory_files[0].width==2 && project.memory_files[0].height==1 &&
        project.memory_files[0].size==8 && project.memory_files[0].data[3]==17 &&
        project.memory_files[0].data[7]==231;
      gmlc_project_free(&project);
    }
    gmlc_classic_manifest_free(&manifest);
  }
  if(!ok) fprintf(stderr,"executable manifest assertions failed: %s\n",err);
  return ok;
}

static int expect_executable_manifest(void){
  Fixture executable;
  if(!build_executable_fixture(&executable) || !expect_executable_manifest_variant(&executable)) return 0;
  /* The direct-header wrapper omits the optional self-offset word. */
  memmove(executable.data+16,executable.data+20,executable.size-20);
  executable.size-=4;
  return expect_executable_manifest_variant(&executable);
}



static int expect_legacy_executable_manifest(void){
  Fixture executable;
  if(!build_legacy_executable_fixture(&executable)) return 0;
  GmlcClassicManifest manifest={0}; char err[256]={0};
  int ok=gmlc_classic_manifest(executable.data,executable.size,&manifest,err,sizeof(err));
  if(!ok) fprintf(stderr,"legacy executable manifest failed: %s\n",err);
  if(ok){
    ok=manifest.inventory.header.version==GMLC_CLASSIC_GM7 &&
       manifest.inventory.header.game_id==0x24681357 && manifest.inventory.settings_version==702 &&
       manifest.inventory.settings.interpolate==1 && manifest.inventory.settings.scaling==150 &&
       manifest.existing[GMLC_CLASSIC_SPRITE]==1 &&
       manifest.existing[GMLC_CLASSIC_BACKGROUND]==1 &&
       manifest.existing[GMLC_CLASSIC_SCRIPT]==1 && manifest.existing[GMLC_CLASSIC_ROOM]==1 &&
       !strcmp(manifest.slots[GMLC_CLASSIC_SCRIPT][0].source,"return 42;") &&
       manifest.room_order_count==1 && manifest.room_order[0]==0 &&
       manifest.library_creation_code_count==1;
    if(!ok) fprintf(stderr,"legacy executable manifest values were not preserved\n");
  }
  if(ok){
    GmlcProject project; gmlc_project_init(&project); project.prefer_memory_files=1;
    ok=gmlc_classic_import_sprites(&manifest,&project,"tmp",err,sizeof(err)) &&
       gmlc_classic_import_backgrounds(&manifest,&project,"tmp",err,sizeof(err)) &&
       gmlc_classic_import_rooms(&manifest,&project,"tmp",err,sizeof(err));
    if(!ok) fprintf(stderr,"legacy executable import failed: %s\n",err);
    if(ok) ok=project.n_sprites==2 && project.n_memory_files>=2 &&
      project.memory_files[0].kind==GMLC_MEMORY_RGBA && project.memory_files[0].size==8 &&
      project.memory_files[0].data[0]==1 && project.memory_files[0].data[1]==2 &&
      project.memory_files[0].data[2]==3 && project.memory_files[0].data[7]==0 &&
      project.memory_files[1].data[0]==11 && project.memory_files[1].data[2]==13 &&
      project.n_rooms==1 && project.rooms[0].width==320 && project.rooms[0].height==240 &&
      project.rooms[0].speed==60 && project.rooms[0].background_color==0xff112233u &&
      project.rooms[0].n_instances==1 && project.rooms[0].instances[0].instance_id==100001 &&
      project.rooms[0].n_tiles==1 && project.rooms[0].tiles[0].tile_id==1000001;
    if(!ok) fprintf(stderr,"legacy executable imported values were not preserved: sprites=%d files=%d "
                           "rooms=%d size=%dx%d speed=%d colour=%08x instances=%d instance=%d "
                           "tiles=%d tile=%d\n",
                           project.n_sprites,project.n_memory_files,project.n_rooms,
                           project.n_rooms?project.rooms[0].width:0,
                           project.n_rooms?project.rooms[0].height:0,
                           project.n_rooms?project.rooms[0].speed:0,
                           project.n_rooms?project.rooms[0].background_color:0,
                           project.n_rooms?project.rooms[0].n_instances:0,
                           project.n_rooms&&project.rooms[0].n_instances?project.rooms[0].instances[0].instance_id:0,
                           project.n_rooms?project.rooms[0].n_tiles:0,
                           project.n_rooms&&project.rooms[0].n_tiles?project.rooms[0].tiles[0].tile_id:0);
    gmlc_project_free(&project);
  }
  gmlc_classic_manifest_free(&manifest);
  return ok;
}

static void fixture_legacy_room(Fixture *f, const char *name){
  fixture_u32(f,1); fixture_string(f,name); fixture_u32(f,541);
  fixture_string(f,"");
  fixture_u32(f,320); fixture_u32(f,240); fixture_u32(f,16); fixture_u32(f,16);
  fixture_u32(f,0); fixture_u32(f,30); fixture_u32(f,0); fixture_u32(f,0); fixture_u32(f,1);
  fixture_string(f,"");
  fixture_u32(f,0); /* backgrounds */
  fixture_u32(f,0); fixture_u32(f,0); /* views enabled/count */
  fixture_u32(f,0); /* instances */
  fixture_u32(f,0); /* tiles */
  for(unsigned i=0;i<14;i++) fixture_u32(f,0);
}

static Fixture legacy_fixture_variant(unsigned container_version, int sparse_rooms){
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
    if(sparse_rooms && type==GMLC_CLASSIC_ROOM){
      fixture_u32(&f,8);
      fixture_u32(&f,0); fixture_u32(&f,0);
      fixture_legacy_room(&f,"resource_room_two");
      fixture_u32(&f,0); fixture_u32(&f,0); fixture_u32(&f,0);
      fixture_legacy_room(&f,"resource_room_six");
      fixture_u32(&f,0);
    } else fixture_u32(&f, 0);
  }
  fixture_u32(&f, 100000); fixture_u32(&f, 1000000);
  if(sparse_rooms){
    if(gm7){
      fixture_u32(&f,620); fixture_u32(&f,0); /* included files */
      fixture_u32(&f,700); fixture_u32(&f,0); /* extensions */
    }
    fixture_u32(&f,600); fixture_u32(&f,0x00ffffff); fixture_u32(&f,1);
    fixture_string(&f,"Game Information");
    fixture_u32(&f,(unsigned)-1); fixture_u32(&f,(unsigned)-1);
    fixture_u32(&f,600); fixture_u32(&f,400);
    fixture_u32(&f,1); fixture_u32(&f,1); fixture_u32(&f,0); fixture_u32(&f,1);
    fixture_string(&f,"");
    fixture_u32(&f,500); fixture_u32(&f,0); /* library code */
    fixture_u32(&f,700); fixture_u32(&f,2); /* explicit room order */
    fixture_u32(&f,6); fixture_u32(&f,2);
  }
  return f;
}

static Fixture legacy_fixture(unsigned container_version){
  return legacy_fixture_variant(container_version,0);
}

static int build_project_fixture(unsigned version, Fixture *out){
  if(version==600){ *out=legacy_fixture(version); return 1; }
  if(version==701 || version==702){
    Fixture plain=legacy_fixture(version);
    size_t encoded_size=0;
    unsigned char *encoded=encode_gm7(plain.data,plain.size,&encoded_size);
    if(!encoded || encoded_size>sizeof(out->data)){ free(encoded); return 0; }
    memset(out,0,sizeof(*out));
    memcpy(out->data,encoded,encoded_size);
    out->size=encoded_size;
    free(encoded);
    return 1;
  }
  if(version==800 || version==810){ *out=manifest_fixture(version); return 1; }
  return 0;
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

static int expect_script_import(void){
  Fixture fixture = manifest_fixture(800);
  GmlcClassicManifest manifest;
  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128] = "tmp/classic_script_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  if(!gmlc_classic_manifest(fixture.data, fixture.size, &manifest, err, sizeof(err))){
    fprintf(stderr, "script import manifest failed: %s\n", err);
    return 0;
  }
  int ok = gmlc_classic_import_scripts(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "script import failed: %s\n", err);
  if(ok){
    FILE *file = fopen(project.scripts[0].source_path, "rb");
    char text[32] = {0};
    size_t got = file ? fread(text, 1, sizeof(text) - 1, file) : 0;
    if(file) fclose(file);
    ok = project.n_scripts == 1 && project.n_script_order == 1 && got == 9 &&
         !strcmp(project.scripts[0].name, "resource_script") && !strcmp(text, "return 7;");
    remove(project.scripts[0].source_path);
  }
  for(int i = 0; i < project.n_scripts; ++i){
    free(project.scripts[i].id); free(project.scripts[i].name); free(project.scripts[i].source_path);
  }
  free(project.scripts);
  for(int i = 0; i < project.n_script_order; ++i) free(project.script_order_ids[i]);
  free(project.script_order_ids);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_sprite_import(int executable_layout){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_SPRITE] = 1;
  manifest.existing[GMLC_CLASSIC_SPRITE] = 1;
  manifest.slots[GMLC_CLASSIC_SPRITE] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_SPRITE]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_SPRITE][0];
  slot->exists = 1;
  slot->version = 800;
  slot->executable_layout = executable_layout;
  slot->name = strdup("resource_sprite");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, 1); fixture_u32(&payload, 2); fixture_u32(&payload, 1);
  fixture_u32(&payload, 800); fixture_u32(&payload, 2); fixture_u32(&payload, 1);
  const unsigned char bgra[8] = {3, 2, 1, 255, 6, 5, 4, 128};
  fixture_u32(&payload, sizeof(bgra));
  memcpy(payload.data + payload.size, bgra, sizeof(bgra)); payload.size += sizeof(bgra);
  if(executable_layout){
    fixture_u32(&payload, 0);
    fixture_u32(&payload, 800); fixture_u32(&payload, 2); fixture_u32(&payload, 1);
    fixture_u32(&payload, 0); fixture_u32(&payload, 1);
    fixture_u32(&payload, 0); fixture_u32(&payload, 0);
    fixture_u32(&payload, 1); fixture_u32(&payload, 1);
  } else {
    fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
    fixture_u32(&payload, 0); fixture_u32(&payload, 1); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  }
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128];
  snprintf(dir,sizeof(dir),"tmp/classic_sprite_%s_fixture",executable_layout?"executable":"project");
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_sprites(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "sprite import failed: %s\n", err);
  if(ok){
    struct stat st;
    int width=0,height=0,components=0;
    unsigned char *rgba=NULL;
    if(project.n_sprites==1 && project.sprites[0].frame_paths)
      rgba=stbi_load(project.sprites[0].frame_paths[0],&width,&height,&components,4);
    ok = project.n_sprites == 1 && project.sprites[0].frame_paths &&
         project.sprites[0].runtime_id == 0 &&
         project.sprites[0].width == 2 && project.sprites[0].height == 1 &&
         project.sprites[0].xorig == 1 && project.sprites[0].yorig == 2 &&
         project.sprites[0].bbox_right == 1 &&
         !stat(project.sprites[0].frame_paths[0], &st) && st.st_size > 0 &&
         rgba && width==2 && height==1 &&
         rgba[0]==1 && rgba[1]==2 && rgba[2]==3 && rgba[3]==255 &&
         rgba[4]==4 && rgba[5]==5 && rgba[6]==6 && rgba[7]==128;
    stbi_image_free(rgba);
    remove(project.sprites[0].frame_paths[0]);
  }
  for(int i = 0; i < project.n_sprites; ++i){
    free(project.sprites[i].id); free(project.sprites[i].name);
    for(int frame = 0; frame < project.sprites[i].n_frames; ++frame) free(project.sprites[i].frame_paths[frame]);
    free(project.sprites[i].frame_paths);
  }
  free(project.sprites);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_sound_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_SOUND] = 1;
  manifest.existing[GMLC_CLASSIC_SOUND] = 1;
  manifest.slots[GMLC_CLASSIC_SOUND] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_SOUND]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_SOUND][0];
  slot->exists = 1; slot->name = strdup("resource_sound");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, 0); fixture_string(&payload, ".wav"); fixture_string(&payload, "fixture.wav");
  fixture_u32(&payload, 1);
  const unsigned char audio[] = {'R','I','F','F'};
  fixture_u32(&payload, sizeof(audio)); memcpy(payload.data + payload.size, audio, sizeof(audio)); payload.size += sizeof(audio);
  fixture_u32(&payload, 0); fixture_double(&payload, 0.5); fixture_double(&payload, 0.0); fixture_u32(&payload, 1);
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128] = "tmp/classic_sound_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_sounds(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "sound import failed: %s\n", err);
  if(ok){
    FILE *file = fopen(project.sounds[0].data_path, "rb");
    unsigned char got[4] = {0};
    size_t count = file ? fread(got, 1, sizeof(got), file) : 0;
    if(file) fclose(file);
    ok = project.n_sounds == 1 && count == sizeof(got) && !memcmp(got, audio, sizeof(audio)) &&
         project.sounds[0].volume > 0.49f && project.sounds[0].volume < 0.51f;
    remove(project.sounds[0].data_path);
  }
  for(int i = 0; i < project.n_sounds; ++i){
    free(project.sounds[i].id); free(project.sounds[i].name); free(project.sounds[i].data_path);
  }
  free(project.sounds);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int setup_legacy_slot(GmlcClassicManifest *manifest, GmlcClassicResourceType type,
                             const char *name, const Fixture *payload){
  memset(manifest,0,sizeof(*manifest));
  manifest->inventory.resource_slots[type]=1;
  manifest->existing[type]=1;
  manifest->slots[type]=(GmlcClassicResourceSlot*)calloc(1,sizeof(GmlcClassicResourceSlot));
  if(!manifest->slots[type]) return 0;
  GmlcClassicResourceSlot *slot=&manifest->slots[type][0];
  slot->exists=1; slot->legacy_layout=1; slot->version=600; slot->name=strdup(name);
  slot->payload=(uint8_t*)malloc(payload->size); slot->payload_size=payload->size;
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(manifest); return 0; }
  memcpy(slot->payload,payload->data,payload->size);
  return 1;
}

static int expect_legacy_media_import(void){
  char err[256],dir[128]="tmp/classic_legacy_media_fixture";
  err[0]='\0';
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir,0777);
#endif
  int ok=1;
  {
    Fixture payload={{0},0};
    fixture_u32(&payload,0); fixture_string(&payload,".wav"); fixture_string(&payload,"fixture.wav");
    fixture_u32(&payload,1); const unsigned char raw[]={'R','I','F','F'}; fixture_compressed(&payload,raw,sizeof(raw));
    fixture_u32(&payload,0); fixture_double(&payload,0.75); fixture_double(&payload,0); fixture_u32(&payload,1);
    GmlcClassicManifest manifest; GmlcProject project; memset(&project,0,sizeof(project));
    ok=setup_legacy_slot(&manifest,GMLC_CLASSIC_SOUND,"legacy_sound",&payload) &&
       gmlc_classic_import_sounds(&manifest,&project,dir,err,sizeof(err));
    if(ok){ unsigned char got[4]={0}; FILE *file=fopen(project.sounds[0].data_path,"rb");
      size_t count=file?fread(got,1,4,file):0; if(file) fclose(file);
      ok=count==4 && !memcmp(got,raw,4) && project.sounds[0].volume>0.74f;
      remove(project.sounds[0].data_path);
    }
    for(int i=0;i<project.n_sounds;i++){ free(project.sounds[i].id); free(project.sounds[i].name); free(project.sounds[i].data_path); }
    free(project.sounds); gmlc_classic_manifest_free(&manifest);
  }
  {
    Fixture payload={{0},0};
    const unsigned fields[13]={2,1,0,1,0,0,1,0,1,0,1,1,0};
    for(int i=0;i<13;i++) fixture_u32(&payload,fields[i]);
    fixture_u32(&payload,1); fixture_legacy_bmp_image(&payload);
    GmlcClassicManifest manifest; GmlcProject project; memset(&project,0,sizeof(project));
    int stage=setup_legacy_slot(&manifest,GMLC_CLASSIC_SPRITE,"legacy_sprite",&payload) &&
      gmlc_classic_import_sprites(&manifest,&project,dir,err,sizeof(err));
    if(stage){ struct stat st; stage=project.n_sprites==1 && project.sprites[0].width==2 &&
      project.sprites[0].xorig==1 && !stat(project.sprites[0].frame_paths[0],&st) && st.st_size>0;
      remove(project.sprites[0].frame_paths[0]); }
    ok=ok&&stage;
    for(int i=0;i<project.n_sprites;i++){ free(project.sprites[i].id); free(project.sprites[i].name);
      for(int f=0;f<project.sprites[i].n_frames;f++) free(project.sprites[i].frame_paths[f]);
      free(project.sprites[i].frame_paths); }
    free(project.sprites); gmlc_classic_manifest_free(&manifest);
  }
  {
    Fixture payload={{0},0};
    const unsigned fields[12]={2,1,1,0,1,1,1,1,0,0,0,0};
    for(int i=0;i<12;i++) fixture_u32(&payload,fields[i]);
    fixture_u32(&payload,1); fixture_legacy_bmp_image(&payload);
    GmlcClassicManifest manifest; GmlcProject project; memset(&project,0,sizeof(project));
    int stage=setup_legacy_slot(&manifest,GMLC_CLASSIC_BACKGROUND,"legacy_background",&payload) &&
      gmlc_classic_import_backgrounds(&manifest,&project,dir,err,sizeof(err));
    if(stage){ struct stat st; stage=project.n_tilesets==1 && project.n_sprites==1 &&
      project.tilesets[0].tile_width==1 && !stat(project.sprites[0].frame_paths[0],&st) && st.st_size>0;
      remove(project.sprites[0].frame_paths[0]); }
    ok=ok&&stage;
    for(int i=0;i<project.n_sprites;i++){ free(project.sprites[i].id); free(project.sprites[i].name);
      for(int f=0;f<project.sprites[i].n_frames;f++) free(project.sprites[i].frame_paths[f]);
      free(project.sprites[i].frame_paths); }
    for(int i=0;i<project.n_tilesets;i++){ free(project.tilesets[i].id); free(project.tilesets[i].name); }
    free(project.sprites); free(project.tilesets); gmlc_classic_manifest_free(&manifest);
  }
#ifndef _WIN32
  rmdir(dir);
#endif
  if(!ok) fprintf(stderr,"legacy media import failed: %s\n",err);
  return ok;
}

static int expect_background_import(int executable_layout){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_BACKGROUND] = 1;
  manifest.existing[GMLC_CLASSIC_BACKGROUND] = 1;
  manifest.slots[GMLC_CLASSIC_BACKGROUND] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_BACKGROUND]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_BACKGROUND][0];
  slot->exists = 1; slot->name = strdup("resource_background");
  slot->version = 800; slot->executable_layout = executable_layout;
  Fixture payload = {{0}, 0};
  if(!executable_layout){
    fixture_u32(&payload, 1); fixture_u32(&payload, 1); fixture_u32(&payload, 1);
    fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  }
  fixture_u32(&payload, 800); fixture_u32(&payload, 1); fixture_u32(&payload, 1);
  fixture_u32(&payload, 4);
  payload.data[payload.size++] = 3; payload.data[payload.size++] = 2;
  payload.data[payload.size++] = 1; payload.data[payload.size++] = 255;
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128];
  snprintf(dir,sizeof(dir),"tmp/classic_background_%s_fixture",executable_layout?"executable":"project");
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_backgrounds(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "background import failed: %s\n", err);
  if(ok){
    int width=0,height=0,components=0;
    unsigned char *rgba=NULL;
    if(project.n_sprites==1 && project.sprites[0].frame_paths)
      rgba=stbi_load(project.sprites[0].frame_paths[0],&width,&height,&components,4);
    ok = project.n_sprites == 1 && project.sprites[0].frame_paths &&
         project.sprites[0].runtime_id == -1 &&
         project.n_tilesets == 1 && project.tilesets[0].sprite_id == 0 &&
         rgba && width==1 && height==1 &&
         rgba[0]==1 && rgba[1]==2 && rgba[2]==3 && rgba[3]==255;
    stbi_image_free(rgba);
  }
  if(project.n_sprites){
    remove(project.sprites[0].frame_paths[0]);
    free(project.sprites[0].frame_paths[0]); free(project.sprites[0].frame_paths);
    free(project.sprites[0].id); free(project.sprites[0].name);
  }
  free(project.sprites);
  if(project.n_tilesets){ free(project.tilesets[0].id); free(project.tilesets[0].name); }
  free(project.tilesets);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int fixture_font_slot(GmlcClassicResourceSlot *slot, const char *name,
                             unsigned size, unsigned bold, unsigned italic,
                             unsigned first, unsigned last){
  slot->exists=1; slot->name=strdup(name);
  Fixture payload={{0},0};
  fixture_string(&payload,"sans"); fixture_u32(&payload,size); fixture_u32(&payload,bold);
  fixture_u32(&payload,italic); fixture_u32(&payload,first); fixture_u32(&payload,last);
  slot->payload=(uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload) return 0;
  memcpy(slot->payload,payload.data,payload.size); slot->payload_size=payload.size;
  return 1;
}

static void free_font_fixture_project(GmlcProject *project){
  for(int i=0;i<project->n_fonts;i++){
    const char *path=project->fonts[i].png_path;
    if(path && strncmp(path,"gmlc-memory://",14)) remove(path);
  }
  gmlc_project_free(project);
}

static int expect_sparse_font_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_FONT]=5;
  manifest.existing[GMLC_CLASSIC_FONT]=2;
  manifest.slots[GMLC_CLASSIC_FONT]=(GmlcClassicResourceSlot*)calloc(5,sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_FONT]) return 0;
  if(!fixture_font_slot(&manifest.slots[GMLC_CLASSIC_FONT][1],"font_first",12,0,0,32,127) ||
     !fixture_font_slot(&manifest.slots[GMLC_CLASSIC_FONT][4],"font_second",8,1,1,65,90)){
    gmlc_classic_manifest_free(&manifest); return 0;
  }
  GmlcProject project; memset(&project,0,sizeof(project));
  char err[256],dir[128]="tmp/classic_font_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir,0777);
#endif
  int ok=gmlc_classic_import_fonts(&manifest,&project,dir,err,sizeof(err));
  if(!ok) fprintf(stderr,"font import failed: %s\n",err);
  if(ok) ok=project.n_fonts==2 && project.cap_fonts==2 &&
    !strcmp(project.fonts[0].name,"font_first") && project.fonts[0].n_glyphs==96 &&
    project.fonts[0].glyphs[0].ch==32 && project.fonts[0].em_size==18 &&
    !strcmp(project.fonts[1].name,"font_second") && project.fonts[1].n_glyphs==26 &&
    project.fonts[1].glyphs[0].ch==65 && project.fonts[1].em_size==12 &&
    project.fonts[1].glyphs[0].h==10 && project.fonts[1].glyphs[0].shift==8 &&
    project.fonts[0].png_path;
  free_font_fixture_project(&project); gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_empty_font_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_FONT]=1520;
  manifest.slots[GMLC_CLASSIC_FONT]=(GmlcClassicResourceSlot*)calloc(1520,sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_FONT]) return 0;
  GmlcProject project; memset(&project,0,sizeof(project));
  char err[256];
  int ok=gmlc_classic_import_fonts(&manifest,&project,"tmp/classic_empty_font_fixture",err,sizeof(err));
  if(!ok) fprintf(stderr,"empty font import failed: %s\n",err);
  if(ok) ok=project.n_fonts==0 && project.cap_fonts==0 && project.fonts==NULL;
  free_font_fixture_project(&project); gmlc_classic_manifest_free(&manifest);
  return ok;
}

static int expect_gm81_font_metadata(void){
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.inventory.header.version=GMLC_CLASSIC_GM81;
  manifest.inventory.resource_slots[GMLC_CLASSIC_FONT]=1;
  manifest.existing[GMLC_CLASSIC_FONT]=1;
  manifest.slots[GMLC_CLASSIC_FONT]=(GmlcClassicResourceSlot*)calloc(1,sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_FONT]) return 0;
  unsigned packed_first=(3u<<24)|(177u<<16)|33u;
  if(!fixture_font_slot(&manifest.slots[GMLC_CLASSIC_FONT][0],"font_packed",12,0,0,
                        packed_first,33)){
    gmlc_classic_manifest_free(&manifest);
    return 0;
  }
  GmlcProject project;
  memset(&project,0,sizeof(project));
  project.prefer_memory_files=1;
  char err[256];
  int ok=gmlc_classic_import_fonts(&manifest,&project,"tmp/classic_font_metadata_fixture",
                                   err,sizeof(err));
  if(!ok) fprintf(stderr,"packed font import failed: %s\n",err);
  if(ok) ok=project.n_fonts==1 && project.fonts[0].n_glyphs==1 &&
    project.fonts[0].glyphs[0].ch==33 && project.fonts[0].glyphs[0].h==14 &&
    project.fonts[0].em_size==18 && project.n_memory_files==1 &&
    project.memory_files[0].kind==GMLC_MEMORY_RGBA;
  if(!ok && project.n_fonts>0)
    fprintf(stderr,"packed font assertion: fonts=%d glyphs=%d ch=%d h=%d em=%u files=%d kind=%d\n",
            project.n_fonts,project.fonts[0].n_glyphs,
            project.fonts[0].n_glyphs?project.fonts[0].glyphs[0].ch:-1,
            project.fonts[0].n_glyphs?project.fonts[0].glyphs[0].h:-1,
            project.fonts[0].em_size,project.n_memory_files,
            project.n_memory_files?(int)project.memory_files[0].kind:-1);
  free_font_fixture_project(&project);
  gmlc_classic_manifest_free(&manifest);
  return ok;
}

static int expect_path_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_PATH] = 1;
  manifest.existing[GMLC_CLASSIC_PATH] = 1;
  manifest.slots[GMLC_CLASSIC_PATH] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_PATH]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_PATH][0];
  slot->exists = 1; slot->name = strdup("resource_path");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, 1); fixture_u32(&payload, 1); fixture_u32(&payload, 4);
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 16); fixture_u32(&payload, 16);
  fixture_u32(&payload, 2);
  fixture_double(&payload, 1.5); fixture_double(&payload, 2.5); fixture_double(&payload, 100.0);
  fixture_double(&payload, 9.5); fixture_double(&payload, 8.5); fixture_double(&payload, 50.0);
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;
  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256];
  int ok = gmlc_classic_import_paths(&manifest, &project, err, sizeof(err));
  if(!ok) fprintf(stderr, "path import failed: %s\n", err);
  if(ok) ok = project.n_paths == 1 && project.paths[0].kind == 1 && project.paths[0].closed &&
              project.paths[0].precision == 4 && project.paths[0].n_points == 2 &&
              project.paths[0].points[1].x > 9.49f && project.paths[0].points[1].speed == 50.0f;
  for(int i = 0; i < project.n_paths; ++i){
    free(project.paths[i].id); free(project.paths[i].name); free(project.paths[i].points);
  }
  free(project.paths);
  gmlc_classic_manifest_free(&manifest);
  return ok;
}

static int expect_timeline_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest,0,sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_TIMELINE]=1;
  manifest.existing[GMLC_CLASSIC_TIMELINE]=1;
  manifest.slots[GMLC_CLASSIC_TIMELINE]=(GmlcClassicResourceSlot*)calloc(1,sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_TIMELINE]) return 0;
  GmlcClassicResourceSlot *slot=&manifest.slots[GMLC_CLASSIC_TIMELINE][0];
  slot->exists=1; slot->name=strdup("resource_timeline");
  Fixture payload={{0},0};
  fixture_u32(&payload,1);  /* one moment */
  fixture_u32(&payload,12); /* moment position */
  fixture_u32(&payload,400); fixture_u32(&payload,1); /* action-list version/count */
  fixture_u32(&payload,440); fixture_u32(&payload,1); fixture_u32(&payload,603);
  fixture_u32(&payload,7); fixture_u32(&payload,0); fixture_u32(&payload,0);
  fixture_u32(&payload,0); fixture_u32(&payload,2);
  fixture_string(&payload,""); fixture_string(&payload,"");
  fixture_u32(&payload,1); fixture_u32(&payload,8);
  for(int i=0;i<8;i++) fixture_u32(&payload,0);
  fixture_u32(&payload,(unsigned)-1); fixture_u32(&payload,0); fixture_u32(&payload,8);
  fixture_string(&payload,"x += 2;");
  for(int i=1;i<8;i++) fixture_string(&payload,"");
  fixture_u32(&payload,0);
  slot->payload=(uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload,payload.data,payload.size); slot->payload_size=payload.size;

  GmlcProject project; memset(&project,0,sizeof(project));
  char err[256],dir[128]="tmp/classic_timeline_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir,0777);
#endif
  int ok=gmlc_classic_import_timelines(&manifest,&project,dir,err,sizeof(err));
  if(!ok) fprintf(stderr,"timeline import failed: %s\n",err);
  if(ok){
    char source[64]={0};
    FILE *file=fopen(project.timelines[0].moments[0].source_path,"rb");
    size_t got=file?fread(source,1,sizeof(source)-1,file):0;
    if(file) fclose(file);
    ok=project.n_timelines==1 && project.timelines[0].n_moments==1 &&
       project.timelines[0].moments[0].step==12 && got && strstr(source,"x += 2;");
    remove(project.timelines[0].moments[0].source_path);
  }
  for(int i=0;i<project.n_timelines;i++){
    free(project.timelines[i].id); free(project.timelines[i].name);
    for(int m=0;m<project.timelines[i].n_moments;m++) free(project.timelines[i].moments[m].source_path);
    free(project.timelines[i].moments);
  }
  free(project.timelines); gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_object_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_OBJECT] = 1;
  manifest.existing[GMLC_CLASSIC_OBJECT] = 1;
  manifest.slots[GMLC_CLASSIC_OBJECT] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_OBJECT]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_OBJECT][0];
  slot->exists = 1; slot->name = strdup("resource_object");
  Fixture payload = {{0}, 0};
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 0); fixture_u32(&payload, 1);
  fixture_u32(&payload, (unsigned)-10); fixture_u32(&payload, 0); fixture_u32(&payload, (unsigned)-100);
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 0); /* fields + final event type */
  fixture_u32(&payload, 0); /* Create subtype */
  fixture_u32(&payload, 400); fixture_u32(&payload, 2);
  fixture_u32(&payload, 440); fixture_u32(&payload, 1); fixture_u32(&payload, 603);
  fixture_u32(&payload, 7); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 2);
  fixture_string(&payload, ""); fixture_string(&payload, "");
  fixture_u32(&payload, 1); fixture_u32(&payload, 8);
  for(int i = 0; i < 8; ++i) fixture_u32(&payload, 0);
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 0); fixture_u32(&payload, 8);
  fixture_string(&payload, "x = 4;");
  for(int i = 1; i < 8; ++i) fixture_string(&payload, "");
  fixture_u32(&payload, 0);
  fixture_u32(&payload, 440); fixture_u32(&payload, 1); fixture_u32(&payload, 603);
  fixture_u32(&payload, 7); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 2);
  fixture_string(&payload, ""); fixture_string(&payload, "");
  fixture_u32(&payload, 1); fixture_u32(&payload, 8);
  for(int i = 0; i < 8; ++i) fixture_u32(&payload, 0);
  fixture_u32(&payload, (unsigned)-1); fixture_u32(&payload, 0); fixture_u32(&payload, 8);
  fixture_string(&payload, "/* disabled action");
  for(int i = 1; i < 8; ++i) fixture_string(&payload, "");
  fixture_u32(&payload, 0);
  fixture_u32(&payload, (unsigned)-1); /* end Create event list */
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128] = "tmp/classic_object_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_objects(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "object import failed: %s\n", err);
  if(ok){
    char source[256] = {0};
    FILE *file = fopen(project.objects[0].events[0].source_path, "rb");
    size_t got = file ? fread(source, 1, sizeof(source) - 1, file) : 0;
    if(file) fclose(file);
    ok = project.n_objects == 1 && project.objects[0].depth == -10 && project.objects[0].n_events == 1 &&
         project.objects[0].events[0].event_type == 0 && got && strstr(source, "(function(){") &&
         strstr(source, "x = 4;") && strstr(source, "/* disabled action\n*/\n})()") &&
         strstr(source, "})()");
    remove(project.objects[0].events[0].source_path);
  }
  for(int i = 0; i < project.n_objects; ++i){
    free(project.objects[i].id); free(project.objects[i].name);
    for(int event = 0; event < project.objects[i].n_events; ++event){
      free(project.objects[i].events[event].id); free(project.objects[i].events[event].source_path);
    }
    free(project.objects[i].events);
  }
  free(project.objects);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_room_import(void){
  GmlcClassicManifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  manifest.inventory.resource_slots[GMLC_CLASSIC_ROOM] = 1;
  manifest.inventory.last_instance_id = 100001;
  manifest.existing[GMLC_CLASSIC_ROOM] = 1;
  manifest.slots[GMLC_CLASSIC_ROOM] = (GmlcClassicResourceSlot*)calloc(1, sizeof(GmlcClassicResourceSlot));
  if(!manifest.slots[GMLC_CLASSIC_ROOM]) return 0;
  GmlcClassicResourceSlot *slot = &manifest.slots[GMLC_CLASSIC_ROOM][0];
  slot->exists = 1; slot->name = strdup("resource_room");
  Fixture payload = {{0}, 0};
  fixture_string(&payload, "caption");
  fixture_u32(&payload, 320); fixture_u32(&payload, 240); fixture_u32(&payload, 16);
  fixture_u32(&payload, 16); fixture_u32(&payload, 0); fixture_u32(&payload, 30);
  fixture_u32(&payload, 0); fixture_u32(&payload, 0x00112233); fixture_u32(&payload, 1);
  fixture_string(&payload, "global.room_ready = 1;");
  fixture_u32(&payload, 1);
  fixture_u32(&payload, 1); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 1);
  fixture_u32(&payload, 1); fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 0); fixture_u32(&payload, 1);
  for(int field = 0; field < 14; ++field) fixture_u32(&payload, field == 3 ? 320 : field == 4 ? 240 : 0);
  fixture_u32(&payload, 1);
  fixture_u32(&payload, 12); fixture_u32(&payload, 34); fixture_u32(&payload, 0); fixture_u32(&payload, 100001);
  fixture_string(&payload, "x += 1;"); fixture_u32(&payload, 0);
  fixture_u32(&payload, 1);
  fixture_u32(&payload, 0); fixture_u32(&payload, 0); fixture_u32(&payload, 0);
  fixture_u32(&payload, 2); fixture_u32(&payload, 3); fixture_u32(&payload, 16);
  fixture_u32(&payload, 16); fixture_u32(&payload, 100); fixture_u32(&payload, 1000001); fixture_u32(&payload, 0);
  for(int field = 0; field < 14; ++field) fixture_u32(&payload, 0);
  slot->payload = (uint8_t*)malloc(payload.size);
  if(!slot->name || !slot->payload){ gmlc_classic_manifest_free(&manifest); return 0; }
  memcpy(slot->payload, payload.data, payload.size); slot->payload_size = payload.size;

  GmlcProject project;
  memset(&project, 0, sizeof(project));
  char err[256], dir[128] = "tmp/classic_room_fixture";
#ifdef _WIN32
  _mkdir(dir);
#else
  mkdir(dir, 0777);
#endif
  int ok = gmlc_classic_import_rooms(&manifest, &project, dir, err, sizeof(err));
  if(!ok) fprintf(stderr, "room import failed: %s\n", err);
  if(ok) ok = project.n_rooms == 1 && project.rooms[0].width == 320 &&
              project.rooms[0].n_backgrounds == 1 && project.rooms[0].n_instances == 1 &&
              project.rooms[0].instances[0].instance_id == 100001 && project.rooms[0].n_tiles == 1 &&
              project.rooms[0].tiles[0].depth == 100 && project.next_instance_id == 100002;
  if(project.n_rooms){
    GmlcRoom *room = &project.rooms[0];
    if(room->creation_code_path) remove(room->creation_code_path);
    free(room->creation_code_path); free(room->id); free(room->name);
    for(int i = 0; i < room->n_instances; ++i){
      if(room->instances[i].creation_code_path) remove(room->instances[i].creation_code_path);
      free(room->instances[i].creation_code_path); free(room->instances[i].id); free(room->instances[i].name);
    }
    free(room->instances); free(room->backgrounds); free(room->tiles);
  }
  free(project.rooms);
  gmlc_classic_manifest_free(&manifest);
#ifndef _WIN32
  rmdir(dir);
#endif
  return ok;
}

static int expect_sparse_room_order(void){
  Fixture plain=legacy_fixture_variant(701,1);
  size_t encoded_size=0;
  unsigned char *encoded=encode_gm7(plain.data,plain.size,&encoded_size);
  if(!encoded) return 0;
  GmlcClassicManifest manifest;
  GmlcProject project;
  memset(&manifest,0,sizeof(manifest));
  memset(&project,0,sizeof(project));
  char err[256]={0};
  int ok=gmlc_classic_manifest(encoded,encoded_size,&manifest,err,sizeof(err));
  if(!ok) fprintf(stderr,"sparse room-order manifest failed: %s\n",err);
  if(ok) ok=manifest.existing[GMLC_CLASSIC_ROOM]==2 && manifest.room_order_count==2 &&
            manifest.room_order[0]==6 && manifest.room_order[1]==2;
  if(ok) ok=gmlc_classic_import_room_order(&manifest,&project,err,sizeof(err));
  if(!ok && err[0]) fprintf(stderr,"sparse room-order import failed: %s\n",err);
  if(ok) ok=project.n_room_order==2 && project.room_order[0]==6 && project.room_order[1]==2;
  free(project.room_order);
  if(manifest.inventory.header.version) gmlc_classic_manifest_free(&manifest);
  free(encoded);
  return ok;
}

static void discard_imported_objects(GmlcProject *project, int remove_sources){
  for(int i = 0; i < project->n_objects; ++i){
    free(project->objects[i].id);
    free(project->objects[i].name);
    for(int event = 0; event < project->objects[i].n_events; ++event){
      free(project->objects[i].events[event].id);
      free(project->objects[i].events[event].collision_id);
      if(remove_sources && project->objects[i].events[event].source_path)
        remove(project->objects[i].events[event].source_path);
      free(project->objects[i].events[event].source_path);
    }
    free(project->objects[i].events);
  }
  free(project->objects);
  project->objects = NULL;
  project->n_objects = project->cap_objects = 0;
}

static void discard_imported_backgrounds(GmlcProject *project, int remove_sources){
  for(int i = 0; i < project->n_sprites; ++i){
    free(project->sprites[i].id); free(project->sprites[i].name);
    for(int frame = 0; frame < project->sprites[i].n_frames; ++frame){
      if(remove_sources && project->sprites[i].frame_paths && project->sprites[i].frame_paths[frame])
        remove(project->sprites[i].frame_paths[frame]);
      free(project->sprites[i].frame_paths ? project->sprites[i].frame_paths[frame] : NULL);
    }
    free(project->sprites[i].frame_paths);
  }
  free(project->sprites);
  for(int i = 0; i < project->n_tilesets; ++i){
    free(project->tilesets[i].id); free(project->tilesets[i].name);
  }
  free(project->tilesets);
  memset(project, 0, sizeof(*project));
}

static void discard_imported_timelines(GmlcProject *project, int remove_sources){
  for(int i=0;i<project->n_timelines;i++){
    free(project->timelines[i].id); free(project->timelines[i].name);
    for(int m=0;m<project->timelines[i].n_moments;m++){
      if(remove_sources && project->timelines[i].moments[m].source_path)
        remove(project->timelines[i].moments[m].source_path);
      free(project->timelines[i].moments[m].source_path);
    }
    free(project->timelines[i].moments);
  }
  free(project->timelines);
  project->timelines=NULL; project->n_timelines=project->cap_timelines=0;
}

static void discard_imported_rooms(GmlcProject *project, int remove_sources){
  for(int i = 0; i < project->n_rooms; ++i){
    GmlcRoom *room = &project->rooms[i];
    if(remove_sources && room->creation_code_path) remove(room->creation_code_path);
    free(room->creation_code_path); free(room->id); free(room->name);
    for(int instance = 0; instance < room->n_instances; ++instance){
      if(remove_sources && room->instances[instance].creation_code_path)
        remove(room->instances[instance].creation_code_path);
      free(room->instances[instance].creation_code_path);
      free(room->instances[instance].id); free(room->instances[instance].name);
    }
    free(room->instances); free(room->backgrounds); free(room->tiles);
  }
  free(project->rooms);
  memset(project, 0, sizeof(*project));
}

int main(int argc, char **argv){
  if(argc==3 && !strcmp(argv[1],"--write-legacy-exe-fixture")){
    Fixture executable;
    if(!build_legacy_executable_fixture(&executable)) return 1;
    FILE *file=fopen(argv[2],"wb");
    int ok=file && fwrite(executable.data,1,executable.size,file)==executable.size;
    if(file && fclose(file)!=0) ok=0;
    if(!ok){ fprintf(stderr,"cannot write legacy executable fixture: %s\n",argv[2]); return 1; }
    printf("wrote legacy executable fixture: %s (%zu bytes)\n",argv[2],executable.size);
    return 0;
  }
  if(argc==3 && !strcmp(argv[1],"--write-exe-fixture")){
    Fixture executable;
    if(!build_executable_fixture(&executable)) return 1;
    FILE *file=fopen(argv[2],"wb");
    int ok=file && fwrite(executable.data,1,executable.size,file)==executable.size;
    if(file && fclose(file)!=0) ok=0;
    if(!ok){ fprintf(stderr,"cannot write executable fixture: %s\n",argv[2]); return 1; }
    printf("wrote executable fixture: %s (%zu bytes)\n",argv[2],executable.size);
    return 0;
  }
  if(argc==4 && !strcmp(argv[1],"--write-project-fixture")){
    unsigned version=(unsigned)strtoul(argv[2],NULL,10);
    Fixture project;
    if(!build_project_fixture(version,&project)){
      fprintf(stderr,"unsupported project fixture version: %s\n",argv[2]);
      return 1;
    }
    FILE *file=fopen(argv[3],"wb");
    int ok=file && fwrite(project.data,1,project.size,file)==project.size;
    if(file && fclose(file)!=0) ok=0;
    if(!ok){ fprintf(stderr,"cannot write project fixture: %s\n",argv[3]); return 1; }
    printf("wrote project fixture %u: %s (%zu bytes)\n",version,argv[3],project.size);
    return 0;
  }
  const unsigned versions[] = {600, 701, 702, 800, 810};
  int passed = 0, failed = 0;
#define EXPECT_PROBE(label,expression) do { \
    if(expression) ++passed; \
    else { fprintf(stderr,"%s failed\n",label); ++failed; } \
  } while(0)
  for(size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); ++i){
    char label[64];
    snprintf(label,sizeof(label),"header %u",versions[i]);
    EXPECT_PROBE(label,expect_header(versions[i]));
  }
  EXPECT_PROBE("reject magic",expect_rejected(0,800,28));
  EXPECT_PROBE("reject version",expect_rejected(GMLC_CLASSIC_MAGIC,999,28));
  EXPECT_PROBE("reject short header",expect_rejected(GMLC_CLASSIC_MAGIC,800,27));
  EXPECT_PROBE("inventory 800",expect_inventory(800));
  EXPECT_PROBE("inventory 810",expect_inventory(810));
  EXPECT_PROBE("manifest 800",expect_manifest());
  EXPECT_PROBE("manifest 810",expect_manifest_810());
  EXPECT_PROBE("executable manifest",expect_executable_manifest());
  EXPECT_PROBE("legacy executable manifest",expect_legacy_executable_manifest());
  EXPECT_PROBE("GM7 decode",expect_gm7_decode());
  EXPECT_PROBE("legacy manifest 600",expect_legacy_manifest(600));
  EXPECT_PROBE("legacy manifest 701",expect_legacy_manifest(701));
  EXPECT_PROBE("script import",expect_script_import());
  EXPECT_PROBE("extension alias import",expect_extension_alias_import());
  EXPECT_PROBE("sprite import project",expect_sprite_import(0));
  EXPECT_PROBE("sprite import executable",expect_sprite_import(1));
  EXPECT_PROBE("background import project",expect_background_import(0));
  EXPECT_PROBE("background import executable",expect_background_import(1));
  EXPECT_PROBE("sparse font import",expect_sparse_font_import());
  EXPECT_PROBE("empty font import",expect_empty_font_import());
  EXPECT_PROBE("GM8.1 font metadata",expect_gm81_font_metadata());
  EXPECT_PROBE("sound import",expect_sound_import());
  EXPECT_PROBE("legacy media import",expect_legacy_media_import());
  EXPECT_PROBE("path import",expect_path_import());
  EXPECT_PROBE("timeline import",expect_timeline_import());
  EXPECT_PROBE("object import",expect_object_import());
  EXPECT_PROBE("room import",expect_room_import());
  EXPECT_PROBE("sparse room order",expect_sparse_room_order());
#undef EXPECT_PROBE

  for(int i = 1; i < argc; ++i){
    GmlcClassicInventory in;
    GmlcClassicHeader h;
    GmlcClassicManifest manifest;
    char err[512];
    if(!gmlc_classic_manifest_file(argv[i], &manifest, err, sizeof(err))){
      fprintf(stderr, "%s: %s\n", argv[i], err);
      ++failed;
      continue;
    }
    in=manifest.inventory;
    h=in.header;
      printf("%s\t%u\t%s\tsettings=%u swapcreate=%d sounds=%u/%u sprites=%u/%u backgrounds=%u/%u paths=%u/%u scripts=%u/%u fonts=%u/%u timelines=%u/%u objects=%u/%u rooms=%u/%u\n",
             argv[i], (unsigned)h.version, gmlc_classic_version_name(h.version), in.settings_version,
             in.settings.swap_creation_events,
             manifest.existing[GMLC_CLASSIC_SOUND], in.resource_slots[GMLC_CLASSIC_SOUND],
             manifest.existing[GMLC_CLASSIC_SPRITE], in.resource_slots[GMLC_CLASSIC_SPRITE],
             manifest.existing[GMLC_CLASSIC_BACKGROUND], in.resource_slots[GMLC_CLASSIC_BACKGROUND],
             manifest.existing[GMLC_CLASSIC_PATH], in.resource_slots[GMLC_CLASSIC_PATH],
             manifest.existing[GMLC_CLASSIC_SCRIPT], in.resource_slots[GMLC_CLASSIC_SCRIPT],
             manifest.existing[GMLC_CLASSIC_FONT], in.resource_slots[GMLC_CLASSIC_FONT],
             manifest.existing[GMLC_CLASSIC_TIMELINE], in.resource_slots[GMLC_CLASSIC_TIMELINE],
             manifest.existing[GMLC_CLASSIC_OBJECT], in.resource_slots[GMLC_CLASSIC_OBJECT],
             manifest.existing[GMLC_CLASSIC_ROOM], in.resource_slots[GMLC_CLASSIC_ROOM]);
      GmlcProject project;
      memset(&project, 0, sizeof(project));
      const char *object_dir = "tmp/classic_object_corpus";
#ifdef _WIN32
      _mkdir(object_dir);
#else
      mkdir(object_dir, 0777);
#endif
      if(!gmlc_classic_import_objects(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: object import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_objects(&project, 1);
      if(!gmlc_classic_import_timelines(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: timeline import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_timelines(&project, 1);
      if(h.version >= GMLC_CLASSIC_GM8 &&
         !gmlc_classic_import_backgrounds(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: background import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_backgrounds(&project, 1);
      if(!gmlc_classic_import_rooms(&manifest, &project, object_dir, err, sizeof(err))){
        fprintf(stderr, "%s: room import: %s\n", argv[i], err);
        gmlc_classic_manifest_free(&manifest);
        ++failed;
        continue;
      }
      discard_imported_rooms(&project, 1);
      gmlc_classic_manifest_free(&manifest);
    ++passed;
  }
  printf("classic probe: passed=%d failed=%d\n", passed, failed);
  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
