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
  gml_draw_sprite_stretched(&render,0,0,75,25,8,6,0xFFFFFFu,1.0);
  expect(covered_pixels()==288 &&
         (frame[151*WIDTH+90]&0xFFFFFFu)==0xFF0000u &&
         (frame[170*WIDTH+90]&0xFFFFFFu)==0x0000FFu &&
         !(frame[151*WIDTH+87]&0xFFFFFFu),
         "view stretched sprite rotates its origin-free canvas before nonuniform projection");
  memset(frame,0,sizeof frame);
  const double positioned_x[4]={73,77,77,73},positioned_y[4]={24,24,26,26};
  gml_draw_sprite_pos(&render,0,0,positioned_x,positioned_y,1.0);
  expect(covered_pixels()==48 &&
         (frame[145*WIDTH+99]&0xFFFFFFu)==0xFF0000u &&
         (frame[154*WIDTH+99]&0xFFFFFFu)==0x0000FFu,
         "view rotates all positioned sprite corners before nonuniform projection");
  memset(frame,0,sizeof frame);
  gml_draw_sprite_part_ext(&render,0,0,2,0,2,2,75,25,1,1,0xFFFFFFu,1.0);
  expect(covered_pixels()==24 &&
         (frame[151*WIDTH+97]&0xFFFFFFu)==0x0000FFu &&
         !(frame[151*WIDTH+95]&0xFFFFFFu),
         "view part geometry samples only its selected source region");
  memset(frame,0,sizeof frame);
  const uint32_t corners[4]={0xFFFFFFu,0u,0xFFFFFFu,0u};
  gml_draw_sprite_general(&render,0,0,2,0,2,2,75,25,1,1,90,corners,1.0);
  expect(covered_pixels()==24 &&
         (frame[151*WIDTH+101]&0xFFFFFFu)==0x0000DFu &&
         !(frame[151*WIDTH+99]&0xFFFFFFu),
         "view general sprites retain local rotation, selection and triangle colours");
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
  memset(frame,0,sizeof frame);
  gml_draw_sprite_part_ext(&render,0,0,2.2,2.1,.2,.2,75,25,10,10,0xFFFFFFu,1.0);
  expect(covered_pixels()==24 &&
         (frame[151*WIDTH+97]&0xFFFFFFu)==0xFF0000u,
         "rotated packaged one-cell crop retains its continuous selected extent");
  gml_render_free(&render);
}

