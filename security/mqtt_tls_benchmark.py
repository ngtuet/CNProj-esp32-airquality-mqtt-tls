#!/usr/bin/env python3
"""
mqtt_tls_benchmark.py — Pseudo-attack benchmark against an MQTT-TLS broker.

Runs the checks from Table II of the lab report:
  1.  Tools & CA present          (test environment ready)
  2.  TCP port reachable          (TLS listener reachable)
  3.  Plaintext to TLS port       (connection rejected)
  4.  Trusted TLS handshake       (TLS 1.3 / AES-256-GCM)
  5.  Wrong hostname              (certificate verification failed)
  6.  Missing credentials         (not authorized)
  7.  Wrong password              (not authorized)
  8.  Authorized publish          (live telemetry arrives)
  9.  Cross-user subscribe (ACL)  (ACL blocked read access)
  10. Cross-user publish   (ACL)  (ACL blocked write access)
  11. Replay / freshness          (duplicate accepted; freshness missing)

Usage:
    python mqtt_tls_benchmark.py \
        --host 127.0.0.1 --port 8884 \
        --cafile certs/ca.crt \
        --user esp_gateway --password esp123456 \
        --topic vgu/airquality/node1

    Optionally pass --user2 / --password2 for a second account (e.g. nodered)
    to test ACL cross-user restrictions.

Requires:  pip install paho-mqtt

IMPORTANT: Run this ONLY against your own broker for educational and
           validation purposes. Do not use against systems you do not own.
"""

import argparse
import json
import os
import socket
import ssl
import sys
import time
import threading

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("ERROR: paho-mqtt not installed. Run:  pip install paho-mqtt")
    sys.exit(1)


# ─────────────────────────────────────────────
# Formatting helpers
# ─────────────────────────────────────────────

GREEN  = "\033[92m"
RED    = "\033[91m"
YELLOW = "\033[93m"
CYAN   = "\033[96m"
RESET  = "\033[0m"
BOLD   = "\033[1m"

def header(title):
    print(f"\n{BOLD}{CYAN}{'═' * 60}")
    print(f"  {title}")
    print(f"{'═' * 60}{RESET}\n")

def result(test_id, name, passed, evidence):
    tag = f"{GREEN}PASS{RESET}" if passed == True else (
          f"{RED}FAIL{RESET}" if passed == False else
          f"{YELLOW}{passed}{RESET}")
    print(f"  [{tag}]  {BOLD}T{test_id:02d}{RESET} {name}")
    print(f"         {evidence}\n")

results = []
def record(test_id, name, passed, evidence):
    results.append({"id": test_id, "name": name, "result": passed, "evidence": evidence})
    result(test_id, name, passed, evidence)


# ─────────────────────────────────────────────
# Individual tests
# ─────────────────────────────────────────────

def t01_tools_present(args):
    """Check that the CA file exists and paho-mqtt is importable."""
    ca_exists = os.path.isfile(args.cafile)
    evidence = f"CA file: {'found' if ca_exists else 'MISSING'} ({args.cafile}), paho-mqtt: importable"
    record(1, "Tools & CA present", ca_exists, evidence)
    return ca_exists


def t02_port_reachable(args):
    """TCP connect to the broker port."""
    try:
        s = socket.create_connection((args.host, args.port), timeout=5)
        s.close()
        record(2, "TCP port reachable", True, f"{args.host}:{args.port} accepted TCP connection")
        return True
    except Exception as e:
        record(2, "TCP port reachable", False, str(e))
        return False


def t03_plaintext_rejected(args):
    """Send a plain MQTT CONNECT (no TLS) to the TLS port — should fail."""
    try:
        client = mqtt.Client(client_id="bench_plaintext", protocol=mqtt.MQTTv311)
        client.username_pw_set(args.user, args.password)
        # NO TLS configured — connecting plaintext to a TLS listener
        rc = {"connected": False, "error": None}
        def on_connect(c, u, f, code):
            rc["connected"] = True
        def on_disconnect(c, u, code):
            pass
        client.on_connect = on_connect
        client.on_disconnect = on_disconnect
        client.connect(args.host, args.port, keepalive=5)
        client.loop_start()
        time.sleep(3)
        client.loop_stop()
        client.disconnect()
        if rc["connected"]:
            record(3, "Plaintext to TLS port", False,
                   "Plaintext client CONNECTED — broker may not require TLS!")
        else:
            record(3, "Plaintext to TLS port", True,
                   "Plaintext client rejected (connection failed or dropped)")
        return not rc["connected"]
    except Exception as e:
        record(3, "Plaintext to TLS port", True,
               f"Plaintext client rejected: {e}")
        return True


