#ifndef RC_CONF_H
#define RC_CONF_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char **v;
    size_t n;
} strlist_t;

typedef struct {
    char *path;
    char *server;
    char *port;
    bool tls;
    char *ca_file;          /* NULL: system CA store */
    char *account;
    char *password;
    char *nick;
    char *realname;
    strlist_t channels;     /* joined after connecting */
    strlist_t invite_allow; /* channels we join when invited */
    strlist_t wake_accounts;/* accounts whose messages may wake the agent */
    char *wake_on;          /* "mention", "all" or "none" (channel messages) */
    int wake_rate;          /* max wakes per minute */
    int history;            /* messages kept in memory */
    char *opencode_url;     /* e.g. http://127.0.0.1:4096 */
    char *opencode_session; /* NULL: most recently updated session */
    char *opencode_directory; /* only sessions of this project directory */
    char *opencode_password;
} conf_t;

/* Load a "key = value" file. On error returns -1 with a message in err. */
int conf_load(conf_t *c, char const *path, char *err, size_t errlen);
void conf_free(conf_t *c);

/* Default config path: $ROBOT_CHAT_CONFIG or ~/.config/robot-chat/robot-chat.conf */
char *conf_default_path(void);

#endif
