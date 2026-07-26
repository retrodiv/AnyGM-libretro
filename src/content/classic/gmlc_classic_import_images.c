/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gmlc_classic_import_internal.h"

#include "anygm_vfs.h"
#include "gml_image_codec.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void free_imported_sprites(GmlcProject *project){
  for(int i = 0; i < project->n_sprites; ++i){
    GmlcSprite *sprite = &project->sprites[i];
    free(sprite->id);
    free(sprite->name);
    for(int frame = 0; frame < sprite->n_frames; ++frame)
      free(sprite->frame_paths ? sprite->frame_paths[frame] : NULL);
    free(sprite->frame_paths);
    free(sprite->collision_mask_data);
  }
  free(project->sprites);
  project->sprites = NULL;
  project->n_sprites = project->cap_sprites = 0;
}

static int write_rgba_png(const AnygmHostServices *host, const char *path,
                          const uint8_t *source_rgba, uint32_t bytes,
                          int width, int height, char *err, size_t errcap){
  int out_width = width > 0 ? width : 1;
  int out_height = height > 0 ? height : 1;
  if((size_t)out_width > SIZE_MAX / (size_t)out_height / 4u){
    if(err && errcap) snprintf(err, errcap, "classic import: sprite dimensions overflow");
    return 0;
  }
  size_t pixels = (size_t)out_width * (size_t)out_height;
  if(width > 0 && height > 0 && source_rgba && bytes < pixels * 4u){
    if(err && errcap) snprintf(err, errcap, "classic import: truncated sprite BGRA pixels");
    return 0;
  }
  uint8_t *rgba = (uint8_t*)calloc(pixels, 4);
  if(!rgba){
    if(err && errcap) snprintf(err, errcap, "classic import: out of memory converting sprite pixels");
    return 0;
  }
  if(width > 0 && height > 0 && source_rgba){
    memcpy(rgba, source_rgba, pixels * 4u);
  }
  GmlMediaBuffer png={0};
  int ok=gml_image_encode_png(rgba,out_width,out_height,4,out_width*4,&png) &&
         anygm_vfs_write_all(host,path,png.data,png.size);
  gml_media_buffer_release(&png);
  free(rgba);
  if(!ok && err && errcap) snprintf(err, errcap, "classic import: cannot write %s", path);
  return ok != 0;
}

static char *import_rgba_path(GmlcProject *project, const char *cache_dir,
                              const char *leaf, const uint8_t *source_rgba,
                              uint32_t bytes, int width, int height,
                              char *err, size_t errcap){
  if(!project->prefer_memory_files){
    char *path=cache_path(cache_dir,leaf);
    if(!path || !write_rgba_png(project->host,path,source_rgba,bytes,width,height,err,errcap)){
      free(path);
      return NULL;
    }
    return path;
  }
  int out_width=width>0?width:1, out_height=height>0?height:1;
  if((size_t)out_width>SIZE_MAX/(size_t)out_height/4u){
    if(err && errcap) snprintf(err,errcap,"classic import: image dimensions overflow");
    return NULL;
  }
  size_t size=(size_t)out_width*(size_t)out_height*4u;
  if(bytes && bytes<size){
    if(err && errcap) snprintf(err,errcap,"classic import: truncated image pixels");
    return NULL;
  }
  uint8_t *zero=NULL;
  const void *pixels=source_rgba;
  if(width<=0 || height<=0 || !pixels){
    zero=(uint8_t*)calloc(size?size:1u,1);
    if(!zero) return NULL;
    pixels=zero;
  }
  char *path=gmlc_project_add_memory_file(project,leaf,GMLC_MEMORY_RGBA,pixels,size,
                                          out_width,out_height);
  free(zero);
  return path;
}

/* GM8 stores uncompressed project and executable image blobs in BGRA byte order.  The legacy
 * layouts above contain encoded bitmap files, which the image codec has converted to RGBA, so
 * only raw blobs pass through this helper. */
