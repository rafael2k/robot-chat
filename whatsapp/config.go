package main

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"

	"go.mau.fi/whatsmeow/types"
)

// chatConf is one allowlisted WhatsApp chat.
type chatConf struct {
	Name string    // short name: the MCP chat name and the IRC channel suffix
	JID  types.JID // user@s.whatsapp.net or group@g.us
	RW   bool      // agents may send to it
}

type config struct {
	Path       string
	Store      string
	Socket     string
	History    int
	MyName     string
	SendPrefix bool
	SendRate   int
	MediaDir   string // "": media not saved
	MediaMaxMB int
	Chats      []*chatConf

	IRCServer        string
	IRCPort          string
	IRCTLS           bool
	IRCCAFile        string
	IRCAccount       string
	IRCPassword      string
	IRCChannelPrefix string
	IRCSendAccounts  []string
	IRCInvite        []string
}

var chatNameRe = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,31}$`)

func expandHome(p string) string {
	if p == "~" || strings.HasPrefix(p, "~/") {
		if h, err := os.UserHomeDir(); err == nil {
			return filepath.Join(h, p[1:])
		}
	}
	return p
}

// stripComment removes a trailing comment: whitespace, then "#" followed by
// a space or the end of the line. "#robots" and "a#b" are values.
func stripComment(v string) string {
	for i := 1; i < len(v); i++ {
		if v[i] == '#' && (v[i-1] == ' ' || v[i-1] == '\t') &&
			(i+1 == len(v) || v[i+1] == ' ' || v[i+1] == '\t') {
			return strings.TrimSpace(v[:i])
		}
	}
	return v
}

func splitList(v string) []string {
	var out []string
	for _, s := range strings.FieldsFunc(v, func(r rune) bool { return r == ',' || r == ' ' || r == '\t' }) {
		out = append(out, s)
	}
	return out
}

func parseBool(v string) (bool, error) {
	switch strings.ToLower(v) {
	case "yes", "true", "on", "1":
		return true, nil
	case "no", "false", "off", "0":
		return false, nil
	}
	return false, fmt.Errorf("expected yes or no, got %q", v)
}

func defaultSocket() string {
	if d := os.Getenv("XDG_RUNTIME_DIR"); d != "" {
		return filepath.Join(d, "robot-chat-whatsapp.sock")
	}
	return expandHome("~/.local/state/robot-chat/whatsapp.sock")
}

func defaultConfigPath() string {
	if p := os.Getenv("ROBOT_CHAT_WHATSAPP_CONFIG"); p != "" {
		return p
	}
	return expandHome("~/.config/robot-chat/whatsapp.conf")
}

func loadConfig(path string) (*config, error) {
	c := &config{
		Path:             path,
		Store:            expandHome("~/.local/share/robot-chat/whatsapp.db"),
		Socket:           defaultSocket(),
		History:          1000,
		MyName:           "me",
		SendPrefix:       true,
		SendRate:         10,
		MediaMaxMB:       25,
		IRCPort:          "",
		IRCChannelPrefix: "#wa-",
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	if st, err := f.Stat(); err == nil && st.Mode().Perm()&0o077 != 0 {
		warnf("warning: %s is readable by other users (chmod 600 it)", path)
	}

	var passwordFile string
	mediaDir := "" // default: next to the store
	sc := bufio.NewScanner(f)
	for n := 1; sc.Scan(); n++ {
		line := strings.TrimSpace(sc.Text())
		if line == "" || line[0] == '#' {
			continue
		}
		k, v, ok := strings.Cut(line, "=")
		if !ok {
			return nil, fmt.Errorf("%s:%d: expected key = value", path, n)
		}
		k, v = strings.TrimSpace(k), stripComment(strings.TrimSpace(v))
		bad := func(format string, a ...any) error {
			return fmt.Errorf("%s:%d: %s", path, n, fmt.Sprintf(format, a...))
		}
		switch k {
		case "store":
			c.Store = expandHome(v)
		case "socket":
			c.Socket = expandHome(v)
		case "history":
			if c.History, err = strconv.Atoi(v); err != nil || c.History < 10 {
				return nil, bad("history must be a number >= 10")
			}
		case "my_name":
			c.MyName = v
		case "send_prefix":
			if c.SendPrefix, err = parseBool(v); err != nil {
				return nil, bad("send_prefix: %v", err)
			}
		case "send_rate":
			if c.SendRate, err = strconv.Atoi(v); err != nil || c.SendRate < 1 {
				return nil, bad("send_rate must be a number >= 1")
			}
		case "media":
			mediaDir = expandHome(v)
		case "media_max_mb":
			if c.MediaMaxMB, err = strconv.Atoi(v); err != nil || c.MediaMaxMB < 1 {
				return nil, bad("media_max_mb must be a number >= 1")
			}
		case "chat":
			// chat = NAME JID [ro|rw]
			f := strings.Fields(v)
			if len(f) < 2 || len(f) > 3 {
				return nil, bad("expected chat = NAME JID [ro|rw]")
			}
			if !chatNameRe.MatchString(f[0]) {
				return nil, bad("chat name %q: use a-z, 0-9, - and _ (max 32)", f[0])
			}
			jid, err := types.ParseJID(f[1])
			if err != nil || jid.User == "" ||
				(jid.Server != types.DefaultUserServer && jid.Server != types.GroupServer) {
				return nil, bad("chat %s: %q is not a user@s.whatsapp.net or group@g.us JID", f[0], f[1])
			}
			cc := &chatConf{Name: f[0], JID: jid.ToNonAD()}
			if len(f) == 3 {
				switch f[2] {
				case "rw":
					cc.RW = true
				case "ro":
				default:
					return nil, bad("chat %s: mode must be ro or rw", f[0])
				}
			}
			for _, o := range c.Chats {
				if o.Name == cc.Name || o.JID == cc.JID {
					return nil, bad("chat %s: duplicate name or JID", f[0])
				}
			}
			c.Chats = append(c.Chats, cc)
		case "irc_server":
			c.IRCServer = v
		case "irc_port":
			c.IRCPort = v
		case "irc_tls":
			if c.IRCTLS, err = parseBool(v); err != nil {
				return nil, bad("irc_tls: %v", err)
			}
		case "irc_ca_file":
			c.IRCCAFile = expandHome(v)
		case "irc_account":
			c.IRCAccount = v
		case "irc_password_file":
			passwordFile = expandHome(v)
		case "irc_channel_prefix":
			if !strings.HasPrefix(v, "#") {
				return nil, bad("irc_channel_prefix must start with #")
			}
			c.IRCChannelPrefix = v
		case "irc_send_accounts":
			c.IRCSendAccounts = splitList(v)
		case "irc_invite":
			c.IRCInvite = splitList(v)
		default:
			return nil, bad("unknown key '%s'", k)
		}
	}
	if err := sc.Err(); err != nil {
		return nil, err
	}

	switch mediaDir {
	case "":
		c.MediaDir = strings.TrimSuffix(c.Store, ".db") + "-media"
	case "no", "off", "none":
	default:
		c.MediaDir = mediaDir
	}

	if c.IRCServer != "" {
		if c.IRCAccount == "" {
			return nil, fmt.Errorf("%s: irc_server needs irc_account", path)
		}
		if c.IRCPort == "" {
			c.IRCPort = "6667"
			if c.IRCTLS {
				c.IRCPort = "6697"
			}
		}
		if p := os.Getenv("ROBOT_CHAT_WHATSAPP_IRC_PASSWORD"); p != "" {
			c.IRCPassword = p
		} else if passwordFile != "" {
			b, err := os.ReadFile(passwordFile)
			if err != nil {
				return nil, fmt.Errorf("irc_password_file: %v", err)
			}
			if st, err := os.Stat(passwordFile); err == nil && st.Mode().Perm()&0o077 != 0 {
				warnf("warning: %s is readable by other users (chmod 600 it)", passwordFile)
			}
			c.IRCPassword, _, _ = strings.Cut(string(b), "\n")
			c.IRCPassword = strings.TrimSpace(c.IRCPassword)
		}
		if c.IRCPassword == "" {
			return nil, fmt.Errorf("%s: no IRC password (set irc_password_file)", path)
		}
	}
	return c, nil
}

func (c *config) chatByJID(jid types.JID) *chatConf {
	jid = jid.ToNonAD()
	for _, cc := range c.Chats {
		if cc.JID == jid {
			return cc
		}
	}
	return nil
}

func (c *config) chatByName(name string) *chatConf {
	name = strings.TrimPrefix(strings.ToLower(name), strings.ToLower(c.IRCChannelPrefix))
	for _, cc := range c.Chats {
		if cc.Name == name {
			return cc
		}
	}
	return nil
}