static void check_view_surface_quad_contract(void){
  GmlRenderImageRef source={GML_RENDER_RESOURCE_SURFACE,1u,1u,1u};
  GmlRenderDrawOp op={.opcode=GML_RENDER_OP_TEXTURED_QUAD,
    .scene_family=GML_RENDER_SCENE_SURFACE,.reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT,
    .source_count=1u,.sources=&source};
  op.target.width=op.target.height=32;op.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  op.target.region=(GmlRenderRect){0,0,32,32};
  GmlRenderQuad *q=&op.payload.quad;q->shape=GML_RENDER_QUAD_GENERAL;
  q->sampling=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_NEAREST,
    GML_RENDER_EDGE_CLAMP_SUBIMAGE,{0,0,8,4},GML_RENDER_QUANTISE_VIEW_SURFACE,0u};
  q->filtered_draw_alpha=1.0;
  for(unsigned i=0;i<4u;i++){
    q->x[i]=(i==1u || i==2u)?8.0:4.0;q->y[i]=i>=2u?8.0:6.0;
    q->u[i]=(i==1u || i==2u)?.75:.25;q->v[i]=i>=2u?.75:.25;
    q->corner_colour[i]=0xFFFFFFFFu;
  }
  GmlRenderDrawStateSnapshot state={.colour=0xFFFFFFFFu,.alpha=255u,.write_mask=15u,
    .sampler_count=1u,.rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA,
    .rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA,
    .alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA,
    .alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA,
    .rgb_equation=GML_RENDER_BLEND_ADD,.alpha_equation=GML_RENDER_BLEND_ADD};
  state.samplers[0]=q->sampling;
  expect(gml_render_view_surface_quad_valid(&op,&state,8,4),
         "view surface contract accepts a selected native UV rectangle");
  for(unsigned i=0;i<4u;i++) q->x[i]=32.0-q->x[i];
  q->sampling.filter=GML_RENDER_FILTER_HARDWARE_BILINEAR;state.samplers[0]=q->sampling;
  expect(gml_render_view_surface_quad_valid(&op,&state,8,4),
         "view surface contract keeps mirrored filtered device geometry");
  for(unsigned test=0;test<14u;test++){
    GmlRenderDrawOp invalid=op;GmlRenderDrawStateSnapshot broken=state;
    switch(test){
      case 0:invalid.payload.quad.x[2]+=1.0;break;
      case 1:invalid.payload.quad.u[1]=NAN;break;
      case 2:invalid.payload.quad.v[0]=-.01;break;
      case 3:invalid.payload.quad.inverse_x[0]=1.0;break;
      case 4:invalid.payload.quad.span_active=1u;break;
      case 5:broken.source_palette.enabled=1u;break;
      case 6:broken.alpha_test_enabled=1u;break;
      case 7:broken.sampler_count=2u;break;
      case 8:invalid.payload.quad.corner_colour[3]=0xFF00FF00u;break;
      case 9:invalid.payload.quad.filtered_draw_alpha=0.0;break;
      case 10:broken.samplers[0].source_region.width=9u;break;
      case 11:broken.coverage_policy=GML_RENDER_COVERAGE_REPLACE_ZERO_ALPHA;break;
      case 12:broken.write_mask=16u;break;
      case 13:invalid.payload.quad.sampling.quantisation=0u;break;
    }
    expect(!gml_render_view_surface_quad_valid(&invalid,&broken,8,4),
           "view surface contract rejects malformed or unsupported packet state");
  }
}

