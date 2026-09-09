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
  return gml_font_add_file(&f->render,"/content/fixture.ttf",32,65,65);
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

int main(int argc,char **argv){
  const char *filter=NULL;
  if(argc==3 && !strcmp(argv[1],"--case")) filter=argv[2];
  else if(argc!=1){ fprintf(stderr,"usage: %s [--case filter]\n",argv[0]); return 2; }
  const AnygmTestCase cases[]={
    {"runtime_lifecycle",lifecycle_case},
    {"restore_deleted_runtime_font",restore_deleted_case},
    {"restore_removes_later_runtime_font",restore_removes_later_case}
  };
  const AnygmTestGroup group={"fonts",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,filter,&result);
  printf("renderer fonts: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed || !result.matched?EXIT_FAILURE:EXIT_SUCCESS;
}
