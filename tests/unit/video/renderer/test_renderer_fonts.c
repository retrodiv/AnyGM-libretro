/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"
#include "gml_render_state.h"
#include "anygm_test_runner.h"
#include "memory_vfs.h"
#include "../../media/font_test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  AnygmMemoryVfs memory;
  AnygmHostServices host;
  GmlWin win;
  GmlRender render;
  uint32_t frame[96*48];
} FontFixture;

static FontFixture *fixture_create(void){
  FontFixture *f=calloc(1,sizeof(*f));
  if(!f) return NULL;
  anygm_memory_vfs_init(&f->memory,&f->host);
  f->win.host=&f->host;
  snprintf(f->win.content_dir,sizeof f->win.content_dir,"/content");
  f->render.win=&f->win;
  f->render.font=-1;
  f->render.default_font.sprite=-1;
  f->render.alpha=1;
  f->render.color=0xFFFFFF;
  f->render.alphablend=1;
  f->render.circle_precision=24;
  f->render.app_draw_enable=1;
  f->render.next_surface_id=1;
  f->render.software_overlay=1;
  for(int i=0;i<GML_MAX_FONTS;i++){
    f->render.fonts[i].sprite=-1;
    f->render.fonts[i].atlas=-1;
  }
  uint8_t bytes[552];
  size_t size=font_fixture_build(bytes);
  /* Map B to the same authored rectangle, outside the initial font_add range. */
  bytes[124+18+66]=1;
  if(!anygm_memory_vfs_add_file(&f->memory,"/content/fixture.ttf",bytes,size)){
    anygm_memory_vfs_destroy(&f->memory);
    free(f);
    return NULL;
  }
  return f;
}

static void fixture_destroy(FontFixture *f){
  if(!f) return;
  gml_render_free(&f->render);
  anygm_memory_vfs_destroy(&f->memory);
  free(f);
}

static int add_font(FontFixture *f){
  return gml_font_add_file(&f->render,"/content/fixture.ttf",32,0,0,65,65);
}

static int draw_font(FontFixture *f,int font){
  memset(f->frame,0,sizeof f->frame);
  gml_render_begin(&f->render,f->frame,96,48,0,0);
  f->render.font=font;
  gml_draw_text(&f->render,2,2,"A");
  int ink=0;
  for(size_t i=0;i<sizeof f->frame/sizeof f->frame[0];i++)
    if(f->frame[i]&0xFFFFFF) ink++;
  return ink;
}

static uint8_t *save_font_state(FontFixture *f,size_t *size){
  *size=gml_render_state_size(&f->render,0);
  uint8_t *bytes=*size?malloc(*size):NULL;
  size_t written=0;
  if(!bytes || !gml_render_state_save(&f->render,0,bytes,*size,&written) || written!=*size){
    free(bytes);
    return NULL;
  }
  return bytes;
}

static int lifecycle_case(void){
  FontFixture *f=fixture_create();
  if(!f) return 0;
  int font=add_font(f);
  GmlRenderFontMetrics metrics={0};
  int ok=font==0 && gml_render_font_exists(&f->render,font) &&
    gml_render_font_metrics(&f->render,font,&metrics) &&
    !metrics.sprite_backed && metrics.glyph_count==1 && draw_font(f,font)>0;
  gml_font_delete(&f->render,font);
  ok &= !gml_render_font_exists(&f->render,font) &&
    !f->render.fonts[0].runtime_face && !f->render.fonts[0].glyphs &&
    f->render.n_atlas==1 && !f->render.atlas[0].px;
  if(!ok) fprintf(stderr,"runtime font construction/deletion control failed\n");
  fixture_destroy(f);
  return ok;
}

/* This is the current-state contract, not an original-runner rendering oracle. A font that
 * existed at save time must remain usable after deletion and restoration in the same run. */
