/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"
#include "gml_render_primitives.h"
#include "gml_render_state.h"
#include "gml_render_cpu_text.h"
#include "gml_render_cpu_workers.h"
#include "gml_render_cpu_dispatch_internal.h"
#include "gml_render_cpu_backend.h"
#include "gml_render_cpu_primitives.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(int condition,const char *message){
  if(condition) return;
  fprintf(stderr,"renderer primitives: %s\n",message);
  failures++;
}

enum { WIDTH=200,HEIGHT=200 };

static uint32_t frame[WIDTH*HEIGHT];

static void reset_render(GmlRender *render,int precision){
  memset(render,0,sizeof *render);
  memset(frame,0,sizeof frame);
  render->fb=render->base_fb=frame;
  render->fbw=render->base_fbw=WIDTH;
  render->fbh=render->base_fbh=HEIGHT;
  render->target_id=-1;
  render->next_surface_id=1;
  render->alphablend=1;
  render->alpha=1.0;
  render->color_write_mask=0x0F;
  render->blend_equation=1;
  render->blend_equation_alpha=1;
  render->app_draw_enable=1;
  render->active_shader=-1;
  render->lut_pal_sprite=-1;
  render->circle_precision=precision;
}

static void set_world_scale(GmlRender *render,double scale_x,double scale_y){
  render->world_transform_active=1;
  render->world_scale_x=scale_x;
  render->world_scale_y=scale_y;
}

static int covered_pixels(void){
  int count=0;
  for(int index=0;index<WIDTH*HEIGHT;index++)
    if(frame[index]&0x00FFFFFFu) count++;
  return count;
}

/* Bounding box of the drawn shape. A regular polygon with an even side count is centrally
 * symmetric, so the centre of this box is the polygon centre at any orientation. That makes it a
 * model-free probe of where the primitive was actually placed. */
static void covered_bounds(int *min_x,int *min_y,int *max_x,int *max_y){
  *min_x=WIDTH; *min_y=HEIGHT; *max_x=-1; *max_y=-1;
  for(int y=0;y<HEIGHT;y++)
    for(int x=0;x<WIDTH;x++){
      if(!(frame[y*WIDTH+x]&0x00FFFFFFu)) continue;
      if(x<*min_x) *min_x=x;
      if(x>*max_x) *max_x=x;
      if(y<*min_y) *min_y=y;
      if(y>*max_y) *max_y=y;
    }
}

/* Filled circles use a regular polygon sampled at pixel centres. Compute containment independently
 * so the assertion compares the renderer against the stated contract rather than its own output. */
static int polygon_contains(double px,double py,double cx,double cy,double rx,double ry,int sides){
  for(int i=0;i<sides;i++){
    double a1=(i*2.0*M_PI)/sides,a2=(((i+1)%sides)*2.0*M_PI)/sides;
    double x1=cx+cos(a1)*rx,y1=cy-sin(a1)*ry;
    double x2=cx+cos(a2)*rx,y2=cy-sin(a2)*ry;
    if((x2-x1)*(py-y1)-(y2-y1)*(px-x1)>0.0) return 0;
  }
  return 1;
}

static int expected_coverage(double cx,double cy,double rx,double ry,int sides){
  int count=0;
  for(int y=0;y<HEIGHT;y++)
    for(int x=0;x<WIDTH;x++)
      if(polygon_contains(x+0.5,y+0.5,cx,cy,rx,ry,sides)) count++;
  return count;
}

/* A fractional radius must not be quantized before rasterization. Truncating it discards up to a
 * whole destination pixel of extent, which is visible as a shrunken shape wherever the mapped
 * radius is not already an integer. */
static void check_fractional_radius_is_not_truncated(void){
  GmlRender render;
  int truncated,fractional;

  reset_render(&render,12);
  gml_render_primitive_circle_subpixel(&render,100.0,100.0,20.0,20.0,0xFFFFFFu,0);
  truncated=covered_pixels();

  reset_render(&render,12);
  gml_render_primitive_circle_subpixel(&render,100.0,100.0,20.75,20.75,0xFFFFFFu,0);
  fractional=covered_pixels();

  expect(fractional>truncated,
         "a fractional radius covered no more than its truncated value");
  expect(fractional==expected_coverage(100.0,100.0,20.75,20.75,12),
         "fractional radius coverage did not match pixel-centre containment");
  expect(truncated==expected_coverage(100.0,100.0,20.0,20.0,12),
         "integral radius coverage did not match pixel-centre containment");
}

