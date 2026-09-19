/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Language-visible math, color, geometry, arrays, strings, variables, structs, and cloning. */
#include "gml_builtin_internal.h"
#include "anygm_compatibility.h"
#include "gml_render.h"
#include "anygm_host.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int point_in_poly(double px, double py, const double *vx, const double *vy, int n){
  int inside=0;
  for(int i=0,j=n-1;i<n;j=i++){
    int crosses=((vy[i]>py)!=(vy[j]>py));
    if(crosses){
      double xint=(vx[j]-vx[i])*(py-vy[i])/(vy[j]-vy[i]) + vx[i];
      if(px<xint) inside=!inside;
    }
  }
  return inside;
}

GmlVal gml_builtin_try_values_math(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- math ---- */
  if(!strcmp(nm,"floor")) return vreal(floor(N(a,n,0)));
  if(!strcmp(nm,"ceil"))  return vreal(ceil(N(a,n,0)));
  if(!strcmp(nm,"round")) return vreal(gm_round(N(a,n,0)));
  if(!strcmp(nm,"sign"))  return vreal(gm_sign(N(a,n,0)));
  if(!strcmp(nm,"abs"))   return vreal(fabs(N(a,n,0)));
  if(!strcmp(nm,"sqrt"))  return vreal(builtin_math_sqrt(vm,N(a,n,0)));
  if(!strcmp(nm,"sqr"))   { double x=N(a,n,0); return vreal(x*x); }
  if(!strcmp(nm,"sin"))   return vreal(sin(N(a,n,0)));
  if(!strcmp(nm,"cos"))   return vreal(cos(N(a,n,0)));
  if(!strcmp(nm,"tan"))   return vreal(tan(N(a,n,0)));
  if(!strcmp(nm,"log10")) return vreal(log10(N(a,n,0)));
  if(!strcmp(nm,"logn"))  return vreal(log(N(a,n,1))/log(N(a,n,0)));
  if(!strcmp(nm,"log2"))  return vreal(log2(N(a,n,0)));
  if(!strcmp(nm,"ln"))    return vreal(log(N(a,n,0)));
  if(!strcmp(nm,"exp"))   return vreal(exp(N(a,n,0)));
  if(!strcmp(nm,"dot_product")) return vreal(N(a,n,0)*N(a,n,2)+N(a,n,1)*N(a,n,3));
  if(!strcmp(nm,"dot_product_3d"))
    return vreal(N(a,n,0)*N(a,n,3)+N(a,n,1)*N(a,n,4)+N(a,n,2)*N(a,n,5));
  /* The normalised forms scale each vector to unit length first, so the answer is the cosine of
   * the angle between them. A zero-length vector has no direction; return 0 instead
   * of dividing by zero. */
  if(!strcmp(nm,"dot_product_normalised")||!strcmp(nm,"dot_product_normalized")){
    double x1=N(a,n,0),y1=N(a,n,1),x2=N(a,n,2),y2=N(a,n,3);
    double l1=sqrt(x1*x1+y1*y1), l2=sqrt(x2*x2+y2*y2);
    if(l1<=0.0||l2<=0.0) return vreal(0);
    return vreal((x1*x2+y1*y2)/(l1*l2));
  }
  if(!strcmp(nm,"dot_product_3d_normalised")||!strcmp(nm,"dot_product_3d_normalized")){
    double x1=N(a,n,0),y1=N(a,n,1),z1=N(a,n,2),x2=N(a,n,3),y2=N(a,n,4),z2=N(a,n,5);
    double l1=sqrt(x1*x1+y1*y1+z1*z1), l2=sqrt(x2*x2+y2*y2+z2*z2);
    if(l1<=0.0||l2<=0.0) return vreal(0);
    return vreal((x1*x2+y1*y2+z1*z2)/(l1*l2));
  }
  if(!strcmp(nm,"dsin"))  return vreal(sin(N(a,n,0)*M_PI/180.0));   /* degree trig (GM classics) */
  if(!strcmp(nm,"dcos"))  return vreal(cos(N(a,n,0)*M_PI/180.0));
  if(!strcmp(nm,"dtan"))  return vreal(tan(N(a,n,0)*M_PI/180.0));
  if(!strcmp(nm,"darcsin")) return vreal(asin(N(a,n,0))*180.0/M_PI);
  if(!strcmp(nm,"darccos")) return vreal(acos(N(a,n,0))*180.0/M_PI);
  if(!strcmp(nm,"darctan")) return vreal(atan(N(a,n,0))*180.0/M_PI);
  if(!strcmp(nm,"darctan2")) return vreal(atan2(N(a,n,0),N(a,n,1))*180.0/M_PI);
  if(!strcmp(nm,"math_get_epsilon")) return vreal(vm?vm->math_epsilon:1e-5);
  if(!strcmp(nm,"math_set_epsilon")){ builtin_math_set_epsilon(vm,N(a,n,0)); return vreal(0); }
  if(!strcmp(nm,"random")) return vreal(gml_rng_value(vm) * N(a,n,0));  /* GM WELL512: (next/2^32)*x */
  if(!strcmp(nm,"randomize")||!strcmp(nm,"randomise")){
    /* GML_RANDOMIZE_SEED optionally fixes the seed; otherwise derive it from the clock and VM address. */
    GmlBuiltinState *state=builtin_state_ensure(vm);
    if(state && !state->random_seed_initialized){
      const char *e=builtin_setting(vm,"GML_RANDOMIZE_SEED");
      state->fixed_random_seed=e?atol(e):-1;
      state->random_seed_initialized=1;
    }
    long fixed=state?state->fixed_random_seed:-1;
    uint64_t host_seed=gml_host_random_seed(vm);
    uint32_t s=fixed>=0?(uint32_t)fixed:(uint32_t)(host_seed^(host_seed>>32));
    vm->rng_state=s; gml_rng_seed(vm,s); return vreal(0); }
  if(!strcmp(nm,"random_set_seed")){ uint32_t s=(uint32_t)(int64_t)N(a,n,0); vm->rng_state=s; gml_rng_seed(vm,s); return vreal(0); }
  if(!strcmp(nm,"random_get_seed"))
    return vreal((double)((vm->win&&anygm_policy_uses_classic_runtime(vm->win))?vm->rng_classic_state:vm->rng_state));
  if(!strncmp(nm,"http_",5)&&(!strcmp(nm,"http_get")||!strcmp(nm,"http_get_file")||!strcmp(nm,"http_post_string")||!strcmp(nm,"http_request")))
    return builtin_http_request_stub(vm);
  if(!strcmp(nm,"random_range")){ double a0=N(a,n,0), a1=N(a,n,1); return vreal(a0 + gml_rng_value(vm)*(a1-a0)); }
  if(!strcmp(nm,"irandom")){
    double upper=floor(N(a,n,0));
    uint64_t mx=upper>=(double)INT64_MAX?(uint64_t)INT64_MAX:
                upper>0.0?(uint64_t)upper:0u;
    return vreal((double)gml_rng_integer(vm,mx)); }
  if(!strcmp(nm,"irandom_range")){ int lo=(int)floor(N(a,n,0)), hi=(int)floor(N(a,n,1));
    if(hi<lo){ int t=lo; lo=hi; hi=t; }
    return vreal(lo+(double)gml_rng_integer(vm,(uint64_t)((int64_t)hi-(int64_t)lo))); }
  if(!strcmp(nm,"choose")){
    if(n<=0) return vreal(0);
    return a[(int)gml_rng_select(vm,(uint64_t)n)]; }
  if(!strcmp(nm,"clamp")){ double x=N(a,n,0), lo=N(a,n,1), hi=N(a,n,2); if(x<lo)x=lo; if(x>hi)x=hi; return vreal(x); }
  if(!strcmp(nm,"lerp")){ double a0=N(a,n,0), a1=N(a,n,1), t=N(a,n,2); return vreal(a0 + (a1-a0)*t); }
  if(!strcmp(nm,"mean")){ double s=0; for(int i=0;i<n;i++) s+=N(a,n,i); return vreal(n? s/n : 0); }
  if(!strcmp(nm,"min")){ if(n<=0) return vreal(0); double v=N(a,n,0); for(int i=1;i<n;i++){ double x=N(a,n,i); if(x<v) v=x; } return vreal(v); }
  if(!strcmp(nm,"max")){ if(n<=0) return vreal(0); double v=N(a,n,0); for(int i=1;i<n;i++){ double x=N(a,n,i); if(x>v) v=x; } return vreal(v); }
  if(!strcmp(nm,"min3") || !strcmp(nm,"max3")){
    if(n<3) return vreal(0);
    return gml_builtin_try_values_math(vm,!strcmp(nm,"min3")?"min":"max",a,3);
  }
  if(!strcmp(nm,"power")) return vreal(pow(N(a,n,0),N(a,n,1)));
  if(!strcmp(nm,"db_to_lin")) return vreal(pow(10.0,N(a,n,0)/20.0));
  if(!strcmp(nm,"lin_to_db")) return vreal(20.0*log10(N(a,n,0)));
  if(!strcmp(nm,"real")) return vreal(N(a,n,0));
  if(!strcmp(nm,"bool")){  /* GMS2.3: numeric -> 0/1; the strings "true"/"1" -> 1 */
    if(n>=1 && a[0].t==V_STR && a[0].s) return vreal(!strcmp(a[0].s,"true")||!strcmp(a[0].s,"1"));
    return vreal(N(a,n,0)!=0); }
  if(!strcmp(nm,"degtorad")) return vreal(N(a,n,0)*M_PI/180.0);
  if(!strcmp(nm,"median")){   /* middle value; even count -> lower middle (games: median(lo,v,hi) clamp) */
    double v[16]; int m=n>16?16:n;
    if(m<=0) return vreal(0);
    for(int i=0;i<m;i++) v[i]=N(a,n,i);
    for(int i=0;i<m;i++) for(int j=i+1;j<m;j++) if(v[j]<v[i]){ double t=v[i]; v[i]=v[j]; v[j]=t; }
    return vreal(v[(m-1)/2]); }
  if(!strcmp(nm,"mean")){ double t=0; for(int i=0;i<n;i++) t+=N(a,n,i); return vreal(n>0?t/n:0); }
  if(!strcmp(nm,"darctan2")) return vreal(atan2(N(a,n,0),N(a,n,1))*180.0/M_PI);
  if(!strcmp(nm,"arctan"))  return vreal(atan(N(a,n,0)));    /* radians */
  if(!strcmp(nm,"arcsin"))  return vreal(asin(N(a,n,0)));
  if(!strcmp(nm,"arccos"))  return vreal(acos(N(a,n,0)));
  if(!strcmp(nm,"arctan2")) return vreal(atan2(N(a,n,0),N(a,n,1)));
  if(!strcmp(nm,"radtodeg")) return vreal(N(a,n,0)*180.0/M_PI);
  if(!strcmp(nm,"degtorad")) return vreal(N(a,n,0)*M_PI/180.0);
  if(!strcmp(nm,"make_color")||!strcmp(nm,"make_colour")||
     !strcmp(nm,"make_color_rgb")||!strcmp(nm,"make_colour_rgb"))
    return vreal((double)((int)N(a,n,0) + ((int)N(a,n,1)<<8) + ((int)N(a,n,2)<<16)));
  if(!strcmp(nm,"make_color_hsv")||!strcmp(nm,"make_colour_hsv")){
    double h=fmod(N(a,n,0),256.0); if(h<0) h+=256.0; double s=N(a,n,1)/255.0, v=N(a,n,2)/255.0;
    double c=v*s, hp=h/42.5, x=c*(1-fabs(fmod(hp,2)-1)), m=v-c, r=0,g=0,b=0;
    if(hp<1){ r=c; g=x; } else if(hp<2){ r=x; g=c; } else if(hp<3){ g=c; b=x; }
    else if(hp<4){ g=x; b=c; } else if(hp<5){ r=x; b=c; } else { r=c; b=x; }
    return vreal((int)((r+m)*255) + ((int)((g+m)*255)<<8) + ((int)((b+m)*255)<<16));
  }
  if(!strcmp(nm,"merge_color")||!strcmp(nm,"merge_colour")){
    uint32_t c1=NU32(a,n,0), c2=NU32(a,n,1); double t=N(a,n,2); if(t<0)t=0; if(t>1)t=1;
    int r=(int)((c1&255)*(1-t)+(c2&255)*t), g=(int)(((c1>>8)&255)*(1-t)+((c2>>8)&255)*t), b=(int)(((c1>>16)&255)*(1-t)+((c2>>16)&255)*t);
    return vreal(r+(g<<8)+(b<<16));
  }
  if(!strcmp(nm,"point_direction")){ double dx=N(a,n,2)-N(a,n,0), dy=N(a,n,3)-N(a,n,1);
    double r=atan2(-dy,dx)*180.0/M_PI; if(r<0)r+=360; return vreal(r); }
  if(!strcmp(nm,"distance_to_point")){ GmlInstance*s=vm->cur_self; if(!s)return vreal(0);
    return vreal(point_to_instance_distance(vm,s,N(a,n,0),N(a,n,1))); }
  if(!strcmp(nm,"point_distance")) return vreal(hypot(N(a,n,2)-N(a,n,0),N(a,n,3)-N(a,n,1)));
  /* Standard-GML pure predicates/getters require explicit dispatch rather than
   * the silent catch-all -> 0. GM colours are r + (g<<8) + (b<<16); HSV is 0-255 (see
   * make_color_hsv above: 6 sectors -> *42.5). */
  if(!strcmp(nm,"color_get_red")||!strcmp(nm,"colour_get_red")) return vreal((int)N(a,n,0)&255);
  if(!strcmp(nm,"color_get_green")||!strcmp(nm,"colour_get_green")) return vreal(((int)N(a,n,0)>>8)&255);
  if(!strcmp(nm,"color_get_blue")||!strcmp(nm,"colour_get_blue")) return vreal(((int)N(a,n,0)>>16)&255);
  if(!strcmp(nm,"color_get_hue")||!strcmp(nm,"colour_get_hue")||
     !strcmp(nm,"color_get_saturation")||!strcmp(nm,"colour_get_saturation")||
     !strcmp(nm,"color_get_value")||!strcmp(nm,"colour_get_value")){
    int col=(int)N(a,n,0); double r=(col&255)/255.0,g=((col>>8)&255)/255.0,b=((col>>16)&255)/255.0;
    double mx=r>g?(r>b?r:b):(g>b?g:b), mn=r<g?(r<b?r:b):(g<b?g:b), dl=mx-mn;
    if(strstr(nm,"value")) return vreal((int)(mx*255+0.5));
    if(strstr(nm,"saturation")) return vreal(mx<=0?0:(int)(dl/mx*255+0.5));
    double h=0; if(dl>0){ if(mx==r) h=fmod((g-b)/dl,6.0); else if(mx==g) h=(b-r)/dl+2.0; else h=(r-g)/dl+4.0; if(h<0)h+=6.0; }
    return vreal((int)(h*42.5+0.5)%255);   /* hue 0-255 */
  }
  if(!strcmp(nm,"point_in_rectangle")){ double px=N(a,n,0),py=N(a,n,1),x1=N(a,n,2),y1=N(a,n,3),x2=N(a,n,4),y2=N(a,n,5);
    return vreal((px>=x1&&py>=y1&&px<=x2&&py<=y2)?1:0); }
  if(!strcmp(nm,"point_in_circle")){ double px=N(a,n,0),py=N(a,n,1),cx=N(a,n,2),cy=N(a,n,3),rr=N(a,n,4);
    double dx=px-cx,dy=py-cy; return vreal((dx*dx+dy*dy)<=rr*rr?1:0); }
  if(!strcmp(nm,"point_in_triangle")){ double px=N(a,n,0),py=N(a,n,1),x1=N(a,n,2),y1=N(a,n,3),x2=N(a,n,4),y2=N(a,n,5),x3=N(a,n,6),y3=N(a,n,7);
    double d1=(px-x2)*(y1-y2)-(x1-x2)*(py-y2), d2=(px-x3)*(y2-y3)-(x2-x3)*(py-y3), d3=(px-x1)*(y3-y1)-(x3-x1)*(py-y1);
    int hasNeg=(d1<0)||(d2<0)||(d3<0), hasPos=(d1>0)||(d2>0)||(d3>0); return vreal(!(hasNeg&&hasPos)?1:0); }
  if(!strcmp(nm,"rectangle_in_triangle")){
    double rx1=N(a,n,0),ry1=N(a,n,1),rx2=N(a,n,2),ry2=N(a,n,3);
    double x1=N(a,n,4),y1=N(a,n,5),x2=N(a,n,6),y2=N(a,n,7),x3=N(a,n,8),y3=N(a,n,9);
    if(rx1>rx2){ double t=rx1; rx1=rx2; rx2=t; } if(ry1>ry2){ double t=ry1; ry1=ry2; ry2=t; }
    double px[4]={rx1,rx2,rx1,rx2}, py[4]={ry1,ry1,ry2,ry2};
    for(int i=0;i<4;i++){
      double d1=(px[i]-x2)*(y1-y2)-(x1-x2)*(py[i]-y2), d2=(px[i]-x3)*(y2-y3)-(x2-x3)*(py[i]-y3);
      double d3=(px[i]-x1)*(y3-y1)-(x3-x1)*(py[i]-y1);
      int hasNeg=(d1<0)||(d2<0)||(d3<0), hasPos=(d1>0)||(d2>0)||(d3>0);
      if(!(hasNeg&&hasPos)) return vreal(1);
    }
    return vreal(point_in_poly(x1,y1,px,py,4)||point_in_poly(x2,y2,px,py,4)||point_in_poly(x3,y3,px,py,4));
  }
  if(!strcmp(nm,"angle_difference")){ double d=fmod(N(a,n,0)-N(a,n,1),360.0); if(d<-180.0)d+=360.0; if(d>180.0)d-=360.0; return vreal(d); }
  if(!strcmp(nm,"frac")) { double v=N(a,n,0); return vreal(v-floor(v)); }   /* fractional part */
  if(!strcmp(nm,"array_length_1d")) return vreal(n>0?gml_val_array_length(a[0]):0);
  /* Newer array_length(arr) returns the top-level element or row count, matching
   * array_length_1d. A zero-valued fallback can make bounds-check helpers recurse indefinitely. */
  if(!strcmp(nm,"array_length")) return vreal(n>0?gml_val_array_length(a[0]):0);
  /* GMS2.3 array-function forms. array_create(size,[val]); get/set(arr,i[,v]); push/pop(arr[,v]);
   * resize(arr,size); copy(dst,di,src,si,count). Operate on the array VALUE (a reference), so set/
   * push/resize mutate the caller's array in place — the same object arr[i] syntax touches. */
  if(!strcmp(nm,"array_create_ext")) return gml_array_create_ext(vm,a,n);
  if(!strcmp(nm,"array_map_ext")) return gml_array_map_ext(vm,a,n);
  if(!strcmp(nm,"array_foreach")) return gml_array_foreach(vm,a,n);
  if(!strcmp(nm,"array_create")){ int sz=n>0?(int)N(a,n,0):0; GmlVal fill=n>1?a[1]:vreal(0); return gml_arr_new(sz,fill); }
  if(!strcmp(nm,"array_get")){ return n>1?gml_arr_get(a[0],(int)N(a,n,1)):vreal(0); }
  if(!strcmp(nm,"array_set")){ if(n>2) gml_arr_set(a[0],(int)N(a,n,1),a[2]); return vreal(0); }
  if(!strcmp(nm,"array_get_2D")||!strcmp(nm,"array_get_2d")){
    return n>2?gml_arr_get_2d(a[0],(int)N(a,n,1),(int)N(a,n,2)):vreal(0); }
  if(!strcmp(nm,"array_set_2D")||!strcmp(nm,"array_set_2d")){
    if(n>3) gml_arr_set_2d(a[0],(int)N(a,n,1),(int)N(a,n,2),a[3]);
    return vreal(0); }
  if(!strcmp(nm,"array_push")){ if(n>1){ for(int i=1;i<n;i++) gml_arr_push(a[0],a[i]); } return vreal(0); }
  if(!strcmp(nm,"array_pop")){ return n>0?gml_arr_pop(a[0]):vreal(0); }
  if(!strcmp(nm,"array_resize")){ if(n>1) gml_arr_resize(a[0],(int)N(a,n,1)); return vreal(0); }
  if(!strcmp(nm,"array_copy")){ if(n>4) gml_arr_copy(a[0],(int)N(a,n,1),a[2],(int)N(a,n,3),(int)N(a,n,4)); return vreal(0); }
  if(!strcmp(nm,"array_concat")){
    /* A new top-level array; element ownership follows the existing array-copy
     * operation. Reject invalid/oversized requests before copying any prefix. */
    if(n<2) return vundef();
    int total=0;
    for(int i=0;i<n;i++){
      if(a[i].t!=V_ARR || !a[i].arr) return vundef();
      int count=gml_val_array_length(a[i]);
      if(count<0 || count>16000000-total) return vundef();
      total+=count;
    }
    GmlVal result=gml_arr_new(total,vreal(0));
    if(!result.arr || gml_val_array_length(result)!=total){
      gml_values_release(&result,1);
      return vundef();
    }
    int offset=0;
    for(int i=0;i<n;i++){
      int count=gml_val_array_length(a[i]);
      gml_arr_copy(result,offset,a[i],0,count);
      offset+=count;
    }
    return result;
  }
  if(!strcmp(nm,"array_insert")){ if(n>2) gml_arr_insert(a[0],(int)N(a,n,1),a+2,n-2); return vreal(0); }
  if(!strcmp(nm,"array_equals")) return vreal(n>1 && array_equals_recursive(vm,a[0],a[1]));
  if(!strcmp(nm,"array_get_index")) return vreal(gml_array_search_index(a,n));
  if(!strcmp(nm,"array_union")){
    GmlVal result=gml_arr_new(0,vreal(0));
    GmlArr *destination=result.arr;
    for(int source_index=0;source_index<n;source_index++){
      if(a[source_index].t!=V_ARR || !a[source_index].arr) continue;
      GmlArr *source=a[source_index].arr;
      for(int value_index=0;value_index<source->len;value_index++){
        int duplicate=0;
        for(int result_index=0;result_index<destination->len;result_index++)
          if(ds_val_equal(destination->data[result_index],source->data[value_index])){
            duplicate=1;
            break;
          }
        if(!duplicate) gml_arr_push(result,source->data[value_index]);
      }
    }
    return result;
  }
  if(!strcmp(nm,"array_sort")){ if(n>0) gml_array_sort(a[0], n<2 || N(a,n,1)!=0); return vreal(0); }
  if(!strcmp(nm,"array_shuffle")) return n>0?gml_array_shuffle_copy(vm,a[0],a,n):gml_arr_new(0,vreal(0));
  if(!strcmp(nm,"array_contains")){
    if(n<2 || a[0].t!=V_ARR || !a[0].arr) return vreal(0);
    GmlArr *A=(GmlArr*)a[0].arr;
    int off=n>2?(int)N(a,n,2):0;
    if(off<0) off+=A->len;
    if(off<0 || off>=A->len) return vreal(0);
    int count=n>3?(int)N(a,n,3):(A->len-off);
    int step=count<0?-1:1, left=count<0?-count:count;
    for(int i=off; left>0 && i>=0 && i<A->len; i+=step,left--)
      if(ds_val_equal(A->data[i],a[1])) return vreal(1);
    return vreal(0);
  }
  /* array_delete(arr,index,number): remove `number` elements at `index`, shifting the tail down
   * (GMS2.3). Negative number deletes that many BEFORE index. Was a silent no-op. */
  if(!strcmp(nm,"array_delete")){
    if(n>2 && a[0].t==V_ARR && a[0].arr){ GmlArr *A=a[0].arr; int idx=(int)N(a,n,1), cnt=(int)N(a,n,2);
      if(cnt<0){ idx+=cnt; cnt=-cnt; }            /* delete before index */
      if(idx<0){ cnt+=idx; idx=0; }
      if(cnt>0 && idx<A->len){ if(idx+cnt>A->len) cnt=A->len-idx;
        for(int i=idx; i+cnt<A->len; i++) A->data[i]=A->data[i+cnt];
        A->len-=cnt; } }
    return vreal(0);
  }
  if(!strcmp(nm,"array_height_2d")) return vreal(n>0?gml_val_array_height_2d(a[0]):0);
  if(!strcmp(nm,"array_length_2d")){
    if(n<=0) return vreal(0);
    return vreal(gml_val_array_length_2d(a[0],(int)N(a,n,1)));
  }
  if(!strcmp(nm,"lengthdir_x")) return vreal(N(a,n,0)*cos(N(a,n,1)*M_PI/180.0));
  if(!strcmp(nm,"lengthdir_y")) return vreal(-N(a,n,0)*sin(N(a,n,1)*M_PI/180.0));  /* GM y down */
  if(!strcmp(nm,"distance_to_object"))
    return vreal(distance_to_target(vm,vm->cur_self,(int)N(a,n,0)));
  /* move_towards_point(x,y,sp): head toward (x,y) at speed sp (sets direction+speed → hspeed/vspeed). */
  if(!strcmp(nm,"move_towards_point")){ GmlInstance*s=vm->cur_self; if(s){
      double dir=atan2(-(N(a,n,1)-s->y),N(a,n,0)-s->x)*180.0/M_PI; double sp=N(a,n,2);
      if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
        dir=fmod(dir,360.0); if(dir<0) dir+=360.0;
      }
      s->direction=dir; s->speed=sp;
      s->hspeed=sp*cos(dir*M_PI/180.0); s->vspeed=-sp*sin(dir*M_PI/180.0);
      if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
        double rounded=round(s->hspeed);
        if(fabs(rounded-s->hspeed)<0.0001) s->hspeed=rounded;
        rounded=round(s->vspeed);
        if(fabs(rounded-s->vspeed)<0.0001) s->vspeed=rounded;
      } }
    return vreal(0); }
  if(!strcmp(nm,"motion_set")){ GmlInstance*s=vm->cur_self; if(s){
      s->direction=N(a,n,0); s->speed=N(a,n,1);
      if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
        s->direction=fmod(s->direction,360.0); if(s->direction<0) s->direction+=360.0;
      }
      s->hspeed=s->speed*cos(s->direction*M_PI/180.0);
      s->vspeed=-s->speed*sin(s->direction*M_PI/180.0);
      if(vm->win && anygm_policy_uses_classic_runtime(vm->win)){
        double rounded=round(s->hspeed);
        if(fabs(rounded-s->hspeed)<0.0001) s->hspeed=rounded;
        rounded=round(s->vspeed);
        if(fabs(rounded-s->vspeed)<0.0001) s->vspeed=rounded;
      } }
    return vreal(0); }
  if(!strcmp(nm,"motion_add")){ GmlInstance*s=vm->cur_self; if(s){
      double dir=N(a,n,0)*M_PI/180.0, amount=N(a,n,1);
      s->hspeed+=amount*cos(dir); s->vspeed-=amount*sin(dir);
      motion_from_components(s); }
    return vreal(0); }
  return gml_builtin_try_collision_planning(vm,nm,a,n);
}
static GmlVal string_filter_ascii(const char *s, int digits){
  size_t n=s?strlen(s):0;
  char *o=malloc(n+1);
  if(!o) return vstr("");
  size_t k=0;
  for(size_t i=0;i<n;i++){
    unsigned char c=(unsigned char)s[i];
    if((c>='A'&&c<='Z') || (c>='a'&&c<='z') || (digits && c>='0'&&c<='9'))
      o[k++]=(char)c;
  }
  o[k]=0;
  return vstr_owned(o);
}
static int gm_string_count(const char *needle, const char *hay){
  if(!needle || !hay || !needle[0]) return 0;
  int n=0;
  size_t nl=strlen(needle);
  const char *p=hay;
  while((p=strstr(p,needle))){
    n++;
    p += nl;
  }
  return n;
}

