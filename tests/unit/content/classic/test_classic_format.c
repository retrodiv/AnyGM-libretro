/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "classic_test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

static int expect_header(unsigned version){
  unsigned char data[32] = {0};
  size_t game_id_offset=version==530?12u:8u;
  put_u32le(data, GMLC_CLASSIC_MAGIC);
  put_u32le(data + 4, version);
  put_u32le(data + game_id_offset, 0x12345678u);
  for(int i = 0; i < 16; ++i) data[game_id_offset + 4u + (size_t)i] = (unsigned char)(0xa0 + i);
  GmlcClassicHeader h;
  char err[128];
  if(!gmlc_classic_probe(data, sizeof(data), &h, err, sizeof(err))){
    fprintf(stderr, "probe %u failed: %s\n", version, err);
    return 0;
  }
  int encrypted = version == 701 || version == 702;
  if((unsigned)h.version != version ||
     (!encrypted && (h.game_id != 0x12345678u ||
                     memcmp(h.guid, data + game_id_offset + 4u, 16))) ||
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
  Fixture settings={{0},0};
  for(unsigned field=0;field<14;field++)
    fixture_u32(&settings,field==1?1u:field==4?200u:0u);
  fixture_zero(&settings,9u*4u); /* shortcuts and process settings */
  fixture_u32(&settings,0); /* loading bar */
  fixture_u32(&settings,0); /* custom loading image */
  fixture_u32(&settings,0); fixture_u32(&settings,255); fixture_u32(&settings,1);
  fixture_u32(&settings,0); /* icon */
  fixture_zero(&settings,4u*4u); /* error settings */
  fixture_string(&settings,""); fixture_string(&settings,"");
  fixture_zero(&settings,8); fixture_string(&settings,"");
  fixture_u32(&settings,1); fixture_zero(&settings,3u*4u);
  for(unsigned i=0;i<4;i++) fixture_string(&settings,"");
  fixture_zero(&settings,8);
  fixture_compressed(&f, settings.data, (int)settings.size);
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
     in.settings.swap_creation_events != 0)
    return 0;
  for(unsigned type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type)
    if(in.resource_slots[type] != type) return 0;
  return 1;
}

