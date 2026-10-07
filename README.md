<p align="center">
  <img src="assets/logo.svg" width="96" height="96" alt="routerd logo">
</p>

<h1 align="center">routerd</h1>

<p align="center"><b>ESP32-based Wi-Fi uplink gateway</b> — share the internet,
hardened hotspot, wire-free setup.</p>

---

## About

An ESP32 that turns any Wi-Fi network into its own hotspot — with NAT at the
hardware level, auto-failover across multiple saved networks, and a web
dashboard for configuration right from your phone.

- **Multi-network** — store up to 5 networks, auto-pick the most recently used
- **Auto-failover** — network dies, switches seamlessly to the next candidate
- **NAT (LwIP NAPT)** — one uplink, many clients
- **Zero-config** — fully configured through a captive portal, no wires needed

## Quickstart

| Parameter | Value |
|---|---|
| Board | ESP32 (4MB flash / 520KB SRAM) |
| AP SSID | `routerd` |
| AP Password | `routerd123` |
| AP IP | `192.168.4.1` |

```sh
make compile   # build firmware
make upload    # flash to /dev/ttyUSB0
make monitor   # serial log @115200
```

Power it on, connect to the `routerd` AP, and follow the dashboard.

## Language & Toolchain

| Layer | Technology |
|---|---|
| Firmware | C++ (Arduino core over ESP-IDF) |
| Build | arduino-cli + Makefile |
| Dashboard | Inline HTML/CSS/JS (embedded) |
| Networking | Wi-Fi 802.11, LwIP NAPT |

## License

MIT — use it, study it, build on it.