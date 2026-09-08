/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "engine_internal.h"
#include "stdio_vfs.h"
#include "synthetic_content.h"
#include "vcdiff_fixture.h"
#include "anygm_test_runner.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct PatchFixture {
  AnygmSyntheticContent content[3];
  uint8_t *bytes[3],*patch[2];
  size_t size[3],patch_size[2];
  char digest[3][65],patch_digest[2][65];
  char anchor[256],ini[256],patch_path[2][256];
  AnygmEngine *engine;
  AnygmContentSource source;
} PatchFixture;

static void write_file(const char *path,const void *bytes,size_t size){
  FILE *file=fopen(path,"wb"); assert(file);
  assert(fwrite(bytes,1,size,file)==size && !fclose(file));
}

static void write_text(const char *path,const char *text){write_file(path,text,strlen(text));}

/* Some host namespaces cannot rename over an existing destination. Exercise
 * that contract on every platform instead of inheriting POSIX replacement. */
static AnygmResult rename_without_replacement(void *userdata,const char *from,const char *to){
  (void)userdata;
  struct stat status;
  if(!stat(to,&status)) return ANYGM_ERROR_IO;
  return rename(from,to)==0?ANYGM_OK:ANYGM_ERROR_IO;
}

static AnygmResult reject_publication(void *userdata,const char *from,const char *to){
  (void)userdata; (void)from; (void)to;
  return ANYGM_ERROR_IO;
}

static void configure(PatchFixture *f,const char *order){
  char text[2048];
  int n=snprintf(text,sizeof text,"[anygm]\npayload=data.win\n[patches]\n"
    "first=xdelta|first.xdelta|%s|%s|%s|%zu\n"
    "second=xdelta|second.xdelta|%s|%s|%s|%zu\n"
    "[pipelines]\ninput.final=%s\n",f->digest[0],f->patch_digest[0],f->digest[1],f->size[1],
    f->digest[1],f->patch_digest[1],f->digest[2],f->size[2],order);
  assert(n>0 && (size_t)n<sizeof text); write_text(f->anchor,text);
}

static void setup(PatchFixture *f){
  memset(f,0,sizeof *f);
  assert(anygm_synthetic_content_create(&f->content[0]));
  assert(anygm_synthetic_anchor_script_content_create(&f->content[1]));
  assert(anygm_synthetic_list_override_content_create(&f->content[2]));
  for(size_t i=0;i<3;i++){
    assert(anygm_synthetic_content_read(&f->content[i],&f->bytes[i],&f->size[i]));
    anygm_test_sha256_hex(f->bytes[i],f->size[i],f->digest[i]);
  }
  for(size_t i=0;i<2;i++){
    f->patch[i]=anygm_test_vcdiff_literal(f->bytes[i+1],f->size[i+1],&f->patch_size[i]);
    anygm_test_sha256_hex(f->patch[i],f->patch_size[i],f->patch_digest[i]);
    snprintf(f->patch_path[i],sizeof f->patch_path[i],"%s/%s.xdelta",f->content[0].directory,
      i?"second":"first");
    write_file(f->patch_path[i],f->patch[i],f->patch_size[i]);
  }
  snprintf(f->anchor,sizeof f->anchor,"%s/content.anygm",f->content[0].directory);
  snprintf(f->ini,sizeof f->ini,"%s/anygm.ini",f->content[0].directory);
  char text[1024];
  /* A generic global probe may decline; the local final chain must still run.
   * Hash-specific runtime policy continues to refer to the original input. */
  snprintf(text,sizeof text,"[transforms]\ninput.probe=buffer probe(){return slice(0,0);}\n"
    "[sha256:%s.overrides]\n$patch_marker=17\n"
    "[sha256:%s.overrides]\n$patch_marker=29\n",f->digest[0],f->digest[2]);
  write_text(f->ini,text); configure(f,"first | second");
  AnygmHostServices services={0}; services.struct_size=sizeof services;
  services.abi_version=ANYGM_HOST_SERVICES_VERSION; anygm_stdio_vfs_services_init(&services);
  services.path_rename=rename_without_replacement;
  assert(anygm_create(&services,&f->engine)==ANYGM_OK);
  f->source.struct_size=sizeof f->source; f->source.kind=ANYGM_CONTENT_PATH;
  f->source.path=f->anchor; f->source.system_directory=f->content[0].directory;
  f->source.cache_directory=f->content[0].directory;
}

