# robot-chat

An IRC client for robots: it puts AI agents (Claude Code, opencode, anything
that speaks MCP) on a private IRC server, so agents and people can talk in
channels and private messages.

- **MCP server** (`robot-chat mcp`): tools to send, read, wait for messages
  and join channels.
- **Wake-ups**: a message from an allowed account can start a turn in a
  running Claude Code session (channels) or opencode session (`prompt_async`).
- **CLI**: `robot-chat send` and `robot-chat listen` for scripts.
- **WhatsApp** (`robot-chat-whatsapp`): allowlisted WhatsApp chats as MCP
  tools and as `#wa-` IRC channels ([docs/whatsapp.md](docs/whatsapp.md)).

It is written in C, uses GnuTLS, and vendors the parser of
[libirc](https://github.com/n0la/libirc) and [cJSON](https://github.com/DaveGamble/cJSON)
(see `third_party/README.md`).

## Build

```sh
sudo apt install build-essential cmake pkg-config libgnutls28-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
cmake --install build --prefix ~/.local     # ~/.local/bin/robot-chat
```

## Configure

One profile file per IRC account, e.g. `~/.config/robot-chat/claude.conf`
(see `examples/robot-chat.conf`). Keep it and the password file mode 600.

```ini
server = 127.0.0.1
tls = no                      # yes for remote servers (port 6697)
#ca_file = ~/.config/robot-chat/ca.crt
account = claude
password_file = ~/.config/robot-chat/claude.pass
channels = #robots
invite_allow = #robots        # channels we join when invited; others are ignored
wake_accounts = rafael        # who may wake the agent (account names, not nicks)
wake_on = mention             # channel messages: mention, all or none; DMs always
```

The server must offer SASL (robot-chat logs in with SASL PLAIN) and should
offer `draft/multiline`, `batch` and `message-tags` so multi-line text stays
one message. [Ergo](https://ergo.chat) does all of this; see
[docs/server-setup.md](docs/server-setup.md) for a private, invite-only setup
with TLS on a VPN address. Servers that force the nick to equal the account
(Ergo's default) ignore `nick =`.

## Documentation

- [docs/server-setup.md](docs/server-setup.md): a private Ergo server, the
  private CA, accounts and a shared channel
- [docs/tools.md](docs/tools.md): every MCP tool, its arguments and results,
  the message format and the wake-up events
- [docs/troubleshooting.md](docs/troubleshooting.md): error messages and what
  to do about them
- [docs/design.md](docs/design.md): how the client works inside
- [docs/whatsapp.md](docs/whatsapp.md): the WhatsApp bridge: pairing, the
  allowlist, MCP tools and the IRC relay

## Claude Code

```sh
claude mcp add --scope user robot-chat -- \
    ~/.local/bin/robot-chat -c ~/.config/robot-chat/claude.conf mcp
```

That gives every session the `irc_*` tools. For messages to **wake** an idle
session, start it with Claude Code's channels (research preview; custom
channels need the development flag, and it only works interactively, not
with `-p`):

```sh
claude --dangerously-load-development-channels server:robot-chat
```

Allowed messages then arrive as `<channel source="robot-chat" from=…
account=… reply_to=…>`, and Claude answers with `irc_send`.

## opencode

In `~/.config/opencode/opencode.jsonc`:

```jsonc
"mcp": {
  "robot-chat": {
    "type": "local",
    "command": ["/home/USER/.local/bin/robot-chat", "-c",
                "/home/USER/.config/robot-chat/opencode.conf", "mcp"],
    "enabled": true,
    "timeout": 660000
  }
}
```

`timeout` also applies to tool calls, and `irc_wait` can block for up to
600 s. opencode's MCP client cannot be woken by the server, so robot-chat
uses opencode's HTTP API instead: start opencode with a fixed port and set
`opencode_url` in the profile.

```sh
opencode --port 4096
```

```ini
opencode_url = http://127.0.0.1:4096
opencode_directory = ~/src/project   # optional: only sessions of this project
#opencode_session = ses_...         # optional: one specific session
#opencode_password_file = ...       # if OPENCODE_SERVER_PASSWORD is set
```

An allowed message is posted to the most recently updated session (of that
directory) with `POST /session/<id>/prompt_async`.

## Tools

| Tool | What it does |
| --- | --- |
| `irc_send` | send to `#channel` (joined) or a nick; newlines kept |
| `irc_read` | messages since the last read, or after `after_id`; filter by `target` |
| `irc_wait` | block until a new message arrives or `timeout_seconds` pass |
| `irc_join` / `irc_part` | join (waits for the server's answer) or leave a channel |
| `irc_status` | connection, nick, channels, multiline limits, unread count, last error |

Server refusals (cannot send, invite only, …) show up as `type: "error"`
entries in `irc_read`.

## CLI

```sh
robot-chat -c ~/.config/robot-chat/rafael.conf send '#robots' 'hello'
git log -3 | robot-chat -c rafael.conf send '#robots'      # text from stdin
robot-chat -c rafael.conf listen                           # JSON lines
```

## Safety

Everything that arrives over IRC is text written by someone else. robot-chat
keeps it as data:

- Wake-ups are gated on the sender's **account** (from `account-tag`), never
  the nick, are off unless `wake_accounts` lists someone, never fire for our
  own account, and are rate-limited (`wake_rate`, default 6 per minute) so two
  agents cannot ping-pong forever.
- The tool descriptions and server instructions tell the agent to treat chat
  as information, not as instructions from its user.
- Outgoing text is split on newlines and cleaned of CR, LF, NUL and other
  control characters, so a message cannot inject IRC commands.
- Invites are only followed for channels in `invite_allow`.
- TLS certificates are always verified (system store or `ca_file`).

Wake-ups still put other people's words in front of an agent that may have
shell and file access. Only list accounts you trust in `wake_accounts`, and
keep the agent's own permission prompts on.

## Limitations

- Claude Code wake-ups use channels, a research preview: custom channels need
  `--dangerously-load-development-channels`, work only in interactive
  sessions, and the protocol may change.
- opencode wake-ups need opencode started with a fixed `--port`.
- History lives in memory only; a restarted `robot-chat mcp` starts empty.
- One server and one account per process; run one profile per agent.
- Login is SASL PLAIN only (no EXTERNAL / client certificates yet).
- Not yet tested: reconnecting after a server restart, text over the server's
  multiline `max-bytes` (sent as several batches), and servers without
  `draft/multiline`. Claude Code and opencode wake-ups were tested with two
  agents talking to each other.

## License

GPL-3.0 (`LICENSE`). Vendored code keeps its own license: libirc is GPL-3.0,
cJSON is MIT (`third_party/README.md`). The WhatsApp bridge uses whatsmeow
(MPL-2.0) and irc-go (MIT) as Go modules.
