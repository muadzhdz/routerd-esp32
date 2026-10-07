# routerd-esp32

Wi-Fi uplink gateway berbasis ESP32 (Xtensa LX6 @240MHz) dengan NAT (LwIP NAPT),
hotspot SoftAP, dan dashboard web captive-portal untuk konfigurasi tanpa kabel.

## Fitur

- Uplink station (WAN) lewat jaringan Wi-Fi tersimpan, pemilihan **LRU**
  (*least-recently-used* → yang paling baru dipakai menang) saat ada beberapa
  jaringan dalam jangkauan.
- Hingga **5 jaringan** disimpan di NVS flash (`net0_*` … `net4_*`) beserta
  kartu waktu pemakaian terakhir (`seq`).
- SoftAP + captive portal untuk setup: scan jaringan, simpan, lupakan, hapus.
- NAPT: klien SoftAP memakai satu IP uplink (masquerade) untuk akses internet.
- Favicon native: aset SVG `assets/logo.svg` diembed ke firmware dan disajikan
  dari `/favicon.svg`.

## Flow boot

1. `setup()` membaca NVS → jika ada jaringan tersimpan: scan udara.
2. Cocokkan SSID tersimpan dengan hasil scan → urutkan menurun oleh `seq` (LRU).
3. Mulai dari yang paling baru dipakai; kegagalan auth menghitung mundur ke
   kandidat berikutnya.
4. Tidak ada kandidat: SoftAP fallback + captive portal, rescan dengan backoff.

## Build & flash

```sh
make compile      # arduino-cli compile, FQBN esp32:esp32:esp32
make upload       # arduino-cli upload ke /dev/ttyUSB0
make monitor      # serial 115200
```

Requirement: `arduino-cli` dengan core `esp32` (dipasang lewat `arduino-cli core install esp32:esp32`).

## Konfigurasi SoftAP default

| Parameter | Nilai |
|---|---|
| SSID | `routerd` |
| Password | `routerd123` |
| IP AP | `192.168.4.1` |

## Routes utama

| Method | Route | Fungsi |
|---|---|---|
| GET | `/` | Dashboard |
| GET | `/api/status` | Telemetri JSON (uplink, klien, heap) |
| GET | `/scan` | Hasil scan + flag `saved` |
| POST | `/save` | Simpan / perbarui kredensial & sambungkan |
| POST | `/forget` | Lupakan satu SSID |
| POST | `/clear` | Hapus semua jaringan |
| POST | `/reboot` | Restart firmware |
| GET | `/favicon.svg` | Logo |

## Batas yang diketahui (known limitations)

- **Kredensial Wi-Fi tersimpan sebagai plaintext di NVS.** Enkripsi NVS membutuhkan
  partisi `nvs_keys` + rebuild core ESP-IDF dengan `CONFIG_NVS_ENCRYPTION`; core
  `esp32:esp32` bawaan Arduino tidak menyediakannya. Jangan simpan jaringan
  kredensial-sensitif pada perangkat yang bisa dibongkar secara fisik.
- Slot NVS terbatas 5 (konstanta `MAX_NETWORKS`); lebih dari itu menimpa slot
  paling lama tidak dipakai.
- SoftAP & STA berbagi satu radio: selama scan uplink, layanan AP dapat
  terinterupsi sejenak.