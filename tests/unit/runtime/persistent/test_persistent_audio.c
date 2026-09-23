/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_audio.h"
#include "gml_wwise.h"
#include "gml_builtin.h"
#include "stdio_vfs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


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
static size_t fixture_pcm16_wav(unsigned char *out,size_t cap,int samples,int16_t value){
  size_t bytes=44u+(size_t)samples*2u;
  if(!out || samples<=0 || bytes>cap) return 0;
  memset(out,0,bytes);
  memcpy(out,"RIFF",4);
  fixture_write_u32(out,4,(uint32_t)bytes-8u);
  memcpy(out+8,"WAVEfmt ",8);
  fixture_write_u32(out,16,16);
  fixture_write_u16(out,20,1);
  fixture_write_u16(out,22,1);
  fixture_write_u32(out,24,8000);
  fixture_write_u32(out,28,16000);
  fixture_write_u16(out,32,2);
  fixture_write_u16(out,34,16);
  memcpy(out+36,"data",4);
  fixture_write_u32(out,40,(uint32_t)samples*2u);
  for(int sample=0;sample<samples;sample++) fixture_write_u16(out,44u+(size_t)sample*2u,(uint16_t)value);
  return bytes;
}
static int fixture_base64_value(unsigned char byte){
  if(byte>='A' && byte<='Z') return byte-'A';
  if(byte>='a' && byte<='z') return byte-'a'+26;
  if(byte>='0' && byte<='9') return byte-'0'+52;
  return byte=='+'?62:byte=='/'?63:-1;
}
static size_t fixture_base64_decode(const char *text,unsigned char *out,size_t cap){
  size_t written=0;
  int bits=0,value=0;
  for(;text && *text;text++){
    int digit=fixture_base64_value((unsigned char)*text);
    if(digit<0) continue;
    value=(value<<6)|digit;
    bits+=6;
    if(bits>=8){
      bits-=8;
      if(written>=cap) return 0;
      out[written++]=(unsigned char)(value>>bits);
      value&=(1<<bits)-1;
    }
  }
  return written;
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


/* A streamed sound can have its own compressed AUDO blob but no loose file.
 * This first-party synthetic MP3 tone checks that fallback only; the raw-PCM
 * stale-AudioID case below still declines a fallback. */
int expect_streamed_sound_without_sidecar_plays_embedded_compressed_blob(void){
  unsigned char data[1024]={0},external[576];
  static const char external_base64[]=
    "/+M4wAAAAAAAAAAAAEluZm8AAAAPAAAACwAABwgAMzMzMzMzMzMzR0dHR0dHR0dHXFxcXFxcXFxccHBwcHBwcHBwhYWFhYWFhYWFmZmZmZmZmZmZrq6urq6urq6uwsLCwsLCwsLC19fX19fX19fX6+vr6+vr6+vr////////////AAAAAExhdmM2Mi4xMQAAAAAAAAAAAAAAACQDwAAAAAAAAAcIs8j25gAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/+MoxAAdMKqIX08AAAubclAAfv379+/f3vSA8ePHjylLv379+/34bJBL+F+A7gLYEMMM463o8ePAQrB8/lDnygf6PfrBw5iAH31g4GMgD76wIc0A/yju/B8HAQBAEAQB8HwfB8CAgCAIBgHwfB+UBAMb//B8HwICAIAg4Dg+D76gQU0i7wBAyE3/1///gAAm/+MoxA4dwdpsAZyQAAYIhUCGCwl+/x//8xGOjAgbPUKo0OXtJ0Hq0kEEzxCAzulLAxDAI0DyQNpDZgsiCwr/8UiHyhq0coUEKCIaLl//8XKTQ5w5xRIqRUyIsRb///MS6XTIvF5Eul1IGv/xKEgaEp0O///9ixwAgYCGr7etL1bhb8BgBYBwBgKoCMBgGYAY/+MoxBof+3oZk9cQAAYBCAxAYGSC5AYOuDMAYPmIzAYc7DRAYvEITAYX+CyAYIOCHAYESAZgYAyAlgYDIAagYBIAVgHADhCv";
  size_t external_size=fixture_base64_decode(external_base64,external,sizeof external);
  if(external_size!=sizeof external) return 0;

  enum { sound_record=32, audo_chunk=72, audo_blob=96, file_string_pointer=900 };
  fixture_write_u32(data,0,1);                         /* SOND count */
  fixture_write_u32(data,4,sound_record);
  fixture_write_u32(data,sound_record+4,100);          /* Regular, deliberately not IsEmbedded */
  fixture_write_u32(data,sound_record+12,file_string_pointer);
  float one=1.0f;
  memcpy(data+sound_record+20,&one,sizeof one);
  fixture_write_u32(data,sound_record+28,0);           /* default audio group */
  fixture_write_u32(data,sound_record+32,0);           /* AudioID names the sound's own bytes */
  fixture_write_u32(data,audo_chunk,1);                /* AUDO count */
  fixture_write_u32(data,audo_chunk+4,audo_blob);
  fixture_write_u32(data,audo_blob,(uint32_t)external_size);
  memcpy(data+audo_blob+4,external,external_size);

  char directory[]="/tmp/anygm-streamed-fallback-audio-XXXXXX";
  if(!mkdtemp(directory)) return 0;                    /* empty: the sidecar never exists */
  char *strings[]={(char*)"menu.mp3"};
  uint32_t string_offsets[]={file_string_pointer};
  AnygmHostServices host={0};
  host.struct_size=sizeof(host);
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&host);
  GmlWin win={0};
  win.data=data; win.size=sizeof data; win.n_chunks=2;
  memcpy(win.chunks[0].name,"SOND",4); win.chunks[0].off=0; win.chunks[0].size=audo_chunk;
  memcpy(win.chunks[1].name,"AUDO",4); win.chunks[1].off=audo_chunk;
  win.chunks[1].size=sizeof(data)-audo_chunk;
  win.strs=strings; win.str_charoff=string_offsets; win.n_strs=1;
  win.host=&host;
  snprintf(win.content_dir,sizeof win.content_dir,"%s",directory);

  GmlAudio *audio=gml_audio_create(&win);
  double length=audio?gml_audio_sound_length(audio,0):0.0;
  int voice=audio?gml_audio_play(audio,0,1):-1;
  int16_t mixed[8192]={0};
  if(audio) gml_audio_mix(audio,mixed,4096);
  int audible=0;
  for(size_t index=0;index<sizeof mixed/sizeof mixed[0];index++) audible|=mixed[index]!=0;
  int ok=audio && length>0.05 && voice>=1000000 && audible;
  gml_audio_free(audio);
  rmdir(directory);
  if(!ok) fprintf(stderr,
    "streamed sound without a sidecar did not play its embedded blob:"
    " length=%.6f voice=%d audible=%d\n",length,voice,audible);
  return ok;
}

/* An empty AGRP table cannot identify a group sidecar. This synthetic
 * fixture checks that embedded RIFF PCM remains available at AudioID
 * and that the first playback call succeeds. */
int expect_undeclared_group_sound_plays_its_embedded_pcm(void){
  enum { embedded_samples=4096, sound_record=32, agrp_chunk=72, audo_chunk=80, audo_blob=96,
         file_string_pointer=900 };
  unsigned char data[audo_blob+8+44+embedded_samples*2];
  unsigned char embedded[44+embedded_samples*2];
  memset(data,0,sizeof data);
  size_t embedded_size=fixture_pcm16_wav(embedded,sizeof embedded,embedded_samples,1000);
  if(!embedded_size) return 0;

  fixture_write_u32(data,0,1);                         /* SOND count */
  fixture_write_u32(data,4,sound_record);
  fixture_write_u32(data,sound_record+4,100);          /* Regular, deliberately not IsEmbedded */
  fixture_write_u32(data,sound_record+12,file_string_pointer);
  float one=1.0f;
  memcpy(data+sound_record+20,&one,sizeof one);
  fixture_write_u32(data,sound_record+28,1);           /* claims group 1 */
  fixture_write_u32(data,sound_record+32,0);           /* AudioID into the payload's own AUDO */
  fixture_write_u32(data,agrp_chunk,0);                /* AGRP declares no group at all */
  fixture_write_u32(data,audo_chunk,1);                /* AUDO count */
  fixture_write_u32(data,audo_chunk+4,audo_blob);
  fixture_write_u32(data,audo_blob,(uint32_t)embedded_size);
  memcpy(data+audo_blob+4,embedded,embedded_size);

  char directory[]="/tmp/anygm-undeclared-group-audio-XXXXXX";
  if(!mkdtemp(directory)) return 0;                    /* empty: no sidecar and no group file */
  char *strings[]={(char*)"music.wav"};
  uint32_t string_offsets[]={file_string_pointer};
  AnygmHostServices host={0};
  host.struct_size=sizeof(host);
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&host);
  GmlWin win={0};
  win.data=data; win.size=sizeof data; win.n_chunks=3;
  memcpy(win.chunks[0].name,"SOND",4); win.chunks[0].off=0; win.chunks[0].size=agrp_chunk;
  memcpy(win.chunks[1].name,"AGRP",4); win.chunks[1].off=agrp_chunk; win.chunks[1].size=4;
  memcpy(win.chunks[2].name,"AUDO",4); win.chunks[2].off=audo_chunk;
  win.chunks[2].size=(uint32_t)(sizeof(data)-audo_chunk);
  win.strs=strings; win.str_charoff=string_offsets; win.n_strs=1;
  win.host=&host;
  snprintf(win.content_dir,sizeof win.content_dir,"%s",directory);

  GmlAudio *audio=gml_audio_create(&win);
  int voice=audio?gml_audio_play(audio,0,0):-1;        /* the first and only play */
  int16_t mixed[8192]={0};
  if(audio) gml_audio_mix(audio,mixed,4096);
  int audible=0;
  for(size_t index=0;index<sizeof mixed/sizeof mixed[0];index++) audible|=mixed[index]!=0;
  int ok=audio && voice>=1000000 && audible;
  gml_audio_free(audio);
  rmdir(directory);
  if(!ok) fprintf(stderr,
    "a sound claiming an undeclared audio group did not play its embedded PCM on its first play:"
    " voice=%d audible=%d\n",voice,audible);
  return ok;
}