/* A fractional centre must survive too, and must move the shape without resizing it. */
static void check_fractional_centre_is_not_quantized(void){
  GmlRender render;
  int aligned,shifted;
  int min_x,min_y,max_x,max_y;

  reset_render(&render,12);
  gml_render_primitive_circle_subpixel(&render,100.0,100.0,20.0,20.0,0xFFFFFFu,0);
  aligned=covered_pixels();
  covered_bounds(&min_x,&min_y,&max_x,&max_y);
  expect((min_x+max_x+1)==200 && (min_y+max_y+1)==200,
         "integral centre did not land on the requested coordinate");

  reset_render(&render,12);
  gml_render_primitive_circle_subpixel(&render,100.5,100.0,20.0,20.0,0xFFFFFFu,0);
  shifted=covered_pixels();
  expect(shifted==expected_coverage(100.5,100.0,20.0,20.0,12),
         "half-pixel centre coverage did not match pixel-centre containment");
  expect(shifted!=aligned || expected_coverage(100.5,100.0,20.0,20.0,12)==aligned,
         "half-pixel centre was quantized away");
}

/* The integer entry point must keep producing exactly what it produced before the continuous one
 * existed, because existing call sites and recorded results depend on it. */
static void check_integer_entry_point_is_unchanged(void){
  GmlRender render;
  int through_int,through_double;

  reset_render(&render,12);
  gml_render_primitive_circle(&render,100,100,20,20,0xFFFFFFu,0);
  through_int=covered_pixels();

  reset_render(&render,12);
  gml_render_primitive_circle_subpixel(&render,100.0,100.0,20.0,20.0,0xFFFFFFu,0);
  through_double=covered_pixels();

  expect(through_int==through_double,
         "the integer circle entry point diverged from the continuous one");
  expect(through_int==expected_coverage(100.0,100.0,20.0,20.0,12),
         "integer circle coverage did not match pixel-centre containment");
}

/* Apply the one-unit logical centre bias before view mapping, so a view magnified by two moves
 * the shape by two destination pixels rather than one. */
static void check_circle_geometry_applies_the_measured_bias(void){
  GmlRender render;
  double cx,cy,rx,ry;

  reset_render(&render,12);
  gml_render_circle_geometry(&render,30.0,40.0,11.0,&cx,&cy,&rx,&ry);
  expect(cx==30.0+GML_RENDER_CIRCLE_CENTER_BIAS && cy==40.0+GML_RENDER_CIRCLE_CENTER_BIAS,
         "unscaled circle centre bias was not applied");
  expect(rx==11.0 && ry==11.0,"unscaled circle radius was altered");

  reset_render(&render,12);
  set_world_scale(&render,2.0,2.0);
  gml_render_circle_geometry(&render,30.0,40.0,11.0,&cx,&cy,&rx,&ry);
  expect(cx==2.0*(30.0+GML_RENDER_CIRCLE_CENTER_BIAS) &&
         cy==2.0*(40.0+GML_RENDER_CIRCLE_CENTER_BIAS),
         "circle centre bias did not scale with the view");
  expect(rx==22.0 && ry==22.0,"scaled circle radius was not mapped continuously");

  reset_render(&render,12);
  set_world_scale(&render,2.0,2.0);
  gml_render_circle_geometry(&render,30.0,40.0,11.003,&cx,&cy,&rx,&ry);
  expect(fabs(rx-22.006)<1e-9 && fabs(ry-22.006)<1e-9,
         "a fractional mapped radius was quantized by the geometry mapping");
}

static void check_dispatch_metadata_does_not_enter_state(void){
  GmlRender render;
  GmlRenderDispatchMetrics metrics;
  reset_render(&render,12);
  size_t size=gml_render_state_size(&render,0),first_written=0,second_written=0;
  uint8_t *first=(uint8_t*)malloc(size),*second=(uint8_t*)malloc(size);
  expect(size && first && second,"dispatch state control allocation failed");
  if(!size || !first || !second){free(first);free(second);return;}
  expect(gml_render_state_save(&render,0,first,size,&first_written),
         "dispatch baseline state could not be saved");
  gml_render_dispatch_diagnostics_enable(&render,1);
  gml_render_dispatch_cpu_record(&render,GML_RENDER_SCENE_PRIMITIVE,
                                 GML_RENDER_DISPATCH_NO_SCENE,3u);
  gml_render_dispatch_cpu_exception_record(&render,GML_RENDER_SCENE_PRIMITIVE,
                                           GML_RENDER_DISPATCH_EXCEPTION_DEPTH,7u);
  expect(gml_render_dispatch_metrics(&render,&metrics) && !metrics.complete &&
         metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_SCENE]==3u,
         "dispatch metadata control did not record its operations");
  GmlRenderDispatchCpuScope scope;
  gml_render_dispatch_cpu_scope_begin(&render,&scope,GML_RENDER_SCENE_PRIMITIVE);
  gml_render_dispatch_cpu_scope_write(&render);
  expect(gml_render_state_size(&render,0)==size &&
         gml_render_state_save(&render,0,second,size,&second_written) &&
         first_written==size && second_written==size && !memcmp(first,second,size),
         "dispatch diagnostics entered canonical state bytes or changed their size");
  gml_render_dispatch_cpu_scope_end(&render,&scope);
  expect(!render.dispatch_cpu_depth,"state observation left an active CPU dispatch scope");
  free(first);free(second);
}

