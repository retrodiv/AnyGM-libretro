/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* Build-time only: include the selected BSD reference encoder to select
 * its private templates. No reference encoder is linked into the core.
 */
#include "vorbisenc.c"
#include <float.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>

static int setup_profile(vorbis_info *vi){
    codec_setup_info *ci=vi->codec_setup;
    const ve_setup_data_template *original=ci->hi.setup;
    if(!original || vi->channels<1 || vi->channels>2 ||
       original->mappings<1 || original->mappings>15 ||
       original->floor_mappings<1 || original->floor_mappings>2) return -1;
    ve_setup_data_template chosen=*original;
    int shorts[16],longs[16],floor0[16],floor1[16];
    const int *floors[2]={floor0,floor1};
    vorbis_mapping_template maps[16];
    vorbis_residue_template low[2];
    for(int i=0;i<original->mappings;i++){
        shorts[i]=256;
        longs[i]=vi->rate<15000?256:2048;
        floor0[i]=original->floor_mapping_list[0][i];
        floor1[i]=original->floor_mappings>1?original->floor_mapping_list[1][i]:0;
        if(vi->rate<15000) floor0[i]=0;
        else if(vi->rate<26000){
            floor0[i]=i==0?0:i==1?2:4;
            floor1[i]=7;
        }else{
            if(floor0[i]==1) floor0[i]=0;
            if(floor1[i]==8) floor1[i]=7;
        }
        maps[i]=original->maps[i];
    }
    if(vi->rate>=15000 && vi->rate<26000){
        if(original->mappings<2) return -1;
        /* The lowest reference profile has only one residue. Give the
         * second block an explicit, existing reference residue; never
         * index past that one-element source array.
         */
        low[0]=maps[0].res[0];
        low[1]=maps[1].res[0];
        maps[0].res=low;
    }
    chosen.blocksize_short=shorts;
    chosen.blocksize_long=longs;
    chosen.floor_mapping_list=floors;
    chosen.maps=maps;
    ci->hi.setup=&chosen;
    int result=vorbis_encode_setup_init(vi);
    ci->hi.setup=original;
    return result;
}

static int word(uint32_t value){
    unsigned char bytes[4];
    for(unsigned i=0;i<4;i++) bytes[i]=(unsigned char)(value>>(8*i));
    return fwrite(bytes,1,4,stdout)==4?0:-1;
}

int main(void){
    _Static_assert(CHAR_BIT==8 && sizeof(float)==4 && FLT_RADIX==2 &&
                   FLT_MANT_DIG==24 && FLT_MAX_EXP==128,"IEEE binary32 host required");
    unsigned channels,rate,quality_bits,coupling;
    int scanned;
    while((scanned=scanf("%u %u %x %u",&channels,&rate,&quality_bits,&coupling))==4){
        if(channels<1 || channels>2 || rate<8000 || rate>48000 || coupling>1) return 2;
        float quality;
        uint32_t bits=quality_bits;
        memcpy(&quality,&bits,sizeof quality);
        if(!(quality>=-0.2f && quality<=1.0f)) return 2;
        vorbis_info vi;
        vorbis_info_init(&vi);
        int selected=(int)coupling;
        if(vorbis_encode_setup_vbr(&vi,(long)channels,(long)rate,quality) ||
           vorbis_encode_ctl(&vi,OV_ECTL_COUPLING_SET,&selected) || setup_profile(&vi)){
            vorbis_info_clear(&vi);
            return 3;
        }
        /* The pinned header serializer only needs vi and its three output
         * buffers. Do not initialize PCM analysis or psychoacoustic lookup
         * state when no samples will be encoded.
         */
        private_state headers={0};
        vorbis_dsp_state dsp={0};
        dsp.vi=&vi;
        dsp.backend_state=&headers;
        vorbis_comment comment;
        vorbis_comment_init(&comment);
        ogg_packet identity,comments,setup;
        int result=vorbis_analysis_headerout(&dsp,&comment,&identity,&comments,&setup);
        if(!result && (setup.bytes<1 || setup.bytes>65536 ||
           word((uint32_t)vorbis_info_blocksize(&vi,0)) ||
           word((uint32_t)vorbis_info_blocksize(&vi,1)) || word((uint32_t)setup.bytes) ||
           fwrite(setup.packet,1,(size_t)setup.bytes,stdout)!=(size_t)setup.bytes)) result=-1;
        vorbis_comment_clear(&comment);
        _ogg_free(headers.header);
        _ogg_free(headers.header1);
        _ogg_free(headers.header2);
        vorbis_info_clear(&vi);
        if(result) return 3;
    }
    return scanned==EOF && !ferror(stdin) && !fflush(stdout)?0:2;
}