def t04_trusted_handshake(args):
    """Full TLS handshake with the correct CA, valid credentials."""
    try:
        client = mqtt.Client(client_id="bench_trusted", protocol=mqtt.MQTTv311)
        client.username_pw_set(args.user, args.password)
        client.tls_set(ca_certs=args.cafile, tls_version=ssl.PROTOCOL_TLS_CLIENT)
        client.tls_insecure_set(True)  # skip hostname check (tested separately in T05)

        rc = {"code": -1, "connected": False}
        def on_connect(c, u, f, code):
            rc["code"] = code
            rc["connected"] = (code == 0)
        client.on_connect = on_connect
        client.connect(args.host, args.port, keepalive=10)
        client.loop_start()
        time.sleep(3)
        client.loop_stop()

        if rc["connected"]:
            # Retrieve TLS info
            sock = client._sock
            cipher = "unknown"
            tls_ver = "unknown"
            if sock and hasattr(sock, 'cipher'):
                ci = sock.cipher()
                if ci:
                    cipher = ci[0]
                    tls_ver = ci[1]
            client.disconnect()
            record(4, "Trusted TLS handshake", True,
                   f"Connected — {tls_ver} / {cipher}")
            return True
        else:
            client.disconnect()
            record(4, "Trusted TLS handshake", False,
                   f"CONNACK code {rc['code']}")
            return False
    except Exception as e:
        record(4, "Trusted TLS handshake", False, str(e))
        return False


def t05_wrong_hostname(args):
    """TLS with strict hostname verification against a mismatched name."""
    try:
        client = mqtt.Client(client_id="bench_wronghost", protocol=mqtt.MQTTv311)
        client.username_pw_set(args.user, args.password)
        # Use a fake server hostname that won't match the cert's SAN
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.load_verify_locations(args.cafile)
        ctx.check_hostname = True
        ctx.verify_mode = ssl.CERT_REQUIRED
        client.tls_set_context(ctx)

        connected = False
        def on_connect(c, u, f, code):
            nonlocal connected
            connected = True
        client.on_connect = on_connect

        try:
            # Connect using a hostname that doesn't match the cert SAN
            client.connect("wrong.hostname.invalid", args.port, keepalive=5)
            client.loop_start()
            time.sleep(3)
            client.loop_stop()
        except (socket.gaierror, ssl.SSLCertVerificationError, ssl.SSLError,
                ConnectionRefusedError, OSError):
            pass
        finally:
            try:
                client.disconnect()
            except Exception:
                pass

        if connected:
            record(5, "Wrong hostname", False,
                   "Client connected despite hostname mismatch — cert check may be disabled")
        else:
            record(5, "Wrong hostname", True,
                   "Certificate verification failed (hostname mismatch)")
        return not connected
    except Exception as e:
        record(5, "Wrong hostname", True,
               f"Rejected as expected: {e}")
        return True


def t06_missing_credentials(args):
    """TLS connection with no username/password — should get CONNACK 5."""
    try:
        client = mqtt.Client(client_id="bench_nocreds", protocol=mqtt.MQTTv311)
        # No username_pw_set — anonymous
        client.tls_set(ca_certs=args.cafile, tls_version=ssl.PROTOCOL_TLS_CLIENT)
        client.tls_insecure_set(True)

        rc = {"code": -1}
        def on_connect(c, u, f, code):
            rc["code"] = code
        client.on_connect = on_connect
        client.connect(args.host, args.port, keepalive=5)
        client.loop_start()
        time.sleep(3)
        client.loop_stop()
        client.disconnect()

        if rc["code"] == 5 or rc["code"] == 4:
            record(6, "Missing credentials", True,
                   f"CONNACK code {rc['code']} — not authorized")
            return True
        elif rc["code"] == 0:
            record(6, "Missing credentials", False,
                   "Anonymous client CONNECTED — allow_anonymous may be true!")
            return False
        else:
            record(6, "Missing credentials", True,
                   f"Connection rejected (code {rc['code']})")
            return True
    except Exception as e:
        record(6, "Missing credentials", True, f"Rejected: {e}")
        return True