static void check_dispatch_exception_rejection_and_overflow(void){
  GmlRender render; GmlRenderDispatchMetrics metrics;
  reset_render(&render,12);
  gml_render_dispatch_diagnostics_enable(&render,1);
  gml_render_dispatch_cpu_exception_record(&render,GML_RENDER_SCENE_PRIMITIVE,
                                           GML_RENDER_DISPATCH_EXCEPTION_COUNT,1u);
  gml_render_dispatch_cpu_exception_record(&render,GML_RENDER_SCENE_UNCLASSIFIED,
                                           GML_RENDER_DISPATCH_EXCEPTION_DEPTH,1u);
  gml_render_dispatch_cpu_exception_record(&render,GML_RENDER_SCENE_PRIMITIVE,
                                           GML_RENDER_DISPATCH_EXCEPTION_DEPTH,UINT64_MAX);
  gml_render_dispatch_cpu_exception_record(&render,GML_RENDER_SCENE_PRIMITIVE,
                                           GML_RENDER_DISPATCH_EXCEPTION_DEPTH,1u);
  expect(gml_render_dispatch_metrics(&render,&metrics) && metrics.unclassified==2u &&
         metrics.overflow &&
         metrics.exceptions[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_EXCEPTION_DEPTH]==UINT64_MAX &&
         !metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE],
         "invalid or overflowing exceptions were accepted as an ordinary zero fallback");
}

static void check_dispatch_counts_cpu_only_pass_and_suffix(void){
  GmlRender render;
  GmlRenderDispatchMetrics metrics;
  uint32_t pixel;
  reset_render(&render,12);
  gml_render_primitive_point(&render,10,10,0x123456u);
  expect(gml_render_dispatch_metrics(&render,&metrics) && !metrics.enabled &&
         !metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE],
         "dispatch diagnostics changed the default path");
  gml_render_dispatch_diagnostics_enable(&render,1);
  for(int i=0;i<3;i++) gml_render_primitive_point(&render,11+i,10,0x123456u);
  expect(gml_render_dispatch_metrics(&render,&metrics) &&
         metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE]==3u,
         "CPU-only draws were omitted after the first dispatch");
  gml_render_primitive_point(&render,-1,10,0x123456u);
  render.alpha=0.0;
  gml_render_primitive_point(&render,11,11,0x123456u);
  expect(gml_render_target_pixel(&render,11,10,&pixel) && pixel==0xff563412u,
         "dispatch diagnostics altered the point's pixels");
  expect(gml_render_dispatch_metrics(&render,&metrics) &&
         metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE]==3u,
         "a clipped/zero-alpha point or pixel query counted as a draw");
  render.alpha=1.0;
  render.device_bound=1;
  render.device.context=&render;
  for(int i=0;i<3;i++) gml_render_primitive_point(&render,11+i,12,0x123456u);
  expect(gml_render_dispatch_metrics(&render,&metrics) &&
         metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_SCENE]==3u,
         "an inactive scene's complete CPU suffix was omitted");
  expect(!metrics.complete,"partial dispatcher coverage qualified a zero-fallback census");
}

static void check_dispatch_rejection_and_overflow_are_not_zero(void){
  GmlRender render;
  GmlRenderDispatchMetrics metrics;
  GmlRenderDrawOp operations[3]={{0}};
  GmlRenderDrawBatch batch={operations,3u,NULL,0u};
  reset_render(&render,12);
  gml_render_dispatch_diagnostics_enable(&render,1);
  operations[0].scene_family=GML_RENDER_SCENE_SPRITE;
  operations[1].scene_family=GML_RENDER_SCENE_TEXT;
  operations[2].scene_family=GML_RENDER_SCENE_UNCLASSIFIED;
  gml_render_dispatch_batch_record(&render,&batch,GML_RENDER_RESOURCE_EXEC_DEVICE);
  gml_render_dispatch_batch_record(&render,&batch,GML_RENDER_RESOURCE_EXEC_CPU);
  gml_render_dispatch_batch_record(&render,&batch,GML_RENDER_RESOURCE_EXEC_FAILED);
  expect(gml_render_dispatch_metrics(&render,&metrics) &&
         metrics.gpu[GML_RENDER_SCENE_SPRITE]==1u &&
         metrics.gpu[GML_RENDER_SCENE_TEXT]==1u && metrics.unclassified==2u &&
         metrics.cpu[GML_RENDER_SCENE_SPRITE][GML_RENDER_DISPATCH_REPLAY]==1u &&
         metrics.cpu[GML_RENDER_SCENE_TEXT][GML_RENDER_DISPATCH_REPLAY]==1u &&
         metrics.failed==3u,
         "failed batches, CPU replay or unknown families became completed GPU work");
  gml_render_dispatch_cpu_record(&render,GML_RENDER_SCENE_PRIMITIVE,
                                 GML_RENDER_DISPATCH_NO_DEVICE,UINT64_MAX);
  gml_render_dispatch_cpu_record(&render,GML_RENDER_SCENE_PRIMITIVE,
                                 GML_RENDER_DISPATCH_NO_DEVICE,1u);
  expect(gml_render_dispatch_metrics(&render,&metrics) && metrics.overflow &&
         metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE]==UINT64_MAX,
         "dispatch overflow wrapped a fallback counter to zero");
  expect(!gml_render_dispatch_metrics(NULL,&metrics) && !metrics.enabled &&
         !gml_render_dispatch_metrics(&render,NULL),
         "invalid diagnostic snapshots were accepted");
}

