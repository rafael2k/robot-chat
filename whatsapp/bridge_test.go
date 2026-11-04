package main

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"go.mau.fi/whatsmeow/proto/waE2E"
	"google.golang.org/protobuf/proto"
)

func writeConf(t *testing.T, body string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "whatsapp.conf")
	if err := os.WriteFile(p, []byte(body), 0o600); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestConfig(t *testing.T) {
	dir := t.TempDir()
	pass := filepath.Join(dir, "pass")
	os.WriteFile(pass, []byte("s3cret\nignored\n"), 0o600)
	c, err := loadConfig(writeConf(t, `
# comment
store = `+dir+`/wa.db
chat = family 5511999999999@s.whatsapp.net rw   # trailing comment
chat = hermes 120363000000000000@g.us
send_rate = 3
irc_server = 127.0.0.1
irc_account = whatsapp
irc_password_file = `+pass+`
irc_send_accounts = rafael, claude
irc_invite = claude opencode
`))
	if err != nil {
		t.Fatal(err)
	}
	if len(c.Chats) != 2 || !c.Chats[0].RW || c.Chats[1].RW {
		t.Fatalf("chats: %+v %+v", c.Chats[0], c.Chats[1])
	}
	if c.IRCPassword != "s3cret" || c.IRCPort != "6667" || c.SendRate != 3 {
		t.Fatalf("irc: %q %q %d", c.IRCPassword, c.IRCPort, c.SendRate)
	}
	if strings.Join(c.IRCSendAccounts, ",") != "rafael,claude" || strings.Join(c.IRCInvite, ",") != "claude,opencode" {
		t.Fatalf("lists: %v %v", c.IRCSendAccounts, c.IRCInvite)
	}
	if c.chatByName("#wa-family") != c.Chats[0] || c.chatByName("FAMILY") != c.Chats[0] || c.chatByName("x") != nil {
		t.Fatal("chatByName")
	}

	for _, bad := range []string{
		"chat = Family 1@s.whatsapp.net",   // upper case name
		"chat = a 1@lid",                   // not a phone or group JID
		"chat = a 1@s.whatsapp.net rx",     // bad mode
		"chat = a 1@g.us\nchat = a 2@g.us", // duplicate
		"nope = 1",                         // unknown key
		"irc_server = x",                   // no account
		"irc_server = x\nirc_account = y",  // no password
	} {
		if _, err := loadConfig(writeConf(t, bad)); err == nil {
			t.Errorf("accepted %q", bad)
		}
	}
}