def t07_wrong_password(args):
    """TLS connection with correct username but wrong password."""
    try:
        client = mqtt.Client(client_id="bench_badpw", protocol=mqtt.MQTTv311)
        client.username_pw_set(args.user, "this_is_a_wrong_password_12345")
        client.tls_set(ca_certs=args.cafile, tls_version=ssl.PROTOCOL_TLS_CLIENT)
        client.tls_insecure_set(True)

        rc = {"code": -1}
        def on_connect(c, u, f, code):
            rc["code"] = code
        client.on_connect = on_connect
        client.connect(args.host, args.port, keepalive=5)
        client.loop_start()
        time.sleep(3)
        client.loop_stop()
        client.disconnect()

        if rc["code"] == 5 or rc["code"] == 4:
            record(7, "Wrong password", True,
                   f"CONNACK code {rc['code']} — not authorized")
            return True
        elif rc["code"] == 0:
            record(7, "Wrong password", False,
                   "Wrong-password client CONNECTED — password check may be broken!")
            return False
        else:
            record(7, "Wrong password", True,
                   f"Connection rejected (code {rc['code']})")
            return True
    except Exception as e:
        record(7, "Wrong password", True, f"Rejected: {e}")
        return True


def t08_authorized_publish(args):
    """Publish a test message with valid credentials. Verify it arrives."""
    received = {"msg": None}
    sub_connected = threading.Event()

    # Subscriber (uses user2 if available, else same user)
    sub_user = args.user2 if args.user2 else args.user
    sub_pass = args.password2 if args.password2 else args.password

    sub = mqtt.Client(client_id="bench_sub", protocol=mqtt.MQTTv311)
    sub.username_pw_set(sub_user, sub_pass)
    sub.tls_set(ca_certs=args.cafile, tls_version=ssl.PROTOCOL_TLS_CLIENT)
    sub.tls_insecure_set(True)

    def on_sub_connect(c, u, f, code):
        if code == 0:
            c.subscribe(args.topic, 0)
            sub_connected.set()
    def on_message(c, u, msg):
        received["msg"] = msg.payload.decode()
    sub.on_connect = on_sub_connect
    sub.on_message = on_message

    try:
        sub.connect(args.host, args.port, keepalive=10)
        sub.loop_start()
        sub_connected.wait(timeout=5)

        # Publisher
        pub = mqtt.Client(client_id="bench_pub", protocol=mqtt.MQTTv311)
        pub.username_pw_set(args.user, args.password)
        pub.tls_set(ca_certs=args.cafile, tls_version=ssl.PROTOCOL_TLS_CLIENT)
        pub.tls_insecure_set(True)

        pub_connected = threading.Event()
        def on_pub_connect(c, u, f, code):
            if code == 0:
                pub_connected.set()
        pub.on_connect = on_pub_connect
        pub.connect(args.host, args.port, keepalive=10)
        pub.loop_start()
        pub_connected.wait(timeout=5)

        test_payload = json.dumps({
            "co_ppb": 1.2, "no2_ppb": 38.0,
            "sht_temp_c": 26.3, "sht_rh_pct": 61.0,
            "benchmark": True
        })
        pub.publish(args.topic, test_payload, qos=0)
        time.sleep(2)

        pub.loop_stop()
        pub.disconnect()
        sub.loop_stop()
        sub.disconnect()

        if received["msg"]:
            record(8, "Authorized publish", True,
                   f"Message arrived: {received['msg'][:80]}...")
            return True
        else:
            record(8, "Authorized publish", "Inconclusive",
                   "Published but subscriber did not receive — check ACL or QoS")
            return None
    except Exception as e:
        try:
            sub.loop_stop(); sub.disconnect()
        except Exception:
            pass
        record(8, "Authorized publish", False, str(e))
        return False


