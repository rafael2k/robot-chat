#define _GNU_SOURCE
#include "net.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <gnutls/gnutls.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct net {
    int fd;
    bool tls;
    gnutls_session_t session;
    gnutls_certificate_credentials_t cred;
};

static int tcp_connect(char const *host, char const *port, int timeout_ms,
                       char *err, size_t errlen)
{
    struct addrinfo hint = {0}, *res = NULL, *ai;
    int fd = -1, r;

    hint.ai_socktype = SOCK_STREAM;
    hint.ai_family = AF_UNSPEC;
    if ((r = getaddrinfo(host, port, &hint, &res)) != 0) {
        snprintf(err, errlen, "resolve %s: %s", host, gai_strerror(r));
        return -1;
    }
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        struct pollfd p;
        int soerr = 0;
        socklen_t sl = sizeof(soerr);

        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
                    ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        if (errno == EINPROGRESS) {
            p.fd = fd;
            p.events = POLLOUT;
            r = poll(&p, 1, timeout_ms);
            if (r == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 &&
                soerr == 0) {
                break;
            }
            errno = r == 0 ? ETIMEDOUT : soerr;
        }
        snprintf(err, errlen, "connect %s:%s: %s", host, port, strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };

        /* blocking from here on, but never stuck forever on a dead peer */
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    return fd;
}

static bool is_ip_literal(char const *host)
{
    unsigned char buf[sizeof(struct in6_addr)];

    return inet_pton(AF_INET, host, buf) == 1 || inet_pton(AF_INET6, host, buf) == 1;
}

static int tls_start(net_t *n, char const *host, char const *ca_file, int timeout_ms,
                     char *err, size_t errlen)
{
    int r;

    gnutls_certificate_allocate_credentials(&n->cred);
    if (ca_file != NULL) {
        r = gnutls_certificate_set_x509_trust_file(n->cred, ca_file, GNUTLS_X509_FMT_PEM);
        if (r <= 0) {
            snprintf(err, errlen, "CA file %s: %s", ca_file,
                     r == 0 ? "no certificates" : gnutls_strerror(r));
            return -1;
        }
    } else if (gnutls_certificate_set_x509_system_trust(n->cred) <= 0) {
        snprintf(err, errlen, "no system CA certificates");
        return -1;
    }
    gnutls_init(&n->session, GNUTLS_CLIENT);
    gnutls_set_default_priority(n->session);
    gnutls_credentials_set(n->session, GNUTLS_CRD_CERTIFICATE, n->cred);
    if (!is_ip_literal(host)) {
        /* SNI must not carry an IP address */
        gnutls_server_name_set(n->session, GNUTLS_NAME_DNS, host, strlen(host));
    }
    /* checks the chain and that the certificate matches host (DNS or IP SAN) */
    gnutls_session_set_verify_cert(n->session, host, 0);
    gnutls_transport_set_int(n->session, n->fd);
    gnutls_handshake_set_timeout(n->session, timeout_ms);

    do {
        r = gnutls_handshake(n->session);
    } while (r < 0 && !gnutls_error_is_fatal(r));

    if (r == GNUTLS_E_CERTIFICATE_VERIFICATION_ERROR) {
        gnutls_datum_t out = {0};
        unsigned status = gnutls_session_get_verify_cert_status(n->session);

        gnutls_certificate_verification_status_print(
            status, gnutls_certificate_type_get(n->session), &out, 0);
        snprintf(err, errlen, "TLS certificate of %s rejected: %s", host,
                 out.data ? (char const *)out.data : "unknown reason");
        gnutls_free(out.data);
        return -1;
    } else if (r < 0) {
        snprintf(err, errlen, "TLS handshake with %s: %s", host, gnutls_strerror(r));
        return -1;
    }
    return 0;
}

net_t *net_connect(char const *host, char const *port, bool tls,
                   char const *ca_file, int timeout_ms,
                   char *err, size_t errlen)
{
    net_t *n = xcalloc(1, sizeof(*n));

    n->fd = tcp_connect(host, port, timeout_ms, err, errlen);
    if (n->fd < 0) {
        free(n);
        return NULL;
    }
    n->tls = tls;
    if (tls && tls_start(n, host, ca_file, timeout_ms, err, errlen) < 0) {
        net_close(n);
        return NULL;
    }
    return n;
}

int net_fd(net_t *n)
{
    return n ? n->fd : -1;
}

bool net_pending(net_t *n)
{
    return n != NULL && n->tls && n->session != NULL &&
           gnutls_record_check_pending(n->session) > 0;
}

ssize_t net_read(net_t *n, void *buf, size_t len)
{
    ssize_t r;

    if (n->tls) {
        do {
            r = gnutls_record_recv(n->session, buf, len);
        } while (r == GNUTLS_E_AGAIN || r == GNUTLS_E_INTERRUPTED);
        if (r == GNUTLS_E_PREMATURE_TERMINATION) {
            return 0;
        }
        return r < 0 ? -1 : r;
    }
    do {
        r = recv(n->fd, buf, len, 0);
    } while (r < 0 && errno == EINTR);
    return r;
}

int net_write(net_t *n, void const *buf, size_t len)
{
    char const *p = buf;

    while (len > 0) {
        ssize_t r;

        if (n->tls) {
            do {
                r = gnutls_record_send(n->session, p, len);
            } while (r == GNUTLS_E_AGAIN || r == GNUTLS_E_INTERRUPTED);
        } else {
            do {
                r = send(n->fd, p, len, MSG_NOSIGNAL);
            } while (r < 0 && errno == EINTR);
        }
        if (r <= 0) {
            return -1;
        }
        p += r;
        len -= (size_t)r;
    }
    return 0;
}

void net_close(net_t *n)
{
    if (n == NULL) {
        return;
    }
    if (n->session != NULL) {
        gnutls_bye(n->session, GNUTLS_SHUT_WR);
        gnutls_deinit(n->session);
    }
    if (n->cred != NULL) {
        gnutls_certificate_free_credentials(n->cred);
    }
    if (n->fd >= 0) {
        close(n->fd);
    }
    free(n);
}
