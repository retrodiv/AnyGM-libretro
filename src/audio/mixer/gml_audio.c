/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* Software mixer for AUDO/SOND sounds and FMOD bank voices.
 * WAV PCM is referenced in place. Embedded and grouped OGG data is decoded
 * on first playback; external OGG registration decodes immediately.
 */
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "deps/stb_vorbis.c"
#undef STB_VORBIS_NO_STDIO
#undef STB_VORBIS_NO_PUSHDATA_API
#include "gml_audio.h"
#include "gml_fmod.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

static uint32_t rd32(const uint8_t *d, uint32_t o){ return d[o]|d[o+1]<<8|d[o+2]<<16|(uint32_t)d[o+3]<<24; }
static uint16_t rd16(const uint8_t *d, uint32_t o){ return (uint16_t)(d[o]|d[o+1]<<8); }
static float    rdf32(const uint8_t *d, uint32_t o){ float f; memcpy(&f,d+o,4); return f; }

typedef struct {
  const int16_t *pcm; uint32_t nval; int channels; float vol; double gain, pitch; int16_t *own;
  const uint8_t *ogg; uint32_t ogg_len; int ogg_failed;
} GmlSound; /* own!=NULL if malloc'd OGG decode */
typedef struct { int snd, loop, active, paused, id; double pos, gain, pitch; } GmlVoice;

#define GML_MAX_VOICES 32
#define GML_AUDIO_BUS_GAIN 0.55
#define GML_MAX_AUDIOGROUPS 17
struct GmlAudio {
  GmlWin *win;
  GmlSound *snd; int n_snd, n_base_snd;   /* n_base_snd = SOND resources; caster OGG appended after */
  GmlVoice voice[GML_MAX_VOICES];
  int paused;          /* audio_pause_all: freeze all voices (keep position) */
  int next_voice_id;
  int channel_num;     /* requested GM audio_channel_num; playback uses min(requested, GML_MAX_VOICES) */
  double master_gain;
  GmlFmodBanks *fmod;  /* optional FMOD Studio bank set; see gml_fmod.c */
  /* external audio: audiogroup<N>.dat blobs (streamed music groups) + loose sound files */
  uint8_t *grp_data[GML_MAX_AUDIOGROUPS]; uint32_t grp_audo_off[GML_MAX_AUDIOGROUPS], grp_n[GML_MAX_AUDIOGROUPS];
  uint8_t **extbuf; int n_ext;
};

static int audio_voice_limit(GmlAudio *a){
  if(!a || a->channel_num<=0) return 0;
  return a->channel_num<GML_MAX_VOICES ? a->channel_num : GML_MAX_VOICES;
}

