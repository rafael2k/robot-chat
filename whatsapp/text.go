package main

import (
	"fmt"
	"strings"
	"unicode/utf8"

	"go.mau.fi/whatsmeow/proto/waE2E"
	"google.golang.org/protobuf/reflect/protoreflect"
)

// cleanText makes text safe to relay: valid UTF-8, \r\n and \r as \n, and no
// control characters other than tab and newline.
func cleanText(s string) string {
	s = strings.ToValidUTF8(s, "�")
	s = strings.ReplaceAll(s, "\r\n", "\n")
	s = strings.ReplaceAll(s, "\r", "\n")
	return strings.Map(func(r rune) rune {
		if r == '\n' || r == '\t' || (r >= 0x20 && r != 0x7f) {
			return r
		}
		return -1
	}, s)
}

// splitLine cuts a line into pieces of at most max bytes, on UTF-8
// boundaries, after a space when there is one in the second half.
func splitLine(line string, max int) []string {
	var out []string
	for len(line) > max {
		cut := max
		for cut > 0 && !utf8.RuneStart(line[cut]) {
			cut--
		}
		if sp := strings.LastIndexByte(line[:cut], ' '); sp >= cut/2 {
			cut = sp + 1
		}
		out = append(out, line[:cut])
		line = line[cut:]
	}
	return append(out, line)
}

func firstLine(s string, max int) string {
	s, _, _ = strings.Cut(s, "\n")
	if len(s) > max {
		cut := max
		for cut > 0 && !utf8.RuneStart(s[cut]) {
			cut--
		}
		s = s[:cut] + "…"
	}
	return s
}

// unwrap takes the content out of wrappers that whatsmeow leaves in place:
// an album's images come as "associated child" messages.
func unwrap(m *waE2E.Message) *waE2E.Message {
	for m.GetAssociatedChildMessage().GetMessage() != nil {
		m = m.GetAssociatedChildMessage().GetMessage()
	}
	return m
}

// messageText turns a WhatsApp message into text. Media becomes a
// "[image] caption" style marker; the file itself is saved by saveMedia.
// Returns "" for messages with nothing to show (receipts, key updates, ...).
func messageText(m *waE2E.Message) string {
	m = unwrap(m)
	if m == nil {
		return ""
	}
	var text string
	var ctx *waE2E.ContextInfo
	withCaption := func(kind, caption string) string {
		if caption == "" {
			return "[" + kind + "]"
		}
		return "[" + kind + "] " + caption
	}
	switch {
	case m.Conversation != nil:
		text = m.GetConversation()
	case m.ExtendedTextMessage != nil:
		text = m.ExtendedTextMessage.GetText()
		ctx = m.ExtendedTextMessage.GetContextInfo()
	case m.ImageMessage != nil:
		text = withCaption("image", m.ImageMessage.GetCaption())
		ctx = m.ImageMessage.GetContextInfo()
	case m.VideoMessage != nil:
		text = withCaption("video", m.VideoMessage.GetCaption())
		ctx = m.VideoMessage.GetContextInfo()
	case m.AudioMessage != nil:
		kind := "audio"
		if m.AudioMessage.GetPTT() {
			kind = "voice message"
		}
		text = fmt.Sprintf("[%s %ds]", kind, m.AudioMessage.GetSeconds())
		ctx = m.AudioMessage.GetContextInfo()
	case m.DocumentMessage != nil:
		text = withCaption("document: "+m.DocumentMessage.GetFileName(), m.DocumentMessage.GetCaption())
		ctx = m.DocumentMessage.GetContextInfo()
	case m.StickerMessage != nil:
		text = "[sticker]"
	case m.AlbumMessage != nil:
		a := m.AlbumMessage
		text = fmt.Sprintf("[album: %d images, %d videos]", a.GetExpectedImageCount(), a.GetExpectedVideoCount())
	case m.LocationMessage != nil:
		l := m.LocationMessage
		text = fmt.Sprintf("[location %.5f,%.5f] %s", l.GetDegreesLatitude(), l.GetDegreesLongitude(), l.GetName())
	case m.LiveLocationMessage != nil:
		text = "[live location]"
	case m.ContactMessage != nil:
		text = "[contact: " + m.ContactMessage.GetDisplayName() + "]"
	case m.ReactionMessage != nil:
		if r := m.ReactionMessage.GetText(); r != "" {
			text = "[reacted " + r + "]"
		}
	case m.PollCreationMessage != nil:
		p := m.PollCreationMessage
		var opts []string
		for _, o := range p.GetOptions() {
			opts = append(opts, o.GetOptionName())
		}
		text = "[poll] " + p.GetName() + " (" + strings.Join(opts, " / ") + ")"
	}
	text = strings.TrimRight(cleanText(text), "\n")
	if text != "" && ctx != nil && ctx.QuotedMessage != nil {
		if q := messageText(ctx.QuotedMessage); q != "" {
			text = "> " + firstLine(q, 80) + "\n" + text
		}
	}
	return text
}

// messageKinds lists the fields set in m, to log messages messageText
// does not know.
func messageKinds(m *waE2E.Message) string {
	var kinds []string
	m.ProtoReflect().Range(func(fd protoreflect.FieldDescriptor, _ protoreflect.Value) bool {
		kinds = append(kinds, fd.JSONName())
		return true
	})
	return strings.Join(kinds, ",")
}
