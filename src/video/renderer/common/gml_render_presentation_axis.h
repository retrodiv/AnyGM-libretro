/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_RENDER_PRESENTATION_AXIS_H
#define GML_RENDER_PRESENTATION_AXIS_H

/* Private inline rule: including implementation must already include renderer storage. */
#include <math.h>
#include <stdint.h>

/* The source index a written position selects, in whichever convention the record was made under.
 * One expression per rule, in one place: an accelerated execution builds its integer maps from the
 * same statements, so the two cannot select different texels.
 *
 * `local` counts from the first written position and `absolute` is the target coordinate; the
 * pixel-centre rule needs the second and the leading-edge rule needs the first. */
static inline int presentation_source_index(const GmlRenderDeferredPresentation *record,
                                     int vertical,int local,int absolute){
  const int source_extent=vertical?record->source_height:record->source_width;
  int index;
  if(record->sampling_rule==GML_RENDER_PRESENTATION_LEADING_EDGE){
    const int destination_extent=(int)(vertical?record->extent_y:record->extent_x);
    const int offset=vertical?record->local_offset_y:record->local_offset_x;
    if(destination_extent<=0) return 0;
    index=(int)(((int64_t)(offset+local)*(int64_t)source_extent)/(int64_t)destination_extent);
  } else {
    const double origin=vertical?record->origin_y:record->origin_x;
    const double extent=vertical?record->extent_y:record->extent_x;
    if(extent<=0.0) return 0;
    index=(int)floor((((double)absolute+0.5)-origin)*(double)source_extent/extent);
  }
  if(index<0) index=0;
  if(index>=source_extent) index=source_extent-1;
  return index;
}

#endif
