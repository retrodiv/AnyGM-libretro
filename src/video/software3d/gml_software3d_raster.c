/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Fixed-function matrix, clipping, sampling, and raster pipeline. */
#include "gml_software3d_internal.h"
#include "anygm_host.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GML_GRAPHICS (graphics_state_for_render(R))
#define g_d3 (gml_render_backend_software3d(R)->d3)
#define g_d3_prim (GML_GRAPHICS->d3_prim)
#define g_d3_prim_kind (GML_GRAPHICS->d3_prim_kind)
#define g_d3_prim_texture (GML_GRAPHICS->d3_prim_texture)
#define g_d3_prim_n (GML_GRAPHICS->d3_prim_n)
#define g_vertex_buffer (GML_GRAPHICS->vertex_buffer)
#define g_prim_kind (GML_GRAPHICS->prim_kind)
#define g_prim_n (GML_GRAPHICS->prim_n)
#define g_prim_texture (GML_GRAPHICS->prim_texture)
#define g_prim_x (GML_GRAPHICS->prim_x)
#define g_prim_y (GML_GRAPHICS->prim_y)
#define g_prim_u (GML_GRAPHICS->prim_u)
#define g_prim_v (GML_GRAPHICS->prim_v)
#define g_prim_a (GML_GRAPHICS->prim_a)
#define g_prim_c (GML_GRAPHICS->prim_c)

static GmlSoftware3D *graphics_state_for_render(GmlRender *render){
  return gml_render_backend_software3d(render);
}

static double d3_dot(const double a[3], const double b[3]){
  return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}
static int d3_normalize(double v[3]){
  double length=sqrt(d3_dot(v,v));
  if(length<1e-12) return 0;
  v[0]/=length; v[1]/=length; v[2]/=length;
  return 1;
}
static void d3_cross(const double a[3], const double b[3], double out[3]){
  out[0]=a[1]*b[2]-a[2]*b[1];
  out[1]=a[2]*b[0]-a[0]*b[2];
  out[2]=a[0]*b[1]-a[1]*b[0];
}
static void d3_matrix_multiply(const double left[16],const double right[16],double out[16]){
  double result[16];
  for(int row=0;row<4;row++) for(int col=0;col<4;col++){
    double sum=0;
    for(int k=0;k<4;k++) sum+=left[row+k*4]*right[k+col*4];
    result[row+col*4]=sum;
  }
  memcpy(out,result,sizeof(result));
}
static void d3_matrix_prepend(GmlRender *R,const double added[16]){
  d3_matrix_multiply(added,g_d3.transform,g_d3.transform);
}
static void d3_matrix_translation(double out[16],double x,double y,double z){
  gml_software3d_matrix_identity(out); out[12]=x; out[13]=y; out[14]=z;
}
static void d3_matrix_scaling(double out[16],double x,double y,double z){
  gml_software3d_matrix_identity(out); out[0]=x; out[5]=y; out[10]=z;
}
static void d3_matrix_rotation_axis(double out[16],double x,double y,double z,double degrees){
  double axis[3]={x,y,z};
  if(!d3_normalize(axis)){ gml_software3d_matrix_identity(out); return; }
  x=axis[0]; y=axis[1]; z=axis[2];
  double angle=degrees*M_PI/180.0,c=cos(angle),s=sin(angle),t=1-c;
  gml_software3d_matrix_identity(out);
  out[0]=t*x*x+c;   out[4]=t*x*y-s*z; out[8]=t*x*z+s*y;
  out[1]=t*x*y+s*z; out[5]=t*y*y+c;   out[9]=t*y*z-s*x;
  out[2]=t*x*z-s*y; out[6]=t*y*z+s*x; out[10]=t*z*z+c;
}

double gml_software3d_dot(const double first[3],const double second[3]){
  return d3_dot(first,second);
}
int gml_software3d_normalize(double vector[3]){
  return d3_normalize(vector);
}
void gml_software3d_cross(const double first[3],const double second[3],double output[3]){
  d3_cross(first,second,output);
}
void gml_software3d_matrix_multiply(const double left[16],const double right[16],double output[16]){
  d3_matrix_multiply(left,right,output);
}
int gml_software3d_matrix_set(GmlRender *render,int type,
                              const double matrix[16]){
  GmlSoftware3D *graphics=graphics_state_for_render(render);
  if(!graphics || !matrix) return 0;
  if(type==GML_SOFTWARE3D_MATRIX_VIEW)
    memcpy(graphics->matrix_view,matrix,sizeof(graphics->matrix_view));
  else if(type==GML_SOFTWARE3D_MATRIX_PROJECTION)
    memcpy(graphics->matrix_projection,matrix,sizeof(graphics->matrix_projection));
  else if(type==GML_SOFTWARE3D_MATRIX_TRANSFORM){
    memcpy(graphics->d3.transform,matrix,sizeof(graphics->d3.transform));
    gml_d3_sync_render_camera(render);
  } else return 0;
  return 1;
}
int gml_software3d_matrix_prepend(GmlRender *render,const double added[16]){
  if(!render || !added || !graphics_state_for_render(render)) return 0;
  d3_matrix_prepend(render,added);
  gml_d3_sync_render_camera(render);
  return 1;
}
int gml_software3d_transform_stack(GmlRender *render,int action){
  GmlSoftware3D *graphics=graphics_state_for_render(render);
  if(!graphics) return 0;
  GmlD3State *d3=&graphics->d3;
  if(action==GML_SOFTWARE3D_STACK_CLEAR){
    d3->transform_stack_n=0;
    return 1;
  }
  if(action==GML_SOFTWARE3D_STACK_EMPTY)
    return d3->transform_stack_n==0;
  if(action==GML_SOFTWARE3D_STACK_PUSH){
    if(d3->transform_stack_n>=32) return 0;
    memcpy(d3->transform_stack[d3->transform_stack_n++],d3->transform,
           sizeof(d3->transform));
    return 1;
  }
  if(action==GML_SOFTWARE3D_STACK_POP){
    if(d3->transform_stack_n<=0) return 0;
    memcpy(d3->transform,d3->transform_stack[--d3->transform_stack_n],
           sizeof(d3->transform));
    gml_d3_sync_render_camera(render);
    return 1;
  }
  if(action==GML_SOFTWARE3D_STACK_TOP){
    if(d3->transform_stack_n<=0) return 0;
    memcpy(d3->transform,d3->transform_stack[d3->transform_stack_n-1],
           sizeof(d3->transform));
    gml_d3_sync_render_camera(render);
    return 1;
  }
  if(action==GML_SOFTWARE3D_STACK_DISCARD){
    if(d3->transform_stack_n<=0) return 0;
    d3->transform_stack_n--;
    return 1;
  }
  return 0;
}
void gml_software3d_matrix_translation(double output[16],double x,double y,double z){
  d3_matrix_translation(output,x,y,z);
}
void gml_software3d_matrix_scaling(double output[16],double x,double y,double z){
  d3_matrix_scaling(output,x,y,z);
}
void gml_software3d_matrix_rotation_axis(double output[16],double x,double y,double z,double degrees){
  d3_matrix_rotation_axis(output,x,y,z,degrees);
}

void gml_d3_sync_render_camera(GmlRender *R){
  if(!R) return;
  GmlSoftware3D *graphics=graphics_state_for_render(R);
  if(!graphics){
    gml_render_backend_sync_camera(R,0,0);
    return;
  }
  GmlD3State *d3=&graphics->d3;
  const double eps=1e-10;
  int translation=fabs(d3->transform[0]-1)<eps && fabs(d3->transform[5]-1)<eps &&
    fabs(d3->transform[10]-1)<eps && fabs(d3->transform[15]-1)<eps;
  for(int i=0;i<12 && translation;i++)
    if(i!=0 && i!=5 && i!=10 && fabs(d3->transform[i])>=eps) translation=0;
  gml_render_backend_sync_camera(R,translation?d3->transform[12]:0,
                                 translation?d3->transform[13]:0);
}
static void d3_transform_point(GmlRender *R,double *x,double *y,double *z){
  double inx=*x,iny=*y,inz=*z;
  *x=g_d3.transform[0]*inx+g_d3.transform[4]*iny+g_d3.transform[8]*inz+g_d3.transform[12];
  *y=g_d3.transform[1]*inx+g_d3.transform[5]*iny+g_d3.transform[9]*inz+g_d3.transform[13];
  *z=g_d3.transform[2]*inx+g_d3.transform[6]*iny+g_d3.transform[10]*inz+g_d3.transform[14];
}
static int d3_depth_prepare(GmlRender *R){
  GmlRenderBackendDrawView draw;
  if(!gml_render_backend_draw_view(R,&draw) || draw.width<=0 || draw.height<=0) return 0;
  size_t count=(size_t)draw.width*(size_t)draw.height;
  if(count>g_d3.depth_cap){
    float *depth=(float*)realloc(g_d3.depth,count*sizeof(*depth));
    if(!depth) return 0;
    g_d3.depth=depth; g_d3.depth_cap=count;
  }
  if(g_d3.depth_frame!=draw.frame || g_d3.depth_w!=draw.width || g_d3.depth_h!=draw.height){
    memset(g_d3.depth,0,count*sizeof(*g_d3.depth));
    g_d3.depth_frame=draw.frame; g_d3.depth_w=draw.width; g_d3.depth_h=draw.height;
  }
  return 1;
}
static int d3_texture_from_backend_view(const GmlRenderBackendTextureView *view,
                                        GmlD3Texture *out){
  if(!view || view->width<=0 || view->height<=0 || view->stride<=0) return 0;
  if(view->pixel_kind==GML_RENDER_BACKEND_PIXELS_RGBA && view->rgba){
    if(out) *out=(GmlD3Texture){.kind=1,.w=view->width,.h=view->height,
      .stride=view->stride,.offset_x=view->source_x,.offset_y=view->source_y,
      .rgba=view->rgba};
    return 1;
  }
  if(view->pixel_kind==GML_RENDER_BACKEND_PIXELS_XRGB && view->xrgb){
    if(out) *out=(GmlD3Texture){.kind=2,.w=view->width,.h=view->height,
      .stride=view->stride,.xrgb=view->xrgb};
    return 1;
  }
  return 0;
}
static int d3_texture(GmlRender *R, int handle, GmlD3Texture *out){
  if(out) memset(out,0,sizeof(*out));
  GmlRenderBackendTextureView view;
  if(!gml_render_backend_texture_view(R,handle,0,&view)) return 0;
  GmlRenderBackendDrawView draw;
  const struct AnygmHostServices *host=
    gml_render_backend_draw_view(R,&draw)?draw.host:NULL;
  if(((uint32_t)handle&GML_TEX_KIND_MASK)==GML_TEX_BG_TAG &&
     anygm_host_development_setting(host,"GML_LOG_D3D") &&
     GML_GRAPHICS && GML_GRAPHICS->d3_texture_log_count++<8)
    anygm_host_logf(host,ANYGM_LOG_DEBUG,
                    "[d3d] texture bg=%d tpag=%d atlas=%d rect=%dx%d\n",
                    view.resource_index,view.page_index,view.atlas_index,
                    view.width,view.height);
  return d3_texture_from_backend_view(&view,out);
}
/* vertex_texcoord receives texture-page UVs (for example those returned by
 * texture_get_uvs), whereas the legacy d3d helpers use sprite-local 0..1 UVs.
 * Resolve tagged sprite/background handles to the full atlas for this API. */
