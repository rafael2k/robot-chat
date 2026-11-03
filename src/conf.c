#define _GNU_SOURCE
#include "conf.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static char *trim(char *s)
{
    char *e;

    while (isspace((unsigned char)*s)) {
        s++;
    }
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) {
        *--e = '\0';
    }
    return s;
}

static void list_set(strlist_t *l, char const *value)
{
    char *dup = xstrdup(value), *save = NULL;

    for (char *t = strtok_r(dup, ", \t", &save); t; t = strtok_r(NULL, ", \t", &save)) {
        l->v = xrealloc(l->v, (l->n + 1) * sizeof(char *));
        l->v[l->n++] = xstrdup(t);
    }
    free(dup);
}

static void list_free(strlist_t *l)
{
    for (size_t i = 0; i < l->n; i++) {
        free(l->v[i]);
    }
    free(l->v);
    l->v = NULL;
    l->n = 0;
}

static int parse_bool(char const *v, bool *out)
{
    if (!strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1") ||
        !strcasecmp(v, "on")) {
        *out = true;
    } else if (!strcasecmp(v, "no") || !strcasecmp(v, "false") ||
               !strcmp(v, "0") || !strcasecmp(v, "off")) {
        *out = false;
    } else {
        return -1;
    }
    return 0;
}

/* First line of a secret file. Warns when others can read it. */
static char *read_secret(char const *file, char *err, size_t errlen)
{
    char *path = expand_home(file);
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0;
    struct stat st;

    if (f == NULL) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        free(path);
        return NULL;
    }
    if (fstat(fileno(f), &st) == 0 && (st.st_mode & 077)) {
        logmsg("warning: %s is readable by other users (chmod 600 it)", path);
    }
    if (getline(&line, &cap, f) < 0) {
        snprintf(err, errlen, "%s: empty", path);
        free(line);
        line = NULL;
    } else {
        line[strcspn(line, "\r\n")] = '\0';
    }
    fclose(f);
    free(path);
    return line;
}

static void set_str(char **dst, char const *v)
{
    free(*dst);
    *dst = xstrdup(v);
}

int conf_load(conf_t *c, char const *path, char *err, size_t errlen)
{
    char *full = expand_home(path);
    FILE *f = fopen(full, "r");
    char *line = NULL;
    size_t cap = 0;
    int lineno = 0, ret = -1;
    char const *env;

    memset(c, 0, sizeof(*c));
    c->path = full;
    c->tls = true;
    c->wake_on = xstrdup("mention");
    c->wake_rate = 6;
    c->history = 1000;
    c->realname = xstrdup("robot-chat agent");

    if (f == NULL) {
        snprintf(err, errlen, "%s: %s", full, strerror(errno));
        return -1;
    }
    while (getline(&line, &cap, f) >= 0) {
        char *key, *value, *eq;

        lineno++;
        key = trim(line);
        if (*key == '\0' || *key == '#') {
            continue;
        }
        eq = strchr(key, '=');
        if (eq == NULL) {
            snprintf(err, errlen, "%s:%d: expected key = value", full, lineno);
            goto out;
        }
        *eq = '\0';
        key = trim(key);
        value = trim(eq + 1);

        if (!strcmp(key, "server")) {
            set_str(&c->server, value);
        } else if (!strcmp(key, "port")) {
            set_str(&c->port, value);
        } else if (!strcmp(key, "tls")) {
            if (parse_bool(value, &c->tls) < 0) {
                snprintf(err, errlen, "%s:%d: tls must be yes or no", full, lineno);
                goto out;
            }
        } else if (!strcmp(key, "ca_file")) {
            free(c->ca_file);
            c->ca_file = expand_home(value);
        } else if (!strcmp(key, "account")) {
            set_str(&c->account, value);
        } else if (!strcmp(key, "password")) {
            set_str(&c->password, value);
        } else if (!strcmp(key, "password_file")) {
            free(c->password);
            if ((c->password = read_secret(value, err, errlen)) == NULL) {
                goto out;
            }
        } else if (!strcmp(key, "nick")) {
            set_str(&c->nick, value);
        } else if (!strcmp(key, "realname")) {
            set_str(&c->realname, value);
        } else if (!strcmp(key, "channels")) {
            list_set(&c->channels, value);
        } else if (!strcmp(key, "invite_allow")) {
            list_set(&c->invite_allow, value);
        } else if (!strcmp(key, "wake_accounts")) {
            list_set(&c->wake_accounts, value);
        } else if (!strcmp(key, "wake_on")) {
            if (strcmp(value, "mention") && strcmp(value, "all") && strcmp(value, "none")) {
                snprintf(err, errlen, "%s:%d: wake_on must be mention, all or none",
                         full, lineno);
                goto out;
            }
            set_str(&c->wake_on, value);
        } else if (!strcmp(key, "wake_rate")) {
            c->wake_rate = atoi(value);
        } else if (!strcmp(key, "history")) {
            c->history = atoi(value);
        } else if (!strcmp(key, "opencode_url")) {
            set_str(&c->opencode_url, value);
        } else if (!strcmp(key, "opencode_session")) {
            set_str(&c->opencode_session, value);
        } else if (!strcmp(key, "opencode_directory")) {
            free(c->opencode_directory);
            c->opencode_directory = expand_home(value);
        } else if (!strcmp(key, "opencode_password_file")) {
            free(c->opencode_password);
            if ((c->opencode_password = read_secret(value, err, errlen)) == NULL) {
                goto out;
            }
        } else {
            snprintf(err, errlen, "%s:%d: unknown key '%s'", full, lineno, key);
            goto out;
        }
    }

    if ((env = getenv("ROBOT_CHAT_PASSWORD")) != NULL && *env) {
        set_str(&c->password, env);
    }
    if (c->server == NULL || c->account == NULL) {
        snprintf(err, errlen, "%s: server and account are required", full);
        goto out;
    }
    if (c->password == NULL) {
        snprintf(err, errlen, "%s: no password (set password_file)", full);
        goto out;
    }
    if (c->port == NULL) {
        c->port = xstrdup(c->tls ? "6697" : "6667");
    }
    if (c->nick == NULL) {
        c->nick = xstrdup(c->account);
    }
    if (c->history < 10) {
        c->history = 10;
    }
    if (c->wake_rate < 0) {
        c->wake_rate = 0;
    }
    ret = 0;

out:
    free(line);
    fclose(f);
    return ret;
}

void conf_free(conf_t *c)
{
    free(c->path);
    free(c->server);
    free(c->port);
    free(c->ca_file);
    free(c->account);
    if (c->password) {
        explicit_bzero(c->password, strlen(c->password));
    }
    free(c->password);
    free(c->nick);
    free(c->realname);
    list_free(&c->channels);
    list_free(&c->invite_allow);
    list_free(&c->wake_accounts);
    free(c->wake_on);
    free(c->opencode_url);
    free(c->opencode_session);
    free(c->opencode_directory);
    free(c->opencode_password);
    memset(c, 0, sizeof(*c));
}

char *conf_default_path(void)
{
    char const *env = getenv("ROBOT_CHAT_CONFIG");

    if (env != NULL && *env) {
        return xstrdup(env);
    }
    return expand_home("~/.config/robot-chat/robot-chat.conf");
}
