/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 retrodiv <retrodiv@proton.me> */
/* Dump selected, licensed static_codebook declarations as canonical values. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "codebook.h"
#include "books/floor/floor_books.h"
#include "books/coupled/res_books_stereo.h"
#include "books/uncoupled/res_books_uncoupled.h"
#include "selected_books.h"

int main(void){
    for(size_t i=0;i<sizeof reference_books/sizeof *reference_books;i++)
        if(reference_books[i]->dim<1 || reference_books[i]->entries<1) return 2;
    puts("[");
    for(size_t nbook=0;nbook<sizeof selected_books/sizeof *selected_books;nbook++){
        const static_codebook *b=selected_books[nbook];
        if(nbook) putchar(',');
        printf("[%ld,%ld,[",b->dim,b->entries);
        for(long i=0;i<b->entries;i++) printf("%s%ld",i?",":"",b->lengthlist[i]);
        if(!b->maptype){printf("],0,0,0,0,0,[]]");continue;}
        printf("],%d,%u,%u,%d,%d,[",b->maptype,(uint32_t)b->q_min,
               (uint32_t)b->q_delta,b->q_quant,b->q_sequencep);
        long count=1;
        if(b->maptype==1){
            for(;;){
                uint64_t power=1;
                for(long dim=0;dim<b->dim;dim++){
                    power*=(uint64_t)(count+1);
                    if(power>(uint64_t)b->entries) break;
                }
                if(power>(uint64_t)b->entries) break;
                count++;
            }
        }else count=b->dim*b->entries;
        for(long i=0;i<count;i++) printf("%s%ld",i?",":"",labs(b->quantlist[i]));
        printf("]]");
    }
    puts("]");
    return fflush(stdout)!=0;
}