enum { DISPATCH_PRIMITIVE_CONTROLS=18 };
static void dispatch_primitive_control(GmlRender *render,int kind,int offset){
  int x=80+offset,y=80+offset;
  switch(kind){
    case 0:gml_render_primitive_point(render,x,y,0x123456u);break;
    case 1:gml_render_primitive_rectangle(render,x,y,x+15,y+12,0x123456u,0);break;
    case 2:gml_render_primitive_rectangle(render,x,y,x+15,y+12,0x123456u,1);break;
    case 3:gml_render_primitive_rectangle_color(render,x,y,x+15,y+12,
               0x123456u,0x234567u,0x345678u,0x456789u,0);break;
    case 4:gml_render_primitive_rectangle_color(render,x,y,x+15,y+12,
               0x123456u,0x234567u,0x345678u,0x456789u,1);break;
    case 5:gml_render_primitive_line(render,x,y,x+15,y+12,0x123456u,3);break;
    case 6:gml_render_primitive_line_color(render,x,y,x+15,y+12,
               0x123456u,0x345678u,3);break;
    case 7:gml_render_primitive_line_color_subpixel(render,x+0.25,y+0.25,
               x+15.25,y+12.25,0x123456u,0x345678u,1);break;
    case 8:gml_render_primitive_circle_subpixel(render,x,y,15,12,0x123456u,0);break;
    case 9:gml_render_primitive_circle_subpixel(render,x,y,15,12,0x123456u,1);break;
    case 10:gml_render_primitive_circle_subpixel(render,x,y,0,0,0x123456u,0);break;
    case 11:gml_render_primitive_circle_color_subpixel(render,x,y,15,12,
               0x123456u,0x345678u,0);break;
    case 12:gml_render_primitive_circle_color_subpixel(render,x,y,15,12,
               0x123456u,0x345678u,1);break;
    case 13:gml_render_primitive_circle_color_subpixel(render,x,y,0,0,
               0x123456u,0x345678u,0);break;
    case 14:gml_render_primitive_triangle_alpha(render,x,y,x+15,y,x,y+12,
               0x123456u,render->alpha,0);break;
    case 15:gml_render_primitive_triangle_alpha(render,x,y,x+15.25,y+0.25,x,y+12,
               0x123456u,render->alpha,0);break;
    case 16:gml_render_primitive_triangle_alpha(render,x,y,x+15,y,x,y+12,
               0x123456u,render->alpha,1);break;
    case 17:gml_render_primitive_rectangle_color(render,x,y,x+15,y+12,
               0x123456u,0x123456u,0x123456u,0x123456u,0);break;
  }
}

static void check_dispatch_complete_cpu_primitive_kernels(void){
  GmlRender render;
  GmlRenderDispatchMetrics metrics;
  uint32_t *baseline=(uint32_t*)malloc(sizeof frame);
  expect(baseline!=NULL,"primitive dispatch pixel control allocation failed");
  if(!baseline) return;
  for(int kind=0;kind<DISPATCH_PRIMITIVE_CONTROLS;kind++){
    for(int partial=0;partial<2;partial++){
      reset_render(&render,12);
      render.alpha=partial?0.5:1.0;
      dispatch_primitive_control(&render,kind,0);
      memcpy(baseline,frame,sizeof frame);
      reset_render(&render,12);
      render.alpha=partial?0.5:1.0;
      gml_render_dispatch_diagnostics_enable(&render,1);
      dispatch_primitive_control(&render,kind,0);
      expect(gml_render_dispatch_metrics(&render,&metrics) &&
             metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE]==1u &&
             !render.dispatch_cpu_depth && !memcmp(baseline,frame,sizeof frame),
             "a CPU primitive was omitted, counted per fragment or changed by diagnostics");
    }
    reset_render(&render,12);
    gml_render_dispatch_diagnostics_enable(&render,1);
    dispatch_primitive_control(&render,kind,WIDTH);
    render.alpha=0.0;
    dispatch_primitive_control(&render,kind,0);
    expect(gml_render_dispatch_metrics(&render,&metrics) &&
           !metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE] &&
           !covered_pixels() && !render.dispatch_cpu_depth,
           "a clipped or zero-alpha CPU primitive counted as executed work");
  }
  reset_render(&render,12);
  gml_render_dispatch_diagnostics_enable(&render,1);
  gml_render_primitive_triangle_alpha(&render,80,80,85,85,90,90,0x123456u,1.0,0);
  expect(gml_render_dispatch_metrics(&render,&metrics) &&
         !metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE],
         "a degenerate triangle counted as an executed CPU primitive");
  gml_draw_layer_color_fill(&render,0x123456u,0.5);
  expect(gml_render_dispatch_metrics(&render,&metrics) &&
         metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE]==1u,
         "a software room-layer colour fill was omitted");
  free(baseline);
}

