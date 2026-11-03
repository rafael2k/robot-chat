#ifndef RC_SESSION_H
#define RC_SESSION_H

#include "conf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    EV_MESSAGE,  /* PRIVMSG, multiline batches already joined */
    EV_NOTICE,
    EV_INVITE,   /* text: the channel */
    EV_KICK,     /* we were kicked; text: reason */
    EV_ERROR,    /* server refused something we did */
} ev_type_t;

typedef struct {
    uint64_t id;
    char time[32];
    ev_type_t type;
    char *from;    /* nick or server name */
    char *account; /* sender's account (account-tag), NULL if unknown */
    char *target;  /* channel, our nick for private messages, NULL for errors */
    char *text;
    bool private_; /* sent to us, not to a channel */
    bool mention;  /* text mentions our nick */
} event_t;

typedef struct session session_t;
typedef void (*event_cb_t)(session_t *s, event_t const *e, void *arg);

session_t *session_new(conf_t const *c);
void session_free(session_t *s);
void session_set_event_cb(session_t *s, event_cb_t cb, void *arg);

/* Main loop hooks: poll session_fd() (when >= 0) for input, wait at most
 * session_timeout_ms() (-1 = no deadline), then call session_tick(). It
 * connects and reconnects, reads, handles and sends. */
int session_fd(session_t *s);
int session_timeout_ms(session_t *s);
void session_tick(session_t *s, bool readable);

bool session_ready(session_t *s);
bool session_outq_empty(session_t *s);
void session_quit(session_t *s, char const *reason);

/* Queue text for target; newlines are kept (draft/multiline when the server
 * has it, one line per message otherwise). 0 or -1 with err. */
int session_send_text(session_t *s, char const *target, char const *text,
                      size_t *lines_out, char *err, size_t errlen);
int session_join(session_t *s, char const *channel, char *err, size_t errlen);
int session_part(session_t *s, char const *channel, char const *reason,
                 char *err, size_t errlen);

bool session_is_channel(char const *name);
bool session_in_channel(session_t *s, char const *channel);
char const *session_nick(session_t *s);
char const *session_state(session_t *s);
char const *session_last_error(session_t *s);
char * const *session_channels(session_t *s, size_t *n);
bool session_multiline(session_t *s, size_t *max_bytes, size_t *max_lines);

/* history: ids are consecutive; ids <= oldest-1 are gone */
uint64_t session_last_id(session_t *s);
uint64_t session_oldest_id(session_t *s);
event_t const *session_event(session_t *s, uint64_t id);

char const *event_type_name(ev_type_t t);

#endif
