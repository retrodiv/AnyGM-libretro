/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* The exact software executor for a render plan. These kernels are the canonical implementation:
 * a GPU execution is accepted only when it reproduces what this file produces, and a pass that
 * cannot be executed exactly elsewhere is replayed here in full. The bodies are the established
 * ones, moved rather than rewritten. */
#include "gml_render_plan.h"
#include "gml_render_internal.h"

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

/* The rows of the accumulator expansion, from an arbitrary starting row. The recurrence's state at
 * that row is derived in the recurrence's own integers -- after y increments the total added is
 * source_height*(2y+1) and the >= rule has subtracted twice the destination height exactly
 * floor(total / (2*destination_height)) times -- so a band beginning mid-way holds exactly the
 * accumulator the sequential walk would have carried there. Each band starts with no previous row,
 * so a repeated source row at a band boundary is expanded instead of copied: identical pixels
 * either way. */
static void execute_nearest_accumulator_rows(const GmlPlanOp *op,const GmlPlanImage *image,
                                             uint32_t *target,uint32_t pitch,
                                             const unsigned *column,
                                             unsigned row_start,unsigned row_end){
  const uint32_t *source=image->cpu_pixels;
  const uint32_t source_height=op->axis_y.source_extent;
  const uint32_t destination_width=op->destination.width;
  const uint32_t destination_height=op->destination.height;
  const uint32_t destination_x=(uint32_t)op->destination.x;
  const uint32_t destination_y=(uint32_t)op->destination.y;
  const uint32_t alpha_write=op->alpha_write;
  uint64_t total=(uint64_t)source_height+2ull*source_height*row_start;
  unsigned source_y=(unsigned)(total/(2ull*destination_height));
  unsigned y_accumulator=(unsigned)(total-2ull*destination_height*source_y);
  unsigned previous_source_y=0;
  const uint32_t *previous_row=NULL;
  for(unsigned y=row_start;y<row_end;y++){
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
}

typedef struct PlanAccumulatorBand {
  const GmlPlanOp *op;
  const GmlPlanImage *image;
  uint32_t *target;
  uint32_t pitch;
  const unsigned *column;
} PlanAccumulatorBand;

static void plan_accumulator_band(void *context,int row_start,int row_end,int slot){
  PlanAccumulatorBand *band=(PlanAccumulatorBand*)context;
  (void)slot;
  execute_nearest_accumulator_rows(band->op,band->image,band->target,band->pitch,band->column,
                                   (unsigned)row_start,(unsigned)row_end);
}

/* The column recurrence restarts identically on every row, and a destination row whose source row
 * repeats the previous one holds exactly the pixels already written. Walk the recurrence once into
 * a column map, expand one destination row per distinct source row, and copy the repeats. The
 * accumulators are the same recurrences the per-pixel form evaluated, so the result is unchanged.
 * With a pool owner and a destination past the visible-pixel threshold the rows split across the
 * renderer's band pool; rows are independent because each derives its whole state from its index. */
static void execute_nearest_accumulator(const GmlPlanOp *op,const GmlPlanImage *image,
                                        uint32_t *target,uint32_t pitch,
                                        GmlRender *pool_owner){
  const uint32_t source_width=op->axis_x.source_extent;
  const uint32_t destination_width=op->destination.width;
  const uint32_t destination_height=op->destination.height;
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
    if(pool_owner &&
       (uint64_t)destination_width*(uint64_t)destination_height>=262144ull){
      PlanAccumulatorBand band={op,image,target,pitch,column};
      gml_run_row_bands(pool_owner,(int)destination_height,plan_accumulator_band,&band);
    } else {
      execute_nearest_accumulator_rows(op,image,target,pitch,column,0,destination_height);
    }
    if(column!=column_stack) free(column);
    return;
  }
  {
    const uint32_t *source=image->cpu_pixels;
    const uint32_t source_height=op->axis_y.source_extent;
    const uint32_t destination_x=(uint32_t)op->destination.x;
    const uint32_t destination_y=(uint32_t)op->destination.y;
    const uint32_t alpha_write=op->alpha_write;
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
typedef struct {
  const GmlPlanOp *op;
  const GmlPlanImage *image;
  uint32_t *target;
  uint32_t pitch;
} PlanBoxBand;

static void execute_box_rows(const GmlPlanOp *op,const GmlPlanImage *image,
                             uint32_t *target,uint32_t pitch,
                             unsigned row_start,unsigned row_end){
  const uint32_t *source=image->cpu_pixels;
  const unsigned source_width=image->width;
  const unsigned source_height=image->height;
  const unsigned destination_width=op->destination.width;
  const unsigned destination_height=op->destination.height;
  const unsigned destination_x=(uint32_t)op->destination.x;
  const unsigned destination_y=(uint32_t)op->destination.y;
  const uint32_t alpha_write=op->alpha_write;
  for(unsigned y=row_start;y<row_end;y++){
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

static void plan_box_band(void *context,int row_start,int row_end,int slot){
  PlanBoxBand *band=(PlanBoxBand*)context;
  (void)slot;
  execute_box_rows(band->op,band->image,band->target,band->pitch,
                   (unsigned)row_start,(unsigned)row_end);
}

static void execute_box(const GmlPlanOp *op,const GmlPlanImage *image,
                        uint32_t *target,uint32_t pitch,GmlRender *pool_owner){
  const unsigned destination_width=op->destination.width;
  const unsigned destination_height=op->destination.height;
  if(pool_owner &&
     (uint64_t)destination_width*(uint64_t)destination_height>=262144ull){
    PlanBoxBand band={op,image,target,pitch};
    gml_run_row_bands(pool_owner,(int)destination_height,plan_box_band,&band);
    return;
  }
  execute_box_rows(op,image,target,pitch,0,destination_height);
}

typedef struct PlanSharpTap {
  uint32_t first,second,weight;
} PlanSharpTap;

/* Sample a virtual integer nearest enlargement without ever allocating it. Rational pixel-centre
 * coordinates preserve exact integer enlargements; the only rounding is the bilinear Q16 weight.
 * Clamp in the expanded plane before converting its two neighbours back to source indices. */
static PlanSharpTap sharp_tap(uint32_t pixel,uint32_t source,uint32_t destination,uint32_t scale){
  PlanSharpTap tap={0,0,0};
  uint64_t numerator=(2ull*pixel+1u)*source*scale;
  uint32_t denominator=2u*destination;
  if(numerator<=destination) return tap;
  numerator-=destination;
  uint64_t first=numerator/denominator;
  tap.first=(uint32_t)(first/scale);
  tap.second=(uint32_t)((first+1u)/scale);
  if(tap.first>=source) tap.first=source-1u;
  if(tap.second>=source) tap.second=source-1u;
  tap.weight=(uint32_t)(((numerator%denominator)*65536u+denominator/2u)/denominator);
  return tap;
}

static uint32_t sharp_lerp(uint32_t a,uint32_t b,uint32_t weight){
  uint32_t inverse=65536u-weight,result=0;
  for(unsigned shift=0;shift<24;shift+=8){
    uint32_t channel=(((a>>shift)&255u)*inverse+((b>>shift)&255u)*weight+32768u)>>16;
    result|=channel<<shift;
  }
  return result;
}

static void execute_sharp_bilinear(const GmlPlanOp *op,const GmlPlanImage *image,
                                   uint32_t *target,uint32_t pitch){
  uint32_t width=op->destination.width,height=op->destination.height;
  uint32_t scale_x=(width+image->width-1u)/image->width;
  uint32_t scale_y=(height+image->height-1u)/image->height;
  uint32_t scale=scale_x<scale_y?scale_x:scale_y;
  PlanSharpTap stack[PLAN_COLUMN_STACK];
  PlanSharpTap *columns=width<=PLAN_COLUMN_STACK?stack:malloc((size_t)width*sizeof(*columns));
  if(columns)
    for(uint32_t x=0;x<width;x++) columns[x]=sharp_tap(x,image->width,width,scale);
  for(uint32_t y=0;y<height;y++){
    PlanSharpTap vertical=sharp_tap(y,image->height,height,scale);
    const uint32_t *row0=image->cpu_pixels+(size_t)vertical.first*image->pitch_pixels;
    const uint32_t *row1=image->cpu_pixels+(size_t)vertical.second*image->pitch_pixels;
    uint32_t *out=target+(size_t)((uint32_t)op->destination.y+y)*pitch+(uint32_t)op->destination.x;
    for(uint32_t x=0;x<width;x++){
      PlanSharpTap horizontal=columns?columns[x]:sharp_tap(x,image->width,width,scale);
      uint32_t a=sharp_lerp(row0[horizontal.first],row0[horizontal.second],horizontal.weight);
      if(vertical.weight && vertical.first!=vertical.second){
        uint32_t b=sharp_lerp(row1[horizontal.first],row1[horizontal.second],horizontal.weight);
        a=sharp_lerp(a,b,vertical.weight);
      }
      out[x]=plan_pixel(a,op->alpha_write);
    }
  }
  if(columns!=stack) free(columns);
}

static void execute_present(const GmlPlanOp *op,const GmlPlanImage *image,
                            uint32_t *target,uint32_t pitch){
  for(uint32_t y=0;y<op->destination.height;y++)
    memcpy(target+(size_t)((uint32_t)op->destination.y+y)*pitch+(uint32_t)op->destination.x,
           image->cpu_pixels+(size_t)y*image->pitch_pixels,
           (size_t)op->destination.width*sizeof(uint32_t));
}

int gml_render_plan_execute_software_pooled(const GmlRenderPlan *plan,uint32_t *target,
                                            uint32_t target_pitch_pixels,
                                            struct GmlRender *pool_owner){
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
          execute_nearest_accumulator(op,image,target,target_pitch_pixels,pool_owner);
        else
          execute_nearest_pixel_centre(op,image,target,target_pitch_pixels);
        break;
      case GML_PLAN_OP_BLIT_OPAQUE_BOX:
        execute_box(op,image,target,target_pitch_pixels,pool_owner);
        break;
      case GML_PLAN_OP_PRESENT_CPU_FRAME:
        execute_present(op,image,target,target_pitch_pixels);
        break;
      case GML_PLAN_OP_BLIT_OPAQUE_SHARP_BILINEAR:
        execute_sharp_bilinear(op,image,target,target_pitch_pixels);
        break;
      default:
        return 0;
    }
  }
  return 1;
}

int gml_render_plan_execute_software(const GmlRenderPlan *plan,uint32_t *target,
                                     uint32_t target_pitch_pixels){
  return gml_render_plan_execute_software_pooled(plan,target,target_pitch_pixels,NULL);
}