static void check_dispatch_cpu_scope_nested(void){
  GmlRender render;
  GmlRenderDispatchCpuScope outer,inner;
  GmlRenderDispatchMetrics metrics;
  reset_render(&render,12);
  outer.reason=UINT_MAX;
  gml_render_dispatch_cpu_scope_begin(&render,&outer,GML_RENDER_SCENE_PRIMITIVE);
  expect(!outer.active && outer.reason==GML_RENDER_DISPATCH_UNQUALIFIED,
         "an inactive dispatch scope retained an undefined reason");
  gml_render_dispatch_cpu_scope_end(&render,&outer);
  gml_render_dispatch_diagnostics_enable(&render,1);
  gml_render_dispatch_cpu_scope_begin(&render,&outer,GML_RENDER_SCENE_PRIMITIVE);
  gml_render_dispatch_cpu_scope_begin(&render,&inner,GML_RENDER_SCENE_PRIMITIVE);
  gml_render_dispatch_cpu_scope_write(&render);
  gml_render_dispatch_cpu_scope_write(&render);
  gml_render_dispatch_cpu_scope_end(&render,&inner);
  expect(render.dispatch_cpu_depth==1u,
         "a nested CPU accounting scope lost its parent");
  gml_render_dispatch_cpu_scope_end(&render,&outer);
  expect(gml_render_dispatch_metrics(&render,&metrics) &&
         metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE]==1u &&
         !render.dispatch_cpu_depth,
         "nested software spans were not grouped into one resolved operation");
  gml_render_dispatch_cpu_scope_end(&render,&outer);
  expect(gml_render_dispatch_metrics(&render,&metrics) && metrics.unclassified==1u &&
         !render.dispatch_cpu_depth,"an unbalanced scope underflow became a zero fallback");
  render.dispatch_cpu_depth=UINT_MAX;
  gml_render_dispatch_cpu_scope_begin(&render,&outer,GML_RENDER_SCENE_PRIMITIVE);
  gml_render_dispatch_cpu_scope_end(&render,&outer);
  expect(gml_render_dispatch_metrics(&render,&metrics) && metrics.unclassified==2u &&
         render.dispatch_cpu_depth==UINT_MAX,"scope nesting overflow wrapped into a clean census");
  render.dispatch_cpu_depth=0u;
}

typedef struct { GmlRender *render; int seen[4]; } DispatchWorkerControl;
static void dispatch_worker_control(void *context,int first,int last,int slot){
  DispatchWorkerControl *control=(DispatchWorkerControl*)context;
  for(int row=first;row<last;row++) gml_render_dispatch_cpu_scope_write(control->render);
  if(slot>=0 && slot<4) control->seen[slot]=1;
}
static void check_dispatch_cpu_scope_row_workers(void){
  GmlRender render;
  GmlRenderDispatchCpuScope scope;
  GmlRenderDispatchMetrics metrics;
  DispatchWorkerControl control={0};
  reset_render(&render,12);
  control.render=&render;
  gml_render_dispatch_diagnostics_enable(&render,1);
  gml_render_dispatch_cpu_scope_begin(&render,&scope,GML_RENDER_SCENE_PRIMITIVE);
  gml_run_row_bands_n(&render,64,4,dispatch_worker_control,&control);
  gml_render_dispatch_cpu_scope_end(&render,&scope);
  expect(control.seen[0] && control.seen[1] && control.seen[2] && control.seen[3],
         "the dispatch coverage control did not exercise all four row-band slots");
  expect(gml_render_dispatch_metrics(&render,&metrics) &&
         metrics.cpu[GML_RENDER_SCENE_PRIMITIVE][GML_RENDER_DISPATCH_NO_DEVICE]==1u &&
         !render.dispatch_cpu_depth,
         "row workers counted fragments separately or lost their coverage mark");
  gml_row_pool_free(&render);
}

