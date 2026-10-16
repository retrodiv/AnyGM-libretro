/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_OPENGL_SHADERS_H
#define GML_GPU_OPENGL_SHADERS_H

/* Checked-in shader bodies: a production build runs no generator and reaches no network.
 * The integer presentation path fetches exact source texels without colour arithmetic. Scene
 * programs separately implement their qualified sampling, blend and effect rules; bounded exact
 * rules use explicit integer arithmetic where ordinary device floating point is insufficient.
 * The version/precision prefix follows the negotiated API, and presentation byte-order swizzling
 * follows the machine layout, so these bodies serve desktop OpenGL and OpenGL ES. */

#define GML_GPU_OPENGL_PREFIX_DESKTOP \
  "#version 330 core\n"

#define GML_GPU_OPENGL_PREFIX_ES \
  "#version 300 es\n" \
  "precision highp float;\n" \
  "precision highp int;\n" \
  "precision highp usampler2D;\n" \
  "precision highp sampler2D;\n"

/* Fixed recognized-palette executor. Only parsed thresholds and byte colours cross the
 * neutral boundary; arbitrary content GLSL remains a separate frontend policy. */
#define GML_GPU_OPENGL_THRESHOLD_PALETTE_VERTEX \
  "attribute vec3 in_Position; attribute vec2 in_TextureCoord; attribute vec4 in_Colour;\n" \
  "uniform mat4 gm_Matrices[5]; varying vec2 palette_uv; varying vec4 palette_tint;\n" \
  "void main(){ gl_Position=gm_Matrices[4]*vec4(in_Position,1.0);\n" \
  "  palette_uv=in_TextureCoord; palette_tint=in_Colour; }\n"

#define GML_GPU_OPENGL_THRESHOLD_PALETTE_FRAGMENT \
  "uniform sampler2D gm_BaseTexture; varying vec2 palette_uv; varying vec4 palette_tint;\n" \
  "uniform vec3 u_palette0,u_palette1,u_palette2,u_palette3,u_palette4;\n" \
  "uniform vec3 u_palette5,u_palette6,u_palette7,u_palette8,u_palette9;\n" \
  "uniform int u_threshold_red,u_palette_bypass; uniform ivec4 u_threshold_green0,u_threshold_green1;\n" \
  "void main(){\n" \
  "  vec4 source=texture2D(gm_BaseTexture,palette_uv);\n" \
  "  if(u_palette_bypass!=0){ gl_FragColor=source; return; }\n" \
  "  ivec3 sample_bytes=ivec3(floor(source.rgb*255.0+0.5));\n" \
  "  bool upper=sample_bytes.r>=u_threshold_red;\n" \
  "  ivec4 edge=upper?u_threshold_green1:u_threshold_green0;\n" \
  "  int band=sample_bytes.g>=edge.x?0:sample_bytes.g>=edge.y?1:sample_bytes.g>=edge.z?2:sample_bytes.g>=edge.w?3:4;\n" \
  "  vec3 colour;\n" \
  "  if(upper) colour=band==0?u_palette5:band==1?u_palette6:band==2?u_palette7:band==3?u_palette8:u_palette9;\n" \
  "  else colour=band==0?u_palette0:band==1?u_palette1:band==2?u_palette2:band==3?u_palette3:u_palette4;\n" \
  "  ivec3 bytes=ivec3(floor(colour*255.0+0.5));\n" \
  "  ivec3 tint=ivec3(floor(palette_tint.rgb*255.0+0.5));\n" \
  "  gl_FragColor=vec4(vec3(bytes*tint/255)/255.0,1.0);\n" \
  "}\n"

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
  "uniform int u_rgb_query;\n" \
  "uniform ivec2 u_box_destination;\n" \
  "out vec4 o_color;\n" \
  "void main(){\n" \
  "  // R03_SHADER_SCREEN_RGB_QUERY: discard only exact RGB zero; coverage is irrelevant.\n" \
  "  if(u_rgb_query!=0){\n" \
  "    vec3 rgb=texelFetch(u_source,ivec2(gl_FragCoord.xy),0).rgb;\n" \
  "    if(all(equal(rgb,vec3(0.0)))) discard;\n" \
  "    o_color=vec4(1.0); return;\n" \
  "  }\n" \
  "  int dx=int(gl_FragCoord.x)-u_dest_x;\n" \
  "  int dy=(u_target_height-1-int(gl_FragCoord.y))-u_dest_y;\n" \
  "  // R03_SHADER_HOST_BOX: the same integer footprint, hardware-averaged in 2x2 cells.\n" \
  "  if(u_box_destination.x>0){\n" \
  "    ivec2 size=textureSize(u_source,0),p=ivec2(dx,dy);\n" \
  "    ivec2 first=p*size/u_box_destination;\n" \
  "    ivec2 last=min(size,max((p+ivec2(1))*size/u_box_destination,first+ivec2(1)));\n" \
  "    vec3 total=vec3(0.0);\n" \
  "    for(int y=first.y;y<last.y;y+=2){\n" \
  "      int rows=min(2,last.y-y); vec3 row=vec3(0.0);\n" \
  "      for(int x=first.x;x<last.x;x+=2){\n" \
  "        int columns=min(2,last.x-x);\n" \
  "        vec2 centre=vec2(float(x)+float(columns)*0.5,float(y)+float(rows)*0.5);\n" \
  "        vec2 uv=vec2(centre.x,float(size.y)-centre.y)/vec2(size);\n" \
  "        row+=textureLod(u_source,uv,0.0).rgb*float(columns);\n" \
  "      }\n" \
  "      total+=row/float(last.x-first.x)*float(rows);\n" \
  "    }\n" \
  "    o_color=vec4(total/float(last.y-first.y),1.0); return;\n" \
  "  }\n" \
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

/* The ordinary scene path sends one compact record per quad. gl_VertexID selects the same
 * 0,1,2,0,2,3 corners as the expanded stream; exact programs keep the expanded body above. */
#define GML_GPU_OPENGL_SCENE_INSTANCED_VERTEX_BODY \
  "in vec4 in_Position01;\n" \
  "in vec4 in_Position23;\n" \
  "in vec4 in_TextureCoord01;\n" \
  "in vec4 in_TextureCoord23;\n" \
  "in vec4 in_Colour0;\n" \
  "in vec4 in_Colour1;\n" \
  "in vec4 in_Colour2;\n" \
  "in vec4 in_Colour3;\n" \
  "in vec4 in_Clamp;\n" \
  "in vec4 in_Map;\n" \
  "in vec4 in_StepMarginActive;\n" \
  "in vec4 in_SpanX;\n" \
  "in vec4 in_SpanY;\n" \
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
  "  int corner=(gl_VertexID==0 || gl_VertexID==3)?0:(gl_VertexID==1?1:((gl_VertexID==2 || gl_VertexID==4)?2:3));\n" \
  "  vec2 position=corner==0?in_Position01.xy:(corner==1?in_Position01.zw:(corner==2?in_Position23.xy:in_Position23.zw));\n" \
  "  vec2 texcoord=corner==0?in_TextureCoord01.xy:(corner==1?in_TextureCoord01.zw:(corner==2?in_TextureCoord23.xy:in_TextureCoord23.zw));\n" \
  "  vec4 colour=corner==0?in_Colour0:(corner==1?in_Colour1:(corner==2?in_Colour2:in_Colour3));\n" \
  "  gl_Position=vec4(2.0*position.x/u_extent.x-1.0,1.0-2.0*position.y/u_extent.y,0.0,1.0);\n" \
  "  v_texcoord=texcoord;\n" \
  "  v_colour=colour;\n" \
  "  v_clamp=in_Clamp;\n" \
  "  v_map=in_Map;\n" \
  "  v_step=in_StepMarginActive.xy;\n" \
  "  v_margin=in_StepMarginActive.z;\n" \
  "  v_span_active=in_StepMarginActive.w;\n" \
  "  v_span_x=in_SpanX;\n" \
  "  v_span_y=in_SpanY;\n" \
  "}\n"

/* R03_SHADER_CHECKPOINT_TILE: exact comparison of two RGBA8 device images, with no filtering,
 * hashes, temporal lag or lossy colour arithmetic. XRGB canonical state deliberately ignores
 * source alpha; every other format compares it. Edge tiles fetch only existing texels. */
#define GML_GPU_OPENGL_RGB_REDUCE_FRAGMENT_BODY \
  "uniform sampler2D u_image;\n" \
  "uniform ivec2 u_source_extent;\n" \
  "out vec4 o_color;\n" \
  "void main(){\n" \
  "  ivec2 first=ivec2(gl_FragCoord.xy)*8;\n" \
  "  for(int y=0;y<8;y++) for(int x=0;x<8;x++){\n" \
  "    ivec2 p=first+ivec2(x,y);\n" \
  "    if(all(lessThan(p,u_source_extent)) &&\n" \
  "       any(notEqual(texelFetch(u_image,p,0).rgb,vec3(0.0)))){\n" \
  "      o_color=vec4(1.0); return;\n" \
  "    }\n" \
  "  }\n" \
  "  o_color=vec4(0.0);\n" \
  "}\n"

#define GML_GPU_OPENGL_CHECKPOINT_FRAGMENT_BODY \
  "uniform sampler2D u_image;\n" \
  "uniform sampler2D u_previous;\n" \
  "uniform int u_alpha;\n" \
  "out vec4 fragment_colour;\n" \
  "bool checkpoint_tile_changed(ivec2 origin){\n" \
  "  ivec2 extent=textureSize(u_image,0);\n" \
  "  for(int y=0;y<32;y++) for(int x=0;x<32;x++){\n" \
  "    ivec2 p=origin+ivec2(x,y);\n" \
  "    if(p.x>=extent.x || p.y>=extent.y) continue;\n" \
  "    vec4 a=texelFetch(u_image,p,0),b=texelFetch(u_previous,p,0);\n" \
  "    if(any(notEqual(a.rgb,b.rgb)) || (u_alpha!=0 && a.a!=b.a)) return true;\n" \
  "  }\n" \
  "  return false;\n" \
  "}\n" \
  "void main(){\n" \
  "  float changed=checkpoint_tile_changed(ivec2(gl_FragCoord.xy)*32)?1.0:0.0;\n" \
  "  fragment_colour=vec4(changed,0.0,0.0,1.0);\n" \
  "}\n"

