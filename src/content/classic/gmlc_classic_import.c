/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#include "gmlc_classic_import.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const uint8_t *data;
  size_t size, pos;
  char *err;
  size_t errcap;
} ImportReader;

static uint32_t import_u32_at(const uint8_t *p){
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int import_u32(ImportReader *r, uint32_t *value, const char *what){
  if(r->pos > r->size || r->size - r->pos < 4){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s", what);
    return 0;
  }
  *value = import_u32_at(r->data + r->pos);
  r->pos += 4;
  return 1;
}

static int import_blob(ImportReader *r, const uint8_t **data, uint32_t *size, const char *what){
  if(!import_u32(r, size, what)) return 0;
  if(r->pos > r->size || *size > r->size - r->pos){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s data", what);
    return 0;
  }
  *data = r->data + r->pos;
  r->pos += *size;
  return 1;
}

static int import_skip_string(ImportReader *r, const uint8_t **text, uint32_t *length, const char *what){
  if(!import_u32(r, length, what)) return 0;
  if(r->pos > r->size || *length > r->size - r->pos){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s", what);
    return 0;
  }
  if(text) *text = r->data + r->pos;
  r->pos += *length;
  return 1;
}

static int import_double(ImportReader *r, double *value, const char *what){
  if(r->pos > r->size || r->size - r->pos < 8){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: truncated %s", what);
    return 0;
  }
  uint64_t bits = (uint64_t)r->data[r->pos] | (uint64_t)r->data[r->pos + 1] << 8 |
                  (uint64_t)r->data[r->pos + 2] << 16 | (uint64_t)r->data[r->pos + 3] << 24 |
                  (uint64_t)r->data[r->pos + 4] << 32 | (uint64_t)r->data[r->pos + 5] << 40 |
                  (uint64_t)r->data[r->pos + 6] << 48 | (uint64_t)r->data[r->pos + 7] << 56;
  memcpy(value, &bits, sizeof(bits));
  r->pos += 8;
  return 1;
}

static char *copy_string(const char *text){
  size_t length = text ? strlen(text) : 0;
  char *copy = (char*)malloc(length + 1);
  if(!copy) return NULL;
  if(length) memcpy(copy, text, length);
  copy[length] = '\0';
  return copy;
}

static char *cache_path(const char *dir, const char *leaf){
  size_t nd = strlen(dir), nl = strlen(leaf);
  int separator = nd && dir[nd - 1] != '/' && dir[nd - 1] != '\\';
  char *path = (char*)malloc(nd + (size_t)separator + nl + 1);
  if(!path) return NULL;
  memcpy(path, dir, nd);
  size_t at = nd;
  if(separator) path[at++] = '/';
  memcpy(path + at, leaf, nl + 1);
  return path;
}

static int write_source(const char *path, const char *source, char *err, size_t errcap){
  FILE *file = fopen(path, "wb");
  if(!file){
    if(err && errcap) snprintf(err, errcap, "classic import: cannot create %s: %s", path, strerror(errno));
    return 0;
  }
  size_t length = source ? strlen(source) : 0;
  int wrote = !length || fwrite(source, 1, length, file) == length;
  int closed = fclose(file) == 0;
  int ok = wrote && closed;
  if(!ok && err && errcap) snprintf(err, errcap, "classic import: cannot write %s", path);
  return ok;
}

static void free_imported_scripts(GmlcProject *project){
  for(int i = 0; i < project->n_scripts; ++i){
    free(project->scripts[i].id);
    free(project->scripts[i].name);
    free(project->scripts[i].source_path);
  }
  free(project->scripts);
  project->scripts = NULL;
  project->n_scripts = project->cap_scripts = 0;
  for(int i = 0; i < project->n_script_order; ++i) free(project->script_order_ids[i]);
  free(project->script_order_ids);
  project->script_order_ids = NULL;
  project->n_script_order = 0;
}

int gmlc_classic_import_scripts(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->scripts || project->n_scripts){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid script-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_SCRIPT];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many script slots");
    return 0;
  }
  project->scripts = (GmlcScript*)calloc(count ? count : 1, sizeof(*project->scripts));
  project->script_order_ids = (char**)calloc(count ? count : 1, sizeof(*project->script_order_ids));
  if(!project->scripts || !project->script_order_ids){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating scripts");
    free_imported_scripts(project);
    return 0;
  }
  project->n_scripts = project->cap_scripts = (int)count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_SCRIPT];
  for(uint32_t i = 0; i < count; ++i){
    char fallback[64], leaf[64];
    snprintf(fallback, sizeof(fallback), "__classic_missing_script_%u", i);
    snprintf(leaf, sizeof(leaf), "classic_script_%06u.gml", i);
    const char *name = slots && slots[i].exists && slots[i].name ? slots[i].name : fallback;
    const char *source = slots && slots[i].exists && slots[i].source ? slots[i].source : "exit;\n";
    GmlcScript *script = &project->scripts[i];
    script->id = copy_string(name);
    script->name = copy_string(name);
    script->source_path = cache_path(cache_dir, leaf);
    if(!script->id || !script->name || !script->source_path ||
       !write_source(script->source_path, source, err, errcap)){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: out of memory importing script %u", i);
      free_imported_scripts(project);
      return 0;
    }
    if(slots && slots[i].exists){
      project->script_order_ids[project->n_script_order] = copy_string(name);
      if(!project->script_order_ids[project->n_script_order]){
        if(err && errcap) snprintf(err, errcap, "classic import: out of memory recording script order");
        free_imported_scripts(project);
        return 0;
      }
      ++project->n_script_order;
    }
  }
  return 1;
}

