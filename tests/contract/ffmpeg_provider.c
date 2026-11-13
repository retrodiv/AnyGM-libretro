/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me>
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <string.h>

#if LIBAVCODEC_VERSION_MAJOR < 62 || LIBAVFORMAT_VERSION_MAJOR < 62 || LIBAVUTIL_VERSION_MAJOR < 60
#error Video playback requires FFmpeg 8 or later development headers.
#endif

typedef struct {
  const char *name;
  unsigned version;
  const char *license,*configuration;
} Provider;

int main(void){
  Provider providers[]={
    {"avcodec",avcodec_version(),avcodec_license(),avcodec_configuration()},
    {"avformat",avformat_version(),avformat_license(),avformat_configuration()},
    {"avutil",avutil_version(),avutil_license(),avutil_configuration()},
    {"swscale",swscale_version(),swscale_license(),swscale_configuration()},
    {"swresample",swresample_version(),swresample_license(),swresample_configuration()}
  };
  int ok=1;
  for(unsigned i=0;i<sizeof providers/sizeof *providers;i++){
    const Provider *p=&providers[i];
    printf("%s %u.%u.%u: %s\n",p->name,p->version>>16,(p->version>>8)&255,
            p->version&255,p->license);
    if(strcmp(p->license,"LGPL version 2.1 or later") ||
       strstr(p->configuration,"--enable-gpl") ||
       strstr(p->configuration,"--enable-nonfree") ||
       strstr(p->configuration,"--enable-version3")) ok=0;
  }
  if(!ok) fputs("FFmpeg provider: the reviewed shared-library route requires LGPL-2.1-or-later; "
               "use tools/media/build_ffmpeg.py or an equivalent verified provider.\n",stderr);
  return ok?0:1;
}
