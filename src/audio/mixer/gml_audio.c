/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* gml_audio.c — software mixer for AUDO/SOND. Handles uncompressed RIFF/WAV and OGG Vorbis
 * (GMS1.4 stores music/ambience as OGG). Decoded PCM is kept in memory for fast mixing. */
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "deps/stb_vorbis.c"
#undef STB_VORBIS_NO_STDIO
#undef STB_VORBIS_NO_PUSHDATA_API
#include "gml_audio.h"
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *d, uint32_t o){ return d[o]|d[o+1]<<8|d[o+2]<<16|(uint32_t)d[o+3]<<24; }
static uint16_t rd16(const uint8_t *d, uint32_t o){ return (uint16_t)(d[o]|d[o+1]<<8); }
static float    rdf32(const uint8_t *d, uint32_t o){ float f; memcpy(&f,d+o,4); return f; }

typedef struct { const int16_t *pcm; uint32_t nval; int channels; float vol; int16_t *own; } GmlSound; /* own!=NULL if malloc'd OGG decode */
typedef struct { int snd, loop, active; uint32_t pos; } GmlVoice;

#define GML_MAX_VOICES 32
struct GmlAudio {
  GmlWin *win;
  GmlSound *snd; int n_snd;
  GmlVoice voice[GML_MAX_VOICES];
  int paused;          /* audio_pause_all: freeze all voices (keep position) */
};

GmlAudio *gml_audio_create(GmlWin *win){
  GmlAudio *a=calloc(1,sizeof(GmlAudio)); a->win=win;
  const uint8_t *d=win->data;
  const GmlChunk *sc=gml_chunk(win,"SOND"), *ac=gml_chunk(win,"AUDO");
  if(!sc||!ac) return a;
  uint32_t ns=rd32(d,sc->off), na=rd32(d,ac->off);
  a->n_snd=(int)ns; a->snd=calloc(ns?ns:1,sizeof(GmlSound));
  for(uint32_t i=0;i<ns;i++){
    uint32_t p=rd32(d,sc->off+4+i*4);
    a->snd[i].vol=rdf32(d,p+20);                  /* SOND: ...,volume(20 f),...,audoid(32 i) */
    int32_t audoid=(int32_t)rd32(d,p+32);
    a->snd[i].channels=2;  /* default stereo; adjusted below */
    if(audoid<0||(uint32_t)audoid>=na) continue;
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
    /* OGG Vorbis (GMS1.4 music/ambience) — decode to PCM on load */
    else if(memcmp(d+base,"OggS",4)==0){
      int ch, rate; int16_t *out=NULL;
      int nsamp=stb_vorbis_decode_memory(d+base,(int)blen,&ch,&rate,&out);
      if(nsamp>0 && out){
        a->snd[i].pcm=out; a->snd[i].nval=(uint32_t)(nsamp*ch);
        a->snd[i].channels=ch; a->snd[i].own=out;
      }
    }
  }
  return a;
}
void gml_audio_free(GmlAudio *a){ if(!a) return; for(int i=0;i<a->n_snd;i++) free(a->snd[i].own); free(a->snd); free(a); }

void gml_audio_play(GmlAudio *a, int snd, int loop){
  if(!a||snd<0||snd>=a->n_snd||!a->snd[snd].pcm) return;
  int slot=0; for(int i=0;i<GML_MAX_VOICES;i++) if(!a->voice[i].active){ slot=i; break; }
  a->voice[slot]=(GmlVoice){snd,loop,1,0};
}
void gml_audio_stop(GmlAudio *a, int snd){
  if(!a) return; for(int i=0;i<GML_MAX_VOICES;i++) if(a->voice[i].active&&a->voice[i].snd==snd) a->voice[i].active=0;
}
void gml_audio_stop_all(GmlAudio *a){ if(!a) return; for(int i=0;i<GML_MAX_VOICES;i++) a->voice[i].active=0; }
void gml_audio_pause_all(GmlAudio *a, int paused){ if(a) a->paused=paused!=0; }
int  gml_audio_is_playing(GmlAudio *a, int snd){
  if(!a) return 0; for(int i=0;i<GML_MAX_VOICES;i++) if(a->voice[i].active&&a->voice[i].snd==snd) return 1; return 0;
}

void gml_audio_mix(GmlAudio *a, int16_t *out, int frames){
  memset(out,0,(size_t)frames*2*sizeof(int16_t));
  if(!a||a->paused) return;   /* paused: emit silence, voices keep their position */
  for(int v=0;v<GML_MAX_VOICES;v++){
    GmlVoice *vo=&a->voice[v]; if(!vo->active) continue;
    GmlSound *s=&a->snd[vo->snd]; if(!s->pcm){ vo->active=0; continue; }
    float vol=s->vol; int ch=s->channels;
    for(int f=0;f<frames;f++){
      if(vo->pos>=s->nval){ if(vo->loop) vo->pos=0; else { vo->active=0; break; } }
      int32_t l,r;
      if(ch>=2){ l=s->pcm[vo->pos]; r=(vo->pos+1<s->nval)?s->pcm[vo->pos+1]:l; vo->pos+=2; }
      else     { l=r=s->pcm[vo->pos]; vo->pos+=1; }
      int32_t ol=out[f*2]+(int32_t)(l*vol), orr=out[f*2+1]+(int32_t)(r*vol);
      out[f*2]   = ol> 32767?32767 : ol< -32768?-32768 : (int16_t)ol;
      out[f*2+1] = orr>32767?32767 : orr<-32768?-32768 : (int16_t)orr;
    }
  }
}
