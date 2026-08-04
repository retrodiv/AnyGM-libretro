/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GMLC_BYTECODE_INTERNAL_H
#define GMLC_BYTECODE_INTERNAL_H

#include "gmlc_bytecode.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t *data;
  size_t len, cap;
} CodeBuf;

typedef enum {
  TOK_EOF, TOK_ID, TOK_NUM, TOK_STR, TOK_SYM
} TokKind;

typedef struct {
  TokKind kind;
  char text[128];
  double num;
  size_t start, end;
} Tok;

typedef struct {
  const char *src;
  size_t pos;
  Tok tok;
  char err[256];
} Lexer;

typedef struct {
  char *name;
  double value;
} Macro;

typedef struct {
  const GmlcProject *project;
  const GmlcFunctionRegistry *funcs;
  const char *source_path;
  int script_index;
  Macro *macros;
  int n_macros, cap_macros;
  CodeBuf code;
  GmlcRefSite *refs;
  int n_refs, cap_refs;
  GmlcStringSite *strings;
  int n_strings, cap_strings;
  char **locals;
  int n_locals, cap_locals;
  char **globals;
  int n_globals, cap_globals;
  size_t *break_sites;
  int n_break_sites, cap_break_sites;
  size_t *continue_sites;
  int n_continue_sites, cap_continue_sites;
  int continue_depth;
  int break_depth;
  int array_depth;
  int expr_boolish;
  int expr_const;
  double expr_const_value;
  size_t expr_const_start;
  const char *constant_stack[64];
  int constant_depth;
  Lexer lex;
  int unsupported;
  int log_statements;
} Compiler;

typedef struct {
  size_t start, end;
} Span;


char *dup_range(const char *s, size_t n);
char *compiler_read_source(const GmlcProject *project, const char *path);
void lx_next(Lexer *l);
char *lx_string_value(const Lexer *l);
int word_match_at(const char *src, size_t pos, const char *w);
void trim_span(const char *src, Span *s);
int span_empty(const char *src, Span s);
size_t skip_ws_comments_at(const char *src, size_t pos);
int scan_matching_delim(const char *src, size_t open_pos, char open, char close, size_t *close_pos);
int function_shape_at(const char *src, size_t pos);
int source_room_code_count(const GmlcProject *project);

/* Cross-owner bytecode emission operations. */
int emit_u32(CodeBuf *b, uint32_t v);
uint32_t fw(uint8_t op, uint8_t type_byte, int16_t low);
int emit_push_real(Compiler *c, double d);
int emit_push_i16_full(Compiler *c, int16_t v);
int emit_push_i32_full(Compiler *c, int32_t v);
int emit_const_number(Compiler *c, double d);
void expr_not_const(Compiler *c);
int emit_push_string_literal(Compiler *c, const char *s);
int emit_push_var(Compiler *c, int inst, const char *name, uint8_t reftype);
int emit_pop_var(Compiler *c, int inst, const char *name, uint8_t reftype, uint8_t type1);
int emit_call(Compiler *c, const char *name, int argc);
int emit_callv(Compiler *c, int argc);
size_t emit_branch(Compiler *c, uint8_t op);
void patch_branch(Compiler *c, size_t pos, size_t target);
int emit_binary(Compiler *c, uint8_t op);
int emit_binary_typed(Compiler *c, uint8_t op, uint8_t type1, uint8_t type2);
int emit_cmp(Compiler *c, uint8_t cmp);
int emit_cmp_typed(Compiler *c, uint8_t cmp, uint8_t type1, uint8_t type2);
int emit_conv(Compiler *c, uint8_t type1, uint8_t type2);
int emit_condition_bool(Compiler *c);

/* Cross-owner source compilation operation. */
int compile_text_internal(const GmlcProject *project, const GmlcFunctionRegistry *funcs, int script_index, const char *source_path, const char *text, const char *params, GmlcCodeBlob *out, char *err, size_t errcap);

/* GMLC_BYTECODE_PRIVATE_OPERATIONS */

#endif