static GmlVal gml_string_concat(GmlVal *args, int count){
  size_t length=0;
  for(int index=0;index<count;index++){
    char formatted[64];
    const char *value=gm_string_format(args[index],formatted);
    size_t value_length=strlen(value);
    if(value_length>SIZE_MAX-length-1) return vstr_owned(strdup(""));
    length+=value_length;
  }
  char *output=malloc(length+1);
  if(!output) return vstr_owned(strdup(""));
  size_t used=0;
  for(int index=0;index<count;index++){
    char formatted[64];
    const char *value=gm_string_format(args[index],formatted);
    size_t value_length=strlen(value);
    memcpy(output+used,value,value_length);
    used+=value_length;
  }
  output[used]=0;
  return vstr_owned(output);
}

static GmlVal gml_string_concat_ext(GmlVal *args, int count){
  if(count<1 || args[0].t!=V_ARR || !args[0].arr)
    return vstr_owned(strdup(""));
  GmlArr *array=(GmlArr*)args[0].arr;
  if(array->len<=0) return vstr_owned(strdup(""));

  double raw_offset=count>1?N(args,count,1):0.0;
  int offset=0;
  if(isnan(raw_offset)) raw_offset=0.0;
  if(isinf(raw_offset)) offset=raw_offset<0.0?0:array->len-1;
  else {
    double integral=trunc(raw_offset);
    if(integral<0.0) integral+=(double)array->len;
    if(integral<=0.0) offset=0;
    else if(integral>=(double)(array->len-1)) offset=array->len-1;
    else offset=(int)integral;
  }

  int direction=1;
  int length=array->len-offset;
  if(count>2){
    double raw_length=N(args,count,2);
    if(isnan(raw_length)) raw_length=0.0;
    direction=raw_length<0.0?-1:1;
    int available=direction>0?array->len-offset:offset+1;
    if(isinf(raw_length)) length=available;
    else {
      double integral=trunc(fabs(raw_length));
      length=integral>=(double)available?available:(int)integral;
    }
  }

  size_t used=0, capacity=32;
  char *output=malloc(capacity);
  if(!output) return vstr_owned(strdup(""));
  for(int visited=0,index=offset;visited<length;visited++,index+=direction){
    char formatted[64];
    const char *value=gm_string_format(array->data[index],formatted);
    size_t value_length=strlen(value);
    if(value_length>SIZE_MAX-used-1){ free(output); return vstr_owned(strdup("")); }
    size_t required=used+value_length+1;
    if(required>capacity){
      size_t grown=capacity;
      while(grown<required && grown<=SIZE_MAX/2) grown*=2;
      if(grown<required) grown=required;
      char *replacement=realloc(output,grown);
      if(!replacement){ free(output); return vstr_owned(strdup("")); }
      output=replacement; capacity=grown;
    }
    memcpy(output+used,value,value_length); used+=value_length;
  }
  output[used]=0;
  return vstr_owned(output);
}
static int utf8_character_bytes(const unsigned char *text){
  if(!text || !text[0]) return 0;
  if(text[0]<0x80) return 1;
  if((text[0]&0xe0)==0xc0 && text[1] && (text[1]&0xc0)==0x80){
    unsigned value=((unsigned)(text[0]&0x1f)<<6)|(text[1]&0x3f);
    if(value>=0x80) return 2;
  } else if((text[0]&0xf0)==0xe0 && text[1] && text[2] &&
            (text[1]&0xc0)==0x80 && (text[2]&0xc0)==0x80){
    unsigned value=((unsigned)(text[0]&15)<<12)|((unsigned)(text[1]&0x3f)<<6)|(text[2]&0x3f);
    if(value>=0x800 && (value<0xd800 || value>0xdfff)) return 3;
  } else if((text[0]&0xf8)==0xf0 && text[1] && text[2] && text[3] &&
            (text[1]&0xc0)==0x80 && (text[2]&0xc0)==0x80 && (text[3]&0xc0)==0x80){
    unsigned value=((unsigned)(text[0]&7)<<18)|((unsigned)(text[1]&0x3f)<<12)|
                   ((unsigned)(text[2]&0x3f)<<6)|(text[3]&0x3f);
    if(value>=0x10000 && value<=0x10ffff) return 4;
  }
  return 1;
}
/* Unicode hash variants consume UTF-16 code units. Runtime strings are UTF-8 here,
 * so encode valid scalar values as little-endian UTF-16 and replace malformed sequences. */
