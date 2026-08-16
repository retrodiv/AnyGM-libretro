/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The exact software executor for a render plan. These kernels are the canonical implementation:
 * a GPU execution is accepted only when it reproduces what this file produces, and a pass that
 * cannot be executed exactly elsewhere is replayed here in full. The bodies are the established
 * ones, moved rather than rewritten. */
#include "gml_render_plan.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { PLAN_COLUMN_STACK=1024 };

static uint32_t plan_pixel(uint32_t source,uint32_t alpha_write){
  return (source&0x00FFFFFFu)|(alpha_write<<24);
}

static void execute_clear(const GmlPlanOp *op,uint32_t *target,uint32_t pitch){
  uint32_t value=op->clear_color&0x00FFFFFFu;
  for(uint32_t y=0;y<op->destination.height;y++){
    uint32_t *row=target+(size_t)((uint32_t)op->destination.y+y)*pitch+(uint32_t)op->destination.x;
    for(uint32_t x=0;x<op->destination.width;x++) row[x]=value;
  }
}

/* The column recurrence restarts identically on every row, and a destination row whose source row
 * repeats the previous one holds exactly the pixels already written. Walk the recurrence once into
 * a column map, expand one destination row per distinct source row, and copy the repeats. The
 * accumulators are the same recurrences the per-pixel form evaluated, so the result is unchanged. */
static void execute_nearest_accumulator(const GmlPlanOp *op,const GmlPlanImage *image,
                                        uint32_t *target,uint32_t pitch){
  const uint32_t *source=image->cpu_pixels;
  const uint32_t source_width=op->axis_x.source_extent;
  const uint32_t source_height=op->axis_y.source_extent;
  const uint32_t destination_width=op->destination.width;
  const uint32_t destination_height=op->destination.height;
  const uint32_t destination_x=(uint32_t)op->destination.x;
  const uint32_t destination_y=(uint32_t)op->destination.y;
  const uint32_t alpha_write=op->alpha_write;
  unsigned column_stack[PLAN_COLUMN_STACK];
  unsigned *column=destination_width<=(unsigned)PLAN_COLUMN_STACK
    ?column_stack:(unsigned*)malloc((size_t)destination_width*sizeof(*column));
  if(column){
    unsigned source_x=0,x_accumulator=source_width;
    for(unsigned x=0;x<destination_width;x++){
      column[x]=source_x;
      x_accumulator+=source_width*2u;
      if(x_accumulator>=destination_width*2u){
        x_accumulator-=destination_width*2u;
        source_x++;
      }
    }
    unsigned source_y=0,y_accumulator=source_height,previous_source_y=0;
    const uint32_t *previous_row=NULL;
    for(unsigned y=0;y<destination_height;y++){
      uint32_t *destination_row=target+(size_t)(destination_y+y)*pitch+destination_x;
      if(previous_row && source_y==previous_source_y){
        memcpy(destination_row,previous_row,
               (size_t)destination_width*sizeof(*destination_row));
      } else {
        const uint32_t *source_row=source+(size_t)source_y*image->pitch_pixels;
        unsigned run=0;
        for(unsigned x=1;x<=destination_width;x++){
          if(x<destination_width && column[x]==column[run]) continue;
          uint32_t value=plan_pixel(source_row[column[run]],alpha_write);
          for(unsigned i=run;i<x;i++) destination_row[i]=value;
          run=x;
        }
        previous_row=destination_row;
        previous_source_y=source_y;
      }
      y_accumulator+=source_height*2u;
      if(y_accumulator>=destination_height*2u){
        y_accumulator-=destination_height*2u;
        source_y++;
      }
    }
    if(column!=column_stack) free(column);
    return;
  }
  {
    unsigned source_y=0,y_accumulator=source_height;
    for(unsigned y=0;y<destination_height;y++){
      const uint32_t *source_row=source+(size_t)source_y*image->pitch_pixels;
      uint32_t *destination_row=target+(size_t)(destination_y+y)*pitch+destination_x;
      unsigned source_x=0,x_accumulator=source_width;
      for(unsigned x=0;x<destination_width;x++){
        destination_row[x]=plan_pixel(source_row[source_x],alpha_write);
        x_accumulator+=source_width*2u;
        if(x_accumulator>=destination_width*2u){
          x_accumulator-=destination_width*2u;
          source_x++;
        }
      }
      y_accumulator+=source_height*2u;
      if(y_accumulator>=destination_height*2u){
        y_accumulator-=destination_height*2u;
        source_y++;
      }
    }
  }
}

/* The source column a destination column selects does not depend on the row, and a destination row
 * whose source row repeats the previous one is an exact copy of what was just written. Hoist the
 * column map, expand one destination row per distinct source row, and repeat that row. The sampled
 * coordinates are the same expressions the per-pixel form evaluated. */
