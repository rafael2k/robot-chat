/* The vendored libirc parser, including robot-chat's local changes. */
#include <irc/message.h>

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do {                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n",              \
                    __FILE__, __LINE__, #cond);                       \
            failures++;                                               \
        }                                                             \
    } while (0)

static irc_message_t parse(char const *line)
{
    return irc_message_parse2(line, strlen(line));
}

static char const *tag(irc_message_t m, char const *key)
{
    for (size_t i = 0; i < m->tagslen; i++) {
        if (!strcmp(m->tags[i]->key, key)) {
            return m->tags[i]->value ? m->tags[i]->value : "";
        }
    }
    return NULL;
}

int main(void)
{
    irc_message_t m;

    /* runs of spaces and leading spaces in the trailing parameter survive */
    m = parse(":alice!a@h PRIVMSG #robots :    if (x)  {   return;  }");
    CHECK(m != NULL);
    CHECK(!strcmp(m->prefix, "alice!a@h"));
    CHECK(!strcmp(m->command, "PRIVMSG"));
    CHECK(m->argslen == 2);
    CHECK(!strcmp(m->args[0], "#robots"));
    CHECK(!strcmp(m->args[1], "    if (x)  {   return;  }"));
    irc_message_unref(m);

    /* a ':' inside the trailing parameter is kept */
    m = parse(":a!b@c PRIVMSG bot :see http://x :y");
    CHECK(m->argslen == 2 && !strcmp(m->args[1], "see http://x :y"));
    irc_message_unref(m);

    /* empty trailing parameter is an argument, not dropped */
    m = parse(":a!b@c PRIVMSG #c :");
    CHECK(m->argslen == 2 && !strcmp(m->args[1], ""));
    irc_message_unref(m);

    /* tags with escapes, batch and a value-less tag */
    m = parse("@time=2026-11-02T23:00:00.000Z;account=alice;batch=x1;"
              "draft/multiline-concat;msg=a\\sb\\:c "
              ":alice!a@h PRIVMSG #robots :part two");
    CHECK(!strcmp(tag(m, "account"), "alice"));
    CHECK(!strcmp(tag(m, "batch"), "x1"));
    CHECK(tag(m, "draft/multiline-concat") != NULL);
    CHECK(!strcmp(tag(m, "msg"), "a b;c"));
    CHECK(!strcmp(m->args[1], "part two"));
    irc_message_unref(m);

    /* middle parameters, no trailing */
    m = parse(":srv 324 robot #modetest +Cint");
    CHECK(m->argslen == 3 && !strcmp(m->args[2], "+Cint"));
    irc_message_unref(m);

    /* PING token */
    m = parse("PING :juba.robotchat");
    CHECK(!strcmp(m->command, "PING") && m->argslen == 1 &&
          !strcmp(m->args[0], "juba.robotchat"));
    irc_message_unref(m);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_parse: ok");
    return 0;
}
