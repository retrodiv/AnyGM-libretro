/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Render-plan lifetime, bounded recording, validation, axis mapping, and the scheduling facts that
 * decide whether a pass may leave the software executor. Nothing here knows what a GPU is. */
#include "gml_render_plan.h"

#include <math.h>
#include <string.h>

static int extent_ok(uint32_t value){
  return value>0u && value<=GML_PLAN_MAX_EXTENT;
}

/* Rectangle arithmetic on values derived from content. Every product and sum that will later index
 * memory is checked here, before the executor or an upload trusts it. */
static int rect_inside(GmlPlanRect rect,uint32_t width,uint32_t height){
  if(rect.width==0u || rect.height==0u) return 0;
  if(!extent_ok(rect.width) || !extent_ok(rect.height)) return 0;
  if(rect.x<0 || rect.y<0) return 0;
  if((uint32_t)rect.x>width || (uint32_t)rect.y>height) return 0;
  if(rect.width>width-(uint32_t)rect.x) return 0;
  if(rect.height>height-(uint32_t)rect.y) return 0;
  return 1;
}

static int axis_ok(const GmlPlanAxis *axis,uint32_t written){
  if(!axis || written==0u) return 0;
  if(!extent_ok(axis->source_extent) || !extent_ok(axis->destination_extent)) return 0;
  if(axis->destination_first<0) return 0;
  if((uint32_t)axis->destination_first>axis->destination_extent) return 0;
  if(written>axis->destination_extent-(uint32_t)axis->destination_first) return 0;
  switch(axis->rule){
    case GML_PLAN_AXIS_ACCUMULATOR:
      /* The recurrence is only defined where the destination does not shrink the source: the
       * reduction is a different operation with a different kernel. */
      return axis->destination_extent>=axis->source_extent;
    case GML_PLAN_AXIS_PIXEL_CENTRE:
      return axis->extent>0.0 && isfinite(axis->origin) && isfinite(axis->extent);
    default:
      return 0;
  }
}

void gml_render_plan_reset(GmlRenderPlan *plan,uint32_t target,
                           uint32_t target_width,uint32_t target_height){
  if(!plan) return;
  memset(plan,0,sizeof *plan);
  plan->target=target;
  plan->target_width=target_width;
  plan->target_height=target_height;
  plan->fallback_reason=GML_PLAN_FALLBACK_NONE;
  if(!extent_ok(target_width) || !extent_ok(target_height)){
    plan->overflowed=1;
    plan->fallback_reason=GML_PLAN_FALLBACK_PLAN_OVERFLOW;
  }
}

uint32_t gml_render_plan_add_image(GmlRenderPlan *plan,const GmlPlanImage *image){
  if(!plan || !image) return GML_PLAN_NO_IMAGE;
  if(plan->image_count>=GML_PLAN_MAX_IMAGES ||
     image->image_class==GML_PLAN_IMAGE_NONE ||
     image->pixel_format!=GML_PLAN_PIXEL_XRGB8888 ||
     !extent_ok(image->width) || !extent_ok(image->height) ||
     image->pitch_pixels<image->width || !extent_ok(image->pitch_pixels)){
    plan->overflowed=1;
    plan->fallback_reason=GML_PLAN_FALLBACK_PLAN_OVERFLOW;
    return GML_PLAN_NO_IMAGE;
  }
  plan->images[plan->image_count]=*image;
  return plan->image_count++;
}

static GmlPlanOp *append(GmlRenderPlan *plan){
  if(!plan) return NULL;
  if(plan->operation_count>=GML_PLAN_MAX_OPERATIONS){
    plan->overflowed=1;
    plan->fallback_reason=GML_PLAN_FALLBACK_PLAN_OVERFLOW;
    return NULL;
  }
  GmlPlanOp *op=&plan->operations[plan->operation_count++];
  memset(op,0,sizeof *op);
  op->source=GML_PLAN_NO_IMAGE;
  return op;
}

int gml_render_plan_add_clear(GmlRenderPlan *plan,GmlPlanRect destination,uint32_t color){
  GmlPlanOp *op=append(plan);
  if(!op) return 0;
  op->opcode=GML_PLAN_OP_CLEAR_XRGB;
  op->destination=destination;
  op->clear_color=color&0x00FFFFFFu;
  return 1;
}

