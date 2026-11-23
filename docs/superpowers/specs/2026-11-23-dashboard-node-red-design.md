# Rancangan: Dashboard Node-RED untuk Deteksi Kebocoran Gas

Tanggal: 2026-11-23
Status: draft untuk direview

## 1. Maksud

Alat deteksi kebocoran gas berbasis ESP32 + MQ-5 saat ini hanya menampilkan
data lewat Serial dan membunyikan alarm lokal. Tujuannya: menambahkan
dashboard Node-RED supaya kondisi gas bisa dipantau dari browser, dan
threshold alarm bisa diatur dari dashboard — dengan perubahan threshold
yang benar-benar mengubah perilaku alat (buzzer + LED).

## 2. Tujuan

- Dashboard menampilkan status terkini: WARMUP / SAFE / ALARM, nilai ADC
  mentah, nilai rata-rata, dan grafik nilai rata-rata terhadap waktu.
- Dashboard punya form untuk mengatur dua threshold: batas nyala alarm
  (`thresholdOn`) dan batas mati alarm (`thresholdOff`).
- Perubahan threshold dari dashboard benar-benar diterapkan di ESP32 dan
  bertahan setelah board reboot.
- Dashboard mencatat riwayat kejadian alarm: waktu mulai, waktu selesai,
  durasi, dan nilai puncak selama alarm.
- Dashboard menunjukkan apakah alat sedang online atau offline.
- Alarm lokal (buzzer, LED, melodi) tetap berfungsi penuh tanpa jaringan.

## 3. Non-goal

- Tidak ada notifikasi eksternal (Telegram, email, WhatsApp) pada tahap ini.
- Tidak ada kontrol buzzer / silence dari dashboard.
- Tidak ada multi-device atau multi-lokasi; satu alat, satu prefix topik.
- Tidak ada autentikasi dashboard atau TLS MQTT pada tahap lokal ini
  (wajib ditangani saat pindah ke VPS, lihat bagian 12).
- Tidak ada penyimpanan data sensor jangka panjang (InfluxDB, dsb.).
  Riwayat alarm disimpan di Node-RED; data grafik hanya untuk tampilan.

## 4. Anti-goal

Hal-hal yang membuat project ini dianggap gagal meskipun "jalan":

- Alarm bergantung pada Node-RED atau WiFi. Kalau jaringan mati, alat harus
  tetap mendeteksi dan membunyikan alarm.
- Dashboard menampilkan threshold yang berbeda dari yang benar-benar
  dipakai alat.
- Threshold berubah di dashboard tapi hilang saat board reboot.
- Riwayat alarm hilang setiap Node-RED restart.

## 5. Kondisi saat ini (fakta)

- Firmware: PlatformIO, `env:esp32doit-devkit-v1`, framework Arduino.
  File utama `src/main.cpp`. Sensor MQ-5 di GPIO 34, LED merah GPIO 2,
  LED hijau GPIO 4, buzzer GPIO 5.
- Logika alarm memakai hysteresis: nyala jika `avg > THRESHOLD_ON` (3350),
  mati jika `avg < THRESHOLD_OFF` (3300). Ada warm-up 60 detik (komentar
  kode menyebut 2 menit; nilai aktual `WARMUP_MS = 60000UL`) di mana alarm
  ditekan dan status WARMUP.
- Pembacaan sensor tiap 1 detik dengan moving average 5 sampel.
- Belum ada WiFi, MQTT, atau penyimpanan threshold di firmware.
- Mesin pengembangan: Fedora 44, Node.js 24.16.0, Node-RED 5.0.10 sudah
  terpasang dan berjalan manual di port 1880. Node-RED masih kosong
  (belum ada flow, belum ada paket dashboard).
- Mosquitto belum terpasang. Podman dan Docker tersedia.
- Direktori project bukan git repository.

## 6. Arsitektur

```
ESP32 (MQ-5)  --MQTT-->  Mosquitto (lokal)  <-->  Node-RED + Dashboard 2.0
     ^                                                  |
     +------------- threshold/set (MQTT) ---------------+
```

- **Logika alarm tetap di ESP32.** Node-RED hanya mengirim konfigurasi dan
  menampilkan keadaan. Ini keputusan sadar demi fail-safe.
- **Mosquitto** menjadi broker tunggal, jalan di mesin yang sama dengan
  Node-RED saat ini; nanti pindah ke VPS.
