/* SPDX-License-Identifier: MIT AND Apache-2.0
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 *
 * Original project contributions are licensed under MIT. The bank metadata
 * readers are adapted from FModBankParser under Apache-2.0; those portions
 * retain that license. Modified for C buffer access and audio-runtime integration.
 * See NOTICE, LICENSES/fmodbankparser.txt and LICENSES/fmodbankparser-NOTICE.txt.
 */
/* FMOD FSB5 parsing, Vorbis stream reconstruction, bank lookup, and playback.
 * The complete generic setup-packet table is generated from the reviewed source documented in
 * THIRD_PARTY_NOTICES.md. Bank event mapping and the software mixer remain in this domain module. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "gml_fmod.h"
#include "anygm_host.h"
#include "anygm_vfs.h"
#include "audio_setup_data.h"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

static uint32_t rd_u32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t rd_u16(const uint8_t *p){ return (uint16_t)p[0]|((uint16_t)p[1]<<8); }
static float    rd_f32(const uint8_t *p){ uint32_t u=rd_u32(p); float f; memcpy(&f,&u,4); return f; }

/* ---- Ogg framing (page writer with the Ogg CRC32: poly 0x04c11db7, no reflection) ---- */
static uint32_t ogg_crc(const uint8_t *d, size_t n){
  uint32_t crc=0;
  for(size_t i=0;i<n;i++){
    crc^=(uint32_t)d[i]<<24;
    for(int bit=0;bit<8;bit++) crc=(crc&0x80000000u)?(crc<<1)^0x04c11db7u:crc<<1;
  }
  return crc;
}

typedef struct { uint8_t *b; size_t len, cap; } Buf;
static int buf_need(Buf *o, size_t add){ if(o->len+add<=o->cap) return 1;
  size_t nc=o->cap?o->cap*2:8192; while(nc<o->len+add) nc*=2;
  uint8_t *nb=realloc(o->b,nc); if(!nb) return 0; o->b=nb; o->cap=nc; return 1; }
static void buf_put(Buf *o, const void *d, size_t n){ if(buf_need(o,n)){ memcpy(o->b+o->len,d,n); o->len+=n; } }

/* Write one Ogg page carrying complete Vorbis packets. Each packet uses
 * floor(len/255) full lacing values followed by a terminating value below 255.
 * The segment array has capacity for 255 values. */
static void ogg_page_pkts(Buf *o, uint8_t htype, uint64_t granule, uint32_t serial, uint32_t *seq,
                          const uint8_t **pkts, const size_t *plens, int npkts){
  uint8_t seg[255]; int nseg=0;
  for(int i=0;i<npkts && nseg<255;i++){ size_t l=plens[i];
    while(l>=255 && nseg<255){ seg[nseg++]=255; l-=255; }
    if(nseg<255) seg[nseg++]=(uint8_t)l;   /* final <255 lacing ends this packet */
  }
  uint8_t hdr[27];
  memcpy(hdr,"OggS",4); hdr[4]=0; hdr[5]=htype;
  for(int i=0;i<8;i++) hdr[6+i]=(uint8_t)(granule>>(8*i));
  hdr[14]=(uint8_t)serial; hdr[15]=(uint8_t)(serial>>8); hdr[16]=(uint8_t)(serial>>16); hdr[17]=(uint8_t)(serial>>24);
  hdr[18]=(uint8_t)*seq; hdr[19]=(uint8_t)(*seq>>8); hdr[20]=(uint8_t)(*seq>>16); hdr[21]=(uint8_t)(*seq>>24);
  hdr[22]=hdr[23]=hdr[24]=hdr[25]=0;       /* crc placeholder */
  hdr[26]=(uint8_t)nseg;
  Buf tmp={0}; buf_put(&tmp,hdr,27); buf_put(&tmp,seg,nseg);
  for(int i=0;i<npkts;i++) buf_put(&tmp,pkts[i],plens[i]);
  uint32_t crc=ogg_crc(tmp.b,tmp.len);
  tmp.b[22]=(uint8_t)crc; tmp.b[23]=(uint8_t)(crc>>8); tmp.b[24]=(uint8_t)(crc>>16); tmp.b[25]=(uint8_t)(crc>>24);
  buf_put(o,tmp.b,tmp.len); free(tmp.b);
  (*seq)++;
}
static void ogg_page(Buf *o, uint8_t htype, uint64_t granule, uint32_t serial, uint32_t *seq,
                     const uint8_t *payload, size_t plen){
  ogg_page_pkts(o,htype,granule,serial,seq,&payload,&plen,1);
}

/* Compare stored CRC-32 keys for the bundled setup packets. */
static const uint8_t *find_codebook(uint32_t crc, int *len){
  for(int i=0;i<anygm_audio_setup_entry_count;i++){
    const AnygmAudioSetupEntry *entry=&anygm_audio_setup_entries[i];
    if(entry->id==crc){ *len=(int)entry->size; return entry->data; }
  }
  return NULL;
}
/* Rebuild a standard Ogg/Vorbis buffer for one FSB5 Vorbis subsound. Returns malloc'd buffer (caller
 * frees) and sets *out_len, or NULL if the setup CRC is unknown. */
static uint8_t *rebuild_ogg(int channels, int rate, uint32_t setup_crc,
                            const uint8_t *pkts, size_t pkts_len, size_t *out_len){
  int cb_len; const uint8_t *cb=find_codebook(setup_crc,&cb_len);
  if(!cb) return NULL;
  Buf o={0}; uint32_t serial=1, seq=0;
  /* Emit a 30-byte identification header with block sizes 256 and 2048. */
  uint8_t idh[30]={0}; idh[0]=1; memcpy(idh+1,"vorbis",6);
  idh[11]=(uint8_t)channels;
  idh[12]=(uint8_t)rate; idh[13]=(uint8_t)(rate>>8); idh[14]=(uint8_t)(rate>>16); idh[15]=(uint8_t)(rate>>24);
  idh[28]=(uint8_t)(8 | (11<<4)); idh[29]=1;
  ogg_page(&o,0x02,0,serial,&seq,idh,30);
  /* comment header (minimal) + setup header (the codebook) — TWO packets on one page */
  uint8_t com[16]={0}; com[0]=3; memcpy(com+1,"vorbis",6); com[15]=1;
  { const uint8_t *pk[2]={com,cb}; size_t pl[2]={16,(size_t)cb_len}; ogg_page_pkts(&o,0,0,serial,&seq,pk,pl,2); }
  /* Input audio packets have uint16 size prefixes. Emit one page per packet;
   * intermediate granule positions remain zero. The final page is emitted below. */
  size_t pos=0; Buf last={0};
  while(pos+2<=pkts_len){
    uint16_t sz=rd_u16(pkts+pos); pos+=2;
    if(pos+sz>pkts_len) break;
    int is_last = (pos+sz>=pkts_len) || (pos+sz+2>pkts_len);
    if(!is_last) ogg_page(&o,0,0,serial,&seq,pkts+pos,sz);
    else { buf_put(&last,pkts+pos,sz); }
    pos+=sz;
    if(is_last) break;
  }
  ogg_page(&o,0x04,0xFFFFFFFFu,serial,&seq,last.b?last.b:(const uint8_t*)"",last.len);  /* EOS */
  free(last.b);
  *out_len=o.len; return o.b;
}

/* Parse an FSB5 chunk and return one subsound's metadata + a pointer to its raw packet data. */
int gml_fmod_fsb5_sample(const uint8_t *fsb5, size_t fsb5_len, int index, GmlFmodSample *out){
  if(fsb5_len<60 || memcmp(fsb5,"FSB5",4)!=0) return 0;
  uint32_t ns=rd_u32(fsb5+8), shs=rd_u32(fsb5+12), nts=rd_u32(fsb5+16), ds=rd_u32(fsb5+20), mode=rd_u32(fsb5+24);
  if(index<0 || (uint32_t)index>=ns) return 0;
  size_t hoff=60, names_off=hoff+shs, data_off=names_off+nts;
  /* walk sample headers to the requested index, capturing (dataOffset, samples, rate, chans) */
  size_t p=hoff; uint32_t doff=0, samples=0; int chans=1, rate=44100; uint32_t crc=0;
  static const int RATES[]={4000,8000,11025,16000,22050,24000,32000,44100,48000,96000};
  for(uint32_t s=0;s<=(uint32_t)index && p+8<=hoff+shs;s++){
    uint64_t raw=0; for(int k=0;k<8;k++) raw|=(uint64_t)fsb5[p+k]<<(8*k); p+=8;
    int nxt=raw&1; int fq=(raw>>1)&0xf; chans=((raw>>5)&1)+1; doff=(uint32_t)(((raw>>6)&0x0FFFFFFF)*16); samples=(uint32_t)(raw>>34);
    rate = (fq>=1 && fq<=10)? RATES[fq-1] : 44100;
    crc=0;
    while(nxt && p+4<=hoff+shs){ uint32_t c=rd_u32(fsb5+p); p+=4; nxt=c&1; uint32_t csz=(c>>1)&0xFFFFFF; uint32_t ctype=(c>>25)&0x7F;
      if(ctype==11 && p+4<=fsb5_len) crc=rd_u32(fsb5+p);  /* VORBISDATA: setup CRC32 */
      p+=csz; }
  }
  /* sub-sound data range: from its dataOffset to the next sample's dataOffset (or end) */
  uint32_t next_doff=ds;
  { size_t q=hoff; for(uint32_t s=0;s<ns && q+8<=hoff+shs;s++){ uint64_t raw=0; for(int k=0;k<8;k++) raw|=(uint64_t)fsb5[q+k]<<(8*k); q+=8;
      int nxt=raw&1; uint32_t o2=(uint32_t)(((raw>>6)&0x0FFFFFFF)*16);
      while(nxt && q+4<=hoff+shs){ uint32_t c=rd_u32(fsb5+q); q+=4; nxt=c&1; q+=(c>>1)&0xFFFFFF; }
      if(o2>doff && o2<next_doff) next_doff=o2; } }
  (void)mode;
  out->channels=chans; out->rate=rate; out->num_samples=samples; out->setup_crc=crc;
  out->data = fsb5 + data_off + doff;
  out->data_len = (next_doff>doff)? (next_doff-doff) : (ds>doff? ds-doff : 0);
  if(data_off+doff+out->data_len > fsb5_len) return 0;
  return 1;
}

