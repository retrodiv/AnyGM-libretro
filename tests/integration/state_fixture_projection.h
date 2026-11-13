/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Test scratch only: expand the current neutral fixture's value/text encodings to its preceding
 * layout. No state is loaded through this helper. Unsupported fixture features fail closed, and
 * the caller retains every historical size/hash pin as well as current-format roundtrip checks. */
#include "gml_value_internal.h"

typedef struct FixtureProjection {
  const uint8_t *input;
  size_t size,pos,used;
  uint8_t output[65536];
  struct { const uint8_t *text; uint32_t length; } strings[256];
  struct { unsigned count; struct { size_t offset,length; } keys[256]; } shapes[64];
  unsigned string_count,array_count,shape_count;
  int ok;
} FixtureProjection;
static void fixture_emit(FixtureProjection *p,const void *data,size_t size){
  if(!p->ok || size>sizeof p->output-p->used){p->ok=0;return;}
  memcpy(p->output+p->used,data,size);p->used+=size;
}
static void fixture_word(FixtureProjection *p,uint32_t value){
  uint8_t bytes[4];write_u32(bytes,value);fixture_emit(p,bytes,4);
}
static unsigned fixture_byte(FixtureProjection *p){
  if(!p->ok || p->pos>=p->size){p->ok=0;return 0;}
  return p->input[p->pos++];
}
static uint32_t fixture_read_word(FixtureProjection *p){
  uint32_t value=0;
  for(unsigned i=0;i<4;i++)value|=(uint32_t)fixture_byte(p)<<(8*i);
  return value;
}
static uint32_t fixture_uint(FixtureProjection *p){
  uint32_t value=0;
  for(unsigned i=0;i<5 && p->ok;i++){
    unsigned byte=fixture_byte(p);
    if((i==4 && (byte&0xf0)) || (i && !byte)){p->ok=0;return 0;}
    value|=(byte&127u)<<(7*i);
    if(!(byte&128))return value;
  }
  p->ok=0;return 0;
}
static void fixture_copy(FixtureProjection *p,size_t size){
  if(!p->ok || p->pos>p->size || size>p->size-p->pos){p->ok=0;return;}
  fixture_emit(p,p->input+p->pos,size);p->pos+=size;
}
static uint32_t fixture_copy_word(FixtureProjection *p){
  uint32_t value=fixture_read_word(p);fixture_word(p,value);return value;
}
static void fixture_empty(FixtureProjection *p){
  if(fixture_copy_word(p)!=0)p->ok=0;
}
static unsigned fixture_bits(uint32_t value){
  unsigned count=0;while(value){count+=value&1u;value>>=1;}return count;
}
static void fixture_string(FixtureProjection *p){
  unsigned kind=fixture_byte(p);uint32_t code=fixture_uint(p);
  if(!p->ok)return;
  if(kind==1){fixture_word(p,UINT32_C(0x80000000)|code);return;}
  const uint8_t *text=NULL;uint32_t length=0;
  if(!kind){
    if(code>p->size-p->pos || p->string_count>=256){p->ok=0;return;}
    text=p->input+p->pos;length=code;p->pos+=code;
    p->strings[p->string_count].text=text;
    p->strings[p->string_count++].length=length;
  }else if(kind==2 && code<p->string_count){
    text=p->strings[code].text;length=p->strings[code].length;
  }else{p->ok=0;return;}
  fixture_word(p,length);fixture_emit(p,text,length);
}
/* Returns true only for the default positive-real zero which the preceding sparse form elides. */
static int fixture_value(FixtureProjection *p,unsigned depth){
  if(depth>64){p->ok=0;return 0;}
  unsigned tag=fixture_byte(p);
  if(tag==0 || tag==6 || tag==7 || tag==8 || tag==9){
    uint64_t bits=0;double value=0;
    if(tag==0){
      for(unsigned i=0;i<8;i++)bits|=(uint64_t)fixture_byte(p)<<(8*i);
    }else if(tag!=6){
      int32_t integer=0;
      if(tag==7)integer=(int32_t)fixture_read_word(p);
      if(tag==8)integer=(int8_t)fixture_byte(p);
      if(tag==9){unsigned low=fixture_byte(p),high=fixture_byte(p);integer=(int16_t)(low|(high<<8));}
      value=integer;memcpy(&bits,&value,8);
    }
    fixture_word(p,0);uint8_t bytes[8];write_u64(bytes,bits);fixture_emit(p,bytes,8);
    return !bits;
  }
  if(tag==1){fixture_word(p,1);fixture_string(p);return 0;}
  if(tag==3){fixture_word(p,3);return 0;}
  if(tag==4){
    uint32_t id=fixture_read_word(p);if(id>p->array_count)p->ok=0;
    fixture_word(p,4);fixture_word(p,id);return 0;
  }
  if(tag!=2 || ++p->array_count>256){p->ok=0;return 0;}
  uint32_t raw=fixture_read_word(p),length=raw&UINT32_C(0x7fffffff);
  if(length>4096){p->ok=0;return 0;}
  fixture_word(p,2);fixture_word(p,length|UINT32_C(0x80000000));
  uint32_t height=fixture_copy_word(p);
  if(height>128){p->ok=0;return 0;}
  fixture_copy(p,(size_t)height*4);fixture_copy(p,4); /* nested-array flag */
  size_t count_at=p->used;fixture_word(p,0);uint32_t populated=0;
  uint32_t count=(raw&UINT32_C(0x80000000))?fixture_read_word(p):length;
  if(count>length){p->ok=0;return 0;}
  for(uint32_t i=0;i<count && p->ok;i++){
    uint32_t index=(raw&UINT32_C(0x80000000))?fixture_read_word(p):i;
    if(index>=length){p->ok=0;break;}
    size_t start=p->used;fixture_word(p,index);
    if(fixture_value(p,depth+1))p->used=start;
    else populated++;
  }
  if(p->ok)write_u32(p->output+count_at,populated);
  return 0;
}
static void fixture_map(FixtureProjection *p){
  uint32_t raw=fixture_read_word(p),count=raw,id=0;
  /* Full-map references remain unsupported in this fixed fixture: array definitions cannot
   * simply be copied a second time. Shapes replay only key bytes, then decode current values. */
  if(raw&UINT32_C(0x80000000)){p->ok=0;return;}
  int shared=(raw&UINT32_C(0x40000000))!=0;
  if(shared){
    id=raw&UINT32_C(0x3fffffff);
    if(id>=p->shape_count){p->ok=0;return;}
    count=p->shapes[id].count;
  }else if(count){
    if(p->shape_count>=64){p->ok=0;return;}
    id=p->shape_count++;p->shapes[id].count=count;
  }
  if(count>256){p->ok=0;return;}
  fixture_word(p,count);
  for(uint32_t i=0;i<count && p->ok;i++){
    if(shared){
      size_t at=p->shapes[id].keys[i].offset,length=p->shapes[id].keys[i].length;
      fixture_emit(p,p->output+at,length);
    }else{
      size_t start=p->used;fixture_string(p);
      p->shapes[id].keys[i].offset=start;p->shapes[id].keys[i].length=p->used-start;
    }
    (void)fixture_value(p,0);
  }
}
/* Only the current neutral fixture is projected, with each omitted sprite field checked
 * against the live fixture. Direct state controls cover mutation/restoration of these fields. */
