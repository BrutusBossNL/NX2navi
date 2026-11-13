// Whether a start does the whole warm on the boot screen (vk_pipeline.c bf_decide, where the reasons are). Own header
// so tests/boot_warm_check.c checks it alone.
//
// <cache folder>/boot-warm-owed.txt is "<GPU and driver> <starts that began the warm>" or "<GPU and driver> done".
// The start that finds the pipeline cache written by another device or driver writes it with 1; a start that finds
// its own GPU and driver in it with a count goes on with the warm and counts up; the start whose warm ends writes
// "done". The pipeline cache's file keeps the earlier driver's header for some starts after that (what a run compiles
// goes to the play cache until that is folded in), and "done" is what keeps those starts ordinary ones. After
// BOOT_WARM_TRIES starts that began the warm and did not end it the file stays as it is, and starts on that GPU and
// driver are ordinary ones too.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_WARM_TRIES 3

// NO: nothing owed (a file there is another GPU's or driver's, or not one: the caller deletes it). YES: this start
// does the warm. GAVE_UP and DONE: an ordinary start, and the file stays.
enum { BOOT_WARM_NO, BOOT_WARM_YES, BOOT_WARM_GAVE_UP, BOOT_WARM_DONE };

// text: the file's content, NULL without the file. key: this GPU and driver, without spaces. foreign: the pipeline
// cache was written by another device or driver. *tries: the count to write with BOOT_WARM_YES, 0 otherwise.
static inline int boot_warm_decide(const char *text, const char *key, int foreign, int *tries) {
    char was[64] = "", tok[16] = "", *end = tok;
    long n = -1;
    *tries = 0;
    if (text && sscanf(text, "%63s %15s", was, tok) == 2 && !strcmp(was, key)) {
        if (!strcmp(tok, "done")) return BOOT_WARM_DONE;
        n = strtol(tok, &end, 10);
        if (end == tok || *end || n < 0) n = -1;
    }
    if (n < 0 && !foreign) return BOOT_WARM_NO;
    if (n < 0) n = 0;                             // this GPU and driver's first start
    if (n >= BOOT_WARM_TRIES) return BOOT_WARM_GAVE_UP;
    *tries = (int)n + 1;
    return BOOT_WARM_YES;
}