static uint8_t *utf16le_alloc(const char *text, size_t *out_len){
  const uint8_t *p=(const uint8_t*)(text?text:"");
  size_t input=strlen((const char*)p), cap=input<=SIZE_MAX/2 ? input*2+2 : 0, used=0;
  uint8_t *out=cap?malloc(cap):NULL;
  if(!out){ if(out_len) *out_len=0; return NULL; }
  while(*p){
    uint32_t cp; size_t take=1;
    if(p[0]<0x80) cp=p[0];
    else if((p[0]&0xe0)==0xc0 && p[1] && (p[1]&0xc0)==0x80){
      cp=((uint32_t)(p[0]&0x1f)<<6)|(p[1]&0x3f); take=2;
      if(cp<0x80) cp=0xfffd, take=1;
    } else if((p[0]&0xf0)==0xe0 && p[1] && p[2] &&
              (p[1]&0xc0)==0x80 && (p[2]&0xc0)==0x80){
      cp=((uint32_t)(p[0]&15)<<12)|((uint32_t)(p[1]&0x3f)<<6)|(p[2]&0x3f); take=3;
      if(cp<0x800 || (cp>=0xd800 && cp<=0xdfff)) cp=0xfffd, take=1;
    } else if((p[0]&0xf8)==0xf0 && p[1] && p[2] && p[3] &&
              (p[1]&0xc0)==0x80 && (p[2]&0xc0)==0x80 && (p[3]&0xc0)==0x80){
      cp=((uint32_t)(p[0]&7)<<18)|((uint32_t)(p[1]&0x3f)<<12)|
         ((uint32_t)(p[2]&0x3f)<<6)|(p[3]&0x3f); take=4;
      if(cp<0x10000 || cp>0x10ffff) cp=0xfffd, take=1;
    } else cp=0xfffd;
    p+=take;
    if(cp<=0xffff){ out[used++]=(uint8_t)cp; out[used++]=(uint8_t)(cp>>8); }
    else {
      cp-=0x10000; uint16_t hi=(uint16_t)(0xd800+(cp>>10)),lo=(uint16_t)(0xdc00+(cp&1023));
      out[used++]=(uint8_t)hi; out[used++]=(uint8_t)(hi>>8);
      out[used++]=(uint8_t)lo; out[used++]=(uint8_t)(lo>>8);
    }
  }
  if(out_len) *out_len=used;
  return out;
}

