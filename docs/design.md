# Design

robot-chat is one process per IRC account. `robot-chat mcp` is started by the
agent (Claude Code, opencode) as an MCP stdio server; it holds the IRC
connection for as long as the agent session lives.

```
 agent  <-- MCP JSON-RPC on stdin/stdout -->  robot-chat mcp  <-- IRC (TLS) -->  Ergo
   ^                                            |
   |  Claude Code: notifications/claude/channel |
   +--------------------------------------------+
   |  opencode:    POST /session/<id>/prompt_async (HTTP, localhost)
```

## Source layout

| File | |
| --- | --- |
| `src/main.c` | command line: `mcp`, `send`, `listen` |
| `src/conf.c` | profile files (`key = value`) |
| `src/net.c` | TCP connect with timeout, GnuTLS with certificate and name checks |
| `src/session.c` | the IRC client: registration, CAP/SASL, send queue, incoming messages, history |
| `src/text.c` | cleaning and splitting outgoing text, mention detection |
| `src/mcp.c` | MCP server: tools, pending waits and joins, wake-ups |
| `src/http.c` | minimal HTTP/1.1 client for opencode's local API |
| `third_party/libirc` | message parser (vendored, two local fixes) |
| `third_party/cjson` | JSON |

Everything runs in one thread around `poll()` on stdin and the IRC socket.
`session_timeout_ms()` tells the loop when the session next needs attention
(reconnect, paced sends, buffered TLS data).

## Why not libirc's client

libirc was the starting point, but its client side has problems that matter
here (`third_party/README.md` lists them): the send queue is LIFO, outgoing
arguments are not checked for CR/LF, there is no SASL, TLS cannot use a private
CA, it joins any channel it is invited to, and PONG has no token. Its parser is
fine apart from two bugs that are fixed locally, so robot-chat uses only the
parser and does the rest itself.

## Connection

1. TCP connect (10 s timeout), then TLS if `tls = yes`. GnuTLS checks the chain
   against `ca_file` or the system store, and the name or IP against the
   certificate's `subjectAltName`. SNI is sent for names only.
2. `CAP LS 302`, `NICK`, `USER`. From the server's list robot-chat requests
   `sasl`, `draft/multiline`, `batch`, `message-tags`, `server-time` and
   `account-tag` if offered.
3. `AUTHENTICATE PLAIN` with the account and password (base64, in 400-byte
   pieces), then `CAP END` after `903`.
4. On `001` it joins `channels`.

PING is answered with `PONG :<token>` straight away.

Reconnects: after a lost connection or failed connect, wait 2 s, doubling up to
60 s. After a SASL failure or `ACCOUNT_REQUIRED`, wait 5 minutes. On a nick
collision during registration it appends `_`, up to 5 times. `robot-chat mcp`
waits for the client's `initialized` notification (or 3 s) before connecting,
so a slow connect cannot delay the MCP handshake.

## Sending

`session_send_text()`:

1. Checks the target: no spaces, commas, control characters, leading `:`. A
   channel must be joined.
2. Cleans and splits the text (`text.c`): newlines become line breaks; NUL,
   other C0 controls and DEL are dropped except tab and the formatting codes
   (bold, colour, italic, ...); invalid UTF-8 becomes U+FFFD; lines are cut to
   fit the line budget on UTF-8 boundaries, after a space when one is in the
   second half of the window. Each piece remembers whether it continues the
   previous one.
3. The budget is 512 bytes minus CRLF, `PRIVMSG <target> :` and the
   `:nick!user@host ` prefix the server adds when relaying (assuming a 10-byte
   user and a 64-byte host), about 400 bytes.
4. With `draft/multiline`, `batch` and `message-tags`, consecutive pieces go
   into `BATCH +rcN draft/multiline <target>` ... `BATCH -rcN`, each line
   tagged `@batch=rcN`, and continued pieces also
   `draft/multiline-concat`. A batch stops before `max-lines` or `max-bytes`.
   Blank lines are sent as empty lines inside a batch (Ergo accepts them).
5. Without multiline, one PRIVMSG per piece; an empty one becomes a space.

`queue_line()` replaces any CR or LF that still reaches it with a space, as a
last guard against command injection.

The send queue is FIFO with pacing: a burst of 5 lines, then one line per
500 ms. Registration lines, PONG and the lines inside a batch are not paced (a
batch counts as one line), so a multi-line message is not dragged out over
many seconds.

## Receiving

Lines are parsed with libirc's `irc_message_parse2()`. Tags give `time`,
`account`, `batch` and `draft/multiline-concat`.

- A `BATCH +ref draft/multiline <target>` starts collecting; its PRIVMSG lines
  are joined with `\n`, or without one when tagged
  `draft/multiline-concat`; `BATCH -ref` turns it into one `message` entry.
- `PRIVMSG` and `NOTICE` become entries. A sender without `!` in the prefix is
  the server or a service, recorded as `notice`.
- Error numerics (401, 403, 404, 405, 471, 473, 474, 475, 477, 482, ...) and
  `FAIL` become `error` entries. JOIN, PART, KICK and NICK keep the joined
  channel list and our nick up to date.
- `INVITE` is recorded, and followed only for `invite_allow` channels.

Entries go into a ring buffer of `history` entries with consecutive ids. Each
new entry runs the event callback: `mcp.c` uses it for wake-ups, `listen`
prints it.

## Wake-ups

An entry wakes the agent when all of these hold:

- it is a `message`, and it has an `account` (from `account-tag`);
- that account is in `wake_accounts` and is not our own;
- it is private, or `wake_on = all`, or `wake_on = mention` and it mentions
  our nick;
- fewer than `wake_rate` wake-ups happened in the last minute.

Then robot-chat sends `notifications/claude/channel` on stdout (Claude Code
shows it to the model only if it loaded robot-chat as a channel; other clients
ignore it), and, if `opencode_url` is set, posts a prompt to opencode.

Gating on the account matters: on an IRC server nicks are claimed, while
accounts are authenticated. On a server with strict nick reservation the two
coincide, but robot-chat does not rely on it.

## MCP details

- JSON-RPC 2.0, one message per line on stdin and stdout.
- `irc_wait` and `irc_join` are answered later: they are kept as pending
  requests and resolved after each loop iteration, when a matching entry
  arrives, the channel shows up in the joined list, an error for that channel
  arrives, or the deadline passes.
- Tool results carry JSON text and `structuredContent`.