int expect_flagged_external_sound_precedes_embedded_audio_id(void){
  enum { embedded_samples=4 };
  unsigned char data[256]={0},embedded[44+embedded_samples*2],external[576];
  size_t embedded_size=fixture_pcm16_wav(embedded,sizeof embedded,embedded_samples,1000);
  /* First-party synthetic 8 kHz MP3 tone, not audio copied from external content. */
  static const char external_base64[]=
    "/+M4wAAAAAAAAAAAAEluZm8AAAAPAAAACwAABwgAMzMzMzMzMzMzR0dHR0dHR0dHXFxcXFxcXFxccHBwcHBwcHBwhYWFhYWFhYWFmZmZmZmZmZmZrq6urq6urq6uwsLCwsLCwsLC19fX19fX19fX6+vr6+vr6+vr////////////AAAAAExhdmM2Mi4xMQAAAAAAAAAAAAAAACQDwAAAAAAAAAcIs8j25gAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/+MoxAAdMKqIX08AAAubclAAfv379+/f3vSA8ePHjylLv379+/34bJBL+F+A7gLYEMMM463o8ePAQrB8/lDnygf6PfrBw5iAH31g4GMgD76wIc0A/yju/B8HAQBAEAQB8HwfB8CAgCAIBgHwfB+UBAMb//B8HwICAIAg4Dg+D76gQU0i7wBAyE3/1///gAAm/+MoxA4dwdpsAZyQAAYIhUCGCwl+/x//8xGOjAgbPUKo0OXtJ0Hq0kEEzxCAzulLAxDAI0DyQNpDZgsiCwr/8UiHyhq0coUEKCIaLl//8XKTQ5w5xRIqRUyIsRb///MS6XTIvF5Eul1IGv/xKEgaEp0O///9ixwAgYCGr7etL1bhb8BgBYBwBgKoCMBgGYAY/+MoxBof+3oZk9cQAAYBCAxAYGSC5AYOuDMAYPmIzAYc7DRAYvEITAYX+CyAYIOCHAYESAZgYAyAlgYDIAagYBIAVgHADhCv";
  size_t external_size=fixture_base64_decode(external_base64,external,sizeof external);
  char directory[]="/tmp/anygm-flagged-external-audio-XXXXXX";
  if(!embedded_size || external_size!=sizeof external || !mkdtemp(directory)) return 0;
  char path[256];
  snprintf(path,sizeof(path),"%s/menu.mp3",directory);
  FILE *file=fopen(path,"wb");
  int ok=file && fwrite(external,1,external_size,file)==external_size;
  if(file && fclose(file)!=0) ok=0;

  enum { sound_record=32, audo_chunk=72, audo_blob=96, file_string_pointer=300 };
  fixture_write_u32(data,0,1);                         /* SOND count */
  fixture_write_u32(data,4,sound_record);
  fixture_write_u32(data,sound_record+4,100);          /* Regular, deliberately not IsEmbedded */
  fixture_write_u32(data,sound_record+12,file_string_pointer);
  float one=1.0f;
  memcpy(data+sound_record+20,&one,sizeof one);
  fixture_write_u32(data,sound_record+28,0);           /* default audio group */
  fixture_write_u32(data,sound_record+32,0);           /* stale/dummy embedded AudioID */
  fixture_write_u32(data,audo_chunk,1);                /* AUDO count */
  fixture_write_u32(data,audo_chunk+4,audo_blob);
  fixture_write_u32(data,audo_blob,(uint32_t)embedded_size);
  memcpy(data+audo_blob+4,embedded,embedded_size);

  char *strings[]={(char*)"menu.mp3"};
  uint32_t string_offsets[]={file_string_pointer};
  AnygmHostServices host={0};
  host.struct_size=sizeof(host);
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&host);
  GmlWin win={0};
  win.data=data; win.size=sizeof data; win.n_chunks=2;
  memcpy(win.chunks[0].name,"SOND",4); win.chunks[0].off=0; win.chunks[0].size=audo_chunk;
  memcpy(win.chunks[1].name,"AUDO",4); win.chunks[1].off=audo_chunk;
  win.chunks[1].size=sizeof(data)-audo_chunk;
  win.strs=strings; win.str_charoff=string_offsets; win.n_strs=1;
  win.host=&host;
  snprintf(win.content_dir,sizeof win.content_dir,"%s",directory);

  GmlAudio *audio=ok?gml_audio_create(&win):NULL;
  /* This fixture's external MP3 is much longer than the four-sample embedded dummy, proving that
   * the IsEmbedded flag, rather than a convenient AudioID, selected the source. */
  double length=audio?gml_audio_sound_length(audio,0):0.0;
  int voice=audio?gml_audio_play(audio,0,1):-1;
  int16_t mixed[8192]={0};
  if(audio) gml_audio_mix(audio,mixed,4096);
  int audible=0;
  for(size_t index=0;index<sizeof mixed/sizeof mixed[0];index++) audible|=mixed[index]!=0;
  ok=ok && audio && length>0.05 && voice>=1000000 && audible;
  gml_audio_free(audio);

  /* The external flag is authoritative. If its sidecar is absent, a stale AudioID must not turn
   * into an unrelated short embedded loop. */
  ok=ok && unlink(path)==0;
  audio=gml_audio_create(&win);
  ok=ok && audio && gml_audio_play(audio,0,1)<0;
  gml_audio_free(audio);
  rmdir(directory);
  if(!ok) fprintf(stderr,
    "flagged external SOND did not override its embedded AudioID: length=%.6f voice=%d audible=%d\n",
    length,voice,audible);
  return ok;
}


/* Build a WAVE around an already-encoded data chunk, declaring the format tag, channel count,
 * bit depth and block alignment the samples were written with. The cases below use it to say the
 * same signal twice in two encodings, so what they compare is the decoder rather than a constant
 * somebody wrote down. */
static size_t fixture_wave(unsigned char *out,size_t cap,uint16_t format,uint16_t channels,
                           uint32_t rate,uint16_t bits,uint16_t block_align,
                           const unsigned char *extra,uint16_t extra_size,
                           const unsigned char *payload,uint32_t payload_size){
  size_t header=36u+(size_t)extra_size+(extra_size?2u:0u);
  size_t bytes=header+8u+payload_size;
  if(!out || bytes>cap) return 0;
  memset(out,0,bytes);
  memcpy(out,"RIFF",4);
  fixture_write_u32(out,4,(uint32_t)bytes-8u);
  memcpy(out+8,"WAVEfmt ",8);
  fixture_write_u32(out,16,(uint32_t)(16u+(extra_size?2u+extra_size:0u)));
  fixture_write_u16(out,20,format);
  fixture_write_u16(out,22,channels);
  fixture_write_u32(out,24,rate);
  fixture_write_u32(out,28,rate*block_align);
  fixture_write_u16(out,32,block_align);
  fixture_write_u16(out,34,bits);
  if(extra_size){
    fixture_write_u16(out,36,extra_size);
    memcpy(out+38,extra,extra_size);
  }
  memcpy(out+header,"data",4);
  fixture_write_u32(out,header+4u,payload_size);
  memcpy(out+header+8u,payload,payload_size);
  return bytes;
}

/* Two embedded sounds, one blob each, both marked embedded so the container's own bytes are what
 * plays. Returns the container size actually used. */
