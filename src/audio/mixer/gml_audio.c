/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* gml_audio.c — software mixer for AUDO/SOND. Handles uncompressed RIFF/WAV, OGG Vorbis,
 * and MP3. WAV data is referenced in-place; compressed data is mostly decoded lazily on
 * first playback so large soundtracks do not consume decoded-PCM memory at boot. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef STB_VORBIS_NO_STDIO
#undef STB_VORBIS_NO_PUSHDATA_API
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#include "minimp3_ex.h"
#include "gml_audio.h"
#include "gml_fmod.h"
#include "gml_hash.h"
#include "anygm_compatibility.h"
#include "anygm_host.h"
#include "anygm_vfs.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

static uint32_t rd32(const uint8_t *d, uint32_t o){ return d[o]|d[o+1]<<8|d[o+2]<<16|(uint32_t)d[o+3]<<24; }
static uint16_t rd16(const uint8_t *d, uint32_t o){ return (uint16_t)(d[o]|d[o+1]<<8); }
static float    rdf32(const uint8_t *d, uint32_t o){ float f; memcpy(&f,d+o,4); return f; }
static int      is_mp3_blob(const uint8_t *d, uint32_t len){ return d && len>=10 && mp3dec_detect_buf(d,len)==0; }

typedef struct {
  const int16_t *pcm; uint32_t nval; int channels, sample_rate; float vol;
  double gain, pitch; int16_t *own; uint8_t *encoded_own;
  const uint8_t *ogg; uint32_t ogg_len; int ogg_failed;
  const uint8_t *mp3; uint32_t mp3_len; int mp3_failed;
  const char *external_filename;
  /* Embedded compressed AUDO fallback when a streamed sound has no loose file.
   * Loose files keep precedence; -1 means no validated fallback blob. */
  int fallback_audoid;
  double length_seconds, loop_start_seconds, gain_target;
  int length_known, group, gain_fade_frames, default_loop, external_tried;
  int bytes_per_second;
  uint32_t external_type;
  uint8_t content_sha256[32];
  int content_hash_known;
} GmlSound; /* own is decoded/copied PCM; encoded_own backs a dynamic compressed blob */
typedef struct {
  int snd, loop, active, paused, id, emitter;
  double pos, gain, pitch, loop_start, spatial_gain, pan;
} GmlVoice;

#define GML_MAX_VOICES 32
#define GML_MAX_AUDIOGROUPS 64
#define GML_LOOSE_AUDIO_BYTES_MAX (64u*1024u*1024u)
#define GML_DYNAMIC_SOUND_LIMIT 65536
#define GML_EXTERNAL_GROUP_FLAG UINT32_C(0x80000000)
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
  uint32_t audo_off, audo_n;   /* data.win AUDO chunk, for streamed-sound embedded fallbacks */
};

void gml_audio_rebind_content(GmlAudio *audio,GmlWin *win){
  if(!audio) return;
  audio->win=win;
  gml_fmod_banks_rebind_host(audio->fmod,win?win->host:NULL);
}

static const char *audio_setting(const GmlAudio *audio,const char *name){
  return anygm_host_development_setting(audio&&audio->win?audio->win->host:NULL,name);
}

static int audio_voice_limit(GmlAudio *a){
  if(!a || a->channel_num<=0) return 0;
  return a->channel_num<GML_MAX_VOICES ? a->channel_num : GML_MAX_VOICES;
}
static void audio_warm_initial_ogg(GmlAudio *a);

/* Load "<content_dir>/audiogroup<N>.dat" on demand: a bare FORM container holding one AUDO
 * chunk. Studio exports can place streamed music and ambience outside data.win. A grouped SOND
 * whose blob is unresolved must not be decoded as audio payload. */
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

/* An audio-group index selects a sidecar only when AGRP declares it.
 * A declared group with a missing sidecar remains unresolved. */
static int audio_group_declared(const GmlWin *win,int g){
  if(!win || g<0) return 0;
  const GmlChunk *gc=gml_chunk(win,"AGRP");
  if(!gc || gc->size<4 || gc->off>win->size || gc->size>win->size-gc->off) return 0;
  uint32_t n=rd32(win->data,gc->off);
  uint32_t max_entries=(gc->size-4)/4;
  if(n>max_entries) return 0;
  return (uint32_t)g<n;
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
       * Extended records contain {name, custom path} and are eight bytes apart.
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
   * the Windows separator so the same data.win works on host's Unix targets. */
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
  uint8_t *buf=NULL;
  size_t size=0;
  if(!anygm_vfs_read_all(a->win?a->win->host:NULL,path,&buf,&size,UINT32_MAX)){
    if(audio_setting(a,"GML_LOG_AUDIO"))
      anygm_host_logf(a && a->win ? a->win->host : NULL,ANYGM_LOG_DEBUG,"[audio] group=%d open failed: %s\n",g,path);
    return 0;
  }
  if(size<20 || size>UINT32_MAX){ free(buf); return 0; }
  uint32_t sz=(uint32_t)size;
  if(memcmp(buf,"FORM",4) || memcmp(buf+8,"AUDO",4)){ free(buf); return 0; }
  uint32_t count=rd32(buf,16);
  if(count>((uint32_t)sz-20u)/4u){ free(buf); return 0; }
  a->grp_data[g]=buf;
  a->grp_size[g]=(uint32_t)sz;
  a->grp_audo_off[g]=16;                        /* FORM(8) + "AUDO"+size(8) -> count at +16 */
  a->grp_n[g]=count;
  if(audio_setting(a,"GML_LOG_AUDIO"))
    anygm_host_logf(a && a->win ? a->win->host : NULL,ANYGM_LOG_DEBUG,"[audio] group=%d loaded=%u path=%s\n",g,count,path);
  return a->grp_n[g]>0;
}
static int audio_sound_load_loose_file(GmlAudio *a,GmlSound *sound){
  const char *filename=sound?sound->external_filename:NULL;
  if(sound && sound->external_tried) return sound->ogg || sound->mp3;
  if(sound) sound->external_tried=1;
  if(!a || !sound || !filename || !filename[0] || filename[0]=='/' || filename[0]=='\\' ||
     strchr(filename,':') || strstr(filename,"..")) return 0;
  size_t length=strlen(filename);
  if(length<=4 || (strcmp(filename+length-4,".ogg") && strcmp(filename+length-4,".mp3")))
    return 0;
  char path[600];
  int written=snprintf(path,sizeof path,"%s/%s",
                       a->win->content_dir[0]?a->win->content_dir:".",filename);
  if(written<0 || (size_t)written>=sizeof path) return 0;
  uint8_t *buffer=NULL;
  size_t size=0;
  if(!anygm_vfs_read_all(a->win->host,path,&buffer,&size,GML_LOOSE_AUDIO_BYTES_MAX)) return 0;
  if(size<=4 || size>UINT32_MAX ||
     (memcmp(buffer,"OggS",4) && !is_mp3_blob(buffer,(uint32_t)size))){
    free(buffer);
    return 0;
  }
  uint8_t **external=realloc(a->extbuf,(size_t)(a->n_ext+1)*sizeof(*external));
  if(!external){ free(buffer); return 0; }
  a->extbuf=external;
  a->extbuf[a->n_ext++]=buffer;
  if(!memcmp(buffer,"OggS",4)){
    sound->ogg=buffer;
    sound->ogg_len=(uint32_t)size;
  } else {
    sound->mp3=buffer;
    sound->mp3_len=(uint32_t)size;
  }
  return 1;
}
/* One RIFF/WAVE header, read once and shared by every container that carries a WAVE. The bit
 * depth and the format tag are part of it: a data chunk addressed as 16-bit samples without
 * asking what it holds turns an 8-bit or an ADPCM effect into full-scale noise, and it does so
 * at exactly the volume the game asked for. */