static void teardown(PatchFixture *f){
  anygm_destroy(f->engine);
  assert(!remove(f->anchor) && !remove(f->ini));
  for(size_t i=0;i<2;i++){assert(!remove(f->patch_path[i]));free(f->patch[i]);}
  for(size_t i=0;i<3;i++){free(f->bytes[i]);anygm_synthetic_content_destroy(&f->content[i]);}
}

static void frame(AnygmEngine *engine){
  AnygmInputFrame input={0}; AnygmFrameOutput output={0};
  input.struct_size=sizeof input; input.pointer_x=input.pointer_y=-1;
  output.struct_size=sizeof output;
  assert(anygm_run_frame(engine,&input,&output)==ANYGM_OK);
}

static void expect_payload(PatchFixture *f,size_t index){
  assert(f->engine->win.size==f->size[index]);
  assert(!memcmp(f->engine->win.data,f->bytes[index],f->size[index]));
  assert(!strcmp(f->engine->win.content_dir,f->content[0].directory));
}

static int ordered_lifecycle(void){
  PatchFixture f; setup(&f);
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_OK); expect_payload(&f,2);
  assert(strcmp(f.engine->current_content_path,f.content[0].path));
  /* This API reports the current exact size, not libretro's session capacity.
   * The adapter's fixed/variable negotiation has its own transport contract test. */
  size_t capacity=anygm_state_size(f.engine),written=0;
  uint8_t *state=malloc(capacity); assert(state);
  assert(anygm_state_save(f.engine,state,capacity,&written)==ANYGM_OK);
  frame(f.engine); frame(f.engine);
  assert(gml_global_num(&f.engine->vm,"patch_marker")==17);
  assert(gml_global_num(&f.engine->vm,"fixture_counter")==2);
  capacity=anygm_state_size(f.engine);
  uint8_t *resized=realloc(state,capacity); assert(resized); state=resized;
  assert(anygm_state_save(f.engine,state,capacity,&written)==ANYGM_OK);
  frame(f.engine);
  assert(anygm_state_load(f.engine,state,written)==ANYGM_OK);
  assert(gml_global_num(&f.engine->vm,"fixture_counter")==2);
  assert(anygm_reset(f.engine)==ANYGM_OK); expect_payload(&f,2); frame(f.engine);
  assert(anygm_state_load(f.engine,state,written)==ANYGM_OK);
  anygm_unload(f.engine);
  /* Selecting the raw payload adopts the same single sibling anchor. */
  f.source.path=f.content[0].path;
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_OK); expect_payload(&f,2);
  assert(anygm_state_load(f.engine,state,written)==ANYGM_OK);
  anygm_unload(f.engine); configure(&f,"first");
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_OK); expect_payload(&f,1);
  frame(f.engine);
  size_t other_capacity=anygm_state_size(f.engine),before_size=0,after_size=0;
  uint8_t *before=malloc(other_capacity),*after=malloc(other_capacity); assert(before && after);
  assert(anygm_state_save(f.engine,before,other_capacity,&before_size)==ANYGM_OK);
  assert(anygm_state_load(f.engine,state,written)==ANYGM_ERROR_STATE_MISMATCH);
  assert(anygm_state_save(f.engine,after,other_capacity,&after_size)==ANYGM_OK);
  assert(before_size==after_size && !memcmp(before,after,before_size));
  free(before); free(after);
  uint8_t *original=NULL; size_t original_size=0;
  assert(anygm_synthetic_content_read(&f.content[0],&original,&original_size));
  assert(original_size==f.size[0] && !memcmp(original,f.bytes[0],original_size));
  free(original); free(state); teardown(&f); return 1;
}

