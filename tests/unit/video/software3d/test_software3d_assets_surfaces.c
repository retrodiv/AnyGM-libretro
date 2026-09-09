/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "software3d_test_fixture.h"

int surface_tiled_fixture(void){
  GmlRender render={0}; GmlVM vm={0};
  uint32_t pixels[48],expected[48];
  const uint32_t pattern[4]={0xFFFF0000u,0xFF00FF00u,0xFF0000FFu,0xFFFFFFFFu};
  render.color=0xFFFFFFu; render.alpha=1; render.alphablend=1;
  render.color_write_mask=15; render.blend_equation=render.blend_equation_alpha=1;
  render.next_surface_id=1; vm.render=&render;
  int source=gml_surface_create(&render,2,2),passed=0;
#define TILED_REQUIRE(condition,label) do { if(!(condition)){ \
  fprintf(stderr,"surface tiling: %s\n",label); goto done; } } while(0)
  TILED_REQUIRE(source>0,"source allocation");
  memcpy(render.surface[source-1].px,pattern,sizeof pattern);
  render.surface[source-1].opaque_known=render.surface[source-1].all_opaque=1;
  render.surface[source-1].all_transparent=0;
  for(int scenario=0;scenario<6;scenario++){
    double xs=scenario==1?2:scenario==2?-1:scenario==3?.75:1;
    double ys=scenario==2?-2:1;
    double alpha=scenario==0?1:.5;
    uint32_t tint=scenario==1?0x00FFFFu:0xFFFFFFu;
    double cx=scenario==0?0:3,cy=scenario==0?0:5;
    for(int reference=0;reference<2;reference++){
      uint32_t *target=reference?expected:pixels;
      memset(target,0,sizeof pixels);
      gml_render_begin(&render,target,8,6,cx,cy);
      if(scenario==4) gml_render_gui_begin(&render,4,3);
      if(scenario==5) gml_render_world_set_logical_extent(&render,4,3);
      if(reference){
        for(int y=-12;y<=12;y++) for(int x=-12;x<=12;x++)
          gml_draw_surface_part_ext(&render,source,0,0,2,2,
            1+x*2*fabs(xs),-1+y*2*fabs(ys),xs,ys,tint,alpha);
      } else {
        GmlVal args[]={vreal(source),vreal(1),vreal(-1),vreal(xs),vreal(ys),vreal(tint),vreal(alpha)};
        if(scenario==0) (void)call_values(&vm,"draw_surface_tiled",args,3);
        else (void)call_values(&vm,"draw_surface_tiled_ext",args,7);
      }
      gml_render_gui_end(&render);
    }
    if(memcmp(pixels,expected,sizeof pixels)){
      fprintf(stderr,"surface tiling scenario %d differs from explicit repeated cells\n",scenario);
      goto done;
    }
    TILED_REQUIRE(colored_pixels(pixels,48)==48,"every target pixel covered");
    if(scenario==0) TILED_REQUIRE(pixels[0]==pattern[3] && pixels[1]==pattern[2],
      "hand-derived phase at the top-left");
  }
  gml_render_begin(&render,pixels,8,6,0,0);
  TILED_REQUIRE(gml_surface_set_target(&render,source),"bind source as target");
  GmlVal self_args[]={vreal(source),vreal(1),vreal(0),vreal(1),vreal(1),vreal(0),vreal(1)};
  (void)call_values(&vm,"draw_surface_tiled_ext",self_args,7);
  TILED_REQUIRE(!memcmp(render.surface[source-1].px,pattern,sizeof pattern),"self-draw is rejected");
  gml_surface_reset_target(&render);
  memcpy(expected,pixels,sizeof pixels);
  const double invalid[]={0,NAN,INFINITY,1e-200};
  for(size_t i=0;i<sizeof invalid/sizeof invalid[0];i++){
    GmlVal args[]={vreal(source),vreal(0),vreal(0),vreal(invalid[i]),vreal(1),vreal(0xFFFFFF),vreal(1)};
    (void)call_values(&vm,"draw_surface_tiled_ext",args,7);
    TILED_REQUIRE(!memcmp(pixels,expected,sizeof pixels),"invalid or zero-raster extent draws nothing");
  }
  passed=1;
done:
  gml_render_free(&render);
  vm.render=NULL;
  gml_vm_free(&vm);
  return passed;
#undef TILED_REQUIRE
}