static int restore_deleted_case(void){
  FontFixture *f=fixture_create();
  if(!f) return 0;
  int font=add_font(f),ink=font>=0?draw_font(f,font):0;
  uint32_t before[96*48];
  memcpy(before,f->frame,sizeof before);
  int width=gml_text_width(&f->render,"A");
  size_t size=0,used=0;
  uint8_t *state=save_font_state(f,&size);
  int ready=font==0 && ink>0 && width>0 && state;
  gml_font_delete(&f->render,font);
  int deleted=!gml_render_font_exists(&f->render,font);
  int loaded=ready && gml_render_state_load(&f->render,state,size,&used);
  GmlRenderFontMetrics metrics={0};
  int usable=loaded && used==size && gml_render_font_exists(&f->render,font) &&
    gml_render_font_metrics(&f->render,font,&metrics) && !metrics.sprite_backed &&
    metrics.glyph_count==1 && f->render.fonts[font].runtime_face;
  int ok=ready && deleted && usable;
  if(usable){
    ok &= draw_font(f,font)==ink && !memcmp(before,f->frame,sizeof before) &&
      gml_text_width(&f->render,"A")==width;
    size_t repeated_size=0;
    uint8_t *repeated=save_font_state(f,&repeated_size);
    ok &= repeated && repeated_size==size && !memcmp(state,repeated,size);
    free(repeated);
  }
  if(!ok)
    fprintf(stderr,"deleted runtime font restore: ready=%d deleted=%d loaded=%d "
      "usable=%d glyphs=%d sprite_backed=%d bytes=%zu/%zu\n",
      ready,deleted,loaded,usable,metrics.glyph_count,metrics.sprite_backed,used,size);
  free(state);
  fixture_destroy(f);
  return ok;
}

static int restore_removes_later_case(void){
  FontFixture *f=fixture_create();
  if(!f) return 0;
  size_t size=0,used=0;
  uint8_t *state=save_font_state(f,&size);
  int font=add_font(f);
  int ready=state && font==0 && draw_font(f,font)>0;
  int loaded=ready && gml_render_state_load(&f->render,state,size,&used);
  int retained=0;
  for(int i=0;i<GML_MAX_FONTS;i++)
    if(f->render.fonts[i].runtime_owned || f->render.fonts[i].runtime_face ||
       f->render.fonts[i].glyphs) retained++;
  int atlas_live=0;
  for(int i=0;i<f->render.n_atlas;i++) if(f->render.atlas[i].px) atlas_live++;
  int ok=ready && loaded && used==size && f->render.n_fonts==0 &&
    !gml_render_font_exists(&f->render,font) && retained==0 && atlas_live==0;
  if(!ok)
    fprintf(stderr,"later runtime font restore: ready=%d loaded=%d count=%d "
      "retained=%d atlas_live=%d bytes=%zu/%zu\n",
      ready,loaded,f->render.n_fonts,retained,atlas_live,used,size);
  free(state);
  fixture_destroy(f);
  return ok;
}

static int lazy_glyph_restore_case(void){
  FontFixture *f=fixture_create();
  if(!f) return 0;
  int font=add_font(f);
  f->render.font=font;
  int width=font>=0?gml_text_width(&f->render,"AB"):0;
  size_t size=0;
  uint8_t *state=save_font_state(f,&size);
  int ok=font==0 && state && width>0 && f->render.fonts[font].n_glyphs==2;
  for(int cycle=0;ok && cycle<12;cycle++){
    gml_font_delete(&f->render,font);
    size_t used=0,repeated_size=0;
    ok=gml_render_state_load(&f->render,state,size,&used) && used==size &&
      gml_text_width(&f->render,"AB")==width && f->render.n_atlas==1 &&
      f->render.fonts[font].n_glyphs==2 && f->render.fonts[font].glyphs[1].ch=='B';
    uint8_t *repeated=ok?save_font_state(f,&repeated_size):NULL;
    ok &= repeated && repeated_size==size && !memcmp(state,repeated,size);
    free(repeated);
  }
  if(!ok) fprintf(stderr,"lazy font glyph replay or repeated atlas reuse failed\n");
  free(state); fixture_destroy(f);
  return ok;
}

static int changed_source_case(void){
  FontFixture *f=fixture_create();
  if(!f) return 0;
  int font=add_font(f);
  size_t size=0,used=0;
  uint8_t *state=save_font_state(f,&size);
  int ok=font==0 && state;
  gml_font_delete(&f->render,font);
  ok &= anygm_memory_vfs_xor_byte(&f->memory,"/content/fixture.ttf",0,1);
  ok &= !gml_render_state_load(&f->render,state,size,&used) &&
    !gml_render_font_exists(&f->render,font);
  if(!ok) fprintf(stderr,"changed runtime font source was not rejected\n");
  free(state); fixture_destroy(f);
  return ok;
}

