# Troubleshooting

robot-chat logs to stderr as `robot-chat[<account>]: ...`. For `send` and
`listen` that is your terminal. For `mcp` it goes to the agent: Claude Code
keeps MCP server output in its debug log (`claude --debug`, then
`~/.claude/debug/`). The quickest check is often to run the same profile by
hand:

```sh
robot-chat -c ~/.config/robot-chat/claude.conf listen
```

Note that this adds a second connection on the same account; Ergo allows that
(multiclient).

Server side: `journalctl -u ergochat`.

## Connecting

**`connect 127.0.0.1:6667: Connection refused`**
Ergo is not running. `systemctl status ergochat`. If the VPN is down, Ergo
cannot bind the VPN address and keeps failing to start (see
[server setup](server-setup.md#when-the-vpn-is-down)); the localhost port is
down too until the VPN is back.

**`TLS certificate of 10.70.96.17 rejected: ... The certificate issuer is unknown.`**
The profile has no `ca_file`, or the wrong one. Point `ca_file` at the server's
private CA certificate.

**`TLS certificate of X rejected: The certificate is NOT trusted. The name in the certificate does not match the expected.`**
`server` in the profile is a name or address that is not in the certificate's
`subjectAltName`. Use one that is, or reissue the certificate.

**`SASL login as X failed: ...`**
Wrong account or password. robot-chat then waits 5 minutes before trying again,
so it does not hammer the server. Check `password_file` (first line only) and
that the account exists (`/msg NickServ INFO X` as an operator).

**`FAIL * ACCOUNT_REQUIRED: You must log in with SASL to join this server`**
The client did not log in. With robot-chat this means the server did not offer
SASL (`server does not offer SASL, cannot log in as X` just before it).

**`warning: ... is readable by other users (chmod 600 it)`**
The password file is group- or world-readable. `chmod 600` it.

**`server and account are required` / `no password (set password_file)` /
`unknown key 'x'`**
A profile problem; the message names the file and line. A comment after a
value needs whitespace before the `#` and a space after it (`tls = no  # ...`);
`#robots` is a value.

## Channels

**`not in #x: join it first`**
`irc_send` to a channel needs us in it. Call `irc_join`, or list it in
`channels =` so it is joined on connect.

**`473 #x: Cannot join channel (+i)`**
Invite-only channel and we have no invite exception. On the server:
`/mode #x +I <account>`, or invite the agent (`/invite claude #x`) and list the
channel in the agent's `invite_allow`.

**Invited, but the agent did not join**
Only channels in `invite_allow` are joined on invite. The invite still shows up
as an `invite` entry in `irc_read`.

**`404 #x: Cannot send to channel`**
Shows up as an `error` entry after `irc_send`: the channel is moderated, or we
were removed from it.

## Wake-ups

**Claude Code does not react to messages**
Work through these in order:

1. Was the session started with
   `claude --dangerously-load-development-channels server:robot-chat`, and did
   you accept the development-channel dialog? Without it, the events are
   dropped silently. It does not work with `claude -p`.
2. Is the MCP server named `robot-chat` (`claude mcp get robot-chat`)? The name
   after `server:` must match.
3. Is the sender's **account** in `wake_accounts`? Nicks do not count, and the
   agent's own account never wakes it.
4. In a channel, with `wake_on = mention`, the message must contain the
   agent's nick as a word (`claude: ...`, `@claude`). Private messages always
   wake.
5. More than `wake_rate` wake-ups in a minute are skipped, with
   `not waking for message N from X: over wake_rate` in the log. The messages
   are still in `irc_read`.

On claude.ai Team and Enterprise plans an admin must enable channels first.

**opencode does not react**

1. Is opencode listening on `opencode_url`? Start it with `opencode --port 4096`
   (the TUI picks a random port otherwise). `curl http://127.0.0.1:4096/session`
   should list sessions.
2. The log says `opencode wake failed: ...` or `opencode wake skipped: ...`
   with the reason (connection refused, HTTP status, no session).
3. If `OPENCODE_SERVER_PASSWORD` is set, put the password in a file and set
   `opencode_password_file`.

**opencode wakes the wrong session**
`GET /session` lists sessions of every directory, newest first. Set
`opencode_directory` to the project's directory, or `opencode_session` to a
fixed session id.

**`irc_wait` fails after 30 s in opencode**
opencode's MCP `timeout` also limits tool calls. Set it above the longest wait,
e.g. `"timeout": 660000`.

**Two agents keep answering each other**
Each agent only wakes for accounts in its `wake_accounts`, so this needs one
listed in the other's. Remove it, use `wake_on = none` for channels, or lower
`wake_rate`.

## Messages

**Multi-line messages show up as separate lines in my IRC client**
Your client does not support `draft/multiline`; the server falls back to one
line per message for it. Agents always get the joined text.

**Indentation or spacing is wrong**
robot-chat keeps spaces and tabs exactly in both directions. If text looks
wrong only in one IRC client, it is probably that client's rendering.

## WhatsApp

**`the WhatsApp daemon is not running`** (from `wa_*` tools or the CLI)
`systemctl --user status robot-chat-whatsapp`, and
`journalctl --user -u robot-chat-whatsapp`.

**`logged out by WhatsApp ... the device was unlinked`** (exit status 3)
The device was removed on the phone, or the phone was offline for about two
weeks. Pair again: `robot-chat-whatsapp serve -phone +<number>`, then start the
service.

**`another client connected with this device (stream replaced)`**
Two processes used the same store. Run only the service; `pair` and `unlink`
refuse while it runs.

**The phone will not read the QR code**
Use a pairing code instead (`-phone`): Link a device → *Link with phone number
instead*.

**`chats -recent` shows nothing**
Activity times come from the history sync right after pairing (only the process
that paired gets it) and from live messages. Pair with `serve -phone`, or wait.

**`chat 'x' is read-only`**
Set it to `rw` in the profile and restart the service.

**`over send_rate`**
More than `send_rate` messages to WhatsApp in a minute, all agents together.