static size_t fixture_two_sound_container(unsigned char *data,size_t cap,
                                          const unsigned char *first,size_t first_size,
                                          const unsigned char *second,size_t second_size,
                                          uint32_t *audo_chunk_out){
  enum { first_record=64, second_record=first_record+40, audo_chunk=first_record+96 };
  size_t first_blob=audo_chunk+12u;
  size_t second_blob=first_blob+4u+first_size;
  size_t total=second_blob+4u+second_size;
  if(!data || total>cap) return 0;
  memset(data,0,total);
  fixture_write_u32(data,0,2);                          /* SOND count */
  fixture_write_u32(data,4,first_record);
  fixture_write_u32(data,8,second_record);
  float one=1.0f;
  const size_t records[2]={first_record,second_record};
  for(int index=0;index<2;index++){
    fixture_write_u32(data,records[index]+4,1);         /* IsEmbedded */
    memcpy(data+records[index]+20,&one,sizeof one);     /* resource volume */
    fixture_write_u32(data,records[index]+28,0);        /* default audio group */
    fixture_write_u32(data,records[index]+32,(uint32_t)index);
  }
  fixture_write_u32(data,audo_chunk,2);                 /* AUDO count */
  fixture_write_u32(data,audo_chunk+4,(uint32_t)first_blob);
  fixture_write_u32(data,audo_chunk+8,(uint32_t)second_blob);
  fixture_write_u32(data,first_blob,(uint32_t)first_size);
  memcpy(data+first_blob+4u,first,first_size);
  fixture_write_u32(data,second_blob,(uint32_t)second_size);
  memcpy(data+second_blob+4u,second,second_size);
  if(audo_chunk_out) *audo_chunk_out=(uint32_t)audo_chunk;
  return total;
}

/* Mix one embedded sound of a two-sound container on its own. Each call builds its own mixer so
 * the two sounds are never summed together. */
static int fixture_mix_one_embedded(unsigned char *data,size_t size,uint32_t audo_chunk,
                                    int sound,int classic_version,int16_t *out,int frames){
  GmlWin win={0};
  win.classic_version=classic_version;
  win.data=data; win.size=(uint32_t)size; win.n_chunks=2;
  memcpy(win.chunks[0].name,"SOND",4); win.chunks[0].off=0; win.chunks[0].size=audo_chunk;
  memcpy(win.chunks[1].name,"AUDO",4); win.chunks[1].off=audo_chunk;
  win.chunks[1].size=(uint32_t)size-audo_chunk;
  GmlAudio *audio=gml_audio_create(&win);
  if(!audio) return 0;
  int voice=gml_audio_play(audio,sound,0);
  memset(out,0,(size_t)frames*2u*sizeof(*out));
  gml_audio_mix(audio,out,frames);
  gml_audio_free(audio);
  return voice>=0;
}

/* The synthetic signal checks Studio's linear resource gain and the
 * separate classic post-mix bus attenuation. */
int expect_resource_volume_is_linear_mixer_gain(void){
  enum { frames=16 };
  unsigned char wave[128],data[512];
  unsigned char samples[frames*2];
  for(int index=0;index<frames;index++)
    fixture_write_u16(samples,(size_t)index*2u,12000);
  size_t wave_size=fixture_wave(wave,sizeof wave,1,1,44100,16,2,NULL,0,
                                samples,sizeof samples);
  uint32_t audo_chunk=0;
  size_t size=wave_size
    ? fixture_two_sound_container(data,sizeof data,wave,wave_size,wave,wave_size,&audo_chunk)
    : 0;
  if(!size) return 0;
  float half=0.5f;
  memcpy(data+64+20,&half,sizeof half);
  int16_t modern[frames*2],classic[frames*2];
  int ok=fixture_mix_one_embedded(data,size,audo_chunk,0,0,modern,frames) &&
         fixture_mix_one_embedded(data,size,audo_chunk,0,810,classic,frames);
  for(int index=0;ok && index<frames*2;index++)
    if(modern[index]!=6000 || classic[index]!=3300){
      fprintf(stderr,"resource volume mixed sample %d as modern=%d classic=%d instead of 6000/3300\n",
              index,modern[index],classic[index]);
      ok=0;
    }
  return ok;
}

/* An 8-bit WAVE says so in its format chunk, and its samples are unsigned around 128. Read as
 * 16-bit little-endian pairs instead, a quiet run of 0x80 bytes becomes -32640 held for the whole
 * effect: full-scale noise. The rule is stated as an equality between two
 * encodings of one signal, so no constant of the mixer's own is written down here. */
int expect_embedded_eight_bit_wave_matches_its_sixteen_bit_signal(void){
  enum { values=64, frames=48 };
  unsigned char eight[128],sixteen[192],data[1024];
  unsigned char eight_payload[values];
  unsigned char sixteen_payload[values*2];
  for(int index=0;index<values;index++){
    unsigned char sample=(unsigned char)(0x80+((index%8)-4)*16);
    eight_payload[index]=sample;
    int16_t widened=(int16_t)(((int)sample-128)*256);
    fixture_write_u16(sixteen_payload,(size_t)index*2u,(uint16_t)widened);
  }
  size_t eight_size=fixture_wave(eight,sizeof eight,1,1,44100,8,1,NULL,0,
                                 eight_payload,(uint32_t)sizeof eight_payload);
  size_t sixteen_size=fixture_wave(sixteen,sizeof sixteen,1,1,44100,16,2,NULL,0,
                                   sixteen_payload,(uint32_t)sizeof sixteen_payload);
  uint32_t audo_chunk=0;
  size_t size=eight_size && sixteen_size
    ? fixture_two_sound_container(data,sizeof data,eight,eight_size,sixteen,sixteen_size,
                                  &audo_chunk)
    : 0;
  if(!size) return 0;
  int16_t narrow[frames*2],wide[frames*2];
  int ok=fixture_mix_one_embedded(data,size,audo_chunk,0,0,narrow,frames) &&
         fixture_mix_one_embedded(data,size,audo_chunk,1,0,wide,frames);
  int audible=0;
  for(int index=0;ok && index<frames*2;index++){
    audible|=wide[index]!=0;
    if(narrow[index]!=wide[index]){
      fprintf(stderr,"8-bit WAVE differs from its 16-bit signal at %d: %d vs %d\n",
              index,narrow[index],wide[index]);
      ok=0;
    }
  }
  if(ok && !audible){ fprintf(stderr,"the compared 8-bit fixture was silent\n"); ok=0; }
  return ok;
}

/* MS ADPCM stores seven bytes of predictor state per channel and one sample per nibble after it.
 * The same rule as above: the block below encodes a signal this test also writes as 16-bit PCM,
 * and the two must mix identically. Without a decoder the compressed bytes were played as if they
 * were samples, which is noise rather than the effect. */
int expect_embedded_ms_adpcm_wave_matches_its_sixteen_bit_signal(void){
  enum { block_align=32, primed=2, nibbles=(block_align-7)*2, values=primed+nibbles, frames=48 };
  unsigned char block[block_align],sixteen[192],adpcm[192],data[1024];
  unsigned char sixteen_payload[values*2];
  /* Predictor set 0 is {256,0}: the next sample is the previous one plus the nibble times the
   * delta, which makes the expected signal something this test can state in closed form. */
  memset(block,0,sizeof block);
  block[0]=0;                                        /* coefficient set */
  fixture_write_u16(block,1,16);                     /* initial delta */
  fixture_write_u16(block,3,(uint16_t)1000);         /* sample1: the second sample out */
  fixture_write_u16(block,5,(uint16_t)2000);         /* sample2: the first sample out */
  block[7]=0x10;                                     /* nibbles 1 then 0 */
  static const int adaptation[16]=
    {230,230,230,230,307,409,512,614,768,614,512,409,307,230,230,230};
  int sample1=1000,sample2=2000,delta=16;
  int16_t expected[values];
  expected[0]=(int16_t)sample2;
  expected[1]=(int16_t)sample1;
  for(int index=0;index<nibbles;index++){
    int nibble=(index==0)?1:0;                       /* one step up, then hold */
    int predictor=sample1;                           /* (sample1*256 + sample2*0) >> 8 */
    predictor+=nibble*delta;
    if(predictor>32767) predictor=32767; else if(predictor<-32768) predictor=-32768;
    sample2=sample1;
    sample1=predictor;
    int next=(adaptation[nibble]*delta)>>8;
    delta=next<16?16:next;
    expected[primed+index]=(int16_t)predictor;
  }
  for(int index=0;index<values;index++)
    fixture_write_u16(sixteen_payload,(size_t)index*2u,(uint16_t)expected[index]);
  unsigned char extra[4];
  fixture_write_u16(extra,0,(uint16_t)((block_align-7)*2+2));   /* samples per block */
  fixture_write_u16(extra,2,0);                                 /* no coefficient table */
  size_t adpcm_size=fixture_wave(adpcm,sizeof adpcm,2,1,44100,4,block_align,extra,sizeof extra,
                                 block,(uint32_t)sizeof block);
  size_t sixteen_size=fixture_wave(sixteen,sizeof sixteen,1,1,44100,16,2,NULL,0,
                                   sixteen_payload,(uint32_t)sizeof sixteen_payload);
  uint32_t audo_chunk=0;
  size_t size=adpcm_size && sixteen_size
    ? fixture_two_sound_container(data,sizeof data,adpcm,adpcm_size,sixteen,sixteen_size,
                                  &audo_chunk)
    : 0;
  if(!size) return 0;
  int16_t compressed[frames*2],wide[frames*2];
  int ok=fixture_mix_one_embedded(data,size,audo_chunk,0,0,compressed,frames) &&
         fixture_mix_one_embedded(data,size,audo_chunk,1,0,wide,frames);
  int audible=0;
  for(int index=0;ok && index<frames*2;index++){
    audible|=wide[index]!=0;
    if(compressed[index]!=wide[index]){
      fprintf(stderr,"MS ADPCM WAVE differs from its 16-bit signal at %d: %d vs %d\n",
              index,compressed[index],wide[index]);
      ok=0;
    }
  }
  if(ok && !audible){ fprintf(stderr,"the compared ADPCM fixture was silent\n"); ok=0; }
  return ok;
}


