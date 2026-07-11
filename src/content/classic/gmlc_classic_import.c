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

static int import_copy_string(ImportReader *r, char **text, const char *what){
  const uint8_t *bytes;
  uint32_t length;
  *text = NULL;
  if(!import_skip_string(r, &bytes, &length, what)) return 0;
  char *copy = (char*)malloc((size_t)length + 1);
  if(!copy){
    if(r->err && r->errcap) snprintf(r->err, r->errcap, "classic import: out of memory reading %s", what);
    return 0;
  }
  memcpy(copy, bytes, length);
  copy[length] = '\0';
  *text = copy;
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

static void free_sprite_range(GmlcProject *project, int first){
  for(int i = first; i < project->n_sprites; ++i){
    GmlcSprite *sprite = &project->sprites[i];
    free(sprite->id); free(sprite->name);
    for(int frame = 0; frame < sprite->n_frames; ++frame)
      free(sprite->frame_paths ? sprite->frame_paths[frame] : NULL);
    free(sprite->frame_paths);
  }
  project->n_sprites = first;
}

static void free_imported_backgrounds(GmlcProject *project, int first_sprite){
  free_sprite_range(project, first_sprite);
  for(int i = 0; i < project->n_tilesets; ++i){
    free(project->tilesets[i].id);
    free(project->tilesets[i].name);
  }
  free(project->tilesets);
  project->tilesets = NULL;
  project->n_tilesets = project->cap_tilesets = 0;
}

int gmlc_classic_import_backgrounds(const GmlcClassicManifest *classic,
                                    GmlcProject *project, const char *cache_dir,
                                    char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->tilesets || project->n_tilesets){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid background-import arguments");
    return 0;
  }
  uint32_t slot_count = classic->inventory.resource_slots[GMLC_CLASSIC_BACKGROUND];
  uint32_t existing = classic->existing[GMLC_CLASSIC_BACKGROUND];
  if(slot_count > INT32_MAX || existing > INT32_MAX ||
     project->n_sprites > INT32_MAX - (int)existing){
    if(err && errcap) snprintf(err, errcap, "classic import: too many backgrounds");
    return 0;
  }
  int first_sprite = project->n_sprites;
  int needed = first_sprite + (int)existing;
  GmlcSprite *sprites = (GmlcSprite*)realloc(project->sprites,
                                              (size_t)(needed ? needed : 1) * sizeof(*sprites));
  if(!sprites){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating background images");
    return 0;
  }
  project->sprites = sprites;
  if(needed > first_sprite)
    memset(project->sprites + first_sprite, 0, (size_t)(needed - first_sprite) * sizeof(*sprites));
  project->cap_sprites = needed;
  project->tilesets = (GmlcTileset*)calloc(slot_count ? slot_count : 1, sizeof(*project->tilesets));
  if(!project->tilesets){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating backgrounds");
    return 0;
  }
  project->n_tilesets = project->cap_tilesets = (int)slot_count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_BACKGROUND];
  for(uint32_t i = 0; i < slot_count; ++i){
    char fallback[64];
    snprintf(fallback, sizeof(fallback), "__classic_missing_background_%u", i);
    const char *name = slots[i].exists && slots[i].name ? slots[i].name : fallback;
    GmlcTileset *background = &project->tilesets[i];
    background->id = copy_string(name);
    background->name = copy_string(name);
    background->sprite_id = -1;
    background->tile_width = background->tile_height = 16;
    if(!background->id || !background->name){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory naming background %u", i);
      free_imported_backgrounds(project, first_sprite);
      return 0;
    }
    if(!slots[i].exists) continue;
    if(slots[i].legacy_layout){
      if(err && errcap) snprintf(err, errcap, "classic import: legacy background pixel conversion is not implemented yet");
      free_imported_backgrounds(project, first_sprite);
      return 0;
    }
    ImportReader r = {slots[i].payload, slots[i].payload_size, 0, err, errcap};
    uint32_t fields[7], image_version, width, height, pixel_bytes = 0;
    const uint8_t *pixels = NULL;
    for(int field = 0; field < 7; ++field)
      if(!import_u32(&r, &fields[field], "background tile field")){
        free_imported_backgrounds(project, first_sprite); return 0;
      }
    if(!import_u32(&r, &image_version, "background image version") ||
       !import_u32(&r, &width, "background width") || !import_u32(&r, &height, "background height") ||
       (width && height && !import_blob(&r, &pixels, &pixel_bytes, "background pixels")) ||
       width > INT32_MAX || height > INT32_MAX || r.pos != r.size){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: invalid background %u", i);
      free_imported_backgrounds(project, first_sprite);
      return 0;
    }
    (void)image_version;
    GmlcSprite *sprite = &project->sprites[project->n_sprites++];
    sprite->id = copy_string(name); sprite->name = copy_string(name);
    sprite->runtime_id = -1; sprite->tileset_source = 1;
    sprite->width = (int)width; sprite->height = (int)height;
    sprite->bbox_right = width ? (int)width - 1 : 0;
    sprite->bbox_bottom = height ? (int)height - 1 : 0;
    sprite->n_frames = 1;
    sprite->frame_paths = (char**)calloc(1, sizeof(*sprite->frame_paths));
    char leaf[80];
    snprintf(leaf, sizeof(leaf), "classic_background_%06u.png", i);
    if(sprite->frame_paths) sprite->frame_paths[0] = cache_path(cache_dir, leaf);
    if(!sprite->id || !sprite->name || !sprite->frame_paths || !sprite->frame_paths[0] ||
       !write_bgra_png(sprite->frame_paths[0], pixels, pixel_bytes, (int)width, (int)height, err, errcap)){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: out of memory importing background %u", i);
      free_imported_backgrounds(project, first_sprite);
      return 0;
    }
    background->sprite_id = project->n_sprites - 1;
    background->sprite_no_export = fields[0] ? 0 : 1;
    background->tile_width = (int32_t)fields[1];
    background->tile_height = (int32_t)fields[2];
    background->border_x = (int32_t)fields[3];
    background->border_y = (int32_t)fields[4];
    int step_x = background->tile_width + (int32_t)fields[5];
    int step_y = background->tile_height + (int32_t)fields[6];
    background->columns = step_x > 0 && (int)width > background->border_x
      ? ((int)width - background->border_x + (int32_t)fields[5]) / step_x : 1;
    int rows = step_y > 0 && (int)height > background->border_y
      ? ((int)height - background->border_y + (int32_t)fields[6]) / step_y : 1;
    if(background->columns < 1) background->columns = 1;
    if(rows < 1) rows = 1;
    int64_t tile_count = (int64_t)background->columns * (int64_t)rows + 1;
    background->tile_count = tile_count > INT32_MAX ? INT32_MAX : (int)tile_count;
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

static void free_imported_paths(GmlcProject *project){
  for(int i = 0; i < project->n_paths; ++i){
    free(project->paths[i].id);
    free(project->paths[i].name);
    free(project->paths[i].points);
  }
  free(project->paths);
  project->paths = NULL;
  project->n_paths = project->cap_paths = 0;
}

int gmlc_classic_import_paths(const GmlcClassicManifest *classic,
                              GmlcProject *project, char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || project->paths || project->n_paths){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid path-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_PATH];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many path slots");
    return 0;
  }
  project->paths = (GmlcPath*)calloc(count ? count : 1, sizeof(*project->paths));
  if(!project->paths){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating paths");
    return 0;
  }
  project->n_paths = project->cap_paths = (int)count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_PATH];
  for(uint32_t i = 0; i < count; ++i){
    const GmlcClassicResourceSlot *source = &slots[i];
    char fallback[64];
    snprintf(fallback, sizeof(fallback), "__classic_missing_path_%u", i);
    const char *name = source->exists && source->name ? source->name : fallback;
    GmlcPath *path = &project->paths[i];
    path->id = copy_string(name);
    path->name = copy_string(name);
    if(!path->id || !path->name){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory importing path %u", i);
      free_imported_paths(project);
      return 0;
    }
    if(!source->exists) continue;
    ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
    uint32_t kind, closed, precision, ignored, points;
    if(!import_u32(&r, &kind, "path kind") || !import_u32(&r, &closed, "path closed flag") ||
       !import_u32(&r, &precision, "path precision") ||
       !import_u32(&r, &ignored, "path editor room") || !import_u32(&r, &ignored, "path snap x") ||
       !import_u32(&r, &ignored, "path snap y") || !import_u32(&r, &points, "path point count") ||
       points > INT32_MAX || (size_t)points > (r.size - r.pos) / 24u){
      free_imported_paths(project);
      return 0;
    }
    path->kind = (int32_t)kind;
    path->closed = closed != 0;
    path->precision = (int32_t)precision;
    path->n_points = (int)points;
    path->points = (GmlcPathPoint*)calloc(points ? points : 1, sizeof(*path->points));
    if(!path->points){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating path points");
      free_imported_paths(project);
      return 0;
    }
    for(uint32_t point = 0; point < points; ++point){
      double x, y, speed;
      if(!import_double(&r, &x, "path point x") || !import_double(&r, &y, "path point y") ||
         !import_double(&r, &speed, "path point speed")){
        free_imported_paths(project);
        return 0;
      }
      path->points[point].x = (float)x;
      path->points[point].y = (float)y;
      path->points[point].speed = (float)speed;
    }
    if(r.pos != r.size){
      if(err && errcap) snprintf(err, errcap, "classic import: trailing path payload");
      free_imported_paths(project);
      return 0;
    }
  }
  return 1;
}

