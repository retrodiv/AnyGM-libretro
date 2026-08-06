/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic.h"
#include "anygm_vfs.h"
#include "gml_image_codec.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static char *classic_inflate_owned(const uint8_t *encoded,size_t encoded_size,
                                   GmlDeflateFraming framing,int *decoded_size){
  GmlMediaBuffer decoded={0};
  if(decoded_size) *decoded_size=0;
  if(!decoded_size ||
     !gml_deflate_decode(encoded,encoded_size,framing,&decoded) ||
     decoded.size>(size_t)INT_MAX){
    gml_media_buffer_release(&decoded);
    return NULL;
  }
  *decoded_size=(int)decoded.size;
  return (char*)decoded.data;
}

static uint32_t read_u32le(const uint8_t *p){
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static void write_u32le(uint8_t *p,uint32_t value){
  p[0]=(uint8_t)value; p[1]=(uint8_t)(value>>8);
  p[2]=(uint8_t)(value>>16); p[3]=(uint8_t)(value>>24);
}

static int known_version(uint32_t version){
  return version == GMLC_CLASSIC_GM6 || version == GMLC_CLASSIC_GM7 ||
         version == GMLC_CLASSIC_GM7_ALT || version == GMLC_CLASSIC_GM8 ||
         version == GMLC_CLASSIC_GM81;
}

typedef struct {
  const uint8_t *data;
  size_t size;
  size_t pos;
  char *err;
  size_t errcap;
} ClassicReader;

static int reader_fail(ClassicReader *r, const char *what){
  if(r->err && r->errcap)
    snprintf(r->err, r->errcap, "classic project: truncated %s at offset %" PRIu64, what, (uint64_t)r->pos);
  return 0;
}

static int reader_u32(ClassicReader *r, uint32_t *out, const char *what){
  if(r->pos > r->size || r->size - r->pos < 4) return reader_fail(r, what);
  *out = read_u32le(r->data + r->pos);
  r->pos += 4;
  return 1;
}

static int reader_skip(ClassicReader *r, size_t count, const char *what){
  if(r->pos > r->size || count > r->size - r->pos) return reader_fail(r, what);
  r->pos += count;
  return 1;
}

static int reader_string(ClassicReader *r, const char *what){
  uint32_t length;
  return reader_u32(r, &length, what) && reader_skip(r, length, what);
}

static int reader_string_copy(ClassicReader *r, char **out, const char *what){
  uint32_t length;
  *out = NULL;
  if(!reader_u32(r, &length, what)) return 0;
  if(r->pos > r->size || length > r->size - r->pos) return reader_fail(r, what);
  char *s = (char*)malloc((size_t)length + 1);
  if(!s){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic project: out of memory reading %s", what);
    return 0;
  }
  memcpy(s, r->data + r->pos, length);
  s[length] = '\0';
  r->pos += length;
  *out = s;
  return 1;
}

static int reader_blocks(ClassicReader *r, uint32_t count, const char *what){
  if(count > (r->size - r->pos) / 4){
    if(r->err && r->errcap)
      snprintf(r->err, r->errcap, "classic project: impossible %s count %u at offset %" PRIu64, what, count, (uint64_t)r->pos);
    return 0;
  }
  for(uint32_t i = 0; i < count; ++i){
    uint32_t length;
    if(!reader_u32(r, &length, what) || !reader_skip(r, length, what)) return 0;
  }
  return 1;
}

static int reader_blob(ClassicReader *r, const char *what){
  uint32_t length;
  return reader_u32(r, &length, what) && reader_skip(r, length, what);
}

static int reader_blob_copy(ClassicReader *r, GmlcClassicBlob *out, const char *what){
  uint32_t length;
  memset(out,0,sizeof(*out));
  if(!reader_u32(r,&length,what)) return 0;
  if(r->pos>r->size || length>r->size-r->pos) return reader_fail(r,what);
  if(length){
    out->data=(uint8_t*)malloc(length);
    if(!out->data){
      if(r->err && r->errcap) snprintf(r->err,r->errcap,"classic project: out of memory reading %s",what);
      return 0;
    }
    memcpy(out->data,r->data+r->pos,length);
    out->size=length;
  }
  r->pos+=length;
  return 1;
}

static int game_information_record_valid(const uint8_t *data,size_t size){
  if(!data || size<12) return 0;
  uint32_t caption=read_u32le(data+8);
  size_t fixed=12u+(size_t)caption+8u*4u+8u;
  if(fixed>size || size-fixed<4u) return 0;
  uint32_t text=read_u32le(data+fixed);
  return (size_t)text<=size-fixed-4u;
}

/* The compact help-record layout omits the eight-byte timestamp. Return the insertion point
 * when its remaining fields are valid so the importer keeps one normalized representation. */
static int game_information_compact_valid(const uint8_t *data,size_t size,size_t *insert_at){
  if(!data || size<12) return 0;
  uint32_t caption=read_u32le(data+8);
  size_t fixed=12u+(size_t)caption+8u*4u;
  if(fixed>size || size-fixed<4u) return 0;
  uint32_t text=read_u32le(data+fixed);
  if((size_t)text>size-fixed-4u) return 0;
  if(insert_at) *insert_at=fixed;
  return 1;
}

int gmlc_classic_game_information_decode(const GmlcClassicBlob *source,
                                         GmlcClassicBlob *decoded,
                                         char *err,size_t errcap){
  enum { GAME_INFORMATION_LIMIT=8*1024*1024 };
  if(err && errcap) err[0]='\0';
  if(!decoded){
    if(err && errcap) snprintf(err,errcap,"classic project: missing game-information output");
    return 0;
  }
  memset(decoded,0,sizeof(*decoded));
  if(!source || !source->size) return 1;
  if(!source->data || source->size>GAME_INFORMATION_LIMIT || source->size>INT_MAX){
    if(err && errcap) snprintf(err,errcap,"classic project: game information is too large");
    return 0;
  }
  const uint8_t *record=source->data;
  size_t record_size=source->size;
  uint8_t *normalized=NULL;
  int inflated_size=0;
  char *inflated=classic_inflate_owned(source->data,source->size,
                                       GML_DEFLATE_ZLIB,&inflated_size);
  if(inflated){
    if(inflated_size<0 || inflated_size>GAME_INFORMATION_LIMIT){
      free(inflated);
      if(err && errcap) snprintf(err,errcap,"classic project: game information expands beyond its limit");
      return 0;
    }
    record=(const uint8_t*)inflated;
    record_size=(size_t)inflated_size;
  }
  if(!game_information_record_valid(record,record_size)){
    size_t insert_at=0;
    if(game_information_compact_valid(record,record_size,&insert_at) &&
       record_size<=SIZE_MAX-8u){
      normalized=(uint8_t*)malloc(record_size+8u);
      if(!normalized){
        free(inflated);
        if(err && errcap) snprintf(err,errcap,
                                   "classic project: out of memory normalizing game information");
        return 0;
      }
      memcpy(normalized,record,insert_at);
      memset(normalized+insert_at,0,8u);
      memcpy(normalized+insert_at+8u,record+insert_at,record_size-insert_at);
      record=normalized;
      record_size+=8u;
    }
  }
  if(!game_information_record_valid(record,record_size)){
    if(err && errcap){
      char preview[3*12+1]={0}; size_t shown=record_size<12?record_size:12;
      uint32_t caption=record_size>=12?read_u32le(record+8):0;
      size_t tail=12u+(size_t)caption+8u*4u;
      uint32_t word0=tail+4<=record_size?read_u32le(record+tail):0;
      uint32_t word4=tail+8<=record_size?read_u32le(record+tail+4):0;
      uint32_t word8=tail+12<=record_size?read_u32le(record+tail+8):0;
      for(size_t i=0;i<shown;i++) snprintf(preview+i*3,sizeof(preview)-i*3,"%02x%s",
                                        record[i],i+1<shown?" ":"");
      snprintf(err,errcap,
               "classic project: invalid game-information record (%" PRIu64 " stored, %" PRIu64 " decoded bytes; "
               "caption=%u tail words=%u/%u/%u; %s%s)",(uint64_t)source->size,(uint64_t)record_size,caption,
               word0,word4,word8,preview,record_size>shown?" ...":"");
    }
    free(normalized); free(inflated);
    return 0;
  }
  decoded->data=(uint8_t*)malloc(record_size?record_size:1u);
  if(!decoded->data){
    free(normalized); free(inflated);
    if(err && errcap) snprintf(err,errcap,"classic project: out of memory decoding game information");
    return 0;
  }
  memcpy(decoded->data,record,record_size);
  decoded->size=record_size;
  free(normalized); free(inflated);
  return 1;
}

static int reader_words(ClassicReader *r, uint32_t count, const char *what){
  if(count > (r->size - r->pos) / 4) return reader_fail(r, what);
  return reader_skip(r, (size_t)count * 4, what);
}

static int read_settings_prefix(ClassicReader *r, GmlcClassicSettings *out){
  uint32_t field[14];
  for(size_t i=0;i<sizeof(field)/sizeof(field[0]);i++)
    if(!reader_u32(r,&field[i],"game settings")) return 0;
  out->start_fullscreen=(int)field[0];
  out->interpolate=(int)field[1];
  out->borderless=(int)field[2];
  out->show_cursor=(int)field[3];
  out->scaling=(int32_t)field[4];
  out->resizable=(int)field[5];
  out->always_on_top=(int)field[6];
  out->outside_color=field[7];
  out->set_resolution=(int)field[8];
  out->color_depth=(int)field[9];
  out->resolution=(int)field[10];
  out->frequency=(int)field[11];
  out->hide_caption_buttons=(int)field[12];
  out->synchronize=(int)field[13];
  return 1;
}

/* The optional tail grew after the original GM8 settings prefix. Read only the
 * creation-order flag needed by the runtime and leave truncated/older tails at
 * their historical default. Length-prefixed images are skipped without
 * inflating them. */
static int settings_tail_u32(ClassicReader *r, uint32_t *out){
  if(!r || r->pos>r->size || r->size-r->pos<4) return 0;
  const uint8_t *p=r->data+r->pos;
  *out=(uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
  r->pos+=4;
  return 1;
}
static int settings_tail_skip_bytes(ClassicReader *r, uint32_t size){
  if(!r || r->pos>r->size || size>r->size-r->pos) return 0;
  r->pos+=size;
  return 1;
}
static int settings_tail_skip_data(ClassicReader *r){
  uint32_t present=0,size=0;
  if(!settings_tail_u32(r,&present)) return 0;
  if(!present) return 1;
  return settings_tail_u32(r,&size) && settings_tail_skip_bytes(r,size);
}
static void read_settings_tail(ClassicReader *r, GmlcClassicSettings *out){
  ClassicReader q=*r;
  uint32_t field=0,loading_bar=0;
  /* screensaver, F4, F1, Escape, F5/F6, F9, close-as-Escape,
   * priority and freeze-on-focus-loss */
  for(int i=0;i<9;i++) if(!settings_tail_u32(&q,&field)) return;
  if(!settings_tail_u32(&q,&loading_bar)) return;
  if(loading_bar==2 && (!settings_tail_skip_data(&q) || !settings_tail_skip_data(&q))) return;
  /* Custom loading image has an outer enable followed by the ordinary data flag. */
  if(!settings_tail_u32(&q,&field)) return;
  if(field && !settings_tail_skip_data(&q)) return;
  /* transparency, translucency, scale-progress */
  for(int i=0;i<3;i++) if(!settings_tail_u32(&q,&field)) return;
  /* raw icon */
  if(!settings_tail_u32(&q,&field) || !settings_tail_skip_bytes(&q,field)) return;
  /* show/log/abort errors and uninitialized-variable policy */
  for(int i=0;i<4;i++) if(!settings_tail_u32(&q,&field)) return;
  /* Later writers append WebGL and then the Create-vs-instance-code order. */
  if(!settings_tail_u32(&q,&field)) return;
  if(settings_tail_u32(&q,&field)) out->swap_creation_events=field!=0;
}

static int read_compressed_settings(const uint8_t *compressed, uint32_t compressed_size,
                                    GmlcClassicSettings *out, char *err, size_t errcap){
  if(!compressed_size) return 1;
  if(compressed_size>INT_MAX){
    if(err&&errcap) snprintf(err,errcap,"classic project: compressed settings are too large");
    return 0;
  }
  int raw_size=0;
  char *raw=classic_inflate_owned(compressed,compressed_size,
                                  GML_DEFLATE_ZLIB,&raw_size);
  if(!raw || raw_size<0){
    if(err&&errcap) snprintf(err,errcap,"classic project: invalid compressed settings");
    free(raw);
    return 0;
  }
  ClassicReader settings={(const uint8_t*)raw,(size_t)raw_size,0,err,errcap};
  int ok=read_settings_prefix(&settings,out);
  if(ok) read_settings_tail(&settings,out);
  free(raw);
  return ok;
}

static int reader_trigger(ClassicReader *outer, GmlcClassicTrigger *out, const char *what){
  uint32_t compressed_size;
  memset(out,0,sizeof(*out));
  if(!reader_u32(outer,&compressed_size,what)) return 0;
  if(outer->pos>outer->size || compressed_size>outer->size-outer->pos || compressed_size>INT_MAX)
    return reader_fail(outer,what);
  int raw_size=0;
  char *raw=classic_inflate_owned(outer->data+outer->pos,compressed_size,
                                  GML_DEFLATE_ZLIB,&raw_size);
  if(!raw && compressed_size>=6)
    raw=classic_inflate_owned(outer->data+outer->pos+2,compressed_size-6,
                              GML_DEFLATE_RAW,&raw_size);
  if(!raw || raw_size<4){
    free(raw);
    if(outer->err && outer->errcap) snprintf(outer->err,outer->errcap,"classic project: invalid %s",what);
    return 0;
  }
  ClassicReader r={(const uint8_t*)raw,(size_t)raw_size,0,outer->err,outer->errcap};
  uint32_t exists=0,version=0;
  int ok=reader_u32(&r,&exists,"trigger existence flag");
  out->exists=exists!=0;
  if(ok && out->exists){
    ok=reader_u32(&r,&version,"trigger version") && version>=800 &&
       reader_string_copy(&r,&out->name,"trigger name") &&
       reader_string_copy(&r,&out->condition,"trigger condition") &&
       reader_u32(&r,&out->moment,"trigger moment") &&
       reader_string_copy(&r,&out->constant_name,"trigger constant name");
  }
  if(ok && r.pos!=r.size && !(r.pos+1==r.size && r.data[r.pos]==0)){
    if(outer->err && outer->errcap)
      snprintf(outer->err,outer->errcap,"classic project: trigger has %" PRIu64 " trailing bytes",(uint64_t)(r.size-r.pos));
    ok=0;
  }
  if(!ok){
    free(out->name); free(out->condition); free(out->constant_name);
    memset(out,0,sizeof(*out));
  }
  free(raw);
  if(ok) outer->pos+=compressed_size;
  return ok;
}

static int reader_included_file(ClassicReader *outer, GmlcClassicIncludedFile *out,
                                int has_timestamp, const char *what){
  uint32_t compressed_size;
  memset(out,0,sizeof(*out));
  if(!reader_u32(outer,&compressed_size,what)) return 0;
  if(outer->pos>outer->size || compressed_size>outer->size-outer->pos || compressed_size>INT_MAX)
    return reader_fail(outer,what);
  int raw_size=0;
  char *raw=classic_inflate_owned(outer->data+outer->pos,compressed_size,
                                  GML_DEFLATE_ZLIB,&raw_size);
  if(!raw && compressed_size>=6)
    raw=classic_inflate_owned(outer->data+outer->pos+2,compressed_size-6,
                              GML_DEFLATE_RAW,&raw_size);
  if(!raw || raw_size<4){
    free(raw);
    if(outer->err && outer->errcap) snprintf(outer->err,outer->errcap,"classic project: invalid %s",what);
    return 0;
  }
  ClassicReader r={(const uint8_t*)raw,(size_t)raw_size,0,outer->err,outer->errcap};
  uint32_t version=0,data_exists=0,stored=0,overwrite=0,free_memory=0,remove_at_end=0;
  GmlcClassicBlob embedded={0};
  int ok=(!has_timestamp || reader_skip(&r,8,"included-file timestamp")) &&
         reader_u32(&r,&version,"included-file version") && version>=800 &&
         reader_string_copy(&r,&out->file_name,"included-file name") &&
         reader_string_copy(&r,&out->source_path,"included-file source path") &&
         reader_u32(&r,&data_exists,"included-file data flag") &&
         reader_u32(&r,&out->source_length,"included-file source length") &&
         reader_u32(&r,&stored,"included-file storage flag");
  if(ok && data_exists && stored) ok=reader_blob_copy(&r,&embedded,"included-file data");
  if(ok) ok=reader_u32(&r,&out->export_mode,"included-file export mode") &&
            reader_string_copy(&r,&out->custom_folder,"included-file custom folder") &&
            reader_u32(&r,&overwrite,"included-file overwrite flag") &&
            reader_u32(&r,&free_memory,"included-file free-memory flag") &&
            reader_u32(&r,&remove_at_end,"included-file remove flag");
  if(ok && r.pos!=r.size && !(r.pos+1==r.size && r.data[r.pos]==0)){
    if(outer->err && outer->errcap)
      snprintf(outer->err,outer->errcap,"classic project: included file has %" PRIu64 " trailing bytes",(uint64_t)(r.size-r.pos));
    ok=0;
  }
  out->data_exists=data_exists!=0; out->stored_in_project=stored!=0;
  out->overwrite_file=overwrite!=0; out->free_memory=free_memory!=0;
  out->remove_at_end=remove_at_end!=0; out->data=embedded.data; out->data_size=embedded.size;
  if(!ok){
    free(out->file_name); free(out->source_path); free(out->custom_folder); free(out->data);
    memset(out,0,sizeof(*out));
  }
  free(raw);
  if(ok) outer->pos+=compressed_size;
  return ok;
}

static int reader_doubles(ClassicReader *r, uint32_t count, const char *what){
  if(count > (r->size - r->pos) / 8) return reader_fail(r, what);
  return reader_skip(r, (size_t)count * 8, what);
}

static int require_payload_end(ClassicReader *r, const char *what){
  if(r->pos == r->size) return 1;
  if(r->err && r->errcap){
    char preview[3*8+1]={0};
    size_t remain=r->size-r->pos, shown=remain<8?remain:8;
    for(size_t i=0;i<shown;i++) snprintf(preview+i*3,sizeof(preview)-i*3,"%02x%s",
                                      r->data[r->pos+i],i+1<shown?" ":"");
    snprintf(r->err, r->errcap,
             "classic project: %s has %" PRIu64 " unexplained trailing bytes at offset %" PRIu64 " (%s%s)",
             what,(uint64_t)remain,(uint64_t)r->pos,preview,remain>shown?" ...":"");
  }
  return 0;
}

static int validate_sound_payload(ClassicReader *r){
  uint32_t has_data;
  if(!reader_words(r, 1, "sound kind") || !reader_string(r, "sound file type") ||
     !reader_string(r, "sound filename") || !reader_u32(r, &has_data, "sound data flag")) return 0;
  if(has_data && !reader_blob(r, "sound data")) return 0;
  return reader_words(r, 1, "sound effects") && reader_doubles(r, 2, "sound volume and pan") &&
         reader_words(r, 1, "sound preload");
}

static int validate_sprite_payload(ClassicReader *r,int executable_layout,uint32_t resource_version){
  uint32_t frames;
  if(!reader_words(r, 2, "sprite origin") || !reader_u32(r, &frames, "sprite frame count")) return 0;
  if(frames > (r->size - r->pos) / 12) return reader_fail(r, "sprite frames");
  for(uint32_t i = 0; i < frames; ++i){
    uint32_t frame_version, width, height;
    if(!reader_u32(r, &frame_version, "sprite frame version") ||
       !reader_u32(r, &width, "sprite frame width") ||
       !reader_u32(r, &height, "sprite frame height")) return 0;
    if(frame_version < 800){
      if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic project: unsupported sprite frame version %u", frame_version);
      return 0;
    }
    if(width && height && !reader_blob(r, "sprite BGRA pixels")) return 0;
  }
  if(!executable_layout) return reader_words(r, 8, "sprite collision fields");
  /* Executable-layout empty sprites retain the separate-mask flag. Consume it even though
   * the following mask records are empty, preserving the next resource boundary. */
  if(!frames) return reader_words(r,1,"empty-sprite collision flag");
  if(resource_version>=810 && !reader_words(r,1,"sprite collision shape")) return 0;
  uint32_t separate;
  if(!reader_u32(r,&separate,"sprite separate collision maps")) return 0;
  uint32_t maps=separate?frames:1;
  for(uint32_t map=0;map<maps;map++){
    uint32_t fields[7];
    for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);i++)
      if(!reader_u32(r,&fields[i],"sprite collision map")) return 0;
    uint64_t pixels=(uint64_t)fields[1]*(uint64_t)fields[2];
    if(pixels>UINT32_MAX || !reader_words(r,(uint32_t)pixels,"sprite collision pixels")) return 0;
  }
  return 1;
}

