/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_OPENGL_SHADERS_H
#define GML_GPU_OPENGL_SHADERS_H

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

#define GML_GPU_OPENGL_PREFIX_DESKTOP \
  "#version 330 core\n"

#define GML_GPU_OPENGL_PREFIX_ES \
  "#version 300 es\n" \
  "precision highp float;\n" \
  "precision highp int;\n" \
  "precision highp usampler2D;\n" \
  "precision highp sampler2D;\n"

/* A single triangle larger than the viewport. It needs no vertex buffer and no attributes, which
 * removes an upload and a piece of state to restore; the viewport and the scissor bound what it
 * covers. */
#define GML_GPU_OPENGL_VERTEX_BODY \
  "void main(){\n" \
  "  vec2 corner=vec2((gl_VertexID==1)?3.0:-1.0,(gl_VertexID==2)?3.0:-1.0);\n" \
  "  gl_Position=vec4(corner,0.0,1.0);\n" \
  "}\n"

/* u_map_x and u_map_y hold the exact source index for every destination index, built on the CPU
 * from the same expression the software executor walks. u_target_height converts the framebuffer's
 * bottom-left row to the top-left row AnyGM addresses. */
#define GML_GPU_OPENGL_FRAGMENT_BODY_HEAD \
  "uniform sampler2D u_source;\n" \
  "uniform usampler2D u_map_x;\n" \
  "uniform usampler2D u_map_y;\n" \
  "uniform int u_dest_x;\n" \
  "uniform int u_dest_y;\n" \
  "uniform int u_target_height;\n" \
  "uniform int u_source_height;\n" \
  "uniform int u_resident_source;\n" \
  "out vec4 o_color;\n" \
  "void main(){\n" \
  "  int dx=int(gl_FragCoord.x)-u_dest_x;\n" \
  "  int dy=(u_target_height-1-int(gl_FragCoord.y))-u_dest_y;\n" \
  "  int sx=int(texelFetch(u_map_x,ivec2(dx,0),0).r);\n" \
  "  int sy=int(texelFetch(u_map_y,ivec2(dy,0),0).r);\n" \
  "  // R03_SHADER_RESIDENT_PRESENT: use the current scene image without upload.\n" \
  "  int source_y=u_resident_source!=0 ? u_source_height-1-sy : sy;\n" \
  "  vec4 texel=texelFetch(u_source,ivec2(sx,source_y),0);\n"

/* A CPU source arrives as raw XRGB words uploaded as bytes, so the sampler sees machine byte
 * order. A resident scene texture is already RGBA with bottom-up rows. Both branches select
 * channels without colour arithmetic. */
#define GML_GPU_OPENGL_FRAGMENT_SWIZZLE_LITTLE \
  "  o_color=u_resident_source!=0 ? vec4(texel.rgb,1.0) : vec4(texel.b,texel.g,texel.r,1.0);\n"
#define GML_GPU_OPENGL_FRAGMENT_SWIZZLE_BIG \
  "  o_color=u_resident_source!=0 ? vec4(texel.rgb,1.0) : vec4(texel.g,texel.b,texel.a,1.0);\n"

#define GML_GPU_OPENGL_FRAGMENT_BODY_TAIL \
  "}\n"

/* The scene prototype consumes neutral, already resolved quads. Its atlas uploads are RGBA bytes,
 * unlike the historical hybrid plan's raw packed-word upload. Normalized sampling is qualified
 * separately for each variant before a scene family can become the default. */
#define GML_GPU_OPENGL_SCENE_VERTEX_BODY \
  "in vec2 in_Position;\n" \
  "in vec2 in_TextureCoord;\n" \
  "in vec4 in_Colour;\n" \
  "in vec4 in_Clamp;\n" \
  "in vec4 in_Map;\n" \
  "in vec2 in_Step;\n" \
  "in float in_Margin;\n" \
  "in vec4 in_SpanX;\n" \
  "in vec4 in_SpanY;\n" \
  "in float in_SpanActive;\n" \
  "uniform vec2 u_extent;\n" \
  "out vec2 v_texcoord;\n" \
  "out vec4 v_colour;\n" \
  "out vec4 v_clamp;\n" \
  "flat out vec4 v_map;\n" \
  "flat out vec2 v_step;\n" \
  "flat out float v_margin;\n" \
  "flat out vec4 v_span_x;\n" \
  "flat out vec4 v_span_y;\n" \
  "flat out float v_span_active;\n" \
  "void main(){\n" \
  "  gl_Position=vec4(2.0*in_Position.x/u_extent.x-1.0,1.0-2.0*in_Position.y/u_extent.y,0.0,1.0);\n" \
  "  v_texcoord=in_TextureCoord;\n" \
  "  // R03_SHADER_VERTEX_COLOUR: interpolate authored corner colours.\n" \
  "  v_colour=in_Colour;\n" \
  "  v_clamp=in_Clamp;\n" \
  "  v_map=in_Map;\n" \
  "  v_step=in_Step;\n" \
  "  v_margin=in_Margin;\n" \
  "  v_span_x=in_SpanX;\n" \
  "  v_span_y=in_SpanY;\n" \
  "  v_span_active=in_SpanActive;\n" \
  "}\n"

