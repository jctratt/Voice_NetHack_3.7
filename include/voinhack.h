/*-Copyright (c) Jeff Tratt, 2026. */
/* NetHack may be freely redistributed.  See license for details. */

#ifndef VOINHACK_H
#define VOINHACK_H

#ifdef VOICE_ENABLED

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

struct voice_exception {
    struct voice_exception *next;
    char *pattern;
    pcre2_code *re;
};

struct voice_force {
    struct voice_force *next;
    char *pattern;
    char *speak_text;     /* Output text, e.g., "$1" or "corpse." */
    pcre2_code *re;
};

extern struct voice_exception *voicelist;
extern struct voice_force *forcelist;

/* Function prototypes */
void handle_voice_output(const char *message);
int add_voice_exception(const char *pattern);
int add_voice_force(const char *pattern);
void free_voice_data(void);
char *strip_voice_patterns(const char *message);

#endif /* VOICE_ENABLED */

#endif /* VOINHACK_H */