static int validate_background_payload(ClassicReader *r,int executable_layout){
  uint32_t image_version, width, height;
  if((!executable_layout && !reader_words(r, 7, "background tile fields")) ||
     !reader_u32(r, &image_version, "background image version") ||
     !reader_u32(r, &width, "background width") ||
     !reader_u32(r, &height, "background height")) return 0;
  if(image_version < 710){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic project: unsupported background image version %u", image_version);
    return 0;
  }
  if(width && height && !reader_blob(r, "background BGRA pixels")) return 0;
  return 1;
}

/* Project records include three editor-only fields: target room and both snap steps. Compiled
 * executable records omit them. The kind, closed flag, precision, and point list are common to
 * both. This is a layout difference rather than a revision difference. */
static int validate_path_payload(ClassicReader *r, int executable_layout){
  uint32_t points;
  if(!reader_words(r, executable_layout ? 3 : 6, "path fields") ||
     !reader_u32(r, &points, "path point count")) return 0;
  return reader_doubles(r, points > UINT32_MAX / 3 ? UINT32_MAX : points * 3, "path points");
}

static int validate_legacy_executable_path_payload(ClassicReader *r){
  uint32_t points;
  if(!reader_words(r,3,"compiled legacy path fields") ||
     !reader_u32(r,&points,"compiled legacy path point count")) return 0;
  return reader_doubles(r,points>UINT32_MAX/3?UINT32_MAX:points*3,
                        "compiled legacy path points");
}

static int validate_font_payload(ClassicReader *r,int executable_layout,int compressed_atlas){
  if(!reader_string(r,"font face") || !reader_words(r,5,"font fields")) return 0;
  /* Project files stop at the typeface metadata. Compiled executable
   * containers append a fixed 256-entry glyph map and compiler-produced
   * alpha atlas. Retain that authoritative raster instead of asking the host
   * to recreate it with a potentially different installed font. */
  if(!executable_layout || (r->pos+1==r->size && r->data[r->pos]==0)) return 1;
  if(!reader_words(r,256u*6u,"compiled font glyph map")) return 0;
  uint32_t width=0,height=0,bytes=0;
  if(!reader_u32(r,&width,"compiled font atlas width") ||
     !reader_u32(r,&height,"compiled font atlas height") ||
     !reader_u32(r,&bytes,"compiled font atlas size")) return 0;
  uint64_t pixels=(uint64_t)width*(uint64_t)height;
  if(width>4096 || height>4096 || !pixels || !bytes ||
     (!compressed_atlas && pixels!=bytes)){
    if(r->err && r->errcap)
      snprintf(r->err,r->errcap,
               "classic project: invalid compiled font atlas %ux%u with %u stored bytes",
               width,height,bytes);
    return 0;
  }
  return reader_skip(r,bytes,"compiled font atlas pixels");
}

static int validate_actions(ClassicReader *r){
  uint32_t version, count;
  if(!reader_u32(r, &version, "action-list version") ||
     !reader_u32(r, &count, "action count")) return 0;
  if(version < 400){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic project: unsupported action-list version %u", version);
    return 0;
  }
  if(count > (r->size - r->pos) / 4) return reader_fail(r, "actions");
  for(uint32_t i = 0; i < count; ++i){
    uint32_t action_version, kinds, arguments;
    if(!reader_u32(r, &action_version, "action version") ||
       !reader_words(r, 7, "action identity and flags") ||
       !reader_string(r, "action function") || !reader_string(r, "action code") ||
       !reader_words(r, 1, "action used-argument count") ||
       !reader_u32(r, &kinds, "action argument-kind count") ||
       !reader_words(r, kinds, "action argument kinds") ||
       !reader_words(r, 2, "action target and relative flag") ||
       !reader_u32(r, &arguments, "action argument count")) return 0;
    if(action_version < 440){
      if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic project: unsupported action version %u", action_version);
      return 0;
    }
    if(arguments > (r->size - r->pos) / 4) return reader_fail(r, "action arguments");
    for(uint32_t arg = 0; arg < arguments; ++arg)
      if(!reader_string(r, "action argument")) return 0;
    if(!reader_words(r, 1, "action negation flag")) return 0;
  }
  return 1;
}