#define GML_GPU_OPENGL_SCENE_FRAGMENT_BODY \
  "uniform sampler2D u_image;\n" \
  "uniform int u_filter;\n" \
  "uniform vec2 u_extent;\n" \
  "in vec2 v_texcoord;\n" \
  "in vec4 v_colour;\n" \
  "in vec4 v_clamp;\n" \
  "flat in vec4 v_map;\n" \
  "flat in vec2 v_step;\n" \
  "flat in float v_margin;\n" \
  "flat in vec4 v_span_x;\n" \
  "flat in vec4 v_span_y;\n" \
  "flat in float v_span_active;\n" \
  "out vec4 o_color;\n" \
  "vec4 margin_tap(ivec2 p,ivec2 first,ivec2 last,int mask){\n" \
  "  if((mask&16)!=0 && (((mask&1)!=0 && p.x<0) || ((mask&2)!=0 && p.y<0) ||\n" \
  "      ((mask&4)!=0 && p.x>last.x) || ((mask&8)!=0 && p.y>last.y))) return vec4(0.0);\n" \
  "  bool raw=(p.x<0 && (mask&1)!=0) || (p.y<0 && (mask&2)!=0) ||\n" \
  "           (p.x>last.x && (mask&4)!=0) || (p.y>last.y && (mask&8)!=0);\n" \
  "  ivec2 at=first+(raw?p:clamp(p,ivec2(0),last));\n" \
  "  ivec2 extent=textureSize(u_image,0);\n" \
  "  if(any(lessThan(at,ivec2(0))) || any(greaterThanEqual(at,extent))) return vec4(0.0);\n" \
  "  return floor(texelFetch(u_image,at,0)*255.0+0.5);\n" \
  "}\n" \
  "void main(){\n" \
  "  // R03_SHADER_DIRECT_TEXTURE: nearest/atlas page sampling.\n" \
  "  if(u_filter==0){ o_color=texture(u_image,v_texcoord)*v_colour; return; }\n" \
  "  vec2 centre=vec2(gl_FragCoord.x,u_extent.y-gl_FragCoord.y);\n" \
  "  // R03_SHADER_BOX_REDUCTION: stretched integer source-box average.\n" \
  "  if(u_filter==4){\n" \
  "    ivec2 logical=ivec2(v_span_x.x,v_span_y.x),dest=ivec2(v_span_x.y,v_span_y.y);\n" \
  "    ivec2 trim=ivec2(v_span_x.z,v_span_y.z),visible=ivec2(v_span_x.w,v_span_y.w);\n" \
  "    ivec2 p=ivec2(floor(centre-v_map.xy));\n" \
  "    if(any(lessThan(p,ivec2(0))) || any(greaterThanEqual(p,dest))) discard;\n" \
  "    ivec2 first=p*logical/dest,last=max((p+ivec2(1))*logical/dest,first+ivec2(1));\n" \
  "    vec4 total=vec4(0.0); int count=0;\n" \
  "    for(int sy=first.y;sy<last.y;sy++) for(int sx=first.x;sx<last.x;sx++){\n" \
  "      ivec2 at=ivec2(sx,sy)-trim;\n" \
  "      if(any(lessThan(at,ivec2(0))) || any(greaterThanEqual(at,visible))) continue;\n" \
  "      total+=floor(texelFetch(u_image,ivec2(v_clamp.xy)+at,0)*255.0+0.5); count++;\n" \
  "    }\n" \
  "    if(count==0) discard;\n" \
  "    vec4 sampled=floor(total/float(count)+vec4(0.5));\n" \
  "    uvec3 tint=uvec3(floor(v_colour.rgb*255.0+vec3(0.5)));\n" \
  "    uvec3 rgb=uvec3(sampled.rgb)*tint/uvec3(255u);\n" \
  "    o_color=vec4(vec3(rgb)/255.0,sampled.a*v_colour.a/255.0); return;\n" \
  "  }\n" \
  "  // R03_SHADER_ROW_SPAN: conditional rotated/warped CPU coverage.\n" \
  "  if(u_filter>=2 && u_filter!=7 && u_filter!=8 && u_filter!=9 && u_filter!=10 && v_span_active>0.5){\n" \
  "    float mn=1e30,mx=-1e30; int hits=0;\n" \
  "    for(int i=0;i<4;i++){\n" \
  "      int j=(i+1)&3; float a=v_span_y[i],b=v_span_y[j];\n" \
  "      if((a<=centre.y && b>centre.y) || (b<=centre.y && a>centre.y)){\n" \
  "        float x=v_span_x[i]+(centre.y-a)/(b-a)*(v_span_x[j]-v_span_x[i]);\n" \
  "        mn=min(mn,x); mx=max(mx,x); hits++;\n" \
  "      }\n" \
  "    }\n" \
  "    float px=floor(centre.x);\n" \
  "    if(hits<2 || px<floor(mn)-1.0 || px>=ceil(mx)+1.0) discard;\n" \
  "  }\n" \
  "  vec2 delta=centre-v_map.xy;\n" \
  "  ivec2 lo; vec2 f;\n" \
  "  // R03_SHADER_INVERSE_GEOMETRY: affine inverse or triangle-local page UV.\n" \
  "  if(u_filter>=2 && u_filter!=7 && u_filter!=8 && u_filter!=9 && u_filter!=10){\n" \
  "    vec2 local=u_filter>=5\n" \
  "      ?v_texcoord\n" \
  "      :delta.x*v_map.zw+delta.y*v_texcoord+v_step;\n" \
  "    // R03_SHADER_POSITIONED_NEAREST: integer texel selection and tint.\n" \
  "    if(u_filter==3 || u_filter==5){\n" \
  "      ivec2 size=ivec2(v_clamp.zw-v_clamp.xy+vec2(1.0));\n" \
  "      ivec2 point=ivec2(floor(local));\n" \
  "      if(u_filter==3 && (any(lessThan(point,ivec2(0))) ||\n" \
  "         any(greaterThanEqual(point,size)))) discard;\n" \
  "      point=clamp(point,ivec2(0),size-ivec2(1));\n" \
  "      vec4 sampled=floor(texelFetch(u_image,ivec2(v_clamp.xy)+point,0)*255.0+0.5);\n" \
  "      uvec3 tint=uvec3(floor(v_colour.rgb*255.0+vec3(0.5)));\n" \
  "      uvec3 rgb=uvec3(sampled.rgb)*tint/uvec3(255u);\n" \
  "      o_color=vec4(vec3(rgb)/255.0,sampled.a*v_colour.a/255.0); return;\n" \
  "    }\n" \
  "    vec2 size=v_clamp.zw-v_clamp.xy+vec2(1.0);\n" \
  "    // R03_SHADER_GENERAL_VISIBLE_CENTRE: keep the authored colour quad while skipping\n" \
  "    // source centres outside the packed sprite page.\n" \
  "    if((int(v_margin+0.5)&32)!=0 &&\n" \
  "       (any(lessThan(local,vec2(-0.5))) ||\n" \
  "        any(greaterThanEqual(local,size-vec2(0.5))))) discard;\n" \
  "    // R03_SHADER_WARP_BILINEAR: physical-page edge clamp.\n" \
  "    if(u_filter==6) local=clamp(local,vec2(0.0),size)-vec2(0.5);\n" \
  "    else if(any(lessThan(local,vec2(-1.0))) ||\n" \
  "            any(greaterThanEqual(local,size))) discard;\n" \
  "    lo=ivec2(floor(local)); f=fract(local);\n" \
  "  } else {\n" \
  "    vec2 whole=floor(delta);\n" \
  "    vec2 remainder=(delta-whole)+delta*v_step+v_map.zw+\n" \
  "                   (u_filter==7 || u_filter==8 || u_filter==9 || u_filter==10?vec2(0.000001):vec2(-0.5));\n" \
  "    lo=ivec2(whole+floor(remainder)); f=fract(remainder);\n" \
  "  }\n" \
  "  // R03_SHADER_NEAREST_PHASED: retain point phase. First-generation exact ties use the\n" \
  "  // positive tolerance; modern ordinary sprites prepare a bounded per-axis UV bias.\n" \
  "  if(u_filter==7 || u_filter==8 || u_filter==9 || u_filter==10) f=vec2(0.0);\n" \
  "  ivec2 first=ivec2(v_clamp.xy),last=ivec2(v_clamp.zw-v_clamp.xy);\n" \
  "  vec4 p00,p01,p10,p11;\n" \
  "  // R03_SHADER_FILTERED_MARGIN: logical trim and physical atlas edge taps.\n" \
  "  if(v_margin<0.5){\n" \
  "    ivec2 a=clamp(lo,ivec2(0),last),b=clamp(lo+ivec2(1),ivec2(0),last);\n" \
  "    p00=floor(texelFetch(u_image,first+a,0)*255.0+0.5);\n" \
  "    p01=floor(texelFetch(u_image,first+ivec2(b.x,a.y),0)*255.0+0.5);\n" \
  "    p10=floor(texelFetch(u_image,first+ivec2(a.x,b.y),0)*255.0+0.5);\n" \
  "    p11=floor(texelFetch(u_image,first+b,0)*255.0+0.5);\n" \
  "  } else {\n" \
  "    int mask=int(v_margin+0.5);\n" \
  "    p00=margin_tap(lo,first,last,mask);\n" \
  "    p01=margin_tap(lo+ivec2(1,0),first,last,mask);\n" \
  "    p10=margin_tap(lo+ivec2(0,1),first,last,mask);\n" \
  "    p11=margin_tap(lo+ivec2(1,1),first,last,mask);\n" \
  "  }\n" \
  "  // R03_SHADER_SNAPPED_NEAREST_SOURCEOVER: a snapped unit-sign sprite uses this same\n" \
  "  // sample equation at texel centres, with no separate nearest-colour kernel.\n" \
  "  vec4 weighted=(p00*(1.0-f.x)*(1.0-f.y)+p01*f.x*(1.0-f.y)+\n" \
  "                 p10*(1.0-f.x)*f.y+p11*f.x*f.y);\n" \
  "  vec4 sampled=floor(weighted+vec4(0.5));\n" \
  "  // R03_SHADER_TINT_ALPHA: truncating RGB tint and per-draw alpha.\n" \
  "  uvec3 tint=uvec3(floor(v_colour.rgb*255.0+vec3(0.5)));\n" \
  "  uvec3 rgb=uvec3(sampled.rgb)*tint/uvec3(255u);\n" \
  "  // R03_SHADER_MODERN_NEAREST_ALPHA_STAMP: only source-covered texels\n" \
  "  // replace destination alpha in the modern screen stage.\n" \
  "  if(u_filter==10){\n" \
  "    if(sampled.a*v_colour.a<=0.0) discard;\n" \
  "    o_color=vec4(0.0,0.0,0.0,1.0); return;\n" \
  "  }\n" \
  "  // R03_SHADER_DIRECT_NEAREST_COPY: the full-alpha CPU point writer replaces ARGB,\n" \
  "  // including opaque alpha above source texels whose stored alpha is zero.\n" \
  "  o_color=vec4(vec3(rgb)/255.0,\n" \
  "               u_filter==8?1.0:sampled.a*v_colour.a/255.0);\n" \
  "}\n"

