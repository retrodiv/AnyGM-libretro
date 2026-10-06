/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"
#include "gml_render_primitives.h"
#include "gml_render_cpu_surfaces.h"
#include "gml_builtin.h"
#include "gml_vm_internal.h"
#include "gml_software3d.h"

#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition,label) do{ \
  if(!(condition)){ \
    fprintf(stderr,"renderer surfaces failed: %s\n",label); \
    return 1; \
  } \
}while(0)

static uint64_t pixel_hash_update(uint64_t hash,const uint32_t *pixels,size_t count){
  for(size_t index=0;index<count;index++){
    uint32_t value=pixels[index];
    for(unsigned shift=0;shift<32;shift+=8){
      hash^=(value>>shift)&0xFFu;
      hash*=UINT64_C(1099511628211);
    }
  }
  return hash;
}

static uint64_t pixel_hash(const uint32_t *pixels,size_t count){
  return pixel_hash_update(UINT64_C(1469598103934665603),pixels,count);
}

static uint32_t destination_colour_expected(uint32_t source,uint32_t destination,
                                             int source_factor,int destination_factor){
  uint32_t result=0;
  for(unsigned shift=0;shift<32;shift+=8){
    unsigned s=(source>>shift)&255u,d=(destination>>shift)&255u;
    unsigned sf=source_factor==7?destination>>24:d;
    unsigned df=destination_factor==3?s:d;
    unsigned value=(s*sf+d*df+127u)/255u;
    if(value>255u) value=255u;
    result|=value<<shift;
  }
  return result;
}

enum { DISPATCH_SURFACE_CONTROLS=19 };
static int dispatch_surface_control(int kind,int diagnostics,uint64_t *hash,
                                     GmlRenderDispatchMetrics *metrics){
  GmlRender render={0};
  int extent=(kind==0 || kind==1 || kind==4 || kind==5 || kind==14)?16:4;
  int target_extent=kind==18?128:16;
  uint32_t *target=(uint32_t*)calloc((size_t)target_extent*target_extent,sizeof(*target));
  REQUIRE(target && gml_render_init(&render,NULL)==0,"dispatch surface fixture allocation");
  gml_render_begin(&render,target,target_extent,target_extent,0,0);
  int source=gml_surface_create(&render,extent,extent);
  REQUIRE(source>0,"dispatch surface source allocation");
  uint32_t *pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(pixels!=NULL,"dispatch surface source access");
  unsigned alpha=(kind==10 || kind==18)?128u:(kind==13 || kind==15)?0u:255u;
  for(int index=0;index<extent*extent;index++)
    pixels[index]=(alpha<<24)|((unsigned)(index*773u+0x123456u)&0xffffffu);
  render.surface[source-1].opaque_known=1;
  render.surface[source-1].all_opaque=alpha==255u;
  render.surface[source-1].all_transparent=alpha==0u;
  if(kind==9) REQUIRE(gml_surface_set_target(&render,source),"self-composition target");
  if(kind==10){render.alpha_test_enable=1;render.alpha_test_ref=254;}
  if(kind==14) render.color_write_mask=7u;
  if(kind==15) render.blendmode=GML_RENDER_BLEND_DEST_SOURCE_COLOUR;
  if(kind==16) render.blendmode=3;
  if(kind==17) render.blendmode=6;
  render.interp=kind==2;
  gml_render_dispatch_diagnostics_enable(&render,diagnostics);
  double width=extent,height=extent,x=0,y=0,draw_alpha=1.0;
  if(kind==1){x=y=5;}
  if(kind==2 || kind==3){width=height=12;x=y=2;}
  if(kind==4){width=height=13;}
  if(kind==5){width=height=7;}
  if(kind==6 || kind==16 || kind==17){width=height=9;draw_alpha=0.5;}
  if(kind==7){width=-9;height=9;x=7;}
  if(kind==11){x=y=target_extent;}
  if(kind==12) draw_alpha=0.0;
  if(kind==18){width=height=target_extent;}
  if(kind==8)
    gml_render_cpu_draw_surface_ext_mapped(&render,source,8,8,2,2,45,0xffffffu,0.5);
  else
    draw_surface_region(&render,source,0,0,extent,extent,x,y,width,height,0xffffffu,draw_alpha);
  REQUIRE(gml_render_dispatch_metrics(&render,metrics) && !render.dispatch_cpu_depth,
          "surface dispatch snapshot or scope cleanup");
  *hash=pixel_hash(render.fb,(size_t)render.fbw*render.fbh);
  if(kind==9) gml_surface_reset_target(&render);
  gml_surface_free(&render,source);
  gml_render_free(&render);
  free(target);
  return 0;
}
static int dispatch_surface_coverage_case(void){
  for(int kind=0;kind<DISPATCH_SURFACE_CONTROLS;kind++){
    uint64_t baseline=0,observed=0;
    GmlRenderDispatchMetrics disabled={0},enabled={0};
    REQUIRE(!dispatch_surface_control(kind,0,&baseline,&disabled) &&
            !dispatch_surface_control(kind,1,&observed,&enabled),"surface dispatch control");
    unsigned expected=kind>=10 && kind<=13?0u:1u;
    if(baseline!=observed ||
       disabled.cpu[GML_RENDER_SCENE_SURFACE][GML_RENDER_DISPATCH_NO_DEVICE]!=0u ||
       enabled.cpu[GML_RENDER_SCENE_SURFACE][GML_RENDER_DISPATCH_NO_DEVICE]!=expected ||
       enabled.complete){
      fprintf(stderr,"surface dispatch control %d: count=%llu expected=%u pixels_equal=%d\n",
        kind,(unsigned long long)enabled.cpu[GML_RENDER_SCENE_SURFACE][GML_RENDER_DISPATCH_NO_DEVICE],
        expected,baseline==observed);
      return 1;
    }
  }
  return 0;
}

static int destination_colour_factor_case(void){
  GmlRender render={0}; GmlVM vm={0}; uint32_t base[16]={0};
  const int pairs[3][2]={{9,3},{9,9},{7,9}};
  const unsigned alphas[3]={0u,64u,255u};
  render.next_surface_id=1;
  gml_render_begin(&render,base,4,4,0,0);
  render.alphablend=1; render.active_shader=-1;
  render.color_write_mask=0x0Fu;
  render.blend_equation=render.blend_equation_alpha=1;
  vm.render=&render;
  int source=gml_surface_create(&render,1,1),target=gml_surface_create(&render,4,4);
  REQUIRE(source>0 && target>0,"destination-colour factor surfaces allocated");
  for(int pair=0;pair<3;pair++) for(int coverage=0;coverage<3;coverage++){
    uint32_t authored=(alphas[coverage]<<24)|0x004080C0u;
    uint32_t destination=0x605078A0u;
    uint32_t expected=destination_colour_expected(authored,destination,
                                                 pairs[pair][0],pairs[pair][1]);
    *surface_pixels(&render,source,NULL,NULL)=authored;
    render.surface[source-1].opaque_known=0;
    render.surface[source-1].all_transparent=0;
    REQUIRE(gml_surface_set_target(&render,target),"factor target bound");
    GmlVal factors[2]={vreal(pairs[pair][0]),vreal(pairs[pair][1])};
    (void)gml_builtin_call(&vm,"gpu_set_blendmode_ext",factors,2);
    for(int raster=0;raster<3;raster++){
      for(int index=0;index<16;index++) render.fb[index]=destination;
      if(raster==0) gml_draw_surface_ext(&render,source,0,0,1,1,0,0xFFFFFFu,1);
      else if(raster==1) gml_draw_surface_stretched(&render,source,0,0,2,2,0xFFFFFFu,1);
      else gml_draw_surface_ext(&render,source,0,2,1,1,90,0xFFFFFFu,1);
      if(render.fb[0]!=expected){
        fprintf(stderr,"destination-colour factors (%d,%d), alpha %u, raster %d: "
                "%08x != %08x\n",pairs[pair][0],pairs[pair][1],alphas[coverage],
                raster,render.fb[0],expected);
        gml_surface_reset_target(&render); gml_render_free(&render); return 1;
      }
    }
    render.fb[0]=destination;
    render.alpha=alphas[coverage]/255.0;
    GmlVal point[3]={vreal(0),vreal(0),vreal(0xC08040)};
    (void)gml_builtin_call(&vm,"draw_point_colour",point,3);
    REQUIRE(render.fb[0]==expected,"point uses the same destination-dependent factors");
    render.fb[0]=destination;
    gml_render_primitive_rectangle_color(&render,0,0,0,0,
      0xC08040u,0xC08040u,0xC08040u,0xC08040u,0);
    REQUIRE(render.fb[0]==expected,"colour rectangle uses destination-dependent factors");
    render.fb[0]=destination;
    render.alpha_test_enable=1; render.alpha_test_ref=254;
    gml_draw_surface_ext(&render,source,0,0,1,1,0,0xFFFFFFu,1);
    REQUIRE(render.fb[0]==(alphas[coverage]==255u?expected:destination),
            "explicit alpha testing admits fragments before factor blending");
    render.alpha_test_enable=0;
    render.fb[0]=destination;
    render.color_write_mask=1u;
    gml_draw_surface_ext(&render,source,0,0,1,1,0,0xFFFFFFu,1);
    REQUIRE(render.fb[0]==((expected&0x00FF0000u)|(destination&0xFF00FFFFu)),
            "destination factors preserve disabled colour channels");
    render.color_write_mask=0x0Fu;
    render.fb[0]=destination;
    render.alphablend=0;
    gml_draw_surface_ext(&render,source,0,0,1,1,0,0xFFFFFFu,1);
    uint32_t disabled=render.fb[0];
    int active_mode=render.blendmode;
    render.blendmode=0; render.fb[0]=destination;
    gml_draw_surface_ext(&render,source,0,0,1,1,0,0xFFFFFFu,1);
    REQUIRE(render.fb[0]==disabled,
            "disabled blending retains the established surface copy policy");
    render.blendmode=active_mode;
    render.alphablend=1;
    gml_surface_reset_target(&render);
  }
  gml_vm_free(&vm);
  gml_render_free(&render);
  return 0;
}

static int destination_colour_sprite_case(void){
  GmlRender render={0}; GmlVM vm={0}; uint32_t base[16]={0};
  GmlWin win={0}; win.bytecode=17;
  GmlSprite sprite={0}; GmlTpag page={0}; GmlAtlas atlas={0};
  uint8_t rgba[4]={64,128,192,64}; int frame=0;
  const int pairs[3][2]={{9,3},{9,9},{7,9}};
  render.next_surface_id=1;
  gml_render_begin(&render,base,4,4,0,0);
  render.win=&win; vm.win=&win;
  render.spr=&sprite; render.n_spr=1; render.tpag=&page; render.n_tpag=1;
  render.atlas=&atlas; render.n_atlas=1;
  sprite.w=sprite.h=1; sprite.n_frames=1; sprite.frame=&frame;
  page.sw=page.sh=page.bw=page.bh=1;
  atlas.px=rgba; atlas.w=atlas.h=1; atlas.decode_attempted=1;
  render.alphablend=1; render.active_shader=-1;
  render.color_write_mask=0x0Fu;
  render.blend_equation=render.blend_equation_alpha=1;
  vm.render=&render;
  int target=gml_surface_create(&render,4,4);
  REQUIRE(target>0 && gml_surface_set_target(&render,target),"sprite factor target bound");
  for(int pair=0;pair<3;pair++) for(int coverage=0;coverage<3;coverage++){
    rgba[3]=coverage?64:0;
    double draw_alpha=coverage==2?0.0:1.0;
    page.alpha_scanned=0;
    GmlVal factors[2]={vreal(pairs[pair][0]),vreal(pairs[pair][1])};
    (void)gml_builtin_call(&vm,"gpu_set_blendmode_ext",factors,2);
    uint32_t expected=destination_colour_expected((coverage==2?0u:(uint32_t)rgba[3]<<24)|0x4080C0u,
                                                 0x605078A0u,pairs[pair][0],pairs[pair][1]);
    for(int raster=0;raster<10;raster++){
      for(int index=0;index<16;index++) render.fb[index]=0x605078A0u;
      sprite.runtime_rgba=raster>=4?rgba:NULL;
      render.interp=raster&1;
      int rotated=(raster%4)>=2;
      render.fb_opaque_known=1;
      if(raster>=8) gml_draw_sprite_part_ext(&render,0,0,0,0,1,1,0,0,
                                             raster==9?2:1,raster==9?2:1,
                                             0xFFFFFFu,draw_alpha);
      else gml_draw_sprite_ext(&render,0,0,0,rotated?2:0,1,1,
                               rotated?90:0,0xFFFFFFu,draw_alpha);
      if(render.fb[0]!=expected){
        fprintf(stderr,"sprite destination factors (%d,%d), alpha %u, raster %d: "
                "%08x != %08x\n",pairs[pair][0],pairs[pair][1],rgba[3],raster,
                render.fb[0],expected); return 1;
      }
      for(int index=1;index<16;index++){
        uint32_t outside_expected=raster==9 && (index==1 || index==4 || index==5)?
                                   expected:0x605078A0u;
        if(render.fb[index]==outside_expected) continue;
        fprintf(stderr,"sprite factor quad spill: raster %d, coverage %d, index %d: %08x\n",
                raster,coverage,index,render.fb[index]); return 1;
      }
      REQUIRE(!render.fb_opaque_known,"factor sprites invalidate target coverage metadata");
    }
  }
  gml_surface_reset_target(&render);
  free(page.alpha_row_min); free(page.alpha_row_max);
  render.spr=NULL; render.n_spr=0; render.tpag=NULL; render.n_tpag=0;
  render.atlas=NULL; render.n_atlas=0;
  gml_vm_free(&vm);
  gml_render_free(&render);
  return 0;
}

