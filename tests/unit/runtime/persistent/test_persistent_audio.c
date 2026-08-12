/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_audio.h"
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