int software3d_case_assets(Software3dRasterFixture *fixture){
  const double perspective[]={
    32,14,5,32,24,0,0,0,1
  };
  int first_row=-1,last_row=-1,empty_inside=0;
  fixture->surface=gml_surface_create(&fixture->render,2,2);
  if(fixture->surface<=0){ fprintf(stderr,"software D3 texture surface create mismatch\n"); return 0; }
  GmlSurface *surface_data=&fixture->render.surface[fixture->surface-1];
  surface_data->px[0]=0xFFFF0000u; surface_data->px[1]=0xFF00FF00u;
  surface_data->px[2]=0xFF0000FFu; surface_data->px[3]=0xFFFFFFFFu;
  {
    /* A structurally recognized dual-sample post-process must apply the integer texture offset
     * and independent channel gains even while portable D3 state exists. */
    fixture->render.shader_pal=calloc(1,sizeof(*fixture->render.shader_pal)); fixture->render.n_shader_pal=1;
    if(!fixture->render.shader_pal){ fprintf(stderr,"dual-sample shader fixture allocation failed\n"); return 0; }
    struct GmlShaderPal *shader=&fixture->render.shader_pal[0];
    shader->dual_sample=1; shader->dual_axis=0; shader->dual_sign=-1;
    shader->dual_value[0]=0.5f; shader->dual_value[1]=1.0f; /* 2-wide texture -> one texel left */
    shader->dual_base_gain[0]=0.5f; shader->dual_base_gain[1]=1.0f;
    shader->dual_base_gain[2]=0.0f; shader->dual_base_gain[3]=1.0f;
    shader->dual_shift_gain[0]=0.5f; shader->dual_shift_gain[1]=0.0f;
    shader->dual_shift_gain[2]=1.0f; shader->dual_shift_gain[3]=1.0f;
    surface_data->opaque_known=surface_data->all_opaque=1;
    gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0); fixture->render.active_shader=0;
    gml_draw_surface_stretched(&fixture->render,fixture->surface,20,20,2,2,0xFFFFFFu,1.0);
    fixture->render.active_shader=-1;
    if((fixture->pixels[20*SOFTWARE3D_WIDTH+20]&0xFFFFFFu)!=0xFF0000u ||
       (fixture->pixels[20*SOFTWARE3D_WIDTH+21]&0xFFFFFFu)!=0x80FF00u ||
       (fixture->pixels[21*SOFTWARE3D_WIDTH+20]&0xFFFFFFu)!=0x0000FFu ||
       (fixture->pixels[21*SOFTWARE3D_WIDTH+21]&0xFFFFFFu)!=0x80FFFFu){
      fprintf(stderr,"dual-sample shader raster mismatch: %06x %06x %06x %06x\n",
        fixture->pixels[20*SOFTWARE3D_WIDTH+20]&0xFFFFFFu,fixture->pixels[20*SOFTWARE3D_WIDTH+21]&0xFFFFFFu,
        fixture->pixels[21*SOFTWARE3D_WIDTH+20]&0xFFFFFFu,fixture->pixels[21*SOFTWARE3D_WIDTH+21]&0xFFFFFFu);
      return 0;
    }
  }
  GmlVal surface_arg=vreal(fixture->surface);
  GmlVal surface_texture=call_values(&fixture->vm,"surface_get_texture",&surface_arg,1);
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection",perspective,9);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_disable,1);
  double textured_floor[]={0,0,0,64,48,0,surface_texture.d,5,5};
  call_numbers(&fixture->vm,"d3d_draw_floor",textured_floor,9);
  first_row=last_row=-1; empty_inside=0;
  for(int y=0;y<SOFTWARE3D_HEIGHT;y++){
    int row=colored_pixels(fixture->pixels+y*SOFTWARE3D_WIDTH,SOFTWARE3D_WIDTH);
    if(row){ if(first_row<0) first_row=y; last_row=y; }
  }
  if(first_row>=0) for(int y=first_row;y<=last_row;y++)
    if(colored_pixels(fixture->pixels+y*SOFTWARE3D_WIDTH,SOFTWARE3D_WIDTH)==0) empty_inside++;
  if(first_row<0 || empty_inside){
    fprintf(stderr,"software D3 textured floor has %d empty interior rows (%d..%d)\n",
            empty_inside,first_row,last_row);
    return 0;
  }
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection_ortho",software3d_ortho,5);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_disable,1);
  double textured_begin[2]={4,surface_texture.d};
  const double textured_a[]={8,8,0,0,0};
  const double textured_b[]={56,8,0,1,0};
  const double textured_c[]={56,40,0,1,1};
  const double textured_d[]={8,40,0,0,1};
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  call_numbers(&fixture->vm,"d3d_primitive_begin_texture",textured_begin,2);
  call_numbers(&fixture->vm,"d3d_vertex_texture",textured_a,5);
  call_numbers(&fixture->vm,"d3d_vertex_texture",textured_b,5);
  call_numbers(&fixture->vm,"d3d_vertex_texture",textured_c,5);
  call_numbers(&fixture->vm,"d3d_vertex_texture",textured_a,5);
  call_numbers(&fixture->vm,"d3d_vertex_texture",textured_c,5);
  call_numbers(&fixture->vm,"d3d_vertex_texture",textured_d,5);
  call_numbers(&fixture->vm,"d3d_primitive_end",NULL,0);
  if((fixture->pixels[14*SOFTWARE3D_WIDTH+16]&0x00FFFFFFu)!=0xFF0000u ||
     (fixture->pixels[14*SOFTWARE3D_WIDTH+48]&0x00FFFFFFu)!=0x00FF00u ||
     (fixture->pixels[34*SOFTWARE3D_WIDTH+16]&0x00FFFFFFu)!=0x0000FFu){
    fprintf(stderr,"software D3 surface texture mismatch: %06x %06x %06x\n",
      fixture->pixels[14*SOFTWARE3D_WIDTH+16]&0xFFFFFFu,fixture->pixels[14*SOFTWARE3D_WIDTH+48]&0xFFFFFFu,fixture->pixels[34*SOFTWARE3D_WIDTH+16]&0xFFFFFFu);
    return 0;
  }
  surface_data->px[0]=0xFF020000u;
  surface_data->px[1]=surface_data->px[2]=surface_data->px[3]=0xFF000000u;
  call_numbers(&fixture->vm,"texture_set_interpolation",software3d_enable,1);
  const double interpolated_point[]={30,20,0,.5,.5,0x0000FE,1};
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  double interpolated_begin[2]={1,surface_texture.d};
  call_numbers(&fixture->vm,"d3d_primitive_begin_texture",interpolated_begin,2);
  call_numbers(&fixture->vm,"d3d_vertex_texture_color",interpolated_point,7);
  call_numbers(&fixture->vm,"d3d_primitive_end",NULL,0);
  if((fixture->pixels[20*SOFTWARE3D_WIDTH+30]&0x00FFFFFFu)!=0){
    fprintf(stderr,"software D3 bilinear modulation quantized early: %06x\n",
      fixture->pixels[20*SOFTWARE3D_WIDTH+30]&0x00FFFFFFu);
    return 0;
  }
  {
    /* Classic fixed-function filtering uses rounded eight-bit fractions and two rounded lerps.
     * The blue component also proves an implicit white vertex colour is not reduced to 254. */
    GmlWin classic_win={0}; classic_win.classic_version=800; fixture->vm.win=&classic_win;
    surface_data->px[0]=0xFF0A141Eu; surface_data->px[1]=0xFF6E7882u;
    surface_data->px[2]=0xFFD2DCE6u; surface_data->px[3]=0xFFFAF0C8u;
    fixture->render.color=0xFFFFFFu;
    const double classic_filtered_point[]={30,20,0,.72,.28};
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    call_numbers(&fixture->vm,"d3d_primitive_begin_texture",interpolated_begin,2);
    call_numbers(&fixture->vm,"d3d_vertex_texture",classic_filtered_point,5);
    call_numbers(&fixture->vm,"d3d_primitive_end",NULL,0);
    fixture->vm.win=NULL;
    if((fixture->pixels[21*SOFTWARE3D_WIDTH+31]&0x00FFFFFFu)!=0x707981u){
      int first=-1;
      for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) if(fixture->pixels[i]&0x00FFFFFFu){ first=i; break; }
      fprintf(stderr,"software classic fixed-function filter mismatch: %06x first=(%d,%d) %06x\n",
        fixture->pixels[21*SOFTWARE3D_WIDTH+31]&0x00FFFFFFu,first<0?-1:first%SOFTWARE3D_WIDTH,first<0?-1:first/SOFTWARE3D_WIDTH,
        first<0?0:fixture->pixels[first]&0x00FFFFFFu);
      return 0;
    }
  }
  call_numbers(&fixture->vm,"texture_set_interpolation",software3d_disable,1);
  {
    surface_data->px[0]=0x00112233u;
    surface_data->px[1]=0x80445566u;
    surface_data->px[2]=0xFF778899u;
    surface_data->px[3]=0x20AABBCCu;
    int alpha_sprite=gml_sprite_create_from_surface(
      &fixture->render,fixture->surface,0,0,2,2,0,0,0,0);
    const uint8_t *rgba=alpha_sprite>=0 && alpha_sprite<fixture->render.n_spr
      ? fixture->render.spr[alpha_sprite].runtime_rgba : NULL;
    if(!rgba || rgba[0]!=0x11 || rgba[1]!=0x22 || rgba[2]!=0x33 || rgba[3]!=0x00 ||
       rgba[4]!=0x44 || rgba[5]!=0x55 || rgba[6]!=0x66 || rgba[7]!=0x80 ||
       rgba[8]!=0x77 || rgba[9]!=0x88 || rgba[10]!=0x99 || rgba[11]!=0xFF ||
       rgba[12]!=0xAA || rgba[13]!=0xBB || rgba[14]!=0xCC || rgba[15]!=0x20){
      fprintf(stderr,"software surface sprite alpha transfer mismatch\n");
      return 0;
    }
    gml_sprite_delete(&fixture->render,alpha_sprite);
  }
  surface_data->px[0]=0xFFFF0000u; surface_data->px[1]=0xFF00FF00u;
  surface_data->px[2]=0xFF0000FFu; surface_data->px[3]=0xFFFFFFFFu;
  fixture->runtime_sprite=gml_sprite_create_from_surface(&fixture->render,fixture->surface,0,0,2,2,0,0,0,0);
  fixture->runtime_sprite_2=gml_sprite_create_from_surface(&fixture->render,fixture->surface,0,0,2,2,0,0,0,0);
  GmlVal runtime_name_arg_1=vreal(fixture->runtime_sprite), runtime_name_arg_2=vreal(fixture->runtime_sprite_2);
  GmlVal runtime_name_1=call_values(&fixture->vm,"sprite_get_name",&runtime_name_arg_1,1);
  GmlVal runtime_name_2=call_values(&fixture->vm,"sprite_get_name",&runtime_name_arg_2,1);
  if(fixture->runtime_sprite<0 || fixture->runtime_sprite_2<0 || fixture->runtime_sprite==fixture->runtime_sprite_2 ||
     runtime_name_1.t!=V_STR || runtime_name_2.t!=V_STR || !runtime_name_1.s || !runtime_name_2.s ||
     !runtime_name_1.s[0] || !runtime_name_2.s[0] || !strcmp(runtime_name_1.s,runtime_name_2.s)){
    fprintf(stderr,"software runtime sprite names are not unique\n");
    return 0;
  }
  GmlVal runtime_lookup=call_values(&fixture->vm,"asset_get_index",&runtime_name_1,1);
  if((int)runtime_lookup.d!=fixture->runtime_sprite){
    fprintf(stderr,"software runtime sprite name lookup mismatch: %d != %d\n",
      (int)runtime_lookup.d,fixture->runtime_sprite);
    return 0;
  }
  GmlVal sprite_args[2]={vreal(fixture->runtime_sprite),vreal(0)};
  GmlVal sprite_texture=call_values(&fixture->vm,"sprite_get_texture",sprite_args,2);
  double sprite_begin[2]={1,sprite_texture.d};
  const double sprite_point[]={30,20,0,.25,.25};
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  call_numbers(&fixture->vm,"d3d_primitive_begin_texture",sprite_begin,2);
  call_numbers(&fixture->vm,"d3d_vertex_texture",sprite_point,5);
  call_numbers(&fixture->vm,"d3d_primitive_end",NULL,0);
  if((fixture->pixels[20*SOFTWARE3D_WIDTH+30]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 runtime sprite texture mismatch\n");
    return 0;
  }
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  GmlVal positioned[11]={vreal(fixture->runtime_sprite),vreal(0),
    vreal(10),vreal(10),vreal(18),vreal(10),vreal(20),vreal(18),vreal(8),vreal(18),vreal(1)};
  (void)call_values(&fixture->vm,"draw_sprite_pos",positioned,11);
  if((fixture->pixels[10*SOFTWARE3D_WIDTH+10]&0x00FFFFFFu)!=0xFF0000u ||
     (fixture->pixels[10*SOFTWARE3D_WIDTH+17]&0x00FFFFFFu)!=0x00FF00u ||
     (fixture->pixels[17*SOFTWARE3D_WIDTH+9]&0x00FFFFFFu)!=0x0000FFu ||
     (fixture->pixels[17*SOFTWARE3D_WIDTH+18]&0x00FFFFFFu)!=0xFFFFFFu || fixture->pixels[5*SOFTWARE3D_WIDTH+5]!=0){
    fprintf(stderr,"software positioned sprite quad mismatch: %06x %06x %06x %06x\n",
      fixture->pixels[10*SOFTWARE3D_WIDTH+10]&0xFFFFFFu,fixture->pixels[10*SOFTWARE3D_WIDTH+17]&0xFFFFFFu,
      fixture->pixels[17*SOFTWARE3D_WIDTH+9]&0xFFFFFFu,fixture->pixels[17*SOFTWARE3D_WIDTH+18]&0xFFFFFFu);
    return 0;
  }
  uint8_t *round_rgba=malloc(4);
  if(!round_rgba){
    fprintf(stderr,"software runtime alpha fixture allocation mismatch\n");
    return 0;
  }
  round_rgba[0]=round_rgba[1]=round_rgba[2]=0; round_rgba[3]=1;
  int round_sprite=gml_sprite_append_from_rgba(&fixture->render,round_rgba,1,1,0,0,"<runtime-alpha>");
  if(round_sprite<0){
    fprintf(stderr,"software runtime alpha fixture creation mismatch\n");
    return 0;
  }
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  fixture->render.classic=1;
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  fixture->pixels[10*SOFTWARE3D_WIDTH+10]=0xFF4984ACu;
  gml_draw_sprite_ext(&fixture->render,round_sprite,0,10,10,1,1,0,0xFFFFFF,1);
  if(fixture->pixels[10*SOFTWARE3D_WIDTH+10]!=0xFF4983ABu){
    fprintf(stderr,"software classic runtime alpha rounding mismatch\n");
    return 0;
  }
  fixture->render.classic=0; memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  fixture->pixels[10*SOFTWARE3D_WIDTH+10]=0xFF4984ACu;
  gml_draw_sprite_ext(&fixture->render,round_sprite,0,10,10,1,1,0,0xFFFFFF,1);
  if(fixture->pixels[10*SOFTWARE3D_WIDTH+10]!=0xFF4883ABu){
    fprintf(stderr,"software modern runtime alpha rounding changed\n");
    return 0;
  }
  {
    const double rectangle[]={10,10,10,10,0};
    fixture->render.classic=1; fixture->render.color=0x0000FFu; fixture->render.alpha=.4;
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->pixels[10*SOFTWARE3D_WIDTH+10]=0xFFE9D6BDu;
    call_numbers(&fixture->vm,"draw_rectangle",rectangle,5);
    if(fixture->pixels[10*SOFTWARE3D_WIDTH+10]!=0xFFF28071u){
      fprintf(stderr,"software classic primitive alpha rounding mismatch: %08x\n",fixture->pixels[10*SOFTWARE3D_WIDTH+10]);
      return 0;
    }
    fixture->render.alpha=.5;
    fixture->pixels[10*SOFTWARE3D_WIDTH+10]=0xFFEAD5BDu;
    call_numbers(&fixture->vm,"draw_rectangle",rectangle,5);
    if(fixture->pixels[10*SOFTWARE3D_WIDTH+10]!=0xFFF46A5Eu){
      fprintf(stderr,"software classic primitive fixed-alpha tie mismatch: %08x\n",fixture->pixels[10*SOFTWARE3D_WIDTH+10]);
      return 0;
    }
    fixture->render.classic=0;
    fixture->render.alpha=.4;
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->pixels[10*SOFTWARE3D_WIDTH+10]=0xFFE9D6BDu;
    call_numbers(&fixture->vm,"draw_rectangle",rectangle,5);
    if(fixture->pixels[10*SOFTWARE3D_WIDTH+10]!=0xFFF18071u){
      fprintf(stderr,"software modern primitive alpha rounding changed: %08x\n",fixture->pixels[10*SOFTWARE3D_WIDTH+10]);
      return 0;
    }
    {
      const double outline_rectangle[]={8,8,12,12,1};
      fixture->render.classic=1; fixture->render.color=0x0000FFu; fixture->render.alpha=1;
      memset(fixture->pixels,0,sizeof(fixture->pixels));
      gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
      gml_render_set_pending_fill(&fixture->render,0xFF123456u);
      call_numbers(&fixture->vm,"draw_rectangle",outline_rectangle,5);
      gml_render_flush_pending_fill(&fixture->render);
      if(fixture->pixels[8*SOFTWARE3D_WIDTH+8]!=0xFFFF0000u || fixture->pixels[10*SOFTWARE3D_WIDTH+10]!=0xFF123456u){
        fprintf(stderr,"software outlined primitive deferred-clear ordering mismatch: edge=%08x centre=%08x\n",
          fixture->pixels[8*SOFTWARE3D_WIDTH+8],fixture->pixels[10*SOFTWARE3D_WIDTH+10]);
        return 0;
      }
    }
    fixture->render.color=0xFFFFFFu; fixture->render.alpha=1;
  }
  {
    GmlInstance relative_self={0}; relative_self.x=13; relative_self.y=17;
    GmlVal relative_on=vreal(1),relative_off=vreal(0);
    GmlVal action_sprite_args[4]={vreal(fixture->runtime_sprite),vreal(2),vreal(3),vreal(0)};
    fixture->vm.cur_self=&relative_self;
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    (void)call_values(&fixture->vm,"action_set_relative",&relative_on,1);
    (void)call_values(&fixture->vm,"action_draw_sprite",action_sprite_args,4);
    if((fixture->pixels[20*SOFTWARE3D_WIDTH+15]&0x00FFFFFFu)!=0xFF0000u || fixture->pixels[3*SOFTWARE3D_WIDTH+2]!=0){
      fprintf(stderr,"software relative action sprite position mismatch\n");
      return 0;
    }
    *gml_varmap_put(&fixture->vm.globals,"lives")=vreal(2);
    GmlVal life_args[3]={vreal(2),vreal(3),vreal(fixture->runtime_sprite)};
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    (void)call_values(&fixture->vm,"action_draw_life_images",life_args,3);
    if((fixture->pixels[20*SOFTWARE3D_WIDTH+15]&0x00FFFFFFu)!=0xFF0000u ||
       (fixture->pixels[20*SOFTWARE3D_WIDTH+17]&0x00FFFFFFu)!=0xFF0000u){
      fprintf(stderr,"software relative action life-image position mismatch\n");
      return 0;
    }
    *gml_varmap_put(&fixture->vm.globals,"health")=vreal(50);
    GmlVal health_args[6]={vreal(1),vreal(2),vreal(21),vreal(6),vreal(0),vreal(0)};
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    (void)call_values(&fixture->vm,"action_draw_health",health_args,6);
    if((fixture->pixels[21*SOFTWARE3D_WIDTH+18]&0x00FFFFFFu)!=0xFFFF00u || fixture->pixels[21*SOFTWARE3D_WIDTH+30]!=0){
      fprintf(stderr,"software relative action health-bar mismatch\n");
      return 0;
    }
    (void)call_values(&fixture->vm,"action_set_relative",&relative_off,1);

    uint8_t *animated_rgba=malloc(4*4);
    if(!animated_rgba){
      fprintf(stderr,"software action sprite fixture allocation mismatch\n");
      return 0;
    }
    memset(animated_rgba,255,4*4);
    int animated_sprite=gml_sprite_append_from_rgba_frames(
      &fixture->render,animated_rgba,1,1,4,0,0,"<action-animation>");
    if(animated_sprite<0){
      fprintf(stderr,"software action sprite fixture creation mismatch\n");
      return 0;
    }
    relative_self.sprite_index=fixture->runtime_sprite;
    relative_self.image_index=2.25;
    relative_self.image_speed=0;
    GmlVal keep_frame[3]={vreal(animated_sprite),vreal(-1),vreal(.5)};
    (void)call_values(&fixture->vm,"action_sprite_set",keep_frame,3);
    if(relative_self.sprite_index!=animated_sprite || relative_self.image_index!=2.25 ||
       relative_self.image_speed!=.5){
      fprintf(stderr,"software action sprite retained-frame mismatch\n");
      return 0;
    }
    relative_self.image_index=8.25;
    (void)call_values(&fixture->vm,"action_sprite_set",keep_frame,3);
    if(relative_self.image_index!=0){
      fprintf(stderr,"software action sprite out-of-range reset mismatch\n");
      return 0;
    }
    GmlVal select_frame[3]={vreal(animated_sprite),vreal(3),vreal(0)};
    (void)call_values(&fixture->vm,"action_sprite_set",select_frame,3);
    if(relative_self.image_index!=3 || relative_self.image_speed!=0){
      fprintf(stderr,"software action sprite explicit-frame mismatch\n");
      return 0;
    }
    fixture->vm.cur_self=NULL;
  }
  for(int i=0;i<4;i++) surface_data->px[i]=0xFFFFFFFFu;
  fixture->depth_sprite=gml_sprite_create_from_surface(&fixture->render,fixture->surface,0,0,2,2,0,0,0,0);
  return 1;
}

