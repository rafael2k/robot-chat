#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do {                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n",              \
                    __FILE__, __LINE__, #cond);                       \
            failures++;                                               \
        }                                                             \
    } while (0)

/* join pieces back the way a multiline-aware client does */
static char *rejoin(textlines_t const *t)
{
    size_t len = 1;
    char *out;

    for (size_t i = 0; i < t->n; i++) {
        len += strlen(t->lines[i]) + 1;
    }
    out = calloc(1, len);
    for (size_t i = 0; i < t->n; i++) {
        if (i > 0 && !t->concat[i]) {
            strcat(out, "\n");
        }
        strcat(out, t->lines[i]);
    }
    return out;
}

static void test_newlines(void)
{
    textlines_t t;

    text_split("one\ntwo\r\nthree\rfour\n\n  indented\n", 400, &t);
    CHECK(t.n == 6);
    CHECK(!strcmp(t.lines[0], "one"));
    CHECK(!strcmp(t.lines[1], "two"));
    CHECK(!strcmp(t.lines[2], "three"));
    CHECK(!strcmp(t.lines[3], "four"));
    CHECK(!strcmp(t.lines[4], ""));
    CHECK(!strcmp(t.lines[5], "  indented"));
    for (size_t i = 0; i < t.n; i++) {
        CHECK(!t.concat[i]);
    }
    textlines_free(&t);
}

static void test_controls(void)
{
    textlines_t t;

    /* NUL can't be in a C string; check the other controls and bad UTF-8 */
    text_split("a\x01" "b\x07" "c\tdi\x02" "bold\x02 \xff \xc3\xa9", 400, &t);
    CHECK(t.n == 1);
    CHECK(!strcmp(t.lines[0], "abc\tdi\x02" "bold\x02 \xef\xbf\xbd \xc3\xa9"));
    textlines_free(&t);
}

static void test_long_line(void)
{
    char buf[3000];
    textlines_t t;
    char *back;

    buf[0] = '\0';
    for (int i = 0; i < 200; i++) {
        strcat(buf, "word   ");
    }
    strcat(buf, "\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac"); /* euro signs */
    text_split(buf, 100, &t);
    CHECK(t.n > 10);
    CHECK(!t.concat[0]);
    for (size_t i = 0; i < t.n; i++) {
        CHECK(strlen(t.lines[i]) <= 100);
        if (i > 0) {
            CHECK(t.concat[i]);
        }
    }
    back = rejoin(&t);
    CHECK(!strcmp(back, buf));
    free(back);
    textlines_free(&t);
}

static void test_utf8_boundary(void)
{
    char buf[400] = "";
    textlines_t t;
    char *back;

    /* no spaces: the cut must not land inside a 3-byte sequence */
    for (int i = 0; i < 100; i++) {
        strcat(buf, "\xe2\x82\xac");
    }
    text_split(buf, 64, &t);
    for (size_t i = 0; i < t.n; i++) {
        CHECK(strlen(t.lines[i]) % 3 == 0);
    }
    back = rejoin(&t);
    CHECK(!strcmp(back, buf));
    free(back);
    textlines_free(&t);
}

static void test_trailing_and_empty(void)
{
    textlines_t t;

    text_split("hello\n\n\n", 400, &t);
    CHECK(t.n == 1);
    textlines_free(&t);
    text_split("", 400, &t);
    CHECK(t.n == 0);
    textlines_free(&t);
    text_split("\n\n", 400, &t);
    CHECK(t.n == 0);
    textlines_free(&t);
}

static void test_mentions(void)
{
    CHECK(text_mentions("claude: hi", "claude"));
    CHECK(text_mentions("hi @Claude!", "claude"));
    CHECK(text_mentions("ping claude", "claude"));
    CHECK(!text_mentions("claudette here", "claude"));
    CHECK(!text_mentions("myclaude", "claude"));
    CHECK(!text_mentions("claude_ is someone else", "claude"));
}

int main(void)
{
    test_newlines();
    test_controls();
    test_long_line();
    test_utf8_boundary();
    test_trailing_and_empty();
    test_mentions();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("test_text: ok");
    return 0;
}
