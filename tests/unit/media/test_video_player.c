/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_video_player.h"
#include "gml_audio.h"
#include "gml_render_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition,label) do { if(!(condition)){ \
  fprintf(stderr,"video player: %s (line %d)\n",label,__LINE__);return 1; \
} } while(0)

static int has_sample(const int16_t *pcm,int frames){
  for(int i=0;i<frames*2;i++) if(pcm[i]) return 1;
  return 0;
}
static int picture(GmlVideoPlayer *player,GmlRender *render,int value){
  int surface=gml_video_player_surface(player),width=0,height=0;
  const uint32_t *pixels=gml_surface_pixels_read(render,surface,&width,&height);
  if(!pixels || width!=16 || height!=16) return 0;
  for(int i=0;i<256;i++){
    if((pixels[i]>>24)!=255) return 0;
    for(int shift=0;shift<24;shift+=8)
      if(abs((int)((pixels[i]>>shift)&255u)-value)>3) return 0;
  }
  return 1;
}
static int queues(GmlAudio *audio){
  int16_t pcm[512],first[512],second[512];
  for(int i=0;i<512;i++) pcm[i]=(int16_t)(1000+i);
  int queue=gml_audio_queue_create_internal(audio,4,44100,1);
  REQUIRE(queue>=0 && gml_audio_queue_submit(audio,queue,99,(uint8_t *)pcm,sizeof pcm),
          "internal queue submission");
  REQUIRE(gml_audio_play(audio,queue,0)>=0,"internal voice");
  size_t size=gml_audio_state_size(audio),written=0,used=0;
  uint8_t *state=malloc(size),*after=malloc(size);
  REQUIRE(state && after && gml_audio_state_save(audio,state,size,&written) && written==size,
          "internal queue snapshot");
  gml_audio_mix(audio,first,256);
  REQUIRE(has_sample(first,256) && !gml_audio_queue_event_count(audio),
          "consumed internal buffers do not publish events");
  REQUIRE(gml_audio_state_load(audio,state,size,&used) && used==size,"internal queue restore");
  gml_audio_mix(audio,second,256);
  REQUIRE(!memcmp(first,second,sizeof first) && !gml_audio_queue_event_count(audio),
          "restored internal queue retains PCM and event policy");
  REQUIRE(gml_audio_state_save(audio,state,size,&written),"post-consumption snapshot");
  state[4]=5;
  REQUIRE(!gml_audio_state_load(audio,state,size,&used),"older mixer state rejected");
  state[4]=6;
  REQUIRE(gml_audio_state_save(audio,after,size,&written) && !memcmp(state,after,written),
          "older state rejection leaves mixer untouched");
  gml_audio_queue_free(audio,queue);
  REQUIRE(!gml_audio_queue_event_count(audio),"internal shutdown stays private");
  free(state);free(after);
  queue=gml_audio_queue_create(audio,4,44100,1);
  REQUIRE(queue>=0 && gml_audio_queue_submit(audio,queue,99,(uint8_t *)pcm,sizeof pcm) &&
          gml_audio_play(audio,queue,0)>=0,"public queue control");
  gml_audio_mix(audio,first,256);
  int event_queue=-1,buffer=-1,shutdown=-1;
  REQUIRE(gml_audio_queue_event_pop(audio,&event_queue,&buffer,&shutdown) &&
          event_queue==queue && buffer==99 && !shutdown,"public completion preserved");
  gml_audio_queue_free(audio,queue);
  REQUIRE(gml_audio_queue_event_pop(audio,&event_queue,&buffer,&shutdown) && shutdown &&
          !gml_audio_queue_event_count(audio),"public shutdown preserved");
  return 0;
}
int main(int argc,char **argv){
  REQUIRE(argc==2 || (argc==3 && !strcmp(argv[2],"--silent")),"input arguments");
  int audible=argc==2;
  FILE *file=fopen(argv[1],"rb");
  REQUIRE(file && !fseek(file,0,SEEK_END),"authored input opens");
  long length=ftell(file);
  REQUIRE(length>0 && length<=1048576 && !fseek(file,0,SEEK_SET),"bounded input");
  uint8_t *bytes=malloc((size_t)length);
  REQUIRE(bytes && fread(bytes,1,(size_t)length,file)==(size_t)length,"input bytes");
  fclose(file);
  GmlWin content={0};GmlRender render={0};
  REQUIRE(!gml_render_init(&render,&content),"existing renderer initializes");
  GmlAudio *audio=gml_audio_create(&content);
  REQUIRE(audio && !queues(audio),"existing mixer initializes and transports queues");
  GmlVideoPlayer *player=gml_video_player_open(bytes,(size_t)length,&render,audio);
  REQUIRE(player && gml_video_player_status(player)==GML_VIDEO_PREPARING,"preparing");
  REQUIRE(gml_video_player_surface(player)==-1,"no premature surface");
  gml_video_player_tick(player,0);
  REQUIRE(gml_video_player_status(player)==GML_VIDEO_PLAYING && picture(player,&render,0) &&
          gml_video_player_notifications(player)==GML_VIDEO_NOTIFY_START,"first frame and start");
  int16_t pcm[8820];
  gml_audio_mix(audio,pcm,4410);
  REQUIRE(has_sample(pcm,4410)==audible,"real decoded audio reaches existing mixer");
  gml_video_player_tick(player,100);
  gml_video_player_pause(player,1);
  double position=gml_video_player_position(player);
  gml_video_player_tick(player,500);
  gml_audio_mix(audio,pcm,4410);
  REQUIRE(gml_video_player_status(player)==GML_VIDEO_PAUSED &&
          gml_video_player_position(player)==position && !has_sample(pcm,4410),
          "pause freezes clock and mixer voice");
  gml_video_player_pause(player,0);
  for(int i=0;i<4;i++){
    gml_audio_mix(audio,pcm,4410);gml_video_player_tick(player,100);
  }
  REQUIRE(fabs(gml_video_player_position(player)-500)<0.001 && picture(player,&render,255),
          "resumed 500ms frame");
  gml_video_player_volume(player,0);
  gml_audio_mix(audio,pcm,4410);
  REQUIRE(!has_sample(pcm,4410),"zero gain silences actual video audio");
  gml_video_player_volume(player,1);
  gml_video_player_tick(player,100);
  gml_audio_mix(audio,pcm,4410);
  REQUIRE(has_sample(pcm,4410)==audible,"gain restored");
  gml_surface_free(&render,gml_video_player_surface(player));
  REQUIRE(picture(player,&render,255),"game-freed video surface is recreated");
  gml_video_player_loop(player,1);
  for(int i=0;i<10;i++){
    gml_video_player_tick(player,100);gml_audio_mix(audio,pcm,4410);
  }
  REQUIRE(gml_video_player_status(player)==GML_VIDEO_PLAYING &&
          gml_video_player_position(player)<200 && picture(player,&render,0) &&
          !gml_video_player_notifications(player),"loop restarts without an end event");
  gml_video_player_loop(player,0);
  for(int i=0;i<16;i++){
    gml_video_player_tick(player,100);gml_audio_mix(audio,pcm,4410);
  }
  REQUIRE(gml_video_player_status(player)==GML_VIDEO_CLOSED &&
          gml_video_player_surface(player)==-2 &&
          gml_video_player_notifications(player)==GML_VIDEO_NOTIFY_END &&
          !gml_video_player_notifications(player),"non-looping end is delivered once");
  gml_video_player_close(player);
  REQUIRE(!gml_audio_queue_event_count(audio),"video never emits public queue events");
  gml_audio_free(audio);gml_render_free(&render);free(bytes);
  puts("video player: timed RGBA, real mixer, pause, gain, loop, end, queue state pass");
  return 0;
}
