#ifndef RC_UTIL_H
#define RC_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void log_init(char const *name);
void logmsg(char const *fmt, ...) __attribute__((format(printf, 1, 2)));

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(char const *s);
char *xasprintf(char const *fmt, ...) __attribute__((format(printf, 1, 2)));

/* monotonic clock in milliseconds */
int64_t now_ms(void);
/* current UTC time as 2026-11-02T23:18:02.541Z */
void iso_time_now(char out[32]);

char *base64_encode(unsigned char const *in, size_t len);
/* "~/x" -> "$HOME/x"; other paths are copied */
char *expand_home(char const *path);

bool streq_ci(char const *a, char const *b);
bool list_contains_ci(char * const *list, size_t n, char const *s);

#endif