/* Decode one FSB5 Vorbis subsound to interleaved S16. Returns frame count (samples per channel), or 0.
 * *out_pcm is malloc'd (caller frees). */
int gml_fmod_decode(const uint8_t *fsb5, size_t fsb5_len, int index, int *channels, int *rate, int16_t **out_pcm){
  GmlFmodSample s;
  if(!gml_fmod_fsb5_sample(fsb5,fsb5_len,index,&s)) return 0;
  size_t ogg_len; uint8_t *ogg=rebuild_ogg(s.channels,s.rate,s.setup_crc,s.data,s.data_len,&ogg_len);
  if(!ogg) return 0;
  int ch=0, sr=0; short *pcm=NULL;
  int frames=stb_vorbis_decode_memory(ogg,(int)ogg_len,&ch,&sr,&pcm);
  free(ogg);
  if(frames<=0){ free(pcm); return 0; }
  *channels=ch; *rate=sr; *out_pcm=pcm; return frames;
}

/* Bank metadata readers adapted from FModBankParser (Apache-2.0).
 * Modified to read little-endian C buffers and populate local runtime structures.
 * Shared by the string table and main-bank event graph. See the file notice. */

/* Read a low 16-bit word. If its top bit is set, read another word,
 * retain the low 15 bits and place the second word at bit 15. */
static uint32_t fx_x16(const uint8_t *b, size_t len, size_t *o){
  if(*o+2>len){ *o=len; return 0; }
  uint16_t low=rd_u16(b+*o); *o+=2;
  uint32_t v=low;
  if(low & 0x8000){
    if(*o+2>len){ *o=len; return 0; }
    uint16_t high=rd_u16(b+*o); *o+=2;
    v=(v & 0x7FFF) | ((uint32_t)high<<15);
  }
  return v;
}
/* ReadElemListImp<T>: count = x16>>1, then ONE u16 payload-size, then `count` fixed-size elements.
 * Returns count and advances *o to the first element byte. */
static uint32_t fx_elem_list(const uint8_t *b, size_t len, size_t *o){
  uint32_t raw=fx_x16(b,len,o);
  uint32_t count=raw>>1;
  if(count==0) return 0;
  if(*o+2>len){ *o=len; return 0; }
  *o+=2; /* payload/element size — not needed (element stride is fixed by type) */
  return count;
}
static uint32_t fx_read24(const uint8_t *b, size_t off){
  return (uint32_t)b[off] | ((uint32_t)b[off+1]<<8) | ((uint32_t)b[off+2]<<16);
}
static size_t fmod_find_chunk(const uint8_t *b, size_t len, const char *id4, uint32_t *out_size){
  for(size_t i=0;i+8<=len;i++){
    if(b[i]==(uint8_t)id4[0] && b[i+1]==(uint8_t)id4[1] && b[i+2]==(uint8_t)id4[2] && b[i+3]==(uint8_t)id4[3]){
      if(out_size) *out_size=rd_u32(b+i+4);
      return i+8;   /* offset of chunk payload */
    }
  }
  return (size_t)-1;
}

typedef struct { uint8_t g[16]; } FGuid;

/* Decode the Master.strings.bank STDT radix tree into parallel (path,guid) arrays.
 * *out_paths is an array of malloc'd C strings; *out_guids parallel. Returns count (0 on failure). */
static int fmod_parse_strings(const uint8_t *buf, size_t len, char ***out_paths, FGuid **out_guids){
  uint32_t stdt_size=0;
  size_t stdt=fmod_find_chunk(buf,len,"STDT",&stdt_size);
  if(stdt==(size_t)-1) return 0;
  size_t end=stdt+stdt_size; if(end>len) end=len;
  size_t o=stdt;
  if(o+4>end) return 0;
  uint32_t type=rd_u32(buf+o); o+=4;
  if(type!=1) return 0;                       /* only StringTable_RadixTree_24Bit */

  uint32_t nnodes=fx_elem_list(buf,end,&o); size_t nodes_off=o; o+=(size_t)nnodes*8;
  uint32_t nguids=fx_elem_list(buf,end,&o);  size_t guids_off=o; o+=(size_t)nguids*16;
  uint32_t blob_len=fx_x16(buf,end,&o);       size_t blob_off=o; o+=blob_len;
  uint32_t nleaf=fx_x16(buf,end,&o);          size_t leaf_off=o; o+=(size_t)nleaf*3;
  uint32_t nparent=fx_x16(buf,end,&o);        size_t parent_off=o; o+=(size_t)nparent*3;
  if(o>end) return 0;
  if(nleaf!=nguids || nparent!=nnodes) return 0;

  char **paths=calloc(nguids,sizeof(char*));
  FGuid *guids=calloc(nguids,sizeof(FGuid));
  if(!paths||!guids){ free(paths); free(guids); return 0; }

  for(uint32_t gi=0; gi<nguids; gi++){
    memcpy(guids[gi].g, buf+guids_off+(size_t)gi*16, 16);
    /* walk leaf→parent collecting node string segments, then reverse-concat */
    uint32_t node=fx_read24(buf,leaf_off+(size_t)gi*3);
    /* segment offsets into the string blob, root-last (we collect leaf-first) */
    size_t seg_off[64]; int nseg=0; size_t total=0; int guard=0;
    while(node!=0xFFFFFF && node<nnodes && nseg<64){
      uint32_t keyinfo=rd_u32(buf+nodes_off+(size_t)node*8);
      uint32_t soff=keyinfo & 0xFFFFFF;
      if(soff!=0xFFFFFF && blob_off+soff<end){
        seg_off[nseg++]=blob_off+soff;
        size_t sl=0; while(blob_off+soff+sl<end && buf[blob_off+soff+sl]!=0) sl++;
        total+=sl;
      }
      if(node>=nparent) break;
      node=fx_read24(buf,parent_off+(size_t)node*3);
      if(++guard>100000) break;
    }
    char *p=malloc(total+1); size_t k=0;
    if(p){
      for(int s=nseg-1;s>=0;s--){ const char *seg=(const char*)(buf+seg_off[s]);
        size_t sl=strlen(seg); memcpy(p+k,seg,sl); k+=sl; }
      p[k]=0;
    }
    paths[gi]=p;
  }
  *out_paths=paths; *out_guids=guids;
  return (int)nguids;
}

/* Bank-set storage retains metadata, sample headers and open bank files.
 * Compressed sample bytes are read on demand rather than retaining whole banks. */

typedef struct {
  uint32_t doff;      /* subsound compressed-data offset within the FSB5 data region */
  uint32_t dlen;      /* compressed length */
  uint32_t crc;       /* Vorbis setup-header CRC (codebook lookup key) */
  int      ch, rate;
  uint32_t samples;
} FSub;

typedef struct {
  char     path[600];
  const AnygmHostServices *host;
  void    *file;             /* kept open for on-demand subsound reads */
  size_t   data_file_off;    /* file offset of the FSB5 data region (subsound N at +doff) */
  FSub    *subs; int nsubs;
  char   **names;            /* subsound names (diagnostics/validation), may be NULL */
  /* event graph → sample: eventGUID → up to 8 subsounds (random-select for multi-instruments) +
   * optional loop region [loop_start, loop_end] in source frames (0/0 = loop whole track). lc[] holds
   * raw loop-back candidates; the real region is chosen against the sample length in the post-pass. */
  struct { FGuid g; int sub[8]; int nsub; int loop; uint32_t loop_start, loop_end;
           float mindist, maxdist;   /* 3D rolloff range (event units); 0/0 = non-positional */
           struct { uint32_t start, dest; } lc[8]; int nlc; } *ev; int nev;
} FBank;

#define FMOD_MAXBANKS 8
/* Persistent voice slots defer audio allocation until playback. */
#define FMOD_MAXVOICES 2048
#define FMOD_MAX_PARAMS 16

typedef struct {
  int handle;            /* 0 = free slot */
  int active, paused, loop, one_shot;
  char path[96];         /* event path, re-resolved on each play for variant selection */
  int bank, sub;         /* resolved source (for lazy decode on play) */
  int16_t *pcm; int own_pcm, frames, ch, rate;   /* small samples: fully decoded in `pcm` */
  double pos, gain;      /* pos in source frames (monotonic) */
  long loop_start, loop_end;   /* loop region in source frames (0/0 = loop whole track) */
  double px, py; int has_3d;   /* 3D emitter position (for stereo panning) */
  struct { char name[40]; double value; } param[FMOD_MAX_PARAMS]; int nparam;
  /* large samples (music): streamed to avoid a decode hitch + holding tens of MB of PCM */
  stb_vorbis *vs; uint8_t *ogg; long stream_pos;       /* next source frame the decoder will produce */
  int16_t *win; int win_cap, win_len; long win_start;   /* decoded source-frame window [win_start, +win_len) */
} FVoice;

typedef struct { int bank, sub, frames, ch, rate; int16_t *pcm; } FCache;

struct GmlFmodBanks {
  const AnygmHostServices *host;
  char  **paths; FGuid *guids; int nstrings;   /* global path↔GUID (from strings.bank) */
  FBank  banks[FMOD_MAXBANKS]; int nbanks;
  uint32_t rng;                                /* for multi-instrument variant selection */
  FVoice voices[FMOD_MAXVOICES]; int next_handle;
  int32_t vh[FMOD_MAXVOICES];                  /* compact mirror of voices[i].handle: lookups/alloc scan 8KB instead of striding the fat structs */
  int32_t vhint[4096];                         /* direct-mapped handle→slot hint, verified on use */
  FCache *cache; int ncache, cap_cache;        /* decoded small samples (SFX), shared */
  double lx, ly; int have_listener;            /* 3D listener position (screen/world units) */
  struct { char name[40]; double value; } param[FMOD_MAX_PARAMS]; int nparam;
};