static int restore_styles_case(void){
  FontFixture *f=fixture_create();
  if(!f) return 0;
  int ok=1,ink=0,width=0;
  uint32_t baseline[96*48]={0};
  for(int i=0;ok && i<4;i++){
    int id=gml_font_add_file(&f->render,"/content/fixture.ttf",32,i&1,i>>1,65,65);
    int drawn=id>=0?draw_font(f,id):0;
    if(i==0){ ink=drawn; width=gml_text_width(&f->render,"A"); memcpy(baseline,f->frame,sizeof baseline); }
    ok=id==i && ink>0 && drawn==ink && gml_text_width(&f->render,"A")==width &&
      !memcmp(baseline,f->frame,sizeof baseline);
  }
  size_t size=0,used=0,repeated_size=0;
  uint8_t *state=ok?save_font_state(f,&size):NULL;
  for(int i=0;i<4;i++) gml_font_delete(&f->render,i);
  ok=ok && state && gml_render_state_load(&f->render,state,size,&used) && used==size;
  for(int i=0;ok && i<4;i++){
    GmlRenderFontMetrics metrics={0};
    ok=gml_render_font_metrics(&f->render,i,&metrics) && metrics.bold==(i&1) &&
      metrics.italic==(i>>1) && draw_font(f,i)==ink &&
      gml_text_width(&f->render,"A")==width && !memcmp(baseline,f->frame,sizeof baseline);
  }
  uint8_t *repeated=ok?save_font_state(f,&repeated_size):NULL;
  ok=ok && repeated && repeated_size==size && !memcmp(state,repeated,size) && f->render.n_atlas==4;
  if(!ok) fprintf(stderr,"font styles changed pixels, layout or restored metadata\n");
  free(repeated); free(state); fixture_destroy(f);
  return ok;
}

static void write_u32(uint8_t *p,uint32_t value){
  for(int i=0;i<4;i++) p[i]=(uint8_t)(value>>(i*8));
}
static uint32_t read_u32(const uint8_t *p){
  uint32_t value=0;
  for(int i=0;i<4;i++) value|=(uint32_t)p[i]<<(i*8);
  return value;
}

static int malformed_record_case(void){
  FontFixture *f=fixture_create();
  if(!f) return 0;
  int font=add_font(f);
  size_t size=0;
  uint8_t *state=save_font_state(f,&size),*candidate=size?malloc(size):NULL;
  int ok=font==0 && state && candidate && size>=36 && read_u32(state+20)==0;
  if(ok){
    size_t metrics=36+(size_t)read_u32(state+32)+32;
    const size_t offsets[]={0,20,24,28,32,32,metrics,metrics,metrics+4,
                             metrics+8,metrics+20,metrics+20,metrics+24,metrics+24,
                             metrics+12,metrics+16};
    const uint32_t values[]={UINT32_MAX,UINT32_MAX,2,9,0,4096,3,257,UINT32_MAX,
                             65536,0,65537,65536,'B',2,UINT32_MAX};
    if(metrics>size || size-metrics<28) ok=0;
    for(size_t i=0;ok && i<sizeof offsets/sizeof offsets[0];i++){
      memcpy(candidate,state,size);
      write_u32(candidate+offsets[i],values[i]);
      size_t used=0,repeated_size=0;
      int rejected=!gml_render_state_load(&f->render,candidate,size,&used);
      uint8_t *repeated=save_font_state(f,&repeated_size);
      ok=rejected && repeated && repeated_size==size && !memcmp(repeated,state,size);
      if(!ok) fprintf(stderr,"font record mutation %zu at %zu failed rejection/retention\n",i,offsets[i]);
      free(repeated);
    }
  }
  if(!ok) fprintf(stderr,"bounded font-state record control failed\n");
  free(state); free(candidate); fixture_destroy(f);
  return ok;
}

int main(int argc,char **argv){
  const char *filter=NULL;
  if(argc==3 && !strcmp(argv[1],"--case")) filter=argv[2];
  else if(argc!=1){ fprintf(stderr,"usage: %s [--case filter]\n",argv[0]); return 2; }
  const AnygmTestCase cases[]={
    {"runtime_lifecycle",lifecycle_case},
    {"restore_deleted_runtime_font",restore_deleted_case},
    {"restore_removes_later_runtime_font",restore_removes_later_case},
    {"lazy_glyph_restore_and_atlas_reuse",lazy_glyph_restore_case},
    {"changed_source_rejected",changed_source_case},
    {"style_restore_preserves_raster",restore_styles_case},
    {"malformed_record_rejected",malformed_record_case}
  };
  const AnygmTestGroup group={"fonts",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,filter,&result);
  printf("renderer fonts: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed || !result.matched?EXIT_FAILURE:EXIT_SUCCESS;
}
