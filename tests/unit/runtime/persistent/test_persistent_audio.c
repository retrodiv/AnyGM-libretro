/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_audio.h"
#include "gml_builtin.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void fixture_write_u32(unsigned char *data,size_t off,uint32_t value){
  data[off]=(unsigned char)value;
  data[off+1]=(unsigned char)(value>>8);
  data[off+2]=(unsigned char)(value>>16);
  data[off+3]=(unsigned char)(value>>24);
}
static void fixture_write_u16(unsigned char *data,size_t off,uint16_t value){
  data[off]=(unsigned char)value;
  data[off+1]=(unsigned char)(value>>8);
}


int expect_audio_group_paths(void){
  unsigned char data[80]={0};
  char *strings[]={(char*)"default",(char*)"music",(char*)"effects",(char*)"groups\\music.dat"};
  uint32_t string_offsets[]={48,52,56,60};
  GmlWin win={0};
  win.data=data; win.size=sizeof(data); win.n_chunks=1;
  memcpy(win.chunks[0].name,"AGRP",4);
  win.chunks[0].off=0; win.chunks[0].size=32;
  win.strs=strings; win.str_charoff=string_offsets; win.n_strs=4;
  snprintf(win.content_dir,sizeof win.content_dir,"/bundle");

  /* Legacy records are one word wide. The word after group 1 is group 2's name,
   * not a custom path field. */
  fixture_write_u32(data,0,3);
  fixture_write_u32(data,4,16); fixture_write_u32(data,8,20); fixture_write_u32(data,12,24);
  fixture_write_u32(data,16,48); fixture_write_u32(data,20,52); fixture_write_u32(data,24,56);
  char path[96];
  int ok=gml_audio_group_file_path(&win,1,path,sizeof path) &&
         !strcmp(path,"/bundle/audiogroup1.dat");

  /* New records are two words wide and may carry a platform-neutral relative sidecar path. */
  memset(data,0,sizeof(data));
  win.chunks[0].size=32;
  fixture_write_u32(data,0,2);
  fixture_write_u32(data,4,12); fixture_write_u32(data,8,20);
  fixture_write_u32(data,12,48); fixture_write_u32(data,16,0);
  fixture_write_u32(data,20,52); fixture_write_u32(data,24,60);
  ok=ok && gml_audio_group_file_path(&win,1,path,sizeof path) &&
     !strcmp(path,"/bundle/groups/music.dat");
  if(!ok) fprintf(stderr,"audio-group path layout mismatch: %s\n",path);
  return ok;
}


int expect_audio_group_gain(void){
  GmlWin win={0};
  GmlAudio *audio=gml_audio_create(&win);
  if(!audio) return 0;
  gml_audio_group_gain(audio,1,0.25,0);
  int ok=fabs(gml_audio_group_get_gain(audio,1)-0.25)<1e-12;
  gml_audio_group_gain(audio,1,0.75,10);
  int16_t mixed[882];
  gml_audio_mix(audio,mixed,220);
  double middle=gml_audio_group_get_gain(audio,1);
  gml_audio_mix(audio,mixed,221);
  ok=ok && middle>0.49 && middle<0.51 && fabs(gml_audio_group_get_gain(audio,1)-0.75)<1e-12;

  size_t size=gml_audio_state_size(audio),written=0,used=0;
  void *state=malloc(size?size:1);
  ok=ok && state && gml_audio_state_save(audio,state,size,&written) && written==size;
  void *repeated_state=malloc(size?size:1);
  size_t repeated_written=0;
  ok=ok && repeated_state &&
     gml_audio_state_save(audio,repeated_state,size,&repeated_written) &&
     repeated_written==written && !memcmp(repeated_state,state,written);
  free(repeated_state);
  gml_audio_group_gain(audio,1,1.0,0);
  ok=ok && gml_audio_state_load(audio,state,size,&used) && used==size &&
     fabs(gml_audio_group_get_gain(audio,1)-0.75)<1e-12;
  free(state);
  gml_audio_free(audio);
  if(!ok) fprintf(stderr,"audio-group gain/fade/state fixture failed\n");
  return ok;
}


int expect_dynamic_audio_extension_state(void){
  unsigned char wav[48]={0};
  memcpy(wav,"RIFF",4);
  fixture_write_u32(wav,4,sizeof(wav)-8);
  memcpy(wav+8,"WAVEfmt ",8);
  fixture_write_u32(wav,16,16);
  fixture_write_u16(wav,20,1);
  fixture_write_u16(wav,22,1);
  fixture_write_u32(wav,24,44100);
  fixture_write_u32(wav,28,44100);
  fixture_write_u16(wav,32,1);
  fixture_write_u16(wav,34,8);
  memcpy(wav+36,"data",4);
  fixture_write_u32(wav,40,4);
  wav[44]=255;
  wav[45]=0;
  wav[46]=192;
  wav[47]=64;

  GmlWin win={0};
  GmlAudio *audio=gml_audio_create(&win);
  if(!audio) return 0;
  int sound=gml_audio_add_encoded(audio,wav,sizeof wav);
  int ok=sound>=0 && gml_audio_exists(audio,sound) &&
         fabs(gml_audio_sound_length(audio,sound)-4.0/44100.0)<1e-12;
  gml_audio_sound_set_default_loop(audio,sound,1);
  gml_audio_sound_set_external_type(audio,sound,UINT32_C(0x12345678));
  gml_audio_sound_gain(audio,sound,0.0);
  int voice=gml_audio_play(
      audio,sound,gml_audio_sound_get_default_loop(audio,sound));
  gml_audio_sound_gain_fade(audio,sound,1.0,10);
  int16_t mixed[882];
  gml_audio_mix(audio,mixed,220);
  double middle_gain=gml_audio_sound_get_gain(audio,sound);
  gml_audio_mix(audio,mixed,221);
  int nonzero=0;
  for(size_t index=0;index<sizeof(mixed)/sizeof(mixed[0]);index++)
    if(mixed[index]){ nonzero=1; break; }
  ok=ok && voice>=1000000 && middle_gain>0.49 && middle_gain<0.51 &&
     fabs(gml_audio_sound_get_gain(audio,sound)-1.0)<1e-12 && nonzero;

  gml_audio_sound_gain_fade(audio,sound,0.0,1000);
  gml_audio_mix(audio,mixed,220);
  double saved_gain=gml_audio_sound_get_gain(audio,sound);
  size_t size=gml_audio_state_size(audio),written=0,used=0;
  void *state=malloc(size?size:1);
  ok=ok && state && gml_audio_state_save(audio,state,size,&written) &&
     written==size;
  gml_audio_sound_set_default_loop(audio,sound,0);
  gml_audio_sound_set_external_type(audio,sound,0);
  gml_audio_sound_gain(audio,sound,0.25);
  ok=ok && gml_audio_state_load(audio,state,size,&used) && used==size &&
     gml_audio_sound_get_default_loop(audio,sound)==1 &&
     gml_audio_sound_get_external_type(audio,sound)==UINT32_C(0x12345678) &&
     fabs(gml_audio_sound_get_gain(audio,sound)-saved_gain)<1e-12;
  free(state);
  gml_audio_caster_free(audio,sound);
  ok=ok && !gml_audio_exists(audio,sound);
  gml_audio_free(audio);
  if(!ok) fprintf(stderr,"dynamic loose-audio/state fixture failed\n");
  return ok;
}