static const char *fmod_setting(const GmlFmodBanks *banks,const char *name){
  return anygm_host_development_setting(banks?banks->host:NULL,name);
}

static const char *fmod_bank_setting(const FBank *bank,const char *name){
  return anygm_host_development_setting(bank?bank->host:NULL,name);
}

static int fmod_bank_read(FBank *bank,uint64_t offset,void *data,size_t size){
  if(!bank || !bank->host || !bank->file || !bank->host->file_seek ||
     !bank->host->file_read || offset>INT64_MAX) return 0;
  if(bank->host->file_seek(bank->host->userdata,bank->file,(int64_t)offset,
                           ANYGM_SEEK_START)<0) return 0;
  size_t used=0;
  while(used<size){
    size_t count=bank->host->file_read(bank->host->userdata,bank->file,
                                       (uint8_t *)data+used,size-used);
    if(!count || count>size-used) return 0;
    used+=count;
  }
  return 1;
}

static void fmod_voice_free_audio(FVoice *v);   /* frees a voice's stream/owned-PCM (defined below) */

static const int FSB5_RATES[]={4000,8000,11025,16000,22050,24000,32000,44100,48000,96000};

/* Parse an in-RAM FSB5 header region (magic + sample-header table). Fills *bank->subs. `hdr` must
 * contain at least the first 60+shs bytes. `data_region_off` is the file offset where FSB5 data
 * begins. Returns 1 on success. */
static int fmod_fsb5_parse_table(FBank *bank, const uint8_t *hdr, size_t hdrlen, size_t data_region_off){
  if(hdrlen<60 || memcmp(hdr,"FSB5",4)!=0) return 0;
  uint32_t ns=rd_u32(hdr+8), shs=rd_u32(hdr+12), nts=rd_u32(hdr+16), ds=rd_u32(hdr+20);
  size_t hoff=60;
  size_t names_off=60+(size_t)shs;
  if(60+(size_t)shs>hdrlen) return 0;
  FSub *subs=calloc(ns?ns:1,sizeof(FSub));
  if(!subs) return 0;
  size_t p=hoff;
  for(uint32_t s=0;s<ns && p+8<=hoff+shs;s++){
    uint64_t raw=0; for(int k=0;k<8;k++) raw|=(uint64_t)hdr[p+k]<<(8*k); p+=8;
    int nxt=raw&1; int fq=(raw>>1)&0xf;
    subs[s].ch=((raw>>5)&1)+1;
    subs[s].doff=(uint32_t)(((raw>>6)&0x0FFFFFFF)*16);
    subs[s].samples=(uint32_t)(raw>>34);
    subs[s].rate=(fq>=1&&fq<=10)?FSB5_RATES[fq-1]:44100;
    subs[s].crc=0;
    while(nxt && p+4<=hoff+shs){ uint32_t c=rd_u32(hdr+p); p+=4; nxt=c&1;
      uint32_t csz=(c>>1)&0xFFFFFF; uint32_t ctype=(c>>25)&0x7F;
      if(ctype==11 && p+4<=hdrlen) subs[s].crc=rd_u32(hdr+p);
      p+=csz; }
  }
  /* compressed length = gap to the next subsound's data offset (or to end of data) */
  for(uint32_t s=0;s<ns;s++){
    uint32_t nd=ds;
    for(uint32_t t=0;t<ns;t++) if(subs[t].doff>subs[s].doff && subs[t].doff<nd) nd=subs[t].doff;
    subs[s].dlen=(nd>subs[s].doff)?(nd-subs[s].doff):0;
  }
  bank->subs=subs; bank->nsubs=(int)ns; bank->data_file_off=data_region_off;
  /* optional name table: ns u32 offsets (relative to the name region) → null-terminated names */
  if(nts>0 && names_off+(size_t)ns*4<=hdrlen && names_off+nts<=hdrlen){
    char **names=calloc(ns?ns:1,sizeof(char*));
    if(names){
      for(uint32_t s=0;s<ns;s++){
        uint32_t noff=rd_u32(hdr+names_off+(size_t)s*4);
        size_t np=names_off+noff;
        if(np<names_off+nts && np<hdrlen){
          size_t nl=0; while(np+nl<hdrlen && np+nl<names_off+nts && hdr[np+nl]) nl++;
          names[s]=malloc(nl+1); if(names[s]){ memcpy(names[s],hdr+np,nl); names[s][nl]=0; }
        }
      }
      bank->names=names;
    }
  }
  return 1;
}

/* Event metadata readers adapted from FModBankParser (Apache-2.0).
 * Parse LIST metadata into local node tables, then resolve GUIDs to subsounds.
 * Node tables are released after resolution. */

/* chunk ids as little-endian int32 (4 ASCII bytes) */
#define FID_LIST 0x5453494cu
#define FID_EVTB 0x42545645u
#define FID_TLNB 0x424e4c54u
#define FID_WAVR 0x20564157u   /* "WAV " */
#define FID_WAIB 0x42494157u
#define FID_MUIB 0x4249554du
#define FID_SPIB 0x42495053u
#define FID_PLST 0x54534c50u
#define FID_INST 0x54534e49u
#define FID_PMLB 0x424c4d50u
#define FID_TRNB 0x424e5254u   /* "TRNB" transition-region body (loop-back regions) */

typedef struct { FGuid *d; int n, cap; } GArr;
static void garr_push(GArr *a, const uint8_t *g16){
  if(a->n>=a->cap){ int nc=a->cap?a->cap*2:8; FGuid *nd=realloc(a->d,(size_t)nc*sizeof(FGuid)); if(!nd) return; a->d=nd; a->cap=nc; }
  memcpy(a->d[a->n].g,g16,16); a->n++;
}
static int garr_has(GArr *a, const uint8_t *g16){ for(int i=0;i<a->n;i++) if(!memcmp(a->d[i].g,g16,16)) return 1; return 0; }
static void garr_free(GArr *a){ free(a->d); a->d=NULL; a->n=a->cap=0; }

typedef struct { FGuid g; FGuid timeline; GArr params, triggered; float mindist, maxdist; } EvN;
typedef struct { FGuid g; GArr boxes; GArr mkg; } TmN;   /* mkg = this timeline's named-marker guids */
typedef struct { FGuid g; int kind; FGuid wavres, inst_tl; GArr pl; int loopcount; } InN;  /* kind 0=wav 1=multi 2=scatter */
typedef struct { FGuid g; int sb, ss; } WvN;
typedef struct { FGuid g; GArr instruments; } PmN;

typedef struct {
  EvN *ev; int nev, cap_ev;
  TmN *tm; int ntm, cap_tm;
  InN *in; int nin, cap_in;
  WvN *wv; int nwv, cap_wv;
  PmN *pm; int npm, cap_pm;
  /* loop-region support: named markers (guid→position) + transition loop-backs (dest marker + start) */
  struct { FGuid g; uint32_t pos; } *mk; int nmk, cap_mk;
  struct { FGuid dest; uint32_t start; } *tr; int ntr, cap_tr;
  int version;
  struct { uint32_t id; FGuid g; } pstk[64]; int psp;
} BP;

static EvN *bp_ev(BP*b){ if(b->nev>=b->cap_ev){int nc=b->cap_ev?b->cap_ev*2:64; b->ev=realloc(b->ev,(size_t)nc*sizeof(EvN)); b->cap_ev=nc;} EvN*n=&b->ev[b->nev++]; memset(n,0,sizeof*n); return n; }
static TmN *bp_tm(BP*b){ if(b->ntm>=b->cap_tm){int nc=b->cap_tm?b->cap_tm*2:64; b->tm=realloc(b->tm,(size_t)nc*sizeof(TmN)); b->cap_tm=nc;} TmN*n=&b->tm[b->ntm++]; memset(n,0,sizeof*n); return n; }
static InN *bp_in(BP*b){ if(b->nin>=b->cap_in){int nc=b->cap_in?b->cap_in*2:64; b->in=realloc(b->in,(size_t)nc*sizeof(InN)); b->cap_in=nc;} InN*n=&b->in[b->nin++]; memset(n,0,sizeof*n); return n; }
static WvN *bp_wv(BP*b){ if(b->nwv>=b->cap_wv){int nc=b->cap_wv?b->cap_wv*2:64; b->wv=realloc(b->wv,(size_t)nc*sizeof(WvN)); b->cap_wv=nc;} WvN*n=&b->wv[b->nwv++]; memset(n,0,sizeof*n); return n; }
static PmN *bp_pm(BP*b){ if(b->npm>=b->cap_pm){int nc=b->cap_pm?b->cap_pm*2:64; b->pm=realloc(b->pm,(size_t)nc*sizeof(PmN)); b->cap_pm=nc;} PmN*n=&b->pm[b->npm++]; memset(n,0,sizeof*n); return n; }
static InN *bp_find_in(BP*b,const uint8_t*g){ for(int i=0;i<b->nin;i++) if(!memcmp(b->in[i].g.g,g,16)) return &b->in[i]; return NULL; }
static TmN *bp_find_tm(BP*b,const uint8_t*g){ for(int i=0;i<b->ntm;i++) if(!memcmp(b->tm[i].g.g,g,16)) return &b->tm[i]; return NULL; }
static WvN *bp_find_wv(BP*b,const uint8_t*g){ for(int i=0;i<b->nwv;i++) if(!memcmp(b->wv[i].g.g,g,16)) return &b->wv[i]; return NULL; }
static PmN *bp_find_pm(BP*b,const uint8_t*g){ for(int i=0;i<b->npm;i++) if(!memcmp(b->pm[i].g.g,g,16)) return &b->pm[i]; return NULL; }

/* ReadElemListImp: count=x16>>1, then one u16 element-stride, then count*stride bytes. Returns the
 * offset of the first element; sets *count,*stride; advances *o past the whole list. */
