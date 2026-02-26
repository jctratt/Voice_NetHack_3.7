/*-Copyright (c) Jeff Tratt, 2026. */
/* NetHack may be freely redistributed.  See license for details. */
#include "hack.h"

#ifdef VOICE_ENABLED
#include "voinhack.h"

#include <ctype.h>

/* Global lists to store voice configuration data */
struct voice_exception *voicelist = (struct voice_exception *) 0;
struct voice_force *forcelist = (struct voice_force *) 0;

/**
 * compile_re: Core helper to compile a PCRE2 regular expression.
 * Returns a pointer to the compiled code, or NULL on failure.
 * Prints an in-game message if compilation fails.
 */
static pcre2_code *
compile_re(const char *pattern)
{
    int errornumber;
    PCRE2_SIZE erroroffset;
    pcre2_code *re;

    re = pcre2_compile((PCRE2_SPTR) pattern, PCRE2_ZERO_TERMINATED, 0,
                       &errornumber, &erroroffset, NULL);
    if (re == NULL) {
        PCRE2_UCHAR buffer[256];
        pcre2_get_error_message(errornumber, buffer, sizeof(buffer));
        pline("PCRE2 compilation failed at offset %d: %s", (int) erroroffset,
              buffer);
    }
    return re;
}

/**
 * add_voice_exception: Registers a regex pattern that, when matched,
 * will prevent the voice engine from speaking the associated message.
 * Returns 0 on success, non-zero on failure.
 */
add_voice_exception(const char *pattern)
{
    struct voice_exception *newve;
    pcre2_code *re = compile_re(pattern);

    if (!re)
        return 1;

    newve = (struct voice_exception *) alloc(sizeof *newve);
    newve->pattern = dupstr(pattern);
    newve->re = re;
    newve->next = voicelist;
    voicelist = newve;
    return 0;
}

/**
 * add_voice_force: Registers a mapping pattern (regex=>replacement)
 * that overrides what the voice engine speaks when a message is matched.
 * Supports PCRE2 backreferences ($1, \1, etc.) in the replacement text.
 */
add_voice_force(const char *pattern)
{
    struct voice_force *newvf;
    char *sep = strstr(pattern, "=>");
    char *match_pattern;
    char *speak_text = NULL;
    pcre2_code *re;

    if (sep) {
        size_t len = sep - pattern;
        match_pattern = (char *) alloc(len + 1);
        memcpy(match_pattern, pattern, len);
        match_pattern[len] = '\0';
        speak_text = dupstr(sep + 2);
    } else {
        match_pattern = dupstr(pattern);
    }

    re = compile_re(match_pattern);
    if (!re) {
        free((genericptr_t) match_pattern);
        if (speak_text)
            free((genericptr_t) speak_text);
        return 1;
    }

    newvf = (struct voice_force *) alloc(sizeof *newvf);
    newvf->pattern = match_pattern;
    newvf->speak_text = speak_text;
    newvf->re = re;
    newvf->next = forcelist;
    forcelist = newvf;
    return 0;
}

/**
 * free_voice_data: Cleans up all voice lists and compiled regex memory.
 * Called during game shutdown or reload.
 */
free_voice_data(void)
{
    struct voice_exception *ve, *nextve;
    struct voice_force *vf, *nextvf;

    for (ve = voicelist; ve; ve = nextve) {
        nextve = ve->next;
        pcre2_code_free(ve->re);
        free((genericptr_t) ve->pattern);
        free((genericptr_t) ve);
    }
    voicelist = NULL;

    for (vf = forcelist; vf; vf = nextvf) {
        nextvf = vf->next;
        pcre2_code_free(vf->re);
        free((genericptr_t) vf->pattern);
        if (vf->speak_text)
            free((genericptr_t) vf->speak_text);
        free((genericptr_t) vf);
    }
    forcelist = NULL;
}

/**
 * strip_voice_patterns: Strips trailing UI hints (like "[ynq]" or "(n)")
 * and associated whitespace from game messages before processing for speech.
 */
strip_voice_patterns(const char *message)
{
    static char result[BUFSZ];
    static pcre2_code *strip_re = NULL;
    pcre2_match_data *match_data;
    int rc;

    if (!strip_re) {
        /* Pattern to match trailing [ynq] (n) etc. */
        strip_re = compile_re("\\[.*\\]|\\(.*\\)");
    }

    strncpy(result, message, BUFSZ - 1);
    result[BUFSZ - 1] = '\0';

    if (!strip_re)
        return result;

    match_data = pcre2_match_data_create_from_pattern(strip_re, NULL);
    rc = pcre2_match(strip_re, (PCRE2_SPTR) result, PCRE2_ZERO_TERMINATED, 0,
                     0, match_data, NULL);

    if (rc >= 0) {
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        /* Truncate at the first match */
        if (ovector[0] != PCRE2_UNSET) {
            result[ovector[0]] = '\0';
            /* Strip trailing spaces */
            int i = (int) strlen(result) - 1;
            while (i >= 0 && isspace((unsigned char) result[i])) {
                result[i--] = '\0';
            }
        }
    }

    pcre2_match_data_free(match_data);
    return result;
}

/**
 * handle_voice_output: Main entry point for voice processing.
 * 1. Checks VOICE_FORCE overrides.
 * 2. Checks VOICE_EXCEPTION silencers.
 * 3. Dispatches speech to the OS shell wrapper.
 * Uses a cross-platform approach: 'start /B' on Windows, 'say' on macOS,
 * and 'flock' on Linux to ensure serialized, non-overlapping speech.
 */