/* The trim predicate for string_trim and its two one-sided variants. It takes the character set as
 * an argument instead of closing over the caller's local, because nested functions are a GCC
 * extension that no other supported compiler accepts. */
static int string_trim_matches(const char *set, char ch){
  return (set&&*set) ? strchr(set,ch)!=NULL
       : (set)       ? 0
                     : isspace((unsigned char)ch)!=0;
}

/* string(template, ...) substitutes numbered placeholders from subsequent arguments.
 * Repeated or reordered indices refer to the same argument positions. A placeholder without
 * an available argument remains unchanged rather than inventing a value. */
static GmlVal gml_string_format(GmlVM *vm, GmlVal *a, int n){
  const char *fmt=S(vm,a,n,0);
  if(!fmt) return vstr_owned(strdup(""));
  size_t cap=strlen(fmt)+64, used=0;
  char *out=malloc(cap);
  if(!out) return vstr_owned(strdup(fmt));
  for(const char *p=fmt; *p; ){
    const char *open=p;
    int index=-1;
    if(*p=='{'){
      const char *q=p+1;
      int value=0, digits=0;
      while(*q>='0' && *q<='9' && digits<9){ value=value*10+(*q-'0'); q++; digits++; }
      if(digits>0 && *q=='}'){ index=value; p=q+1; }
    }
    const char *piece=NULL;
    char one[2];
    size_t len;
    if(index>=0 && index+1<n){
      piece=S(vm,a,n,index+1);
      if(!piece) piece="";
      len=strlen(piece);
    } else if(index>=0){
      /* No argument for this index: keep the placeholder exactly as written. */
      piece=open; len=(size_t)(p-open);
    } else {
      one[0]=*p; one[1]=0; piece=one; len=1; p++;
    }
    if(used+len+1>cap){
      size_t want=(used+len+1)*2;
      char *grown=realloc(out,want);
      if(!grown){ free(out); return vstr_owned(strdup(fmt)); }
      out=grown; cap=want;
    }
    memcpy(out+used,piece,len); used+=len;
  }
  out[used]=0;
  return vstr_owned(out);
}

