package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"time"
)

// The daemon ('serve') holds the only WhatsApp connection. 'mcp' processes
// and the 'chats' command reach it over a unix socket (mode 600): one JSON
// request line, one JSON response line, per connection.

type request struct {
	Op      string `json:"op"` // status, read, wait, send, chats
	AfterID int64  `json:"after_id,omitempty"`
	Chat    string `json:"chat,omitempty"`
	Limit   int    `json:"limit,omitempty"`
	Timeout int    `json:"timeout,omitempty"` // seconds, for wait
	Text    string `json:"text,omitempty"`
	Agent   string `json:"agent,omitempty"`
	Filter  string `json:"filter,omitempty"`
	Recent  bool   `json:"recent,omitempty"` // chats: only ones with activity, newest first
}

type response struct {
	Error string `json:"error,omitempty"`

	Messages    []entry `json:"messages,omitempty"`
	NextAfterID int64   `json:"next_after_id"`
	More        bool    `json:"more,omitempty"`
	Missed      bool    `json:"missed_older,omitempty"`
	TimedOut    bool    `json:"timed_out,omitempty"`

	Sent  *entry     `json:"sent,omitempty"`
	Chats []chatInfo `json:"chats,omitempty"`

	Status *status `json:"status,omitempty"`
}

type chatStatus struct {
	Name  string `json:"name"`
	Group bool   `json:"group"`
	Mode  string `json:"mode"`
	IRC   string `json:"irc_channel,omitempty"`
}

type status struct {
	Connected bool         `json:"connected"`
	LoggedIn  bool         `json:"logged_in"`
	Chats     []chatStatus `json:"chats"`
	LastID    int64        `json:"last_id"`
	IRC       string       `json:"irc,omitempty"` // "connected", "offline", or "" if not configured
	SendRate  int          `json:"send_rate"`
}

// socketAlive reports whether a daemon answers on path.
func socketAlive(path string) bool {
	c, err := net.DialTimeout("unix", path, time.Second)
	if err != nil {
		return false
	}
	c.Close()
	return true
}

func (b *bridge) listen() (net.Listener, error) {
	path := b.cfg.Socket
	if socketAlive(path) {
		return nil, fmt.Errorf("a daemon is already running on %s", path)
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return nil, err
	}
	os.Remove(path)
	old := umask(0o177)
	l, err := net.Listen("unix", path)
	umask(old)
	if err != nil {
		return nil, err
	}
	os.Chmod(path, 0o600)
	return l, nil
}

func (b *bridge) serveSocket(l net.Listener) {
	for {
		c, err := l.Accept()
		if err != nil {
			if !errors.Is(err, net.ErrClosed) {
				warnf("socket: %v", err)
			}
			return
		}
		go b.handleConn(c)
	}
}

func (b *bridge) handleConn(c net.Conn) {
	defer c.Close()
	c.SetReadDeadline(time.Now().Add(10 * time.Second))
	line, err := bufio.NewReaderSize(c, 1<<20).ReadBytes('\n')
	if err != nil {
		return
	}
	var req request
	var resp response
	if err := json.Unmarshal(line, &req); err != nil {
		resp.Error = "bad request: " + err.Error()
	} else {
		resp = b.handle(req)
	}
	out, _ := json.Marshal(resp)
	c.SetWriteDeadline(time.Now().Add(10 * time.Second))
	c.Write(append(out, '\n'))
}

func (b *bridge) handle(req request) (resp response) {
	if req.Chat != "" && req.Op != "send" && b.cfg.chatByName(req.Chat) == nil {
		resp.Error = fmt.Sprintf("unknown chat '%s' (see wa_chats)", req.Chat)
		return
	}
	if cc := b.cfg.chatByName(req.Chat); cc != nil {
		req.Chat = cc.Name
	}
	limit := req.Limit
	if limit <= 0 {
		limit = 50
	}
	limit = min(limit, 200)

	switch req.Op {
	case "status":
		st := &status{Connected: b.cli.IsConnected(), LoggedIn: b.cli.IsLoggedIn(), SendRate: b.cfg.SendRate, Chats: []chatStatus{}}
		for _, cc := range b.cfg.Chats {
			cs := chatStatus{Name: cc.Name, Group: cc.JID.Server == "g.us", Mode: "ro"}
			if cc.RW {
				cs.Mode = "rw"
			}
			if b.irc != nil {
				cs.IRC = b.cfg.IRCChannelPrefix + cc.Name
			}
			st.Chats = append(st.Chats, cs)
		}
		_, _, _, st.LastID, _ = b.hist.since(1<<62, "", 0)
		if b.irc != nil {
			st.IRC = "offline"
			if b.irc.conn.Connected() {
				st.IRC = "connected"
			}
		}
		resp.Status = st
	case "read", "wait":
		msgs, more, missed, last, changed := b.hist.since(req.AfterID, req.Chat, limit)
		if req.Op == "wait" && len(msgs) == 0 {
			t := min(max(req.Timeout, 1), 600)
			timer := time.NewTimer(time.Duration(t) * time.Second)
			defer timer.Stop()
			for len(msgs) == 0 {
				select {
				case <-changed:
					msgs, more, missed, last, changed = b.hist.since(req.AfterID, req.Chat, limit)
				case <-timer.C:
					resp.TimedOut = true
					resp.NextAfterID = max(req.AfterID, last)
					if req.Chat != "" {
						resp.NextAfterID = req.AfterID
					}
					return
				}
			}
		}
		resp.Messages, resp.More, resp.Missed = msgs, more, missed
		resp.NextAfterID = req.AfterID
		if len(msgs) > 0 {
			resp.NextAfterID = msgs[len(msgs)-1].ID
		} else if req.Chat == "" {
			resp.NextAfterID = max(req.AfterID, last)
		}
	case "send":
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		e, err := b.send(ctx, req.Chat, req.Text, req.Agent, "mcp")
		if err != nil {
			resp.Error = err.Error()
		} else {
			resp.Sent = &e
		}
	case "chats":
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		chats, err := b.listChats(ctx, req.Filter, req.Recent)
		if err != nil {
			resp.Error = err.Error()
		} else {
			resp.Chats = chats
		}
	default:
		resp.Error = "unknown op '" + req.Op + "'"
	}
	return
}

// call sends one request to the daemon.
func call(socket string, req request, timeout time.Duration) (*response, error) {
	c, err := net.DialTimeout("unix", socket, 2*time.Second)
	if err != nil {
		return nil, fmt.Errorf("the WhatsApp daemon is not running (%v); start it with 'robot-chat-whatsapp serve' (systemctl --user start robot-chat-whatsapp)", err)
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(timeout))
	out, _ := json.Marshal(req)
	if _, err := c.Write(append(out, '\n')); err != nil {
		return nil, err
	}
	line, err := bufio.NewReaderSize(c, 1<<20).ReadBytes('\n')
	if err != nil {
		return nil, fmt.Errorf("no answer from the daemon: %v", err)
	}
	var resp response
	if err := json.Unmarshal(line, &resp); err != nil {
		return nil, err
	}
	return &resp, nil
}