static void check_dispatch_subpixel_glyph_requires_visible_source(void){
  GmlRender render;
  GmlRenderDispatchMetrics metrics;
  GmlFont font={0};
  GmlGlyph glyph={0};
  GmlAtlas atlas={0};
  uint8_t rgba[16]={0};
  uint32_t baseline[4];
  reset_render(&render,12);
  atlas.px=rgba;atlas.w=atlas.h=2;
  render.atlas=&atlas;render.n_atlas=1;
  font.subpixel=1;font.atlas=0;
  glyph.w=glyph.h=2;
  gml_render_dispatch_diagnostics_enable(&render,1);
  expect(gml_render_cpu_classic_info_subpixel_glyph(&render,&font,&glyph,
           10,10,1,1,0x123456u,1.0),"transparent subpixel glyph dispatch was rejected");
  rgba[3]=rgba[7]=rgba[11]=rgba[15]=255u;
  expect(gml_render_cpu_classic_info_subpixel_glyph(&render,&font,&glyph,
           WIDTH,HEIGHT,1,1,0x123456u,1.0),"clipped subpixel glyph dispatch was rejected");
  expect(gml_render_dispatch_metrics(&render,&metrics) &&
         !metrics.cpu[GML_RENDER_SCENE_TEXT][GML_RENDER_DISPATCH_NO_DEVICE],
         "an empty or clipped subpixel glyph counted as a CPU draw");
  gml_render_dispatch_diagnostics_enable(&render,0);
  expect(gml_render_cpu_classic_info_subpixel_glyph(&render,&font,&glyph,
           10,10,1,1,0x123456u,1.0),"baseline subpixel glyph dispatch was rejected");
  baseline[0]=frame[10*WIDTH+10];baseline[1]=frame[10*WIDTH+11];
  baseline[2]=frame[11*WIDTH+10];baseline[3]=frame[11*WIDTH+11];
  memset(frame,0,sizeof frame);
  gml_render_dispatch_diagnostics_enable(&render,1);
  expect(gml_render_cpu_classic_info_subpixel_glyph(&render,&font,&glyph,
           10,10,1,1,0x123456u,1.0),"observed subpixel glyph dispatch was rejected");
  expect(gml_render_dispatch_metrics(&render,&metrics) &&
         metrics.cpu[GML_RENDER_SCENE_TEXT][GML_RENDER_DISPATCH_NO_DEVICE]==1u &&
         frame[10*WIDTH+10]==baseline[0] && frame[10*WIDTH+11]==baseline[1] &&
         frame[11*WIDTH+10]==baseline[2] && frame[11*WIDTH+11]==baseline[3],
         "a subpixel glyph was omitted, counted per pixel or altered by diagnostics");
}

typedef struct {GmlRender *render;uint32_t color;} CirclePixelReference;
static void circle_pixel_reference(void *context,int x0,int x1,int y){
  CirclePixelReference *reference=context;
  for(int x=x0;x<=x1;x++)
    gml_render_cpu_backend_draw_pixel(reference->render,x,y,reference->color);
}
static void check_circle_span_matches_pixel_writer(void){
  const int precision[]={-1,4,12,64,69};
  const unsigned masks[]={0u,7u,15u};
  uint32_t expected[WIDTH*HEIGHT];
  unsigned controls=0;
  for(unsigned n=0;n<5u;n++) for(unsigned mask=0;mask<3u;mask++)
  for(unsigned policy=0;policy<7u;policy++) for(unsigned surface=0;surface<2u;surface++)
  for(unsigned clipped=0;clipped<2u;clipped++) for(unsigned pending=0;pending<2u;pending++){
    GmlRender actual,reference;
    reset_render(&actual,precision[n]);
    for(unsigned i=0;i<WIDTH*HEIGHT;i++)frame[i]=0x78273859u+(i&15u);
    actual.target_sp=(int)surface;actual.color_write_mask=masks[mask];
    actual.fb_opaque_known=1;actual.fb_all_opaque=0;actual.fb_all_transparent=1;
    if(policy==1u)actual.alpha=2.0;
    if(policy==2u)actual.alpha=.6;
    if(policy==3u)actual.alphablend=0;
    if(policy==4u){actual.alphablend=0;actual.alpha=.6;}
    if(policy==5u)actual.blend_equation_alpha=3;
    if(policy==6u)actual.blendmode=2;
    if(pending)gml_render_set_pending_fill(&actual,0x41627384u);
    memcpy(expected,frame,sizeof expected);reference=actual;reference.fb=reference.base_fb=expected;
    GmlRenderDispatchCpuScope scope_a,scope_b;
    gml_render_dispatch_diagnostics_enable(&actual,1);
    gml_render_dispatch_diagnostics_enable(&reference,1);
    gml_render_dispatch_cpu_scope_begin(&actual,&scope_a,GML_RENDER_SCENE_PRIMITIVE);
    gml_render_dispatch_cpu_scope_begin(&reference,&scope_b,GML_RENDER_SCENE_PRIMITIVE);
    double cx=clipped?1.25:100.25,cy=clipped?-3.5:100.5;
    CirclePixelReference pixels={&reference,0xB94D27u};
    expect(gml_render_circle_filled_spans(&reference,cx,cy,31.75,21.25,
                                         circle_pixel_reference,&pixels),"circle reference admitted");
    gml_render_cpu_circle(&actual,cx,cy,31.75,21.25,0xB94D27u,0);
    gml_render_dispatch_cpu_scope_end(&actual,&scope_a);
    gml_render_dispatch_cpu_scope_end(&reference,&scope_b);
    GmlRenderDispatchMetrics ma,mb;
    expect(!memcmp(frame,expected,sizeof frame),"span fill differs from old per-pixel ARGB");
    expect(actual.fb_opaque_known==reference.fb_opaque_known &&
           actual.fb_all_opaque==reference.fb_all_opaque &&
           actual.fb_all_transparent==reference.fb_all_transparent &&
           actual.pending_fill==reference.pending_fill,
           "span fill changes opacity or deferred-clear metadata");
    expect(gml_render_dispatch_metrics(&actual,&ma) && gml_render_dispatch_metrics(&reference,&mb) &&
           !memcmp(&ma,&mb,sizeof ma),"span fill changes actual-write dispatch witness");
    controls++;
  }
  printf("circle span ARGB/metadata controls: %u\n",controls);
}

