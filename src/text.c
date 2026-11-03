#include "text.h"
#include "util.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char *buf;
    size_t len, cap;
} buf_t;

static void buf_add(buf_t *b, char const *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->buf = xrealloc(b->buf, b->cap);
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

/* Length of the valid UTF-8 sequence at s (n bytes left), or 0 if invalid. */
static size_t utf8_seq(unsigned char const *s, size_t n)
{
    size_t len;
    uint32_t cp;

    if (s[0] < 0x80) {
        return 1;
    } else if ((s[0] & 0xe0) == 0xc0) {
        len = 2;
        cp = s[0] & 0x1f;
    } else if ((s[0] & 0xf0) == 0xe0) {
        len = 3;
        cp = s[0] & 0x0f;
    } else if ((s[0] & 0xf8) == 0xf0) {
        len = 4;
        cp = s[0] & 0x07;
    } else {
        return 0;
    }
    if (len > n) {
        return 0;
    }
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xc0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    /* overlong forms, surrogates, out of range */
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) || cp > 0x10ffff ||
        (cp >= 0xd800 && cp <= 0xdfff)) {
        return 0;
    }
    return len;
}

static bool keep_control(unsigned char c)
{
    switch (c) {
    case '\t':
    case 0x02: /* bold */
    case 0x03: /* colour */
    case 0x04: /* hex colour */
    case 0x0f: /* reset */
    case 0x11: /* monospace */
    case 0x16: /* reverse */
    case 0x1d: /* italic */
    case 0x1e: /* strikethrough */
    case 0x1f: /* underline */
        return true;
    default:
        return false;
    }
}

static void add_line(textlines_t *t, char const *s, size_t n, bool concat)
{
    t->lines = xrealloc(t->lines, (t->n + 1) * sizeof(char *));
    t->concat = xrealloc(t->concat, (t->n + 1) * sizeof(bool));
    t->lines[t->n] = xmalloc(n + 1);
    memcpy(t->lines[t->n], s, n);
    t->lines[t->n][n] = '\0';
    t->concat[t->n] = concat;
    t->n++;
}

/* Cut one clean logical line (no newlines) into pieces of at most max. */
static void split_line(textlines_t *t, char const *s, size_t len, size_t max)
{
    size_t pos = 0;
    bool concat = false;

    if (len == 0) {
        add_line(t, "", 0, false);
        return;
    }
    while (len - pos > max) {
        size_t cut = pos + max;

        /* back up to the start of a UTF-8 sequence */
        while (cut > pos && ((unsigned char)s[cut] & 0xc0) == 0x80) {
            cut--;
        }
        /* prefer to cut just after a space in the second half */
        for (size_t i = cut; i > pos + max / 2; i--) {
            if (s[i - 1] == ' ') {
                cut = i;
                break;
            }
        }
        if (cut == pos) {
            cut = pos + max;
        }
        add_line(t, s + pos, cut - pos, concat);
        concat = true;
        pos = cut;
    }
    add_line(t, s + pos, len - pos, concat);
}

void text_split(char const *text, size_t max_bytes, textlines_t *out)
{
    unsigned char const *s = (unsigned char const *)text;
    size_t n = strlen(text);
    buf_t line = {0};

    memset(out, 0, sizeof(*out));
    if (max_bytes < 16) {
        max_bytes = 16;
    }
    for (size_t i = 0; i <= n;) {
        size_t seq;

        if (i == n || s[i] == '\n' || s[i] == '\r') {
            split_line(out, line.buf ? line.buf : "", line.len, max_bytes);
            line.len = 0;
            if (i == n) {
                break;
            }
            if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') {
                i++;
            }
            i++;
            continue;
        }
        if (s[i] < 0x20 || s[i] == 0x7f) {
            if (keep_control(s[i])) {
                buf_add(&line, (char const *)s + i, 1);
            }
            i++;
            continue;
        }
        seq = utf8_seq(s + i, n - i);
        if (seq == 0) {
            buf_add(&line, "\xef\xbf\xbd", 3);
            i++;
        } else {
            buf_add(&line, (char const *)s + i, seq);
            i += seq;
        }
    }
    free(line.buf);
    /* a trailing newline should not send an empty line */
    while (out->n > 0 && out->lines[out->n - 1][0] == '\0') {
        free(out->lines[--out->n]);
    }
}

void textlines_free(textlines_t *t)
{
    for (size_t i = 0; i < t->n; i++) {
        free(t->lines[i]);
    }
    free(t->lines);
    free(t->concat);
    memset(t, 0, sizeof(*t));
}

static bool nick_char(unsigned char c)
{
    /* c != 0: strchr() would match the terminator */
    return isalnum(c) || c >= 0x80 || (c != 0 && strchr("-_[]\\`^{}|", c) != NULL);
}

bool text_mentions(char const *text, char const *nick)
{
    size_t nl = nick ? strlen(nick) : 0;

    if (nl == 0) {
        return false;
    }
    for (char const *p = text; *p; p++) {
        if (strncasecmp(p, nick, nl) == 0 &&
            (p == text || !nick_char((unsigned char)p[-1])) &&
            !nick_char((unsigned char)p[nl])) {
            return true;
        }
    }
    return false;
}
