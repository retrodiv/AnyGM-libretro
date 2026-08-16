/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Exact characterization of the coarse presentation passes that the render plan describes.
 *
 * These cases are written against the sampling conventions the software renderer already
 * implements, not against a formula chosen for convenience. They exist so that extracting those
 * passes into a neutral plan, and later executing an eligible plan on a GPU, can be proved to
 * select the same source texel for every destination pixel, including the fractional and clipped
 * edges where a mathematically similar expression would disagree. */
#include "anygm_test_runner.h"

#include "gml_render_internal.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The shared test harness reads a non-zero return as a pass, so a case reports failure by returning
 * zero after naming what broke. */
#define REQUIRE(condition,label) do{ \
  if(!(condition)){ \
    fprintf(stderr,"render plan failed: %s\n",label); \
    return 0; \
  } \
}while(0)

enum { PLAN_TEST_MAX_PIXELS=1u<<20 };

/* An asymmetric pattern: no rotation, transpose or mirror of it equals itself, so a corner landing
 * in the wrong corner is visible in a single comparison rather than only at one edge. */
static void fill_asymmetric(uint32_t *pixels,int width,int height){
  for(int y=0;y<height;y++) for(int x=0;x<width;x++)
    pixels[(size_t)y*width+x]=
      0xFF000000u|((uint32_t)(11u+(unsigned)x*37u+(unsigned)y*3u)<<16)|
      ((uint32_t)(23u+(unsigned)x*5u+(unsigned)y*61u)<<8)|
      (uint32_t)(41u+(unsigned)x*17u+(unsigned)y*29u);
}

static void render_reset(GmlRender *render,const GmlWin *content,
                         const uint32_t *source,int source_width,int source_height,
                         uint32_t *target,int target_width,int target_height,
                         int application_draw_enabled){
  memset(render,0,sizeof *render);
  render->win=(GmlWin*)content;
  render->app_surface=(uint32_t*)source;
  render->app_w=source_width;
  render->app_h=source_height;
  render->app_surface_opaque=1;
  render->alphablend=1;
  render->alpha=1.0;
  render->color_write_mask=0x0F;
  render->blend_equation=1;
  render->blend_equation_alpha=1;
  render->app_draw_enable=application_draw_enabled;
  render->active_shader=-1;
  render->lut_pal_sprite=-1;
  gml_render_begin(render,target,target_width,target_height,0.0,0.0);
}

/* The destination rectangle the software kernel writes, in the kernel's own convention: a
 * destination pixel belongs to the rectangle when its centre does. */
static void destination_bounds(double dx,double dy,double dw,double dh,
                               int target_width,int target_height,
                               int *x0,int *y0,int *x1,int *y1){
  *x0=(int)ceil(dx-0.5);
  *y0=(int)ceil(dy-0.5);
  *x1=(int)ceil(dx+dw-0.5);
  *y1=(int)ceil(dy+dh-0.5);
  if(*x0<0) *x0=0;
  if(*y0<0) *y0=0;
  if(*x1>target_width) *x1=target_width;
  if(*y1>target_height) *y1=target_height;
}

/* The source index a destination pixel centre selects. Clamping, not wrapping, is what the kernel
 * does at a rectangle whose edge falls outside the target. */
static int source_index(int destination,double origin,double extent,int source_extent){
  int index=(int)floor((((double)destination+0.5)-origin)*(double)source_extent/extent);
  if(index<0) index=0;
  if(index>=source_extent) index=source_extent-1;
  return index;
}

static int compare_presentation(const uint32_t *target,int target_width,int target_height,
                                const uint32_t *source,int source_width,int source_height,
                                double dx,double dy,double dw,double dh,
                                uint32_t untouched,const char *label){
  int x0,y0,x1,y1;
  destination_bounds(dx,dy,dw,dh,target_width,target_height,&x0,&y0,&x1,&y1);
  for(int y=0;y<target_height;y++) for(int x=0;x<target_width;x++){
    uint32_t actual=target[(size_t)y*target_width+x];
    uint32_t expected;
    if(x>=x0 && x<x1 && y>=y0 && y<y1){
      int sx=source_index(x,dx,dw,source_width);
      int sy=source_index(y,dy,dh,source_height);
      expected=source[(size_t)sy*source_width+sx]|0xFF000000u;
    } else {
      expected=untouched;
    }
    if(actual!=expected){
      fprintf(stderr,"render plan %s mismatch at %d,%d: %08x != %08x\n",
              label,x,y,actual,expected);
      return 1;
    }
  }
  return 0;
}