static void check_world_view_rotation_geometry(void){
  GmlRender render;
  reset_render(&render,24);
  set_world_scale(&render,2.0,3.0);
  gml_render_world_set_view_rotation(&render,90.0,100.0,50.0);
  double x=150.0,y=75.0;
  gml_render_view_rotate_point(&render,&x,&y);
  expect(fabs(x-100.0)<1e-10 && fabs(y-150.0)<1e-10,
         "view rotation must precede nonuniform raster scaling");
  gml_render_world_set_view_rotation(&render,-450.0,100.0,50.0);
  x=150.0;y=75.0;
  gml_render_view_rotate_point(&render,&x,&y);
  expect(fabs(x-100.0)<1e-10 && fabs(y)<1e-10,
         "negative multi-turn view angle has the opposite placement");
  render.target_sp++;
  x=150.0;y=75.0;gml_render_view_rotate_point(&render,&x,&y);
  expect(x==150.0 && y==75.0,"nested target retains local coordinates");
  render.target_sp--;
  render.gui_pass_active=1;
  gml_render_view_rotate_point(&render,&x,&y);
  expect(x==150.0 && y==75.0,"GUI retains its own coordinates");
  render.gui_pass_active=0;
  gml_render_view_rotate_point(&render,&x,&y);
  expect(fabs(x-100.0)<1e-10 && fabs(y)<1e-10,
         "world transform resumes after local target and GUI");
  gml_render_world_set_view_rotation(&render,720.0,100.0,50.0);
  expect(!gml_render_view_rotation_active(&render),"complete turns retain identity draw paths");

  reset_render(&render,24);
  gml_render_primitive_rectangle(&render,130,95,140,105,0x00FF00u,0);
  int unrotated_area=covered_pixels();
  expect(unrotated_area==121,"axis rectangle establishes inclusive integer footprint");
  reset_render(&render,24);
  gml_render_world_set_view_rotation(&render,90.0,WIDTH,HEIGHT);
  gml_render_primitive_rectangle(&render,130,95,140,105,0x00FF00u,0);
  expect((frame[135*WIDTH+100]&0x00FFFFFFu)==0x00FF00u &&
         !(frame[100*WIDTH+135]&0x00FFFFFFu),
         "opaque rectangle uses placed rotated geometry in CPU mode");
  expect(covered_pixels()==unrotated_area,
         "quarter-turn rectangle must retain the axis control area");
}

