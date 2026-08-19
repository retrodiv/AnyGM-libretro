/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GMLC_PROJECT_H
#define GMLC_PROJECT_H

#include <stddef.h>
#include <stdint.h>
#include "anygm.h"

typedef enum {
  GMLC_RES_SPRITE,
  GMLC_RES_SOUND,
  GMLC_RES_SCRIPT,
  GMLC_RES_OBJECT,
  GMLC_RES_ROOM,
  GMLC_RES_SHADER,
  GMLC_RES_FONT,
  GMLC_RES_TILESET,
  GMLC_RES_OTHER
} GmlcResKind;

typedef struct {
  char *id;
  char *name;
  char *type_name;
  char *path;
  char *abs_path;
  GmlcResKind kind;
  int type_id;
} GmlcResource;

typedef struct {
  char *id;
  char *name;
  char *creation_code_path;
  int x, y, object_id, instance_id;
  float sx, sy, rotation;
  uint32_t color;
} GmlcRoomInstance;

typedef struct {
  char *name;
  int sprite_id;
  int x, y;
  float sx, sy, rotation, frame, speed;
  uint32_t color;
} GmlcRoomAsset;

typedef struct {
  char *id;
  char *name;
  int layer_id;
  int type;
  int depth;
  int visible;
  float x, y, hspeed, vspeed;
  int bg_sprite_id;
  int bg_htiled, bg_vtiled, bg_stretch;
  uint32_t bg_color;
  float bg_frame, bg_speed;
  int tile_tileset_id, tile_cols, tile_rows;
  uint32_t *tile_data;
  uint32_t *instance_ids;
  int n_instance_ids, cap_instance_ids;
  GmlcRoomAsset *assets;
  int n_assets, cap_assets;
} GmlcRoomLayer;

typedef struct {
  int visible;
  int xview, yview, wview, hview;
  int xport, yport, wport, hport;
  int hborder, vborder, hspeed, vspeed;
  int object_id;
} GmlcRoomView;

typedef struct {
  int visible, foreground, background_id;
  int x, y, htiled, vtiled, hspeed, vspeed, stretch;
} GmlcRoomBackground;

typedef struct {
  int x, y, background_id, source_x, source_y, width, height, depth, tile_id;
} GmlcRoomTile;

typedef struct {
  char *id;
  char *name;
  char *caption;
  int width, height, speed, persistent;
  uint32_t background_color;
  int draw_background_color;
  int clear_view_background;
  int physics_world;
  float physics_gravity_x, physics_gravity_y, physics_scale;
  int view_enabled;
  int view_w, view_h, port_w, port_h;
  GmlcRoomView views[8];
  int n_views;
  GmlcRoomBackground *backgrounds;
  int n_backgrounds, cap_backgrounds;
  GmlcRoomTile *tiles;
  int n_tiles, cap_tiles;
  GmlcRoomInstance *instances;
  int n_instances, cap_instances;
  GmlcRoomLayer *layers;
  int n_layers, cap_layers;
  char *creation_code_path;
} GmlcRoom;

typedef struct {
  char *id;
  int event_type;
  int event_number;
  int collision_object_id;
  char *collision_id;
  char *source_path;
} GmlcObjectEvent;

typedef struct {
  char *id;
  char *name;
  int sprite_id, mask_id, parent_id, depth;
  int visible, solid, persistent;
  int physics_enabled, physics_sensor, physics_shape, physics_group;
  int physics_awake, physics_kinematic;
  float physics_density, physics_restitution, physics_linear_damping;
  float physics_angular_damping, physics_friction;
  struct GmlcPhysicsPoint *physics_points;
  int n_physics_points;
  GmlcObjectEvent *events;
  int n_events, cap_events;
} GmlcObject;

typedef struct GmlcPhysicsPoint {
  float x, y;
} GmlcPhysicsPoint;

typedef struct {
  char *id;
  char *name;
  int runtime_id;
  int tileset_source;
  int width, height, xorig, yorig;
  int bbox_left, bbox_right, bbox_top, bbox_bottom;
  int bbox_mode, col_kind, col_tolerance, sep_masks;
  /* Optional authored collision maps, packed MSB-first by row. Classic packages
   * carry these independently of visible-frame alpha. */
  uint8_t *collision_mask_data;
  size_t collision_mask_stride;
  int collision_mask_count;
  int n_frames;
  char **frame_paths;
} GmlcSprite;

typedef struct {
  char *id;
  char *name;
  char *source_path;
} GmlcScript;

typedef struct {
  char *public_name;
  char *target_name;
  int ambiguous;
} GmlcFunctionAlias;

typedef struct {
  float x, y, speed;
} GmlcPathPoint;

typedef struct {
  char *id;
  char *name;
  int kind, closed, precision;
  GmlcPathPoint *points;
  int n_points;
} GmlcPath;

typedef struct {
  int step;
  char *source_path;
} GmlcTimelineMoment;

