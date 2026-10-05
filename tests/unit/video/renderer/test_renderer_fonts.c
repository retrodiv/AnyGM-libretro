/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"
#include "gml_render_cpu_text.h"
#include "gml_render_text_internal.h"
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

static int draw_font(FontFixture *f,int font);

/* Neutral per-channel coverage control for the classic information-page CPU seam. */
static int classic_subpixel_case(void){
  uint8_t pixels[4]={0,128,255,255};
  GmlAtlas atlas={.px=pixels,.w=1,.h=1};
  GmlFont font={.atlas=0,.subpixel=1};
  GmlGlyph glyph={.w=1,.h=1};
  uint32_t frame=0xFF204060u;
  GmlRender render={0};
  render.atlas=&atlas;
  render.n_atlas=1;
  render.fb=&frame;
  render.fbw=1;
  render.fbh=1;
  render.alphablend=1;
  int ok=gml_render_cpu_classic_info_subpixel_glyph(&render,&font,&glyph,
                                                     0,0,1,1,0x0080C0FFu,1.0) &&
    frame==0xFFFF8060u;
  frame=0xFF204060u;
  pixels[3]=0;
  ok &= gml_render_cpu_classic_info_subpixel_glyph(&render,&font,&glyph,
                                                    0,0,1,1,0x0080C0FFu,1.0) &&
    frame==0xFF204060u;
  ok &= !gml_render_cpu_classic_info_subpixel_glyph(&render,&font,&glyph,
                                                     0,0,1,1,0x0080C0FFu,0.5);
  if(!ok) fprintf(stderr,"classic subpixel glyph coverage changed\n");
  return ok;
}

static int runtime_sprite_font_mapping_case(void){
  FontFixture *f=fixture_create();
  if(!f) return 0;
  GmlRender *r=&f->render;
  r->spr=(GmlSprite*)calloc(1,sizeof *r->spr);
  if(!r->spr){ fixture_destroy(f); return 0; }
  r->n_spr=1;
  GmlSprite *sprite=&r->spr[0];
  sprite->w=sprite->h=2;
  sprite->n_frames=2;
  sprite->runtime_opaque=0;
  sprite->runtime_rgba=(uint8_t*)calloc(32u,1u);
  if(!sprite->runtime_rgba){ fixture_destroy(f); return 0; }
  sprite->runtime_rgba[0]=255u;
  sprite->runtime_rgba[3]=255u;
  sprite->runtime_rgba[16u+1u]=255u;
  sprite->runtime_rgba[16u+3u]=255u;
  int plain=gml_font_add_sprite(r,0,'A',0,0);
  int mapped=gml_font_add_sprite_ext(r,0,"BA",0,0);
  int ok=plain==0 && mapped==1 &&
    glyph_frame(&r->fonts[plain],'A')==0 &&
    glyph_frame(&r->fonts[mapped],'B')==0 &&
    glyph_frame(&r->fonts[mapped],'A')==1 &&
    draw_font(f,plain)>0 && f->frame[2u+2u*96u]==0xFFFF0000u &&
    f->frame[3u+2u*96u]==0u;
  sprite->runtime_rgba[0]=0u;
  sprite->runtime_rgba[1]=255u;
  ok &=draw_font(f,plain)>0 && f->frame[2u+2u*96u]==0xFF00FF00u &&
    f->frame[3u+2u*96u]==0u;
  gml_font_delete(r,plain);
  gml_font_delete(r,mapped);
  ok &=sprite->runtime_rgba && !r->fonts[plain].map && !r->fonts[mapped].map;
  if(!ok) fprintf(stderr,"runtime sprite-font mapping/current-source control failed\n");
  fixture_destroy(f);
  return ok;
}