/* A synthetic eight-bit signal is silent except for its terminal sample. Classic playback must omit that frame, while modern playback and classic sixteen-bit playback keep their terminal samples. */
int expect_classic_sound_drops_its_trailing_frame(void){
  enum { values=32, frames=64 };
  unsigned char eight[128],data[1024];
  unsigned char payload[values];
  for(int index=0;index<values;index++) payload[index]=0x80;   /* digital silence throughout ... */
  payload[values-1]=0x00;                                      /* ... except one full-scale sample */
  size_t wave=fixture_wave(eight,sizeof eight,1,1,44100,8,1,NULL,0,payload,(uint32_t)sizeof payload);
  uint32_t audo_chunk=0;
  size_t size=wave?fixture_two_sound_container(data,sizeof data,eight,wave,eight,wave,&audo_chunk):0;
  if(!size) return 0;

  /* The same container read as classic content and as modern content. Only the first drops it. */
  int16_t classic[frames*2],modern[frames*2];
  GmlWin win={0};
  win.data=data; win.size=(uint32_t)size; win.n_chunks=2;
  memcpy(win.chunks[0].name,"SOND",4); win.chunks[0].off=0; win.chunks[0].size=audo_chunk;
  memcpy(win.chunks[1].name,"AUDO",4); win.chunks[1].off=audo_chunk;
  win.chunks[1].size=(uint32_t)size-audo_chunk;

  win.classic_version=810;
  GmlAudio *audio=gml_audio_create(&win);
  if(!audio) return 0;
  int played=gml_audio_play(audio,0,0)>=0;
  memset(classic,0,sizeof classic);
  gml_audio_mix(audio,classic,frames);
  gml_audio_free(audio);

  win.classic_version=0;
  win.bytecode=17;
  audio=gml_audio_create(&win);
  if(!audio) return 0;
  played=played && gml_audio_play(audio,0,0)>=0;
  memset(modern,0,sizeof modern);
  gml_audio_mix(audio,modern,frames);
  gml_audio_free(audio);

  int classic_peak=0,modern_peak=0;
  for(int index=0;index<frames*2;index++){
    int c=classic[index]<0?-classic[index]:classic[index];
    int m=modern[index]<0?-modern[index]:modern[index];
    if(c>classic_peak) classic_peak=c;
    if(m>modern_peak) modern_peak=m;
  }
  /* The same signal written as sixteen-bit PCM, read as classic content. The rule is eight-bit
   * only, so this one must keep its final sample: without this half, a later reading could widen
   * the rule without noticing that nothing ever asked for it. */
  int16_t wide_payload[values];
  for(int index=0;index<values;index++) wide_payload[index]=0;
  wide_payload[values-1]=-32768;
  unsigned char sixteen[192],wide_data[1024];
  unsigned char wide_bytes[values*2];
  for(int index=0;index<values;index++)
    fixture_write_u16(wide_bytes,(size_t)index*2u,(uint16_t)wide_payload[index]);
  size_t wide_wave=fixture_wave(sixteen,sizeof sixteen,1,1,44100,16,2,NULL,0,
                                wide_bytes,(uint32_t)sizeof wide_bytes);
  uint32_t wide_chunk=0;
  size_t wide_size=wide_wave?fixture_two_sound_container(wide_data,sizeof wide_data,sixteen,
                                                         wide_wave,sixteen,wide_wave,&wide_chunk):0;
  int16_t wide_mix[frames*2]; int wide_peak=0;
  if(wide_size){
    GmlWin wide_win={0};
    wide_win.data=wide_data; wide_win.size=(uint32_t)wide_size; wide_win.n_chunks=2;
    memcpy(wide_win.chunks[0].name,"SOND",4);
    wide_win.chunks[0].off=0; wide_win.chunks[0].size=wide_chunk;
    memcpy(wide_win.chunks[1].name,"AUDO",4); wide_win.chunks[1].off=wide_chunk;
    wide_win.chunks[1].size=(uint32_t)wide_size-wide_chunk;
    wide_win.classic_version=810;
    audio=gml_audio_create(&wide_win);
    if(audio){
      played=played && gml_audio_play(audio,0,0)>=0;
      memset(wide_mix,0,sizeof wide_mix);
      gml_audio_mix(audio,wide_mix,frames);
      gml_audio_free(audio);
      for(int index=0;index<frames*2;index++){
        int v=wide_mix[index]<0?-wide_mix[index]:wide_mix[index];
        if(v>wide_peak) wide_peak=v;
      }
    }
  }

  /* Silence throughout except that one sample, so the peak is the whole assertion: the eight-bit
   * classic reading must produce nothing at all, and both the modern reading of the same bytes and
   * the sixteen-bit classic one must produce the sample their file holds. */
  int ok = played && wide_size && classic_peak==0 && modern_peak>10000 && wide_peak>10000;
  if(!ok) fprintf(stderr,
    "classic trailing frame: played=%d peaks 8-bit-classic=%d 8-bit-modern=%d 16-bit-classic=%d"
    " (want 0, loud, loud)\n",played,classic_peak,modern_peak,wide_peak);
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

int expect_classic_dynamic_sound_lifecycle(void){
  enum { sample_count=128 };
  unsigned char wav[44+sample_count*2]={0};
  size_t wav_size=fixture_pcm16_wav(wav,sizeof wav,sample_count,12000);
  char directory[]="/tmp/anygm-classic-dynamic-sound-XXXXXX";
  if(!wav_size || !mkdtemp(directory)) return 0;
  char path[256];
  snprintf(path,sizeof path,"%s/effect.wav",directory);
  FILE *file=fopen(path,"wb");
  int ok=file && fwrite(wav,1,wav_size,file)==wav_size;
  if(file && fclose(file)!=0) ok=0;

  AnygmHostServices host={0};
  host.struct_size=sizeof host;
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&host);
  GmlWin win={0};
  win.classic_version=530;
  snprintf(win.content_dir,sizeof win.content_dir,"%s",directory);
  GmlVM vm={0};
  if(!ok || gml_vm_init(&vm,&win,&host)!=0){
    unlink(path); rmdir(directory); return 0;
  }
  GmlAudio *audio=gml_audio_create(&win);
  vm.audio=audio;
  GmlVal add_args[4]={vstr("effect.wav"),vreal(1),vreal(0),vreal(1)};
  GmlVal added=gml_builtin_call(&vm,"sound_add",add_args,4);
  GmlVal sound=vreal(added.t==V_REAL?added.d:-1);
  ok=ok && audio && sound.d>=0 &&
     gml_builtin_call(&vm,"sound_exists",&sound,1).d==1;
  GmlVal voice=gml_builtin_call(&vm,"sound_play",&sound,1);
  int16_t mixed[256]={0};
  if(audio) gml_audio_mix(audio,mixed,128);
  int audible=0;
  for(size_t index=0;index<sizeof mixed/sizeof mixed[0];index++)
    audible|=mixed[index]!=0;
  ok=ok && voice.t==V_REAL && voice.d>=1000000 && audible;
  (void)gml_builtin_call(&vm,"sound_delete",&sound,1);
  ok=ok && gml_builtin_call(&vm,"sound_exists",&sound,1).d==0;

  gml_vm_free(&vm);
  gml_audio_free(audio);
  unlink(path);
  rmdir(directory);
  if(!ok) fprintf(stderr,"classic dynamic-sound lifecycle fixture failed\n");
  return ok;
}


