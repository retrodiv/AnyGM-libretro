/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_video_codec.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/avutil.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#if LIBAVCODEC_VERSION_MAJOR < 62 || LIBAVFORMAT_VERSION_MAJOR < 62 || LIBAVUTIL_VERSION_MAJOR < 60 || LIBSWSCALE_VERSION_MAJOR < 9 || LIBSWRESAMPLE_VERSION_MAJOR < 6
#error Video playback requires FFmpeg 8 or later development headers.
#endif

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { VIDEO_MAX_PIXELS=67108864, VIDEO_MAX_AUDIO=1048576, VIDEO_RATE=GML_VIDEO_AUDIO_RATE };
void gml_video_codec_stamp(uint32_t versions[5]){
  versions[0]=avformat_version();versions[1]=avcodec_version();versions[2]=avutil_version();
  versions[3]=swscale_version();versions[4]=swresample_version();
}
struct GmlVideoCodec {
  const uint8_t *bytes; size_t size, position;
  AVFormatContext *format; AVIOContext *io;
  AVCodecContext *decoders[2]; int streams[2];
  AVPacket *packet; AVFrame *frame;
  struct SwsContext *scale; struct SwrContext *resample;
  AVChannelLayout audio_layout;
  int audio_format,audio_rate,frame_pending;
  uint8_t *image, *audio; size_t image_capacity, audio_capacity;
  int receiving, packet_pending, input_done, sent_end[2], finished[2], failed;
  GmlVideoInfo info;
  double origin_ms, audio_next_ms, image_next_ms;
  unsigned selected;
};

