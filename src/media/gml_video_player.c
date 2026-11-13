/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_video_player.h"
#include "gml_audio.h"
#include "gml_render.h"
#include "gml_render_state.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct GmlVideoPlayer {
  GmlVideoCodec *picture,*sound;
  GmlRender *render;
  GmlAudio *audio;
  GmlVideoInfo info;
  GmlVideoDecoded next_picture,next_sound;
  uint8_t *image;
  size_t image_size;
  int width,height,surface,queue,voice;
  int picture_pending,sound_pending,picture_done,sound_done;
  int ready,paused,loop,ended,failed,clock_started;
  int64_t audio_cursor;
  double position,volume,image_end;
  unsigned notifications;
  uint32_t picture_reads,sound_reads;
  int resources_owned;
};

static void close_queue(GmlVideoPlayer *p){
  if(p->resources_owned && p->queue>=0) gml_audio_queue_free(p->audio,p->queue);
  p->queue=p->voice=-1;
}
void gml_video_player_close(GmlVideoPlayer *p){
  if(!p) return;
  close_queue(p);
  if(p->resources_owned && p->surface>=0) gml_surface_free(p->render,p->surface);
  gml_video_codec_close(p->picture);gml_video_codec_close(p->sound);
  free(p->image);free(p);
}
static GmlVideoPlayer *player_create(const uint8_t *bytes,size_t size,
                                     GmlRender *render,GmlAudio *audio,int sound,int own){
  if(!render || !bytes || !size) return NULL;
  GmlVideoPlayer *p=calloc(1,sizeof *p);
  if(!p) return NULL;
  p->render=render;p->audio=audio;p->surface=p->queue=p->voice=-1;p->volume=1;
  p->resources_owned=own;
  p->picture=gml_video_codec_open_streams(bytes,size,GML_VIDEO_DECODE_IMAGE);
  if(!p->picture) goto fail;
  p->info=gml_video_codec_info(p->picture);
  if(sound>0 && (!p->info.has_audio || !audio)) goto fail;
  if(sound!=0 && p->info.has_audio && audio){
    p->sound=gml_video_codec_open_streams(bytes,size,GML_VIDEO_DECODE_AUDIO);
    if(own) p->queue=gml_audio_queue_create_internal(audio,4,GML_VIDEO_AUDIO_RATE,1);
    if(!p->sound || (own && p->queue<0)) goto fail;
  }else p->sound_done=1;
  return p;
fail:
  gml_video_player_close(p);return NULL;
}
GmlVideoPlayer *gml_video_player_open(const uint8_t *bytes,size_t size,
                                     GmlRender *render,GmlAudio *audio){
  return player_create(bytes,size,render,audio,-1,1);
}
static int upload(GmlVideoPlayer *p){
  if(!p->image || !p->render) return -1;
  if(p->surface>=0 && (!gml_surface_exists(p->render,p->surface) ||
     gml_surface_width(p->render,p->surface)!=p->width ||
     gml_surface_height(p->render,p->surface)!=p->height)){
    gml_surface_free(p->render,p->surface);p->surface=-1;
  }
  if(p->surface<0) p->surface=gml_surface_create(p->render,p->width,p->height);
  if(p->surface<0 || !gml_surface_write_rgba8(p->render,p->surface,p->image,p->image_size))
    return -1;
  return p->surface;
}
static int retain_picture(GmlVideoPlayer *p,const GmlVideoDecoded *next){
  size_t size=(size_t)next->width*(size_t)next->height*4u;
  if(size!=p->image_size){
    uint8_t *image=realloc(p->image,size);
    if(!image) return 0;
    p->image=image;p->image_size=size;
  }
  memcpy(p->image,next->rgba,size);p->width=next->width;p->height=next->height;
  p->image_end=fmax(p->image_end,next->time_ms+next->duration_ms);
  return 1;
}
static int present(GmlVideoPlayer *p){
  for(unsigned work=0;work<65536;work++){
    if(!p->picture_pending && !p->picture_done){
      int result=gml_video_codec_next(p->picture,&p->next_picture);
      if(result==GML_VIDEO_END) p->picture_done=1;
      else if(result!=GML_VIDEO_IMAGE) return 0;
      else {
        if(p->picture_reads==UINT32_MAX) return 0;
        p->picture_reads++;p->picture_pending=1;
      }
    }
    if(!p->picture_pending || p->next_picture.time_ms>p->position+0.000001) return 1;
    if(!retain_picture(p,&p->next_picture)) return 0;
    p->picture_pending=0;
    if(!p->ready){p->ready=1;p->notifications|=GML_VIDEO_NOTIFY_START;}
  }
  return 0;
}
static int64_t sample_at(double ms){
  double sample=ms*(GML_VIDEO_AUDIO_RATE/1000.0);
  if(!isfinite(sample) || sample<(double)INT64_MIN || sample>=(double)INT64_MAX)
    return INT64_MIN;
  return (int64_t)llround(sample);
}
static int submit(GmlVideoPlayer *p,const int16_t *pcm,int frames){
  /* The decoder returns native-endian samples; GML's existing PCM queue consumes little endian. */
  uint8_t bytes[4096];
  while(frames>0){
    int count=frames>1024?1024:frames;
    for(int i=0;i<count*2;i++){
      uint16_t sample=(uint16_t)pcm[i];
      bytes[i*2]=(uint8_t)sample;bytes[i*2+1]=(uint8_t)(sample>>8);
    }
    if(!gml_audio_queue_submit(p->audio,p->queue,0,bytes,(size_t)count*4u)) return 0;
    p->audio_cursor+=count;pcm+=count*2;frames-=count;
  }
  return 1;
}
static int supply_sound(GmlVideoPlayer *p){
  if(!p->sound || p->queue<0) return 1;
  int64_t target=sample_at(p->position+250.0);
  int64_t limit=p->info.duration_ms>0?sample_at(p->info.duration_ms):INT64_MAX;
  if(target<0 || limit<0) return 0;
  if(target>limit) target=limit;
  static const int16_t silence[1024]={0};
  for(unsigned work=0;work<65536 && p->audio_cursor<target;work++){
    if(!p->sound_pending && !p->sound_done){
      int result=gml_video_codec_next(p->sound,&p->next_sound);
      if(result==GML_VIDEO_END) p->sound_done=1;
      else if(result!=GML_VIDEO_AUDIO) return 0;
      else {
        if(p->sound_reads==UINT32_MAX) return 0;
        p->sound_reads++;p->sound_pending=1;
      }
    }
    if(!p->sound_pending) break;
    int64_t start=sample_at(p->next_sound.time_ms);
    if(start==INT64_MIN || start>INT64_MAX-p->next_sound.samples) return 0;
    if(start>p->audio_cursor){
      int64_t gap=start-p->audio_cursor;
      if(gap>target-p->audio_cursor) gap=target-p->audio_cursor;
      if(gap>512) gap=512;
      if(!submit(p,silence,(int)gap)) return 0;
      continue;
    }
    int64_t offset=p->audio_cursor-start;
    if(offset>=p->next_sound.samples){p->sound_pending=0;continue;}
    int64_t count=p->next_sound.samples-offset;
    if(count>target-p->audio_cursor) count=target-p->audio_cursor;
    if(!submit(p,p->next_sound.pcm+offset*2,(int)count)) return 0;
    if(offset+count==p->next_sound.samples) p->sound_pending=0;
  }
  if(p->audio_cursor>0 && !gml_audio_is_playing(p->audio,p->voice)){
    p->voice=gml_audio_play(p->audio,p->queue,0);
    if(p->voice<0) return 0;
    gml_audio_sound_gain(p->audio,p->voice,p->volume);
    gml_audio_pause_sound(p->audio,p->voice,p->paused);
  }
  return 1;
}
static int restart(GmlVideoPlayer *p,double position){
  if(!gml_video_codec_rewind(p->picture) ||
     (p->sound && !gml_video_codec_rewind(p->sound))) return 0;
  close_queue(p);
  if(p->sound){
    p->queue=gml_audio_queue_create_internal(p->audio,4,GML_VIDEO_AUDIO_RATE,1);
    if(p->queue<0) return 0;
  }
  p->picture_pending=p->sound_pending=p->picture_done=0;
  p->picture_reads=p->sound_reads=0;
  p->sound_done=p->sound==NULL;p->audio_cursor=sample_at(position);
  p->image_end=0;p->position=position;
  return present(p) && supply_sound(p);
}
void gml_video_player_tick(GmlVideoPlayer *p,double milliseconds){
  if(!p || p->failed || p->ended || p->paused || !isfinite(milliseconds) || milliseconds<0)
    return;
  if(p->clock_started) p->position+=milliseconds;
  p->clock_started=1;
  if(!isfinite(p->position) || !present(p) || !supply_sound(p)) goto fail;
  if(!p->ready){if(p->picture_done) goto fail;return;}
  double end=p->info.duration_ms>0?p->info.duration_ms:p->image_end;
  if(p->picture_done && end>0 && p->position>=end){
    if(p->loop){
      if(!restart(p,fmod(p->position,end))) goto fail;
    }else{
      p->position=end;p->ended=1;p->notifications|=GML_VIDEO_NOTIFY_END;
      close_queue(p);
    }
  }
  return;
fail:
  p->failed=1;close_queue(p);
}
void gml_video_player_pause(GmlVideoPlayer *p,int paused){
  if(!p || p->ended || p->failed) return;
  p->paused=paused!=0;
  if(p->voice>=0) gml_audio_pause_sound(p->audio,p->voice,p->paused);
}
void gml_video_player_loop(GmlVideoPlayer *p,int enabled){if(p)p->loop=enabled!=0;}
void gml_video_player_volume(GmlVideoPlayer *p,double volume){
  if(!p || !isfinite(volume)) return;
  p->volume=fmax(0,fmin(1,volume));
  if(p->voice>=0) gml_audio_sound_gain(p->audio,p->voice,p->volume);
}
int gml_video_player_status(const GmlVideoPlayer *p){
  if(!p || p->failed || p->ended) return GML_VIDEO_CLOSED;
  if(p->paused) return GML_VIDEO_PAUSED;
  return p->ready?GML_VIDEO_PLAYING:GML_VIDEO_PREPARING;
}
double gml_video_player_position(const GmlVideoPlayer *p){return p?p->position:0;}
double gml_video_player_duration(const GmlVideoPlayer *p){return p?p->info.duration_ms:0;}
int gml_video_player_surface(GmlVideoPlayer *p){
  if(!p || p->failed || !p->ready) return -1;
  if(p->ended) return -2;
  return upload(p);
}
unsigned gml_video_player_notifications(GmlVideoPlayer *p){
  unsigned notices=p?p->notifications:0;
  if(p) p->notifications=0;
  return notices;
}