#define GML_GPU_OPENGL_SCENE_FRAGMENT_BODY \
  "uniform sampler2D u_image;\n" \
  "uniform sampler2D u_previous;\n" \
  "uniform ivec2 u_region_origin;\n" \
  "uniform ivec4 u_rotated_alpha16[64];\n" \
  "uniform int u_filter;\n" \
  "uniform int u_alpha_test;\n" \
  "uniform usampler2D u_point_map_x,u_point_map_y;\n" \
  "uniform ivec2 u_point_origin;\n" \
  "uniform sampler2D u_recovery_screen,u_recovery_application;\n" \
  "uniform ivec4 u_recovery_rect;\n" \
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
  "vec4 hardware_background_tap(ivec2 at){\n" \
  "  ivec2 extent=textureSize(u_image,0);\n" \
  "  if(any(lessThan(at,ivec2(0))) || any(greaterThanEqual(at,extent))) return vec4(0.0);\n" \
  "  return texelFetch(u_image,at,0);\n" \
  "}\n" \
  "vec4 hardware_background_sample(vec2 uv,ivec2 first,ivec2 last,int mask){\n" \
  "  ivec2 low=first-ivec2((mask&1)!=0?1:0,(mask&2)!=0?1:0);\n" \
  "  ivec2 high=last+ivec2((mask&4)!=0?1:0,(mask&8)!=0?1:0);\n" \
  "  vec2 extent=vec2(textureSize(u_image,0));\n" \
  "  vec4 result=texture(u_image,clamp(uv,(vec2(low)+0.5)/extent,(vec2(high)+0.5)/extent));\n" \
  "  vec2 p=uv*extent-vec2(0.5);\n" \
  "  bool x_plain=(p.x<float(first.x)&&(mask&1)==0)||(p.x>float(last.x)&&(mask&4)==0);\n" \
  "  bool y_plain=(p.y<float(first.y)&&(mask&2)==0)||(p.y>float(last.y)&&(mask&8)==0);\n" \
  "  bool x_pad=(p.x<float(first.x)&&(mask&1)!=0)||(p.x>float(last.x)&&(mask&4)!=0);\n" \
  "  bool y_pad=(p.y<float(first.y)&&(mask&2)!=0)||(p.y>float(last.y)&&(mask&8)!=0);\n" \
  "  if(!((x_plain&&y_pad)||(y_plain&&x_pad))) return result;\n" \
  "  // A padded tap escapes both axes; repair only these mixed logical corners.\n" \
  "  ivec2 base=ivec2(floor(p)); vec2 f=fract(p);\n" \
  "  for(int y=0;y<2;y++) for(int x=0;x<2;x++){\n" \
  "    ivec2 at=base+ivec2(x,y);\n" \
  "    bool escape=(at.x<first.x&&(mask&1)!=0)||(at.x>last.x&&(mask&4)!=0)||\n" \
  "                (at.y<first.y&&(mask&2)!=0)||(at.y>last.y&&(mask&8)!=0);\n" \
  "    ivec2 authored=escape?at:clamp(at,first,last),hardware=clamp(at,low,high);\n" \
  "    if(any(notEqual(authored,hardware))) result+=\n" \
  "      (hardware_background_tap(authored)-hardware_background_tap(hardware))*\n" \
  "      (x==0?1.0-f.x:f.x)*(y==0?1.0-f.y:f.y);\n" \
  "  }\n" \
  "  return clamp(result,vec4(0.0),vec4(1.0));\n" \
  "}\n" \
  "vec4 hardware_physical_sprite_sample(vec2 uv,ivec2 first,ivec2 last,int mask){\n" \
  "  ivec2 extent=textureSize(u_image,0); vec2 p=uv*vec2(extent)-vec2(0.5);\n" \
  "  ivec2 low=first-ivec2((mask&1)!=0?1:0,(mask&2)!=0?1:0);\n" \
  "  ivec2 high=last+ivec2((mask&4)!=0?1:0,(mask&8)!=0?1:0);\n" \
  "  if(all(greaterThanEqual(low,ivec2(0))) && all(lessThan(high,extent)) &&\n" \
  "     all(greaterThanEqual(p,vec2(low))) && all(lessThanEqual(p,vec2(high))))\n" \
  "    return hardware_background_sample(uv,first,last,mask);\n" \
  "  // Rounded minified coverage may reach beyond one padding texel.\n" \
  "  // Keep hardware filtering; repair only crop-clamped or absent atlas taps.\n" \
  "  vec4 result=texture(u_image,uv); ivec2 base=ivec2(floor(p)); vec2 f=fract(p);\n" \
  "  for(int y=0;y<2;y++) for(int x=0;x<2;x++){\n" \
  "    ivec2 at=base+ivec2(x,y);\n" \
  "    bool escape=(at.x<first.x&&(mask&1)!=0)||(at.x>last.x&&(mask&4)!=0)||\n" \
  "                (at.y<first.y&&(mask&2)!=0)||(at.y>last.y&&(mask&8)!=0);\n" \
  "    ivec2 authored=escape?at:clamp(at,first,last);\n" \
  "    ivec2 hardware=clamp(at,ivec2(0),extent-ivec2(1));\n" \
  "    if(any(notEqual(authored,hardware))) result+=\n" \
  "      (hardware_background_tap(authored)-hardware_background_tap(hardware))*\n" \
  "      (x==0?1.0-f.x:f.x)*(y==0?1.0-f.y:f.y);\n" \
  "  }\n" \
  "  return clamp(result,vec4(0.0),vec4(1.0));\n" \
  "}\n" \
  "float stretched_float_phase(int remainder,int denominator){\n" \
  "  if(remainder==0) return 0.0;\n" \
  "  uint scaled=uint(remainder),divisor=uint(denominator); int exponent=127;\n" \
  "  while(scaled<divisor){ scaled<<=1u; exponent--; }\n" \
  "  uint significand=uint(float(scaled)*8388608.0/float(divisor));\n" \
  "  // The quotient is within a few codes. Its exact bounded residual survives unsigned wrap.\n" \
  "  int residual=int((scaled<<23u)-significand*divisor);\n" \
  "  for(int step=0;step<8;step++){\n" \
  "    if(residual<0){ significand--; residual+=denominator; }\n" \
  "    else if(residual>=denominator){ significand++; residual-=denominator; }\n" \
  "    else break;\n" \
  "  }\n" \
  "  if(residual*2>denominator ||\n" \
  "     (residual*2==denominator && (significand&1u)!=0u)) significand++;\n" \
  "  if(significand==16777216u){ significand>>=1u; exponent++; }\n" \
  "  return uintBitsToFloat((uint(exponent)<<23u)|(significand&8388607u));\n" \
  "}\n" \
  "float stretched_float_add(float left,float right){\n" \
  "  // Nonnegative normal channel products only. Preserve each CPU float addition\n" \
  "  // with guard/round/sticky bits instead of allowing contracted shader arithmetic.\n" \
  "  uint a=floatBitsToUint(left),b=floatBitsToUint(right);\n" \
  "  if(a==0u) return right; if(b==0u) return left;\n" \
  "  if(a<b){ uint swap=a; a=b; b=swap; }\n" \
  "  uint exponent=(a>>23u)&255u,shift=exponent-((b>>23u)&255u);\n" \
  "  uint am=((a&8388607u)|8388608u)<<3u,bm=((b&8388607u)|8388608u)<<3u;\n" \
  "  if(shift>=27u) bm=1u;\n" \
  "  else if(shift>0u){ uint lost=bm&((1u<<shift)-1u); bm=(bm>>shift)|(lost>0u?1u:0u); }\n" \
  "  uint total=am+bm;\n" \
  "  if(total>=134217728u){ total=(total>>1u)|(total&1u); exponent++; }\n" \
  "  uint significand=total>>3u,tail=total&7u;\n" \
  "  if(tail>4u || (tail==4u && (significand&1u)!=0u)) significand++;\n" \
  "  if(significand==16777216u){ significand>>=1u; exponent++; }\n" \
  "  return uintBitsToFloat((exponent<<23u)|(significand&8388607u));\n" \
  "}\n" \
  "float particle_line_cell(vec2 p){\n" \
  "  float x=clamp(min((p.x+29.0)/5.0,(28.0-p.x)/5.0),0.0,1.0);\n" \
  "  float y=clamp(min((p.y+5.75)/2.5,(4.75-p.y)/2.5),0.0,1.0);\n" \
  "  return x*y;\n" \
  "}\n" \
  "ivec3 rectangle_rgb(float encoded_rgb){\n" \
  "  uint c=uint(encoded_rgb); return ivec3((c>>16u)&255u,(c>>8u)&255u,c&255u);\n" \
  "}\n" \
  "ivec3 rectangle_lerp(ivec3 first,ivec3 last,int n,int d){\n" \
  "  if(d<=0 || n<=0) return first; if(n>=d) return last;\n" \
  "  return first+(last-first)*n/d;\n" \
  "}\n" \
  "void main(){\n" \
  "  if(u_filter>=34 && u_filter<=36){\n" \
  "    // R03_SHADER_RECTANGLE: integer authored colours and exact clipped coverage.\n" \
  "    ivec2 pixel=ivec2(int(gl_FragCoord.x),int(u_extent.y)-1-int(gl_FragCoord.y));\n" \
  "    ivec2 p=pixel-ivec2(v_map.xy),den=ivec2(v_map.zw);\n" \
  "    uint flags=uint(v_margin); bool outline=(flags&1u)!=0u,gradient=(flags&8u)!=0u;\n" \
  "    uint blend=(flags>>4u)&15u;\n" \
  "    if(outline && p.x!=0 && p.x!=den.x && p.y!=0 && p.y!=den.y) discard;\n" \
  "    if(u_filter==36){ o_color=vec4(0.0,0.0,0.0,1.0); return; }\n" \
  "    if(blend==1u && ((!outline && !gradient) || (flags&256u)!=0u) && v_span_active!=0.0){\n" \
  "      o_color=vec4(vec3(rectangle_rgb(v_span_y.z))/255.0,v_span_y.w/255.0); return;\n" \
  "    }\n" \
  "    ivec3 rgb=rectangle_rgb(v_span_x.x);\n" \
  "    if(gradient){\n" \
  "      ivec3 left=rectangle_lerp(rgb,rectangle_rgb(v_span_x.w),p.y,den.y);\n" \
  "      ivec3 right=rectangle_lerp(rectangle_rgb(v_span_x.y),rectangle_rgb(v_span_x.z),p.y,den.y);\n" \
  "      if(v_colour.a>=1.0 && blend<9u){\n" \
  "        int offset=int(v_span_y.x)-int(v_map.x),visible=int(v_span_y.y);\n" \
  "        ivec3 clipped_left=rectangle_lerp(left,right,offset,den.x);\n" \
  "        right=rectangle_lerp(left,right,offset+visible,den.x);\n" \
  "        rgb=rectangle_lerp(clipped_left,right,pixel.x-int(v_span_y.x),visible);\n" \
  "      } else rgb=rectangle_lerp(left,right,p.x,den.x);\n" \
  "    }\n" \
  "    float alpha=v_colour.a;\n" \
  "    if(v_span_active==0.0 || alpha>=1.0){\n" \
  "      alpha=(!outline && !gradient && blend==0u) || (flags&4u)==0u?1.0:floor(alpha*255.0+0.5)/255.0;\n" \
  "    }\n" \
  "    if(blend>=2u){\n" \
  "      alpha=floor(alpha*255.0+0.5)/255.0;\n" \
  "      if(blend!=4u && (flags&4u)==0u) alpha=1.0;\n" \
  "    }\n" \
  "    o_color=vec4(vec3(rgb)/255.0,alpha); return;\n" \
  "  }\n" \
  "  if(u_filter==31 || u_filter==32){ o_color=v_colour; return; }\n" \
  "  // R03_SHADER_STUDIO_ALPHA_RECT: a coverage-only pass stamps written alpha.\n" \
  "  if(u_filter==33){ o_color=vec4(0.0,0.0,0.0,1.0); return; }\n" \
  "  if(u_filter==28){\n" \
  "    // R03_SHADER_PARTICLE_LINE: procedural soft coverage, separate fixed source-over.\n" \
  "    vec2 pixel=vec2(floor(gl_FragCoord.x),u_extent.y-1.0-floor(gl_FragCoord.y));\n" \
  "    vec2 delta=pixel-v_map.xy;\n" \
  "    vec2 local=vec2(v_map.z*delta.x-v_map.w*delta.y,v_map.w*delta.x+v_map.z*delta.y);\n" \
  "    vec2 point=(local+vec2(0.5))/v_step-vec2(0.5);\n" \
  "    float coverage;\n" \
  "    if(v_span_x.z==0.0) coverage=particle_line_cell(floor(point+vec2(0.5)));\n" \
  "    else {\n" \
  "      vec2 low=floor(point),phase=fract(point);\n" \
  "      float a=particle_line_cell(low),b=particle_line_cell(low+vec2(1.0,0.0));\n" \
  "      float c=particle_line_cell(low+vec2(0.0,1.0)),d=particle_line_cell(low+vec2(1.0));\n" \
  "      coverage=mix(mix(a,b,phase.x),mix(c,d,phase.x),phase.y);\n" \
  "    }\n" \
  "    if(coverage<=0.0) discard;\n" \
  "    float alpha=v_span_x.w==0.0?1.0:floor(clamp(coverage*v_colour.a,0.0,1.0)*255.0+0.5)/255.0;\n" \
  "    if(alpha==0.0) discard;\n" \
  "    o_color=vec4(v_colour.rgb,alpha); return;\n" \
  "  }\n" \
  "  // Exact global RGB predicate; no CPU query or destination sampling in the blit.\n" \
  "  if(u_filter==23 || u_filter==24){\n" \
  "    if(texelFetch(u_recovery_screen,ivec2(0),0).r!=0.0 ||\n" \
  "       texelFetch(u_recovery_application,ivec2(0),0).r==0.0) discard;\n" \
  "    ivec2 pixel=ivec2(floor(vec2(gl_FragCoord.x,u_extent.y-gl_FragCoord.y)))-u_recovery_rect.xy;\n" \
  "    ivec2 size=textureSize(u_image,0);\n" \
  "    ivec2 point=clamp(pixel*size/u_recovery_rect.zw,ivec2(0),size-ivec2(1));\n" \
  "    vec4 sampled=texelFetch(u_image,ivec2(point.x,size.y-1-point.y),0);\n" \
  "    if(sampled.a==0.0) discard;\n" \
  "    o_color=u_filter==24?vec4(0.0,0.0,0.0,1.0):sampled; return;\n" \
  "  }\n" \
  "  // R03_SHADER_DIRECT_TEXTURE: nearest/atlas page sampling.\n" \
  "  // Exact CPU-selected rotated texel spans, truncating tint and scalar16 RGB.\n" \
  "  if(u_filter==29){\n" \
  "    ivec2 point=ivec2(floor(v_texcoord*vec2(textureSize(u_image,0))));\n" \
  "    if(v_span_active>0.5){\n" \
  "      int offset=int(floor(gl_FragCoord.x))-int(v_map.x);\n" \
  "      ivec4 sx=ivec4(v_span_x),sy=ivec4(v_span_y);\n" \
  "      int px=(sx.x*65536+sx.y+offset*(sx.z*65536+sx.w))>>20;\n" \
  "      int py=(sy.x*65536+sy.y+offset*(sy.z*65536+sy.w))>>20;\n" \
  "      point=ivec2(px,py)+ivec2(v_clamp.xy);\n" \
  "      if(any(lessThan(point,ivec2(v_clamp.xy))) || any(greaterThan(point,ivec2(v_clamp.zw)))) discard;\n" \
  "    }\n" \
  "    ivec4 sampled=ivec4(floor(texelFetch(u_image,point,0)*255.0+0.5));\n" \
  "    int encoded=u_rotated_alpha16[sampled.a/4][sampled.a%4];\n" \
  "    int factor=encoded&131071;\n" \
  "    if(factor==0) discard;\n" \
  "    ivec3 tint=ivec3(floor(v_colour.rgb*255.0+0.5));\n" \
  "    ivec3 rgb=sampled.rgb*tint/255;\n" \
  "    ivec4 prior=ivec4(floor(texelFetch(u_previous,ivec2(gl_FragCoord.xy)-u_region_origin,0)*255.0+0.5));\n" \
  "    rgb=(rgb*factor+prior.rgb*(65536-factor))/65536;\n" \
  "    int alpha=encoded>>17;\n" \
  "    alpha=(alpha*alpha+prior.a*(255-alpha)+127)/255;\n" \
  "    o_color=vec4(rgb,alpha)/255.0; return;\n" \
  "  }\n" \
  "  // R03_SHADER_INTEGER_SPRITE: whole nearest texels, integer tint and fixed blending.\n" \
  "  if(u_filter>=37 && u_filter<=64){\n" \
  "    int rule=u_filter>50?u_filter-14:u_filter;\n" \
  "    uvec4 sampled;\n" \
  "    if(u_filter>50){\n" \
  "      ivec2 dest=ivec2(abs(v_map.zw)),size=textureSize(u_image,0);\n" \
  "      ivec2 p=ivec2(int(gl_FragCoord.x),int(v_step.y)-1-int(gl_FragCoord.y))-ivec2(v_map.xy);\n" \
  "      if(v_map.z<0.0) p.x=dest.x-1-p.x;\n" \
  "      if(v_map.w<0.0) p.y=dest.y-1-p.y;\n" \
  "      ivec2 at=p*size/dest;\n" \
  "      if(v_span_x.x==1.0) at.x=(2*p.x*size.x+dest.x)/(2*dest.x);\n" \
  "      if(v_span_x.x==2.0) at.x=(2*p.x+1)*size.x/(2*dest.x);\n" \
  "      if(v_span_x.y==1.0) at.y=(2*p.y*size.y+dest.y)/(2*dest.y);\n" \
  "      if(v_span_x.y==2.0) at.y=(2*p.y+1)*size.y/(2*dest.y);\n" \
  "      if(v_span_x.x==3.0) at.x=((2*p.x+1)*size.x-1)/(2*dest.x);\n" \
  "      if(v_span_x.y==3.0) at.y=((2*p.y+1)*size.y-1)/(2*dest.y);\n" \
  "      at=clamp(at,ivec2(0),size-ivec2(1));\n" \
  "      if(v_step.x!=0.0) at.y=size.y-1-at.y;\n" \
  "      sampled=uvec4(floor(texelFetch(u_image,at,0)*255.0+0.5));\n" \
  "    }else sampled=uvec4(floor(texture(u_image,v_texcoord)*255.0+0.5));\n" \
  "    if(u_alpha_test>=0 && sampled.a<=uint(u_alpha_test)) discard;\n" \
  "    if(u_filter>50 && sampled.a==0u && v_span_y.x!=1.0) discard;\n" \
  "    if(rule<44 && (sampled.a==0u || (rule<41 && v_colour.a<=0.0 && u_filter<=50)) && !(u_filter>50 && v_span_y.x==1.0)) discard;\n" \
  "    if(rule==41 || rule==48){ o_color=vec4(0.0,0.0,0.0,1.0); return; }\n" \
  "    uvec3 tint=uvec3(floor(v_colour.rgb*255.0+0.5));\n" \
  "    uint bias=(rule==38 || rule==40 || rule==43 || rule==45 || rule==47 || rule==50)?127u:0u;\n" \
  "    uvec3 rgb=(sampled.rgb*tint+uvec3(bias))/255u;\n" \
  "    float alpha=(rule==42 || rule==43 || rule>=49)?1.0:float(sampled.a)*v_colour.a/255.0;\n" \
  "    if(u_filter>50 && v_span_y.x>0.0 && v_span_y.x<3.0) alpha=float(sampled.a)/255.0;\n" \
  "    if(u_filter>50 && v_span_y.x==3.0 && sampled.a==255u) alpha=1.0;\n" \
  "    o_color=vec4(vec3(rgb)/255.0,alpha); return;\n" \
  "  }\n" \
  "  if(u_filter==0){ o_color=texture(u_image,v_texcoord)*v_colour; return; }\n" \
  "  // R03_SHADER_APPLICATION_PIXEL_CENTRE: opaque presentation copies RGB and stamps alpha.\n" \
  "  if(u_filter==30){\n" \
  "    vec2 extent=vec2(textureSize(u_image,0));\n" \
  "    vec2 position=vec2(gl_FragCoord.x,v_step.y-gl_FragCoord.y);\n" \
  "    vec2 texel=floor((position-v_map.xy)*extent/v_map.zw);\n" \
  "    if(v_step.x!=0.0) texel.y=extent.y-1.0-texel.y;\n" \
  "    texel=clamp(texel,v_clamp.xy,v_clamp.zw);\n" \
  "    o_color=vec4(texture(u_image,(texel+vec2(0.5))/extent).rgb,1.0); return;\n" \
  "  }\n" \
  "  // R03_SHADER_HARDWARE_FILTERED_ADDITIVE: Hardware bilinear additive without destination sampling.\n" \
  "  if(u_filter==18){\n" \
  "    vec2 extent=vec2(textureSize(u_image,0));\n" \
  "    vec2 uv=clamp(v_texcoord,(v_clamp.xy+vec2(0.5))/extent,\n" \
  "                            (v_clamp.zw+vec2(0.5))/extent);\n" \
  "    vec4 sampled=floor(texture(u_image,uv)*255.0+vec4(0.5));\n" \
  "    float alpha=sampled.a*v_colour.a;\n" \
  "    if(alpha<=0.0) discard;\n" \
  "    uvec3 tint=uvec3(floor(v_colour.rgb*255.0+vec3(0.5)));\n" \
  "    uvec3 rgb=uvec3(sampled.rgb)*tint/uvec3(255u);\n" \
  "    o_color=vec4(floor(vec3(rgb)*(alpha/255.0))/255.0,floor(alpha+0.5)/255.0);\n" \
  "    return;\n" \
  "  }\n" \
  "  // R03_SHADER_HARDWARE_FILTERED_BACKGROUND: hardware sampling and fixed source-over.\n" \
  "  if(u_filter==19 || u_filter==20){\n" \
  "    int mask=int(floor(v_margin+0.5));\n" \
  "    uvec4 sampled=uvec4(floor(hardware_background_sample(v_texcoord,\n" \
  "                          ivec2(v_clamp.xy),ivec2(v_clamp.zw),mask)*255.0+0.5));\n" \
  "    if(sampled.a==0u) discard;\n" \
  "    if(u_filter==20){\n" \
  "      // Unit-white application draws leave combined rounding to the framebuffer.\n" \
  "      o_color=vec4(vec3(sampled.rgb)/255.0,float(sampled.a)/255.0);\n" \
  "      return;\n" \
  "    }\n" \
  "    uvec3 tint=uvec3(floor(v_colour.rgb*255.0+0.5));\n" \
  "    uvec3 rgb=sampled.rgb*tint/uvec3(255u);\n" \
  "    rgb=(rgb*sampled.a+uvec3(127u))/uvec3(255u);\n" \
  "    o_color=vec4(vec3(rgb)/255.0,float(sampled.a)/255.0); return;\n" \
  "  }\n" \
  "  // R03_SHADER_HARDWARE_FILTERED_SURFACE: hardware sampling and batched coverage.\n" \
  "  if((u_filter>=65 && u_filter<=74)||(u_filter>=79 && u_filter<=82)){\n" \
  "    vec2 extent=vec2(textureSize(u_image,0));\n" \
  "    vec2 uv=clamp(v_texcoord,(v_clamp.xy+vec2(0.5))/extent,\n" \
  "                            (v_clamp.zw+vec2(0.5))/extent);\n" \
  "    int margin=int(floor(v_margin+0.5));\n" \
  "    vec4 sampled=margin!=0?hardware_physical_sprite_sample(v_texcoord,\n" \
  "      ivec2(v_clamp.xy),ivec2(v_clamp.zw),margin):texture(u_image,uv);\n" \
  "    if(u_alpha_test>=0 && sampled.a*255.0<=float(u_alpha_test)) discard;\n" \
  "    if(u_filter>=79){\n" \
  "      float alpha=sampled.a*v_colour.a; if(alpha<=0.0) discard;\n" \
  "      o_color=u_filter==82?vec4(0.0,0.0,0.0,1.0):\n" \
  "        vec4(sampled.rgb*v_colour.rgb,u_filter==81?1.0:alpha); return;\n" \
  "    }\n" \
  "    uvec4 bytes=uvec4(floor(clamp(sampled,0.0,1.0)*255.0+vec4(0.5)));\n" \
  "    if(bytes.a==0u && u_filter!=68 && u_filter!=69 && u_filter!=73 && u_filter!=74) discard;\n" \
  "    uvec3 tint=uvec3(v_colour.rgb*255.0+0.5);\n" \
  "    vec3 rgb=vec3((bytes.rgb*tint)/255u)/255.0;\n" \
  "    float alpha=float(bytes.a)*v_colour.a/255.0;\n" \
  "    o_color=(u_filter==72 || u_filter==74)?vec4(0.0,0.0,0.0,1.0):\n" \
  "      vec4(rgb,(u_filter==67 || u_filter==73)?1.0:alpha); return;\n" \
  "  }\n" \
  "  if(u_filter==21 || u_filter==22 || u_filter==25 || u_filter==26){\n" \
  "    vec2 extent=vec2(textureSize(u_image,0));\n" \
  "    vec2 uv=clamp(v_texcoord,(v_clamp.xy+vec2(0.5))/extent,\n" \
  "                            (v_clamp.zw+vec2(0.5))/extent);\n" \
  "    int margin=int(floor(v_margin+0.5));\n" \
  "    vec4 sampled=margin!=0?hardware_background_sample(v_texcoord,\n" \
  "      ivec2(v_clamp.xy),ivec2(v_clamp.zw),margin):texture(u_image,uv);\n" \
  "    float alpha=sampled.a*v_colour.a;\n" \
  "    if(alpha<=0.0) discard;\n" \
  "    if(u_filter==25){\n" \
  "      uvec3 rgb=uvec3(sampled.rgb*255.0+0.5);\n" \
  "      uvec3 tint=uvec3(v_colour.rgb*255.0+0.5);\n" \
  "      sampled.rgb=vec3((rgb*tint)/255u)/255.0;\n" \
  "    }\n" \
  "    o_color=(u_filter==22 || u_filter==26)?vec4(0.0,0.0,0.0,1.0):\n" \
  "      vec4(u_filter==25?sampled.rgb:sampled.rgb*v_colour.rgb,alpha); return;\n" \
  "  }\n" \
  "  vec2 centre=vec2(gl_FragCoord.x,u_extent.y-gl_FragCoord.y);\n" \
  "  // R03_SHADER_BOX_REDUCTION: stretched integer source-box average.\n" \
  "  if(u_filter==77 || u_filter==78){\n" \
  "    ivec2 logical=ivec2(v_span_x.x,v_span_y.x),dest=ivec2(v_span_x.y,v_span_y.y);\n" \
  "    ivec2 trim=ivec2(v_span_x.z,v_span_y.z),visible=ivec2(v_span_x.w,v_span_y.w);\n" \
  "    ivec2 p=ivec2(floor(centre-v_map.xy)),den=2*dest;\n" \
  "    ivec2 numerator=clamp((2*p+ivec2(1))*logical-dest,ivec2(0),(logical-ivec2(1))*den);\n" \
  "    ivec2 low=numerator/den;vec2 phase=vec2(numerator%den)/vec2(den);\n" \
  "    vec2 local=vec2(low-trim)+phase;\n" \
  "    vec2 at=clamp(local,vec2(0.0),vec2(visible-ivec2(1)));\n" \
  "    vec4 sampled=texture(u_image,(v_clamp.xy+at+vec2(0.5))/vec2(textureSize(u_image,0)));\n" \
  "    vec2 coverage=clamp(local+vec2(1.0),0.0,1.0)*clamp(vec2(visible)-local,0.0,1.0);\n" \
  "    sampled*=coverage.x*coverage.y;\n" \
  "    // Coverage uses logical taps even when the hardware's tiny alpha rounds away.\n" \
  "    float witness=0.0;\n" \
  "    for(int y=0;y<2;y++) for(int x=0;x<2;x++){\n" \
  "      ivec2 tap=min(low+ivec2(x,y),logical-ivec2(1))-trim;\n" \
  "      if(any(lessThan(tap,ivec2(0))) || any(greaterThanEqual(tap,visible))) continue;\n" \
  "      float weight=(x==0?1.0-phase.x:phase.x)*(y==0?1.0-phase.y:phase.y);\n" \
  "      witness+=texelFetch(u_image,ivec2(v_clamp.xy)+tap,0).a*weight;\n" \
  "    }\n" \
  "    if(witness<=0.0 || (u_alpha_test>=0 && sampled.a*255.0<=float(u_alpha_test))) discard;\n" \
  "    o_color=u_filter==78?vec4(0.0,0.0,0.0,1.0):vec4(sampled.rgb*v_colour.rgb,sampled.a*v_colour.a); return;\n" \
  "  }\n" \
  "  if(u_filter==4 || u_filter==75 || u_filter==76){\n" \
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
  "    // R03_SHADER_STRETCHED_BOX: raw cutoff, float tint and fixed blending.\n" \
  "    if(u_filter==75 || u_filter==76){\n" \
  "      if((u_alpha_test>=0 && int(total.a)<=u_alpha_test*count) || sampled.a<=0.0) discard;\n" \
  "      if(u_filter==76){o_color=vec4(0.0,0.0,0.0,1.0); return;}\n" \
  "      uvec3 tint=uvec3(floor(v_colour.rgb*255.0+0.5));\n" \
  "      uvec3 rounding=all(equal(logical,dest))?uvec3(0u):uvec3(127u);\n" \
  "      vec3 rgb=vec3((uvec3(sampled.rgb)*tint+rounding)/uvec3(255u))/255.0;\n" \
  "      o_color=vec4(rgb,sampled.a*v_colour.a/255.0); return;\n" \
  "    }\n" \
  "    uvec3 tint=uvec3(floor(v_colour.rgb*255.0+vec3(0.5)));\n" \
  "    uvec3 rgb=uvec3(sampled.rgb)*tint/uvec3(255u);\n" \
  "    o_color=vec4(vec3(rgb)/255.0,sampled.a*v_colour.a/255.0); return;\n" \
  "  }\n" \
  "  // R03_SHADER_ROW_SPAN: conditional rotated/warped CPU coverage.\n" \
  "  if(u_filter>=2 && u_filter!=7 && u_filter!=8 && u_filter!=9 && u_filter!=10 && u_filter!=27 &&\n" \
  "     u_filter!=12 && u_filter!=13 && v_span_active>0.5){\n" \
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
  "  // R03_SHADER_STRETCHED_FLOAT: exact integer phase before the CPU band's float weights.\n" \
  "  // R03_SHADER_MODERN_NUMBERED_POINT_MAP: original binary64 point indices,\n" \
  "  // including reflected anchors and skipped physical crop edges.\n" \
  "  if(u_filter==27){\n" \
  "    ivec2 p=ivec2(floor(centre))-u_point_origin;\n" \
  "    lo=ivec2(texelFetch(u_point_map_x,ivec2(p.x,0),0).r,\n" \
  "             texelFetch(u_point_map_y,ivec2(p.y,0),0).r);\n" \
  "    if(any(equal(lo,ivec2(65535)))) discard;\n" \
  "    f=vec2(0.0);\n" \
  "  } else if(u_filter==11 || u_filter==14){\n" \
  "    ivec2 logical=ivec2(v_span_x.x,v_span_y.x),dest=ivec2(v_span_x.y,v_span_y.y);\n" \
  "    // Add one denominator before division so edge phases never use negative remainder.\n" \
  "    ivec2 numerator=(ivec2(floor(delta))*2+ivec2(1))*logical+dest;\n" \
  "    ivec2 denominator=dest*2,remainder=numerator%denominator;\n" \
  "    lo=numerator/denominator-ivec2(1);\n" \
  "    f=vec2(stretched_float_phase(remainder.x,denominator.x),\n" \
  "           stretched_float_phase(remainder.y,denominator.y));\n" \
  "  } else if(u_filter>=2 && u_filter!=7 && u_filter!=8 && u_filter!=9 && u_filter!=10 && u_filter!=27 &&\n" \
  "            u_filter!=12 && u_filter!=13){\n" \
  "    vec2 local=u_filter>=5 && u_filter!=15 && u_filter!=17\n" \
  "      ?v_texcoord\n" \
  "      :delta.x*v_map.zw+delta.y*v_texcoord+v_step;\n" \
  "    // R03_SHADER_POSITIONED_NEAREST: integer texel selection and tint.\n" \
  "    if(u_filter==3 || u_filter==5 || u_filter==15 || u_filter==17){\n" \
  "      ivec2 size=ivec2(v_clamp.zw-v_clamp.xy+vec2(1.0));\n" \
  "      ivec2 point=ivec2(floor(local));\n" \
  "      if(u_filter!=5 && (any(lessThan(point,ivec2(0))) ||\n" \
  "         any(greaterThanEqual(point,size)))) discard;\n" \
  "      point=clamp(point,ivec2(0),size-ivec2(1));\n" \
  "      vec4 sampled=floor(texelFetch(u_image,ivec2(v_clamp.xy)+point,0)*255.0+0.5);\n" \
  "      // R03_SHADER_GUI_ROTATED_NEAREST_COVERAGE: shared binary64 alpha LUT,\n" \
  "      // exact16 integer source-over and the CPU packet/tail alpha policy.\n" \
  "      if(u_filter==15 || u_filter==17){\n" \
  "        int source_alpha=int(sampled.a);\n" \
  "        int factor=u_rotated_alpha16[source_alpha/4][source_alpha%4];\n" \
  "        if(factor==0) discard;\n" \
  "        ivec3 tint=ivec3(floor(v_colour.rgb*255.0+vec3(0.5)));\n" \
  "        ivec4 pixel=ivec4(ivec3(sampled.rgb)*tint/255,255);\n" \
  "        ivec4 prior=ivec4(floor(texelFetch(u_previous,ivec2(gl_FragCoord.xy)-u_region_origin,0)*255.0+0.5));\n" \
  "        ivec4 result=(pixel*factor+prior*(65536-factor))/65536;\n" \
  "        if(v_margin<0.5) result.a=255;\n" \
  "        o_color=vec4(result)/255.0; return;\n" \
  "      }\n" \
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
  "                   (u_filter==7 || u_filter==8 || u_filter==9 || u_filter==10 || u_filter==27?vec2(0.000001):vec2(-0.5));\n" \
  "    // R03_SHADER_GUI_AXIS_PHASE: keep integral products out of the fractional\n" \
  "    // sum and restore the vertex preparator's low phase/reciprocal residuals.\n" \
  "    if(u_filter==12 || u_filter==13){\n" \
  "      vec2 high_delta=floor(delta/4096.0)*4096.0,low_delta=delta-high_delta;\n" \
  "      vec2 high_step=uintBitsToFloat(floatBitsToUint(v_step)&uvec2(0xffffe000u));\n" \
  "      vec2 high_product=high_delta*high_step,low_product=low_delta*high_step;\n" \
  "      vec2 integral=floor(high_product)+floor(low_product);\n" \
  "      vec2 residual=delta*(v_step-high_step)+delta*vec2(v_span_x.y,v_span_y.y);\n" \
  "      remainder=fract(high_product)+fract(low_product)+residual+(delta-whole)+\n" \
  "                v_map.zw+vec2(v_span_x.x,v_span_y.x)-vec2(0.5);\n" \
  "      whole+=integral;\n" \
  "    }\n" \
  "    lo=ivec2(whole+floor(remainder)); f=fract(remainder);\n" \
  "  }\n" \
  "  // R03_SHADER_NEAREST_PHASED: retain point phase. First-generation exact ties use the\n" \
  "  // positive tolerance; modern ordinary sprites prepare a bounded per-axis UV bias.\n" \
  "  if(u_filter==7 || u_filter==8 || u_filter==9 || u_filter==10 || u_filter==27) f=vec2(0.0);\n" \
  "  ivec2 first=ivec2(v_clamp.xy),last=ivec2(v_clamp.zw-v_clamp.xy);\n" \
  "  vec4 p00,p01,p10,p11;\n" \
  "  // R03_SHADER_FILTERED_MARGIN: logical trim and physical atlas edge taps.\n" \
  "  if(u_filter==14){\n" \
  "    // Clamp logical source edges before resolving transparent packed trim.\n" \
  "    ivec2 logical=ivec2(v_span_x.x,v_span_y.x),trim=ivec2(v_span_x.z,v_span_y.z);\n" \
  "    ivec2 a=clamp(lo,ivec2(0),logical-ivec2(1))-trim;\n" \
  "    ivec2 b=clamp(lo+ivec2(1),ivec2(0),logical-ivec2(1))-trim;\n" \
  "    p00=margin_tap(a,first,last,31);\n" \
  "    p01=margin_tap(ivec2(b.x,a.y),first,last,31);\n" \
  "    p10=margin_tap(ivec2(a.x,b.y),first,last,31);\n" \
  "    p11=margin_tap(b,first,last,31);\n" \
  "  } else if(v_margin<0.5){\n" \
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
  "  if(u_filter==11 || u_filter==14){\n" \
  "    vec4 weights=vec4((1.0-f.x)*(1.0-f.y),f.x*(1.0-f.y),\n" \
  "                      (1.0-f.x)*f.y,f.x*f.y);\n" \
  "    vec4 a=p00*weights.x,b=p01*weights.y,c=p10*weights.z,d=p11*weights.w;\n" \
  "    for(int channel=0;channel<4;channel++)\n" \
  "      weighted[channel]=stretched_float_add(stretched_float_add(\n" \
  "        stretched_float_add(a[channel],b[channel]),c[channel]),d[channel]);\n" \
  "  }\n" \
  "  // R03_SHADER_STRETCHED_COVERED_ALPHA: the CPU float band blends unrounded\n" \
  "  // channels and stamps alpha only where unrounded effective alpha is positive.\n" \
  "  if(u_filter==14){\n" \
  "    float oa=(weighted.a*(1.0/255.0))*v_colour.a;\n" \
  "    if(oa<=0.0) discard;\n" \
  "    vec3 rgb=weighted.rgb*v_colour.rgb;\n" \
  "    if(oa<1.0){\n" \
  "      vec3 previous=floor(texelFetch(u_previous,ivec2(gl_FragCoord.xy)-u_region_origin,0).rgb*255.0+vec3(0.5));\n" \
  "      float ia=1.0-oa;\n" \
  "      for(int channel=0;channel<3;channel++)\n" \
  "        rgb[channel]=stretched_float_add(rgb[channel]*oa,previous[channel]*ia);\n" \
  "    }\n" \
  "    for(int channel=0;channel<3;channel++)\n" \
  "      rgb[channel]=floor(stretched_float_add(rgb[channel],0.5));\n" \
  "    o_color=vec4(rgb/255.0,1.0); return;\n" \
  "  }\n" \
  "  vec4 sampled=floor(weighted+vec4(0.5));\n" \
  "  // R03_SHADER_MODERN_NUMBERED_NEAREST: ordered predecessor and binary64-prepared alpha LUT.\n" \
  "  if(u_filter==27){\n" \
  "    int source_alpha=int(sampled.a);\n" \
  "    int encoded=u_rotated_alpha16[source_alpha/4][source_alpha%4];\n" \
  "    int factor=encoded&8388607;\n" \
  "    if(factor==0) discard;\n" \
  "    ivec3 tint=ivec3(floor(v_colour.rgb*255.0+vec3(0.5)));\n" \
  "    ivec3 rgb=(ivec3(sampled.rgb)*tint+ivec3(127))/255;\n" \
  "    ivec4 prior=ivec4(floor(texelFetch(u_previous,ivec2(gl_FragCoord.xy)-u_region_origin,0)*255.0+0.5));\n" \
  "    rgb=(rgb*factor+prior.rgb*(4194304-factor)+ivec3(2097152))/4194304;\n" \
  "    int alpha=encoded>>23;\n" \
  "    alpha=(alpha*alpha+prior.a*(255-alpha)+127)/255;\n" \
  "    o_color=vec4(rgb,alpha)/255.0; return;\n" \
  "  }\n" \
  "  // R03_SHADER_TINT_ALPHA: truncating RGB tint and per-draw alpha.\n" \
  "  uvec3 tint=uvec3(floor(v_colour.rgb*255.0+vec3(0.5)));\n" \
  "  uvec3 rgb=uvec3(sampled.rgb)*tint/uvec3(255u);\n" \
  "  // R03_SHADER_MODERN_NEAREST_ALPHA_STAMP / R03_SHADER_GUI_FILTERED_ALPHA_STAMP:\n" \
  "  // only source-covered texels replace screen alpha. Filtered GUI admission has\n" \
  "  // positive original draw alpha, even when its packed RGB factor rounds to zero.\n" \
  "  if(u_filter==10 || u_filter==13){\n" \
  "    if(sampled.a<=0.0 || (u_filter==10 && v_colour.a<=0.0)) discard;\n" \
  "    o_color=vec4(0.0,0.0,0.0,1.0); return;\n" \
  "  }\n" \
  "  // R03_SHADER_DIRECT_NEAREST_COPY: the full-alpha CPU point writer replaces ARGB,\n" \
  "  // including opaque alpha above source texels whose stored alpha is zero.\n" \
  "  o_color=vec4(vec3(rgb)/255.0,\n" \
  "               u_filter==8?1.0:sampled.a*v_colour.a/255.0);\n" \
  "}\n"