static int owned_application_begin_case(void){
  GmlRender render={0};
  GmlRenderApplicationWriteView view={0};
  int width=-1,height=-1;
  render.next_surface_id=1;
  render.target_id=-1;
  REQUIRE(!gml_render_application_surface_owned_extent(&render,&width,&height) &&
          width==0 && height==0,
          "a missing owned application target has no extent");
  REQUIRE(!gml_render_begin_owned_application(&render,0.0,0.0,0),
          "a missing owned application target cannot begin a pass");
  REQUIRE(gml_render_application_surface_ensure_owned(&render,4,3),
          "owned application target allocated");
  REQUIRE(gml_render_application_surface_owned_extent(&render,&width,&height) &&
          width==4 && height==3,
          "metadata selection reports dimensions without a pixel borrow");
  REQUIRE(gml_render_begin_owned_application(&render,2.0,3.0,0),
          "owned application target begins a normal pass");
  REQUIRE(gml_render_application_surface_owned_view(&render,&view) &&
          render.fb==view.pixels && render.fbw==view.width && render.fbh==view.height &&
          render.cam_x==2.0 && render.cam_y==3.0,
          "owned begin retains the established CPU target and camera");
  view.pixels[5]=0xFF123456u;
  int surface=gml_surface_create(&render,2,2);
  REQUIRE(surface>0 && gml_surface_set_target(&render,surface),
          "a numbered surface may suspend the owned application target");
  REQUIRE(gml_render_begin_owned_application(&render,4.0,5.0,1) &&
          gml_surface_get_target(&render)==surface,
          "retaining begin preserves the numbered surface binding");
  REQUIRE(gml_render_application_surface_ensure_owned(&render,6,4),
          "the owned application target may resize while suspended");
  REQUIRE(gml_render_application_surface_owned_extent(&render,&width,&height) &&
          width==6 && height==4,
          "metadata selection observes the new dimensions");
  gml_surface_reset_target(&render);
  REQUIRE(gml_render_application_surface_owned_view(&render,&view) &&
          render.fb==view.pixels && render.fbw==6 && render.fbh==4 &&
          view.pixels[7]==0xFF123456u,
          "target reset returns to the resized owned allocation with retained pixels");
  gml_render_free(&render);
  return 0;
}

static int surface_budget_refusal_case(void){
  GmlRender render={0};
  uint32_t base[4]={0};
  uint32_t *bound;
  int ids[GML_MAX_SURFACES],reused;
  render.next_surface_id=1;
  gml_render_begin(&render,base,2,2,0.0,0.0);
  REQUIRE(gml_surface_create(&render,0,1)==-1 &&
          gml_surface_create(&render,1,0)==-1 &&
          gml_surface_create(&render,4097,1)==-1 &&
          gml_surface_create(&render,1,4097)==-1,
          "invalid and over-limit surface extents refuse before allocation");
  ids[0]=gml_surface_create(&render,2,2);
  REQUIRE(ids[0]>0 && gml_surface_set_target(&render,ids[0]),
          "a bounded surface can become the current target");
  bound=render.fb;
  gml_render_set_pending_fill(&render,0xFF315779u);
  gml_surface_resize(&render,ids[0],0,2);
  gml_surface_resize(&render,ids[0],4097,2);
  REQUIRE(render.fb==bound && render.fbw==2 && render.fbh==2 &&
          render.target_id==ids[0] && render.target_sp==1 && render.pending_fill &&
          gml_surface_width(&render,ids[0])==2 && gml_surface_height(&render,ids[0])==2,
          "refused resize preserves bound storage, stack and deferred fill");
  for(int index=1;index<GML_MAX_SURFACES;index++){
    ids[index]=gml_surface_create(&render,1,1);
    REQUIRE(ids[index]>0,"each bounded surface slot can be allocated once");
  }
  REQUIRE(gml_surface_create(&render,1,1)==-1 && render.fb==bound &&
          render.target_id==ids[0] && render.pending_fill,
          "slot exhaustion refuses without disturbing the active target");
  gml_surface_free(&render,ids[GML_MAX_SURFACES-1]);
  reused=gml_surface_create(&render,1,1);
  REQUIRE(reused==ids[GML_MAX_SURFACES-1] && render.fb==bound &&
          render.target_id==ids[0] && render.pending_fill,
          "a freed slot is reusable without an unbounded pool or target change");
  gml_surface_reset_target(&render);
  REQUIRE(render.fb==base && render.target_id==-1 && render.target_sp==0 &&
          render.surface[ids[0]-1].px[0]==0xFF315779u,
          "the deferred fill still commits exactly after bounded refusals");
  gml_render_free(&render);
  return 0;
}

static int numbered_surface_resize_bound_case(void){
  GmlRender render={0};
  uint32_t base[16]={0};
  uint32_t *pixels;
  int outer,inner;
  render.next_surface_id=1;
  render.active_shader=-1;
  render.color_write_mask=0x0Fu;
  gml_render_begin(&render,base,4,4,0.0,0.0);
  outer=gml_surface_create(&render,2,2);
  inner=gml_surface_create(&render,2,2);
  REQUIRE(outer>0 && inner>0,"bound resize creates two numbered surfaces");
  pixels=surface_pixels(&render,outer,NULL,NULL);
  REQUIRE(pixels!=NULL,"bound resize borrows outer pixels");
  pixels[0]=0xFF123456u;
  pixels[3]=0xFFABCDEFu;
  REQUIRE(gml_surface_set_target(&render,outer) &&
          gml_surface_set_target(&render,inner),
          "bound resize nests outer and inner targets");
  gml_surface_resize(&render,outer,3,3);
  pixels=render.surface[outer-1].px;
  REQUIRE(pixels!=NULL && render.fb==render.surface[inner-1].px &&
          render.target_stack[1].fb==pixels &&
          render.target_stack[1].w==3 && render.target_stack[1].h==3,
          "bound resize updates the suspended target reference");
  gml_surface_reset_target(&render);
  REQUIRE(render.fb==pixels && render.fbw==3 && render.fbh==3 &&
          render.fb[0]==0xFF123456u && render.fb[4]==0xFFABCDEFu &&
          render.fb[8]==0u,
          "bound resize restores the resized outer target and retained pixels");
  gml_surface_resize(&render,outer,4,4);
  pixels=render.surface[outer-1].px;
  REQUIRE(render.fb==pixels && render.fbw==4 && render.fbh==4 &&
          render.fb[0]==0xFF123456u && render.fb[5]==0xFFABCDEFu &&
          render.fb[15]==0u,
          "bound resize updates the live target reference and extent");
  render.fb[15]=0xFF617283u;
  gml_surface_reset_target(&render);
  REQUIRE(render.fb==base && render.fbw==4 && render.fbh==4 &&
          render.surface[outer-1].px[15]==0xFF617283u,
          "bound resize retains subsequent target writes across reset");
  gml_surface_free(&render,inner);
  gml_surface_free(&render,outer);
  gml_render_free(&render);
  return 0;
}

static int numbered_surface_resize_pending_fill_case(void){
  GmlRender render={0};
  uint32_t base[16]={0};
  int outer,inner;
  render.next_surface_id=1;
  render.active_shader=-1;
  render.color_write_mask=0x0Fu;
  gml_render_begin(&render,base,4,4,0.0,0.0);
  outer=gml_surface_create(&render,2,2);
  inner=gml_surface_create(&render,2,2);
  REQUIRE(outer>0 && inner>0 && gml_surface_set_target(&render,outer),
          "pending-fill resize creates and binds numbered surfaces");
  gml_render_set_pending_fill(&render,0xFF214365u);
  REQUIRE(render.pending_fill && gml_surface_set_target(&render,inner),
          "pending-fill resize suspends the deferred outer clear");
  gml_surface_resize(&render,outer,3,3);
  REQUIRE(render.target_stack[1].fb==render.surface[outer-1].px &&
          !render.target_stack[1].pending_fill,
          "pending-fill resize realizes the suspended clear before replacing storage");
  gml_surface_reset_target(&render);
  REQUIRE(render.fb==render.surface[outer-1].px && render.fbw==3 && render.fbh==3 &&
          render.fb[0]==0xFF214365u && render.fb[1]==0xFF214365u &&
          render.fb[3]==0xFF214365u && render.fb[4]==0xFF214365u &&
          render.fb[2]==0u && render.fb[8]==0u,
          "suspended clear survives resize with the original rectangle and new zero border");
  gml_render_set_pending_fill(&render,0xFF7395B7u);
  gml_surface_resize(&render,outer,4,4);
  REQUIRE(render.fb==render.surface[outer-1].px && render.fbw==4 && render.fbh==4 &&
          !render.pending_fill && render.fb[0]==0xFF7395B7u &&
          render.fb[8]==0xFF7395B7u && render.fb[10]==0xFF7395B7u &&
          render.fb[3]==0u && render.fb[15]==0u,
          "active deferred clear survives resize without filling the newly exposed border");
  gml_surface_reset_target(&render);
  gml_surface_free(&render,inner);
  gml_surface_free(&render,outer);
  gml_render_free(&render);
  return 0;
}

static int numbered_surface_free_bound_case(void){
  GmlRender render={0};
  uint32_t base[16]={0};
  int outer,inner,reused;
  render.next_surface_id=1;
  render.active_shader=-1;
  render.color_write_mask=0x0Fu;
  gml_render_begin(&render,base,4,4,0.0,0.0);
  outer=gml_surface_create(&render,2,2);
  inner=gml_surface_create(&render,2,2);
  REQUIRE(outer>0 && inner>0 && gml_surface_set_target(&render,outer) &&
          gml_surface_set_target(&render,inner),
          "bound free creates nested numbered surfaces");
  gml_surface_free(&render,outer);
  REQUIRE(!gml_surface_exists(&render,outer) && render.target_id==inner &&
          render.fb==render.surface[inner-1].px && render.target_sp==1 &&
          render.target_stack[0].fb==base,
          "freeing a suspended target removes its stale stack entry");
  render.next_surface_id=outer;
  reused=gml_surface_create(&render,3,3);
  REQUIRE(reused==outer && render.target_id==inner,
          "freeing and recreating an id does not rebind the older target");
  gml_surface_reset_target(&render);
  REQUIRE(render.fb==base && render.target_id==-1 && render.target_sp==0,
          "reset after suspended free returns to the valid base target");
  REQUIRE(gml_surface_set_target(&render,reused) &&
          gml_surface_set_target(&render,reused),
          "the reused surface may be nested as the active target");
  gml_surface_free(&render,reused);
  REQUIRE(!gml_surface_exists(&render,reused) && render.fb==base &&
          render.target_id==-1 && render.target_sp==0,
          "freeing a multiply bound active surface restores the live base target");
  gml_surface_free(&render,inner);
  gml_render_free(&render);
  GmlRender alias={0};
  alias.next_surface_id=1;
  int borrowed=gml_surface_create(&alias,2,2);
  REQUIRE(borrowed>0,"base-alias free creates a numbered surface");
  gml_render_begin(&alias,alias.surface[borrowed-1].px,2,2,0.0,0.0);
  gml_render_application_surface_bind(&alias,alias.surface[borrowed-1].px,2,2,0);
  gml_surface_free(&alias,borrowed);
  REQUIRE(alias.fb==NULL && alias.base_fb==NULL &&
          alias.app_surface==NULL && alias.fbw==0 && alias.fbh==0 &&
          alias.base_fbw==0 && alias.base_fbh==0 && alias.app_w==0 && alias.app_h==0,
          "freeing storage borrowed as base/application detaches every public pointer");
  gml_render_free(&alias);
  GmlRender nested_alias={0};
  nested_alias.next_surface_id=1;
  int borrowed_base=gml_surface_create(&nested_alias,2,2);
  int surviving=gml_surface_create(&nested_alias,2,2);
  REQUIRE(borrowed_base>0 && surviving>0,"nested base-alias free creates surfaces");
  gml_render_begin(&nested_alias,nested_alias.surface[borrowed_base-1].px,
                   2,2,0.0,0.0);
  REQUIRE(gml_surface_set_target(&nested_alias,surviving),
          "nested base-alias free binds a surviving target");
  gml_surface_free(&nested_alias,borrowed_base);
  REQUIRE(nested_alias.base_fb==NULL && nested_alias.target_sp==0 &&
          nested_alias.fb==nested_alias.surface[surviving-1].px,
          "freeing a borrowed base keeps the active surviving target");
  gml_surface_reset_target(&nested_alias);
  REQUIRE(nested_alias.fb==NULL && nested_alias.fbw==0 && nested_alias.fbh==0 &&
          nested_alias.target_id==-1,
          "reset without a base detaches the former numbered target");
  gml_surface_free(&nested_alias,surviving);
  gml_render_free(&nested_alias);
  return 0;
}