static int validate_timeline_payload(ClassicReader *r){
  uint32_t moments;
  if(!reader_u32(r, &moments, "timeline moment count")) return 0;
  if(moments > (r->size - r->pos) / 4) return reader_fail(r, "timeline moments");
  for(uint32_t i = 0; i < moments; ++i)
    if(!reader_words(r, 1, "timeline moment") || !validate_actions(r)) return 0;
  return 1;
}

static int validate_object_payload(ClassicReader *r){
  uint32_t last_event_type;
  if(!reader_words(r, 7, "object fields") ||
     !reader_u32(r, &last_event_type, "object event-type count")) return 0;
  if(last_event_type > 64){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic project: unreasonable object event-type value %u", last_event_type);
    return 0;
  }
  for(uint32_t type = 0; type <= last_event_type; ++type){
    for(;;){
      uint32_t event_number;
      if(!reader_u32(r, &event_number, "object event number")) return 0;
      if(event_number == UINT32_MAX) break;
      if(!validate_actions(r)) return 0;
    }
  }
  return 1;
}

static int validate_room_gameplay_payload(ClassicReader *r){
  uint32_t backgrounds, views, instances, tiles;
  if(!reader_string(r, "room caption") || !reader_words(r, 9, "room fields") ||
     !reader_string(r, "room creation code") ||
     !reader_u32(r, &backgrounds, "room background count") ||
     !reader_words(r, backgrounds > UINT32_MAX / 10 ? UINT32_MAX : backgrounds * 10,
                   "room backgrounds") ||
     !reader_words(r, 1, "room view-enabled flag") ||
     !reader_u32(r, &views, "room view count") ||
     !reader_words(r, views > UINT32_MAX / 14 ? UINT32_MAX : views * 14, "room views") ||
     !reader_u32(r, &instances, "room instance count")) return 0;
  if(instances > (r->size - r->pos) / 24) return reader_fail(r, "room instances");
  for(uint32_t i = 0; i < instances; ++i)
    if(!reader_words(r, 4, "room instance fields") ||
       !reader_string(r, "room instance creation code") ||
       !reader_words(r, 1, "room instance locked flag")) return 0;
  if(!reader_u32(r, &tiles, "room tile count")) return 0;
  return reader_words(r, tiles > UINT32_MAX / 10 ? UINT32_MAX : tiles * 10, "room tiles");
}

static int validate_room_payload(ClassicReader *r){
  return validate_room_gameplay_payload(r) && reader_words(r, 14, "room editor fields");
}

/* Some GM8 executables compact the tail of an object block: trailing zero bytes are elided and
 * event lists after the primary Draw event are replaced by compact executable metadata. Rebuild the
 * self-contained object payload expected by the project normalizer when that compact form is
 * detected. */
static int repair_executable_object(char **raw_io, int *raw_size_io){
  int original=*raw_size_io;
  char *raw=(char*)realloc(*raw_io,(size_t)original+64);
  if(!raw) return 0;
  memset(raw+original,0,64);
  *raw_io=raw;
  ClassicReader r={(const uint8_t*)raw,(size_t)original+64,0,NULL,0};
  uint32_t exists,version,event;
  if(!reader_u32(&r,&exists,"object exists") || !exists ||
     !reader_string(&r,"object name") || !reader_u32(&r,&version,"object version") ||
     !reader_words(&r,7,"object fields")) return 0;
  (void)version;
  size_t last_type_off=r.pos;
  if(!reader_u32(&r,&event,"object event count")) return 0;
  raw[last_type_off]=8; raw[last_type_off+1]=raw[last_type_off+2]=raw[last_type_off+3]=0;
  for(int type=0;type<=8;type++){
    for(;;){
      if(r.pos+4>(size_t)original){
        if(r.pos+4>r.size) return 0;
        memset(raw+r.pos,0xFF,4); r.pos+=4; break;
      }
      if(!reader_u32(&r,&event,"object event")) return 0;
      if(event==UINT32_MAX) break;
      if(!validate_actions(&r)) return 0;
    }
  }
  *raw_size_io=(int)r.pos;
  return 1;
}

/* Executable room blocks omit project-editor state. Insert the three grid fields used by the
 * project representation and append a neutral editor tail so the regular room importer can be
 * shared by projects and executables. */
static int normalize_executable_room(char **raw_io, int *raw_size_io){
  int original=*raw_size_io;
  if(original<0 || (size_t)original>SIZE_MAX-76u) return 0;
  ClassicReader r={(const uint8_t*)*raw_io,(size_t)original,0,NULL,0};
  uint32_t exists,version;
  if(!reader_u32(&r,&exists,"room exists")) return 0;
  if(!exists) return r.pos==(size_t)original;
  if(!reader_string(&r,"room name") || !reader_u32(&r,&version,"room version") ||
     !reader_string(&r,"room caption") || !reader_words(&r,2,"room dimensions")) return 0;
  (void)version;
  size_t insert_at=r.pos;
  size_t working_size=(size_t)original+12+64;
  char *normalized=(char*)malloc(working_size);
  if(!normalized) return 0;
  memcpy(normalized,*raw_io,insert_at);
  memset(normalized+insert_at,0,12);
  normalized[insert_at]=16;
  normalized[insert_at+4]=16;
  memcpy(normalized+insert_at+12,*raw_io+insert_at,(size_t)original-insert_at);
  memset(normalized+original+12,0,64);
  size_t data_size=(size_t)original+12;
  ClassicReader compact={(const uint8_t*)normalized,data_size,0,NULL,0};
  uint32_t count=0,instances=0;
  if(!reader_u32(&compact,&exists,"room exists") || !reader_string(&compact,"room name") ||
     !reader_u32(&compact,&version,"room version") || !reader_string(&compact,"room caption") ||
     !reader_words(&compact,9,"room fields") || !reader_string(&compact,"room creation code") ||
     !reader_u32(&compact,&count,"room backgrounds") ||
     !reader_words(&compact,count>UINT32_MAX/10?UINT32_MAX:count*10,"room backgrounds") ||
     !reader_words(&compact,1,"room views enabled") || !reader_u32(&compact,&count,"room views") ||
     !reader_words(&compact,count>UINT32_MAX/14?UINT32_MAX:count*14,"room views") ||
     !reader_u32(&compact,&instances,"room instances") || instances>UINT32_MAX/5){
    free(normalized); return 0;
  }
  size_t records_start=compact.pos;
  for(uint32_t instance=0;instance<instances;instance++)
    if(!reader_words(&compact,4,"compact room instance fields") ||
       !reader_string(&compact,"compact room instance creation code")){
      free(normalized); return 0;
    }
  size_t tail_start=compact.pos,growth=(size_t)instances*4u;
  if(growth>SIZE_MAX-data_size-64u){ free(normalized); return 0; }
  working_size=data_size+growth+64u;
  char *expanded=(char*)malloc(working_size);
  if(!expanded){ free(normalized); return 0; }
  memcpy(expanded,normalized,records_start);
  size_t source_at=records_start,target_at=records_start;
  for(uint32_t instance=0;instance<instances;instance++){
    uint32_t source_size=read_u32le((const uint8_t*)normalized+source_at+16u);
    size_t record_size=20u+(size_t)source_size;
    memcpy(expanded+target_at,normalized+source_at,record_size);
    target_at+=record_size;
    write_u32le((uint8_t*)expanded+target_at,0); /* compiled rooms omit the editor lock flag */
    target_at+=4u;
    source_at+=record_size;
  }
  memcpy(expanded+target_at,normalized+tail_start,data_size-tail_start);
  free(normalized);
  normalized=expanded;
  data_size+=growth;
  memset(normalized+data_size,0,64);

  /* Compiled rooms omit the editor-only lock flag from every tile record.  Expand the compact
   * nine-word records before handing the room to the shared project-layout validator/importer. */
  ClassicReader tile_probe={(const uint8_t*)normalized,data_size,0,NULL,0};
  uint32_t tiles=0;
  if(!reader_u32(&tile_probe,&exists,"room exists") ||
     !reader_string(&tile_probe,"room name") || !reader_u32(&tile_probe,&version,"room version") ||
     !reader_string(&tile_probe,"room caption") || !reader_words(&tile_probe,9,"room fields") ||
     !reader_string(&tile_probe,"room creation code") ||
     !reader_u32(&tile_probe,&count,"room backgrounds") ||
     !reader_words(&tile_probe,count>UINT32_MAX/10?UINT32_MAX:count*10,"room backgrounds") ||
     !reader_words(&tile_probe,1,"room views enabled") ||
     !reader_u32(&tile_probe,&count,"room views") ||
     !reader_words(&tile_probe,count>UINT32_MAX/14?UINT32_MAX:count*14,"room views") ||
     !reader_u32(&tile_probe,&instances,"room instances")){
    free(normalized); return 0;
  }
  for(uint32_t instance=0;instance<instances;instance++)
    if(!reader_words(&tile_probe,4,"room instance fields") ||
       !reader_string(&tile_probe,"room instance creation code") ||
       !reader_words(&tile_probe,1,"room instance locked flag")){
      free(normalized); return 0;
    }
  if(!reader_u32(&tile_probe,&tiles,"room tiles") || tiles>UINT32_MAX/9){
    free(normalized); return 0;
  }
  size_t tile_records_start=tile_probe.pos;
  if(!reader_words(&tile_probe,tiles*9,"compact room tiles") || tile_probe.pos!=data_size){
    free(normalized); return 0;
  }
  size_t tile_growth=(size_t)tiles*4u;
  if(tile_growth>SIZE_MAX-data_size-64u){ free(normalized); return 0; }
  working_size=data_size+tile_growth+64u;
  expanded=(char*)realloc(normalized,working_size);
  if(!expanded){ free(normalized); return 0; }
  normalized=expanded;
  for(uint32_t tile=tiles;tile-- > 0;){
    uint8_t *source=(uint8_t*)normalized+tile_records_start+(size_t)tile*36u;
    uint8_t *target=(uint8_t*)normalized+tile_records_start+(size_t)tile*40u;
    memmove(target,source,36);
    write_u32le(target+36,0);
  }
  data_size+=tile_growth;
  memset(normalized+data_size,0,64);

  ClassicReader body={(const uint8_t*)normalized,working_size,0,NULL,0};
  if(!reader_u32(&body,&exists,"room exists") || !reader_string(&body,"room name") ||
     !reader_u32(&body,&version,"room version") || !validate_room_gameplay_payload(&body)){
    free(normalized);
    return 0;
  }
  if(body.pos>(size_t)INT_MAX-56u){ free(normalized); return 0; }
  size_t normalized_size=body.pos+56u;
  char *complete=(char*)realloc(normalized,normalized_size);
  if(!complete){ free(normalized); return 0; }
  normalized=complete;
  memset(normalized+body.pos,0,56);
  free(*raw_io);
  *raw_io=normalized;
  *raw_size_io=(int)normalized_size;
  return 1;
}

static int read_file(const AnygmHostServices *host,const char *path,
                     uint8_t **data,size_t *size,
                     char *err, size_t errcap){
  *data = NULL;
  *size = 0;
  uint8_t *bytes=NULL;
  size_t got=0;
  if(!anygm_vfs_read_all(host,path,&bytes,&got,512u*1024u*1024u)){
    if(err && errcap) snprintf(err, errcap, "classic project: cannot read %s", path);
    return 0;
  }
  *data = bytes;
  *size = got;
  return 1;
}

int gmlc_classic_probe(const void *data, size_t size, GmlcClassicHeader *out,
                       char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!data || !out){
    if(err && errcap) snprintf(err, errcap, "classic project: invalid probe arguments");
    return 0;
  }
  if(size < 8){
    if(err && errcap) snprintf(err, errcap, "classic project: truncated common header (%" PRIu64 " bytes)", (uint64_t)size);
    return 0;
  }
  const uint8_t *p = (const uint8_t*)data;
  uint32_t magic = read_u32le(p);
  uint32_t version = read_u32le(p + 4);
  if(magic != GMLC_CLASSIC_MAGIC){
    if(err && errcap) snprintf(err, errcap, "classic project: bad magic %u", magic);
    return 0;
  }
  if(!known_version(version)){
    if(err && errcap) snprintf(err, errcap, "classic project: unsupported container version %u", version);
    return 0;
  }
  memset(out, 0, sizeof(*out));
  out->version = (GmlcClassicVersion)version;
  
  if(version != GMLC_CLASSIC_GM7 && version != GMLC_CLASSIC_GM7_ALT){
    if(size < 28){
      if(err && errcap) snprintf(err, errcap, "classic project: truncated common header (%" PRIu64 " bytes)", (uint64_t)size);
      return 0;
    }
    out->game_id = read_u32le(p + 8);
    memcpy(out->guid, p + 12, sizeof(out->guid));
  }
  return 1;
}