static int vertex_texture(GmlRender *R,int handle,GmlD3Texture *out){
  if(out) memset(out,0,sizeof(*out));
  if(handle==-1) return 1;
  GmlRenderBackendTextureView view;
  if(!gml_render_backend_texture_view(R,handle,1,&view)) return 0;
  return d3_texture_from_backend_view(&view,out);
}
static void d3_texture_texel(const GmlD3Texture *texture,int x,int y,int channel[4]){
  if(texture->kind==1){
    const uint8_t *pixel=texture->rgba+((size_t)(texture->offset_y+y)*texture->stride+texture->offset_x+x)*4;
    for(int c=0;c<4;c++) channel[c]=pixel[c];
  } else {
    uint32_t pixel=texture->xrgb[(size_t)y*texture->stride+x];
    channel[0]=(pixel>>16)&255; channel[1]=(pixel>>8)&255;
    channel[2]=pixel&255; channel[3]=(pixel>>24)&255;
  }
}
static void d3_sample(GmlRender *R,const GmlRenderBackendDrawView *draw,
                      const GmlD3Texture *texture,double u,double v,double channel[4]){
  if(!isfinite(u) || !isfinite(v)){
    for(int c=0;c<4;c++) channel[c]=0;
    return;
  }
  u-=floor(u); v-=floor(v);
  double fx=u*texture->w-0.5, fy=v*texture->h-0.5;
  int x0=(int)floor(fx), y0=(int)floor(fy);
  double ax=fx-floor(fx), ay=fy-floor(fy);
  x0%=texture->w; y0%=texture->h;
  if(x0<0) x0+=texture->w;
  if(y0<0) y0+=texture->h;
  if(!draw->interpolate){
    x0=(x0+(ax>=0.5))%texture->w;
    y0=(y0+(ay>=0.5))%texture->h;
    int nearest[4];
    d3_texture_texel(texture,x0,y0,nearest);
    for(int c=0;c<4;c++) channel[c]=nearest[c];
    return;
  }
  int x1=(x0+1)%texture->w, y1=(y0+1)%texture->h;
  int p[4][4];
  d3_texture_texel(texture,x0,y0,p[0]); d3_texture_texel(texture,x1,y0,p[1]);
  d3_texture_texel(texture,x0,y1,p[2]); d3_texture_texel(texture,x1,y1,p[3]);
  if(draw->interpolate && g_d3.classic){
    /* The classic fixed-function filter quantizes each texture fraction to eight bits and
     * rounds after both the horizontal and vertical lerps. Keeping the two stages separate is
     * observable: collapsing them to one floating-point expression moves many channels by one. */
    enum { BITS=8, LEVELS=1<<BITS, BIAS=LEVELS/2 };
    int ix=(int)floor(ax*LEVELS+.5),iy=(int)floor(ay*LEVELS+.5);
    if(ix<0) ix=0; else if(ix>LEVELS) ix=LEVELS;
    if(iy<0) iy=0; else if(iy>LEVELS) iy=LEVELS;
    for(int c=0;c<4;c++){
      int top=(p[0][c]*(LEVELS-ix)+p[1][c]*ix+BIAS)>>BITS;
      int bottom=(p[2][c]*(LEVELS-ix)+p[3][c]*ix+BIAS)>>BITS;
      channel[c]=(top*(LEVELS-iy)+bottom*iy+BIAS)>>BITS;
    }
    return;
  }
  double w[4]={(1-ax)*(1-ay),ax*(1-ay),(1-ax)*ay,ax*ay};
  for(int c=0;c<4;c++) channel[c]=p[0][c]*w[0]+p[1][c]*w[1]+p[2][c]*w[2]+p[3][c]*w[3];
}
static double d3_edge(double ax,double ay,double bx,double by,double px,double py){
  return (px-ax)*(by-ay)-(py-ay)*(bx-ax);
}
static void d3_raster_triangle(GmlRender *R, const GmlD3Vertex in[3], const GmlD3Texture *texture){
  GmlRenderBackendDrawView draw;
  if(!d3_depth_prepare(R) || !gml_render_backend_draw_view(R,&draw) ||
     !draw.pixels || draw.width<=0 || draw.height<=0) return;
  const char *fov_text=anygm_host_development_setting(draw.host,"GML_D3D_FOV");
  double fov=fov_text?atof(fov_text):g_d3.fov;
  if(fov<1.0 || fov>170.0) fov=41.2;
  double tangent=tan(fov*M_PI/360.0);
  double focal_y=(draw.height*0.5)/tangent;
  double focal_x=g_d3.aspect>1e-9?(draw.width*0.5)/(tangent*g_d3.aspect):focal_y;
  double sx[3],sy[3],iz[3],uz[3],vz[3],riz[3],giz[3],biz[3],aiz[3];
  double depth_value[3],view_distance[3];
  int flat_color=in[0].r==in[1].r&&in[0].r==in[2].r&&
                 in[0].g==in[1].g&&in[0].g==in[2].g&&
                 in[0].b==in[1].b&&in[0].b==in[2].b&&
                 in[0].alpha==in[1].alpha&&in[0].alpha==in[2].alpha;
  int opaque_white=flat_color&&in[0].r==255.0&&in[0].g==255.0&&in[0].b==255.0&&
                   in[0].alpha>=1.0&&!g_d3.lighting&&!g_d3.fog&&
                   (!draw.alpha_blend||draw.blend_mode==0);
  /* The fixed-function viewport uses a half-pixel anchor represented just below 0.5.
   * Keeping the 11-bit phase avoids pushing boundary samples into the next texel. */
  double pixel_offset=g_d3.classic?0.5-1.0/2048.0:0.0;
  /* Bias the vertices once: perspective interpolation preserves a constant phase exactly, while
   * keeping this work out of the per-pixel inner loop. */
  double u_phase=(g_d3.classic&&!draw.interpolate)?1.0/65536.0:0.0;
  double v_phase=(g_d3.classic&&!draw.interpolate)?-1.0/524288.0:0.0;
  for(int i=0;i<3;i++){
    if(g_d3.ortho){
      double ow=fabs(g_d3.ortho_w)>1e-9?g_d3.ortho_w:1;
      double oh=fabs(g_d3.ortho_h)>1e-9?g_d3.ortho_h:1;
      iz[i]=1; uz[i]=in[i].u+u_phase; vz[i]=in[i].v+v_phase;
      if(!flat_color){
        riz[i]=in[i].r; giz[i]=in[i].g; biz[i]=in[i].b; aiz[i]=in[i].alpha;
      }
      sx[i]=in[i].x*draw.width/ow+pixel_offset; sy[i]=in[i].y*draw.height/oh+pixel_offset;
      depth_value[i]=1000000.0-in[i].z;
      view_distance[i]=fabs(in[i].z);
    } else {
      if(in[i].z<=1e-6) return;
      iz[i]=1.0/in[i].z; uz[i]=(in[i].u+u_phase)*iz[i]; vz[i]=(in[i].v+v_phase)*iz[i];
      if(!flat_color){
        riz[i]=in[i].r*iz[i]; giz[i]=in[i].g*iz[i]; biz[i]=in[i].b*iz[i]; aiz[i]=in[i].alpha*iz[i];
      }
      sx[i]=draw.width*0.5+in[i].x*focal_x*iz[i]+pixel_offset;
      sy[i]=draw.height*0.5-in[i].y*focal_y*iz[i]+pixel_offset;
      depth_value[i]=iz[i];
      view_distance[i]=in[i].z;
    }
  }
  double area=d3_edge(sx[0],sy[0],sx[1],sy[1],sx[2],sy[2]);
  if(fabs(area)<1e-9) return;
  if(g_d3.culling && area>=0) return;
  int minx=(int)floor(fmin(sx[0],fmin(sx[1],sx[2]))), maxx=(int)ceil(fmax(sx[0],fmax(sx[1],sx[2])));
  int miny=(int)floor(fmin(sy[0],fmin(sy[1],sy[2]))), maxy=(int)ceil(fmax(sy[0],fmax(sy[1],sy[2])));
  if(minx<0) minx=0;
  if(miny<0) miny=0;
  if(maxx>=draw.width) maxx=draw.width-1;
  if(maxy>=draw.height) maxy=draw.height-1;
  double inv_area=1.0/area;
  double edge0_step=sy[2]-sy[1],edge1_step=sy[0]-sy[2];
  int opaque_prepared=0;
  for(int y=miny;y<=maxy;y++){
    double py=y+0.5,px=minx+0.5;
    double edge0=d3_edge(sx[1],sy[1],sx[2],sy[2],px,py);
    double edge1=d3_edge(sx[2],sy[2],sx[0],sy[0],px,py);
    for(int x=minx;x<=maxx;x++,edge0+=edge0_step,edge1+=edge1_step){
    double b0=edge0*inv_area;
    double b1=edge1*inv_area;
    double b2=1.0-b0-b1;
    if(b0<-1e-9 || b1<-1e-9 || b2<-1e-9) continue;
    double invz=b0*iz[0]+b1*iz[1]+b2*iz[2];
    double ztest=b0*depth_value[0]+b1*depth_value[1]+b2*depth_value[2];
    size_t di=(size_t)y*draw.width+x;
    /* The fixed-function hidden-surface path accepts fragments at the current depth
     * (the usual LESS_EQUAL comparison).  A strict comparison makes coplanar 2D batches eat
     * one another: text glyphs and quads are emitted as multiple triangles at one layer depth. */
    if(g_d3.hidden && ztest<g_d3.depth[di]) continue;
    double sampled[4]={255,255,255,255};
    if(texture&&texture->kind){
      double u=(b0*uz[0]+b1*uz[1]+b2*uz[2])/invz;
      double v=(b0*vz[0]+b1*vz[1]+b2*vz[2])/invz;
      d3_sample(R,&draw,texture,u,v,sampled);
    }
    if(opaque_white&&sampled[3]>=255.0){
      if(!opaque_prepared){
        gml_render_backend_prepare_draw(R);
        if(!gml_render_backend_draw_view(R,&draw) || !draw.pixels) return;
        opaque_prepared=1;
      }
      draw.pixels[di]=0xFF000000u|((uint32_t)sampled[0]<<16)|
                      ((uint32_t)sampled[1]<<8)|(uint32_t)sampled[2];
      if(g_d3.hidden&&g_d3.zwrite) g_d3.depth[di]=(float)ztest;
      continue;
    }
    double vr=in[0].r,vg=in[0].g,vb=in[0].b,vertex_alpha=in[0].alpha;
    if(!flat_color){
      vr=(b0*riz[0]+b1*riz[1]+b2*riz[2])/invz;
      vg=(b0*giz[0]+b1*giz[1]+b2*giz[2])/invz;
      vb=(b0*biz[0]+b1*biz[1]+b2*biz[2])/invz;
      vertex_alpha=(b0*aiz[0]+b1*aiz[1]+b2*aiz[2])/invz;
    }
    if(vr<0)vr=0; else if(vr>255)vr=255;
    if(vg<0)vg=0; else if(vg>255)vg=255;
    if(vb<0)vb=0; else if(vb>255)vb=255;
    int mod_r=(int)lround(sampled[0]*vr/255.0);
    int mod_g=(int)lround(sampled[1]*vg/255.0);
    int mod_b=(int)lround(sampled[2]*vb/255.0);
    uint32_t color=(uint32_t)mod_r|((uint32_t)mod_g<<8)|((uint32_t)mod_b<<16);
    if(g_d3.lighting){
      int cr=(int)((color&255)*g_d3.shade_r), cg=(int)(((color>>8)&255)*g_d3.shade_g);
      int cb=(int)(((color>>16)&255)*g_d3.shade_b);
      if(cr>255) cr=255;
      if(cg>255) cg=255;
      if(cb>255) cb=255;
      color=(uint32_t)cr|((uint32_t)cg<<8)|((uint32_t)cb<<16);
    }
    if(g_d3.fog){
      double distance=g_d3.ortho?
        b0*view_distance[0]+b1*view_distance[1]+b2*view_distance[2]:1.0/invz;
      double span=g_d3.fog_end-g_d3.fog_start;
      double amount=span>1e-9?(distance-g_d3.fog_start)/span:(distance>=g_d3.fog_end?1:0);
      if(amount<0) amount=0; else if(amount>1) amount=1;
      uint32_t fog=g_d3.fog_color;
      int cr=(int)lround((color&255)*(1-amount)+(fog&255)*amount);
      int cg=(int)lround(((color>>8)&255)*(1-amount)+((fog>>8)&255)*amount);
      int cb=(int)lround(((color>>16)&255)*(1-amount)+((fog>>16)&255)*amount);
      color=(uint32_t)cr|((uint32_t)cg<<8)|((uint32_t)cb<<16);
    }
    double alpha=(sampled[3]/255.0)*vertex_alpha;
    gml_render_backend_draw_pixel_alpha(R,x,y,color,alpha);
    if(g_d3.hidden && g_d3.zwrite && alpha>0.0) g_d3.depth[di]=(float)ztest;
  }
  }
}
static GmlD3Vertex d3_camera_vertex(GmlRender *R,double x,double y,double z,double u,double v){
  d3_transform_point(R,&x,&y,&z);
  if(g_d3.ortho){
    double dx=x-g_d3.ortho_x,dy=y-g_d3.ortho_y;
    double angle=g_d3.ortho_angle*M_PI/180.0,cs=cos(angle),sn=sin(angle);
    GmlD3Vertex out={0};
    out.x=cs*dx+sn*dy; out.y=-sn*dx+cs*dy; out.z=z; out.u=u; out.v=v;
    return out;
  }
  double d[3]={x-g_d3.eye[0],y-g_d3.eye[1],z-g_d3.eye[2]};
  GmlD3Vertex out={0};
  out.x=gml_software3d_dot(d,g_d3.right); out.y=gml_software3d_dot(d,g_d3.up);
  out.z=gml_software3d_dot(d,g_d3.forward); out.u=u; out.v=v;
  return out;
}
static GmlD3Vertex d3_vertex_lerp(GmlD3Vertex a,GmlD3Vertex b,double amount){
  GmlD3Vertex out;
#define D3_LERP(field) out.field=a.field+(b.field-a.field)*amount
  D3_LERP(x); D3_LERP(y); D3_LERP(z); D3_LERP(u); D3_LERP(v);
  D3_LERP(r); D3_LERP(g); D3_LERP(b); D3_LERP(alpha);
  D3_LERP(nx); D3_LERP(ny); D3_LERP(nz);
#undef D3_LERP
  out.has_normal=a.has_normal||b.has_normal;
  return out;
}
static int d3_clip_z(const GmlD3Vertex *input,int count,GmlD3Vertex *output,double plane,int keep_greater){
  int n=0;
  for(int i=0;i<count;i++){
    GmlD3Vertex a=input[i], b=input[(i+1)%count];
    int ain=keep_greater?a.z>=plane:a.z<=plane;
    int bin=keep_greater?b.z>=plane:b.z<=plane;
    if(ain) output[n++]=a;
    if(ain!=bin){
      double k=(plane-a.z)/(b.z-a.z);
      output[n]=d3_vertex_lerp(a,b,k); output[n++].z=plane;
    }
  }
  return n;
}
static void d3_emit_triangle(GmlRender *R,const GmlD3Vertex world[3],const GmlD3Texture *texture);
static void d3_draw_quad(GmlRender *R, const double p[4][3], int texture, double hrep, double vrep,
                         uint32_t vertex_color,int reverse_normal,int uv_mode){
  GmlRenderBackendDrawView draw;
  if(!gml_render_backend_draw_view(R,&draw)) return;
  GmlD3Texture resolved={0};
  if(texture!=-1) d3_texture(R,texture,&resolved);
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  double normal[3]={0,0,1};
  if(g_d3.lighting){
    double e1[3]={p[1][0]-p[0][0],p[1][1]-p[0][1],p[1][2]-p[0][2]};
    double e2[3]={p[2][0]-p[0][0],p[2][1]-p[0][1],p[2][2]-p[0][2]};
    gml_software3d_cross(e1,e2,normal); gml_software3d_normalize(normal);
    if(reverse_normal) for(int i=0;i<3;i++) normal[i]=-normal[i];
    double center[3]={0,0,0}; for(int i=0;i<4;i++) for(int k=0;k<3;k++) center[k]+=p[i][k]*.25;
    double lr=(g_d3.ambient_color&255)/255.0;
    double lg=((g_d3.ambient_color>>8)&255)/255.0;
    double lb=((g_d3.ambient_color>>16)&255)/255.0;
    for(int i=0;i<8;i++) if(g_d3.light[i].defined&&g_d3.light[i].enabled){
      uint32_t col=g_d3.light[i].color;
      if(g_d3.light[i].range<0){
        double ray[3]={-g_d3.light[i].x,-g_d3.light[i].y,-g_d3.light[i].z};
        if(!gml_software3d_normalize(ray)) continue;
        double diffuse=fmax(0.0,gml_software3d_dot(normal,ray));
        lr+=diffuse*(col&255)/255.0; lg+=diffuse*((col>>8)&255)/255.0; lb+=diffuse*((col>>16)&255)/255.0;
        continue;
      }
      double ray[3]={g_d3.light[i].x-center[0],g_d3.light[i].y-center[1],g_d3.light[i].z-center[2]};
      double dist=sqrt(gml_software3d_dot(ray,ray)); if(dist<=1e-9 || dist>=g_d3.light[i].range) continue;
      ray[0]/=dist; ray[1]/=dist; ray[2]/=dist;
      double diffuse=fmax(0.0,gml_software3d_dot(normal,ray))/(1.0+4.0*dist/g_d3.light[i].range);
      lr+=diffuse*(col&255)/255.0; lg+=diffuse*((col>>8)&255)/255.0; lb+=diffuse*((col>>16)&255)/255.0;
    }
    g_d3.shade_r=lr; g_d3.shade_g=lg; g_d3.shade_b=lb;
  }
  static const double uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
  if(g_d3.lighting&&g_d3.smooth){
    GmlD3Vertex world[4];
    for(int i=0;i<4;i++){
      double u=uv[i][0],v=uv[i][1];
      if(uv_mode&4){ double swap=u; u=v; v=swap; }
      if(uv_mode&1) u=1-u;
      if(uv_mode&2) v=1-v;
      memset(&world[i],0,sizeof(world[i]));
      world[i].x=p[i][0]; world[i].y=p[i][1]; world[i].z=p[i][2];
      world[i].u=u*hrep; world[i].v=v*vrep;
      world[i].r=vertex_color&255; world[i].g=(vertex_color>>8)&255;
      world[i].b=(vertex_color>>16)&255; world[i].alpha=draw.alpha;
      world[i].nx=normal[0]; world[i].ny=normal[1]; world[i].nz=normal[2]; world[i].has_normal=1;
    }
    GmlD3Vertex first[3]={world[0],world[1],world[2]};
    GmlD3Vertex second[3]={world[0],world[2],world[3]};
    d3_emit_triangle(R,first,&resolved); d3_emit_triangle(R,second,&resolved);
    return;
  }
  GmlD3Vertex q[4], clipped[12], far_clipped[12];
  for(int i=0;i<4;i++){
    double u=uv[i][0],v=uv[i][1];
    if(uv_mode&4){ double swap=u; u=v; v=swap; }
    if(uv_mode&1) u=1-u;
    if(uv_mode&2) v=1-v;
    q[i]=d3_camera_vertex(R,p[i][0],p[i][1],p[i][2],u*hrep,v*vrep);
    q[i].r=vertex_color&255; q[i].g=(vertex_color>>8)&255;
    q[i].b=(vertex_color>>16)&255; q[i].alpha=draw.alpha;
  }
  int count;
  if(g_d3.ortho){ memcpy(clipped,q,sizeof(q)); count=4; }
  else {
    double nearz=g_d3.near_clip>1e-6?g_d3.near_clip:.05;
    double farz=g_d3.far_clip>nearz?g_d3.far_clip:32000;
    count=d3_clip_z(q,4,clipped,nearz,1);
    if(count>0){ count=d3_clip_z(clipped,count,far_clipped,farz,0); memcpy(clipped,far_clipped,(size_t)count*sizeof(*clipped)); }
  }
  for(int i=1;i+1<count;i++){
    GmlD3Vertex tri[3]={clipped[0],clipped[i],clipped[i+1]};
    d3_raster_triangle(R,tri,&resolved);
  }
}
static void d3_set_camera(GmlRender *R,double xfrom,double yfrom,double zfrom,
                          double xto,double yto,double zto,
                          double xup,double yup,double zup){
  g_d3.ortho=0; g_d3.perspective=1;
  g_d3.eye[0]=xfrom; g_d3.eye[1]=yfrom; g_d3.eye[2]=zfrom;
  g_d3.forward[0]=xto-xfrom; g_d3.forward[1]=yto-yfrom; g_d3.forward[2]=zto-zfrom;
  double supplied_up[3]={xup,yup,zup};
  if(!gml_software3d_normalize(g_d3.forward)) g_d3.forward[0]=1;
  gml_software3d_cross(supplied_up,g_d3.forward,g_d3.right);
  if(!gml_software3d_normalize(g_d3.right)){ g_d3.right[0]=0; g_d3.right[1]=1; g_d3.right[2]=0; }
  gml_software3d_cross(g_d3.forward,g_d3.right,g_d3.up);
  gml_software3d_normalize(g_d3.up);
}
static void d3_set_default_projection(GmlRender *R,double x,double y,double width,double height,double angle){
  GmlRenderBackendDrawView draw;
  int have_draw=gml_render_backend_draw_view(R,&draw);
  if(fabs(width)<1e-9) width=have_draw&&draw.width>0?draw.width:640;
  if(fabs(height)<1e-9) height=have_draw&&draw.height>0?draw.height:480;
  double distance=fabs(width); if(distance<1) distance=1;
  double radians=angle*M_PI/180.0,cs=cos(radians),sn=sin(radians);
  g_d3.ortho=0; g_d3.perspective=1;
  g_d3.eye[0]=x+width*.5; g_d3.eye[1]=y+height*.5; g_d3.eye[2]=distance;
  g_d3.forward[0]=0; g_d3.forward[1]=0; g_d3.forward[2]=-1;
  g_d3.right[0]=cs; g_d3.right[1]=sn; g_d3.right[2]=0;
  g_d3.up[0]=-sn; g_d3.up[1]=cs; g_d3.up[2]=0;
  g_d3.fov=2*atan(fabs(height)*.5/distance)*180.0/M_PI;
  if(g_d3.fov<1||g_d3.fov>170) g_d3.fov=41.2;
  g_d3.aspect=fabs(width/height); g_d3.near_clip=1; g_d3.far_clip=32000;
}
static GmlD3Vertex d3_vertex_camera(GmlRender *R,GmlD3Vertex input){
  GmlD3Vertex output=d3_camera_vertex(R,input.x,input.y,input.z,input.u,input.v);
  output.r=input.r; output.g=input.g; output.b=input.b; output.alpha=input.alpha;
  output.nx=input.nx; output.ny=input.ny; output.nz=input.nz; output.has_normal=input.has_normal;
  return output;
}
static int d3_transform_normal(GmlRender *R,const GmlD3Vertex *vertex,double output[3]){
  double a00=g_d3.transform[0],a01=g_d3.transform[4],a02=g_d3.transform[8];
  double a10=g_d3.transform[1],a11=g_d3.transform[5],a12=g_d3.transform[9];
  double a20=g_d3.transform[2],a21=g_d3.transform[6],a22=g_d3.transform[10];
  double c00=a11*a22-a12*a21,c01=a12*a20-a10*a22,c02=a10*a21-a11*a20;
  double c10=a02*a21-a01*a22,c11=a00*a22-a02*a20,c12=a01*a20-a00*a21;
  double c20=a01*a12-a02*a11,c21=a02*a10-a00*a12,c22=a00*a11-a01*a10;
  double determinant=a00*c00+a01*c01+a02*c02;
  if(fabs(determinant)<1e-12) return 0;
  output[0]=(c00*vertex->nx+c01*vertex->ny+c02*vertex->nz)/determinant;
  output[1]=(c10*vertex->nx+c11*vertex->ny+c12*vertex->nz)/determinant;
  output[2]=(c20*vertex->nx+c21*vertex->ny+c22*vertex->nz)/determinant;
  return gml_software3d_normalize(output);
}
static int d3_vertex_light_factor(GmlRender *R,const GmlD3Vertex *vertex,double factor[3]){
  if(!vertex->has_normal) return 0;
  double normal[3];
  if(!d3_transform_normal(R,vertex,normal)) return 0;
  double x=vertex->x,y=vertex->y,z=vertex->z;
  d3_transform_point(R,&x,&y,&z);
  factor[0]=(g_d3.ambient_color&255)/255.0;
  factor[1]=((g_d3.ambient_color>>8)&255)/255.0;
  factor[2]=((g_d3.ambient_color>>16)&255)/255.0;
  for(int i=0;i<8;i++) if(g_d3.light[i].defined&&g_d3.light[i].enabled){
    uint32_t color=g_d3.light[i].color;
    double diffuse=0;
    if(g_d3.light[i].range<0){
      double ray[3]={-g_d3.light[i].x,-g_d3.light[i].y,-g_d3.light[i].z};
      if(!gml_software3d_normalize(ray)) continue;
      diffuse=fmax(0.0,gml_software3d_dot(normal,ray));
    } else {
      double ray[3]={g_d3.light[i].x-x,g_d3.light[i].y-y,g_d3.light[i].z-z};
      double distance=sqrt(gml_software3d_dot(ray,ray));
      if(distance<=1e-9 || distance>=g_d3.light[i].range) continue;
      ray[0]/=distance; ray[1]/=distance; ray[2]/=distance;
      diffuse=fmax(0.0,gml_software3d_dot(normal,ray))/(1.0+4.0*distance/g_d3.light[i].range);
    }
    factor[0]+=diffuse*(color&255)/255.0;
    factor[1]+=diffuse*((color>>8)&255)/255.0;
    factor[2]+=diffuse*((color>>16)&255)/255.0;
  }
  return 1;
}
static void d3_apply_light_factor(GmlD3Vertex *vertex,const double factor[3]){
  vertex->r*=factor[0]; vertex->g*=factor[1]; vertex->b*=factor[2];
  if(vertex->r>255) vertex->r=255;
  if(vertex->g>255) vertex->g=255;
  if(vertex->b>255) vertex->b=255;
}
static void d3_emit_triangle(GmlRender *R,const GmlD3Vertex world[3],const GmlD3Texture *texture){
  GmlD3Vertex camera[3],near_clipped[12],far_clipped[12];
  GmlD3Vertex lit[3]={world[0],world[1],world[2]};
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  if(g_d3.lighting){
    double flat[3]; int have_flat=0;
    if(!g_d3.smooth) for(int i=0;i<3&&!have_flat;i++) have_flat=d3_vertex_light_factor(R,&lit[i],flat);
    for(int i=0;i<3;i++){
      double factor[3]; int have=have_flat;
      if(have_flat) memcpy(factor,flat,sizeof(factor));
      else have=d3_vertex_light_factor(R,&lit[i],factor);
      if(have) d3_apply_light_factor(&lit[i],factor);
    }
  }
  for(int i=0;i<3;i++) camera[i]=d3_vertex_camera(R,lit[i]);
  int count;
  if(g_d3.ortho){ memcpy(near_clipped,camera,sizeof(camera)); count=3; }
  else {
    double nearz=g_d3.near_clip>1e-6?g_d3.near_clip:.05;
    double farz=g_d3.far_clip>nearz?g_d3.far_clip:32000;
    count=d3_clip_z(camera,3,near_clipped,nearz,1);
    if(count>0){
      count=d3_clip_z(near_clipped,count,far_clipped,farz,0);
      memcpy(near_clipped,far_clipped,(size_t)count*sizeof(*near_clipped));
    }
  }
  for(int i=1;i+1<count;i++){
    GmlD3Vertex triangle[3]={near_clipped[0],near_clipped[i],near_clipped[i+1]};
    d3_raster_triangle(R,triangle,texture);
  }
}
static void d3_draw_ellipsoid(GmlRender *R,double x1,double y1,double z1,
                              double x2,double y2,double z2,int texture,
                              double hrep,double vrep,int steps,uint32_t color){
  if(!R) return;
  GmlRenderBackendDrawView draw;
  if(!gml_render_backend_draw_view(R,&draw)) return;
  if(steps<3) steps=3; else if(steps>128) steps=128;
  int rows=(steps+1)/2;
  double cx=(x1+x2)*.5,cy=(y1+y2)*.5,cz=(z1+z2)*.5;
  double rx=(x2-x1)*.5,ry=(y2-y1)*.5,rz=(z2-z1)*.5;
  GmlD3Texture resolved={0};
  if(texture!=-1) d3_texture(R,texture,&resolved);
  for(int row=0;row<rows;row++){
    double t0=M_PI*row/rows,t1=M_PI*(row+1)/rows;
    double st[2]={sin(t0),sin(t1)},ct[2]={cos(t0),cos(t1)};
    for(int column=0;column<steps;column++){
      double p0=2*M_PI*column/steps,p1=2*M_PI*(column+1)/steps;
      double sp[2]={sin(p0),sin(p1)},cp[2]={cos(p0),cos(p1)};
      GmlD3Vertex q[4]; memset(q,0,sizeof(q));
      const int latitude[4]={0,0,1,1},longitude[4]={0,1,1,0};
      for(int i=0;i<4;i++){
        int a=latitude[i],b=longitude[i];
        q[i].x=cx+rx*st[a]*cp[b]; q[i].y=cy+ry*st[a]*sp[b]; q[i].z=cz+rz*ct[a];
        q[i].nx=st[a]*cp[b]; q[i].ny=st[a]*sp[b]; q[i].nz=ct[a]; q[i].has_normal=1;
        q[i].u=hrep*(column+b)/steps; q[i].v=vrep*(row+a)/rows;
        q[i].r=color&255; q[i].g=(color>>8)&255; q[i].b=(color>>16)&255; q[i].alpha=draw.alpha;
      }
      GmlD3Vertex first[3]={q[0],q[1],q[2]},second[3]={q[0],q[2],q[3]};
      d3_emit_triangle(R,first,&resolved); d3_emit_triangle(R,second,&resolved);
    }
  }
}
void gml_d3_set_draw_depth(GmlRender *R,double depth){ if(GML_GRAPHICS) g_d3.draw_depth=depth; }
int gml_d3_is_active(GmlRender *R){ return GML_GRAPHICS && g_d3.active; }
int gml_d3_draw_sprite_2d(GmlRender *R,int sprite_id,int subimg,double x,double y,
                          double xs,double ys,double rotation,uint32_t blend,double alpha){
  if(!GML_GRAPHICS || !g_d3.active) return 0;
  if(!R || alpha<=0) return 1;
  int handle=(int)(GML_TEX_SPR_TAG|((sprite_id&0xFFFF)<<10)|(subimg&0x3FF));
  GmlRenderBackendTextureView view;
  if(!gml_render_backend_texture_view(R,handle,0,&view)) return 1;
  double left=-view.origin_x,top=-view.origin_y;
  double right=view.logical_width-view.origin_x,bottom=view.logical_height-view.origin_y;
  if(!view.runtime){
    left=view.trim_x-view.origin_x; top=view.trim_y-view.origin_y;
    right=left+view.width; bottom=top+view.height;
  }
  GmlD3Texture texture={0}; if(!d3_texture_from_backend_view(&view,&texture)) return 1;
  double radians=rotation*M_PI/180.0,cs=cos(radians),sn=sin(radians);
  const double local[4][2]={{left,top},{right,top},{right,bottom},{left,bottom}};
  const double uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
  GmlD3Vertex vertex[4];
  for(int i=0;i<4;i++){
    double px=local[i][0]*xs,py=local[i][1]*ys;
    memset(&vertex[i],0,sizeof(vertex[i]));
    vertex[i].x=x+px*cs+py*sn; vertex[i].y=y-px*sn+py*cs; vertex[i].z=g_d3.draw_depth;
    vertex[i].u=uv[i][0]; vertex[i].v=uv[i][1];
    vertex[i].r=blend&255; vertex[i].g=(blend>>8)&255; vertex[i].b=(blend>>16)&255; vertex[i].alpha=alpha;
  }
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  gml_render_backend_prepare_draw(R);
  GmlD3Vertex first[3]={vertex[0],vertex[1],vertex[2]};
  GmlD3Vertex second[3]={vertex[0],vertex[2],vertex[3]};
  d3_emit_triangle(R,first,&texture); d3_emit_triangle(R,second,&texture);
  return 1;
}
int gml_d3_draw_sprite_pos_2d(GmlRender *R,int sprite_id,int subimg,
                              const double x[4],const double y[4],double alpha){
  if(!GML_GRAPHICS || !g_d3.active) return 0;
  if(!R||alpha<=0) return 1;
  GmlD3Texture texture={0};
  int handle=(int)(GML_TEX_SPR_TAG|((sprite_id&0xFFFF)<<10)|(subimg&0x3FF));
  if(!d3_texture(R,handle,&texture)) return 1;
  static const double uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
  GmlD3Vertex vertex[4];
  for(int i=0;i<4;i++){
    memset(&vertex[i],0,sizeof(vertex[i]));
    vertex[i].x=x[i]; vertex[i].y=y[i]; vertex[i].z=g_d3.draw_depth;
    vertex[i].u=uv[i][0]; vertex[i].v=uv[i][1];
    vertex[i].r=vertex[i].g=vertex[i].b=255; vertex[i].alpha=alpha;
  }
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  gml_render_backend_prepare_draw(R);
  GmlD3Vertex first[3]={vertex[0],vertex[1],vertex[2]};
  GmlD3Vertex second[3]={vertex[0],vertex[2],vertex[3]};
  d3_emit_triangle(R,first,&texture); d3_emit_triangle(R,second,&texture);
  return 1;
}
int gml_d3_draw_background_2d(GmlRender *R,int background,double x,double y,
                              double xs,double ys,uint32_t blend,double alpha){
  if(!GML_GRAPHICS || !g_d3.active) return 0;
  if(!R || alpha<=0) return 1;
  GmlRenderBackendTextureView view;
  int handle=(int)(GML_TEX_BG_TAG|(background&0x00FFFFFF));
  if(!gml_render_backend_texture_view(R,handle,0,&view)) return 1;
  GmlD3Texture texture={0}; if(!d3_texture_from_backend_view(&view,&texture)) return 1;
  double left=x+view.trim_x*xs,top=y+view.trim_y*ys;
  double right=left+view.width*xs,bottom=top+view.height*ys;
  const double point[4][2]={{left,top},{right,top},{right,bottom},{left,bottom}};
  const double uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
  GmlD3Vertex vertex[4];
  for(int i=0;i<4;i++){
    memset(&vertex[i],0,sizeof(vertex[i]));
    vertex[i].x=point[i][0]; vertex[i].y=point[i][1]; vertex[i].z=g_d3.draw_depth;
    vertex[i].u=uv[i][0]; vertex[i].v=uv[i][1];
    vertex[i].r=blend&255; vertex[i].g=(blend>>8)&255; vertex[i].b=(blend>>16)&255; vertex[i].alpha=alpha;
  }
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  gml_render_backend_prepare_draw(R);
  GmlD3Vertex first[3]={vertex[0],vertex[1],vertex[2]};
  GmlD3Vertex second[3]={vertex[0],vertex[2],vertex[3]};
  d3_emit_triangle(R,first,&texture); d3_emit_triangle(R,second,&texture);
  return 1;
}
static int d3_draw_texture_part_2d(GmlRender *R,int handle,double trim_x,double trim_y,
                                   double texture_w,double texture_h,
                                   double sx,double sy,double sw,double sh,
                                   double x,double y,double xs,double ys,uint32_t blend,double alpha){
  if(!GML_GRAPHICS || !g_d3.active) return 0;
  if(!R || sw<=0 || sh<=0 || texture_w<=0 || texture_h<=0 || alpha<=0) return 1;
  double source_left=fmax(sx,trim_x),source_top=fmax(sy,trim_y);
  double source_right=fmin(sx+sw,trim_x+texture_w),source_bottom=fmin(sy+sh,trim_y+texture_h);
  if(source_right<=source_left || source_bottom<=source_top) return 1;
  GmlD3Texture texture={0}; if(!d3_texture(R,handle,&texture)) return 1;
  double left=x+(source_left-sx)*xs,top=y+(source_top-sy)*ys;
  double right=x+(source_right-sx)*xs,bottom=y+(source_bottom-sy)*ys;
  double u0=(source_left-trim_x)/texture_w,v0=(source_top-trim_y)/texture_h;
  double u1=(source_right-trim_x)/texture_w,v1=(source_bottom-trim_y)/texture_h;
  const double point[4][2]={{left,top},{right,top},{right,bottom},{left,bottom}};
  const double uv[4][2]={{u0,v0},{u1,v0},{u1,v1},{u0,v1}};
  GmlD3Vertex vertex[4];
  for(int i=0;i<4;i++){
    memset(&vertex[i],0,sizeof(vertex[i]));
    vertex[i].x=point[i][0]; vertex[i].y=point[i][1]; vertex[i].z=g_d3.draw_depth;
    vertex[i].u=uv[i][0]; vertex[i].v=uv[i][1];
    vertex[i].r=blend&255; vertex[i].g=(blend>>8)&255; vertex[i].b=(blend>>16)&255; vertex[i].alpha=alpha;
  }
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  gml_render_backend_prepare_draw(R);
  GmlD3Vertex first[3]={vertex[0],vertex[1],vertex[2]};
  GmlD3Vertex second[3]={vertex[0],vertex[2],vertex[3]};
  d3_emit_triangle(R,first,&texture); d3_emit_triangle(R,second,&texture);
  return 1;
}
int gml_d3_draw_sprite_part_2d(GmlRender *R,int sprite_id,int subimg,
                               double sx,double sy,double sw,double sh,double x,double y,
                               double xs,double ys,uint32_t blend,double alpha){
  if(!GML_GRAPHICS || !g_d3.active) return 0;
  if(!R) return 1;
  int handle=(int)(GML_TEX_SPR_TAG|((sprite_id&0xFFFF)<<10)|(subimg&0x3FF));
  GmlRenderBackendTextureView view;
  if(!gml_render_backend_texture_view(R,handle,0,&view)) return 1;
  if(!view.runtime) return d3_draw_texture_part_2d(R,handle,
    view.trim_x,view.trim_y,view.width,view.height,
    sx,sy,sw,sh,x,y,xs,ys,blend,alpha);
  return d3_draw_texture_part_2d(R,handle,0,0,view.logical_width,view.logical_height,
    sx,sy,sw,sh,x,y,xs,ys,blend,alpha);
}
int gml_d3_draw_background_part_2d(GmlRender *R,int background,
                                   double sx,double sy,double sw,double sh,double x,double y,
                                   double xs,double ys,uint32_t blend,double alpha){
  if(!GML_GRAPHICS || !g_d3.active) return 0;
  if(!R) return 1;
  int handle=(int)(GML_TEX_BG_TAG|(background&0x00FFFFFF));
  GmlRenderBackendTextureView view;
  if(!gml_render_backend_texture_view(R,handle,0,&view)) return 1;
  return d3_draw_texture_part_2d(R,handle,view.trim_x,view.trim_y,view.width,view.height,
    sx,sy,sw,sh,x,y,xs,ys,blend,alpha);
}
int gml_d3_draw_atlas_part_2d(GmlRender *R,int atlas_id,int sx,int sy,int width,int height,
                              double x,double y,double xs,double ys,uint32_t blend,double alpha){
  if(!GML_GRAPHICS || !g_d3.active) return 0;
  if(!R || width<=0 || height<=0 || alpha<=0) return 1;
  GmlRenderBackendTextureView view;
  if(!gml_render_backend_atlas_view(R,atlas_id,&view) ||
     sx<0 || sy<0 || sx+width>view.full_width || sy+height>view.full_height) return 1;
  GmlD3Texture texture={.kind=1,.w=width,.h=height,.stride=view.stride,
    .offset_x=sx,.offset_y=sy,.rgba=view.rgba};
  double right=x+width*xs,bottom=y+height*ys;
  const double point[4][2]={{x,y},{right,y},{right,bottom},{x,bottom}};
  const double uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
  GmlD3Vertex vertex[4];
  for(int i=0;i<4;i++){
    memset(&vertex[i],0,sizeof(vertex[i]));
    vertex[i].x=point[i][0]; vertex[i].y=point[i][1]; vertex[i].z=g_d3.draw_depth;
    vertex[i].u=uv[i][0]; vertex[i].v=uv[i][1];
    vertex[i].r=blend&255; vertex[i].g=(blend>>8)&255; vertex[i].b=(blend>>16)&255; vertex[i].alpha=alpha;
  }
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  gml_render_backend_prepare_draw(R);
  GmlD3Vertex first[3]={vertex[0],vertex[1],vertex[2]};
  GmlD3Vertex second[3]={vertex[0],vertex[2],vertex[3]};
  d3_emit_triangle(R,first,&texture); d3_emit_triangle(R,second,&texture);
  return 1;
}
int gml_d3_draw_surface_part_2d(GmlRender *R,int surface,
                                double sx,double sy,double sw,double sh,double x,double y,
                                double xs,double ys,uint32_t blend,double alpha){
  if(!GML_GRAPHICS || !g_d3.active) return 0;
  GmlRenderBackendTextureView view;
  int handle=(int)(GML_TEX_SURF_TAG|(surface&0xFFFF));
  if(!gml_render_backend_texture_view(R,handle,0,&view)) return 1;
  return d3_draw_texture_part_2d(R,(int)(GML_TEX_SURF_TAG|(surface&0xFFFF)),
    0,0,view.width,view.height,sx,sy,sw,sh,x,y,xs,ys,blend,alpha);
}
static int d3_project_vertex(GmlRender *R,const GmlRenderBackendDrawView *draw,
                             const GmlD3Vertex *vertex,
                             double *screen_x,double *screen_y,double *inverse_z,
                             double *depth,double *distance){
  if(!R || !draw || !vertex) return 0;
  if(g_d3.ortho){
    double width=fabs(g_d3.ortho_w)>1e-9?g_d3.ortho_w:1;
    double height=fabs(g_d3.ortho_h)>1e-9?g_d3.ortho_h:1;
    double pixel_offset=g_d3.classic?0.5:0.0;
    *screen_x=vertex->x*draw->width/width+pixel_offset;
    *screen_y=vertex->y*draw->height/height+pixel_offset;
    *inverse_z=1; *depth=1000000.0-vertex->z; *distance=fabs(vertex->z);
    return 1;
  }
  double nearz=g_d3.near_clip>1e-6?g_d3.near_clip:.05;
  double farz=g_d3.far_clip>nearz?g_d3.far_clip:32000;
  if(vertex->z<nearz || vertex->z>farz) return 0;
  double fov=g_d3.fov;
  if(fov<1.0 || fov>170.0) fov=41.2;
  double tangent=tan(fov*M_PI/360.0);
  double focal_y=(draw->height*.5)/tangent;
  double focal_x=g_d3.aspect>1e-9?(draw->width*.5)/(tangent*g_d3.aspect):focal_y;
  *inverse_z=1.0/vertex->z;
  double pixel_offset=g_d3.classic?0.5:0.0;
  *screen_x=draw->width*.5+vertex->x*focal_x*(*inverse_z)+pixel_offset;
  *screen_y=draw->height*.5-vertex->y*focal_y*(*inverse_z)+pixel_offset;
  *depth=*inverse_z; *distance=vertex->z;
  return 1;
}
static void d3_raster_sample(GmlRender *R,const GmlRenderBackendDrawView *draw,
                             int x,int y,double depth,double distance,
                             double u,double v,double red,double green,double blue,double alpha,
                             const GmlD3Texture *texture){
  if(!R || !draw || x<0 || y<0 || x>=draw->width || y>=draw->height || alpha<=0) return;
  size_t index=(size_t)y*draw->width+x;
  if(g_d3.hidden && depth<g_d3.depth[index]) return;
  double sampled[4]={255,255,255,255};
  if(texture&&texture->kind) d3_sample(R,draw,texture,u,v,sampled);
  if(red<0) red=0; else if(red>255) red=255;
  if(green<0) green=0; else if(green>255) green=255;
  if(blue<0) blue=0; else if(blue>255) blue=255;
  int cr=(int)lround(sampled[0]*red/255.0);
  int cg=(int)lround(sampled[1]*green/255.0);
  int cb=(int)lround(sampled[2]*blue/255.0);
  if(g_d3.lighting){
    cr=(int)(cr*g_d3.shade_r); cg=(int)(cg*g_d3.shade_g); cb=(int)(cb*g_d3.shade_b);
    if(cr>255) cr=255;
    if(cg>255) cg=255;
    if(cb>255) cb=255;
  }
  if(g_d3.fog){
    double span=g_d3.fog_end-g_d3.fog_start;
    double amount=span>1e-9?(distance-g_d3.fog_start)/span:(distance>=g_d3.fog_end?1:0);
    if(amount<0) amount=0; else if(amount>1) amount=1;
    uint32_t fog=g_d3.fog_color;
    cr=(int)lround(cr*(1-amount)+(fog&255)*amount);
    cg=(int)lround(cg*(1-amount)+((fog>>8)&255)*amount);
    cb=(int)lround(cb*(1-amount)+((fog>>16)&255)*amount);
  }
  double final_alpha=(sampled[3]/255.0)*alpha;
  gml_render_backend_draw_pixel_alpha(R,x,y,(uint32_t)cr|((uint32_t)cg<<8)|((uint32_t)cb<<16),final_alpha);
  if(g_d3.hidden && g_d3.zwrite && final_alpha>0) g_d3.depth[index]=(float)depth;
}
static int d3_clip_segment_plane(GmlD3Vertex *a,GmlD3Vertex *b,double plane,int keep_greater){
  int a_inside=keep_greater?a->z>=plane:a->z<=plane;
  int b_inside=keep_greater?b->z>=plane:b->z<=plane;
  if(!a_inside&&!b_inside) return 0;
  if(a_inside!=b_inside){
    double amount=(plane-a->z)/(b->z-a->z);
    GmlD3Vertex intersection=d3_vertex_lerp(*a,*b,amount); intersection.z=plane;
    if(!a_inside) *a=intersection; else *b=intersection;
  }
  return 1;
}
static void d3_emit_point(GmlRender *R,GmlD3Vertex world,const GmlD3Texture *texture){
  GmlRenderBackendDrawView draw;
  if(!gml_render_backend_draw_view(R,&draw)) return;
  if(g_d3.lighting){ double factor[3]; if(d3_vertex_light_factor(R,&world,factor)) d3_apply_light_factor(&world,factor); }
  GmlD3Vertex camera=d3_vertex_camera(R,world);
  double x,y,inverse_z,depth,distance;
  if(!d3_project_vertex(R,&draw,&camera,&x,&y,&inverse_z,&depth,&distance)) return;
  (void)inverse_z;
  d3_raster_sample(R,&draw,(int)lround(x),(int)lround(y),depth,distance,camera.u,camera.v,
                   camera.r,camera.g,camera.b,camera.alpha,texture);
}
static void d3_emit_line(GmlRender *R,GmlD3Vertex a,GmlD3Vertex b,const GmlD3Texture *texture){
  GmlRenderBackendDrawView draw;
  if(!gml_render_backend_draw_view(R,&draw)) return;
  if(g_d3.lighting){
    double factor[3]; int have=d3_vertex_light_factor(R,&a,factor);
    if(have) d3_apply_light_factor(&a,factor);
    if(g_d3.smooth){ if(d3_vertex_light_factor(R,&b,factor)) d3_apply_light_factor(&b,factor); }
    else if(have) d3_apply_light_factor(&b,factor);
  }
  a=d3_vertex_camera(R,a); b=d3_vertex_camera(R,b);
  if(!g_d3.ortho){
    double nearz=g_d3.near_clip>1e-6?g_d3.near_clip:.05;
    double farz=g_d3.far_clip>nearz?g_d3.far_clip:32000;
    if(!d3_clip_segment_plane(&a,&b,nearz,1) || !d3_clip_segment_plane(&a,&b,farz,0)) return;
  }
  double ax,ay,a_inverse_z,a_depth,a_distance,bx,by,b_inverse_z,b_depth,b_distance;
  if(!d3_project_vertex(R,&draw,&a,&ax,&ay,&a_inverse_z,&a_depth,&a_distance) ||
     !d3_project_vertex(R,&draw,&b,&bx,&by,&b_inverse_z,&b_depth,&b_distance)) return;
  int steps=(int)ceil(fmax(fabs(bx-ax),fabs(by-ay)));
  if(steps<1) steps=1;
  for(int i=0;i<=steps;i++){
    double amount=(double)i/steps;
    double inverse_z=a_inverse_z+(b_inverse_z-a_inverse_z)*amount;
    double denominator=g_d3.ortho?1:inverse_z;
#define D3_LINE_ATTR(field) ((a.field*a_inverse_z+(b.field*b_inverse_z-a.field*a_inverse_z)*amount)/denominator)
    double u=D3_LINE_ATTR(u),v=D3_LINE_ATTR(v);
    double red=D3_LINE_ATTR(r),green=D3_LINE_ATTR(g),blue=D3_LINE_ATTR(b);
    double alpha=D3_LINE_ATTR(alpha);
#undef D3_LINE_ATTR
    double distance=g_d3.ortho?a_distance+(b_distance-a_distance)*amount:1.0/inverse_z;
    double depth=a_depth+(b_depth-a_depth)*amount;
    int x=(int)lround(ax+(bx-ax)*amount),y=(int)lround(ay+(by-ay)*amount);
    d3_raster_sample(R,&draw,x,y,depth,distance,u,v,red,green,blue,alpha,texture);
  }
}
static void d3_primitive_flush(GmlRender *R){
  if(!R || g_d3_prim_n<=0) return;
  GmlD3Texture texture={0};
  if(g_d3_prim_texture!=-1) d3_texture(R,g_d3_prim_texture,&texture);
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  gml_render_backend_prepare_draw(R);
  if(!d3_depth_prepare(R)) return;
  if(g_d3_prim_kind==1){
    for(int i=0;i<g_d3_prim_n;i++) d3_emit_point(R,g_d3_prim[i],&texture);
  } else if(g_d3_prim_kind==2){
    for(int i=0;i+1<g_d3_prim_n;i+=2) d3_emit_line(R,g_d3_prim[i],g_d3_prim[i+1],&texture);
  } else if(g_d3_prim_kind==3){
    for(int i=0;i+1<g_d3_prim_n;i++) d3_emit_line(R,g_d3_prim[i],g_d3_prim[i+1],&texture);
  } else if(g_d3_prim_kind==4){
    for(int i=0;i+2<g_d3_prim_n;i+=3) d3_emit_triangle(R,&g_d3_prim[i],&texture);
  } else if(g_d3_prim_kind==5){
    for(int i=2;i<g_d3_prim_n;i++){
      GmlD3Vertex triangle[3];
      if(i&1){ triangle[0]=g_d3_prim[i-1]; triangle[1]=g_d3_prim[i-2]; }
      else { triangle[0]=g_d3_prim[i-2]; triangle[1]=g_d3_prim[i-1]; }
      triangle[2]=g_d3_prim[i]; d3_emit_triangle(R,triangle,&texture);
    }
  } else if(g_d3_prim_kind==6){
    for(int i=1;i+1<g_d3_prim_n;i++){
      GmlD3Vertex triangle[3]={g_d3_prim[0],g_d3_prim[i],g_d3_prim[i+1]};
      d3_emit_triangle(R,triangle,&texture);
    }
  }
}
static GmlD3Vertex *vertex_blend_scratch_ensure(GmlRender *R,int count){
  GmlSoftware3D *graphics=GML_GRAPHICS;
  if(!graphics || count<=0) return NULL;
  if(count>graphics->vertex_blend_scratch_capacity){
    GmlD3Vertex *grown=realloc(graphics->vertex_blend_scratch,
                               (size_t)count*sizeof(GmlD3Vertex));
    if(!grown) return NULL;
    graphics->vertex_blend_scratch=grown;
    graphics->vertex_blend_scratch_capacity=count;
  }
  return graphics->vertex_blend_scratch;
}
static void vertex_submit_buffer(GmlRender *R,int id,int primitive,int texture_handle,
                                 int first,int number){
  if(!R || id<0 || id>=GML_VERTEX_BUFFER_MAX || !g_vertex_buffer[id].used) return;
  GmlVertexBuffer *buffer=&g_vertex_buffer[id];
  if(first<0) first=0;
  if(first>buffer->vertex_n) first=buffer->vertex_n;
  if(number<0 || number>buffer->vertex_n-first) number=buffer->vertex_n-first;
  if(number<=0) return;
  const GmlD3Vertex *vertex=buffer->vertex+first;
  /* A vertex program may modulate the colour it passes on by a uniform, which is how text
   * libraries carry a string's tint and fade. Apply it here, to a copy, so the rasterizer keeps
   * receiving plain vertex colours and every other submission path is untouched. */
  double blend[4];
  if(gml_render_shader_vertex_colour_blend(R,blend) &&
     (blend[0]<1.0 || blend[1]<1.0 || blend[2]<1.0 || blend[3]<1.0)){
    GmlD3Vertex *modulated=vertex_blend_scratch_ensure(R,number);
    if(modulated){
      for(int i=0;i<number;i++){
        modulated[i]=vertex[i];
        modulated[i].r*=blend[0];
        modulated[i].g*=blend[1];
        modulated[i].b*=blend[2];
        modulated[i].alpha*=blend[3];
      }
      vertex=modulated;
    }
  }
  GmlD3Texture texture={0};
  if(!vertex_texture(R,texture_handle,&texture)) memset(&texture,0,sizeof(texture));
  GmlRenderBackendDrawView draw;
  if(!gml_render_backend_draw_view(R,&draw)) return;

  /* The ordinary 2D API also exposes vertex buffers. Configure a
   * temporary orthographic software pipeline when legacy d3d mode is not
   * active, then restore all state while retaining a possibly grown depth
   * allocation. */
  int temporary=!g_d3.active;
  GmlD3State saved;
  if(temporary){
    saved=g_d3;
    g_d3.active=1; g_d3.ortho=1; g_d3.hidden=0; g_d3.zwrite=0;
    g_d3.lighting=0; g_d3.fog=0; g_d3.culling=0; g_d3.smooth=1;
    double scale_x=draw.coordinate_scale_x>0?draw.coordinate_scale_x:1.0;
    double scale_y=draw.coordinate_scale_y>0?draw.coordinate_scale_y:1.0;
    g_d3.ortho_x=draw.camera_x/scale_x; g_d3.ortho_y=draw.camera_y/scale_y;
    g_d3.ortho_w=draw.width>0?draw.width/scale_x:1;
    g_d3.ortho_h=draw.height>0?draw.height/scale_y:1;
    g_d3.ortho_angle=0; g_d3.draw_depth=0;
    /* A pure world translation is mirrored into the renderer camera for
     * non-vertex draws; adjust the projection anchor to apply it once. */
    int translation=fabs(saved.transform[0]-1)<1e-10 &&
      fabs(saved.transform[5]-1)<1e-10 && fabs(saved.transform[10]-1)<1e-10 &&
      fabs(saved.transform[15]-1)<1e-10;
    for(int i=0;i<12 && translation;i++)
      if(i!=0 && i!=5 && i!=10 && fabs(saved.transform[i])>=1e-10) translation=0;
    if(translation){
      g_d3.ortho_x+=saved.transform[12];
      g_d3.ortho_y+=saved.transform[13];
    }
  }
  g_d3.shade_r=g_d3.shade_g=g_d3.shade_b=1;
  gml_render_backend_prepare_draw(R);
  if(primitive==1){
    for(int i=0;i<number;i++) d3_emit_point(R,vertex[i],&texture);
  } else if(primitive==2){
    for(int i=0;i+1<number;i+=2) d3_emit_line(R,vertex[i],vertex[i+1],&texture);
  } else if(primitive==3){
    for(int i=0;i+1<number;i++) d3_emit_line(R,vertex[i],vertex[i+1],&texture);
  } else if(primitive==4){
    for(int i=0;i+2<number;i+=3) d3_emit_triangle(R,vertex+i,&texture);
  } else if(primitive==5){
    for(int i=2;i<number;i++){
      GmlD3Vertex triangle[3];
      if(i&1){ triangle[0]=vertex[i-1]; triangle[1]=vertex[i-2]; }
      else { triangle[0]=vertex[i-2]; triangle[1]=vertex[i-1]; }
      triangle[2]=vertex[i]; d3_emit_triangle(R,triangle,&texture);
    }
  } else if(primitive==6){
    for(int i=1;i+1<number;i++){
      GmlD3Vertex triangle[3]={vertex[0],vertex[i],vertex[i+1]};
      d3_emit_triangle(R,triangle,&texture);
    }
  }
  if(temporary){
    float *depth=g_d3.depth; size_t depth_cap=g_d3.depth_cap;
    int depth_w=g_d3.depth_w,depth_h=g_d3.depth_h; long depth_frame=g_d3.depth_frame;
    g_d3=saved;
    g_d3.depth=depth; g_d3.depth_cap=depth_cap;
    g_d3.depth_w=depth_w; g_d3.depth_h=depth_h; g_d3.depth_frame=depth_frame;
  }
}