static size_t fx_list(const uint8_t*b,size_t len,size_t*o,uint32_t*count,uint32_t*stride){
  uint32_t raw=fx_x16(b,len,o); uint32_t c=raw>>1;
  if(c==0){*count=0;*stride=0;return *o;}
  if(*o+2>len){*o=len;*count=0;*stride=0;return *o;}
  uint32_t st=rd_u16(b+*o); *o+=2;
  size_t data=*o; *o+=(size_t)c*st; if(*o>len)*o=len;
  *count=c;*stride=st;return data;
}
/* ReadVersionedElemListImp: count=x16>>1, then per element: u16 size + `size` bytes. Skip only. */
static void fx_vlist_skip(const uint8_t*b,size_t len,size_t*o){
  uint32_t raw=fx_x16(b,len,o); uint32_t c=raw>>1;
  for(uint32_t i=0;i<c;i++){ if(*o+2>len){*o=len;return;} uint32_t sz=rd_u16(b+*o); *o+=2+sz; if(*o>len){*o=len;return;} }
}

static void bp_push(BP*b,uint32_t id,const uint8_t*g){ if(b->psp<64){ b->pstk[b->psp].id=id; memcpy(b->pstk[b->psp].g.g,g,16); b->psp++; } }

/* --- individual chunk-body parsers (o = body start, end = body end) --- */
static void parse_evtb(BP*bp,const uint8_t*b,size_t o,size_t end){
  EvN*n=bp_ev(bp); if(o+16>end)return; memcpy(n->g.g,b+o,16); o+=16;
  o+=16;                                   /* SnapshotGuid */
  if(o+16<=end) memcpy(n->timeline.g,b+o,16);
  o+=16;   /* TimelineGuid */
  o+=16; o+=16;                            /* InputBus, MasterTrack */
  o+=4; o+=4; o+=1; o+=4;                  /* MaxPoly, Priority, PolyLimit(byte), Scheduling */
  { uint32_t c,st; size_t d=fx_list(b,end,&o,&c,&st); for(uint32_t i=0;i<c;i++) if(st>=16) garr_push(&n->params,b+d+(size_t)i*st); } /* ParameterLayouts */
  if(bp->version<0x3C){ o+=2; }
  fx_vlist_skip(b,end,&o);                 /* UserPropertyFloatList */
  fx_vlist_skip(b,end,&o);                 /* UserPropertyStringList */
  if(bp->version>=0x30) o+=4;              /* DopplerScale */
  if(bp->version>=0x34) o+=4;              /* PolyphonyLimitBehavior (i32) */
  if(bp->version>=0x4e) o+=4;              /* TriggerCooldown */
  if(bp->version>=0x61) o+=4;              /* Flags */
  if(bp->version>=0x6b){ uint32_t c,st; fx_list(b,end,&o,&c,&st); }   /* NonMasterTracks */
  if(bp->version>=0x76){ uint32_t c,st; fx_list(b,end,&o,&c,&st); }   /* ParameterIds */
  if(bp->version>=0x83){ uint32_t c,st; size_t d=fx_list(b,end,&o,&c,&st); for(uint32_t i=0;i<c;i++) if(st>=16) garr_push(&n->triggered,b+d+(size_t)i*st); } /* EventTriggeredInstruments */
  if(bp->version>=0x89 && o+8<=end){ n->mindist=rd_f32(b+o); n->maxdist=rd_f32(b+o+4); }   /* 3D Min/MaxDistance */
}
static void bp_add_marker(BP*bp,const uint8_t*g,uint32_t pos){
  if(bp->nmk>=bp->cap_mk){ int nc=bp->cap_mk?bp->cap_mk*2:64; bp->mk=realloc(bp->mk,(size_t)nc*sizeof(*bp->mk)); bp->cap_mk=nc; }
  memcpy(bp->mk[bp->nmk].g.g,g,16); bp->mk[bp->nmk].pos=pos; bp->nmk++;
}
static void parse_tlnb(BP*bp,const uint8_t*b,size_t o,size_t end){
  TmN*n=bp_tm(bp); if(o+16>end)return; memcpy(n->g.g,b+o,16); o+=16;
  if(bp->version<0x6d) o+=16;              /* LegacyGuid */
  { uint32_t c,st; size_t d=fx_list(b,end,&o,&c,&st); for(uint32_t i=0;i<c;i++) if(st>=16) garr_push(&n->boxes,b+d+(size_t)i*st); } /* TriggerBoxes (guid@0) */
  { uint32_t c,st; size_t d=fx_list(b,end,&o,&c,&st); for(uint32_t i=0;i<c;i++) if(st>=16) garr_push(&n->boxes,b+d+(size_t)i*st); } /* TimeLockedTriggerBoxes */
  if(bp->version>=0x84) fx_vlist_skip(b,end,&o);   /* SustainPoints */
  /* TimelineNamedMarkers (versioned): each = BaseGuid(16) + Position u32(4) + Name(string) + ... */
  { uint32_t raw=fx_x16(b,end,&o); uint32_t c=raw>>1;
    for(uint32_t i=0;i<c;i++){ if(o+2>end){o=end;break;} uint32_t sz=rd_u16(b+o); o+=2;
      if(o+20<=end){ uint32_t pos=rd_u32(b+o+16); bp_add_marker(bp,b+o,pos); garr_push(&n->mkg,b+o); }
      o+=sz; if(o>end){o=end;break;} } }
}
static void parse_trnb(BP*bp,const uint8_t*b,size_t o,size_t end){
  if(o+36>end) return;                     /* BaseGuid(16) + DestinationGuid(16) + Start u32 + End u32 */
  if(bp->ntr>=bp->cap_tr){ int nc=bp->cap_tr?bp->cap_tr*2:64; bp->tr=realloc(bp->tr,(size_t)nc*sizeof(*bp->tr)); bp->cap_tr=nc; }
  memcpy(bp->tr[bp->ntr].dest.g,b+o+16,16); bp->tr[bp->ntr].start=rd_u32(b+o+32); bp->ntr++;
}
static void parse_waib(BP*bp,const uint8_t*b,size_t o,size_t end){
  InN*n=bp_in(bp); n->kind=0; n->loopcount=1; if(o+16>end)return; memcpy(n->g.g,b+o,16); o+=16;
  if(bp->version<0x46) o+=4;               /* LegacyLoadingMode */
  if(o+16<=end) memcpy(n->wavres.g,b+o,16);
  bp_push(bp,FID_WAIB,n->g.g);
}
static void parse_muib(BP*bp,const uint8_t*b,size_t o,size_t end,uint32_t id,int kind){
  InN*n=bp_in(bp); n->kind=kind; n->loopcount=1; if(o+16>end)return; memcpy(n->g.g,b+o,16);
  bp_push(bp,id,n->g.g);
}
static void parse_plst(BP*bp,const uint8_t*b,size_t o,size_t end){
  if(bp->psp<=0) return;
  uint32_t tid=bp->pstk[bp->psp-1].id; FGuid tg=bp->pstk[bp->psp-1].g;
  if(tid!=FID_MUIB && tid!=FID_SPIB) return;
  InN*inst=bp_find_in(bp,tg.g);
  o+=4; o+=4;                              /* PlayMode, SelectionMode */
  uint32_t c,st; size_t d=fx_list(b,end,&o,&c,&st);
  if(inst) for(uint32_t i=0;i<c;i++) if(st>=16) garr_push(&inst->pl,b+d+(size_t)i*st);  /* FPlaylistEntry guid@0 */
  bp->psp--; bp_push(bp,FID_PLST,tg.g);    /* pop MUIB/SPIB, re-push same guid so INST attaches */
}
static void parse_inst(BP*bp,const uint8_t*b,size_t o,size_t end){
  if(bp->psp<=0) return;
  FGuid tg=bp->pstk[bp->psp-1].g;
  InN*inst=bp_find_in(bp,tg.g);
  if(inst){
    if(o+16<=end) memcpy(inst->inst_tl.g,b+o,16);          /* TimelineGuid */
    size_t lp=o+16+4+4;                                     /* skip Volume, Pitch */
    if(lp+4<=end) inst->loopcount=(int)rd_u32(b+lp);        /* LoopCount */
  }
  bp->psp--;
}
static void parse_wavr(BP*bp,const uint8_t*b,size_t o,size_t end){
  WvN*n=bp_wv(bp); if(o+16>end)return; memcpy(n->g.g,b+o,16); o+=16;
  o+=2;                                    /* payload size */
  if(o+8<=end){ n->sb=(int)rd_u32(b+o); n->ss=(int)rd_u32(b+o+4); }
}
static void parse_pmlb(BP*bp,const uint8_t*b,size_t o,size_t end){
  PmN*n=bp_pm(bp); if(o+16>end)return; memcpy(n->g.g,b+o,16); o+=16;
  o+=16;                                   /* ParameterGuid */
  if(bp->version<0x6d) o+=16;              /* LegacyGuid */
  if(bp->version>=0x82){ uint32_t c,st; size_t d=fx_list(b,end,&o,&c,&st); for(uint32_t i=0;i<c;i++) if(st>=16) garr_push(&n->instruments,b+d+(size_t)i*st); }
  /* older layouts carry trigger-box instrument guids; v0x8e uses the Instruments list above */
}

/* recursive chunk walk over a LIST payload range */
static void fmod_walk(BP*bp,const uint8_t*b,size_t pos,size_t end){
  while(pos+8<=end){
    uint32_t id=rd_u32(b+pos);
    if((id&0xFF)==0){ pos+=1; if(pos+8>end) break; id=rd_u32(b+pos); }
    size_t nodeStart=pos;
    uint32_t size=rd_u32(b+nodeStart+4);
    size_t body=nodeStart+8;
    size_t nextNode=nodeStart+8+(size_t)size;
    if(nextNode>end) nextNode=end;
    if(size==0){ pos=nextNode; continue; }
    switch(id){
      case FID_LIST: if(body+4<=nextNode) fmod_walk(bp,b,body+4,nextNode); break;  /* skip 4-byte list id */
      case FID_EVTB: parse_evtb(bp,b,body,nextNode); break;
      case FID_TLNB: parse_tlnb(bp,b,body,nextNode); break;
      case FID_WAIB: parse_waib(bp,b,body,nextNode); break;
      case FID_MUIB: parse_muib(bp,b,body,nextNode,FID_MUIB,1); break;
      case FID_SPIB: parse_muib(bp,b,body,nextNode,FID_SPIB,2); break;
      case FID_PLST: parse_plst(bp,b,body,nextNode); break;
      case FID_INST: parse_inst(bp,b,body,nextNode); break;
      case FID_WAVR: parse_wavr(bp,b,body,nextNode); break;
      case FID_PMLB: parse_pmlb(bp,b,body,nextNode); break;
      case FID_TRNB: parse_trnb(bp,b,body,nextNode); break;
      default: break;
    }
    pos=nextNode;
  }
}