/* Load an indexed audiogroup<N>.dat FORM/AUDO container on demand. */
static int audio_group_load_dat(GmlAudio *a, int g){
  if(g<=0 || g>=GML_MAX_AUDIOGROUPS) return 0;
  if(a->grp_data[g]) return a->grp_n[g]>0;
  a->grp_n[g]=0; a->grp_data[g]=(uint8_t*)1;   /* mark tried (failure keeps 1 so we don't retry) */
  char path[600];
  snprintf(path,sizeof path,"%s/audiogroup%d.dat",a->win->content_dir[0]?a->win->content_dir:".",g);
  FILE *f=fopen(path,"rb"); if(!f) return 0;
  fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
  if(sz<16){ fclose(f); return 0; }
  uint8_t *buf=malloc((size_t)sz);
  if(!buf || fread(buf,1,(size_t)sz,f)!=(size_t)sz){ free(buf); fclose(f); return 0; }
  fclose(f);
  if(memcmp(buf,"FORM",4) || memcmp(buf+8,"AUDO",4)){ free(buf); return 0; }
  a->grp_data[g]=buf;
  a->grp_audo_off[g]=16;                        /* FORM(8) + "AUDO"+size(8) -> count at +16 */
  a->grp_n[g]=rd32(buf,16);
  return a->grp_n[g]>0;
}
GmlAudio *gml_audio_create(GmlWin *win){
  GmlAudio *a=calloc(1,sizeof(GmlAudio)); a->win=win; a->next_voice_id=1000000; a->channel_num=128; a->master_gain=1.0;
  a->fmod=gml_fmod_banks_load(win->content_dir);   /* load an optional FMOD bank set */
  const uint8_t *d=win->data;
  const GmlChunk *sc=gml_chunk(win,"SOND"), *ac=gml_chunk(win,"AUDO");
  if(!sc||!ac) return a;
  uint32_t ns=rd32(d,sc->off), na=rd32(d,ac->off);
  a->n_snd=(int)ns; a->snd=calloc(ns?ns:1,sizeof(GmlSound));
  for(uint32_t i=0;i<ns;i++){
    uint32_t p=rd32(d,sc->off+4+i*4);
    a->snd[i].gain=1.0;
    a->snd[i].pitch=1.0;
    a->snd[i].vol=rdf32(d,p+20);                  /* SOND: vol(+20 f), pitch(+24 f), group(+28 i), audoid(+32 i) */
    int32_t group=(int32_t)rd32(d,p+28);
    int32_t audoid=(int32_t)rd32(d,p+32);
    a->snd[i].channels=2;  /* default stereo; adjusted below */
    if(audoid>=0 && group>0 && group<GML_MAX_AUDIOGROUPS && audio_group_load_dat(a,group)){
      /* grouped sound: blob lives in audiogroup<group>.dat's AUDO */
      const uint8_t *gd=a->grp_data[group];
      if((uint32_t)audoid<a->grp_n[group]){
        uint32_t gap=rd32(gd,a->grp_audo_off[group]+4+(uint32_t)audoid*4);
        uint32_t gblen=rd32(gd,gap), gbase=gap+4;
        if(memcmp(gd+gbase,"RIFF",4)==0){
          uint32_t end=gbase+8+rd32(gd,gbase+4);
          uint32_t o=gbase+12; const uint8_t *pcm=NULL; uint32_t plen=0;
          while(o+8<=end){
            uint32_t csz=rd32(gd,o+4);
            if(memcmp(gd+o,"fmt ",4)==0)      a->snd[i].channels=rd16(gd,o+10);
            else if(memcmp(gd+o,"data",4)==0){ pcm=gd+o+8; plen=csz; }
            o+=8+csz+(csz&1);
          }
          if(pcm){ a->snd[i].pcm=(const int16_t*)pcm; a->snd[i].nval=plen/2; }
        } else if(memcmp(gd+gbase,"OggS",4)==0){
          a->snd[i].ogg=gd+gbase; a->snd[i].ogg_len=gblen;
        }
      }
      continue;
    }
    if(audoid<0){
      /* Resolve the loose filename from SOND +12 beside the data file.
       * Retain the compressed bytes for lazy OGG decoding. */
      const char *fn=gml_str_by_ptr(win,rd32(d,p+12));
      size_t fl=fn?strlen(fn):0;
      if(fn && fl>4 && !strcmp(fn+fl-4,".ogg")){
        char path[600];
        snprintf(path,sizeof path,"%s/%s",win->content_dir[0]?win->content_dir:".",fn);
        FILE *f=fopen(path,"rb");
        if(f){ fseek(f,0,SEEK_END); long szf=ftell(f); fseek(f,0,SEEK_SET);
          if(szf>4){ uint8_t *buf=malloc((size_t)szf);
            if(buf && fread(buf,1,(size_t)szf,f)==(size_t)szf && !memcmp(buf,"OggS",4)){
              uint8_t **ne=realloc(a->extbuf,(a->n_ext+1)*sizeof(*ne));
              if(ne){ a->extbuf=ne; a->extbuf[a->n_ext++]=buf;
                a->snd[i].ogg=buf; a->snd[i].ogg_len=(uint32_t)szf; buf=NULL; } }
            free(buf); }
          fclose(f); } }
      continue;
    }
    if((uint32_t)audoid>=na) continue;
    uint32_t ap=rd32(d,ac->off+4+audoid*4);       /* AUDO blob: len(u32) + bytes */
    uint32_t blen=rd32(d,ap), base=ap+4;
    if(base+12>win->size) continue;
    /* uncompressed RIFF/WAV */
    if(memcmp(d+base,"RIFF",4)==0){
      uint32_t end=base+8+rd32(d,base+4); if(end>base+blen) end=base+blen;
      uint32_t o=base+12; const uint8_t *pcm=NULL; uint32_t plen=0;
      while(o+8<=end){
        uint32_t csz=rd32(d,o+4);
        if(memcmp(d+o,"fmt ",4)==0)      a->snd[i].channels=rd16(d,o+10);
        else if(memcmp(d+o,"data",4)==0){ pcm=d+o+8; plen=csz; }
        o+=8+csz+(csz&1);
      }
      if(pcm){ a->snd[i].pcm=(const int16_t*)pcm; a->snd[i].nval=plen/2; }
    }
    /* OGG Vorbis: keep compressed until playback. */
    else if(memcmp(d+base,"OggS",4)==0){
      a->snd[i].ogg=d+base;
      a->snd[i].ogg_len=blen;
    }
  }
  a->n_base_snd=a->n_snd;
  return a;
}
struct GmlFmodBanks *gml_audio_get_fmod(GmlAudio *a){ return a?a->fmod:NULL; }

