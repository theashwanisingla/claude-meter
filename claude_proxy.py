#!/usr/bin/env python3
"""
Claude Usage Proxy
Reads Claude Code OAuth credentials, polls the Anthropic API for unified
5h/7d utilization headers, and serves the result as JSON over HTTP so the
M5Stack Core2 can fetch it over local WiFi.

Usage:
    python3 claude_proxy.py

Then set SERVER_URL in the Arduino sketch to:
    http://<YOUR_PC_IP>:8765/usage
"""

import json
import ssl
import time
import threading
import urllib.request
import urllib.error
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
import socket

CREDENTIALS_PATH = Path.home() / ".claude" / ".credentials.json"
API_URL = "https://api.anthropic.com/v1/messages"
PORT = 8765
POLL_INTERVAL = 30  # seconds

_data = {"ok": False, "s": 0, "sr": 0, "w": 0, "wr": 0, "st": "unknown", "delta": 0, "spike": False}
_lock = threading.Lock()
_prev_s = None  # previous 5h utilization %, for spike detection

SPIKE_THRESHOLD = 5  # % jump in one poll that counts as a spike


def read_token():
    try:
        blob = CREDENTIALS_PATH.read_text().strip()
        d = json.loads(blob)
        if isinstance(d.get("accessToken"), str):
            return d["accessToken"]
        for v in d.values():
            if isinstance(v, dict) and isinstance(v.get("accessToken"), str):
                return v["accessToken"]
    except Exception as e:
        print(f"[token] {e}")
    return None


def _parse_headers(headers):
    global _prev_s
    now = time.time()

    def mins(key):
        try:
            r = float(headers.get(key) or "0")
            return max(0, int(round((r - now) / 60.0)))
        except Exception:
            return 0

    def pct(key):
        try:
            return int(round(float(headers.get(key) or "0") * 100))
        except Exception:
            return 0

    current_s = pct("anthropic-ratelimit-unified-5h-utilization")
    delta = max(0, current_s - _prev_s) if _prev_s is not None else 0
    _prev_s = current_s

    return {
        "s":  current_s,
        "sr": mins("anthropic-ratelimit-unified-5h-reset"),
        "w":  pct("anthropic-ratelimit-unified-7d-utilization"),
        "wr": mins("anthropic-ratelimit-unified-7d-reset"),
        "st": headers.get("anthropic-ratelimit-unified-5h-status") or "unknown",
        "delta": delta,
        "spike": delta >= SPIKE_THRESHOLD,
        "ok": True,
    }


def poll():
    token = read_token()
    if not token:
        print("[poll] no token — check ~/.claude/.credentials.json")
        return

    body = json.dumps({
        "model": "claude-haiku-4-5-20251001",
        "max_tokens": 1,
        "messages": [{"role": "user", "content": "hi"}],
    }).encode()

    req = urllib.request.Request(API_URL, data=body, method="POST")
    req.add_header("Authorization", f"Bearer {token}")
    req.add_header("anthropic-version", "2023-06-01")
    req.add_header("anthropic-beta", "oauth-2025-04-20")
    req.add_header("Content-Type", "application/json")
    req.add_header("User-Agent", "claude-code/2.1.5")

    ctx = ssl.create_default_context()
    try:
        with urllib.request.urlopen(req, timeout=20, context=ctx) as resp:
            result = _parse_headers(resp.headers)
            with _lock:
                _data.update(result)
            print(f"[poll] {result}")
    except urllib.error.HTTPError as e:
        # Rate-limit headers are still present on 4xx responses
        result = _parse_headers(e.headers)
        result["ok"] = (e.code < 500)
        with _lock:
            _data.update(result)
        print(f"[poll] HTTP {e.code}: {result}")
        e.close()
    except Exception as e:
        print(f"[poll] error: {e}")
        with _lock:
            _data["ok"] = False


def poll_loop():
    while True:
        poll()
        time.sleep(POLL_INTERVAL)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/usage":
            with _lock:
                payload = json.dumps(_data).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *args):
        pass  # suppress per-request logs; poll loop already prints


if __name__ == "__main__":
    try:
        local_ip = socket.gethostbyname(socket.gethostname())
    except Exception:
        local_ip = "127.0.0.1"

    t = threading.Thread(target=poll_loop, daemon=True)
    t.start()

    print(f"Claude Usage Proxy  —  port {PORT}")
    print(f"Set SERVER_URL in the sketch to:  http://{local_ip}:{PORT}/usage")
    print("Ctrl+C to stop\n")

    try:
        HTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