/* The bounded filtered-font rule preserves the CPU's ordered binary64 expression.
 * Embedded programs specialise the generation and write one RGB channel per pass:
 * inlining all generations/channels together overwhelms small-device compilers.
 * Desktop programs retain their combined single-pass interface. */
#define GML_GPU_OPENGL_FILTERED_FONT_BODY \
  "// R03_SHADER_FILTERED_FONT: bounded normal binary64, round-to-nearest-even.\n" \
  "// Integer limbs retain the CPU operations on both GL 3.3 and GLES 3.0.\n" \
  "uniform ivec4 u_font_origin;\n" \
  "uniform ivec4 u_font_scale;\n" \
  "uniform ivec2 u_font_alpha;\n" \
  "uniform ivec4 u_font_crop;\n" \
  "uniform ivec4 u_font_tint;\n" \
  "uniform int u_font_margin;\n" \
  "#if GML_EXACT_FONT_MODE>0\n" \
  "#define u_font_mode GML_EXACT_FONT_MODE\n" \
  "uniform int u_font_channel;\n" \
  "#else\n" \
  "uniform int u_font_mode;\n" \
  "#endif\n" \
  "uniform int u_font_preserve_alpha;\n" \
  "bool d64_zero(uvec2 a){ return (a.x|(a.y&0x7fffffffu))==0u; }\n" \
  "bool limb_less(uvec2 a,uvec2 b){ return a.y<b.y || (a.y==b.y && a.x<b.x); }\n" \
  "uvec2 limb_add(uvec2 a,uvec2 b){\n" \
  "  uint lo=a.x+b.x; return uvec2(lo,a.y+b.y+uint(lo<a.x));\n" \
  "}\n" \
  "uvec2 limb_sub(uvec2 a,uvec2 b){ return uvec2(a.x-b.x,a.y-b.y-uint(a.x<b.x)); }\n" \
  "uvec2 limb_left(uvec2 a,int n){\n" \
  "  // Callers use nonnegative counts; every evaluated native shift stays below 32.\n" \
  "  uint k=uint(n)&31u;\n" \
  "  uint lo=a.x<<k,hi=(a.y<<k)|((a.x>>((32u-k)&31u))*uint(k!=0u));\n" \
  "  uvec2 q=n<32?uvec2(lo,hi):uvec2(0u,lo);\n" \
  "  return n<64?q:uvec2(0u);\n" \
  "}\n" \
  "uvec2 limb_right(uvec2 a,int n){\n" \
  "  uint k=uint(n)&31u;\n" \
  "  uint hi=a.y>>k,lo=(a.x>>k)|((a.y<<((32u-k)&31u))*uint(k!=0u));\n" \
  "  uvec2 q=n<32?uvec2(lo,hi):uvec2(hi,0u);\n" \
  "  return n<64?q:uvec2(0u);\n" \
  "}\n" \
  "uvec2 limb_jam(uvec2 a,int n){\n" \
  "  uvec2 q=limb_right(a,n);\n" \
  "  q.x|=uint(any(notEqual(limb_left(q,n),a)));\n" \
  "  return q;\n" \
  "}\n" \
  "uvec2 d64_mantissa(uvec2 a){ return uvec2(a.x,(a.y&0xfffffu)|0x100000u); }\n" \
  "int d64_exponent(uvec2 a){ return int((a.y>>20u)&2047u)-1023; }\n" \
  "int d64_high_bit(uint value){\n" \
  "  int result=0; uint n=value>=65536u?16u:0u; value>>=n; result+=int(n);\n" \
  "  n=value>=256u?8u:0u; value>>=n; result+=int(n);\n" \
  "  n=value>=16u?4u:0u; value>>=n; result+=int(n);\n" \
  "  n=value>=4u?2u:0u; value>>=n; result+=int(n);\n" \
  "  return result+int(value>=2u);\n" \
  "}\n" \
  "uvec2 d64_pack(uvec2 extended,int exponent,uint sign){\n" \
  "  if((extended.x|extended.y)==0u) return uvec2(0u);\n" \
  "  if(extended.y>=0x1000000u){ extended=limb_jam(extended,1); exponent++; }\n" \
  "  // Inputs contain at most 57 bits. The one-bit carry leaves high<=55.\n" \
  "  // Find the same normalisation as repeated left shifts without a shader loop.\n" \
  "  int high=extended.y!=0u?32+d64_high_bit(extended.y):d64_high_bit(extended.x);\n" \
  "  int shift=55-high; extended=limb_left(extended,shift); exponent-=shift;\n" \
  "  uint residue=extended.x&7u;\n" \
  "  uvec2 mantissa=limb_right(extended,3);\n" \
  "  if(residue>4u || (residue==4u && (mantissa.x&1u)!=0u))\n" \
  "    mantissa=limb_add(mantissa,uvec2(1u,0u));\n" \
  "  if(mantissa.y>=0x200000u){ mantissa=limb_right(mantissa,1); exponent++; }\n" \
  "  return uvec2(mantissa.x,sign|(uint(exponent+1023)<<20u)|(mantissa.y&0xfffffu));\n" \
  "}\n" \
  "uvec2 d64_uint(uint value){\n" \
  "  if(value==0u) return uvec2(0u);\n" \
  "  int exponent=d64_high_bit(value);\n" \
  "  uvec2 m=limb_left(uvec2(value,0u),52-exponent);\n" \
  "  return uvec2(m.x,(uint(exponent+1023)<<20u)|(m.y&0xfffffu));\n" \
  "}\n" \
  "uvec2 d64_int(int value){\n" \
  "  uvec2 result=d64_uint(uint(value<0?-value:value));\n" \
  "  if(value<0) result.y|=0x80000000u;\n" \
  "  return result;\n" \
  "}\n" \
  "uvec2 d64_add(uvec2 a,uvec2 b){\n" \
  "  if(d64_zero(a)) return b;\n" \
  "  if(d64_zero(b)) return a;\n" \
  "  int ea=d64_exponent(a),eb=d64_exponent(b);\n" \
  "  uvec2 ma=limb_left(d64_mantissa(a),3),mb=limb_left(d64_mantissa(b),3);\n" \
  "  uint sa=a.y&0x80000000u,sb=b.y&0x80000000u;\n" \
  "  if(ea<eb){ uvec2 swap=ma;ma=mb;mb=swap;int e=ea;ea=eb;eb=e;uint s=sa;sa=sb;sb=s; }\n" \
  "  mb=limb_jam(mb,ea-eb);\n" \
  "  uvec2 extended;\n" \
  "  if(sa==sb) extended=limb_add(ma,mb);\n" \
  "  else if(limb_less(ma,mb)){ extended=limb_sub(mb,ma);sa=sb; }\n" \
  "  else extended=limb_sub(ma,mb);\n" \
  "  return d64_pack(extended,ea,sa);\n" \
  "}\n" \
  "uvec2 d64_sub(uvec2 a,uvec2 b){ b.y^=0x80000000u; return d64_add(a,b); }\n" \
  "uvec2 d64_mul(uvec2 a,uvec2 b){\n" \
  "  if(d64_zero(a) || d64_zero(b)) return uvec2(0u);\n" \
  "  uvec2 ma=d64_mantissa(a),mb=d64_mantissa(b);\n" \
  "  uint aa[4],bb[4],p[8];\n" \
  "  aa[0]=ma.x&65535u;aa[1]=ma.x>>16u;aa[2]=ma.y&65535u;aa[3]=ma.y>>16u;\n" \
  "  bb[0]=mb.x&65535u;bb[1]=mb.x>>16u;bb[2]=mb.y&65535u;bb[3]=mb.y>>16u;\n" \
  "  for(int i=0;i<8;i++) p[i]=0u;\n" \
  "  for(int i=0;i<4;i++){\n" \
  "    uint carry=0u;\n" \
  "    for(int j=0;j<4;j++){\n" \
  "      uint term=aa[i]*bb[j]+p[i+j]+carry;\n" \
  "      p[i+j]=term&65535u;carry=term>>16u;\n" \
  "    }\n" \
  "    p[i+4]=carry;\n" \
  "  }\n" \
  "  uvec4 w=uvec4(p[0]|(p[1]<<16u),p[2]|(p[3]<<16u),\n" \
  "                p[4]|(p[5]<<16u),p[6]|(p[7]<<16u));\n" \
  "  int high=(p[6]&512u)!=0u?1:0;\n" \
  "  int shift=49+high;\n" \
  "  uvec2 extended=uvec2((w.y>>uint(shift-32))|(w.z<<uint(64-shift)),\n" \
  "                       (w.z>>uint(shift-32))|(w.w<<uint(64-shift)));\n" \
  "  if(w.x!=0u || (w.y&((1u<<uint(shift-32))-1u))!=0u) extended.x|=1u;\n" \
  "  return d64_pack(extended,d64_exponent(a)+d64_exponent(b)+high,\n" \
  "                   (a.y^b.y)&0x80000000u);\n" \
  "}\n" \
  "uvec2 d64_div(uvec2 a,uvec2 b){\n" \
  "  if(d64_zero(a)) return uvec2(0u);\n" \
  "  uvec2 numerator=d64_mantissa(a),denominator=d64_mantissa(b);\n" \
  "  int exponent=d64_exponent(a)-d64_exponent(b);\n" \
  "  if(limb_less(numerator,denominator)){ numerator=limb_left(numerator,1);exponent--; }\n" \
  "  uvec2 remainder=limb_sub(numerator,denominator),quotient=uvec2(1u,0u);\n" \
  "  for(int bit=0;bit<55;bit++){\n" \
  "    remainder=limb_left(remainder,1);quotient=limb_left(quotient,1);\n" \
  "    if(!limb_less(remainder,denominator)){\n" \
  "      remainder=limb_sub(remainder,denominator);quotient.x|=1u;\n" \
  "    }\n" \
  "  }\n" \
  "  if((remainder.x|remainder.y)!=0u) quotient.x|=1u;\n" \
  "  return d64_pack(quotient,exponent,(a.y^b.y)&0x80000000u);\n" \
  "}\n" \
  "int d64_floor(uvec2 a){\n" \
  "  if(d64_zero(a)) return 0;\n" \
  "  bool negative=(a.y&0x80000000u)!=0u;\n" \
  "  int exponent=d64_exponent(a);\n" \
  "  if(exponent<0) return negative?-1:0;\n" \
  "  uint integral=limb_right(d64_mantissa(a),52-exponent).x;\n" \
  "  if(!negative) return int(integral);\n" \
  "  uvec2 absolute=uvec2(a.x,a.y&0x7fffffffu);\n" \
  "  return -int(integral)-int(any(notEqual(absolute,d64_uint(integral))));\n" \
  "}\n" \
  "int d64_round(uvec2 a){ return d64_floor(d64_add(a,uvec2(0u,0x3fe00000u))); }\n" \
  "int d64_lround(uvec2 a){\n" \
  "  int integral=d64_floor(a);\n" \
  "  uvec2 boundary=d64_add(d64_uint(uint(integral)),uvec2(0u,0x3fe00000u));\n" \
  "  return integral+int(!limb_less(a,boundary));\n" \
  "}\n" \
  "uvec2 font_channel(ivec4 a,ivec4 b,ivec4 c,ivec4 d,int channel,\n" \
  "                   uvec2 wa,uvec2 wb,uvec2 wc,uvec2 wd){\n" \
  "  uvec2 sum=d64_add(d64_mul(d64_uint(uint(a[channel])),wa),\n" \
  "                    d64_mul(d64_uint(uint(b[channel])),wb));\n" \
  "  sum=d64_add(sum,d64_mul(d64_uint(uint(c[channel])),wc));\n" \
  "  return d64_add(sum,d64_mul(d64_uint(uint(d[channel])),wd));\n" \
  "}\n" \
  "// R03_SHADER_GUI_FILTERED_EXACT_TAPS: retain the CPU TPAG tap decision.\n" \
  "ivec4 font_tpag_tap(ivec2 local){\n" \
  "  bool neighbour=(local.x<0 && (u_font_margin&1)!=0) ||\n" \
  "    (local.y<0 && (u_font_margin&2)!=0) ||\n" \
  "    (local.x>=u_font_crop.z && (u_font_margin&4)!=0) ||\n" \
  "    (local.y>=u_font_crop.w && (u_font_margin&8)!=0);\n" \
  "  ivec2 tap=(neighbour?local:clamp(local,ivec2(0),u_font_crop.zw-ivec2(1)))+u_font_crop.xy;\n" \
  "  ivec2 size=textureSize(u_image,0);\n" \
  "  if(any(lessThan(tap,ivec2(0))) || any(greaterThanEqual(tap,size))) return ivec4(0);\n" \
  "  return ivec4(round(texelFetch(u_image,tap,0)*255.0));\n" \
  "}\n" \
  "ivec4 filtered_font_pixel(ivec4 prior){\n" \
  "  uvec2 one=uvec2(0u,0x3ff00000u),half_unit=uvec2(0u,0x3fe00000u);\n" \
  "  ivec2 pixel=ivec2(int(gl_FragCoord.x),int(u_extent.y)-1-int(gl_FragCoord.y));\n" \
  "  uvec2 px=d64_add(d64_int(pixel.x),half_unit),py=d64_add(d64_int(pixel.y),half_unit);\n" \
  "  uvec2 sx=d64_sub(d64_div(d64_sub(px,uvec2(u_font_origin.xy)),\n" \
  "                           uvec2(u_font_scale.xy)),half_unit);\n" \
  "  uvec2 sy=d64_sub(d64_div(d64_sub(py,uvec2(u_font_origin.zw)),\n" \
  "                           uvec2(u_font_scale.zw)),half_unit);\n" \
  "  ivec2 low=ivec2(d64_floor(sx),d64_floor(sy)),high=low+ivec2(1);\n" \
  "  uvec2 fx=d64_sub(sx,d64_int(low.x)),fy=d64_sub(sy,d64_int(low.y));\n" \
  "  uvec2 ix=d64_sub(one,fx),iy=d64_sub(one,fy);\n" \
  "  uvec2 wa=d64_mul(ix,iy),wb=d64_mul(fx,iy),wc=d64_mul(ix,fy),wd=d64_mul(fx,fy);\n" \
  "  ivec4 a=font_tpag_tap(ivec2(low.x,low.y)),b=font_tpag_tap(ivec2(high.x,low.y));\n" \
  "  ivec4 c=font_tpag_tap(ivec2(low.x,high.y)),d=font_tpag_tap(ivec2(high.x,high.y));\n" \
  "  uvec2 samples[4];\n" \
  "#if GML_EXACT_FONT_MODE>0\n" \
  "  samples[3]=font_channel(a,b,c,d,3,wa,wb,wc,wd);\n" \
  "  uvec2 sample_rgb=font_channel(a,b,c,d,u_font_channel,wa,wb,wc,wd);\n" \
  "#else\n" \
  "  for(int channel=0;channel<4;channel++) samples[channel]=font_channel(a,b,c,d,channel,wa,wb,wc,wd);\n" \
  "#endif\n" \
  "  uvec2 alpha=uvec2(u_font_alpha),denominator=d64_uint(255u),sa;\n" \
  "  uint source_alpha;\n" \
  "  if(u_font_mode==2){\n" \
  "    int aa=d64_round(samples[3]);\n" \
  "    if(aa<=0) return prior;\n" \
  "    sa=d64_mul(d64_div(d64_uint(uint(aa)),denominator),alpha);\n" \
  "    source_alpha=uint(d64_lround(d64_mul(d64_uint(uint(aa)),alpha)));\n" \
  "  } else {\n" \
  "    sa=d64_div(d64_mul(samples[3],alpha),denominator);\n" \
  "    if(d64_zero(sa)) return prior;\n" \
  "    source_alpha=uint(d64_lround(d64_mul(samples[3],alpha)));\n" \
  "  }\n" \
  "  uvec2 inverse=d64_sub(one,sa);\n" \
  "  ivec4 result=prior;\n" \
  "#if GML_EXACT_FONT_MODE>0\n" \
  "  int channel=u_font_channel; {\n" \
  "    uvec2 sample_value=sample_rgb;\n" \
  "#else\n" \
  "  for(int channel=0;channel<3;channel++){\n" \
  "    uvec2 sample_value=samples[channel];\n" \
  "#endif\n" \
  "    if(u_font_mode==2){\n" \
  "      uint tinted=uint(d64_round(sample_value))*uint(u_font_tint[channel])/255u;\n" \
  "      result[channel]=d64_round(d64_mul(d64_uint(tinted),sa))+\n" \
  "                       d64_round(d64_mul(d64_uint(uint(prior[channel])),inverse));\n" \
  "    } else {\n" \
  "      uvec2 tinted=d64_div(d64_mul(sample_value,d64_uint(uint(u_font_tint[channel]))),denominator);\n" \
  "      if(!limb_less(sa,one)) result[channel]=d64_round(tinted);\n" \
  "      else result[channel]=d64_round(d64_add(d64_mul(tinted,sa),\n" \
  "                                d64_mul(d64_uint(uint(prior[channel])),inverse)));\n" \
  "    }\n" \
  "  }\n" \
  "  result.rgb=clamp(result.rgb,ivec3(0),ivec3(255));\n" \
  "  result.a=u_font_preserve_alpha!=0\n" \
  "    ?int(min(255u,(source_alpha*source_alpha+uint(prior.a)*(255u-source_alpha)+127u)/255u)):255;\n" \
  "  return result;\n" \
  "}\n"