/* DFS-resolve one event's GUID graph → subsounds (+loop flag). */
static void bp_resolve_event(BP*bp, EvN*ev, int sub_out[8], int *nsub_out, int *loop_out){
  GArr stack={0}, visited={0}; int nsub=0, loop=0;
  garr_push(&stack, ev->timeline.g);
  for(int i=0;i<ev->triggered.n;i++) garr_push(&stack, ev->triggered.d[i].g);
  for(int i=0;i<ev->params.n;i++){
    PmN*pm=bp_find_pm(bp, ev->params.d[i].g);
    if(pm){ for(int j=0;j<pm->instruments.n;j++) garr_push(&stack, pm->instruments.d[j].g); }
    else garr_push(&stack, ev->params.d[i].g);
  }
  int guard=0;
  while(stack.n>0 && guard++<100000){
    FGuid g=stack.d[--stack.n];
    if(garr_has(&visited,g.g)) continue;
    garr_push(&visited,g.g);
    TmN*tm=bp_find_tm(bp,g.g);
    if(tm){ for(int i=0;i<tm->boxes.n;i++) garr_push(&stack,tm->boxes.d[i].g); }
    InN*in=bp_find_in(bp,g.g);
    if(in){
      if(memcmp(in->inst_tl.g,"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",16)!=0) garr_push(&stack,in->inst_tl.g);
      if(in->kind==0){
        WvN*wv=bp_find_wv(bp,in->wavres.g);
        if(wv){
          int dup=0; for(int k=0;k<nsub;k++) if(sub_out[k]==wv->ss) dup=1;
          if(!dup && nsub<8) sub_out[nsub++]=wv->ss;
          if(in->loopcount==-1) loop=1;
        }
      } else {
        for(int i=0;i<in->pl.n;i++) garr_push(&stack,in->pl.d[i].g);
      }
    }
  }
  garr_free(&stack); garr_free(&visited);
  *nsub_out=nsub; *loop_out=loop;
}

static void fmod_bank_parse_events(FBank *bank, const uint8_t *list, size_t list_len, int version){
  BP bp; memset(&bp,0,sizeof bp);
  bp.version=version?version:0x8e;   /* FMT FileVersion; fallback keeps old behavior for malformed banks */
  /* the LIST payload begins with a 4-byte list-type id (skipped), then sub-chunks */
  size_t start = (list_len>=4)? 4 : list_len;
  fmod_walk(&bp,list,start,list_len);

  /* resolve every event → subsound(s) + loop region */
  bank->ev=calloc(bp.nev?bp.nev:1,sizeof(*bank->ev)); bank->nev=0;
  for(int e=0;e<bp.nev;e++){
    int subs[8], nsub=0, loop=0;
    bp_resolve_event(&bp,&bp.ev[e],subs,&nsub,&loop);
    if(nsub>0){
      memcpy(bank->ev[bank->nev].g.g, bp.ev[e].g.g, 16);
      for(int k=0;k<nsub;k++) bank->ev[bank->nev].sub[k]=subs[k];
      bank->ev[bank->nev].nsub=nsub;
      bank->ev[bank->nev].loop=loop;
      bank->ev[bank->nev].mindist=bp.ev[e].mindist;
      bank->ev[bank->nev].maxdist=bp.ev[e].maxdist;
      /* collect loop-back candidates: transitions whose destination is one of this event's timeline
       * markers (loop = [destPos, Start]). The final region is chosen against the sample length in
       * the post-pass, so a spurious past-the-end transition can't win. */
      TmN *tm=bp_find_tm(&bp, bp.ev[e].timeline.g);
      if(tm){
        int nlc=0;
        for(int t=0;t<bp.ntr && nlc<8;t++){
          if(!garr_has(&tm->mkg, bp.tr[t].dest.g)) continue;
          uint32_t dpos=0; for(int m=0;m<bp.nmk;m++) if(!memcmp(bp.mk[m].g.g,bp.tr[t].dest.g,16)){ dpos=bp.mk[m].pos; break; }
          if(bp.tr[t].start>dpos){ bank->ev[bank->nev].lc[nlc].start=bp.tr[t].start; bank->ev[bank->nev].lc[nlc].dest=dpos; nlc++; }
        }
        bank->ev[bank->nev].nlc=nlc;
      }
      bank->nev++;
    }
  }
  if(fmod_bank_setting(bank,"GML_DBG_FMOD")){ int wp=0,wt=0; for(int e=0;e<bp.nev;e++){ if(bp.ev[e].params.n>0) wp++; if(bp.ev[e].triggered.n>0) wt++; }
    anygm_host_logf(bank ? bank->host : NULL,ANYGM_LOG_DEBUG,
                    "[fmod]   events w/ parameters: %d, w/ triggered-instruments: %d (of %d); markers=%d transitions=%d\n",
                    wp,wt,bp.nev,bp.nmk,bp.ntr); }
  /* free transient tables */
  for(int i=0;i<bp.nev;i++){ garr_free(&bp.ev[i].params); garr_free(&bp.ev[i].triggered); }
  for(int i=0;i<bp.ntm;i++){ garr_free(&bp.tm[i].boxes); garr_free(&bp.tm[i].mkg); }
  for(int i=0;i<bp.nin;i++) garr_free(&bp.in[i].pl);
  for(int i=0;i<bp.npm;i++) garr_free(&bp.pm[i].instruments);
  free(bp.ev); free(bp.tm); free(bp.in); free(bp.wv); free(bp.pm); free(bp.mk); free(bp.tr);
}

/* Open one main .bank file: locate its LIST (metadata → event graph) and SND (FSB5) chunks. */
static int fmod_bank_open(FBank *bank,const AnygmHostServices *host,const char *path){
  if(!bank || !host || !host->file_open || !host->file_read || !host->file_seek ||
     !host->file_close) return 0;
  bank->host=host;
  bank->file=host->file_open(host->userdata,path,ANYGM_FILE_READ);
  if(!bank->file) return 0;
  uint8_t head[12];
  int64_t end=host->file_seek(host->userdata,bank->file,0,ANYGM_SEEK_END);
  if(end<12 || !fmod_bank_read(bank,0,head,sizeof head) ||
     memcmp(head,"RIFF",4)!=0 || memcmp(head+8,"FEV ",4)!=0){
    host->file_close(host->userdata,bank->file); bank->file=NULL; return 0;
  }
  uint64_t fsz=(uint64_t)end;
  uint64_t base=12;
  int have_snd=0,have_events=0,fmt_version=0;
  while(base+8<=fsz){
    uint8_t chdr[8];
    if(!fmod_bank_read(bank,base,chdr,sizeof chdr)) break;
    uint32_t csz=rd_u32(chdr+4); uint64_t payload=base+8;
    if(csz>fsz-payload) break;
    if(memcmp(chdr,"FMT ",4)==0){
      uint8_t fmt[8]; size_t amount=csz>=8?8:4;
      if(csz>=4 && fmod_bank_read(bank,payload,fmt,amount)) fmt_version=(int)rd_u32(fmt);
    } else if(memcmp(chdr,"LIST",4)==0){
      /* metadata: read into RAM (bounded; ≤~1 MB) and parse the event graph */
      uint8_t *lst=csz<=64u*1024u*1024u?malloc(csz?csz:1):NULL;
      if(lst){
        if(fmod_bank_read(bank,payload,lst,csz)){ fmod_bank_parse_events(bank,lst,csz,fmt_version); have_events=1; }
        free(lst); }
    } else if(memcmp(chdr,"SND ",4)==0){
      /* the FSB5 begins at (or shortly after) the SND payload — scan a small window for the magic */
      uint8_t probe[64]; size_t pr=csz<sizeof probe?csz:sizeof probe;
      if(!fmod_bank_read(bank,payload,probe,pr)) pr=0;
      uint64_t fsb5_off=UINT64_MAX;
      for(size_t i=0;i+4<=pr;i++) if(memcmp(probe+i,"FSB5",4)==0){ fsb5_off=payload+i; break; }
      if(fsb5_off!=UINT64_MAX){
        uint8_t h0[60];
        if(fsb5_off<=fsz-sizeof h0 && fmod_bank_read(bank,fsb5_off,h0,sizeof h0) &&
           memcmp(h0,"FSB5",4)==0){
          uint32_t shs=rd_u32(h0+12), nts=rd_u32(h0+16);
          size_t hdrlen=60+(size_t)shs+(size_t)nts;   /* include the name table */
          uint8_t *hdr=hdrlen<=64u*1024u*1024u?malloc(hdrlen):NULL;
          if(hdr){
            if(fsb5_off<=SIZE_MAX && hdrlen<=fsz-fsb5_off &&
               fmod_bank_read(bank,fsb5_off,hdr,hdrlen)){
              size_t data_region=(size_t)fsb5_off+60u+shs+nts;
              if(fmod_fsb5_parse_table(bank,hdr,hdrlen,data_region)) have_snd=1;
            }
            free(hdr); }
        }
      }
    }
    base=payload+csz;
  }
  (void)have_events;
  if(!have_snd){ bank->subs=NULL; bank->nsubs=0; }  /* Master.bank has no SND — buses only */
  /* A looping event's loop region lives in timeline markers that are not parsed. A long sound is
   * treated as music or ambience and looped. This content-derived rule
   * makes create_instance'd music loop; one-shots override it to play once anyway. */
  for(int e=0;e<bank->nev;e++){
    int ss=bank->ev[e].sub[0];
    if(ss<0 || ss>=bank->nsubs || !bank->subs || bank->subs[ss].rate<=0){ bank->ev[e].nlc=0; continue; }
    uint32_t samples=bank->subs[ss].samples; int rate=bank->subs[ss].rate;
    if(!bank->ev[e].loop && (double)samples/rate>=10.0) bank->ev[e].loop=1;
    /* Accept a loop-back only when it lands near the END of the track (the normal end-of-body loop):
     * Start within [0.80·samples, samples], destination before it, ≥1s of body. This keeps the clear
     * simple loops and rejects mid-track, short, or interactive candidates in favor of a full loop,
     * so a wrong region can never make a track worse than the safe whole-track loop. */
    uint32_t lo_ok=(uint32_t)(0.80*(double)samples), best_end=0, best_start=0;
    for(int i=0;i<bank->ev[e].nlc;i++){
      uint32_t st=bank->ev[e].lc[i].start, ds=bank->ev[e].lc[i].dest;
      if(st<=samples && st>=lo_ok && ds<st && (st-ds)>=(uint32_t)rate && st>best_end){ best_end=st; best_start=ds; }
    }
    bank->ev[e].loop_start=best_start; bank->ev[e].loop_end=best_end;
  }
  size_t path_size=strlen(path);
  if(path_size>=sizeof bank->path) path_size=sizeof bank->path-1;
  memcpy(bank->path,path,path_size);
  bank->path[path_size]='\0';
  return 1;
}