static void check_view_background_quad_contract(void){
  GmlRenderImageRef source={GML_RENDER_RESOURCE_ATLAS,1u,1u,1u};
  GmlRenderDrawOp op={.opcode=GML_RENDER_OP_TEXTURED_QUAD,.scene_family=GML_RENDER_SCENE_BACKGROUND,
    .reconstruction_mode=GML_RENDER_DEVICE_CHECKPOINT,.source_count=1u,.sources=&source};
  op.target.width=op.target.height=32;op.target.pixel_format=GML_RENDER_PIXEL_ARGB8888;
  op.target.region=(GmlRenderRect){0,0,32,32};
  GmlRenderQuad *q=&op.payload.quad;q->shape=GML_RENDER_QUAD_GENERAL;q->filtered_draw_alpha=1;
  q->sampling=(GmlRenderSamplingPolicy){GML_RENDER_FILTER_NEAREST,
    GML_RENDER_EDGE_CLAMP_SUBIMAGE,{4,2,8,4},GML_RENDER_QUANTISE_VIEW_BACKGROUND,0u};
  for(unsigned i=0;i<4u;i++){
    q->x[i]=(i==1 || i==2)?8:4;q->y[i]=i>=2?8:6;
    q->u[i]=(i==1 || i==2)?10.0/16:6.0/16;q->v[i]=i>=2?5.0/8:3.0/8;
    q->corner_colour[i]=0xFFFFFFFFu;
  }
  GmlRenderDrawStateSnapshot state={.colour=0xFFFFFFFFu,.alpha=255u,.write_mask=15u,.sampler_count=1u,
    .rgb_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA,.rgb_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA,
    .alpha_source_factor=GML_RENDER_BLEND_SOURCE_ALPHA,.alpha_destination_factor=GML_RENDER_BLEND_ONE_MINUS_SOURCE_ALPHA,
    .rgb_equation=GML_RENDER_BLEND_ADD,.alpha_equation=GML_RENDER_BLEND_ADD};
  state.samplers[0]=q->sampling;
  expect(gml_render_view_background_quad_valid(&op,&state,16,8),
         "view background accepts selected UVs inside its own atlas rectangle");
  for(unsigned i=0;i<4u;i++) q->x[i]=32-q->x[i];
  q->sampling.filter=GML_RENDER_FILTER_HARDWARE_BILINEAR;state.samplers[0]=q->sampling;
  expect(gml_render_view_background_quad_valid(&op,&state,16,8),
         "view background retains mirrored hardware-filtered geometry");
  for(unsigned test=0;test<8u;test++){
    GmlRenderDrawOp invalid=op;GmlRenderDrawStateSnapshot broken=state;GmlRenderImageRef bad_source=source;
    switch(test){
      case 0:invalid.scene_family=GML_RENDER_SCENE_SURFACE;break;
      case 1:bad_source.resource_class=GML_RENDER_RESOURCE_SURFACE;invalid.sources=&bad_source;break;
      case 2:invalid.payload.quad.u[0]=0.0;break;
      case 3:invalid.payload.quad.sampling.source_region.width=32;break;
      case 4:invalid.payload.quad.sampling.quantisation=GML_RENDER_QUANTISE_VIEW_SURFACE;break;
      case 5:broken.samplers[0].source_region.x=0;break;
      case 6:invalid.payload.quad.x[2]+=1;break;
      case 7:broken.source_palette.enabled=1;break;
    }
    expect(!gml_render_view_background_quad_valid(&invalid,&broken,16,8),
           "view background rejects cross-family resources and malformed source geometry");
  }
  op.scene_family=GML_RENDER_SCENE_TILE;
  q->sampling.quantisation=GML_RENDER_QUANTISE_VIEW_TILE;state.samplers[0]=q->sampling;
  expect(gml_render_view_tile_quad_valid(&op,&state,16,8) &&
         !gml_render_view_background_quad_valid(&op,&state,16,8),
         "view tile has its own typed atlas admission and hardware sampling");
  q->sampling.filter=GML_RENDER_FILTER_NEAREST;state.samplers[0]=q->sampling;
  expect(gml_render_view_tile_quad_valid(&op,&state,16,8),
         "view tile retains nearest UV admission for mirrored geometry");
  GmlRenderDrawOp invalid=op;
  invalid.scene_family=GML_RENDER_SCENE_BACKGROUND;
  expect(!gml_render_view_tile_quad_valid(&invalid,&state,16,8),
         "view tile rejects a background-family packet");
  invalid=op;invalid.payload.quad.v[0]=0;
  expect(!gml_render_view_tile_quad_valid(&invalid,&state,16,8),
         "view tile rejects UVs outside the source crop");
  op.scene_family=GML_RENDER_SCENE_TEXT;
  q->sampling.quantisation=GML_RENDER_QUANTISE_VIEW_TEXT;state.samplers[0]=q->sampling;
  expect(gml_render_view_text_quad_valid(&op,&state,16,8) &&
         !gml_render_view_tile_quad_valid(&op,&state,16,8),
         "view glyph has a distinct typed atlas packet and native UV geometry");
  invalid=op;invalid.scene_family=GML_RENDER_SCENE_SPRITE;
  expect(!gml_render_view_text_quad_valid(&invalid,&state,16,8),
         "view glyph rejects an incorrectly classified sprite packet");
  invalid=op;invalid.payload.quad.sampling.quantisation=GML_RENDER_QUANTISE_VIEW_TILE;
  expect(!gml_render_view_text_quad_valid(&invalid,&state,16,8),
         "view glyph rejects tile sampling policy");

}

