/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_VIDEO_PROVIDER_H
#define GML_VIDEO_PROVIDER_H

#ifndef ANYGM_VIDEO_PROVIDER_DYNAMIC
#define ANYGM_VIDEO_PROVIDER_DYNAMIC 0
#endif

#if ANYGM_VIDEO_PROVIDER_DYNAMIC
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#if LIBAVCODEC_VERSION_MAJOR < 62 || LIBAVFORMAT_VERSION_MAJOR < 62 || \
    LIBAVUTIL_VERSION_MAJOR < 60 || LIBSWSCALE_VERSION_MAJOR < 9 || \
    LIBSWRESAMPLE_VERSION_MAJOR < 6
#error Video playback requires FFmpeg 8 or later development headers.
#endif

typedef struct GmlVideoProviderApi {
#define GML_VIDEO_PROVIDER_SYMBOL(name) __typeof__(&name) name
  GML_VIDEO_PROVIDER_SYMBOL(av_channel_layout_check);
  GML_VIDEO_PROVIDER_SYMBOL(av_channel_layout_compare);
  GML_VIDEO_PROVIDER_SYMBOL(av_channel_layout_copy);
  GML_VIDEO_PROVIDER_SYMBOL(av_channel_layout_default);
  GML_VIDEO_PROVIDER_SYMBOL(av_channel_layout_uninit);
  GML_VIDEO_PROVIDER_SYMBOL(av_dict_free);
  GML_VIDEO_PROVIDER_SYMBOL(av_dict_set);
  GML_VIDEO_PROVIDER_SYMBOL(av_find_best_stream);
  GML_VIDEO_PROVIDER_SYMBOL(av_frame_alloc);
  GML_VIDEO_PROVIDER_SYMBOL(av_frame_free);
  GML_VIDEO_PROVIDER_SYMBOL(av_frame_get_buffer);
  GML_VIDEO_PROVIDER_SYMBOL(av_frame_unref);
  GML_VIDEO_PROVIDER_SYMBOL(av_free);
  GML_VIDEO_PROVIDER_SYMBOL(av_freep);
  GML_VIDEO_PROVIDER_SYMBOL(av_get_bytes_per_sample);
  GML_VIDEO_PROVIDER_SYMBOL(av_malloc);
  GML_VIDEO_PROVIDER_SYMBOL(av_packet_alloc);
  GML_VIDEO_PROVIDER_SYMBOL(av_packet_free);
  GML_VIDEO_PROVIDER_SYMBOL(av_packet_unref);
  GML_VIDEO_PROVIDER_SYMBOL(av_read_frame);
  GML_VIDEO_PROVIDER_SYMBOL(av_realloc);
  GML_VIDEO_PROVIDER_SYMBOL(av_rescale_rnd);
  GML_VIDEO_PROVIDER_SYMBOL(avcodec_alloc_context3);
  GML_VIDEO_PROVIDER_SYMBOL(avcodec_find_decoder);
  GML_VIDEO_PROVIDER_SYMBOL(avcodec_free_context);
  GML_VIDEO_PROVIDER_SYMBOL(avcodec_open2);
  GML_VIDEO_PROVIDER_SYMBOL(avcodec_parameters_to_context);
  GML_VIDEO_PROVIDER_SYMBOL(avcodec_receive_frame);
  GML_VIDEO_PROVIDER_SYMBOL(avcodec_send_packet);
  GML_VIDEO_PROVIDER_SYMBOL(avcodec_version);
  GML_VIDEO_PROVIDER_SYMBOL(avformat_alloc_context);
  GML_VIDEO_PROVIDER_SYMBOL(avformat_close_input);
  GML_VIDEO_PROVIDER_SYMBOL(avformat_find_stream_info);
  GML_VIDEO_PROVIDER_SYMBOL(avformat_open_input);
  GML_VIDEO_PROVIDER_SYMBOL(avformat_version);
  GML_VIDEO_PROVIDER_SYMBOL(avio_alloc_context);
  GML_VIDEO_PROVIDER_SYMBOL(avio_context_free);
  GML_VIDEO_PROVIDER_SYMBOL(avutil_version);
  GML_VIDEO_PROVIDER_SYMBOL(swresample_version);
  GML_VIDEO_PROVIDER_SYMBOL(swr_alloc_set_opts2);
  GML_VIDEO_PROVIDER_SYMBOL(swr_convert);
  GML_VIDEO_PROVIDER_SYMBOL(swr_free);
  GML_VIDEO_PROVIDER_SYMBOL(swr_get_delay);
  GML_VIDEO_PROVIDER_SYMBOL(swr_init);
  GML_VIDEO_PROVIDER_SYMBOL(swscale_version);
  GML_VIDEO_PROVIDER_SYMBOL(sws_freeContext);
  GML_VIDEO_PROVIDER_SYMBOL(sws_getCachedContext);
  GML_VIDEO_PROVIDER_SYMBOL(sws_getCoefficients);
  GML_VIDEO_PROVIDER_SYMBOL(sws_scale);
  GML_VIDEO_PROVIDER_SYMBOL(sws_setColorspaceDetails);
#undef GML_VIDEO_PROVIDER_SYMBOL
} GmlVideoProviderApi;

extern GmlVideoProviderApi gml_video_provider_api;
#endif

int gml_video_provider_acquire(void);
void gml_video_provider_release(void);
int gml_video_provider_available(void);
const char *gml_video_provider_error(void);

#endif