void gml_audio_free(GmlAudio *a){ if(!a) return;
  gml_fmod_banks_free(a->fmod);
  for(int i=0;i<a->n_snd;i++) free(a->snd[i].own);
  for(int g=1;g<GML_MAX_AUDIOGROUPS;g++) if(a->grp_data[g] && a->grp_data[g]!=(uint8_t*)1) free(a->grp_data[g]);
  for(int e=0;e<a->n_ext;e++) free(a->extbuf[e]);
  free(a->extbuf);
  free(a->snd); free(a); }

/* Decode an external OGG blob immediately and register its PCM as a sound.
 * Reuse a freed external slot when available and return the sound handle. */
int gml_audio_add_ogg(GmlAudio *a, const uint8_t *ogg, int len){
  if(!a || !ogg || len<=0) return -1;
  int ch=0, rate=0; int16_t *out=NULL;
  int nsamp=stb_vorbis_decode_memory(ogg,len,&ch,&rate,&out);
  (void)rate;
  if(nsamp<=0 || !out){ free(out); return -1; }
  int slot=-1;                                   /* reuse a freed caster slot */
  for(int i=a->n_base_snd;i<a->n_snd;i++)
    if(!a->snd[i].pcm && !a->snd[i].own && !a->snd[i].ogg){ slot=i; break; }
  if(slot<0){
    GmlSound *ns=realloc(a->snd,(size_t)(a->n_snd+1)*sizeof(GmlSound));
    if(!ns){ free(out); return -1; }
    a->snd=ns; slot=a->n_snd++;
  }
  memset(&a->snd[slot],0,sizeof(GmlSound));
  a->snd[slot].pcm=out; a->snd[slot].own=out;
  a->snd[slot].nval=(uint32_t)(nsamp*(ch>0?ch:1));
  a->snd[slot].channels=ch>0?ch:1;
  a->snd[slot].vol=1.0f; a->snd[slot].gain=1.0; a->snd[slot].pitch=1.0;
  if(getenv("GML_LOG_AUDIO")){ int live=0; for(int i=a->n_base_snd;i<a->n_snd;i++) if(a->snd[i].own) live++;
    fprintf(stderr,"[add_ogg] slot=%d n_snd=%d live=%d nval=%u\n",slot,a->n_snd,live,a->snd[slot].nval); }
  return slot;
}
void gml_audio_caster_free(GmlAudio *a, int handle){
  if(!a || handle<a->n_base_snd || handle>=a->n_snd) return;
  gml_audio_stop(a,handle);
  free(a->snd[handle].own);
  memset(&a->snd[handle],0,sizeof(GmlSound));   /* slot becomes reusable */
}
void gml_audio_caster_free_all(GmlAudio *a){
  if(!a) return;
  for(int i=a->n_base_snd;i<a->n_snd;i++) gml_audio_caster_free(a,i);
}