/* A state must fully describe the sound table it was taken from. Sounds created after a state was
 * saved are not in it, so restoring that state has to release them again; otherwise the table only
 * ever grows, a save->load->save pair stops being stable, and the per-sound records -- which are
 * stored positionally -- start landing on the wrong sounds. A frontend that saves and restores
 * every frame, as run-ahead and rewind do, hits this on the first sound a session loads at run
 * time. */
int expect_state_load_releases_later_dynamic_sounds(void){
  unsigned char wav[44+4410]={0};
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
  fixture_write_u32(wav,40,4410);
  memset(wav+44,128,4410);
  wav[44]=255; wav[45]=0; wav[46]=192; wav[47]=64;

  GmlWin win={0};
  GmlAudio *audio=gml_audio_create(&win);
  if(!audio) return 0;

  int first=gml_audio_add_encoded(audio,wav,sizeof wav);
  int ok=first>=0 && gml_audio_exists(audio,first);
  gml_audio_sound_gain(audio,first,0.5);

  /* The state a frontend would keep from before the next sound is loaded. */
  size_t size=gml_audio_state_size(audio),written=0,used=0;
  void *state=malloc(size?size:1);
  ok=ok && state && gml_audio_state_save(audio,state,size,&written) && written==size;

  /* Crossing into content that loads its own audio appends a sound the state predates. */
  int second=gml_audio_add_encoded(audio,wav,sizeof wav);
  ok=ok && second>=0 && second!=first && gml_audio_exists(audio,second);
  size_t grown=gml_audio_state_size(audio);
  ok=ok && grown>size;

  ok=ok && gml_audio_state_load(audio,state,size,&used) && used==size;
  /* The sound the state predates must be gone, and the one it describes must survive intact. */
  ok=ok && !gml_audio_exists(audio,second);
  ok=ok && gml_audio_exists(audio,first);
  ok=ok && fabs(gml_audio_sound_get_gain(audio,first)-0.5)<1e-12;
  /* Restoring must return the table to the size the state describes, so a further save matches it
   * and repeated restores stay stable rather than drifting. */
  ok=ok && gml_audio_state_size(audio)==size;
  size_t rewritten=0;
  void *again=malloc(size?size:1);
  ok=ok && again && gml_audio_state_save(audio,again,size,&rewritten) &&
     rewritten==size && !memcmp(again,state,size);
  free(again);

  /* A released slot inside the table remains present so that the following handle keeps its
   * identity. Its state record is deliberately all zeroes. Loading that record must not normalize
   * only its pitch to one, or a byte-exact frontend roundtrip drifts despite identical playback. */
  int retained=gml_audio_add_encoded(audio,wav,sizeof wav);
  ok=ok && retained==second && gml_audio_exists(audio,retained);
  gml_audio_caster_free(audio,first);
  ok=ok && !gml_audio_exists(audio,first) && gml_audio_exists(audio,retained);
  size_t sparse_size=gml_audio_state_size(audio),sparse_written=0,sparse_used=0;
  void *sparse=malloc(sparse_size?sparse_size:1);
  void *sparse_again=malloc(sparse_size?sparse_size:1);
  ok=ok && sparse && sparse_again &&
     gml_audio_state_save(audio,sparse,sparse_size,&sparse_written) &&
     sparse_written==sparse_size &&
     gml_audio_state_load(audio,sparse,sparse_size,&sparse_used) && sparse_used==sparse_size;
  size_t sparse_rewritten=0;
  ok=ok && gml_audio_state_save(audio,sparse_again,sparse_size,&sparse_rewritten) &&
     sparse_rewritten==sparse_size && !memcmp(sparse_again,sparse,sparse_size);
  free(sparse_again);
  free(sparse);
  free(state);
  gml_audio_free(audio);
  if(!ok) fprintf(stderr,"state load did not release sounds created after the state\n");
  return ok;
}

/* The mirror case. A state may name more run-time sounds than the session holds, and it carries
 * their mixer parameters but not their identity, so they cannot be recreated. Their records must
 * then not be applied to whatever occupies those slots, and the sounds the state says nothing
 * trustworthy about must keep what they have rather than being reset. */
int expect_state_load_ignores_unmatched_dynamic_records(void){
  enum { sample_count=4410 };
  unsigned char wav[44+sample_count]={0};
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
  fixture_write_u32(wav,40,sample_count);
  for(int sample=0;sample<sample_count;sample++)
    wav[44+sample]=(unsigned char)(128+((sample%64)-32)*3);

  GmlWin win={0};
  GmlAudio *audio=gml_audio_create(&win);
  if(!audio) return 0;

  /* A session that had two run-time sounds when the state was taken. */
  int first=gml_audio_add_encoded(audio,wav,sizeof wav);
  int second=gml_audio_add_encoded(audio,wav,sizeof wav);
  int ok=first>=0 && second>=0 && second!=first;
  gml_audio_sound_gain(audio,first,0.25);
  gml_audio_sound_gain(audio,second,0.75);
  size_t size=gml_audio_state_size(audio),written=0,used=0;
  void *state=malloc(size?size:1);
  ok=ok && state && gml_audio_state_save(audio,state,size,&written) && written==size;
  gml_audio_free(audio);

  /* A fresh session that has loaded only one run-time sound, and a different one. */
  audio=gml_audio_create(&win);
  if(!audio){ free(state); return 0; }
  int only=gml_audio_add_encoded(audio,wav,sizeof wav);
  ok=ok && only>=0 && only==first;
  gml_audio_sound_gain(audio,only,0.5);

  ok=ok && gml_audio_state_load(audio,state,size,&used) && used==size;
  /* The state names two run-time sounds and this session holds one, so neither record can be
   * trusted to name it. Its gain must survive untouched instead of taking 0.25 or a default. */
  ok=ok && gml_audio_exists(audio,only);
  ok=ok && fabs(gml_audio_sound_get_gain(audio,only)-0.5)<1e-12;
  free(state);
  gml_audio_free(audio);
  if(!ok) fprintf(stderr,"state load applied run-time records onto an unmatched table\n");
  return ok;
}

