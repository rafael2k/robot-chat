#define _GNU_SOURCE
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static char log_name[64] = "robot-chat";

void log_init(char const *name)
{
    snprintf(log_name, sizeof(log_name), "robot-chat[%s]", name);
}

void logmsg(char const *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: ", log_name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void oom(void)
{
    fputs("robot-chat: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);

    if (p == NULL) {
        oom();
    }
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);

    if (p == NULL) {
        oom();
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (p == NULL) {
        oom();
    }
    return p;
}

char *xstrdup(char const *s)
{
    char *d = strdup(s ? s : "");

    if (d == NULL) {
        oom();
    }
    return d;
}

char *xasprintf(char const *fmt, ...)
{
    va_list ap;
    char *s = NULL;
    int r;

    va_start(ap, fmt);
    r = vasprintf(&s, fmt, ap);
    va_end(ap);
    if (r < 0) {
        oom();
    }
    return s;
}

int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void iso_time_now(char out[32])
{
    struct timespec ts;
    struct tm tm;

    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(out + strlen(out), 32 - strlen(out), ".%03dZ",
             (int)(ts.tv_nsec / 1000000) % 1000);
}

char *base64_encode(unsigned char const *in, size_t len)
{
    static char const tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *out = xmalloc((len + 2) / 3 * 4 + 1);
    char *o = out;
    size_t i;

    for (i = 0; i + 2 < len; i += 3) {
        *o++ = tab[in[i] >> 2];
        *o++ = tab[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        *o++ = tab[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
        *o++ = tab[in[i + 2] & 63];
    }
    if (i < len) {
        *o++ = tab[in[i] >> 2];
        if (i + 1 < len) {
            *o++ = tab[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
            *o++ = tab[(in[i + 1] & 15) << 2];
        } else {
            *o++ = tab[(in[i] & 3) << 4];
            *o++ = '=';
        }
        *o++ = '=';
    }
    *o = '\0';
    return out;
}

char *expand_home(char const *path)
{
    char const *home = getenv("HOME");

    if (path[0] == '~' && path[1] == '/' && home != NULL) {
        return xasprintf("%s%s", home, path + 1);
    }
    return xstrdup(path);
}

bool streq_ci(char const *a, char const *b)
{
    return a != NULL && b != NULL && strcasecmp(a, b) == 0;
}

bool list_contains_ci(char * const *list, size_t n, char const *s)
{
    for (size_t i = 0; i < n; i++) {
        if (streq_ci(list[i], s)) {
            return true;
        }
    }
    return false;
}
