/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_VIDEO_CODEC_H
#define GML_VIDEO_CODEC_H

#include <stddef.h>
#include <stdint.h>

typedef struct GmlVideoCodec GmlVideoCodec;
typedef struct {
  int width, height, has_audio;
  double duration_ms, frame_ms;
} GmlVideoInfo;
typedef struct {
  double time_ms, duration_ms;
  const uint8_t *rgba;
  int width, height;
  const int16_t *pcm;
  int samples; /* stereo frames at GML_VIDEO_AUDIO_RATE */
} GmlVideoDecoded;
enum { GML_VIDEO_ERROR=-1, GML_VIDEO_END=0, GML_VIDEO_IMAGE=1, GML_VIDEO_AUDIO=2 };
enum { GML_VIDEO_AUDIO_RATE=44100 };
enum { GML_VIDEO_DECODE_IMAGE=1, GML_VIDEO_DECODE_AUDIO=2 };

/* The immutable input borrow must outlive the decoder. Outputs borrow its storage until the
 * next operation. Neither the decoder nor its demuxer may open another file or network source. */
GmlVideoCodec *gml_video_codec_open(const uint8_t *bytes,size_t size);
/* Independent cursors let picture presentation retain one future frame without delaying audio
 * packets interleaved behind it. Metadata always describes the selected video stream. */
GmlVideoCodec *gml_video_codec_open_streams(const uint8_t *bytes,size_t size,unsigned streams);
GmlVideoInfo gml_video_codec_info(const GmlVideoCodec *codec);
int gml_video_codec_next(GmlVideoCodec *codec,GmlVideoDecoded *out);
int gml_video_codec_rewind(GmlVideoCodec *codec);
int gml_video_codec_error(const GmlVideoCodec *codec);
void gml_video_codec_close(GmlVideoCodec *codec);
/* A derived decoder continuation is accepted only by the same provider versions. */
void gml_video_codec_stamp(uint32_t versions[5]);
/* Runtime availability is separate from compilation: a missing shared provider leaves the
 * engine usable and causes video_open to take the generic skip path. */
int gml_video_codec_available(void);
const char *gml_video_codec_provider_error(void);

#endif
