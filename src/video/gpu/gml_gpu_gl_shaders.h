/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_GL_SHADERS_H
#define GML_GPU_GL_SHADERS_H

/* The two shader bodies this backend uses, checked in rather than generated: a production build
 * runs no generator and reaches no network.
 *
 * Both are written so that no colour value passes through arithmetic. The destination pixel picks
 * its source index out of an integer texture and fetches that exact texel, so the only float
 * involved is the fixed 0-or-1 alpha the XRGB contract requires. A float scale factor or a
 * normalized sampler coordinate would reintroduce the rounding this whole path exists to avoid:
 * two conforming drivers can round the same float differently and neither is wrong.
 *
 * The version and precision prefix is chosen at build time from the negotiated API, and the source
 * byte-order swizzle is chosen from the machine's own layout, so one body serves desktop OpenGL and
 * OpenGL ES on either endianness. */

#define GML_GPU_GL_PREFIX_DESKTOP \
  "#version 330 core\n"

#define GML_GPU_GL_PREFIX_ES \
  "#version 300 es\n" \
  "precision highp float;\n" \
  "precision highp int;\n" \
  "precision highp usampler2D;\n" \
  "precision highp sampler2D;\n"

/* A single triangle larger than the viewport. It needs no vertex buffer and no attributes, which
 * removes an upload and a piece of state to restore; the viewport and the scissor bound what it
 * covers. */
#define GML_GPU_GL_VERTEX_BODY \
  "void main(){\n" \
  "  vec2 corner=vec2((gl_VertexID==1)?3.0:-1.0,(gl_VertexID==2)?3.0:-1.0);\n" \
  "  gl_Position=vec4(corner,0.0,1.0);\n" \
  "}\n"

/* u_map_x and u_map_y hold the exact source index for every destination index, built on the CPU
 * from the same expression the software executor walks. u_target_height converts the framebuffer's
 * bottom-left row to the top-left row AnyGM addresses. */
#define GML_GPU_GL_FRAGMENT_BODY_HEAD \
  "uniform sampler2D u_source;\n" \
  "uniform usampler2D u_map_x;\n" \
  "uniform usampler2D u_map_y;\n" \
  "uniform int u_dest_x;\n" \
  "uniform int u_dest_y;\n" \
  "uniform int u_target_height;\n" \
  "out vec4 o_color;\n" \
  "void main(){\n" \
  "  int dx=int(gl_FragCoord.x)-u_dest_x;\n" \
  "  int dy=(u_target_height-1-int(gl_FragCoord.y))-u_dest_y;\n" \
  "  int sx=int(texelFetch(u_map_x,ivec2(dx,0),0).r);\n" \
  "  int sy=int(texelFetch(u_map_y,ivec2(dy,0),0).r);\n" \
  "  vec4 texel=texelFetch(u_source,ivec2(sx,sy),0);\n"

/* The source arrives as raw XRGB words uploaded as bytes, so the component order the sampler sees
 * is the machine's memory order rather than red-green-blue. Permuting components is exact; it
 * moves values without computing on them. */
#define GML_GPU_GL_FRAGMENT_SWIZZLE_LITTLE \
  "  o_color=vec4(texel.b,texel.g,texel.r,1.0);\n"
#define GML_GPU_GL_FRAGMENT_SWIZZLE_BIG \
  "  o_color=vec4(texel.g,texel.b,texel.a,1.0);\n"

#define GML_GPU_GL_FRAGMENT_BODY_TAIL \
  "}\n"

#endif