/* The eight-bit truncating source-over rule used by a characterised CPU leaf. Sampling the
 * previous destination from a copied, bounded region makes overlaps observe prior writes. */
#define GML_GPU_OPENGL_SCENE_EXACT_FRAGMENT_BODY \
  "uniform sampler2D u_image;\n" \
  "uniform sampler2D u_previous;\n" \
  "uniform ivec2 u_region_origin;\n" \
  "uniform int u_repetitions;\n" \
  "in vec2 v_texcoord;\n" \
  "in vec4 v_colour;\n" \
  "out vec4 o_color;\n" \
  "void main(){\n" \
  "  ivec4 source=ivec4(round(texture(u_image,v_texcoord)*v_colour*255.0));\n" \
  "  ivec4 prior=ivec4(round(texelFetch(u_previous,ivec2(gl_FragCoord.xy)-u_region_origin,0)*255.0));\n" \
  "  int factor=source.a==255?256:source.a;\n" \
  "  // R03_SHADER_EXACT_OVERLAP: ordered destination-dependent eight-bit truncation.\n" \
  "  ivec3 colour=prior.rgb;\n" \
  "  for(int index=0;index<u_repetitions;index++)\n" \
  "    colour=(source.rgb*factor+colour*(256-factor))/256;\n" \
  "  o_color=vec4(vec3(colour)/255.0,1.0);\n" \
  "}\n"

#endif
