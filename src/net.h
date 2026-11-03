#ifndef RC_NET_H
#define RC_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct net net_t;

/* TCP connect, then TLS when tls is set. The certificate is checked against
 * ca_file (or the system store when NULL) and the host name or IP address. */
net_t *net_connect(char const *host, char const *port, bool tls,
                   char const *ca_file, int timeout_ms,
                   char *err, size_t errlen);
int net_fd(net_t *n);
/* TLS has decrypted data buffered that poll() will not report */
bool net_pending(net_t *n);
/* >0 bytes, 0 closed, -1 error */
ssize_t net_read(net_t *n, void *buf, size_t len);
/* 0 when everything was written, -1 on error */
int net_write(net_t *n, void const *buf, size_t len);
void net_close(net_t *n);

#endif