static void check_view_world_visible_bounds(void){
  GmlRender render;reset_render(&render,24);
  render.fbh=render.base_fbh=150;render.cam_x=14;render.cam_y=27;
  set_world_scale(&render,2,3);
  gml_render_world_set_view_rotation(&render,90,100,50);
  double x,y,width,height;
  expect(gml_render_world_view(&render,&x,&y,&width,&height) &&
         fabs(x-32)<1e-10 && fabs(y+16)<1e-10 &&
         fabs(width-50)<1e-10 && fabs(height-100)<1e-10,
         "quarter-turn visibility converts inverse target bounds into authored world units");
  gml_render_world_set_view_rotation(&render,45,100,50);
  double extent=150.0/sqrt(2.0);
  expect(gml_render_world_view(&render,&x,&y,&width,&height) &&
         fabs(x-(57-extent*.5))<1e-10 && fabs(y-(34-extent*.5))<1e-10 &&
         fabs(width-extent)<1e-10 && fabs(height-extent)<1e-10,
         "diagonal visibility covers cells beyond the axis camera rectangle");
  render.target_sp++;
  expect(gml_render_world_view(&render,&x,&y,&width,&height) &&
         x==7 && y==9 && width==100 && height==50,
         "independent target retains the established unturned world query");
}

static void check_view_background_nonuniform_projection(void){
  GmlRender render;reset_render(&render,24);
  GmlBg background={.tpag=0};GmlAtlas atlas={0};GmlTpag page={0};
  uint8_t rgba[8*4*4];
  for(unsigned i=0;i<8u*4u;i++){
    unsigned white=i%8>=2 && i%8<6 && i/8>=1 && i/8<3;
    rgba[i*4u]=rgba[i*4u+1u]=white?255:0;rgba[i*4u+2u]=rgba[i*4u+3u]=255;
  }
  atlas.px=rgba;atlas.w=8;atlas.h=4;
  page.sw=page.tw=page.bw=8;page.sh=page.th=page.bh=4;page.atlas=0;
  render.bg=&background;render.n_bg=1;render.atlas=&atlas;render.n_atlas=1;
  render.tpag=&page;render.n_tpag=1;
  set_world_scale(&render,2,3);
  gml_render_world_set_view_rotation(&render,90,100,50);
  gml_draw_background(&render,0,75,25);
  expect(covered_pixels()==192 && (frame[151*WIDTH+99]&0xFFFFFFu)==0x0000FFu &&
         (frame[157*WIDTH+97]&0xFFFFFFu)==0xFFFFFFu,
         "background atlas source rotates before nonuniform target scaling");
  memset(frame,0,sizeof frame);
  gml_draw_background_part_ext(&render,0,2,1,4,2,75,25,1,1,0x00FF00u,1);
  expect(covered_pixels()==48 && (frame[151*WIDTH+99]&0xFFFFFFu)==0x00FF00u,
         "background view part retains source selection and uniform tint");
  memset(frame,0,sizeof frame);
  gml_render_world_set_view_rotation(&render,45,100,HEIGHT/3.0);
  gml_draw_background_tiled_ext(&render,0,1,2,-1,1,0xFFFFFFu,1,1,1);
  expect(covered_pixels()==WIDTH*HEIGHT,
         "background repeated cells cover the turned viewport under nonuniform scale and mirrors");
  gml_render_world_set_view_rotation(&render,90,100,50);
  for(unsigned flags=0;flags<8u;flags++) for(unsigned signs=0;signs<4u;signs++){
    memset(frame,0,sizeof frame);
    double xs=signs&1u?-1:1,ys=signs&2u?-1:1;
    gml_draw_background_tile(&render,0,2,1,4,2,75,25,xs,ys,
                             flags&1u,flags&2u,flags&4u,0x00FF00u,1);
    int x0,y0,x1,y1;covered_bounds(&x0,&y0,&x1,&y1);
    int expected_x=(flags&4u) || ys>0?96:100;
    int expected_y=(flags&4u) || xs>0?150:138;
    expect(covered_pixels()==48 && x0==expected_x && y0==expected_y &&
           x1==x0+3 && y1==y0+11 && (frame[y0*WIDTH+x0]&0xFFFFFFu)==0x00FF00u,
           "non-square view tiles retain all flag/signed-axis footprints before nonuniform projection");
  }
  memset(frame,0,sizeof frame);
  gml_render_world_set_view_rotation(&render,45,100,HEIGHT/3.0);
  for(unsigned row=0;row<35u;row++) for(unsigned col=0;col<18u;col++){
    unsigned flags=(col+3u*row)%8u;
    gml_draw_background_tile(&render,0,0,0,8,4,-32+8*(int)col,-40+4*(int)row,1,1,
                             flags&1u,flags&2u,flags&4u,0xFFFFFFu,1);
  }
  expect(covered_pixels()==WIDTH*HEIGHT,
         "adjacent transformed tiles retain opaque coverage at shared mirrored source edges");
}