GmlVal gml_builtin_try_values_strings(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  /* ---- strings ---- */
  if(!strcmp(nm,"string"))
    return n>1 ? gml_string_format(vm,a,n) : vstr_owned(strdup(S(vm,a,n,0)));
  if(!strcmp(nm,"string_length")) return vreal((double)strlen(S(vm,a,n,0)));
  if(!strcmp(nm,"string_byte_length")) return vreal((double)strlen(S(vm,a,n,0)));
  if(!strcmp(nm,"string_concat")) return gml_string_concat(a,n);
  if(!strcmp(nm,"string_concat_ext")) return gml_string_concat_ext(a,n);
  if(!strcmp(nm,"string_starts_with")){
    const char *text=S(vm,a,n,0), *prefix=S(vm,a,n,1);
    size_t prefix_length=strlen(prefix);
    return vreal(prefix_length<=strlen(text) && !memcmp(text,prefix,prefix_length));
  }
  if(!strcmp(nm,"string_ends_with")){
    const char *text=S(vm,a,n,0), *suffix=S(vm,a,n,1);
    size_t text_length=strlen(text), suffix_length=strlen(suffix);
    return vreal(suffix_length<=text_length &&
                 !memcmp(text+text_length-suffix_length,suffix,suffix_length));
  }
  if(!strcmp(nm,"string_foreach")){
    if(n<2) return vreal(0);
    const char *text=S(vm,a,n,0); size_t bytes=strlen(text);
    if(bytes==0 || bytes>INT_MAX) return vreal(0);
    const char **start=malloc(bytes*sizeof(*start));
    unsigned char *width=malloc(bytes);
    if(!start || !width){ free(start); free(width); return vreal(0); }
    int characters=0;
    for(const unsigned char *cursor=(const unsigned char*)text;*cursor;){
      int length=utf8_character_bytes(cursor);
      start[characters]=(const char*)cursor; width[characters]=(unsigned char)length;
      characters++; cursor+=length;
    }
    double raw_position=n>2?N(a,n,2):1.0;
    if(isnan(raw_position)) raw_position=1.0;
    int position;
    if(isinf(raw_position)) position=raw_position<0.0?1:characters;
    else {
      double integral=trunc(raw_position);
      if(integral<0.0) integral+=(double)characters+1.0;
      if(integral<=1.0) position=1;
      else if(integral>=(double)characters) position=characters;
      else position=(int)integral;
    }
    int direction=1, length=characters-(position-1);
    if(n>3){
      double raw_length=N(a,n,3);
      if(isnan(raw_length)) raw_length=0.0;
      direction=raw_length<0.0?-1:1;
      int available=direction>0?characters-(position-1):position;
      if(isinf(raw_length)) length=available;
      else {
        double integral=trunc(fabs(raw_length));
        length=integral>=(double)available?available:(int)integral;
      }
    }
    for(int visited=0,index=position-1;visited<length;visited++,index+=direction){
      char character[5]={0}; memcpy(character,start[index],width[index]);
      GmlVal callback_args[2]={vstr(character),vreal(index+1)};
      GmlVal result=gml_vm_call_callable(vm,a[1],callback_args,2);
      if(result.t==V_STR && result.s && result.d!=0) free((void*)result.s);
    }
    free(start); free(width);
    return vreal(0);
  }
  if(!strcmp(nm,"md5_string_utf8")){ const char *s=S(vm,a,n,0); return md5_hex_val((const uint8_t*)s,strlen(s)); }
  if(!strcmp(nm,"md5_string_unicode")){ const char *s=S(vm,a,n,0); size_t len=0; uint8_t *u=utf16le_alloc(s,&len);
    GmlVal out=md5_hex_val(u,len); free(u); return out; }
  if(!strcmp(nm,"sha1_string_utf8")){ const char *s=S(vm,a,n,0); return sha1_hex_val((const uint8_t*)s,strlen(s)); }
  if(!strcmp(nm,"sha1_string_unicode")){ const char *s=S(vm,a,n,0); size_t len=0; uint8_t *u=utf16le_alloc(s,&len);
    GmlVal out=sha1_hex_val(u,len); free(u); return out; }
  if(!strcmp(nm,"string_count")) return vreal(gm_string_count(S(vm,a,n,0),S(vm,a,n,1)));
  if(!strcmp(nm,"string_split")){
    const char *src=S(vm,a,n,0), *sep=S(vm,a,n,1);
    int seplen=(int)strlen(sep);
    GmlVal arr=gml_arr_new(0,vreal(0));
    if(seplen<=0){ gml_arr_push(arr,vstr(src)); return arr; }
    const char *p=src;
    for(;;){
      const char *q=strstr(p,sep);
      size_t len=q ? (size_t)(q-p) : strlen(p);
      char *part=malloc(len+1);
      if(part){ memcpy(part,p,len); part[len]=0; gml_arr_push(arr,vstr(part)); free(part); }
      else gml_arr_push(arr,vstr(""));
      if(!q) break;
      p=q+seplen;
    }
    return arr;
  }
  if(!strcmp(nm,"string_split_ext")){
    const char *src=S(vm,a,n,0);
    GmlVal out=gml_arr_new(0,vreal(0));
    int remove_empty=n>2 && N(a,n,2)!=0.0;
    int splits_left=n>3?(int)N(a,n,3):-1;
    if(splits_left<0) splits_left=INT_MAX;
    const char *p=src;
    for(;;){
      const char *best=NULL;
      size_t best_len=0;
      if(splits_left>0 && n>1 && a[1].t==V_ARR && a[1].arr){
        GmlArr *D=(GmlArr*)a[1].arr;
        for(int i=0;i<D->len;i++) if(D->data[i].t==V_STR && D->data[i].s && D->data[i].s[0]){
          const char *q=strstr(p,D->data[i].s);
          if(q && (!best || q<best)){ best=q; best_len=strlen(D->data[i].s); }
        }
      } else if(splits_left>0 && n>1){
        const char *sep=S(vm,a,n,1);
        if(sep[0]){ best=strstr(p,sep); best_len=strlen(sep); }
      }
      size_t len=best?(size_t)(best-p):strlen(p);
      if(len || !remove_empty){ char *part=dup_n(p,(int)len); gml_arr_push(out,vstr(part)); free(part); }
      if(!best) break;
      p=best+best_len;
      splits_left--;
    }
    return out;
  }
  if(!strcmp(nm,"string_copy")){ const char*s=S(vm,a,n,0); int idx=(int)N(a,n,1), cnt=(int)N(a,n,2);
    int len=strlen(s); if(idx<1)idx=1; if(idx>len)return vstr("");
    if(cnt<0) cnt=0;
    if(idx-1+cnt>len) cnt=len-(idx-1);
    char*o=malloc(cnt+1); memcpy(o,s+idx-1,cnt); o[cnt]=0; return vstr_owned(o); }
  if(!strcmp(nm,"chr")){ int c=(int)N(a,n,0); char b[2]={(char)(c&0xff),0}; return vstr_owned(strdup(b)); }
  if(!strcmp(nm,"ord")){ const unsigned char*s=(const unsigned char*)S(vm,a,n,0); return vreal(s[0]); }
  /* base64_encode(str)/base64_decode(str): standard RFC-4648 (was hitting the catch-all -> 0).
   * Operates on the string's bytes; decode stops a returned C-string at the first NUL, so it is for
   * text/base64 payloads (binary blobs use the buffer_* API). */
  if(!strcmp(nm,"base64_encode")||!strcmp(nm,"base64_encode_string")){
    static const char B64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const unsigned char *u=(const unsigned char*)S(vm,a,n,0); size_t L=u?strlen((const char*)u):0;
    char *o=malloc((L+2)/3*4+1), *p=o; if(!o) return vstr("");
    size_t i=0;
    for(; i+3<=L; i+=3){ uint32_t v=((uint32_t)u[i]<<16)|((uint32_t)u[i+1]<<8)|u[i+2];
      *p++=B64[(v>>18)&63]; *p++=B64[(v>>12)&63]; *p++=B64[(v>>6)&63]; *p++=B64[v&63]; }
    if(L-i==1){ uint32_t v=(uint32_t)u[i]<<16; *p++=B64[(v>>18)&63]; *p++=B64[(v>>12)&63]; *p++='='; *p++='='; }
    else if(L-i==2){ uint32_t v=((uint32_t)u[i]<<16)|((uint32_t)u[i+1]<<8);
      *p++=B64[(v>>18)&63]; *p++=B64[(v>>12)&63]; *p++=B64[(v>>6)&63]; *p++='='; }
    *p=0; return vstr_owned(o);
  }
  if(!strcmp(nm,"base64_decode")||!strcmp(nm,"base64_decode_string")){
    int len=0; unsigned char *o=base64_decode_alloc(S(vm,a,n,0),&len);
    if(!o) return vstr("");
    o[len]=0; return vstr_owned((char*)o);
  }
  if(!strcmp(nm,"string_char_at")){ const char*s=S(vm,a,n,0); int idx=(int)N(a,n,1), len=(int)strlen(s);
    if(idx<1||idx>len) return vstr("");
    return vstr_owned(dup_n(s+idx-1,1)); }
  if(!strcmp(nm,"string_byte_at")){ const unsigned char*s=(const unsigned char*)S(vm,a,n,0); int idx=(int)N(a,n,1), len=(int)strlen((const char*)s);
    return vreal((idx>=1&&idx<=len)?s[idx-1]:0); }
  if(!strcmp(nm,"string_ord_at")){ const unsigned char*s=(const unsigned char*)S(vm,a,n,0); int idx=(int)N(a,n,1), len=(int)strlen((const char*)s);
    return vreal((idx>=1&&idx<=len)?s[idx-1]:0); }
  if(!strcmp(nm,"string_delete")){ const char*s=S(vm,a,n,0); int idx=(int)N(a,n,1), cnt=(int)N(a,n,2), len=(int)strlen(s);
    if(idx<1) idx=1;
    if(cnt<0) cnt=0;
    if(idx>len||cnt==0) return vstr_owned(strdup(s));
    if(idx-1+cnt>len) cnt=len-(idx-1);
    char *o=malloc((size_t)(len-cnt)+1); if(!o) return vstr_owned(strdup(""));
    memcpy(o,s,(size_t)(idx-1)); memcpy(o+idx-1,s+idx-1+cnt,(size_t)(len-(idx-1+cnt)+1)); return vstr_owned(o); }
  if(!strcmp(nm,"string_insert")){  /* string_insert(substr, str, index): insert BEFORE 1-based index */
    const char*sub=S(vm,a,n,0), *s=S(vm,a,n,1); int idx=(int)N(a,n,2);
    int sl=(int)strlen(sub), len=(int)strlen(s);
    if(idx<1) idx=1;
    if(idx>len+1) idx=len+1;
    char *o=malloc((size_t)(len+sl)+1); if(!o) return vstr_owned(strdup(""));
    memcpy(o,s,(size_t)(idx-1)); memcpy(o+idx-1,sub,(size_t)sl);
    memcpy(o+idx-1+sl,s+idx-1,(size_t)(len-(idx-1))+1);
    return vstr_owned(o); }
  if(!strcmp(nm,"string_replace")||!strcmp(nm,"string_replace_all")){
    /* GM: string_replace replaces the FIRST occurrence; string_replace_all replaces every one. */
    const char*s=S(vm,a,n,0), *sub=S(vm,a,n,1), *rep=S(vm,a,n,2);
    if(!sub||!sub[0]) return vstr_owned(strdup(s?s:""));
    int all=!strcmp(nm,"string_replace_all"), sl=(int)strlen(sub), rl=(int)strlen(rep?rep:"");
    /* count occurrences to size the output */
    int cnt=0; for(const char*p=s; (p=strstr(p,sub)); p+=sl){ cnt++; if(!all) break; }
    size_t out=strlen(s)+(size_t)cnt*(rl-sl>0?rl-sl:0)+1;
    char *o=malloc(out+ (size_t)cnt*(rl>sl?rl:0) +8); if(!o) return vstr_owned(strdup(""));
    char *w=o; const char*p=s; int done=0;
    while(*p){ if((all||!done) && !strncmp(p,sub,(size_t)sl)){ memcpy(w,rep?rep:"",(size_t)rl); w+=rl; p+=sl; done=1; }
      else *w++=*p++; }
    *w=0; return vstr_owned(o); }
  if(!strcmp(nm,"string_hash_to_newline")){ /* legacy text helper: keep the string intact (safer than 0) */
    const char*s=S(vm,a,n,0); return vstr_owned(strdup(s?s:"")); }
  if(!strcmp(nm,"int64")){ return vreal((double)(int64_t)N(a,n,0)); }
  if(!strcmp(nm,"ptr")){  /* pointers are opaque numbers here; the conversion must keep the value */
    return vreal(N(a,n,0)); }
  if(!strcmp(nm,"string_upper")){ const char*s=S(vm,a,n,0); int len=(int)strlen(s); char *o=dup_n(s,len);
    for(int i=0;i<len;i++) o[i]=(char)toupper((unsigned char)o[i]);
    return vstr_owned(o); }
  if(!strcmp(nm,"string_lower")){ const char*s=S(vm,a,n,0); int len=(int)strlen(s); char *o=dup_n(s,len);
    for(int i=0;i<len;i++) o[i]=(char)tolower((unsigned char)o[i]);
    return vstr_owned(o); }
  /* string_trim removes leading and trailing whitespace, or the characters of an optional second
   * argument. The start and end variants restrict the operation to one side. */
  if(!strcmp(nm,"string_trim")||!strcmp(nm,"string_trim_start")||!strcmp(nm,"string_trim_end")){
    const char *s=S(vm,a,n,0);
    const char *set=(n>1)?S(vm,a,n,1):NULL;
    size_t len=strlen(s), start=0, stop=len;
    int both=!strcmp(nm,"string_trim");
    int lead=both||!strcmp(nm,"string_trim_start");
    int trail=both||!strcmp(nm,"string_trim_end");
    /* An empty character set trims nothing, which is not the same as no set at all: passing "" is
     * a deliberate request to leave the string alone. */
    if(lead)  while(start<stop && string_trim_matches(set,s[start])) start++;
    if(trail) while(stop>start && string_trim_matches(set,s[stop-1])) stop--;
    return vstr_owned(dup_n(s+start,(int)(stop-start)));
  }
  if(!strcmp(nm,"string_letters")) return string_filter_ascii(S(vm,a,n,0),0);
  if(!strcmp(nm,"string_lettersdigits")) return string_filter_ascii(S(vm,a,n,0),1);
  if(!strcmp(nm,"string_digits")){
    const char *s=S(vm,a,n,0);
    size_t len=strlen(s);
    char *o=malloc(len+1);
    if(!o) return vstr("");
    size_t k=0;
    for(size_t i=0;i<len;i++)
      if(s[i]>='0'&&s[i]<='9') o[k++]=s[i];
    o[k]=0;
    return vstr_owned(o);
  }
  if(!strcmp(nm,"string_pos")){ const char*needle=S(vm,a,n,0), *hay=S(vm,a,n,1);
    if(!needle[0]) return vreal(0);
    char *p=strstr(hay,needle); return vreal(p ? (double)(p-hay+1) : 0); }
  if(!strcmp(nm,"string_pos_ext")){  /* the search skips the first start_pos characters */
    const char*needle=S(vm,a,n,0), *hay=S(vm,a,n,1); int start=(int)N(a,n,2);
    if(!needle[0]) return vreal(0);
    if(start<0) start=0;
    if((size_t)start>=strlen(hay)) return vreal(0);
    char *p=strstr(hay+start,needle); return vreal(p ? (double)(p-hay+1) : 0); }
  if(!strcmp(nm,"string_last_pos")){ const char*needle=S(vm,a,n,0), *hay=S(vm,a,n,1);
    if(!needle[0]) return vreal(0);
    const char *last=NULL, *p=hay;
    while((p=strstr(p,needle))){ last=p; p++; }
    return vreal(last ? (double)(last-hay+1) : 0); }
  if(!strcmp(nm,"string_last_pos_ext")){  /* a match may begin at or before start_pos characters in */
    const char*needle=S(vm,a,n,0), *hay=S(vm,a,n,1); int start=(int)N(a,n,2);
    if(!needle[0] || start<0) return vreal(0);
    const char *last=NULL, *p=hay;
    while((p=strstr(p,needle)) && p-hay<=start){ last=p; p++; }
    return vreal(last ? (double)(last-hay+1) : 0); }
  if(!strcmp(nm,"string_format")){  /* string_format(val, tot, dec): width-padded fixed-point (GM) */
    double v=N(a,n,0); int tot=(int)N(a,n,1), dec=(int)N(a,n,2);
    if(dec<0) dec=0;
    if(dec>17) dec=17;
    if(tot<0) tot=0;
    if(tot>64) tot=64;
    char buf[96]; snprintf(buf,sizeof buf,"%*.*f",tot,dec,v);
    return vstr_owned(strdup(buf)); }
  if(!strcmp(nm,"string_repeat")){ const char*s=S(vm,a,n,0); int count=(int)N(a,n,1); if(count<0) count=0;
    size_t len=strlen(s), total=len*(size_t)count; if(len && total/len!=(size_t)count) total=1024*1024;
    if(total>1024*1024) total=1024*1024;
    char *o=malloc(total+1); if(!o) return vstr_owned(strdup(""));
    size_t pos=0; for(int i=0;i<count && pos+len<=total;i++){ memcpy(o+pos,s,len); pos+=len; }
    o[pos]=0; return vstr_owned(o); }
  if(!strcmp(nm,"string_replace_all")){ const char*src=S(vm,a,n,0), *from=S(vm,a,n,1), *to=S(vm,a,n,2);
    size_t fl=strlen(from), tl=strlen(to), sl=strlen(src);
    if(!fl) return vstr_owned(strdup(src));
    size_t hits=0; for(const char*p=src;(p=strstr(p,from));p+=fl) hits++;
    size_t outlen=sl + hits*(tl>fl ? tl-fl : 0);
    if(outlen>1024*1024) outlen=1024*1024;
    char *o=malloc(outlen+1); if(!o) return vstr_owned(strdup(""));
    size_t pos=0; const char*p=src;
    while(*p && pos<outlen){ const char*q=strstr(p,from);
      if(!q){ size_t ncopy=strlen(p); if(pos+ncopy>outlen) ncopy=outlen-pos; memcpy(o+pos,p,ncopy); pos+=ncopy; break; }
      size_t pre=(size_t)(q-p); if(pos+pre>outlen) pre=outlen-pos; memcpy(o+pos,p,pre); pos+=pre;
      size_t rep=tl; if(pos+rep>outlen) rep=outlen-pos; memcpy(o+pos,to,rep); pos+=rep; p=q+fl;
    }
    o[pos]=0; return vstr_owned(o); }

  return gml_builtin_try_io_ini(vm,nm,a,n);
}
static int varmap_delete_key(GmlVarMap *m, const char *key){
  if(!m||!m->slots||!key) return 0;
  int found=-1;
  for(int i=0;i<m->cap;i++) if(m->slots[i].key && !strcmp(m->slots[i].key,key)){ found=i; break; }
  if(found<0) return 0;
  GmlVarMap nm={0};
  for(int i=0;i<m->cap;i++){
    if(i==found || !m->slots[i].key) continue;
    *gml_varmap_put(&nm,m->slots[i].key)=m->slots[i].val;
  }
  free(m->slots);
  *m=nm;
  return 1;
}
GmlVal var_store_clone(GmlVal v){
  if(v.t==V_STR){
    char *c=v.s?strdup(v.s):NULL;
    return c?vstr(c):vstr("");
  }
  if(v.t==V_ARR){ gml_arr_mark_escaped(v); return v; }
  if(v.t==V_UNDEF) return vundef();
  return vreal(v.t==V_REAL?v.d:0.0);
}
static GmlVarMap *struct_public_map(GmlVM *vm, GmlVal ref, GmlInstance **owner){
  if(owner) *owner=NULL;
  if(!vm || ref.t!=V_REAL) return NULL;
  if((int)ref.d==IT_GLOBAL) return &vm->globals;
  if(!GML_IS_STRUCT_ID(ref.d)) return NULL;
  GmlInstance *st=gml_struct_find(vm,(unsigned)ref.d);
  if(owner) *owner=st;
  return st?&st->vars:NULL;
}
static int struct_internal_name(const char *name){
  /* Runtime-private struct fields include __ctor and __name just as bound methods use __fn and
   * __self. Public field enumeration must hide all four, matching the JSON encoder. */
  return name && (!strcmp(name,"__fn") || !strcmp(name,"__self") ||
                  !strcmp(name,"__ctor") || !strcmp(name,"__name"));
}
static int struct_public_name_count(GmlVarMap *map){
  if(!map) return -1;
  int count=0;
  for(int i=0;i<map->cap;i++)
    if(map->slots[i].key && !struct_internal_name(map->slots[i].key)) count++;
  return count;
}
static GmlVal struct_public_names(GmlVarMap *map){
  int count=struct_public_name_count(map);
  GmlVal names=gml_arr_new(count>0?count:0,vreal(0));
  if(count<=0) return names;
  int out=0;
  for(int i=0;i<map->cap;i++){
    const char *name=map->slots[i].key;
    if(name && !struct_internal_name(name)) gml_arr_set(names,out++,vstr(name));
  }
  return names;
}
static int gml_value_is_method(GmlVM *vm, GmlVal value){
  if(!vm || value.t!=V_REAL || !GML_IS_STRUCT_ID(value.d)) return 0;
  GmlInstance *st=gml_struct_find(vm,(unsigned)value.d);
  if(!st) return 0;
  GmlVal *fn=gml_varmap_get(&st->vars,"__fn");
  return st->method_bound || (fn && fn->t==V_REAL && GML_IS_FUNCVAL((int)fn->d));
}

