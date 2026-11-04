package main

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"log"
	"net"
	"os"
	"slices"
	"strings"
	"time"

	"github.com/ergochat/irc-go/ircevent"
	"github.com/ergochat/irc-go/ircmsg"
)

const (
	ircLineBytes  = 400  // text per PRIVMSG, leaving room for the relay prefix
	ircBatchLines = 90   // below Ergo's multiline max-lines (100)
	ircBatchBytes = 3500 // below Ergo's multiline max-bytes (4096)
)

// ircRelay mirrors each allowlisted chat to an IRC channel (prefix + name),
// and sends what allowed accounts write there back to WhatsApp.
type ircRelay struct {
	b    *bridge
	conn *ircevent.Connection
}

func newIRCRelay(b *bridge) (*ircRelay, error) {
	cfg := b.cfg
	r := &ircRelay{b: b}
	c := &ircevent.Connection{
		Server:        net.JoinHostPort(cfg.IRCServer, cfg.IRCPort),
		Nick:          cfg.IRCAccount,
		User:          cfg.IRCAccount,
		RealName:      "robot-chat WhatsApp bridge",
		UseSASL:       true,
		SASLLogin:     cfg.IRCAccount,
		SASLPassword:  cfg.IRCPassword,
		RequestCaps:   []string{"message-tags", "batch", "draft/multiline", "server-time", "account-tag"},
		ReconnectFreq: 30 * time.Second,
		KeepAlive:     2 * time.Minute,
		Timeout:       time.Minute,
		Log:           log.New(os.Stderr, "robot-chat-whatsapp irc: ", 0),
	}
	if cfg.IRCTLS {
		tc := &tls.Config{ServerName: cfg.IRCServer, MinVersion: tls.VersionTLS12}
		if cfg.IRCCAFile != "" {
			pem, err := os.ReadFile(cfg.IRCCAFile)
			if err != nil {
				return nil, fmt.Errorf("irc_ca_file: %v", err)
			}
			pool := x509.NewCertPool()
			if !pool.AppendCertsFromPEM(pem) {
				return nil, fmt.Errorf("irc_ca_file %s: no certificates", cfg.IRCCAFile)
			}
			tc.RootCAs = pool
		}
		c.UseTLS, c.TLSConfig = true, tc
	}
	c.AddConnectCallback(func(ircmsg.Message) {
		for _, cc := range cfg.Chats {
			c.Join(cfg.IRCChannelPrefix + cc.Name)
		}
	})
	c.AddCallback("JOIN", func(m ircmsg.Message) {
		// We create the channels, so we hold ops: let the agents in.
		if m.Nick() == c.CurrentNick() && len(m.Params) > 0 {
			for _, nick := range cfg.IRCInvite {
				c.Send("MODE", m.Params[0], "+I", nick+"!*@*")
			}
		}
	})
	c.AddCallback("INVITE", func(ircmsg.Message) {}) // never follow invites
	c.AddCallback("PRIVMSG", func(m ircmsg.Message) {
		if len(m.Params) == 2 {
			_, account := m.GetTag("account")
			r.fromIRC(m.Params[0], m.Nick(), account, m.Params[1])
		}
	})
	c.AddBatchCallback(func(bt *ircevent.Batch) bool {
		if len(bt.Params) < 3 || bt.Params[1] != "draft/multiline" {
			return false
		}
		// Ergo tags the BATCH line with the sender's account, not the items.
		var sb strings.Builder
		nick := bt.Nick()
		_, account := bt.GetTag("account")
		for i, it := range bt.Items {
			if it.Command != "PRIVMSG" || len(it.Params) < 2 {
				continue
			}
			if concat, _ := it.GetTag("draft/multiline-concat"); i > 0 && !concat {
				sb.WriteByte('\n')
			}
			sb.WriteString(it.Params[1])
			if nick == "" {
				nick = it.Nick()
			}
			if account == "" {
				_, account = it.GetTag("account")
			}
		}
		r.fromIRC(bt.Params[2], nick, account, sb.String())
		return true
	})
	r.conn = c
	return r, nil
}

func (r *ircRelay) run() error {
	if err := r.conn.Connect(); err != nil {
		r.conn.Log.Printf("connect: %v (retrying)", err)
	}
	go r.conn.Loop()
	return nil
}

func (r *ircRelay) fromIRC(target, nick, account, text string) {
	cfg := r.b.cfg
	if !strings.HasPrefix(strings.ToLower(target), strings.ToLower(cfg.IRCChannelPrefix)) {
		return // private messages and other channels are not relayed
	}
	cc := cfg.chatByName(target)
	if cc == nil || account == "" || account == "*" ||
		!slices.ContainsFunc(cfg.IRCSendAccounts, func(a string) bool { return strings.EqualFold(a, account) }) {
		return
	}
	if strings.HasPrefix(text, "\x01ACTION ") {
		text = "/me " + strings.TrimSuffix(strings.TrimPrefix(text, "\x01ACTION "), "\x01")
	} else if strings.HasPrefix(text, "\x01") {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	if _, err := r.b.send(ctx, cc.Name, text, nick, "irc"); err != nil {
		r.conn.Notice(target, nick+": not sent to WhatsApp: "+err.Error())
	}
}

// relay posts a WhatsApp entry to the chat's channel, as one multiline
// batch when the server supports it.
func (r *ircRelay) relay(e entry) {
	if !r.conn.Connected() {
		return
	}
	who := e.From
	if e.Agent != "" {
		who = e.Agent + " (via " + e.From + ")"
	}
	text := "<" + who + "> " + e.Text
	if e.Edited {
		text = "<" + who + "> (edited) " + e.Text
	}
	target := r.b.cfg.IRCChannelPrefix + e.Chat

	type piece struct {
		text   string
		concat bool
	}
	var pieces []piece
	for _, line := range strings.Split(text, "\n") {
		for i, p := range splitLine(line, ircLineBytes) {
			pieces = append(pieces, piece{p, i > 0})
		}
	}
	caps := r.conn.AcknowledgedCaps()
	_, ml := caps["draft/multiline"]
	_, bt := caps["batch"]
	if !ml || !bt || len(pieces) == 1 {
		for _, p := range pieces {
			if p.text == "" {
				p.text = " "
			}
			r.conn.Privmsg(target, p.text)
		}
		return
	}
	for len(pieces) > 0 {
		var msgs []ircmsg.Message
		size := 0
		for len(pieces) > 0 && len(msgs) < ircBatchLines && size+len(pieces[0].text) <= ircBatchBytes {
			m := ircmsg.MakeMessage(nil, "", "PRIVMSG", target, pieces[0].text)
			if pieces[0].concat && len(msgs) > 0 {
				m.SetTag("draft/multiline-concat", "")
			}
			msgs = append(msgs, m)
			size += len(pieces[0].text)
			pieces = pieces[1:]
		}
		if err := r.conn.SendBatch(msgs, nil, "draft/multiline", target); err != nil {
			r.conn.Log.Printf("relay to %s: %v", target, err)
			return
		}
	}
}