static void check_view_surface_nonuniform_projection(void){
  GmlRender render;GmlWin content={.bytecode=17};
  reset_render(&render,24);render.win=&content;
  int source=gml_surface_create(&render,4,2);
  uint8_t pixels[32]={0};
  for(unsigned i=0;i<8u;i++){
    pixels[i*4u+(i%4u<2u?0u:2u)]=255;
    pixels[i*4u+3u]=255;
  }
  expect(source>0 && gml_surface_write_rgba8(&render,source,pixels,sizeof pixels),
         "view surface fixture creates its source bytes");
  if(source<=0){gml_render_free(&render);return;}
  set_world_scale(&render,2.0,3.0);
  gml_render_world_set_view_rotation(&render,90.0,100.0,50.0);
  gml_draw_surface_ext(&render,source,75,25,1,1,90,0xFFFFFFu,1.0);
  expect(covered_pixels()==48 &&
         (frame[151*WIDTH+101]&0xFFFFFFu)==0xFF0000u &&
         (frame[151*WIDTH+106]&0xFFFFFFu)==0x0000FFu &&
         !(frame[151*WIDTH+99]&0xFFFFFFu),
         "local and view surface turns cancel before nonuniform projection");
  memset(frame,0,sizeof frame);
  gml_draw_surface_stretched(&render,source,75,25,8,6,0xFFFFFFu,1.0);
  expect(covered_pixels()==288 &&
         (frame[151*WIDTH+90]&0xFFFFFFu)==0xFF0000u &&
         (frame[170*WIDTH+90]&0xFFFFFFu)==0x0000FFu,
         "view surface canvas keeps its two independently scaled axes");
  memset(frame,0,sizeof frame);
  gml_draw_surface_part_ext(&render,source,2,0,2,2,75,25,1,1,0xFFFFFFu,1.0);
  expect(covered_pixels()==24 &&
         (frame[151*WIDTH+97]&0xFFFFFFu)==0x0000FFu &&
         !(frame[151*WIDTH+95]&0xFFFFFFu),
         "view surface part retains its independent source selection");
  gml_render_free(&render);
}