static int declared_canvas_preview_case(void){
  GmlRender render={0};
  uint32_t frame[16*8]={0};
  render.next_surface_id=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  int source=gml_surface_create(&render,16,8);
  REQUIRE(source>0,"canvas source");
  uint32_t *pixels=surface_pixels(&render,source,NULL,NULL);
  for(int i=0;i<16*8;i++) pixels[i]=0xFFFFFFFFu;
  render.surface[source-1].opaque_known=1;
  render.surface[source-1].all_opaque=1;
  render.surface[source-1].all_transparent=0;
  render.surface_canvas=source;
  render.surface_canvas_width=8;
  render.surface_canvas_height=8;
  gml_render_begin(&render,frame,16,8,0,0);
  render.active_shader=-1;
  gml_draw_surface_stretched(&render,source,0,0,8,4,0xFFFFFF,1);
  for(int y=0;y<8;y++) for(int x=0;x<16;x++)
    REQUIRE((frame[y*16+x]&0xFFFFFFu)==(x<8 && y<4 ? 0xFFFFFFu : 0u),
            "a reduced canvas preview retains its requested destination");
  gml_surface_free(&render,source);
  return 0;
}

static int threshold_palette_surface_case(void){
  enum { WIDTH=3,HEIGHT=2 };
  GmlRender render;
  struct GmlShaderPal shader;
  uint32_t frame[WIDTH*HEIGHT];
  memset(&render,0,sizeof render);
  memset(&shader,0,sizeof shader);
  memset(frame,0,sizeof frame);
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.lut_pal_sprite=-1;
  render.shader_pal=&shader;
  render.n_shader_pal=1;
  shader.threshold_palette=1;
  shader.threshold_palette_red=0.25f;
  shader.threshold_palette_green[0][0]=0.8f;
  shader.threshold_palette_green[0][1]=0.6f;
  shader.threshold_palette_green[0][2]=0.4f;
  shader.threshold_palette_green[0][3]=0.2f;
  shader.threshold_palette_green[1][0]=0.86f;
  shader.threshold_palette_green[1][1]=0.61f;
  shader.threshold_palette_green[1][2]=0.43f;
  shader.threshold_palette_green[1][3]=0.31f;
  shader.threshold_palette_colour[7][1]=50;
  shader.threshold_palette_colour[7][2]=50;
  shader.threshold_palette_set=0x3ffu;

  int source=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(source==1,"threshold palette surface id");
  uint32_t *source_pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixels!=NULL,"threshold palette source pixels");
  for(int index=0;index<WIDTH*HEIGHT;index++) source_pixels[index]=0xFFFF8000u;
  render.surface[0].opaque_known=1;
  render.surface[0].all_opaque=1;
  render.surface[0].all_transparent=0;
  gml_render_begin(&render,frame,WIDTH,HEIGHT,0.0,0.0);
  render.active_shader=0;
  gml_draw_surface_stretched(
    &render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  for(int index=0;index<WIDTH*HEIGHT;index++)
    REQUIRE(frame[index]==0xFF003232u,
            "threshold palette maps a complete surface composite");

  source_pixels[0]=0x00FF8000u;
  render.surface[0].opaque_known=0;
  render.surface[0].all_opaque=0;
  render.surface[0].all_transparent=0;
  frame[0]=0xFF112233u;
  render.alphablend=0;
  gml_draw_surface_stretched(
    &render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(frame[0]==0x00003232u,
          "disabled blending writes mapped transparent pixels");

  frame[0]=0xFF112233u;
  render.alphablend=1;
  gml_draw_surface_stretched(
    &render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(frame[0]==0xFF112233u,
          "enabled blending skips mapped transparent pixels");
  gml_surface_free(&render,source);
  return 0;
}

static int presentation_latch_coverage_case(void){
  enum { WIDTH=4,HEIGHT=3 };
  GmlRender render;
  uint32_t frame[WIDTH*HEIGHT];
  memset(&render,0,sizeof render);
  memset(frame,0,sizeof frame);
  render.fb=render.base_fb=frame;
  render.fbw=render.base_fbw=WIDTH;
  render.fbh=render.base_fbh=HEIGHT;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;

  int source=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(source==1 && surface_known_transparent(&render,source),
          "presentation transparent source");
  gml_draw_surface_stretched(
    &render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(!gml_render_content_composited_screen(&render),
          "transparent scratch surface does not latch presentation");

  REQUIRE(gml_surface_set_target(&render,source),
          "presentation authored transparent source target");
  gml_render_primitive_rectangle(&render,0,0,0,0,0xFFFFFFu,0);
  gml_render_set_pending_fill(&render,0);
  gml_surface_reset_target(&render);
  REQUIRE(surface_known_transparent(&render,source),
          "presentation authored source resolves transparent");
  gml_draw_surface_stretched(
    &render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(gml_render_content_composited_screen(&render),
          "authored transparent surface latches presentation");

  gml_render_clear_content_composited_screen(&render);
  REQUIRE(gml_surface_set_target(&render,source),
          "presentation opaque source target");
  gml_render_clear(&render,0x000000u,1.0);
  gml_surface_reset_target(&render);
  REQUIRE(surface_known_opaque(&render,source),
          "presentation opaque source coverage");
  gml_draw_surface_stretched(
    &render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(gml_render_content_composited_screen(&render),
          "opaque black surface latches presentation");

  gml_render_clear_content_composited_screen(&render);
  REQUIRE(!gml_render_content_composited_screen(&render),
          "presentation latch clears between frames");
  gml_surface_free(&render,source);
  return 0;
}

static int composition_cases(void){
  GmlRender render;
  uint32_t frame[12*10];
  memset(&render,0,sizeof render);
  for(int y=0;y<10;y++) for(int x=0;x<12;x++){
    unsigned red=(unsigned)(9+x*11+y*3);
    unsigned green=(unsigned)(21+x*5+y*13);
    unsigned blue=(unsigned)(33+x*7+y*17);
    frame[(size_t)y*12+x]=0xFF000000u|(red<<16)|(green<<8)|blue;
  }
  render.fb=render.base_fb=frame;
  render.fbw=render.base_fbw=12;
  render.fbh=render.base_fbh=10;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  render.fb_opaque_known=1;
  render.fb_all_opaque=1;

  int source=gml_surface_create(&render,4,3);
  int target=gml_surface_create(&render,7,6);
  REQUIRE(source==1 && target==2,"composition surface ids");
  uint32_t *source_pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixels!=NULL,"composition source pixels");
  static const uint32_t authored[12]={
    0xFF102030u,0xC0503020u,0x804080C0u,0x00000000u,
    0xFF90A020u,0x4010D080u,0xFFA060E0u,0xC0F02070u,
    0x00203040u,0xFF4060A0u,0x8080C040u,0xFFFFFFFFu
  };
  memcpy(source_pixels,authored,sizeof authored);
  render.surface[0].opaque_known=0;
  render.surface[0].all_opaque=0;
  render.surface[0].all_transparent=0;

  gml_draw_surface_stretched(&render,source,1.25,1.0,7.0,5.0,0xD0A080u,0.75);
  render.interp=1;
  gml_draw_surface_stretched(&render,source,3.0,0.0,8.0,7.0,0xFFFFFFu,0.6);

  render.interp=0;
  gml_draw_surface_ext(&render,source,8.0,4.0,1.15,0.9,33.0,0x80C0FFu,0.7);
  render.blendmode=3;
  gml_draw_surface_ext(&render,source,6.0,2.0,1.0,1.0,90.0,0xC08060u,0.8);
  render.blendmode=0;
  render.color_write_mask=0x05;
  gml_draw_surface_part_ext(&render,source,1.0,0.0,3.0,2.0,
                            7.0,7.0,1.0,1.0,0xE0B060u,1.0);
  render.color_write_mask=0x0F;

  REQUIRE(gml_surface_set_target(&render,target),"composition set target");
  render.interp=1;
  gml_draw_surface_stretched(&render,source,-0.5,0.25,8.0,6.5,0xFFFFFFu,0.9);
  gml_surface_reset_target(&render);
  int target_width=0,target_height=0;
  const uint32_t *target_pixels=
    gml_surface_pixels_read(&render,target,&target_width,&target_height);
  REQUIRE(target_pixels!=NULL && target_width==7 && target_height==6,
          "composition target pixels");

  uint64_t hash=pixel_hash(frame,sizeof frame/sizeof frame[0]);
  hash=pixel_hash_update(hash,target_pixels,(size_t)target_width*target_height);
  if(hash!=UINT64_C(0xb3f2b2d525ff4b17)){
    fprintf(stderr,"renderer surface composition hash: %016llx\n",
            (unsigned long long)hash);
    gml_surface_free(&render,source);
    gml_surface_free(&render,target);
    return 1;
  }
  gml_surface_free(&render,source);
  gml_surface_free(&render,target);
  return 0;
}

static int screen_raster_part_case(void){
  enum { SOURCE_WIDTH=4,SOURCE_HEIGHT=3,TARGET_WIDTH=8,TARGET_HEIGHT=6 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  GmlRender render;
  memset(&render,0,sizeof render);
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++)
    source[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|((uint32_t)(31+x*41+y*17)<<16)|
      ((uint32_t)(23+x*13+y*47)<<8)|(uint32_t)(11+x*29+y*19);
  memset(target,0x5A,sizeof target);
  render.app_surface=source;
  render.app_w=SOURCE_WIDTH;
  render.app_h=SOURCE_HEIGHT;
  render.alphablend=1;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
  gml_render_gui_begin(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_render_gui_set_size(&render,SOURCE_WIDTH,SOURCE_HEIGHT);
  gml_draw_surface_part_ext(&render,0,0.0,0.0,SOURCE_WIDTH,SOURCE_HEIGHT,
                            0.0,0.0,2.0,2.0,0xFFFFFFu,1.0);
  gml_render_gui_end(&render);
  for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
    uint32_t expected=source[(size_t)(y/2)*SOURCE_WIDTH+x/2];
    if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
      fprintf(stderr,"renderer screen raster part mismatch at %d,%d: %08x != %08x\n",
              x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
      return 1;
    }
  }
  return 0;
}

static int first_generation_point_sampling_case(void){
  enum { SOURCE_WIDTH=4,SOURCE_HEIGHT=3,TARGET_WIDTH=13,TARGET_HEIGHT=10 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  GmlRender render;
  GmlWin content={0};
  memset(&render,0,sizeof render);
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++)
    source[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|((uint32_t)(31+x*41+y*17)<<16)|
      ((uint32_t)(23+x*13+y*47)<<8)|(uint32_t)(11+x*29+y*19);
  memset(target,0,sizeof target);
  content.bytecode=14;
  render.win=&content;
  render.app_surface=source;
  render.app_w=SOURCE_WIDTH;
  render.app_h=SOURCE_HEIGHT;
  render.app_surface_opaque=1;
  render.alphablend=1;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=0;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
  gml_render_gui_begin(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_render_gui_set_size(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_draw_surface_stretched(&render,0,0.0,0.0,
                             TARGET_WIDTH,TARGET_HEIGHT,0xFFFFFFu,1.0);
  gml_render_gui_end(&render);
  for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
    int source_x=(int)(((int64_t)(2*x+1)*SOURCE_WIDTH)/(2*TARGET_WIDTH));
    int source_y=(int)(((int64_t)(2*y+1)*SOURCE_HEIGHT)/(2*TARGET_HEIGHT));
    uint32_t expected=source[(size_t)source_y*SOURCE_WIDTH+source_x];
    if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
      fprintf(stderr,"renderer first-generation point sample mismatch at %d,%d: %08x != %08x\n",
              x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
      return 1;
    }
  }
  return 0;
}

static int first_generation_high_magnification_case(void){
  enum { SOURCE_WIDTH=5,SOURCE_HEIGHT=3,TARGET_WIDTH=1103,TARGET_HEIGHT=19 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  GmlRender render;
  GmlWin content={0};
  memset(&render,0,sizeof render);
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++)
    source[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|((uint32_t)(17+x*29+y*31)<<16)|
      ((uint32_t)(23+x*37+y*19)<<8)|(uint32_t)(31+x*13+y*47);
  memset(target,0xA5,sizeof target);
  content.bytecode=14;
  render.win=&content;
  render.app_surface=source;
  render.app_w=SOURCE_WIDTH;
  render.app_h=SOURCE_HEIGHT;
  render.app_surface_opaque=1;
  render.alphablend=1;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=0;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
  gml_render_gui_begin(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_render_gui_set_size(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_draw_surface_stretched(&render,0,0.0,0.0,
                             TARGET_WIDTH,TARGET_HEIGHT,0xFFFFFFu,1.0);
  gml_render_gui_end(&render);
  for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
    int source_x=(int)(((int64_t)(2*x+1)*SOURCE_WIDTH)/(2*TARGET_WIDTH));
    int source_y=(int)(((int64_t)(2*y+1)*SOURCE_HEIGHT)/(2*TARGET_HEIGHT));
    uint32_t expected=source[(size_t)source_y*SOURCE_WIDTH+source_x];
    if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
      fprintf(stderr,
              "renderer first-generation high magnification mismatch at %d,%d: %08x != %08x\n",
              x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
      return 1;
    }
  }
  return 0;
}

typedef struct {
  GmlRender *render;
  const uint32_t *source;
  int calls,valid,accept;
} UnderlayShaderControl;

static int underlay_shader_execute(void *context,const GmlRenderShaderRequest *request){
  UnderlayShaderControl *control=(UnderlayShaderControl*)context;
  control->calls++;
  control->valid=request->shader==0 && control->render->active_shader==0 &&
    request->source==control->source && request->source_identity==0 &&
    request->source_width==2 && request->source_height==2 && request->source_pitch==2 &&
    request->region_x==0 && request->region_y==0 &&
    request->region_width==2 && request->region_height==2 &&
    request->width==4 && request->height==4 &&
    request->target_width==8 && request->target_height==6 &&
    request->dest_x==2 && request->dest_y==1 &&
    request->geometry.valid && request->geometry.origin_x==2.0 &&
    request->geometry.origin_y==1.0 && request->geometry.extent_x==4.0 &&
    request->geometry.extent_y==4.0 &&
    request->vertex_colour[0]==1.0f && request->vertex_colour[1]==1.0f &&
    request->vertex_colour[2]==1.0f && request->vertex_colour[3]==1.0f &&
    control->render->shader_pal[0].generic_uniform[0].value[0]==0.25f;
  if(!control->accept) return 0;
  for(int index=0;index<request->width*request->height;index++)
    request->output[index]=0xFF315273u;
  return 1;
}

static int automatic_underlay_shader_case(void){
  uint32_t source[4]={0xFF204060u,0xFF204060u,0xFF204060u,0xFF204060u};
  /* Accepted and refused executors, software palette and channel-mask shaders,
   * no shader and no executor, plus shaded and plain presentations with D3 active.
   * The source and target are distinct: this is automatic presentation, not feedback. */
  for(int kind=0;kind<8;kind++){
    GmlRender render={0};
    struct GmlShaderPal shader={0};
    uint32_t target[8*6];
    UnderlayShaderControl control={&render,source,0,0,kind==0 || kind==6};
    for(int index=0;index<8*6;index++) target[index]=0xFF102030u;
    render.app_surface=source;
    render.app_w=render.app_h=2;
    render.app_surface_opaque=1;
    render.alphablend=1;
    render.color_write_mask=0x0F;
    render.blend_equation=render.blend_equation_alpha=1;
    render.app_draw_enable=1;
    render.lut_pal_sprite=-1;
    gml_render_begin(&render,target,8,6,3.0,-2.0);
    if(kind>=6){
      render.software3d=gml_software3d_create();
      REQUIRE(render.software3d!=NULL,"automatic presentation D3 fixture allocation");
      gml_software3d_begin(&render,11.0,17.0,23.0,29.0,13.0);
      GmlSoftware3DStatus status={0};
      REQUIRE(gml_software3d_status_get(render.software3d,&status) && status.active,
              "automatic presentation D3 fixture is active");
    }
    render.shader_pal=&shader;
    render.n_shader_pal=1;
    shader.source_vertex_es="void main(){gl_Position=vec4(0.0);}";
    shader.source_fragment_es="void main(){gl_FragColor=vec4(0.25,0.5,0.75,1.0);}";
    shader.generic_uniform_count=1;
    strcpy(shader.generic_uniform[0].name,"fixture_gain");
    shader.generic_uniform[0].count=1;
    shader.generic_uniform[0].value[0]=0.25f;
    if(kind==2){
      shader.threshold_palette=1;
      shader.threshold_palette_red=0.25f;
      shader.threshold_palette_set=0x3ffu;
      for(int index=0;index<10;index++) shader.threshold_palette_colour[index][0]=32;
    }
    if(kind==5){ shader.channel_mask=1; shader.channel_mask_keep=4; }
    render.shade_vertex_colour_valid=1;
    render.shade_vertex_colour[2]=render.shade_vertex_colour[3]=0.5f;
    int plain=kind==3 || kind==7;
    render.active_shader=plain?-1:0;
    gml_render_set_shader_executor(&render,kind==4?NULL:underlay_shader_execute,&control);
    gml_render_set_pending_underlay(&render,2,1,4,4);
    if(kind<2 || kind==6)
      REQUIRE(control.calls==1 && control.valid,
              "automatic shader executes at the screen rectangle before GUI state changes");
    else
      REQUIRE(control.calls==0,"recognized and absent shaders do not use the host executor");
    if(!plain)
      REQUIRE(!render.pending_underlay,"active presentation shader completes before GUI");
    else
      REQUIRE(render.pending_underlay,"unshaded presentation retains deferred composition");
    REQUIRE(render.active_shader==(plain?-1:0) &&
            render.cam_x==3.0 && render.cam_y==-2.0 && !render.dispatch_cpu_depth &&
            render.shade_vertex_colour_valid && render.shade_vertex_colour[0]==0.0f &&
            render.shade_vertex_colour[2]==0.5f && render.shade_vertex_colour[3]==0.5f,
            "automatic shader preserves camera and draw state");
    /* A GUI Begin shader reset must not discard or change the earlier presentation. */
    render.active_shader=-1;
    shader.generic_uniform[0].value[0]=0.75f;
    render.cam_x=render.cam_y=0.0;
    gml_render_flush_pending_underlay(&render);
    uint32_t colour=(kind==0 || kind==6)?0xFF315273u:
      (kind==2 || kind==5)?0xFF200000u:0xFF204060u;
    for(int y=0;y<6;y++) for(int x=0;x<8;x++){
      uint32_t expected=x>=2 && x<6 && y>=1 && y<5?colour:0xFF102030u;
      if(target[y*8+x]!=expected){
        fprintf(stderr,"automatic underlay kind %d at %d,%d: %08x != %08x\n",
                kind,x,y,target[y*8+x],expected);
        REQUIRE(0,"automatic presentation is screen-space and survives a GUI shader reset");
      }
    }
    REQUIRE(!memcmp(source,(uint32_t[]){0xFF204060u,0xFF204060u,0xFF204060u,0xFF204060u},
                    sizeof source),"automatic shader does not overwrite its source");
    free(render.shaded_plane);
    free(render.shader_account);
    if(kind>=6){
      GmlSoftware3DStatus status={0};
      REQUIRE(gml_software3d_status_get(render.software3d,&status) && status.active,
              "automatic presentation preserves active D3 state");
      gml_software3d_destroy(render.software3d);
    }
  }
  return 0;
}

typedef struct {
  GmlRender *render;
  int calls,change_alpha,valid;
} DeferredShaderControl;

static int deferred_shader_execute(void *context,const GmlRenderShaderRequest *request){
  DeferredShaderControl *control=context;
  control->calls++;
  control->valid=request->shader==0 && request->geometry.valid &&
    request->width==8 && request->height==6 &&
    request->target_width==8 && request->target_height==6 &&
    request->dest_x==0 && request->dest_y==0 &&
    request->source_width==8 && request->source_height==6 &&
    control->render->shader_pal[0].generic_uniform[0].value[0]==0.25f;
  for(int y=0;y<6;y++) for(int x=0;x<8;x++){
    uint32_t pixel=request->source[y*request->source_pitch+x];
    if(control->change_alpha)
      pixel=(pixel&0xFFFFFFu)|((x==4 && y==3)?0u:0x99000000u);
    request->output[y*8+x]=pixel;
  }
  return 1;
}

/* Full-target coverage is not an opaque overwrite. Compare the same shader draw with and
 * without deferral, including a program that makes an opaque input partially transparent. */
static int deferred_shader_alpha_case(void){
  for(int change_alpha=0;change_alpha<2;change_alpha++) for(int mutate=0;mutate<6;mutate++){
    uint32_t reference[48]={0};
    for(int deferred=0;deferred<2;deferred++){
      GmlRender render={0};
      struct GmlShaderPal shader={0};
      uint32_t target[48];
      DeferredShaderControl control={&render,0,change_alpha,0};
      render.next_surface_id=1;
      render.color_write_mask=15;
      render.lut_pal_sprite=-1;
      render.shader_pal=&shader; render.n_shader_pal=1;
      shader.source_vertex_es="void main(){gl_Position=vec4(0.0);}";
      shader.source_fragment_es="void main(){gl_FragColor=vec4(0.0);}";
      shader.generic_uniform_count=1;
      strcpy(shader.generic_uniform[0].name,"fixture_gain");
      shader.generic_uniform[0].count=1;
      shader.generic_uniform[0].value[0]=0.25f;
      gml_render_begin(&render,target,8,6,0,0);
      gml_render_clear(&render,0x0D0B07u,1.0);
      int surface=gml_surface_create(&render,8,6);
      REQUIRE(surface>0,"deferred shader source allocated");
      uint32_t *source=surface_pixels(&render,surface,NULL,NULL);
      for(int pixel=0;pixel<48;pixel++) source[pixel]=
        change_alpha?0xFF000000u:pixel==28?0u:0x99000000u;
      render.surface[surface-1].opaque_known=0;
      render.surface[surface-1].all_transparent=0;
      gml_render_set_shader_executor(&render,deferred_shader_execute,&control);
      gml_render_set_deferred_presentation(&render,deferred);
      gml_render_shader_set_current(&render,0);
      gml_draw_surface_stretched(&render,surface,0,0,8,6,0xFFFFFFu,1.0);
      gml_render_shader_set_current(&render,-1);
      if(mutate==1){
        double value=0.75;
        int handle=gml_render_shader_uniform_handle(&render,0,"fixture_gain");
        gml_render_shader_uniform_set_values(&render,handle,&value,1,0);
        REQUIRE(shader.generic_uniform[0].value[0]==0.75f,
                "later uniform setter keeps its requested value");
      }
      if(mutate==2){
        REQUIRE(gml_surface_set_target(&render,surface),"later target binds");
        gml_render_clear(&render,0xFFFFFFu,1.0);
        gml_surface_reset_target(&render);
      }
      if(mutate==3) gml_surface_free(&render,surface);
      if(mutate==4){
        uint32_t *writable=surface_pixels(&render,surface,NULL,NULL);
        memset(writable,0,48*sizeof *writable);
      }
      if(mutate==5) gml_surface_resize(&render,surface,1,1);
      /* Materialization must use normal state without leaking it into the later draw. */
      render.interp=1; render.blendmode=6; render.color_write_mask=7;
      gml_render_primitive_point(&render,7,5,0xFFFFFFu);
      int matches=control.calls==1 && control.valid && target[28]==0xFF070B0Du &&
        target[0]==0xFF020405u && render.active_shader==-1 &&
        render.interp==1 && render.blendmode==6 && render.color_write_mask==7;
      if(!deferred) memcpy(reference,target,sizeof reference);
      matches=matches && !memcmp(reference,target,sizeof reference);
      if(!matches)
        fprintf(stderr,"deferred alpha source_opaque=%d mutation=%d enabled=%d calls=%d valid=%d "
                       "corner=%08x center=%08x equal=%d\n",
                change_alpha,mutate,deferred,control.calls,control.valid,target[0],target[28],
                !memcmp(reference,target,sizeof reference));
      gml_surface_free(&render,surface);
      free(render.shaded_plane);
      REQUIRE(matches,"deferred shader preserves destination and shader-output alpha");
    }
  }
  return 0;
}

typedef struct {
  GmlRenderShaderGeometry geometry;
  int calls,valid,reject,width,height,dest_x,dest_y;
  float vertex;
} DeferredGeometryControl;

static int deferred_geometry_execute(void *context,const GmlRenderShaderRequest *request){
  DeferredGeometryControl *control=context;
  control->calls++;
  control->valid=request->width==control->width && request->height==control->height &&
    request->dest_x==control->dest_x && request->dest_y==control->dest_y &&
    request->geometry.origin_x==control->geometry.origin_x &&
    request->geometry.origin_y==control->geometry.origin_y &&
    request->geometry.extent_x==control->geometry.extent_x &&
    request->geometry.extent_y==control->geometry.extent_y &&
    request->geometry.valid==control->geometry.valid &&
    request->vertex_colour[0]==control->vertex;
  if(!control->valid)
    fprintf(stderr,"deferred request size=%dx%d/%dx%d destination=%d,%d/%d,%d "
                   "geometry=(%g,%g %gx%g)/(%g,%g %gx%g) vertex=%g/%g\n",
            request->width,request->height,
            control->width,control->height,request->dest_x,request->dest_y,
            control->dest_x,control->dest_y,
            request->geometry.origin_x,request->geometry.origin_y,
            request->geometry.extent_x,request->geometry.extent_y,
            control->geometry.origin_x,control->geometry.origin_y,
            control->geometry.extent_x,control->geometry.extent_y,
            request->vertex_colour[0],control->vertex);
  if(control->reject) return 0;
  for(int y=0;y<request->height;y++) for(int x=0;x<request->width;x++){
    int sx=x*request->source_width/request->width;
    int sy=y*request->source_height/request->height;
    uint32_t pixel=request->source[sy*request->source_pitch+sx];
    if(control->vertex!=1.0f) pixel=(pixel&0xFF000000u)|0x00102030u;
    request->output[y*request->width+x]=pixel;
  }
  return 1;
}

/* Exercise geometry retained across a camera change, executor refusal/absence, and draw state
 * which the terminal record cannot represent. The immediate path is the controlled comparator. */
static int deferred_shader_geometry_case(void){
  for(int clipped=0;clipped<2;clipped++) for(int executor=0;executor<3;executor++)
    for(int tinted=0;tinted<2;tinted++){
      uint32_t reference[48]={0};
      for(int deferred=0;deferred<2;deferred++){
        GmlRender render={0};
        struct GmlShaderPal shader={0};
        uint32_t target[48];
        double x=clipped?-1.25:0.0,y=clipped?-0.75:0.0;
        double width=clipped?10.0:8.0,height=clipped?8.0:6.0;
        DeferredGeometryControl control={0};
        render.next_surface_id=1; render.color_write_mask=15; render.lut_pal_sprite=-1;
        render.shader_pal=&shader; render.n_shader_pal=1;
        shader.source_vertex_es="void main(){gl_Position=vec4(0.0);}";
        shader.source_fragment_es="void main(){gl_FragColor=vec4(0.0);}";
        gml_render_begin(&render,target,8,6,2.5,1.5);
        gml_render_clear(&render,0x0D0B07u,1.0);
        int surface=gml_surface_create(&render,8,6);
        REQUIRE(surface>0,"geometry shader source allocated");
        uint32_t *source=surface_pixels(&render,surface,NULL,NULL);
        for(int pixel=0;pixel<48;pixel++) source[pixel]=pixel==28?0u:0x991A2B3Cu;
        /* This resource-free fixture writes storage directly, so its creation-time transparent
         * certificate is no longer valid. Ordinary target draws update that metadata themselves. */
        render.surface[surface-1].opaque_known=0;
        render.surface[surface-1].all_transparent=0;
        control.geometry=gml_render_shader_geometry(&render,x+2.5,y+1.5,width,height,1);
        control.width=(int)width; control.height=(int)height;
        control.dest_x=(int)lround(x); control.dest_y=(int)lround(y);
        control.reject=executor==1; control.vertex=tinted?0.5f:1.0f;
        render.shade_vertex_colour_valid=1;
        for(int channel=0;channel<4;channel++) render.shade_vertex_colour[channel]=control.vertex;
        if(executor!=2) gml_render_set_shader_executor(&render,deferred_geometry_execute,&control);
        gml_render_set_deferred_presentation(&render,deferred);
        gml_render_shader_set_current(&render,0);
        gml_draw_surface_stretched(&render,surface,x+2.5,y+1.5,width,height,0xFFFFFFu,1.0);
        if(deferred && !tinted) REQUIRE(render.pending_presentation,"eligible draw is retained");
        if(tinted) REQUIRE(!render.pending_presentation,"nonwhite vertex draw executes immediately");
        gml_render_shader_set_current(&render,-1);
        render.cam_x=7.0; render.cam_y=-3.0;
        gml_render_primitive_point(&render,14,2,0xFFFFFFu);
        int matches=control.calls==(executor==2?0:1) && (executor==2 || control.valid) &&
          render.cam_x==7.0 && render.cam_y==-3.0 && render.active_shader==-1 &&
          render.shade_vertex_colour_valid && render.shade_vertex_colour[0]==control.vertex;
        if(!deferred) memcpy(reference,target,sizeof reference);
        matches=matches && !memcmp(reference,target,sizeof reference);
        if(!matches) fprintf(stderr,"deferred geometry clipped=%d executor=%d tinted=%d deferred=%d "
                            "calls=%d valid=%d equal=%d\n",clipped,executor,tinted,deferred,
                            control.calls,control.valid,!memcmp(reference,target,sizeof reference));
        gml_surface_free(&render,surface); free(render.shaded_plane);
        REQUIRE(matches,"deferred shader matches immediate geometry and fallback composition");
      }
    }
  return 0;
}

static int opaque_integer_scale_case(void){
  enum { SOURCE_WIDTH=5,SOURCE_HEIGHT=3,TARGET_WIDTH=20,TARGET_HEIGHT=12 };
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t opaque_target[TARGET_WIDTH*TARGET_HEIGHT];
  uint32_t unknown_target[TARGET_WIDTH*TARGET_HEIGHT];
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++)
    source[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|((uint32_t)(17+x*31+y*7)<<16)|
      ((uint32_t)(29+x*11+y*37)<<8)|(uint32_t)(41+x*23+y*13);

  for(int pass=0;pass<2;pass++){
    GmlRender render;
    uint32_t *target=pass?unknown_target:opaque_target;
    memset(&render,0,sizeof render);
    for(size_t index=0;index<TARGET_WIDTH*TARGET_HEIGHT;index++)
      target[index]=0xFF102030u+(uint32_t)index;
    render.app_surface=source;
    render.app_w=SOURCE_WIDTH;
    render.app_h=SOURCE_HEIGHT;
    render.app_surface_opaque=pass?0:1;
    render.alphablend=1;
    render.color_write_mask=0x0F;
    render.blend_equation=1;
    render.blend_equation_alpha=1;
    render.app_draw_enable=1;
    render.active_shader=-1;
    render.lut_pal_sprite=-1;
    gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
    gml_render_set_pending_underlay(&render,0,0,TARGET_WIDTH,TARGET_HEIGHT);
    gml_render_flush_pending_underlay(&render);
    for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
      uint32_t expected=source[(size_t)(y/(TARGET_HEIGHT/SOURCE_HEIGHT))*SOURCE_WIDTH+
                               x/(TARGET_WIDTH/SOURCE_WIDTH)];
      if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
        fprintf(stderr,"renderer opaque integer scale mismatch at %d,%d: %08x != %08x\n",
                x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
        return 1;
      }
    }
  }
  REQUIRE(!memcmp(opaque_target,unknown_target,sizeof opaque_target),
          "opaque integer scale equivalence");
  return 0;
}

static int point_downscale_case(void){
  enum { SOURCE_WIDTH=4,SOURCE_HEIGHT=4,TARGET_WIDTH=2,TARGET_HEIGHT=2 };
  GmlRender render;
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  memset(&render,0,sizeof render);
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  int source=gml_surface_create(&render,SOURCE_WIDTH,SOURCE_HEIGHT);
  REQUIRE(source==1,"point downscale surface id");
  uint32_t *source_pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixels!=NULL,"point downscale source pixels");
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++){
    uint32_t value=(uint32_t)(16+y*40+x*8);
    source_pixels[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|(value<<16)|(value<<8)|value;
  }
  render.surface[0].opaque_known=1;
  render.surface[0].all_opaque=1;
  render.surface[0].all_transparent=0;

  memset(target,0,sizeof target);
  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
  gml_draw_surface_stretched(&render,source,0.0,0.0,
                             TARGET_WIDTH,TARGET_HEIGHT,0xFFFFFFu,1.0);
  static const int point_source[4][2]={{0,0},{2,0},{0,2},{2,2}};
  for(int index=0;index<4;index++){
    int x=point_source[index][0],y=point_source[index][1];
    uint32_t expected=source_pixels[(size_t)y*SOURCE_WIDTH+x];
    if(target[index]!=expected){
      fprintf(stderr,"renderer point downscale mismatch at %d: %08x != %08x\n",
              index,target[index],expected);
      gml_surface_free(&render,source);
      return 1;
    }
  }

  memset(target,0,sizeof target);
  render.interp=1;
  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
  gml_draw_surface_stretched(&render,source,0.0,0.0,
                             TARGET_WIDTH,TARGET_HEIGHT,0xFFFFFFu,1.0);
  static const uint32_t filtered[4]={
    0xFF282828u,0xFF383838u,0xFF787878u,0xFF888888u
  };
  REQUIRE(!memcmp(target,filtered,sizeof filtered),"filtered downscale average");
  gml_surface_free(&render,source);
  return 0;
}

static int opaque_near_identity_reduction_case(void){
  enum { SOURCE_WIDTH=5,SOURCE_HEIGHT=4,TARGET_WIDTH=4,TARGET_HEIGHT=3 };
  GmlRender render;
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  memset(&render,0,sizeof render);
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  int source=gml_surface_create(&render,SOURCE_WIDTH,SOURCE_HEIGHT);
  REQUIRE(source==1,"near-identity reduction surface id");
  uint32_t *source_pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixels!=NULL,"near-identity reduction source pixels");
  for(int y=0;y<SOURCE_HEIGHT;y++) for(int x=0;x<SOURCE_WIDTH;x++){
    uint32_t value=(uint32_t)(13+y*47+x*9);
    source_pixels[(size_t)y*SOURCE_WIDTH+x]=
      0xFF000000u|(value<<16)|(value<<8)|value;
  }
  render.surface[0].opaque_known=1;
  render.surface[0].all_opaque=1;
  render.surface[0].all_transparent=0;

  for(int pass=0;pass<3;pass++){
    int known=pass!=0;
    render.surface[0].opaque_known=known;
    render.surface[0].all_opaque=known;
    memset(target,0x4C,sizeof target);
    gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,0.0,0.0);
    render.alpha_test_enable=pass==2;
    render.alpha_test_ref=254;
    gml_draw_surface_stretched(&render,source,0.0,0.0,
                               TARGET_WIDTH,TARGET_HEIGHT,0xFFFFFFu,1.0);
    for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
      /* A point-sampled reduction reads the texel under the destination pixel's CENTRE, with a
       * sample landing exactly on a texel boundary belonging to the preceding texel. Stated here
       * rather than copied from the kernel: the previous expectation was the kernel's own
       * leading-edge expression, which reads half a destination pixel early and, once that
       * exceeds one source texel, skips source rows outright. */
      int source_x=(int)((((int64_t)x*2+1)*SOURCE_WIDTH-1)/((int64_t)TARGET_WIDTH*2));
      int source_y=(int)((((int64_t)y*2+1)*SOURCE_HEIGHT-1)/((int64_t)TARGET_HEIGHT*2));
      uint32_t expected=source_pixels[(size_t)source_y*SOURCE_WIDTH+source_x];
      if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
        fprintf(stderr,"renderer near-identity reduction mismatch at %d,%d: %08x != %08x\n",
                x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
        gml_surface_free(&render,source);
        return 1;
      }
    }
    REQUIRE(render.fb_opaque_known && render.fb_all_opaque,
            "near-identity reduction opaque coverage");
  }
  gml_surface_free(&render,source);
  return 0;
}

static int zero_reference_alpha_test_case(void){
  enum { WIDTH=4,HEIGHT=3 };
  GmlRender render;
  uint32_t baseline[WIDTH*HEIGHT],zero_test[WIDTH*HEIGHT],threshold_test[WIDTH*HEIGHT];
  memset(&render,0,sizeof render);
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  int source=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(source==1,"zero-reference alpha-test surface id");
  uint32_t *source_pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixels!=NULL,"zero-reference alpha-test source pixels");
  for(int x=0;x<WIDTH;x++){
    source_pixels[x]=0x00102030u+(uint32_t)x;
    source_pixels[WIDTH+x]=0x80406080u+(uint32_t)x;
    source_pixels[WIDTH*2+x]=0xFFA0C0E0u+(uint32_t)x;
  }
  render.surface[0].opaque_known=0;
  render.surface[0].all_opaque=0;
  render.surface[0].all_transparent=0;

  for(size_t index=0;index<WIDTH*HEIGHT;index++)
    baseline[index]=zero_test[index]=threshold_test[index]=0xFF182838u;
  gml_render_begin(&render,baseline,WIDTH,HEIGHT,0.0,0.0);
  render.alpha_test_enable=0;
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  gml_render_begin(&render,zero_test,WIDTH,HEIGHT,0.0,0.0);
  render.alpha_test_enable=1;
  render.alpha_test_ref=0;
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(!memcmp(baseline,zero_test,sizeof baseline),
          "zero-reference alpha-test equivalence");

  gml_render_begin(&render,threshold_test,WIDTH,HEIGHT,0.0,0.0);
  render.alpha_test_enable=1;
  render.alpha_test_ref=128;
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  for(int x=0;x<WIDTH;x++){
    REQUIRE(threshold_test[x]==0xFF182838u,
            "transparent row remains below alpha threshold");
    REQUIRE(threshold_test[WIDTH+x]==0xFF182838u,
            "threshold row is discarded");
    REQUIRE(threshold_test[WIDTH*2+x]==source_pixels[WIDTH*2+x],
            "opaque row survives alpha threshold");
  }
  gml_surface_free(&render,source);
  return 0;
}

static int max_preset_surface_case(void){
  GmlRender render;
  uint32_t target=0xFF204080u;
  memset(&render,0,sizeof render);
  render.fb=render.base_fb=&target;
  render.fbw=render.base_fbw=1;
  render.fbh=render.base_fbh=1;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  int source=gml_surface_create(&render,1,1);
  REQUIRE(source==1,"maximum preset surface id");
  uint32_t *source_pixel=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixel!=NULL,"maximum preset surface pixel");
  *source_pixel=0x804080C0u;
  render.surface[0].opaque_known=1;
  render.surface[0].all_opaque=0;
  render.surface[0].all_transparent=0;
  render.blendmode=4;
  gml_draw_surface_stretched(&render,source,0.0,0.0,1.0,1.0,0xFFFFFFu,0.5);
  if(target!=0xFF284050u){
    fprintf(stderr,"renderer maximum preset surface mismatch: %08x\n",target);
    gml_surface_free(&render,source);
    return 1;
  }
  gml_surface_free(&render,source);
  return 0;
}

static int subtract_surface_coverage_case(void){
  enum { WIDTH=5,HEIGHT=5 };
  GmlRender render;
  uint32_t frame[WIDTH*HEIGHT],background[WIDTH*HEIGHT];
  memset(&render,0,sizeof render);
  for(size_t index=0;index<WIDTH*HEIGHT;index++)
    frame[index]=background[index]=0xFF204060u+(uint32_t)index;
  render.fb=render.base_fb=frame;
  render.fbw=render.base_fbw=WIDTH;
  render.fbh=render.base_fbh=HEIGHT;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;

  int mask=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(mask==1,"subtract mask surface id");
  REQUIRE(gml_surface_set_target(&render,mask),"subtract mask set target");
  gml_render_primitive_rectangle(&render,0,0,WIDTH-1,HEIGHT-1,0x40A020u,0);
  render.blendmode=2;
  gml_render_primitive_circle(&render,WIDTH/2,HEIGHT/2,2,2,0xFFFFFFu,0);
  render.blendmode=0;
  gml_surface_reset_target(&render);

  int mask_width=0,mask_height=0;
  const uint32_t *mask_pixels=
    gml_surface_pixels_read(&render,mask,&mask_width,&mask_height);
  REQUIRE(mask_pixels && mask_width==WIDTH && mask_height==HEIGHT,
          "subtract mask pixels");
  int holes=0,covered=0;
  for(size_t index=0;index<WIDTH*HEIGHT;index++){
    if(!(mask_pixels[index]>>24)) holes++;
    else if((mask_pixels[index]>>24)==255) covered++;
  }
  REQUIRE(holes>0 && covered>0,"subtract mask mixed coverage");
  REQUIRE(!surface_known_opaque(&render,mask),"subtract mask not opaque");

  gml_draw_surface_stretched(&render,mask,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  for(size_t index=0;index<WIDTH*HEIGHT;index++){
    if(!(mask_pixels[index]>>24) && frame[index]!=background[index]){
      fprintf(stderr,"renderer subtract mask hole mismatch at %zu: %08x != %08x\n",
              index,frame[index],background[index]);
      gml_surface_free(&render,mask);
      return 1;
    }
  }
  gml_surface_free(&render,mask);
  return 0;
}

static int masked_sprite_channels_case(void){
  enum { MAX_WIDTH=9,HEIGHT=5 };
  static const uint8_t source[24]={
    13,29,47,0, 61,79,97,63, 113,131,149,127,
    167,181,199,191, 211,227,239,255, 251,233,217,31
  };
  static const uint32_t channel_bits[4]={
    0x00FF0000u,0x0000FF00u,0x000000FFu,0xFF000000u
  };
  GmlRender render={0};
  uint32_t storage[MAX_WIDTH*HEIGHT+2],initial[MAX_WIDTH*HEIGHT];
  uint32_t unmasked[MAX_WIDTH*HEIGHT];
  render.fb=render.base_fb=storage+1;
  render.fbh=render.base_fbh=HEIGHT;
  render.target_id=-1; render.alphablend=1; render.alpha=1.0;
  render.active_shader=-1; render.lut_pal_sprite=-1;
  render.blend_equation=render.blend_equation_alpha=1;
  render.n_spr=1;
  render.spr=calloc(1,sizeof(*render.spr));
  REQUIRE(render.spr!=NULL,"channel-mask sprite allocation");
  render.spr[0].w=3; render.spr[0].h=2; render.spr[0].n_frames=1;
  render.spr[0].runtime_owned=1;
  render.spr[0].runtime_rgba=malloc(sizeof source);
  REQUIRE(render.spr[0].runtime_rgba!=NULL,"channel-mask sprite pixels");
  memcpy(render.spr[0].runtime_rgba,source,sizeof source);
  for(int classic=0;classic<2;classic++) for(int interp=0;interp<2;interp++)
    for(int width=1;width<=MAX_WIDTH;width++) for(int variant=0;variant<5;variant++){
      size_t pixels=(size_t)width*HEIGHT;
      render.fbw=render.base_fbw=width;
      render.classic=classic; render.interp=interp;
      render.blendmode=variant==4?1:0;
      for(size_t index=0;index<pixels;index++)
        initial[index]=UINT32_C(0x192F435B)+(uint32_t)index*UINT32_C(0x0305070B);
      for(unsigned mask=0;mask<=16;mask++){
        unsigned requested=mask==0?15u:mask-1u;
        memcpy(render.fb,initial,pixels*sizeof(*initial));
        storage[0]=storage[pixels+1]=UINT32_C(0x5A6B7C8D);
        render.fb_opaque_known=render.fb_all_opaque=render.fb_all_transparent=0;
        render.color_write_mask=(uint8_t)requested;
        if(variant==3)
          gml_draw_sprite_tiled_ext(&render,0,0,-0.25,0.75,1.5,1.25,0xB3D5F7u,0.625);
        else
          gml_draw_sprite_ext(&render,0,0,variant==1?4.25:0.25,1.25,
                              variant==1?-1.5:1.5,1.25,
                              variant==2?33.0:0.0,0xB3D5F7u,0.625);
        REQUIRE(render.color_write_mask==requested,"sprite restores requested channel mask");
        REQUIRE(storage[0]==UINT32_C(0x5A6B7C8D) &&
                storage[pixels+1]==UINT32_C(0x5A6B7C8D),"channel restore respects framebuffer bounds");
        if(mask==0){ memcpy(unmasked,render.fb,pixels*sizeof(*unmasked)); continue; }
        uint32_t bits=0;
        for(unsigned channel=0;channel<4;channel++)
          if(requested&(1u<<channel)) bits|=channel_bits[channel];
        for(size_t index=0;index<pixels;index++){
          uint32_t expected=(initial[index]&~bits)|(unmasked[index]&bits);
          if(render.fb[index]!=expected){
            fprintf(stderr,"sprite channel mask %u, width %d, variant %d, classic %d, "
                    "interp %d, pixel %zu: %08x != %08x\n",requested,width,variant,
                    classic,interp,index,render.fb[index],expected);
            gml_render_free(&render); return 1;
          }
        }
      }
    }
  gml_render_free(&render);
  return 0;
}

static int masked_sprite_surface_case(int classic,uint32_t expected_blue){
  enum { WIDTH=4,HEIGHT=3 };
  GmlRender render;
  uint32_t frame[WIDTH*HEIGHT];
  memset(&render,0,sizeof render);
  for(size_t index=0;index<WIDTH*HEIGHT;index++) frame[index]=0xFF000000u;
  render.fb=render.base_fb=frame;
  render.fbw=render.base_fbw=WIDTH;
  render.fbh=render.base_fbh=HEIGHT;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;
  render.classic=classic;
  render.n_spr=1;
  render.spr=calloc(1,sizeof(*render.spr));
  REQUIRE(render.spr!=NULL,"masked sprite allocation");
  render.spr[0].w=render.spr[0].h=1;
  render.spr[0].n_frames=1;
  render.spr[0].runtime_owned=1;
  render.spr[0].runtime_opaque=1;
  render.spr[0].runtime_rgba=malloc(4);
  REQUIRE(render.spr[0].runtime_rgba!=NULL,"masked sprite pixels");
  render.spr[0].runtime_rgba[0]=0;
  render.spr[0].runtime_rgba[1]=0;
  render.spr[0].runtime_rgba[2]=255;
  render.spr[0].runtime_rgba[3]=255;

  int surface=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(surface==1,"masked sprite surface id");
  REQUIRE(gml_surface_set_target(&render,surface),"masked sprite set target");
  gml_render_clear(&render,0xFFFFFFu,0.25);
  render.color_write_mask=0x08;
  gml_draw_sprite_ext(&render,0,0,1.0,1.0,1.0,1.0,0.0,0xFFFFFFu,1.0);
  const uint32_t *surface_pixels_now=surface_pixels(&render,surface,NULL,NULL);
  REQUIRE(surface_pixels_now[0]==0x40FFFFFFu,
          "alpha-only sprite preserves surface rgb");
  REQUIRE(surface_pixels_now[WIDTH+1]==0xFFFFFFFFu,
          "alpha-only sprite updates selected alpha");
  render.color_write_mask=0x07;
  gml_draw_sprite_tiled_ext(&render,0,0,0.0,0.0,1.0,1.0,0xFFFFFFu,1.0);
  REQUIRE(surface_pixels_now[0]==0x400000FFu,
          "rgb-only tiled sprite preserves surface alpha");
  REQUIRE(surface_pixels_now[WIDTH+1]==0xFF0000FFu,
          "rgb-only tiled sprite preserves opaque mask alpha");
  render.color_write_mask=0x0F;
  gml_surface_reset_target(&render);
  gml_draw_surface_stretched(&render,surface,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  /* The same channel operands distinguish classic rounding from modern truncation of
   * the final eighth-bit blend shift. */
  if(frame[0]!=expected_blue || frame[WIDTH+1]!=0xFF0000FFu){
    fprintf(stderr,"renderer masked surface composite mismatch: %08x / %08x\n",
            frame[0],frame[WIDTH+1]);
    gml_render_free(&render);
    return 1;
  }
  gml_render_free(&render);
  return 0;
}

static int world_raster_scale_case(void){
  enum { TARGET_WIDTH=12,TARGET_HEIGHT=8 };
  GmlRender render;
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  memset(&render,0,sizeof render);
  memset(target,0,sizeof target);
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;

  int source=gml_surface_create(&render,1,1);
  int offscreen=gml_surface_create(&render,6,4);
  REQUIRE(source==1 && offscreen==2,"world raster surface ids");
  uint32_t *source_pixel=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixel!=NULL,"world raster source pixel");
  *source_pixel=0xFF30A070u;
  render.surface[0].opaque_known=1;
  render.surface[0].all_opaque=1;
  render.surface[0].all_transparent=0;

  gml_render_begin(&render,target,TARGET_WIDTH,TARGET_HEIGHT,1.5,0.5);
  gml_render_world_set_logical_extent(&render,6,4);
  GmlRenderTargetMetrics metrics={0};
  REQUIRE(gml_render_target_metrics(&render,&metrics) &&
          metrics.width==TARGET_WIDTH && metrics.height==TARGET_HEIGHT &&
          metrics.camera_x==3.0 && metrics.camera_y==1.0,
          "world raster physical target metrics");
  double point_x=2.0,point_y=1.0,scale_x=1.5,scale_y=0.5;
  gml_render_draw_map_point(&render,&point_x,&point_y);
  gml_render_draw_map_scale(&render,&scale_x,&scale_y);
  REQUIRE(point_x==4.0 && point_y==2.0 && scale_x==3.0 && scale_y==1.0,
          "world raster coordinate transform");

  gml_draw_surface_stretched(&render,source,2.0,1.0,2.0,1.0,0xFFFFFFu,1.0);
  for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
    int covered=x>=1 && x<5 && y>=1 && y<3;
    uint32_t expected=covered?0xFF30A070u:0;
    if(target[(size_t)y*TARGET_WIDTH+x]!=expected){
      fprintf(stderr,"renderer world raster mismatch at %d,%d: %08x != %08x\n",
              x,y,target[(size_t)y*TARGET_WIDTH+x],expected);
      gml_surface_free(&render,source);
      gml_surface_free(&render,offscreen);
      return 1;
    }
  }

  REQUIRE(gml_surface_set_target(&render,offscreen),"world raster offscreen target");
  gml_draw_surface_stretched(&render,source,1.0,1.0,2.0,1.0,0xFFFFFFu,1.0);
  gml_surface_reset_target(&render);
  int offscreen_width=0,offscreen_height=0;
  const uint32_t *offscreen_pixels=
    gml_surface_pixels_read(&render,offscreen,&offscreen_width,&offscreen_height);
  REQUIRE(offscreen_pixels && offscreen_width==6 && offscreen_height==4,
          "world raster offscreen pixels");
  for(int y=0;y<offscreen_height;y++) for(int x=0;x<offscreen_width;x++){
    int covered=x>=1 && x<3 && y==1;
    uint32_t expected=covered?0xFF30A070u:0;
    if(offscreen_pixels[(size_t)y*offscreen_width+x]!=expected){
      fprintf(stderr,"renderer world raster offscreen mismatch at %d,%d\n",x,y);
      gml_surface_free(&render,source);
      gml_surface_free(&render,offscreen);
      return 1;
    }
  }
  gml_surface_free(&render,source);
  gml_surface_free(&render,offscreen);
  return 0;
}

static int replace_and_classic_present_case(void){
  /* The base canvas is wider than the surface so the whole-frame copy fast paths stand down
   * and the per-run kernel — the one the defect lived in — is what this case exercises. */
  enum { WIDTH=4,HEIGHT=3,BASEW=6,BASEH=5 };
  GmlRender render;
  uint32_t frame[BASEW*BASEH];
  memset(&render,0,sizeof render);
  memset(frame,0,sizeof frame);
  render.fb=render.base_fb=frame;
  render.fbw=render.base_fbw=BASEW;
  render.fbh=render.base_fbh=BASEH;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;

  /* A compositor draws a translucent pass into its surface: the colour is resolved there and
   * the destination-alpha arithmetic leaves partial coverage behind. */
  int source=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(source==1,"replace case surface id");
  REQUIRE(gml_surface_set_target(&render,source),"replace case surface target");
  gml_render_clear(&render,0x000000u,1.0);
  render.alpha=0.5;
  gml_render_primitive_rectangle(&render,1,1,1,1,0x302010u,0);
  render.alpha=1.0;
  gml_surface_reset_target(&render);
  uint32_t *pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(pixels!=NULL,"replace case surface pixels");
  uint32_t held=pixels[1*WIDTH+1];
  REQUIRE((held>>24)>0u && (held>>24)<255u,
          "replace case surface holds partial coverage");
  render.surface[0].opaque_known=0;
  render.surface[0].all_opaque=0;
  render.surface[0].all_transparent=0;

  /* (one, zero) copies what the surface holds, coverage included. */
  render.blendmode=6;
  memset(frame,0,sizeof frame);
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE(frame[1*BASEW+1]==held,
          "one-zero surface copy lands the texel verbatim");
  render.blendmode=0;

  /* The classic automatic present models an alphaless backbuffer: the colour reaches the
   * screen untouched however much coverage the surface accumulated. */
  render.classic=1;
  render.app_surface=pixels;
  memset(frame,0,sizeof frame);
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE((frame[1*BASEW+1]&0x00FFFFFFu)==(held&0x00FFFFFFu),
          "classic present keeps the composed colour");
  REQUIRE((frame[1*BASEW+1]>>24)==0xFFu,
          "classic present writes an opaque screen pixel");

  /* An ordinary surface draw keeps blending by coverage: only the pad above is exempt. */
  render.app_surface=NULL;
  memset(frame,0,sizeof frame);
  gml_draw_surface_stretched(&render,source,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  REQUIRE((frame[1*BASEW+1]&0x00FFFFFFu)!=(held&0x00FFFFFFu),
          "an ordinary surface draw still blends partial coverage");

  gml_surface_free(&render,source);
  return 0;
}

/* Sampling a cleared application surface while another target is bound
 * must realize its pending fill before copying pixels. */
static int suspended_clear_read_back_case(void){
  enum { WIDTH=4,HEIGHT=3 };
  GmlRender render;
  uint32_t screen[WIDTH*HEIGHT];
  memset(&render,0,sizeof render);
  for(int i=0;i<WIDTH*HEIGHT;i++) screen[i]=0xFF3060A0u;
  render.fb=render.base_fb=screen;
  render.fbw=render.base_fbw=WIDTH;
  render.fbh=render.base_fbh=HEIGHT;
  render.app_surface=screen;
  render.app_w=WIDTH;
  render.app_h=HEIGHT;
  render.target_id=-1;
  render.next_surface_id=1;
  render.alphablend=1;
  render.alpha=1.0;
  render.color_write_mask=0x0F;
  render.blend_equation=1;
  render.blend_equation_alpha=1;
  render.app_draw_enable=1;
  render.active_shader=-1;
  render.lut_pal_sprite=-1;

  int composed=gml_surface_create(&render,WIDTH,HEIGHT);
  REQUIRE(composed==1,"composed surface id");
  gml_render_dispatch_diagnostics_enable(&render,1);
  gml_render_clear(&render,0x000000u,1.0);
  REQUIRE(screen[0]==0xFF3060A0u,
          "the clear was performed at once, so this case cannot observe the deferred one");
  REQUIRE(gml_surface_set_target(&render,composed),"composed surface target");
  gml_draw_surface_stretched(&render,0,0.0,0.0,WIDTH,HEIGHT,0xFFFFFFu,1.0);
  const uint32_t *composed_pixels=surface_pixels(&render,composed,NULL,NULL);
  REQUIRE(composed_pixels!=NULL,"composed surface pixels");
  for(int i=0;i<WIDTH*HEIGHT;i++)
    REQUIRE((composed_pixels[i]&0x00FFFFFFu)==0u,
            "the composed surface carries the picture the clear replaced");
  GmlRenderDispatchMetrics metrics={0};
  REQUIRE(gml_render_dispatch_metrics(&render,&metrics) &&
          metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE]==1u &&
          metrics.cpu[GML_RENDER_SCENE_SURFACE][GML_RENDER_DISPATCH_NO_DEVICE]==1u,
          "the suspended clear and its subsequent composition both executed on CPU");
  gml_surface_reset_target(&render);
  gml_surface_free(&render,composed);
  return 0;
}

static int sprite_identity_executor(void *context,const GmlRenderShaderRequest *request){
  int *calls=context;
  (*calls)++;
  if(!request->source || !request->output || request->width!=3 || request->height!=2 ||
     request->source_width!=3 || request->source_height!=2) return 0;
  for(int y=0;y<2;y++) for(int x=0;x<3;x++)
    request->output[y*3+x]=request->source[y*request->source_pitch+x];
  return 1;
}

static int filtered_additive_shader_sprite_case(void){
  GmlRender render={0};GmlWin content={0};GmlSprite sprite={0};
  GmlTpag page={0};GmlAtlas atlas={0};struct GmlShaderPal program={0};
  uint32_t base[24]={0},plain[24];int frame=0,calls=0;
  uint8_t pixels[24]={17,35,53,152,71,89,107,196,125,143,161,0,
                      179,197,215,255,233,15,33,32,51,69,87,64};
  REQUIRE(gml_render_init(&render,NULL)==0,"identity sprite renderer initialization");
  GmlAtlas *owned_atlas=render.atlas;int owned_atlas_count=render.n_atlas;
  content.bytecode=17;render.win=&content;
  gml_render_begin(&render,base,6,4,0,0);
  render.interp=1;render.blendmode=1;render.active_shader=-1;
  render.spr=&sprite;render.n_spr=1;render.tpag=&page;render.n_tpag=1;
  render.atlas=&atlas;render.n_atlas=1;
  sprite.w=3;sprite.h=2;sprite.n_frames=1;sprite.frame=&frame;
  page.sw=page.tw=page.bw=3;page.sh=page.th=page.bh=2;
  atlas.px=pixels;atlas.w=3;atlas.h=2;atlas.decode_attempted=1;
  program.source_vertex_es="void main(){gl_Position=vec4(0.0);}";
  program.source_fragment_es="void main(){gl_FragColor=vec4(1.0);}";
  render.shader_pal=&program;render.n_shader_pal=1;
  int target=gml_surface_create(&render,6,4);
  REQUIRE(target>0 && gml_surface_set_target(&render,target),"identity sprite numbered target");
  gml_render_set_shader_executor(&render,sprite_identity_executor,&calls);
  for(int variant=0;variant<8;variant++){
    double x=(variant&1)?-1.0:0.0,y=(variant&1)?-1.0:0.0;
    double alpha=(variant&2)?0.6:1.0;
    uint32_t colour=(variant&4)?0u:0xffffffu;
    render.active_shader=-1;gml_render_clear(&render,0,0);
    gml_draw_sprite_ext(&render,0,0,x,y,1,1,0,colour,alpha);
    gml_render_maybe_prepare_draw(&render);
    memcpy(plain,render.fb,sizeof plain);
    if(variant==0) REQUIRE((plain[0]>>24)==91u,"plain fragment coverage control");
    gml_render_clear(&render,0,0);render.active_shader=0;
    gml_draw_sprite_ext(&render,0,0,x,y,1,1,0,colour,alpha);
    gml_render_maybe_prepare_draw(&render);
    if(memcmp(plain,render.fb,sizeof plain)){
      fprintf(stderr,"filtered additive identity sprite variant %d: plain=%08x shaded=%08x\n",
              variant,plain[0],render.fb[0]);
      return 1;
    }
    REQUIRE(render.active_shader==0,"identity sprite restores the content binding");
  }
  uint32_t plane[6];
  for(int i=0;i<6;i++) plane[i]=((uint32_t)pixels[i*4+3]<<24)|
    ((uint32_t)pixels[i*4]<<16)|((uint32_t)pixels[i*4+1]<<8)|pixels[i*4+2];
  for(int variant=0;variant<6;variant++){
    double rx=variant==5?1.0:0.0,rw=variant==5?2.0:3.0;
    double dx=variant==4?0.5:0.0,dw=variant==3?6.0:rw;
    render.interp=variant==0?0:1;render.blendmode=variant==1?0:1;
    render.color_write_mask=variant==2?7u:15u;
    render.active_shader=-1;gml_render_clear(&render,0,0);
    REQUIRE(gml_render_compose_shaded_plane(&render,plane,3,2,rx,0,rw,2,
                                            dx,0,dw,2,0xffffffu,1),
            "excluded sprite surface-composition control");
    gml_render_maybe_prepare_draw(&render);memcpy(plain,render.fb,sizeof plain);
    gml_render_clear(&render,0,0);render.active_shader=0;
    REQUIRE(gml_render_shade_target_sprite_part(&render,0,0,rx,0,rw,2,
                                                dx,0,dw,2,0xffffffu,1),
            "excluded sprite shaded draw");
    gml_render_maybe_prepare_draw(&render);
    REQUIRE(memcmp(plain,render.fb,sizeof plain)==0,
            "excluded sprite preserves its established composition path");
    REQUIRE(render.active_shader==0,"excluded sprite restores the content binding");
  }
  REQUIRE(calls==1,"identity sprite executes and reuses the immutable shaded page");
  render.active_shader=-1;gml_surface_reset_target(&render);
  gml_render_texture_page_cache_clear(&render,&page);
  render.spr=NULL;render.n_spr=0;render.tpag=NULL;render.n_tpag=0;
  render.atlas=owned_atlas;render.n_atlas=owned_atlas_count;
  render.shader_pal=NULL;render.n_shader_pal=0;
  gml_render_free(&render);
  return 0;
}

static int replacement_sprite_variant(int interpolation,int mode,double alpha,
                                      unsigned source_alpha,unsigned mask,int discard){
  GmlRender *render=calloc(1,sizeof *render);
  GmlWin content={0};GmlSprite sprite={0};GmlTpag page={0};GmlAtlas atlas={0};
  struct GmlShaderPal program={0};
  uint32_t base[16]={0};uint8_t texel[4]={255,255,255,(uint8_t)source_alpha};int frame=0;
  REQUIRE(render!=NULL,"replacement sprite renderer allocation");
  REQUIRE(gml_render_init(render,NULL)==0,"replacement sprite renderer initialization");
  content.bytecode=17;render->win=&content;
  gml_render_begin(render,base,4,4,0,0);
  render->interp=interpolation;render->blendmode=mode;render->color_write_mask=mask;
  render->active_shader=-1;
  sprite.w=sprite.h=1;sprite.n_frames=1;sprite.frame=&frame;
  page.sw=page.sh=page.tw=page.th=page.bw=page.bh=1;
  atlas.w=atlas.h=1;atlas.px=texel;atlas.decode_attempted=1;
  render->spr=&sprite;render->n_spr=1;render->tpag=&page;render->n_tpag=1;
  render->atlas=&atlas;render->n_atlas=1;
  if(discard==1){render->alpha_test_enable=1;render->alpha_test_ref=128;}
  if(discard==2){
    program.alpha_discard=program.alpha_discard_inclusive=1;
    program.alpha_discard_cutoff=0.5f;
    render->shader_pal=&program;render->n_shader_pal=1;render->active_shader=0;
  }
  int target=gml_surface_create(render,4,4);
  REQUIRE(target>0 && gml_surface_set_target(render,target),"replacement sprite numbered target");
  for(int i=0;i<16;i++) render->fb[i]=0xff563412u;
  render->fb_opaque_known=render->fb_all_opaque=1;render->fb_all_transparent=0;
  gml_draw_sprite_ext(render,0,0,1,1,2,2,0,0x332211u,alpha);
  gml_render_maybe_prepare_draw(render);
  uint32_t fragment=((uint32_t)lround(alpha*source_alpha)<<24)|0x112233u;
  uint32_t bits=(mask&1u?0xff0000u:0)|(mask&2u?0xff00u:0)|
    (mask&4u?0xffu:0)|(mask&8u?0xff000000u:0);
  uint32_t expected=(0xff563412u&~bits)|(fragment&bits);
  int rejected=(discard==1 && source_alpha<=128u) ||
               (discard==2 && source_alpha<=127u);
  if(mode==0 || rejected) expected=0xff563412u;
  int failures=0;
  for(int y=0;y<4;y++) for(int x=0;x<4;x++){
    uint32_t want=(x>=1 && x<=2 && y>=1 && y<=2)?expected:0xff563412u;
    if(render->fb[y*4+x]!=want) failures++;
  }
  if(mode==6 && !rejected && (fragment>>24)<255u && (mask&8u) &&
     render->fb_all_opaque) failures++;
  if(failures)
    fprintf(stderr,"replacement sprite: interp=%d mode=%d alpha=%g source=%u mask=%u "
                   "discard=%d middle=%08x expected=%08x failures=%d\n",
            interpolation,mode,alpha,source_alpha,mask,discard,render->fb[5],expected,failures);
  gml_surface_reset_target(render);
  gml_render_texture_page_cache_clear(render,&page);
  render->win=NULL;render->spr=NULL;render->n_spr=0;render->tpag=NULL;render->n_tpag=0;
  render->atlas=NULL;render->n_atlas=0;render->shader_pal=NULL;render->n_shader_pal=0;
  gml_render_free(render);free(render);
  return failures!=0;
}

static int replacement_sprite_case(void){
  const double alpha[]={0,0.5,1};const unsigned source_alpha[]={0,128,255};
  int failures=0;
  for(int interpolation=0;interpolation<2;interpolation++){
    failures+=replacement_sprite_variant(interpolation,0,0,255,15,0);
    for(unsigned a=0;a<3;a++) for(unsigned s=0;s<3;s++) for(unsigned mask=0;mask<16;mask++)
      failures+=replacement_sprite_variant(interpolation,6,alpha[a],source_alpha[s],mask,0);
    for(int discard=1;discard<=2;discard++) for(unsigned s=0;s<3;s++)
      failures+=replacement_sprite_variant(interpolation,6,1,source_alpha[s],15,discard);
  }
  REQUIRE(failures==0,"nearest and filtered replacement sprites, masks, coverage and discard");
  return 0;
}

int main(void){
  GmlRender render;
  uint32_t base[8*6];
  memset(&render,0,sizeof render);
  memset(base,0,sizeof base);
  render.fb=render.base_fb=base;
  render.fbw=render.base_fbw=8;
  render.fbh=render.base_fbh=6;
  render.target_id=-1;
  render.next_surface_id=1;

  int source=gml_surface_create(&render,4,3);
  int destination=gml_surface_create(&render,6,5);
  REQUIRE(source==1 && destination==2,"surface ids");
  uint32_t *source_pixels=surface_pixels(&render,source,NULL,NULL);
  REQUIRE(source_pixels!=NULL,"source pixels");
  for(int y=0;y<3;y++) for(int x=0;x<4;x++){
    unsigned red=(unsigned)(31+x*47+y*13);
    unsigned green=(unsigned)(17+x*19+y*53);
    unsigned blue=(unsigned)(7+x*29+y*37);
    source_pixels[(size_t)y*4+x]=0xFF000000u|(red<<16)|(green<<8)|blue;
  }

  render.projection_cam_x=4.0;
  render.projection_cam_y=5.0;
  render.cam_x=render.projection_cam_x;
  render.cam_y=render.projection_cam_y;
  REQUIRE(gml_surface_set_target(&render,source),"set target");
  REQUIRE(render.fb==source_pixels && render.fbw==4 && render.fbh==3 &&
          render.cam_x==0.0 && render.cam_y==0.0,"target state");
  render.fb_opaque_known=1;
  render.fb_all_opaque=1;
  render.fb_all_transparent=0;
  gml_surface_reset_target(&render);
  REQUIRE(render.fb==base && render.fbw==8 && render.fbh==6 &&
          render.cam_x==4.0 && render.cam_y==5.0 &&
          render.projection_cam_x==4.0 && render.projection_cam_y==5.0,
          "restored target state");
  REQUIRE(surface_known_opaque(&render,source),"opaque coverage");

  int part_destination=gml_surface_create(&render,6,5);
  REQUIRE(part_destination>0,"copy-part destination surface");
  uint32_t *destination_before=surface_pixels(&render,part_destination,NULL,NULL);
  REQUIRE(destination_before!=NULL,"copy-part destination pixels");
  for(int i=0;i<6*5;i++) destination_before[i]=0xFF010203u;
  gml_surface_copy_part(&render,part_destination,2,1,source,1,1,2,2);
  REQUIRE(destination_before[(size_t)1*6+2]==source_pixels[(size_t)1*4+1] &&
          destination_before[(size_t)1*6+3]==source_pixels[(size_t)1*4+2] &&
          destination_before[(size_t)2*6+2]==source_pixels[(size_t)2*4+1] &&
          destination_before[(size_t)2*6+3]==source_pixels[(size_t)2*4+2] &&
          destination_before[(size_t)1*6+1]==0xFF010203u &&
          destination_before[(size_t)3*6+2]==0xFF010203u,
          "surface_copy_part copies only the selected rectangle");
  uint32_t overlap_source[4]={
    destination_before[(size_t)1*6+2], destination_before[(size_t)1*6+3],
    destination_before[(size_t)2*6+2], destination_before[(size_t)2*6+3]
  };
  gml_surface_copy_part(&render,part_destination,3,1,part_destination,2,1,2,2);
  REQUIRE(destination_before[(size_t)1*6+3]==overlap_source[0] &&
          destination_before[(size_t)1*6+4]==overlap_source[1] &&
          destination_before[(size_t)2*6+3]==overlap_source[2] &&
          destination_before[(size_t)2*6+4]==overlap_source[3],
          "surface_copy_part snapshots overlapping source pixels");
  gml_surface_free(&render,part_destination);

  gml_surface_copy(&render,destination,1,1,source);
  REQUIRE(gml_surface_width(&render,destination)==6 &&
          gml_surface_height(&render,destination)==5,"copy dimensions");
  gml_surface_resize(&render,destination,4,4);
  int width=0,height=0;
  const uint32_t *destination_pixels=
    gml_surface_pixels_read(&render,destination,&width,&height);
  REQUIRE(destination_pixels!=NULL && width==4 && height==4,"resize dimensions");
  uint64_t hash=pixel_hash(destination_pixels,(size_t)width*height);
  if(hash!=UINT64_C(0x7d8e0da85e14b8af)){
    fprintf(stderr,"renderer surfaces hash: %016llx\n",(unsigned long long)hash);
    return 1;
  }

  gml_surface_free(&render,source);
  gml_surface_free(&render,destination);
  REQUIRE(!gml_surface_exists(&render,source) &&
          !gml_surface_exists(&render,destination),"freed surfaces");
  REQUIRE(presentation_latch_coverage_case()==0,
          "presentation latch coverage case");
  REQUIRE(owned_application_begin_case()==0,"owned application begin case");
  REQUIRE(surface_budget_refusal_case()==0,"surface budget refusal case");
  REQUIRE(numbered_surface_resize_bound_case()==0,
          "numbered surface resize bound target case");
  REQUIRE(numbered_surface_resize_pending_fill_case()==0,
          "numbered surface resize pending fill case");
  REQUIRE(numbered_surface_free_bound_case()==0,
          "numbered surface free bound target case");
  REQUIRE(threshold_palette_surface_case()==0,
          "threshold palette surface case");
  REQUIRE(declared_canvas_preview_case()==0,"declared canvas preview case");
  REQUIRE(composition_cases()==0,"composition cases");
  REQUIRE(destination_colour_factor_case()==0,"destination-colour factor case");
  REQUIRE(destination_colour_sprite_case()==0,"destination-colour sprite case");
  REQUIRE(screen_raster_part_case()==0,"screen raster part case");
  REQUIRE(first_generation_point_sampling_case()==0,
          "first-generation point sampling case");
  REQUIRE(first_generation_high_magnification_case()==0,
          "first-generation high magnification case");
  REQUIRE(opaque_integer_scale_case()==0,"opaque integer scale case");
  REQUIRE(automatic_underlay_shader_case()==0,"automatic underlay shader case");
  REQUIRE(deferred_shader_alpha_case()==0,"deferred shader alpha case");
  REQUIRE(deferred_shader_geometry_case()==0,"deferred shader geometry case");
  REQUIRE(filtered_additive_shader_sprite_case()==0,"filtered additive shader sprite case");
  REQUIRE(replacement_sprite_case()==0,"replacement sprite case");
  REQUIRE(point_downscale_case()==0,"point downscale case");
  REQUIRE(opaque_near_identity_reduction_case()==0,
          "opaque near-identity reduction case");
  REQUIRE(zero_reference_alpha_test_case()==0,
          "zero-reference alpha-test case");
  REQUIRE(max_preset_surface_case()==0,"maximum preset surface case");
  REQUIRE(subtract_surface_coverage_case()==0,"subtract surface coverage case");
  REQUIRE(masked_sprite_surface_case(1,0xFF000040u)==0,
          "classic masked sprite surface case");
  REQUIRE(masked_sprite_surface_case(0,0xFF00003Fu)==0,
          "modern masked sprite surface case");
  REQUIRE(masked_sprite_channels_case()==0,"all sprite channel masks and restore tails");
  REQUIRE(world_raster_scale_case()==0,"world raster scale case");
  REQUIRE(replace_and_classic_present_case()==0,"replace and classic present case");
  REQUIRE(suspended_clear_read_back_case()==0,"suspended clear read-back case");
  REQUIRE(dispatch_surface_coverage_case()==0,"resolved CPU surface dispatch coverage");
  puts("renderer surfaces: ok");
  return 0;
}
