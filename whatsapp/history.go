package main

import (
	"encoding/json"
	"os"
	"slices"
	"sync"
	"time"
)

// entry is one message in the history, as agents see it.
type entry struct {
	ID      int64  `json:"id"`
	Time    string `json:"time"`
	Chat    string `json:"chat"`
	Group   bool   `json:"group"`
	From    string `json:"from"`
	FromMe  bool   `json:"from_me"`
	Agent   string `json:"agent,omitempty"` // sent by this agent through the bridge
	Edited  bool   `json:"edited,omitempty"`
	History bool   `json:"history,omitempty"` // from the phone's history sync, not live
	Text    string `json:"text"`

	WAID string `json:"wa_id,omitempty"` // WhatsApp message id, to skip duplicates
}

// history is a ring of the last entries of the allowlisted chats.
type history struct {
	mu      sync.Mutex
	max     int
	entries []entry
	lastID  int64
	changed chan struct{} // closed and replaced on every add
}

func newHistory(max int) *history {
	return &history{max: max, changed: make(chan struct{})}
}

// load reads entries saved by save, keeping those keep accepts (chats that
// are still allowlisted), with their ids.
func (h *history) load(path string, keep func(entry) bool) {
	b, err := os.ReadFile(path)
	if err != nil {
		return
	}
	var saved []entry
	if json.Unmarshal(b, &saved) != nil {
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, e := range saved {
		if keep(e) {
			h.entries = append(h.entries, e)
		}
		h.lastID = max(h.lastID, e.ID)
	}
	h.trim()
}

// trim drops the oldest entries by time, not by id: history-sync imports
// arrive newest batch first, so ids are not in time order.
func (h *history) trim() {
	n := len(h.entries) - h.max
	if n <= 0 {
		return
	}
	times := make([]string, len(h.entries))
	for i, e := range h.entries {
		times[i] = e.Time
	}
	slices.Sort(times)
	cut := times[n-1] // drop entries up to this time, n of them
	kept := h.entries[:0]
	for _, e := range h.entries {
		if n > 0 && e.Time <= cut {
			n--
			continue
		}
		kept = append(kept, e)
	}
	h.entries = kept
}

// save writes the history to path, mode 600.
func (h *history) save(path string) error {
	h.mu.Lock()
	b, err := json.Marshal(h.entries)
	h.mu.Unlock()
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func (h *history) add(e entry) entry {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.lastID++
	e.ID = h.lastID
	if e.Time == "" {
		e.Time = time.Now().UTC().Format("2006-01-02T15:04:05.000Z")
	}
	h.entries = append(h.entries, e)
	h.trim()
	close(h.changed)
	h.changed = make(chan struct{})
	return e
}

// hasWAID reports whether a WhatsApp message is already in the history.
func (h *history) hasWAID(id string) bool {
	if id == "" {
		return false
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, e := range h.entries {
		if e.WAID == id {
			return true
		}
	}
	return false
}

// since returns up to limit entries after afterID (of chat, if not empty),
// whether more are left, and whether entries after afterID were already
// dropped from the ring.
func (h *history) since(afterID int64, chat string, limit int) (out []entry, more, missed bool, last int64, ch <-chan struct{}) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if len(h.entries) > 0 && h.entries[0].ID > afterID+1 {
		missed = true
	}
	for _, e := range h.entries {
		if e.ID <= afterID || (chat != "" && e.Chat != chat) {
			continue
		}
		if len(out) == limit {
			more = true
			break
		}
		out = append(out, e)
	}
	return out, more, missed, h.lastID, h.changed
}