/* One shared driver: the content-owned presentation gate accepts an arbitrary destination
 * rectangle, so every edge convention can be exercised without the GUI transform also moving. */
static int presentation_case(int source_width,int source_height,
                             int target_width,int target_height,
                             double dx,double dy,double dw,double dh,
                             const char *label){
  GmlWin content={0};
  GmlRender render;
  uint32_t *source=NULL,*target=NULL;
  int failed=1;
  if((size_t)source_width*source_height>PLAN_TEST_MAX_PIXELS ||
     (size_t)target_width*target_height>PLAN_TEST_MAX_PIXELS) return 1;
  source=(uint32_t*)malloc((size_t)source_width*source_height*sizeof *source);
  target=(uint32_t*)malloc((size_t)target_width*target_height*sizeof *target);
  if(!source || !target){ free(source); free(target); return 1; }
  content.bytecode=14;
  fill_asymmetric(source,source_width,source_height);
  for(size_t index=0;index<(size_t)target_width*target_height;index++)
    target[index]=0xFF5A5A5Au;
  render_reset(&render,&content,source,source_width,source_height,
               target,target_width,target_height,0);
  gml_render_gui_begin(&render,target_width,target_height);
  gml_render_gui_set_size(&render,target_width,target_height);
  gml_draw_surface_stretched(&render,0,dx,dy,dw,dh,0xFFFFFFu,1.0);
  gml_render_gui_end(&render);
  failed=compare_presentation(target,target_width,target_height,
                              source,source_width,source_height,
                              dx,dy,dw,dh,0xFF5A5A5Au,label);
  free(source);
  free(target);
  return failed;
}

static int presentation_fractional_quad_case(void){
  /* A destination whose edges fall between pixel centres. Rounding the rectangle before sampling
   * moves the first and last column by one source texel. */
  REQUIRE(presentation_case(5,3,17,11,0.37,0.62,16.25,9.75,"fractional quad")==0,
          "fractional destination edges");
  return 1;
}

static int presentation_clipped_edges_case(void){
  /* All four edges outside the target at once, so left/top clipping (which changes which source
   * column the first written pixel selects) is separated from right/bottom clipping (which does
   * not). */
  REQUIRE(presentation_case(7,5,19,13,-4.25,-3.5,27.5,20.25,"clipped edges")==0,
          "clipped destination on four sides");
  REQUIRE(presentation_case(7,5,19,13,-0.5,-0.5,20.0,14.0,"clipped half pixel")==0,
          "clipped by half a destination pixel");
  return 1;
}

static int presentation_odd_dimensions_case(void){
  /* Odd source and destination extents keep the doubled-accumulator half step observable. */
  REQUIRE(presentation_case(3,7,13,9,0.0,0.0,13.0,9.0,"odd extents")==0,
          "odd source and destination extents");
  REQUIRE(presentation_case(11,9,11,9,0.0,0.0,11.0,9.0,"identity extents")==0,
          "identity magnification");
  return 1;
}

static int presentation_repeated_rows_case(void){
  /* A magnification large enough that most destination rows repeat the previous source row, which
   * is the case the row-expanding implementation exists for. */
  REQUIRE(presentation_case(5,3,1103,19,0.0,0.0,1103.0,19.0,"repeated rows")==0,
          "repeated destination rows at high magnification");
  REQUIRE(presentation_case(2,2,640,480,0.0,0.0,640.0,480.0,"large magnification")==0,
          "large magnification from a tiny source");
  return 1;
}