static int sound_ensure_pcm(GmlSound *s){
  if(!s) return 0;
  if(s->pcm) return 1;
  if(!s->ogg || s->ogg_failed) return 0;
  int ch=0, rate=0; int16_t *out=NULL;
  int nsamp=stb_vorbis_decode_memory(s->ogg,(int)s->ogg_len,&ch,&rate,&out);
  (void)rate;
  if(nsamp<=0 || !out){ s->ogg_failed=1; return 0; }
  s->pcm=out;
  s->nval=(uint32_t)(nsamp*ch);
  s->channels=ch>0?ch:1;
  s->own=out;
  return 1;
}
static int audio_voice_prepare(GmlAudio *a, GmlVoice *v){
  if(!a || !v || !v->active) return 0;
  if(v->snd<0 || v->snd>=a->n_snd){ v->active=0; return 0; }
  if(!isfinite(v->pos) || v->pos<0.0) v->pos=0.0;
  if(!isfinite(v->gain) || v->gain<0.0) v->gain=1.0;
  if(!isfinite(v->pitch) || v->pitch<=0.0) v->pitch=1.0;
  GmlSound *s=&a->snd[v->snd];
  if(!sound_ensure_pcm(s)){ v->active=0; return 0; }
  if(s->nval==0){ v->active=0; return 0; }
  if(v->pos>(double)s->nval){
    if(v->loop) v->pos=fmod(v->pos,(double)s->nval);
    else { v->active=0; return 0; }
  }
  return 1;
}

static int voice_matches(GmlAudio *a, GmlVoice *v, int target){
  if(!a || !v || !v->active) return 0;
  if(target>=1000000) return v->id==target;
  return v->snd==target;
}
int gml_audio_play(GmlAudio *a, int snd, int loop){
  if(!a||snd<0||snd>=a->n_snd||!sound_ensure_pcm(&a->snd[snd])) return -1;
  int limit=audio_voice_limit(a); if(limit<=0) return -1;
  int slot=-1; for(int i=0;i<limit;i++) if(!a->voice[i].active){ slot=i; break; }
  if(slot<0) slot=0;
  int id=a->next_voice_id++;
  if(a->next_voice_id<1000000) a->next_voice_id=1000000;
  double gain=a->snd[snd].gain>=0.0?a->snd[snd].gain:1.0;
  double pitch=a->snd[snd].pitch>0.0?a->snd[snd].pitch:1.0;
  a->voice[slot]=(GmlVoice){snd,loop,1,0,id,0.0,gain,pitch};
  return id;
}
void gml_audio_stop(GmlAudio *a, int snd){
  if(!a) return;
  for(int i=0;i<GML_MAX_VOICES;i++)
    if(voice_matches(a,&a->voice[i],snd)) a->voice[i].active=0;
}
void gml_audio_stop_all(GmlAudio *a){ if(!a) return; for(int i=0;i<GML_MAX_VOICES;i++) a->voice[i].active=0; }
void gml_audio_pause_all(GmlAudio *a, int paused){ if(a) a->paused=paused!=0; }
void gml_audio_pause_sound(GmlAudio *a, int target, int paused){
  if(!a) return;
  for(int i=0;i<GML_MAX_VOICES;i++)
    if(voice_matches(a,&a->voice[i],target)) a->voice[i].paused=paused!=0;
}
int  gml_audio_is_playing(GmlAudio *a, int snd){
  if(!a) return 0;
  for(int i=0;i<GML_MAX_VOICES;i++)
    if(voice_matches(a,&a->voice[i],snd)) return 1;
  return 0;
}
int  gml_audio_voice_paused(GmlAudio *a, int snd){
  if(!a) return 0;
  if(a->paused) return 1;
  for(int i=0;i<GML_MAX_VOICES;i++)
    if(voice_matches(a,&a->voice[i],snd)) return a->voice[i].paused;
  return 0;
}
void gml_audio_set_master_gain(GmlAudio *a, double gain){
  if(!a) return;
  if(gain<0) gain=0;
  a->master_gain=gain;
}
double gml_audio_get_master_gain(GmlAudio *a){
  return a ? a->master_gain : 1.0;
}
void gml_audio_channel_num(GmlAudio *a, int channels){
  if(!a) return;
  if(channels<0) channels=0;
  a->channel_num=channels;
  gml_audio_stop_all(a);
}
int gml_audio_get_channel_num(GmlAudio *a){
  return a ? a->channel_num : 128;
}
void gml_audio_sound_gain(GmlAudio *a, int target, double gain){
  if(!a) return;
  if(gain<0) gain=0;
  if(target>=0 && target<a->n_snd) a->snd[target].gain=gain;
  for(int i=0;i<GML_MAX_VOICES;i++) if(voice_matches(a,&a->voice[i],target)) a->voice[i].gain=gain;
}
double gml_audio_sound_get_gain(GmlAudio *a, int target){
  if(!a) return 1.0;
  if(target>=0 && target<a->n_snd) return a->snd[target].gain;
  for(int i=0;i<GML_MAX_VOICES;i++) if(voice_matches(a,&a->voice[i],target)) return a->voice[i].gain;
  return 1.0;
}
void gml_audio_sound_pitch(GmlAudio *a, int target, double pitch){
  if(!a) return;
  if(pitch<=0) pitch=1.0;
  if(target>=0 && target<a->n_snd) a->snd[target].pitch=pitch;
  for(int i=0;i<GML_MAX_VOICES;i++) if(voice_matches(a,&a->voice[i],target)) a->voice[i].pitch=pitch;
}
double gml_audio_sound_get_pitch(GmlAudio *a, int target){
  if(!a) return 1.0;
  if(target>=0 && target<a->n_snd) return a->snd[target].pitch;
  for(int i=0;i<GML_MAX_VOICES;i++) if(voice_matches(a,&a->voice[i],target)) return a->voice[i].pitch;
  return 1.0;
}
void gml_audio_sound_set_track_position(GmlAudio *a, int target, double seconds){
  if(!a) return;
  if(seconds<0) seconds=0;
  for(int i=0;i<GML_MAX_VOICES;i++) if(voice_matches(a,&a->voice[i],target)){
    GmlSound *s=&a->snd[a->voice[i].snd]; int ch=s->channels>0?s->channels:1;
    double pos=seconds*44100.0*ch;
    if(pos<0) pos=0;
    if(pos>(double)s->nval) pos=(double)s->nval;
    a->voice[i].pos=pos;
  }
}
double gml_audio_sound_get_track_position(GmlAudio *a, int target){
  if(!a) return 0.0;
  for(int i=0;i<GML_MAX_VOICES;i++) if(voice_matches(a,&a->voice[i],target)){
    GmlSound *s=&a->snd[a->voice[i].snd]; int ch=s->channels>0?s->channels:1;
    return a->voice[i].pos/(44100.0*ch);
  }
  return 0.0;
}