GmlFmodBanks *gml_fmod_banks_load(const AnygmHostServices *host,const char *dir){
  if(!host || !dir || !dir[0]) return NULL;
  /* FMOD-GameMaker games keep banks in content/sound/<platform>/. Try the usual layouts. */
  static const char *sub[]={ "/sound/Desktop", "/sound/desktop", "/sound", "" };
  char bankdir[700]={0}; char sp[760];
  uint8_t *sbuf=NULL; size_t ssz=0;
  for(int i=0;i<4;i++){
    snprintf(bankdir,sizeof bankdir,"%s%s",dir,sub[i]);
    snprintf(sp,sizeof sp,"%s/Master.strings.bank",bankdir);
    if(anygm_vfs_read_all(host,sp,&sbuf,&ssz,64u*1024u*1024u)) break;
  }
  if(!sbuf || !ssz) return NULL;
  dir=bankdir;   /* open the main banks from the same folder */
  GmlFmodBanks *b=calloc(1,sizeof *b);
  if(!b){ free(sbuf); return NULL; }
  b->host=host;
  b->nstrings=fmod_parse_strings(sbuf,ssz,&b->paths,&b->guids);
  free(sbuf);
  if(b->nstrings<=0){ free(b); return NULL; }
  /* Attempt the three named banks below in order. */
  static const char *names[]={"Master.bank","music.bank","sfx.bank"};
  for(int i=0;i<3 && b->nbanks<FMOD_MAXBANKS;i++){
    char p[760]; snprintf(p,sizeof p,"%s/%s",dir,names[i]);
    if(fmod_bank_open(&b->banks[b->nbanks],host,p)) b->nbanks++;
  }
  if(fmod_setting(b,"GML_DBG_FMOD_3D")){
    for(int bi=0;bi<b->nbanks;bi++){ FBank*bk=&b->banks[bi]; int shown=0, with3d=0;
      for(int e=0;e<bk->nev;e++){ if(bk->ev[e].maxdist>0){ with3d++;
        if(shown<8){ const char*nm=(bk->names&&bk->ev[e].sub[0]<bk->nsubs)?bk->names[bk->ev[e].sub[0]]:"?";
          anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"[fmod3d] %s min=%.1f max=%.1f\n",nm?nm:"?",bk->ev[e].mindist,bk->ev[e].maxdist); shown++; } } }
      anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"[fmod3d] bank %d: %d/%d events have 3D rolloff (maxdist>0)\n",bi,with3d,bk->nev); }
  }
  if(fmod_setting(b,"GML_DBG_FMOD")){
    anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"[fmod] bank set: %d string paths, %d banks\n",b->nstrings,b->nbanks);
    int multi=0,tot=0; for(int i=0;i<b->nbanks;i++) for(int e=0;e<b->banks[i].nev;e++){ tot++; if(b->banks[i].ev[e].nsub>1) multi++; }
    anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"[fmod] events with >1 subsound (multi-instrument): %d / %d\n",multi,tot);
    for(int i=0;i<b->nbanks;i++) anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"[fmod]   %s: %d subsounds, %d events\n",
      b->banks[i].path,b->banks[i].nsubs,b->banks[i].nev);
  }
  if(fmod_setting(b,"GML_DBG_FMOD_MAP")){
    int res=0,unres=0;
    for(int i=0;i<b->nstrings;i++){ if(!b->paths[i]||strncmp(b->paths[i],"event:",6)) continue;
      int bi,ss,lp;
      if(gml_fmod_banks_resolve(b,b->paths[i],&bi,&ss,&lp,NULL,NULL)){ res++;
        const char*nm=(b->banks[bi].names&&ss<b->banks[bi].nsubs)?b->banks[bi].names[ss]:NULL;
        anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"MAP %-46s -> bank%d sub%-4d loop%d  %s\n",b->paths[i],bi,ss,lp,nm?nm:"(noname)");
      } else { unres++; anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"MAP %-46s -> UNRESOLVED\n",b->paths[i]); }
    }
    anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"[fmod] resolved %d / %d event paths\n",res,res+unres);
  }
  return b;
}

void gml_fmod_banks_rebind_host(GmlFmodBanks *b,const AnygmHostServices *host){
  if(!b) return;
  b->host=host;
  for(int index=0;index<b->nbanks;index++) b->banks[index].host=host;
}

void gml_fmod_banks_free(GmlFmodBanks *b){
  if(!b) return;
  for(int i=0;i<b->nstrings;i++) free(b->paths[i]);
  free(b->paths); free(b->guids);
  for(int i=0;i<b->nbanks;i++){ FBank *bk=&b->banks[i];
    if(bk->file && bk->host && bk->host->file_close)
      bk->host->file_close(bk->host->userdata,bk->file);
    if(bk->names){ for(int s=0;s<bk->nsubs;s++) free(bk->names[s]); free(bk->names); }
    free(bk->subs); free(bk->ev); }
  for(int i=0;i<FMOD_MAXVOICES;i++) fmod_voice_free_audio(&b->voices[i]);
  for(int i=0;i<b->ncache;i++) free(b->cache[i].pcm);
  free(b->cache);
  free(b);
}

int gml_fmod_banks_num_events(GmlFmodBanks *b){
  if(!b) return 0;
  int n=0; for(int i=0;i<b->nbanks;i++) n+=b->banks[i].nev; return n;
}

/* Resolve an event path → (bank, subsound, loop). Path→GUID via the string table, GUID→subsound via
 * the per-bank event graph. Returns 0 (silent) for anything unresolved — never a guessed sound. */
int gml_fmod_banks_resolve(GmlFmodBanks *b, const char *path, int *bank_index, int *subsound, int *loop,
                           uint32_t *loop_start, uint32_t *loop_end){
  if(!b||!path) return 0;
  const FGuid *g=NULL;
  for(int i=0;i<b->nstrings;i++) if(b->paths[i] && strcmp(b->paths[i],path)==0){ g=&b->guids[i]; break; }
  if(!g) return 0;
  for(int bi=0;bi<b->nbanks;bi++){ FBank *bk=&b->banks[bi];
    for(int e=0;e<bk->nev;e++) if(memcmp(bk->ev[e].g.g,g->g,16)==0){
      int ns=bk->ev[e].nsub; if(ns<=0) return 0;
      b->rng=b->rng*1103515245u+12345u;
      int pick=bk->ev[e].sub[(b->rng>>16)%(unsigned)ns];   /* random variant for multi-instruments */
      if(pick<0||pick>=bk->nsubs) return 0;
      *bank_index=bi; *subsound=pick; *loop=bk->ev[e].loop;
      if(loop_start) *loop_start=bk->ev[e].loop_start;
      if(loop_end)   *loop_end=bk->ev[e].loop_end;
      return 1;
    }
  }
  return 0;
}

/* Decode a resolved (bank, subsound) to interleaved S16, pulling the compressed bytes from disk. */
int gml_fmod_banks_decode(GmlFmodBanks *b, int bank_index, int subsound, int *channels, int *rate, int16_t **out_pcm){
  if(!b||bank_index<0||bank_index>=b->nbanks) return 0;
  FBank *bk=&b->banks[bank_index];
  if(subsound<0||subsound>=bk->nsubs||!bk->file) return 0;
  FSub *s=&bk->subs[subsound];
  if(s->dlen==0) return 0;
  uint8_t *cd=malloc(s->dlen);
  if(!cd) return 0;
  if(!fmod_bank_read(bk,(uint64_t)bk->data_file_off+s->doff,cd,s->dlen)){
    free(cd); return 0;
  }
  size_t ogg_len; uint8_t *ogg=rebuild_ogg(s->ch,s->rate,s->crc,cd,s->dlen,&ogg_len);
  free(cd);
  if(!ogg){
    if(fmod_setting(b,"GML_DBG_FMOD"))
      anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"[fmod] decode: no codebook crc=0x%08x (bank %d sub %d)\n",
              s->crc,bank_index,subsound);
    return 0;
  }
  int ch=0,sr=0; short *pcm=NULL;
  int frames=stb_vorbis_decode_memory(ogg,(int)ogg_len,&ch,&sr,&pcm);
  free(ogg);
  if(frames<=0){ free(pcm); return 0; }
  *channels=ch; *rate=sr; *out_pcm=pcm; return frames;
}

/* =============================================================================================
 * Runtime playback: event-instance voices + software mixer.
 * ============================================================================================= */

