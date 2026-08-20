/*
 * zmachine_wrap.c
 *
 * Optional presentation-layer word wrapping for Tcl/IRC consumers.
 *
 * IRC imposes byte-oriented message limits.  The VM therefore exposes a byte
 * limit rather than a character-count limit.  The implementation preserves
 * existing story newlines, prefers ASCII whitespace as wrap points, and never
 * splits inside a UTF-8 continuation sequence.
 */

#include "zmachine_wrap.h"

#include <string.h>

/* Return nonzero when byte is a UTF-8 continuation byte (10xxxxxx). */
static int is_utf8_continuation(unsigned char byte)
{
    return (byte & 0xC0U) == 0x80U;
}

/*
 * Move a tentative split point backward until it lies on a UTF-8 character
 * boundary.  start is the beginning of the current physical line.
 */
static size_t utf8_safe_split(const char *text, size_t start, size_t split)
{
    while (split > start &&
           is_utf8_continuation((unsigned char)text[split])) {
        --split;
    }
    return split;
}

/* ASCII spaces and tabs are safe word boundaries for Z-machine text output. */
static int is_wrap_space(unsigned char byte)
{
    return byte == ' ' || byte == '\t';
}

int zmachine_wrap_output(const char *text,
                         size_t length,
                         size_t max_bytes,
                         Tcl_DString *result)
{
    size_t pos = 0U;

    if (!text || !result)
        return TCL_ERROR;

    Tcl_DStringSetLength(result, 0);

    if (max_bytes == 0U) {
        Tcl_DStringAppend(result, text, (int)length);
        return TCL_OK;
    }

    while (pos < length) {
        size_t line_end = pos;
        size_t remaining;

        /* Find the story's next existing newline, if any. */
        while (line_end < length && text[line_end] != '\n')
            ++line_end;

        remaining = line_end - pos;
        while (remaining > max_bytes) {
            size_t limit = pos + max_bytes;
            size_t split = utf8_safe_split(text, pos, limit);
            size_t whitespace;

            /*
             * Prefer the last whitespace reachable within the byte limit.
             *
             * A special case is needed when a word itself occupies exactly
             * max_bytes and the following byte is whitespace.  That boundary
             * whitespace is not part of the emitted line, but consuming it
             * prevents the next wrapped line from beginning with a space.
             */
            if (split < line_end &&
                is_wrap_space((unsigned char)text[split])) {
                whitespace = split + 1U;
            } else {
                whitespace = split;
                while (whitespace > pos &&
                       !is_wrap_space((unsigned char)text[whitespace - 1U])) {
                    --whitespace;
                }
            }

            if (whitespace > pos) {
                size_t out_end = whitespace;

                while (out_end > pos &&
                       is_wrap_space((unsigned char)text[out_end - 1U])) {
                    --out_end;
                }

                Tcl_DStringAppend(result, text + pos, (int)(out_end - pos));
                Tcl_DStringAppend(result, "\n", 1);

                pos = whitespace;
                while (pos < line_end &&
                       is_wrap_space((unsigned char)text[pos])) {
                    ++pos;
                }
            } else {
                /* A single long word: split at a UTF-8 character boundary. */
                if (split == pos) {
                    split = limit;
                    while (split < line_end &&
                           is_utf8_continuation((unsigned char)text[split])) {
                        ++split;
                    }
                }

                Tcl_DStringAppend(result, text + pos, (int)(split - pos));
                Tcl_DStringAppend(result, "\n", 1);
                pos = split;
            }

            remaining = line_end - pos;
        }

        Tcl_DStringAppend(result, text + pos, (int)(line_end - pos));
        pos = line_end;

        /* Preserve story-supplied newline boundaries exactly. */
        if (pos < length && text[pos] == '\n') {
            Tcl_DStringAppend(result, "\n", 1);
            ++pos;
        }
    }

    return TCL_OK;
}
