/* songtext - a DS-10 song as editable text, and back again.
 *
 * Not part of the engine: the DS-10 has no text format, this one is ours. That is
 * why it lives in extras/ rather than core/, which is only ever a reimplementation
 * of what the ROM does.
 *
 * The format is `docs/songtext.md`. This is the same encoder and parser as
 * `tools/songtext.py`, and it has to agree with it byte for byte: `make
 * test-songtext` dumps every song both ways and compares. The Python one stays
 * the reference because it is where the format is worked out; this one is what
 * the engine, the native tools and the browser build can use.
 *
 * Everything here works on a payload - the 11212, 11260, 12028 or 12252 bytes
 * after a song file's 16-byte MD5 - because that is what a `raw` line's offset
 * refers to. The two ds10_song wrappers are for callers that already hold the
 * parsed struct.
 */
#ifndef DS10_SONGTEXT_H
#define DS10_SONGTEXT_H

#include <stddef.h>
#include <stdint.h>

#include "../core/song.h"

enum ds10_songtext_status {
    DS10_SONGTEXT_OK = 0,
    DS10_SONGTEXT_SYNTAX,   /* a line this format does not accept */
    DS10_SONGTEXT_SIZE,     /* no `payload` line, or one that is not a real length */
    DS10_SONGTEXT_TRUNC,    /* the output buffer was too small; nothing written */
};

/* Text for a payload. Returns the length it needs, excluding the NUL - so a
 * return of >= cap means the buffer was too small and only a truncated,
 * NUL-terminated prefix was written. `source`, if given, becomes a leading `#`
 * comment. `clean` drops the gate, velocity and pan of a step with no note:
 * dead bytes the sequencer never reads, which is what makes the text tidy at the
 * cost of not being byte-identical when it is loaded back. */
size_t ds10_songtext_write(const uint8_t *payload, size_t len,
                           const char *source, int clean,
                           char *out, size_t cap);

/* Payload for a text. Returns a ds10_songtext_status; on SYNTAX, *errline is the
 * 1-based line that failed. `cap` must be at least the payload length the
 * `payload` line asks for; *len receives it. */
int ds10_songtext_read(const char *text, uint8_t *out, size_t cap, size_t *len,
                       int *errline);

/* The same two for a parsed song. */
size_t ds10_songtext_dump(const ds10_song *s, const char *source, int clean,
                          char *out, size_t cap);
int ds10_songtext_parse(ds10_song *s, const char *text, int *errline);

#endif /* DS10_SONGTEXT_H */
