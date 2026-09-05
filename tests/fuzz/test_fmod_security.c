/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_fmod.h"
#include "anygm_test_runner.h"

#include <stdio.h>
#include <string.h>

static void write_u32(uint8_t *data,size_t offset,uint32_t value){
  for(size_t byte=0;byte<4;byte++) data[offset+byte]=(uint8_t)(value>>(8*byte));
}

static void write_u64(uint8_t *data,size_t offset,uint64_t value){
  for(size_t byte=0;byte<8;byte++) data[offset+byte]=(uint8_t)(value>>(8*byte));
}

static void make_sample(uint8_t *data,size_t size,uint32_t headers,uint32_t names,uint32_t payload,
                        uint64_t sample_header){
  memset(data,0,size);
  if(size<60) return;
  memcpy(data,"FSB5",4);
  write_u32(data,8,1);
  write_u32(data,12,headers);
  write_u32(data,16,names);
  write_u32(data,20,payload);
  if(size>=68) write_u64(data,60,sample_header);
}

static int rejected_without_output(const uint8_t *data,size_t size){
  GmlFmodSample output;
  memset(&output,0xA5,sizeof output);
  unsigned char before[sizeof output];
  memcpy(before,&output,sizeof output);
  return !gml_fmod_fsb5_sample(data,size,0,&output) &&
         !memcmp(before,&output,sizeof output);
}

static int missing_sample_header(void){
  uint8_t bad[60];
  make_sample(bad,sizeof bad,8,0,0,0);
  return rejected_without_output(bad,sizeof bad);
}

static int truncated_complete_sample(void){
  uint8_t sample[72];
  make_sample(sample,sizeof sample,8,0,4,((uint64_t)1<<34)|((uint64_t)8<<1));
  for(size_t size=0;size<sizeof sample;size++) if(!rejected_without_output(sample,size)) return 0;
  GmlFmodSample output;
  if(!gml_fmod_fsb5_sample(sample,sizeof sample,0,&output)) return 0;
  return output.channels==1 && output.rate==44100 && output.num_samples==1 &&
         output.setup_crc==0 && output.data==sample+68 && output.data_len==4;
}

static int incomplete_metadata(void){
  uint8_t bad[72];
  make_sample(bad,sizeof bad,12,0,0,((uint64_t)8<<1)|1);
  write_u32(bad,68,2);          /* one metadata byte, absent from the header region */
  return rejected_without_output(bad,sizeof bad);
}

static int incomplete_sample_table(void){
  uint8_t bad[68];
  make_sample(bad,sizeof bad,8,0,0,((uint64_t)8<<1));
  write_u32(bad,8,2);           /* two declared entries but exactly one header */
  return rejected_without_output(bad,sizeof bad);
}

static int out_of_range_data_offset(void){
  uint8_t bad[72];
  make_sample(bad,sizeof bad,8,0,4,((uint64_t)8<<1)|((uint64_t)1<<6));
  return rejected_without_output(bad,sizeof bad);
}

static int vorbis_metadata_crc(void){
  uint8_t sample[76];
  make_sample(sample,sizeof sample,16,0,0,((uint64_t)8<<1)|1);
  write_u32(sample,68,((uint32_t)11<<25)|8); /* VORBISDATA metadata with four CRC bytes. */
  write_u32(sample,72,0xDEADBEEFu);
  GmlFmodSample output;
  return gml_fmod_fsb5_sample(sample,sizeof sample,0,&output) &&
         output.setup_crc==0xDEADBEEFu && output.data==sample+76 && output.data_len==0;
}

int main(int argc,char **argv){
  const char *filter=NULL;
  if(argc==3 && !strcmp(argv[1],"--case")) filter=argv[2];
  else if(argc!=1) return 2;
  static const AnygmTestCase cases[]={
    {"missing_sample_header",missing_sample_header},
    {"truncated_complete_sample",truncated_complete_sample},
    {"incomplete_metadata",incomplete_metadata},
    {"incomplete_sample_table",incomplete_sample_table},
    {"out_of_range_data_offset",out_of_range_data_offset},
    {"vorbis_metadata_crc",vorbis_metadata_crc},
  };
  static const AnygmTestGroup groups[]={
    {"fsb5",cases,sizeof cases/sizeof cases[0]},
  };
  AnygmTestResult result;
  int ok=anygm_test_run_groups(groups,sizeof groups/sizeof groups[0],filter,&result);
  printf("FSB5 security: passed=%d failed=%d\n",result.passed,result.failed);
  return ok?0:1;
}
