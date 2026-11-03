#ifndef RC_TEXT_H
#define RC_TEXT_H

#include <stdbool.h>
#include <stddef.h>

/* Text cut into lines that are safe to send as IRC message bodies. */
typedef struct {
    char **lines;
    /* concat[i]: line i continues line i-1 (it was cut for length, not at a
     * newline), which is what draft/multiline-concat marks */
    bool *concat;
    size_t n;
} textlines_t;

/* Split text for sending.
 * - "\r\n" and lone "\r" are newlines; every newline starts a new line.
 * - NUL, other C0 controls and DEL are dropped, except tab and the IRC
 *   formatting codes; invalid UTF-8 becomes U+FFFD.
 * - Lines longer than max_bytes are cut on a UTF-8 boundary, after a space
 *   when there is one in the second half of the window, so that joining
 *   the pieces gives back the original line.
 * Empty lines are kept as "". */
void text_split(char const *text, size_t max_bytes, textlines_t *out);
void textlines_free(textlines_t *t);

/* Does text mention nick as a word ("nick:", "@nick", "hi nick")? */
bool text_mentions(char const *text, char const *nick);

#endif
