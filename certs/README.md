# TLS Certificate Setup

This folder should contain your TLS certificates for the Mosquitto broker. **Do not commit actual certificate or key files** — they are generated per-deployment.

## What you need

| File | Purpose | Committed? |
|------|---------|------------|
| `ca.key` | CA private key | **No** — never share |
| `ca.crt` | CA certificate | **No** — regenerate per deployment |
| `server.key` | Mosquitto server private key | **No** |
| `server.crt` | Mosquitto server certificate | **No** |

## Generate certificates

Run these commands from this `certs/` directory. Replace `YOUR_LAPTOP_IP` with the IP address your ESP32 will connect to (e.g. `10.45.132.180`).

### Step 1 — Create the CA (Certificate Authority)

```bash
openssl genrsa -out ca.key 2048

openssl req -new -x509 -days 365 -key ca.key -out ca.crt \
  -subj "/CN=AirQuality Lab CA"
```

### Step 2 — Create the server certificate with SAN

Create a config file `server.cnf`:

```ini
[req]
default_bits       = 2048
prompt             = no
distinguished_name = dn
req_extensions     = v3_req

[dn]
CN = AirQuality Broker

[v3_req]
subjectAltName = @alt_names

[alt_names]
IP.1  = YOUR_LAPTOP_IP
IP.2  = 127.0.0.1
DNS.1 = localhost
```

Then generate and sign:

```bash
openssl genrsa -out server.key 2048

openssl req -new -key server.key -out server.csr -config server.cnf

openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out server.crt -days 365 \
  -extfile server.cnf -extensions v3_req
```

### Step 3 — Verify

```bash
openssl verify -CAfile ca.crt server.crt
# Should print: server.crt: OK

openssl x509 -in server.crt -noout -text | grep -A2 "Subject Alternative Name"
# Should show your IP and localhost
```

## Mosquitto configuration

In your `mosquitto.conf`:

```
listener 8884
cafile   /path/to/certs/ca.crt
certfile /path/to/certs/server.crt
keyfile  /path/to/certs/server.key
allow_anonymous false
password_file /path/to/passwd
```

Create users:

```bash
mosquitto_passwd -c /path/to/passwd esp_gateway
mosquitto_passwd    /path/to/passwd nodered
```

## ESP32 gateway

The gateway firmware embeds `ca.crt` at build time via the `EMBED_TXTFILES` directive in `CMakeLists.txt`:

```cmake
EMBED_TXTFILES "../certs/ca.crt"
```

Place `ca.crt` in the gateway project's `certs/` directory (one level above `main/`), or adjust the path in `CMakeLists.txt`.

## If your laptop IP changes

The server certificate's SAN must match the IP the ESP32 connects to. If your laptop gets a new IP (DHCP), you need to:

1. Edit `server.cnf` — update `IP.1`
2. Regenerate `server.csr` and `server.crt` (Steps 2–3 above)
3. Restart Mosquitto
4. Reflash the gateway if you embedded the old `ca.crt` (only needed if you regenerated the CA itself; if you only regenerated the server cert with the same CA, the embedded `ca.crt` still works)

## Node-RED

Point the MQTT broker node's TLS config at `ca.crt`:

- **CA Certificate:** `/path/to/certs/ca.crt`
- **Server Name:** `localhost` (if Node-RED runs on the same machine as Mosquitto)
- **Verify server certificate:** enabled