static uint8_t *import_bgra_to_rgba(const uint8_t *bgra, uint32_t bytes,
                                    uint32_t width, uint32_t height,
                                    const char *what, char *err, size_t errcap){
  if(!bgra || !width || !height) return NULL;
  if((size_t)height>SIZE_MAX/(size_t)width){
    if(err && errcap) snprintf(err,errcap,"classic import: oversized %s",what);
    return NULL;
  }
  size_t count=(size_t)width*(size_t)height;
  if(count>SIZE_MAX/4u || bytes<count*4u){
    if(err && errcap) snprintf(err,errcap,"classic import: truncated %s BGRA pixels",what);
    return NULL;
  }
  uint8_t *rgba=(uint8_t*)malloc(count*4u);
  if(!rgba){
    if(err && errcap) snprintf(err,errcap,"classic import: out of memory converting %s pixels",what);
    return NULL;
  }
  for(size_t pixel=0;pixel<count;pixel++){
    rgba[pixel*4u]=bgra[pixel*4u+2u];
    rgba[pixel*4u+1u]=bgra[pixel*4u+1u];
    rgba[pixel*4u+2u]=bgra[pixel*4u];
    rgba[pixel*4u+3u]=bgra[pixel*4u+3u];
  }
  return rgba;
}

static int decode_legacy_image(ImportReader *r, int expected_width, int expected_height,
                               int transparent, int executable_layout,
                               uint8_t **rgba_out, uint32_t *bytes_out,
                               const char *what){
  uint32_t marker;
  *rgba_out=NULL; *bytes_out=0;
  if(!import_u32(r,&marker,what)) return 0;
  if(marker==UINT32_MAX) return 1;
  if(executable_layout){
    uint32_t exists=0,width=0,height=0;
    const uint8_t *compressed=NULL; uint32_t compressed_size=0;
    if(marker<400 || !import_u32(r,&exists,what) || exists>1) return 0;
    if(!exists) return 1;
    if(!import_u32(r,&width,what) || !import_u32(r,&height,what) ||
       width!=(uint32_t)expected_width || height!=(uint32_t)expected_height ||
       !import_blob(r,&compressed,&compressed_size,what) || compressed_size>INT32_MAX ||
       (uint64_t)width*(uint64_t)height*4u>UINT32_MAX) return 0;
    int raw_size=0;
    char *raw=import_inflate_owned(compressed,compressed_size,&raw_size);
    uint32_t expected_bytes=(uint32_t)((uint64_t)width*(uint64_t)height*4u);
    if(!raw || raw_size<0 || (uint32_t)raw_size!=expected_bytes){
      if(r->err && r->errcap) snprintf(r->err,r->errcap,"classic import: invalid compressed %s",what);
      free(raw); return 0;
    }
    uint8_t *rgba=import_bgra_to_rgba((const uint8_t*)raw,expected_bytes,width,height,what,
                                      r->err,r->errcap);
    free(raw);
    if(!rgba) return 0;
    *rgba_out=rgba; *bytes_out=expected_bytes;
    return 1;
  }
  const uint8_t *compressed; uint32_t compressed_size;
  if(!import_blob(r,&compressed,&compressed_size,what) || compressed_size>INT32_MAX) return 0;
  int raw_size=0;
  char *raw=import_inflate_owned(compressed,compressed_size,&raw_size);
  if(!raw || raw_size<=0){
    if(r->err && r->errcap) snprintf(r->err,r->errcap,"classic import: invalid compressed %s",what);
    free(raw); return 0;
  }
  int width=0,height=0,components=0;
  GmlMediaBuffer image={0};
  int decoded=gml_image_decode_rgba((const uint8_t*)raw,(size_t)raw_size,&image,
                                    &width,&height,&components);
  free(raw);
  uint8_t *rgba=image.data;
  if(!decoded || width!=expected_width || height!=expected_height ||
     width<0 || height<0 || (size_t)width>UINT32_MAX/(size_t)(height?height:1)/4u){
    if(r->err && r->errcap) snprintf(r->err,r->errcap,"classic import: invalid %s dimensions",what);
    gml_media_buffer_release(&image); return 0;
  }
  uint32_t bytes=(uint32_t)((size_t)width*(size_t)height*4u);
  if(transparent && width>0 && height>0){
    const uint8_t *key=rgba+((size_t)(height-1)*(size_t)width)*4u;
    uint8_t kr=key[0],kg=key[1],kb=key[2];
    for(size_t p=0;p<(size_t)width*(size_t)height;p++){
      uint8_t *pixel=rgba+p*4u;
      if(pixel[0]==kr && pixel[1]==kg && pixel[2]==kb) pixel[3]=0;
    }
  }
  *rgba_out=rgba; *bytes_out=bytes;
  return 1;
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
    GmlcSprite *sprite = &project->sprites[project->n_sprites++];
    sprite->id = copy_string(source->name);
    sprite->name = copy_string(source->name);
    sprite->runtime_id = (int)slot_index;
    ImportReader r = {source->payload, source->payload_size, 0, err, errcap};
    uint32_t xorigin, yorigin, frames;
    if(source->legacy_layout){
      uint32_t fields[13];
      for(int field=0;field<13;field++) if(!import_u32(&r,&fields[field],"legacy sprite field")){
        free_imported_sprites(project); return 0;
      }
      if(!import_u32(&r,&frames,"legacy sprite frame count") || frames>INT32_MAX ||
         fields[0]>INT32_MAX || fields[1]>INT32_MAX){
        free_imported_sprites(project); return 0;
      }
      sprite->width=(int)fields[0]; sprite->height=(int)fields[1];
      sprite->bbox_left=(int32_t)fields[2]; sprite->bbox_right=(int32_t)fields[3];
      sprite->bbox_bottom=(int32_t)fields[4]; sprite->bbox_top=(int32_t)fields[5];
      sprite->bbox_mode=(int32_t)fields[9]; sprite->col_kind=(int32_t)fields[10];
      sprite->xorig=(int32_t)fields[11]; sprite->yorig=(int32_t)fields[12];
      sprite->n_frames=(int)frames;
      sprite->frame_paths=(char**)calloc(frames?frames:1,sizeof(*sprite->frame_paths));
      if(!sprite->id || !sprite->name || !sprite->frame_paths){ free_imported_sprites(project); return 0; }
      for(uint32_t frame=0;frame<frames;frame++){
        uint8_t *rgba=NULL; uint32_t rgba_bytes=0;
        if(!decode_legacy_image(&r,sprite->width,sprite->height,fields[6]!=0,
                                source->executable_layout,
                                &rgba,&rgba_bytes,"legacy sprite image")){
          free_imported_sprites(project); return 0;
        }
        char leaf[96];
        snprintf(leaf,sizeof(leaf),"classic_sprite_%06u_%06u.png",slot_index,frame);
        sprite->frame_paths[frame]=import_rgba_path(project,cache_dir,leaf,rgba,rgba_bytes,
                                                    sprite->width,sprite->height,err,errcap);
        int ok=sprite->frame_paths[frame]!=NULL;
        free(rgba);
        if(!ok){ free_imported_sprites(project); return 0; }
      }
      if(r.pos!=r.size){ if(err && errcap) snprintf(err,errcap,"classic import: trailing legacy sprite payload"); free_imported_sprites(project); return 0; }
      continue;
    }
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
      uint8_t *converted=NULL;
      const uint8_t *frame_pixels=pixels;
      if(pixels && width && height){
        converted=import_bgra_to_rgba(pixels,pixel_bytes,width,height,"sprite",err,errcap);
        if(!converted){ free_imported_sprites(project); return 0; }
        frame_pixels=converted;
      }
      sprite->frame_paths[frame] = import_rgba_path(project,cache_dir,leaf,frame_pixels,pixel_bytes,
                                                    (int)width,(int)height,err,errcap);
      free(converted);
      if(!sprite->frame_paths[frame]){
        free_imported_sprites(project);
        return 0;
      }
    }
    uint32_t collision[8]={0};
    if(source->executable_layout && !frames){
      if(!import_skip_words(&r,1,"empty-sprite collision flag")){
        free_imported_sprites(project); return 0;
      }
    } else if(source->executable_layout && frames){
      if(source->version>=810 && !import_skip_words(&r,1,"sprite collision shape")){
        free_imported_sprites(project); return 0;
      }
      uint32_t separate;
      if(!import_u32(&r,&separate,"sprite separate collision maps")){
        free_imported_sprites(project); return 0;
      }
      uint32_t maps=separate?frames:1;
      size_t mask_rowbytes=((size_t)sprite->width+7u)/8u;
      if(sprite->width<0 || sprite->height<0 ||
         (sprite->height && mask_rowbytes>SIZE_MAX/(size_t)sprite->height) ||
         (maps && mask_rowbytes*(size_t)sprite->height>SIZE_MAX/(size_t)maps)){
        if(err && errcap) snprintf(err,errcap,"classic import: sprite collision dimensions overflow");
        free_imported_sprites(project); return 0;
      }
      size_t mask_stride=mask_rowbytes*(size_t)sprite->height;
      size_t mask_total=mask_stride*(size_t)maps;
      sprite->collision_mask_data=(uint8_t*)calloc(mask_total?mask_total:1u,1u);
      if(!sprite->collision_mask_data){
        if(err && errcap) snprintf(err,errcap,"classic import: out of memory retaining sprite collision maps");
        free_imported_sprites(project); return 0;
      }
      sprite->collision_mask_stride=mask_stride;
      sprite->collision_mask_count=(int)maps;
      int32_t left=INT32_MAX,right=INT32_MIN,top=INT32_MAX,bottom=INT32_MIN;
      for(uint32_t map=0;map<maps;map++){
        uint32_t fields[7];
        for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);i++)
          if(!import_u32(&r,&fields[i],"sprite collision map")){
            free_imported_sprites(project); return 0;
          }
        uint64_t pixels=(uint64_t)fields[1]*(uint64_t)fields[2];
        const uint8_t *words=r.data+r.pos;
        if(!import_skip_words(&r,pixels,"sprite collision pixels")){
          free_imported_sprites(project); return 0;
        }
        for(uint64_t pixel=0;pixel<pixels;pixel++){
          if(!import_u32_at(words+(size_t)pixel*4u)) continue;
          uint32_t x=fields[1]?(uint32_t)(pixel%fields[1]):0;
          uint32_t y=fields[1]?(uint32_t)(pixel/fields[1]):0;
          if(x<(uint32_t)sprite->width && y<(uint32_t)sprite->height)
            sprite->collision_mask_data[(size_t)map*mask_stride+(size_t)y*mask_rowbytes+x/8u] |=
              (uint8_t)(1u<<(7u-(x&7u)));
        }
        if((int32_t)fields[3]<left) left=(int32_t)fields[3];
        if((int32_t)fields[4]>right) right=(int32_t)fields[4];
        if((int32_t)fields[6]<top) top=(int32_t)fields[6];
        if((int32_t)fields[5]>bottom) bottom=(int32_t)fields[5];
      }
      collision[0]=0; collision[1]=0; collision[2]=separate!=0; collision[3]=2;
      collision[4]=(uint32_t)(left==INT32_MAX?0:left);
      collision[5]=(uint32_t)(right==INT32_MIN?sprite->width-1:right);
      collision[6]=(uint32_t)(bottom==INT32_MIN?sprite->height-1:bottom);
      collision[7]=(uint32_t)(top==INT32_MAX?0:top);
    } else if(!source->executable_layout){
      for(int i = 0; i < 8; ++i)
        if(!import_u32(&r, &collision[i], "sprite collision field")){ free_imported_sprites(project); return 0; }
    }
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
    free(sprite->collision_mask_data);
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
      ImportReader r={slots[i].payload,slots[i].payload_size,0,err,errcap};
      uint32_t fields[12]={0},has_image;
      int field_count=slots[i].executable_layout?5:12;
      for(int field=0;field<field_count;field++) if(!import_u32(&r,&fields[field],"legacy background field")){
        free_imported_backgrounds(project,first_sprite); return 0;
      }
      if(!import_u32(&r,&has_image,"legacy background image flag") ||
         fields[0]>INT32_MAX || fields[1]>INT32_MAX){
        free_imported_backgrounds(project,first_sprite); return 0;
      }
      uint8_t *rgba=NULL; uint32_t rgba_bytes=0;
      if(has_image && !decode_legacy_image(&r,(int)fields[0],(int)fields[1],fields[2]!=0,
                                           slots[i].executable_layout,
                                           &rgba,&rgba_bytes,"legacy background image")){
        free_imported_backgrounds(project,first_sprite); return 0;
      }
      if(r.pos!=r.size){ free(rgba); if(err && errcap) snprintf(err,errcap,"classic import: trailing legacy background payload"); free_imported_backgrounds(project,first_sprite); return 0; }
      GmlcSprite *sprite=&project->sprites[project->n_sprites++];
      sprite->id=copy_string(name); sprite->name=copy_string(name);
      sprite->runtime_id=-1; sprite->tileset_source=1;
      sprite->width=(int)fields[0]; sprite->height=(int)fields[1];
      sprite->bbox_right=sprite->width?sprite->width-1:0;
      sprite->bbox_bottom=sprite->height?sprite->height-1:0;
      sprite->n_frames=1; sprite->frame_paths=(char**)calloc(1,sizeof(*sprite->frame_paths));
      char leaf[80]; snprintf(leaf,sizeof(leaf),"classic_background_%06u.png",i);
      if(sprite->frame_paths)
        sprite->frame_paths[0]=import_rgba_path(project,cache_dir,leaf,rgba,rgba_bytes,
                                                sprite->width,sprite->height,err,errcap);
      int image_ok=sprite->id && sprite->name && sprite->frame_paths && sprite->frame_paths[0];
      free(rgba);
      if(!image_ok){ free_imported_backgrounds(project,first_sprite); return 0; }
      background->sprite_id=project->n_sprites-1;
      background->sprite_no_export=slots[i].executable_layout?1:(fields[5]?0:1);
      if(!slots[i].executable_layout){
        background->tile_width=(int32_t)fields[6]; background->tile_height=(int32_t)fields[7];
        background->border_x=(int32_t)fields[8]; background->border_y=(int32_t)fields[9];
      }
      int step_x=background->tile_width+(int32_t)fields[10];
      int step_y=background->tile_height+(int32_t)fields[11];
      background->columns=step_x>0 && sprite->width>background->border_x
        ? (sprite->width-background->border_x+(int32_t)fields[10])/step_x : 1;
      int rows=step_y>0 && sprite->height>background->border_y
        ? (sprite->height-background->border_y+(int32_t)fields[11])/step_y : 1;
      if(background->columns<1) background->columns=1;
      if(rows<1) rows=1;
      int64_t tile_count=(int64_t)background->columns*(int64_t)rows+1;
      background->tile_count=tile_count>INT32_MAX?INT32_MAX:(int)tile_count;
      continue;
    }
    ImportReader r = {slots[i].payload, slots[i].payload_size, 0, err, errcap};
    uint32_t fields[7] = {0}, image_version, width, height, pixel_bytes = 0;
    const uint8_t *pixels = NULL;
    if(!slots[i].executable_layout)
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
    uint8_t *converted = NULL;
    const uint8_t *image_pixels = pixels;
    if(pixels && width && height){
      converted=import_bgra_to_rgba(pixels,pixel_bytes,width,height,"background",err,errcap);
      if(!converted){ free_imported_backgrounds(project,first_sprite); return 0; }
      image_pixels=converted;
    }
    if(sprite->frame_paths)
      sprite->frame_paths[0]=import_rgba_path(project,cache_dir,leaf,image_pixels,pixel_bytes,
                                              (int)width,(int)height,err,errcap);
    free(converted);
    if(!sprite->id || !sprite->name || !sprite->frame_paths || !sprite->frame_paths[0]){
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