int gml_render_plan_add_blit_nearest(GmlRenderPlan *plan,uint32_t source,
                                     GmlPlanRect destination,
                                     GmlPlanAxis axis_x,GmlPlanAxis axis_y,
                                     uint32_t alpha_write){
  GmlPlanOp *op=append(plan);
  if(!op) return 0;
  op->opcode=GML_PLAN_OP_BLIT_OPAQUE_NEAREST;
  op->source=source;
  op->destination=destination;
  op->axis_x=axis_x;
  op->axis_y=axis_y;
  op->alpha_write=alpha_write&0xFFu;
  return 1;
}

int gml_render_plan_add_blit_box(GmlRenderPlan *plan,uint32_t source,
                                 GmlPlanRect destination,uint32_t alpha_write){
  GmlPlanOp *op=append(plan);
  if(!op) return 0;
  op->opcode=GML_PLAN_OP_BLIT_OPAQUE_BOX;
  op->source=source;
  op->destination=destination;
  op->alpha_write=alpha_write&0xFFu;
  return 1;
}

int gml_render_plan_add_present_cpu_frame(GmlRenderPlan *plan,uint32_t source,
                                          GmlPlanRect destination){
  GmlPlanOp *op=append(plan);
  if(!op) return 0;
  op->opcode=GML_PLAN_OP_PRESENT_CPU_FRAME;
  op->source=source;
  op->destination=destination;
  op->alpha_write=0xFFu;
  return 1;
}

int gml_render_plan_validate(const GmlRenderPlan *plan){
  if(!plan || plan->overflowed) return 0;
  if(!extent_ok(plan->target_width) || !extent_ok(plan->target_height)) return 0;
  if(plan->operation_count==0u || plan->operation_count>GML_PLAN_MAX_OPERATIONS) return 0;
  if(plan->image_count>GML_PLAN_MAX_IMAGES) return 0;
  if(plan->target!=GML_PLAN_TARGET_CPU_FRAME &&
     plan->target!=GML_PLAN_TARGET_HOST_FRAMEBUFFER) return 0;
  for(uint32_t index=0;index<plan->image_count;index++){
    const GmlPlanImage *image=&plan->images[index];
    if(image->pixel_format!=GML_PLAN_PIXEL_XRGB8888) return 0;
    if(!extent_ok(image->width) || !extent_ok(image->height)) return 0;
    if(image->pitch_pixels<image->width || !extent_ok(image->pitch_pixels)) return 0;
    /* height * pitch must stay inside the addressable range the executor will walk. */
    if((uint64_t)image->pitch_pixels*(uint64_t)image->height >
       (uint64_t)GML_PLAN_MAX_EXTENT*(uint64_t)GML_PLAN_MAX_EXTENT) return 0;
  }
  for(uint32_t index=0;index<plan->operation_count;index++){
    const GmlPlanOp *op=&plan->operations[index];
    if(!rect_inside(op->destination,plan->target_width,plan->target_height)) return 0;
    if(op->opcode==GML_PLAN_OP_CLEAR_XRGB){
      if(op->source!=GML_PLAN_NO_IMAGE) return 0;
      continue;
    }
    if(op->source>=plan->image_count) return 0;
    const GmlPlanImage *image=&plan->images[op->source];
    if(!image->cpu_pixels) return 0;
    switch(op->opcode){
      case GML_PLAN_OP_BLIT_OPAQUE_NEAREST:
        if(!axis_ok(&op->axis_x,op->destination.width)) return 0;
        if(!axis_ok(&op->axis_y,op->destination.height)) return 0;
        if(op->axis_x.source_extent!=image->width) return 0;
        if(op->axis_y.source_extent!=image->height) return 0;
        break;
      case GML_PLAN_OP_BLIT_OPAQUE_BOX:
        /* The reduction reads the whole source and writes the whole destination rectangle. */
        if(image->width<op->destination.width && image->height<op->destination.height) return 0;
        break;
      case GML_PLAN_OP_PRESENT_CPU_FRAME:
        if(op->destination.width!=image->width || op->destination.height!=image->height) return 0;
        break;
      default:
        return 0;
    }
  }
  return 1;
}