int gmlc_classic_probe_file(const AnygmHostServices *host,const char *path,
                            GmlcClassicHeader *out,
                            char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!path || !out){
    if(err && errcap) snprintf(err, errcap, "classic project: invalid file probe arguments");
    return 0;
  }
  uint8_t header[28];
  size_t got=0;
  if(!anygm_vfs_read_prefix(host,path,header,sizeof header,&got)){
    if(err && errcap) snprintf(err, errcap, "classic project: cannot read %s", path);
    return 0;
  }
  return gmlc_classic_probe(header, got, out, err, errcap);
}

static int skip_legacy_image(ClassicReader *r, const char *what){
  uint32_t marker;
  if(!reader_u32(r, &marker, what)) return 0;
  return marker == UINT32_MAX || reader_blob(r, what);
}

static int skip_legacy_settings(ClassicReader *r, uint32_t container_version,
                                uint32_t *settings_version, GmlcClassicSettings *settings,
                                uint32_t *constant_count, GmlcClassicManifest *manifest){
  uint32_t loading_bar, own_loading_image, constants;
  if(!reader_u32(r, settings_version, "legacy settings version")) return 0;
  int gm7 = container_version == GMLC_CLASSIC_GM7 || container_version == GMLC_CLASSIC_GM7_ALT;
  uint32_t fixed_before_loading = gm7 ? 22u : 20u;
  if(!read_settings_prefix(r,settings) ||
     !reader_words(r, fixed_before_loading-14u, "legacy game settings") ||
     !reader_u32(r, &loading_bar, "loading-bar mode")) return 0;
  if(loading_bar == 2 &&
     (!skip_legacy_image(r, "loading-bar background") ||
      !skip_legacy_image(r, "loading-bar foreground"))) return 0;
  if(!reader_u32(r, &own_loading_image, "custom loading-image flag")) return 0;
  if(own_loading_image && !skip_legacy_image(r, "custom loading image")) return 0;
  if(!reader_words(r, 3, "loading-image settings") || !reader_blob(r, "game icon") ||
     !reader_words(r, 4, "legacy error settings") || !reader_string(r, "game author")) return 0;
  if(gm7){
    if(!reader_string(r, "game version")) return 0;
  } else if(!reader_words(r, 1, "numeric game version")) return 0;
  if(!reader_skip(r, 8, "settings timestamp") || !reader_string(r, "game information") ||
     !reader_u32(r, &constants, "settings constant count")) return 0;
  if(constants > (r->size - r->pos) / 8) return reader_fail(r, "settings constants");
  if(constant_count) *constant_count=constants;
  if(manifest && constants){
    manifest->constant_defs=(GmlcClassicConstant*)calloc(constants,sizeof(*manifest->constant_defs));
    if(!manifest->constant_defs) return reader_fail(r,"settings constants allocation");
    manifest->constant_def_count=constants;
  }
  for(uint32_t i = 0; i < constants; ++i){
    if(manifest){
      if(!reader_string_copy(r,&manifest->constant_defs[i].name,"constant name") ||
         !reader_string_copy(r,&manifest->constant_defs[i].value,"constant value")) return 0;
    } else if(!reader_string(r, "constant name") || !reader_string(r, "constant value")) return 0;
  }
  if(gm7){
    if(!reader_words(r, 4, "game version components") ||
       !reader_string(r, "company") || !reader_string(r, "product") ||
       !reader_string(r, "copyright") || !reader_string(r, "description")) return 0;
  } else {
    uint32_t includes;
    if(!reader_u32(r, &includes, "legacy include count")) return 0;
    if(includes > (r->size - r->pos) / 4) return reader_fail(r, "legacy includes");
    for(uint32_t i = 0; i < includes; ++i)
      if(!reader_string(r, "legacy include filename")) return 0;
    if(!reader_words(r, 3, "legacy include settings")) return 0;
  }
  return 1;
}

static int validate_legacy_sprite_payload(ClassicReader *r){
  uint32_t frames;
  if(!reader_words(r, 13, "legacy sprite fields") ||
     !reader_u32(r, &frames, "legacy sprite frame count")) return 0;
  if(frames > (r->size - r->pos) / 4) return reader_fail(r, "legacy sprite frames");
  for(uint32_t i = 0; i < frames; ++i)
    if(!skip_legacy_image(r, "legacy sprite image")) return 0;
  return 1;
}

static int validate_legacy_background_payload(ClassicReader *r){
  uint32_t has_image;
  if(!reader_words(r, 12, "legacy background fields") ||
     !reader_u32(r, &has_image, "legacy background image flag")) return 0;
  return !has_image || skip_legacy_image(r, "legacy background image");
}

static int validate_legacy_executable_image(ClassicReader *r, const char *what){
  uint32_t version=0,exists=0,width=0,height=0,bytes=0;
  if(!reader_u32(r,&version,what) || version<400 ||
     !reader_u32(r,&exists,what) || exists>1) return 0;
  if(!exists) return 1;
  if(!reader_u32(r,&width,what) || !reader_u32(r,&height,what) ||
     width>65536u || height>65536u ||
     (uint64_t)width*(uint64_t)height*4u>SIZE_MAX ||
     !reader_u32(r,&bytes,what)) return 0;
  return reader_skip(r,bytes,what);
}

static int validate_legacy_executable_sprite_payload(ClassicReader *r){
  uint32_t frames=0;
  if(!reader_words(r,13,"compiled legacy sprite fields") ||
     !reader_u32(r,&frames,"compiled legacy sprite frame count") ||
     frames>(r->size-r->pos)/8u) return 0;
  for(uint32_t frame=0;frame<frames;frame++)
    if(!validate_legacy_executable_image(r,"compiled legacy sprite image")) return 0;
  return 1;
}

static int validate_legacy_executable_background_payload(ClassicReader *r){
  uint32_t has_image=0;
  if(!reader_words(r,5,"compiled legacy background fields") ||
     !reader_u32(r,&has_image,"compiled legacy background image flag") || has_image>1) return 0;
  return !has_image || validate_legacy_executable_image(r,"compiled legacy background image");
}

static int validate_legacy_executable_room_payload(ClassicReader *r){
  uint32_t backgrounds=0,views=0,instances=0,tiles=0;
  if(!reader_string(r,"compiled legacy room caption") ||
     !reader_words(r,6,"compiled legacy room fields") ||
     !reader_string(r,"compiled legacy room creation code") ||
     !reader_u32(r,&backgrounds,"compiled legacy room background count") ||
     backgrounds>UINT32_MAX/10u ||
     !reader_words(r,backgrounds*10u,"compiled legacy room backgrounds") ||
     !reader_words(r,1,"compiled legacy room view-enabled flag") ||
     !reader_u32(r,&views,"compiled legacy room view count") ||
     views>UINT32_MAX/14u ||
     !reader_words(r,views*14u,"compiled legacy room views") ||
     !reader_u32(r,&instances,"compiled legacy room instance count") ||
     instances>(r->size-r->pos)/20u) return 0;
  for(uint32_t instance=0;instance<instances;instance++)
    if(!reader_words(r,4,"compiled legacy room instance fields") ||
       !reader_string(r,"compiled legacy room instance code")) return 0;
  if(!reader_u32(r,&tiles,"compiled legacy room tile count") ||
     tiles>UINT32_MAX/9u) return 0;
  return reader_words(r,tiles*9u,"compiled legacy room tiles");
}

static int parse_legacy_slot(ClassicReader *r, GmlcClassicResourceType type,
                             GmlcClassicResourceSlot *slot, int retain_payload){
  uint32_t exists;
  if(!reader_u32(r, &exists, "legacy resource existence flag")) return 0;
  slot->exists = exists != 0;
  if(!slot->exists) return 1;
  if(!reader_string_copy(r, &slot->name, "legacy resource name") ||
     !reader_u32(r, &slot->version, "legacy resource version")) return 0;
  size_t payload_start = r->pos;
  int valid = 0;
  switch(type){
    case GMLC_CLASSIC_SOUND: valid = validate_sound_payload(r); break;
    case GMLC_CLASSIC_SPRITE: valid = validate_legacy_sprite_payload(r); break;
    case GMLC_CLASSIC_BACKGROUND: valid = validate_legacy_background_payload(r); break;
    case GMLC_CLASSIC_PATH: valid = validate_path_payload(r, 0); break;
    case GMLC_CLASSIC_SCRIPT:
      valid = reader_string_copy(r, &slot->source, "legacy script source"); break;
    case GMLC_CLASSIC_FONT: valid = validate_font_payload(r,0,0); break;
    case GMLC_CLASSIC_TIMELINE: valid = validate_timeline_payload(r); break;
    case GMLC_CLASSIC_OBJECT: valid = validate_object_payload(r); break;
    case GMLC_CLASSIC_ROOM: valid = validate_room_payload(r); break;
    default: break;
  }
  if(!valid){
    free(slot->name); slot->name = NULL;
    free(slot->source); slot->source = NULL;
  } else if(retain_payload && r->pos > payload_start){
    slot->payload_size = r->pos - payload_start;
    slot->payload = (uint8_t*)malloc(slot->payload_size);
    if(!slot->payload){
      if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic project: out of memory retaining legacy payload");
      free(slot->name); slot->name = NULL;
      free(slot->source); slot->source = NULL;
      return 0;
    }
    memcpy(slot->payload, r->data + payload_start, slot->payload_size);
    slot->legacy_layout = 1;
  }
  return valid;
}



static int parse_legacy_executable_slot(ClassicReader *r,GmlcClassicResourceType type,
                                        GmlcClassicResourceSlot *slot){
  uint32_t exists=0;
  if(!reader_u32(r,&exists,"compiled legacy resource existence flag")) return 0;
  slot->exists=exists!=0;
  if(!slot->exists) return 1;
  if(!reader_string_copy(r,&slot->name,"compiled legacy resource name") ||
     !reader_u32(r,&slot->version,"compiled legacy resource version")) return 0;
  size_t payload_start=r->pos;
  int valid=0;
  switch(type){
    case GMLC_CLASSIC_SOUND: valid=validate_sound_payload(r); break;
    case GMLC_CLASSIC_SPRITE: valid=validate_legacy_executable_sprite_payload(r); break;
    case GMLC_CLASSIC_BACKGROUND: valid=validate_legacy_executable_background_payload(r); break;
    case GMLC_CLASSIC_PATH: valid=validate_legacy_executable_path_payload(r); break;
    case GMLC_CLASSIC_SCRIPT:
      valid=reader_legacy_executable_script(r,&slot->source); break;
    case GMLC_CLASSIC_FONT: valid=validate_font_payload(r,1,1); break;
    case GMLC_CLASSIC_TIMELINE: valid=validate_timeline_payload(r); break;
    case GMLC_CLASSIC_OBJECT: valid=validate_object_payload(r); break;
    case GMLC_CLASSIC_ROOM: valid=validate_legacy_executable_room_payload(r); break;
    default: break;
  }
  if(!valid){
    free(slot->name); slot->name=NULL;
    free(slot->source); slot->source=NULL;
    return 0;
  }
  slot->payload_size=r->pos-payload_start;
  if(slot->payload_size){
    slot->payload=(uint8_t*)malloc(slot->payload_size);
    if(!slot->payload){
      free(slot->name); slot->name=NULL;
      free(slot->source); slot->source=NULL;
      return reader_fail(r,"compiled legacy resource allocation");
    }
    memcpy(slot->payload,r->data+payload_start,slot->payload_size);
  }
  slot->legacy_layout=1;
  slot->executable_layout=1;
  return 1;
}