/* Get decoded PCM for (bank,subsound). Small samples are cached & shared (SFX replay a lot); large
 * ones (music) are decoded fresh and owned by the voice. *owned tells the caller who frees it. */
static int16_t *fmod_get_pcm(GmlFmodBanks *b, int bank, int sub, int *frames, int *ch, int *rate, int *owned){
  for(int i=0;i<b->ncache;i++) if(b->cache[i].bank==bank && b->cache[i].sub==sub){
    *frames=b->cache[i].frames; *ch=b->cache[i].ch; *rate=b->cache[i].rate; *owned=0; return b->cache[i].pcm; }
  int c=0,r=0; int16_t *pcm=NULL;
  int fr=gml_fmod_banks_decode(b,bank,sub,&c,&r,&pcm);
  if(fr<=0) return NULL;
  *frames=fr; *ch=c; *rate=r;
  if((size_t)fr*(size_t)c*2 < 4u*1024*1024){   /* cache small samples permanently */
    if(b->ncache>=b->cap_cache){ int nc=b->cap_cache?b->cap_cache*2:32; b->cache=realloc(b->cache,(size_t)nc*sizeof(FCache)); b->cap_cache=nc; }
    b->cache[b->ncache]=(FCache){bank,sub,fr,c,r,pcm}; b->ncache++;
    *owned=0; return pcm;
  }
  *owned=1; return pcm;
}

static FVoice *fmod_voice_by_handle(GmlFmodBanks *b, int handle){
  if(handle<=0) return NULL;
  int hint=b->vhint[(unsigned)handle & 4095u];
  if(hint>=0 && hint<FMOD_MAXVOICES && b->vh[hint]==handle) return &b->voices[hint];
  for(int i=0;i<FMOD_MAXVOICES;i++) if(b->vh[i]==handle){ b->vhint[(unsigned)handle & 4095u]=i; return &b->voices[i]; }
  return NULL;
}
static void fmod_voice_set_handle(GmlFmodBanks *b, FVoice *v, int handle){
  int slot=(int)(v - b->voices);
  v->handle=handle; b->vh[slot]=handle;
  if(handle>0) b->vhint[(unsigned)handle & 4095u]=slot;
}

/* Retain compressed Ogg data and decode PCM through a bounded window. */
static void fmod_stream_open(GmlFmodBanks *b, FVoice *v){
  FBank *bk=&b->banks[v->bank]; FSub *s=&bk->subs[v->sub];
  if(s->dlen==0||!bk->file) return;
  uint8_t *cd=malloc(s->dlen); if(!cd) return;
  if(!fmod_bank_read(bk,(uint64_t)bk->data_file_off+s->doff,cd,s->dlen)){
    free(cd); return;
  }
  size_t ogg_len; uint8_t *ogg=rebuild_ogg(s->ch,s->rate,s->crc,cd,s->dlen,&ogg_len); free(cd);
  if(!ogg) return;
  int err=0; stb_vorbis *vs=stb_vorbis_open_memory(ogg,(int)ogg_len,&err,NULL);
  if(!vs){ free(ogg); return; }
  v->vs=vs; v->ogg=ogg; v->ch=s->ch; v->rate=s->rate; v->frames=s->samples;
  v->win_start=0; v->win_len=0; v->stream_pos=0;
}

/* Ensure the decode window covers source frames [first, last]. Drops the consumed prefix and decodes
 * forward (looping via seek_start). Returns 0 if the stream ended (non-loop) before `last`. */
static int fmod_stream_fill(GmlFmodBanks *b,FVoice *v,long first,long last){
  int ch=v->ch;
  if(first>v->win_start){
    long drop=first-v->win_start;
    if(drop>=v->win_len){ v->win_len=0; v->win_start=first; }
    else { memmove(v->win, v->win+drop*ch, (size_t)(v->win_len-drop)*ch*sizeof(int16_t)); v->win_len-=(int)drop; v->win_start=first; }
  }
  long need=last-v->win_start+1;
  if(need<=0) return 1;
  if(need>v->win_cap){ v->win_cap=(int)need+2048; int16_t *nw=realloc(v->win,(size_t)v->win_cap*ch*sizeof(int16_t)); if(!nw) return 0; v->win=nw; }
  while(v->win_len<need){
    long want=need-v->win_len;
    /* with a loop region, never decode past loop_end in one call so we can jump back exactly to it */
    if(v->loop && v->loop_end>0 && v->stream_pos<v->loop_end){
      long to_le=v->loop_end-v->stream_pos; if(want>to_le) want=to_le;
    }
    int got=stb_vorbis_get_samples_short_interleaved(v->vs,ch,v->win+(size_t)v->win_len*ch,(int)(want*ch));
    if(got<=0){
      if(v->loop){ if(v->loop_end>0){ stb_vorbis_seek(v->vs,(unsigned)v->loop_start); v->stream_pos=v->loop_start; }
                   else { stb_vorbis_seek_start(v->vs); v->stream_pos=0; } continue; }
      return 0;
    }
    v->win_len+=got; v->stream_pos+=got;
    if(v->loop && v->loop_end>0 && v->stream_pos>=v->loop_end){   /* reached loop end → jump to loop start */
      if(fmod_setting(b,"GML_DBG_FMOD_LOOP"))
        anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"[fmod] loop: %ld -> %ld (region end %ld)\n",
                v->stream_pos,v->loop_start,v->loop_end);
      stb_vorbis_seek(v->vs,(unsigned)v->loop_start); v->stream_pos=v->loop_start;
    }
  }
  return 1;
}

static void fmod_voice_decode(GmlFmodBanks *b, FVoice *v){
  if(v->pcm||v->vs) return;
  FBank *bk=&b->banks[v->bank];
  if(v->sub<0||v->sub>=bk->nsubs) return;
  long est=(long)bk->subs[v->sub].samples*bk->subs[v->sub].ch*2;
  if(est > 4*1024*1024){ fmod_stream_open(b,v); }          /* music → stream */
  else { int owned=0; v->pcm=fmod_get_pcm(b,v->bank,v->sub,&v->frames,&v->ch,&v->rate,&owned); v->own_pcm=owned; }
}

/* Re-resolve the event path on playback and prepare the selected subsound.
 * If the selection changes, release its previous audio before preparing the new one. */
static void fmod_voice_arm(GmlFmodBanks *b, FVoice *v){
  int bank,sub,loop; uint32_t ls=0,le=0;
  if(v->path[0] && gml_fmod_banks_resolve(b,v->path,&bank,&sub,&loop,&ls,&le)){
    if(bank!=v->bank || sub!=v->sub){ fmod_voice_free_audio(v); v->bank=bank; v->sub=sub; }
    v->loop = v->one_shot?0:loop;
    v->loop_start = v->one_shot?0:(long)ls;
    v->loop_end   = v->one_shot?0:(long)le;
  }
  fmod_voice_decode(b,v);
}

/* release a voice's audio resources (streamed or owned PCM) without clearing its handle/state */
static void fmod_voice_free_audio(FVoice *v){
  if(v->vs){ stb_vorbis_close(v->vs); v->vs=NULL; }
  free(v->ogg); v->ogg=NULL;
  free(v->win); v->win=NULL; v->win_cap=v->win_len=0; v->win_start=0;
  if(v->own_pcm){ free(v->pcm); v->own_pcm=0; }
  v->pcm=NULL;
}