static int project_fixture_playback(AnygmEngine *engine,uint8_t **data,size_t *written){
  uint8_t *bytes=*data;
  if(*written<112 || (uint32_t)read_u64(bytes+4)!=70 || engine->render.n_spr<0 ||
     gml_builtin_account_picker_pending_count(&engine->vm))return 0;
  /* Preserve the preceding fixture's bytes only after proving that the new queue is empty.
   * This scratch-only normalization never supplies an older layout to the runtime reader. */
  size_t vm_start=112+(size_t)read_u64(bytes+64)+(size_t)read_u64(bytes+72);
  size_t vm_size=(size_t)read_u64(bytes+80);
  if(vm_start>*written || vm_size>*written-vm_start || vm_size<12 ||
     (uint32_t)read_u64(bytes+vm_start+4)!=45 ||
     memcmp(bytes+vm_start+vm_size-4,(const uint8_t[4]){0},4))return 0;
  size_t vm_end=vm_start+vm_size;
  memmove(bytes+vm_end-4,bytes+vm_end,*written-vm_end);
  *written-=4;
  write_u32(bytes+4,69);write_u32(bytes+vm_start+4,44);
  write_u64(bytes+16,*written);write_u64(bytes+80,vm_size-4);
  write_u64(bytes+96,*written-112);write_u64(bytes+56,state_checksum(bytes+112,*written-112));
  /* The historical fixture contains no dynamic sprite information or nine-slice.
   * Validate each current metadata word before omitting this new trailer. */
  size_t metadata_start=112+(size_t)read_u64(bytes+64);
  size_t metadata_size=(size_t)read_u64(bytes+72);
  size_t metadata_count=(size_t)engine->render.n_spr;
  if(metadata_count>(SIZE_MAX-4u)/44u)return 0;
  size_t metadata_bytes=4u+44u*metadata_count;
  if(metadata_start>*written || metadata_size>*written-metadata_start ||
     metadata_bytes>metadata_size)return 0;
  size_t metadata_at=metadata_start+metadata_size-metadata_bytes;
  if((uint32_t)read_u64(bytes+metadata_at)!=(uint32_t)metadata_count)return 0;
  for(size_t i=0;i<metadata_count;i++){
    const GmlSprite *sprite=&engine->render.spr[i];
    if(sprite->metadata_flags || sprite->ns_enabled || sprite->ns_l || sprite->ns_t ||
       sprite->ns_r || sprite->ns_b)return 0;
    for(int mode=0;mode<5;mode++)if(sprite->ns_tile[mode])return 0;
    if(memcmp(bytes+metadata_at+4u+44u*i,(const uint8_t[44]){0},44))return 0;
  }
  size_t metadata_end=metadata_start+metadata_size;
  memmove(bytes+metadata_at,bytes+metadata_end,*written-metadata_end);
  *written-=metadata_bytes;
  write_u64(bytes+72,metadata_size-metadata_bytes);
  /* Only a neutral current blend may disappear from this historical scratch pin. */
  size_t render=112+(size_t)read_u64(bytes+64),render_size=(size_t)read_u64(bytes+72);
  const uint8_t neutral_blend[12]={0,0,0,0,1,0,0,0,1,0,0,0};
  if(render>*written || render_size>*written-render || render_size<sizeof neutral_blend ||
     memcmp(bytes+render+render_size-sizeof neutral_blend,neutral_blend,sizeof neutral_blend))return 0;
  size_t blend_end=render+render_size;
  memmove(bytes+blend_end-sizeof neutral_blend,bytes+blend_end,*written-blend_end);
  *written-=sizeof neutral_blend;
  write_u64(bytes+72,render_size-sizeof neutral_blend);
  /* Only neutral audio is projected: the new inactive-voice pitch cells and default
   * music trailer are checked before removing them from scratch, never from runtime. */
  size_t audio=112;
  for(unsigned offset=64;offset<=80;offset+=8){
    uint64_t part=read_u64(bytes+offset);
    if(part>*written-audio)return 0;
    audio+=(size_t)part;
  }
  size_t audio_size=(size_t)read_u64(bytes+88);
  uint8_t music[24]={0};
  write_u32(music,UINT32_MAX);
  write_u64(music+4,UINT64_C(0x3ff0000000000000));
  write_u64(music+12,UINT64_C(0x3ff0000000000000));
  if(audio_size!=*written-audio || audio_size<32 ||
     (uint32_t)read_u64(bytes+audio)!=UINT32_C(0x53554141) ||
     (uint32_t)read_u64(bytes+audio+4)!=6 ||
     memcmp(bytes+*written-sizeof music,music,sizeof music))return 0;
  if(audio_size<28u+32u*80u+sizeof music)return 0;
  for(unsigned i=0;i<32u;i++){
    size_t voice=audio+28u+(size_t)i*80u;
    if((uint32_t)read_u64(bytes+voice+8u)!=0 || read_u64(bytes+voice+72u)!=0)return 0;
  }
  size_t source=audio+28u,target=source;
  for(unsigned i=0;i<32u;i++){
    memmove(bytes+target,bytes+source,72u);source+=80u;target+=72u;
  }
  memmove(bytes+target,bytes+source,*written-source);
  *written-=32u*8u;audio_size-=32u*8u;
  *written-=sizeof music;
  write_u32(bytes+audio+4,3);write_u64(bytes+88,audio_size-sizeof music);
  write_u64(bytes+16,*written);write_u64(bytes+96,*written-112);
  write_u64(bytes+56,state_checksum(bytes+112,*written-112));
  size_t start=112+(size_t)read_u64(bytes+64),size=(size_t)read_u64(bytes+72);
  size_t count=(size_t)engine->render.n_spr;
  size_t trailer=4;
  for(size_t i=0;i<count;i++){
    size_t row=engine->render.spr[i].runtime_rgba?16:32;
    if(row>SIZE_MAX-trailer)return 0;
    trailer+=row;
  }
  if(start>*written || size>*written-start || trailer>size)return 0;
  size_t at=start+size-trailer;
  if((uint32_t)read_u64(bytes+at)!=(uint32_t)count)return 0;
  size_t row_at=at+4;
  for(size_t i=0;i<count;i++){
    GmlSprite *sprite=&engine->render.spr[i];uint32_t bits=0;
    memcpy(&bits,&sprite->playback_speed,sizeof bits);
    if((uint32_t)read_u64(bytes+row_at)!=bits ||
       (uint32_t)read_u64(bytes+row_at+4)!=(uint32_t)sprite->playback_speed_type ||
       (uint32_t)read_u64(bytes+row_at+8)!=(uint32_t)sprite->playback_speed_valid ||
       (uint32_t)read_u64(bytes+row_at+12)!=(uint32_t)sprite->bbox_mode)return 0;
    row_at+=16;
    if(!sprite->runtime_rgba){
      if((uint32_t)read_u64(bytes+row_at)!=(uint32_t)sprite->ml ||
         (uint32_t)read_u64(bytes+row_at+4)!=(uint32_t)sprite->mt ||
         (uint32_t)read_u64(bytes+row_at+8)!=(uint32_t)sprite->mr ||
         (uint32_t)read_u64(bytes+row_at+12)!=(uint32_t)sprite->mb)return 0;
      row_at+=16;
    }
  }
  if(row_at!=at+trailer)return 0;
  uint8_t *result=malloc(*written-trailer);
  if(!result)return 0;
  memcpy(result,bytes,at);memcpy(result+at,bytes+at+trailer,*written-at-trailer);
  *written-=trailer;
  write_u32(result+4,43);write_u64(result+16,*written);write_u64(result+72,size-trailer);
  write_u64(result+96,*written-112);write_u64(result+56,state_checksum(result+112,*written-112));
  free(*data);*data=result;return 1;
}
static int project_fixture_vm(AnygmEngine *engine,uint8_t **data,size_t *written){
  if(!project_fixture_playback(engine,data,written))return 0;
  const uint8_t *bytes=*data;
  if(*written<112 || (uint32_t)read_u64(bytes+4)!=43 || engine->vm.n_structs ||
     engine->vm.n_tilemaps || engine->vm.n_rtl || engine->vm.n_rte ||
     engine->vm.room_visuals || engine->vm.wait.active || engine->vm.n_timelines ||
     engine->vm.next_array_owner || engine->vm.member_names.len ||
     engine->vm.n_objects<0)return 0;
  size_t start=112+(size_t)read_u64(bytes+64)+(size_t)read_u64(bytes+72);
  size_t size=(size_t)read_u64(bytes+80);
  if(start>*written || size>*written-start || size<132)return 0;
  size_t wire_size=size;
  /* Only absent media and async wait ownership are neutral in this historical pin. */
  if(engine->vm.wait.async_map || size<8 ||
     memcmp(bytes+start+size-8,(const uint8_t[8]){0},8))return 0;
  size-=8;
  /* Only an absent parked payload is neutral in this historical pin. */
  if(engine->vm.wait.broadcast_map || size<4 ||
     memcmp(bytes+start+size-4,(const uint8_t[4]){0},4))return 0;
  size-=4;
  /* Only an absent pending file request is neutral in this historical pin. */
  if(engine->vm.pending_game_load || size<4 ||
     memcmp(bytes+start+size-4,(const uint8_t[4]){0},4))return 0;
  size-=4;
  FixtureProjection *p=calloc(1,sizeof(*p));if(!p)return 0;
  /* This bounded fixture has no recycled/relinked object groups. Strip only the verified
   * empty order trailer; nonempty new behavior belongs to its direct continuation controls. */
  /* Only the neutral ownership trailer may be omitted from this old payload pin. */
  if(size<29 || bytes[start+size-1]!=0 ||
     memcmp(bytes+start+size-21,(const uint8_t[20]){0},20)){free(p);return 0;}
  if(memcmp(bytes+start+size-25,(const uint8_t[4]){0},4)){free(p);return 0;}
  if(memcmp(bytes+start+size-29,(const uint8_t[4]){0},4)){free(p);return 0;}
  p->input=bytes+start;p->size=size-29;p->ok=1;
  if((size_t)engine->vm.n_objects>(SIZE_MAX-4u)/28u){free(p);return 0;}
  size_t objects=4u+(size_t)engine->vm.n_objects*28u;
  if(objects>p->size || p->size-objects<4u){free(p);return 0;}
  size_t timeline_at=p->size-objects-4u;
  if((uint32_t)read_u64(p->input+timeline_at)!=0 ||
     (uint32_t)read_u64(p->input+timeline_at+4)!=(uint32_t)engine->vm.n_objects){free(p);return 0;}
  fixture_copy(p,132);
  if((uint32_t)read_u64(p->output+4)!=44)p->ok=0;
  write_u32(p->output+4,19);
  for(unsigned i=0;i<16;i++)(void)fixture_value(p,0);
  fixture_copy(p,76+26*4+616*8+10*4); /* RNG and unchanged software-3D scalars */
  /* Empty model/vertex resources have an explicit current header and four
   * empty vertex-builder/format/buffer words. Preserve the older model header. */
  const uint8_t empty_resources[28]={'A','D','M','S',2};
  uint32_t models=fixture_read_word(p);
  if(models!=sizeof empty_resources || p->pos>p->size || models>p->size-p->pos ||
     memcmp(p->input+p->pos,empty_resources,sizeof empty_resources))p->ok=0;
  fixture_word(p,12);fixture_word(p,UINT32_C(0x534d4441));fixture_word(p,1);fixture_word(p,0);
  if(p->ok)p->pos+=models;
  uint32_t rooms=fixture_copy_word(p);fixture_copy(p,rooms);
  fixture_copy(p,4+sizeof engine->vm.tile_mut);
  fixture_copy(p,4+sizeof engine->vm.tile_del_at);
  fixture_empty(p);fixture_copy(p,4); /* empty tilemap table and next identity */
  fixture_copy(p,8);fixture_empty(p); /* I/O cursors, no live buffers */
  fixture_empty(p);fixture_empty(p);fixture_string(p); /* no INI entries/open file */
  fixture_copy(p,4);fixture_empty(p);fixture_empty(p);fixture_empty(p); /* DS pools */
  fixture_map(p);
  for(int i=0;i<engine->vm.inst_count && p->ok;i++){
    fixture_copy(p,21);uint32_t fields=fixture_copy_word(p);
    if(fields&UINT32_C(0xffc00000)){p->ok=0;break;}
    fixture_copy(p,48+fixture_bits(fields&UINT32_C(0xfffff))*8);
    unsigned low=fixture_byte(p),high=fixture_byte(p);uint8_t alarms[2]={(uint8_t)low,(uint8_t)high};
    fixture_emit(p,alarms,2);fixture_copy(p,fixture_bits(low|(high<<8))*8);
    if(fields&(1u<<20))fixture_copy(p,11*8+1);
    if(fields&(1u<<21))fixture_copy(p,5*8);
    fixture_copy(p,4);fixture_map(p);
  }
  fixture_empty(p);fixture_empty(p); /* no structures or reclaimed slots */
  fixture_copy(p,20);fixture_empty(p);fixture_empty(p);fixture_empty(p); /* XY, allocator, visuals */
  fixture_copy(p,40); /* unchanged physics scalars */
  /* Omit only the verified default particle configuration from the old pin. */
  const uint8_t neutral_physics_particles[36]={
    0,0,0,0,0,0,0xf0,0x3f, 0,0,0,0,0,0,0xf0,0x3f,
    0,0,0,0,0,0,0xf0,0x3f, 0,0,0,0,0,0,0xf0,0x3f, 0,0,0,0
  };
  if(p->pos>p->size || sizeof neutral_physics_particles>p->size-p->pos ||
     memcmp(p->input+p->pos,neutral_physics_particles,sizeof neutral_physics_particles))p->ok=0;
  if(p->ok)p->pos+=sizeof neutral_physics_particles;
  /* Only verified empty particle, bond, group, identity and pending-definition words
   * are absent from the historical wire. Nonempty state is never projected away. */
  for(int i=0;i<5;i++) if(fixture_read_word(p)!=0)p->ok=0;
  fixture_empty(p);fixture_empty(p); /* empty fixtures/joints */
  fixture_copy(p,4);uint32_t particles=fixture_copy_word(p);fixture_copy(p,particles);
  fixture_copy(p,sizeof engine->vm.key_map);
  /* New emitter pitch/velocity cells may be omitted only for the verified inactive fixture. */
  const uint8_t neutral_emitter[32]={0};
  if(p->pos>p->size || sizeof neutral_emitter>p->size-p->pos ||
     memcmp(p->input+p->pos,neutral_emitter,sizeof neutral_emitter))p->ok=0;
  fixture_copy(p,32);
  for(unsigned i=0;i<32u && p->ok;i++){
    fixture_copy(p,8); /* gain */
    if(p->pos>p->size || sizeof neutral_emitter>p->size-p->pos ||
       memcmp(p->input+p->pos,neutral_emitter,sizeof neutral_emitter))p->ok=0;
    if(p->ok)p->pos+=sizeof neutral_emitter;
    fixture_copy(p,6*8); /* unchanged position and falloff */
  }
  fixture_copy(p,9*8+4); /* listener and falloff model */
  fixture_empty(p);fixture_empty(p);fixture_string(p);fixture_empty(p); /* external audio */
  fixture_empty(p); /* no live script statics */
  fixture_copy(p,8);fixture_empty(p); /* no scheduled language callbacks */
  /* The verified empty timeline count precedes the unchanged object-default
   * table. A populated timeline must never be silently removed here. */
  const size_t thresholds=16u*8u;
  if(timeline_at<thresholds || p->pos>timeline_at-thresholds)p->ok=0;
  if(p->ok){
    fixture_copy(p,timeline_at-thresholds-p->pos);
    for(unsigned i=0;i<16u && p->ok;i++){
      if(read_u64(p->input+p->pos)!=UINT64_C(0x3fe0000000000000))p->ok=0;
      p->pos+=8;
    }
    p->pos+=4;fixture_copy(p,p->size-p->pos);
  }
  if(!p->ok){fprintf(stderr,"fixture projection stopped at VM byte %zu/%zu\n",p->pos,size);free(p);return 0;}
  size_t total=*written-wire_size+p->used;
  uint8_t *result=malloc(total);
  if(!result){free(p);return 0;}
  memcpy(result,bytes,start);memcpy(result+start,p->output,p->used);
  memcpy(result+start+p->used,bytes+start+wire_size,*written-start-wire_size);
  write_u32(result+4,37);write_u64(result+16,total);write_u64(result+80,p->used);
  write_u64(result+96,total-112);write_u64(result+56,state_checksum(result+112,total-112));
  free(p);free(*data);*data=result;*written=total;return 1;
}