static void free_imported_sprites(GmlcProject *project){
  for(int i = 0; i < project->n_sprites; ++i){
    GmlcSprite *sprite = &project->sprites[i];
    free(sprite->id);
    free(sprite->name);
    for(int frame = 0; frame < sprite->n_frames; ++frame)
      free(sprite->frame_paths ? sprite->frame_paths[frame] : NULL);
    free(sprite->frame_paths);
  }
  free(project->sprites);
  project->sprites = NULL;
  project->n_sprites = project->cap_sprites = 0;
}

static int write_bgra_png(const char *path, const uint8_t *bgra, uint32_t bytes,
                          int width, int height, char *err, size_t errcap){
  int out_width = width > 0 ? width : 1;
  int out_height = height > 0 ? height : 1;
  if((size_t)out_width > SIZE_MAX / (size_t)out_height / 4u){
    if(err && errcap) snprintf(err, errcap, "classic import: sprite dimensions overflow");
    return 0;
  }
  size_t pixels = (size_t)out_width * (size_t)out_height;
  if(width > 0 && height > 0 && bytes < pixels * 4u){
    if(err && errcap) snprintf(err, errcap, "classic import: truncated sprite BGRA pixels");
    return 0;
  }
  uint8_t *rgba = (uint8_t*)calloc(pixels, 4);
  if(!rgba){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory converting sprite pixels");
    return 0;
  }
  if(width > 0 && height > 0){
    for(size_t i = 0; i < pixels; ++i){
      rgba[i * 4] = bgra[i * 4 + 2];
      rgba[i * 4 + 1] = bgra[i * 4 + 1];
      rgba[i * 4 + 2] = bgra[i * 4];
      rgba[i * 4 + 3] = bgra[i * 4 + 3];
    }
  }
  int ok = stbi_write_png(path, out_width, out_height, 4, rgba, out_width * 4);
  free(rgba);
  if(!ok && err && errcap) snprintf(err, errcap, "classic import: cannot write %s", path);
  return ok != 0;
}