func TestStripComment(t *testing.T) {
	for in, want := range map[string]string{
		"#robots":            "#robots",
		"#a, #b":             "#a, #b",
		"no   # yes for TLS": "no",
		"x #":                "x",
		"a#b":                "a#b",
	} {
		if got := stripComment(in); got != want {
			t.Errorf("stripComment(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestText(t *testing.T) {
	if got := cleanText("a\r\nb\rc\x00\x1b[31m\td"); got != "a\nb\nc[31m\td" {
		t.Errorf("cleanText: %q", got)
	}
	if got := cleanText("bad \xff utf8"); got != "bad � utf8" {
		t.Errorf("cleanText utf8: %q", got)
	}
	long := strings.Repeat("ação ", 200)
	parts := splitLine(long, 100)
	if strings.Join(parts, "") != long {
		t.Error("splitLine lost text")
	}
	for _, p := range parts {
		if len(p) > 100 || !strings.HasSuffix(p, " ") && p != parts[len(parts)-1] {
			t.Errorf("piece %q", p)
		}
	}

	quoted := &waE2E.Message{ExtendedTextMessage: &waE2E.ExtendedTextMessage{
		Text: proto.String("yes!"),
		ContextInfo: &waE2E.ContextInfo{QuotedMessage: &waE2E.Message{
			Conversation: proto.String("dinner at 8?\nor 9"),
		}},
	}}
	for m, want := range map[*waE2E.Message]string{
		{Conversation: proto.String("hi\r\nthere")}: "hi\nthere",
		quoted: "> dinner at 8?\nyes!",
		{ImageMessage: &waE2E.ImageMessage{Caption: proto.String("look")}}:                   "[image] look",
		{AudioMessage: &waE2E.AudioMessage{PTT: proto.Bool(true), Seconds: proto.Uint32(7)}}: "[voice message 7s]",
		{DocumentMessage: &waE2E.DocumentMessage{FileName: proto.String("a.pdf")}}:           "[document: a.pdf]",
		{ReactionMessage: &waE2E.ReactionMessage{Text: proto.String("")}}:                    "",
		{}: "",
	} {
		if got := messageText(m); got != want {
			t.Errorf("messageText = %q, want %q", got, want)
		}
	}
}

// TestSocketAndMCP runs the daemon side (socket, history) with an unpaired
// device, and talks to it through the MCP server's tool code.
func TestSocketAndMCP(t *testing.T) {
	dir := t.TempDir()
	cfg, err := loadConfig(writeConf(t, "store = "+dir+"/wa.db\nsocket = "+dir+"/s.sock\n"+
		"chat = family 5511999999999@s.whatsapp.net rw\nchat = hermes 120363000000000000@g.us ro\n"))
	if err != nil {
		t.Fatal(err)
	}
	container, dev, err := openDevice(context.Background(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer container.Close()
	if st, _ := os.Stat(cfg.Store); st.Mode().Perm() != 0o600 {
		t.Errorf("store mode %v", st.Mode().Perm())
	}
	b := newBridge(cfg, dev)
	l, err := b.listen()
	if err != nil {
		t.Fatal(err)
	}
	defer l.Close()
	go b.serveSocket(l)
	if st, _ := os.Stat(cfg.Socket); st.Mode().Perm() != 0o600 {
		t.Errorf("socket mode %v", st.Mode().Perm())
	}
	if _, err := b.listen(); err == nil {
		t.Error("second daemon allowed on the same socket")
	}

	b.hist.add(entry{Chat: "family", From: "Mãe", Text: "old message"})
	s := &mcpServer{cfg: cfg, agent: "claude"}

	// A new session starts at the current end of history.
	r, err := s.call("wa_read", nil)
	if err != nil || len(r.(map[string]any)["messages"].([]entry)) != 0 {
		t.Fatalf("first read: %v %v", r, err)
	}
	r, _ = s.call("wa_read", map[string]any{"after_id": float64(0)})
	if m := r.(map[string]any)["messages"].([]entry); len(m) != 1 || m[0].Text != "old message" {
		t.Fatalf("history read: %v", r)
	}

	// wa_wait wakes on a new entry.
	go func() {
		time.Sleep(200 * time.Millisecond)
		b.hist.add(entry{Chat: "hermes", Group: true, From: "Pedro", Text: "line 1\nline 2"})
	}()
	start := time.Now()
	r, err = s.call("wa_wait", map[string]any{"timeout_seconds": float64(5)})
	if err != nil {
		t.Fatal(err)
	}
	if m := r.(map[string]any)["messages"].([]entry); len(m) != 1 || m[0].Text != "line 1\nline 2" || time.Since(start) > 3*time.Second {
		t.Fatalf("wait: %v", r)
	}
	r, _ = s.call("wa_wait", map[string]any{"timeout_seconds": float64(1)})
	if r.(map[string]any)["timed_out"] != true {
		t.Fatalf("wait timeout: %v", r)
	}

	// A filtered read does not move the cursor.
	b.hist.add(entry{Chat: "family", From: "Mãe", Text: "new"})
	s.call("wa_read", map[string]any{"chat": "hermes"})
	r, _ = s.call("wa_read", nil)
	if m := r.(map[string]any)["messages"].([]entry); len(m) != 1 || m[0].Text != "new" {
		t.Fatalf("cursor after filtered read: %v", r)
	}

	// Sending: read-only chats, unknown chats, and no connection all fail.
	for chat, want := range map[string]string{"hermes": "read-only", "nope": "unknown chat", "family": "not connected"} {
		if _, err := s.call("wa_send", map[string]any{"chat": chat, "text": "hi"}); err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("send to %s: %v, want %q", chat, err, want)
		}
	}
	if _, err := s.call("wa_read", map[string]any{"chat": "nope"}); err == nil {
		t.Error("read of unknown chat")
	}

	st, err := s.call("wa_status", nil)
	if err != nil {
		t.Fatal(err)
	}
	js, _ := json.Marshal(st)
	if !strings.Contains(string(js), `"name":"family","group":false,"mode":"rw"`) || strings.Contains(string(js), "5511999999999") {
		t.Errorf("status: %s", js)
	}
}

func TestHistoryTrimByTime(t *testing.T) {
	h := newHistory(3)
	h.add(entry{Time: "2026-11-04T10:00", Text: "newest"})
	h.add(entry{Time: "2026-11-01T10:00", Text: "old"})
	h.add(entry{Time: "2026-11-02T10:00", Text: "mid"})
	h.add(entry{Time: "2026-11-03T10:00", Text: "late"})
	got, _, _, _, _ := h.since(0, "", 10)
	var texts []string
	for _, e := range got {
		texts = append(texts, e.Text)
	}
	if strings.Join(texts, ",") != "newest,mid,late" {
		t.Fatalf("kept %v", texts)
	}
}

func TestMediaPath(t *testing.T) {
	for _, c := range []struct{ mime, name, want string }{
		{"image/jpeg", "", ".jpg"},
		{"audio/ogg; codecs=opus", "", ".ogg"},
		{"application/pdf", "Report.PDF", ".pdf"},
		{"application/octet-stream", "a.tar.gz", ".gz"},
		{"application/x-unknown-thing", "", ".bin"},
		{"image/png", "../../evil", ".png"},
	} {
		if got := mediaExt(c.mime, c.name); got != c.want {
			t.Errorf("mediaExt(%q, %q) = %q, want %q", c.mime, c.name, got, c.want)
		}
	}
	en := entry{Time: "2026-11-04T21:07:05.123Z", Chat: "team", WAID: "3EB0/../AB12"}
	if got, want := mediaPath("/m", en, ".jpg"), "/m/team/20261104-210705-3EB0AB12.jpg"; got != want {
		t.Errorf("mediaPath = %q, want %q", got, want)
	}
}