static int runtime_renderer_reinit_generation_case(void){
  FontFixture *f=fixture_create();
  if(!f) return 0;
  int first=add_font(f);
  GmlRenderResourceSource old_page={0},new_page={0};
  int ink=first>=0?draw_font(f,first):0;
  uint32_t before[96u*48u];
  memcpy(before,f->frame,sizeof before);
  int ok=first>=0 && ink>0 &&
    gml_render_atlas_scene_source_view(&f->render,f->render.fonts[first].atlas,
                                       &old_page);
  gml_render_free(&f->render);
  ok &=gml_render_init(&f->render,&f->win)==0;
  int second=add_font(f);
  ok &=second>=0 &&
    gml_render_atlas_scene_source_view(&f->render,f->render.fonts[second].atlas,
                                       &new_page) &&
    new_page.image.incarnation!=old_page.image.incarnation &&
    new_page.image.content_version==1u &&
    draw_font(f,second)==ink && !memcmp(before,f->frame,sizeof before);
  if(!ok) fprintf(stderr,"runtime font renderer-reinit generation control failed\n");
  fixture_destroy(f);
  return ok;
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
  GmlRenderResourceSource source={0},grown={0},reused_source={0};
  GmlRenderFontMetrics metrics={0};
  int ink=font>=0?draw_font(f,font):0;
  uint32_t before[96*48];
  memcpy(before,f->frame,sizeof before);
  int ok=font==0 && gml_render_font_exists(&f->render,font) &&
    gml_render_font_metrics(&f->render,font,&metrics) &&
    !metrics.sprite_backed && metrics.glyph_count==1 && ink>0 &&
    f->render.fonts[font].atlas==0 && f->render.atlas[0].runtime_font_page &&
    gml_render_atlas_scene_source_view(&f->render,0,&source) && source.bytes &&
    !source.borrowed_immutable && source.image.content_version==1u;
  ok &= real_glyph_demand(&f->render,&f->render.fonts[font],'B')!=NULL &&
    gml_render_atlas_scene_source_view(&f->render,0,&grown) &&
    grown.image.incarnation==source.image.incarnation &&
    grown.image.content_version==source.image.content_version+1u &&
    grown.bytes==f->render.atlas[0].px;
  gml_font_delete(&f->render,font);
  ok &= !gml_render_font_exists(&f->render,font) &&
    !f->render.fonts[0].runtime_face && !f->render.fonts[0].glyphs &&
    f->render.n_atlas==1 && !f->render.atlas[0].px;
  int reused=add_font(f);
  ok &= reused==1 && reused>=0 && f->render.fonts[reused].atlas==0 &&
    f->render.atlas[0].runtime_font_page &&
    gml_render_atlas_scene_source_view(&f->render,0,&reused_source) &&
    !reused_source.borrowed_immutable &&
    reused_source.image.incarnation!=source.image.incarnation &&
    reused_source.image.content_version==1u &&
    draw_font(f,reused)==ink && !memcmp(before,f->frame,sizeof before);
  gml_font_delete(&f->render,reused);
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
  GmlRenderResourceSource original_page={0},restored_page={0};
  uint32_t before[96*48];
  memcpy(before,f->frame,sizeof before);
  int width=gml_text_width(&f->render,"A");
  size_t size=0,used=0;
  uint8_t *state=save_font_state(f,&size);
  int ready=font==0 && ink>0 && width>0 && state &&
    gml_render_atlas_scene_source_view(&f->render,f->render.fonts[font].atlas,
                                       &original_page);
  gml_font_delete(&f->render,font);
  int deleted=!gml_render_font_exists(&f->render,font);
  int loaded=ready && gml_render_state_load(&f->render,state,size,&used);
  GmlRenderFontMetrics metrics={0};
  int usable=loaded && used==size && gml_render_font_exists(&f->render,font) &&
    gml_render_font_metrics(&f->render,font,&metrics) && !metrics.sprite_backed &&
    metrics.glyph_count==1 && f->render.fonts[font].runtime_face &&
    gml_render_atlas_scene_source_view(&f->render,f->render.fonts[font].atlas,
                                       &restored_page) &&
    restored_page.image.incarnation!=original_page.image.incarnation &&
    restored_page.image.content_version==1u;
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

static int runtime_font_capacity_case(void){
  FontFixture *f=fixture_create();
  if(!f) return 0;
  f->render.n_fonts=GML_MAX_FONTS;
  int refused=add_font(f);
  int ok=refused==-1 && f->render.n_fonts==GML_MAX_FONTS &&
    f->render.n_atlas==0 && !f->render.atlas;
  f->render.n_fonts=0;
  int accepted=add_font(f);
  ok &= accepted==0 && draw_font(f,accepted)>0 &&
    f->render.n_atlas==1 && f->render.atlas[0].runtime_font_page;
  if(!ok) fprintf(stderr,"runtime font capacity or recovery changed\n");
  fixture_destroy(f);
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

typedef struct { int calls,refuse; } GlyphShaderProbe;

/* Authored host stand-in: copy the selected source, without claiming device work. */
static int glyph_shader_copy(void *context,const GmlRenderShaderRequest *request){
  GlyphShaderProbe *probe=context;
  probe->calls++;
  if(probe->refuse) return 0;
  if(!request || !request->source || !request->output ||
     request->width<=0 || request->height<=0) return 0;
  for(int y=0;y<request->height;y++) for(int x=0;x<request->width;x++){
    int sx=request->region_x+x*request->region_width/request->width;
    int sy=request->region_y+y*request->region_height/request->height;
    request->output[y*request->width+x]=request->source[sy*request->source_pitch+sx];
  }
  return 1;
}

/* Compare complete targets, not just counters, at the shared glyph dispatch. */
static int dispatch_glyph_family_case(void){
  enum { REAL,REAL_ROTATED,REAL_FILTERED,REAL_SUBPIXEL,REAL_SDF,
         ATLAS,ATLAS_ROTATED,RUNTIME,RUNTIME_ROTATED,EXTENDED,
         SPACE,CLIPPED,TRANSPARENT,ZERO_ALPHA,DISCARDED,MEASURE,
         CLASSIC_PHASE,SHADER_DECLINED,NO_SCENE,SHADER_GLYPH,SHADER_RUN,
         SHADER_RUN_REFUSED,SHADER_GLYPH_REFUSED,SHADER_RUN_TRANSPARENT,GLYPH_CASES };
  int ok=1;
  for(int kind=0;kind<GLYPH_CASES;kind++){
    uint32_t images[2][96*48],phases[2][96*48];
    for(int enabled=0;enabled<2;enabled++){
      FontFixture *f=fixture_create();
      if(!f) return 0;
      GmlRender *r=&f->render;
      struct GmlShaderPal shader={0};
      GlyphShaderProbe probe={0};
      r->atlas=calloc(1,sizeof *r->atlas);
      r->tpag=calloc(1,sizeof *r->tpag);
      r->spr=calloc(1,sizeof *r->spr);
      if(!r->atlas || !r->tpag || !r->spr){ fixture_destroy(f); return 0; }
      r->n_atlas=r->n_tpag=r->n_spr=1;
      GmlAtlas *atlas=&r->atlas[0];
      GmlTpag *page=&r->tpag[0];
      GmlSprite *sprite=&r->spr[0];
      atlas->w=atlas->h=2;
      atlas->px=malloc(16);
      sprite->frame=malloc(sizeof *sprite->frame);
      r->fonts[0].glyphs=calloc(1,sizeof *r->fonts[0].glyphs);
      if(!atlas->px || !sprite->frame || !r->fonts[0].glyphs){ fixture_destroy(f); return 0; }
      for(int pixel=0;pixel<4;pixel++){
        atlas->px[pixel*4]=(uint8_t)(90+pixel*20);
        atlas->px[pixel*4+1]=120;
        atlas->px[pixel*4+2]=180;
        atlas->px[pixel*4+3]=kind==TRANSPARENT || kind==SHADER_RUN_TRANSPARENT?0:255;
      }
      *sprite->frame=0;
      sprite->w=sprite->h=2; sprite->n_frames=1;
      page->sw=page->sh=page->bw=page->bh=page->tw=page->th=2;
      page->atlas=0;
      GmlFont *font=&r->fonts[0];
      font->real=kind!=ATLAS && kind!=ATLAS_ROTATED &&
                 kind!=RUNTIME && kind!=RUNTIME_ROTATED && kind!=SPACE;
      font->sprite=0; font->first='A'; font->sep=1;
      font->atlas=0; font->n_glyphs=1; font->line_height=3;
      font->glyphs[0]=(GmlGlyph){.ch='A',.w=2,.h=2,.shift=4};
      font->subpixel=kind==REAL_SUBPIXEL;
      font->sdf_spread=kind==REAL_SDF?1:0;
      if(kind==RUNTIME || kind==RUNTIME_ROTATED){
        sprite->runtime_rgba=malloc(16);
        if(!sprite->runtime_rgba){ fixture_destroy(f); return 0; }
        memcpy(sprite->runtime_rgba,atlas->px,16);
      }
      for(int pixel=0;pixel<96*48;pixel++)
        f->frame[pixel]=phases[enabled][pixel]=0xFF182838u;
      gml_render_dispatch_diagnostics_enable(r,enabled);
      gml_render_begin(r,f->frame,96,48,0,0);
      r->font=0; r->n_fonts=1; r->active_shader=kind==SHADER_DECLINED?0:-1;
      r->color_write_mask=15; r->blend_equation=r->blend_equation_alpha=1;
      r->interp=kind==REAL_FILTERED;
      if(kind>=SHADER_GLYPH){
        shader.source_vertex_es="attribute vec3 in_Position; void main(){ gl_Position=vec4(in_Position,1.0); }";
        shader.source_fragment_es="void main(){ gl_FragColor=vec4(1.0); }";
        shader.position_dependent=kind==SHADER_RUN || kind==SHADER_RUN_REFUSED || kind==SHADER_RUN_TRANSPARENT;
        probe.refuse=kind==SHADER_RUN_REFUSED || kind==SHADER_GLYPH_REFUSED;
        r->shader_pal=&shader; r->n_shader_pal=1; r->active_shader=0;
        gml_render_set_shader_executor(r,glyph_shader_copy,&probe);
      }
      if(kind==ZERO_ALPHA) r->alpha=0;
      if(kind==DISCARDED){ r->alpha_test_enable=1; r->alpha_test_ref=255; }
      if(kind==CLASSIC_PHASE){
        r->classic=1; r->classic_phase_y=phases[enabled];
      }
      if(kind==NO_SCENE) r->device_bound=1;
      double x=kind==CLIPPED?-100:12;
      double rotation=kind==REAL_ROTATED || kind==ATLAS_ROTATED ||
                      kind==RUNTIME_ROTATED?23:0;
      if(kind==MEASURE){
        ok &=gml_text_width(r,"AA")>0 && gml_text_height(r,"AA")>0;
      }else if(kind==EXTENDED){
        gml_draw_text_ext(r,x,12,"A\nA",5,20);
      }else{
        gml_draw_text_transformed(r,x,12,kind==SPACE?"  ":"AA",
                                  1,1,rotation,r->color,r->alpha);
      }
      gml_render_flush_rotated_batch(r);
      GmlRenderDispatchMetrics metrics;
      unsigned reason=kind==NO_SCENE?GML_RENDER_DISPATCH_NO_SCENE:GML_RENDER_DISPATCH_NO_DEVICE;
      uint64_t expected=kind==SPACE || kind==CLIPPED || kind==TRANSPARENT ||
                        kind==ZERO_ALPHA || kind==DISCARDED || kind==MEASURE || kind==SHADER_RUN_TRANSPARENT?0u:
                        kind==SHADER_RUN || kind==SHADER_RUN_REFUSED?3u:2u;
      uint64_t cpu=0,gpu=0,exceptions=0;
      ok &=gml_render_dispatch_metrics(r,&metrics);
      for(unsigned family=0;family<GML_RENDER_SCENE_FAMILY_COUNT;family++){
        gpu+=metrics.gpu[family];
        for(unsigned why=0;why<GML_RENDER_DISPATCH_REASON_COUNT;why++) cpu+=metrics.cpu[family][why];
        for(unsigned why=0;why<GML_RENDER_DISPATCH_EXCEPTION_COUNT;why++) exceptions+=metrics.exceptions[family][why];
      }
      int counted=cpu==(enabled?expected:0u) &&
        metrics.cpu[GML_RENDER_SCENE_TEXT][reason]==(enabled?expected:0u) &&
        !gpu && !exceptions && !metrics.failed && !metrics.unclassified &&
        !metrics.complete && !r->dispatch_cpu_depth;
      if(!counted) fprintf(stderr,"glyph dispatch kind=%d enabled=%d cpu=%llu text=%llu wanted=%llu\n",
        kind,enabled,(unsigned long long)cpu,
        (unsigned long long)metrics.cpu[GML_RENDER_SCENE_TEXT][reason],
        (unsigned long long)(enabled?expected:0u));
      ok &=counted;
      if(kind>=SHADER_GLYPH){
        int calls=kind==SHADER_GLYPH_REFUSED?2:1;
        if(probe.calls!=calls) fprintf(stderr,"glyph shader kind=%d calls=%d wanted=%d\n",kind,probe.calls,calls);
        ok &=probe.calls==calls && !r->run_plane_active && !r->shade_vertex_colour_valid;
      }
      int changed=0;
      for(int pixel=0;pixel<96*48;pixel++) changed|=f->frame[pixel]!=0xFF182838u;
      if(changed!=(expected!=0)) fprintf(stderr,"glyph dispatch kind=%d fixture coverage=%d wanted=%d\n",
                                            kind,changed,expected!=0);
      ok &=changed==(expected!=0);
      memcpy(images[enabled],f->frame,sizeof f->frame);
      r->classic_phase_y=NULL; r->device_bound=0;
      r->shader_pal=NULL; r->n_shader_pal=0;
      fixture_destroy(f);
    }
    ok &= !memcmp(images[0],images[1],sizeof images[0]) &&
          !memcmp(phases[0],phases[1],sizeof phases[0]);
  }
  if(!ok) fprintf(stderr,"shared glyph dispatch accounting changed family, coverage or RGBA\n");
  return ok;
}

int main(int argc,char **argv){
  const char *filter=NULL;
  if(argc==3 && !strcmp(argv[1],"--case")) filter=argv[2];
  else if(argc!=1){ fprintf(stderr,"usage: %s [--case filter]\n",argv[0]); return 2; }
  const AnygmTestCase cases[]={
    {"dispatch_glyph_family",dispatch_glyph_family_case},
    {"classic_subpixel_channels",classic_subpixel_case},
    {"runtime_sprite_font_mapping",runtime_sprite_font_mapping_case},
    {"runtime_renderer_reinit_generation",runtime_renderer_reinit_generation_case},
    {"runtime_lifecycle",lifecycle_case},
    {"restore_deleted_runtime_font",restore_deleted_case},
    {"restore_removes_later_runtime_font",restore_removes_later_case},
    {"lazy_glyph_restore_and_atlas_reuse",lazy_glyph_restore_case},
    {"changed_source_rejected",changed_source_case},
    {"runtime_font_capacity",runtime_font_capacity_case},
    {"style_restore_preserves_raster",restore_styles_case},
    {"malformed_record_rejected",malformed_record_case}
  };
  const AnygmTestGroup group={"fonts",cases,sizeof cases/sizeof cases[0]};
  AnygmTestResult result;
  anygm_test_run_groups(&group,1,filter,&result);
  printf("renderer fonts: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed || !result.matched?EXIT_FAILURE:EXIT_SUCCESS;
}