void gml_software3d_transform_point(GmlRender *render,double *x,double *y,double *z){
  d3_transform_point(render,x,y,z);
}
int gml_software3d_depth_prepare(GmlRender *render){
  return d3_depth_prepare(render);
}
int gml_software3d_texture(GmlRender *render,int handle,GmlD3Texture *texture){
  return d3_texture(render,handle,texture);
}
void gml_software3d_emit_triangle(GmlRender *render,const GmlD3Vertex vertices[3],
                                  const GmlD3Texture *texture){
  d3_emit_triangle(render,vertices,texture);
}
void gml_software3d_emit_point(GmlRender *render,GmlD3Vertex vertex,
                               const GmlD3Texture *texture){
  d3_emit_point(render,vertex,texture);
}
void gml_software3d_emit_line(GmlRender *render,GmlD3Vertex first,GmlD3Vertex second,
                              const GmlD3Texture *texture){
  d3_emit_line(render,first,second,texture);
}
void gml_software3d_draw_quad(GmlRender *render,const double points[4][3],int texture,
                              double hrepeat,double vrepeat,uint32_t color,
                              int reverse_normal,int uv_mode){
  d3_draw_quad(render,points,texture,hrepeat,vrepeat,color,reverse_normal,uv_mode);
}
void gml_software3d_set_camera(GmlRender *render,double xfrom,double yfrom,double zfrom,
                               double xto,double yto,double zto,
                               double xup,double yup,double zup){
  d3_set_camera(render,xfrom,yfrom,zfrom,xto,yto,zto,xup,yup,zup);
}
void gml_software3d_set_default_projection(GmlRender *render,double x,double y,
                                           double width,double height,double angle){
  d3_set_default_projection(render,x,y,width,height,angle);
}
void gml_software3d_begin(GmlRender *render,double x,double y,
                          double width,double height,double angle){
  d3_set_default_projection(render,x,y,width,height,angle);
  GmlSoftware3D *graphics=graphics_state_for_render(render);
  if(!graphics) return;
  graphics->d3.active=1;
  graphics->d3.hidden=1;
  graphics->d3.zwrite=1;
  graphics->d3.depth_frame=-1;
}
void gml_software3d_draw_ellipsoid(GmlRender *render,double x1,double y1,double z1,
                                   double x2,double y2,double z2,int texture,
                                   double hrepeat,double vrepeat,int steps,uint32_t color){
  d3_draw_ellipsoid(render,x1,y1,z1,x2,y2,z2,texture,hrepeat,vrepeat,steps,color);
}
void gml_software3d_primitive_3d_end(GmlRender *render){
  d3_primitive_flush(render);
  GmlSoftware3D *graphics=graphics_state_for_render(render);
  if(graphics) graphics->d3_prim_n=0;
}
void gml_software3d_vertex_submit_buffer(GmlRender *render,int id,int primitive,
                                         int texture_handle,int first,int number){
  vertex_submit_buffer(render,id,primitive,texture_handle,first,number);
}