- **Node-RED** menjalankan flow dan menyajikan dashboard di `/ui`.
- **Sumber kebenaran threshold adalah ESP32** (disimpan di NVS). Node-RED
  menyimpan salinan hanya untuk tampilan, dan selalu disinkronkan lewat
  topik state yang retained.

## 7. Kontrak MQTT

Prefix: `gas/rumah1/`

| Topik | Arah | Retained | Payload |
|---|---|---|---|
| `gas/rumah1/data` | ESP32 → Node-RED | tidak | `{"raw":1234,"avg":1200,"status":"SAFE"}` tiap 1 detik |
| `gas/rumah1/availability` | ESP32 → Node-RED | ya | `"online"` saat connect, `"offline"` via LWT |
| `gas/rumah1/threshold/state` | ESP32 → Node-RED | ya | `{"on":3350,"off":3300}` saat connect dan setelah perubahan valid |
| `gas/rumah1/threshold/set` | Node-RED → ESP32 | tidak | `{"on":3400,"off":3300}` |

Aturan validasi (dipakai di kedua sisi): `0 <= off < on <= 4095`.

Status yang mungkin: `WARMUP`, `SAFE`, `ALARM`.

## 8. Perubahan firmware

File yang disentuh:

- `platformio.ini`: tambah `lib_deps` untuk `knolleary/PubSubClient` dan
  `bblanchon/ArduinoJson` (v7).
- `include/secrets.h` (baru, masuk `.gitignore`): `WIFI_SSID`,
  `WIFI_PASS`, `MQTT_HOST`, `MQTT_PORT`. Sertakan `include/secrets.example.h`.
- `src/main.cpp`:
  - WiFi STA + auto reconnect, tanpa memblokir loop alarm.
  - MQTT client dengan LWT `availability = "offline"` (retained), publish
    `"online"` saat connect, subscribe `threshold/set`.
  - Threshold dibaca dari NVS (namespace `gas`, key `th_on`/`th_off`)
    saat boot; fallback ke 3350/3300 jika tidak ada atau tidak valid.
  - `THRESHOLD_ON`/`THRESHOLD_OFF` berubah dari konstanta menjadi variabel.
  - Publish `data` tiap 1 detik hanya saat MQTT terhubung.
  - Publish `threshold/state` (retained) setiap kali connect dan setiap
    perubahan valid.
  - Terima `threshold/set`: parse JSON, validasi, simpan ke NVS, perbarui
    variabel, publish ulang state. Jika tidak valid: abaikan, catat di
    Serial, publish state lama.
  - Logika alarm, hysteresis, warm-up, moving average, dan melodi tidak
    berubah perilakunya.
  - Reconnect WiFi/MQTT dicoba berkala (mis. 5–10 detik) dari `loop()`,
    bukan blocking wait.

Catatan: `WARMUP_MS` saat ini 60000 ms walau komentar menyebut 2 menit.
Rancangan ini mempertahankan nilai aktual 60000 ms; builder tidak mengubah
perilaku tersebut tanpa persetujuan.

## 9. Setup broker (Mosquitto)

- Install native via `dnf` (bukan container) agar sederhana dan mudah
  direplikasi di VPS.
- Konfigurasi lokal: listener `1883` di `0.0.0.0`, `allow_anonymous true`.
  Ini hanya untuk jaringan lokal tepercaya.
- Buka port 1883 di firewalld.
- Aktifkan service saat boot.

## 10. Node-RED: flow dan dashboard

Paket: `@flowfuse/node-red-dashboard` (Dashboard 2.0, versi 1.37.0,
kompatibel dengan Node-RED ≥ 3.0).

Pengaturan `settings.js`: aktifkan context store berbasis file
(`contextStorage.default = localfilesystem`) supaya riwayat alarm dan
status terakhir bertahan saat Node-RED restart.

Halaman dashboard (satu halaman, bahasa Indonesia):

1. Indikator online/offline alat (dari topik availability).
2. Kartu status besar: WARMUP (kuning), SAFE (hijau), ALARM (merah).
3. Gauge nilai `avg` (rentang 0–4095).
4. Grafik `avg` terhadap waktu (jendela tampilan terbatas, mis. 15 menit).
5. Panel threshold: input `thresholdOn`, input `thresholdOff`, tombol
   "Simpan", dan teks "threshold aktif" yang berasal dari topik state.
6. Tabel riwayat alarm: mulai, selesai, durasi, nilai puncak. Urut terbaru
   di atas, maksimum 200 kejadian.

Alur data:

- `data` → parse JSON → gauge, grafik, kartu status, dan pencatat kejadian.
- Pencatat kejadian menyimpan `lastStatus` dan kejadian alarm yang sedang
  berjalan di flow context. Transisi SAFE→ALARM membuka kejadian;
  ALARM→SAFE menutupnya dan menambah baris ke riwayat. Nilai puncak
  diperbarui selama alarm berjalan. Karena context berbasis file, kejadian
  yang masih terbuka tetap konsisten setelah Node-RED restart.
- Kejadian yang masih terbuka juga ditutup saat:
  - status berubah ALARM→WARMUP (alat reboot di tengah alarm), atau
  - topik availability menjadi `"offline"` (alat putus).
  Pada kedua kasus, waktu selesai memakai timestamp data terakhir yang
  diterima dan baris riwayat diberi keterangan "koneksi terputus" /
  "alat restart", supaya tidak ada baris yang menggantung selamanya.
- Jika data ALARM diterima tanpa kejadian terbuka (mis. context hilang),
  kejadian baru dibuka dengan waktu mulai = waktu data tersebut diterima,
  dan ditandai "mulai tidak diketahui".
- `threshold/state` → memperbarui teks threshold aktif; nilai ini juga
  dipakai sebagai nilai awal form saat halaman dibuka. Jika state retained
  belum pernah diterima (alat belum pernah connect), form memakai nilai
  default 3350/3300 dan diberi tanda "belum tersinkron dengan alat".
- Tombol Simpan → validasi `0 <= off < on <= 4095` dan `on > off` → publish
  ke `threshold/set`. Dashboard menandai "menunggu konfirmasi alat" sampai
  `threshold/state` baru diterima, sehingga tampilan tidak pernah
  mendahului kenyataan.

## 11. Verifikasi

1. Broker: `mosquitto_sub -t 'gas/#' -v` sambil `mosquitto_pub` manual.
2. Firmware: `pio run` sukses; setelah flash, Serial menunjukkan koneksi
   WiFi dan MQTT.
3. End-to-end: dashboard menampilkan data; ubah threshold; Serial alat
   mencatat nilai baru; reboot alat dan nilai threshold tetap.
4. Uji alarm: turunkan `thresholdOn` sementara atau beri paparan gas;
   buzzer menyala, kartu berubah ALARM, satu baris muncul di riwayat.
5. Restart Node-RED: riwayat alarm tetap ada; setelah alat reconnect,
   threshold aktif tersinkron lagi.
6. Putuskan WiFi: alarm lokal tetap bekerja; dashboard menunjukkan offline.

## 12. Migrasi ke VPS (tahap berikutnya, bukan sekarang)

- Mosquitto dan Node-RED pindah ke VPS.
- Wajib ditambahkan sebelum akses publik: autentikasi broker + TLS,
  proteksi akses dashboard, dan firewall yang ketat.
- Firmware tidak perlu diubah selain `MQTT_HOST`/`MQTT_PORT` di
  `secrets.h`, karena alamat broker sudah dipisah sebagai konfigurasi.

## 13. Artefak yang dihasilkan

- `src/main.cpp` dan `platformio.ini` yang sudah diperbarui.
- `include/secrets.example.h` (contoh) dan `include/secrets.h` (dibuat
  builder/user, masuk `.gitignore`).
- `node-red/flows.json` — salinan flow yang bisa diimpor, disimpan di repo
  supaya bisa direkonstruksi.
- `node-red/README.md` — langkah setup broker, instalasi Dashboard 2.0,
  perubahan `settings.js`, dan cara import flow.
- `docs/superpowers/specs/2026-11-23-dashboard-node-red-design.md` (dokumen
  ini).

## 14. Yang diserahkan ke builder

- Pilihan widget untuk kartu status berwarna dinamis (ui_text dengan class
  dinamis atau ui_template) — perlu dicek kemampuan Dashboard 2.0.
- Jendela dan jumlah titik grafik.
- Apakah perlu tombol "hapus riwayat".
- Penataan layout dan tema visual.
- Apakah Node-RED dijalankan sebagai systemd user service (opsional,
  dianjurkan).
- Detail kecil kontrak JSON yang tidak mengubah perilaku.

## 15. Keputusan yang sudah diambil pemilik

- Dua threshold terpisah (on/off), bukan satu angka.
- Threshold dari dashboard harus mengubah perilaku alat.
- Logika alarm tetap di alat (fail-safe).
- Riwayat kejadian alarm diperlukan.
- Lokal dulu, VPS menyusul.
- Stack: Node-RED + Dashboard 2.0 + Mosquitto + MQTT.