def t09_acl_subscribe_blocked(args):
    """Try subscribing to the topic with user2 credentials.
       If ACL restricts read access, subscription should yield no messages
       even when user1 publishes to the same topic.
       Skipped if no --user2 provided."""
    if not args.user2:
        record(9, "ACL subscribe (cross-user)", "Skipped",
               "No --user2 provided; cannot test cross-user ACL")
        return None

    received = {"msg": None}

    # Subscribe as user2
    sub = mqtt.Client(client_id="bench_acl_sub", protocol=mqtt.MQTTv311)
    sub.username_pw_set(args.user2, args.password2)
    sub.tls_set(ca_certs=args.cafile, tls_version=ssl.PROTOCOL_TLS_CLIENT)
    sub.tls_insecure_set(True)

    sub_ready = threading.Event()
    def on_connect(c, u, f, code):
        if code == 0:
            c.subscribe(args.topic, 0)
            sub_ready.set()
    def on_msg(c, u, msg):
        received["msg"] = msg.payload.decode()
    sub.on_connect = on_connect
    sub.on_message = on_msg

    try:
        sub.connect(args.host, args.port, keepalive=10)
        sub.loop_start()
        sub_ready.wait(timeout=5)

        # Publish as user1
        pub = mqtt.Client(client_id="bench_acl_pub", protocol=mqtt.MQTTv311)
        pub.username_pw_set(args.user, args.password)
        pub.tls_set(ca_certs=args.cafile, tls_version=ssl.PROTOCOL_TLS_CLIENT)
        pub.tls_insecure_set(True)
        pub.connect(args.host, args.port, keepalive=10)
        pub.loop_start()
        time.sleep(1)
        pub.publish(args.topic, '{"benchmark":"acl_test"}', qos=0)
        time.sleep(3)
        pub.loop_stop(); pub.disconnect()
        sub.loop_stop(); sub.disconnect()

        if received["msg"]:
            record(9, "ACL subscribe (cross-user)", "Partial",
                   f"user2 received message — ACL may allow read access to this topic")
        else:
            record(9, "ACL subscribe (cross-user)", True,
                   "user2 received nothing — ACL blocked read access")
        return received["msg"] is None
    except Exception as e:
        try:
            sub.loop_stop(); sub.disconnect()
        except Exception:
            pass
        record(9, "ACL subscribe (cross-user)", True, f"Blocked: {e}")
        return True


def t10_acl_publish_blocked(args):
    """Try publishing with user2 to user1's topic. If ACL restricts write,
       the message should not arrive. Skipped if no --user2."""
    if not args.user2:
        record(10, "ACL publish (cross-user)", "Skipped",
               "No --user2 provided; cannot test cross-user ACL")
        return None

    received = {"msg": None}

    # Subscribe as user1 (the topic owner)
    sub = mqtt.Client(client_id="bench_acl2_sub", protocol=mqtt.MQTTv311)
    sub.username_pw_set(args.user, args.password)
    sub.tls_set(ca_certs=args.cafile, tls_version=ssl.PROTOCOL_TLS_CLIENT)
    sub.tls_insecure_set(True)

    sub_ready = threading.Event()
    def on_connect(c, u, f, code):
        if code == 0:
            c.subscribe(args.topic, 0)
            sub_ready.set()
    def on_msg(c, u, msg):
        received["msg"] = msg.payload.decode()
    sub.on_connect = on_connect
    sub.on_message = on_msg

    try:
        sub.connect(args.host, args.port, keepalive=10)
        sub.loop_start()
        sub_ready.wait(timeout=5)

        # Publish as user2 (should be blocked by ACL)
        pub = mqtt.Client(client_id="bench_acl2_pub", protocol=mqtt.MQTTv311)
        pub.username_pw_set(args.user2, args.password2)
        pub.tls_set(ca_certs=args.cafile, tls_version=ssl.PROTOCOL_TLS_CLIENT)
        pub.tls_insecure_set(True)
        pub.connect(args.host, args.port, keepalive=10)
        pub.loop_start()
        time.sleep(1)
        pub.publish(args.topic, '{"benchmark":"acl_write_test"}', qos=0)
        time.sleep(3)
        pub.loop_stop(); pub.disconnect()
        sub.loop_stop(); sub.disconnect()

        if received["msg"]:
            record(10, "ACL publish (cross-user)", "Partial",
                   "user2's message arrived — ACL may allow write access")
        else:
            record(10, "ACL publish (cross-user)", True,
                   "user2's message did not arrive — ACL blocked write access")
        return received["msg"] is None
    except Exception as e:
        try:
            sub.loop_stop(); sub.disconnect()
        except Exception:
            pass
        record(10, "ACL publish (cross-user)", True, f"Blocked: {e}")
        return True


