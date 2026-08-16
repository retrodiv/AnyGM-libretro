/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_GL_API_H
#define GML_GPU_GL_API_H

#include <stddef.h>
#include <stdint.h>

/* The exact subset of OpenGL this backend uses, declared here rather than pulled from a platform
 * header or a generated loader.
 *
 * Three reasons this is a private declaration set. The core resolves every entry point through the
 * callback the host supplied and links against no graphics library, so a platform header would
 * offer prototypes it must not call. The set is small and fixed, so a generator's output would be
 * mostly unused text with no provenance to review. And keeping it here is what makes the
 * architecture rule enforceable: an OpenGL token anywhere else in the tree is a boundary error.
 *
 * The type widths and enumerant values are the Khronos ABI, which both desktop OpenGL and OpenGL ES
 * share for everything used here. Nothing in this file is copied implementation. */

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef char GLchar;
typedef intptr_t GLintptr;
typedef ptrdiff_t GLsizeiptr;
typedef void GLvoid;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0
#define GL_TRIANGLES 0x0004
#define GL_ZERO 0
#define GL_ONE 1
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_STENCIL_TEST 0x0B90
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_DITHER 0x0BD0
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_MULTISAMPLE 0x809D
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_TEXTURE2 0x84C2
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_RGBA 0x1908
#define GL_RGBA8 0x8058
#define GL_RED_INTEGER 0x8D94
#define GL_R16UI 0x8234
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#define GL_UNPACK_SKIP_ROWS 0x0CF3
#define GL_UNPACK_SKIP_PIXELS 0x0CF4
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_FRAMEBUFFER 0x8D40
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_DEPTH_WRITEMASK 0x0B72

/* Every entry point the backend uses, resolved once per context reset into an engine-owned table.
 * The mandatory ones are listed in the implementation; a missing name is named exactly and rejects
 * initialization rather than crashing at the first call. */
typedef struct GmlGlApi {
  GLenum   (*GetError)(void);
  const GLubyte *(*GetString)(GLenum name);
  void     (*Enable)(GLenum cap);
  void     (*Disable)(GLenum cap);
  void     (*Viewport)(GLint x,GLint y,GLsizei width,GLsizei height);
  void     (*Scissor)(GLint x,GLint y,GLsizei width,GLsizei height);
  void     (*ClearColor)(GLfloat red,GLfloat green,GLfloat blue,GLfloat alpha);
  void     (*Clear)(GLbitfield mask);
  void     (*ColorMask)(GLboolean red,GLboolean green,GLboolean blue,GLboolean alpha);
  void     (*DepthMask)(GLboolean flag);
  void     (*BindFramebuffer)(GLenum target,GLuint framebuffer);
  void     (*GenTextures)(GLsizei n,GLuint *textures);
  void     (*DeleteTextures)(GLsizei n,const GLuint *textures);
  void     (*BindTexture)(GLenum target,GLuint texture);
  void     (*ActiveTexture)(GLenum texture);
  void     (*TexParameteri)(GLenum target,GLenum pname,GLint param);
  void     (*TexImage2D)(GLenum target,GLint level,GLint internalformat,GLsizei width,
                         GLsizei height,GLint border,GLenum format,GLenum type,
                         const void *pixels);
  void     (*TexSubImage2D)(GLenum target,GLint level,GLint xoffset,GLint yoffset,
                            GLsizei width,GLsizei height,GLenum format,GLenum type,
                            const void *pixels);
  void     (*PixelStorei)(GLenum pname,GLint param);
  GLuint   (*CreateShader)(GLenum type);
  void     (*ShaderSource)(GLuint shader,GLsizei count,const GLchar *const *string,
                           const GLint *length);
  void     (*CompileShader)(GLuint shader);
  void     (*GetShaderiv)(GLuint shader,GLenum pname,GLint *params);
  void     (*GetShaderInfoLog)(GLuint shader,GLsizei bufSize,GLsizei *length,GLchar *infoLog);
  void     (*DeleteShader)(GLuint shader);
  GLuint   (*CreateProgram)(void);
  void     (*AttachShader)(GLuint program,GLuint shader);
  void     (*LinkProgram)(GLuint program);
  void     (*GetProgramiv)(GLuint program,GLenum pname,GLint *params);
  void     (*GetProgramInfoLog)(GLuint program,GLsizei bufSize,GLsizei *length,GLchar *infoLog);
  void     (*UseProgram)(GLuint program);
  void     (*DeleteProgram)(GLuint program);
  GLint    (*GetUniformLocation)(GLuint program,const GLchar *name);
  void     (*Uniform1i)(GLint location,GLint v0);
  void     (*GenVertexArrays)(GLsizei n,GLuint *arrays);
  void     (*BindVertexArray)(GLuint array);
  void     (*DeleteVertexArrays)(GLsizei n,const GLuint *arrays);
  void     (*DrawArrays)(GLenum mode,GLint first,GLsizei count);
} GmlGlApi;

#endif