enum { GML_WAVE_PCM=1, GML_WAVE_MS_ADPCM=2, GML_WAVE_COEF_MAX=16 };
typedef struct {
  const uint8_t *data;                 /* the data chunk's bytes */
  uint32_t bytes;                      /* its size */
  int format, channels, sample_rate, bits, block_align;
  int coef_count;                      /* MS ADPCM predictor coefficients, from the header */
  int16_t coef1[GML_WAVE_COEF_MAX], coef2[GML_WAVE_COEF_MAX];
} GmlWave;

/* The predictor coefficients every MS ADPCM encoder ships with. A file that declares its own in
 * the format chunk overrides these; one that declares none is decoded against them. */
static const int16_t gml_ms_adpcm_default_coef1[7]={256,512,0,192,240,460,392};
static const int16_t gml_ms_adpcm_default_coef2[7]={0,-256,0,64,0,-208,-232};
static const int16_t gml_ms_adpcm_adapt[16]=
  {230,230,230,230,307,409,512,614,768,614,512,409,307,230,230,230};

/* Walk the chunks of one RIFF/WAVE between `base` and `limit`, which is the end of the containing
 * blob rather than whatever the header claims: a declared size larger than the container is a
 * corrupt file, not permission to read past it. */
static int wave_parse(const uint8_t *d,uint32_t base,uint32_t limit,GmlWave *w){
  if(!d || !w || limit<base || limit-base<12u) return 0;
  if(memcmp(d+base,"RIFF",4) || memcmp(d+base+8,"WAVE",4)) return 0;
  memset(w,0,sizeof(*w));
  w->channels=1;
  w->sample_rate=44100;
  uint64_t declared=(uint64_t)base+8u+rd32(d,base+4);
  uint32_t end=declared<(uint64_t)limit ? (uint32_t)declared : limit;
  int have_format=0;
  for(uint32_t o=base+12; o+8<=end;){
    uint32_t size=rd32(d,o+4);
    uint32_t body=o+8;
    if(size>end-body) break;
    if(!memcmp(d+o,"fmt ",4) && size>=16){
      w->format=(int)rd16(d,body);
      w->channels=(int)rd16(d,body+2);
      w->sample_rate=(int)rd32(d,body+4);
      w->block_align=(int)rd16(d,body+12);
      w->bits=(int)rd16(d,body+14);
      if(w->format==GML_WAVE_MS_ADPCM && size>=22){
        int count=(int)rd16(d,body+20);
        if(count>GML_WAVE_COEF_MAX) count=GML_WAVE_COEF_MAX;
        if(count>0 && size>=22u+(uint32_t)count*4u){
          for(int i=0;i<count;i++){
            w->coef1[i]=(int16_t)rd16(d,body+22+(uint32_t)i*4);
            w->coef2[i]=(int16_t)rd16(d,body+24+(uint32_t)i*4);
          }
          w->coef_count=count;
        }
      }
      have_format=1;
    } else if(!memcmp(d+o,"data",4) && !w->data){
      w->data=d+body;
      w->bytes=size;
    }
    uint64_t next=(uint64_t)body+size+(size&1u);
    if(next>end) break;
    o=(uint32_t)next;
  }
  if(w->format==GML_WAVE_MS_ADPCM && !w->coef_count){
    for(int i=0;i<7;i++){
      w->coef1[i]=gml_ms_adpcm_default_coef1[i];
      w->coef2[i]=gml_ms_adpcm_default_coef2[i];
    }
    w->coef_count=7;
  }
  return have_format && w->data!=NULL && w->channels>=1 && w->channels<=2 &&
         w->sample_rate>0 && w->sample_rate<=384000;
}

/* Samples one MS ADPCM block holds per channel. The block opens with seven bytes of state per
 * channel and every remaining nibble is one sample, on top of the two the state already primed. */
static uint32_t ms_adpcm_block_samples(const GmlWave *w){
  if(w->block_align<=7*w->channels) return 0;
  return (uint32_t)((w->block_align-7*w->channels)*8/(4*w->channels))+2u;
}
static int16_t ms_adpcm_sample(int nibble,int *sample1,int *sample2,int *delta,
                               int coef1,int coef2){
  static const int signed_nibble[16]={0,1,2,3,4,5,6,7,-8,-7,-6,-5,-4,-3,-2,-1};
  /* The coefficients come from the file when it carries its own table, so the product is bounded
   * by two full int16 ranges rather than by the standard table's 512. */
  int64_t wide=((int64_t)*sample1*coef1+(int64_t)*sample2*coef2)>>8;
  wide+=(int64_t)signed_nibble[nibble]*(*delta);
  int predictor=wide>32767?32767:(wide<-32768?-32768:(int)wide);
  *sample2=*sample1;
  *sample1=predictor;
  /* The step size only ever triples, so an encoder's own stream stays in the low thousands. The
   * ceiling is here because a corrupt block can ask for that growth without end. */
  int next=(gml_ms_adpcm_adapt[nibble]*(*delta))>>8;
  *delta=next<16?16:(next>(1<<20)?(1<<20):next);
  return (int16_t)predictor;
}
static uint32_t ms_adpcm_decode(const GmlWave *w,int16_t *out,uint32_t capacity){
  int channels=w->channels;
  uint32_t block=(uint32_t)w->block_align;
  uint32_t produced=0;
  for(uint32_t offset=0; block>0 && w->bytes-offset>=block; offset+=block){
    const uint8_t *b=w->data+offset;
    int sample1[2]={0,0}, sample2[2]={0,0}, delta[2]={16,16}, index[2]={0,0};
    uint32_t cursor=0;
    for(int c=0;c<channels;c++){
      int selector=b[cursor++];
      index[c]=selector<w->coef_count?selector:w->coef_count-1;
    }
    for(int c=0;c<channels;c++){ delta[c]=(int16_t)rd16(b,cursor); cursor+=2; }
    for(int c=0;c<channels;c++){ sample1[c]=(int16_t)rd16(b,cursor); cursor+=2; }
    for(int c=0;c<channels;c++){ sample2[c]=(int16_t)rd16(b,cursor); cursor+=2; }
    for(int c=0;c<channels && produced<capacity;c++) out[produced++]=(int16_t)sample2[c];
    for(int c=0;c<channels && produced<capacity;c++) out[produced++]=(int16_t)sample1[c];
    int channel=0;
    for(;cursor<block && produced<capacity;cursor++)
      for(int half=0;half<2 && produced<capacity;half++){
        int nibble=half?(b[cursor]&15):(b[cursor]>>4);
        out[produced++]=ms_adpcm_sample(nibble,&sample1[channel],&sample2[channel],
                                        &delta[channel],w->coef1[index[channel]],
                                        w->coef2[index[channel]]);
        channel=channels>1?channel^1:0;
      }
  }
  return produced;
}

/* One decoded sound may not exceed this. It bounds a corrupt header as much as a long effect:
 * an ADPCM block count is read from the file and the expansion is four times the stored bytes. */
#define GML_WAVE_DECODED_MAX (64u*1024u*1024u)

/* Present a parsed WAVE as the mixer's interleaved 16-bit samples. 16-bit PCM is referenced where
 * it already lies, so the common case still costs nothing; every other encoding is decoded into a
 * buffer the caller owns. Answering 0 leaves the sound silent, which is what an encoding nobody
 * has implemented should sound like. */
