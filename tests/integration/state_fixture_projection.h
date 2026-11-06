/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
/* Test scratch only: expand the current neutral fixture's value/text encodings to its preceding
 * layout. No state is loaded through this helper. Unsupported fixture features fail closed, and
 * the caller retains every historical size/hash pin as well as current-format roundtrip checks. */
typedef struct FixtureProjection {
  const uint8_t *input;
  size_t size,pos,used;
  uint8_t output[65536];
  struct { const uint8_t *text; uint32_t length; } strings[256];
  unsigned string_count,array_count;
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
  uint32_t count=fixture_read_word(p);
  /* The fixed Classic fixture owns distinct maps. A new repeated-map feature must be measured
   * and projected explicitly, rather than silently copying an array definition a second time. */
  if(count>256){p->ok=0;return;}
  fixture_word(p,count);
  for(uint32_t i=0;i<count && p->ok;i++){fixture_string(p);(void)fixture_value(p,0);}
}
static int project_fixture_vm(AnygmEngine *engine,uint8_t **data,size_t *written){
  const uint8_t *bytes=*data;
  if(*written<112 || (uint32_t)read_u64(bytes+4)!=41 || engine->vm.n_structs ||
     engine->vm.n_tilemaps || engine->vm.n_rtl || engine->vm.n_rte ||
     engine->vm.room_visuals || engine->vm.wait.active)return 0;
  size_t start=112+(size_t)read_u64(bytes+64)+(size_t)read_u64(bytes+72);
  size_t size=(size_t)read_u64(bytes+80);
  if(start>*written || size>*written-start || size<132)return 0;
  FixtureProjection *p=calloc(1,sizeof(*p));if(!p)return 0;
  p->input=bytes+start;p->size=size;p->ok=1;
  fixture_copy(p,132);
  if((uint32_t)read_u64(p->output+4)!=23)p->ok=0;
  write_u32(p->output+4,19);
  for(unsigned i=0;i<16;i++)(void)fixture_value(p,0);
  fixture_copy(p,76+26*4+616*8+10*4); /* RNG and unchanged software-3D scalars */
  uint32_t models=fixture_copy_word(p);fixture_copy(p,models);
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
  fixture_copy(p,40);fixture_empty(p);fixture_empty(p); /* physics scalars, empty fixtures/joints */
  fixture_copy(p,4);uint32_t particles=fixture_copy_word(p);fixture_copy(p,particles);
  fixture_copy(p,sizeof engine->vm.key_map);
  fixture_copy(p,32+32*7*8+9*8+4); /* unchanged emitter/listener scalars */
  fixture_empty(p);fixture_empty(p);fixture_string(p);fixture_empty(p); /* external audio */
  fixture_empty(p); /* no live script statics */
  fixture_copy(p,8);fixture_empty(p); /* no scheduled language callbacks */
  if(p->pos<=p->size)fixture_copy(p,p->size-p->pos);
  if(!p->ok){fprintf(stderr,"fixture projection stopped at VM byte %zu/%zu\n",p->pos,size);free(p);return 0;}
  size_t total=*written-size+p->used;
  uint8_t *result=malloc(total);
  if(!result){free(p);return 0;}
  memcpy(result,bytes,start);memcpy(result+start,p->output,p->used);
  memcpy(result+start+p->used,bytes+start+size,*written-start-size);
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
  uint8_t *after=NULL;size_t after_size=0;
  ok=ok && save_state(engine,&after,&after_size) && after_size==size &&
    !memcmp(after,baseline,size);
  free(after);
  if(!ok)fputs("current-to-preceding fixture projection discriminator failed\n",stderr);
  return ok;
}
