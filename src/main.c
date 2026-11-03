#define _GNU_SOURCE
#include "conf.h"
#include "mcp.h"
#include "session.h"
#include "util.h"

#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CLI_TIMEOUT_MS 20000

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static void usage(FILE *f)
{
    fputs("usage: robot-chat [-c CONFIG] mcp\n"
          "       robot-chat [-c CONFIG] send TARGET [TEXT]   (TEXT from stdin if omitted or -)\n"
          "       robot-chat [-c CONFIG] listen\n"
          "\n"
          "CONFIG defaults to $ROBOT_CHAT_CONFIG or ~/.config/robot-chat/robot-chat.conf.\n"
          "mcp     MCP server on stdin/stdout for Claude Code, opencode and other agents\n"
          "send    connect, send one message (newlines kept), disconnect\n"
          "listen  stay connected and print incoming messages as JSON lines\n", f);
}

/* one poll + tick; returns false on a signal */
static bool step(session_t *s, int extra_timeout)
{
    struct pollfd p = { .fd = session_fd(s), .events = POLLIN };
    int t = session_timeout_ms(s);

    if (t < 0 || t > extra_timeout) {
        t = extra_timeout;
    }
    if (poll(&p, p.fd >= 0 ? 1 : 0, t) < 0 && errno != EINTR) {
        return false;
    }
    session_tick(s, p.fd >= 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)));
    return !stop;
}

typedef struct {
    char const *target;
    char error[512];
} send_ctx_t;

static void send_on_event(session_t *s, event_t const *e, void *arg)
{
    send_ctx_t *ctx = arg;

    (void)s;
    if (e->type == EV_ERROR && (e->target == NULL || streq_ci(e->target, ctx->target))) {
        snprintf(ctx->error, sizeof(ctx->error), "%s", e->text);
    }
}

static char *read_all_stdin(void)
{
    size_t len = 0, cap = 4096;
    char *buf = xmalloc(cap);
    ssize_t r;

    while ((r = read(STDIN_FILENO, buf + len, cap - len - 1)) > 0) {
        len += (size_t)r;
        if (len + 1 == cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
    }
    buf[len] = '\0';
    return buf;
}

static int run_send(conf_t const *c, char const *target, char const *text)
{
    session_t *s = session_new(c);
    send_ctx_t ctx = { .target = target };
    char err[512];
    int64_t deadline = now_ms() + CLI_TIMEOUT_MS;
    int ret = 1;

    session_set_event_cb(s, send_on_event, &ctx);
    while (!session_ready(s) && now_ms() < deadline && step(s, 500)) {
    }
    if (!session_ready(s)) {
        fprintf(stderr, "robot-chat: could not connect: %s\n", session_last_error(s));
        goto out;
    }
    if (session_is_channel(target) && !session_in_channel(s, target)) {
        if (session_join(s, target, err, sizeof(err)) < 0) {
            fprintf(stderr, "robot-chat: %s\n", err);
            goto out;
        }
        while (!session_in_channel(s, target) && !*ctx.error && now_ms() < deadline &&
               step(s, 500)) {
        }
        if (!session_in_channel(s, target)) {
            fprintf(stderr, "robot-chat: cannot join %s: %s\n", target,
                    *ctx.error ? ctx.error : "no reply from the server");
            goto out;
        }
    }
    if (session_send_text(s, target, text, NULL, err, sizeof(err)) < 0) {
        fprintf(stderr, "robot-chat: %s\n", err);
        goto out;
    }
    while (!session_outq_empty(s) && now_ms() < deadline && step(s, 500)) {
    }
    /* give the server a moment to refuse it (404 and friends) */
    for (int64_t until = now_ms() + 700; now_ms() < until && step(s, 100);) {
    }
    if (*ctx.error) {
        fprintf(stderr, "robot-chat: %s\n", ctx.error);
        goto out;
    }
    ret = session_outq_empty(s) ? 0 : 1;

out:
    session_quit(s, "done");
    session_free(s);
    return ret;
}

static void listen_on_event(session_t *s, event_t const *e, void *arg)
{
    cJSON *o = event_to_json(e);
    char *line = cJSON_PrintUnformatted(o);

    (void)s;
    (void)arg;
    puts(line);
    fflush(stdout);
    cJSON_free(line);
    cJSON_Delete(o);
}

static int run_listen(conf_t const *c)
{
    session_t *s = session_new(c);

    session_set_event_cb(s, listen_on_event, NULL);
    while (step(s, 30000)) {
    }
    session_quit(s, "listener stopped");
    session_free(s);
    return 0;
}

int main(int argc, char **argv)
{
    char *path = NULL, err[512];
    conf_t conf;
    char const *cmd;
    int opt, ret;

    while ((opt = getopt(argc, argv, "+c:hV")) != -1) {
        switch (opt) {
        case 'c':
            free(path);
            path = xstrdup(optarg);
            break;
        case 'V':
            puts("robot-chat 0.1.0");
            return 0;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (optind >= argc) {
        usage(stderr);
        return 2;
    }
    cmd = argv[optind++];
    if (path == NULL) {
        path = conf_default_path();
    }
    if (conf_load(&conf, path, err, sizeof(err)) < 0) {
        fprintf(stderr, "robot-chat: %s\n", err);
        free(path);
        return 1;
    }
    free(path);
    log_init(conf.account);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (!strcmp(cmd, "mcp")) {
        ret = mcp_run(&conf);
    } else if (!strcmp(cmd, "send")) {
        char *text;

        if (optind >= argc) {
            usage(stderr);
            conf_free(&conf);
            return 2;
        }
        if (optind + 1 >= argc || !strcmp(argv[optind + 1], "-")) {
            text = read_all_stdin();
        } else {
            text = xstrdup(argv[optind + 1]);
        }
        ret = run_send(&conf, argv[optind], text);
        free(text);
    } else if (!strcmp(cmd, "listen")) {
        ret = run_listen(&conf);
    } else {
        usage(stderr);
        ret = 2;
    }
    conf_free(&conf);
    return ret;
}