handle_voice_output(const char *message)
{
    char sayit[BUFSZ * 3];
    char escaped_message[BUFSZ * 2];
    int i, j = 0;
    const char *stripped = strip_voice_patterns(message);
    pcre2_match_data *match_data;
    int rc;

    /* Escape double quotes for shell command */
    for (i = 0; stripped[i] && j < (int) sizeof(escaped_message) - 2; i++) {
        if (stripped[i] == '"')
            escaped_message[j++] = '\\';
        escaped_message[j++] = stripped[i];
    }
    escaped_message[j] = '\0';

    /* 1. Check VOICE_FORCE */
    struct voice_force *vf;
    for (vf = forcelist; vf; vf = vf->next) {
        match_data = pcre2_match_data_create_from_pattern(vf->re, NULL);
        rc = pcre2_match(vf->re, (PCRE2_SPTR) stripped, PCRE2_ZERO_TERMINATED,
                         0, 0, match_data, NULL);
        if (rc >= 0) {
            char final_text[BUFSZ * 2];
            if (vf->speak_text) {
                /* Handle backreferences $1, $2 or \1, \2 */
                /* For now, simplistic replacement or full message */
                /* Actually let's use pcre2_substitute if we want power,
                   but a simple case is often enough. */
                PCRE2_SIZE outlen = sizeof(final_text);
                int sub_rc = pcre2_substitute(
                    vf->re, (PCRE2_SPTR) stripped, PCRE2_ZERO_TERMINATED, 0,
                    PCRE2_SUBSTITUTE_EXTENDED | PCRE2_SUBSTITUTE_GLOBAL,
                    match_data, NULL, (PCRE2_SPTR) vf->speak_text,
                    PCRE2_ZERO_TERMINATED, (PCRE2_UCHAR *) final_text,
                    &outlen);
                if (sub_rc >= 0) {
                    final_text[outlen] = '\0';
                } else {
                    strncpy(final_text, escaped_message,
                            sizeof(final_text) - 1);
                }
            } else {
                strncpy(final_text, escaped_message, sizeof(final_text) - 1);
            }

            pcre2_match_data_free(match_data);
            if (flags.voice_engine[0]) {
#if defined(WIN32)
                snprintf(sayit, sizeof(sayit),
                         "start /B cmd /c \"echo %s | %s %s\"", final_text,
                         flags.voice_engine, flags.voice_command);
#elif defined(__APPLE__)
                snprintf(sayit, sizeof(sayit), "say \"%s\" &", final_text);
#else
                snprintf(sayit, sizeof(sayit),
                         "( flock 9; printf \"%%s\" \"%s\" | %s %s ) "
                         "9>/tmp/nethack_voice.lock &",
                         final_text, flags.voice_engine, flags.voice_command);
#endif
            } else {
                /* fallback to espeak if engine not set */
#if defined(WIN32)
                snprintf(sayit, sizeof(sayit), "start /B espeak %s \"%s\"",
                         flags.voice_command, final_text);
#elif defined(__APPLE__)
                snprintf(sayit, sizeof(sayit), "say %s \"%s\" &",
                         flags.voice_command, final_text);
#else
                snprintf(sayit, sizeof(sayit),
                         "( flock 9; /usr/bin/espeak %s \"%s\" ) "
                         "9>/tmp/nethack_voice.lock &",
                         flags.voice_command, final_text);
#endif
            }
            (void) system(sayit);
            return;
        }
        pcre2_match_data_free(match_data);
    }

    /* 2. Check VOICE_EXCEPTION */
    struct voice_exception *ve;
    for (ve = voicelist; ve; ve = ve->next) {
        match_data = pcre2_match_data_create_from_pattern(ve->re, NULL);
        rc = pcre2_match(ve->re, (PCRE2_SPTR) stripped, PCRE2_ZERO_TERMINATED,
                         0, 0, match_data, NULL);
        if (rc >= 0) {
            pcre2_match_data_free(match_data);
            return; /* Exception found, don't speak */
        }
        pcre2_match_data_free(match_data);
    }

    /* 3. Default voice output */
    if (flags.voice_engine[0]) {
#if defined(WIN32)
        snprintf(sayit, sizeof(sayit), "start /B cmd /c \"echo %s | %s %s\"",
                 escaped_message, flags.voice_engine, flags.voice_command);
#elif defined(__APPLE__)
        snprintf(sayit, sizeof(sayit), "say \"%s\" &", escaped_message);
#else
        snprintf(sayit, sizeof(sayit),
                 "( flock 9; printf \"%%s\" \"%s\" | %s %s ) "
                 "9>/tmp/nethack_voice.lock &",
                 escaped_message, flags.voice_engine, flags.voice_command);
#endif
    } else {
#if defined(WIN32)
        snprintf(sayit, sizeof(sayit), "start /B espeak %s \"%s\"",
                 flags.voice_command, escaped_message);
#elif defined(__APPLE__)
        snprintf(sayit, sizeof(sayit), "say %s \"%s\" &", flags.voice_command,
                 escaped_message);
#else
        snprintf(sayit, sizeof(sayit),
                 "( flock 9; /usr/bin/espeak %s \"%s\" ) "
                 "9>/tmp/nethack_voice.lock &",
                 flags.voice_command, escaped_message);
#endif
    }
    (void) system(sayit);
}

#endif /* VOICE_ENABLED */
