#define _GNU_SOURCE
#include "session.h"
#include "net.h"
#include "text.h"
#include "util.h"

#include <irc/message.h>
#include <irc/tag.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CONNECT_TIMEOUT_MS 10000
#define BACKOFF_MIN_S      2
#define BACKOFF_MAX_S      60
#define BACKOFF_AUTH_S     300
/* send pacing: a burst of 5 lines, then 2 lines per second */
#define TOKENS_BURST       5.0
#define TOKEN_MS           500.0
#define INBUF_SIZE         (8192 + 1024)

typedef enum { ST_OFFLINE, ST_REGISTERING, ST_READY } state_t;

typedef struct {
    char *line;  /* without CRLF */
    bool free_;  /* not paced: registration, PONG, lines inside a batch */
} outline_t;

typedef struct {
    char *ref;
    char *target, *from, *account;
    char time[32];
    char *text;
    size_t len;
    size_t lines;
} inbatch_t;

struct session {
    conf_t const *conf;
    state_t state;
    net_t *net;
    char inbuf[INBUF_SIZE];
    size_t inlen;

    outline_t *outq;
    size_t outq_head, outq_len, outq_cap;
    double tokens;
    int64_t tokens_ms;

    int64_t next_connect_ms;
    int backoff_s;
    bool quitting;
    int nick_tries;

    char *nick;
    char *caps_ls;
    bool cap_sasl, cap_multiline, cap_batch, cap_tags;
    size_t ml_max_bytes, ml_max_lines;

    char **chans;
    size_t nchans;

    event_t *ev;
    size_t ev_cap;
    uint64_t ev_next_id;

    inbatch_t *inb;
    size_t ninb;
    unsigned batch_seq;

    char last_error[512];
    event_cb_t cb;
    void *cb_arg;
};

static char const *wanted_caps[] = {
    "sasl", "draft/multiline", "batch", "message-tags", "server-time",
    "account-tag", NULL
};

/* ---- small helpers ---------------------------------------------------- */

char const *event_type_name(ev_type_t t)
{
    switch (t) {
    case EV_MESSAGE: return "message";
    case EV_NOTICE:  return "notice";
    case EV_INVITE:  return "invite";
    case EV_KICK:    return "kick";
    case EV_ERROR:   return "error";
    }
    return "unknown";
}

bool session_is_channel(char const *name)
{
    return name != NULL && (name[0] == '#' || name[0] == '&');
}

/* nick or channel we are willing to put on the wire as one parameter */
static bool valid_name(char const *s)
{
    if (s == NULL || *s == '\0' || *s == ':' || strlen(s) > 200) {
        return false;
    }
    for (; *s; s++) {
        if ((unsigned char)*s <= ' ' || *s == ',' || *s == 0x07 || *s == 0x7f) {
            return false;
        }
    }
    return true;
}

static void set_error(session_t *s, char const *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void set_error(session_t *s, char const *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s->last_error, sizeof(s->last_error), fmt, ap);
    va_end(ap);
    logmsg("%s", s->last_error);
}

static char const *tag_get(irc_message_t m, char const *key, bool *present)
{
    for (size_t i = 0; i < m->tagslen; i++) {
        if (m->tags[i]->key != NULL && !strcmp(m->tags[i]->key, key)) {
            if (present) {
                *present = true;
            }
            return m->tags[i]->value;
        }
    }
    if (present) {
        *present = false;
    }
    return NULL;
}

static char *prefix_nick(irc_message_t m)
{
    char *p;

    if (m->prefix == NULL) {
        return NULL;
    }
    p = xstrdup(m->prefix);
    p[strcspn(p, "!@")] = '\0';
    return p;
}

static char const *arg(irc_message_t m, size_t i)
{
    return i < m->argslen ? m->args[i] : "";
}

static char const *last_arg(irc_message_t m)
{
    return m->argslen > 0 ? m->args[m->argslen - 1] : "";
}

/* ---- outgoing --------------------------------------------------------- */

