/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
#ifndef GMLC_PROJECT_H
#define GMLC_PROJECT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
  GMLC_RES_SPRITE,
  GMLC_RES_SOUND,
  GMLC_RES_SCRIPT,
  GMLC_RES_OBJECT,
  GMLC_RES_ROOM,
  GMLC_RES_SHADER,
  GMLC_RES_FONT,
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
  int x, y, object_id, instance_id;
  float sx, sy, rotation;
  uint32_t color;
} GmlcRoomInstance;

typedef struct {
  char *id;
  char *name;
  int width, height, speed;
  int view_enabled;
  int view_w, view_h, port_w, port_h;
  GmlcRoomInstance *instances;
  int n_instances, cap_instances;
} GmlcRoom;

typedef struct {
  int event_type;
  int event_number;
  int collision_object_id;
} GmlcObjectEvent;

typedef struct {
  char *id;
  char *name;
  int sprite_id, mask_id, parent_id;
  int visible, solid, persistent;
  GmlcObjectEvent *events;
  int n_events, cap_events;
} GmlcObject;

typedef struct {
  char *id;
  char *name;
  int width, height, xorig, yorig;
  int bbox_left, bbox_right, bbox_top, bbox_bottom;
  int n_frames;
  char **frame_paths;
} GmlcSprite;

typedef struct {
  char *id;
  char *name;
  char *source_path;
} GmlcScript;

typedef struct {
  char *id;
  char *name;
  char *data_path;
  float volume, pitch;
} GmlcSound;

typedef struct {
  char *root_dir;
  char *yyp_path;
  char *name;
  GmlcResource *resources;
  int n_resources, cap_resources;
  GmlcSprite *sprites;
  int n_sprites, cap_sprites;
  GmlcSound *sounds;
  int n_sounds, cap_sounds;
  GmlcScript *scripts;
  int n_scripts, cap_scripts;
  GmlcObject *objects;
  int n_objects, cap_objects;
  GmlcRoom *rooms;
  int n_rooms, cap_rooms;
  int n_shaders, n_fonts;
} GmlcProject;

void gmlc_project_init(GmlcProject *p);
void gmlc_project_free(GmlcProject *p);
int gmlc_project_load_yyp(GmlcProject *p, const char *path, char *err, size_t errcap);
int gmlc_project_find_object(const GmlcProject *p, const char *id);
int gmlc_project_find_sprite(const GmlcProject *p, const char *id);
int gmlc_project_find_sound(const GmlcProject *p, const char *id);
int gmlc_project_find_room(const GmlcProject *p, const char *id);
char *gmlc_strdup(const char *s);
char *gmlc_path_dirname(const char *path);
char *gmlc_path_join(const char *a, const char *b);
void gmlc_path_slashes(char *s);

#endif
