# Vendored code

## libirc

- Upstream: https://github.com/n0la/libirc
- Version: 0.2.2, commit `c083bf664380921f9084ea3a09e8d8d932206fe2` (2025-08-13)
- License: GPL-3.0 (`libirc/COPYING`)
- Copied with `git archive`, minus upstream's `.github/`.

robot-chat builds only the message parser and its helpers (`lib/message.c`,
`lib/tag.c`, `lib/strbuf.c`, `lib/util.c`) straight from its own
`CMakeLists.txt`. libirc's connection, send queue, TLS and config code are not
used, so the problems listed below do not affect robot-chat. libirc's own
`CMakeLists.txt` is kept for reference and still builds and passes its tests.

### Local changes

Both in `lib/message.c`, marked "robot-chat local change", and covered by
`tests/test_parse.c`:

- The trailing parameter is taken verbatim to the end of the line. Upstream
  split it on spaces and re-joined the words with single spaces, which
  collapsed runs of spaces and dropped leading ones (code and indentation in
  messages).
- An empty trailing parameter (`PRIVMSG #c :`) is kept as `""` instead of
  being dropped.

To update, replace `libirc/` with a newer `git archive`, re-apply these two
changes and update the commit above.

### Known problems in the parts robot-chat does not use

- The send queue is LIFO: `irc_queue_pop()` takes from the tail that
  `irc_queue_push()` appends to.
- `irc_message_string()` does not filter CR, LF or NUL in arguments, and only
  prefixes an argument with `:` when it contains a space.
- No SASL, PASS or CAP support; `irc_think()` sends USER and NICK itself.
- The GnuTLS backend trusts only the system CA store; the libtls backend never
  performs the handshake (`irc_ssl_client_new()` already creates the context,
  so `irc_ssl_client_connect()` returns early). CMake prefers libtls.
- The INVITE handler joins any channel and reads `args[1]` after checking for
  one argument; the PING handler answers with a bare `PONG`.
- `irc_think()` parses one line per call; `irc_client_disconnect()` frees the
  host and port so `irc_client_connect()` cannot reconnect.

## cJSON

- Upstream: https://github.com/DaveGamble/cJSON
- Version: 1.7.18, commit `acc76239bee01d8e9c858ae2cab296704e52d916` (2024-05-13)
- License: MIT (`cjson/LICENSE`)
- Only `cJSON.c` and `cJSON.h`, unmodified.