static void queue_line(session_t *s, bool free_, char const *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void queue_line(session_t *s, bool free_, char const *fmt, ...)
{
    va_list ap;
    char *line = NULL;

    va_start(ap, fmt);
    if (vasprintf(&line, fmt, ap) < 0) {
        abort();
    }
    va_end(ap);
    /* last line of defence: never let CR, LF or NUL split a command */
    for (char *p = line; *p; p++) {
        if (*p == '\r' || *p == '\n') {
            *p = ' ';
        }
    }
    if (s->outq_head > 0 && s->outq_head == s->outq_len) {
        s->outq_head = s->outq_len = 0;
    }
    if (s->outq_len == s->outq_cap) {
        if (s->outq_head > 0) {
            memmove(s->outq, s->outq + s->outq_head,
                    (s->outq_len - s->outq_head) * sizeof(outline_t));
            s->outq_len -= s->outq_head;
            s->outq_head = 0;
        }
        if (s->outq_len == s->outq_cap) {
            s->outq_cap = s->outq_cap ? s->outq_cap * 2 : 64;
            s->outq = xrealloc(s->outq, s->outq_cap * sizeof(outline_t));
        }
    }
    s->outq[s->outq_len].line = line;
    s->outq[s->outq_len].free_ = free_;
    s->outq_len++;
}

static void outq_clear(session_t *s)
{
    for (size_t i = s->outq_head; i < s->outq_len; i++) {
        free(s->outq[i].line);
    }
    s->outq_head = s->outq_len = 0;
}

static void disconnect(session_t *s, int retry_s);

static void refill(session_t *s)
{
    int64_t now = now_ms();

    s->tokens += (double)(now - s->tokens_ms) / TOKEN_MS;
    if (s->tokens > TOKENS_BURST) {
        s->tokens = TOKENS_BURST;
    }
    s->tokens_ms = now;
}

static void flush_out(session_t *s)
{
    refill(s);
    while (s->net != NULL && s->outq_head < s->outq_len) {
        outline_t *o = &s->outq[s->outq_head];
        char *wire;
        int r;

        if (!o->free_ && s->tokens < 1.0) {
            break;
        }
        wire = xasprintf("%s\r\n", o->line);
        r = net_write(s->net, wire, strlen(wire));
        free(wire);
        if (r < 0) {
            set_error(s, "write to server failed");
            disconnect(s, BACKOFF_MIN_S);
            return;
        }
        if (!o->free_) {
            s->tokens -= 1.0;
        }
        free(o->line);
        s->outq_head++;
    }
}

/* ---- connection ------------------------------------------------------- */

static void chans_clear(session_t *s)
{
    for (size_t i = 0; i < s->nchans; i++) {
        free(s->chans[i]);
    }
    free(s->chans);
    s->chans = NULL;
    s->nchans = 0;
}

static void inbatch_free(inbatch_t *b)
{
    free(b->ref);
    free(b->target);
    free(b->from);
    free(b->account);
    free(b->text);
}

static void inbatches_clear(session_t *s)
{
    for (size_t i = 0; i < s->ninb; i++) {
        inbatch_free(&s->inb[i]);
    }
    free(s->inb);
    s->inb = NULL;
    s->ninb = 0;
}

static void disconnect(session_t *s, int retry_s)
{
    net_close(s->net);
    s->net = NULL;
    s->state = ST_OFFLINE;
    s->inlen = 0;
    outq_clear(s);
    chans_clear(s);
    inbatches_clear(s);
    s->next_connect_ms = now_ms() + (int64_t)retry_s * 1000;
}

static void sess_connect(session_t *s)
{
    conf_t const *c = s->conf;
    char err[512] = "";

    s->net = net_connect(c->server, c->port, c->tls, c->ca_file, CONNECT_TIMEOUT_MS,
                         err, sizeof(err));
    if (s->net == NULL) {
        set_error(s, "%s", err);
        s->next_connect_ms = now_ms() + (int64_t)s->backoff_s * 1000;
        s->backoff_s = s->backoff_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : s->backoff_s * 2;
        return;
    }
    logmsg("connected to %s:%s%s", c->server, c->port, c->tls ? " (TLS)" : "");
    s->state = ST_REGISTERING;
    free(s->nick);
    s->nick = xstrdup(c->nick);
    s->nick_tries = 0;
    free(s->caps_ls);
    s->caps_ls = xstrdup("");
    s->cap_sasl = s->cap_multiline = s->cap_batch = s->cap_tags = false;
    s->ml_max_bytes = s->ml_max_lines = 0;
    s->tokens = TOKENS_BURST;
    s->tokens_ms = now_ms();

    queue_line(s, true, "CAP LS 302");
    queue_line(s, true, "NICK %s", s->nick);
    queue_line(s, true, "USER %s 0 * :%s", c->account, c->realname);
    flush_out(s);
}

/* ---- history ---------------------------------------------------------- */

static void event_clear(event_t *e)
{
    free(e->from);
    free(e->account);
    free(e->target);
    free(e->text);
    memset(e, 0, sizeof(*e));
}

static void emit(session_t *s, ev_type_t type, char const *from, char const *account,
                 char const *target, char const *text, char const *time)
{
    uint64_t id = s->ev_next_id++;
    event_t *e = &s->ev[(id - 1) % s->ev_cap];

    event_clear(e);
    e->id = id;
    if (time != NULL && *time) {
        snprintf(e->time, sizeof(e->time), "%s", time);
    } else {
        iso_time_now(e->time);
    }
    e->type = type;
    e->from = from ? xstrdup(from) : NULL;
    e->account = account && *account ? xstrdup(account) : NULL;
    e->target = target ? xstrdup(target) : NULL;
    e->text = xstrdup(text);
    e->private_ = target != NULL && !session_is_channel(target);
    e->mention = type == EV_MESSAGE && text_mentions(text, s->nick);
    if (s->cb != NULL) {
        s->cb(s, e, s->cb_arg);
    }
}

uint64_t session_last_id(session_t *s)
{
    return s->ev_next_id - 1;
}

uint64_t session_oldest_id(session_t *s)
{
    return s->ev_next_id > s->ev_cap ? s->ev_next_id - s->ev_cap : 1;
}

event_t const *session_event(session_t *s, uint64_t id)
{
    if (id < session_oldest_id(s) || id >= s->ev_next_id) {
        return NULL;
    }
    return &s->ev[(id - 1) % s->ev_cap];
}

/* ---- capabilities and login ------------------------------------------- */

/* value of cap in the LS list ("draft/multiline=max-bytes=4096,...") */
static char *cap_value(session_t *s, char const *cap)
{
    size_t cl = strlen(cap);

    for (char const *p = s->caps_ls; *p;) {
        size_t n = strcspn(p, " ");

        if (n >= cl && !strncmp(p, cap, cl) && (n == cl || p[cl] == '=')) {
            return n == cl ? xstrdup("") : strndup(p + cl + 1, n - cl - 1);
        }
        p += n;
        p += strspn(p, " ");
    }
    return NULL;
}

static void cap_request(session_t *s)
{
    char req[512] = "";

    for (size_t i = 0; wanted_caps[i] != NULL; i++) {
        char *v = cap_value(s, wanted_caps[i]);

        if (v != NULL) {
            if (*req) {
                strcat(req, " ");
            }
            strcat(req, wanted_caps[i]);
            free(v);
        }
    }
    if (strstr(req, "sasl") == NULL) {
        set_error(s, "server does not offer SASL, cannot log in as %s", s->conf->account);
    }
    if (*req) {
        queue_line(s, true, "CAP REQ :%s", req);
    } else {
        queue_line(s, true, "CAP END");
    }
}

static void multiline_limits(session_t *s)
{
    char *v = cap_value(s, "draft/multiline"), *save = NULL;

    s->ml_max_bytes = 4096;
    s->ml_max_lines = 100;
    if (v == NULL) {
        return;
    }
    for (char *t = strtok_r(v, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (!strncmp(t, "max-bytes=", 10)) {
            s->ml_max_bytes = strtoul(t + 10, NULL, 10);
        } else if (!strncmp(t, "max-lines=", 10)) {
            s->ml_max_lines = strtoul(t + 10, NULL, 10);
        }
    }
    if (s->ml_max_lines == 0) {
        s->ml_max_lines = 100;
    }
    free(v);
}

static void handle_cap(session_t *s, irc_message_t m)
{
    char const *sub = arg(m, 1);

    if (!strcasecmp(sub, "LS")) {
        bool more = m->argslen >= 4 && !strcmp(arg(m, 2), "*");
        char *all = xasprintf("%s%s%s", s->caps_ls, *s->caps_ls ? " " : "", last_arg(m));

        free(s->caps_ls);
        s->caps_ls = all;
        if (!more && s->state == ST_REGISTERING) {
            cap_request(s);
        }
    } else if (!strcasecmp(sub, "ACK")) {
        char *list = xstrdup(last_arg(m)), *save = NULL;

        for (char *t = strtok_r(list, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
            if (!strcmp(t, "sasl")) {
                s->cap_sasl = true;
            } else if (!strcmp(t, "draft/multiline")) {
                s->cap_multiline = true;
                multiline_limits(s);
            } else if (!strcmp(t, "batch")) {
                s->cap_batch = true;
            } else if (!strcmp(t, "message-tags")) {
                s->cap_tags = true;
            }
        }
        free(list);
        if (s->state == ST_REGISTERING) {
            if (s->cap_sasl) {
                queue_line(s, true, "AUTHENTICATE PLAIN");
            } else {
                queue_line(s, true, "CAP END");
            }
        }
    } else if (!strcasecmp(sub, "NAK")) {
        set_error(s, "server refused capabilities: %s", last_arg(m));
        if (s->state == ST_REGISTERING) {
            queue_line(s, true, "CAP END");
        }
    }
}

static void send_sasl_plain(session_t *s)
{
    conf_t const *c = s->conf;
    size_t al = strlen(c->account), pl = strlen(c->password);
    size_t n = 1 + al + 1 + pl;
    unsigned char *raw = xmalloc(n);
    char *b64;
    size_t len, off = 0;

    /* authzid (empty) \0 authcid \0 password */
    raw[0] = '\0';
    memcpy(raw + 1, c->account, al);
    raw[1 + al] = '\0';
    memcpy(raw + 2 + al, c->password, pl);
    b64 = base64_encode(raw, n);
    explicit_bzero(raw, n);
    free(raw);

    len = strlen(b64);
    do {
        size_t chunk = len - off > 400 ? 400 : len - off;

        queue_line(s, true, "AUTHENTICATE %.*s", (int)chunk, b64 + off);
        off += chunk;
        if (off == len && chunk == 400) {
            queue_line(s, true, "AUTHENTICATE +");
        }
    } while (off < len);
    explicit_bzero(b64, len);
    free(b64);
}

/* ---- incoming messages ------------------------------------------------ */

static void chan_add(session_t *s, char const *chan)
{
    if (session_in_channel(s, chan)) {
        return;
    }
    s->chans = xrealloc(s->chans, (s->nchans + 1) * sizeof(char *));
    s->chans[s->nchans++] = xstrdup(chan);
}

static void chan_del(session_t *s, char const *chan)
{
    for (size_t i = 0; i < s->nchans; i++) {
        if (streq_ci(s->chans[i], chan)) {
            free(s->chans[i]);
            s->chans[i] = s->chans[--s->nchans];
            return;
        }
    }
}

static inbatch_t *inbatch_find(session_t *s, char const *ref)
{
    for (size_t i = 0; ref != NULL && i < s->ninb; i++) {
        if (!strcmp(s->inb[i].ref, ref)) {
            return &s->inb[i];
        }
    }
    return NULL;
}

static void handle_batch(session_t *s, irc_message_t m)
{
    char const *ref = arg(m, 0);

    if (ref[0] == '+' && !strcmp(arg(m, 1), "draft/multiline") && m->argslen >= 3) {
        inbatch_t *b;
        char const *time = tag_get(m, "time", NULL);

        s->inb = xrealloc(s->inb, (s->ninb + 1) * sizeof(inbatch_t));
        b = &s->inb[s->ninb++];
        memset(b, 0, sizeof(*b));
        b->ref = xstrdup(ref + 1);
        b->target = xstrdup(arg(m, 2));
        b->from = prefix_nick(m);
        b->account = tag_get(m, "account", NULL) ? xstrdup(tag_get(m, "account", NULL)) : NULL;
        snprintf(b->time, sizeof(b->time), "%s", time ? time : "");
        b->text = xstrdup("");
    } else if (ref[0] == '-') {
        for (size_t i = 0; i < s->ninb; i++) {
            inbatch_t *b = &s->inb[i];

            if (!strcmp(b->ref, ref + 1)) {
                emit(s, EV_MESSAGE, b->from, b->account, b->target, b->text, b->time);
                inbatch_free(b);
                s->inb[i] = s->inb[--s->ninb];
                break;
            }
        }
    }
}

static void handle_privmsg(session_t *s, irc_message_t m, bool notice)
{
    char const *target = arg(m, 0);
    char const *text = m->argslen >= 2 ? last_arg(m) : "";
    char const *time = tag_get(m, "time", NULL);
    char const *account = tag_get(m, "account", NULL);
    inbatch_t *b = inbatch_find(s, tag_get(m, "batch", NULL));
    char *from = prefix_nick(m);
    char *action = NULL;

    if (b != NULL && !notice) {
        bool concat = false;
        size_t tl = strlen(text);

        tag_get(m, "draft/multiline-concat", &concat);
        b->text = xrealloc(b->text, b->len + tl + 2);
        if (b->lines > 0 && !concat) {
            b->text[b->len++] = '\n';
        }
        memcpy(b->text + b->len, text, tl + 1);
        b->len += tl;
        b->lines++;
        if (b->account == NULL && account != NULL) {
            b->account = xstrdup(account);
        }
        free(from);
        return;
    }
    if (text[0] == '\x01') {
        /* CTCP: keep ACTION as "/me ...", drop the rest */
        if (strncmp(text, "\x01" "ACTION ", 8) != 0) {
            free(from);
            return;
        }
        action = xasprintf("/me %s", text + 8);
        action[strcspn(action, "\x01")] = '\0';
        text = action;
    }
    if (from == NULL || strchr(m->prefix, '!') == NULL) {
        /* from the server itself or a service without a user part */
        notice = true;
    }
    emit(s, notice ? EV_NOTICE : EV_MESSAGE, from ? from : "server", account, target,
         text, time);
    free(action);
    free(from);
}

static bool is_error_numeric(char const *cmd)
{
    static char const *codes[] = {
        "401", "403", "404", "405", "406", "407", "411", "412", "417", "421",
        "441", "442", "443", "471", "473", "474", "475", "476", "477", "478",
        "482", "489", NULL
    };

    for (size_t i = 0; codes[i] != NULL; i++) {
        if (!strcmp(cmd, codes[i])) {
            return true;
        }
    }
    return false;
}

static void handle_line(session_t *s, char const *line)
{
    irc_message_t m = irc_message_parse2(line, strlen(line));
    char const *cmd;
    char *pnick;

    if (m == NULL || m->command == NULL) {
        irc_message_unref(m);
        return;
    }
    cmd = m->command;
    pnick = prefix_nick(m);

    if (!strcmp(cmd, "PING")) {
        queue_line(s, true, "PONG :%s", last_arg(m));
    } else if (!strcmp(cmd, "CAP")) {
        handle_cap(s, m);
    } else if (!strcmp(cmd, "AUTHENTICATE")) {
        if (!strcmp(arg(m, 0), "+")) {
            send_sasl_plain(s);
        }
    } else if (!strcmp(cmd, "903")) {
        queue_line(s, true, "CAP END");
    } else if (!strcmp(cmd, "902") || !strcmp(cmd, "904") || !strcmp(cmd, "905") ||
               !strcmp(cmd, "906") || !strcmp(cmd, "908")) {
        set_error(s, "SASL login as %s failed: %s", s->conf->account, last_arg(m));
        disconnect(s, BACKOFF_AUTH_S);
    } else if (!strcmp(cmd, "001")) {
        s->state = ST_READY;
        s->backoff_s = BACKOFF_MIN_S;
        free(s->nick);
        s->nick = xstrdup(arg(m, 0));
        s->last_error[0] = '\0';
        logmsg("registered as %s", s->nick);
        for (size_t i = 0; i < s->conf->channels.n; i++) {
            if (valid_name(s->conf->channels.v[i])) {
                queue_line(s, false, "JOIN %s", s->conf->channels.v[i]);
            }
        }
    } else if ((!strcmp(cmd, "433") || !strcmp(cmd, "432") || !strcmp(cmd, "437")) &&
               s->state == ST_REGISTERING) {
        if (++s->nick_tries > 5) {
            set_error(s, "no usable nick: %s", last_arg(m));
            disconnect(s, BACKOFF_MAX_S);
        } else {
            char *n = xasprintf("%s_", s->nick);

            free(s->nick);
            s->nick = n;
            queue_line(s, true, "NICK %s", s->nick);
        }
    } else if (!strcmp(cmd, "JOIN")) {
        if (streq_ci(pnick, s->nick)) {
            chan_add(s, arg(m, 0));
        }
    } else if (!strcmp(cmd, "PART")) {
        if (streq_ci(pnick, s->nick)) {
            chan_del(s, arg(m, 0));
        }
    } else if (!strcmp(cmd, "KICK")) {
        if (streq_ci(arg(m, 1), s->nick)) {
            chan_del(s, arg(m, 0));
            emit(s, EV_KICK, pnick, tag_get(m, "account", NULL), arg(m, 0),
                 m->argslen >= 3 ? last_arg(m) : "", tag_get(m, "time", NULL));
        }
    } else if (!strcmp(cmd, "NICK")) {
        if (streq_ci(pnick, s->nick)) {
            free(s->nick);
            s->nick = xstrdup(arg(m, 0));
        }
    } else if (!strcmp(cmd, "INVITE")) {
        char const *chan = arg(m, 1);

        emit(s, EV_INVITE, pnick, tag_get(m, "account", NULL), chan, chan,
             tag_get(m, "time", NULL));
        if (list_contains_ci(s->conf->invite_allow.v, s->conf->invite_allow.n, chan) &&
            valid_name(chan)) {
            queue_line(s, false, "JOIN %s", chan);
        }
    } else if (!strcmp(cmd, "PRIVMSG") || !strcmp(cmd, "NOTICE")) {
        handle_privmsg(s, m, cmd[0] == 'N');
    } else if (!strcmp(cmd, "BATCH")) {
        handle_batch(s, m);
    } else if (!strcmp(cmd, "FAIL")) {
        char *t = xasprintf("FAIL %s %s: %s", arg(m, 0), arg(m, 1), last_arg(m));

        set_error(s, "%s", t);
        emit(s, EV_ERROR, pnick, NULL, NULL, t, NULL);
        free(t);
        if (s->state == ST_REGISTERING && !strcmp(arg(m, 1), "ACCOUNT_REQUIRED")) {
            disconnect(s, BACKOFF_AUTH_S);
        }
    } else if (!strcmp(cmd, "ERROR")) {
        set_error(s, "server closed the connection: %s", last_arg(m));
        disconnect(s, s->quitting ? 0 : s->backoff_s);
    } else if (is_error_numeric(cmd)) {
        /* "<nick> <subject> :<reason>", e.g. 473 #chan :Cannot join (+i) */
        char *t = m->argslen >= 3
            ? xasprintf("%s %s: %s", cmd, arg(m, 1), last_arg(m))
            : xasprintf("%s %s", cmd, last_arg(m));

        snprintf(s->last_error, sizeof(s->last_error), "%s", t);
        emit(s, EV_ERROR, pnick, NULL, m->argslen >= 3 ? arg(m, 1) : NULL, t, NULL);
        free(t);
    }
    free(pnick);
    irc_message_unref(m);
}

static void read_input(session_t *s)
{
    do {
        ssize_t r = net_read(s->net, s->inbuf + s->inlen, sizeof(s->inbuf) - s->inlen - 1);
        char *start, *nl;

        if (r <= 0) {
            set_error(s, "connection to %s lost", s->conf->server);
            disconnect(s, s->quitting ? 0 : s->backoff_s);
            return;
        }
        s->inlen += (size_t)r;
        s->inbuf[s->inlen] = '\0';
        start = s->inbuf;
        while ((nl = memchr(start, '\n', s->inlen - (size_t)(start - s->inbuf))) != NULL) {
            *nl = '\0';
            if (nl > start && nl[-1] == '\r') {
                nl[-1] = '\0';
            }
            if (*start) {
                handle_line(s, start);
            }
            if (s->net == NULL) {
                return; /* handle_line disconnected */
            }
            start = nl + 1;
        }
        s->inlen -= (size_t)(start - s->inbuf);
        memmove(s->inbuf, start, s->inlen);
        if (s->inlen == sizeof(s->inbuf) - 1) {
            logmsg("dropping an over-long line from the server");
            s->inlen = 0;
        }
    } while (net_pending(s->net));
}

/* ---- public API ------------------------------------------------------- */

session_t *session_new(conf_t const *c)
{
    session_t *s = xcalloc(1, sizeof(*s));

    s->conf = c;
    s->state = ST_OFFLINE;
    s->backoff_s = BACKOFF_MIN_S;
    s->ev_cap = (size_t)c->history;
    s->ev = xcalloc(s->ev_cap, sizeof(event_t));
    s->ev_next_id = 1;
    s->nick = xstrdup(c->nick);
    s->caps_ls = xstrdup("");
    return s;
}

void session_free(session_t *s)
{
    if (s == NULL) {
        return;
    }
    disconnect(s, 0);
    for (size_t i = 0; i < s->ev_cap; i++) {
        event_clear(&s->ev[i]);
    }
    free(s->ev);
    free(s->outq);
    free(s->nick);
    free(s->caps_ls);
    free(s);
}

void session_set_event_cb(session_t *s, event_cb_t cb, void *arg)
{
    s->cb = cb;
    s->cb_arg = arg;
}

int session_fd(session_t *s)
{
    return net_fd(s->net);
}

int session_timeout_ms(session_t *s)
{
    int64_t now = now_ms();

    if (s->state == ST_OFFLINE) {
        if (s->quitting) {
            return -1;
        }
        return s->next_connect_ms > now ? (int)(s->next_connect_ms - now) : 0;
    }
    if (net_pending(s->net)) {
        return 0;
    }
    if (s->outq_head < s->outq_len) {
        refill(s);
        if (s->outq[s->outq_head].free_ || s->tokens >= 1.0) {
            return 0;
        }
        return (int)((1.0 - s->tokens) * TOKEN_MS) + 1;
    }
    return -1;
}

void session_tick(session_t *s, bool readable)
{
    if (s->state == ST_OFFLINE) {
        if (!s->quitting && now_ms() >= s->next_connect_ms) {
            sess_connect(s);
        }
        return;
    }
    if (readable || net_pending(s->net)) {
        read_input(s);
    }
    if (s->net != NULL) {
        flush_out(s);
    }
}

bool session_ready(session_t *s)
{
    return s->state == ST_READY;
}

bool session_outq_empty(session_t *s)
{
    return s->outq_head >= s->outq_len;
}

void session_quit(session_t *s, char const *reason)
{
    s->quitting = true;
    if (s->net != NULL) {
        queue_line(s, true, "QUIT :%s", reason ? reason : "bye");
        flush_out(s);
    }
}

static size_t chunk_budget(session_t *s, char const *target)
{
    /* 512 minus CRLF, "PRIVMSG <target> :", and the ":nick!user@host "
     * prefix the server adds when relaying (user <= 10, host <= 64) */
    long budget = 512 - 2 - 8 - (long)strlen(target) - 2 -
                  (1 + (long)strlen(s->nick) + 1 + 10 + 1 + 64 + 1);

    return budget < 64 ? 64 : (size_t)budget;
}

int session_send_text(session_t *s, char const *target, char const *text,
                      size_t *lines_out, char *err, size_t errlen)
{
    textlines_t tl;
    size_t i = 0;

    if (s->state != ST_READY) {
        snprintf(err, errlen, "not connected%s%s", *s->last_error ? ": " : "",
                 s->last_error);
        return -1;
    }
    if (!valid_name(target)) {
        snprintf(err, errlen, "invalid target '%s'", target ? target : "");
        return -1;
    }
    if (session_is_channel(target) && !session_in_channel(s, target)) {
        snprintf(err, errlen, "not in %s: join it first", target);
        return -1;
    }
    text_split(text ? text : "", chunk_budget(s, target), &tl);
    if (tl.n == 0) {
        snprintf(err, errlen, "empty message");
        textlines_free(&tl);
        return -1;
    }

    while (i < tl.n) {
        size_t j = i, bytes = 0;

        if (s->cap_multiline && s->cap_batch && s->cap_tags) {
            while (j < tl.n && j - i < s->ml_max_lines) {
                size_t add = strlen(tl.lines[j]) + (j > i ? 1 : 0);

                if (j > i && bytes + add > s->ml_max_bytes) {
                    break;
                }
                bytes += add;
                j++;
            }
        } else {
            j = i + 1;
        }

        if (j - i == 1) {
            /* an empty PRIVMSG is refused, a single space is not */
            queue_line(s, false, "PRIVMSG %s :%s", target,
                       tl.lines[i][0] ? tl.lines[i] : " ");
        } else {
            unsigned ref = ++s->batch_seq;

            queue_line(s, false, "BATCH +rc%u draft/multiline %s", ref, target);
            for (size_t k = i; k < j; k++) {
                bool concat = k > i && tl.concat[k];

                /* blank lines are fine inside a multiline batch */
                queue_line(s, true, "@batch=rc%u%s PRIVMSG %s :%s", ref,
                           concat ? ";draft/multiline-concat" : "", target,
                           tl.lines[k]);
            }
            queue_line(s, true, "BATCH -rc%u", ref);
        }
        i = j;
    }
    if (lines_out != NULL) {
        *lines_out = tl.n;
    }
    textlines_free(&tl);
    flush_out(s);
    return 0;
}

int session_join(session_t *s, char const *channel, char *err, size_t errlen)
{
    if (s->state != ST_READY) {
        snprintf(err, errlen, "not connected%s%s", *s->last_error ? ": " : "",
                 s->last_error);
        return -1;
    }
    if (!session_is_channel(channel) || !valid_name(channel)) {
        snprintf(err, errlen, "invalid channel '%s'", channel ? channel : "");
        return -1;
    }
    queue_line(s, false, "JOIN %s", channel);
    flush_out(s);
    return 0;
}

int session_part(session_t *s, char const *channel, char const *reason,
                 char *err, size_t errlen)
{
    textlines_t tl = {0};

    if (s->state != ST_READY) {
        snprintf(err, errlen, "not connected");
        return -1;
    }
    if (!session_is_channel(channel) || !valid_name(channel)) {
        snprintf(err, errlen, "invalid channel '%s'", channel ? channel : "");
        return -1;
    }
    if (reason != NULL && *reason) {
        text_split(reason, 300, &tl);
    }
    if (tl.n > 0) {
        queue_line(s, false, "PART %s :%s", channel, tl.lines[0]);
    } else {
        queue_line(s, false, "PART %s", channel);
    }
    textlines_free(&tl);
    flush_out(s);
    return 0;
}

bool session_in_channel(session_t *s, char const *channel)
{
    return list_contains_ci(s->chans, s->nchans, channel);
}

char const *session_nick(session_t *s)
{
    return s->nick;
}

char const *session_state(session_t *s)
{
    switch (s->state) {
    case ST_OFFLINE:     return "offline";
    case ST_REGISTERING: return "connecting";
    case ST_READY:       return "ready";
    }
    return "unknown";
}

char const *session_last_error(session_t *s)
{
    return s->last_error;
}

char * const *session_channels(session_t *s, size_t *n)
{
    *n = s->nchans;
    return s->chans;
}

bool session_multiline(session_t *s, size_t *max_bytes, size_t *max_lines)
{
    *max_bytes = s->ml_max_bytes;
    *max_lines = s->ml_max_lines;
    return s->cap_multiline && s->cap_batch && s->cap_tags;
}