int gmlc_classic_import_sprites(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->sprites || project->n_sprites){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid sprite-import arguments");
    return 0;
  }
  uint32_t slots_count = classic->inventory.resource_slots[GMLC_CLASSIC_SPRITE];
  uint32_t existing = classic->existing[GMLC_CLASSIC_SPRITE];
  if(existing > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many sprites");
    return 0;
  }
  project->sprites = (GmlcSprite*)calloc(existing ? existing : 1, sizeof(*project->sprites));
  if(!project->sprites){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating sprites");
    return 0;
  }
  project->cap_sprites = (int)existing;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_SPRITE];
  for(uint32_t slot_index = 0; slot_index < slots_count; ++slot_index){
    const GmlcClassicResourceSlot *source = &slots[slot_index];
    if(!source->exists) continue;
    if(source->legacy_layout){
      if(err && errcap) snprintf(err, errcap, "classic import: legacy sprite pixel conversion is not implemented yet");
      free_imported_sprites(project);
      return 0;
    }
    GmlcSprite *sprite = &project->sprites[project->n_sprites++];
    sprite->id = copy_string(source->name);
    sprite->name = copy_string(source->name);
    sprite->runtime_id = (int)slot_index;
    ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
    uint32_t xorigin, yorigin, frames;
    if(!sprite->id || !sprite->name || !import_u32(&r, &xorigin, "sprite x origin") ||
       !import_u32(&r, &yorigin, "sprite y origin") || !import_u32(&r, &frames, "sprite frame count") ||
       frames > INT32_MAX){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: invalid sprite %u", slot_index);
      free_imported_sprites(project);
      return 0;
    }
    sprite->xorig = (int32_t)xorigin;
    sprite->yorig = (int32_t)yorigin;
    sprite->n_frames = (int)frames;
    sprite->frame_paths = (char**)calloc(frames ? frames : 1, sizeof(*sprite->frame_paths));
    if(!sprite->frame_paths){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating sprite frames");
      free_imported_sprites(project);
      return 0;
    }
    for(uint32_t frame = 0; frame < frames; ++frame){
      uint32_t frame_version, width, height, pixel_bytes = 0;
      const uint8_t *pixels = NULL;
      if(!import_u32(&r, &frame_version, "sprite frame version") ||
         !import_u32(&r, &width, "sprite width") || !import_u32(&r, &height, "sprite height") ||
         (width && height && !import_blob(&r, &pixels, &pixel_bytes, "sprite pixels")) ||
         width > INT32_MAX || height > INT32_MAX){
        free_imported_sprites(project);
        return 0;
      }
      (void)frame_version;
      if(frame == 0){ sprite->width = (int)width; sprite->height = (int)height; }
      if((int)width != sprite->width || (int)height != sprite->height){
        if(err && errcap) snprintf(err, errcap, "classic import: inconsistent dimensions in sprite slot %u", slot_index);
        free_imported_sprites(project);
        return 0;
      }
      char leaf[96];
      snprintf(leaf, sizeof(leaf), "classic_sprite_%06u_%06u.png", slot_index, frame);
      sprite->frame_paths[frame] = cache_path(cache_dir, leaf);
      if(!sprite->frame_paths[frame] ||
         !write_bgra_png(sprite->frame_paths[frame], pixels, pixel_bytes, (int)width, (int)height, err, errcap)){
        free_imported_sprites(project);
        return 0;
      }
    }
    uint32_t collision[8];
    for(int i = 0; i < 8; ++i)
      if(!import_u32(&r, &collision[i], "sprite collision field")){ free_imported_sprites(project); return 0; }
    if(r.pos != r.size){
      if(err && errcap) snprintf(err, errcap, "classic import: trailing sprite payload");
      free_imported_sprites(project);
      return 0;
    }
    sprite->col_kind = (int32_t)collision[0];
    sprite->col_tolerance = (int32_t)collision[1];
    sprite->sep_masks = collision[2] != 0;
    sprite->bbox_mode = (int32_t)collision[3];
    sprite->bbox_left = (int32_t)collision[4];
    sprite->bbox_right = (int32_t)collision[5];
    sprite->bbox_bottom = (int32_t)collision[6];
    sprite->bbox_top = (int32_t)collision[7];
  }
  return 1;
}

static int write_binary(const char *path, const uint8_t *data, size_t size,
                        char *err, size_t errcap){
  FILE *file = fopen(path, "wb");
  if(!file){
    if(err && errcap) snprintf(err, errcap, "classic import: cannot create %s: %s", path, strerror(errno));
    return 0;
  }
  int wrote = !size || fwrite(data, 1, size, file) == size;
  int closed = fclose(file) == 0;
  if((!wrote || !closed) && err && errcap) snprintf(err, errcap, "classic import: cannot write %s", path);
  return wrote && closed;
}