static double pcm_lerp(const int16_t *pcm, uint32_t nval, uint32_t i0, uint32_t i1, double frac){
  double a=(i0<nval)?pcm[i0]:0.0;
  double b=(i1<nval)?pcm[i1]:a;
  return a + (b-a)*frac;
}
static int16_t audio_soft_clip(int32_t v){
  double x=(double)v;
  const double knee=30000.0, span=2767.0;
  if(x>knee) x=knee + span*(1.0 - exp(-(x-knee)/span));
  else if(x<-knee) x=-knee - span*(1.0 - exp(-(-x-knee)/span));
  if(x>32767.0) return 32767;
  if(x<-32768.0) return -32768;
  return (int16_t)lrint(x);
}
static void audio_mix_audo(GmlAudio *a, int16_t *out, int frames){
  if(!a||a->paused) return;   /* paused: emit silence, voices keep their position */
  int32_t stack_mix[4096];
  int nvals=frames*2;
  int32_t *mix = nvals <= (int)(sizeof(stack_mix)/sizeof(stack_mix[0])) ? stack_mix : calloc((size_t)nvals,sizeof(*mix));
  if(!mix) return;
  if(mix==stack_mix) memset(mix,0,(size_t)nvals*sizeof(*mix));
  int limit=audio_voice_limit(a);
  for(int v=0;v<limit;v++){
    GmlVoice *vo=&a->voice[v]; if(!vo->active || vo->paused) continue;
    if(!audio_voice_prepare(a,vo)) continue;
    GmlSound *s=&a->snd[vo->snd];
    double vol=s->vol*vo->gain*a->master_gain; int ch=s->channels;
    for(int f=0;f<frames;f++){
      if(vo->pos>=s->nval){
        if(vo->loop && s->nval>0) vo->pos=fmod(vo->pos,(double)s->nval);
        else { vo->active=0; break; }
      }
      double l,r;
      if(ch>=2){
        double fp=vo->pos*0.5;
        uint32_t fr=(uint32_t)floor(fp);
        double frac=fp-(double)fr;
        uint32_t i0=fr*2, i1=i0+2;
        l=pcm_lerp(s->pcm,s->nval,i0,i1,frac);
        r=pcm_lerp(s->pcm,s->nval,i0+1,i1+1,frac);
        vo->pos+=2.0*vo->pitch;
      } else {
        uint32_t i0=(uint32_t)floor(vo->pos);
        double frac=vo->pos-(double)i0;
        l=r=pcm_lerp(s->pcm,s->nval,i0,i0+1,frac);
        vo->pos+=vo->pitch;
      }
      mix[f*2]   += (int32_t)lrint(l*vol);
      mix[f*2+1] += (int32_t)lrint(r*vol);
    }
  }
  for(int i=0;i<nvals;i++)
    out[i]=audio_soft_clip((int32_t)lrint((double)mix[i]*GML_AUDIO_BUS_GAIN));
  if(mix!=stack_mix) free(mix);
}