static GmlD3Vertex d3_2d_vertex(GmlRender *R,double x,double y,uint32_t color,double alpha){
  GmlD3Vertex vertex={0}; vertex.x=x; vertex.y=y; vertex.z=g_d3.draw_depth;
  vertex.r=color&255; vertex.g=(color>>8)&255; vertex.b=(color>>16)&255; vertex.alpha=alpha;
  return vertex;
}
static void d3_2d_line(GmlRender *R,double x1,double y1,double x2,double y2,
                       uint32_t color1,uint32_t color2,double alpha,double width){
  GmlD3Texture texture={0};
  if(width<=1){
    d3_emit_line(R,d3_2d_vertex(R,x1,y1,color1,alpha),d3_2d_vertex(R,x2,y2,color2,alpha),&texture);
    return;
  }
  double dx=x2-x1,dy=y2-y1,length=hypot(dx,dy);
  if(length<=1e-9){
    double half=width*.5;
    GmlD3Vertex triangle1[3]={d3_2d_vertex(R,x1-half,y1-half,color1,alpha),d3_2d_vertex(R,x1+half,y1-half,color1,alpha),d3_2d_vertex(R,x1+half,y1+half,color1,alpha)};
    GmlD3Vertex triangle2[3]={triangle1[0],triangle1[2],d3_2d_vertex(R,x1-half,y1+half,color1,alpha)};
    d3_emit_triangle(R,triangle1,&texture); d3_emit_triangle(R,triangle2,&texture); return;
  }
  double nx=-dy/length*width*.5,ny=dx/length*width*.5;
  GmlD3Vertex quad[4]={d3_2d_vertex(R,x1-nx,y1-ny,color1,alpha),d3_2d_vertex(R,x2-nx,y2-ny,color2,alpha),
                       d3_2d_vertex(R,x2+nx,y2+ny,color2,alpha),d3_2d_vertex(R,x1+nx,y1+ny,color1,alpha)};
  GmlD3Vertex triangle1[3]={quad[0],quad[1],quad[2]},triangle2[3]={quad[0],quad[2],quad[3]};
  d3_emit_triangle(R,triangle1,&texture); d3_emit_triangle(R,triangle2,&texture);
}
static void d3_2d_triangle(GmlRender *R,GmlD3Vertex a,GmlD3Vertex b,GmlD3Vertex c,int outline){
  GmlD3Texture texture={0};
  if(outline){
    d3_emit_line(R,a,b,&texture); d3_emit_line(R,b,c,&texture); d3_emit_line(R,c,a,&texture);
  } else { GmlD3Vertex triangle[3]={a,b,c}; d3_emit_triangle(R,triangle,&texture); }
}
static void d3_2d_rectangle(GmlRender *R,double x1,double y1,double x2,double y2,
                            const uint32_t color[4],double alpha,int outline){
  GmlD3Vertex vertex[4]={d3_2d_vertex(R,x1,y1,color[0],alpha),d3_2d_vertex(R,x2,y1,color[1],alpha),
                         d3_2d_vertex(R,x2,y2,color[2],alpha),d3_2d_vertex(R,x1,y2,color[3],alpha)};
  GmlD3Texture texture={0};
  if(outline){ for(int i=0;i<4;i++) d3_emit_line(R,vertex[i],vertex[(i+1)&3],&texture); }
  else {
    GmlD3Vertex first[3]={vertex[0],vertex[1],vertex[2]},second[3]={vertex[0],vertex[2],vertex[3]};
    d3_emit_triangle(R,first,&texture); d3_emit_triangle(R,second,&texture);
  }
}
int gml_d3_draw_rectangle_2d(GmlRender *R,double x1,double y1,double x2,double y2,
                              uint32_t color,double alpha,int outline){
  if(!GML_GRAPHICS || !g_d3.active) return 0;
  uint32_t colors[4]={color,color,color,color};
  d3_2d_rectangle(R,x1,y1,x2,y2,colors,alpha,outline);
  return 1;
}
static void d3_2d_ellipse(GmlRender *R,double cx,double cy,double rx,double ry,
                          uint32_t inner,uint32_t outer,double alpha,int outline){
  GmlRenderBackendDrawView draw;
  int segments=gml_render_backend_draw_view(R,&draw)&&draw.circle_precision>=3?
    draw.circle_precision:24;
  if(segments>256) segments=256;
  GmlD3Texture texture={0}; GmlD3Vertex center=d3_2d_vertex(R,cx,cy,inner,alpha);
  for(int i=0;i<segments;i++){
    double angle0=2*M_PI*i/segments,angle1=2*M_PI*(i+1)/segments;
    GmlD3Vertex a=d3_2d_vertex(R,cx+cos(angle0)*rx,cy+sin(angle0)*ry,outer,alpha);
    GmlD3Vertex b=d3_2d_vertex(R,cx+cos(angle1)*rx,cy+sin(angle1)*ry,outer,alpha);
    if(outline) d3_emit_line(R,a,b,&texture);
    else { GmlD3Vertex triangle[3]={center,a,b}; d3_emit_triangle(R,triangle,&texture); }
  }
}
/* Fast form of the application-surface compositor: a four-vertex triangle strip,
 * full 0..1 UV rectangle, uniform white modulation. It is still recognized solely from API state
 * and geometry (never from a title or object name). Routing this affine identity case through the
 * renderer's parallel surface scaler avoids a two-triangle, per-pixel barycentric pass at 1080p/4K.
 * Skewed, cropped, coloured or otherwise general primitives continue through the full rasterizer. */
