# WhatsApp

`robot-chat-whatsapp` (in `whatsapp/`, Go, built on
[whatsmeow](https://github.com/tulir/whatsmeow)) gives agents a WhatsApp
account's allowlisted chats, in two ways:

- **directly**, as MCP tools (`wa_read`, `wa_wait`, `wa_send`, `wa_status`);
- **over IRC**: each chat is mirrored to a channel `#wa-<name>` on the
  robot-chat server.

```
 phone (WhatsApp) <== linked device ==> robot-chat-whatsapp serve  --IRC-->  Ergo  #wa-<name>
                                              ^  unix socket (mode 600)
                         robot-chat-whatsapp mcp (one per agent session)
```

The bridge is a *linked device* of the phone, like WhatsApp Web. It is an
unofficial client: WhatsApp may ban numbers that use one, so a dedicated number
is the safer choice. A linked device can read every chat of the account; the
bridge passes on only the chats listed in its profile.

## Build and install

```sh
cd whatsapp
go build -trimpath -o ~/.local/bin/robot-chat-whatsapp .
go test ./...
```

It needs Go 1.26 and cgo (SQLite). To build against a local whatsmeow
checkout, add a `go.work` next to `go.mod` (it is ignored by git):

```
go 1.26.0

use (
	.
	/path/to/whatsmeow
)
```

## Profile

`~/.config/robot-chat/whatsapp.conf`, mode 600 (see
`examples/whatsapp.conf`):

```ini
store = ~/.local/share/robot-chat/whatsapp.db   # the linked device's keys
my_name = rafael              # how your own messages are shown
send_prefix = yes             # agents' messages start with "[claude] "
send_rate = 10                # messages to WhatsApp per minute, all agents

# chat = NAME JID [ro|rw]; nothing else is passed on
chat = team 120363000000000000@g.us ro
chat = alice 5511999999999@s.whatsapp.net rw

irc_server = 127.0.0.1
irc_account = whatsapp
irc_password_file = ~/.config/robot-chat/whatsapp.pass
irc_invite = rafael, claude, opencode         # +I on the #wa- channels
irc_send_accounts = rafael, claude, opencode  # may send to rw chats from IRC
```

- `ro` (default): agents read the chat. `rw`: agents may also send to it,
  from the account's number.
- Leave out `irc_server` for MCP only.
- Other keys: `socket` (default `$XDG_RUNTIME_DIR/robot-chat-whatsapp.sock`),
  `history` (messages kept, default 1000), `irc_port`, `irc_tls`,
  `irc_ca_file`, `irc_channel_prefix` (default `#wa-`).
- A comment can follow a value after whitespace and `# `.

The IRC account is an ordinary one (`/msg NickServ SAREGISTER whatsapp
<password>`, see [server setup](server-setup.md)). The bridge creates the
`#wa-` channels, so it holds ops there and sets `+I` for `irc_invite`.

## Pairing

One of:

```sh
robot-chat-whatsapp serve -phone +5511999999999   # pairing code, then keeps running
robot-chat-whatsapp pair -phone +5511999999999    # pairing code, then exits
robot-chat-whatsapp pair -png /run/user/1000/qr.png   # QR code (terminal and PNG)
```

With a pairing code: on the phone, WhatsApp → Settings → Linked devices →
Link a device → *Link with phone number instead*, and type the code (about 2
minutes). With a QR code, scan it on that same screen.

Prefer `serve -phone`: right after pairing the phone sends a *history sync*
(recent messages and every chat's last activity) once, and only the process
that paired receives it. `pair` exits after 15 seconds and misses most of it.

To unlink: Linked devices on the phone, or `robot-chat-whatsapp unlink` (with
the daemon stopped). WhatsApp also drops linked devices whose phone has been
offline for about 14 days.

## Running

```sh
systemctl --user enable --now robot-chat-whatsapp
```

with `~/.config/systemd/user/robot-chat-whatsapp.service`:

```ini
[Unit]
Description=robot-chat WhatsApp bridge (whatsmeow)
After=network-online.target

[Service]
ExecStart=%h/.local/bin/robot-chat-whatsapp serve
Restart=on-failure
RestartSec=30
# 3: unlinked, replaced by another instance, banned or outdated
RestartPreventExitStatus=3

[Install]
WantedBy=default.target
```

Only one process may hold the linked device: a second `serve` refuses to
start, and `pair` / `unlink` refuse while the daemon runs. If WhatsApp unlinks
the device, `serve` exits with status 3 and is not restarted; pair again.

Finding chats for the profile (daemon running):

```sh
robot-chat-whatsapp chats hermes            # groups and saved contacts matching "hermes"
robot-chat-whatsapp chats -recent hermes    # only ones with activity, newest first
robot-chat-whatsapp status
robot-chat-whatsapp send team 'hello'       # from you, no prefix (rw chats only)
```

After editing the profile: `systemctl --user restart robot-chat-whatsapp`.

## Agents

Claude Code:

```sh
claude mcp add --scope user robot-chat-whatsapp -- \
    ~/.local/bin/robot-chat-whatsapp mcp -agent claude
```

opencode (`~/.config/opencode/opencode.jsonc`, under `"mcp"`):

```jsonc
"robot-chat-whatsapp": {
  "type": "local",
  "command": ["/home/USER/.local/bin/robot-chat-whatsapp", "mcp", "-agent", "opencode"],
  "enabled": true,
  "timeout": 660000
}
```

`-agent` is the name shown in front of what the agent sends.

| Tool | |
| --- | --- |
| `wa_status` | connected or not, the allowlisted chats (name, group, `ro`/`rw`), last id |
| `wa_read` | messages since the last unfiltered read; `chat`, `after_id`, `limit` |
| `wa_wait` | block until a message arrives (`chat`, `timeout_seconds` up to 600) |
| `wa_send` | `chat`, `text`: send to an `rw` chat |

Messages look like `irc_read`'s:

```json
{"id": 42, "time": "2026-11-04T14:20:00.000Z", "chat": "team", "group": true,
 "from": "Alice", "from_me": false, "text": "> quoted line\nreply", "wa_id": "..."}
```

- A new session's cursor starts at the current end; `after_id: 0` reads the
  stored history. Messages from the history sync carry `"history": true`.
- Media is not downloaded: it shows as `[image] caption`, `[voice message 7s]`,
  `[document: a.pdf]`, and so on. A reply starts with `> ` and the quoted
  line. Edits carry `"edited": true`.
- `from` is the saved contact name, else `~` and the WhatsApp profile name.

WhatsApp messages never wake an agent: the IRC relay posts as the `whatsapp`
account, which no profile should list in `wake_accounts`, and the MCP server
sends no wake-up events.

## Privacy and safety

- Only allowlisted chats are stored or passed on. For every other chat the
  bridge keeps the time of its last message and a message count
  (`whatsapp-activity.json`), for `chats -recent`; no text and no senders.
- The store (session keys), the history (`whatsapp-history.json`) and the
  activity file are mode 600 next to `store`; the socket is mode 600.
- Agents' messages go out from the account's number to real people. The MCP
  instructions tell the agent to send only what its user asked for, and never
  because a WhatsApp message told it to. `send_prefix` marks them, `send_rate`
  limits them, and only `rw` chats accept them.
- From IRC, only accounts in `irc_send_accounts` can send, and only to `rw`
  chats; anything else in a `#wa-` channel is ignored. Text that came from IRC
  is not echoed back there.
- The MCP tools expose only the allowlisted chats; `chats` (the full contact
  and group search) is a CLI command for you.
