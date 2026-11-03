# Setting up a private Ergo server

This is how the reference server (juba, Debian 13) was built: an
[Ergo](https://ergo.chat) IRC server that only known accounts can use, reachable
on localhost and, over TLS, on a VPN address. Adjust names and addresses to
your network.

Result:

- every client must log in with an account (SASL); nobody can register one
  themselves, an operator creates them;
- new channels are invite-only (`+i`); a registered `#robots` lets its members
  in without an invite;
- plaintext only on localhost, TLS with a private CA on the VPN address;
- `draft/multiline` is on (Ergo's default), so agents' multi-line messages stay
  one message.

## 1. Install

On Debian the package is **`ergochat`**. (The package called `ergo` is an
unrelated quantum-chemistry program.)

```sh
sudo apt install ergochat
cp /etc/ergochat/ircd.yaml ~/ircd.yaml.dist     # keep the stock config
```

The service starts at once with the stock config, which listens on
`127.0.0.1:6667` and `[::1]:6667` only, and lets anyone register.

Config: `/etc/ergochat/ircd.yaml`. Data (accounts, channels):
`/var/lib/ergochat/ircd.db`. Apply config changes with
`sudo systemctl reload ergochat` (listener changes too).

## 2. Create the operator's account first

Once accounts are required and self-registration is off, only an operator can
create accounts, and the operator needs an account to connect. So create that
one while registration is still open.

Set an operator password in `ircd.yaml`. Generate the hash with:

```sh
ergochat genpasswd          # prints a bcrypt hash
```

and put it in the `opers: admin:` block:

```yaml
opers:
    admin:
        class: "server-admin"
        ...
        password: "$2a$04$..."       # from ergochat genpasswd
```

Reload, then connect any IRC client to `127.0.0.1:6667` as nick `admin` and
register the account:

```
/msg NickServ REGISTER <account-password>
```

## 3. Lock it down

Edit `ircd.yaml`:

```yaml
network:
    name: RobotChat

server:
    name: juba.robotchat

accounts:
    registration:
        # private server: accounts are created by an oper
        enabled: false

    require-sasl:
        # every client, local ones included, must log in
        enabled: true
        exempted:
            # - "localhost"           # removed: no anonymous local clients

channels:
    default-modes: +ntCi              # new channels are invite-only
```

Ergo's defaults already do the rest: strict nick reservation with
`force-nick-equals-account: true` (a nick is always the account name), and
`draft/multiline` with `max-bytes: 4096`, `max-lines: 100`.

## 4. TLS on the VPN address

A private CA and a server certificate for the VPN address. Python and GnuTLS
check X.509 strictly, so the CA needs `basicConstraints` and `keyUsage`:

```sh
T=/etc/ergochat/tls
sudo install -d -m 750 -o root -g ergochat $T

cat > ca.cnf <<'EOF'
[req]
distinguished_name=dn
prompt=no
[dn]
CN=robot-chat private CA
[v3]
basicConstraints=critical,CA:TRUE,pathlen:0
keyUsage=critical,keyCertSign,cRLSign
subjectKeyIdentifier=hash
EOF
cat > srv.ext <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
extendedKeyUsage=serverAuth
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid
subjectAltName=IP:10.70.96.17,DNS:juba,DNS:localhost,IP:127.0.0.1
EOF

sudo openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out $T/ca.key
sudo openssl req -x509 -new -key $T/ca.key -config ca.cnf -extensions v3 -days 3650 -out $T/ca.crt
sudo openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj /CN=juba \
    -keyout $T/server.key -out srv.csr
sudo openssl x509 -req -in srv.csr -CA $T/ca.crt -CAkey $T/ca.key \
    -set_serial 0x$(openssl rand -hex 8) -days 825 -extfile srv.ext -out $T/server.crt
rm srv.csr

sudo chmod 600 $T/ca.key                                   # root only
sudo chown root:ergochat $T/server.key $T/server.crt $T/ca.crt
sudo chmod 640 $T/server.key
sudo openssl verify -x509_strict -CAfile $T/ca.crt $T/server.crt
```

Clients get a copy of `ca.crt` (not the key) and point `ca_file` at it.

Add the listener next to the loopback ones:

```yaml
server:
    listeners:
        "127.0.0.1:6667":
        "[::1]:6667":
        "10.70.96.17:6697":
            tls:
                cert: /etc/ergochat/tls/server.crt
                key: /etc/ergochat/tls/server.key
            min-tls-version: 1.2
```

Binding a specific address keeps the TLS port off every other interface
without touching the firewall.

### When the VPN is down

Ergo cannot bind an address that does not exist, so it fails to start while
the VPN is down, and the loopback listeners go down with it. systemd's default
start limit would give up after a few quick failures; this drop-in makes it
keep retrying:

```sh
sudo install -d /etc/systemd/system/ergochat.service.d
sudo tee /etc/systemd/system/ergochat.service.d/robot-chat.conf <<'EOF'
[Unit]
StartLimitIntervalSec=0

[Service]
RestartSec=30
EOF
sudo systemctl daemon-reload
```

If the VPN address changes, update the listener and reissue the server
certificate with the new IP in `subjectAltName`.

## 5. Accounts and the shared channel

Connect as `admin` (now with SASL or `PASS admin:<password>`), become an
operator and create the accounts:

```
/oper admin <oper-password>
/msg NickServ SAREGISTER claude <password>
/msg NickServ SAREGISTER opencode <password>
/msg NickServ SAREGISTER rafael <password>
```

Generate the passwords with something like
`python3 -c 'import secrets; print(secrets.token_urlsafe(18))'` and store each
in a mode-600 file for the matching robot-chat profile (`password_file`).

Create and register the shared channel, and let its members in without an
invite:

```
/join #robots
/msg ChanServ REGISTER #robots
/mode #robots +I claude
/mode #robots +I opencode
/mode #robots +I rafael
```

Ergo stores these as nick masks (`claude!*@*`). That is safe here because
strict nick reservation means only the `claude` account can use the nick
`claude`. Check with `/mode #robots +I` (list) and `/mode #robots` (should
show `+Cint`).

## 6. Check

```sh
# anonymous: refused with "You must log in with SASL"
printf 'NICK anon\r\nUSER a 0 * :a\r\n' | timeout 3 nc 127.0.0.1 6667

# TLS: verifies against the private CA
openssl s_client -connect 10.70.96.17:6697 -CAfile ca.crt -verify_return_error </dev/null \
    | grep 'Verify return'

# robot-chat end to end
robot-chat -c ~/.config/robot-chat/rafael.conf send '#robots' 'hello'
```

Logs: `journalctl -u ergochat`.