int gml_render_plan_axis_map(const GmlPlanAxis *axis,uint16_t *map,uint32_t count){
  if(!map || !axis_ok(axis,count)) return 0;
  if(axis->source_extent>0xFFFFu) return 0;
  uint32_t first=(uint32_t)axis->destination_first;
  if(axis->rule==GML_PLAN_AXIS_ACCUMULATOR){
    /* Walk the recurrence from its own origin rather than seeking into it: the accumulator is the
     * definition, and a closed form that agrees for the values tested is not the same statement. */
    uint32_t source=0u;
    uint32_t accumulator=axis->source_extent;
    for(uint32_t index=0;index<first+count;index++){
      if(index>=first) map[index-first]=(uint16_t)source;
      accumulator+=axis->source_extent*2u;
      if(accumulator>=axis->destination_extent*2u){
        accumulator-=axis->destination_extent*2u;
        source++;
      }
    }
    return 1;
  }
  for(uint32_t index=0;index<count;index++){
    double destination=(double)(first+index);
    int source=(int)floor(((destination+0.5)-axis->origin)*
                          (double)axis->source_extent/axis->extent);
    if(source<0) source=0;
    if(source>=(int)axis->source_extent) source=(int)axis->source_extent-1;
    map[index]=(uint16_t)source;
  }
  return 1;
}

int gml_render_plan_compose_maps(const uint16_t *outer,const uint16_t *inner,
                                 uint16_t *map,uint32_t count,uint32_t inner_count){
  if(!outer || !inner || !map || count==0u || inner_count==0u) return 0;
  for(uint32_t index=0;index<count;index++){
    uint32_t middle=outer[index];
    if(middle>=inner_count) return 0;
    map[index]=inner[middle];
  }
  return 1;
}

int gml_render_plan_gpu_eligible(GmlRenderPlan *plan){
  if(!plan) return 0;
  if(!gml_render_plan_validate(plan)){
    if(plan->fallback_reason==GML_PLAN_FALLBACK_NONE)
      plan->fallback_reason=GML_PLAN_FALLBACK_PLAN_OVERFLOW;
    return 0;
  }
  for(uint32_t index=0;index<plan->operation_count;index++){
    const GmlPlanOp *op=&plan->operations[index];
    if(op->opcode==GML_PLAN_OP_BLIT_OPAQUE_BOX){
      plan->fallback_reason=GML_PLAN_FALLBACK_BOX_REDUCTION;
      return 0;
    }
    if(op->opcode!=GML_PLAN_OP_CLEAR_XRGB &&
       op->opcode!=GML_PLAN_OP_BLIT_OPAQUE_NEAREST &&
       op->opcode!=GML_PLAN_OP_PRESENT_CPU_FRAME){
      plan->fallback_reason=GML_PLAN_FALLBACK_UNSUPPORTED_OPERATION;
      return 0;
    }
    if(op->opcode==GML_PLAN_OP_BLIT_OPAQUE_NEAREST &&
       (op->axis_x.source_extent>0xFFFFu || op->axis_y.source_extent>0xFFFFu)){
      /* The integer axis maps are what make the nearest selection exact, and their element type
       * bounds the source extent they can address. */
      plan->fallback_reason=GML_PLAN_FALLBACK_UNSUPPORTED_OPERATION;
      return 0;
    }
  }
  plan->fallback_reason=GML_PLAN_FALLBACK_NONE;
  return 1;
}

const char *gml_render_plan_fallback_name(uint32_t reason){
  switch(reason){
    case GML_PLAN_FALLBACK_NONE: return "none";
    case GML_PLAN_FALLBACK_UNSUPPORTED_OPERATION: return "unsupported_operation";
    case GML_PLAN_FALLBACK_SOURCE_ALIAS: return "source_alias";
    case GML_PLAN_FALLBACK_SOURCE_LIFETIME: return "source_lifetime";
    case GML_PLAN_FALLBACK_BOX_REDUCTION: return "box_reduction";
    case GML_PLAN_FALLBACK_BLEND_NOT_EXACT: return "blend_not_exact";
    case GML_PLAN_FALLBACK_SURFACE_READBACK: return "surface_readback";
    case GML_PLAN_FALLBACK_PLAN_OVERFLOW: return "plan_overflow";
    case GML_PLAN_FALLBACK_CONTEXT_UNAVAILABLE: return "context_unavailable";
    case GML_PLAN_FALLBACK_RESOURCE_UPLOAD_FAILURE: return "resource_upload_failure";
    default: return "unknown";
  }
}