typedef struct { GmlArr *source, *clone; } VariableCloneArray;
typedef struct { unsigned source, clone; } VariableCloneStruct;
typedef struct {
  GmlVM *vm;
  VariableCloneArray *array; int array_count, array_cap;
  VariableCloneStruct *structure; int structure_count, structure_cap;
} VariableCloneContext;

static GmlVal variable_clone_value(VariableCloneContext *context, GmlVal value, int depth);

static int variable_clone_note_array(VariableCloneContext *context, GmlArr *source, GmlArr *clone){
  if(context->array_count>=context->array_cap){
    int cap=context->array_cap?context->array_cap*2:16;
    VariableCloneArray *next=realloc(context->array,(size_t)cap*sizeof(*next));
    if(!next) return 0;
    context->array=next; context->array_cap=cap;
  }
  context->array[context->array_count++]=(VariableCloneArray){source,clone};
  return 1;
}

static int variable_clone_note_struct(VariableCloneContext *context, unsigned source, unsigned clone){
  if(context->structure_count>=context->structure_cap){
    int cap=context->structure_cap?context->structure_cap*2:16;
    VariableCloneStruct *next=realloc(context->structure,(size_t)cap*sizeof(*next));
    if(!next) return 0;
    context->structure=next; context->structure_cap=cap;
  }
  context->structure[context->structure_count++]=(VariableCloneStruct){source,clone};
  return 1;
}

static unsigned variable_clone_struct_lookup(VariableCloneContext *context, unsigned source){
  for(int i=0;i<context->structure_count;i++)
    if(context->structure[i].source==source) return context->structure[i].clone;
  return 0;
}

static GmlVal variable_clone_array(VariableCloneContext *context, GmlArr *source, int depth){
  if(!source) return vreal(0);
  for(int i=0;i<context->array_count;i++) if(context->array[i].source==source){
    GmlVal found=vreal(0); found.t=V_ARR; found.arr=context->array[i].clone; return found;
  }
  GmlArr *clone=calloc(1,sizeof(*clone));
  if(!clone) return vreal(0);
  if(!variable_clone_note_array(context,source,clone)){ free(clone); return vreal(0); }
  clone->escaped=1;
  clone->is_2d=source->is_2d; clone->height2d=source->height2d;
  clone->nested_2d=source->nested_2d;
  if(source->row_cap>0 && source->row_len){
    clone->row_len=malloc((size_t)source->row_cap*sizeof(*clone->row_len));
    if(clone->row_len){
      memcpy(clone->row_len,source->row_len,(size_t)source->row_cap*sizeof(*clone->row_len));
      clone->row_cap=source->row_cap;
    }
  }
  if(source->len>0 && source->data){
    clone->data=calloc((size_t)source->len,sizeof(*clone->data));
    if(clone->data){
      clone->len=clone->cap=source->len;
      for(int i=0;i<source->len;i++)
        clone->data[i]=depth>0
          ?variable_clone_value(context,source->data[i],depth-1)
          :gml_arr_store_clone(source->data[i]);
    }
  }
  GmlVal result=vreal(0); result.t=V_ARR; result.arr=clone; return result;
}

static GmlVal variable_clone_struct(VariableCloneContext *context, GmlInstance *source, int depth){
  unsigned existing=variable_clone_struct_lookup(context,source->id);
  if(existing) return vreal((double)existing);
  GmlInstance *clone=gml_struct_new(context->vm);
  if(!clone || !variable_clone_note_struct(context,source->id,clone->id))
    return vreal((double)source->id);
  for(int i=0;i<source->vars.cap;i++){
    GmlVarSlot *slot=&source->vars.slots[i];
    if(!slot->key) continue;
    GmlVal copy=depth>0
      ?variable_clone_value(context,slot->val,depth-1)
      :gml_arr_store_clone(slot->val);
    *gml_varmap_put(&clone->vars,slot->key)=copy;
  }
  /* A cloned bound method whose receiver is part of this clone graph follows the receiver clone.
   * External receivers remain bound to their original instance, matching variable_clone. */
  GmlVal *source_self=gml_varmap_get(&source->vars,"__self");
  GmlVal *clone_self=gml_varmap_get(&clone->vars,"__self");
  if(source_self && clone_self && source_self->t==V_REAL && GML_IS_STRUCT_ID(source_self->d)){
    unsigned rebound=variable_clone_struct_lookup(context,(unsigned)source_self->d);
    if(rebound) *clone_self=vreal((double)rebound);
  }
  clone->method_bound=0;
  return vreal((double)clone->id);
}

