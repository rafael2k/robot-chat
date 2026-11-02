# Vendored code

## libirc

- Upstream: https://github.com/n0la/libirc
- Version: 0.2.2, commit `c083bf664380921f9084ea3a09e8d8d932206fe2` (2025-08-13)
- License: GPL-3.0 (`libirc/COPYING`)
- Copied unmodified with `git archive`, minus upstream's `.github/`.
  Fixes and workarounds live in the robot-chat client, not here.

To update, replace `libirc/` with a newer `git archive` and update the commit above.

### Building

Build against GnuTLS. `CMakeLists.txt` prefers libtls whenever
`pkg-config libtls` succeeds (Debian: `libretls-dev`), and the libtls backend
never performs the handshake (see below). Build deps: cmake, bison, flex,
pkg-config, libgnutls28-dev, libcmocka-dev (required even though only the
tests use it).

### Known issues the client has to handle

- **Send queue is LIFO.** `irc_queue_pop()` takes from the tail that
  `irc_queue_push()` appends to, so commands queued between two `irc_pop()`
  calls go out in reverse order. Pop after every queued command, or bypass the
  queue (`irc_message_string()` + `irc_client_write()`), especially for
  multi-line text and `BATCH` start/end lines.
- **No CR/LF/NUL filtering.** `irc_message_string()` copies arguments as is, so
  a newline in a message injects IRC commands. Split text on `\n` and strip
  `\r` and `\0` before building messages.
- **No SASL, PASS or CAP.** `irc_think()` sends `USER` and `NICK` on its own.
  Write `PASS account:password` (or CAP/SASL) raw with `irc_client_write()`
  right after connecting, before the first `irc_think()`.
- **TLS trusts only the system CA store** (GnuTLS backend). There is no option
  for a private CA.
- **libtls backend never connects.** `irc_ssl_client_new()` creates the TLS
  context, so `irc_ssl_client_connect()` returns success without a handshake.
- **Auto-joins on INVITE.** The built-in INVITE handler joins any channel the
  bot is invited to, and reads `args[1]` after checking only for one argument.
- **PONG has no token.** The PING handler replies with a bare `PONG`.
- **`irc_think()` parses one line per call** and returns success when there is
  nothing to parse, so call it in a loop after each read.
- **No reconnect after disconnect.** `irc_client_disconnect()` frees the stored
  host and port, so `irc_client_connect()` cannot reconnect; use
  `irc_client_connect2()`. A failed TLS handshake leaves the socket open.
- **No multiline support.** Tags are supported (`irc/tag.h`), so
  `draft/multiline` batches can be built on top.
