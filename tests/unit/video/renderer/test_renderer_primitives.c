/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_render_internal.h"
#include "gml_render_primitives.h"

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

int main(void){
  check_fractional_radius_is_not_truncated();
  check_fractional_centre_is_not_quantized();
  check_integer_entry_point_is_unchanged();
  check_circle_geometry_applies_the_measured_bias();
  if(failures){
    fprintf(stderr,"renderer primitives: %d failure(s)\n",failures);
    return 1;
  }
  printf("renderer primitives: ok\n");
  return 0;
}
