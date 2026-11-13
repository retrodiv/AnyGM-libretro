/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_video_codec.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { int images, audio, nonzero; uint64_t hash; } Observation;
static uint64_t append_hash(uint64_t hash,const void *data,size_t size){
  const uint8_t *bytes=data;
  for(size_t i=0;i<size;i++)hash=(hash^bytes[i])*UINT64_C(1099511628211);
  return hash;
}
static int observe(GmlVideoCodec *codec,Observation *result,int has_audio){
  Observation seen={.hash=UINT64_C(14695981039346656037)};
  double last_image=-1e30,last_audio=-1e30;
  for(int work=0;work<10000;work++){
    GmlVideoDecoded frame;int kind=gml_video_codec_next(codec,&frame);
    if(kind==GML_VIDEO_END){
      *result=seen;
      return seen.images==3 && (has_audio?(seen.audio>10000 && seen.nonzero>1000):
        (!seen.audio && !seen.nonzero)) &&
        gml_video_codec_next(codec,&frame)==GML_VIDEO_END;
    }
    if(kind<0 || !isfinite(frame.time_ms))return 0;
    seen.hash=append_hash(seen.hash,&kind,sizeof kind);
    seen.hash=append_hash(seen.hash,&frame.time_ms,sizeof frame.time_ms);
    if(kind==GML_VIDEO_IMAGE){
      if(frame.width!=16 || frame.height!=16 || frame.time_ms<last_image ||
         fabs(frame.time_ms-seen.images*500.0)>1.0 || !frame.rgba)return 0;
      int value=seen.images==1?255:0;
      for(int pixel=0;pixel<256;pixel++){
        for(int channel=0;channel<3;channel++)
          if(abs(frame.rgba[pixel*4+channel]-value)>3)return 0;
        if(frame.rgba[pixel*4+3]!=255)return 0;
      }
      seen.hash=append_hash(seen.hash,frame.rgba,16*16*4);
      seen.images++;last_image=frame.time_ms;
    }else if(kind==GML_VIDEO_AUDIO){
      if(!frame.pcm || frame.samples<=0 || frame.time_ms+0.01<last_audio)return 0;
      for(int i=0;i<frame.samples*2;i++)if(frame.pcm[i])seen.nonzero++;
      seen.hash=append_hash(seen.hash,frame.pcm,(size_t)frame.samples*4);
      seen.audio+=frame.samples;last_audio=frame.time_ms;
    }else return 0;
  }
  return 0;
}
int main(int argc,char **argv){
  if(argc!=2 && (argc!=3 || strcmp(argv[2],"--silent")))return 2;
  int has_audio=argc==2;
  FILE *file=fopen(argv[1],"rb");
  if(!file || fseek(file,0,SEEK_END))return 2;
  long length=ftell(file);
  if(length<=0 || length>1048576 || fseek(file,0,SEEK_SET)){fclose(file);return 2;}
  uint8_t *bytes=malloc((size_t)length);
  if(!bytes){fclose(file);return 2;}
  int read=fread(bytes,1,(size_t)length,file)==(size_t)length;fclose(file);
  GmlVideoCodec *codec=read?gml_video_codec_open(bytes,(size_t)length):NULL;
  GmlVideoInfo info=gml_video_codec_info(codec);
  int ok=codec && info.width==16 && info.height==16 && info.has_audio==has_audio &&
    (has_audio?(info.duration_ms>=1499 && info.duration_ms<=1600):info.duration_ms==0);
  Observation initial={0},repeat={0};
  if(ok)ok=observe(codec,&initial,has_audio);
  for(int pass=0;pass<2 && ok;pass++)
    ok=gml_video_codec_rewind(codec) && observe(codec,&repeat,has_audio) &&
       initial.images==repeat.images && initial.audio==repeat.audio &&
       initial.nonzero==repeat.nonzero && initial.hash==repeat.hash;
  if(!ok)fprintf(stderr,"video codec: frame/audio/time/replay control failed (error=%d)\n",
                 gml_video_codec_error(codec));
  else printf("video codec: 3 authored frames, %d stereo PCM frames, %d active samples; two exact replays\n",
              initial.audio,initial.nonzero);
  gml_video_codec_close(codec);free(bytes);
  const uint8_t invalid[64]={0};
  codec=gml_video_codec_open(invalid,sizeof invalid);
  if(codec){gml_video_codec_close(codec);ok=0;}
  if(gml_video_codec_open(NULL,0))ok=0;
  gml_video_codec_close(NULL);
  return ok?0:1;
}
