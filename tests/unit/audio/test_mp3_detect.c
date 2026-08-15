/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* The audio bank asks the MP3 detector whether an arbitrary container blob is MP3, so the detector
 * runs over bytes nobody designed for it. It must answer, and it must read only the bytes it was
 * given.
 *
 * It did neither. Its free-format frame size was read before it was written, so a blob whose first
 * bytes form a valid free-format header took that stack value as a frame length: the match loop
 * accumulated it into an offset, a negative one walked the offset backwards, the bound check let it
 * through because it compares against a maximum only, and the header comparison read from an
 * address far outside the blob. The outcome depended on what the stack happened to hold,
 * so identical inputs could either fault or return normally.
 *
 * The blob here sits alone in a page with an unreadable page on each side, so a read that leaves it
 * is a fault rather than a wrong answer, and the call runs in a child process so that fault is a
 * failed assertion instead of a dead test. The stack is filled with a pattern that reads as a large
 * negative integer immediately before the call, which is what the uninitialized read used to find.
 */
#include "minimp3_ex.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
int main(void){
  puts("mp3 detect bounds: skipped (needs POSIX memory protection and fork)");
  return 0;
}
#else
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* A valid MPEG-1 Layer III header whose bitrate index is zero, which is what selects free format
 * and therefore the frame size the detector used to leave uninitialized. Nothing follows it that
 * could be a second header, so a detector reading only these bytes answers "not MP3". */
static const unsigned char FREE_FORMAT_HEADER[16] = {
  0xFF,0xFB,0x00,0x00, 0,0,0,0, 0,0,0,0, 0,0,0,0
};

static void poison_stack(void){
  volatile unsigned char scratch[8192];
  memset((void*)scratch,0x80,sizeof scratch);   /* 0x80808080 reads as a large negative int */
  if(scratch[0]!=0x80) puts("");                /* keep the write */
}

/* The blob alone on a readable page, with an unreadable page on each side. Returns NULL when the
 * platform refuses the mapping, which is a reason to skip rather than to fail. */
static unsigned char *guarded_copy(const unsigned char *data,size_t size,int at_page_end,
                                   unsigned char **mapping,size_t *mapping_size){
  long page=sysconf(_SC_PAGESIZE);
  if(page<=0 || size>(size_t)page) return NULL;
  size_t total=(size_t)page*3u;
  unsigned char *base=mmap(NULL,total,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(base==MAP_FAILED) return NULL;
  if(mprotect(base,(size_t)page,PROT_NONE) || mprotect(base+2*(size_t)page,(size_t)page,PROT_NONE)){
    munmap(base,total);
    return NULL;
  }
  unsigned char *blob=base+(size_t)page+(at_page_end?(size_t)page-size:0u);
  memcpy(blob,data,size);
  *mapping=base; *mapping_size=total;
  return blob;
}

/* mp3dec_detect_buf in a child, so a read outside the blob is reported rather than fatal. */
static int detects_without_leaving_the_blob(int at_page_end,int *answer){
  int pipefd[2];
  if(pipe(pipefd)!=0) return -1;
  pid_t child=fork();
  if(child<0){ close(pipefd[0]); close(pipefd[1]); return -1; }
  if(child==0){
    close(pipefd[0]);
    unsigned char *mapping=NULL; size_t mapping_size=0;
    unsigned char *blob=guarded_copy(FREE_FORMAT_HEADER,sizeof FREE_FORMAT_HEADER,
                                     at_page_end,&mapping,&mapping_size);
    if(!blob) _exit(2);
    poison_stack();
    int result=mp3dec_detect_buf(blob,sizeof FREE_FORMAT_HEADER);
    ssize_t written=write(pipefd[1],&result,sizeof result);
    (void)written;
    _exit(0);
  }
  close(pipefd[1]);
  int result=0;
  ssize_t got=read(pipefd[0],&result,sizeof result);
  close(pipefd[0]);
  int status=0;
  if(waitpid(child,&status,0)<0) return -1;
  if(WIFSIGNALED(status)){
    fprintf(stderr,"the detector left its blob: killed by signal %d (blob at page %s)\n",
            WTERMSIG(status),at_page_end?"end":"start");
    return 0;
  }
  if(!WIFEXITED(status)) return -1;
  if(WEXITSTATUS(status)==2) return -1;                 /* could not map: skip */
  if(WEXITSTATUS(status)!=0 || got!=(ssize_t)sizeof result){
    fprintf(stderr,"the detector did not answer (blob at page %s)\n",at_page_end?"end":"start");
    return 0;
  }
  *answer=result;
  return 1;
}

int main(void){
  int skipped=0;
  for(int at_page_end=0;at_page_end<2;at_page_end++){
    int answer=0;
    int outcome=detects_without_leaving_the_blob(at_page_end,&answer);
    if(outcome<0){ skipped=1; continue; }
    if(outcome==0) return 1;
    if(answer==0){
      fprintf(stderr,"a lone free-format header is not MP3, and the detector accepted it "
                     "(blob at page %s)\n",at_page_end?"end":"start");
      return 1;
    }
  }
  puts(skipped ? "mp3 detect bounds: skipped (no guarded mapping available)"
               : "mp3 detect bounds: ok");
  return 0;
}
#endif