void gml_video_player_rebind(GmlVideoPlayer *p,GmlRender *render,GmlAudio *audio){
  if(p){p->render=render;p->audio=audio;}
}
size_t gml_video_player_state_capacity_hint(const GmlVideoPlayer *p){
  if(!p || p->failed || p->ended) return 0;
  int width=p->ready?p->width:p->info.width,height=p->ready?p->height:p->info.height;
  size_t pixels=gml_render_surface_state_capacity(width,height);
  size_t sound=p->sound?65536u:0u;
  return pixels>SIZE_MAX-sound?SIZE_MAX:pixels+sound;
}
int gml_video_player_state_get(const GmlVideoPlayer *p,GmlVideoPlayerState *s){
  if(!p || !s) return 0;
  memset(s,0,sizeof *s);gml_video_codec_stamp(s->provider);
  s->picture_reads=p->picture_reads;s->sound_reads=p->sound_reads;
  s->flags=(unsigned)p->ready | (unsigned)p->paused<<1 | (unsigned)p->loop<<2 |
    (unsigned)p->ended<<3 | (unsigned)p->failed<<4 | (unsigned)p->clock_started<<5 |
    (unsigned)p->picture_pending<<6 | (unsigned)p->sound_pending<<7 |
    (unsigned)p->picture_done<<8 | (unsigned)p->sound_done<<9 | (p->sound?1u<<10:0);
  s->notifications=p->notifications;s->surface=p->surface;s->queue=p->queue;s->voice=p->voice;
  s->audio_cursor=(uint64_t)p->audio_cursor;s->position=p->position;
  s->volume=p->volume;s->image_end=p->image_end;
  return 1;
}
GmlVideoPlayer *gml_video_player_state_restore(const uint8_t *bytes,size_t size,
                          GmlRender *render,GmlAudio *audio,const GmlVideoPlayerState *s){
  uint32_t provider[5];gml_video_codec_stamp(provider);
  if(!s || memcmp(provider,s->provider,sizeof provider) || (s->flags&~2047u) ||
     (s->notifications&~3u) || !isfinite(s->position) || s->position<0 ||
     !isfinite(s->image_end) || s->image_end<0 || !isfinite(s->volume) ||
     s->volume<0 || s->volume>1 || s->audio_cursor>INT64_MAX || s->surface< -1 ||
     s->surface==0 || s->surface>GML_MAX_SURFACES || s->queue< -1 ||
     (s->voice!= -1 && s->voice<1000000) || (s->queue<0 && s->voice!= -1)) return NULL;
  GmlVideoPlayer *p=player_create(bytes,size,render,audio,(s->flags>>10)&1u,0);
  if(!p) return NULL;
  p->ready=s->flags&1u;p->paused=(s->flags>>1)&1u;p->loop=(s->flags>>2)&1u;
  p->ended=(s->flags>>3)&1u;p->failed=(s->flags>>4)&1u;p->clock_started=(s->flags>>5)&1u;
  p->picture_pending=(s->flags>>6)&1u;p->sound_pending=(s->flags>>7)&1u;
  p->picture_done=(s->flags>>8)&1u;p->sound_done=(s->flags>>9)&1u;
  if((p->picture_pending && (!s->picture_reads || p->picture_done)) ||
     (p->sound_pending && (!s->sound_reads || p->sound_done)) ||
     (!p->sound && (s->sound_reads || p->sound_pending || !p->sound_done || s->queue>=0)) ||
     (!p->ready && s->surface>=0)) goto fail;
  uint32_t shown=s->picture_reads-(unsigned)p->picture_pending;
  for(uint32_t i=0;i<s->picture_reads;i++){
    if(gml_video_codec_next(p->picture,&p->next_picture)!=GML_VIDEO_IMAGE) goto fail;
    if(i<shown){
      p->image_end=fmax(p->image_end,p->next_picture.time_ms+p->next_picture.duration_ms);
      if(i+1==shown && !retain_picture(p,&p->next_picture)) goto fail;
    }
  }
  if(p->picture_done && gml_video_codec_next(p->picture,&p->next_picture)!=GML_VIDEO_END) goto fail;
  for(uint32_t i=0;i<s->sound_reads;i++)
    if(!p->sound || gml_video_codec_next(p->sound,&p->next_sound)!=GML_VIDEO_AUDIO) goto fail;
  if(p->sound && p->sound_done && gml_video_codec_next(p->sound,&p->next_sound)!=GML_VIDEO_END)
    goto fail;
  if(p->ready!=(p->image!=NULL) || p->image_end!=s->image_end) goto fail;
  p->picture_reads=s->picture_reads;p->sound_reads=s->sound_reads;
  p->notifications=s->notifications;p->surface=s->surface;p->queue=s->queue;p->voice=s->voice;
  p->audio_cursor=(int64_t)s->audio_cursor;p->position=s->position;p->volume=s->volume;
  return p;
fail:
  gml_video_player_close(p);return NULL;
}
int gml_video_player_state_adopt(GmlVideoPlayer *p){
  if(!p) return 1;
  if(p->queue>=0 && (!gml_audio_queue_is_internal_pcm16_stereo(p->audio,p->queue,
                                                            GML_VIDEO_AUDIO_RATE) ||
     (gml_audio_is_playing(p->audio,p->voice) &&
      gml_audio_voice_sound(p->audio,p->voice)!=p->queue))) return 0;
  p->resources_owned=1;return 1;
}