static int wave_to_pcm16(const GmlWave *w,const int16_t **pcm,uint32_t *nval,int16_t **owned){
  *pcm=NULL; *nval=0; *owned=NULL;
  if(!w->data || !w->bytes) return 0;
  if(w->format==GML_WAVE_PCM && w->bits==16){
    *pcm=(const int16_t*)w->data;
    *nval=w->bytes/2u;
    return 1;
  }
  uint32_t values=0;
  if(w->format==GML_WAVE_PCM && w->bits==8) values=w->bytes;
  else if(w->format==GML_WAVE_MS_ADPCM && w->bits==4){
    uint32_t per_block=ms_adpcm_block_samples(w);
    uint32_t blocks=w->block_align>0?w->bytes/(uint32_t)w->block_align:0;
    if(!per_block || !blocks) return 0;
    if(per_block>GML_WAVE_DECODED_MAX/((uint32_t)w->channels*blocks)) return 0;
    values=per_block*blocks*(uint32_t)w->channels;
  } else return 0;
  if(!values || values>GML_WAVE_DECODED_MAX/sizeof(int16_t)) return 0;
  int16_t *out=(int16_t*)malloc((size_t)values*sizeof(*out));
  if(!out) return 0;
  if(w->format==GML_WAVE_PCM){
    /* 8-bit WAVE samples are unsigned around a 128 midpoint. */
    for(uint32_t i=0;i<values;i++) out[i]=(int16_t)(((int)w->data[i]-128)*256);
  } else {
    uint32_t produced=ms_adpcm_decode(w,out,values);
    if(!produced){ free(out); return 0; }
    values=produced;
  }
  *pcm=out; *nval=values; *owned=out;
  return 1;
}

static void wave_report_unsupported(GmlAudio *a,const GmlWave *w){
  /* An empty data chunk is an empty sound, which every reading of it plays as silence. Only an
   * encoding carrying samples nobody decodes is worth a line. */
  if(!w->bytes || !audio_setting(a,"GML_LOG_AUDIO")) return;
  anygm_host_logf(a && a->win ? a->win->host : NULL,ANYGM_LOG_DEBUG,
                  "[audio] WAVE format=%d bits=%d is not decoded; the sound stays silent\n",
                  w->format,w->bits);
}

/* Take the samples of one parsed WAVE onto a sound, whatever the container it came from. */
/* Classic eight-bit PCM omits one complete terminal frame after decoding. The branch is limited to classic policy and eight-bit PCM; other depths and modern content retain their full decoded frame count. The channel count is used so multichannel samples remain paired. */
static void sound_take_wave(GmlAudio *a,GmlSound *s,const GmlWave *w){
  s->channels=w->channels;
  s->sample_rate=w->sample_rate;
  const int16_t *pcm=NULL; uint32_t nval=0; int16_t *owned=NULL;
  if(wave_to_pcm16(w,&pcm,&nval,&owned)){
    if(a && w->format==GML_WAVE_PCM && w->bits==8 &&
       anygm_policy_uses_classic_runtime(a->win)){
      uint32_t frame=(uint32_t)(w->channels>0?w->channels:1);
      if(nval>=frame) nval-=frame;   /* a whole frame, so the channels stay paired */
    }
    s->pcm=pcm;
    s->nval=nval;
    if(owned){ free(s->own); s->own=owned; }
  } else wave_report_unsupported(a,w);
}

/* Attach one data.win AUDO blob: RIFF PCM stays raw, OGG and MP3 stay compressed. */
static void audio_sound_attach_embedded(GmlAudio *a,GmlSound *s,uint32_t audoid){
  const GmlWin *win=a->win;
  const uint8_t *d=win->data;
  if(audoid>=a->audo_n) return;
  uint32_t ap=rd32(d,a->audo_off+4+audoid*4);       /* AUDO blob: len(u32) + bytes */
  uint32_t blen=rd32(d,ap), base=ap+4;
  if(base+12>win->size) return;
  /* uncompressed RIFF/WAV */
  if(memcmp(d+base,"RIFF",4)==0){
    uint64_t declared_end=(uint64_t)base+blen;
    uint32_t limit=declared_end<(uint64_t)win->size ? (uint32_t)declared_end : (uint32_t)win->size;
    GmlWave w;
    if(wave_parse(d,base,limit,&w)) sound_take_wave(a,s,&w);
  }
  /* OGG Vorbis stays compressed until playback. */
  else if(memcmp(d+base,"OggS",4)==0){
    s->ogg=d+base;
    s->ogg_len=blen;
  }
  else if(is_mp3_blob(d+base,blen)){
    s->mp3=d+base;
    s->mp3_len=blen;
  }
}
static int audio_sound_load_loose(GmlAudio *a,GmlSound *sound){
  if(!sound) return 0;
  if(sound->ogg || sound->mp3 || sound->pcm) return 1;
  if(audio_sound_load_loose_file(a,sound)) return 1;
  if(sound->fallback_audoid>=0){
    audio_sound_attach_embedded(a,sound,(uint32_t)sound->fallback_audoid);
    sound->fallback_audoid=-1;
    if(sound->ogg || sound->mp3 || sound->pcm){
      anygm_host_logf(a->win?a->win->host:NULL,ANYGM_LOG_DEBUG,
                      "[audio] streamed sound file is absent; playing its embedded blob\n");
      return 1;
    }
  }
  return 0;
}
GmlAudio *gml_audio_create(GmlWin *win){
  GmlAudio *a=calloc(1,sizeof(GmlAudio)); a->win=win; a->next_voice_id=1000000; a->channel_num=128; a->master_gain=1.0;
  for(int g=0;g<GML_MAX_AUDIOGROUPS;g++) a->group_gain[g]=a->group_target[g]=1.0;
  a->fmod=gml_fmod_banks_load(win->host,win->content_dir);   /* optional FMOD bank set */
  const uint8_t *d=win->data;
  const GmlChunk *sc=gml_chunk(win,"SOND"), *ac=gml_chunk(win,"AUDO");
  if(!sc||!ac) return a;
  uint32_t ns=rd32(d,sc->off), na=rd32(d,ac->off);
  a->audo_off=ac->off;
  a->audo_n=na;
  a->n_snd=(int)ns; a->snd=calloc(ns?ns:1,sizeof(GmlSound));
  for(uint32_t i=0;i<ns;i++){
    uint32_t p=rd32(d,sc->off+4+i*4);
    a->snd[i].gain=1.0;
    a->snd[i].gain_target=1.0;
    a->snd[i].pitch=1.0;
    a->snd[i].sample_rate=44100;
    a->snd[i].fallback_audoid=-1;
    a->snd[i].vol=rdf32(d,p+20);                  /* SOND: vol(+20 f), pitch(+24 f), group(+28 i), audoid(+32 i) */
    uint32_t flags=rd32(d,p+4);
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
          GmlWave w;
          if(wave_parse(gd,gbase,gbase+gblen,&w)) sound_take_wave(a,&a->snd[i],&w);
        } else if(memcmp(gd+gbase,"OggS",4)==0){
          a->snd[i].ogg=gd+gbase; a->snd[i].ogg_len=gblen;
        } else if(is_mp3_blob(gd+gbase,gblen)){
          a->snd[i].mp3=gd+gbase; a->snd[i].mp3_len=gblen;
        }
      }
      continue;
    }
    if(!(flags&1u)){
      /* A clear IsEmbedded bit selects the loose path at SOND +12 when present.
       * If absent, a valid AudioID can provide compressed embedded bytes.
       * Only OGG/MP3 qualify; a raw PCM AudioID remains unused for a
       * streamed sound. Audio-group sidecars retain their precedence. */
      const char *filename=gml_str_by_ptr(win,rd32(d,p+12));
      a->snd[i].external_filename=filename;
      /* An undeclared group may use the embedded RIFF blob at AudioID.
       * Keep the default-group raw-PCM refusal unchanged. */
      int undeclared_group=group>0 && !audio_group_declared(win,group);
      if(audoid>=0 && (uint32_t)audoid<na){
        uint32_t fap=rd32(d,ac->off+4+(uint32_t)audoid*4);
        uint32_t fblen=rd32(d,fap), fbase=fap+4;
        if(fbase+12<=win->size && fblen>=4 && fblen<=win->size-fbase &&
           (!memcmp(d+fbase,"OggS",4) || is_mp3_blob(d+fbase,fblen) ||
            (undeclared_group && !memcmp(d+fbase,"RIFF",4))))
          a->snd[i].fallback_audoid=audoid;
      }
      continue;
    }
    if(audoid<0){
      /* Not embedded: SOND +12 names a loose sound file. Load it and decode lazily. */
      a->snd[i].external_filename=gml_str_by_ptr(win,rd32(d,p+12));
      continue;
    }
    if((uint32_t)audoid>=na) continue;
    audio_sound_attach_embedded(a,&a->snd[i],(uint32_t)audoid);
  }
  a->n_base_snd=a->n_snd;
  audio_warm_initial_ogg(a);
  return a;
}
struct GmlFmodBanks *gml_audio_get_fmod(GmlAudio *a){ return a?a->fmod:NULL; }