int software3d_case_vm_draw(Software3dRasterFixture *fixture){
  {
    uint8_t room_data[128]={0};
    GmlWin draw_win={0}; GmlVM draw_vm={0};
    GmlObject draw_object={0}; GmlInstance draw_instance={0};
    char *draw_room_strings[]={"neutral_room"};
    uint32_t draw_room_charoffs[]={80};
    store_u32le(room_data,1);
    store_u32le(room_data+4,16);
    store_u32le(room_data+16,80);
    store_u32le(room_data+16+8,SOFTWARE3D_WIDTH);
    store_u32le(room_data+16+12,SOFTWARE3D_HEIGHT);
    store_u32le(room_data+16+16,30);
    draw_win.data=room_data; draw_win.size=sizeof(room_data);
    draw_win.n_chunks=1; memcpy(draw_win.chunks[0].name,"ROOM",5);
    draw_win.chunks[0].off=0; draw_win.chunks[0].size=sizeof(room_data);
    draw_win.classic_version=800;
    draw_win.strs=draw_room_strings; draw_win.str_charoff=draw_room_charoffs; draw_win.n_strs=1;
    draw_object.name="neutral_default_draw"; draw_object.parent=-1;
    draw_vm.win=&draw_win; draw_vm.render=&fixture->render; draw_vm.room_index=0;
    draw_vm.objects=&draw_object; draw_vm.n_objects=1;
    draw_vm.inst=&draw_instance; draw_vm.inst_count=draw_vm.inst_cap=1;
    draw_instance.active=1; draw_instance.obj=0; draw_instance.visible=1;
    draw_instance.sprite_index=fixture->depth_sprite; draw_instance.mask_index=-1;
    draw_instance.x=4; draw_instance.y=4;
    draw_instance.image_xscale=draw_instance.image_yscale=1;
    draw_instance.image_alpha=.75; draw_instance.image_blend=0xFFFFFF;
    draw_instance.draw_layer_order=-1;
    GmlVal room_name=vstr("neutral_room");
    GmlVal room_lookup=call_values(&draw_vm,"asset_get_index",&room_name,1);
    if(room_lookup.t!=V_REAL || (int)room_lookup.d!=0){
      fprintf(stderr,"room asset name lookup mismatch: %g\n",room_lookup.d);
      return 0;
    }
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
    fixture->render.classic=1;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->render.alpha=.25;
    gml_vm_draw(&draw_vm);
    unsigned automatic=(fixture->pixels[4*SOFTWARE3D_WIDTH+4]>>16)&255u;
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->render.alpha=.25;
    draw_vm.cur_self=&draw_instance;
    (void)call_values(&draw_vm,"draw_self",NULL,0);
    draw_vm.cur_self=NULL;
    unsigned self_draw=(fixture->pixels[4*SOFTWARE3D_WIDTH+4]>>16)&255u;
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->render.alpha=.25;
    draw_vm.cur_self=&draw_instance;
    (void)call_values(&draw_vm,"draw_full_sprite",NULL,0);
    draw_vm.cur_self=NULL;
    unsigned full_sprite=(fixture->pixels[4*SOFTWARE3D_WIDTH+4]>>16)&255u;
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->render.alpha=.25;
    gml_draw_sprite(&fixture->render,fixture->depth_sprite,0,4,4);
    unsigned classic_basic=(fixture->pixels[4*SOFTWARE3D_WIDTH+4]>>16)&255u;
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    gml_draw_sprite_ext(&fixture->render,fixture->depth_sprite,0,4,4,1,1,0,0xFFFFFF,.25);
    unsigned explicit_alpha=(fixture->pixels[4*SOFTWARE3D_WIDTH+4]>>16)&255u;
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
    fixture->render.classic=0;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->render.alpha=.25;
    gml_draw_sprite(&fixture->render,fixture->depth_sprite,0,4,4);
    unsigned modern_basic=(fixture->pixels[4*SOFTWARE3D_WIDTH+4]>>16)&255u;
    /* GMS2 instance-layer visibility is a render gate independent of instance.visible. Hidden
     * layers keep stepping and colliding without drawing their sprites. */
    GmlRtLayer draw_layer={0};
    draw_layer.used=1; draw_layer.visible=0; draw_layer.order=7;
    draw_layer.script_begin=draw_layer.script_end=-1;
    draw_vm.rtl=&draw_layer; draw_vm.n_rtl=1; draw_vm.cap_rtl=1;
    draw_instance.draw_layer_order=7;
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    gml_vm_draw(&draw_vm);
    unsigned hidden_layer_pixel=fixture->pixels[4*SOFTWARE3D_WIDTH+4]&0x00FFFFFFu;
    draw_layer.visible=1;
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    gml_vm_draw(&draw_vm);
    unsigned visible_layer_pixel=fixture->pixels[4*SOFTWARE3D_WIDTH+4]&0x00FFFFFFu;
    /* A ROOM record has a global creation list and a distinct per-layer element list. At an
     * equal depth, the layer list is the authored back-to-front order; reversing creation order
     * would incorrectly leave element zero on top. */
    GmlInstance layer_order_instances[2]; memset(layer_order_instances,0,sizeof layer_order_instances);
    for(int i=0;i<2;i++){
      GmlInstance *in=&layer_order_instances[i];
      in->active=1; in->obj=0; in->id=(uint32_t)(100000+i); in->creation_seq=(uint64_t)(i+1);
      in->room_placed=1; in->visible=1; in->sprite_index=fixture->depth_sprite; in->mask_index=-1;
      in->x=in->y=4; in->image_xscale=in->image_yscale=1; in->image_alpha=1;
      in->image_blend=i?0x00FF00:0x0000FF;
      in->draw_layer_order=7; in->draw_layer_element_order=i;
    }
    draw_vm.inst=layer_order_instances; draw_vm.inst_count=draw_vm.inst_cap=2;
    draw_win.classic_version=0;
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    gml_vm_draw(&draw_vm);
    unsigned authored_layer_front=fixture->pixels[4*SOFTWARE3D_WIDTH+4]&0x00FFFFFFu;
    draw_win.classic_version=800;
    draw_vm.inst=&draw_instance; draw_vm.inst_count=draw_vm.inst_cap=1;
    draw_vm.rtl=NULL; draw_vm.n_rtl=draw_vm.cap_rtl=0;
    draw_instance.draw_layer_order=-1;
    if(automatic<185 || automatic>195 || self_draw!=automatic || full_sprite!=255 ||
       gml_builtin_fast_id(&draw_vm,"draw_full_sprite")!=gml_builtin_fast_id(&draw_vm,"draw_self") || classic_basic!=255 ||
       explicit_alpha<55 || explicit_alpha>70 || modern_basic<55 || modern_basic>70 ||
       hidden_layer_pixel!=0 || visible_layer_pixel==0 || authored_layer_front!=0x00FF00u){
      fprintf(stderr,"default/self/full/basic/explicit/layer draw mismatch: automatic=%u self=%u full=%u classic=%u explicit=%u modern=%u hidden=%06x visible=%06x front=%06x\n",
        automatic,self_draw,full_sprite,classic_basic,explicit_alpha,modern_basic,
        hidden_layer_pixel,visible_layer_pixel,authored_layer_front);
      return 0;
    }
    {
      uint32_t shadow_pixels[SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT];
      const double simple_geometry[]={3,4,5,9,0x404040,0x404040,0};
      const double extended_geometry[]={3,4,5,7,0x404040,0x404040,0};
      GmlVal extended_args[2]={vreal(2),vreal(.25)};
      GmlVal invalid_arg=vreal(9);
      fixture->render.classic=1;
      for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
      gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
      fixture->render.alpha=.9; draw_vm.cur_self=&draw_instance;
      (void)call_values(&draw_vm,"draw_shadow",NULL,0);
      draw_vm.cur_self=NULL;
      memcpy(shadow_pixels,fixture->pixels,sizeof(shadow_pixels));
      if(fixture->render.alpha!=1){
        fprintf(stderr,"legacy simple shadow alpha reset mismatch: %.6f\n",fixture->render.alpha);
        return 0;
      }
      for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
      gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
      fixture->render.alpha=.5;
      call_numbers(&draw_vm,"draw_ellipse_color",simple_geometry,7);
      if(memcmp(shadow_pixels,fixture->pixels,sizeof(shadow_pixels))){
        fprintf(stderr,"legacy simple shadow geometry mismatch\n");
        return 0;
      }
      for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
      gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
      fixture->render.alpha=.8; draw_vm.cur_self=&draw_instance;
      (void)call_values(&draw_vm,"draw_shadow_ext",extended_args,2);
      draw_vm.cur_self=NULL;
      memcpy(shadow_pixels,fixture->pixels,sizeof(shadow_pixels));
      if(fixture->render.alpha!=1){
        fprintf(stderr,"legacy extended shadow alpha reset mismatch: %.6f\n",fixture->render.alpha);
        return 0;
      }
      for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
      gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
      fixture->render.alpha=.25;
      call_numbers(&draw_vm,"draw_ellipse_color",extended_geometry,7);
      if(memcmp(shadow_pixels,fixture->pixels,sizeof(shadow_pixels)) ||
         gml_builtin_fast_id(&draw_vm,"draw_shadow")<0 ||
         gml_builtin_fast_id(&draw_vm,"draw_shadow")!=gml_builtin_fast_id(&draw_vm,"draw_shadow_ext")){
        fprintf(stderr,"legacy extended shadow geometry/dispatch mismatch\n");
        return 0;
      }
      for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF000000u;
      gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
      fixture->render.alpha=.3; draw_vm.cur_self=&draw_instance;
      (void)call_values(&draw_vm,"draw_shadow",&invalid_arg,1);
      draw_vm.cur_self=NULL;
      if(fixture->render.alpha!=.3 || colored_pixels(fixture->pixels,SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT)!=0){
        fprintf(stderr,"legacy shadow invalid-arity guard mismatch\n");
        return 0;
      }
    }
    fixture->render.classic=0;
    fixture->render.alpha=1;
  }
  {
    GmlVM extension_vm={0};
    fixture_attach_input(&extension_vm);
    gml_keyboard_unset_map(&extension_vm);
    GmlWin extension_win={0};
    GmlObject extension_object={0};
    GmlVal create_args[3]={vreal(12.5),vreal(34.5),vreal(0)};
    GmlVal invalid_arg=vreal(0);
    extension_object.name="neutral_extension_object";
    extension_object.parent=-1;
    extension_object.sprite_index=extension_object.mask_index=-1;
    extension_object.visible=1;
    extension_vm.win=&extension_win;
    extension_vm.objects=&extension_object;
    extension_vm.n_objects=1;
    extension_vm.inst=calloc(4,sizeof(*extension_vm.inst));
    extension_vm.inst_cap=4;
    extension_vm.next_id=100001;
    if(!extension_vm.inst){
      fprintf(stderr,"legacy extension instance fixture allocation mismatch\n");
      return 0;
    }
    GmlVal create_result=call_values(&extension_vm,"crear",create_args,3);
    if(create_result.d!=0 || extension_vm.inst_count!=1 ||
       extension_vm.inst[0].x!=12.5 || extension_vm.inst[0].y!=34.5 ||
       extension_vm.inst[0].obj!=0 ||
       gml_builtin_fast_id(&extension_vm,"crear")<0){
      fprintf(stderr,"legacy extension instance-create alias mismatch\n");
      free(extension_vm.inst);
      return 0;
    }
    (void)call_values(&extension_vm,"crear",&invalid_arg,1);
    if(extension_vm.inst_count!=1){
      fprintf(stderr,"legacy extension instance-create arity mismatch\n");
      free(extension_vm.inst);
      return 0;
    }
    extension_vm.cur_self=&extension_vm.inst[0];
    extension_vm.cur_self->y=250;
    extension_vm.cur_self->depth=77;
    (void)call_values(&extension_vm,"depthy",NULL,0);
    if(extension_vm.cur_self->depth!=-2.5 || gml_builtin_fast_id(&extension_vm,"depthy")<0){
      fprintf(stderr,"legacy extension depth-by-y mismatch: %.6f\n",extension_vm.cur_self->depth);
      free(extension_vm.inst);
      return 0;
    }
    extension_vm.cur_self->depth=19;
    (void)call_values(&extension_vm,"depthy",&invalid_arg,1);
    if(extension_vm.cur_self->depth!=19){
      fprintf(stderr,"legacy extension depth-by-y arity mismatch\n");
      free(extension_vm.inst);
      return 0;
    }
    {
      GmlVal movement_args[6]={vreal(10),vreal(20),vreal(30),vreal(3),vreal(.5),vreal(1)};
      GmlInstance *moving=extension_vm.cur_self;
      moving->vspeed=0; moving->hspeed=0; moving->image_index=4;
      fixture_key=37; fixture_key_edge=0;
      (void)call_values(&extension_vm,"move_rpg",movement_args,5);
      if(moving->hspeed!=-3 || moving->vspeed!=0 || moving->sprite_index!=20 ||
         moving->image_xscale!=-1 || moving->image_speed!=.5 || moving->speed!=3 ||
         moving->direction!=180 || gml_builtin_fast_id(&extension_vm,"move_rpg")<0){
        fprintf(stderr,"legacy extension cursor movement mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      fixture_key_edge=2;
      (void)call_values(&extension_vm,"move_rpg",movement_args,5);
      if(moving->hspeed!=0 || moving->image_speed!=0 || moving->image_index!=0 || moving->speed!=0){
        fprintf(stderr,"legacy extension movement release mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      fixture_key='D'; fixture_key_edge=0;
      (void)call_values(&extension_vm,"move_rpg",movement_args,6);
      if(moving->hspeed!=3 || moving->sprite_index!=20 || moving->image_xscale!=1 || moving->image_speed!=.5){
        fprintf(stderr,"legacy extension letter movement mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      fixture_key=fixture_key_edge=-1;
      moving->hspeed=7;
      (void)call_values(&extension_vm,"move_rpg",movement_args,4);
      if(moving->hspeed!=7){
        fprintf(stderr,"legacy extension movement arity mismatch\n");
        free(extension_vm.inst);
        return 0;
      }

      GmlVal direction_args[4]={vreal(20),vreal(10),vreal(30),vreal(.25)};
      moving->direction=0; moving->image_xscale=-1;
      (void)call_values(&extension_vm,"direction_rpg",direction_args,4);
      if(moving->sprite_index!=20 || moving->image_xscale!=1 || moving->image_speed!=.25 ||
         gml_builtin_fast_id(&extension_vm,"direction_rpg")<0){
        fprintf(stderr,"legacy extension right-facing animation mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      moving->direction=90;
      (void)call_values(&extension_vm,"direction_rpg",direction_args,4);
      if(moving->sprite_index!=10){
        fprintf(stderr,"legacy extension upward animation mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      moving->direction=180;
      (void)call_values(&extension_vm,"direction_rpg",direction_args,4);
      if(moving->sprite_index!=20 || moving->image_xscale!=-1){
        fprintf(stderr,"legacy extension left-facing animation mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      moving->direction=270;
      (void)call_values(&extension_vm,"direction_rpg",direction_args,4);
      if(moving->sprite_index!=30){
        fprintf(stderr,"legacy extension downward animation mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      moving->direction=270; moving->y=0; moving->vspeed=9;
      (void)call_values(&extension_vm,"friction_platform",NULL,0);
      if(moving->y!=12 || moving->vspeed!=0 || gml_builtin_fast_id(&extension_vm,"friction_platform")<0){
        fprintf(stderr,"legacy extension platform-contact mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
      (void)call_values(&extension_vm,"keyboard_wait",NULL,0);
      (void)call_values(&extension_vm,"destruir",NULL,0);
      if(!moving->marked || gml_builtin_fast_id(&extension_vm,"destruir")<0){
        fprintf(stderr,"legacy extension destroy-self mismatch\n");
        free(extension_vm.inst);
        return 0;
      }
    }
    free_extension_fixture(&extension_vm);
  }
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  call_numbers(&fixture->vm,"d3d_start",NULL,0);
  call_numbers(&fixture->vm,"d3d_set_projection_ortho",software3d_ortho,5);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_disable,1);
  call_numbers(&fixture->vm,"d3d_set_hidden",software3d_enable,1);
  return 1;
}