static int expect_manifest(void){
  Fixture f = manifest_fixture(800);
  GmlcClassicManifest manifest;
  char err[128];
  if(!gmlc_classic_manifest(f.data, f.size, &manifest, err, sizeof(err))){
    fprintf(stderr, "manifest failed: %s\n", err);
    return 0;
  }
  int ok = !manifest.executable_layout &&
           manifest.existing[GMLC_CLASSIC_SCRIPT] == 1 &&
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
  /* The compact record omits the timestamp. The decoder inserts a neutral value so both
   * on-disk layouts expose the same normalized structure. */
  if(ok){
    Fixture compact=expected_information;
    size_t caption=12u+get_u32le(compact.data+8)+8u*4u;
    memmove(compact.data+caption,compact.data+caption+8u,compact.size-caption-8u);
    compact.size-=8u;
    GmlcClassicBlob source={compact.data,compact.size};
    memset(&decoded,0,sizeof(decoded));
    ok=gmlc_classic_game_information_decode(&source,&decoded,err,sizeof(err)) &&
       decoded.size==expected_information.size &&
       !memcmp(decoded.data,expected_information.data,caption) &&
       !memcmp(decoded.data+caption,"\0\0\0\0\0\0\0\0",8u) &&
       !memcmp(decoded.data+caption+8u,compact.data+caption,compact.size-caption);
    free(decoded.data);
  }
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

static int expect_gm53_manifest(void){
  Fixture plain=legacy_fixture(530),executable={{0},0};
  GmlcClassicManifest manifest={0}; char err[256]={0};
  int ok=gmlc_classic_manifest(plain.data,plain.size,&manifest,err,sizeof(err));
  if(ok) ok=manifest.inventory.header.version==GMLC_CLASSIC_GM53 &&
             manifest.inventory.header.game_id==42 &&
             manifest.inventory.settings_version==530 &&
             manifest.inventory.settings.scaling==100 && !manifest.executable_layout;
  gmlc_classic_manifest_free(&manifest);
  if(ok) ok=build_gm53_executable_fixture(&executable) &&
            gmlc_classic_manifest(executable.data,executable.size,&manifest,err,sizeof(err));
  if(ok) ok=manifest.inventory.header.version==GMLC_CLASSIC_GM53 &&
             manifest.inventory.header.game_id==42 && !manifest.executable_layout;
  gmlc_classic_manifest_free(&manifest);
  GmlcClassicBlob extracted={0}; GmlcClassicVersion version=GMLC_CLASSIC_UNKNOWN;
  if(ok) ok=gmlc_classic_embedded_project(executable.data,executable.size,&extracted,&version,
                                           err,sizeof(err)) &&
            version==GMLC_CLASSIC_GM53 && extracted.size==plain.size &&
            !memcmp(extracted.data,plain.data,plain.size);
  free(extracted.data);
  if(!ok) fprintf(stderr,"Game Maker 5.3 manifest failed: %s\n",err);
  return ok;
}

static int expect_embedded_project_rejects_compiled_layout(void){
  Fixture executable={{0},0}; GmlcClassicBlob extracted={(uint8_t*)1,1};
  GmlcClassicVersion version=GMLC_CLASSIC_GM53; char err[256]={0};
  int ok=build_gm6_executable_fixture(&executable) &&
    !gmlc_classic_embedded_project(executable.data,executable.size,&extracted,&version,
                                   err,sizeof(err)) &&
    !extracted.data && !extracted.size && version==GMLC_CLASSIC_UNKNOWN && err[0];
  if(!ok) fprintf(stderr,"compiled-layout extraction was not rejected: %s\n",err);
  return ok;
}

static int expect_executable_manifest_variant(const Fixture *executable){
  GmlcClassicManifest manifest;
  char err[256]={0};
  int ok=gmlc_classic_manifest(executable->data,executable->size,&manifest,err,sizeof(err));
  if(!ok) fprintf(stderr,"executable manifest failed: %s\n",err);
  if(ok){
    ok=manifest.executable_layout &&
       manifest.inventory.header.version==GMLC_CLASSIC_GM8 &&
       manifest.inventory.settings.swap_creation_events==1 &&
       manifest.inventory.header.game_id==0x13572468 && manifest.room_order_count==1 &&
       manifest.room_order[0]==0 &&
       manifest.extension_count==1 && !strcmp(manifest.extension_names[0],"fixture_executable_extension") &&
       manifest.extension_detail_count==1 && manifest.extensions &&
       manifest.extensions[0].version==700 &&
       !strcmp(manifest.extensions[0].name,"fixture_executable_extension") &&
       !strcmp(manifest.extensions[0].folder,"fixture_folder") &&
       manifest.extensions[0].file_count==1 && manifest.extensions[0].data_size==4 &&
       manifest.extensions[0].files[0].version==700 &&
       !strcmp(manifest.extensions[0].files[0].name,"fixture.bin") &&
       manifest.extensions[0].files[0].kind==4 &&
       manifest.extensions[0].files[0].function_count==1 &&
       !strcmp(manifest.extensions[0].files[0].functions[0].name,"fixture_function") &&
       !strcmp(manifest.extensions[0].files[0].functions[0].external_name,"fixture_external") &&
       manifest.extensions[0].files[0].functions[0].signature[0]==100 &&
       manifest.extensions[0].files[0].functions[0].signature[20]==120 &&
       manifest.extensions[0].files[0].constant_count==1 &&
       !strcmp(manifest.extensions[0].files[0].constants[0].name,
               "fixture_extension_constant") &&
       manifest.existing[GMLC_CLASSIC_SCRIPT]==1 &&
       manifest.existing[GMLC_CLASSIC_SPRITE]==1 &&
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
      fixture_project_init(&project);
      project.prefer_memory_files=1;
      ok=gmlc_classic_import_fonts(&manifest,&project,"tmp",err,sizeof(err)) &&
         gmlc_classic_import_rooms(&manifest,&project,"tmp",err,sizeof(err));
      if(ok) ok=project.n_fonts==1 && project.fonts[0].n_glyphs==2 &&
        project.fonts[0].em_size==1 && project.fonts[0].glyphs[0].ch==65 &&
        project.fonts[0].glyphs[0].x==0 && project.fonts[0].glyphs[0].w==1 &&
        project.fonts[0].glyphs[0].shift==3 && project.fonts[0].glyphs[0].offset==0 &&
        project.fonts[0].glyphs[1].ch==66 && project.fonts[0].glyphs[1].x==1 &&
        project.fonts[0].glyphs[1].shift==4 && project.fonts[0].glyphs[1].offset==-1 &&
        project.n_memory_files==2 && project.memory_files[0].kind==GMLC_MEMORY_RGBA &&
        project.memory_files[0].width==2 && project.memory_files[0].height==1 &&
        project.memory_files[0].size==8 && project.memory_files[0].data[3]==17 &&
        project.memory_files[1].kind==GMLC_MEMORY_TEXT &&
        project.memory_files[1].size==strlen("x += 1;") &&
        !memcmp(project.memory_files[1].data,"x += 1;",strlen("x += 1;")) &&
        project.memory_files[0].data[7]==231 && project.n_rooms==1 &&
        project.rooms[0].n_instances==1 &&
        project.rooms[0].instances[0].creation_code_path!=NULL &&
        project.rooms[0].n_tiles==1 && project.rooms[0].tiles[0].x==30 &&
        project.rooms[0].tiles[0].y==40 && project.rooms[0].tiles[0].depth==100 &&
        project.rooms[0].tiles[0].tile_id==1000001;
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

static int expect_executable_manifest_ignores_late_decoy(void){
  Fixture executable;
  if(!build_executable_fixture(&executable)) return 0;
  fixture_u32(&executable,GMLC_CLASSIC_MAGIC);
  fixture_u32(&executable,GMLC_CLASSIC_GM8);
  fixture_u32(&executable,0);
  fixture_u32(&executable,GMLC_CLASSIC_GM8);
  fixture_u32(&executable,0);
  return expect_executable_manifest_variant(&executable);
}

static int expect_executable_candidate_flood_rejected(void){
  Fixture executable={{'M','Z'},2};
  for(unsigned candidate=0;candidate<66;candidate++){
    fixture_u32(&executable,GMLC_CLASSIC_MAGIC);
    fixture_u32(&executable,GMLC_CLASSIC_GM8);
    fixture_u32(&executable,0);
    fixture_u32(&executable,GMLC_CLASSIC_GM8);
    fixture_u32(&executable,0);
  }
  GmlcClassicManifest manifest={0}; char error[256]={0};
  int ok=!gmlc_classic_manifest(executable.data,executable.size,&manifest,
                                error,sizeof(error)) &&
    strstr(error,"too many embedded-data candidates");
  gmlc_classic_manifest_free(&manifest);
  if(!ok) fprintf(stderr,"candidate flood was not rejected: %s\n",error);
  return ok;
}

static int expect_legacy_executable_manifest(void){
  Fixture executable;
  if(!build_legacy_executable_fixture(&executable)) return 0;
  GmlcClassicManifest manifest={0}; char err[256]={0};
  int ok=gmlc_classic_manifest(executable.data,executable.size,&manifest,err,sizeof(err));
  if(!ok) fprintf(stderr,"legacy executable manifest failed: %s\n",err);
  if(ok){
    ok=manifest.executable_layout &&
       manifest.inventory.header.version==GMLC_CLASSIC_GM7 &&
       manifest.inventory.header.game_id==0x24681357 && manifest.inventory.settings_version==702 &&
       manifest.inventory.settings.interpolate==1 && manifest.inventory.settings.scaling==150 &&
       manifest.existing[GMLC_CLASSIC_SPRITE]==1 &&
       manifest.existing[GMLC_CLASSIC_BACKGROUND]==1 &&
       manifest.existing[GMLC_CLASSIC_PATH]==1 &&
       manifest.existing[GMLC_CLASSIC_SCRIPT]==1 &&
       manifest.existing[GMLC_CLASSIC_FONT]==1 &&
       manifest.existing[GMLC_CLASSIC_ROOM]==1 &&
       !strcmp(manifest.slots[GMLC_CLASSIC_SCRIPT][0].source,"return 42;") &&
       manifest.room_order_count==1 && manifest.room_order[0]==0 &&
       manifest.library_creation_code_count==1;
    if(!ok) fprintf(stderr,"legacy executable manifest values were not preserved\n");
  }
  if(ok){
    GmlcProject project; fixture_project_init(&project); project.prefer_memory_files=1;
    ok=gmlc_classic_import_sprites(&manifest,&project,"tmp",err,sizeof(err)) &&
       gmlc_classic_import_backgrounds(&manifest,&project,"tmp",err,sizeof(err)) &&
       gmlc_classic_import_paths(&manifest,&project,err,sizeof(err)) &&
       gmlc_classic_import_fonts(&manifest,&project,"tmp",err,sizeof(err)) &&
       gmlc_classic_import_rooms(&manifest,&project,"tmp",err,sizeof(err));
    if(!ok) fprintf(stderr,"legacy executable import failed: %s\n",err);
    if(ok) ok=project.n_sprites==2 && project.n_memory_files>=2 &&
      project.memory_files[0].kind==GMLC_MEMORY_RGBA && project.memory_files[0].size==8 &&
      project.memory_files[0].width==2 && project.memory_files[0].height==1 &&
      project.memory_files[0].data[0]==0 && project.memory_files[0].data[1]==192 &&
      project.memory_files[0].data[2]==0 && project.memory_files[0].data[3]==0 &&
      project.memory_files[0].data[4]==0 && project.memory_files[0].data[5]==192 &&
      project.memory_files[0].data[6]==0 && project.memory_files[0].data[7]==255 &&
      project.sprites[0].col_kind==0 &&
      project.memory_files[1].data[0]==11 && project.memory_files[1].data[2]==13 &&
      project.n_paths==1 && project.paths[0].kind==1 && project.paths[0].closed &&
      project.paths[0].precision==4 && project.paths[0].n_points==2 &&
      project.paths[0].points[1].x>9.49f && project.paths[0].points[1].speed==50.0f &&
      project.n_fonts==1 && project.fonts[0].n_glyphs==2 &&
      project.fonts[0].glyphs[0].ch==65 && project.fonts[0].glyphs[0].x==0 &&
      project.fonts[0].glyphs[0].w==1 && project.fonts[0].glyphs[0].shift==3 &&
      project.fonts[0].glyphs[1].ch==66 && project.fonts[0].glyphs[1].x==1 &&
      project.fonts[0].glyphs[1].w==1 && project.fonts[0].glyphs[1].shift==4 &&
      project.fonts[0].glyphs[1].offset==-1 &&
      project.n_memory_files>=3 && project.memory_files[2].kind==GMLC_MEMORY_RGBA &&
      project.memory_files[2].width==2 && project.memory_files[2].height==1 &&
      project.memory_files[2].size==8 && project.memory_files[2].data[3]==23 &&
      project.memory_files[2].data[7]==211 &&
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

static int expect_gm6_executable_manifest(void){
  Fixture executable;
  if(!build_gm6_executable_fixture(&executable)) return 0;
  GmlcClassicManifest manifest={0}; char err[256]={0};
  int ok=gmlc_classic_manifest(executable.data,executable.size,&manifest,err,sizeof(err));
  if(ok) ok=manifest.executable_layout &&
    manifest.inventory.header.version==GMLC_CLASSIC_GM6 &&
    manifest.inventory.header.game_id==0x31415926 &&
    manifest.inventory.settings_version==GMLC_CLASSIC_GM6 &&
    manifest.inventory.settings.interpolate==1 && manifest.inventory.settings.scaling==150 &&
    manifest.constant_def_count==1 &&
    !strcmp(manifest.constant_defs[0].name,"fixture_gm6_constant") &&
    !strcmp(manifest.constant_defs[0].value,"6*7") &&
    manifest.library_creation_code_count==1 &&
    !strcmp(manifest.library_creation_code[0],"global.gm6_executable_started = 1;") &&
    manifest.room_order_count==0 && manifest.included_file_count==2 &&
    !strcmp(manifest.included_files[0].file_name,"before.dat") &&
    manifest.included_files[0].data_size==6 &&
    !memcmp(manifest.included_files[0].data,"before",6) &&
    !strcmp(manifest.included_files[1].file_name,"after.dat") &&
    manifest.included_files[1].data_size==5 &&
    !memcmp(manifest.included_files[1].data,"after",5);
  if(!ok) fprintf(stderr,"Game Maker 6 executable manifest failed: %s\n",err);
  gmlc_classic_manifest_free(&manifest);
  return ok;
}

static int expect_legacy_executable_font_corruption(void){
  Fixture executable;
  if(!build_legacy_executable_fixture(&executable)) return 0;
  GmlcClassicManifest manifest={0}; char err[256]={0};
  if(!gmlc_classic_manifest(executable.data,executable.size,&manifest,err,sizeof(err))){
    fprintf(stderr,"legacy executable corruption fixture failed to parse: %s\n",err);
    return 0;
  }
  int ok=manifest.inventory.resource_slots[GMLC_CLASSIC_FONT]==1 &&
         manifest.slots[GMLC_CLASSIC_FONT] &&
         manifest.slots[GMLC_CLASSIC_FONT][0].exists;
  GmlcClassicResourceSlot *slot=ok?&manifest.slots[GMLC_CLASSIC_FONT][0]:NULL;
  size_t atlas_size_offset=0,atlas_data_offset=0;
  uint32_t encoded_size=0;
  if(ok){
    if(slot->payload_size<4) ok=0;
    else {
      uint32_t face_size=get_u32le(slot->payload);
      atlas_size_offset=4u+(size_t)face_size+5u*4u+256u*6u*4u+2u*4u;
      if(atlas_size_offset>slot->payload_size ||
         slot->payload_size-atlas_size_offset<4u) ok=0;
      else {
        encoded_size=get_u32le(slot->payload+atlas_size_offset);
        atlas_data_offset=atlas_size_offset+4u;
        if(!encoded_size || atlas_data_offset>slot->payload_size ||
           encoded_size>slot->payload_size-atlas_data_offset) ok=0;
      }
    }
  }
  GmlcProject project;
  fixture_project_init(&project);
  project.prefer_memory_files=1;
  if(ok){
    slot->payload[atlas_data_offset]^=0xffu;
    err[0]='\0';
    ok=!gmlc_classic_import_fonts(&manifest,&project,"tmp",err,sizeof(err)) &&
       err[0] && !project.fonts && project.n_fonts==0;
  }
  if(!ok) fprintf(stderr,"corrupt compressed font atlas was not rejected transactionally: %s\n",err);
  gmlc_project_free(&project);
  gmlc_classic_manifest_free(&manifest);
  return ok;
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
    GmlcClassicManifest manifest; GmlcProject project; fixture_project_clear(&project);
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
    GmlcClassicManifest manifest; GmlcProject project; fixture_project_clear(&project);
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
    GmlcClassicManifest manifest; GmlcProject project; fixture_project_clear(&project);
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

static int test_header_530(void){ return expect_header(530); }
static int test_header_600(void){ return expect_header(600); }
static int test_header_701(void){ return expect_header(701); }
static int test_header_702(void){ return expect_header(702); }
static int test_header_800(void){ return expect_header(800); }
static int test_header_810(void){ return expect_header(810); }
static int test_reject_magic(void){
  return expect_rejected(0,800,28);
}
static int test_reject_version(void){
  return expect_rejected(GMLC_CLASSIC_MAGIC,999,28);
}
static int test_reject_short_header(void){
  return expect_rejected(GMLC_CLASSIC_MAGIC,800,27);
}
static int test_inventory_800(void){ return expect_inventory(800); }
static int test_inventory_810(void){ return expect_inventory(810); }
static int test_legacy_manifest_600(void){ return expect_legacy_manifest(600); }
static int test_legacy_manifest_701(void){ return expect_legacy_manifest(701); }

AnygmTestGroup classic_test_format_group(void){
  static const AnygmTestCase cases[]={
    {"header-530",test_header_530},
    {"header-600",test_header_600},
    {"header-701",test_header_701},
    {"header-702",test_header_702},
    {"header-800",test_header_800},
    {"header-810",test_header_810},
    {"reject-magic",test_reject_magic},
    {"reject-version",test_reject_version},
    {"reject-short-header",test_reject_short_header},
    {"inventory-800",test_inventory_800},
    {"inventory-810",test_inventory_810},
    {"manifest-800",expect_manifest},
    {"manifest-810",expect_manifest_810},
    {"manifest-530-executable",expect_gm53_manifest},
    {"embedded-project-rejects-compiled-layout",expect_embedded_project_rejects_compiled_layout},
    {"executable-manifest",expect_executable_manifest},
    {"executable-manifest-late-decoy",expect_executable_manifest_ignores_late_decoy},
    {"executable-candidate-flood",expect_executable_candidate_flood_rejected},
    {"gm6-executable-manifest",expect_gm6_executable_manifest},
    {"legacy-executable-manifest",expect_legacy_executable_manifest},
    {"legacy-executable-font-corruption",expect_legacy_executable_font_corruption},
    {"gm7-decode",expect_gm7_decode},
    {"legacy-manifest-600",test_legacy_manifest_600},
    {"legacy-manifest-701",test_legacy_manifest_701},
  };
  const AnygmTestGroup group={
    "classic.format",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}

AnygmTestGroup classic_test_legacy_media_group(void){
  static const AnygmTestCase cases[]={
    {"import",expect_legacy_media_import},
  };
  const AnygmTestGroup group={
    "classic.legacy-media",cases,sizeof cases/sizeof cases[0]
  };
  return group;
}