typedef struct {
  char *data;
  size_t length, capacity;
} ImportText;

static int text_reserve(ImportText *text, size_t extra){
  if(extra > SIZE_MAX - text->length - 1) return 0;
  size_t need = text->length + extra + 1;
  if(need <= text->capacity) return 1;
  size_t capacity = text->capacity ? text->capacity : 256;
  while(capacity < need){
    if(capacity > SIZE_MAX / 2){ capacity = need; break; }
    capacity *= 2;
  }
  char *data = (char*)realloc(text->data, capacity);
  if(!data) return 0;
  text->data = data;
  text->capacity = capacity;
  return 1;
}

static int text_append_n(ImportText *text, const char *value, size_t length){
  if(!text_reserve(text, length)) return 0;
  memcpy(text->data + text->length, value, length);
  text->length += length;
  text->data[text->length] = '\0';
  return 1;
}

static int text_append(ImportText *text, const char *value){
  return text_append_n(text, value ? value : "", value ? strlen(value) : 0);
}

static int text_append_int(ImportText *text, int32_t value){
  char number[32];
  snprintf(number, sizeof(number), "%d", value);
  return text_append(text, number);
}

static int text_append_quoted(ImportText *text, const char *value){
  if(!text_append(text, "\"")) return 0;
  for(const unsigned char *p = (const unsigned char*)(value ? value : ""); *p; ++p){
    char escaped[2] = {(char)*p, '\0'};
    if(*p == '\\' || *p == '"'){
      if(!text_append(text, "\\")) return 0;
    } else if(*p == '\n'){
      if(!text_append(text, "\\n")) return 0;
      continue;
    } else if(*p == '\r'){
      if(!text_append(text, "\\r")) return 0;
      continue;
    }
    if(!text_append(text, escaped)) return 0;
  }
  return text_append(text, "\"");
}