int expect_dynamic_audio_extension_state(void){
  enum { sample_count=4410 };
  unsigned char wav[44+sample_count]={0};
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

static void fixture_call_sound_fade(GmlVM *vm,int cached,int id,GmlVal *args,int count){
  if(cached) gml_builtin_call_fast_id(vm,id,"sound_fade",args,count);
  else gml_builtin_call(vm,"sound_fade",args,count);
}

int expect_builtin_sound_fade(void){
  int ok=1;
  for(int cached=0;cached<2;cached++){
    unsigned char wav[60];
    size_t wav_size=fixture_pcm16_wav(wav,sizeof wav,8,8000);
    GmlWin win={0};
    GmlAudio *audio=gml_audio_create(&win);
    if(!audio) return 0;
    GmlVM vm={0}; vm.audio=audio;
    int sound=gml_audio_add_encoded(audio,wav,(int)wav_size);
    int voice=gml_audio_play(audio,sound,1);
    int id=gml_builtin_fast_id(&vm,"sound_fade");
    GmlVal args[]={vreal(sound),vreal(0),vreal(20)};
    fixture_call_sound_fade(&vm,cached,id,args,3);
    ok=ok && sound>=0 && voice>=1000000 && id>0 &&
       gml_audio_sound_get_gain(audio,sound)==1;
    int16_t mixed[882], replay[882];
    gml_audio_mix(audio,mixed,441);
    double middle=gml_audio_sound_get_gain(audio,sound);
    int audible=0;
    for(size_t i=0;i<sizeof mixed/sizeof mixed[0];i++) audible|=mixed[i]!=0;
    ok=ok && fabs(middle-0.5)<1e-12 && audible;

    size_t size=gml_audio_state_size(audio),written=0,used=0;
    void *state=malloc(size?size:1);
    int saved=state && gml_audio_state_save(audio,state,size,&written) && written==size;
    gml_audio_mix(audio,mixed,441);
    double end=gml_audio_sound_get_gain(audio,sound);
    int restored=saved && gml_audio_state_load(audio,state,size,&used) && used==size;
    ok=ok && restored && fabs(gml_audio_sound_get_gain(audio,sound)-middle)<1e-12;
    gml_audio_mix(audio,replay,441);
    ok=ok && end==0 && gml_audio_sound_get_gain(audio,sound)==0 &&
       !memcmp(mixed,replay,sizeof mixed);
    free(state);
    gml_audio_mix(audio,mixed,441);
    for(size_t i=0;i<sizeof mixed/sizeof mixed[0];i++) ok=ok && mixed[i]==0;

    args[1]=vreal(1); args[2]=vreal(20);
    fixture_call_sound_fade(&vm,cached,id,args,3);
    gml_audio_mix(audio,mixed,441);
    ok=ok && fabs(gml_audio_sound_get_gain(audio,sound)-0.5)<1e-12;
    args[1]=vreal(0.75); args[2]=vreal(10);
    fixture_call_sound_fade(&vm,cached,id,args,3);
    gml_audio_mix(audio,mixed,441);
    ok=ok && gml_audio_sound_get_gain(audio,sound)==0.75;
    args[1]=vreal(0.25); args[2]=vreal(0);
    fixture_call_sound_fade(&vm,cached,id,args,3);
    ok=ok && gml_audio_sound_get_gain(audio,sound)==0.25;

    /* Invalid numeric arguments must not reach an out-of-range integer cast. */
    static const double invalid[]={NAN,INFINITY,-INFINITY,1e100};
    for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
      args[0]=vreal(invalid[i]); args[1]=vreal(0); args[2]=vreal(0);
      fixture_call_sound_fade(&vm,cached,id,args,3);
      args[0]=vreal(sound); args[2]=vreal(invalid[i]);
      fixture_call_sound_fade(&vm,cached,id,args,3);
      ok=ok && gml_audio_sound_get_gain(audio,sound)==0.25;
    }
    args[1]=vreal(NAN); args[2]=vreal(0);
    fixture_call_sound_fade(&vm,cached,id,args,3);
    fixture_call_sound_fade(&vm,cached,id,args,1);
    ok=ok && gml_audio_sound_get_gain(audio,sound)==0.25;
    args[0]=vreal(voice); args[1]=vreal(1); args[2]=vreal(10);
    fixture_call_sound_fade(&vm,cached,id,args,3);
    ok=ok && gml_audio_sound_get_gain(audio,voice)==0.25;
    args[0]=vreal(-1); args[2]=vreal(0);
    fixture_call_sound_fade(&vm,cached,id,args,3);
    ok=ok && gml_audio_sound_get_gain(audio,sound)==0.25;
    args[0]=vreal(sound); args[1]=vreal(2);
    fixture_call_sound_fade(&vm,cached,id,args,3);
    ok=ok && gml_audio_sound_get_gain(audio,sound)==1;
    args[1]=vreal(-1); args[2]=vreal(-10);
    fixture_call_sound_fade(&vm,cached,id,args,3);
    ok=ok && gml_audio_sound_get_gain(audio,sound)==0;
    if(middle!=0.5 || end!=0)
      fprintf(stderr,"sound fade mode=%d midpoint=%.17g endpoint=%.17g\n",cached,middle,end);
    gml_vm_free(&vm);
    gml_audio_free(audio);
  }
  return ok;
}

int expect_saudio_portable_playback(void){
  int failure_stage=0;
  static const char open_name[]=
    "__anygm_external_73617564696f2e646c6c_6f70656e";
  static const char play_name[]=
    "__anygm_external_73617564696f2e646c6c_706c6179";
  static const char stop_name[]=
    "__anygm_external_73617564696f2e646c6c_73746f70";
  static const char pause_name[]=
    "__anygm_external_73617564696f2e646c6c_7061757365";
  static const char resume_name[]=
    "__anygm_external_73617564696f2e646c6c_726573756d65";
  static const char position_name[]=
    "__anygm_external_73617564696f2e646c6c_706f736974696f6e";
  static const char length_name[]=
    "__anygm_external_73617564696f2e646c6c_6c656e677468";
  static const char seek_name[]=
    "__anygm_external_73617564696f2e646c6c_7365656b";
  static const char status_name[]=
    "__anygm_external_73617564696f2e646c6c_737461747573";
  static const char channels_name[]=
    "__anygm_external_73617564696f2e646c6c_6368616e6e656c73";
  static const char bytes_name[]=
    "__anygm_external_73617564696f2e646c6c_6279746573706572736563";
  static const char canplay_name[]=
    "__anygm_external_73617564696f2e646c6c_63616e706c6179";
  static const char close_name[]=
    "__anygm_external_73617564696f2e646c6c_636c6f7365";

  enum { sample_count=4410 };
  unsigned char wav[44+sample_count]={0};
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
  fixture_write_u32(wav,40,sample_count);
  for(int sample=0;sample<sample_count;sample++)
    wav[44+sample]=(unsigned char)(128+((sample%64)-32)*3);

  char directory[]="/tmp/anygm-saudio-XXXXXX";
  if(!mkdtemp(directory)) return 0;
  char path[256];
  snprintf(path,sizeof(path),"%s/tone.wav",directory);
  FILE *file=fopen(path,"wb");
  int ok=file && fwrite(wav,1,sizeof(wav),file)==sizeof(wav);
  if(file && fclose(file)!=0) ok=0;

  AnygmHostServices host={0};
  host.struct_size=sizeof(host);
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&host);
  GmlWin win={0};
  snprintf(win.content_dir,sizeof(win.content_dir),"%s",directory);
  GmlVM vm={0};
  if(!ok || gml_vm_init(&vm,&win,&host)!=0){
    unlink(path); rmdir(directory);
    return 0;
  }
  GmlAudio *audio=gml_audio_create(&win);
  vm.audio=audio;
  if(!audio){
    gml_vm_free(&vm);
    unlink(path); rmdir(directory);
    return 0;
  }

  GmlVal id=vstr("music");
  GmlVal open_args[]={vstr("tone.wav"),id};
  GmlVal opened=gml_builtin_call(&vm,open_name,open_args,2);
  GmlVal canplay=gml_builtin_call(&vm,canplay_name,&id,1);
  GmlVal status=gml_builtin_call(&vm,status_name,&id,1);
  GmlVal channels=gml_builtin_call(&vm,channels_name,&id,1);
  GmlVal bytes=gml_builtin_call(&vm,bytes_name,&id,1);
  ok=opened.t==V_REAL && opened.d==0.0 &&
     canplay.t==V_STR && !strcmp(canplay.s,"true") &&
     status.t==V_STR && !strcmp(status.s,"stopped") &&
     channels.t==V_STR && !strcmp(channels.s,"1") &&
     bytes.t==V_STR && !strcmp(bytes.s,"44100");
  if(!ok){
    failure_stage=1;
    fprintf(stderr,
      "Saudio initial values: open=%d/%g canplay=%d/%s status=%d/%s channels=%d/%s bytes=%d/%s\n",
      opened.t,opened.d,canplay.t,canplay.t==V_STR?canplay.s:"?",
      status.t,status.t==V_STR?status.s:"?",
      channels.t,channels.t==V_STR?channels.s:"?",
      bytes.t,bytes.t==V_STR?bytes.s:"?");
  }

  GmlVal played=gml_builtin_call(&vm,play_name,&id,1);
  status=gml_builtin_call(&vm,status_name,&id,1);
  ok=ok && played.t==V_REAL && played.d==0.0 && status.t==V_STR &&
     !strcmp(status.s,"playing");
  if(!ok && !failure_stage) failure_stage=2;
  int16_t mixed[128];
  gml_audio_mix(audio,mixed,64);
  GmlVal position=gml_builtin_call(&vm,position_name,&id,1);
  GmlVal length=gml_builtin_call(&vm,length_name,&id,1);
  ok=ok && position.t==V_STR && strtod(position.s,NULL)>=0.0 &&
     length.t==V_STR && strtod(length.s,NULL)>=0.0;
  if(!ok && !failure_stage) failure_stage=3;

  (void)gml_builtin_call(&vm,pause_name,&id,1);
  status=gml_builtin_call(&vm,status_name,&id,1);
  ok=ok && status.t==V_STR && !strcmp(status.s,"paused");
  if(!ok && !failure_stage){
    failure_stage=4;
    fprintf(stderr,"Saudio paused status: type=%d value=%s playing=%d paused=%d\n",
            status.t,status.t==V_STR?status.s:"?",
            gml_audio_is_playing(audio,0),gml_audio_voice_paused(audio,0));
  }
  (void)gml_builtin_call(&vm,resume_name,&id,1);
  status=gml_builtin_call(&vm,status_name,&id,1);
  ok=ok && status.t==V_STR && !strcmp(status.s,"playing");
  if(!ok && !failure_stage) failure_stage=5;

  GmlVal seek_args[]={vstr("0"),id};
  ok=ok && gml_builtin_call(&vm,seek_name,seek_args,2).d==0.0;
  size_t state_size=gml_vm_state_size(&vm),written=0,used=0;
  unsigned char *before=(unsigned char*)malloc(state_size?state_size:1u);
  unsigned char *after=(unsigned char*)malloc(state_size?state_size:1u);
  ok=ok && before && after &&
     gml_vm_state_save(&vm,before,state_size,&written) && written==state_size;
  if(!ok && !failure_stage) failure_stage=6;
  /* Closing releases the dynamic mixer slot. Loading the older canonical state must rehydrate the
   * exact SHA-bound file into that same handle before restoring the Saudio registry. */
  (void)gml_builtin_call(&vm,close_name,&id,1);
  canplay=gml_builtin_call(&vm,canplay_name,&id,1);
  ok=ok && canplay.t==V_STR && !strcmp(canplay.s,"false") &&
     gml_vm_state_load(&vm,before,written,&used) && used==written;
  if(!ok && !failure_stage) failure_stage=7;
  canplay=gml_builtin_call(&vm,canplay_name,&id,1);
  size_t repeated=0;
  ok=ok && canplay.t==V_STR && !strcmp(canplay.s,"true") &&
     gml_vm_state_save(&vm,after,state_size,&repeated) && repeated==written &&
     !memcmp(before,after,written);
  if(!ok && !failure_stage) failure_stage=8;

  (void)gml_builtin_call(&vm,stop_name,&id,1);
  status=gml_builtin_call(&vm,status_name,&id,1);
  ok=ok && status.t==V_STR && !strcmp(status.s,"stopped");
  gml_audio_pause_all(audio,1);
  status=gml_builtin_call(&vm,status_name,&id,1);
  ok=ok && status.t==V_STR && !strcmp(status.s,"stopped");
  gml_audio_pause_all(audio,0);
  if(!ok && !failure_stage) failure_stage=9;
  (void)gml_builtin_call(&vm,close_name,&id,1);
  canplay=gml_builtin_call(&vm,canplay_name,&id,1);
  ok=ok && canplay.t==V_STR && !strcmp(canplay.s,"false");
  if(!ok && !failure_stage) failure_stage=10;

  /* A savestate cannot silently bind its handle to different bytes at the same path. */
  wav[44]^=1u;
  file=fopen(path,"wb");
  int changed=file && fwrite(wav,1,sizeof(wav),file)==sizeof(wav);
  if(file && fclose(file)!=0) changed=0;
  used=0;
  ok=ok && changed && !gml_vm_state_load(&vm,before,written,&used);
  if(!ok && !failure_stage) failure_stage=11;

  free(before); free(after);
  gml_vm_free(&vm);
  gml_audio_free(audio);
  unlink(path); rmdir(directory);
  if(!ok) fprintf(stderr,"portable Saudio playback/state fixture failed at stage %d\n",
                  failure_stage);
  return ok;
}