static int source_read(void *opaque,uint8_t *out,int count){
  GmlVideoCodec *d=opaque;
  if(count<=0 || d->position>d->size) return AVERROR(EINVAL);
  size_t remaining=d->size-d->position;
  if(!remaining) return AVERROR_EOF;
  if((size_t)count>remaining) count=(int)remaining;
  memcpy(out,d->bytes+d->position,(size_t)count);d->position+=(size_t)count;
  return count;
}
static int64_t source_seek(void *opaque,int64_t offset,int whence){
  GmlVideoCodec *d=opaque;
  if(whence&AVSEEK_SIZE) return (int64_t)d->size;
  whence&=~AVSEEK_FORCE;
  int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?(int64_t)d->position:
    whence==SEEK_END?(int64_t)d->size:-1;
  if(base<0 || offset< -base || offset>(int64_t)d->size-base) return AVERROR(EINVAL);
  d->position=(size_t)(base+offset);return (int64_t)d->position;
}
static int refuse_external_io(AVFormatContext *format,AVIOContext **io,
                              const char *url,int flags,AVDictionary **options){
  (void)format;(void)io;(void)url;(void)flags;(void)options;
  return AVERROR(EPERM);
}
static void release_resources(GmlVideoCodec *d){
  for(int i=0;i<2;i++) avcodec_free_context(&d->decoders[i]);
  av_packet_free(&d->packet);av_frame_free(&d->frame);
  avformat_close_input(&d->format);
  if(d->io){av_freep(&d->io->buffer);avio_context_free(&d->io);}
  sws_freeContext(d->scale);swr_free(&d->resample);
  av_channel_layout_uninit(&d->audio_layout);
  av_free(d->image);av_free(d->audio);
}
void gml_video_codec_close(GmlVideoCodec *d){
  if(d){release_resources(d);free(d);}
}
static int open_stream(GmlVideoCodec *d,int slot,enum AVMediaType type){
  int stream=av_find_best_stream(d->format,type,-1,-1,NULL,0);
  if(stream<0){d->streams[slot]=-1;return slot==1;}
  const AVCodecParameters *parameters=d->format->streams[stream]->codecpar;
  const AVCodec *codec=avcodec_find_decoder(parameters->codec_id);
  AVCodecContext *context=codec?avcodec_alloc_context3(codec):NULL;
  d->decoders[slot]=context;d->streams[slot]=stream;
  if(!context || avcodec_parameters_to_context(context,parameters)<0) return 0;
  context->thread_count=1;context->thread_type=0;context->max_pixels=VIDEO_MAX_PIXELS;
  return avcodec_open2(context,codec,NULL)>=0;
}
GmlVideoCodec *gml_video_codec_open(const uint8_t *bytes,size_t size){
  return gml_video_codec_open_streams(bytes,size,GML_VIDEO_DECODE_IMAGE|GML_VIDEO_DECODE_AUDIO);
}
GmlVideoCodec *gml_video_codec_open_streams(const uint8_t *bytes,size_t size,unsigned selected){
  if(!bytes || !size || size>INT64_MAX || !selected || (selected&~3u)) return NULL;
  GmlVideoCodec *d=calloc(1,sizeof *d);
  if(!d) return NULL;
  d->bytes=bytes;d->size=size;d->selected=selected;d->streams[0]=d->streams[1]=-1;
  uint8_t *buffer=av_malloc(32768);
  if(!buffer) goto fail;
  d->io=avio_alloc_context(buffer,32768,0,d,source_read,NULL,source_seek);
  if(!d->io){av_free(buffer);goto fail;}
  d->format=avformat_alloc_context();
  if(!d->format) goto fail;
  d->format->pb=d->io;d->format->flags|=AVFMT_FLAG_CUSTOM_IO;
  d->format->io_open=refuse_external_io;d->format->max_streams=32;
  d->format->probesize=1048576;d->format->max_analyze_duration=2*AV_TIME_BASE;
  if(avformat_open_input(&d->format,NULL,NULL,NULL)<0) goto fail;
  unsigned streams=d->format->nb_streams;
  AVDictionary **options=calloc(streams?streams:1,sizeof *options);
  if(!options) goto fail;
  for(unsigned i=0;i<streams;i++){
    av_dict_set(&options[i],"threads","1",0);
    av_dict_set(&options[i],"max_pixels","67108864",0);
  }
  int found=avformat_find_stream_info(d->format,options);
  for(unsigned i=0;i<streams;i++) av_dict_free(&options[i]);
  free(options);
  if(found<0 || !open_stream(d,0,AVMEDIA_TYPE_VIDEO) ||
     !open_stream(d,1,AVMEDIA_TYPE_AUDIO)) goto fail;
  AVCodecContext *video=d->decoders[0];
  if(video->width<=0 || video->height<=0 ||
     (int64_t)video->width*video->height>VIDEO_MAX_PIXELS) goto fail;
  d->info.width=video->width;d->info.height=video->height;
  d->info.has_audio=d->decoders[1]!=NULL;
  const AVStream *picture=d->format->streams[d->streams[0]];
  AVRational rate=picture->avg_frame_rate;
  if(rate.num<=0 || rate.den<=0)rate=picture->r_frame_rate;
  d->info.frame_ms=rate.num>0 && rate.den>0?1000.0*rate.den/rate.num:1000.0/30.0;
  if(d->format->duration!=AV_NOPTS_VALUE && d->format->duration>=0)
    d->info.duration_ms=d->format->duration/(AV_TIME_BASE/1000.0);
  if(d->info.duration_ms<=0 && picture->duration>0)
    d->info.duration_ms=picture->duration*av_q2d(picture->time_base)*1000.0;
  if(d->format->start_time!=AV_NOPTS_VALUE)
    d->origin_ms=d->format->start_time/(AV_TIME_BASE/1000.0);
  if(picture->start_time!=AV_NOPTS_VALUE)
    d->origin_ms=picture->start_time*av_q2d(picture->time_base)*1000.0;
  if(!(selected&GML_VIDEO_DECODE_IMAGE)) avcodec_free_context(&d->decoders[0]);
  if(!(selected&GML_VIDEO_DECODE_AUDIO)) avcodec_free_context(&d->decoders[1]);
  d->packet=av_packet_alloc();d->frame=av_frame_alloc();
  if(!d->packet || !d->frame) goto fail;
  return d;
fail:
  gml_video_codec_close(d);return NULL;
}
GmlVideoInfo gml_video_codec_info(const GmlVideoCodec *d){
  return d?d->info:(GmlVideoInfo){0};
}
int gml_video_codec_error(const GmlVideoCodec *d){return d?d->failed:AVERROR(EINVAL);}
static int reserve_bytes(uint8_t **bytes,size_t *capacity,size_t required){
  if(required<=*capacity) return 1;
  uint8_t *next=av_realloc(*bytes,required);
  if(!next) return 0;
  *bytes=next;*capacity=required;return 1;
}
static double frame_time(GmlVideoCodec *d,int slot,double fallback){
  int64_t timestamp=d->frame->best_effort_timestamp;
  if(timestamp==AV_NOPTS_VALUE) timestamp=d->frame->pts;
  if(timestamp==AV_NOPTS_VALUE) return fallback;
  return timestamp*av_q2d(d->format->streams[d->streams[slot]]->time_base)*1000-d->origin_ms;
}
static int convert_image(GmlVideoCodec *d,GmlVideoDecoded *out){
  AVFrame *frame=d->frame;int w=frame->width,h=frame->height;
  if(w<=0 || h<=0 || (int64_t)w*h>VIDEO_MAX_PIXELS || w>INT_MAX/4) return 0;
  size_t bytes=(size_t)w*(size_t)h*4u;
  if(!reserve_bytes(&d->image,&d->image_capacity,bytes)) return 0;
  d->scale=sws_getCachedContext(d->scale,w,h,frame->format,w,h,AV_PIX_FMT_RGBA,
                               SWS_BILINEAR|SWS_BITEXACT,NULL,NULL,NULL);
  if(!d->scale) return 0;
  int matrix=frame->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:
    frame->colorspace==AVCOL_SPC_BT2020_NCL?SWS_CS_BT2020:SWS_CS_DEFAULT;
  const int *coefficients=sws_getCoefficients(matrix);
  if(sws_setColorspaceDetails(d->scale,coefficients,frame->color_range==AVCOL_RANGE_JPEG,
                              coefficients,1,0,1<<16,1<<16)<0) return 0;
  uint8_t *planes[4]={d->image};int strides[4]={w*4};
  if(sws_scale(d->scale,(const uint8_t *const *)frame->data,frame->linesize,0,h,
               planes,strides)!=h) return 0;
  out->time_ms=frame_time(d,0,d->image_next_ms);
  double duration=frame->duration>0?
    frame->duration*av_q2d(d->format->streams[d->streams[0]]->time_base)*1000.0:
    d->info.frame_ms;
  out->duration_ms=duration;d->image_next_ms=out->time_ms+duration;
  out->rgba=d->image;out->width=w;out->height=h;
  return 1;
}
static int audio_buffer(GmlVideoCodec *d,int samples){
  return samples>0 && samples<=VIDEO_MAX_AUDIO &&
    reserve_bytes(&d->audio,&d->audio_capacity,(size_t)samples*4u);
}
static int drain_audio(GmlVideoCodec *d,GmlVideoDecoded *out){
  if(!d->resample) return 0;
  if(!audio_buffer(d,4096)) return -1;
  uint8_t *planes[1]={d->audio};
  int samples=swr_convert(d->resample,planes,4096,NULL,0);
  if(samples<0) return -1;
  if(samples){
    out->time_ms=d->audio_next_ms;out->pcm=(const int16_t *)d->audio;out->samples=samples;
    d->audio_next_ms+=samples*1000.0/VIDEO_RATE;
  }
  return samples>0;
}
static int convert_audio(GmlVideoCodec *d,GmlVideoDecoded *out){
  AVFrame *frame=d->frame;
  if(frame->sample_rate<=0 || frame->sample_rate>768000 || frame->nb_samples<0 ||
     frame->nb_samples>VIDEO_MAX_AUDIO || frame->ch_layout.nb_channels<=0 ||
     frame->ch_layout.nb_channels>64 || !av_channel_layout_check(&frame->ch_layout) ||
     av_get_bytes_per_sample(frame->format)<=0) return 0;
  if(d->resample && (d->audio_format!=frame->format || d->audio_rate!=frame->sample_rate ||
                    av_channel_layout_compare(&d->audio_layout,&frame->ch_layout)!=0)){
    /* Keep the received frame alive until every old-format delayed sample is delivered. */
    int drained=drain_audio(d,out);
    if(drained) return drained>0?2:0;
    swr_free(&d->resample);av_channel_layout_uninit(&d->audio_layout);
  }
  if(!d->resample){
    AVChannelLayout stereo=AV_CHANNEL_LAYOUT_STEREO;
    if(swr_alloc_set_opts2(&d->resample,&stereo,AV_SAMPLE_FMT_S16,VIDEO_RATE,
                          &frame->ch_layout,frame->format,frame->sample_rate,0,NULL)<0 ||
       swr_init(d->resample)<0 || av_channel_layout_copy(&d->audio_layout,&frame->ch_layout)<0)
      return 0;
    d->audio_format=frame->format;d->audio_rate=frame->sample_rate;
  }
  int64_t delay=swr_get_delay(d->resample,frame->sample_rate);
  int64_t count=av_rescale_rnd(delay+frame->nb_samples,VIDEO_RATE,frame->sample_rate,AV_ROUND_UP);
  if(count<=0 || count>VIDEO_MAX_AUDIO || !audio_buffer(d,(int)count)) return 0;
  uint8_t *planes[1]={d->audio};
  int samples=swr_convert(d->resample,planes,(int)count,
                          (const uint8_t **)frame->extended_data,frame->nb_samples);
  if(samples<0) return 0;
  double delay_ms=delay*1000.0/frame->sample_rate;
  out->time_ms=frame_time(d,1,d->audio_next_ms+delay_ms)-delay_ms;
  out->pcm=(const int16_t *)d->audio;out->samples=samples;
  d->audio_next_ms=out->time_ms+samples*1000.0/VIDEO_RATE;
  return 1;
}
int gml_video_codec_next(GmlVideoCodec *d,GmlVideoDecoded *out){
  if(!d || !out || d->failed) return GML_VIDEO_ERROR;
  memset(out,0,sizeof *out);
  for(unsigned work=0;work<65536;work++){
    if(d->receiving){
      int slot=d->receiving-1;
      int result=d->frame_pending?0:avcodec_receive_frame(d->decoders[slot],d->frame);
      if(result>=0){
        int ok=slot==0?convert_image(d,out):convert_audio(d,out);
        d->frame_pending=ok==2;
        if(!d->frame_pending) av_frame_unref(d->frame);
        if(!ok){d->failed=AVERROR_INVALIDDATA;break;}
        if(slot==0 || out->samples) return slot==0?GML_VIDEO_IMAGE:GML_VIDEO_AUDIO;
        continue;
      }
      d->receiving=0;
      if(result==AVERROR_EOF)d->finished[slot]=1;
      else if(result!=AVERROR(EAGAIN)){d->failed=result;break;}
    }
    if(d->packet_pending){
      int slot=d->packet->stream_index==d->streams[0]?0:1;
      int result=avcodec_send_packet(d->decoders[slot],d->packet);
      d->receiving=slot+1;
      if(result==AVERROR(EAGAIN))continue;
      av_packet_unref(d->packet);d->packet_pending=0;
      if(result<0){d->failed=result;break;}
      continue;
    }
    if(!d->input_done){
      int result=av_read_frame(d->format,d->packet);
      if(result>=0){
        if((d->decoders[0] && d->packet->stream_index==d->streams[0]) ||
           (d->decoders[1] && d->packet->stream_index==d->streams[1]))d->packet_pending=1;
        else av_packet_unref(d->packet);
        continue;
      }
      if(result!=AVERROR_EOF){d->failed=result;break;}
      d->input_done=1;
    }
    int drain=-1;
    for(int i=0;i<2;i++)if(d->decoders[i] && !d->finished[i]){drain=i;break;}
    if(drain>=0){
      if(!d->sent_end[drain]){
        int result=avcodec_send_packet(d->decoders[drain],NULL);
        if(result==AVERROR_EOF){d->finished[drain]=1;continue;}
        if(result<0 && result!=AVERROR(EAGAIN)){d->failed=result;break;}
        if(result>=0)d->sent_end[drain]=1;
      }
      d->receiving=drain+1;continue;
    }
    int drained=drain_audio(d,out);
    if(drained<0){d->failed=AVERROR_INVALIDDATA;break;}
    if(drained) return GML_VIDEO_AUDIO;
    return GML_VIDEO_END;
  }
  if(!d->failed)d->failed=AVERROR_INVALIDDATA;
  return GML_VIDEO_ERROR;
}
int gml_video_codec_rewind(GmlVideoCodec *d){
  if(!d)return 0;
  GmlVideoCodec *fresh=gml_video_codec_open_streams(d->bytes,d->size,d->selected);
  if(!fresh)return 0;
  release_resources(d);*d=*fresh;d->io->opaque=d;free(fresh);
  return 1;
}
