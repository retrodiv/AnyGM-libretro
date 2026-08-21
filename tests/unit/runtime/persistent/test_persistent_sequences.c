/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "persistent_test_fixture.h"

#include "gml_vm_internal.h"
#include "gml_value_internal.h"
#include "gml_render.h"
#include "gml_render_internal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct SequenceFixtureWriter {
  uint8_t *data;
  size_t cursor, capacity;
} SequenceFixtureWriter;

static void sequence_fixture_u32(SequenceFixtureWriter *writer, uint32_t value){
  if(!writer || writer->cursor+4>writer->capacity) return;
  fixture_w32(writer->data,writer->cursor,value);
  writer->cursor+=4;
}

static void sequence_fixture_f32(SequenceFixtureWriter *writer, double value){
  float encoded=(float)value;
  uint32_t bits=0;
  memcpy(&bits,&encoded,sizeof bits);
  sequence_fixture_u32(writer,bits);
}

static void sequence_fixture_track_header(SequenceFixtureWriter *writer,
                                          uint32_t model, uint32_t name,
                                          uint32_t builtin, uint32_t subtracks){
  sequence_fixture_u32(writer,model);
  sequence_fixture_u32(writer,name);
  sequence_fixture_u32(writer,builtin);
  sequence_fixture_u32(writer,0); /* traits */
  sequence_fixture_u32(writer,0); /* creation track */
  sequence_fixture_u32(writer,0); /* tags */
  sequence_fixture_u32(writer,0); /* owned resources */
  sequence_fixture_u32(writer,subtracks);
}

static void sequence_fixture_key_header(SequenceFixtureWriter *writer,
                                        double frame, double length,
                                        uint32_t disabled, uint32_t channels){
  sequence_fixture_f32(writer,frame);
  sequence_fixture_f32(writer,length);
  sequence_fixture_u32(writer,0); /* stretch */
  sequence_fixture_u32(writer,disabled);
  sequence_fixture_u32(writer,channels);
}

static void sequence_fixture_real_channel(SequenceFixtureWriter *writer,
                                          uint32_t channel, double value){
  sequence_fixture_u32(writer,channel);
  sequence_fixture_f32(writer,value);
  sequence_fixture_u32(writer,0);          /* no embedded curve */
  sequence_fixture_u32(writer,UINT32_MAX); /* no curve asset */
}

static void sequence_fixture_colour_channel(SequenceFixtureWriter *writer,
                                            uint32_t argb){
  sequence_fixture_u32(writer,0);
  sequence_fixture_u32(writer,argb);
  sequence_fixture_u32(writer,0);
  sequence_fixture_u32(writer,UINT32_MAX);
}

static void sequence_fixture_asset_key(SequenceFixtureWriter *writer,
                                       double frame, double length,
                                       uint32_t disabled, int sprite){
  sequence_fixture_key_header(writer,frame,length,disabled,1);
  sequence_fixture_u32(writer,0);
  sequence_fixture_u32(writer,(uint32_t)sprite);
}

static uint32_t sequence_fixture_string(uint8_t *data, size_t *cursor,
                                        const char *value){
  size_t length=strlen(value)+1;
  uint32_t offset=(uint32_t)*cursor;
  memcpy(data+*cursor,value,length);
  *cursor+=length;
  return offset;
}