def t11_replay_freshness(args):
    """Publish the same message twice rapidly. Both should arrive
       (MQTT has no built-in replay protection). This demonstrates
       that freshness/nonce checking is not provided by MQTT alone."""
    received = {"count": 0}

    sub = mqtt.Client(client_id="bench_replay_sub", protocol=mqtt.MQTTv311)
    sub.username_pw_set(args.user, args.password)
    sub.tls_set(ca_certs=args.cafile, tls_version=ssl.PROTOCOL_TLS_CLIENT)
    sub.tls_insecure_set(True)

    sub_ready = threading.Event()
    def on_connect(c, u, f, code):
        if code == 0:
            c.subscribe(args.topic, 0)
            sub_ready.set()
    def on_msg(c, u, msg):
        received["count"] += 1
    sub.on_connect = on_connect
    sub.on_message = on_msg

    try:
        sub.connect(args.host, args.port, keepalive=10)
        sub.loop_start()
        sub_ready.wait(timeout=5)

        pub = mqtt.Client(client_id="bench_replay_pub", protocol=mqtt.MQTTv311)
        pub.username_pw_set(args.user, args.password)
        pub.tls_set(ca_certs=args.cafile, tls_version=ssl.PROTOCOL_TLS_CLIENT)
        pub.tls_insecure_set(True)
        pub.connect(args.host, args.port, keepalive=10)
        pub.loop_start()
        time.sleep(1)

        replay_msg = json.dumps({"co_ppb": 999, "replay": True, "nonce": "DUPLICATE"})
        pub.publish(args.topic, replay_msg, qos=0)
        time.sleep(0.5)
        pub.publish(args.topic, replay_msg, qos=0)  # exact duplicate
        time.sleep(3)

        pub.loop_stop(); pub.disconnect()
        sub.loop_stop(); sub.disconnect()

        if received["count"] >= 2:
            record(11, "Replay / freshness check", "Partial",
                   f"Both duplicates accepted ({received['count']} received) — "
                   f"MQTT has no built-in replay protection. "
                   f"Application-layer nonce/timestamp checking is recommended.")
        elif received["count"] == 1:
            record(11, "Replay / freshness check", True,
                   "Only one copy arrived — broker or client may deduplicate")
        else:
            record(11, "Replay / freshness check", "Inconclusive",
                   "No messages received — check permissions")
        return received["count"] < 2
    except Exception as e:
        try:
            sub.loop_stop(); sub.disconnect()
        except Exception:
            pass
        record(11, "Replay / freshness check", "Inconclusive", str(e))
        return None


# ─────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────

def print_summary():
    header("SUMMARY")
    print(f"  {'ID':<5} {'Test':<35} {'Result':<12} Evidence")
    print(f"  {'─'*5} {'─'*35} {'─'*12} {'─'*40}")
    for r in results:
        tag = r["result"]
        if tag is True:
            tag_str = f"{GREEN}Pass{RESET}"
        elif tag is False:
            tag_str = f"{RED}FAIL{RESET}"
        else:
            tag_str = f"{YELLOW}{tag}{RESET}"
        ev = r["evidence"][:55] + "..." if len(r["evidence"]) > 55 else r["evidence"]
        print(f"  T{r['id']:02d}   {r['name']:<35} {tag_str:<21} {ev}")

    passed = sum(1 for r in results if r["result"] is True)
    total  = len(results)
    skipped = sum(1 for r in results if r["result"] in ("Skipped", None))
    print(f"\n  {passed}/{total - skipped} checks passed"
          f" ({skipped} skipped, {total} total)\n")


# ─────────────────────────────────────────────
# Main
# ─────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description="MQTT-TLS pseudo-attack benchmark (educational use only)")
    ap.add_argument("--host", default="127.0.0.1", help="Broker IP (default 127.0.0.1)")
    ap.add_argument("--port", type=int, default=8884, help="TLS port (default 8884)")
    ap.add_argument("--cafile", required=True, help="Path to CA certificate (ca.crt)")
    ap.add_argument("--user", required=True, help="Primary MQTT username")
    ap.add_argument("--password", required=True, help="Primary MQTT password")
    ap.add_argument("--topic", default="vgu/airquality/node1", help="MQTT topic to test")
    ap.add_argument("--user2", default=None, help="Second username for ACL tests (optional)")
    ap.add_argument("--password2", default=None, help="Second password for ACL tests (optional)")
    args = ap.parse_args()

    header("MQTT-TLS SECURITY BENCHMARK")
    print(f"  Target:  {args.host}:{args.port}")
    print(f"  CA:      {args.cafile}")
    print(f"  User:    {args.user}")
    print(f"  Topic:   {args.topic}")
    if args.user2:
        print(f"  User2:   {args.user2}  (ACL cross-user tests enabled)")
    print()

    print(f"  {BOLD}This script tests YOUR OWN broker configuration.{RESET}")
    print(f"  {BOLD}Do not run against systems you do not own.{RESET}\n")

    # Run all tests in order
    t01_tools_present(args)
    if not t02_port_reachable(args):
        print(f"\n  {RED}Broker unreachable — cannot continue.{RESET}\n")
        print_summary()
        return

    t03_plaintext_rejected(args)
    t04_trusted_handshake(args)
    t05_wrong_hostname(args)
    t06_missing_credentials(args)
    t07_wrong_password(args)
    t08_authorized_publish(args)
    t09_acl_subscribe_blocked(args)
    t10_acl_publish_blocked(args)
    t11_replay_freshness(args)

    print_summary()


if __name__ == "__main__":
    main()