static int dependency_rejection(void){
  PatchFixture f; setup(&f);
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_OK); expect_payload(&f,2);
  anygm_unload(f.engine);
  /* A warm output never substitutes for a missing or altered dependency. */
  assert(!remove(f.patch_path[1]));
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_ERROR_INVALID_CONTENT);
  assert(!f.engine->win.data);
  f.patch[1][f.patch_size[1]-1]^=1;
  write_file(f.patch_path[1],f.patch[1],f.patch_size[1]);
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_ERROR_INVALID_CONTENT);
  f.patch[1][f.patch_size[1]-1]^=1;
  write_file(f.patch_path[1],f.patch[1],f.patch_size[1]);
  configure(&f,"second | first");
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_ERROR_INVALID_CONTENT);
  assert(!f.engine->win.data);
  configure(&f,"first | second");
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_OK); expect_payload(&f,2);
  teardown(&f); return 1;
}

static int memory_dependency_rejection(void){
  PatchFixture f; setup(&f);
  char text[1024];
  snprintf(text,sizeof text,"[patches]\nfirst=xdelta|first.xdelta|%s|%s|%s|%zu\n"
    "[pipelines]\ninput.final=first\n",f.digest[0],f.patch_digest[0],f.digest[1],f.size[1]);
  write_text(f.ini,text);
  f.source.kind=ANYGM_CONTENT_MEMORY; f.source.path=NULL;
  f.source.data=f.bytes[0]; f.source.size=f.size[0];
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_ERROR_INVALID_CONTENT);
  assert(!f.engine->win.data);
  teardown(&f); return 1;
}

static int cache_reuse_and_repair(void){
  PatchFixture f; setup(&f);
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_OK); expect_payload(&f,2);
  char cached[1536]; snprintf(cached,sizeof cached,"%s",f.engine->current_content_path);
  anygm_unload(f.engine);
  /* Exact cached bytes need no publication, even when renames are unavailable. */
  f.engine->host.path_rename=reject_publication;
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_OK); expect_payload(&f,2);
  anygm_unload(f.engine);
  /* Temporarily hiding an anchor selects the original image. Restoring its
   * extension must recover the same prepared input without clearing caches. */
  char parked[300]; snprintf(parked,sizeof parked,"%s.disabled",f.anchor);
  assert(!rename(f.anchor,parked)); f.source.path=f.content[0].path;
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_OK); expect_payload(&f,0);
  anygm_unload(f.engine);
  assert(!rename(parked,f.anchor)); f.source.path=f.anchor;
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_OK); expect_payload(&f,2);
  anygm_unload(f.engine);
  /* A same-size alteration is not a hit. A failed repair publishes no image. */
  f.bytes[2][f.size[2]-1]^=1;
  write_file(cached,f.bytes[2],f.size[2]);
  f.bytes[2][f.size[2]-1]^=1;
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_ERROR_INVALID_CONTENT);
  assert(!f.engine->win.data);
  f.engine->host.path_rename=rename_without_replacement;
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_OK); expect_payload(&f,2);
  anygm_unload(f.engine);
  write_text(cached,"truncated");
  assert(anygm_load(f.engine,&f.source,NULL)==ANYGM_OK); expect_payload(&f,2);
  teardown(&f); return 1;
}

int main(int argc,char **argv){
  static const AnygmTestCase cases[]={
    {"ordered_lifecycle",ordered_lifecycle},
    {"dependency_rejection",dependency_rejection},
    {"memory_dependency_rejection",memory_dependency_rejection},
    {"cache_reuse_and_repair",cache_reuse_and_repair},
  };
  const AnygmTestGroup group={"engine_content_patch",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result={0};
  int ok=anygm_test_run_groups(&group,1,argc>1?argv[1]:NULL,&result);
  printf("engine content patches: %d passed, %d failed\n",result.passed,result.failed);
  return ok?0:1;
}