static void execute_nearest_pixel_centre(const GmlPlanOp *op,const GmlPlanImage *image,
                                         uint32_t *target,uint32_t pitch){
  const uint32_t *source=image->cpu_pixels;
  const int source_width=(int)op->axis_x.source_extent;
  const int source_height=(int)op->axis_y.source_extent;
  const int span=(int)op->destination.width;
  const int rows=(int)op->destination.height;
  const int first_x=op->axis_x.destination_first;
  const int first_y=op->axis_y.destination_first;
  const double dx=op->axis_x.origin,dw=op->axis_x.extent;
  const double dy=op->axis_y.origin,dh=op->axis_y.extent;
  const uint32_t alpha_write=op->alpha_write;
  int column_stack[PLAN_COLUMN_STACK];
  int *column=span<=PLAN_COLUMN_STACK?column_stack:(int*)malloc((size_t)span*sizeof(*column));
  if(!column){
    for(int y=0;y<rows;y++){
      int sy=(int)floor((((double)(first_y+y)+0.5)-dy)*(double)source_height/dh);
      if(sy<0) sy=0; else if(sy>=source_height) sy=source_height-1;
      uint32_t *destination=target+(size_t)((uint32_t)op->destination.y+(uint32_t)y)*pitch+
                            (uint32_t)op->destination.x;
      for(int x=0;x<span;x++){
        int sx=(int)floor((((double)(first_x+x)+0.5)-dx)*(double)source_width/dw);
        if(sx<0) sx=0; else if(sx>=source_width) sx=source_width-1;
        destination[x]=plan_pixel(source[(size_t)sy*image->pitch_pixels+(unsigned)sx],alpha_write);
      }
    }
    return;
  }
  for(int x=0;x<span;x++){
    int sx=(int)floor((((double)(first_x+x)+0.5)-dx)*(double)source_width/dw);
    if(sx<0) sx=0; else if(sx>=source_width) sx=source_width-1;
    column[x]=sx;
  }
  {
    int previous_sy=-1;
    const uint32_t *previous_row=NULL;
    for(int y=0;y<rows;y++){
      int sy=(int)floor((((double)(first_y+y)+0.5)-dy)*(double)source_height/dh);
      if(sy<0) sy=0; else if(sy>=source_height) sy=source_height-1;
      uint32_t *destination=target+(size_t)((uint32_t)op->destination.y+(uint32_t)y)*pitch+
                            (uint32_t)op->destination.x;
      if(sy==previous_sy){
        memcpy(destination,previous_row,(size_t)span*sizeof(*destination));
        continue;
      }
      {
        const uint32_t *source_row=source+(size_t)sy*image->pitch_pixels;
        for(int x=0;x<span;x++) destination[x]=plan_pixel(source_row[column[x]],alpha_write);
      }
      previous_sy=sy;
      previous_row=destination;
    }
  }
  if(column!=column_stack) free(column);
}

/* A real box average over the source pixels each destination pixel covers. An ordinary bilinear
 * sample is not equivalent, which is why this stays software until a GPU reduction reproduces the
 * integer channel sums and the division rounding. */
static void execute_box(const GmlPlanOp *op,const GmlPlanImage *image,
                        uint32_t *target,uint32_t pitch){
  const uint32_t *source=image->cpu_pixels;
  const unsigned source_width=image->width;
  const unsigned source_height=image->height;
  const unsigned destination_width=op->destination.width;
  const unsigned destination_height=op->destination.height;
  const unsigned destination_x=(uint32_t)op->destination.x;
  const unsigned destination_y=(uint32_t)op->destination.y;
  const uint32_t alpha_write=op->alpha_write;
  for(unsigned y=0;y<destination_height;y++){
    unsigned source_y0=y*source_height/destination_height;
    unsigned source_y1=(y+1)*source_height/destination_height;
    if(source_y1<=source_y0) source_y1=source_y0+1;
    for(unsigned x=0;x<destination_width;x++){
      unsigned source_x0=x*source_width/destination_width;
      unsigned source_x1=(x+1)*source_width/destination_width;
      if(source_x1<=source_x0) source_x1=source_x0+1;
      unsigned red=0,green=0,blue=0,count=0;
      for(unsigned source_y=source_y0;
          source_y<source_y1 && source_y<source_height;source_y++)
        for(unsigned source_x=source_x0;
            source_x<source_x1 && source_x<source_width;source_x++){
          uint32_t pixel=source[(size_t)source_y*image->pitch_pixels+source_x];
          red+=(pixel>>16)&0xFFu;
          green+=(pixel>>8)&0xFFu;
          blue+=pixel&0xFFu;
          count++;
        }
      if(!count) count=1;
      target[(size_t)(destination_y+y)*pitch+destination_x+x]=
        plan_pixel(((red/count)<<16)|((green/count)<<8)|(blue/count),alpha_write);
    }
  }
}

static void execute_present(const GmlPlanOp *op,const GmlPlanImage *image,
                            uint32_t *target,uint32_t pitch){
  for(uint32_t y=0;y<op->destination.height;y++)
    memcpy(target+(size_t)((uint32_t)op->destination.y+y)*pitch+(uint32_t)op->destination.x,
           image->cpu_pixels+(size_t)y*image->pitch_pixels,
           (size_t)op->destination.width*sizeof(uint32_t));
}

int gml_render_plan_execute_software(const GmlRenderPlan *plan,uint32_t *target,
                                     uint32_t target_pitch_pixels){
  if(!plan || !target) return 0;
  if(target_pitch_pixels<plan->target_width) return 0;
  if(!gml_render_plan_validate(plan)) return 0;
  for(uint32_t index=0;index<plan->operation_count;index++){
    const GmlPlanOp *op=&plan->operations[index];
    const GmlPlanImage *image=op->source<plan->image_count?&plan->images[op->source]:NULL;
    switch(op->opcode){
      case GML_PLAN_OP_CLEAR_XRGB:
        execute_clear(op,target,target_pitch_pixels);
        break;
      case GML_PLAN_OP_BLIT_OPAQUE_NEAREST:
        if(op->axis_x.rule==GML_PLAN_AXIS_ACCUMULATOR)
          execute_nearest_accumulator(op,image,target,target_pitch_pixels);
        else
          execute_nearest_pixel_centre(op,image,target,target_pitch_pixels);
        break;
      case GML_PLAN_OP_BLIT_OPAQUE_BOX:
        execute_box(op,image,target,target_pitch_pixels);
        break;
      case GML_PLAN_OP_PRESENT_CPU_FRAME:
        execute_present(op,image,target,target_pitch_pixels);
        break;
      default:
        return 0;
    }
  }
  return 1;
}
