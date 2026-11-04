// robot-chat-whatsapp: WhatsApp for agents, next to robot-chat's IRC.
//
// One daemon ('serve') holds the WhatsApp linked-device session (whatsmeow)
// and passes only allowlisted chats on: to IRC channels, and to agents over
// MCP ('mcp', one per agent session, talking to the daemon's socket).
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/mdp/qrterminal/v3"
	"go.mau.fi/whatsmeow"
	"rsc.io/qr"
)

const version = "0.1.0"

const usage = `usage: robot-chat-whatsapp [-c CONFIG] COMMAND

commands:
  pair [-png FILE]      link this bridge to your phone (WhatsApp > Linked devices)
  pair -phone NUMBER    the same with a pairing code typed on the phone, no QR
  serve [-phone NUMBER] run the daemon: WhatsApp session, IRC relay, socket;
                        -phone: pair first if needed, keeping the phone's history sync
  chats [-recent] [FILTER]
                        find group and contact JIDs for the config (daemon running);
                        -recent: only chats with activity, newest first
  status                show the daemon's status
  send CHAT [TEXT|-]    send to an allowlisted rw chat (text from stdin with -)
  mcp [-agent NAME]     MCP stdio server for an agent session
  unlink                log out and remove this linked device

CONFIG defaults to $ROBOT_CHAT_WHATSAPP_CONFIG or ~/.config/robot-chat/whatsapp.conf
`

func umask(m int) int { return syscall.Umask(m) }

func main() {
	fs := flag.NewFlagSet("robot-chat-whatsapp", flag.ExitOnError)
	cfgPath := fs.String("c", defaultConfigPath(), "config file")
	showVersion := fs.Bool("V", false, "print the version")
	fs.Usage = func() { fmt.Fprint(os.Stderr, usage) }
	fs.Parse(os.Args[1:])
	if *showVersion {
		fmt.Println("robot-chat-whatsapp", version)
		return
	}
	if fs.NArg() == 0 {
		fs.Usage()
		os.Exit(2)
	}
	cfg, err := loadConfig(expandHome(*cfgPath))
	if err != nil {
		warnf("%v", err)
		os.Exit(1)
	}
	cmd, args := fs.Arg(0), fs.Args()[1:]
	switch cmd {
	case "pair":
		err = cmdPair(cfg, args)
	case "serve":
		sub := flag.NewFlagSet("serve", flag.ExitOnError)
		phone := sub.String("phone", "", "if not paired yet: pair first with a pairing code for this number")
		sub.Parse(args)
		err = cmdServe(cfg, *phone)
	case "chats", "status", "send":
		err = cmdClient(cfg, cmd, args)
	case "mcp":
		sub := flag.NewFlagSet("mcp", flag.ExitOnError)
		agent := sub.String("agent", "agent", "name shown in front of messages this agent sends")
		sub.Parse(args)
		err = runMCP(cfg, *agent)
	case "unlink":
		err = cmdUnlink(cfg)
	default:
		fs.Usage()
		os.Exit(2)
	}
	if err != nil {
		warnf("%v", err)
		if errors.As(err, new(sessionError)) {
			os.Exit(3)
		}
		os.Exit(1)
	}
}

func refuseIfDaemon(cfg *config) error {
	if socketAlive(cfg.Socket) {
		return fmt.Errorf("the daemon is running (%s); stop it first (systemctl --user stop robot-chat-whatsapp)", cfg.Socket)
	}
	return nil
}

