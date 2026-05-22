# Claude Meter

A physical usage monitor for Claude Code, built on M5Stack Core2.

Shows your 5h and 7d Claude usage limits on a small screen sitting on your desk. No need to check the terminal or app — just glance at the device.

![Claude Meter](./preview.jpg)

---

## What it does

- Shows current (5h window) and weekly (7d window) Claude usage as a percentage
- Progress bars for both
- Bottom status line that changes based on how hard you're hitting Claude (`Idle`, `Light`, `Baking...`, `Cooking...`, `Heavy`, `Maxed!`)
- **Spike detection** — if you suddenly start burning tokens fast, it shows a red warning with the jump amount
- Battery level shown in the top right corner
- Auto-refreshes every 30 seconds
- Touch anywhere on screen to manually refresh

---

## Hardware needed

- [M5Stack Core2](https://shop.m5stack.com/products/m5stack-core2-esp32-iot-development-kit)
- USB-C cable (for flashing)
- Same WiFi network as your PC

---

## How it works

The M5Stack connects to your WiFi and fetches Claude usage data from a small Python proxy (`claude_proxy.py`) running on your PC. The proxy reads your Claude Code credentials and calls the Anthropic API to get rate limit headers.

```
M5Stack  <--WiFi-->  claude_proxy.py  <--HTTPS-->  Anthropic API
```

If you have a personal Anthropic API key, you can skip the proxy entirely and have the M5Stack call Anthropic directly (see `secrets.h.example`).

---

## Setup

### 1. Python proxy (on your PC)

```bash
python3 claude_proxy.py
```

Needs Python 3. No extra packages — uses only stdlib. Reads credentials from `~/.claude/.credentials.json` automatically (Claude Code puts them there).

**To run it as a background service (auto-starts with your PC):**

```bash
cp claude_proxy.service.example ~/.config/systemd/user/claude-proxy.service
# edit the ExecStart path in that file to match your actual path
systemctl --user daemon-reload
systemctl --user enable --now claude-proxy
```

### 2. Arduino sketch (on M5Stack)

1. Install [Arduino IDE](https://www.arduino.cc/en/software)
2. Add M5Stack board support and install `M5Unified` library
3. Copy credentials file:
   ```bash
   cp secrets.h.example secrets.h
   ```
4. Fill in `secrets.h` with your WiFi credentials and proxy URL (your PC's local IP)
5. Flash `claude_usage_monitor.ino` to the device

---

## secrets.h

This file holds your WiFi password and proxy URL. It is gitignored and never committed.

Copy `secrets.h.example` to `secrets.h` and fill in your values. Two modes available:

- **Proxy mode** (default) — fetches 5h/7d Claude Code usage via the proxy
- **Direct API mode** — if you have a personal Anthropic API key, define `ANTHROPIC_API_KEY` and skip the proxy entirely

---

## Project structure

```
claude_usage_monitor.ino   — Arduino sketch for M5Stack
claude_proxy.py            — Python proxy, runs on your PC
secrets.h.example          — Template for credentials (copy to secrets.h)
claude_proxy.service.example — Systemd service template
```

---

## Built with

- [M5Unified](https://github.com/m5stack/M5Unified) — M5Stack Arduino library
- Python 3 stdlib only (no pip install needed)
- Anthropic API rate limit headers for usage data

---

Made for personal use. Works for me, should work for you too.
