#ifndef RC_HTTP_H
#define RC_HTTP_H

#include <stddef.h>

/* Minimal plain-HTTP client for a local server (opencode serve).
 * base_url is "http://host:port". Returns the HTTP status, or -1 with err.
 * *body_out (may be NULL) receives the response body. */
int http_request(char const *base_url, char const *method, char const *path,
                 char const *json_body, char const *user, char const *password,
                 char **body_out, char *err, size_t errlen);

#endif
