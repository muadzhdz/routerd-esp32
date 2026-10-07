<p align="center">
  <img src="assets/logo.svg" width="96" height="96" alt="routerd logo">
</p>

<h1 align="center">routerd</h1>

<p align="center"><b>Wi-Fi uplink gateway berbasis ESP32</b> — berbagi internet,
hotspot tangguh, setup tanpa kabel.</p>

---

## Tentang

ESP32 yang mengubah jaringan Wi-Fi apa pun menjadi hotspot sendiri —
dengan NAT di level hardware, auto-failover antar beberapa jaringan tersimpan,
dan dashboard web untuk konfigurasi lewat ponsel.

- **Multi-network** — simpan hingga 5 jaringan, seleksi otomatis yang paling baru dipakai
- **Auto-failover** — jaringan mati, pindah mulus ke kandidat berikutnya
- **NAT (LwIP NAPT)** — satu uplink, banyak klien
- **Zero-config** — setup sepenuhnya lewat captive portal, tanpa kabel

## Hardening

| Parameter | Nilai |
|---|---|
| Board | ESP32 (4MB flash / 520KB SRAM) |
| AP SSID | `routerd` |
| AP Password | `routerd123` |
| AP IP | `192.168.4.1` |

## Build

```sh
make compile   # compile firmware
make upload    # flash ke /dev/ttyUSB0
make monitor   # serial log 115200
```

Setelah flash: nyalakan, hubungkan ke AP `routerd`, ikuti dashboard.

## Bahasa & Toolchain

| Lapisan | Teknologi |
|---|---|
| Firmware | C++ (Arduino core over ESP-IDF) |
| Boilerplate build | arduino-cli + Makefile |
| Dashboard | HTML/CSS/JS inline (embedded) |
| Protokol | WiFi 802.11, LwIP NAPT |

## Lisensi

MIT — silakan dipakai, dipelajari, dan dikembangkan.