/* Controlled live-VM probes independently check the changed leaf encodings. The historical
 * whole-state pins below remain authoritative; these probes do not generate replacements. */
static int fixture_projection_controls(AnygmEngine *engine,const uint8_t *baseline,size_t size){
  GmlVal original=engine->vm.script_args[0];
  const double numbers[]={0.0,-0.0,-128,128,32768,0.125};
  int ok=1;
  for(unsigned i=0;i<sizeof numbers/sizeof numbers[0] && ok;i++){
    uint8_t *bytes=NULL;size_t count=0;
    engine->vm.script_args[0]=vreal(numbers[i]);
    ok=save_state(engine,&bytes,&count) && project_fixture_vm(engine,&bytes,&count);
    if(ok){
      size_t at=112+(size_t)read_u64(bytes+64)+(size_t)read_u64(bytes+72)+132;
      uint64_t bits=0;memcpy(&bits,&numbers[i],8);
      ok=at+12<=count && (uint32_t)read_u64(bytes+at)==0 && read_u64(bytes+at+4)==bits;
    }
    free(bytes);
  }
  const char text[]="transient projection probe";
  if(gml_win_intern_lookup(&engine->win,text))ok=0;
  if(ok){
    uint8_t *bytes=NULL;size_t count=0;engine->vm.script_args[0]=vstr(text);
    ok=save_state(engine,&bytes,&count) && project_fixture_vm(engine,&bytes,&count);
    if(ok){
      size_t at=112+(size_t)read_u64(bytes+64)+(size_t)read_u64(bytes+72)+132;
      ok=at+8+sizeof text-1<=count && (uint32_t)read_u64(bytes+at)==1 &&
        (uint32_t)read_u64(bytes+at+4)==sizeof text-1 &&
        !memcmp(bytes+at+8,text,sizeof text-1);
    }
    free(bytes);
  }
  const int lengths[]={2,128};
  for(unsigned i=0;i<2 && ok;i++){
    GmlVal array=gml_arr_new(lengths[i],vreal(0));
    gml_arr_set(array,lengths[i]-1,vreal(99));engine->vm.script_args[0]=array;
    uint8_t *bytes=NULL;size_t count=0;
    ok=save_state(engine,&bytes,&count) && project_fixture_vm(engine,&bytes,&count);
    if(ok){
      size_t at=112+(size_t)read_u64(bytes+64)+(size_t)read_u64(bytes+72)+132;
      uint8_t expected[36]={0};write_u32(expected,2);
      write_u32(expected+4,UINT32_C(0x80000000)|(unsigned)lengths[i]);
      write_u32(expected+16,1);write_u32(expected+20,(unsigned)lengths[i]-1);
      double value=99;uint64_t bits=0;memcpy(&bits,&value,8);write_u64(expected+28,bits);
      ok=at+sizeof expected<=count && !memcmp(bytes+at,expected,sizeof expected);
    }
    free(bytes);engine->vm.script_args[0]=original;gml_values_release_owned(&array,1);
  }
  engine->vm.script_args[0]=original;
  if(ok){
    uint8_t *bytes=NULL;size_t count=0;
    engine->vm.pending_game_load=strdup("neutral-request.sav");
    ok=engine->vm.pending_game_load && save_state(engine,&bytes,&count) &&
       !project_fixture_vm(engine,&bytes,&count);
    free(engine->vm.pending_game_load);engine->vm.pending_game_load=NULL;
    free(bytes);
  }
  const uint32_t unknown_roots[]={50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,71};
  for(unsigned i=0;i<sizeof unknown_roots/sizeof unknown_roots[0] && ok;i++){
    uint8_t *bytes=NULL;size_t count=0;
    ok=save_state(engine,&bytes,&count);
    if(ok){
      write_u32(bytes+4,unknown_roots[i]);
      ok=!project_fixture_vm(engine,&bytes,&count);
    }
    free(bytes);
  }
  for(unsigned variant=0;variant<3 && ok;variant++){
    uint8_t *bytes=NULL;size_t count=0;
    ok=save_state(engine,&bytes,&count);
    size_t start=ok?112+(size_t)read_u64(bytes+64)+(size_t)read_u64(bytes+72):0;
    size_t length=ok?(size_t)read_u64(bytes+80):0;
    if(ok && start<=count && length<=count-start && length>=12){
      if(variant<2)write_u32(bytes+start+4,variant?46:44);
      else write_u32(bytes+start+length-4,1);
      write_u64(bytes+56,state_checksum(bytes+112,count-112));
      ok=!project_fixture_vm(engine,&bytes,&count);
    }else ok=0;
    free(bytes);
  }
  const uint32_t unknown_vms[]={43,44,46};
  for(unsigned i=0;i<sizeof unknown_vms/sizeof unknown_vms[0] && ok;i++){
    uint8_t *bytes=NULL;size_t count=0;
    ok=save_state(engine,&bytes,&count);
    if(ok){
      size_t start=112+(size_t)read_u64(bytes+64)+(size_t)read_u64(bytes+72);
      write_u32(bytes+start+4,unknown_vms[i]);
      ok=!project_fixture_vm(engine,&bytes,&count);
    }
    free(bytes);
  }
  if(ok){
    uint8_t *bytes=NULL;size_t count=0;
    engine->vm.date_timezone=1;
    ok=save_state(engine,&bytes,&count) && !project_fixture_vm(engine,&bytes,&count);
    engine->vm.date_timezone=0;
    free(bytes);
  }
  if(ok && engine->render.n_spr>0){
    uint8_t *bytes=NULL;size_t count=0;
    uint32_t flags=engine->render.spr[0].metadata_flags;
    engine->render.spr[0].metadata_flags|=GML_SPRITE_META_SMOOTH;
    ok=save_state(engine,&bytes,&count) && !project_fixture_playback(engine,&bytes,&count);
    engine->render.spr[0].metadata_flags=flags;
    free(bytes);
  }
  if(ok){
    uint8_t *bytes=NULL;size_t count=0;
    gml_builtin_call(&engine->vm,"variable_get_hash",(GmlVal[]){vstr("hidden_member")},1);
    ok=save_state(engine,&bytes,&count) && !project_fixture_vm(engine,&bytes,&count);
    gml_varmap_free_ex(&engine->vm.member_names,0);
    free(bytes);
  }
  if(ok){
    uint8_t *bytes=NULL;size_t count=0;
    gml_builtin_call(&engine->vm,"gamepad_set_button_threshold",(GmlVal[]){vreal(5),vreal(0.25)},2);
    ok=save_state(engine,&bytes,&count) && !project_fixture_vm(engine,&bytes,&count);
    gml_builtin_call(&engine->vm,"gamepad_set_button_threshold",(GmlVal[]){vreal(5),vreal(0.5)},2);
    free(bytes);
  }
  if(ok){
    uint8_t *bytes=NULL;size_t count=0;
    ok=save_state(engine,&bytes,&count);
    size_t render=112+(size_t)read_u64(bytes+64),render_size=(size_t)read_u64(bytes+72);
    size_t sprites=(size_t)engine->render.n_spr;
    if(sprites>(SIZE_MAX-4u)/44u)ok=0;
    size_t metadata_bytes=4u+44u*sprites;
    const uint8_t neutral[12]={0,0,0,0,1,0,0,0,1,0,0,0};
    if(ok && render<=count && render_size<=count-render && render_size>=metadata_bytes+12u){
      size_t mode_at=render+render_size-metadata_bytes-12u;
      ok=!memcmp(bytes+mode_at,neutral,sizeof neutral);
      if(ok){
        write_u32(bytes+mode_at,1u);
        write_u64(bytes+56,state_checksum(bytes+112,count-112));
        ok=!project_fixture_vm(engine,&bytes,&count);
      }
    }else ok=0;
    free(bytes);
  }
  /* Every new particle setting must be neutral before historical projection. */
  for(unsigned field=0;field<5 && ok;field++){
    uint8_t *bytes=NULL;size_t count=0;
    ok=save_state(engine,&bytes,&count);
    size_t start=ok?112+(size_t)read_u64(bytes+64)+(size_t)read_u64(bytes+72):0;
    size_t length=ok?(size_t)read_u64(bytes+80):0,at=0;int matches=0;
    const uint8_t defaults[36]={
      0,0,0,0,0,0,0xf0,0x3f, 0,0,0,0,0,0,0xf0,0x3f,
      0,0,0,0,0,0,0xf0,0x3f, 0,0,0,0,0,0,0xf0,0x3f, 0,0,0,0
    };
    for(size_t i=start;ok && i+sizeof defaults<=start+length;i++)
      if(!memcmp(bytes+i,defaults,sizeof defaults)){at=i;matches++;}
    ok=ok && matches==1;
    if(ok){
      if(field<4)write_u64(bytes+at+field*8,UINT64_C(0x4000000000000000));
      else write_u32(bytes+at+32,128);
      write_u64(bytes+56,state_checksum(bytes+112,count-112));
      ok=!project_fixture_vm(engine,&bytes,&count);
    }
    free(bytes);
  }
  /* Nonempty points, bonds, groups, allocator or pending definition cannot disappear into a pin. */
  for(unsigned field=0;field<5 && ok;field++){
    uint8_t *bytes=NULL;size_t count=0;ok=save_state(engine,&bytes,&count);
    size_t start=ok?112+(size_t)read_u64(bytes+64)+(size_t)read_u64(bytes+72):0;
    size_t length=ok?(size_t)read_u64(bytes+80):0,at=0;int matches=0;
    const uint8_t defaults[36]={
      0,0,0,0,0,0,0xf0,0x3f, 0,0,0,0,0,0,0xf0,0x3f,
      0,0,0,0,0,0,0xf0,0x3f, 0,0,0,0,0,0,0xf0,0x3f, 0,0,0,0
    };
    for(size_t i=start;ok && i+sizeof defaults+20<=start+length;i++)
      if(!memcmp(bytes+i,defaults,sizeof defaults)){at=i;matches++;}
    ok=ok && matches==1;
    if(ok){
      write_u32(bytes+at+36+field*4,1);
      write_u64(bytes+56,state_checksum(bytes+112,count-112));
      ok=!project_fixture_vm(engine,&bytes,&count);
    }
    free(bytes);
  }
  /* A non-default new audio cell must fail closed instead of disappearing into an old pin. */
  if(ok){
    uint8_t *bytes=NULL;size_t count=0;
    ok=save_state(engine,&bytes,&count);
    size_t audio=112;
    if(ok)for(unsigned offset=64;offset<=80;offset+=8){
      uint64_t part=read_u64(bytes+offset);
      if(audio>count || part>count-audio){ok=0;break;}audio+=(size_t)part;
    }
    if(ok && audio+108u<=count){
      write_u64(bytes+audio+100u,UINT64_C(0x3ff0000000000000));
      write_u64(bytes+56,state_checksum(bytes+112,count-112));
      ok=!project_fixture_vm(engine,&bytes,&count);
    }else ok=0;
    free(bytes);
  }
  uint8_t *after=NULL;size_t after_size=0;
  ok=ok && save_state(engine,&after,&after_size) && after_size==size &&
    !memcmp(after,baseline,size);
  free(after);
  if(!ok)fputs("current-to-preceding fixture projection discriminator failed\n",stderr);
  return ok;
}