static int read_legacy_included_file(ClassicReader *r, GmlcClassicIncludedFile *out){
  uint32_t version=0,data_exists=0,stored=0,overwrite=0,free_memory=0,remove_at_end=0;
  GmlcClassicBlob embedded={0};
  memset(out,0,sizeof(*out));
  int ok=reader_u32(r,&version,"legacy included-file version") && version>=620 &&
    reader_string_copy(r,&out->file_name,"legacy included-file name") &&
    reader_string_copy(r,&out->source_path,"legacy included-file source path") &&
    reader_u32(r,&data_exists,"legacy included-file data flag") &&
    reader_u32(r,&out->source_length,"legacy included-file source length") &&
    reader_u32(r,&stored,"legacy included-file storage flag");
  if(ok && data_exists && stored) ok=reader_blob_copy(r,&embedded,"legacy included-file data");
  if(ok) ok=reader_u32(r,&out->export_mode,"legacy included-file export mode") &&
    reader_string_copy(r,&out->custom_folder,"legacy included-file custom folder") &&
    reader_u32(r,&overwrite,"legacy included-file overwrite flag") &&
    reader_u32(r,&free_memory,"legacy included-file free-memory flag") &&
    reader_u32(r,&remove_at_end,"legacy included-file remove flag");
  out->data_exists=data_exists!=0; out->stored_in_project=stored!=0;
  out->overwrite_file=overwrite!=0; out->free_memory=free_memory!=0;
  out->remove_at_end=remove_at_end!=0; out->data=embedded.data; out->data_size=embedded.size;
  if(!ok){
    free(out->file_name); free(out->source_path); free(out->custom_folder); free(out->data);
    memset(out,0,sizeof(*out));
  }
  return ok;
}

/* GM6/7 store Game Information as fields in the project tail, while GM8 wraps the
 * same logical record in a length-delimited (usually compressed) blob.  Preserve
 * the legacy fields in the normalized GM8-shaped record consumed by the shared
 * software renderer.  The legacy layout has no timestamp, so insert its neutral
 * eight-byte value rather than dropping the whole information page. */
static int read_legacy_game_information(ClassicReader *r,uint32_t version,
                                        GmlcClassicBlob *out){
  uint32_t leading[2]={0,0},window[8]={0};
  const uint8_t *caption=NULL,*text=NULL;
  uint32_t caption_size=0,text_size=0;
  memset(out,0,sizeof(*out));
  if(!reader_u32(r,&leading[0],"legacy game-information color") ||
     !reader_u32(r,&leading[1],"legacy game-information mode")) return 0;
  if(version>=600){
    if(!reader_u32(r,&caption_size,"legacy game-information caption") ||
       r->pos>r->size || caption_size>r->size-r->pos)
      return reader_fail(r,"legacy game-information caption");
    caption=r->data+r->pos;
    if(!reader_skip(r,caption_size,"legacy game-information caption")) return 0;
    for(size_t i=0;i<8;i++)
      if(!reader_u32(r,&window[i],"legacy game-information window fields")) return 0;
  }
  if(!reader_u32(r,&text_size,"legacy game information") ||
     r->pos>r->size || text_size>r->size-r->pos)
    return reader_fail(r,"legacy game information");
  text=r->data+r->pos;
  if(!reader_skip(r,text_size,"legacy game information")) return 0;

  size_t size=56u+(size_t)caption_size;
  if(size<caption_size || (size_t)text_size>SIZE_MAX-size){
    if(r->err && r->errcap)
      snprintf(r->err,r->errcap,"classic project: legacy game information is too large");
    return 0;
  }
  size+=(size_t)text_size;
  uint8_t *record=(uint8_t*)calloc(size?size:1u,1u);
  if(!record){
    if(r->err && r->errcap)
      snprintf(r->err,r->errcap,"classic project: out of memory reading legacy game information");
    return 0;
  }
  size_t at=0;
  write_u32le(record+at,leading[0]); at+=4;
  write_u32le(record+at,leading[1]); at+=4;
  write_u32le(record+at,caption_size); at+=4;
  if(caption_size){ memcpy(record+at,caption,caption_size); at+=caption_size; }
  for(size_t i=0;i<8;i++){ write_u32le(record+at,window[i]); at+=4; }
  at+=8; /* normalized timestamp */
  write_u32le(record+at,text_size); at+=4;
  if(text_size){ memcpy(record+at,text,text_size); at+=text_size; }
  out->data=record;
  out->size=at;
  return 1;
}

/* GM6/7 keep project metadata and the executable room order after the resource
 * arrays. The resource tree follows this data, but room sequencing is already
 * represented explicitly here and must not be inferred from sparse slot ids. */
static int parse_legacy_tail(ClassicReader *r, uint32_t container_version,
                             GmlcClassicManifest *manifest){
  if(r->pos==r->size || !manifest) return 1;
  uint32_t version=0,count=0;
  int gm7=container_version==GMLC_CLASSIC_GM7 || container_version==GMLC_CLASSIC_GM7_ALT;
  if(gm7){
    if(!reader_u32(r,&version,"legacy included-file section version") || version<620 ||
       !reader_u32(r,&count,"legacy included-file count")) return 0;
    if(count>(r->size-r->pos)/36) return reader_fail(r,"legacy included files");
    if(count){
      manifest->included_files=(GmlcClassicIncludedFile*)calloc(count,sizeof(*manifest->included_files));
      if(!manifest->included_files) return reader_fail(r,"legacy included-file allocation");
      manifest->included_file_count=count;
    }
    for(uint32_t i=0;i<count;i++) if(!read_legacy_included_file(r,&manifest->included_files[i])) return 0;

    if(!reader_u32(r,&version,"legacy extension section version") || version<700 ||
       !reader_u32(r,&count,"legacy extension count")) return 0;
    if(count>(r->size-r->pos)/4) return reader_fail(r,"legacy extensions");
    if(count){
      manifest->extension_names=(char**)calloc(count,sizeof(*manifest->extension_names));
      if(!manifest->extension_names) return reader_fail(r,"legacy extension allocation");
      manifest->extension_count=count;
    }
    for(uint32_t i=0;i<count;i++)
      if(!reader_string_copy(r,&manifest->extension_names[i],"legacy extension name")) return 0;
  }

  if(!reader_u32(r,&version,"legacy game-information version") || version<430 ||
     !read_legacy_game_information(r,version,&manifest->game_information)) return 0;

  if(!reader_u32(r,&version,"legacy library-code section version") || version<500 ||
     !reader_u32(r,&count,"legacy library-code count")) return 0;
  if(count>(r->size-r->pos)/4) return reader_fail(r,"legacy library creation code");
  if(count){
    manifest->library_creation_code=(char**)calloc(count,sizeof(*manifest->library_creation_code));
    if(!manifest->library_creation_code) return reader_fail(r,"legacy library-code allocation");
    manifest->library_creation_code_count=count;
  }
  for(uint32_t i=0;i<count;i++)
    if(!reader_string_copy(r,&manifest->library_creation_code[i],"legacy library creation code")) return 0;

  uint32_t slots_count=manifest->inventory.resource_slots[GMLC_CLASSIC_ROOM];
  if(!reader_u32(r,&version,"legacy room-order section version") || version<500 ||
     !reader_u32(r,&count,"legacy room-order count") || count>slots_count ||
     count>(r->size-r->pos)/4) return reader_fail(r,"legacy room order");
  if(count!=manifest->existing[GMLC_CLASSIC_ROOM]){
    if(r->err && r->errcap)
      snprintf(r->err,r->errcap,"classic project: legacy room order has %u entries for %u rooms",
               count,manifest->existing[GMLC_CLASSIC_ROOM]);
    return 0;
  }
  manifest->room_order=(uint32_t*)calloc(count?count:1,sizeof(*manifest->room_order));
  unsigned char *seen=(unsigned char*)calloc(slots_count?slots_count:1,1);
  if(!manifest->room_order || !seen){ free(seen); return reader_fail(r,"legacy room-order allocation"); }
  manifest->room_order_count=count;
  for(uint32_t i=0;i<count;i++){
    uint32_t slot=0;
    if(!reader_u32(r,&slot,"legacy room index")){ free(seen); return 0; }
    if(slot>=slots_count || !manifest->slots[GMLC_CLASSIC_ROOM][slot].exists || seen[slot]){
      free(seen);
      if(r->err && r->errcap) snprintf(r->err,r->errcap,"classic project: invalid legacy room index %u",slot);
      return 0;
    }
    seen[slot]=1; manifest->room_order[i]=slot;
  }
  free(seen);
  return 1;
}

static int parse_legacy_project(const void *data, size_t size,
                                GmlcClassicInventory *inventory,
                                GmlcClassicManifest *manifest,
                                char *err, size_t errcap){
  const uint8_t *plain = (const uint8_t*)data;
  size_t plain_size = size;
  uint8_t *decoded = NULL;
  uint32_t container_version = size >= 8 ? read_u32le(plain + 4) : 0;
  if(container_version == GMLC_CLASSIC_GM7 || container_version == GMLC_CLASSIC_GM7_ALT){
    if(!(0 /* This operation is unavailable. */)) return 0;
    plain = decoded;
  }
  if(plain_size < 28 || read_u32le(plain) != GMLC_CLASSIC_MAGIC){
    if(err && errcap) snprintf(err, errcap, "classic project: truncated legacy project header");
    free(decoded);
    return 0;
  }
  memset(inventory, 0, sizeof(*inventory));
  inventory->header.version = (GmlcClassicVersion)container_version;
  inventory->header.game_id = read_u32le(plain + 8);
  memcpy(inventory->header.guid, plain + 12, 16);
  ClassicReader r = {plain, plain_size, 28, err, errcap};
  if(!skip_legacy_settings(&r, container_version, &inventory->settings_version,
                           &inventory->settings,&inventory->constants,manifest)){
    free(decoded);
    return 0;
  }
  for(int type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    uint32_t section_version, count;
    inventory->resource_section_offsets[type] = r.pos;
    if(!reader_u32(&r, &section_version, "legacy resource section version") ||
       !reader_u32(&r, &count, "legacy resource count")) goto fail;
    (void)section_version;
    inventory->resource_slots[type] = count;
    if(count > (r.size - r.pos) / 4){ reader_fail(&r, "legacy resource slots"); goto fail; }
    GmlcClassicResourceSlot *slots = NULL;
    if(manifest && count){
      slots = (GmlcClassicResourceSlot*)calloc(count, sizeof(*slots));
      if(!slots){
        if(err && errcap) snprintf(err, errcap, "classic project: out of memory allocating legacy resources");
        goto fail;
      }
      manifest->slots[type] = slots;
    }
    for(uint32_t i = 0; i < count; ++i){
      GmlcClassicResourceSlot temporary = {0};
      GmlcClassicResourceSlot *slot = slots ? &slots[i] : &temporary;
      if(!parse_legacy_slot(&r, (GmlcClassicResourceType)type, slot, slots != NULL)){
        free(temporary.name); free(temporary.source); free(temporary.payload);
        goto fail;
      }
      if(slot->exists && manifest) ++manifest->existing[type];
      if(!slots){ free(temporary.name); free(temporary.source); free(temporary.payload); }
    }
  }
  if(!reader_u32(&r, &inventory->last_instance_id, "last legacy instance id") ||
     !reader_u32(&r, &inventory->last_tile_id, "last legacy tile id")) goto fail;
  inventory->payload_end = r.pos;
  if(!parse_legacy_tail(&r,container_version,manifest)) goto fail;
  free(decoded);
  return 1;
fail:
  free(decoded);
  if(manifest) gmlc_classic_manifest_free(manifest);
  return 0;
}