static int emit_action_call(ImportText *text, const char *function_name,
                            char **arguments, uint32_t *argument_kinds,
                            uint32_t used_arguments){
  if(!function_name || !*function_name) return text_append(text, "/* empty action */");
  if(!text_append(text, function_name) || !text_append(text, "(")) return 0;
  for(uint32_t i = 0; i < used_arguments; ++i){
    if(i && !text_append(text, ",")) return 0;
    if(argument_kinds && argument_kinds[i] == 1){
      if(!text_append_quoted(text, arguments[i])) return 0;
    } else if(!text_append(text, arguments[i] && *arguments[i] ? arguments[i] : "0")) return 0;
  }
  return text_append(text, ")");
}

static int import_actions(ImportReader *r, ImportText *text){
  uint32_t list_version, count;
  if(!import_u32(r, &list_version, "action-list version") || !import_u32(r, &count, "action count")) return 0;
  (void)list_version;
  for(uint32_t action_index = 0; action_index < count; ++action_index){
    uint32_t action_version = 0, library_id = 0, action_id = 0, kind = 0;
    uint32_t may_relative = 0, question = 0, applies = 0, type = 0;
    uint32_t used_arguments = 0, kind_count = 0, target = 0, relative = 0;
    uint32_t argument_count = 0, negate = 0;
    char *function_name = NULL, *code = NULL;
    uint32_t *argument_kinds = NULL;
    char **arguments = NULL;
    int ok = import_u32(r, &action_version, "action version") &&
      import_u32(r, &library_id, "action library") && import_u32(r, &action_id, "action id") &&
      import_u32(r, &kind, "action kind") && import_u32(r, &may_relative, "action relative capability") &&
      import_u32(r, &question, "action question flag") && import_u32(r, &applies, "action target flag") &&
      import_u32(r, &type, "action type") && import_copy_string(r, &function_name, "action function") &&
      import_copy_string(r, &code, "action code") && import_u32(r, &used_arguments, "used action arguments") &&
      import_u32(r, &kind_count, "action argument-kind count");
    if(!ok) goto action_done;
    if(kind_count > 1024 || used_arguments > kind_count){ ok = 0; goto action_done; }
    argument_kinds = (uint32_t*)calloc(kind_count ? kind_count : 1, sizeof(*argument_kinds));
    if(!argument_kinds){ ok = 0; goto action_done; }
    for(uint32_t i = 0; i < kind_count; ++i)
      if(!import_u32(r, &argument_kinds[i], "action argument kind")){ ok = 0; goto action_done; }
    if(!import_u32(r, &target, "action target") || !import_u32(r, &relative, "action relative flag") ||
       !import_u32(r, &argument_count, "action argument count") || argument_count > 1024){ ok = 0; goto action_done; }
    arguments = (char**)calloc(argument_count ? argument_count : 1, sizeof(*arguments));
    if(!arguments){ ok = 0; goto action_done; }
    for(uint32_t i = 0; i < argument_count; ++i)
      if(!import_copy_string(r, &arguments[i], "action argument")){ ok = 0; goto action_done; }
    if(!import_u32(r, &negate, "action negation flag")){ ok = 0; goto action_done; }
    (void)action_version; (void)library_id; (void)action_id; (void)may_relative;

    if(kind == 1) ok = text_append(text, "{\n");
    else if(kind == 2) ok = text_append(text, "}\n");
    else if(kind == 3) ok = text_append(text, "else\n");
    else if(kind == 4) ok = text_append(text, "exit;\n");
    else if(kind == 5){
      ok = text_append(text, "repeat (") && text_append(text, argument_count && arguments[0][0] ? arguments[0] : "0") &&
           text_append(text, ")\n");
    } else if(kind == 6){
      const char *lhs = argument_count > 0 && arguments[0][0] ? arguments[0] : "__classic_variable";
      const char *rhs = argument_count > 1 && arguments[1][0] ? arguments[1] : "0";
      ok = text_append(text, lhs) && text_append(text, relative ? " += " : " = ") && text_append(text, rhs) && text_append(text, ";\n");
    } else {
      int wrapped_target = applies && !question && (int32_t)target != -1;
      if(wrapped_target){
        ok = text_append(text, "with (") && text_append_int(text, (int32_t)target) && text_append(text, ") {\n");
      }
      if(ok && relative && !question) ok = text_append(text, "action_set_relative(1);\n");
      if(ok && question) ok = text_append(text, "if (") && (!negate || text_append(text, "!"));
      if(ok){
        if(type == 2 || kind == 7) ok = text_append(text, code && *code ? code : "/* empty code action */");
        else ok = emit_action_call(text, function_name, arguments, argument_kinds,
                                   used_arguments < argument_count ? used_arguments : argument_count);
      }
      if(ok && question) ok = text_append(text, ")\n");
      else if(ok) ok = text_append(text, ";\n");
      if(ok && relative && !question) ok = text_append(text, "action_set_relative(0);\n");
      if(ok && wrapped_target) ok = text_append(text, "}\n");
    }
action_done:
    for(uint32_t i = 0; arguments && i < argument_count; ++i) free(arguments[i]);
    free(arguments); free(argument_kinds); free(function_name); free(code);
    if(!ok){
      if(r->err && r->errcap && !r->err[0]) snprintf(r->err, r->errcap, "classic import: invalid action %u", action_index);
      return 0;
    }
  }
  return 1;
}

