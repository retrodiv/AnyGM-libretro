/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_classic.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_GIF
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static uint32_t read_u32le(const uint8_t *p){
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
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
    snprintf(r->err, r->errcap, "classic project: truncated %s at offset %zu", what, r->pos);
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
      snprintf(r->err, r->errcap, "classic project: impossible %s count %u at offset %zu", what, count, r->pos);
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

static int read_compressed_settings(const uint8_t *compressed, uint32_t compressed_size,
                                    GmlcClassicSettings *out, char *err, size_t errcap){
  if(!compressed_size) return 1;
  if(compressed_size>INT_MAX){
    if(err&&errcap) snprintf(err,errcap,"classic project: compressed settings are too large");
    return 0;
  }
  int raw_size=0;
  char *raw=stbi_zlib_decode_malloc((const char*)compressed,(int)compressed_size,&raw_size);
  if(!raw || raw_size<0){
    if(err&&errcap) snprintf(err,errcap,"classic project: invalid compressed settings");
    STBI_FREE(raw);
    return 0;
  }
  ClassicReader settings={(const uint8_t*)raw,(size_t)raw_size,0,err,errcap};
  int ok=read_settings_prefix(&settings,out);
  STBI_FREE(raw);
  return ok;
}

static int reader_doubles(ClassicReader *r, uint32_t count, const char *what){
  if(count > (r->size - r->pos) / 8) return reader_fail(r, what);
  return reader_skip(r, (size_t)count * 8, what);
}

static int require_payload_end(ClassicReader *r, const char *what){
  if(r->pos == r->size) return 1;
  if(r->err && r->errcap)
    snprintf(r->err, r->errcap, "classic project: %s has %zu unexplained trailing bytes",
             what, r->size - r->pos);
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

static int validate_sprite_payload(ClassicReader *r){
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
  return reader_words(r, 8, "sprite collision fields");
}

static int validate_background_payload(ClassicReader *r){
  uint32_t image_version, width, height;
  if(!reader_words(r, 7, "background tile fields") ||
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

static int validate_path_payload(ClassicReader *r){
  uint32_t points;
  if(!reader_words(r, 6, "path fields") || !reader_u32(r, &points, "path point count")) return 0;
  return reader_doubles(r, points > UINT32_MAX / 3 ? UINT32_MAX : points * 3, "path points");
}

static int validate_font_payload(ClassicReader *r){
  return reader_string(r, "font face") && reader_words(r, 5, "font fields");
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
  ClassicReader body={(const uint8_t*)normalized,working_size,0,NULL,0};
  if(!reader_u32(&body,&exists,"room exists") || !reader_string(&body,"room name") ||
     !reader_u32(&body,&version,"room version") || !validate_room_gameplay_payload(&body)){
    free(normalized);
    return 0;
  }
  size_t normalized_size=body.pos+56;
  char *complete=(char*)realloc(normalized,normalized_size);
  if(!complete){ free(normalized); return 0; }
  normalized=complete;
  memset(normalized+body.pos,0,56);
  STBI_FREE(*raw_io);
  *raw_io=normalized;
  *raw_size_io=(int)normalized_size;
  return 1;
}

static int read_file(const char *path, uint8_t **data, size_t *size,
                     char *err, size_t errcap){
  *data = NULL;
  *size = 0;
  FILE *f = fopen(path, "rb");
  if(!f){
    if(err && errcap) snprintf(err, errcap, "classic project: cannot open %s: %s", path, strerror(errno));
    return 0;
  }
  if(fseek(f, 0, SEEK_END) || ftell(f) < 0){
    if(err && errcap) snprintf(err, errcap, "classic project: cannot size %s", path);
    fclose(f);
    return 0;
  }
  long length = ftell(f);
  if(fseek(f, 0, SEEK_SET) || (unsigned long)length > SIZE_MAX){
    if(err && errcap) snprintf(err, errcap, "classic project: invalid size for %s", path);
    fclose(f);
    return 0;
  }
  uint8_t *bytes = (uint8_t*)malloc(length ? (size_t)length : 1);
  if(!bytes){
    if(err && errcap) snprintf(err, errcap, "classic project: out of memory reading %s", path);
    fclose(f);
    return 0;
  }
  size_t got = fread(bytes, 1, (size_t)length, f);
  int ok = got == (size_t)length && !ferror(f);
  fclose(f);
  if(!ok){
    if(err && errcap) snprintf(err, errcap, "classic project: cannot read %s", path);
    free(bytes);
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
    if(err && errcap) snprintf(err, errcap, "classic project: truncated common header (%zu bytes)", size);
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
      if(err && errcap) snprintf(err, errcap, "classic project: truncated common header (%zu bytes)", size);
      return 0;
    }
    out->game_id = read_u32le(p + 8);
    memcpy(out->guid, p + 12, sizeof(out->guid));
  }
  return 1;
}



int gmlc_classic_probe_file(const char *path, GmlcClassicHeader *out,
                            char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!path || !out){
    if(err && errcap) snprintf(err, errcap, "classic project: invalid file probe arguments");
    return 0;
  }
  FILE *f = fopen(path, "rb");
  if(!f){
    if(err && errcap) snprintf(err, errcap, "classic project: cannot open %s: %s", path, strerror(errno));
    return 0;
  }
  uint8_t header[28];
  size_t got = fread(header, 1, sizeof(header), f);
  int io_error = ferror(f);
  fclose(f);
  if(io_error){
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
                                uint32_t *settings_version, GmlcClassicSettings *settings){
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
  for(uint32_t i = 0; i < constants; ++i)
    if(!reader_string(r, "constant name") || !reader_string(r, "constant value")) return 0;
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
    case GMLC_CLASSIC_PATH: valid = validate_path_payload(r); break;
    case GMLC_CLASSIC_SCRIPT:
      valid = reader_string_copy(r, &slot->source, "legacy script source"); break;
    case GMLC_CLASSIC_FONT: valid = validate_font_payload(r); break;
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
                           &inventory->settings)){
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
    ? stbi_zlib_decode_noheader_malloc((const char*)compressed+2,(int)compressed_size-6,&raw_size)
    : stbi_zlib_decode_malloc((const char*)compressed, (int)compressed_size, &raw_size);
  if(!raw || raw_size < 4){
    if(err && errcap) snprintf(err, errcap, "classic project: invalid compressed resource block");
    STBI_FREE(raw);
    return 0;
  }
  if(raw_deflate){
    char *p=(char*)realloc(raw,(size_t)raw_size+1);
    if(!p){ STBI_FREE(raw); return 0; }
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
        if(!repair_executable_object(&raw,&raw_size)){ STBI_FREE(raw); return 0; }
      }
    } else if(type==GMLC_CLASSIC_ROOM){
      raw_size--;
      if(!normalize_executable_room(&raw,&raw_size)){
        if(err && errcap) snprintf(err,errcap,"classic executable: invalid compact room block");
        STBI_FREE(raw); return 0;
      }
    }
  }
  ClassicReader r = {(const uint8_t*)raw, (size_t)raw_size, 0, err, errcap};
  uint32_t exists;
  if(!reader_u32(&r, &exists, "resource existence flag")){
    STBI_FREE(raw);
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
    STBI_FREE(raw);
    return 0;
  }
  if(slot->exists) payload_start = r.pos;
  if(slot->exists && type == GMLC_CLASSIC_SCRIPT &&
     !reader_string_copy(&r, &slot->source, "script source")){
    free(slot->name);
    slot->name = NULL;
    STBI_FREE(raw);
    return 0;
  }
  int valid = 1;
  if(slot->exists){
    switch(type){
      case GMLC_CLASSIC_SOUND: valid = validate_sound_payload(&r); break;
      case GMLC_CLASSIC_SPRITE: valid = validate_sprite_payload(&r); break;
      case GMLC_CLASSIC_BACKGROUND: valid = validate_background_payload(&r); break;
      case GMLC_CLASSIC_PATH: valid = validate_path_payload(&r); break;
      case GMLC_CLASSIC_SCRIPT: break;
      case GMLC_CLASSIC_FONT: valid = validate_font_payload(&r); break;
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
    free(slot->name); slot->name = NULL;
    free(slot->source); slot->source = NULL;
    STBI_FREE(raw);
    return 0;
  }
  if(slot->exists && r.pos > payload_start){
    slot->payload_size = r.pos - payload_start;
    slot->payload = (uint8_t*)malloc(slot->payload_size);
    if(!slot->payload){
      if(err && errcap) snprintf(err, errcap, "classic project: out of memory retaining resource payload");
      free(slot->name); slot->name = NULL;
      free(slot->source); slot->source = NULL;
      STBI_FREE(raw);
      return 0;
    }
    memcpy(slot->payload, (const uint8_t*)raw + payload_start, slot->payload_size);
  }
  STBI_FREE(raw);
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
  free(manifest->room_order);
  memset(manifest, 0, sizeof(*manifest));
}

static int parse_modern_room_order(const void *data, size_t size,
                                   GmlcClassicManifest *manifest,
                                   char *err, size_t errcap){
  ClassicReader r = {(const uint8_t*)data, size, manifest->inventory.payload_end, err, errcap};
  uint32_t version, count;
  if(!reader_u32(&r, &version, "included-file section version") || version < 620 ||
     !reader_u32(&r, &count, "included-file count")) return 0;
  if(count > (r.size - r.pos) / 4) return reader_fail(&r, "included files");
  for(uint32_t i = 0; i < count; ++i)
    if(!reader_blob(&r, "included file")) return 0;
  if(!reader_u32(&r, &version, "extension section version") || version < 700 ||
     !reader_u32(&r, &count, "extension count")) return 0;
  if(count > (r.size - r.pos) / 4) return reader_fail(&r, "extensions");
  for(uint32_t i = 0; i < count; ++i)
    if(!reader_string(&r, "extension name")) return 0;
  if(!reader_u32(&r, &version, "game-information version") || version < 600 ||
     !reader_blob(&r, "game information") ||
     !reader_u32(&r, &version, "library-code section version") || version < 500 ||
     !reader_u32(&r, &count, "library-code count")) return 0;
  if(count > (r.size - r.pos) / 4) return reader_fail(&r, "library creation code");
  for(uint32_t i = 0; i < count; ++i)
    if(!reader_string(&r, "library creation code")) return 0;
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
  if(count){
    if(err && errcap) snprintf(err,errcap,"classic executable: extensions are not supported yet");
    return 0;
  }
  if(!reader_u32(&r,&section_version,"executable trigger version") || section_version<800 ||
     !reader_u32(&r,&out->inventory.trigger_slots,"executable trigger count")) return 0;
  for(uint32_t i=0;i<out->inventory.trigger_slots;i++) if(!reader_blob(&r,"executable trigger")) return 0;
  if(!reader_u32(&r,&section_version,"executable constant version") || section_version<800 ||
     !reader_u32(&r,&out->inventory.constants,"executable constant count")) return 0;
  for(uint32_t i=0;i<out->inventory.constants;i++)
    if(!reader_string(&r,"executable constant name") || !reader_string(&r,"executable constant value")) return 0;
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
  for(uint32_t i=0;i<count;i++) if(!reader_blob(&r,"executable include")) return 0;
  if(!reader_u32(&r,&section_version,"executable help version") || !reader_blob(&r,"executable help")) return 0;
  if(!reader_u32(&r,&section_version,"executable library version") ||
     !reader_u32(&r,&count,"executable library count")) return 0;
  for(uint32_t i=0;i<count;i++) if(!reader_string(&r,"executable library code")) return 0;
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
  if(size>=2 && ((const uint8_t*)data)[0]=='M' && ((const uint8_t*)data)[1]=='Z')
    return parse_executable_manifest((const uint8_t*)data,size,out,err,errcap);
  GmlcClassicHeader header;
  if(!gmlc_classic_probe(data, size, &header, err, errcap)) return 0;
  if(header.version == GMLC_CLASSIC_GM6 || header.version == GMLC_CLASSIC_GM7 ||
     header.version == GMLC_CLASSIC_GM7_ALT)
    return parse_legacy_project(data, size, &out->inventory, out, err, errcap);
  if(!gmlc_classic_inventory(data, size, &out->inventory, err, errcap)) return 0;
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
     !parse_modern_room_order(data, size, out, err, errcap)){
    gmlc_classic_manifest_free(out);
    return 0;
  }
  return 1;
}

int gmlc_classic_manifest_file(const char *path, GmlcClassicManifest *out,
                               char *err, size_t errcap){
  uint8_t *data;
  size_t size;
  if(!read_file(path, &data, &size, err, errcap)) return 0;
  int ok = gmlc_classic_manifest(data, size, out, err, errcap);
  free(data);
  return ok;
}

int gmlc_classic_inventory_file(const char *path, GmlcClassicInventory *out,
                                char *err, size_t errcap){
  uint8_t *data;
  size_t size;
  if(!read_file(path, &data, &size, err, errcap)) return 0;
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
