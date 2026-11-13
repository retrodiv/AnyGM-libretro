/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_VIDEO_CONSTANTS_H
#define GML_VIDEO_CONSTANTS_H

enum { GML_VIDEO_CLOSED=0, GML_VIDEO_PREPARING=1, GML_VIDEO_PLAYING=2, GML_VIDEO_PAUSED=3 };
enum { GML_VIDEO_FORMAT_RGBA=0, GML_VIDEO_FORMAT_YUV=1 };

/* Interface values shared by source compilation and runtime identifier reads. */
#define GML_VIDEO_CONSTANTS(X) \
  X(GML_VIDEO_CLOSED,"video_status_closed") \
  X(GML_VIDEO_PREPARING,"video_status_preparing") \
  X(GML_VIDEO_PLAYING,"video_status_playing") \
  X(GML_VIDEO_PAUSED,"video_status_paused") \
  X(GML_VIDEO_FORMAT_RGBA,"video_format_rgba") \
  X(GML_VIDEO_FORMAT_YUV,"video_format_yuv")

#endif