int expect_sequence_asset_keys_and_parameters(void){
  uint8_t data[2048]={0};
  size_t string_cursor=1536;
  enum {
    STR_SEQUENCE,
    STR_GRAPHIC_MODEL,
    STR_FRONT,
    STR_REAL_MODEL,
    STR_POSITION,
    STR_COLOUR_MODEL,
    STR_BLEND,
    STR_BACK,
    STR_COUNT
  };
  static const char *const string_values[STR_COUNT]={
    "neutral_sequence","GMGraphicTrack","front_track","GMRealTrack",
    "position","GMColourTrack","blend_multiply","back_track"
  };
  uint32_t string_offsets[STR_COUNT]={0};
  char *strings[STR_COUNT]={0};
  for(int i=0;i<STR_COUNT;i++){
    string_offsets[i]=sequence_fixture_string(data,&string_cursor,string_values[i]);
    strings[i]=(char*)data+string_offsets[i];
  }

  const size_t chunk_offset=64;
  const size_t sequence_offset=80;
  SequenceFixtureWriter writer={data,chunk_offset,sizeof data};
  sequence_fixture_u32(&writer,1); /* SEQN version */
  sequence_fixture_u32(&writer,1); /* sequence count */
  sequence_fixture_u32(&writer,(uint32_t)sequence_offset);
  writer.cursor=sequence_offset;
  sequence_fixture_u32(&writer,string_offsets[STR_SEQUENCE]);
  sequence_fixture_u32(&writer,0); /* one-shot */
  sequence_fixture_f32(&writer,60);
  sequence_fixture_u32(&writer,0); /* frames per second */
  sequence_fixture_f32(&writer,100);
  sequence_fixture_u32(&writer,5); /* sequence origin x */
  sequence_fixture_u32(&writer,7); /* sequence origin y */
  sequence_fixture_f32(&writer,1); /* volume */
  sequence_fixture_u32(&writer,0); /* broadcast keys */
  sequence_fixture_u32(&writer,2); /* top-level tracks */

  sequence_fixture_track_header(&writer,string_offsets[STR_GRAPHIC_MODEL],
                                 string_offsets[STR_FRONT],0,2);
  sequence_fixture_track_header(&writer,string_offsets[STR_REAL_MODEL],
                                 string_offsets[STR_FRONT],14,0);
  sequence_fixture_u32(&writer,1); /* linear interpolation */
  sequence_fixture_u32(&writer,2);
  sequence_fixture_key_header(&writer,0,1,0,2);
  sequence_fixture_real_channel(&writer,0,10);
  sequence_fixture_real_channel(&writer,1,20);
  sequence_fixture_key_header(&writer,10,1,0,2);
  sequence_fixture_real_channel(&writer,0,30);
  sequence_fixture_real_channel(&writer,1,40);

  sequence_fixture_track_header(&writer,string_offsets[STR_COLOUR_MODEL],
                                 string_offsets[STR_FRONT],10,0);
  sequence_fixture_u32(&writer,1);
  sequence_fixture_u32(&writer,2);
  sequence_fixture_key_header(&writer,0,1,0,1);
  sequence_fixture_colour_channel(&writer,0x00FF0000u);
  sequence_fixture_key_header(&writer,10,1,0,1);
  sequence_fixture_colour_channel(&writer,0xFF0000FFu);

  size_t asset_count_offset=writer.cursor;
  sequence_fixture_u32(&writer,2);
  sequence_fixture_asset_key(&writer,2,4,0,0);
  sequence_fixture_asset_key(&writer,20,3,1,2);

  sequence_fixture_track_header(&writer,string_offsets[STR_GRAPHIC_MODEL],
                                 string_offsets[STR_BACK],0,0);
  sequence_fixture_u32(&writer,1);
  sequence_fixture_asset_key(&writer,0,100,0,1);
  size_t sequence_end=writer.cursor;

  const size_t room_chunk_offset=1024;
  const size_t room_record_offset=1032;
  fixture_w32(data,room_chunk_offset,1);
  fixture_w32(data,room_chunk_offset+4,(uint32_t)room_record_offset);
  fixture_w32(data,room_record_offset,string_offsets[STR_SEQUENCE]);
  fixture_w32(data,room_record_offset+4,string_offsets[STR_SEQUENCE]);
  fixture_w32(data,room_record_offset+8,32);
  fixture_w32(data,room_record_offset+12,32);
  fixture_w32(data,room_record_offset+16,60);

  GmlWin win={0};
  win.data=data;
  win.size=sizeof data;
  win.n_chunks=2;
  memcpy(win.chunks[0].name,"SEQN",5);
  win.chunks[0].off=(uint32_t)chunk_offset;
  win.chunks[0].size=(uint32_t)(sequence_end-chunk_offset);
  memcpy(win.chunks[1].name,"ROOM",5);
  win.chunks[1].off=(uint32_t)room_chunk_offset;
  win.chunks[1].size=64;
  win.strs=strings;
  win.str_charoff=string_offsets;
  win.n_strs=STR_COUNT;

  GmlVM vm={0};
  vm.win=&win;
  gml_vm_rooms_init(&vm);
  int ok=vm.n_sequences==1 && vm.sequences && vm.sequences[0].n_graphics==2 &&
    vm.sequences[0].origin_x==5 && vm.sequences[0].origin_y==7;
  if(ok){
    GmlSeqGraphic *front=&vm.sequences[0].graphics[0];
    GmlSeqGraphic *back=&vm.sequences[0].graphics[1];
    double key_head=-1;
    ok=front->n_keys==2 && front->n_tracks==2 && back->n_keys==1 &&
      gml_sequence_sprite_at(front,1.999,NULL)==-1 &&
      gml_sequence_sprite_at(front,2,&key_head)==0 && key_head==2 &&
      gml_sequence_sprite_at(front,5.999,NULL)==0 &&
      gml_sequence_sprite_at(front,6,NULL)==-1 &&
      gml_sequence_sprite_at(front,20,NULL)==-1 &&
      gml_sequence_sprite_at(back,0,NULL)==1 &&
      fabs(gml_sequence_value(front,"position",0,0,-1)-10)<1e-12 &&
      fabs(gml_sequence_value(front,"position",0,5,-1)-20)<1e-12 &&
      fabs(gml_sequence_value(front,"position",1,5,-1)-30)<1e-12 &&
      fabs(gml_sequence_value(front,"position",0,10,-1)-30)<1e-12 &&
      gml_sequence_colour(front,"blend_multiply",0,0)==0x00FF0000u &&
      gml_sequence_colour(front,"blend_multiply",5,0)==0x80800080u &&
      gml_sequence_colour(front,"blend_multiply",10,0)==0xFF0000FFu;
    if(ok){
      front->tracks[0].interpolation=0;
      ok=gml_sequence_value(front,"position",0,5,-1)==10;
    }
    if(ok){
      GmlRender render={0};
      GmlSprite sprites[2]={{0}};
      GmlTpag pages[2]={{0}};
      GmlAtlas atlas={0};
      uint8_t pixels[8]={255,255,255,255,0,255,0,255};
      int frames[2]={0,1};
      uint32_t framebuffer[32*32]={0};
      GmlRtLayer layer={.id=1,.used=1,.visible=1,.order=0,.script_begin=-1,.script_end=-1};
      GmlRtElem element={.id=2,.used=1,.layer=1,.type=9,.sprite=0,
                        .x=3,.y=4,.xs=1,.ys=1,.alpha=1,.visible=1,.blend=0xFFFFFFu,
                        .image_index=2};
      render.spr=sprites; render.n_spr=2;
      render.tpag=pages; render.n_tpag=2;
      render.atlas=&atlas; render.n_atlas=1;
      render.alpha=1; render.color_write_mask=0x0F;
      atlas.px=pixels; atlas.w=2; atlas.h=1; atlas.decode_attempted=1;
      for(int i=0;i<2;i++){
        sprites[i].w=sprites[i].h=sprites[i].n_frames=1;
        sprites[i].frame=&frames[i];
        pages[i].sx=i; pages[i].sw=pages[i].sh=1;
        pages[i].bw=pages[i].bh=1; pages[i].atlas=0;
      }
      vm.render=&render; vm.room_index=0; vm.rtl=&layer; vm.n_rtl=1;
      vm.rte=&element; vm.n_rte=1;
      front->tracks[0].interpolation=1;
      front->tracks[1].interpolation=0;
      front->tracks[1].keys[0].colour=0xFFFF0000u;
      gml_render_begin(&render,framebuffer,32,32,0,0);
      gml_vm_draw(&vm);
      ok=framebuffer[21*32+12]==UINT32_C(0xFFFF0000);
      if(!ok)
        fprintf(stderr,"sequence position/origin draw mismatch: pixel=%08x\n",
                framebuffer[21*32+12]);

      for(int key=0;key<front->tracks[0].n_keys;key++)
        front->tracks[0].keys[key].value[0]=front->tracks[0].keys[key].value[1]=0;
      vm.sequences[0].origin_x=vm.sequences[0].origin_y=0;
      memset(framebuffer,0,sizeof framebuffer);
      gml_render_begin(&render,framebuffer,32,32,0,0);
      gml_vm_draw(&vm);
      if(framebuffer[4*32+3]!=UINT32_C(0xFFFF0000)){
        fprintf(stderr,"sequence front-to-back draw mismatch: pixel=%08x\n",
                framebuffer[4*32+3]);
        ok=0;
      }

      front->tracks[1].keys[0].colour=0x00FF0000u;
      memset(framebuffer,0,sizeof framebuffer);
      gml_render_begin(&render,framebuffer,32,32,0,0);
      gml_vm_draw(&vm);
      if(framebuffer[4*32+3]!=UINT32_C(0xFF00FF00)){
        fprintf(stderr,"sequence colour alpha draw mismatch: pixel=%08x\n",
                framebuffer[4*32+3]);
        ok=0;
      }
      gml_vm_frame_cleanup(&vm);
      gml_varmap_free(&vm.globals);
      vm.render=NULL; vm.rtl=NULL; vm.n_rtl=0; vm.rte=NULL; vm.n_rte=0;
    }
  }
  if(!ok) fputs("sequence asset-key or parameter evaluation mismatch\n",stderr);
  gml_vm_sequences_clear(&vm);
  if(vm.sequences || vm.n_sequences) ok=0;

  fixture_w32(data,asset_count_offset,4097);
  gml_vm_rooms_init(&vm);
  if(vm.n_sequences!=1 || !vm.sequences || vm.sequences[0].n_graphics!=0){
    fputs("oversized sequence asset-key store was not rejected\n",stderr);
    ok=0;
  }
  gml_vm_sequences_clear(&vm);

  fixture_w32(data,asset_count_offset,2);
  win.chunks[0].size=(uint32_t)(sequence_end-chunk_offset-1);
  gml_vm_rooms_init(&vm);
  if(vm.n_sequences!=1 || !vm.sequences || vm.sequences[0].n_graphics!=0){
    fputs("truncated sequence track was not rejected\n",stderr);
    ok=0;
  }
  gml_vm_sequences_clear(&vm);
  return ok;
}
