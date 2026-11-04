package main

import (
	"cmp"
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"time"

	_ "github.com/mattn/go-sqlite3"
	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/proto/waHistorySync"
	"go.mau.fi/whatsmeow/store"
	"go.mau.fi/whatsmeow/store/sqlstore"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
	waLog "go.mau.fi/whatsmeow/util/log"
	"google.golang.org/protobuf/proto"
)

func warnf(format string, a ...any) {
	fmt.Fprintf(os.Stderr, "robot-chat-whatsapp: "+format+"\n", a...)
}

// openDevice opens the session store, creating it mode 600 in a mode 700
// directory: it holds the keys of the linked device.
func openDevice(ctx context.Context, cfg *config) (*sqlstore.Container, *store.Device, error) {
	if err := os.MkdirAll(filepath.Dir(cfg.Store), 0o700); err != nil {
		return nil, nil, err
	}
	f, err := os.OpenFile(cfg.Store, os.O_RDWR|os.O_CREATE, 0o600)
	if err != nil {
		return nil, nil, err
	}
	f.Close()
	if st, err := os.Stat(cfg.Store); err == nil && st.Mode().Perm()&0o077 != 0 {
		warnf("warning: %s is readable by other users (chmod 600 it)", cfg.Store)
	}
	store.SetOSInfo("robot-chat", [3]uint32{0, 1, 0})
	c, err := sqlstore.New(ctx, "sqlite3", "file:"+cfg.Store+"?_foreign_keys=on&_busy_timeout=5000", waLog.Stdout("store", "WARN", false))
	if err != nil {
		return nil, nil, err
	}
	dev, err := c.GetFirstDevice(ctx)
	if err != nil {
		c.Close()
		return nil, nil, err
	}
	return c, dev, nil
}

type bridge struct {
	cfg  *config
	cli  *whatsmeow.Client
	hist *history
	act  *activity
	irc  *ircRelay

	histPath string     // allowlisted messages, kept across restarts
	fail     chan error // fatal session events: logged out, replaced, banned

	sendMu    sync.Mutex
	sendTimes []time.Time
}

func newBridge(cfg *config, dev *store.Device) *bridge {
	b := &bridge{
		cfg:  cfg,
		cli:  whatsmeow.NewClient(dev, waLog.Stdout("whatsapp", "WARN", false)),
		hist: newHistory(cfg.History),
		act:  loadActivity(strings.TrimSuffix(cfg.Store, ".db") + "-activity.json"),
		fail: make(chan error, 1),
	}
	b.histPath = strings.TrimSuffix(cfg.Store, ".db") + "-history.json"
	b.hist.load(b.histPath, func(e entry) bool { return cfg.chatByName(e.Chat) != nil })
	// History syncs are read for the chats' activity times only; their
	// messages are not imported, agents only see live ones.
	b.cli.AddEventHandler(b.onEvent)
	return b
}

func (b *bridge) fatal(err error) {
	select {
	case b.fail <- err:
	default:
	}
}

func (b *bridge) onEvent(evt any) {
	switch e := evt.(type) {
	case *events.Message:
		b.onMessage(e)
	case *events.HistorySync:
		n := b.act.fromHistory(e.Data)
		m := b.fromHistory(e.Data)
		if n > 0 || m > 0 {
			warnf("history sync (%v): activity times of %d chats, %d messages of allowlisted chats", e.Data.GetSyncType(), n, m)
		}
	case *events.Connected:
		warnf("connected to WhatsApp as %s", b.cli.Store.ID)
	case *events.Disconnected:
		warnf("disconnected from WhatsApp, reconnecting")
	case *events.LoggedOut:
		b.fatal(fmt.Errorf("logged out by WhatsApp (reason %v): the device was unlinked; run 'pair' again", e.Reason))
	case *events.StreamReplaced:
		b.fatal(errors.New("another client connected with this device (stream replaced); only one 'serve' may run"))
	case *events.TemporaryBan:
		b.fatal(fmt.Errorf("WhatsApp temporary ban: %v", e))
	case *events.ClientOutdated:
		b.fatal(errors.New("WhatsApp says this client is outdated: update whatsmeow and rebuild"))
	case *events.ConnectFailure:
		warnf("connect failure: %v %s", e.Reason, e.Message)
	}
}

// chatOf maps the message's chat to an allowlisted chat. Direct chats may
// be addressed by LID (hidden user id) instead of the phone number.
func (b *bridge) chatOf(info *types.MessageInfo) *chatConf {
	chat := info.Chat.ToNonAD()
	if cc := b.cfg.chatByJID(chat); cc != nil {
		return cc
	}
	if chat.Server != types.HiddenUserServer {
		return nil
	}
	for _, alt := range []types.JID{info.SenderAlt, info.RecipientAlt} {
		if alt.Server == types.DefaultUserServer {
			if cc := b.cfg.chatByJID(alt); cc != nil {
				return cc
			}
		}
	}
	if pn, err := b.cli.Store.LIDs.GetPNForLID(context.Background(), chat); err == nil && !pn.IsEmpty() {
		return b.cfg.chatByJID(pn)
	}
	return nil
}

