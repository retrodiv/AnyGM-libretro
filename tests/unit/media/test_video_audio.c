/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Exercise the private decoded-frame boundary with authored PCM; no encoder or media file. */
#include "../../../src/media/gml_video_codec.c"

static int feed(GmlVideoCodec *codec,int format,int channels,int rate,int samples,
                int left,int right,int *total){
  AVFrame *frame=codec->frame;
  av_frame_unref(frame);frame->format=format;frame->sample_rate=rate;frame->nb_samples=samples;
  frame->pts=frame->best_effort_timestamp=AV_NOPTS_VALUE;
  av_channel_layout_default(&frame->ch_layout,channels);
  if(av_frame_get_buffer(frame,0)<0) return 0;
  if(format==AV_SAMPLE_FMT_S16){
    int16_t *pcm=(int16_t *)frame->data[0];
    for(int i=0;i<samples;i++) for(int channel=0;channel<channels;channel++)
      pcm[i*channels+channel]=(int16_t)(channel?right:left);
  }else{
    for(int channel=0;channel<channels;channel++){
      float *pcm=(float *)frame->data[channel];
      for(int i=0;i<samples;i++) pcm[i]=(float)(channel?right:left)/32768.0f;
    }
  }
  codec->receiving=2;codec->frame_pending=1;
  for(int work=0;work<32;work++){
    GmlVideoDecoded out={0};int result=gml_video_codec_next(codec,&out);
    if(result!=GML_VIDEO_AUDIO || fabs(out.time_ms-*total*1000.0/VIDEO_RATE)>0.00001) return 0;
    if(!codec->frame_pending && rate==VIDEO_RATE && channels==2){
      if(out.samples!=samples) return 0;
      for(int i=0;i<out.samples;i++)
        if(out.pcm[i*2]!=left || out.pcm[i*2+1]!=right) return 0;
    }
    *total+=out.samples;
    if(!codec->frame_pending) return 1;
  }
  return 0;
}

int main(void){
  GmlVideoCodec codec={0};codec.frame=av_frame_alloc();int ok=0,total=0;
  if(!codec.frame) return 1;
  if(!feed(&codec,AV_SAMPLE_FMT_S16,2,44100,4410,2048,-1024,&total)){
    fputs("video audio: initial signed stereo control failed\n",stderr);goto done;
  }
  if(!feed(&codec,AV_SAMPLE_FMT_FLTP,2,44100,4410,8192,-4096,&total)){
    fputs("video audio: changed planar input must retain signed stereo values\n",stderr);goto done;
  }
  if(!feed(&codec,AV_SAMPLE_FMT_S16,1,22050,2205,4096,4096,&total) ||
     !feed(&codec,AV_SAMPLE_FMT_S16,1,22050,2205,4096,4096,&total)){
    fputs("video audio: mono/rate change or untimestamped continuation failed\n",stderr);goto done;
  }
  if(!feed(&codec,AV_SAMPLE_FMT_S16,2,48000,4800,1200,-600,&total)){
    fputs("video audio: delayed samples must drain before changing stereo rate\n",stderr);goto done;
  }
  codec.input_done=1;codec.receiving=0;
  for(int work=0;work<32;work++){
    GmlVideoDecoded out={0};int result=gml_video_codec_next(&codec,&out);
    if(result==GML_VIDEO_END){ok=total==22050;break;}
    if(result!=GML_VIDEO_AUDIO || fabs(out.time_ms-total*1000.0/VIDEO_RATE)>0.00001) break;
    total+=out.samples;
  }
  if(!ok) fprintf(stderr,"video audio: expected 22050 continuous output frames, got %d\n",total);
done:
  release_resources(&codec);
  if(ok) puts("video audio: packed/planar, stereo/mono, three rates, delayed tails and missing timestamps pass");
  return ok?0:1;
}