static void check_view_tiling_visible_corners(void){
  GmlRender render;
  reset_render(&render,24);
  GmlRenderViewTileGrid grid;
  gml_render_world_set_view_rotation(&render,45.0,WIDTH,HEIGHT);
  expect(gml_render_view_tile_grid(&render,1e100,-1e100,8,4,&grid),
         "view tiling reduces remote anchors before integer enumeration");
  expect(grid.x<0.0 && grid.y<0.0 && grid.x+grid.columns*8>WIDTH &&
         grid.y+grid.rows*4>HEIGHT,
         "diagonal tiling includes world cells outside the axis viewport");
  GmlRenderViewTileGrid unchanged=grid;
  expect(!gml_render_view_tile_grid(&render,NAN,0,8,4,&grid) &&
         !gml_render_view_tile_grid(&render,0,0,0,4,&grid) &&
         !gml_render_view_tile_grid(&render,0,0,1e-100,4,&grid) &&
         memcmp(&grid,&unchanged,sizeof grid)==0,
         "invalid or unrepresentable tile grids leave the caller untouched");
  int source=gml_surface_create(&render,8,4);
  uint8_t pixels[8*4*4];
  for(unsigned i=0;i<8u*4u;i++){
    pixels[i*4u]=255;pixels[i*4u+1u]=255;pixels[i*4u+2u]=255;pixels[i*4u+3u]=255;
  }
  expect(source>0 && gml_surface_write_rgba8(&render,source,pixels,sizeof pixels),
         "tiled view control owns a non-square opaque source");
  if(source<=0){gml_render_free(&render);return;}
  const double angles[]={45,-45,135,90,-90};
  for(unsigned i=0;i<sizeof angles/sizeof angles[0];i++){
    for(unsigned signs=0;signs<4u;signs++){
      memset(frame,0,sizeof frame);
      render.cam_x=7;render.cam_y=9;
      gml_render_world_set_view_rotation(&render,angles[i],WIDTH,HEIGHT);
      gml_draw_surface_tiled_ext(&render,source,8,11,signs&1u?-1:1,
                                 signs&2u?-1:1,0xFFFFFFu,1.0);
      expect(covered_pixels()==WIDTH*HEIGHT,
             "turned repeated surface cells cover every visible pixel for each mirror");
    }
  }
  set_world_scale(&render,2,3);
  memset(frame,0,sizeof frame);
  gml_render_world_set_view_rotation(&render,45.0,100,HEIGHT/3.0);
  gml_draw_surface_tiled_ext(&render,source,8,11,1,1,0xFFFFFFu,1.0);
  expect(covered_pixels()==WIDTH*HEIGHT,
         "inverse visible bounds retain coverage under nonuniform world scaling");
  memset(frame,0,sizeof frame);
  render.interp=1;
  gml_draw_surface_tiled_ext(&render,source,8,11,1,1,0xFFFFFFu,1.0);
  int unchanged_colour=1;
  for(unsigned i=0;i<WIDTH*HEIGHT;i++)
    if(frame[i]!=0xFFFFFFFFu){unchanged_colour=0;break;}
  expect(unchanged_colour,
         "filtered view cells preserve uniform source channels and opacity exactly");
  gml_render_free(&render);
}

static void check_selected_view_point_and_triangles(void){
  for(unsigned primitive=0;primitive<3u;primitive++)
  for(unsigned turn=0;turn<2u;turn++){
    GmlRender render;
    reset_render(&render,24);
    gml_render_world_set_view_rotation(&render,turn?90:0,WIDTH,HEIGHT);
    if(!primitive) gml_render_primitive_point(&render,140,80,0xFFFFFFu);
    else gml_render_primitive_triangle_alpha(&render,130,70,150,70,130,90,
                                              0xFFFFFFu,1.0,primitive==2u);
    int left,top,right,bottom;
    covered_bounds(&left,&top,&right,&bottom);
    int ink=covered_pixels(),outside=0;
    for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++){
      if(!(frame[y*WIDTH+x]&0xFFFFFFu)) continue;
      if(turn && (!primitive?(x<118 || x>121 || y<138 || y>141):
                              (x<109 || x>131 || y<129 || y>151))) outside++;
    }
    int ok=ink>0 && !outside;
    if(!primitive) ok=ok && ink==1;
    else if(turn) ok=ok && (primitive==2u?
      !(frame[137*WIDTH+123]&0xFFFFFFu):(frame[137*WIDTH+123]&0xFFFFFFu)!=0);
    else ok=ok && (primitive==2u?
      !(frame[77*WIDTH+137]&0xFFFFFFu):(frame[77*WIDTH+137]&0xFFFFFFu)!=0);
    fprintf(stderr,"selected primitive=%u turn=%u ink=%d outside=%d bounds=%d,%d..%d,%d pass=%d\n",
      primitive,turn,ink,outside,left,top,right,bottom,ok);
    expect(ok,"selected view places point and triangle geometry without filling the outline interior");
    gml_render_free(&render);
  }
}

