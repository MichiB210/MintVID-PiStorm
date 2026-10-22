/* MintVID-pi P96W73: Paula sound base offset (ms, negative = sound earlier),
 * shared by the MintVID-pi menu and mrberry (which has its own copy of the
 * reader).  Stored as text in ENVARC:MintVID-pi.paula, e.g. "-350".  A
 * missing or unreadable file means the built-in -350 ms.  The per-film fine
 * value (+/- keys, .t line P:) is added on top by mrberry. */
#ifndef MR_PAULA_PREF_H
#define MR_PAULA_PREF_H

#include <dos/dos.h>
#include <exec/types.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MR_PAULA_PREF_FILE "ENVARC:MintVID-pi.paula"
#define MR_PAULA_PREF_TMP  "ENVARC:MintVID-pi.paula.tmp"
#define MR_PAULA_BASE_DEFAULT_MS (-350L)
#define MR_PAULA_BASE_MIN_MS (-1000L)
#define MR_PAULA_BASE_MAX_MS 0L

static inline long mr_paula_pref_load(void)
{
    char text[16];
    BPTR file = Open((CONST_STRPTR)MR_PAULA_PREF_FILE, MODE_OLDFILE);
    LONG got;
    long v;
    if (!file) return MR_PAULA_BASE_DEFAULT_MS;
    got = Read(file, text, (LONG)sizeof text - 1);
    Close(file);
    if (got <= 0) return MR_PAULA_BASE_DEFAULT_MS;
    text[got] = 0;
    v = strtol(text, NULL, 10);
    if (v < MR_PAULA_BASE_MIN_MS || v > MR_PAULA_BASE_MAX_MS)
        return MR_PAULA_BASE_DEFAULT_MS;
    return v;
}

static inline int mr_paula_pref_save(long ms)
{
    char text[16];
    BPTR file;
    LONG len, written;
    snprintf(text, sizeof text, "%ld\n", ms);
    len = (LONG)strlen(text);
    file = Open((CONST_STRPTR)MR_PAULA_PREF_TMP, MODE_NEWFILE);
    if (!file) return 0;
    written = Write(file, text, len);
    Close(file);
    if (written != len) {
        DeleteFile((CONST_STRPTR)MR_PAULA_PREF_TMP);
        return 0;
    }
    DeleteFile((CONST_STRPTR)MR_PAULA_PREF_FILE);
    if (!Rename((CONST_STRPTR)MR_PAULA_PREF_TMP,
                (CONST_STRPTR)MR_PAULA_PREF_FILE)) {
        DeleteFile((CONST_STRPTR)MR_PAULA_PREF_TMP);
        return 0;
    }
    return 1;
}

#endif /* MR_PAULA_PREF_H */