static int presentation_letterbox_case(void){
  /* A destination rectangle strictly inside the target: everything outside it must be left exactly
   * as it was, which is what lets the host clear the margins once instead of every frame. */
  REQUIRE(presentation_case(4,4,32,24,6.0,4.0,20.0,16.0,"letterbox")==0,
          "letterbox margins untouched");
  return 1;
}

static int presentation_orientation_case(void){
  /* Four distinct corner colours over a neutral field. A flipped or transposed result still has the
   * right histogram, so the corners are asserted individually rather than through a hash. */
  enum { SOURCE=4,TARGET=12 };
  GmlWin content={0};
  GmlRender render;
  uint32_t source[SOURCE*SOURCE];
  uint32_t target[TARGET*TARGET];
  content.bytecode=14;
  for(size_t index=0;index<SOURCE*SOURCE;index++) source[index]=0xFF202020u;
  source[0]=0xFFFF0000u;
  source[SOURCE-1]=0xFF00FF00u;
  source[(size_t)(SOURCE-1)*SOURCE]=0xFF0000FFu;
  source[(size_t)(SOURCE-1)*SOURCE+(SOURCE-1)]=0xFFFFFF00u;
  memset(target,0,sizeof target);
  render_reset(&render,&content,source,SOURCE,SOURCE,target,TARGET,TARGET,0);
  gml_render_gui_begin(&render,TARGET,TARGET);
  gml_render_gui_set_size(&render,TARGET,TARGET);
  gml_draw_surface_stretched(&render,0,0.0,0.0,TARGET,TARGET,0xFFFFFFu,1.0);
  gml_render_gui_end(&render);
  REQUIRE(target[0]==0xFFFF0000u,"orientation top-left corner");
  REQUIRE(target[TARGET-1]==0xFF00FF00u,"orientation top-right corner");
  REQUIRE(target[(size_t)(TARGET-1)*TARGET]==0xFF0000FFu,"orientation bottom-left corner");
  REQUIRE(target[(size_t)(TARGET-1)*TARGET+(TARGET-1)]==0xFFFFFF00u,
          "orientation bottom-right corner");
  return 1;
}

