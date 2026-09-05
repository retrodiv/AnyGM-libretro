/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include "gml_fmod.h"
#include "anygm_test_runner.h"
#include "memory_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile the private FSB5-bank parser with this synthetic security corpus. This keeps
 * parser internals private to production while allowing its transactional guarantees to
 * be checked directly. */
#include "../../src/audio/banks/gml_fmod.c"

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

static int extreme_sizes(void){
  uint8_t bad[60];
  make_sample(bad,sizeof bad,UINT32_MAX,UINT32_MAX,UINT32_MAX,0);
  return rejected_without_output(bad,sizeof bad);
}

static int short_vorbis_metadata_crc(void){
  uint8_t bad[75];
  make_sample(bad,sizeof bad,15,0,0,((uint64_t)8<<1)|1);
  write_u32(bad,68,((uint32_t)11<<25)|6); /* VORBISDATA metadata with only three CRC bytes. */
  return rejected_without_output(bad,sizeof bad);
}

static int malformed_later_sample(void){
  uint8_t bad[76];
  make_sample(bad,sizeof bad,16,0,0,((uint64_t)8<<1));
  write_u32(bad,8,2);
  write_u64(bad,68,((uint64_t)8<<1)|1); /* The second header requires absent metadata. */
  return rejected_without_output(bad,sizeof bad);
}

static void free_table(FBank *bank){
  if(!bank) return;
  if(bank->names){
    for(int index=0;index<bank->nsubs;index++) free(bank->names[index]);
    free(bank->names);
  }
  free(bank->subs);
  bank->names=NULL;
  bank->subs=NULL;
  bank->nsubs=0;
}

static void make_named_table(uint8_t *data,const char *name){
  make_sample(data,74,8,6,4,((uint64_t)8<<1));
  write_u32(data,68,4);
  data[72]=(uint8_t)name[0];
  data[73]=(uint8_t)name[1];
}

static int table_rejection_preserves_state(void){
  uint8_t valid[74],invalid_offset[74],unterminated[74];
  make_named_table(valid,"x");
  make_named_table(invalid_offset,"x");
  make_named_table(unterminated,"xy");
  write_u32(invalid_offset,68,0);
  FBank bank={0};
  if(!fmod_fsb5_parse_table(&bank,valid,sizeof valid,123,4)){
    free_table(&bank);
    return 0;
  }
  FSub *subs=bank.subs;
  char **names=bank.names;
  FSub first=bank.subs[0];
  int ok=!fmod_fsb5_parse_table(&bank,invalid_offset,sizeof invalid_offset,456,4) &&
         !fmod_fsb5_parse_table(&bank,unterminated,sizeof unterminated,789,4) &&
         bank.subs==subs && bank.names==names && bank.nsubs==1 &&
         bank.data_file_off==123 && !memcmp(&bank.subs[0],&first,sizeof first) &&
         bank.names && bank.names[0] && !strcmp(bank.names[0],"x");
  free_table(&bank);
  return ok;
}

static int snd_extent_is_enforced(void){
  uint8_t bytes[92]={0};
  memcpy(bytes,"RIFF",4);
  memcpy(bytes+8,"FEV ",4);
  memcpy(bytes+12,"SND ",4);
  write_u32(bytes,16,68);
  make_sample(bytes+20,72,8,0,4,((uint64_t)8<<1));
  memset(bytes+88,0x5A,4); /* Present in the file but outside the SND chunk. */
  AnygmMemoryVfs memory;
  AnygmHostServices services;
  anygm_memory_vfs_init(&memory,&services);
  int added=anygm_memory_vfs_add_file(&memory,"mem/overflow.bank",bytes,sizeof bytes);
  anygm_memory_vfs_guard_reads(&memory,"mem/overflow.bank",0,88);
  FBank bank={0};
  int opened=added && fmod_bank_open(&bank,&services,"mem/overflow.bank");
  if(bank.nsubs>0){
    uint8_t data[4];
    (void)fmod_bank_read(&bank,bank.data_file_off,data,sizeof data);
  }
  int ok=opened && bank.nsubs==0 && !memory.read_violation;
  if(bank.file) services.file_close(services.userdata,bank.file);
  free_table(&bank);
  anygm_memory_vfs_destroy(&memory);
  return ok;
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
    {"extreme_sizes",extreme_sizes},
    {"short_vorbis_metadata_crc",short_vorbis_metadata_crc},
    {"malformed_later_sample",malformed_later_sample},
    {"table_rejection_preserves_state",table_rejection_preserves_state},
    {"snd_extent_is_enforced",snd_extent_is_enforced},
  };
  static const AnygmTestGroup groups[]={
    {"fsb5",cases,sizeof cases/sizeof cases[0]},
  };
  AnygmTestResult result;
  int ok=anygm_test_run_groups(groups,sizeof groups/sizeof groups[0],filter,&result);
  printf("FSB5 security: passed=%d failed=%d\n",result.passed,result.failed);
  return ok?0:1;
}
