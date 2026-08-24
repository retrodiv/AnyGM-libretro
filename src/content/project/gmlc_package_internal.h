/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GMLC_PACKAGE_INTERNAL_H
#define GMLC_PACKAGE_INTERNAL_H

#include "gmlc_bytecode.h"
#include "gmlc_package.h"

#include <stddef.h>
#include <stdint.h>


typedef struct {
  uint8_t *data;
  size_t len, cap;
} Buf;

typedef struct {
  char **items;
  uint32_t *char_off;
  int *hash_slots;
  int hash_cap;
  int n, cap;
} StrTab;

typedef struct {
  uint32_t pos;
  int sid;
} StrPatch;

typedef struct {
  uint32_t pos;
  int frame;
} FramePatch;

typedef struct {
  uint32_t pos;
  int font;
} FontPatch;

typedef struct {
  char *name;
  GmlcRefKind kind;
  uint32_t instr_abs;
  uint32_t ref_abs;
  uint32_t high_bits;
  int code_index;
  int inst;
} CodeRef;

typedef struct {
  uint32_t rel_pos;
  uint32_t blob_off;
} CodeBlobPatch;

typedef struct {
  int sid;
} CodeNameRef;

typedef struct {
  uint16_t sx, sy, sw, sh;
  uint16_t xoff, yoff;
  uint16_t atlas;
} TexturePlacement;

typedef struct {
  char *sprite_name;
  int frame;
  TexturePlacement place;
} RefTextureFrame;

typedef struct {
  int enabled;
  RefTextureFrame *frames;
  int n_frames, cap_frames;
  uint8_t *png;
  size_t png_len;
} RefTextureLayout;

typedef struct {
  Buf b;
  Buf code_data;
  StrTab strs;
  StrPatch *patches;
  int n_patches, cap_patches;
  FramePatch *frame_patches;
  int n_frame_patches, cap_frame_patches;
  FontPatch *font_patches;
  int n_font_patches, cap_font_patches;
  uint32_t *frame_tpag_ptr;
  uint32_t *font_tpag_ptr;
  int n_frames;
  int n_texture_pages;
  TexturePlacement *texture_place;
  int n_texture_items;
  int n_atlas_pages;
  int atlas_dim;
  CodeRef *code_refs;
  int n_code_refs, cap_code_refs;
  CodeBlobPatch *code_blob_patches;
  int n_code_blob_patches, cap_code_blob_patches;
  CodeNameRef *code_name_refs;
  int n_code_name_refs, cap_code_name_refs;
  uint32_t code_blob_base;
  int code_data_emitted;
  int code_blobs_finalized;
  int refs_patched;
  int compiled_code;
  int placeholder_code;
  int code_placeholders;
  int fail_on_placeholder;
  int log_code_compile;
  RefTextureLayout ref_tex;
} Pkg;


/* Cross-owner package operations. */
int add_frame_patch(Pkg *p, uint32_t pos, int frame);
size_t chunk_begin(Pkg *p, const char name[4]);
void chunk_end(Pkg *p, size_t szpos);
int empty_list_chunk(Pkg *p, const char name[4]);
int write_extn(Pkg *pkg, const GmlcProject *p);
int fixed_zero_chunk(Pkg *p, const char name[4], size_t n);
int intern(Pkg *p, const char *s);
void patch32(Buf *b, size_t pos, uint32_t v);
int read_blob(const GmlcProject *project, const char *path,
                     uint8_t **out, size_t *out_len);
int wbytes(Buf *b, const void *p, size_t n);
int wf32(Buf *b, float f);
int wi32(Buf *b, int32_t v);
int write_agrp(Pkg *p);
int write_audo(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap);
int write_classic_marker(Pkg *pkg, const GmlcProject *project);
int write_embi(Pkg *p);
int write_font(Pkg *pkg, const GmlcProject *p);
int write_gen8(Pkg *pkg, const GmlcProject *p);
int write_objt(Pkg *pkg, const GmlcProject *p);
int write_optn(Pkg *p);
int write_room_chunk(Pkg *pkg, const GmlcProject *p);
int write_scpt(Pkg *pkg, const GmlcProject *p);
int write_shdr(Pkg *pkg, const GmlcProject *p);
int write_sond(Pkg *pkg, const GmlcProject *p);
int write_strg(Pkg *pkg);
int write_tmln(Pkg *pkg, const GmlcProject *p);
int wstrptr(Pkg *p, int sid);
int wu16(Buf *b, uint16_t v);
int wu32(Buf *b, uint32_t v);
int wu8(Buf *b, uint8_t v);
int zfill(Buf *b, size_t n);
void free_ref_texture_layout(RefTextureLayout *r);
int load_reference_texture_layout(Pkg *pkg, const GmlcProject *p, const char *path, char *err, size_t errcap);
int write_bgnd(Pkg *pkg, const GmlcProject *p);
int write_path(Pkg *pkg, const GmlcProject *p);
int write_sprt(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap);
int write_tpag(Pkg *pkg, const GmlcProject *p);
int write_txtr(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap);
int event_type_rank(int event_type);
int room_code_count(const GmlcProject *p);
int room_creation_code_index(const GmlcProject *p, int room_index);
int room_instance_creation_code_index(const GmlcProject *p, int room_index, int inst_index);
int seed_code_string_order(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap);
int timeline_moment_code_index(const GmlcProject *p, int timeline_index, int moment_index);
int total_timeline_moments(const GmlcProject *p);
int write_code(Pkg *pkg, const GmlcProject *p, char *err, size_t errcap);
int write_func(Pkg *pkg);
int write_trig(Pkg *pkg, const GmlcProject *p);
int write_vari(Pkg *pkg);

/* GMLC_PACKAGE_PRIVATE_OPERATIONS */

#endif