int gmlc_classic_inventory(const void *data, size_t size,
                           GmlcClassicInventory *out, char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!data || !out){
    if(err && errcap) snprintf(err, errcap, "classic project: invalid inventory arguments");
    return 0;
  }
  memset(out, 0, sizeof(*out));
  if(!gmlc_classic_probe(data, size, &out->header, err, errcap)) return 0;
  if(out->header.version == GMLC_CLASSIC_GM6 || out->header.version == GMLC_CLASSIC_GM7 ||
     out->header.version == GMLC_CLASSIC_GM7_ALT)
    return parse_legacy_project(data, size, out, NULL, err, errcap);
  if(out->header.version != GMLC_CLASSIC_GM8 && out->header.version != GMLC_CLASSIC_GM81){
    if(err && errcap)
      snprintf(err, errcap, "classic project: inventory for container version %u is not implemented yet",
               (unsigned)out->header.version);
    return 0;
  }

  ClassicReader r = {(const uint8_t*)data, size, 28, err, errcap};
  uint32_t compressed_length, section_version;
  if(!reader_u32(&r, &out->settings_version, "settings version") ||
     !reader_u32(&r, &compressed_length, "compressed settings length")) return 0;
  if(r.pos>r.size || compressed_length>r.size-r.pos) return reader_fail(&r,"compressed settings");
  if(!read_compressed_settings(r.data+r.pos,compressed_length,&out->settings,err,errcap) ||
     !reader_skip(&r, compressed_length, "compressed settings")) return 0;

  if(!reader_u32(&r, &section_version, "trigger section version") ||
     !reader_u32(&r, &out->trigger_slots, "trigger count") ||
     !reader_blocks(&r, out->trigger_slots, "trigger block") ||
     !reader_skip(&r, 8, "trigger timestamp")) return 0;
  if(section_version < 800){
    if(err && errcap) snprintf(err, errcap, "classic project: unsupported trigger section version %u", section_version);
    return 0;
  }

  if(!reader_u32(&r, &section_version, "constant section version") ||
     !reader_u32(&r, &out->constants, "constant count")) return 0;
  if(section_version < 800){
    if(err && errcap) snprintf(err, errcap, "classic project: unsupported constant section version %u", section_version);
    return 0;
  }
  for(uint32_t i = 0; i < out->constants; ++i)
    if(!reader_string(&r, "constant name") || !reader_string(&r, "constant value")) return 0;
  if(!reader_skip(&r, 8, "constant timestamp")) return 0;

  for(int type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    out->resource_section_offsets[type] = r.pos;
    if(!reader_u32(&r, &section_version, "resource section version") ||
       !reader_u32(&r, &out->resource_slots[type], "resource count") ||
       !reader_blocks(&r, out->resource_slots[type], gmlc_classic_resource_name((GmlcClassicResourceType)type)))
      return 0;
    if(section_version < 800){
      if(err && errcap)
        snprintf(err, errcap, "classic project: unsupported %s section version %u",
                 gmlc_classic_resource_name((GmlcClassicResourceType)type), section_version);
      return 0;
    }
  }
  if(!reader_u32(&r, &out->last_instance_id, "last instance id") ||
     !reader_u32(&r, &out->last_tile_id, "last tile id")) return 0;
  out->payload_end = r.pos;
  return 1;
}

static int parse_manifest_slot_layout(GmlcClassicResourceType type,
                                      const uint8_t *compressed, uint32_t compressed_size,
                                      GmlcClassicResourceSlot *slot, int has_timestamp,
                                      int raw_deflate, char *err, size_t errcap){
  if(compressed_size > INT_MAX){
    if(err && errcap) snprintf(err, errcap, "classic project: compressed resource is too large");
    return 0;
  }
  int raw_size = 0;
  char *raw = raw_deflate && compressed_size>=6
    ? classic_inflate_owned(compressed+2,compressed_size-6,GML_DEFLATE_RAW,&raw_size)
    : classic_inflate_owned(compressed,compressed_size,GML_DEFLATE_ZLIB,&raw_size);
  if(!raw || raw_size < 4){
    if(err && errcap) snprintf(err, errcap, "classic project: invalid compressed resource block");
    free(raw);
    return 0;
  }
  if(raw_deflate){
    char *p=(char*)realloc(raw,(size_t)raw_size+1);
    if(!p){ free(raw); return 0; }
    raw=p; raw[raw_size++]=0; /* tolerate the compact form's elided final zero byte */
    if(type==GMLC_CLASSIC_OBJECT){
      ClassicReader probe={(const uint8_t*)raw,(size_t)raw_size,0,NULL,0};
      uint32_t exists=0,version=0;
      int complete=reader_u32(&probe,&exists,"object exists") &&
        (!exists || (reader_string(&probe,"object name") && reader_u32(&probe,&version,"object version") &&
                     validate_object_payload(&probe))) &&
        (probe.pos==probe.size || (probe.pos+1==probe.size && probe.data[probe.pos]==0));
      (void)version;
      if(!complete){ raw_size--;
        if(!repair_executable_object(&raw,&raw_size)){ free(raw); return 0; }
      }
    } else if(type==GMLC_CLASSIC_ROOM){
      raw_size--;
      if(!normalize_executable_room(&raw,&raw_size)){
        if(err && errcap) snprintf(err,errcap,"classic executable: invalid compact room block");
        free(raw); return 0;
      }
    }
  }
  ClassicReader r = {(const uint8_t*)raw, (size_t)raw_size, 0, err, errcap};
  slot->executable_layout=raw_deflate;
  uint32_t exists;
  if(!reader_u32(&r, &exists, "resource existence flag")){
    free(raw);
    return 0;
  }
  slot->exists = exists != 0;
  size_t payload_start = 0;
  if(slot->exists &&
     (!reader_string_copy(&r, &slot->name, "resource name") ||
      (has_timestamp && !reader_skip(&r, 8, "resource timestamp")) ||
      !reader_u32(&r, &slot->version, "resource format version"))){
    free(slot->name);
    slot->name = NULL;
    free(raw);
    return 0;
  }
  if(slot->exists) payload_start = r.pos;
  if(slot->exists && type == GMLC_CLASSIC_SCRIPT &&
     !reader_string_copy(&r, &slot->source, "script source")){
    free(slot->name);
    slot->name = NULL;
    free(raw);
    return 0;
  }
  int valid = 1;
  if(slot->exists){
    switch(type){
      case GMLC_CLASSIC_SOUND: valid = validate_sound_payload(&r); break;
      case GMLC_CLASSIC_SPRITE: valid = validate_sprite_payload(&r,raw_deflate,slot->version); break;
      case GMLC_CLASSIC_BACKGROUND: valid = validate_background_payload(&r,raw_deflate); break;
      case GMLC_CLASSIC_PATH: valid = validate_path_payload(&r,raw_deflate); break;
      case GMLC_CLASSIC_SCRIPT: break;
      case GMLC_CLASSIC_FONT: valid = validate_font_payload(&r,raw_deflate,0); break;
      case GMLC_CLASSIC_TIMELINE: valid = validate_timeline_payload(&r); break;
      case GMLC_CLASSIC_OBJECT: valid = validate_object_payload(&r); break;
      case GMLC_CLASSIC_ROOM: valid = validate_room_payload(&r); break;
      default: break;
    }
    if(valid){
      if(raw_deflate && r.pos+1==r.size && r.data[r.pos]==0) { /* synthetic padding */ }
      else valid = require_payload_end(&r, "resource payload");
    }
  } else {
    if(raw_deflate && r.pos+1==r.size && r.data[r.pos]==0) { /* synthetic padding */ }
    else valid = require_payload_end(&r, "absent resource slot");
  }
  if(!valid){
    /* Preserve enough structural context for an unfamiliar layout variant to be diagnosed from
     * its loader error alone. Resource blocks are independent, so a bare "trailing bytes" error
     * otherwise gives no indication which layout needs extending. */
    if(err && errcap){
      char cause[256];
      snprintf(cause,sizeof(cause),"%s",err[0]?err:"invalid resource payload");
      snprintf(err,errcap,"classic project: invalid %s resource '%s' (version %u): %s",
               gmlc_classic_resource_name(type),slot->name?slot->name:"",slot->version,cause);
    }
    free(slot->name); slot->name = NULL;
    free(slot->source); slot->source = NULL;
    free(raw);
    return 0;
  }
  if(slot->exists && r.pos > payload_start){
    slot->payload_size = r.pos - payload_start;
    slot->payload = (uint8_t*)malloc(slot->payload_size);
    if(!slot->payload){
      if(err && errcap) snprintf(err, errcap, "classic project: out of memory retaining resource payload");
      free(slot->name); slot->name = NULL;
      free(slot->source); slot->source = NULL;
      free(raw);
      return 0;
    }
    memcpy(slot->payload, (const uint8_t*)raw + payload_start, slot->payload_size);
  }
  free(raw);
  return 1;
}

static int parse_manifest_slot(GmlcClassicResourceType type,
                               const uint8_t *compressed, uint32_t compressed_size,
                               GmlcClassicResourceSlot *slot, char *err, size_t errcap){
  return parse_manifest_slot_layout(type,compressed,compressed_size,slot,1,0,err,errcap);
}

void gmlc_classic_manifest_free(GmlcClassicManifest *manifest){
  if(!manifest) return;
  for(int type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    uint32_t count = manifest->inventory.resource_slots[type];
    for(uint32_t i = 0; i < count; ++i){
      free(manifest->slots[type][i].name);
      free(manifest->slots[type][i].source);
      free(manifest->slots[type][i].payload);
    }
    free(manifest->slots[type]);
  }
  for(uint32_t i=0;i<manifest->constant_def_count;i++){
    free(manifest->constant_defs[i].name); free(manifest->constant_defs[i].value);
  }
  free(manifest->constant_defs);
  for(uint32_t i=0;i<manifest->trigger_def_count;i++){
    free(manifest->trigger_defs[i].name);
    free(manifest->trigger_defs[i].condition);
    free(manifest->trigger_defs[i].constant_name);
  }
  free(manifest->trigger_defs);
  for(uint32_t i=0;i<manifest->included_file_count;i++){
    free(manifest->included_files[i].file_name);
    free(manifest->included_files[i].source_path);
    free(manifest->included_files[i].custom_folder);
    free(manifest->included_files[i].data);
  }
  free(manifest->included_files);
  for(uint32_t i=0;i<manifest->extension_count;i++) free(manifest->extension_names[i]);
  free(manifest->extension_names);
  free(manifest->game_information.data);
  for(uint32_t i=0;i<manifest->library_creation_code_count;i++) free(manifest->library_creation_code[i]);
  free(manifest->library_creation_code);
  free(manifest->room_order);
  memset(manifest, 0, sizeof(*manifest));
}

static int parse_modern_metadata(const void *data, size_t size,
                                 GmlcClassicManifest *manifest,
                                 char *err, size_t errcap){
  ClassicReader r={(const uint8_t*)data,size,28,err,errcap};
  uint32_t version,count,compressed_length;
  if(!reader_u32(&r,&version,"settings version") ||
     !reader_u32(&r,&compressed_length,"compressed settings length") ||
     !reader_skip(&r,compressed_length,"compressed settings") ||
     !reader_u32(&r,&version,"trigger section version") || version<800 ||
     !reader_u32(&r,&count,"trigger count") || count!=manifest->inventory.trigger_slots)
    return reader_fail(&r,"trigger metadata");
  if(count){
    manifest->trigger_defs=(GmlcClassicTrigger*)calloc(count,sizeof(*manifest->trigger_defs));
    if(!manifest->trigger_defs) return reader_fail(&r,"trigger allocation");
    manifest->trigger_def_count=count;
  }
  for(uint32_t i=0;i<count;i++)
    if(!reader_trigger(&r,&manifest->trigger_defs[i],"trigger block")) return 0;
  if(!reader_skip(&r,8,"trigger timestamp") ||
     !reader_u32(&r,&version,"constant section version") || version<800 ||
     !reader_u32(&r,&count,"constant count") || count!=manifest->inventory.constants)
    return reader_fail(&r,"constant metadata");
  if(count){
    manifest->constant_defs=(GmlcClassicConstant*)calloc(count,sizeof(*manifest->constant_defs));
    if(!manifest->constant_defs) return reader_fail(&r,"constant allocation");
    manifest->constant_def_count=count;
  }
  for(uint32_t i=0;i<count;i++)
    if(!reader_string_copy(&r,&manifest->constant_defs[i].name,"constant name") ||
       !reader_string_copy(&r,&manifest->constant_defs[i].value,"constant value")) return 0;
  return reader_skip(&r,8,"constant timestamp");
}