static void check_view_sprite_nonuniform_scale_and_mirroring(void){
  GmlRender render;GmlWin content={.bytecode=17};
  reset_render(&render,24);render.win=&content;
  render.spr=calloc(1,sizeof *render.spr);render.n_spr=1;
  expect(render.spr!=NULL,"view sprite fixture allocates its source");
  if(!render.spr) return;
  GmlSprite *sprite=render.spr;
  sprite->w=4;sprite->h=2;sprite->n_frames=1;
  sprite->originx=2;sprite->originy=1;
  sprite->runtime_owned=sprite->runtime_opaque=sprite->runtime_scene_binary=1;
  sprite->runtime_rgba=calloc(8,4);
  expect(sprite->runtime_rgba!=NULL,"view sprite fixture allocates pixel storage");
  if(!sprite->runtime_rgba){gml_render_free(&render);return;}
  for(unsigned i=0;i<8;i++){
    sprite->runtime_rgba[i*4u+(i%4u<2u?0u:2u)]=255;
    sprite->runtime_rgba[i*4u+3u]=255;
  }
  set_world_scale(&render,2.0,3.0);
  gml_render_world_set_view_rotation(&render,90.0,100.0,50.0);
  gml_draw_sprite_ext(&render,0,0,75,25,1,1,90,0xFFFFFFu,1.0);
  expect(covered_pixels()==48 &&
         (frame[148*WIDTH+96]&0xFFFFFFu)==0xFF0000u &&
         (frame[148*WIDTH+103]&0xFFFFFFu)==0x0000FFu &&
         !(frame[148*WIDTH+95]&0xFFFFFFu),
         "camera/local turns cancel before nonuniform sprite projection");
  memset(frame,0,sizeof frame);
  const double positioned_x[4]={73,77,77,73},positioned_y[4]={24,24,26,26};
  gml_draw_sprite_pos(&render,0,0,positioned_x,positioned_y,1.0);
  expect(covered_pixels()==48 &&
         (frame[145*WIDTH+99]&0xFFFFFFu)==0xFF0000u &&
         (frame[154*WIDTH+99]&0xFFFFFFu)==0x0000FFu,
         "view rotates all positioned sprite corners before nonuniform projection");
  memset(frame,0,sizeof frame);
  gml_draw_sprite_ext(&render,0,0,75,25,-1,1,90,0xFFFFFFu,1.0);
  expect(covered_pixels()==48 &&
         (frame[148*WIDTH+96]&0xFFFFFFu)==0x0000FFu &&
         (frame[148*WIDTH+103]&0xFFFFFFu)==0xFF0000u,
         "view sprite retains mirrored texture coordinates and extent");
  int alpha_target=gml_surface_create(&render,WIDTH,HEIGHT);
  expect(alpha_target>0 && gml_surface_set_target(&render,alpha_target),
         "view sprite alpha fixture selects an explicit target");
  if(alpha_target<=0 || render.target_id!=alpha_target){gml_render_free(&render);return;}
  render.world_transform_active=0;
  gml_render_set_pending_fill(&render,0xFF000000u);
  gml_render_world_set_view_rotation(&render,90.0,WIDTH,HEIGHT);
  gml_draw_sprite_ext(&render,0,0,130,100,1,1,90,0xFFFFFFu,128.0/255.0);
  uint32_t alpha_pixel=render.fb[129*WIDTH+98];
  if((alpha_pixel>>24)!=191u || render.fb_opaque_known || render.fb_all_opaque)
    fprintf(stderr,"view sprite alpha: pixel=%08x known=%d opaque=%d depth=%d active=%d\n",
      alpha_pixel,render.fb_opaque_known,render.fb_all_opaque,
      render.target_sp,gml_render_view_rotation_active(&render));
  expect((alpha_pixel>>24)==191u && !render.fb_opaque_known &&
         !render.fb_all_opaque,
         "partial-alpha view sprites invalidate opaque target certificates");
  gml_surface_reset_target(&render);
  set_world_scale(&render,2.0,3.0);
  gml_render_world_set_view_rotation(&render,90.0,100.0,50.0);
  render.atlas=calloc(1,sizeof *render.atlas);
  render.tpag=calloc(1,sizeof *render.tpag);
  sprite->frame=calloc(1,sizeof *sprite->frame);
  expect(render.atlas && render.tpag && sprite->frame,
         "view sprite fixture allocates packaged crop metadata");
  if(!render.atlas || !render.tpag || !sprite->frame){gml_render_free(&render);return;}
  render.n_atlas=render.n_tpag=1;
  render.atlas[0].px=sprite->runtime_rgba;
  render.atlas[0].w=4;render.atlas[0].h=2;
  sprite->runtime_rgba=NULL;
  render.tpag[0]=(GmlTpag){.atlas=0,.sw=4,.sh=2,.tw=8,.th=4,.tx=1,.ty=2};
  memset(frame,0,sizeof frame);
  gml_draw_sprite_ext(&render,0,0,75,25,1,1,90,0xFFFFFFu,1.0);
  expect(covered_pixels()==192 &&
         (frame[153*WIDTH+98]&0xFFFFFFu)==0xFF0000u &&
         (frame[153*WIDTH+113]&0xFFFFFFu)==0x0000FFu &&
         !(frame[152*WIDTH+98]&0xFFFFFFu),
         "view sprite projects the authored crop size independently of texture size");
  gml_render_free(&render);
}

int main(void){
  check_view_sprite_nonuniform_scale_and_mirroring();
  check_world_view_rotation_geometry();
  check_circle_span_matches_pixel_writer();
  check_fractional_radius_is_not_truncated();
  check_fractional_centre_is_not_quantized();
  check_integer_entry_point_is_unchanged();
  check_circle_geometry_applies_the_measured_bias();
  check_dispatch_metadata_does_not_enter_state();
  check_dispatch_counts_cpu_only_pass_and_suffix();
  check_dispatch_rejection_and_overflow_are_not_zero();
  check_dispatch_exception_rejection_and_overflow();
  check_dispatch_complete_cpu_primitive_kernels();
  check_dispatch_cpu_scope_nested();
  check_dispatch_cpu_scope_row_workers();
  check_dispatch_subpixel_glyph_requires_visible_source();
  if(failures){
    fprintf(stderr,"renderer primitives: %d failure(s)\n",failures);
    return 1;
  }
  printf("renderer primitives: ok\n");
  return 0;
}