static int prim_try_fast_surface_quad(GmlRender *R){
  if(!R || g_d3.active || g_prim_kind!=5 || g_prim_n!=4) return 0;
  uint32_t encoded=(uint32_t)g_prim_texture;
  if((encoded&GML_TEX_KIND_MASK)!=GML_TEX_SURF_TAG) return 0;
  const double eps=1e-7;
  if(fabs(g_prim_x[0]-g_prim_x[2])>eps || fabs(g_prim_x[1]-g_prim_x[3])>eps ||
     fabs(g_prim_y[0]-g_prim_y[1])>eps || fabs(g_prim_y[2]-g_prim_y[3])>eps ||
     fabs(g_prim_u[0])>eps || fabs(g_prim_u[2])>eps ||
     fabs(g_prim_u[1]-1)>eps || fabs(g_prim_u[3]-1)>eps ||
     fabs(g_prim_v[0])>eps || fabs(g_prim_v[1])>eps ||
     fabs(g_prim_v[2]-1)>eps || fabs(g_prim_v[3]-1)>eps) return 0;
  uint32_t color=g_prim_c[0]&0xFFFFFFu;
  double alpha=g_prim_a[0];
  for(int i=1;i<4;i++)
    if((g_prim_c[i]&0xFFFFFFu)!=color || fabs(g_prim_a[i]-alpha)>eps) return 0;
  if(color!=0xFFFFFFu || alpha<=0) return 0;
  double x0=g_prim_x[0],y0=g_prim_y[0],z0=g_d3.draw_depth;
  double x1=g_prim_x[3],y1=g_prim_y[3],z1=g_d3.draw_depth;
  d3_transform_point(R,&x0,&y0,&z0); d3_transform_point(R,&x1,&y1,&z1);
  if(fabs(z0-z1)>eps || x1<=x0 || y1<=y0) return 0;
  int surface=(int)(encoded&0xFFFFu);
  return gml_render_backend_surface_stretched(
    R,surface,x0,y0,x1-x0,y1-y0,0xFFFFFFu,alpha);
}
static void d3_flush_2d_primitive(GmlRender *R){
  if(!R || g_prim_n<=0) return;
  if(prim_try_fast_surface_quad(R)) return;
  GmlRenderBackendDrawView draw;
  if(!gml_render_backend_draw_view(R,&draw)) return;
  /* Immediate-mode primitives are part of the ordinary 2D API too. Reuse the textured,
   * per-vertex software rasterizer under a temporary pixel-coordinate orthographic projection
   * when no legacy d3d projection is active. The old non-d3 path discarded texture coordinates
   * entirely, turning every textured compositor quad into a solid vertex-colour rectangle. */
  int temporary=!g_d3.active;
  GmlD3State saved;
  if(temporary){
    saved=g_d3;
    g_d3.active=1; g_d3.ortho=1; g_d3.hidden=0; g_d3.zwrite=0;
    g_d3.lighting=0; g_d3.fog=0; g_d3.culling=0; g_d3.smooth=1;
    g_d3.ortho_x=draw.camera_x; g_d3.ortho_y=draw.camera_y;
    g_d3.ortho_w=draw.width>0?draw.width:1; g_d3.ortho_h=draw.height>0?draw.height:1;
    g_d3.ortho_angle=0; g_d3.draw_depth=0;
  }
  GmlD3Texture texture={0}; if(g_prim_texture!=-1) d3_texture(R,g_prim_texture,&texture);
  gml_render_backend_prepare_draw(R);
  GmlD3Vertex vertex[GML_PRIM_MAX];
  for(int i=0;i<g_prim_n;i++){
    double x=g_prim_x[i],y=g_prim_y[i];
    gml_render_backend_draw_map_point(R,&x,&y);
    vertex[i]=d3_2d_vertex(R,x,y,g_prim_c[i],g_prim_a[i]);
    vertex[i].u=g_prim_u[i]; vertex[i].v=g_prim_v[i];
  }
  if(g_prim_kind==1){ for(int i=0;i<g_prim_n;i++) d3_emit_point(R,vertex[i],&texture); }
  else if(g_prim_kind==2){ for(int i=0;i+1<g_prim_n;i+=2) d3_emit_line(R,vertex[i],vertex[i+1],&texture); }
  else if(g_prim_kind==3){ for(int i=0;i+1<g_prim_n;i++) d3_emit_line(R,vertex[i],vertex[i+1],&texture); }
  else if(g_prim_kind==4){
    for(int i=0;i+2<g_prim_n;i+=3){ GmlD3Vertex triangle[3]={vertex[i],vertex[i+1],vertex[i+2]}; d3_emit_triangle(R,triangle,&texture); }
  } else if(g_prim_kind==5){
    for(int i=2;i<g_prim_n;i++){
      GmlD3Vertex triangle[3];
      if(i&1){ triangle[0]=vertex[i-1]; triangle[1]=vertex[i-2]; }
      else { triangle[0]=vertex[i-2]; triangle[1]=vertex[i-1]; }
      triangle[2]=vertex[i]; d3_emit_triangle(R,triangle,&texture);
    }
  } else if(g_prim_kind==6){
    for(int i=1;i+1<g_prim_n;i++){ GmlD3Vertex triangle[3]={vertex[0],vertex[i],vertex[i+1]}; d3_emit_triangle(R,triangle,&texture); }
  }
  if(temporary){
    float *depth=g_d3.depth; size_t depth_cap=g_d3.depth_cap;
    int depth_w=g_d3.depth_w,depth_h=g_d3.depth_h; long depth_frame=g_d3.depth_frame;
    g_d3=saved;
    g_d3.depth=depth; g_d3.depth_cap=depth_cap;
    g_d3.depth_w=depth_w; g_d3.depth_h=depth_h; g_d3.depth_frame=depth_frame;
  }
}
void gml_software3d_draw_point_2d(GmlRender *render,double x,double y,
                                  uint32_t color,double alpha){
  GmlD3Texture texture={0};
  d3_emit_point(render,d3_2d_vertex(render,x,y,color,alpha),&texture);
}
void gml_software3d_draw_line_2d(GmlRender *render,
                                 double x1,double y1,double x2,double y2,
                                 uint32_t color1,uint32_t color2,
                                 double alpha,double width){
  d3_2d_line(render,x1,y1,x2,y2,color1,color2,alpha,width);
}
void gml_software3d_draw_triangle_2d(GmlRender *render,
                                     const double points[3][2],
                                     const uint32_t colors[3],
                                     double alpha,int outline){
  if(!points || !colors) return;
  d3_2d_triangle(render,
    d3_2d_vertex(render,points[0][0],points[0][1],colors[0],alpha),
    d3_2d_vertex(render,points[1][0],points[1][1],colors[1],alpha),
    d3_2d_vertex(render,points[2][0],points[2][1],colors[2],alpha),
    outline);
}
void gml_software3d_draw_rectangle_2d(GmlRender *render,
                                      double x1,double y1,double x2,double y2,
                                      const uint32_t color[4],
                                      double alpha,int outline){
  d3_2d_rectangle(render,x1,y1,x2,y2,color,alpha,outline);
}
void gml_software3d_draw_ellipse_2d(GmlRender *render,
                                    double center_x,double center_y,
                                    double radius_x,double radius_y,
                                    uint32_t inner,uint32_t outer,
                                    double alpha,int outline){
  d3_2d_ellipse(render,center_x,center_y,radius_x,radius_y,inner,outer,alpha,outline);
}
void gml_software3d_primitive_2d_end(GmlRender *render){
  d3_flush_2d_primitive(render);
  GmlSoftware3D *graphics=graphics_state_for_render(render);
  if(graphics) graphics->prim_n=0;
}
