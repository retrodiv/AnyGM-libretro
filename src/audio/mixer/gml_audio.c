/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* Software mixer for AUDO/SOND sounds and FMOD bank voices.
 * WAV PCM is referenced in place. Embedded and grouped OGG/MP3 data is decoded
 * mostly on first playback, with initial warming; external OGG registration decodes immediately.
 */
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "deps/stb_vorbis.c"
#undef STB_VORBIS_NO_STDIO
#undef STB_VORBIS_NO_PUSHDATA_API
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#include "deps/minimp3_ex.h"
#include "gml_audio.h"
#include "gml_fmod.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

static uint32_t rd32(const uint8_t *d, uint32_t o){ return d[o]|d[o+1]<<8|d[o+2]<<16|(uint32_t)d[o+3]<<24; }
static uint16_t rd16(const uint8_t *d, uint32_t o){ return (uint16_t)(d[o]|d[o+1]<<8); }
static float    rdf32(const uint8_t *d, uint32_t o){ float f; memcpy(&f,d+o,4); return f; }
static int      is_mp3_blob(const uint8_t *d, uint32_t len){ return d && len>=10 && mp3dec_detect_buf(d,len)==0; }

typedef struct {
  const int16_t *pcm; uint32_t nval; int channels, sample_rate; float vol; double gain, pitch; int16_t *own;
  const uint8_t *ogg; uint32_t ogg_len; int ogg_failed;
  const uint8_t *mp3; uint32_t mp3_len; int mp3_failed;
  double length_seconds, loop_start_seconds; int length_known, group;
} GmlSound; /* own!=NULL if a compressed blob was decoded to PCM */
typedef struct {
  int snd, loop, active, paused, id, emitter;
  double pos, gain, pitch, loop_start, spatial_gain, pan;
} GmlVoice;

#define GML_MAX_VOICES 32
#define GML_AUDIO_BUS_GAIN 0.55
#define GML_MAX_AUDIOGROUPS 64
struct GmlAudio {
  GmlWin *win;
  GmlSound *snd; int n_snd, n_base_snd;   /* n_base_snd = SOND resources; dynamic sounds follow */
  GmlVoice voice[GML_MAX_VOICES];
  int paused;          /* audio_pause_all: freeze all voices (keep position) */
  int next_voice_id;
  int channel_num;     /* requested GM audio_channel_num; playback uses min(requested, GML_MAX_VOICES) */
  double master_gain;
  double group_gain[GML_MAX_AUDIOGROUPS], group_target[GML_MAX_AUDIOGROUPS];
  int group_fade_frames[GML_MAX_AUDIOGROUPS];
  GmlFmodBanks *fmod;  /* optional FMOD Studio bank set; see gml_fmod.c */
  /* external audio: audiogroup<N>.dat blobs (streamed music groups) + loose sound files */
  uint8_t *grp_data[GML_MAX_AUDIOGROUPS];
  uint32_t grp_size[GML_MAX_AUDIOGROUPS], grp_audo_off[GML_MAX_AUDIOGROUPS], grp_n[GML_MAX_AUDIOGROUPS];
  uint8_t **extbuf; int n_ext;
};

static int audio_voice_limit(GmlAudio *a){
  if(!a || a->channel_num<=0) return 0;
  return a->channel_num<GML_MAX_VOICES ? a->channel_num : GML_MAX_VOICES;
}
static void audio_warm_initial_ogg(GmlAudio *a);

/* Load an indexed audio-group container on demand. */
static const char *audio_group_exact_string(const GmlWin *win, uint32_t ptr){
  if(!win || !win->str_charoff || !win->strs) return NULL;
  int lo=0, hi=win->n_strs-1;
  while(lo<=hi){
    int mid=lo+(hi-lo)/2;
    if(win->str_charoff[mid]==ptr) return win->strs[mid];
    if(win->str_charoff[mid]<ptr) lo=mid+1; else hi=mid-1;
  }
  return NULL;
}