void gml_audio_free(GmlAudio *a){ if(!a) return;
  gml_fmod_banks_free(a->fmod);
  for(int i=0;i<a->n_snd;i++){
    free(a->snd[i].own);
    free(a->snd[i].encoded_own);
  }
  for(int g=1;g<GML_MAX_AUDIOGROUPS;g++) if(a->grp_data[g] && a->grp_data[g]!=(uint8_t*)1) free(a->grp_data[g]);
  for(int e=0;e<a->n_ext;e++) free(a->extbuf[e]);
  free(a->extbuf);
  free(a->snd); free(a); }

static int audio_dynamic_slot(GmlAudio *a){
  if(!a) return -1;
  int slot=-1;                                   /* reuse a freed caster slot */
  for(int i=a->n_base_snd;i<a->n_snd;i++)
    if(!a->snd[i].pcm && !a->snd[i].own && !a->snd[i].encoded_own &&
       !a->snd[i].ogg && !a->snd[i].mp3 && !a->snd[i].nval){
      slot=i;
      break;
  }
  if(slot<0){
    if(a->n_snd<a->n_base_snd ||
       (uint64_t)(unsigned)(a->n_snd-a->n_base_snd)>=GML_DYNAMIC_SOUND_LIMIT)
      return -1;
    GmlSound *ns=realloc(a->snd,(size_t)(a->n_snd+1)*sizeof(GmlSound));
    if(!ns) return -1;
    a->snd=ns; slot=a->n_snd++;
  }
  memset(&a->snd[slot],0,sizeof(GmlSound));
  a->snd[slot].vol=1.0f;
  a->snd[slot].gain=1.0;
  a->snd[slot].gain_target=1.0;
  a->snd[slot].pitch=1.0;
  a->snd[slot].sample_rate=44100;
  return slot;
}

static int audio_dynamic_slot_at(GmlAudio *a,int requested){
  if(requested<0) return audio_dynamic_slot(a);
  /* An existing content sound may be replaced by its asset index. Only new
   * slots above the original sound range may be created here. */
  if(!a || requested<0 ||
     (requested>=a->n_base_snd &&
      (uint64_t)(unsigned)(requested-a->n_base_snd)>=GML_DYNAMIC_SOUND_LIMIT))
    return -1;
  if(requested<a->n_base_snd && requested>=a->n_snd) return -1;
  if(requested>=a->n_snd){
    size_t count=(size_t)requested+1u;
    GmlSound *sounds=(GmlSound*)realloc(a->snd,count*sizeof(*sounds));
    if(!sounds) return -1;
    memset(sounds+a->n_snd,0,(count-(size_t)a->n_snd)*sizeof(*sounds));
    a->snd=sounds;
    a->n_snd=requested+1;
  } else {
    gml_audio_stop(a,requested);
    free(a->snd[requested].own);
    free(a->snd[requested].encoded_own);
  }
  memset(&a->snd[requested],0,sizeof(a->snd[requested]));
  a->snd[requested].vol=1.0f;
  a->snd[requested].gain=1.0;
  a->snd[requested].gain_target=1.0;
  a->snd[requested].pitch=1.0;
  a->snd[requested].sample_rate=44100;
  return requested;
}

/* Register an external OGG blob (caster_load) as a new sound. Decode eagerly to retain the
 * established caster failure behavior while the generic loose-file path below stays lazy. */
int gml_audio_add_ogg(GmlAudio *a, const uint8_t *ogg, int len){
  if(!a || !ogg || len<=0) return -1;
  int ch=0, rate=0; int16_t *out=NULL;
  int nsamp=stb_vorbis_decode_memory(ogg,len,&ch,&rate,&out);
  if(nsamp<=0 || !out || ch<=0 || ch>2 || rate<=0 || rate>384000 ||
     (uint64_t)(unsigned)nsamp*(uint64_t)(unsigned)ch>UINT32_MAX){
    free(out);
    return -1;
  }
  int slot=audio_dynamic_slot(a);
  if(slot<0){ free(out); return -1; }
  a->snd[slot].pcm=out; a->snd[slot].own=out;
  a->snd[slot].nval=(uint32_t)((unsigned)nsamp*(unsigned)ch);
  a->snd[slot].channels=ch;
  a->snd[slot].sample_rate=rate;
  a->snd[slot].length_seconds=(double)nsamp/(double)rate;
  a->snd[slot].length_known=1;
  if(a->snd[slot].length_seconds>0.0){
    double estimate=(double)len/a->snd[slot].length_seconds;
    if(isfinite(estimate) && estimate>0.0)
      a->snd[slot].bytes_per_second=estimate>(double)INT_MAX
        ? INT_MAX : (int)llround(estimate);
  }
  gml_sha256(ogg,(size_t)len,a->snd[slot].content_sha256);
  a->snd[slot].content_hash_known=1;
  if(audio_setting(a,"GML_LOG_AUDIO")){ int live=0; for(int i=a->n_base_snd;i<a->n_snd;i++) if(a->snd[i].own) live++;
    anygm_host_logf(a && a->win ? a->win->host : NULL,ANYGM_LOG_DEBUG,"[add_ogg] slot=%d n_snd=%d live=%d nval=%u\n",slot,a->n_snd,live,a->snd[slot].nval); }
  return slot;
}