static int parse_modern_tail(const void *data, size_t size,
                             GmlcClassicManifest *manifest,
                             char *err, size_t errcap){
  ClassicReader r = {(const uint8_t*)data, size, manifest->inventory.payload_end, err, errcap};
  uint32_t version, count;
  if(!reader_u32(&r, &version, "included-file section version") || version < 620 ||
     !reader_u32(&r, &count, "included-file count")) return 0;
  if(count > (r.size - r.pos) / 4) return reader_fail(&r, "included files");
  if(count){
    manifest->included_files=(GmlcClassicIncludedFile*)calloc(count,sizeof(*manifest->included_files));
    if(!manifest->included_files) return reader_fail(&r,"included-file allocation");
    manifest->included_file_count=count;
  }
  for(uint32_t i = 0; i < count; ++i)
    if(!reader_included_file(&r,&manifest->included_files[i],1,"included file")) return 0;
  if(!reader_u32(&r, &version, "extension section version") || version < 700 ||
     !reader_u32(&r, &count, "extension count")) return 0;
  if(count > (r.size - r.pos) / 4) return reader_fail(&r, "extensions");
  if(count){
    manifest->extension_names=(char**)calloc(count,sizeof(*manifest->extension_names));
    if(!manifest->extension_names) return reader_fail(&r,"extension allocation");
    manifest->extension_count=count;
  }
  for(uint32_t i = 0; i < count; ++i)
    if(!reader_string_copy(&r,&manifest->extension_names[i], "extension name")) return 0;
  if(!reader_u32(&r, &version, "game-information version") || version < 600 ||
     !reader_blob_copy(&r, &manifest->game_information, "game information") ||
     !reader_u32(&r, &version, "library-code section version") || version < 500 ||
     !reader_u32(&r, &count, "library-code count")) return 0;
  if(count > (r.size - r.pos) / 4) return reader_fail(&r, "library creation code");
  if(count){
    manifest->library_creation_code=(char**)calloc(count,sizeof(*manifest->library_creation_code));
    if(!manifest->library_creation_code) return reader_fail(&r,"library creation-code allocation");
    manifest->library_creation_code_count=count;
  }
  for(uint32_t i = 0; i < count; ++i)
    if(!reader_string_copy(&r,&manifest->library_creation_code[i], "library creation code")) return 0;
  if(!reader_u32(&r, &version, "room-order section version") || version < 500 ||
     !reader_u32(&r, &count, "executable room count") ||
     count > manifest->inventory.resource_slots[GMLC_CLASSIC_ROOM] ||
     count > (r.size - r.pos) / 4) return reader_fail(&r, "executable room order");
  manifest->room_order = (uint32_t*)calloc(count ? count : 1, sizeof(*manifest->room_order));
  if(!manifest->room_order){
    if(err && errcap) snprintf(err, errcap, "classic project: out of memory reading room order");
    return 0;
  }
  manifest->room_order_count = count;
  for(uint32_t i = 0; i < count; ++i){
    if(!reader_u32(&r, &manifest->room_order[i], "executable room index")) return 0;
    if(manifest->room_order[i] >= manifest->inventory.resource_slots[GMLC_CLASSIC_ROOM]){
      if(err && errcap) snprintf(err, errcap, "classic project: invalid executable room index %u",
                                 manifest->room_order[i]);
      return 0;
    }
  }
  return 1;
}

static int manifest_append_constant(GmlcClassicManifest *manifest, char *name, char *value){
  uint32_t count=manifest->constant_def_count;
  GmlcClassicConstant *items=(GmlcClassicConstant*)realloc(
    manifest->constant_defs,(size_t)(count+1)*sizeof(*items));
  if(!items) return 0;
  manifest->constant_defs=items;
  items[count].name=name; items[count].value=value;
  manifest->constant_def_count=count+1;
  return 1;
}

static int manifest_append_library_code(GmlcClassicManifest *manifest, char *source){
  uint32_t count=manifest->library_creation_code_count;
  char **items=(char**)realloc(manifest->library_creation_code,(size_t)(count+1)*sizeof(*items));
  if(!items) return 0;
  manifest->library_creation_code=items; items[count]=source;
  manifest->library_creation_code_count=count+1;
  return 1;
}

static int parse_executable_extensions(ClassicReader *r, GmlcClassicManifest *out,
                                       uint32_t count){
  if(count){
    out->extension_names=(char**)calloc(count,sizeof(*out->extension_names));
    if(!out->extension_names) return reader_fail(r,"executable extension allocation");
    out->extension_count=count;
  }
  for(uint32_t extension=0;extension<count;extension++){
    uint32_t version=0,file_count=0;
    if(!reader_u32(r,&version,"executable extension version") || version<700 ||
       !reader_string_copy(r,&out->extension_names[extension],"executable extension name") ||
       !reader_string(r,"executable extension folder") ||
       !reader_u32(r,&file_count,"executable extension file count")) return 0;
    if(file_count>(r->size-r->pos)/20) return reader_fail(r,"executable extension files");
    for(uint32_t file=0;file<file_count;file++){
      uint32_t kind=0,function_count=0,constant_count=0;
      char *initializer=NULL,*finalizer=NULL;
      if(!reader_u32(r,&version,"executable extension-file version") || version<700 ||
         !reader_string(r,"executable extension-file name") ||
         !reader_u32(r,&kind,"executable extension-file kind") ||
         !reader_string_copy(r,&initializer,"executable extension initializer") ||
         !reader_string_copy(r,&finalizer,"executable extension finalizer") ||
         !reader_u32(r,&function_count,"executable extension function count")){
        free(initializer); free(finalizer); return 0;
      }
      if(initializer && *initializer){
        if(!manifest_append_library_code(out,initializer)){ free(initializer); free(finalizer); return 0; }
      } else free(initializer);
      free(finalizer);
      (void)kind;
      if(function_count>(r->size-r->pos)/96) return reader_fail(r,"executable extension functions");
      for(uint32_t function=0;function<function_count;function++)
        if(!reader_u32(r,&version,"executable extension-function version") || version<700 ||
           !reader_string(r,"executable extension-function name") ||
           !reader_string(r,"executable extension-function external name") ||
           !reader_words(r,21,"executable extension-function signature")) return 0;
      if(!reader_u32(r,&constant_count,"executable extension constant count")) return 0;
      if(constant_count>(r->size-r->pos)/12) return reader_fail(r,"executable extension constants");
      for(uint32_t constant=0;constant<constant_count;constant++){
        char *name=NULL,*value=NULL;
        if(!reader_u32(r,&version,"executable extension-constant version") || version<700 ||
           !reader_string_copy(r,&name,"executable extension-constant name") ||
           !reader_string_copy(r,&value,"executable extension-constant value") ||
           !manifest_append_constant(out,name,value)){
          free(name); free(value); return 0;
        }
      }
    }
    uint32_t encrypted_size=0;
    if(!reader_u32(r,&encrypted_size,"executable extension data length") || encrypted_size<4 ||
       !reader_skip(r,encrypted_size,"executable extension data")) return 0;
  }
  return 1;
}



static int parse_legacy_executable_data(const uint8_t *data,size_t size,
                                        uint32_t settings_version,
                                        const GmlcClassicSettings *settings,
                                        GmlcClassicManifest *out,char *err,size_t errcap){
  ClassicReader r={data,size,0,err,errcap};
  uint32_t runner_id=0,version=0,count=0;
  if(!reader_u32(&r,&runner_id,"legacy executable runtime id") ||
     !reader_u32(&r,&out->inventory.header.game_id,"legacy executable game id") ||
     !reader_skip(&r,16,"legacy executable guid")) return 0;
  memcpy(out->inventory.header.guid,data+r.pos-16u,16u);
  out->inventory.header.version=GMLC_CLASSIC_GM7;
  out->inventory.settings_version=settings_version;
  if(settings) out->inventory.settings=*settings;
  (void)runner_id;

  size_t section_offset=r.pos;
  if(!reader_u32(&r,&version,"legacy executable extension version") || version<700 ||
     !reader_u32(&r,&count,"legacy executable extension count") ||
     !parse_executable_extensions(&r,out,count)){
    if(err && errcap && !err[0])
      snprintf(err,errcap,"classic executable: invalid extension section at offset %" PRIu64,
               (uint64_t)section_offset);
    return 0;
  }

  for(int type=0;type<GMLC_CLASSIC_RESOURCE_TYPES;type++){
    section_offset=r.pos;
    if(!reader_u32(&r,&version,"legacy executable resource version") || version<400 ||
       !reader_u32(&r,&count,"legacy executable resource count") ||
       count>(r.size-r.pos)/4u){
      if(err && errcap && !err[0])
        snprintf(err,errcap,"classic executable: invalid %s section at offset %" PRIu64,
                 gmlc_classic_resource_name((GmlcClassicResourceType)type),
                 (uint64_t)section_offset);
      return 0;
    }
    out->inventory.resource_section_offsets[type]=r.pos-8u;
    out->inventory.resource_slots[type]=count;
    if(count){
      out->slots[type]=(GmlcClassicResourceSlot*)calloc(count,sizeof(*out->slots[type]));
      if(!out->slots[type]) return reader_fail(&r,"legacy executable resource allocation");
    }
    for(uint32_t slot=0;slot<count;slot++){
      size_t slot_offset=r.pos;
      if(!parse_legacy_executable_slot(&r,(GmlcClassicResourceType)type,&out->slots[type][slot])){
        if(err && errcap && !err[0])
          snprintf(err,errcap,"classic executable: invalid %s slot %u at offset %" PRIu64,
                   gmlc_classic_resource_name((GmlcClassicResourceType)type),slot,
                   (uint64_t)slot_offset);
        return 0;
      }
      if(out->slots[type][slot].exists) out->existing[type]++;
    }
  }
  if(!reader_u32(&r,&out->inventory.last_instance_id,"legacy executable last instance id") ||
     !reader_u32(&r,&out->inventory.last_tile_id,"legacy executable last tile id")) return 0;
  out->inventory.payload_end=r.pos;

  section_offset=r.pos;
  if(!reader_u32(&r,&version,"legacy executable include version") || version<620 ||
     !reader_u32(&r,&count,"legacy executable include count") ||
     count>(r.size-r.pos)/36u){
    if(err && errcap && !err[0])
      snprintf(err,errcap,
               "classic executable: invalid included-file section at offset %" PRIu64,
               (uint64_t)section_offset);
    return 0;
  }
  if(count){
    out->included_files=(GmlcClassicIncludedFile*)calloc(count,sizeof(*out->included_files));
    if(!out->included_files) return reader_fail(&r,"legacy executable include allocation");
    out->included_file_count=count;
  }
  for(uint32_t include=0;include<count;include++)
    if(!read_legacy_included_file(&r,&out->included_files[include])) return 0;

  section_offset=r.pos;
  if(!reader_u32(&r,&version,"legacy executable game-information version") || version<430 ||
     !reader_words(&r,2,"legacy executable game-information fields")){
    if(err && errcap && !err[0])
      snprintf(err,errcap,
               "classic executable: invalid game-information section at offset %" PRIu64,
               (uint64_t)section_offset);
    return 0;
  }
  if(version>=600 &&
     (!reader_string(&r,"legacy executable game-information caption") ||
      !reader_words(&r,8,"legacy executable game-information window fields"))) return 0;
  section_offset=r.pos;
  if(!reader_blob(&r,"legacy executable game information") ||
     !reader_u32(&r,&version,"legacy executable library-code version") || version<500 ||
     !reader_u32(&r,&count,"legacy executable library-code count") ||
     count>(r.size-r.pos)/4u){
    if(err && errcap && !err[0])
      snprintf(err,errcap,
               "classic executable: invalid information or library-code section at offset %" PRIu64,
               (uint64_t)section_offset);
    return 0;
  }
  for(uint32_t code=0;code<count;code++){
    char *source=NULL;
    if(!reader_string_copy(&r,&source,"legacy executable library creation code") ||
       !manifest_append_library_code(out,source)){
      free(source); return 0;
    }
  }

  section_offset=r.pos;
  if(!reader_u32(&r,&version,"legacy executable room-order version") || version<500 ||
     !reader_u32(&r,&count,"legacy executable room-order count") ||
     count!=out->existing[GMLC_CLASSIC_ROOM] ||
     count>out->inventory.resource_slots[GMLC_CLASSIC_ROOM] ||
     count>(r.size-r.pos)/4u){
    if(err && errcap && !err[0])
      snprintf(err,errcap,"classic executable: invalid room-order section at offset %" PRIu64,
               (uint64_t)section_offset);
    return 0;
  }
  out->room_order=(uint32_t*)calloc(count?count:1u,sizeof(*out->room_order));
  unsigned char *seen=(unsigned char*)calloc(
    out->inventory.resource_slots[GMLC_CLASSIC_ROOM]?
      out->inventory.resource_slots[GMLC_CLASSIC_ROOM]:1u,1u);
  if(!out->room_order || !seen){ free(seen); return reader_fail(&r,"legacy executable room order allocation"); }
  out->room_order_count=count;
  for(uint32_t room=0;room<count;room++){
    uint32_t slot=0;
    if(!reader_u32(&r,&slot,"legacy executable room index") ||
       slot>=out->inventory.resource_slots[GMLC_CLASSIC_ROOM] ||
       !out->slots[GMLC_CLASSIC_ROOM][slot].exists || seen[slot]){
      free(seen); return reader_fail(&r,"legacy executable room order");
    }
    seen[slot]=1; out->room_order[room]=slot;
  }
  free(seen);
  return 1;
}

