package main

import (
	"context"
	"errors"
	"fmt"
	"mime"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types"
)

// Media of allowlisted chats (images, videos, audio, documents) is saved
// under the media directory, one mode 700 folder per chat, files mode 600;
// the entry's "media" field is the file's path.

type mediaMessage interface {
	whatsmeow.DownloadableMessage
	GetMimetype() string
	GetFileLength() uint64
}

// mediaOf returns the downloadable part of m, if any.
func mediaOf(m *waE2E.Message) (mediaMessage, string) {
	m = unwrap(m)
	switch {
	case m == nil:
	case m.ImageMessage != nil:
		return m.ImageMessage, ""
	case m.VideoMessage != nil:
		return m.VideoMessage, ""
	case m.AudioMessage != nil:
		return m.AudioMessage, ""
	case m.DocumentMessage != nil:
		return m.DocumentMessage, m.DocumentMessage.GetFileName()
	}
	return nil, ""
}

var commonExt = map[string]string{
	"image/jpeg":      ".jpg",
	"image/png":       ".png",
	"image/webp":      ".webp",
	"image/gif":       ".gif",
	"video/mp4":       ".mp4",
	"audio/ogg":       ".ogg",
	"audio/mpeg":      ".mp3",
	"audio/mp4":       ".m4a",
	"application/pdf": ".pdf",
}

var extRe = regexp.MustCompile(`^\.[A-Za-z0-9]{1,8}$`)

// mediaExt picks a file extension: the document's own, else one for the
// MIME type, else ".bin".
func mediaExt(mimetype, fileName string) string {
	if e := filepath.Ext(fileName); extRe.MatchString(e) {
		return strings.ToLower(e)
	}
	base, _, _ := strings.Cut(mimetype, ";")
	base = strings.TrimSpace(strings.ToLower(base))
	if e, ok := commonExt[base]; ok {
		return e
	}
	if exts, _ := mime.ExtensionsByType(base); len(exts) > 0 && extRe.MatchString(exts[0]) {
		return exts[0]
	}
	return ".bin"
}

var safeIDRe = regexp.MustCompile(`[^A-Za-z0-9_-]`)

// mediaPath is where the media of en goes: DIR/CHAT/YYYYMMDD-HHMMSS-ID.EXT.
func mediaPath(dir string, en entry, ext string) string {
	t, err := time.Parse("2006-01-02T15:04:05.000Z", en.Time)
	if err != nil {
		t = time.Now().UTC()
	}
	id := safeIDRe.ReplaceAllString(en.WAID, "")
	if id == "" {
		id = "x"
	}
	return filepath.Join(dir, en.Chat, t.Format("20060102-150405")+"-"+id+ext)
}

// saveMedia downloads the media of m, if any and not too big, and returns
// its path ("" if there is none or it failed).
func (b *bridge) saveMedia(m *waE2E.Message, en entry) string {
	if b.cfg.MediaDir == "" {
		return ""
	}
	dm, fileName := mediaOf(m)
	if dm == nil {
		return ""
	}
	if n := dm.GetFileLength(); n > uint64(b.cfg.MediaMaxMB)<<20 {
		warnf("media of %s %s: %d MB, over media_max_mb; not saved", en.Chat, en.WAID, n>>20)
		return ""
	}
	path := mediaPath(b.cfg.MediaDir, en, mediaExt(dm.GetMimetype(), fileName))
	if _, err := os.Stat(path); err == nil {
		return path
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		warnf("media: %v", err)
		return ""
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	data, err := b.cli.Download(ctx, dm)
	if err != nil {
		if errors.Is(err, whatsmeow.ErrMediaDownloadFailedWith404) || errors.Is(err, whatsmeow.ErrMediaDownloadFailedWith410) {
			err = fmt.Errorf("%v (no longer on WhatsApp's servers)", err)
		}
		warnf("media of %s %s: %v", en.Chat, en.WAID, err)
		return ""
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0o600); err != nil {
		warnf("media: %v", err)
		return ""
	}
	if err := os.Rename(tmp, path); err != nil {
		warnf("media: %v", err)
		return ""
	}
	return path
}

// backfill asks the phone for up to count messages of chat older than the
// oldest one stored (with latest: than the newest, to fill gaps). With none stored yet, it is done on the chat's next
// message. The messages arrive later, as an on-demand history sync.
func (b *bridge) backfill(ctx context.Context, chat string, count int, latest bool) (string, error) {
	cc := b.cfg.chatByName(chat)
	if cc == nil {
		return "", fmt.Errorf("unknown chat '%s'", chat)
	}
	if !b.cli.IsLoggedIn() {
		return "", errors.New("not connected to WhatsApp")
	}
	count = min(max(count, 1), 500)
	anchor, ok := b.edgeOf(cc, latest)
	if !ok {
		b.backfillMu.Lock()
		b.pendingBackfill[cc.Name] = count
		b.backfillMu.Unlock()
		return fmt.Sprintf("no message of %s stored yet: asking for %d older ones when the next one arrives", cc.Name, count), nil
	}
	if _, err := b.cli.SendPeerMessage(ctx, b.cli.BuildHistorySyncRequest(anchor, count)); err != nil {
		return "", fmt.Errorf("history request failed: %v", err)
	}
	return fmt.Sprintf("asked the phone for %d messages of %s before %s; they arrive in a while (wa_read)", count, cc.Name, anchor.Timestamp.UTC().Format(time.RFC3339)), nil
}

// edgeOf is the oldest (or, with latest, the newest) stored message of cc
// that came from WhatsApp.
func (b *bridge) edgeOf(cc *chatConf, latest bool) (*types.MessageInfo, bool) {
	b.hist.mu.Lock()
	defer b.hist.mu.Unlock()
	var old *entry
	for i, e := range b.hist.entries {
		if e.Chat == cc.Name && e.WAID != "" && (old == nil || (e.Time < old.Time) != latest) {
			old = &b.hist.entries[i]
		}
	}
	if old == nil {
		return nil, false
	}
	t, err := time.Parse("2006-01-02T15:04:05.000Z", old.Time)
	if err != nil {
		return nil, false
	}
	return &types.MessageInfo{
		MessageSource: types.MessageSource{Chat: cc.JID, IsFromMe: old.FromMe, IsGroup: cc.JID.Server == types.GroupServer},
		ID:            old.WAID,
		Timestamp:     t,
	}, true
}

// runPendingBackfill sends a backfill request that waited for a message
// of the chat.
func (b *bridge) runPendingBackfill(chat string) {
	b.backfillMu.Lock()
	count, ok := b.pendingBackfill[chat]
	delete(b.pendingBackfill, chat)
	b.backfillMu.Unlock()
	if !ok {
		return
	}
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		msg, err := b.backfill(ctx, chat, count, false)
		if err != nil {
			warnf("backfill of %s: %v", chat, err)
			return
		}
		warnf("backfill: %s", msg)
	}()
}