static int audio_add_encoded_at(GmlAudio *a,const uint8_t *encoded,int len,int requested,
                                const uint8_t expected_sha256[32]){
  if(!a || !encoded || len<=0) return -1;
  const uint8_t *pcm=NULL;
  uint32_t pcm_bytes=0;
  int channels=0;
  int sample_rate=0;
  int bytes_per_second=0;
  int pcm_bits=0;
  enum { DYNAMIC_WAV, DYNAMIC_OGG, DYNAMIC_MP3 } kind;
  double length_seconds=0.0;

  if(len>=12 && !memcmp(encoded,"RIFF",4) && !memcmp(encoded+8,"WAVE",4)){
    uint64_t declared_end=8u+(uint64_t)rd32(encoded,4);
    uint32_t end=declared_end<(uint64_t)(uint32_t)len
      ? (uint32_t)declared_end : (uint32_t)len;
    uint16_t format=0,bits=0,block_align=0;
    for(uint32_t offset=12;offset<=end && end-offset>=8;){
      uint32_t chunk_size=rd32(encoded,offset+4);
      uint32_t body=offset+8;
      if(chunk_size>end-body) return -1;
      if(!memcmp(encoded+offset,"fmt ",4) && chunk_size>=16){
        format=rd16(encoded,body);
        channels=(int)rd16(encoded,body+2);
        sample_rate=(int)rd32(encoded,body+4);
        bytes_per_second=(int)rd32(encoded,body+8);
        block_align=rd16(encoded,body+12);
        bits=rd16(encoded,body+14);
      } else if(!memcmp(encoded+offset,"data",4) && !pcm){
        pcm=encoded+body;
        pcm_bytes=chunk_size;
      }
      uint64_t next=(uint64_t)body+chunk_size+(chunk_size&1u);
      if(next>end) break;
      offset=(uint32_t)next;
    }
    if(format!=1 || (channels!=1 && channels!=2) ||
       sample_rate<=0 || sample_rate>384000 || (bits!=8 && bits!=16) ||
       block_align!=(uint16_t)(channels*(bits/8)) || !pcm ||
       pcm_bytes<block_align || pcm_bytes%block_align) return -1;
    kind=DYNAMIC_WAV;
    pcm_bits=bits;
    length_seconds=(double)(pcm_bytes/(uint32_t)(bits/8))/
                   ((double)channels*(double)sample_rate);
  } else if(len>=4 && !memcmp(encoded,"OggS",4)){
    int error=0;
    stb_vorbis *stream=stb_vorbis_open_memory(encoded,len,&error,NULL);
    if(!stream) return -1;
    stb_vorbis_info info=stb_vorbis_get_info(stream);
    channels=info.channels;
    sample_rate=(int)info.sample_rate;
    length_seconds=stb_vorbis_stream_length_in_seconds(stream);
    stb_vorbis_close(stream);
    if(channels<=0 || sample_rate<=0) return -1;
    kind=DYNAMIC_OGG;
  } else if(is_mp3_blob(encoded,(uint32_t)len)){
    mp3dec_ex_t stream;
    if(mp3dec_ex_open_buf(&stream,encoded,(size_t)len,MP3D_SEEK_TO_SAMPLE))
      return -1;
    channels=stream.info.channels;
    sample_rate=stream.info.hz;
    if(channels>0 && sample_rate>0)
      length_seconds=(double)stream.samples/
                     ((double)channels*(double)sample_rate);
    mp3dec_ex_close(&stream);
    if(channels<=0 || sample_rate<=0) return -1;
    kind=DYNAMIC_MP3;
  } else {
    return -1;
  }

  uint8_t digest[32];
  gml_sha256(encoded,(size_t)len,digest);
  if(expected_sha256 && memcmp(digest,expected_sha256,sizeof(digest))) return -1;
  int16_t *prepared_pcm=NULL;
  uint8_t *prepared_encoded=NULL;
  size_t sample_count=0;
  if(kind==DYNAMIC_WAV){
    sample_count=pcm_bytes/(size_t)(pcm_bits/8);
    if(sample_count>SIZE_MAX/sizeof(*prepared_pcm)) return -1;
    prepared_pcm=(int16_t*)malloc(sample_count*sizeof(*prepared_pcm));
    if(!prepared_pcm) return -1;
    if(pcm_bits==16) memcpy(prepared_pcm,pcm,sample_count*sizeof(*prepared_pcm));
    else for(size_t index=0;index<sample_count;index++)
      prepared_pcm[index]=(int16_t)(((int)pcm[index]-128)*256);
  } else {
    prepared_encoded=(uint8_t*)malloc((size_t)len);
    if(!prepared_encoded) return -1;
    memcpy(prepared_encoded,encoded,(size_t)len);
  }
  int slot=audio_dynamic_slot_at(a,requested);
  if(slot<0){ free(prepared_pcm); free(prepared_encoded); return -1; }
  GmlSound *sound=&a->snd[slot];
  sound->channels=channels;
  sound->sample_rate=sample_rate;
  sound->length_seconds=isfinite(length_seconds) && length_seconds>=0.0
    ? length_seconds : 0.0;
  if(bytes_per_second<=0 && sound->length_seconds>0.0){
    double estimated=(double)len/sound->length_seconds;
    if(isfinite(estimated) && estimated>0.0)
      bytes_per_second=estimated>(double)INT_MAX?INT_MAX:(int)llround(estimated);
  }
  sound->bytes_per_second=bytes_per_second;
  sound->length_known=1;
  memcpy(sound->content_sha256,digest,sizeof(digest));
  sound->content_hash_known=1;
  if(kind==DYNAMIC_WAV){
    sound->pcm=prepared_pcm;
    sound->own=prepared_pcm;
    sound->nval=(uint32_t)sample_count;
  } else {
    sound->encoded_own=prepared_encoded;
    if(kind==DYNAMIC_OGG){
      sound->ogg=prepared_encoded;
      sound->ogg_len=(uint32_t)len;
    } else {
      sound->mp3=prepared_encoded;
      sound->mp3_len=(uint32_t)len;
    }
  }
  return slot;
}

int gml_audio_add_encoded(GmlAudio *a,const uint8_t *encoded,int len){
  return audio_add_encoded_at(a,encoded,len,-1,NULL);
}

static int audio_add_pcm16_at(GmlAudio *a,const int16_t *pcm,uint32_t frames,
                              int channels,int sample_rate,
                              const uint8_t *identity,size_t identity_size,int requested,
                              const uint8_t expected_sha256[32]){
  if(!a || !pcm || !frames || (channels!=1 && channels!=2) || sample_rate<=0 ||
     sample_rate>384000 || !identity || !identity_size ||
     (uint64_t)frames*(uint32_t)channels>UINT32_MAX) return -1;
  uint8_t digest[32];
  gml_sha256(identity,identity_size,digest);
  if(expected_sha256 && memcmp(digest,expected_sha256,sizeof(digest))) return -1;
  size_t values=(size_t)frames*(size_t)channels;
  if(values>SIZE_MAX/sizeof(*pcm)) return -1;
  int16_t *copy=(int16_t*)malloc(values*sizeof(*copy));
  if(!copy) return -1;
  memcpy(copy,pcm,values*sizeof(*copy));
  int slot=audio_dynamic_slot_at(a,requested);
  if(slot<0){ free(copy); return -1; }
  GmlSound *sound=&a->snd[slot];
  sound->pcm=copy; sound->own=copy; sound->nval=(uint32_t)values;
  sound->channels=channels; sound->sample_rate=sample_rate;
  sound->length_seconds=(double)frames/(double)sample_rate;
  sound->length_known=1;
  sound->bytes_per_second=sample_rate*channels*(int)sizeof(int16_t);
  memcpy(sound->content_sha256,digest,sizeof(digest));
  sound->content_hash_known=1;
  return slot;
}

int gml_audio_add_pcm16(GmlAudio *a,const int16_t *pcm,uint32_t frames,
                        int channels,int sample_rate,
                        const uint8_t *identity,size_t identity_size){
  return audio_add_pcm16_at(a,pcm,frames,channels,sample_rate,identity,identity_size,-1,NULL);
}

int gml_audio_restore_encoded(GmlAudio *a,int handle,const uint8_t *encoded,int len,
                              const uint8_t expected_sha256[32]){
  return audio_add_encoded_at(a,encoded,len,handle,expected_sha256)==handle;
}

int gml_audio_replace_encoded(GmlAudio *a,int snd,const uint8_t *encoded,int len){
  return audio_add_encoded_at(a,encoded,len,snd,NULL)==snd;
}

int gml_audio_restore_pcm16(GmlAudio *a,int handle,const int16_t *pcm,uint32_t frames,
                            int channels,int sample_rate,
                            const uint8_t *identity,size_t identity_size,
                            const uint8_t expected_sha256[32]){
  return audio_add_pcm16_at(a,pcm,frames,channels,sample_rate,identity,identity_size,
                            handle,expected_sha256)==handle;
}

