#define _GNU_SOURCE
#include "http.h"
#include "net.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define HTTP_TIMEOUT_MS 5000
#define HTTP_MAX_BODY   (4 << 20)

static char *dechunk(char const *p, size_t len)
{
    char *out = xmalloc(len + 1);
    size_t o = 0;
    char const *end = p + len;

    while (p < end) {
        char *next;
        unsigned long n = strtoul(p, &next, 16);
        char const *data = strstr(next, "\r\n");

        if (data == NULL || n == 0) {
            break;
        }
        data += 2;
        if (data + n > end) {
            n = (unsigned long)(end - data);
        }
        memcpy(out + o, data, n);
        o += n;
        p = data + n + 2;
    }
    out[o] = '\0';
    return out;
}

int http_request(char const *base_url, char const *method, char const *path,
                 char const *json_body, char const *user, char const *password,
                 char **body_out, char *err, size_t errlen)
{
    char host[256], port[16] = "80", *auth = NULL, *req, *resp = NULL, *hdr_end;
    char const *p;
    size_t rlen = 0, rcap = 0;
    net_t *n;
    int status = -1;

    if (body_out != NULL) {
        *body_out = NULL;
    }
    if (strncmp(base_url, "http://", 7) != 0) {
        snprintf(err, errlen, "only http:// URLs are supported: %s", base_url);
        return -1;
    }
    p = base_url + 7;
    snprintf(host, sizeof(host), "%.*s", (int)strcspn(p, ":/"), p);
    p += strcspn(p, ":/");
    if (*p == ':') {
        snprintf(port, sizeof(port), "%.*s", (int)strcspn(p + 1, "/"), p + 1);
    }

    if (password != NULL && *password) {
        char *cred = xasprintf("%s:%s", user ? user : "opencode", password);

        auth = base64_encode((unsigned char *)cred, strlen(cred));
        explicit_bzero(cred, strlen(cred));
        free(cred);
    }
    req = xasprintf("%s %s HTTP/1.1\r\nHost: %s:%s\r\nConnection: close\r\n"
                    "Accept: application/json\r\n%s%s%s"
                    "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
                    method, path, host, port,
                    auth ? "Authorization: Basic " : "", auth ? auth : "",
                    auth ? "\r\n" : "",
                    json_body ? strlen(json_body) : 0, json_body ? json_body : "");
    free(auth);

    n = net_connect(host, port, false, NULL, HTTP_TIMEOUT_MS, err, errlen);
    if (n == NULL) {
        free(req);
        return -1;
    }
    if (net_write(n, req, strlen(req)) < 0) {
        snprintf(err, errlen, "write to %s failed", base_url);
        goto out;
    }
    for (;;) {
        ssize_t r;

        if (rlen + 4096 + 1 > rcap) {
            rcap = (rlen + 4096 + 1) * 2;
            resp = xrealloc(resp, rcap);
        }
        r = net_read(n, resp + rlen, rcap - rlen - 1);
        if (r <= 0) {
            break;
        }
        rlen += (size_t)r;
        if (rlen > HTTP_MAX_BODY) {
            snprintf(err, errlen, "response from %s too large", base_url);
            goto out;
        }
    }
    if (resp == NULL || sscanf(resp, "HTTP/%*s %d", &status) != 1) {
        snprintf(err, errlen, "no HTTP response from %s", base_url);
        status = -1;
        goto out;
    }
    resp[rlen] = '\0';
    hdr_end = strstr(resp, "\r\n\r\n");
    if (body_out != NULL && hdr_end != NULL) {
        char const *body = hdr_end + 4;
        size_t blen = rlen - (size_t)(body - resp);
        bool chunked = false;

        *hdr_end = '\0';
        for (char *h = strcasestr(resp, "\r\ntransfer-encoding:"); h; h = NULL) {
            chunked = strcasestr(h, "chunked") != NULL &&
                      strcasestr(h, "chunked") < h + strcspn(h + 2, "\r") + 2;
        }
        *body_out = chunked ? dechunk(body, blen) : strndup(body, blen);
    }

out:
    free(resp);
    free(req);
    net_close(n);
    return status;
}