// pnChat is the chat's JID, with a LID direct chat mapped to the phone
// number when known.
func (b *bridge) pnChat(info *types.MessageInfo) types.JID {
	chat := info.Chat.ToNonAD()
	if chat.Server != types.HiddenUserServer {
		return chat
	}
	for _, alt := range []types.JID{info.SenderAlt, info.RecipientAlt} {
		if alt.Server == types.DefaultUserServer {
			return alt.ToNonAD()
		}
	}
	if pn, err := b.cli.Store.LIDs.GetPNForLID(context.Background(), chat); err == nil && !pn.IsEmpty() {
		return pn
	}
	return chat
}

func (b *bridge) senderName(info *types.MessageInfo) string {
	if info.IsFromMe {
		return b.cfg.MyName
	}
	if n := b.contactName(info.Sender, info.SenderAlt); n != "" {
		return n
	}
	if info.PushName != "" {
		return "~" + info.PushName
	}
	return "~unknown"
}

// contactName is the saved contact name (or, failing that, the WhatsApp
// profile name as "~name") of the first JID that has one. LIDs are also
// tried as their phone number.
func (b *bridge) contactName(jids ...types.JID) string {
	ctx := context.Background()
	var all []types.JID
	for _, j := range jids {
		if j.IsEmpty() {
			continue
		}
		j = j.ToNonAD()
		all = append(all, j)
		if j.Server == types.HiddenUserServer {
			if pn, err := b.cli.Store.LIDs.GetPNForLID(ctx, j); err == nil && !pn.IsEmpty() {
				all = append(all, pn)
			}
		}
	}
	push := ""
	for _, j := range all {
		c, err := b.cli.Store.Contacts.GetContact(ctx, j)
		if err != nil || !c.Found {
			continue
		}
		if c.FullName != "" {
			return c.FullName
		}
		if c.FirstName != "" {
			return c.FirstName
		}
		if push == "" && c.PushName != "" {
			push = "~" + c.PushName
		}
	}
	return push
}

// renameUnknown gives names to entries saved as "~<lid digits>" by
// earlier versions, now that contacts and LID mappings may be known.
func (b *bridge) renameUnknown() {
	b.hist.mu.Lock()
	defer b.hist.mu.Unlock()
	for i, e := range b.hist.entries {
		u := strings.TrimPrefix(e.From, "~")
		if u == e.From || u == "" || strings.Trim(u, "0123456789") != "" {
			continue
		}
		if n := b.contactName(types.NewJID(u, types.HiddenUserServer), types.NewJID(u, types.DefaultUserServer)); n != "" {
			b.hist.entries[i].From = n
		} else {
			b.hist.entries[i].From = "~unknown"
		}
	}
}

func (b *bridge) onMessage(e *events.Message) {
	b.act.seen(b.pnChat(&e.Info), e.Info.Timestamp, true)
	if en, ok := b.entryFor(e); ok {
		en = b.hist.add(en)
		if b.irc != nil {
			b.irc.relay(en)
		}
	}
}

// entryFor turns a message of an allowlisted chat into a history entry.
func (b *bridge) entryFor(e *events.Message) (entry, bool) {
	cc := b.chatOf(&e.Info)
	if cc == nil {
		return entry{}, false // not allowlisted: never stored, never relayed
	}
	m, edited := e.Message, e.IsEdit
	if pm := m.GetProtocolMessage(); pm != nil {
		if pm.GetType() != waE2E.ProtocolMessage_MESSAGE_EDIT {
			return entry{}, false
		}
		m, edited = pm.GetEditedMessage(), true
	}
	text := messageText(m)
	if text == "" {
		return entry{}, false
	}
	return entry{
		Time:   e.Info.Timestamp.UTC().Format("2006-01-02T15:04:05.000Z"),
		Chat:   cc.Name,
		Group:  cc.JID.Server == types.GroupServer,
		From:   cleanText(b.senderName(&e.Info)),
		FromMe: e.Info.IsFromMe,
		Edited: edited,
		Text:   text,
		WAID:   e.Info.ID,
	}, true
}