func cmdPair(cfg *config, args []string) error {
	sub := flag.NewFlagSet("pair", flag.ExitOnError)
	png := sub.String("png", "", "also write each QR code to this PNG file")
	phone := sub.String("phone", "", "pair with a code instead of a QR: your number with country code")
	sub.Parse(args)
	if err := refuseIfDaemon(cfg); err != nil {
		return err
	}
	ctx := context.Background()
	container, dev, err := openDevice(ctx, cfg)
	if err != nil {
		return err
	}
	defer container.Close()
	if dev.ID != nil {
		return fmt.Errorf("already paired as %s; run 'unlink' first to pair again", dev.ID)
	}
	cli := whatsmeow.NewClient(dev, nil)
	if err := pairClient(ctx, cli, *phone, *png); err != nil {
		return err
	}
	defer cli.Disconnect()
	fmt.Fprintln(os.Stderr, "finishing the first sync...")
	time.Sleep(15 * time.Second)
	fmt.Fprintln(os.Stderr, "done. Start the daemon: systemctl --user start robot-chat-whatsapp\n"+
		"(the phone's history sync goes to this process: 'serve -phone' pairs and keeps it)")
	return nil
}

// pairClient connects an unpaired client and links it, with a QR code or,
// given phone, a pairing code. It returns once paired, still connected.
func pairClient(ctx context.Context, cli *whatsmeow.Client, phone, png string) error {
	qrc, err := cli.GetQRChannel(ctx)
	if err != nil {
		return err
	}
	if err := cli.Connect(); err != nil {
		return err
	}
	if png != "" {
		defer os.Remove(png)
	}
	codeShown := false
	for item := range qrc {
		switch item.Event {
		case whatsmeow.QRChannelEventCode:
			if phone != "" {
				if !codeShown {
					code, err := cli.PairPhone(ctx, phone, true, whatsmeow.PairClientChrome, "Chrome (Linux)")
					if err != nil {
						cli.Disconnect()
						return fmt.Errorf("pairing code: %v", err)
					}
					codeShown = true
					fmt.Fprintf(os.Stderr, "\nPairing code: %s\n\nOn the phone: WhatsApp > Settings > Linked devices > Link a device >\n\"Link with phone number instead\", then type the code (about 2 minutes).\n", code)
				}
				continue
			}
			fmt.Fprintln(os.Stderr, "\nScan with WhatsApp > Settings > Linked devices > Link a device:")
			qrterminal.GenerateWithConfig(item.Code, qrterminal.Config{
				Level: qrterminal.L, Writer: os.Stderr, HalfBlocks: true,
				BlackChar: qrterminal.BLACK_BLACK, WhiteChar: qrterminal.WHITE_WHITE,
				BlackWhiteChar: qrterminal.BLACK_WHITE, WhiteBlackChar: qrterminal.WHITE_BLACK,
				QuietZone: 2,
			})
			if png != "" {
				if err := writeQRPNG(png, item.Code); err != nil {
					warnf("writing %s: %v", png, err)
				}
			}
			fmt.Fprintf(os.Stderr, "(next code in %v)\n", item.Timeout.Round(time.Second))
		case whatsmeow.QRChannelSuccess.Event:
			fmt.Fprintf(os.Stderr, "paired as %s\n", cli.Store.ID)
			return nil
		case whatsmeow.QRChannelTimeout.Event:
			cli.Disconnect()
			return errors.New("no scan in time; run pair again")
		case "error":
			cli.Disconnect()
			return fmt.Errorf("pairing failed: %v", item.Error)
		default:
			fmt.Fprintf(os.Stderr, "pairing: %s %v\n", item.Event, item.Error)
		}
	}
	cli.Disconnect()
	return errors.New("pairing ended without success")
}

