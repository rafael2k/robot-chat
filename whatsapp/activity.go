package main

import (
	"encoding/json"
	"os"
	"sync"
	"time"

	"go.mau.fi/whatsmeow/proto/waHistorySync"
	"go.mau.fi/whatsmeow/types"
)

// activity keeps, for every chat, only when its last message was and how
// many messages the bridge has seen: no text, no senders. It helps to pick
// chats for the allowlist ('chats -recent'). Saved next to the store.
type activity struct {
	mu    sync.Mutex
	path  string
	chats map[string]*chatActivity // by JID
	dirty bool
}

type chatActivity struct {
	Last  int64 `json:"last"`  // unix seconds of the last message
	Count int   `json:"count"` // messages seen live since tracking began
}

func loadActivity(path string) *activity {
	a := &activity{path: path, chats: map[string]*chatActivity{}}
	if b, err := os.ReadFile(path); err == nil {
		json.Unmarshal(b, &a.chats)
	}
	return a
}

func (a *activity) seen(jid types.JID, t time.Time, live bool) {
	if jid.IsEmpty() || t.IsZero() {
		return
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	k := jid.ToNonAD().String()
	c := a.chats[k]
	if c == nil {
		c = &chatActivity{}
		a.chats[k] = c
	}
	if t.Unix() > c.Last {
		c.Last = t.Unix()
	}
	if live {
		c.Count++
	}
	a.dirty = true
}

// fromHistory takes the conversations' last-message times from a history
// sync blob; the messages in it are not kept.
func (a *activity) fromHistory(h *waHistorySync.HistorySync) int {
	n := 0
	for _, c := range h.GetConversations() {
		jid, err := types.ParseJID(c.GetID())
		if err != nil {
			continue
		}
		ts := c.GetConversationTimestamp()
		if l := c.GetLastMsgTimestamp(); l > ts {
			ts = l
		}
		if ts > 0 {
			a.seen(jid, time.Unix(int64(ts), 0), false)
			n++
		}
	}
	return n
}

func (a *activity) last(jid types.JID) (time.Time, int) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if c := a.chats[jid.ToNonAD().String()]; c != nil {
		return time.Unix(c.Last, 0), c.Count
	}
	return time.Time{}, 0
}

func (a *activity) save() {
	a.mu.Lock()
	defer a.mu.Unlock()
	if !a.dirty {
		return
	}
	b, _ := json.Marshal(a.chats)
	tmp := a.path + ".tmp"
	if err := os.WriteFile(tmp, b, 0o600); err == nil && os.Rename(tmp, a.path) == nil {
		a.dirty = false
	}
}
