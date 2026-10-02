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

/* R03_SHADER_CHECKPOINT_TILE: exact comparison of two RGBA8 device images, with no filtering,
 * hashes, temporal lag or lossy colour arithmetic. XRGB canonical state deliberately ignores
 * source alpha; every other format compares it. Edge tiles fetch only existing texels. */
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
  "  if(u_filter>=2 && u_filter!=7 && u_filter!=8 && u_filter!=9 && u_filter!=10 &&\n" \
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
  "  if(u_filter==11){\n" \
  "    ivec2 logical=ivec2(v_span_x.x,v_span_y.x),dest=ivec2(v_span_x.y,v_span_y.y);\n" \
  "    // Add one denominator before division so edge phases never use negative remainder.\n" \
  "    ivec2 numerator=(ivec2(floor(delta))*2+ivec2(1))*logical+dest;\n" \
  "    ivec2 denominator=dest*2,remainder=numerator%denominator;\n" \
  "    lo=numerator/denominator-ivec2(1);\n" \
  "    f=vec2(stretched_float_phase(remainder.x,denominator.x),\n" \
  "           stretched_float_phase(remainder.y,denominator.y));\n" \
  "  } else if(u_filter>=2 && u_filter!=7 && u_filter!=8 && u_filter!=9 && u_filter!=10 &&\n" \
  "            u_filter!=12 && u_filter!=13){\n" \
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
  "  if(u_filter==11){\n" \
  "    vec4 weights=vec4((1.0-f.x)*(1.0-f.y),f.x*(1.0-f.y),\n" \
  "                      (1.0-f.x)*f.y,f.x*f.y);\n" \
  "    vec4 a=p00*weights.x,b=p01*weights.y,c=p10*weights.z,d=p11*weights.w;\n" \
  "    for(int channel=0;channel<4;channel++)\n" \
  "      weighted[channel]=stretched_float_add(stretched_float_add(\n" \
  "        stretched_float_add(a[channel],b[channel]),c[channel]),d[channel]);\n" \
  "  }\n" \
  "  vec4 sampled=floor(weighted+vec4(0.5));\n" \
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