static int parse_legacy_executable_manifest(const uint8_t *file,size_t size,size_t payload,
                                            GmlcClassicManifest *out,char *err,size_t errcap){
  if(payload>size || size-payload<16u){
    if(err && errcap) snprintf(err,errcap,"classic executable: truncated legacy header");
    return 0;
  }
  uint32_t settings_version=read_u32le(file+payload+12u);
  GmlcClassicSettings settings={0};
  ClassicReader settings_reader={file,size,payload+16u,NULL,0};
  (void)read_settings_prefix(&settings_reader,&settings);

  size_t compressed_pos=(size_t)-1;
  uint32_t compressed_size=0;
  for(size_t pos=payload+16u;pos+6u<=size;pos++){
    uint32_t candidate=read_u32le(file+pos);
    if(candidate>=2u && candidate<=INT_MAX && (size_t)candidate==size-pos-4u &&
       file[pos+4u]==0x78u){
      compressed_pos=pos+4u; compressed_size=candidate;
    }
  }
  if(compressed_pos==(size_t)-1){
    if(err && errcap) snprintf(err,errcap,"classic executable: legacy data block not found");
    return 0;
  }
  int envelope_size=0;
  char *envelope=classic_inflate_owned(file+compressed_pos,compressed_size,
                                       GML_DEFLATE_ZLIB,&envelope_size);
  if(!envelope || envelope_size<13){
    free(envelope);
    if(err && errcap) snprintf(err,errcap,"classic executable: invalid compressed legacy data");
    return 0;
  }
  uint8_t *decoded=NULL; size_t decoded_size=0;
  int ok=0; /* This operation is unavailable. */
  free(envelope);
  if(ok) ok=parse_legacy_executable_data(decoded,decoded_size,settings_version,
                                         &settings,out,err,errcap);
  free(decoded);
  if(!ok) gmlc_classic_manifest_free(out);
  return ok;
}

static int parse_executable_data(const uint8_t *data, size_t size,
                                 uint32_t version, uint32_t settings_version,
                                 GmlcClassicManifest *out, char *err, size_t errcap){
  ClassicReader r={data,size,0,err,errcap};
  uint32_t count,section_version,pro;
  if(!reader_u32(&r,&count,"executable leading junk count") ||
     count>(r.size-r.pos)/4 || !reader_words(&r,count,"executable leading junk") ||
     !reader_u32(&r,&pro,"executable edition flag") ||
     !reader_u32(&r,&out->inventory.header.game_id,"executable game id") ||
     !reader_skip(&r,16,"executable guid")) return 0;
  memcpy(out->inventory.header.guid,data+r.pos-16,16);
  out->inventory.header.version=(GmlcClassicVersion)version;
  out->inventory.settings_version=settings_version;
  (void)pro;
  if(!reader_u32(&r,&section_version,"executable extension version") ||
     !reader_u32(&r,&count,"executable extension count")) return 0;
  if(section_version<700 || !parse_executable_extensions(&r,out,count)) return 0;
  if(!reader_u32(&r,&section_version,"executable trigger version") || section_version<800 ||
     !reader_u32(&r,&out->inventory.trigger_slots,"executable trigger count")) return 0;
  if(out->inventory.trigger_slots){
    out->trigger_defs=(GmlcClassicTrigger*)calloc(out->inventory.trigger_slots,sizeof(*out->trigger_defs));
    if(!out->trigger_defs) return reader_fail(&r,"executable trigger allocation");
    out->trigger_def_count=out->inventory.trigger_slots;
  }
  for(uint32_t i=0;i<out->inventory.trigger_slots;i++)
    if(!reader_trigger(&r,&out->trigger_defs[i],"executable trigger")) return 0;
  if(!reader_u32(&r,&section_version,"executable constant version") || section_version<800 ||
     !reader_u32(&r,&out->inventory.constants,"executable constant count")) return 0;
  for(uint32_t i=0;i<out->inventory.constants;i++){
    char *name=NULL,*value=NULL;
    if(!reader_string_copy(&r,&name,"executable constant name") ||
       !reader_string_copy(&r,&value,"executable constant value") ||
       !manifest_append_constant(out,name,value)){
      free(name); free(value); return 0;
    }
  }
  for(int type=0;type<GMLC_CLASSIC_RESOURCE_TYPES;type++){
    if(!reader_u32(&r,&section_version,"executable resource version") || section_version<800 ||
       !reader_u32(&r,&count,"executable resource count")) return 0;
    out->inventory.resource_section_offsets[type]=r.pos-8;
    out->inventory.resource_slots[type]=count;
    if(count){
      out->slots[type]=(GmlcClassicResourceSlot*)calloc(count,sizeof(*out->slots[type]));
      if(!out->slots[type]) return reader_fail(&r,"executable resource allocation");
    }
    for(uint32_t i=0;i<count;i++){
      uint32_t compressed_size;
      if(!reader_u32(&r,&compressed_size,"executable resource length") ||
         r.pos>r.size || compressed_size>r.size-r.pos ||
         !parse_manifest_slot_layout((GmlcClassicResourceType)type,r.data+r.pos,compressed_size,
                                     &out->slots[type][i],0,1,err,errcap)) return 0;
      r.pos+=compressed_size;
      if(out->slots[type][i].exists) out->existing[type]++;
    }
  }
  if(!reader_u32(&r,&out->inventory.last_instance_id,"last executable instance id") ||
     !reader_u32(&r,&out->inventory.last_tile_id,"last executable tile id")) return 0;
  out->inventory.payload_end=r.pos;
  if(!reader_u32(&r,&section_version,"executable include version") ||
     !reader_u32(&r,&count,"executable include count")) return 0;
  if(count){
    out->included_files=(GmlcClassicIncludedFile*)calloc(count,sizeof(*out->included_files));
    if(!out->included_files) return reader_fail(&r,"executable include allocation");
    out->included_file_count=count;
  }
  for(uint32_t i=0;i<count;i++)
    if(!reader_included_file(&r,&out->included_files[i],0,"executable include")) return 0;
  if(!reader_u32(&r,&section_version,"executable help version") ||
     !reader_blob_copy(&r,&out->game_information,"executable help")) return 0;
  if(!reader_u32(&r,&section_version,"executable library version") ||
     !reader_u32(&r,&count,"executable library count")) return 0;
  for(uint32_t i=0;i<count;i++){
    char *source=NULL;
    if(!reader_string_copy(&r,&source,"executable library code") ||
       !manifest_append_library_code(out,source)){
      free(source); return 0;
    }
  }
  if(!reader_u32(&r,&section_version,"executable room-order version") ||
     !reader_u32(&r,&count,"executable room-order count")) return 0;
  if(count>out->inventory.resource_slots[GMLC_CLASSIC_ROOM]){
    if(err && errcap) snprintf(err,errcap,"classic executable: invalid room-order count %u at offset %llu",
                               count,(unsigned long long)r.pos);
    return 0;
  }
  out->room_order=(uint32_t*)calloc(count?count:1,sizeof(*out->room_order));
  if(!out->room_order) return reader_fail(&r,"executable room order allocation");
  out->room_order_count=count;
  for(uint32_t i=0;i<count;i++){
    if(r.size-r.pos<4){
      if(i+1!=count){
        if(err && errcap) snprintf(err,errcap,"classic executable: truncated room order at offset %llu",
                                   (unsigned long long)r.pos);
        return 0;
      }
      uint32_t index=0;
      for(size_t byte=0;byte<r.size-r.pos;byte++) index|=(uint32_t)r.data[r.pos+byte]<<(byte*8);
      out->room_order[i]=index;
      r.pos=r.size;
    } else if(!reader_u32(&r,&out->room_order[i],"executable room index")) return 0;
    if(out->room_order[i]>=out->inventory.resource_slots[GMLC_CLASSIC_ROOM]){
      if(err && errcap && !err[0])
        snprintf(err,errcap,"classic executable: invalid room index at offset %llu",
                 (unsigned long long)r.pos);
      return 0;
    }
  }
  return 1;
}



int gmlc_classic_manifest(const void *data, size_t size,
                          GmlcClassicManifest *out, char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!data || !out){
    if(err && errcap) snprintf(err, errcap, "classic project: invalid manifest arguments");
    return 0;
  }
  memset(out, 0, sizeof(*out));
  if(size>=2 && ((const uint8_t*)data)[0]=='M' && ((const uint8_t*)data)[1]=='Z'){
    int ok=parse_executable_manifest((const uint8_t*)data,size,out,err,errcap);
    if(ok) out->executable_layout=1;
    return ok;
  }
  GmlcClassicHeader header;
  if(!gmlc_classic_probe(data, size, &header, err, errcap)) return 0;
  if(header.version == GMLC_CLASSIC_GM6 || header.version == GMLC_CLASSIC_GM7 ||
     header.version == GMLC_CLASSIC_GM7_ALT)
    return parse_legacy_project(data, size, &out->inventory, out, err, errcap);
  if(!gmlc_classic_inventory(data, size, &out->inventory, err, errcap)) return 0;
  if(!parse_modern_metadata(data,size,out,err,errcap)){
    gmlc_classic_manifest_free(out);
    return 0;
  }
  for(int type = 0; type < GMLC_CLASSIC_RESOURCE_TYPES; ++type){
    uint32_t count = out->inventory.resource_slots[type];
    if(count){
      out->slots[type] = (GmlcClassicResourceSlot*)calloc(count, sizeof(*out->slots[type]));
      if(!out->slots[type]){
        if(err && errcap) snprintf(err, errcap, "classic project: out of memory allocating %s slots",
                                  gmlc_classic_resource_name((GmlcClassicResourceType)type));
        gmlc_classic_manifest_free(out);
        return 0;
      }
    }
    ClassicReader r = {(const uint8_t*)data, size, out->inventory.resource_section_offsets[type], err, errcap};
    uint32_t section_version, observed_count;
    if(!reader_u32(&r, &section_version, "resource section version") ||
       !reader_u32(&r, &observed_count, "resource count") || observed_count != count){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic project: inconsistent resource inventory");
      gmlc_classic_manifest_free(out);
      return 0;
    }
    (void)section_version;
    for(uint32_t i = 0; i < count; ++i){
      uint32_t compressed_size;
      if(!reader_u32(&r, &compressed_size, "compressed resource length") ||
         r.pos > r.size || compressed_size > r.size - r.pos ||
         !parse_manifest_slot((GmlcClassicResourceType)type, r.data + r.pos, compressed_size,
                              &out->slots[type][i], err, errcap)){
        if(err && errcap && !err[0])
          snprintf(err, errcap, "classic project: invalid %s slot %u",
                   gmlc_classic_resource_name((GmlcClassicResourceType)type), i);
        gmlc_classic_manifest_free(out);
        return 0;
      }
      r.pos += compressed_size;
      if(out->slots[type][i].exists) ++out->existing[type];
    }
  }
  if(header.version >= GMLC_CLASSIC_GM8 &&
     !parse_modern_tail(data, size, out, err, errcap)){
    gmlc_classic_manifest_free(out);
    return 0;
  }
  return 1;
}

int gmlc_classic_manifest_file(const AnygmHostServices *host,const char *path,
                               GmlcClassicManifest *out,
                               char *err, size_t errcap){
  uint8_t *data;
  size_t size;
  if(!read_file(host,path, &data, &size, err, errcap)) return 0;
  int ok = gmlc_classic_manifest(data, size, out, err, errcap);
  free(data);
  return ok;
}

int gmlc_classic_inventory_file(const AnygmHostServices *host,const char *path,
                                GmlcClassicInventory *out,
                                char *err, size_t errcap){
  uint8_t *data;
  size_t size;
  if(!read_file(host,path, &data, &size, err, errcap)) return 0;
  int ok = gmlc_classic_inventory(data, size, out, err, errcap);
  free(data);
  return ok;
}

const char *gmlc_classic_version_name(GmlcClassicVersion version){
  switch(version){
    case GMLC_CLASSIC_GM6: return "GameMaker 6";
    case GMLC_CLASSIC_GM7:
    case GMLC_CLASSIC_GM7_ALT: return "GameMaker 7";
    case GMLC_CLASSIC_GM8: return "GameMaker 8";
    case GMLC_CLASSIC_GM81: return "GameMaker 8.1";
    default: return "unknown classic GameMaker";
  }
}

const char *gmlc_classic_resource_name(GmlcClassicResourceType type){
  static const char *const names[GMLC_CLASSIC_RESOURCE_TYPES] = {
    "sound", "sprite", "background", "path", "script", "font",
    "timeline", "object", "room"
  };
  return type >= 0 && type < GMLC_CLASSIC_RESOURCE_TYPES ? names[type] : "resource";
}
