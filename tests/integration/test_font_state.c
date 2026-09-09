/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "anygm.h"
#include "engine_internal.h"
#include "gml_render.h"
#include "memory_vfs.h"
#include "synthetic_content.h"
#include "../unit/media/font_test_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t read64(const uint8_t *p){
  uint64_t value=0;
  for(int i=0;i<8;i++) value|=(uint64_t)p[i]<<(i*8);
  return value;
}
static void write64(uint8_t *p,uint64_t value){
  for(int i=0;i<8;i++) p[i]=(uint8_t)(value>>(i*8));
}
static uint64_t checksum(const uint8_t *p,size_t size){
  uint64_t hash=UINT64_C(1469598103934665603);
  while(size>=8){ hash^=read64(p); hash*=UINT64_C(1099511628211); p+=8; size-=8; }
  while(size--){ hash^=*p++; hash*=UINT64_C(1099511628211); }
  return hash;
}
static uint8_t *save(AnygmEngine *engine,size_t *size){
  size_t capacity=anygm_state_size(engine);
  uint8_t *bytes=capacity?malloc(capacity):NULL;
  if(!bytes || anygm_state_save(engine,bytes,capacity,size)!=ANYGM_OK){ free(bytes); return NULL; }
  return bytes;
}

/* The target removes a live font before a later VM field rejects. The rollback must recover
 * the live font without depending on the source file still containing its original bytes. */
static int rejected_load_preserves_font(int remove_source){
  AnygmSyntheticContent content={0};
  uint8_t *payload=NULL,*target=NULL,*baseline=NULL,*after=NULL;
  size_t payload_size=0,target_size=0,baseline_size=0,after_size=0;
  AnygmMemoryVfs *memory=calloc(1,sizeof(*memory));
  AnygmHostServices host={0};
  AnygmEngine *engine=NULL;
  int created=anygm_synthetic_content_create(&content),ok=0;
  if(!memory || !created || !anygm_synthetic_content_read(&content,&payload,&payload_size)) goto done;
  anygm_memory_vfs_init(memory,&host);
  uint8_t font_bytes[552];
  size_t font_size=font_fixture_build(font_bytes);
  if(!anygm_memory_vfs_add_file(memory,"/content/fixture.ttf",font_bytes,font_size) ||
     anygm_create(&host,&engine)!=ANYGM_OK) goto done;
  AnygmContentSource source={0};
  source.struct_size=sizeof source; source.kind=ANYGM_CONTENT_MEMORY;
  source.path="/content/data.win"; source.data=payload; source.size=payload_size;
  if(anygm_load(engine,&source,NULL)!=ANYGM_OK) goto done;
  target=save(engine,&target_size);
  int font=gml_font_add_file(&engine->render,"/content/fixture.ttf",32,1,1,65,65);
  if(!target || font<0) goto done;
  engine->render.font=font;
  int width=gml_text_width(&engine->render,"A");
  baseline=save(engine,&baseline_size);
  if(!baseline || width<=0) goto done;
  if(remove_source){
    if(host.path_remove(host.userdata,"/content/fixture.ttf")!=ANYGM_OK) goto done;
  } else if(!anygm_memory_vfs_xor_byte(memory,"/content/fixture.ttf",0,1)) goto done;
  if(target_size<112) goto done;
  uint64_t vm_offset=112+read64(target+64)+read64(target+72);
  if(vm_offset>target_size || target_size-vm_offset<12) goto done;
  memset(target+(size_t)vm_offset+8,0xFF,4); /* impossible VM instance count */
  write64(target+56,checksum(target+112,target_size-112));
  AnygmResult result=anygm_state_load(engine,target,target_size);
  after=save(engine,&after_size);
  GmlRenderFontMetrics metrics={0};
  ok=result==ANYGM_ERROR_STATE_MISMATCH && after && after_size==baseline_size &&
    !memcmp(after,baseline,baseline_size) && gml_render_font_exists(&engine->render,font) &&
    gml_render_font_metrics(&engine->render,font,&metrics) && !metrics.sprite_backed &&
    metrics.bold==1 && metrics.italic==1 && metrics.glyph_count==1 &&
    gml_text_width(&engine->render,"A")==width;
done:
  if(!ok) fprintf(stderr,"font rollback after %s source failed: sizes=%zu/%zu\n",
                   remove_source?"missing":"changed",after_size,baseline_size);
  anygm_destroy(engine);
  if(memory){ anygm_memory_vfs_destroy(memory); free(memory); }
  if(created) anygm_synthetic_content_destroy(&content);
  free(payload); free(target); free(baseline); free(after);
  return ok;
}

int main(void){
  int missing=rejected_load_preserves_font(1);
  int changed=rejected_load_preserves_font(0);
  if(!missing || !changed) return 1;
  puts("runtime font root rollback: ok");
  return 0;
}