int expect_generic_external_audio_restore(void){
  static const char create_name[]=
    "__anygm_external_5347417564696f2e646c6c_7367615f437265617465456d6974746572";
  static const char destroy_name[]=
    "__anygm_external_5347417564696f2e646c6c_7367615f44657374726f79456d6974746572";
  enum { sample_count=4410 };
  unsigned char wav[44+sample_count]={0};
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
  fixture_write_u32(wav,40,sample_count);
  for(int sample=0;sample<sample_count;sample++)
    wav[44+sample]=(unsigned char)(128+((sample%64)-32)*3);

  char directory[]="/tmp/anygm-external-audio-XXXXXX";
  if(!mkdtemp(directory)) return 0;
  char path[256];
  snprintf(path,sizeof(path),"%s/tone.wav",directory);
  FILE *file=fopen(path,"wb");
  int ok=file && fwrite(wav,1,sizeof(wav),file)==sizeof(wav);
  if(file && fclose(file)!=0) ok=0;

  AnygmHostServices host={0};
  host.struct_size=sizeof(host);
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&host);
  GmlWin win={0};
  snprintf(win.content_dir,sizeof(win.content_dir),"%s",directory);
  GmlVM vm={0};
  if(!ok || gml_vm_init(&vm,&win,&host)!=0){
    unlink(path); rmdir(directory);
    return 0;
  }
  GmlAudio *audio=gml_audio_create(&win);
  vm.audio=audio;
  if(!audio){
    gml_vm_free(&vm);
    unlink(path); rmdir(directory);
    return 0;
  }

  GmlVal relative=vstr("tone.wav");
  static const char supersound_load[]=
    "__anygm_external_7375706572736f756e642e646c6c_53535f4c6f6164536f756e64";
  static const char supersound_loop[]=
    "__anygm_external_7375706572736f756e642e646c6c_53535f4c6f6f70536f756e64";
  static const char supersound_set_volume[]=
    "__anygm_external_7375706572736f756e642e646c6c_53535f536574536f756e64566f6c";
  static const char supersound_get_volume[]=
    "__anygm_external_7375706572736f756e642e646c6c_53535f476574536f756e64566f6c";
  static const char supersound_free[]=
    "__anygm_external_7375706572736f756e642e646c6c_53535f46726565536f756e64";
  GmlVal ss_loaded=gml_builtin_call(&vm,supersound_load,&relative,1);
  int ss_sound=ss_loaded.t==V_STR && ss_loaded.s ? atoi(ss_loaded.s) : -1;
  int ss_ok=ss_loaded.t==V_STR && ss_loaded.d==1.0 && ss_sound>=0 &&
    gml_audio_exists(audio,ss_sound);
  if(ss_ok){
    GmlVal ss_handle=vreal(ss_sound);
    GmlVal ss_level[]={ss_handle,vreal(0)};
    (void)gml_builtin_call(&vm,supersound_loop,&ss_handle,1);
    (void)gml_builtin_call(&vm,supersound_set_volume,ss_level,2);
    int16_t mixed[128]={0};
    gml_audio_mix(audio,mixed,64);
    int audible=0;
    for(size_t sample=0;sample<sizeof mixed/sizeof mixed[0];sample++)
      audible |= mixed[sample]!=0;
    GmlVal full=gml_builtin_call(&vm,supersound_get_volume,&ss_handle,1);
    ss_ok=audible && full.t==V_REAL && full.d==0.0 &&
      gml_audio_sound_get_gain(audio,ss_sound)==1.0;
    ss_level[1]=vreal(-600);
    (void)gml_builtin_call(&vm,supersound_set_volume,ss_level,2);
    GmlVal half=gml_builtin_call(&vm,supersound_get_volume,&ss_handle,1);
    ss_ok=ss_ok && half.t==V_REAL && half.d==-600.0 &&
      fabs(gml_audio_sound_get_gain(audio,ss_sound)-0.5011872336)<0.000001;
    ss_level[1]=vreal(-10000);
    (void)gml_builtin_call(&vm,supersound_set_volume,ss_level,2);
    GmlVal silent=gml_builtin_call(&vm,supersound_get_volume,&ss_handle,1);
    ss_ok=ss_ok && silent.t==V_REAL && silent.d==-10000.0 &&
      gml_audio_sound_get_gain(audio,ss_sound)==0.0 &&
      atoi(ss_loaded.s)==ss_sound;
    (void)gml_builtin_call(&vm,supersound_free,&ss_handle,1);
    ss_ok=ss_ok && !gml_audio_exists(audio,ss_sound);
  }
  if(ss_loaded.t==V_STR && ss_loaded.d==1.0) free((void *)ss_loaded.s);
  ok=ok && ss_ok;
  GmlVal loaded=gml_builtin_call(&vm,create_name,&relative,1);
  int sound=loaded.t==V_REAL?(int)loaded.d:-1;
  ok=ok && sound>=0 && gml_audio_exists(audio,sound);
  size_t state_size=gml_vm_state_size(&vm),written=0,used=0;
  unsigned char *before=(unsigned char*)malloc(state_size?state_size:1u);
  unsigned char *after=(unsigned char*)malloc(state_size?state_size:1u);
  ok=ok && before && after &&
    gml_vm_state_save(&vm,before,state_size,&written) && written==state_size;

  GmlVal handle=vreal(sound);
  (void)gml_builtin_call(&vm,destroy_name,&handle,1);
  ok=ok && !gml_audio_exists(audio,sound) &&
    gml_vm_state_load(&vm,before,written,&used) && used==written &&
    gml_audio_exists(audio,sound);
  size_t repeated=0;
  ok=ok && gml_vm_state_save(&vm,after,state_size,&repeated) &&
    repeated==written && !memcmp(before,after,written);

  /* Identity is content-based for every supported extension API, not only for Saudio IDs. */
  (void)gml_builtin_call(&vm,destroy_name,&handle,1);
  wav[44]^=1u;
  file=fopen(path,"wb");
  int changed=file && fwrite(wav,1,sizeof(wav),file)==sizeof(wav);
  if(file && fclose(file)!=0) changed=0;
  used=0;
  ok=ok && changed && !gml_vm_state_load(&vm,before,written,&used);

  free(before);
  free(after);
  gml_vm_free(&vm);
  gml_audio_free(audio);
  unlink(path);
  rmdir(directory);
  if(!ok) fprintf(stderr,"generic external-audio identity/state fixture failed\n");
  return ok;
}