/* Mix AUDO/SOND first, then add FMOD voices. Pausing the AUDO bus
 * does not pause FMOD voices; their controls are separate. */
void gml_audio_mix(GmlAudio *a, int16_t *out, int frames){
  memset(out,0,(size_t)frames*2*sizeof(int16_t));
  if(a && !a->paused) audio_mix_audo(a,out,frames);
  if(a && a->fmod) gml_fmod_mix(a->fmod,out,frames,44100);
}

typedef struct { uint8_t *data; size_t cap, pos; int ok; } AudW;
typedef struct { const uint8_t *data; size_t cap, pos; int ok; } AudR;
static void aw_raw(AudW *s, const void *p, size_t n){
  if(s->data){ if(s->pos+n<=s->cap) memcpy(s->data+s->pos,p,n); else s->ok=0; }
  s->pos+=n;
}
static void ar_raw(AudR *s, void *p, size_t n){
  if(s->pos+n<=s->cap) memcpy(p,s->data+s->pos,n);
  else { memset(p,0,n); s->ok=0; }
  s->pos+=n;
}
static void aw_i32(AudW *s, int v){ int32_t x=(int32_t)v; aw_raw(s,&x,sizeof(x)); }
static void aw_u32(AudW *s, uint32_t v){ aw_raw(s,&v,sizeof(v)); }
static void aw_d(AudW *s, double v){ aw_raw(s,&v,sizeof(v)); }
static int ar_i32(AudR *s){ int32_t v=0; ar_raw(s,&v,sizeof(v)); return (int)v; }
static uint32_t ar_u32(AudR *s){ uint32_t v=0; ar_raw(s,&v,sizeof(v)); return v; }
static double ar_d(AudR *s){ double v=0; ar_raw(s,&v,sizeof(v)); return v; }

