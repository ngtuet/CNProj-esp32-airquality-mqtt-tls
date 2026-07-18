# MQTT-TLS Security Benchmark

A pseudo-attack validation script that tests the Mosquitto broker's TLS and authentication configuration against 11 checks from the project's security evaluation (Table II in the lab report).

**This is an educational tool for validating your own broker. Do not run it against systems you do not own.**

## Requirements

```
pip install paho-mqtt
```

Python 3.7+ required. No other dependencies.

## Quick Start

Make sure Mosquitto is running with TLS enabled, then:

```bash
python mqtt_tls_benchmark.py \
  --host 127.0.0.1 --port 8884 \
  --cafile ../certs/ca.crt \
  --user esp_gateway --password your_password \
  --topic "your/topic/here"
```

To also test ACL cross-user restrictions, add a second account:

```bash
python mqtt_tls_benchmark.py \
  --host 127.0.0.1 --port 8884 \
  --cafile ../certs/ca.crt \
  --user esp_gateway --password your_password \
  --user2 nodered --password2 your_nodered_password \
  --topic "your/topic/here"
```

## What It Tests

| ID | Check | Expected result | What happens if it fails |
|----|-------|----------------|--------------------------|
| T01 | Tools & CA present | Pass | CA file missing or paho-mqtt not installed |
| T02 | TCP port reachable | Pass | Broker not running or firewall blocking port |
| T03 | Plaintext to TLS port | Pass (rejected) | Broker accepted a non-TLS client — TLS not enforced |
| T04 | Trusted TLS handshake | Pass | CA mismatch or credentials wrong |
| T05 | Wrong hostname | Pass (rejected) | Broker accepted a hostname that doesn't match the cert SAN |
| T06 | Missing credentials | Pass (rejected) | Anonymous access is enabled — set `allow_anonymous false` |
| T07 | Wrong password | Pass (rejected) | Password check is broken or bypassed |
| T08 | Authorized publish | Pass | Valid credentials can't publish — check ACL or topic |
| T09 | ACL subscribe (cross-user) | Pass (blocked) | user2 can read user1's topic — tighten the ACL file |
| T10 | ACL publish (cross-user) | Pass (blocked) | user2 can write to user1's topic — tighten the ACL file |
| T11 | Replay / freshness | Partial | Both duplicates accepted — this is expected (see below) |

## Understanding the Results

**Pass** — The broker behaved correctly. The attack was blocked or the valid operation succeeded.

**Partial** — The test produced a known limitation, not a misconfiguration. T11 (replay) will always show Partial because MQTT has no built-in replay protection at the protocol level. Duplicate messages are accepted as-is. Mitigation would require application-layer nonce or timestamp checking, which is identified as future work.

**Fail** — The broker has a configuration issue that should be fixed before deployment. The evidence column explains what went wrong.

**Skipped** — T09 and T10 require `--user2` / `--password2`. If not provided, ACL cross-user tests are skipped.

**Inconclusive** — The test ran but couldn't determine the outcome (e.g., network timeout, unexpected CONNACK code). Check the evidence text and re-run.

## Example Output

```
══════════════════════════════════════════════════════════
  MQTT-TLS SECURITY BENCHMARK
══════════════════════════════════════════════════════════

  Target:  127.0.0.1:8884
  CA:      ../certs/ca.crt
  User:    esp_gateway
  Topic:   vgu/airquality/node1

  [PASS]  T01 Tools & CA present
          CA file: found (../certs/ca.crt), paho-mqtt: importable

  [PASS]  T02 TCP port reachable
          127.0.0.1:8884 accepted TCP connection

  [PASS]  T03 Plaintext to TLS port
          Plaintext client rejected (connection failed or dropped)

  [PASS]  T04 Trusted TLS handshake
          Connected — TLSv1.3 / TLS_AES_256_GCM_SHA384

  [PASS]  T05 Wrong hostname
          Certificate verification failed (hostname mismatch)

  [PASS]  T06 Missing credentials
          CONNACK code 5 — not authorized

  [PASS]  T07 Wrong password
          CONNACK code 5 — not authorized

  [PASS]  T08 Authorized publish
          Message arrived: {"co_ppb": 1.2, "no2_ppb": 38.0, ...}

  [Partial] T11 Replay / freshness check
          Both duplicates accepted (2 received) — MQTT has no
          built-in replay protection.

  ─────────────────────────────────────────────────────
  9/11 checks passed (0 skipped, 11 total)
```

## How It Relates to the Report

This script automates the manual checks described in Section IV-C (TLS Verification) and Table II (Pseudo-attack benchmark) of the IEEE lab report. Each test ID corresponds to a row in Table II. Running the script reproduces the same evidence that was collected manually during the lab session, making the security claims in the report independently verifiable.

## Mosquitto Configuration Checklist

For all 11 tests to produce the expected results, your `mosquitto.conf` should have:

```
# TLS only — no plaintext listener
listener 8884
cafile     /path/to/ca.crt
certfile   /path/to/server.crt
keyfile    /path/to/server.key

# Authentication required
allow_anonymous false
password_file /path/to/passwd

# ACL (optional, enables T09/T10)
acl_file /path/to/acl
```

Example ACL file for cross-user isolation:

```
user esp_gateway
topic write vgu/airquality/node1

user nodered
topic read vgu/airquality/node1
```

This grants `esp_gateway` write-only and `nodered` read-only on the topic, which is the least-privilege model for a sensor gateway publishing to a dashboard subscriber.

## License

Educational use. Part of the [esp32-airquality-mqtt-tls](../) project.
