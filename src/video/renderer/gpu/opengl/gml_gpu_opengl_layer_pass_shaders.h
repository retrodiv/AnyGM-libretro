/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#ifndef GML_GPU_OPENGL_LAYER_PASS_SHADERS_H
#define GML_GPU_OPENGL_LAYER_PASS_SHADERS_H

/* Integer byte sums fit binary32 for admitted axes. Prefix images are reduced
 * by eight; every draw covers one rectangular image, never CPU coverage rows. */
static const char GML_LAYER_PASS_VERTEX[]=
  "void main(){\n"
  "  vec2 p=vec2(float((gl_VertexID<<1)&2),float(gl_VertexID&2));\n"
  "  gl_Position=vec4(p*2.0-1.0,0.0,1.0);\n"
  "}\n"
;

static const char GML_LAYER_PASS_FRAGMENT[]=
  "uniform highp sampler2D u_source;\n"
  "uniform highp sampler2D u_prefix;\n"
  "uniform ivec2 u_size;\n"
  "uniform int u_axis,u_mode,u_step,u_radius,u_flip;\n"
  "layout(location=0) out vec4 o_colour;\n"
  "vec4 byte_sample(ivec2 p){\n"
  "  if(u_flip!=0)p.y=u_size.y-1-p.y;\n"
  "  return floor(texelFetch(u_source,p,0)*255.0+0.5);\n"
  "}\n"
  "vec4 prefix_sample(ivec2 p,int index){\n"
  "  if(index<0)return vec4(0.0);\n"
  "  p[u_axis]=index;\n"
  "  return texelFetch(u_prefix,p,0);\n"
  "}\n"
  "void main(){\n"
  "  ivec2 p=ivec2(gl_FragCoord.xy);\n"
  "  if(u_mode==0){\n"
  "    vec4 sum=vec4(0.0);\n"
  "    int first=p[u_axis]*8;\n"
  "    for(int i=0;i<8;i++){\n"
  "      ivec2 q=p;q[u_axis]=first+i;\n"
  "      if(q[u_axis]<u_size[u_axis])sum+=byte_sample(q);\n"
  "    }\n"
  "    o_colour=sum;return;\n"
  "  }\n"
  "  if(u_mode==1){\n"
  "    vec4 sum=texelFetch(u_source,p,0);\n"
  "    if(p[u_axis]>=u_step){ivec2 q=p;q[u_axis]-=u_step;sum+=texelFetch(u_source,q,0);}\n"
  "    o_colour=sum;return;\n"
  "  }\n"
  "  int position=p[u_axis],length=u_size[u_axis];\n"
  "  if(u_mode==3){\n"
  "    vec4 sum=vec4(0.0);\n"
  "    for(int i=-7;i<=7;i++)if(abs(i)<=u_radius){\n"
  "      ivec2 q=p;q[u_axis]=clamp(position+i,0,length-1);sum+=byte_sample(q);\n"
  "    }\n"
  "    o_colour=floor(clamp(sum/float(u_radius*2+1),0.0,255.0)+0.5)/255.0;return;\n"
  "  }\n"
  "  int lo=max(0,position-u_radius),hi=min(length-1,position+u_radius);\n"
  "  int first=(lo+7)/8,last=(hi+1)/8;\n"
  "  vec4 sum=vec4(0.0);\n"
  "  if(last>first)sum=prefix_sample(p,last-1)-prefix_sample(p,first-1);\n"
  "  for(int i=0;i<7;i++){\n"
  "    int left=lo+i,right=last*8+i;\n"
  "    ivec2 q=p;\n"
  "    if(left<min(hi+1,first*8)){q[u_axis]=left;sum+=byte_sample(q);}\n"
  "    if(right>=first*8 && right<=hi){q[u_axis]=right;sum+=byte_sample(q);}\n"
  "  }\n"
  "  ivec2 edge=p;edge[u_axis]=0;\n"
  "  sum+=float(max(0,u_radius-position))*byte_sample(edge);\n"
  "  edge[u_axis]=length-1;\n"
  "  sum+=float(max(0,position+u_radius-length+1))*byte_sample(edge);\n"
  "  o_colour=floor(clamp(sum/float(u_radius*2+1),0.0,255.0)+0.5)/255.0;\n"
  "}\n"
;

#endif