/* The eight-bit truncating source-over rule used by a characterised CPU leaf. Sampling the
 * previous destination from a copied, bounded region makes overlaps observe prior writes. */
#define GML_GPU_OPENGL_SCENE_EXACT_FRAGMENT_BODY \
  "uniform sampler2D u_image;\n" \
  "uniform sampler2D u_previous;\n" \
  "uniform vec2 u_extent;\n" \
  "uniform ivec2 u_region_origin;\n" \
  "uniform int u_repetitions;\n" \
  "uniform int u_blend_rule;\n" \
  "uniform ivec4 u_surface_rule;\n" \
  "uniform ivec4 u_surface_tint;\n" \
  "uniform ivec2 u_surface_alpha;\n" \
  "uniform ivec4 u_underlay_size;\n" \
  "uniform ivec2 u_underlay_origin;\n" \
  "uniform int u_underlay_flags;\n" \
  "uniform float u_additive_alpha;\n" \
  "uniform ivec4 u_effect_colour;\n" \
  "uniform float u_effect_intensity;\n" \
  "uniform vec4 u_effect_zoom;\n" \
  "uniform int u_effect_linear;\n" \
  "uniform int u_effect_seed;\n" \
  "in vec2 v_texcoord;\n" \
  "in vec4 v_colour;\n" \
  "out vec4 o_color;\n" \
  GML_GPU_OPENGL_FILTERED_FONT_BODY \
  "// R03_SHADER_AUTOMATIC_UNDERLAY: the host surface's point/box and coverage rules.\n" \
  "int underlay_point(int pixel,int source,int extent){\n" \
  "  return extent>=source?pixel*source/extent:((pixel*2+1)*source-1)/(extent*2);\n" \
  "}\n" \
  "ivec4 underlay_texel(ivec2 at){\n" \
  "  if((u_underlay_flags&8)!=0) at.y=u_underlay_size.y-1-at.y;\n" \
  "  return ivec4(floor(texelFetch(u_image,at,0)*255.0+0.5));\n" \
  "}\n" \
  "ivec4 automatic_underlay_pixel(ivec4 prior){\n" \
  "  ivec2 source=u_underlay_size.xy,extent=u_underlay_size.zw;\n" \
  "  ivec2 origin=u_underlay_origin;\n" \
  "  ivec2 p=ivec2(int(gl_FragCoord.x),int(u_extent.y)-1-int(gl_FragCoord.y))-origin;\n" \
  "  bool opaque=(u_underlay_flags&1)!=0,filtered=(u_underlay_flags&2)!=0;\n" \
  "  bool firstgen=(u_underlay_flags&4)!=0;\n" \
  "  if(all(equal(source,extent))){\n" \
  "    ivec4 sampled=underlay_texel(p);\n" \
  "    if(opaque || sampled.a==255) return sampled;\n" \
  "    if(sampled.a==0) return prior;\n" \
  "    if(firstgen) return ivec4((sampled.rgb*sampled.a+prior.rgb*(255-sampled.a)+127)/255,255);\n" \
  "    int factor=sampled.a*256/255;\n" \
  "    return ivec4((sampled.rgb*factor+prior.rgb*(256-factor))/256,255);\n" \
  "  }\n" \
  "  if(opaque && all(equal(origin,ivec2(0))) && all(equal(extent,ivec2(u_extent))) &&\n" \
  "     all(equal(extent%source,ivec2(0)))){\n" \
  "    ivec4 sampled=underlay_texel(p/(extent/source)); return ivec4(sampled.rgb,255);\n" \
  "  }\n" \
  "  if(opaque && !filtered && extent.x<source.x){\n" \
  "    int left=max(0,-origin.x),right=min(extent.x,int(u_extent.x)-origin.x);\n" \
  "    if(underlay_point(right-1,source.x,extent.x)-underlay_point(left,source.x,extent.x)==right-left-1)\n" \
  "      return underlay_texel(ivec2(underlay_point(p.x,source.x,extent.x),\n" \
  "                                  underlay_point(p.y,source.y,extent.y)));\n" \
  "  }\n" \
  "  ivec2 lo,hi;\n" \
  "  for(int axis=0;axis<2;axis++){\n" \
  "    lo[axis]=filtered?p[axis]*source[axis]/extent[axis]:\n" \
  "                       underlay_point(p[axis],source[axis],extent[axis]);\n" \
  "    hi[axis]=filtered && extent[axis]<source[axis]?\n" \
  "      max((p[axis]+1)*source[axis]/extent[axis],lo[axis]+1):lo[axis]+1;\n" \
  "    lo[axis]=max(lo[axis],0); hi[axis]=min(hi[axis],source[axis]);\n" \
  "  }\n" \
  "  ivec4 total=ivec4(0); int count=0;\n" \
  "  for(int y=lo.y;y<hi.y;y++) for(int x=lo.x;x<hi.x;x++){\n" \
  "    total+=underlay_texel(ivec2(x,y)); count++;\n" \
  "  }\n" \
  "  if(count==0 || total.a==0) return prior;\n" \
  "  ivec3 rgb=total.rgb/count;\n" \
  "  if(total.a==count*255) return ivec4(rgb,255);\n" \
  "  if(count==1 && firstgen) return ivec4((rgb*total.a+prior.rgb*(255-total.a)+127)/255,255);\n" \
  "  uvec2 one=uvec2(0u,0x3ff00000u);\n" \
  "  uvec2 alpha=d64_div(d64_div(d64_uint(uint(total.a)),d64_uint(uint(count))),d64_uint(255u));\n" \
  "  uvec2 inverse=d64_sub(one,alpha);\n" \
  "  ivec4 result=ivec4(0,0,0,255);\n" \
  "  for(int channel=0;channel<3;channel++)\n" \
  "    result[channel]=d64_floor(d64_add(d64_mul(d64_uint(uint(rgb[channel])),alpha),\n" \
  "                                      d64_mul(d64_uint(uint(prior[channel])),inverse)));\n" \
  "  return result;\n" \
  "}\n" \
  "vec4 effect_capture_sample(vec2 uv){\n" \
  "  vec2 position=clamp(uv,vec2(0.0),vec2(1.0))*u_extent;\n" \
  "  ivec2 maximum=ivec2(u_extent)-ivec2(1);\n" \
  "  if(u_effect_linear==0){\n" \
  "    ivec2 texel=clamp(ivec2(floor(position)),ivec2(0),maximum);\n" \
  "    return texelFetch(u_image,texel,0);\n" \
  "  }\n" \
  "  vec2 base=position-vec2(0.5);\n" \
  "  ivec2 low=ivec2(floor(base));\n" \
  "  ivec2 high=low+ivec2(1);\n" \
  "  vec2 weight=fract(base);\n" \
  "  low=clamp(low,ivec2(0),maximum);\n" \
  "  high=clamp(high,ivec2(0),maximum);\n" \
  "  vec4 a=mix(texelFetch(u_image,ivec2(low.x,low.y),0),\n" \
  "             texelFetch(u_image,ivec2(high.x,low.y),0),weight.x);\n" \
  "  vec4 b=mix(texelFetch(u_image,ivec2(low.x,high.y),0),\n" \
  "             texelFetch(u_image,ivec2(high.x,high.y),0),weight.x);\n" \
  "  return mix(a,b,weight.y);\n" \
  "}\n" \
  "#if GML_EXACT_FONT_MODE>0\n" \
  "void main(){\n" \
  "  ivec4 prior=ivec4(round(texelFetch(u_previous,ivec2(gl_FragCoord.xy)-u_region_origin,0)*255.0));\n" \
  "  o_color=vec4(filtered_font_pixel(prior))/255.0;\n" \
  "}\n" \
  "#else\n" \
  "void main(){\n" \
  "  ivec4 source=ivec4(round(texture(u_image,v_texcoord)*v_colour*255.0));\n" \
  "  ivec4 prior=ivec4(round(texelFetch(u_previous,ivec2(gl_FragCoord.xy)-u_region_origin,0)*255.0));\n" \
  "#if GML_EXACT_FONT_MODE<0\n" \
  "  if(u_blend_rule==7){ o_color=vec4(filtered_font_pixel(prior))/255.0; return; }\n" \
  "#endif\n" \
  "  if(u_blend_rule==8){ o_color=vec4(automatic_underlay_pixel(prior))/255.0; return; }\n" \
  "  // R03_SHADER_SURFACE_DESTINATION: rounded integer factors, independent RGBA equations.\n" \
  "  if(u_blend_rule==9){\n" \
  "    ivec2 source_size=u_underlay_size.xy,extent=u_underlay_size.zw;\n" \
  "    ivec2 p=ivec2(int(gl_FragCoord.x),int(u_extent.y)-1-int(gl_FragCoord.y))-u_underlay_origin;\n" \
  "    ivec2 lo=p*source_size/extent;\n" \
  "    ivec2 hi=(u_underlay_flags&2)!=0?max((p+ivec2(1))*source_size/extent,lo+ivec2(1)):lo+ivec2(1);\n" \
  "    hi=min(hi,source_size);\n" \
  "    ivec4 total=ivec4(0);int count=0;\n" \
  "    for(int y=lo.y;y<hi.y;y++) for(int x=lo.x;x<hi.x;x++){\n" \
  "      total+=underlay_texel(ivec2(x,y));count++;\n" \
  "    }\n" \
  "    if(count==0){o_color=vec4(prior)/255.0;return;}\n" \
  "    ivec4 sampled=total/count;\n" \
  "    sampled.rgb=sampled.rgb*u_surface_tint.rgb/255;\n" \
  "    // Positive rational channel averages round exactly at unit draw alpha.\n" \
  "    if(all(equal(u_surface_alpha,ivec2(0,0x3ff00000)))) sampled.a=(2*total.a+count)/(2*count);\n" \
  "    else {\n" \
  "      uvec2 average=count==1?d64_uint(uint(total.a)):d64_div(d64_uint(uint(total.a)),d64_uint(uint(count)));\n" \
  "      sampled.a=d64_lround(d64_mul(average,uvec2(u_surface_alpha)));\n" \
  "    }\n" \
  "    ivec4 result;\n" \
  "    for(int channel=0;channel<4;channel++){\n" \
  "      int equation=channel==3?u_surface_rule.z:u_surface_rule.y;\n" \
  "      int sf=u_surface_rule.x==11?prior.a:prior[channel];\n" \
  "      int df=u_surface_rule.x==9?sampled[channel]:prior[channel];\n" \
  "      int s=sampled[channel]*sf,d=prior[channel]*df;\n" \
  "      int scaled=equation==3?d-s:equation==4?s-d:s+d;\n" \
  "      int value=equation==2?max(sampled[channel],prior[channel]):\n" \
  "                equation==5?min(sampled[channel],prior[channel]):max(0,scaled+127)/255;\n" \
  "      result[channel]=min(value,255);\n" \
  "    }\n" \
  "    if(u_surface_rule.w==0) result.a=255;\n" \
  "    o_color=vec4(result)/255.0; return;\n" \
  "  }\n" \
  "  // R03_SHADER_LAYER_RGB_NOISE: the CPU hash and alpha-weighted RGB mix.\n" \
  "  if(u_blend_rule==5){\n" \
  "    uint x=uint(int(gl_FragCoord.x));\n" \
  "    uint y=uint(int(u_extent.y)-1-int(gl_FragCoord.y));\n" \
  "    uint h=x*0x9E3779B1u ^ (y+0x7F4A7C15u)*0x85EBCA77u ^ uint(u_effect_seed)*0xC2B2AE3Du;\n" \
  "    h^=h>>15; h*=0x2C1B3C6Du; h^=h>>12; h*=0x297A2D39u; h^=h>>15;\n" \
  "    uvec3 sample_rgb=uvec3(h&255u,(h>>8)&255u,(h>>16)&255u);\n" \
  "    uvec3 tint=uvec3(u_effect_colour.rgb);\n" \
  "    ivec3 noise=ivec3((sample_rgb*tint+uvec3(127u))/uvec3(255u));\n" \
  "    float weight=float(source.a)/255.0*u_effect_intensity;\n" \
  "    ivec3 rgb=ivec3(floor(vec3(source.rgb)+(vec3(noise)-vec3(source.rgb))*weight+0.5));\n" \
  "    o_color=vec4(vec3(clamp(rgb,ivec3(0),ivec3(255))),float(source.a))/255.0;\n" \
  "    return;\n" \
  "  }\n" \
  "  // R03_SHADER_LAYER_TINT_COLOURISE: transform premultiplied capture, then source-over.\n" \
  "  if(u_blend_rule==2 || u_blend_rule==3 || u_blend_rule==4){\n" \
  "    if(u_blend_rule!=4 && source.a==0){ o_color=vec4(prior)/255.0; return; }\n" \
  "    ivec4 filtered=source;\n" \
  "    if(u_blend_rule==2){\n" \
  "      filtered=ivec4(floor(vec4(source)*vec4(u_effect_colour)/255.0+0.5));\n" \
  "    } else if(u_blend_rule==3){\n" \
  "      vec3 weights=vec3(0.299,0.587,0.114);\n" \
  "      vec3 tint=vec3(u_effect_colour.rgb)/255.0;\n" \
  "      float tint_luma=dot(tint,weights);\n" \
  "      vec3 straight=vec3(source.rgb)/float(source.a);\n" \
  "      float luma=dot(straight,weights);\n" \
  "      vec3 ramp;\n" \
  "      if(luma<=tint_luma){\n" \
  "        float t=tint_luma>0.0?luma/tint_luma:0.0;\n" \
  "        ramp=tint*t;\n" \
  "      } else {\n" \
  "        float t=tint_luma<1.0?(luma-tint_luma)/(1.0-tint_luma):1.0;\n" \
  "        ramp=mix(tint,vec3(1.0),t);\n" \
  "      }\n" \
  "      vec3 result=mix(straight,ramp,clamp(u_effect_intensity,0.0,1.0))*float(source.a)/255.0;\n" \
  "      filtered.rgb=ivec3(floor(clamp(result,0.0,1.0)*255.0+0.5));\n" \
  "    } else {\n" \
  "      // R03_SHADER_LAYER_ZOOM_BLUR: sixteen bounded capture samples, then source-over.\n" \
  "      vec2 point=v_texcoord*u_extent;\n" \
  "      vec2 centre=vec2(u_effect_zoom.x,1.0-u_effect_zoom.y)*u_extent;\n" \
  "      float distance_to_centre=distance(point,centre);\n" \
  "      float edge0=u_effect_zoom.w;\n" \
  "      float edge1=edge0+max(distance_to_centre,1.0);\n" \
  "      float phase=clamp((distance_to_centre-edge0)/(edge1-edge0),0.0,1.0);\n" \
  "      float reach=u_effect_zoom.z*phase*phase*(3.0-2.0*phase);\n" \
  "      vec4 sum=vec4(0.0);\n" \
  "      for(int tap=0;tap<16;tap++){\n" \
  "        float amount=reach*float(tap)/16.0;\n" \
  "        sum+=effect_capture_sample(mix(point,centre,amount)/u_extent);\n" \
  "      }\n" \
  "      filtered=ivec4(floor(clamp(sum/16.0,0.0,1.0)*255.0+0.5));\n" \
  "    }\n" \
  "    if(filtered.a==0){ o_color=vec4(prior)/255.0; return; }\n" \
  "    vec3 retained=vec3(prior.rgb)*(1.0-float(filtered.a)/255.0);\n" \
  "    ivec3 colour=min(ivec3(255),filtered.rgb+ivec3(floor(retained+0.5)));\n" \
  "    o_color=vec4(vec3(colour)/255.0,1.0);\n" \
  "    return;\n" \
  "  }\n" \
  "  // R03_SHADER_ADDITIVE_BACKGROUND: exact saturated point-sampled bm_add RGB.\n" \
  "  if(u_blend_rule==1){\n" \
  "    if(source.a==0){ o_color=vec4(prior)/255.0; return; }\n" \
  "    float sa=float(source.a)/255.0*u_additive_alpha;\n" \
  "    ivec3 increment=ivec3(vec3(source.rgb)*sa);\n" \
  "    ivec3 colour=prior.rgb;\n" \
  "    for(int index=0;index<u_repetitions;index++) colour=min(ivec3(255),colour+increment);\n" \
  "    o_color=vec4(vec3(colour)/255.0,1.0);\n" \
  "    return;\n" \
  "  }\n" \
  "  // R03_SHADER_COARSE_VEIL_ROUND: exact rounded straight-alpha composition.\n" \
  "  if(u_blend_rule==6){\n" \
  "    if(source.a==0){ o_color=vec4(prior)/255.0; return; }\n" \
  "    ivec3 colour=prior.rgb;\n" \
  "    for(int index=0;index<u_repetitions;index++)\n" \
  "      colour=(source.rgb*source.a+colour*(255-source.a)+ivec3(127))/255;\n" \
  "    o_color=vec4(vec3(colour)/255.0,1.0);\n" \
  "    return;\n" \
  "  }\n" \
  "  int factor=source.a==255?256:source.a;\n" \
  "  // R03_SHADER_EXACT_OVERLAP: ordered destination-dependent eight-bit truncation.\n" \
  "  ivec3 colour=prior.rgb;\n" \
  "  for(int index=0;index<u_repetitions;index++)\n" \
  "    colour=(source.rgb*factor+colour*(256-factor))/256;\n" \
  "  o_color=vec4(vec3(colour)/255.0,1.0);\n" \
  "}\n" \
  "#endif\n"

#endif