static int presentation_transformed_quad_case(void){
  /* The other gate into the same kernel: automatic presentation is still enabled and the GUI
   * transform leaves the quad edges fractional. The rectangle the kernel receives is the mapped
   * one, so the expectation is computed from the mapped values. */
  enum { SOURCE_WIDTH=5,SOURCE_HEIGHT=3,TARGET_WIDTH=17,TARGET_HEIGHT=11,
         LOGICAL_WIDTH=7,LOGICAL_HEIGHT=5 };
  GmlWin content={0};
  GmlRender render;
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  double scale_x=(double)TARGET_WIDTH/(double)LOGICAL_WIDTH;
  double scale_y=(double)TARGET_HEIGHT/(double)LOGICAL_HEIGHT;
  content.bytecode=14;
  fill_asymmetric(source,SOURCE_WIDTH,SOURCE_HEIGHT);
  for(size_t index=0;index<TARGET_WIDTH*TARGET_HEIGHT;index++) target[index]=0xFF5A5A5Au;
  render_reset(&render,&content,source,SOURCE_WIDTH,SOURCE_HEIGHT,
               target,TARGET_WIDTH,TARGET_HEIGHT,1);
  gml_render_gui_begin(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_render_gui_set_size(&render,LOGICAL_WIDTH,LOGICAL_HEIGHT);
  gml_draw_surface_stretched(&render,0,0.0,0.0,SOURCE_WIDTH,SOURCE_HEIGHT,0xFFFFFFu,1.0);
  gml_render_gui_end(&render);
  REQUIRE(compare_presentation(target,TARGET_WIDTH,TARGET_HEIGHT,
                               source,SOURCE_WIDTH,SOURCE_HEIGHT,
                               0.0,0.0,(double)SOURCE_WIDTH*scale_x,(double)SOURCE_HEIGHT*scale_y,
                               0xFF5A5A5Au,"transformed quad")==0,
          "transformed GUI quad sampling");
  return 1;
}

/* A recordable presentation followed by an operation the plan cannot represent. Whatever a later
 * phase does with the first operation, the observable result must remain what the software renderer
 * alone produces: the complete pass replayed, then the unsupported drawing on top. */
static int presentation_software_replay_case(void){
  enum { SOURCE_WIDTH=4,SOURCE_HEIGHT=3,TARGET_WIDTH=20,TARGET_HEIGHT=15 };
  GmlWin content={0};
  GmlRender render;
  uint32_t source[SOURCE_WIDTH*SOURCE_HEIGHT];
  uint32_t target[TARGET_WIDTH*TARGET_HEIGHT];
  content.bytecode=14;
  fill_asymmetric(source,SOURCE_WIDTH,SOURCE_HEIGHT);
  for(size_t index=0;index<TARGET_WIDTH*TARGET_HEIGHT;index++) target[index]=0xFF5A5A5Au;
  render_reset(&render,&content,source,SOURCE_WIDTH,SOURCE_HEIGHT,
               target,TARGET_WIDTH,TARGET_HEIGHT,0);
  gml_render_gui_begin(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_render_gui_set_size(&render,TARGET_WIDTH,TARGET_HEIGHT);
  gml_draw_surface_stretched(&render,0,0.0,0.0,TARGET_WIDTH,TARGET_HEIGHT,0xFFFFFFu,1.0);
  /* An opaque rectangle over the middle: not a plan operation, and it reads nothing, so the pass
   * before it must be complete in the target before it runs. The colour is grey so that the
   * renderer's blue-green-red constant order cannot hide a channel swap in the comparison. */
  gml_render_primitive_rectangle(&render,4,3,12,9,0x3A3A3Au,0);
  gml_render_gui_end(&render);
  for(int y=0;y<TARGET_HEIGHT;y++) for(int x=0;x<TARGET_WIDTH;x++){
    uint32_t actual=target[(size_t)y*TARGET_WIDTH+x];
    uint32_t expected;
    if(x>=4 && x<=12 && y>=3 && y<=9) continue; /* the unsupported operation owns these */
    {
      int sx=source_index(x,0.0,(double)TARGET_WIDTH,SOURCE_WIDTH);
      int sy=source_index(y,0.0,(double)TARGET_HEIGHT,SOURCE_HEIGHT);
      expected=source[(size_t)sy*SOURCE_WIDTH+sx]|0xFF000000u;
    }
    if(actual!=expected){
      fprintf(stderr,"render plan software replay mismatch at %d,%d: %08x != %08x\n",
              x,y,actual,expected);
      return 0;
    }
  }
  {
    uint32_t centre=target[(size_t)6*TARGET_WIDTH+8];
    REQUIRE((centre&0xFFFFFFu)==0x3A3A3Au,"unsupported operation reached the target");
  }
  return 1;
}

int main(int argc,char **argv){
  const char *filter=NULL;
  for(int index=1;index<argc;++index){
    if(strcmp(argv[index],"--case")) continue;
    if(index+1>=argc){
      fprintf(stderr,"--case requires a filter\n");
      return EXIT_FAILURE;
    }
    filter=argv[++index];
  }
  static const AnygmTestCase presentation_cases[]={
    {"fractional_quad",presentation_fractional_quad_case},
    {"clipped_edges",presentation_clipped_edges_case},
    {"odd_dimensions",presentation_odd_dimensions_case},
    {"repeated_rows",presentation_repeated_rows_case},
    {"letterbox",presentation_letterbox_case},
    {"orientation",presentation_orientation_case},
    {"transformed_quad",presentation_transformed_quad_case},
    {"software_replay",presentation_software_replay_case},
  };
  const AnygmTestGroup groups[]={
    {"presentation",presentation_cases,
     sizeof presentation_cases/sizeof presentation_cases[0]},
  };
  AnygmTestResult result;
  anygm_test_run_groups(groups,sizeof groups/sizeof groups[0],filter,&result);
  printf("render plan cases: passed=%d failed=%d\n",result.passed,result.failed);
  return result.failed?EXIT_FAILURE:EXIT_SUCCESS;
}