/* The bounded filtered-font rule preserves the CPU's ordered binary64 expression. */
#define GML_GPU_OPENGL_FILTERED_FONT_BODY \
  "// R03_SHADER_FILTERED_FONT: bounded normal binary64, round-to-nearest-even.\n" \
  "// Integer limbs retain the CPU operations on both GL 3.3 and GLES 3.0.\n" \
  "uniform ivec4 u_font_origin;\n" \
  "uniform ivec4 u_font_scale;\n" \
  "uniform ivec2 u_font_alpha;\n" \
  "uniform ivec4 u_font_crop;\n" \
  "uniform ivec4 u_font_tint;\n" \
  "uniform int u_font_mode;\n" \
  "uniform int u_font_preserve_alpha;\n" \
  "bool d64_zero(uvec2 a){ return (a.x|(a.y&0x7fffffffu))==0u; }\n" \
  "bool limb_less(uvec2 a,uvec2 b){ return a.y<b.y || (a.y==b.y && a.x<b.x); }\n" \
  "uvec2 limb_add(uvec2 a,uvec2 b){\n" \
  "  uint lo=a.x+b.x; return uvec2(lo,a.y+b.y+uint(lo<a.x));\n" \
  "}\n" \
  "uvec2 limb_sub(uvec2 a,uvec2 b){ return uvec2(a.x-b.x,a.y-b.y-uint(a.x<b.x)); }\n" \
  "uvec2 limb_left(uvec2 a,int n){\n" \
  "  if(n==0) return a;\n" \
  "  if(n>=64) return uvec2(0u);\n" \
  "  if(n>=32) return uvec2(0u,a.x<<uint(n-32));\n" \
  "  return uvec2(a.x<<uint(n),(a.y<<uint(n))|(a.x>>uint(32-n)));\n" \
  "}\n" \
  "uvec2 limb_right(uvec2 a,int n){\n" \
  "  if(n==0) return a;\n" \
  "  if(n>=64) return uvec2(0u);\n" \
  "  if(n>=32) return uvec2(a.y>>uint(n-32),0u);\n" \
  "  return uvec2((a.x>>uint(n))|(a.y<<uint(32-n)),a.y>>uint(n));\n" \
  "}\n" \
  "uvec2 limb_jam(uvec2 a,int n){\n" \
  "  if(n==0) return a;\n" \
  "  if(n>=64) return uvec2(uint((a.x|a.y)!=0u),0u);\n" \
  "  uvec2 q=limb_right(a,n);\n" \
  "  uvec2 restored=limb_left(q,n);\n" \
  "  if(any(notEqual(restored,a))) q.x|=1u;\n" \
  "  return q;\n" \
  "}\n" \
  "uvec2 d64_mantissa(uvec2 a){ return uvec2(a.x,(a.y&0xfffffu)|0x100000u); }\n" \
  "int d64_exponent(uvec2 a){ return int((a.y>>20u)&2047u)-1023; }\n" \
  "uvec2 d64_pack(uvec2 extended,int exponent,uint sign){\n" \
  "  if((extended.x|extended.y)==0u) return uvec2(0u);\n" \
  "  if(extended.y>=0x1000000u){ extended=limb_jam(extended,1); exponent++; }\n" \
  "  while(extended.y<0x800000u){ extended=limb_left(extended,1); exponent--; }\n" \
  "  uint residue=extended.x&7u;\n" \
  "  uvec2 mantissa=limb_right(extended,3);\n" \
  "  if(residue>4u || (residue==4u && (mantissa.x&1u)!=0u))\n" \
  "    mantissa=limb_add(mantissa,uvec2(1u,0u));\n" \
  "  if(mantissa.y>=0x200000u){ mantissa=limb_right(mantissa,1); exponent++; }\n" \
  "  return uvec2(mantissa.x,sign|(uint(exponent+1023)<<20u)|(mantissa.y&0xfffffu));\n" \
  "}\n" \
  "uvec2 d64_uint(uint value){\n" \
  "  if(value==0u) return uvec2(0u);\n" \
  "  int exponent=0; uint leading=value;\n" \
  "  while(leading>=2u){ leading>>=1u; exponent++; }\n" \
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
  "  if(sa==sb) return d64_pack(limb_add(ma,mb),ea,sa);\n" \
  "  if(limb_less(ma,mb)) return d64_pack(limb_sub(mb,ma),ea,sb);\n" \
  "  return d64_pack(limb_sub(ma,mb),ea,sa);\n" \
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
  "  low=clamp(low,ivec2(0),u_font_crop.zw-ivec2(1))+u_font_crop.xy;\n" \
  "  high=clamp(high,ivec2(0),u_font_crop.zw-ivec2(1))+u_font_crop.xy;\n" \
  "  ivec4 a=ivec4(round(texelFetch(u_image,ivec2(low.x,low.y),0)*255.0));\n" \
  "  ivec4 b=ivec4(round(texelFetch(u_image,ivec2(high.x,low.y),0)*255.0));\n" \
  "  ivec4 c=ivec4(round(texelFetch(u_image,ivec2(low.x,high.y),0)*255.0));\n" \
  "  ivec4 d=ivec4(round(texelFetch(u_image,ivec2(high.x,high.y),0)*255.0));\n" \
  "  uvec2 samples[4];\n" \
  "  for(int channel=0;channel<4;channel++) samples[channel]=font_channel(a,b,c,d,channel,wa,wb,wc,wd);\n" \
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
  "  ivec4 result;\n" \
  "  for(int channel=0;channel<3;channel++){\n" \
  "    if(u_font_mode==2){\n" \
  "      uint tinted=uint(d64_round(samples[channel]))*uint(u_font_tint[channel])/255u;\n" \
  "      result[channel]=d64_round(d64_mul(d64_uint(tinted),sa))+\n" \
  "                       d64_round(d64_mul(d64_uint(uint(prior[channel])),inverse));\n" \
  "    } else {\n" \
  "      uvec2 tinted=d64_div(d64_mul(samples[channel],d64_uint(uint(u_font_tint[channel]))),denominator);\n" \
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
  "void main(){\n" \
  "  ivec4 source=ivec4(round(texture(u_image,v_texcoord)*v_colour*255.0));\n" \
  "  ivec4 prior=ivec4(round(texelFetch(u_previous,ivec2(gl_FragCoord.xy)-u_region_origin,0)*255.0));\n" \
  "  if(u_blend_rule==7){ o_color=vec4(filtered_font_pixel(prior))/255.0; return; }\n" \
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
  "}\n"

#endif