static void free_imported_objects(GmlcProject *project){
  for(int i = 0; i < project->n_objects; ++i){
    GmlcObject *object = &project->objects[i];
    free(object->id); free(object->name);
    for(int event = 0; event < object->n_events; ++event){
      free(object->events[event].id); free(object->events[event].collision_id);
      free(object->events[event].source_path);
    }
    free(object->events);
  }
  free(project->objects);
  project->objects = NULL;
  project->n_objects = project->cap_objects = 0;
}

static int append_object_event(GmlcObject *object, GmlcObjectEvent event){
  if(object->n_events >= object->cap_events){
    int capacity = object->cap_events ? object->cap_events * 2 : 4;
    GmlcObjectEvent *events = (GmlcObjectEvent*)realloc(object->events, (size_t)capacity * sizeof(*events));
    if(!events) return 0;
    object->events = events;
    object->cap_events = capacity;
  }
  object->events[object->n_events++] = event;
  return 1;
}

int gmlc_classic_import_objects(const GmlcClassicManifest *classic,
                                GmlcProject *project, const char *cache_dir,
                                char *err, size_t errcap){
  if(err && errcap) err[0] = '\0';
  if(!classic || !project || !cache_dir || !*cache_dir || project->objects || project->n_objects){
    if(err && errcap) snprintf(err, errcap, "classic import: invalid object-import arguments");
    return 0;
  }
  uint32_t count = classic->inventory.resource_slots[GMLC_CLASSIC_OBJECT];
  if(count > INT32_MAX){
    if(err && errcap) snprintf(err, errcap, "classic import: too many object slots");
    return 0;
  }
  project->objects = (GmlcObject*)calloc(count ? count : 1, sizeof(*project->objects));
  if(!project->objects){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory allocating objects");
    return 0;
  }
  project->n_objects = project->cap_objects = (int)count;
  const GmlcClassicResourceSlot *slots = classic->slots[GMLC_CLASSIC_OBJECT];
  for(uint32_t i = 0; i < count; ++i){
    char fallback[64];
    snprintf(fallback, sizeof(fallback), "__classic_missing_object_%u", i);
    const char *name = slots[i].exists && slots[i].name ? slots[i].name : fallback;
    project->objects[i].id = copy_string(name);
    project->objects[i].name = copy_string(name);
    project->objects[i].sprite_id = project->objects[i].mask_id = -1;
    project->objects[i].parent_id = -100;
    project->objects[i].visible = slots[i].exists ? 1 : 0;
    if(!project->objects[i].id || !project->objects[i].name){
      if(err && errcap) snprintf(err, errcap, "classic import: out of memory naming object %u", i);
      free_imported_objects(project);
      return 0;
    }
  }
  for(uint32_t i = 0; i < count; ++i){
    const GmlcClassicResourceSlot *source = &slots[i];
    if(!source->exists) continue;
    ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
    uint32_t sprite, solid, visible, depth, persistent, parent, mask, last_event_type;
    if(!import_u32(&r, &sprite, "object sprite") || !import_u32(&r, &solid, "object solid flag") ||
       !import_u32(&r, &visible, "object visible flag") || !import_u32(&r, &depth, "object depth") ||
       !import_u32(&r, &persistent, "object persistent flag") || !import_u32(&r, &parent, "object parent") ||
       !import_u32(&r, &mask, "object mask") || !import_u32(&r, &last_event_type, "object event-type count") ||
       last_event_type > 64){ free_imported_objects(project); return 0; }
    GmlcObject *object = &project->objects[i];
    object->sprite_id = (int32_t)sprite; object->solid = solid != 0; object->visible = visible != 0;
    object->depth = (int32_t)depth; object->persistent = persistent != 0;
    object->parent_id = (int32_t)parent; object->mask_id = (int32_t)mask;
    for(uint32_t event_type = 0; event_type <= last_event_type; ++event_type){
      for(;;){
        uint32_t event_number;
        if(!import_u32(&r, &event_number, "object event number")){ free_imported_objects(project); return 0; }
        if(event_number == UINT32_MAX) break;
        ImportText text = {0};
        if(!import_actions(&r, &text)){
          free(text.data); free_imported_objects(project); return 0;
        }
        if(!text.data && !text_append(&text, "exit;\n")){
          free_imported_objects(project); return 0;
        }
        char leaf[112], event_id[64];
        snprintf(leaf, sizeof(leaf), "classic_object_%06u_event_%02u_%010u.gml", i, event_type, event_number);
        snprintf(event_id, sizeof(event_id), "classic_event_%u_%u_%u", i, event_type, event_number);
        GmlcObjectEvent event;
        memset(&event, 0, sizeof(event));
        event.id = copy_string(event_id);
        event.event_type = (int)event_type;
        event.event_number = (int32_t)event_number;
        event.collision_object_id = event_type == 4 ? (int32_t)event_number : -1;
        event.source_path = cache_path(cache_dir, leaf);
        if(!event.id || !event.source_path || !write_source(event.source_path, text.data, err, errcap) ||
           !append_object_event(object, event)){
          free(event.id); free(event.source_path); free(text.data);
          if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: out of memory importing object event");
          free_imported_objects(project); return 0;
        }
        free(text.data);
      }
    }
    if(r.pos != r.size){
      if(err && errcap) snprintf(err, errcap, "classic import: trailing object payload");
      free_imported_objects(project); return 0;
    }
  }
  return 1;
}