static void check_selected_view_lines(void){
  const double turns[3]={0,90,-90};
  for(unsigned mode=0;mode<3u;mode++) for(unsigned thick=0;thick<2u;thick++)
  for(unsigned turn=0;turn<3u;turn++){
    GmlRender render;
    reset_render(&render,24);
    gml_render_world_set_view_rotation(&render,turns[turn],WIDTH,HEIGHT);
    int width=thick?5:1;
    if(!mode) gml_render_primitive_line(&render,130,70,150,70,0xFFFFFFu,width);
    else if(mode==1u)
      gml_render_primitive_line_color(&render,130,70,150,70,0x0000FFu,0xFF0000u,width);
    else gml_render_primitive_line_color_subpixel(&render,130,70,150,70,
                                                  0x0000FFu,0xFF0000u,width);
    int ink=covered_pixels(),outside=0;
    for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++){
      if(!(frame[y*WIDTH+x]&0xFFFFFFu)) continue;
      if(turn==1u?(x<126 || x>134 || y<128 || y>152):
         turn==2u?(x<66 || x>74 || y<48 || y>72):
                   (x<128 || x>152 || y<66 || y>74)) outside++;
    }
    /* Integer endpoints refer to physical pixel centres; the quarter turn maps
     * their 70.5 ordinate to 129.5. Subpixel entries retain the authored 70. */
    int centre=mode==2u?130:129;
    int ax=turn==1u?centre:turn==2u?70:134;
    int ay=turn==1u?134:turn==2u?66:70;
    int bx=turn==1u?centre:turn==2u?70:146;
    int by=turn==1u?146:turn==2u?54:70;
    uint32_t first=frame[ay*WIDTH+ax],second=frame[by*WIDTH+bx];
    int gradient=!mode || (((first>>16)&255u)>(first&255u) &&
                           (second&255u)>((second>>16)&255u));
    int ok=ink>0 && !outside && (first&0xFFFFFFu) && (second&0xFFFFFFu) && gradient;
    fprintf(stderr,"selected line mode=%u width=%d turn=%g ink=%d outside=%d gradient=%d pass=%d\n",
      mode,width,turns[turn],ink,outside,gradient,ok);
    expect(ok,"selected view places continuous flat and gradient line geometry and retains endpoint colours");
    gml_render_free(&render);
  }
}

static void check_selected_view_rectangles(void){
  const double turns[3]={0,90,-90};
  for(unsigned kind=0;kind<4u;kind++) for(unsigned opacity=0;opacity<2u;opacity++)
  for(unsigned turn=0;turn<3u;turn++){
    GmlRender render;
    reset_render(&render,24);
    render.alpha=opacity?.37:1;
    gml_render_world_set_view_rotation(&render,turns[turn],WIDTH,HEIGHT);
    if(kind<2u) gml_render_primitive_rectangle(&render,130,70,150,86,0xFFFFFFu,kind&1u);
    else gml_render_primitive_rectangle_color(&render,130,70,150,86,
      0x0000FFu,0x00FF00u,0xFF0000u,0xFFFFFFu,kind&1u);
    int ink=covered_pixels(),outside=0;
    for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++){
      if(!(frame[y*WIDTH+x]&0xFFFFFFu)) continue;
      if(turn==1u?(x<112 || x>132 || y<128 || y>152):
         turn==2u?(x<68 || x>88 || y<47 || y>72):
                   (x<128 || x>152 || y<68 || y>88)) outside++;
    }
    const int probes[4][2]={{140,78},{140,70},{132,72},{148,84}};
    uint32_t sample[4];
    for(unsigned p=0;p<4u;p++){
      double x=probes[p][0]+.5,y=probes[p][1]+.5;
      double px=turn==1u?WIDTH-y:turn==2u?y:x;
      double py=turn==1u?x:turn==2u?HEIGHT-x:y;
      sample[p]=frame[(int)floor(py)*WIDTH+(int)floor(px)]&0xFFFFFFu;
    }
    int ok=ink>0 && !outside && sample[1] &&
      ((kind&1u)?sample[0]==0:sample[0]!=0);
    if(kind==2u) ok=ok && ((sample[2]>>16)&255u)>(sample[2]&255u) &&
                          (sample[3]&255u)>((sample[3]>>16)&255u);
    if(kind<2u){
      int expected=(int)lround(255*render.alpha);
      ok=ok && abs((int)(sample[1]&255u)-expected)<=2;
    }
    fprintf(stderr,"selected rectangle kind=%u alpha=%g turn=%g ink=%d outside=%d pass=%d\n",
      kind,render.alpha,turns[turn],ink,outside,ok);
    expect(ok,"selected view preserves rectangle placement, hollow interiors, opacity and corner colour order");
    gml_render_free(&render);
  }
}