func writeQRPNG(path, code string) error {
	c, err := qr.Encode(code, qr.L)
	if err != nil {
		return err
	}
	c.Scale = 10
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, c.PNG(), 0o600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func cmdUnlink(cfg *config) error {
	if err := refuseIfDaemon(cfg); err != nil {
		return err
	}
	ctx := context.Background()
	container, dev, err := openDevice(ctx, cfg)
	if err != nil {
		return err
	}
	defer container.Close()
	if dev.ID == nil {
		return errors.New("not paired")
	}
	cli := whatsmeow.NewClient(dev, nil)
	if err := cli.Connect(); err != nil {
		return err
	}
	defer cli.Disconnect()
	for i := 0; i < 30 && !cli.IsLoggedIn(); i++ {
		time.Sleep(500 * time.Millisecond)
	}
	if err := cli.Logout(ctx); err != nil {
		return fmt.Errorf("logout: %v (unlink it on the phone: Linked devices)", err)
	}
	fmt.Fprintln(os.Stderr, "unlinked")
	return nil
}

func cmdServe(cfg *config, phone string) error {
	ctx := context.Background()
	container, dev, err := openDevice(ctx, cfg)
	if err != nil {
		return err
	}
	defer container.Close()
	if dev.ID == nil && phone == "" {
		return errors.New("not paired: run 'robot-chat-whatsapp pair' (or 'serve -phone NUMBER') first")
	}
	if len(cfg.Chats) == 0 {
		warnf("no chats allowlisted in %s: nothing will be passed on (find JIDs with 'chats')", cfg.Path)
	}
	b := newBridge(cfg, dev)
	l, err := b.listen()
	if err != nil {
		return err
	}
	defer func() { l.Close(); os.Remove(cfg.Socket) }()
	if cfg.IRCServer != "" {
		if b.irc, err = newIRCRelay(b); err != nil {
			return err
		}
	}
	if dev.ID == nil {
		// Pair in this process, so the phone's history sync reaches it.
		if err := pairClient(ctx, b.cli, phone, ""); err != nil {
			return err
		}
	} else if err := b.cli.Connect(); err != nil {
		return fmt.Errorf("connecting to WhatsApp: %v", err)
	}
	defer b.cli.Disconnect()
	if b.irc != nil {
		b.irc.run()
		defer b.irc.conn.Quit()
	}
	b.renameUnknown()
	go b.serveSocket(l)
	warnf("serving %d chat(s) on %s", len(cfg.Chats), cfg.Socket)
	saveState := func() {
		b.act.save()
		if err := b.hist.save(b.histPath); err != nil {
			warnf("saving history: %v", err)
		}
	}
	defer saveState()
	go func() {
		for range time.Tick(30 * time.Second) {
			saveState()
		}
	}()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	select {
	case s := <-sig:
		warnf("%v, exiting", s)
		return nil
	case err := <-b.fail:
		return sessionError{err}
	}
}

// sessionError ends 'serve' with exit status 3: restarting cannot help
// (unlinked, replaced by another instance, banned, outdated).
type sessionError struct{ error }

func cmdClient(cfg *config, cmd string, args []string) error {
	req := request{Op: cmd} // sent by you: no agent prefix
	switch cmd {
	case "chats":
		if len(args) > 0 && args[0] == "-recent" {
			req.Recent, args = true, args[1:]
		}
		req.Filter = strings.Join(args, " ")
	case "send":
		if len(args) < 1 || len(args) > 2 {
			return errors.New("usage: send CHAT [TEXT|-]")
		}
		req.Chat = args[0]
		if len(args) == 2 && args[1] != "-" {
			req.Text = args[1]
		} else {
			b, err := io.ReadAll(os.Stdin)
			if err != nil {
				return err
			}
			req.Text = string(b)
		}
	}
	r, err := call(cfg.Socket, req, 40*time.Second)
	if err != nil {
		return err
	}
	if r.Error != "" {
		return errors.New(r.Error)
	}
	if cmd == "chats" {
		for _, c := range r.Chats {
			kind := "contact"
			if c.Group {
				kind = "group  "
			}
			conf := ""
			if c.Configured != "" {
				conf = "  (configured as " + c.Configured + ")"
			}
			last := "-"
			if c.Last > 0 {
				last = time.Unix(c.Last, 0).Format("2006-01-02 15:04")
			}
			fmt.Printf("%s  %-16s %4d  %-32s  %s%s\n", kind, last, c.Count, c.JID, c.Name, conf)
		}
		return nil
	}
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	if r.Status != nil {
		return enc.Encode(r.Status)
	}
	return enc.Encode(r.Sent)
}
