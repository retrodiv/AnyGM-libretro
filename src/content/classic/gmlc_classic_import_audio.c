/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic_import_internal.h"

#include "anygm_vfs.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_binary(const AnygmHostServices *host,const char *path,
                        const uint8_t *data,size_t size,
                        char *err, size_t errcap){
  int ok=anygm_vfs_write_all(host,path,data,size);
  if(!ok && err && errcap) snprintf(err,errcap,"classic import: cannot write %s",path);
  return ok;
}

static char *import_binary_path(GmlcProject *project, const char *cache_dir,
                                const char *leaf, const uint8_t *data, size_t size,
                                char *err, size_t errcap){
  if(project->prefer_memory_files){
    char *path=gmlc_project_add_memory_file(project,leaf,GMLC_MEMORY_BLOB,
                                            data,size,0,0);
    if(!path && err && errcap)
      snprintf(err,errcap,"classic import: out of memory retaining %s",leaf);
    return path;
  }
  char *path=cache_path(cache_dir,leaf);
  if(!path || !write_binary(project->host,path,data,size,err,errcap)){
    free(path);
    return NULL;
  }
  return path;
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
    if(!source->exists) continue;
    char leaf[96];
    const char *name = source->name ? source->name : "";
    const uint8_t *audio = silent_wav;
    size_t audio_size = sizeof(silent_wav);
    char *owned_audio = NULL;
    char extension_buffer[12];
    const char *extension = ".wav";
    double volume = 1.0;
    {
      ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
      uint32_t kind, type_length, filename_length=0, has_data=0, blob_size = 0, ignored;
      const uint8_t *type_text = NULL, *filename_text = NULL, *blob = NULL;
      double pan=0.0;
      int parsed=import_u32(&r,&kind,"sound kind") &&
                 import_skip_string(&r,&type_text,&type_length,"sound file type");
      if(parsed && source->version==440u){
        has_data=kind!=UINT32_MAX;
        parsed=(!has_data || import_blob(&r,&blob,&blob_size,"sound data")) &&
               import_u32(&r,&ignored,"sound legacy flag") &&
               import_u32(&r,&ignored,"sound legacy flag") &&
               import_u32(&r,&ignored,"sound preload");
      } else if(parsed){
        parsed=import_skip_string(&r,&filename_text,&filename_length,"sound filename") &&
               import_u32(&r,&has_data,"sound data flag") &&
               (!has_data || import_blob(&r,&blob,&blob_size,"sound data")) &&
               import_u32(&r,&ignored,"sound effects") &&
               import_double(&r,&volume,"sound volume") &&
               import_double(&r,&pan,"sound pan") &&
               import_u32(&r,&ignored,"sound preload");
      }
      if(!parsed || r.pos != r.size){
        free_imported_sounds(project);
        return 0;
      }
      (void)kind; (void)filename_text; (void)filename_length; (void)pan;
      if(has_data){
        if(source->legacy_layout){
          int decoded_size=0;
          if(blob_size>INT32_MAX ||
             !(owned_audio=import_inflate_owned(blob,blob_size,&decoded_size)) ||
             decoded_size<0){
            if(err && errcap) snprintf(err,errcap,"classic import: invalid compressed legacy sound");
            free(owned_audio); free_imported_sounds(project); return 0;
          }
          audio=(const uint8_t*)owned_audio; audio_size=(size_t)decoded_size;
        } else { audio = blob; audio_size = blob_size; }
      }
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
    sound->data_path = import_binary_path(project,cache_dir,leaf,audio,audio_size,err,errcap);
    sound->volume = (float)volume;
    sound->pitch = 1.0f;
    int wrote=sound->id && sound->name && sound->data_path;
    free(owned_audio);
    if(!wrote){
      if(err && errcap && !err[0]) snprintf(err, errcap, "classic import: out of memory importing sound %u", i);
      free_imported_sounds(project);
      return 0;
    }
  }
  return 1;
}