static void free_imported_sounds(GmlcProject *project){
  for(int i = 0; i < project->n_sounds; ++i){
    free(project->sounds[i].id);
    free(project->sounds[i].name);
    free(project->sounds[i].data_path);
  }
  free(project->sounds);
  project->sounds = NULL;
  project->n_sounds = project->cap_sounds = 0;
}

int gmlc_classic_import_sounds(const GmlcClassicManifest *classic,
                               GmlcProject *project, const char *cache_dir,
                               char *err, size_t errcap){
  static const uint8_t silent_wav[44] = {
    'R','I','F','F',36,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
    1,0,1,0,0x44,0xAC,0,0,0x88,0x58,1,0,2,0,16,0,'d','a','t','a',0,0,0,0
  };
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->sounds || project->n_sounds){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid sound-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_SOUND];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many sound slots");
    return 0;
  }
  project->sounds = (GmlcSound*)calloc(count ? count : 1, sizeof(*project->sounds));
  if(!project->sounds){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating sounds");
    return 0;
  }
  project->n_sounds = project->cap_sounds = (int)count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_SOUND];
  for(uint32_t i = 0; i < count; ++i){
    const GmlcClassicResourceSlot *source = &slots[i];
    char fallback[64], leaf[96];
    snprintf(fallback, sizeof(fallback), "__classic_missing_sound_%u", i);
    const char *name = source->exists && source->name ? source->name : fallback;
    const uint8_t *audio = silent_wav;
    size_t audio_size = sizeof(silent_wav);
    char extension_buffer[12];
    const char *extension = ".wav";
    double volume = 1.0;
    if(source->exists){
      if(source->legacy_layout){
        if(err && errcap) snprintf(err, errcap, "classic import: legacy sound decompression is not implemented yet");
        free_imported_sounds(project);
        return 0;
      }
      ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
      uint32_t kind, type_length, filename_length, has_data, blob_size = 0, ignored;
      const uint8_t *type_text = NULL, *filename_text = NULL, *blob = NULL;
      double pan;
      if(!import_u32(&r, &kind, "sound kind") ||
         !import_skip_string(&r, &type_text, &type_length, "sound file type") ||
         !import_skip_string(&r, &filename_text, &filename_length, "sound filename") ||
         !import_u32(&r, &has_data, "sound data flag") ||
         (has_data && !import_blob(&r, &blob, &blob_size, "sound data")) ||
         !import_u32(&r, &ignored, "sound effects") || !import_double(&r, &volume, "sound volume") ||
         !import_double(&r, &pan, "sound pan") || !import_u32(&r, &ignored, "sound preload") ||
         r.pos != r.size){
        free_imported_sounds(project);
        return 0;
      }
      (void)kind; (void)filename_text; (void)filename_length; (void)pan;
      if(has_data){ audio = blob; audio_size = blob_size; }
      if(type_length > 1 && type_length < 12 && type_text[0] == '.'){
        int safe = 1;
        for(uint32_t c = 1; c < type_length; ++c)
          if(!((type_text[c] >= 'a' && type_text[c] <= 'z') ||
               (type_text[c] >= 'A' && type_text[c] <= 'Z') ||
               (type_text[c] >= '0' && type_text[c] <= '9'))) safe = 0;
        if(safe){
          memcpy(extension_buffer, type_text, type_length); extension_buffer[type_length] = '\0';
          extension = extension_buffer;
        }
      }
    }
    snprintf(leaf, sizeof(leaf), "classic_sound_%06u%s", i, extension);
    GmlcSound *sound = &project->sounds[i];
    sound->id = copy_string(name);
    sound->name = copy_string(name);
    sound->data_path = cache_path(cache_dir, leaf);
    sound->volume = (float)volume;
    sound->pitch = 1.0f;
    if(!sound->id || !sound->name || !sound->data_path ||
       !write_binary(sound->data_path, audio, audio_size, err, errcap)){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: out of memory importing sound %u", i);
      free_imported_sounds(project);
      return 0;
    }
  }
  return 1;
}