static GmlVal variable_clone_value(VariableCloneContext *context, GmlVal value, int depth){
  if(value.t==V_STR){
    char *copy=strdup(value.s?value.s:"");
    return copy?vstr_owned(copy):vstr("");
  }
  if(value.t==V_ARR) return variable_clone_array(context,(GmlArr*)value.arr,depth);
  if(value.t==V_REAL && GML_IS_STRUCT_ID(value.d)){
    GmlInstance *source=gml_struct_find(context->vm,(unsigned)value.d);
    if(source) return variable_clone_struct(context,source,depth);
  }
  return value.t==V_UNDEF?vundef():vreal(value.d);
}

static GmlVal gml_variable_clone(GmlVM *vm, GmlVal *args, int count){
  if(count<1) return vundef();
  int depth=count>1?(int)N(args,count,1):128;
  if(depth<0) depth=0;
  if(depth>128) depth=128;
  VariableCloneContext context={.vm=vm};
  GmlVal result=variable_clone_value(&context,args[0],depth);
  free(context.array); free(context.structure);
  return result;
}

GmlVal gml_builtin_try_values_variables(GmlVM *vm, const char *nm, GmlVal *a, int n){
  GmlRender *R=(GmlRender*)vm->render;
  (void)R;
  if(!strcmp(nm,"is_undefined")) return vreal(n>0 && a[0].t==V_UNDEF);
  if(!strcmp(nm,"is_string")) return vreal(n>0 && a[0].t==V_STR);
  if(!strcmp(nm,"is_real")||!strcmp(nm,"is_numeric")) return vreal(n>0 && a[0].t==V_REAL);
  if(!strcmp(nm,"is_array")) return vreal(n>0 && a[0].t==V_ARR);
  /* Only a real can be either: a string or an undefined is neither NaN nor infinite, and
   * answering from a coerced number would call is_nan("") true. */
  if(!strcmp(nm,"is_nan")) return vreal(n>0 && a[0].t==V_REAL && isnan(a[0].d));
  if(!strcmp(nm,"is_infinity")) return vreal(n>0 && a[0].t==V_REAL && isinf(a[0].d));
  /* typeof() exposes the language-visible value category, not the C storage used by this VM.
   * Structs and bound methods deliberately share V_REAL with ordinary numbers because their
   * public handles must survive bytecode arithmetic/copies; recover their logical type from the
   * reserved struct-id range.  A raw compiler function value remains "number", matching the
   * documented script-index behaviour, while method(scope, function) is a method. */
  if(!strcmp(nm,"typeof")){
    if(n<1 || a[0].t==V_UNDEF) return vstr("undefined");
    if(a[0].t==V_STR) return vstr("string");
    if(a[0].t==V_ARR) return vstr("array");
    if(a[0].t==V_REAL && GML_IS_STRUCT_ID(a[0].d)){
      GmlInstance *st=gml_struct_find(vm,(unsigned)a[0].d);
      if(st){
        if(gml_value_is_method(vm,a[0])) return vstr("method");
        return vstr("struct");
      }
    }
    if(a[0].t==V_REAL && a[0].d>=100000.0){
      unsigned id=(unsigned)a[0].d;
      for(int i=0;i<vm->inst_count;i++)
        if(vm->inst[i].active && !vm->inst[i].marked && vm->inst[i].id==id)
          return vstr("ref");
    }
    return vstr(a[0].t==V_REAL?"number":"unknown");
  }
  if(!strcmp(nm,"is_struct")){
    return vreal(n>0 && a[0].t==V_REAL && GML_IS_STRUCT_ID(a[0].d) &&
                 gml_struct_find(vm,(unsigned)a[0].d)!=NULL);
  }
  if(!strcmp(nm,"is_method")) return vreal(n>0 && gml_value_is_method(vm,a[0]));
  if(!strcmp(nm,"method_get_index")||!strcmp(nm,"method_get_self")){
    int want_index = nm[11]=='i';
    if(n>0 && a[0].t==V_REAL && GML_IS_STRUCT_ID(a[0].d)){
      GmlInstance *st=gml_struct_find(vm,(unsigned)a[0].d);
      if(st){
        GmlVal *p=gml_varmap_get(&st->vars,want_index?"__fn":"__self");
        if(p) return *p;
      }
    }
    /* A bare function used as a method index answers with its own function value. */
    if(want_index && n>0 && a[0].t==V_REAL && GML_IS_FUNCVAL((int)a[0].d)) return a[0];
    return vundef();
  }
  if(!strcmp(nm,"is_callable")){
    int raw=n>0 && a[0].t==V_REAL && GML_IS_FUNCVAL((int)a[0].d);
    return vreal(raw || (n>0 && gml_value_is_method(vm,a[0])));
  }
  if(!strcmp(nm,"is_bool")) return vreal(n>0 && a[0].t==V_REAL && (a[0].d==0||a[0].d==1));
  /* In GM6/7/8 "local" means a field on the current instance, not a temporary VM stack
   * local. Classic projects use this to initialise a field only once from a Step event. */
  if(!strcmp(nm,"variable_local_exists"))
    return vreal(n>0 && gml_inst_var_exists(vm,vreal(IT_SELF),S(vm,a,n,0)));
  if(!strcmp(nm,"variable_local_get")){
    int ok=0; GmlVal out=n>0?gml_inst_var_get_val(vm,vreal(IT_SELF),S(vm,a,n,0),&ok):vundef();
    return ok?out:vreal(0);
  }
  if(!strcmp(nm,"variable_local_set")){
    if(n>1) gml_inst_var_set_val(vm,vreal(IT_SELF),S(vm,a,n,0),var_store_clone(a[1]));
    return vreal(0);
  }
  if(!strcmp(nm,"variable_clone")) return gml_variable_clone(vm,a,n);
  if(!strcmp(nm,"variable_global_exists")) return vreal(gml_varmap_get(&vm->globals,S(vm,a,n,0))!=NULL);
  if(!strcmp(nm,"variable_global_get")){
    GmlVal *p=gml_varmap_get(&vm->globals,S(vm,a,n,0));
    if(!p) return vundef();
    GmlVal out=*p; if(out.t==V_STR) out.d=0;
    return out;
  }
  if(!strcmp(nm,"variable_global_set")){
    if(n>1){
      const char *key=S(vm,a,n,0);
      GmlVal *p=gml_varmap_get(&vm->globals,key);
      if(p) *p=var_store_clone(a[1]);
      else {
        char *owned=strdup(key?key:"");
        if(owned) *gml_varmap_put(&vm->globals,owned)=var_store_clone(a[1]);
      }
    }
    return vreal(0);
  }
  if(!strcmp(nm,"variable_instance_exists")||!strcmp(nm,"variable_instance_get")||
     !strcmp(nm,"variable_instance_set")){
    const char *key=S(vm,a,n,1);
    if(!strcmp(nm,"variable_instance_exists")) return vreal(n>1 && gml_inst_var_exists(vm,a[0],key));
    if(!strcmp(nm,"variable_instance_get")){
      int ok=0; GmlVal out=(n>1)?gml_inst_var_get_val(vm,a[0],key,&ok):vundef();
      return ok?out:vundef();
    }
    if(n>2) gml_inst_var_set_val(vm,a[0],key,var_store_clone(a[2]));
    return vreal(0);
  }
  /* Instance counterparts of the struct-name queries below. Both use the same
   * public-name filter so the returned array and count describe the same map. */
  if(!strcmp(nm,"variable_instance_get_names")){
    GmlVarMap *map=n>0?gml_inst_varmap(vm,a[0]):NULL;
    return struct_public_names(map);
  }
  if(!strcmp(nm,"variable_instance_names_count")){
    GmlVarMap *map=n>0?gml_inst_varmap(vm,a[0]):NULL;
    return vreal(struct_public_name_count(map));
  }
  if(!strcmp(nm,"variable_struct_get_names")||!strcmp(nm,"struct_get_names")){
    GmlVarMap *map=n>0?struct_public_map(vm,a[0],NULL):NULL;
    return struct_public_names(map);
  }
  if(!strcmp(nm,"variable_struct_names_count")||!strcmp(nm,"struct_names_count")){
    GmlVarMap *map=n>0?struct_public_map(vm,a[0],NULL):NULL;
    return vreal(struct_public_name_count(map));
  }
  /* The wrapper retains a strong ref for now; liveness checks still resolve
   * the referenced struct rather than reporting it alive unconditionally. */
  if(!strcmp(nm,"weak_ref_create")){
    GmlInstance *w=gml_struct_new(vm);
    if(!w) return n>0?a[0]:vundef();
    *gml_varmap_put(&w->vars,"ref")=gml_arr_store_clone(n>0?a[0]:vundef());
    return vreal((double)w->id);
  }
  if(!strcmp(nm,"weak_ref_alive")){
    GmlVarMap *wm=n>0?struct_public_map(vm,a[0],NULL):NULL;
    GmlVal *p=wm?gml_varmap_get(wm,"ref"):NULL;
    if(!p || p->t==V_UNDEF) return vreal(0);
    return vreal(struct_public_map(vm,*p,NULL)!=NULL);
  }
  if(!strcmp(nm,"weak_ref_any_alive")){
    if(n<1 || a[0].t!=V_ARR || !a[0].arr) return vreal(0);
    GmlArr *A=(GmlArr*)a[0].arr;
    int from=(n>1)?(int)N(a,n,1):0, count=(n>2)?(int)N(a,n,2):A->len-from;
    if(from<0) from=0;
    if(count>A->len-from) count=A->len-from;
    for(int i=0;i<count;i++){
      GmlVarMap *wm=struct_public_map(vm,A->data[from+i],NULL);
      GmlVal *p=wm?gml_varmap_get(wm,"ref"):NULL;
      if(p && p->t!=V_UNDEF && struct_public_map(vm,*p,NULL)) return vreal(1);
    }
    return vreal(0);
  }
  if(!strcmp(nm,"variable_struct_exists")||!strcmp(nm,"struct_exists")||
     !strcmp(nm,"variable_struct_get")||!strcmp(nm,"struct_get")||
     !strcmp(nm,"variable_struct_set")||!strcmp(nm,"struct_set")||
     !strcmp(nm,"variable_struct_remove")||!strcmp(nm,"struct_remove")){
    GmlInstance *st=NULL;
    GmlVarMap *map=n>0?struct_public_map(vm,a[0],&st):NULL;
    const char *key=S(vm,a,n,1);
    int get=!strcmp(nm,"variable_struct_get")||!strcmp(nm,"struct_get");
    int exists=!strcmp(nm,"variable_struct_exists")||!strcmp(nm,"struct_exists");
    int set=!strcmp(nm,"variable_struct_set")||!strcmp(nm,"struct_set");
    if(!map) return get?vundef():vreal(0);
    if(exists) return vreal(gml_varmap_get(map,key)!=NULL);
    if(get){
      GmlVal *p=gml_varmap_get(map,key);
      if(!p) return vundef();
      GmlVal out=*p; if(out.t==V_STR) out.d=0;
      return out;
    }
    if(set){
      if(n>2){
        if(st && key && (!strcmp(key,"__fn") || !strcmp(key,"__self"))) st->method_bound=0;
        GmlVal *p=gml_varmap_get(map,key);
        if(p) *p=var_store_clone(a[2]);
        else {
          char *owned=strdup(key?key:"");
          if(owned) *gml_varmap_put(map,owned)=var_store_clone(a[2]);
        }
      }
      return vreal(0);
    }
    if(st && key && (!strcmp(key,"__fn") || !strcmp(key,"__self"))) st->method_bound=0;
    return vreal(varmap_delete_key(map,key));
  }
  if(!strcmp(nm,"struct_get_from_hash")){
    GmlInstance *st=(n>0 && a[0].t==V_REAL && GML_IS_STRUCT_ID(a[0].d))?gml_struct_find(vm,(unsigned)a[0].d):NULL;
    if(!st || st->vars.cap<=0) return vundef();
    double hv=N(a,n,1);
    uint32_t want = hv < 0 ? (uint32_t)(int32_t)hv : (uint32_t)hv;
    for(int i=0;i<st->vars.cap;i++){
      GmlVarSlot *slot=&st->vars.slots[i];
      if(slot->key && slot->hash==want){
        GmlVal out=slot->val; if(out.t==V_STR) out.d=0;
        return out;
      }
    }
    return vundef();
  }
  return gml_builtin_try_draw_3d(vm,nm,a,n);
}
GmlVal gml_builtin_try_values_language(GmlVM *vm, const char *nm, GmlVal *a, int n){
  /* ---- GMS2.3 internal function/struct machinery (partial stubs — enough for common init paths) ---- */
  /* The bytecode compiler uses this conversion before a StackTop member read. Its argument is
   * already the concrete receiver reference; replacing it with a generic zero disconnects the
   * following method lookup from the live instance. */
  if(!strcmp(nm,"@@GetInstance@@")) return n>0?a[0]:vreal(0);
  if(!strcmp(nm,"@@This@@"))   return vreal(vm->cur_self ? (double)vm->cur_self->id : -1);
  if(!strcmp(nm,"@@Other@@"))  return vreal(vm->cur_other? (double)vm->cur_other->id : -2);
  if(!strcmp(nm,"@@Global@@")) return vreal(-5);      /* global scope sentinel */
  if(!strcmp(nm,"@@NullObject@@")) return vreal(-4);  /* noone */
  if(!strcmp(nm,"@@NewGMLArray@@")){ GmlArr *A=calloc(1,sizeof(*A)); if(!A) return vreal(0);
    /* Copy stored strings and mark nested arrays escaped, as for other array stores. */
    if(n>0){ A->data=calloc(n,sizeof(GmlVal)); if(A->data){ A->len=A->cap=n; for(int i=0;i<n;i++) A->data[i]=gml_arr_store_clone(a[i]); } }
    GmlVal v=vreal(0); v.t=V_ARR; v.arr=A; return v; }
  /* GMS2.3 method(scope, func): a BOUND method — a struct carrying the function and the self it runs
   * with. OP_CALLV dispatches it with self=__self. (Returning the bare func lost the scope, so any
   * bound input-check/handler that reads its own struct fields ran on the wrong self and read 0.) */
  if(!strcmp(nm,"method")){
    if(n<2) return n>0?a[0]:vreal(0);
    int fv=(int)N(a,n,1);
    /* only wrap real function values; method(self, non-func) is a no-op passthrough */
    if(!GML_IS_FUNCVAL(fv) && !GML_IS_STRUCT_ID(N(a,n,1))) return a[1];
    GmlInstance *bm=gml_struct_new(vm); if(!bm) return a[1];
    /* Resolve the self NOW (bind time). GMS2.3 method() binds to the CURRENT self; keeping the scope
     * sentinel (-1 self / -2 other) would re-resolve it against whoever runs the method LATER — so a
     * binding built in a constructor would read the caller's fields at dispatch, not its own. */
    GmlVal selfv=a[0];
    if(selfv.t==V_REAL && (selfv.d==-1.0 || selfv.d==-2.0)){
      /* Ordinary self/other bindings capture a concrete receiver now. Constructor-static methods
       * retain their -16 marker: one shared function is rebound to each struct on dot invocation. */
      GmlInstance *si = selfv.d==-2.0 ? vm->cur_other : vm->cur_self;
      if(si) selfv=vreal((double)si->id);
    }
    *gml_varmap_put(&bm->vars,"__fn")=gml_arr_store_clone(a[1]);
    *gml_varmap_put(&bm->vars,"__self")=gml_arr_store_clone(selfv);
    if(GML_IS_FUNCVAL(fv)){
      bm->method_bound=1;
      bm->method_fci=fv & 0x00FFFFFF;
      bm->method_self=selfv;
    }
    if(builtin_setting(vm,"GML_DBG_STRUCT")){ 
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,"[struct] f%ld bind id=%u scope=%.0f fn=%d ci=%d name=%s\n",
        vm->frame,bm->id,N(a,n,0),fv,bm->method_bound?bm->method_fci:-1,
        (bm->method_bound && vm->win && bm->method_fci>=0 && bm->method_fci<vm->win->n_code)
          ? vm->win->code[bm->method_fci].name : "?"); }
    return vreal((double)bm->id);
  }
  /* GMS2.3 struct construction: @@NewGMLObject@@(constructor_func [, ctor_args...]). Allocate a struct
   * and run the constructor with self=the new struct so it populates its fields; return the struct. */
  if(!strcmp(nm,"@@NewGMLObject@@")){
    GmlInstance *st=gml_struct_new(vm); if(!st) return vreal(0);
    int fci=-1;
    if(n>=1){ double a0=N(a,n,0);
      /* The constructor arrives either as a raw funcval (a named `new Foo()`) OR as a bound
       * method struct (an INLINE constructor defined via method(self,ctor) — GMS2.3 emits this
       * for anonymous struct classes). Unwrap the bound method's __fn so its body actually runs;
       * otherwise the struct is created but never initialised (empty fields). */
      if(GML_IS_FUNCVAL((int)a0)) fci=(int)a0 & 0x00FFFFFF;
      else if(GML_IS_STRUCT_ID(a0)){ GmlInstance *bm=gml_struct_find(vm,(unsigned)a0);
        if(bm){ GmlVal *pf=gml_varmap_get(&bm->vars,"__fn"); if(pf && pf->t==V_REAL){ int f=(int)pf->d; if(GML_IS_FUNCVAL(f)) fci=f & 0x00FFFFFF; } } }
      if(fci>=0){
        *gml_varmap_put(&st->vars,"__ctor")=vreal((double)fci);
        const char *cn=(vm->win&&fci<vm->win->n_code&&vm->win->code[fci].name)?vm->win->code[fci].name:"";
        /* Global-scope constructors use the gml_GlobalScript_ prefix. Strip either compiler
         * prefix before exposing the constructor name. */
        if(!strncmp(cn,"gml_GlobalScript_",17)) cn+=17;
        else if(!strncmp(cn,"gml_Script_",11)) cn+=11;
        *gml_varmap_put(&st->vars,"__name")=vstr_owned(strdup(cn));
        gml_vm_run_code(vm,fci,st,vm->cur_self,(n>1)?a+1:0,n-1);
      } }
    if(builtin_setting(vm,"GML_DBG_STRUCT")){ 
      /* Report the stored constructor name alongside the raw code entry so diagnostics reflect
       * the value returned by `instanceof`. */
      GmlVal *stored=gml_varmap_get(&st->vars,"__name");
      anygm_host_logf(vm ? vm->host : NULL,ANYGM_LOG_DEBUG,
        "[struct] f%ld new id=%u argc=%d ctor=%d entry=%s __name=%s fields=%d\n",
        vm->frame,st->id,n,fci,
        (fci>=0 && vm->win && fci<vm->win->n_code)?vm->win->code[fci].name:"?",
        (stored && stored->t==V_STR && stored->s)?stored->s:"(none)",st->vars.len); }
    return vreal((double)st->id);
  }
  /* `instanceof` exposes direct constructor identity. Inheritance-aware queries require a
   * distinct runtime contract. */
  if(!strcmp(nm,"instanceof")){
    GmlInstance *st=(n>0 && a[0].t==V_REAL && GML_IS_STRUCT_ID(a[0].d))?gml_struct_find(vm,(unsigned)a[0].d):NULL;
    GmlVal *pc=st?gml_varmap_get(&st->vars,"__ctor"):NULL;
    int have=(pc && pc->t==V_REAL);
    if(n<2){
      if(!st) return vstr("");
      GmlVal *pn=gml_varmap_get(&st->vars,"__name");
      if(pn && pn->t==V_STR && pn->s) return vstr(pn->s);
      int ci=have?(int)pc->d:-1;
      return vstr((vm->win&&ci>=0&&ci<vm->win->n_code&&vm->win->code[ci].name)?vm->win->code[ci].name:"");
    }
    int want=-1;
    if(a[1].t==V_REAL){
      int iv=(int)a[1].d;
      if(GML_IS_FUNCVAL(iv)) want=iv & 0x00FFFFFF;
      else if(GML_IS_STRUCT_ID(a[1].d)){ GmlInstance *bm=gml_struct_find(vm,(unsigned)a[1].d);
        GmlVal *pf=bm?gml_varmap_get(&bm->vars,"__fn"):NULL;
        if(pf && pf->t==V_REAL){ int f=(int)pf->d; if(GML_IS_FUNCVAL(f)) want=f & 0x00FFFFFF; }
      }
    }
    return vreal(have && want>=0 && (int)pc->d==want);
  }
  if(!strcmp(nm,"@@SetStatic@@")||!strcmp(nm,"@@GetStatic@@")||!strcmp(nm,"@@CopyStatic@@")||
     !strcmp(nm,"@@try_hook@@")||!strcmp(nm,"@@try_unhook@@")||
     !strcmp(nm,"@@finish_catch@@")||!strcmp(nm,"@@finally@@")||
     !strcmp(nm,"@@throw@@")) return vreal(0);
  return gml_builtin_try_instances_destroy(vm,nm,a,n);
}
/* Values math/arrays implementation. */