int gml_audio_group_file_path(const GmlWin *win, int g, char *out, size_t out_cap){
  if(!win || !out || out_cap<2 || g<0) return 0;
  const GmlChunk *gc=gml_chunk(win,"AGRP");
  const uint8_t *d=win->data;
  const char *rel=NULL;
  if(gc && gc->off<=win->size && gc->size<=win->size-gc->off && gc->size>=4){
    size_t chunk_end=(size_t)gc->off+gc->size;
    uint32_t n=rd32(d,gc->off);
    uint32_t max_entries=(gc->size-4)/4;
    if(n<=max_entries && (uint32_t)g<n){
      uint32_t rec=rd32(d,gc->off+4+(uint32_t)g*4);
      uint32_t record_size=0;
      if((uint32_t)g+1<n){
        uint32_t next=rd32(d,gc->off+8+(uint32_t)g*4);
        if(next>rec && (size_t)next<=chunk_end) record_size=next-rec;
      } else if(g>0){
        uint32_t prev=rd32(d,gc->off+(uint32_t)g*4);
        if(prev<rec && prev>=gc->off+4+n*4) record_size=rec-prev;
      }
      /* Legacy AGRP records contain one string pointer and are packed four bytes apart.
       * Later records contain {name, custom path} and are eight bytes apart.
       * A one-record table has no neighbour from which to infer its width, so only accept its
       * second word when it is itself an exact STRG pointer. */
      if(rec>=gc->off+4+n*4 && (size_t)rec<=chunk_end && chunk_end-(size_t)rec>=8 &&
         (record_size>=8 || n==1))
        rel=audio_group_exact_string(win,rd32(d,rec+4));
    }
  }
  char fallback[64];
  if(!rel || !rel[0]){
    snprintf(fallback,sizeof fallback,"audiogroup%d.dat",g);
    rel=fallback;
  }
  /* Audio-group paths are relative content paths. Refuse traversal/drive paths and normalize
   * the Windows separator so the same data.win works on libretro's Unix targets. */
  if(rel[0]=='/' || rel[0]=='\\' || strchr(rel,':') || strstr(rel,"..")) return 0;
  size_t base=strlen(win->content_dir[0]?win->content_dir:".");
  size_t nr=strlen(rel);
  if(base+1+nr+1>out_cap) return 0;
  memcpy(out,win->content_dir[0]?win->content_dir:".",base);
  out[base++]='/';
  for(size_t i=0;i<nr;i++) out[base+i]=(rel[i]=='\\')?'/':rel[i];
  out[base+nr]=0;
  return 1;
}
static int audio_group_load_dat(GmlAudio *a, int g){
  if(g<=0 || g>=GML_MAX_AUDIOGROUPS) return 0;
  if(a->grp_data[g]) return a->grp_n[g]>0;
  a->grp_n[g]=0; a->grp_data[g]=(uint8_t*)1;   /* mark tried (failure keeps 1 so we don't retry) */
  char path[600];
  if(!gml_audio_group_file_path(a->win,g,path,sizeof path)) return 0;
  FILE *f=fopen(path,"rb");
  if(!f){ if(getenv("GML_LOG_AUDIO")) fprintf(stderr,"[audio] group=%d open failed: %s\n",g,path); return 0; }
  if(fseek(f,0,SEEK_END)){ fclose(f); return 0; }
  long sz=ftell(f);
  if(sz<20 || (uint64_t)sz>UINT32_MAX || fseek(f,0,SEEK_SET)){ fclose(f); return 0; }
  uint8_t *buf=malloc((size_t)sz);
  if(!buf || fread(buf,1,(size_t)sz,f)!=(size_t)sz){ free(buf); fclose(f); return 0; }
  fclose(f);
  if(memcmp(buf,"FORM",4) || memcmp(buf+8,"AUDO",4)){ free(buf); return 0; }
  uint32_t count=rd32(buf,16);
  if(count>((uint32_t)sz-20u)/4u){ free(buf); return 0; }
  a->grp_data[g]=buf;
  a->grp_size[g]=(uint32_t)sz;
  a->grp_audo_off[g]=16;                        /* FORM(8) + "AUDO"+size(8) -> count at +16 */
  a->grp_n[g]=count;
  if(getenv("GML_LOG_AUDIO")) fprintf(stderr,"[audio] group=%d loaded=%u path=%s\n",g,count,path);
  return a->grp_n[g]>0;
}
GmlAudio *gml_audio_create(GmlWin *win){
  GmlAudio *a=calloc(1,sizeof(GmlAudio)); a->win=win; a->next_voice_id=1000000; a->channel_num=128; a->master_gain=1.0;
  for(int g=0;g<GML_MAX_AUDIOGROUPS;g++) a->group_gain[g]=a->group_target[g]=1.0;
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
    a->snd[i].sample_rate=44100;
    a->snd[i].vol=rdf32(d,p+20);                  /* SOND: vol(+20 f), pitch(+24 f), group(+28 i), audoid(+32 i) */
    int32_t group=(int32_t)rd32(d,p+28);
    a->snd[i].group=(group>=0 && group<GML_MAX_AUDIOGROUPS)?group:0;
    int32_t audoid=(int32_t)rd32(d,p+32);
    a->snd[i].channels=2;  /* default stereo; adjusted below */
    if(audoid>=0 && group>0 && group<GML_MAX_AUDIOGROUPS && audio_group_load_dat(a,group)){
      /* grouped sound: blob lives in audiogroup<group>.dat's AUDO */
      const uint8_t *gd=a->grp_data[group];
      if((uint32_t)audoid<a->grp_n[group]){
        uint32_t gap=rd32(gd,a->grp_audo_off[group]+4+(uint32_t)audoid*4);
        if(gap>a->grp_size[group]-4u) continue;
        uint32_t gblen=rd32(gd,gap), gbase=gap+4;
        if(gblen>a->grp_size[group]-gbase || gblen<4) continue;
        if(gblen>=12 && memcmp(gd+gbase,"RIFF",4)==0){
          uint64_t declared_end=(uint64_t)gbase+8u+rd32(gd,gbase+4);
          uint32_t end=declared_end<(uint64_t)gbase+gblen ? (uint32_t)declared_end : gbase+gblen;
          uint32_t o=gbase+12; const uint8_t *pcm=NULL; uint32_t plen=0;
          while(o+8<=end){
            uint32_t csz=rd32(gd,o+4);
            if(csz>end-o-8) break;
            if(memcmp(gd+o,"fmt ",4)==0 && csz>=8){
              a->snd[i].channels=rd16(gd,o+10);
              a->snd[i].sample_rate=(int)rd32(gd,o+12);
            }
            else if(memcmp(gd+o,"data",4)==0){ pcm=gd+o+8; plen=csz; }
            o+=8+csz+(csz&1);
          }
          if(pcm){ a->snd[i].pcm=(const int16_t*)pcm; a->snd[i].nval=plen/2; }
        } else if(memcmp(gd+gbase,"OggS",4)==0){
          a->snd[i].ogg=gd+gbase; a->snd[i].ogg_len=gblen;
        } else if(is_mp3_blob(gd+gbase,gblen)){
          a->snd[i].mp3=gd+gbase; a->snd[i].mp3_len=gblen;
        }
      }
      continue;
    }
    if(audoid<0){
      /* Resolve the loose filename from SOND +12 beside the data file.
       * Retain the compressed bytes for lazy OGG/MP3 decoding. */
      const char *fn=gml_str_by_ptr(win,rd32(d,p+12));
      size_t fl=fn?strlen(fn):0;
      if(fn && fl>4 && (!strcmp(fn+fl-4,".ogg") || !strcmp(fn+fl-4,".mp3"))){
        char path[600];
        snprintf(path,sizeof path,"%s/%s",win->content_dir[0]?win->content_dir:".",fn);
        FILE *f=fopen(path,"rb");
        if(f){ fseek(f,0,SEEK_END); long szf=ftell(f); fseek(f,0,SEEK_SET);
          if(szf>4){ uint8_t *buf=malloc((size_t)szf);
            if(buf && fread(buf,1,(size_t)szf,f)==(size_t)szf &&
               (!memcmp(buf,"OggS",4) || is_mp3_blob(buf,(uint32_t)szf))){
              uint8_t **ne=realloc(a->extbuf,(a->n_ext+1)*sizeof(*ne));
              if(ne){ a->extbuf=ne; a->extbuf[a->n_ext++]=buf;
                if(!memcmp(buf,"OggS",4)){ a->snd[i].ogg=buf; a->snd[i].ogg_len=(uint32_t)szf; }
                else { a->snd[i].mp3=buf; a->snd[i].mp3_len=(uint32_t)szf; }
                buf=NULL; } }
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
        if(memcmp(d+o,"fmt ",4)==0){
          a->snd[i].channels=rd16(d,o+10);
          a->snd[i].sample_rate=(int)rd32(d,o+12);
        }
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
    else if(is_mp3_blob(d+base,blen)){
      a->snd[i].mp3=d+base;
      a->snd[i].mp3_len=blen;
    }
  }
  a->n_base_snd=a->n_snd;
  audio_warm_initial_ogg(a);
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
  a->snd[slot].sample_rate=rate>0?rate:44100;
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
  if(s->ogg && !s->ogg_failed){
    int ch=0, rate=0; int16_t *out=NULL;
    int nsamp=stb_vorbis_decode_memory(s->ogg,(int)s->ogg_len,&ch,&rate,&out);
    if(nsamp<=0 || !out){ s->ogg_failed=1; return 0; }
    s->pcm=out;
    s->nval=(uint32_t)(nsamp*ch);
    s->channels=ch>0?ch:1;
    s->sample_rate=rate>0?rate:44100;
    s->own=out;
    return 1;
  }
  if(s->mp3 && !s->mp3_failed){
    mp3dec_t dec;
    mp3dec_file_info_t info;
    memset(&dec,0,sizeof(dec));
    memset(&info,0,sizeof(info));
    if(mp3dec_load_buf(&dec,s->mp3,s->mp3_len,&info,NULL,NULL) || !info.buffer || !info.samples){
      free(info.buffer);
      s->mp3_failed=1;
      return 0;
    }
    s->pcm=info.buffer;
    s->nval=info.samples>UINT32_MAX ? UINT32_MAX : (uint32_t)info.samples;
    s->channels=info.channels>0?info.channels:1;
    s->sample_rate=info.hz>0?info.hz:44100;
    s->own=info.buffer;
    return 1;
  }
  return 0;
}
int gml_audio_warm_sound(GmlAudio *a, int snd){
  if(!a || snd<0 || snd>=a->n_snd) return 0;
  GmlSound *s=&a->snd[snd];
  int had_pcm=s->pcm!=NULL;
  if(!sound_ensure_pcm(s)) return 0;
  if(!had_pcm && getenv("GML_LOG_AUDIO"))
    fprintf(stderr,"[audio] warm_sound sound=%d ogg=%u pcm=%u\n",snd,s->ogg_len,s->nval);
  return 1;
}
static uint32_t audio_initial_ogg_warm_budget(void){
  const char *e=getenv("GML_AUDIO_WARM_OGG_BYTES");
  if(e && (!strcmp(e,"0") || !strcmp(e,"off") || !strcmp(e,"false"))) return 0;
  if(e && *e){
    long v=strtol(e,NULL,0);
    return v>0 ? (uint32_t)v : 0;
  }
  return 2u*1024u*1024u;
}
static void audio_warm_initial_ogg(GmlAudio *a){
  if(!a || !a->snd) return;
  uint32_t budget=audio_initial_ogg_warm_budget();
  if(!budget) return;
  int warmed=0;
  uint32_t used=0;
  for(int i=0;i<a->n_snd;i++){
    GmlSound *s=&a->snd[i];
    if(s->pcm || !s->ogg || s->ogg_failed) continue;
    if(used>=budget) break;
    if(s->ogg_len>budget-used) continue;
    if(sound_ensure_pcm(s)){
      used += s->ogg_len;
      warmed++;
      if(getenv("GML_LOG_AUDIO"))
        fprintf(stderr,"[audio] warm_ogg sound=%d ogg=%u pcm=%u budget=%u\n",i,s->ogg_len,s->nval,budget);
    }
    if(used>=budget) break;
  }
}
static int audio_voice_prepare(GmlAudio *a, GmlVoice *v){
  if(!a || !v || !v->active) return 0;
  if(v->snd<0 || v->snd>=a->n_snd){ v->active=0; return 0; }
  if(!isfinite(v->pos) || v->pos<0.0) v->pos=0.0;
  if(!isfinite(v->gain) || v->gain<0.0) v->gain=1.0;
  if(!isfinite(v->pitch) || v->pitch<=0.0) v->pitch=1.0;
  if(!isfinite(v->loop_start) || v->loop_start<0.0) v->loop_start=0.0;
  if(!isfinite(v->spatial_gain) || v->spatial_gain<0.0) v->spatial_gain=1.0;
  if(!isfinite(v->pan)) v->pan=0.0;
  if(v->pan<-1.0) v->pan=-1.0; else if(v->pan>1.0) v->pan=1.0;
  GmlSound *s=&a->snd[v->snd];
  if(!sound_ensure_pcm(s)){ v->active=0; return 0; }
  if(s->nval==0){ v->active=0; return 0; }
  if(v->loop_start>(double)s->nval) v->loop_start=0.0;
  if(v->pos>(double)s->nval){
    if(v->loop){ double span=(double)s->nval-v->loop_start;
      v->pos=span>0.0?v->loop_start+fmod(fmax(0.0,v->pos-v->loop_start),span):v->loop_start; }
    else { v->active=0; return 0; }
  }
  return 1;
}

static int voice_matches(GmlAudio *a, GmlVoice *v, int target){
  if(!a || !v || !v->active) return 0;
  if(target>=1000000) return v->id==target;
  return v->snd==target;
}
int gml_audio_play_on(GmlAudio *a, int snd, int loop, int emitter){
  if(!a||snd<0||snd>=a->n_snd||!sound_ensure_pcm(&a->snd[snd])) return -1;
  int limit=audio_voice_limit(a); if(limit<=0) return -1;
  int slot=-1; for(int i=0;i<limit;i++) if(!a->voice[i].active){ slot=i; break; }
  if(slot<0) slot=0;
  int id=a->next_voice_id++;
  if(a->next_voice_id<1000000) a->next_voice_id=1000000;
  double gain=a->snd[snd].gain>=0.0?a->snd[snd].gain:1.0;
  double pitch=a->snd[snd].pitch>0.0?a->snd[snd].pitch:1.0;
  GmlSound *s=&a->snd[snd];
  int ch=s->channels>0?s->channels:1, rate=s->sample_rate>0?s->sample_rate:44100;
  double loop_start=s->loop_start_seconds*(double)rate*ch;
  if(loop_start<0.0 || loop_start>=(double)s->nval) loop_start=0.0;
  a->voice[slot]=(GmlVoice){.snd=snd,.loop=loop,.active=1,.paused=0,.id=id,.emitter=emitter,
    .pos=0.0,.gain=gain,.pitch=pitch,.loop_start=loop_start,.spatial_gain=1.0,.pan=0.0};
  return id;
}
int gml_audio_play(GmlAudio *a, int snd, int loop){ return gml_audio_play_on(a,snd,loop,-1); }
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
int gml_audio_exists(GmlAudio *a, int target){
  if(!a) return 0;
  if(target>=1000000){
    for(int i=0;i<GML_MAX_VOICES;i++)
      if(a->voice[i].active && a->voice[i].id==target) return 1;
    return 0;
  }
  if(target<0 || target>=a->n_snd) return 0;
  if(target<a->n_base_snd) return 1;
  GmlSound *sound=&a->snd[target];
  return sound->pcm || sound->own || sound->ogg || sound->mp3 || sound->nval>0;
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
void gml_audio_group_gain(GmlAudio *a, int group, double gain, int milliseconds){
  if(!a || group<0 || group>=GML_MAX_AUDIOGROUPS) return;
  if(!isfinite(gain) || gain<0.0) gain=0.0;
  a->group_target[group]=gain;
  if(milliseconds<=0){
    a->group_gain[group]=gain;
    a->group_fade_frames[group]=0;
    return;
  }
  double frames=ceil((double)milliseconds*44.1);
  a->group_fade_frames[group]=frames>(double)INT32_MAX?INT32_MAX:(int)frames;
}
double gml_audio_group_get_gain(GmlAudio *a, int group){
  return a && group>=0 && group<GML_MAX_AUDIOGROUPS ? a->group_gain[group] : 1.0;
}
void gml_audio_group_stop_all(GmlAudio *a, int group){
  if(!a || group<0 || group>=GML_MAX_AUDIOGROUPS) return;
  for(int i=0;i<GML_MAX_VOICES;i++){
    GmlVoice *v=&a->voice[i];
    if(v->active && v->snd>=0 && v->snd<a->n_snd && a->snd[v->snd].group==group) v->active=0;
  }
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
double gml_audio_sound_length(GmlAudio *a, int sound){
  if(!a) return 0.0;
  if(sound>=1000000){
    for(int i=0;i<GML_MAX_VOICES;i++) if(a->voice[i].active && a->voice[i].id==sound){
      sound=a->voice[i].snd;
      break;
    }
  }
  if(sound<0 || sound>=a->n_snd) return 0.0;
  GmlSound *s=&a->snd[sound];
  if(s->length_known) return s->length_seconds;

  double seconds=0.0;
  if(s->pcm && s->nval){
    int channels=s->channels>0?s->channels:1;
    int rate=s->sample_rate>0?s->sample_rate:44100;
    seconds=(double)s->nval/((double)channels*(double)rate);
  } else if(s->ogg && s->ogg_len){
    int error=0;
    stb_vorbis *stream=stb_vorbis_open_memory(s->ogg,(int)s->ogg_len,&error,NULL);
    if(stream){
      seconds=stb_vorbis_stream_length_in_seconds(stream);
      stb_vorbis_close(stream);
    }
  } else if(s->mp3 && s->mp3_len){
    mp3dec_ex_t stream;
    if(mp3dec_ex_open_buf(&stream,s->mp3,s->mp3_len,MP3D_SEEK_TO_SAMPLE)==0){
      int channels=stream.info.channels>0?stream.info.channels:1;
      int rate=stream.info.hz>0?stream.info.hz:44100;
      seconds=(double)stream.samples/((double)channels*(double)rate);
      mp3dec_ex_close(&stream);
    }
  }
  if(!isfinite(seconds) || seconds<0.0) seconds=0.0;
  s->length_seconds=seconds;
  s->length_known=1;
  if(getenv("GML_LOG_AUDIO"))
    fprintf(stderr,"[audio] sound_length id=%d seconds=%.6f\n",sound,seconds);
  return seconds;
}
void gml_audio_sound_set_track_position(GmlAudio *a, int target, double seconds){
  if(!a) return;
  if(seconds<0) seconds=0;
  for(int i=0;i<GML_MAX_VOICES;i++) if(voice_matches(a,&a->voice[i],target)){
    GmlSound *s=&a->snd[a->voice[i].snd]; int ch=s->channels>0?s->channels:1;
    int rate=s->sample_rate>0?s->sample_rate:44100;
    double pos=seconds*(double)rate*ch;
    if(pos<0) pos=0;
    if(pos>(double)s->nval) pos=(double)s->nval;
    a->voice[i].pos=pos;
  }
}
double gml_audio_sound_get_track_position(GmlAudio *a, int target){
  if(!a) return 0.0;
  for(int i=0;i<GML_MAX_VOICES;i++) if(voice_matches(a,&a->voice[i],target)){
    GmlSound *s=&a->snd[a->voice[i].snd]; int ch=s->channels>0?s->channels:1;
    int rate=s->sample_rate>0?s->sample_rate:44100;
    return a->voice[i].pos/((double)rate*ch);
  }
  return 0.0;
}
void gml_audio_sound_loop_start(GmlAudio *a, int target, double seconds){
  if(!a) return;
  if(!isfinite(seconds) || seconds<0.0) seconds=0.0;
  if(target>=0 && target<a->n_snd) a->snd[target].loop_start_seconds=seconds;
  for(int i=0;i<GML_MAX_VOICES;i++) if(voice_matches(a,&a->voice[i],target)){
    GmlSound *s=&a->snd[a->voice[i].snd]; int ch=s->channels>0?s->channels:1;
    int rate=s->sample_rate>0?s->sample_rate:44100;
    double pos=seconds*(double)rate*ch;
    a->voice[i].loop_start=(pos>=0.0 && pos<(double)s->nval)?pos:0.0;
  }
}
void gml_audio_emitter_mix(GmlAudio *a, int emitter, double gain, double pan){
  if(!a) return;
  if(!isfinite(gain) || gain<0.0) gain=0.0;
  if(!isfinite(pan)) pan=0.0;
  if(pan<-1.0) pan=-1.0; else if(pan>1.0) pan=1.0;
  for(int i=0;i<GML_MAX_VOICES;i++) if(a->voice[i].active && a->voice[i].emitter==emitter){
    a->voice[i].spatial_gain=gain; a->voice[i].pan=pan;
  }
}
void gml_audio_voice_spatial(GmlAudio *a, int target, double gain, double pan){
  if(!a) return;
  if(!isfinite(gain) || gain<0.0) gain=0.0;
  if(!isfinite(pan)) pan=0.0;
  if(pan<-1.0) pan=-1.0; else if(pan>1.0) pan=1.0;
  for(int i=0;i<GML_MAX_VOICES;i++) if(voice_matches(a,&a->voice[i],target)){
    a->voice[i].spatial_gain=gain; a->voice[i].pan=pan;
  }
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
    double vol=s->vol*vo->gain*vo->spatial_gain*a->master_gain; int ch=s->channels;
    int group=s->group>=0 && s->group<GML_MAX_AUDIOGROUPS?s->group:0;
    double group_start=a->group_gain[group], group_target=a->group_target[group];
    int group_remaining=a->group_fade_frames[group];
    double group_step=group_remaining>0?(group_target-group_start)/(double)group_remaining:0.0;
    double pan=vo->pan, left_pan=pan>0.0?1.0-pan:1.0, right_pan=pan<0.0?1.0+pan:1.0;
    double rate_scale=(double)(s->sample_rate>0?s->sample_rate:44100)/44100.0;
    for(int f=0;f<frames;f++){
      if(vo->pos>=s->nval){
        if(vo->loop && s->nval>0){ double span=(double)s->nval-vo->loop_start;
          vo->pos=span>0.0?vo->loop_start+fmod(fmax(0.0,vo->pos-vo->loop_start),span):vo->loop_start; }
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
        vo->pos+=2.0*vo->pitch*rate_scale;
      } else {
        uint32_t i0=(uint32_t)floor(vo->pos);
        double frac=vo->pos-(double)i0;
        l=r=pcm_lerp(s->pcm,s->nval,i0,i0+1,frac);
        vo->pos+=vo->pitch*rate_scale;
      }
      double group_level=group_remaining>f?group_start+group_step*(double)f:group_target;
      mix[f*2]   += (int32_t)lrint(l*vol*group_level*left_pan);
      mix[f*2+1] += (int32_t)lrint(r*vol*group_level*right_pan);
    }
  }
  for(int g=0;g<GML_MAX_AUDIOGROUPS;g++) if(a->group_fade_frames[g]>0){
    int advance=frames<a->group_fade_frames[g]?frames:a->group_fade_frames[g];
    double step=(a->group_target[g]-a->group_gain[g])/(double)a->group_fade_frames[g];
    a->group_gain[g]+=step*(double)advance;
    a->group_fade_frames[g]-=advance;
    if(!a->group_fade_frames[g]) a->group_gain[g]=a->group_target[g];
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
  aw_u32(s,0x37445541u); /* AUD7: audio-group gain/fade state */
  aw_i32(s,a?a->paused:0);
  aw_i32(s,a?a->next_voice_id:1000000);
  aw_d(s,a?a->master_gain:1.0);
  aw_i32(s,a?a->channel_num:128);
  for(int i=0;i<GML_MAX_VOICES;i++){
    GmlVoice v=a?a->voice[i]:(GmlVoice){0};
    aw_i32(s,v.snd); aw_i32(s,v.loop); aw_i32(s,v.active); aw_i32(s,v.paused); aw_i32(s,v.id);
    aw_d(s,v.pos); aw_d(s,v.gain); aw_d(s,v.pitch);
    aw_d(s,v.loop_start); aw_d(s,v.spatial_gain); aw_d(s,v.pan); aw_i32(s,v.emitter);
  }
  aw_i32(s,a?a->n_snd:0);
  if(a) for(int i=0;i<a->n_snd;i++){
    aw_d(s,a->snd[i].gain); aw_d(s,a->snd[i].pitch); aw_d(s,a->snd[i].loop_start_seconds);
  }
  aw_i32(s,GML_MAX_AUDIOGROUPS);
  for(int g=0;g<GML_MAX_AUDIOGROUPS;g++){
    aw_d(s,a?a->group_gain[g]:1.0); aw_d(s,a?a->group_target[g]:1.0);
    aw_i32(s,a?a->group_fade_frames[g]:0);
  }
}
size_t gml_audio_state_size(GmlAudio *a){ AudW s={0}; s.ok=1; audio_state_write(&s,a); return s.pos; }
int gml_audio_state_save(GmlAudio *a, void *data, size_t len, size_t *written){
  AudW s={(uint8_t*)data,len,0,1}; audio_state_write(&s,a); if(written) *written=s.pos; return s.ok && s.pos<=len;
}
int gml_audio_state_load(GmlAudio *a, const void *data, size_t len, size_t *used){
  AudR s={(const uint8_t*)data,len,0,1};
  uint32_t magic=ar_u32(&s); if((magic!=0x31445541u && magic!=0x32445541u && magic!=0x33445541u && magic!=0x34445541u && magic!=0x35445541u && magic!=0x36445541u && magic!=0x37445541u) || !s.ok) return 0;
  int v2=magic>=0x32445541u, v3=magic>=0x33445541u, v4=magic>=0x34445541u;
  int v5=magic>=0x35445541u, v6=magic>=0x36445541u, v7=magic>=0x37445541u;
  int paused=ar_i32(&s);
  int next_voice_id=v2?ar_i32(&s):1000000;
  double master_gain=v3?ar_d(&s):1.0;
  int channel_num=v4?ar_i32(&s):128;
  GmlVoice tmp[GML_MAX_VOICES];
  int snd_count=a?a->n_snd:0;
  double *snd_gain=NULL, *snd_pitch=NULL, *snd_loop=NULL;
  if(snd_count>0){
    snd_gain=malloc((size_t)snd_count*sizeof(*snd_gain));
    snd_pitch=malloc((size_t)snd_count*sizeof(*snd_pitch));
    snd_loop=malloc((size_t)snd_count*sizeof(*snd_loop));
    if(!snd_gain || !snd_pitch || !snd_loop){ free(snd_gain); free(snd_pitch); free(snd_loop); return 0; }
    for(int i=0;i<snd_count;i++){ snd_gain[i]=1.0; snd_pitch[i]=1.0; snd_loop[i]=0.0; }
  }
  for(int i=0;i<GML_MAX_VOICES;i++){
    memset(&tmp[i],0,sizeof(tmp[i]));
    tmp[i].spatial_gain=1.0; tmp[i].emitter=-1;
    tmp[i].snd=ar_i32(&s); tmp[i].loop=ar_i32(&s); tmp[i].active=ar_i32(&s);
    if(v2){
      tmp[i].paused=ar_i32(&s); tmp[i].id=ar_i32(&s);
      tmp[i].pos=ar_d(&s); tmp[i].gain=ar_d(&s); tmp[i].pitch=ar_d(&s);
      if(v6){ tmp[i].loop_start=ar_d(&s); tmp[i].spatial_gain=ar_d(&s);
        tmp[i].pan=ar_d(&s); tmp[i].emitter=ar_i32(&s); }
      else { tmp[i].loop_start=0.0; tmp[i].spatial_gain=1.0; tmp[i].pan=0.0; tmp[i].emitter=-1; }
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
  if(v5){
    int stored=ar_i32(&s);
    for(int i=0;i<stored;i++){
      double g=ar_d(&s), p=ar_d(&s);
      double l=v6?ar_d(&s):0.0;
      if(i<snd_count){
        snd_gain[i]=g>=0.0?g:1.0;
        snd_pitch[i]=p>0.0?p:1.0;
        snd_loop[i]=l>=0.0?l:0.0;
      }
    }
  }
  double group_gain[GML_MAX_AUDIOGROUPS], group_target[GML_MAX_AUDIOGROUPS];
  int group_fade_frames[GML_MAX_AUDIOGROUPS];
  for(int g=0;g<GML_MAX_AUDIOGROUPS;g++){
    group_gain[g]=group_target[g]=1.0; group_fade_frames[g]=0;
  }
  if(v7){
    int stored=ar_i32(&s);
    if(stored<0 || stored>4096){ free(snd_gain); free(snd_pitch); free(snd_loop); return 0; }
    for(int g=0;g<stored;g++){
      double gain=ar_d(&s), target=ar_d(&s); int remaining=ar_i32(&s);
      if(g<GML_MAX_AUDIOGROUPS){
        group_gain[g]=isfinite(gain)&&gain>=0.0?gain:1.0;
        group_target[g]=isfinite(target)&&target>=0.0?target:group_gain[g];
        group_fade_frames[g]=remaining>0?remaining:0;
      }
    }
  }
  if(!s.ok){ free(snd_gain); free(snd_pitch); free(snd_loop); return 0; }
  if(master_gain<0) master_gain=1.0;
  if(channel_num<0) channel_num=0;
  if(a){
    a->paused=paused!=0; a->next_voice_id=next_voice_id<1000000?1000000:next_voice_id; a->master_gain=master_gain; a->channel_num=channel_num; memcpy(a->voice,tmp,sizeof(tmp));
    memcpy(a->group_gain,group_gain,sizeof group_gain);
    memcpy(a->group_target,group_target,sizeof group_target);
    memcpy(a->group_fade_frames,group_fade_frames,sizeof group_fade_frames);
    for(int i=0;i<a->n_snd;i++){ a->snd[i].gain=snd_gain?snd_gain[i]:1.0;
      a->snd[i].pitch=snd_pitch?snd_pitch[i]:1.0; a->snd[i].loop_start_seconds=snd_loop?snd_loop[i]:0.0; }
  }
  free(snd_gain); free(snd_pitch); free(snd_loop);
  if(used) *used=s.pos;
  return s.pos<=len;
}
