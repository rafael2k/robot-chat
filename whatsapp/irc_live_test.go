package main

import (
	"context"
	"os"
	"testing"
	"time"
)

// TestIRCLive needs a server: ROBOT_CHAT_WA_TEST_CONF=path/to/whatsapp.conf
// with irc_* set and a chat named "test". It relays one multi-line entry and
// keeps the relay up for ROBOT_CHAT_WA_TEST_SECONDS (default 5) so another
// client can check what arrived and post replies.
func TestIRCLive(t *testing.T) {
	path := os.Getenv("ROBOT_CHAT_WA_TEST_CONF")
	if path == "" {
		t.Skip("ROBOT_CHAT_WA_TEST_CONF not set")
	}
	cfg, err := loadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	cfg.Store = t.TempDir() + "/wa.db"
	cfg.Socket = t.TempDir() + "/s.sock"
	container, dev, err := openDevice(context.Background(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	defer container.Close()
	b := newBridge(cfg, dev)
	if b.irc, err = newIRCRelay(b); err != nil {
		t.Fatal(err)
	}
	b.irc.run()
	defer b.irc.conn.Quit()
	for i := 0; i < 50 && !b.irc.conn.Connected(); i++ {
		time.Sleep(100 * time.Millisecond)
	}
	delay := 2 * time.Second // join, +I
	if d, err := time.ParseDuration(os.Getenv("ROBOT_CHAT_WA_TEST_DELAY")); err == nil {
		delay = d
	}
	time.Sleep(delay)
	b.irc.relay(b.hist.add(entry{Chat: "test", From: "Mãe", Text: "first line\n  indented second\n\nafter a blank"}))
	secs := 5
	if s := os.Getenv("ROBOT_CHAT_WA_TEST_SECONDS"); s != "" {
		if n, err := time.ParseDuration(s + "s"); err == nil {
			secs = int(n.Seconds())
		}
	}
	time.Sleep(time.Duration(secs) * time.Second)
}