// fromHistory imports the recent messages that a history sync carries for
// allowlisted chats, oldest first, skipping ones already in the history.
// They go to agents (wa_read) but are not relayed to IRC.
func (b *bridge) fromHistory(h *waHistorySync.HistorySync) int {
	var got []entry
	for _, conv := range h.GetConversations() {
		jid, err := types.ParseJID(conv.GetID())
		if err != nil || b.cfg.chatByJID(jid) == nil {
			continue
		}
		for _, hm := range conv.GetMessages() {
			evt, err := b.cli.ParseWebMessage(jid, hm.GetMessage())
			if err != nil {
				continue
			}
			if en, ok := b.entryFor(evt); ok && !b.hist.hasWAID(en.WAID) {
				en.History = true
				got = append(got, en)
			}
		}
	}
	slices.SortStableFunc(got, func(x, y entry) int { return strings.Compare(x.Time, y.Time) })
	for _, en := range got {
		b.hist.add(en)
	}
	return len(got)
}

// send sends text to an allowlisted rw chat, as agent (shown as a
// "[agent] " prefix when send_prefix is on), within send_rate. origin is
// "irc" or "mcp"; what came from IRC is not relayed back there.
func (b *bridge) send(ctx context.Context, chat, text, agent, origin string) (entry, error) {
	cc := b.cfg.chatByName(chat)
	if cc == nil {
		return entry{}, fmt.Errorf("unknown chat '%s' (see wa_chats)", chat)
	}
	if !cc.RW {
		return entry{}, fmt.Errorf("chat '%s' is read-only (mode ro in %s)", cc.Name, b.cfg.Path)
	}
	text = strings.TrimSpace(cleanText(text))
	if text == "" {
		return entry{}, errors.New("empty message")
	}
	if !b.cli.IsLoggedIn() {
		return entry{}, errors.New("not connected to WhatsApp")
	}

	b.sendMu.Lock()
	now := time.Now()
	recent := b.sendTimes[:0]
	for _, t := range b.sendTimes {
		if now.Sub(t) < time.Minute {
			recent = append(recent, t)
		}
	}
	b.sendTimes = recent
	if len(recent) >= b.cfg.SendRate {
		b.sendMu.Unlock()
		return entry{}, fmt.Errorf("over send_rate (%d per minute); try again later", b.cfg.SendRate)
	}
	b.sendTimes = append(b.sendTimes, now)
	b.sendMu.Unlock()

	body := text
	if b.cfg.SendPrefix && agent != "" {
		body = "[" + agent + "] " + text
	}
	if _, err := b.cli.SendMessage(ctx, cc.JID, &waE2E.Message{Conversation: proto.String(body)}); err != nil {
		return entry{}, fmt.Errorf("WhatsApp send failed: %v", err)
	}
	en := b.hist.add(entry{
		Chat:   cc.Name,
		Group:  cc.JID.Server == types.GroupServer,
		From:   b.cfg.MyName,
		FromMe: true,
		Agent:  agent,
		Text:   text,
	})
	if b.irc != nil && origin != "irc" {
		b.irc.relay(en)
	}
	return en, nil
}

// chatInfo is one line of the 'chats' listing.
type chatInfo struct {
	JID        string `json:"jid"`
	Name       string `json:"name"`
	Group      bool   `json:"group"`
	Configured string `json:"configured,omitempty"`
	Last       int64  `json:"last,omitempty"`  // unix time of the last message seen
	Count      int    `json:"count,omitempty"` // messages seen live
}

// listChats lists groups and saved contacts whose name or number contains
// filter (case-insensitive), to find JIDs for the config.
func (b *bridge) listChats(ctx context.Context, filter string, recent bool) ([]chatInfo, error) {
	filter = strings.ToLower(filter)
	match := func(s ...string) bool {
		for _, x := range s {
			if strings.Contains(strings.ToLower(x), filter) {
				return true
			}
		}
		return false
	}
	var out []chatInfo
	add := func(jid types.JID, name string, group bool) {
		ci := chatInfo{JID: jid.String(), Name: name, Group: group}
		if cc := b.cfg.chatByJID(jid); cc != nil {
			ci.Configured = cc.Name
		}
		if t, n := b.act.last(jid); !t.IsZero() {
			ci.Last, ci.Count = t.Unix(), n
		} else if recent {
			return
		}
		out = append(out, ci)
	}
	groups, err := b.cli.GetJoinedGroups(ctx)
	if err != nil {
		return nil, fmt.Errorf("listing groups: %v", err)
	}
	for _, g := range groups {
		if match(g.Name, g.JID.User) {
			add(g.JID, g.Name, true)
		}
	}
	contacts, err := b.cli.Store.Contacts.GetAllContacts(ctx)
	if err != nil {
		return nil, fmt.Errorf("listing contacts: %v", err)
	}
	for jid, c := range contacts {
		if jid.Server != types.DefaultUserServer || c.FullName == "" {
			continue // only saved contacts with a phone-number JID
		}
		if match(c.FullName, c.FirstName, jid.User) {
			add(jid, c.FullName, false)
		}
	}
	if recent {
		slices.SortFunc(out, func(x, y chatInfo) int { return cmp.Compare(y.Last, x.Last) })
	}
	return out, nil
}
