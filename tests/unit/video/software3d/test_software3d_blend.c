/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "software3d_test_fixture.h"


int software3d_case_blend_shader(Software3dRasterFixture *fixture){
  const double far_depth[]={10},farther_depth[]={20},near_depth[]={-10};
  call_numbers(&fixture->vm,"d3d_set_depth",far_depth,1);
  gml_draw_sprite_ext(&fixture->render,fixture->depth_sprite,0,24,12,8,8,0,0x0000FF,1);
  call_numbers(&fixture->vm,"d3d_set_depth",farther_depth,1);
  gml_draw_sprite_ext(&fixture->render,fixture->depth_sprite,0,24,12,8,8,0,0x00FF00,1);
  if((fixture->pixels[18*SOFTWARE3D_WIDTH+30]&0x00FFFFFFu)!=0xFF0000u){
    fprintf(stderr,"software D3 2D sprite far-depth mismatch\n");
    return 0;
  }
  call_numbers(&fixture->vm,"d3d_set_depth",near_depth,1);
  gml_draw_sprite_ext(&fixture->render,fixture->depth_sprite,0,24,12,8,8,0,0x00FF00,1);
  if((fixture->pixels[18*SOFTWARE3D_WIDTH+30]&0x00FFFFFFu)!=0x00FF00u){
    fprintf(stderr,"software D3 2D sprite near-depth mismatch\n");
    return 0;
  }
  call_numbers(&fixture->vm,"d3d_set_depth",far_depth,1);
  gml_draw_sprite_ext(&fixture->render,fixture->depth_sprite,0,4,12,8,8,0,0x0000FF,1);
  gml_draw_sprite_ext(&fixture->render,fixture->depth_sprite,0,4,12,8,8,0,0x00FF00,1);
  if((fixture->pixels[18*SOFTWARE3D_WIDTH+10]&0x00FFFFFFu)!=0x00FF00u){
    fprintf(stderr,"software D3 coplanar 2D overwrite mismatch\n");
    return 0;
  }
  fixture->render.atlas=calloc(1,sizeof(*fixture->render.atlas)); fixture->render.tpag=calloc(1,sizeof(*fixture->render.tpag));
  fixture->render.bg=calloc(1,sizeof(*fixture->render.bg)); fixture->render.n_atlas=fixture->render.n_tpag=fixture->render.n_bg=1;
  if(!fixture->render.atlas || !fixture->render.tpag || !fixture->render.bg || !(fixture->render.atlas[0].px=malloc(16))){
    fprintf(stderr,"software D3 background texture allocation mismatch\n");
    return 0;
  }
  fixture->render.atlas[0].w=fixture->render.atlas[0].h=2;
  for(int i=0;i<4;i++){
    fixture->render.atlas[0].px[i*4]=fixture->render.atlas[0].px[i*4+1]=fixture->render.atlas[0].px[i*4+2]=fixture->render.atlas[0].px[i*4+3]=255;
  }
  fixture->render.tpag[0].atlas=0; fixture->render.tpag[0].sw=fixture->render.tpag[0].sh=2;
  fixture->render.tpag[0].bw=fixture->render.tpag[0].bh=2; fixture->render.bg[0].tpag=0;
  fixture->render.classic=1; gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  {
    enum { PROJECTED_W=62, PROJECTED_H=47 };
    uint32_t phase[3][SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT];
    const uint8_t rgba[16]={
      255,0,0,255, 0,255,0,255,
      0,0,255,255, 255,255,255,255
    };
    memcpy(fixture->render.atlas[0].px,rgba,sizeof(rgba));
    memset(fixture->pixels,0,sizeof(fixture->pixels)); memset(phase,0,sizeof(phase));
    fixture->render.interp=1;
    gml_render_begin(&fixture->render,fixture->pixels,PROJECTED_W,PROJECTED_H,340,0);
    for(int q=0;q<3;q++) fixture->render.classic_interp_phase[q]=phase[q];
    gml_draw_background_ext(&fixture->render,0,340,0,1,1,0xFFFFFF,1);
    if(fixture->render.interp_subrect_count!=1 || fixture->render.interp_subrect_bytes!=48 ||
       !fixture->render.interp_subrect_cache[0].projected_x ||
       fixture->render.interp_subrect_cache[0].projected_y ||
       !fixture->render.interp_subrect_cache[0].phase[0] ||
       !fixture->render.interp_subrect_cache[0].phase[1] ||
       !fixture->render.interp_subrect_cache[0].phase[2] ||
       phase[0][0]!=fixture->render.interp_subrect_cache[0].phase[0][1] ||
       phase[1][0]!=fixture->render.interp_subrect_cache[0].phase[1][2] ||
       phase[2][0]!=fixture->render.interp_subrect_cache[0].phase[2][3]){
      fprintf(stderr,"software classic projected interpolation cache mismatch\n");
      return 0;
    }
    size_t cached_bytes=fixture->render.interp_subrect_bytes;
    fixture->render.interp_subrect_bytes=16u*1024u*1024u;
    memset(fixture->pixels,0,sizeof(fixture->pixels)); memset(phase,0,sizeof(phase));
    gml_render_begin(&fixture->render,fixture->pixels,PROJECTED_W,PROJECTED_H,340,0);
    for(int q=0;q<3;q++) fixture->render.classic_interp_phase[q]=phase[q];
    gml_draw_background_ext(&fixture->render,0,340,0,1,1,0xFFFFFF,1);
    if(fixture->render.interp_subrect_count!=1 || fixture->render.interp_subrect_bytes!=16u*1024u*1024u){
      fprintf(stderr,"software classic projected interpolation cache reuse mismatch\n");
      return 0;
    }
    fixture->render.interp_subrect_bytes=cached_bytes;
    fixture->render.interp=0;
    free(fixture->render.tpag[0].alpha_row_min); fixture->render.tpag[0].alpha_row_min=NULL;
    free(fixture->render.tpag[0].alpha_row_max); fixture->render.tpag[0].alpha_row_max=NULL;
    free(fixture->render.tpag[0].alpha_runs); fixture->render.tpag[0].alpha_runs=NULL;
    free(fixture->render.tpag[0].argb_cache); fixture->render.tpag[0].argb_cache=NULL;
    fixture->render.tpag[0].alpha_scanned=0;
    fixture->render.tpag[0].alpha_runs_built=0;
    fixture->render.tpag[0].alpha_run_count=0;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  }
  {
    uint32_t phase[3][SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT];
    uint8_t *fringe_atlas=(uint8_t*)realloc(fixture->render.atlas[0].px,12);
    if(!fringe_atlas){
      fprintf(stderr,"software classic transparent-fringe allocation mismatch\n");
      return 0;
    }
    fixture->render.atlas[0].px=fringe_atlas;
    fixture->render.atlas[0].w=3; fixture->render.atlas[0].h=1;
    const uint8_t rgba[12]={
      200,0,0,0, 100,100,100,255, 0,200,0,0
    };
    memcpy(fixture->render.atlas[0].px,rgba,sizeof(rgba));
    fixture->render.tpag[0].sx=1; fixture->render.tpag[0].sy=0;
    fixture->render.tpag[0].sw=fixture->render.tpag[0].sh=1;
    fixture->render.tpag[0].tx=1; fixture->render.tpag[0].ty=0;
    fixture->render.tpag[0].bw=3; fixture->render.tpag[0].bh=1;
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) fixture->pixels[i]=0xFF202020u;
    for(int q=0;q<3;q++) for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) phase[q][i]=0xFF202020u;
    fixture->render.interp=1; fixture->render.blendmode=0; fixture->render.alphablend=1;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,32,0,0);
    for(int q=0;q<3;q++) fixture->render.classic_interp_phase[q]=phase[q];
    gml_draw_background_ext(&fixture->render,0,4,4,1,1,0xFFFFFF,1);
    if(phase[0][4*SOFTWARE3D_WIDTH+4]!=0xFF5B2929u ||
       phase[0][4*SOFTWARE3D_WIDTH+5]!=0xFF295B29u ||
       !fixture->render.tpag[0].interp_phase_cache[0]){
      fprintf(stderr,"software classic transparent-fringe interpolation mismatch: %08x %08x\n",
              phase[0][4*SOFTWARE3D_WIDTH+4],phase[0][4*SOFTWARE3D_WIDTH+5]);
      return 0;
    }
    for(int q=0;q<3;q++){
      fixture->render.classic_interp_phase[q]=NULL;
      free(fixture->render.tpag[0].interp_phase_cache[q]);
      fixture->render.tpag[0].interp_phase_cache[q]=NULL;
    }
    fixture->render.interp=0;
    uint8_t *restored_atlas=(uint8_t*)realloc(fixture->render.atlas[0].px,16);
    if(!restored_atlas){
      fprintf(stderr,"software classic atlas restore allocation mismatch\n");
      return 0;
    }
    fixture->render.atlas[0].px=restored_atlas;
    fixture->render.atlas[0].w=fixture->render.atlas[0].h=2;
    fixture->render.tpag[0].sx=fixture->render.tpag[0].sy=0;
    fixture->render.tpag[0].sw=fixture->render.tpag[0].sh=2;
    fixture->render.tpag[0].tx=fixture->render.tpag[0].ty=0;
    fixture->render.tpag[0].bw=fixture->render.tpag[0].bh=2;
    free(fixture->render.tpag[0].alpha_row_min); fixture->render.tpag[0].alpha_row_min=NULL;
    free(fixture->render.tpag[0].alpha_row_max); fixture->render.tpag[0].alpha_row_max=NULL;
    free(fixture->render.tpag[0].alpha_runs); fixture->render.tpag[0].alpha_runs=NULL;
    free(fixture->render.tpag[0].argb_cache); fixture->render.tpag[0].argb_cache=NULL;
    fixture->render.tpag[0].alpha_scanned=0;
    fixture->render.tpag[0].alpha_runs_built=0;
    fixture->render.tpag[0].alpha_run_count=0;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  }
  for(int i=0;i<4;i++){
    fixture->render.atlas[0].px[i*4]=fixture->render.atlas[0].px[i*4+1]=fixture->render.atlas[0].px[i*4+2]=192;
    fixture->render.atlas[0].px[i*4+3]=254;
  }
  fixture->pixels[5*SOFTWARE3D_WIDTH+5]=0xFF8F4848u;
  gml_draw_background_part_ext(&fixture->render,0,0,0,2,2,5,5,1,1,0xFFFFFF,1);
  if(fixture->pixels[5*SOFTWARE3D_WIDTH+5]!=0xFFC0BFBFu){
    fprintf(stderr,"software classic atlas fixed-point blend mismatch: %08x\n",fixture->pixels[5*SOFTWARE3D_WIDTH+5]);
    return 0;
  }
  for(int i=0;i<4;i++){
    fixture->render.atlas[0].px[i*4]=255;
    fixture->render.atlas[0].px[i*4+1]=64;
    fixture->render.atlas[0].px[i*4+2]=64;
    fixture->render.atlas[0].px[i*4+3]=111;
  }
  fixture->pixels[5*SOFTWARE3D_WIDTH+5]=0xFF46AB70u;
  gml_draw_background_ext(&fixture->render,0,5,5,1,1,0xFFFFFF,1);
  if(fixture->pixels[5*SOFTWARE3D_WIDTH+5]!=0xFF977D5Bu){
    fprintf(stderr,"software classic cached atlas blend mismatch: %08x\n",fixture->pixels[5*SOFTWARE3D_WIDTH+5]);
    return 0;
  }
  free(fixture->render.tpag[0].alpha_row_min); fixture->render.tpag[0].alpha_row_min=NULL;
  free(fixture->render.tpag[0].alpha_row_max); fixture->render.tpag[0].alpha_row_max=NULL;
  free(fixture->render.tpag[0].alpha_runs); fixture->render.tpag[0].alpha_runs=NULL;
  free(fixture->render.tpag[0].argb_cache); fixture->render.tpag[0].argb_cache=NULL;
  fixture->render.tpag[0].alpha_scanned=0;
  fixture->render.tpag[0].alpha_runs_built=0;
  fixture->render.tpag[0].alpha_run_count=0;
  {
    uint8_t atlas_backup[16];
    memcpy(atlas_backup,fixture->render.atlas[0].px,sizeof(atlas_backup));
    for(int i=0;i<4;i++){
      fixture->render.atlas[0].px[i*4]=fixture->render.atlas[0].px[i*4+1]=fixture->render.atlas[0].px[i*4+2]=0;
      fixture->render.atlas[0].px[i*4+3]=1;
    }
    fixture->pixels[5*SOFTWARE3D_WIDTH+5]=0xFF858585u;
    gml_draw_background_ext(&fixture->render,0,5,5,1,1,0xFFFFFF,.5);
    if(fixture->pixels[5*SOFTWARE3D_WIDTH+5]!=0xFF848484u){
      fprintf(stderr,"software classic draw-alpha quantization mismatch: %08x\n",fixture->pixels[5*SOFTWARE3D_WIDTH+5]);
      return 0;
    }
    free(fixture->render.tpag[0].alpha_row_min); fixture->render.tpag[0].alpha_row_min=NULL;
    free(fixture->render.tpag[0].alpha_row_max); fixture->render.tpag[0].alpha_row_max=NULL;
    free(fixture->render.tpag[0].alpha_runs); fixture->render.tpag[0].alpha_runs=NULL;
    free(fixture->render.tpag[0].argb_cache); fixture->render.tpag[0].argb_cache=NULL;
    fixture->render.tpag[0].alpha_scanned=0;
    fixture->render.tpag[0].alpha_runs_built=0;
    fixture->render.tpag[0].alpha_run_count=0;
    for(int i=0;i<4;i++){
      fixture->render.atlas[0].px[i*4]=fixture->render.atlas[0].px[i*4+1]=fixture->render.atlas[0].px[i*4+2]=1;
      fixture->render.atlas[0].px[i*4+3]=255;
    }
    GmlWin blend_win={0};
    fixture->render.classic=0; fixture->render.win=&blend_win;
    blend_win.bytecode=15;
    fixture->pixels[5*SOFTWARE3D_WIDTH+5]=0xFF000000u;
    gml_draw_background_ext(&fixture->render,0,5,5,1,1,0xFFFFFF,128.0/255.0);
    if(fixture->pixels[5*SOFTWARE3D_WIDTH+5]!=0xFF000000u){
      fprintf(stderr,"software Studio 1.x combined truncation mismatch: %08x\n",fixture->pixels[5*SOFTWARE3D_WIDTH+5]);
      return 0;
    }
    blend_win.bytecode=17;
    fixture->pixels[5*SOFTWARE3D_WIDTH+5]=0xFF000000u;
    gml_draw_background_ext(&fixture->render,0,5,5,1,1,0xFFFFFF,128.0/255.0);
    if(fixture->pixels[5*SOFTWARE3D_WIDTH+5]!=0xFF010101u){
      fprintf(stderr,"software GMS2 combined UNORM blend mismatch: %08x\n",fixture->pixels[5*SOFTWARE3D_WIDTH+5]);
      return 0;
    }
    fixture->render.classic=1; fixture->render.win=NULL;
    memcpy(fixture->render.atlas[0].px,atlas_backup,sizeof(atlas_backup));
    free(fixture->render.tpag[0].alpha_row_min); fixture->render.tpag[0].alpha_row_min=NULL;
    free(fixture->render.tpag[0].alpha_row_max); fixture->render.tpag[0].alpha_row_max=NULL;
    free(fixture->render.tpag[0].alpha_runs); fixture->render.tpag[0].alpha_runs=NULL;
    free(fixture->render.tpag[0].argb_cache); fixture->render.tpag[0].argb_cache=NULL;
    fixture->render.tpag[0].alpha_scanned=0;
    fixture->render.tpag[0].alpha_runs_built=0;
    fixture->render.tpag[0].alpha_run_count=0;
  }
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  for(int i=0;i<4;i++) fixture->render.atlas[0].px[i*4+3]=255;
  fixture->render.atlas[0].px[0]=0;   fixture->render.atlas[0].px[1]=16; fixture->render.atlas[0].px[2]=255;
  fixture->render.atlas[0].px[4]=255; fixture->render.atlas[0].px[5]=0;  fixture->render.atlas[0].px[6]=0;
  fixture->pixels[5*SOFTWARE3D_WIDTH+5]=fixture->pixels[5*SOFTWARE3D_WIDTH+6]=0xFFC0C0C0u;
  gml_draw_background_ext(&fixture->render,0,5.2,5.2,.75,1,0xFFFFFF,231.0/255.0);
  if(fixture->pixels[5*SOFTWARE3D_WIDTH+5]!=0xFF1220F9u || fixture->pixels[5*SOFTWARE3D_WIDTH+6]!=0xFFF91212u){
    fprintf(stderr,"software classic scaled atlas phase/blend mismatch: %08x %08x\n",
            fixture->pixels[5*SOFTWARE3D_WIDTH+5],fixture->pixels[5*SOFTWARE3D_WIDTH+6]);
    return 0;
  }
  fixture->pixels[8*SOFTWARE3D_WIDTH+6]=0xFFC0C0C0u;
  gml_draw_background_ext(&fixture->render,0,5.5,8,.5,1,0xFFFFFF,1);
  if(fixture->pixels[8*SOFTWARE3D_WIDTH+6]!=0xFFFF0000u){
    fprintf(stderr,"software classic reciprocal-scale texel tie mismatch: %08x\n",
            fixture->pixels[8*SOFTWARE3D_WIDTH+6]);
    return 0;
  }
  for(int i=0;i<4;i++)
    fixture->render.atlas[0].px[i*4]=fixture->render.atlas[0].px[i*4+1]=fixture->render.atlas[0].px[i*4+2]=fixture->render.atlas[0].px[i*4+3]=255;
  fixture->flipped_sprite=fixture->render.n_spr++;
  GmlSprite *grown_sprites=realloc(fixture->render.spr,(size_t)fixture->render.n_spr*sizeof(*fixture->render.spr));
  if(!grown_sprites){
    fprintf(stderr,"software sprite flip allocation mismatch\n");
    return 0;
  }
  fixture->render.spr=grown_sprites;
  GmlSprite *flipped=&fixture->render.spr[fixture->flipped_sprite];
  memset(flipped,0,sizeof(*flipped));
  flipped->w=flipped->h=2; flipped->n_frames=1;
  flipped->frame=malloc(sizeof(*flipped->frame));
  if(!flipped->frame){
    fprintf(stderr,"software sprite flip frame allocation mismatch\n");
    return 0;
  }
  flipped->frame[0]=0;
  {
    /* Sprite blending applies source-alpha factors to target alpha. A transparent render target
     * must therefore retain partial coverage across the optimized unscaled, scaled, flipped,
     * rotated and interpolated atlas paths. */
    uint8_t atlas_backup[16];
    memcpy(atlas_backup,fixture->render.atlas[0].px,sizeof(atlas_backup));
    for(int i=0;i<4;i++){
      fixture->render.atlas[0].px[i*4]=120;
      fixture->render.atlas[0].px[i*4+1]=130;
      fixture->render.atlas[0].px[i*4+2]=177;
      fixture->render.atlas[0].px[i*4+3]=100;
    }
    free(fixture->render.tpag[0].alpha_row_min); fixture->render.tpag[0].alpha_row_min=NULL;
    free(fixture->render.tpag[0].alpha_row_max); fixture->render.tpag[0].alpha_row_max=NULL;
    free(fixture->render.tpag[0].alpha_runs); fixture->render.tpag[0].alpha_runs=NULL;
    free(fixture->render.tpag[0].argb_cache); fixture->render.tpag[0].argb_cache=NULL;
    fixture->render.tpag[0].alpha_scanned=0;
    fixture->render.tpag[0].alpha_runs_built=0;
    fixture->render.tpag[0].alpha_run_count=0;
    int alpha_surface=gml_surface_create(&fixture->render,16,16);
    GmlWin alpha_win={0}; alpha_win.bytecode=17;
    GmlWin *saved_win=fixture->render.win;
    int saved_classic=fixture->render.classic,saved_interp=fixture->render.interp;
    fixture->render.win=&alpha_win; fixture->render.classic=0; fixture->render.interp=0;
    if(alpha_surface<=0 || !gml_surface_set_target(&fixture->render,alpha_surface)){
      fprintf(stderr,"software sprite target-alpha surface creation mismatch\n");
      return 0;
    }
    gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,1,1,1,1,0,0xFFFFFF,1);
    gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,5,1,2,2,0,0xFFFFFF,1);
    gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,13,1,-1,1,0,0xFFFFFF,1);
    gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,3,11,1,1,37,0xFFFFFF,1);
    fixture->render.interp=1;
    gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,10,10,1,1,0,0xFFFFFF,1);
    gml_surface_reset_target(&fixture->render);
    GmlSurface *alpha_data=&fixture->render.surface[alpha_surface-1];
    int covered=0,bad_alpha=0;
    for(int i=0;i<alpha_data->w*alpha_data->h;i++){
      unsigned coverage=alpha_data->px[i]>>24;
      if(coverage){ covered++; if(coverage!=39u) bad_alpha++; }
    }
    if(covered<28 || bad_alpha){
      fprintf(stderr,"software sprite target-alpha mismatch: covered=%d bad=%d\n",
              covered,bad_alpha);
      return 0;
    }
    gml_surface_free(&fixture->render,alpha_surface);
    fixture->render.win=saved_win; fixture->render.classic=saved_classic; fixture->render.interp=saved_interp;
    memcpy(fixture->render.atlas[0].px,atlas_backup,sizeof(atlas_backup));
    free(fixture->render.tpag[0].alpha_row_min); fixture->render.tpag[0].alpha_row_min=NULL;
    free(fixture->render.tpag[0].alpha_row_max); fixture->render.tpag[0].alpha_row_max=NULL;
    free(fixture->render.tpag[0].alpha_runs); fixture->render.tpag[0].alpha_runs=NULL;
    free(fixture->render.tpag[0].argb_cache); fixture->render.tpag[0].argb_cache=NULL;
    fixture->render.tpag[0].alpha_scanned=0;
    fixture->render.tpag[0].alpha_runs_built=0;
    fixture->render.tpag[0].alpha_run_count=0;
  }
  {
    /* Integer-aligned unit-scale modern sprites sample exact texel centres. Enabling hardware
     * interpolation must therefore preserve the raster byte-for-byte. */
    uint32_t nearest[SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT];
    uint32_t filtered[SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT];
    uint8_t atlas_backup[16];
    memcpy(atlas_backup,fixture->render.atlas[0].px,sizeof(atlas_backup));
    fixture->render.atlas[0].px[3]=128;
    free(fixture->render.tpag[0].alpha_row_min); fixture->render.tpag[0].alpha_row_min=NULL;
    free(fixture->render.tpag[0].alpha_row_max); fixture->render.tpag[0].alpha_row_max=NULL;
    free(fixture->render.tpag[0].alpha_runs); fixture->render.tpag[0].alpha_runs=NULL;
    free(fixture->render.tpag[0].argb_cache); fixture->render.tpag[0].argb_cache=NULL;
    fixture->render.tpag[0].alpha_scanned=0;
    fixture->render.tpag[0].alpha_runs_built=0;
    fixture->render.tpag[0].alpha_run_count=0;
    for(int i=0;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++)
      nearest[i]=filtered[i]=0xFF102030u+(uint32_t)(i&15);
    GmlWin modern_win={0};
    modern_win.bytecode=17;
    GmlWin *saved_win=fixture->render.win;
    int saved_classic=fixture->render.classic;
    int saved_interp=fixture->render.interp;
    fixture->render.win=&modern_win;
    fixture->render.classic=0;
    fixture->render.interp=0;
    gml_render_begin(&fixture->render,nearest,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,10,10,1,1,0,0xFFFFFF,1);
    fixture->render.interp=1;
    gml_render_begin(&fixture->render,filtered,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,10,10,1,1,0,0xFFFFFF,1);
    fixture->render.win=saved_win;
    fixture->render.classic=saved_classic;
    fixture->render.interp=saved_interp;
    memcpy(fixture->render.atlas[0].px,atlas_backup,sizeof(atlas_backup));
    free(fixture->render.tpag[0].alpha_row_min); fixture->render.tpag[0].alpha_row_min=NULL;
    free(fixture->render.tpag[0].alpha_row_max); fixture->render.tpag[0].alpha_row_max=NULL;
    free(fixture->render.tpag[0].alpha_runs); fixture->render.tpag[0].alpha_runs=NULL;
    free(fixture->render.tpag[0].argb_cache); fixture->render.tpag[0].argb_cache=NULL;
    fixture->render.tpag[0].alpha_scanned=0;
    fixture->render.tpag[0].alpha_runs_built=0;
    fixture->render.tpag[0].alpha_run_count=0;
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    if(memcmp(nearest,filtered,sizeof(nearest))){
      fprintf(stderr,"software modern exact-centre interpolation mismatch\n");
      return 0;
    }
  }
  fixture->render.classic=1;
  gml_vm_software3d_reset(&fixture->vm); memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,20,20,-1,1,0,0xFFFFFF,1);
  gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,30,30,1,-1,0,0xFFFFFF,1);
  if((fixture->pixels[20*SOFTWARE3D_WIDTH+18]&0x00FFFFFFu)==0 ||
     (fixture->pixels[20*SOFTWARE3D_WIDTH+19]&0x00FFFFFFu)==0 || fixture->pixels[20*SOFTWARE3D_WIDTH+20]!=0 ||
     (fixture->pixels[28*SOFTWARE3D_WIDTH+30]&0x00FFFFFFu)==0 ||
     (fixture->pixels[29*SOFTWARE3D_WIDTH+30]&0x00FFFFFFu)==0 || fixture->pixels[30*SOFTWARE3D_WIDTH+30]!=0){
    fprintf(stderr,"software negative sprite scale anchor mismatch\n");
    return 0;
  }
  fixture->render.classic=0;
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,20,20,-1,1,0,0xFFFFFF,1);
  gml_draw_sprite_ext(&fixture->render,fixture->flipped_sprite,0,30,30,1,-1,0,0xFFFFFF,1);
  if((fixture->pixels[20*SOFTWARE3D_WIDTH+18]&0x00FFFFFFu)==0 ||
     (fixture->pixels[20*SOFTWARE3D_WIDTH+19]&0x00FFFFFFu)==0 || fixture->pixels[20*SOFTWARE3D_WIDTH+20]!=0 ||
     (fixture->pixels[28*SOFTWARE3D_WIDTH+30]&0x00FFFFFFu)==0 ||
     (fixture->pixels[29*SOFTWARE3D_WIDTH+30]&0x00FFFFFFu)==0 || fixture->pixels[30*SOFTWARE3D_WIDTH+30]!=0){
    fprintf(stderr,"software modern negative sprite scale anchor mismatch\n");
    return 0;
  }
  /* A layer background is a logical cell, not a sprite instance: ignore the sprite origin and
   * repeat only along the selected axis.  This also protects against an untiled vertical copy
   * reappearing above/below the authored layer. */
  flipped->originx=flipped->originy=1;
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  gml_draw_layer_background_sprite(&fixture->render,fixture->flipped_sprite,0,0,10,1,1,0xFFFFFF,1,1,0);
  if((fixture->pixels[10*SOFTWARE3D_WIDTH]&0x00FFFFFFu)==0 ||
     (fixture->pixels[11*SOFTWARE3D_WIDTH]&0x00FFFFFFu)==0 ||
     fixture->pixels[9*SOFTWARE3D_WIDTH]!=0 || fixture->pixels[12*SOFTWARE3D_WIDTH]!=0 ||
     (fixture->pixels[10*SOFTWARE3D_WIDTH+SOFTWARE3D_WIDTH-1]&0x00FFFFFFu)==0){
    fprintf(stderr,"software layer-background origin/axis tiling mismatch\n");
    return 0;
  }
  memset(fixture->pixels,0,sizeof(fixture->pixels));
  gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
  gml_draw_layer_background_sprite(&fixture->render,fixture->flipped_sprite,0,-.5,10,1,1,0xFFFFFF,1,0,0);
  if((fixture->pixels[10*SOFTWARE3D_WIDTH]&0x00FFFFFFu)==0 ||
     (fixture->pixels[10*SOFTWARE3D_WIDTH+1]&0x00FFFFFFu)==0 || fixture->pixels[10*SOFTWARE3D_WIDTH+2]!=0){
    fprintf(stderr,"software layer-background negative fractional anchor mismatch\n");
    return 0;
  }
  {
    /* draw_sprite_tiled anchors the full logical cell at x/y; the sprite origin affects an
     * ordinary sprite draw but must not translate the tiling phase. */
    uint8_t *tile_rgba=calloc(4,4);
    if(!tile_rgba){
      fprintf(stderr,"software tiled-sprite fixture allocation failed\n");
      return 0;
    }
    for(int py=0;py<2;py++){
      tile_rgba[(py*2)*4+0]=255;
      tile_rgba[(py*2)*4+1]=255;
      tile_rgba[(py*2)*4+2]=255;
      tile_rgba[(py*2)*4+3]=255;
    }
    flipped->runtime_rgba=tile_rgba;
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    gml_draw_sprite_tiled_ext(&fixture->render,fixture->flipped_sprite,0,2,2,1,1,0xFFFFFF,1);
    flipped->runtime_rgba=NULL;
    free(tile_rgba);
    if((fixture->pixels[0]&0x00FFFFFFu)==0 || fixture->pixels[1]!=0 ||
       (fixture->pixels[2]&0x00FFFFFFu)==0 || fixture->pixels[3]!=0){
      fprintf(stderr,"software tiled-sprite origin phase mismatch: %08x %08x %08x %08x\n",
              fixture->pixels[0],fixture->pixels[1],fixture->pixels[2],fixture->pixels[3]);
      return 0;
    }
  }
  {
    /* Texture-remap shaders apply to ordinary atlas sprites/backgrounds, not only to surfaces. */
    uint8_t palette_rgba[16]={255,0,0,255, 0,255,0,255,
                              255,0,0,255, 0,255,0,255};
    if(!fixture->render.shader_pal || fixture->render.n_shader_pal<1){
      fprintf(stderr,"software texture-remap shader fixture state missing\n");
      return 0;
    }
    memset(fixture->render.shader_pal,0,sizeof(*fixture->render.shader_pal));
    fixture->render.shader_pal[0].lut=1; fixture->render.shader_pal[0].lut_row=0;
    flipped->runtime_rgba=palette_rgba;
    fixture->render.lut_pal_sprite=fixture->flipped_sprite; fixture->render.lut_pal_frame=0;
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->render.active_shader=0;
    gml_draw_background_ext(&fixture->render,0,5,5,1,1,0xFFFFFF,1);
    fixture->render.active_shader=-1; fixture->render.lut_pal_sprite=-1; flipped->runtime_rgba=NULL;
    if(fixture->pixels[5*SOFTWARE3D_WIDTH+5]!=0xFF00FF00u){
      fprintf(stderr,"software atlas texture-remap shader mismatch: %08x\n",fixture->pixels[5*SOFTWARE3D_WIDTH+5]);
      return 0;
    }
  }
  {
    uint8_t source_backup[4];
    memcpy(source_backup,fixture->render.atlas[0].px,4);
    fixture->render.atlas[0].px[0]=255; fixture->render.atlas[0].px[1]=0;
    fixture->render.atlas[0].px[2]=0; fixture->render.atlas[0].px[3]=255;
    memset(fixture->render.shader_pal,0,sizeof(*fixture->render.shader_pal));
    fixture->render.shader_pal[0].grayscale=1; fixture->render.shader_pal[0].grayscale_alpha=1;
    fixture->render.shader_pal[0].grayscale_weight[0]=.2125f;
    fixture->render.shader_pal[0].grayscale_weight[1]=.7154f;
    fixture->render.shader_pal[0].grayscale_weight[2]=.0721f;
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->render.active_shader=0;
    gml_draw_background_ext(&fixture->render,0,5,5,1,1,0xFFFFFF,1);
    fixture->render.active_shader=-1;
    memcpy(fixture->render.atlas[0].px,source_backup,4);
    if((fixture->pixels[5*SOFTWARE3D_WIDTH+5]&0x00FFFFFFu)!=0x363636u){
      fprintf(stderr,"software atlas luminance-shader mismatch: %08x\n",fixture->pixels[5*SOFTWARE3D_WIDTH+5]);
      return 0;
    }
  }
  {
    /* A recognized untextured procedural fragment must replace the primitive's flat colour and
     * vary across quantized coordinates.  Constants are deliberately generic fixture values. */
    if(!fixture->render.shader_pal || fixture->render.n_shader_pal<1){
      fprintf(stderr,"software procedural-shader fixture state missing\n");
      return 0;
    }
    memset(fixture->render.shader_pal,0,sizeof(*fixture->render.shader_pal));
    struct GmlShaderPal *shader=&fixture->render.shader_pal[0];
    shader->paint=1; shader->paint_opaque=1;
    shader->paint_resolution[0]=64; shader->paint_resolution[1]=48;
    shader->paint_pixel_factor=64; shader->paint_spin_ease=.5f;
    shader->paint_spin_amount=.1f; shader->paint_contrast=1.5f;
    shader->paint_time=2.25f;
    for(int c=0;c<4;c++){
      shader->paint_color[0][c]=c==3?1.0f:.1f;
      shader->paint_color[1][c]=c==3?1.0f:.2f;
      shader->paint_color[2][c]=c==3?1.0f:0.0f;
    }
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->render.active_shader=0;
    if(!gml_render_shader_fill_rect(&fixture->render,0,0,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT)){
      fprintf(stderr,"software procedural-shader draw was not handled\n");
      return 0;
    }
    uint32_t first=fixture->pixels[0]; int differs=0;
    for(int i=1;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) if(fixture->pixels[i]!=first){ differs=1; break; }
    if(!differs || (first>>24)!=255){
      fprintf(stderr,"software procedural-shader raster mismatch: first=%08x varies=%d\n",first,differs);
      return 0;
    }
    shader->paint_resolution_mediump=1;
    shader->paint_resolution[0]=720;
    shader->paint_resolution[1]=640;
    memset(fixture->pixels,0,sizeof(fixture->pixels));
    gml_render_begin(&fixture->render,fixture->pixels,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT,0,0);
    fixture->render.active_shader=0;
    if(!gml_render_shader_fill_rect(&fixture->render,0,0,SOFTWARE3D_WIDTH,SOFTWARE3D_HEIGHT)){
      fprintf(stderr,"software mediump procedural-shader draw was not handled\n");
      return 0;
    }
    first=fixture->pixels[0]; differs=0;
    for(int i=1;i<SOFTWARE3D_WIDTH*SOFTWARE3D_HEIGHT;i++) if(fixture->pixels[i]!=first){ differs=1; break; }
    if(differs || first!=0xFF949494u){
      fprintf(stderr,"software mediump procedural-shader overflow mismatch: first=%08x varies=%d\n",
              first,differs);
      return 0;
    }
  }
  flipped->originx=flipped->originy=0;
  return 1;
}