static void check_selected_view_circles(void){
  const double turns[3]={0,90,-90};
  for(unsigned kind=0;kind<4u;kind++) for(unsigned ellipse=0;ellipse<2u;ellipse++)
  for(unsigned fractional=0;fractional<2u;fractional++)
  for(unsigned opacity=0;opacity<2u;opacity++) for(unsigned turn=0;turn<3u;turn++){
    GmlRender render;
    reset_render(&render,24);
    render.alpha=opacity?.37:1;
    double cx=fractional?140.25:140,cy=fractional?80.75:80;
    double rx=10,ry=ellipse?6:10;
    gml_render_world_set_view_rotation(&render,turns[turn],WIDTH,HEIGHT);
    if(kind<2u){
      if(fractional) gml_render_primitive_circle_subpixel(&render,cx,cy,rx,ry,0xFFFFFFu,kind&1u);
      else gml_render_primitive_circle(&render,(int)cx,(int)cy,(int)rx,(int)ry,0xFFFFFFu,kind&1u);
    } else {
      if(fractional) gml_render_primitive_circle_color_subpixel(&render,cx,cy,rx,ry,0x0000FFu,0xFF0000u,kind&1u);
      else gml_render_primitive_circle_color(&render,(int)cx,(int)cy,(int)rx,(int)ry,0x0000FFu,0xFF0000u,kind&1u);
    }
    double px=turn==1u?WIDTH-cy:turn==2u?cy:cx;
    double py=turn==1u?cx:turn==2u?HEIGHT-cx:cy;
    double ex=turn?ry:rx,ey=turn?rx:ry;
    int ink=covered_pixels(),outside=0;
    for(int y=0;y<HEIGHT;y++) for(int x=0;x<WIDTH;x++)
      if((frame[y*WIDTH+x]&0xFFFFFFu) &&
         (x<floor(px-ex)-2 || x>ceil(px+ex)+2 || y<floor(py-ey)-2 || y>ceil(py+ey)+2)) outside++;
    uint32_t centre=frame[(int)floor(py)*WIDTH+(int)floor(px)]&0xFFFFFFu;
    double sx=(kind&1u)?lround(cx+rx)+.5:cx+rx-1;
    double sy=(kind&1u)?lround(cy)+.5:cy;
    int edge_x=(int)floor(turn==1u?WIDTH-sy:turn==2u?sy:sx);
    int edge_y=(int)floor(turn==1u?sx:turn==2u?HEIGHT-sx:sy);
    int edge=0;
    for(int y=edge_y-1;y<=edge_y+1;y++) for(int x=edge_x-1;x<=edge_x+1;x++){
      uint32_t colour=frame[y*WIDTH+x]&0xFFFFFFu;
      edge|=kind<2u?colour!=0:(colour&255u)>((colour>>16)&255u);
    }
    int ok=ink>0 && !outside && edge && ((kind&1u)?centre==0:centre!=0);
    if(kind==2u) ok=ok && ((centre>>16)&255u)>(centre&255u);
    fprintf(stderr,"selected circle kind=%u ellipse=%u fractional=%u alpha=%g turn=%g ink=%d outside=%d pass=%d\n",
      kind,ellipse,fractional,render.alpha,turns[turn],ink,outside,ok);
    expect(ok,"selected view places circle/ellipse polygon geometry, hollow rims and centre-to-edge colours");
    gml_render_free(&render);
  }
}

int main(void){
  check_selected_view_circles();
  check_selected_view_rectangles();
  check_selected_view_lines();
  check_selected_view_point_and_triangles();
  check_view_world_visible_bounds();
  check_view_background_quad_contract();
  check_view_background_nonuniform_projection();
  check_view_tiling_visible_corners();
  check_view_sprite_nonuniform_scale_and_mirroring();
  check_view_surface_nonuniform_projection();
  check_view_surface_quad_contract();
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