int gml_fmod_start(GmlFmodBanks *b, const char *path, int play_now, int one_shot){
  if(!b||!path) return 0;
  int bank,sub,loop;
  if(!gml_fmod_banks_resolve(b,path,&bank,&sub,&loop,NULL,NULL)) return 0;   /* unknown → silent, no voice */
  int slot=-1;
  for(int i=0;i<FMOD_MAXVOICES;i++) if(b->vh[i]==0){ slot=i; break; }
  if(slot<0){ /* steal the oldest one-shot, else a finished voice */
    for(int i=0;i<FMOD_MAXVOICES && slot<0;i++) if(b->voices[i].one_shot && !b->voices[i].active) slot=i;
    for(int i=0;i<FMOD_MAXVOICES && slot<0;i++) if(!b->voices[i].active) slot=i;
    if(slot<0) slot=0;
    fmod_voice_free_audio(&b->voices[slot]);
  }
  FVoice *v=&b->voices[slot]; memset(v,0,sizeof *v);
  if(++b->next_handle<=0) b->next_handle=1;
  fmod_voice_set_handle(b,v,b->next_handle);
  snprintf(v->path,sizeof v->path,"%s",path);   /* remember for per-play variant re-pick */
  v->bank=bank; v->sub=sub; v->loop=one_shot?0:loop; v->one_shot=one_shot?1:0; v->gain=1.0;
  if(fmod_setting(b,"GML_DBG_FMOD")){ const char*nm=(b->banks[bank].names&&sub<b->banks[bank].nsubs)?b->banks[bank].names[sub]:NULL;
    anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"[fmod] %s '%s' -> h%d bank%d sub%d loop%d %s\n",one_shot?"one_shot":"instance",path,v->handle,bank,sub,v->loop,nm?nm:""); }
  if(play_now){ fmod_voice_arm(b,v); v->active=(v->pcm!=NULL||v->vs!=NULL); if(!v->active && one_shot){ fmod_voice_set_handle(b,v,0); return 0; } }
  return v->handle;
}
void gml_fmod_play(GmlFmodBanks *b, int handle){
  FVoice *v=fmod_voice_by_handle(b,handle); if(!v) return;
  fmod_voice_arm(b,v);                                     /* re-pick variant (multi-instrument) + decode */
  if(v->vs){ stb_vorbis_seek_start(v->vs); v->win_len=0; v->win_start=0; v->stream_pos=0; }   /* restart the stream */
  v->pos=0; v->active=(v->pcm!=NULL||v->vs!=NULL); v->paused=0;
}
void gml_fmod_stop(GmlFmodBanks *b, int handle){
  FVoice *v=fmod_voice_by_handle(b,handle); if(!v) return;
  v->active=0; v->pos=0;
  fmod_voice_free_audio(v);   /* free stream/owned PCM; re-prepared on replay (cached SFX stay in cache) */
  if(v->one_shot) fmod_voice_set_handle(b,v,0);
}
void gml_fmod_release(GmlFmodBanks *b, int handle){
  FVoice *v=fmod_voice_by_handle(b,handle); if(!v) return;
  fmod_voice_free_audio(v);
  memset(v,0,sizeof *v);   /* handle=0 → slot free */
  fmod_voice_set_handle(b,v,0);
}
void gml_fmod_set_paused(GmlFmodBanks *b, int handle, int paused){
  FVoice *v=fmod_voice_by_handle(b,handle); if(v) v->paused=paused?1:0;
}
void gml_fmod_set_paused_all(GmlFmodBanks *b, int paused){
  if(!b) return;
  for(int i=0;i<FMOD_MAXVOICES;i++) if(b->voices[i].handle) b->voices[i].paused=paused?1:0;
}
int gml_fmod_is_playing(GmlFmodBanks *b, int handle){
  FVoice *v=fmod_voice_by_handle(b,handle); return (v && v->active && !v->paused)?1:0;
}
int gml_fmod_get_paused(GmlFmodBanks *b, int handle){
  FVoice *v=fmod_voice_by_handle(b,handle); return (v && v->paused)?1:0;
}
static int fmod_param_find(const char *name, void *base, int n, size_t stride){
  if(!name || !*name) return -1;
  for(int i=0;i<n;i++){
    char *pn=(char*)base + (size_t)i*stride;
    if(!strcmp(pn,name)) return i;
  }
  return -1;
}
void gml_fmod_set_param(GmlFmodBanks *b, int handle, const char *name, double value){
  FVoice *v=fmod_voice_by_handle(b,handle); if(!v || !name || !*name) return;
  int i=fmod_param_find(name,v->param,v->nparam,sizeof(v->param[0]));
  if(i<0){
    if(v->nparam>=FMOD_MAX_PARAMS) return;
    i=v->nparam++;
    snprintf(v->param[i].name,sizeof(v->param[i].name),"%s",name);
  }
  v->param[i].value=value;
}
double gml_fmod_get_param(GmlFmodBanks *b, int handle, const char *name){
  FVoice *v=fmod_voice_by_handle(b,handle); if(!v || !name || !*name) return 0;
  int i=fmod_param_find(name,v->param,v->nparam,sizeof(v->param[0]));
  return i>=0 ? v->param[i].value : 0;
}
void gml_fmod_set_global_param(GmlFmodBanks *b, const char *name, double value){
  if(!b || !name || !*name) return;
  int i=fmod_param_find(name,b->param,b->nparam,sizeof(b->param[0]));
  if(i<0){
    if(b->nparam>=FMOD_MAX_PARAMS) return;
    i=b->nparam++;
    snprintf(b->param[i].name,sizeof(b->param[i].name),"%s",name);
  }
  b->param[i].value=value;
}
double gml_fmod_get_global_param(GmlFmodBanks *b, const char *name){
  if(!b || !name || !*name) return 0;
  int i=fmod_param_find(name,b->param,b->nparam,sizeof(b->param[0]));
  return i>=0 ? b->param[i].value : 0;
}
double gml_fmod_get_length(GmlFmodBanks *b, const char *path){
  int bank,sub,loop; if(!gml_fmod_banks_resolve(b,path,&bank,&sub,&loop,NULL,NULL)) return 0;
  FBank *bk=&b->banks[bank]; if(sub<0||sub>=bk->nsubs||bk->subs[sub].rate<=0) return 0;
  return (double)bk->subs[sub].samples/(double)bk->subs[sub].rate;
}
/* current playback position in ms — lets audio-synced visuals (beat cues) track the real position */
double gml_fmod_get_timeline_pos(GmlFmodBanks *b, int handle){
  FVoice *v=fmod_voice_by_handle(b,handle); if(!v||v->rate<=0) return 0;
  double p = (v->frames>0)? fmod(v->pos,(double)v->frames) : v->pos;
  return p/(double)v->rate*1000.0;
}
void gml_fmod_set_timeline_pos(GmlFmodBanks *b, int handle, double ms){
  FVoice *v=fmod_voice_by_handle(b,handle); if(!v||v->rate<=0) return;
  long sample=(long)(ms/1000.0*v->rate); if(sample<0) sample=0;
  if(v->frames>0 && sample>=v->frames) sample=v->frames-1;
  if(v->vs){ stb_vorbis_seek(v->vs,(unsigned)sample); v->win_len=0; v->win_start=sample; }
  v->pos=(double)sample;
}
void gml_fmod_stop_all(GmlFmodBanks *b){
  if(!b) return;
  for(int i=0;i<FMOD_MAXVOICES;i++){ FVoice *v=&b->voices[i]; if(!v->handle) continue;
    v->active=0; fmod_voice_free_audio(v); if(v->one_shot){ memset(v,0,sizeof *v); b->vh[(int)(v-b->voices)]=0; } }
}
void gml_fmod_set_listener(GmlFmodBanks *b, double x, double y){
  if(!b) return;
  b->lx=x; b->ly=y; b->have_listener=1;
}
void gml_fmod_set_3d(GmlFmodBanks *b, int handle, double x, double y){
  FVoice *v=fmod_voice_by_handle(b,handle); if(!v) return; v->px=x; v->py=y; v->has_3d=1;
  if(fmod_setting(b,"GML_DBG_FMOD_3D"))
    anygm_host_logf(b ? b->host : NULL,ANYGM_LOG_DEBUG,"[fmod3d] voice h%d pos %.1f,%.1f\n",handle,x,y);
}

/* soft knee limiter (same curve as the AUDO mixer): music is mastered near 0 dBFS, so summing SFX on
 * top would hard-clip — compress the overshoot instead of clamping, preserving loudness. */
static int16_t fmod_softclip(int32_t v){
  double x=(double)v; const double knee=30000.0, span=2767.0;
  if(x>knee)       x= knee+span*(1.0-exp(-( x-knee)/span));
  else if(x<-knee) x=-(knee+span*(1.0-exp(-(-x-knee)/span)));
  if(x>32767) x=32767;
  if(x<-32768) x=-32768;
  return (int16_t)lrint(x);
}

void gml_fmod_mix(GmlFmodBanks *b, int16_t *out, int frames, int out_rate){
  if(!b||out_rate<=0||frames<=0) return;
  int nvals=frames*2;
  int32_t stack[4096];
  int32_t *mix = nvals<=4096 ? stack : calloc((size_t)nvals,sizeof(int32_t));
  if(!mix) return;
  memset(mix,0,(size_t)nvals*sizeof(int32_t));
  int any=0;
  for(int i=0;i<FMOD_MAXVOICES;i++){
    FVoice *v=&b->voices[i];
    if(!v->handle || !v->active || v->paused || v->frames<=0) continue;
    if(!v->pcm && !v->vs) continue;
    any=1;
    double step=(double)v->rate/(double)out_rate;
    int ch=v->ch; double g=v->gain;
    /* Azimuth pan from the emitter's horizontal offset is energy preserving. Distance attenuation
     * is not applied because the coordinate-to-FMOD-unit scale is not available here. */
    double gl=1.0, gr=1.0;
    if(v->has_3d && b->have_listener){
      double dx=v->px-b->lx, dy=v->py-b->ly, d=sqrt(dx*dx+dy*dy);
      double pan=(d>1e-6)? dx/d : 0.0;
      if(pan>0) gl=1.0-pan; else if(pan<0) gr=1.0+pan;
    }
    double glg=g*gl, grg=g*gr;
    if(v->vs){
      /* streamed (music): decode just enough to cover this block, then resample from the window */
      long first=(long)v->pos, last=(long)(v->pos+step*frames)+2;
      fmod_stream_fill(b,v,first,last);
      for(int f=0;f<frames;f++){
        long i0=(long)v->pos, i1=i0+1;
        long w0=i0-v->win_start, w1=i1-v->win_start;
        if(w1>=v->win_len || w0<0){ v->active=0; fmod_voice_free_audio(v); if(v->one_shot){ memset(v,0,sizeof *v); b->vh[(int)(v-b->voices)]=0; } break; }
        double frac=v->pos-(double)i0;
        double l,r;
        if(ch>=2){ l=(1-frac)*v->win[w0*2]+frac*v->win[w1*2]; r=(1-frac)*v->win[w0*2+1]+frac*v->win[w1*2+1]; }
        else     { l=r=(1-frac)*v->win[w0]+frac*v->win[w1]; }
        mix[f*2]+=(int32_t)lrint(l*glg); mix[f*2+1]+=(int32_t)lrint(r*grg);
        v->pos+=step;
      }
    } else {
      double lo=(v->loop_end>0)?(double)v->loop_start:0.0;
      double hi=(v->loop_end>0)?(double)v->loop_end:(double)(v->frames-1);
      for(int f=0;f<frames;f++){
        if(v->pos>=hi){
          if(v->loop && hi>lo){ v->pos=lo+fmod(v->pos-lo,hi-lo); }
          else { v->active=0; fmod_voice_free_audio(v); if(v->one_shot){ memset(v,0,sizeof *v); b->vh[(int)(v-b->voices)]=0; } break; }
        }
        int i0=(int)v->pos; int i1=i0+1; if(i1>=v->frames) i1=v->frames-1;
        double frac=v->pos-(double)i0;
        double l,r;
        if(ch>=2){ l=(1-frac)*v->pcm[i0*2]  +frac*v->pcm[i1*2];
                   r=(1-frac)*v->pcm[i0*2+1]+frac*v->pcm[i1*2+1]; }
        else     { l=r=(1-frac)*v->pcm[i0]+frac*v->pcm[i1]; }
        mix[f*2]  +=(int32_t)lrint(l*glg);
        mix[f*2+1]+=(int32_t)lrint(r*grg);
        v->pos+=step;
      }
    }
  }
  if(any) for(int i=0;i<nvals;i++) out[i]=fmod_softclip((int32_t)out[i]+mix[i]);
  if(mix!=stack) free(mix);
}