int gml_audio_sound_content_hash(GmlAudio *a,int handle,uint8_t digest[32]){
  /* Report an identity only for a sound with installed encoded bytes. An
   * unmodified content sound has no replacement identity. */
  if(!a || !digest || handle<0 || handle>=a->n_snd ||
     !a->snd[handle].content_hash_known) return 0;
  memcpy(digest,a->snd[handle].content_sha256,32);
  return 1;
}

void gml_audio_caster_free(GmlAudio *a, int handle){
  if(!a || handle<a->n_base_snd || handle>=a->n_snd) return;
  gml_audio_stop(a,handle);
  free(a->snd[handle].own);
  free(a->snd[handle].encoded_own);
  memset(&a->snd[handle],0,sizeof(GmlSound));   /* slot becomes reusable */
}
void gml_audio_caster_free_all(GmlAudio *a){
  if(!a) return;
  for(int i=a->n_base_snd;i<a->n_snd;i++) gml_audio_caster_free(a,i);
}

static int sound_ensure_pcm(GmlAudio *a,GmlSound *s){
  if(!s) return 0;
  if(s->pcm) return 1;
  if(!s->ogg && !s->mp3 && s->external_filename && !audio_sound_load_loose(a,s)){
    if(audio_setting(a,"GML_LOG_AUDIO"))
      anygm_host_logf(a && a->win ? a->win->host : NULL,ANYGM_LOG_DEBUG,
                      "[audio] streamed sound open failed: %s\n",s->external_filename);
    return 0;
  }
  /* A direct PCM load is already ready for its first play. */
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
  if(!sound_ensure_pcm(a,s)) return 0;
  if(!had_pcm && audio_setting(a,"GML_LOG_AUDIO"))
    anygm_host_logf(a && a->win ? a->win->host : NULL,ANYGM_LOG_DEBUG,"[audio] warm_sound sound=%d ogg=%u pcm=%u\n",snd,s->ogg_len,s->nval);
  return 1;
}
static uint32_t audio_initial_ogg_warm_budget(GmlAudio *a){
  const char *e=audio_setting(a,"GML_AUDIO_WARM_OGG_BYTES");
  if(e && (!strcmp(e,"0") || !strcmp(e,"off") || !strcmp(e,"false"))) return 0;
  if(e && *e){
    long v=strtol(e,NULL,0);
    return v>0 ? (uint32_t)v : 0;
  }
  return 2u*1024u*1024u;
}
static void audio_warm_initial_ogg(GmlAudio *a){
  if(!a || !a->snd) return;
  uint32_t budget=audio_initial_ogg_warm_budget(a);
  if(!budget) return;
  int warmed=0;
  uint32_t used=0;
  for(int i=0;i<a->n_snd;i++){
    GmlSound *s=&a->snd[i];
    /* Streaming is observable not only as the source path but as its lifetime: decoding every
     * loose music track during boot defeats the format's memory policy and can exhaust a frontend
     * before the first room. Decode a streamed sound when it is first played. */
    if(s->external_filename || s->pcm || !s->ogg || s->ogg_failed) continue;
    if(used>=budget) break;
    if(s->ogg_len>budget-used) continue;
    if(sound_ensure_pcm(a,s)){
      used += s->ogg_len;
      warmed++;
      if(audio_setting(a,"GML_LOG_AUDIO"))
        anygm_host_logf(a && a->win ? a->win->host : NULL,ANYGM_LOG_DEBUG,"[audio] warm_ogg sound=%d ogg=%u pcm=%u budget=%u\n",i,s->ogg_len,s->nval,budget);
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
  if(!sound_ensure_pcm(a,s)){ v->active=0; return 0; }
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
  if(!a||snd<0||snd>=a->n_snd||!sound_ensure_pcm(a,&a->snd[snd])) return -1;
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
int gml_audio_voice_sound(GmlAudio *a,int voice){
  if(!a) return -1;
  for(int i=0;i<GML_MAX_VOICES;i++)
    if(a->voice[i].active && a->voice[i].id==voice) return a->voice[i].snd;
  return -1;
}
double gml_audio_voice_pan(GmlAudio *a,int voice){
  if(!a) return 0.0;
  for(int i=0;i<GML_MAX_VOICES;i++)
    if(a->voice[i].active && a->voice[i].id==voice) return a->voice[i].pan;
  return 0.0;
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
    if(v->active && v->snd>=0 && v->snd<a->n_snd){
      GmlSound *sound=&a->snd[v->snd];
      int sound_group=(sound->external_type&GML_EXTERNAL_GROUP_FLAG)
        ? (int)(sound->external_type&~GML_EXTERNAL_GROUP_FLAG) : sound->group;
      if(sound_group==group) v->active=0;
    }
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
  if(!isfinite(gain) || gain<0) gain=0;
  if(target>=0 && target<a->n_snd){
    a->snd[target].gain=gain;
    a->snd[target].gain_target=gain;
    a->snd[target].gain_fade_frames=0;
  }
  for(int i=0;i<GML_MAX_VOICES;i++) if(voice_matches(a,&a->voice[i],target)) a->voice[i].gain=gain;
}
void gml_audio_sound_gain_fade(GmlAudio *a, int target, double gain, int milliseconds){
  if(!a) return;
  if(!isfinite(gain) || gain<0.0) gain=0.0;
  if(target<0 || target>=a->n_snd || milliseconds<=0){
    gml_audio_sound_gain(a,target,gain);
    return;
  }
  double frames=ceil((double)milliseconds*44.1);
  a->snd[target].gain_target=gain;
  a->snd[target].gain_fade_frames=
    frames>(double)INT32_MAX?INT32_MAX:(int)frames;
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
void gml_audio_sound_set_default_loop(GmlAudio *a, int sound, int loop){
  if(!a || sound<0 || sound>=a->n_snd) return;
  a->snd[sound].default_loop=loop!=0;
  for(int i=0;i<GML_MAX_VOICES;i++)
    if(voice_matches(a,&a->voice[i],sound)) a->voice[i].loop=loop!=0;
}
int gml_audio_sound_get_default_loop(GmlAudio *a, int sound){
  return a && sound>=0 && sound<a->n_snd ? a->snd[sound].default_loop : 0;
}
void gml_audio_sound_set_external_type(GmlAudio *a, int sound, uint32_t type){
  if(a && sound>=0 && sound<a->n_snd) a->snd[sound].external_type=type;
}
uint32_t gml_audio_sound_get_external_type(GmlAudio *a, int sound){
  return a && sound>=0 && sound<a->n_snd ? a->snd[sound].external_type : 0;
}
void gml_audio_sound_set_external_group(GmlAudio *a,int sound,int group){
  if(a && sound>=0 && sound<a->n_snd && group>=0 && group<GML_MAX_AUDIOGROUPS)
    a->snd[sound].external_type=GML_EXTERNAL_GROUP_FLAG|(uint32_t)group;
}
const int16_t *gml_audio_sound_pcm16(GmlAudio *a,int sound,uint32_t *frames,int *channels){
  if(frames) *frames=0;
  if(channels) *channels=0;
  if(!a || sound<0 || sound>=a->n_snd) return NULL;
  GmlSound *s=&a->snd[sound];
  if(!s->pcm || !s->nval) return NULL;
  int ch=s->channels>0?s->channels:1;
  if(frames) *frames=s->nval/(uint32_t)ch;
  if(channels) *channels=ch;
  return s->pcm;
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
  if(!s->ogg && !s->mp3 && s->external_filename) (void)audio_sound_load_loose(a,s);

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
  if(audio_setting(a,"GML_LOG_AUDIO"))
    anygm_host_logf(a && a->win ? a->win->host : NULL,ANYGM_LOG_DEBUG,"[audio] sound_length id=%d seconds=%.6f\n",sound,seconds);
  return seconds;
}
int gml_audio_sound_format(GmlAudio *a,int sound,int *channels,
                           int *sample_rate,int *bytes_per_second){
  if(channels) *channels=0;
  if(sample_rate) *sample_rate=0;
  if(bytes_per_second) *bytes_per_second=0;
  if(!a || sound<0 || sound>=a->n_snd || !gml_audio_exists(a,sound)) return 0;
  GmlSound *entry=&a->snd[sound];
  if(channels) *channels=entry->channels>0?entry->channels:0;
  if(sample_rate) *sample_rate=entry->sample_rate>0?entry->sample_rate:0;
  if(bytes_per_second)
    *bytes_per_second=entry->bytes_per_second>0?entry->bytes_per_second:0;
  return 1;
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
    double vol=s->vol*vo->spatial_gain*a->master_gain; int ch=s->channels;
    double gain_start=s->gain,gain_target=s->gain_target;
    int gain_remaining=s->gain_fade_frames;
    double gain_step=gain_remaining>0
      ? (gain_target-gain_start)/(double)gain_remaining : 0.0;
    int group=(s->external_type&GML_EXTERNAL_GROUP_FLAG)
      ? (int)(s->external_type&~GML_EXTERNAL_GROUP_FLAG)
      : (s->group>=0 && s->group<GML_MAX_AUDIOGROUPS?s->group:0);
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
      double sound_level=gain_remaining>f
        ? gain_start+gain_step*(double)f
        : (gain_remaining>0?gain_target:vo->gain);
      mix[f*2]   += (int32_t)lrint(l*vol*sound_level*group_level*left_pan);
      mix[f*2+1] += (int32_t)lrint(r*vol*sound_level*group_level*right_pan);
    }
  }
  for(int sound=0;sound<a->n_snd;sound++) if(a->snd[sound].gain_fade_frames>0){
    GmlSound *entry=&a->snd[sound];
    int advance=frames<entry->gain_fade_frames?frames:entry->gain_fade_frames;
    double step=(entry->gain_target-entry->gain)/(double)entry->gain_fade_frames;
    entry->gain+=step*(double)advance;
    entry->gain_fade_frames-=advance;
    if(!entry->gain_fade_frames) entry->gain=entry->gain_target;
    for(int voice=0;voice<GML_MAX_VOICES;voice++)
      if(voice_matches(a,&a->voice[voice],sound))
        a->voice[voice].gain=entry->gain;
  }
  for(int g=0;g<GML_MAX_AUDIOGROUPS;g++) if(a->group_fade_frames[g]>0){
    int advance=frames<a->group_fade_frames[g]?frames:a->group_fade_frames[g];
    double step=(a->group_target[g]-a->group_gain[g])/(double)a->group_fade_frames[g];
    a->group_gain[g]+=step*(double)advance;
    a->group_fade_frames[g]-=advance;
    if(!a->group_fade_frames[g]) a->group_gain[g]=a->group_target[g];
  }
  for(int i=0;i<nvals;i++)
    out[i]=audio_soft_clip(mix[i]);
  if(mix!=stack_mix) free(mix);
}

/* Fill the output batch with AUDO/SOND first, then FMOD Studio voices. Pausing the AUDO bus does
 * not pause FMOD instances, which have their own explicit controls. */
void gml_audio_mix(GmlAudio *a, int16_t *out, int frames){
  memset(out,0,(size_t)frames*2*sizeof(int16_t));
  if(a && !a->paused) audio_mix_audo(a,out,frames);
  if(a && a->fmod) gml_fmod_mix(a->fmod,out,frames,44100);
}

enum { GML_AUDIO_STATE_SCHEMA=2 };
#define GML_AUDIO_STATE_MAGIC UINT32_C(0x53554141)
typedef struct { uint8_t *data; size_t cap, pos; int ok; } AudW;
typedef struct { const uint8_t *data; size_t cap, pos; int ok; } AudR;
static void aw_raw(AudW *s, const void *p, size_t n){
  if(n>SIZE_MAX-s->pos){ s->ok=0; s->pos=SIZE_MAX; return; }
  if(s->data){ if(s->pos<=s->cap && n<=s->cap-s->pos) memcpy(s->data+s->pos,p,n); else s->ok=0; }
  s->pos+=n;
}
static void ar_raw(AudR *s, void *p, size_t n){
  if(n>SIZE_MAX-s->pos){ memset(p,0,n); s->ok=0; s->pos=SIZE_MAX; return; }
  if(s->pos<=s->cap && n<=s->cap-s->pos) memcpy(p,s->data+s->pos,n);
  else { memset(p,0,n); s->ok=0; }
  s->pos+=n;
}
static void aw_u32(AudW *s, uint32_t v){
  uint8_t b[4]={(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
  aw_raw(s,b,sizeof b);
}
static void aw_i32(AudW *s, int v){ aw_u32(s,(uint32_t)(int32_t)v); }
static void aw_d(AudW *s, double v){
  uint64_t bits=0; uint8_t b[8]; memcpy(&bits,&v,sizeof bits);
  for(unsigned i=0;i<8;i++) b[i]=(uint8_t)(bits>>(i*8));
  aw_raw(s,b,sizeof b);
}
static uint32_t ar_u32(AudR *s){
  uint8_t b[4]={0}; ar_raw(s,b,sizeof b);
  return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static int ar_i32(AudR *s){ return (int)(int32_t)ar_u32(s); }
static double ar_d(AudR *s){
  uint8_t b[8]={0}; uint64_t bits=0; ar_raw(s,b,sizeof b);
  for(unsigned i=0;i<8;i++) bits|=(uint64_t)b[i]<<(i*8);
  double v=0; memcpy(&v,&bits,sizeof v); return v;
}

static void audio_state_write(AudW *s, GmlAudio *a){
  aw_u32(s,GML_AUDIO_STATE_MAGIC); /* AAUS */
  aw_u32(s,GML_AUDIO_STATE_SCHEMA);
  aw_i32(s,a?a->paused:0);
  aw_i32(s,a?a->next_voice_id:1000000);
  aw_d(s,a?a->master_gain:1.0);
  aw_i32(s,a?a->channel_num:128);
  for(int i=0;i<GML_MAX_VOICES;i++){
    GmlVoice v=(a&&a->voice[i].active)?a->voice[i]:(GmlVoice){0};
    aw_i32(s,v.snd); aw_i32(s,v.loop); aw_i32(s,v.active); aw_i32(s,v.paused); aw_i32(s,v.id);
    aw_d(s,v.pos); aw_d(s,v.gain); aw_d(s,v.pitch);
    aw_d(s,v.loop_start); aw_d(s,v.spatial_gain); aw_d(s,v.pan); aw_i32(s,v.emitter);
  }
  aw_i32(s,a?a->n_snd:0);
  if(a) for(int i=0;i<a->n_snd;i++){
    aw_d(s,a->snd[i].gain); aw_d(s,a->snd[i].pitch); aw_d(s,a->snd[i].loop_start_seconds);
    aw_d(s,a->snd[i].gain_target);
    aw_i32(s,a->snd[i].gain_fade_frames);
    aw_i32(s,a->snd[i].default_loop);
    aw_u32(s,a->snd[i].external_type);
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
  uint32_t magic=ar_u32(&s);
  uint32_t schema=ar_u32(&s);
  if(magic!=GML_AUDIO_STATE_MAGIC || schema!=GML_AUDIO_STATE_SCHEMA || !s.ok) return 0;
  int paused=ar_i32(&s);
  int next_voice_id=ar_i32(&s);
  double master_gain=ar_d(&s);
  int channel_num=ar_i32(&s);
  GmlVoice tmp[GML_MAX_VOICES];
  int snd_count=a?a->n_snd:0;
  typedef struct {
    double gain,pitch,loop_start,gain_target;
    int gain_fade_frames,default_loop;
    uint32_t external_type;
  } AudSoundState;
  AudSoundState *sound_state=NULL;
  if(snd_count>0){
    sound_state=calloc((size_t)snd_count,sizeof(*sound_state));
    if(!sound_state) return 0;
    for(int i=0;i<snd_count;i++){
      sound_state[i].gain=1.0;
      sound_state[i].pitch=1.0;
      sound_state[i].gain_target=1.0;
    }
  }
  for(int i=0;i<GML_MAX_VOICES;i++){
    memset(&tmp[i],0,sizeof(tmp[i]));
    tmp[i].spatial_gain=1.0; tmp[i].emitter=-1;
    tmp[i].snd=ar_i32(&s); tmp[i].loop=ar_i32(&s); tmp[i].active=ar_i32(&s);
    tmp[i].paused=ar_i32(&s); tmp[i].id=ar_i32(&s);
    tmp[i].pos=ar_d(&s); tmp[i].gain=ar_d(&s); tmp[i].pitch=ar_d(&s);
    tmp[i].loop_start=ar_d(&s); tmp[i].spatial_gain=ar_d(&s);
    tmp[i].pan=ar_d(&s); tmp[i].emitter=ar_i32(&s);
    if(!a || tmp[i].snd<0 || tmp[i].snd>=a->n_snd) tmp[i].active=0;
    if(!tmp[i].active){ memset(&tmp[i],0,sizeof(tmp[i])); continue; }
    if(tmp[i].id<1000000){
      tmp[i].id=next_voice_id++;
    }
    if(!isfinite(tmp[i].pos) || tmp[i].pos<0.0) tmp[i].pos=0.0;
    if(!isfinite(tmp[i].gain) || tmp[i].gain<0.0) tmp[i].gain=1.0;
    if(!isfinite(tmp[i].pitch) || tmp[i].pitch<=0.0) tmp[i].pitch=1.0;
    if(a && tmp[i].active) audio_voice_prepare(a,&tmp[i]);
  }
  int stored_sounds=ar_i32(&s);
  if(stored_sounds<0 || stored_sounds>1000000){
    free(sound_state); return 0;
  }
  /* Sounds past n_base_snd are created at run time, so a state saved before one existed must not
   * leave it behind. Releasing them here makes a load restore the table the state describes rather
   * than whatever the session has accumulated, which is what keeps a save->load->save pair stable
   * for a frontend that stores and restores every frame. Sounds recorded in a state but missing
   * from this session cannot be recreated -- the format carries their mixer parameters, not their
   * identity -- so a larger stored count still applies index-wise onto what exists. */
  if(a && stored_sounds>=a->n_base_snd && stored_sounds<a->n_snd){
    for(int i=stored_sounds;i<a->n_snd;i++) gml_audio_caster_free(a,i);
    a->n_snd=stored_sounds;
    if(snd_count>a->n_snd) snd_count=a->n_snd;
    for(int i=0;i<GML_MAX_VOICES;i++)
      if(tmp[i].active && (tmp[i].snd<0 || tmp[i].snd>=a->n_snd))
        memset(&tmp[i],0,sizeof(tmp[i]));
  }
  /* Records are positional and run-time sounds carry no identity, so an index only names the same
   * sound when both tables have the same shape. Resource-backed sounds always do. Run-time ones do
   * only when the counts agree, which the release above arranges whenever the state describes the
   * smaller table; when the state describes a larger one its surplus sounds cannot be recreated, and
   * applying their gains and fades to whatever occupies those slots would corrupt unrelated sounds. */
  int dynamic_layout_matches=a && stored_sounds==a->n_snd;
  int applied_limit=snd_count;
  if(a && !dynamic_layout_matches && a->n_base_snd<applied_limit)
    applied_limit=a->n_base_snd;
  for(int i=0;i<stored_sounds;i++){
    double g=ar_d(&s), p=ar_d(&s), l=ar_d(&s);
    double target=ar_d(&s);
    int remaining=ar_i32(&s);
    int default_loop=ar_i32(&s);
    uint32_t external_type=ar_u32(&s);
    if(i<applied_limit){
      sound_state[i].gain=isfinite(g)&&g>=0.0?g:1.0;
      /* A released run-time slot is serialized as an all-zero record. Preserve that canonical
       * record across a load; playback already treats a live sound's non-positive pitch as the
       * default, while changing a free slot from zero to one makes save->load->save drift. */
      sound_state[i].pitch=isfinite(p)&&p>=0.0?p:1.0;
      sound_state[i].loop_start=isfinite(l)&&l>=0.0?l:0.0;
      sound_state[i].gain_target=isfinite(target)&&target>=0.0
        ? target : sound_state[i].gain;
      sound_state[i].gain_fade_frames=remaining>0?remaining:0;
      sound_state[i].default_loop=default_loop!=0;
      sound_state[i].external_type=external_type;
    }
  }
  double group_gain[GML_MAX_AUDIOGROUPS], group_target[GML_MAX_AUDIOGROUPS];
  int group_fade_frames[GML_MAX_AUDIOGROUPS];
  for(int g=0;g<GML_MAX_AUDIOGROUPS;g++){
    group_gain[g]=group_target[g]=1.0; group_fade_frames[g]=0;
  }
  int stored_groups=ar_i32(&s);
  if(stored_groups<0 || stored_groups>4096){ free(sound_state); return 0; }
  for(int g=0;g<stored_groups;g++){
    double gain=ar_d(&s), target=ar_d(&s); int remaining=ar_i32(&s);
    if(g<GML_MAX_AUDIOGROUPS){
      group_gain[g]=isfinite(gain)&&gain>=0.0?gain:1.0;
      group_target[g]=isfinite(target)&&target>=0.0?target:group_gain[g];
      group_fade_frames[g]=remaining>0?remaining:0;
    }
  }
  if(!s.ok){ free(sound_state); return 0; }
  if(master_gain<0) master_gain=1.0;
  if(channel_num<0) channel_num=0;
  if(a){
    a->paused=paused!=0; a->next_voice_id=next_voice_id<1000000?1000000:next_voice_id; a->master_gain=master_gain; a->channel_num=channel_num; memcpy(a->voice,tmp,sizeof(tmp));
    memcpy(a->group_gain,group_gain,sizeof group_gain);
    memcpy(a->group_target,group_target,sizeof group_target);
    memcpy(a->group_fade_frames,group_fade_frames,sizeof group_fade_frames);
    /* Only the sounds the state can actually name are rewritten. A sound the state says nothing
     * trustworthy about keeps what it has, rather than being reset to defaults it never carried. */
    int restore_limit=a->n_snd<applied_limit?a->n_snd:applied_limit;
    for(int i=0;i<restore_limit;i++){
      a->snd[i].gain=sound_state?sound_state[i].gain:1.0;
      a->snd[i].pitch=sound_state?sound_state[i].pitch:1.0;
      a->snd[i].loop_start_seconds=sound_state?sound_state[i].loop_start:0.0;
      a->snd[i].gain_target=sound_state?sound_state[i].gain_target:1.0;
      a->snd[i].gain_fade_frames=sound_state?sound_state[i].gain_fade_frames:0;
      a->snd[i].default_loop=sound_state?sound_state[i].default_loop:0;
      a->snd[i].external_type=sound_state?sound_state[i].external_type:0;
    }
  }
  free(sound_state);
  if(used) *used=s.pos;
  return s.pos<=len;
}