int expect_faudio_gms_portable_playback(void){
  enum { sample_count=4410 };
  unsigned char wav[44+sample_count]={0};
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
  fixture_write_u32(wav,40,sample_count);
  for(int sample=0;sample<sample_count;sample++)
    wav[44+sample]=(unsigned char)(128+((sample%64)-32)*3);

  char directory[]="/tmp/anygm-faudio-gms-XXXXXX";
  if(!mkdtemp(directory)) return 0;
  char path[256];
  snprintf(path,sizeof path,"%s/tone.wav",directory);
  FILE *file=fopen(path,"wb");
  int ok=file && fwrite(wav,1,sizeof wav,file)==sizeof wav;
  if(file && fclose(file)!=0) ok=0;

  AnygmHostServices host={0};
  host.struct_size=sizeof host;
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&host);
  GmlWin win={0};
  snprintf(win.content_dir,sizeof win.content_dir,"%s",directory);
  GmlVM vm={0};
  if(!ok || gml_vm_init(&vm,&win,&host)!=0){
    unlink(path); rmdir(directory); return 0;
  }
  GmlAudio *audio=gml_audio_create(&win);
  vm.audio=audio;
  if(!audio){ gml_vm_free(&vm); unlink(path); rmdir(directory); return 0; }

  GmlVal relative=vstr("tone.wav");
  GmlVal loaded=gml_builtin_call(&vm,"FAudioGMS_StaticSound_LoadWAV",&relative,1);
  GmlVal instance=gml_builtin_call(
    &vm,"FAudioGMS_StaticSound_CreateSoundInstance",&loaded,1);
  int source=loaded.t==V_REAL?(int)loaded.d:-1;
  int sound=instance.t==V_REAL?(int)instance.d:-1;
  ok=ok && source>=0 && sound>=0 && source!=sound &&
    gml_audio_exists(audio,source) && gml_audio_exists(audio,sound);

  GmlVal sound_argument=vreal(sound);
  GmlVal action_before=gml_builtin_call(&vm,"action_if_sound",&sound_argument,1);
  ok=ok && action_before.t==V_REAL && action_before.d==0.0;

  (void)gml_builtin_call(&vm,"FAudioGMS_SoundInstance_Play",&instance,1);
  GmlVal action_during=gml_builtin_call(&vm,"action_if_sound",&sound_argument,1);
  GmlVal gain_args[]={instance,vreal(0.25)};
  GmlVal pitch_args[]={instance,vreal(1.5)};
  (void)gml_builtin_call(&vm,"FAudioGMS_SoundInstance_SetVolume",gain_args,2);
  (void)gml_builtin_call(&vm,"FAudioGMS_SoundInstance_SetPitch",pitch_args,2);
  GmlVal gain=gml_builtin_call(&vm,"FAudioGMS_SoundInstance_GetVolume",&instance,1);
  GmlVal pitch_value=gml_builtin_call(
    &vm,"FAudioGMS_SoundInstance_GetPitch",&instance,1);
  GmlVal length=gml_builtin_call(
    &vm,"FAudioGMS_SoundInstance_GetTrackLengthInSeconds",&instance,1);
  int16_t mixed[128]={0};
  gml_audio_mix(audio,mixed,64);
  ok=ok && gml_audio_is_playing(audio,sound) &&
    action_during.t==V_REAL && action_during.d==1.0 &&
    gain.t==V_REAL && gain.d==0.25 &&
    pitch_value.t==V_REAL && pitch_value.d==1.5 && length.t==V_REAL && length.d>0.09;

  (void)gml_builtin_call(&vm,"FAudioGMS_SoundInstance_Stop",&instance,1);
  GmlVal action_after=gml_builtin_call(&vm,"action_if_sound",&sound_argument,1);
  ok=ok && action_after.t==V_REAL && action_after.d==0.0;
  (void)gml_builtin_call(&vm,"FAudioGMS_SoundInstance_Destroy",&instance,1);
  ok=ok && !gml_audio_exists(audio,sound) && gml_audio_exists(audio,source);
  (void)gml_builtin_call(&vm,"FAudioGMS_StaticSound_Destroy",&loaded,1);
  ok=ok && !gml_audio_exists(audio,source);

  gml_vm_free(&vm);
  gml_audio_free(audio);
  unlink(path); rmdir(directory);
  if(!ok) fprintf(stderr,"portable FAudioGMS playback fixture failed\n");
  return ok;
}

int expect_wwise_portable_bank_state(void){
  unsigned char bank[25]={0};
  memcpy(bank,"HIRC",4);
  fixture_write_u32(bank,4,17);
  fixture_write_u32(bank,8,1);
  bank[12]=4;
  fixture_write_u32(bank,13,8);
  fixture_write_u32(bank,17,UINT32_C(0x12345678));
  fixture_write_u32(bank,21,0);
  uint8_t *invalid_ogg=(uint8_t *)1;
  size_t invalid_size=1;
  int ok=!gml_wwise_wem_to_ogg(bank,sizeof bank,&invalid_ogg,&invalid_size) &&
    invalid_ogg==NULL && invalid_size==0;

  char directory[]="/tmp/anygm-wwise-bank-XXXXXX";
  if(!mkdtemp(directory)) return 0;
  char path[256];
  snprintf(path,sizeof path,"%s/Synthetic.bnk",directory);
  FILE *file=fopen(path,"wb");
  ok=ok && file && fwrite(bank,1,sizeof bank,file)==sizeof bank;
  if(file && fclose(file)!=0) ok=0;
  AnygmHostServices host={0};
  host.struct_size=sizeof host;
  host.abi_version=ANYGM_HOST_SERVICES_VERSION;
  anygm_stdio_vfs_services_init(&host);
  GmlWin win={0};
  snprintf(win.content_dir,sizeof win.content_dir,"%s",directory);
  GmlVM vm={0};
  if(!ok || gml_vm_init(&vm,&win,&host)!=0){ unlink(path); rmdir(directory); return 0; }
  GmlAudio *audio=gml_audio_create(&win);
  vm.audio=audio;
  GmlVal base=vstr(directory),name=vstr("Synthetic.bnk");
  ok=audio && gml_builtin_call(&vm,"gmwSetBasePath",&base,1).d==1 &&
    gml_builtin_call(&vm,"gmwLoadBank",&name,1).d==1;
  const char *const accepted[]={
    "gmwRegisterObject","gmwUnregisterObject","gmwRegisterGroup","gmwUnregisterGroup",
    "gmwSetParameter","gmwSetGlobalParameter","gmwSetSwitch","gmwSetState",
    "gmwSet2DListenerPosition","gmwSet2DPosition","gmwSet3DListenerPosition",
    "gmwSet3DPosition","gmwSetActiveListeners","gmwPostTrigger","gmwProcess"
  };
  for(size_t index=0;index<sizeof accepted/sizeof accepted[0];index++){
    GmlVal result=gml_builtin_call(&vm,accepted[index],NULL,0);
    ok=ok && result.t==V_REAL && result.d==1;
  }
  GmlVal error=gml_builtin_call(&vm,"gmwGetError",NULL,0);
  GmlVal parameter=gml_builtin_call(&vm,"gmwGetParameter",NULL,0);
  ok=ok && error.t==V_STR && !strcmp(error.s,"") &&
    parameter.t==V_REAL && parameter.d==0;
  size_t state_size=gml_vm_state_size(&vm),written=0,used=0;
  uint8_t *state=malloc(state_size?state_size:1u);
  ok=ok && state && gml_vm_state_save(&vm,state,state_size,&written) &&
    written==state_size && gml_builtin_call(&vm,"gmwUnloadBank",&name,1).d==1;
  unsigned char changed[sizeof bank];
  memcpy(changed,bank,sizeof changed);
  changed[17]^=1u;
  file=fopen(path,"wb");
  ok=ok && file && fwrite(changed,1,sizeof changed,file)==sizeof changed;
  if(file && fclose(file)!=0) ok=0;
  ok=ok && !gml_vm_state_load(&vm,state,written,&used);
  file=fopen(path,"wb");
  ok=ok && file && fwrite(bank,1,sizeof bank,file)==sizeof bank;
  if(file && fclose(file)!=0) ok=0;
  ok=ok &&
    gml_vm_state_load(&vm,state,written,&used) && used==written &&
    gml_builtin_call(&vm,"gmwUnloadBank",&name,1).d==1;
  free(state);
  gml_vm_free(&vm);
  gml_audio_free(audio);
  unlink(path); rmdir(directory);
  if(!ok) fprintf(stderr,"portable Wwise bank/state fixture failed\n");
  return ok;
}