typedef struct {
  char *id;
  char *name;
  GmlcTimelineMoment *moments;
  int n_moments, cap_moments;
} GmlcTimeline;

typedef struct {
  char *id;
  char *name;
  char *data_path;
  float volume, pitch;
} GmlcSound;

typedef struct {
  char *id;
  char *name;
  char *vertex_source;
  char *fragment_source;
} GmlcShader;

typedef struct {
  int ch, x, y, w, h, shift, offset;
} GmlcFontGlyph;

typedef struct {
  char *id;
  char *name;
  char *png_path;
  int width, height, em_size;
  GmlcFontGlyph *glyphs;
  int n_glyphs, cap_glyphs;
} GmlcFont;

typedef struct {
  char *id;
  char *name;
  int sprite_id;
  int sprite_no_export;
  int tile_width, tile_height;
  int border_x, border_y;
  int columns, tile_count;
} GmlcTileset;

typedef struct {
  char *name;
  char *expression;
} GmlcProjectConstant;

typedef struct {
  char *name;
  char *condition_path;
  int moment;
  int runtime_id;
} GmlcProjectTrigger;

typedef struct {
  char *file_name;
  char *custom_folder;
  uint8_t *data;
  size_t data_size;
  int export_mode;
  int overwrite_file;
} GmlcProjectIncludedFile;

typedef enum {
  GMLC_MEMORY_BLOB,
  GMLC_MEMORY_TEXT,
  GMLC_MEMORY_RGBA
} GmlcMemoryFileKind;

typedef struct {
  uint8_t *data;
  size_t size;
  int width, height;
  GmlcMemoryFileKind kind;
} GmlcMemoryFile;

typedef struct {
  const AnygmHostServices *host;
  char *root_dir;
  char *yyp_path;
  char *name;
  char *startup_code_path;
  int classic_version;
  int classic_scaling;
  int classic_interpolate;
  int classic_swap_creation_events;
  int classic_executable_layout;
  uint32_t classic_outside_color;
  uint8_t *classic_game_information;
  size_t classic_game_information_size;
  GmlcResource *resources;
  int n_resources, cap_resources;
  GmlcSprite *sprites;
  int n_sprites, cap_sprites;
  GmlcSound *sounds;
  int n_sounds, cap_sounds;
  GmlcScript *scripts;
  int n_scripts, cap_scripts;
  GmlcFunctionAlias *function_aliases;
  int n_function_aliases, cap_function_aliases;
  GmlcPath *paths;
  int n_paths, cap_paths;
  GmlcTimeline *timelines;
  int n_timelines, cap_timelines;
  char **resource_order_ids;
  int n_resource_order;
  char **script_order_ids;
  int n_script_order;
  GmlcObject *objects;
  int n_objects, cap_objects;
  GmlcRoom *rooms;
  int n_rooms, cap_rooms;
  int *room_order;
  int n_room_order;
  int next_instance_id;
  int next_layer_id;
  GmlcShader *shaders;
  int n_shaders, cap_shaders;
  GmlcFont *fonts;
  int n_fonts, cap_fonts;
  GmlcTileset *tilesets;
  int n_tilesets, cap_tilesets;
  GmlcProjectConstant *constants;
  int n_constants, cap_constants;
  GmlcProjectTrigger *triggers;
  int n_triggers, cap_triggers;
  GmlcProjectIncludedFile *included_files;
  int n_included_files, cap_included_files;
  GmlcMemoryFile *memory_files;
  int n_memory_files, cap_memory_files;
  int prefer_memory_files;
} GmlcProject;

void gmlc_project_init(GmlcProject *p);
void gmlc_project_free(GmlcProject *p);
int gmlc_project_load_yyp(GmlcProject *p,const AnygmHostServices *host,
                          const char *path,char *err,size_t errcap);
int gmlc_project_find_object(const GmlcProject *p, const char *id);
int gmlc_project_find_sprite(const GmlcProject *p, const char *id);
int gmlc_project_sprite_runtime_id(const GmlcProject *p, int sprite_index);
int gmlc_project_runtime_sprite_count(const GmlcProject *p);
int gmlc_project_find_sound(const GmlcProject *p, const char *id);
int gmlc_project_find_room(const GmlcProject *p, const char *id);
int gmlc_project_find_tileset(const GmlcProject *p, const char *id);
char *gmlc_strdup(const char *s);
char *gmlc_path_dirname(const char *path);
char *gmlc_path_join(const char *a, const char *b);
void gmlc_path_slashes(char *s);
char *gmlc_project_add_memory_file(GmlcProject *p, const char *label,
                                   GmlcMemoryFileKind kind, const void *data,
                                   size_t size, int width, int height);
const GmlcMemoryFile *gmlc_project_find_memory_file(const GmlcProject *p,
                                                    const char *path);
char *gmlc_project_read_source(const GmlcProject *p, const char *path);

#endif