static void audio_state_write(AudW *s, GmlAudio *a){
  aw_u32(s,0x35445541u); /* AUD5 */
  aw_i32(s,a?a->paused:0);
  aw_i32(s,a?a->next_voice_id:1000000);
  aw_d(s,a?a->master_gain:1.0);
  aw_i32(s,a?a->channel_num:128);
  for(int i=0;i<GML_MAX_VOICES;i++){
    GmlVoice v=a?a->voice[i]:(GmlVoice){0};
    aw_i32(s,v.snd); aw_i32(s,v.loop); aw_i32(s,v.active); aw_i32(s,v.paused); aw_i32(s,v.id);
    aw_d(s,v.pos); aw_d(s,v.gain); aw_d(s,v.pitch);
  }
  aw_i32(s,a?a->n_snd:0);
  if(a) for(int i=0;i<a->n_snd;i++){ aw_d(s,a->snd[i].gain); aw_d(s,a->snd[i].pitch); }
}
size_t gml_audio_state_size(GmlAudio *a){ AudW s={0}; s.ok=1; audio_state_write(&s,a); return s.pos; }
int gml_audio_state_save(GmlAudio *a, void *data, size_t len, size_t *written){
  AudW s={(uint8_t*)data,len,0,1}; audio_state_write(&s,a); if(written) *written=s.pos; return s.ok && s.pos<=len;
}
int gml_audio_state_load(GmlAudio *a, const void *data, size_t len, size_t *used){
  AudR s={(const uint8_t*)data,len,0,1};
  uint32_t magic=ar_u32(&s); if((magic!=0x31445541u && magic!=0x32445541u && magic!=0x33445541u && magic!=0x34445541u && magic!=0x35445541u) || !s.ok) return 0;
  int paused=ar_i32(&s);
  int next_voice_id=(magic==0x32445541u || magic==0x33445541u || magic==0x34445541u || magic==0x35445541u)?ar_i32(&s):1000000;
  double master_gain=(magic==0x33445541u || magic==0x34445541u || magic==0x35445541u)?ar_d(&s):1.0;
  int channel_num=(magic==0x34445541u || magic==0x35445541u)?ar_i32(&s):128;
  GmlVoice tmp[GML_MAX_VOICES];
  int snd_count=a?a->n_snd:0;
  double *snd_gain=NULL, *snd_pitch=NULL;
  if(snd_count>0){
    snd_gain=malloc((size_t)snd_count*sizeof(*snd_gain));
    snd_pitch=malloc((size_t)snd_count*sizeof(*snd_pitch));
    if(!snd_gain || !snd_pitch){ free(snd_gain); free(snd_pitch); return 0; }
    for(int i=0;i<snd_count;i++){ snd_gain[i]=1.0; snd_pitch[i]=1.0; }
  }
  for(int i=0;i<GML_MAX_VOICES;i++){
    memset(&tmp[i],0,sizeof(tmp[i]));
    tmp[i].snd=ar_i32(&s); tmp[i].loop=ar_i32(&s); tmp[i].active=ar_i32(&s);
    if(magic==0x32445541u || magic==0x33445541u || magic==0x34445541u || magic==0x35445541u){
      tmp[i].paused=ar_i32(&s); tmp[i].id=ar_i32(&s);
      tmp[i].pos=ar_d(&s); tmp[i].gain=ar_d(&s); tmp[i].pitch=ar_d(&s);
    } else {
      tmp[i].pos=ar_u32(&s); tmp[i].gain=1.0; tmp[i].pitch=1.0; tmp[i].id=next_voice_id++;
    }
    if(!a || tmp[i].snd<0 || tmp[i].snd>=a->n_snd) tmp[i].active=0;
    if(tmp[i].id<1000000) tmp[i].id=next_voice_id++;
    if(!isfinite(tmp[i].pos) || tmp[i].pos<0.0) tmp[i].pos=0.0;
    if(!isfinite(tmp[i].gain) || tmp[i].gain<0.0) tmp[i].gain=1.0;
    if(!isfinite(tmp[i].pitch) || tmp[i].pitch<=0.0) tmp[i].pitch=1.0;
    if(a && tmp[i].active) audio_voice_prepare(a,&tmp[i]);
  }
  if(magic==0x35445541u){
    int stored=ar_i32(&s);
    for(int i=0;i<stored;i++){
      double g=ar_d(&s), p=ar_d(&s);
      if(i<snd_count){
        snd_gain[i]=g>=0.0?g:1.0;
        snd_pitch[i]=p>0.0?p:1.0;
      }
    }
  }
  if(!s.ok){ free(snd_gain); free(snd_pitch); return 0; }
  if(master_gain<0) master_gain=1.0;
  if(channel_num<0) channel_num=0;
  if(a){
    a->paused=paused!=0; a->next_voice_id=next_voice_id<1000000?1000000:next_voice_id; a->master_gain=master_gain; a->channel_num=channel_num; memcpy(a->voice,tmp,sizeof(tmp));
    for(int i=0;i<a->n_snd;i++){ a->snd[i].gain=snd_gain?snd_gain[i]:1.0; a->snd[i].pitch=snd_pitch?snd_pitch[i]:1.0; }
  }
  free(snd_gain); free(snd_pitch);
  if(used) *used=s.pos;
  return s.pos<=len;
}
