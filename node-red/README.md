# Dashboard Node-RED — Monitor Kebocoran Gas

Dashboard untuk alat ESP32 + sensor MQ-5. Alarm tetap diputuskan di ESP32
(fail-safe saat jaringan putus); Node-RED hanya menampilkan data, menerima
threshold baru, dan mencatat riwayat kejadian.

Versi yang dipakai saat pengujian: Node-RED 5.0.10, Node.js 24.16.0,
`@flowfuse/node-red-dashboard` 1.37.0, Mosquitto 2.x di Fedora 44.

## Prasyarat

- Node.js 24.x dan Node-RED 5.x (terpasang global, perintah `node-red` tersedia).
- Mosquitto sebagai broker MQTT lokal.
- Dashboard 2.0: `@flowfuse/node-red-dashboard`.
- ESP32 yang menjalankan firmware di repo ini, dengan `include/secrets.h` terisi.

## 1. Broker Mosquitto

Fedora menyediakan paket `mosquitto` yang sudah berisi client
`mosquitto_pub`/`mosquitto_sub` (tidak ada paket `mosquitto-clients` terpisah).
File `/etc/mosquitto/mosquitto.conf` kosong dan tidak ada direktori `conf.d`,
jadi konfigurasi listener ditulis langsung ke file tersebut.

```bash
sudo dnf install mosquitto
```

Tambahkan ke `/etc/mosquitto/mosquitto.conf`:

```conf
listener 1883 0.0.0.0
allow_anonymous true
```

`0.0.0.0` diperlukan agar ESP32 bisa terhubung lewat LAN. Untuk produksi,
jangan biarkan anonymous; lihat bagian migrasi VPS.

```bash
sudo systemctl enable --now mosquitto
sudo systemctl restart mosquitto
sudo firewall-cmd --permanent --add-port=1883/tcp
sudo firewall-cmd --reload
```

Uji cepat:

```bash
mosquitto_sub -h 127.0.0.1 -t 'uji/#' -v &
mosquitto_pub -h 127.0.0.1 -t 'uji/tes' -m 'halo'
```

## 2. Dashboard 2.0 dan context storage

```bash
cd ~/.node-red
npm install @flowfuse/node-red-dashboard
```

Riwayat alarm disimpan di flow context. Aktifkan penyimpanan lokal di
`~/.node-red/settings.js`:

```js
contextStorage: {
    default: {
        module: "localfilesystem"
    },
},
```

Restart Node-RED setelah mengubah `settings.js`.

## 3. Import flow

Flow ada di `node-red/flows.json`.

- Lewat editor: buka `http://localhost:1880`, menu **Import**, pilih
  `node-red/flows.json`, lalu **Deploy**.
- Atau lewat Admin API:

  ```bash
  python3 -c 'import json;json.dump({"flows":json.load(open("node-red/flows.json"))},open("/tmp/flows-body.json","w"))'
  curl -X POST http://localhost:1880/flows \
    -H 'Content-Type: application/json' \
    -H 'Node-RED-API-Version: v2' \
    --data-binary @/tmp/flows-body.json
  ```

Setelah deploy:

- Dashboard: `http://localhost:1880/ui` (dialihkan ke `/ui/monitor`).
- Node broker bernama **Mosquitto Lokal** memakai `127.0.0.1:1883`.
  Gunakan `127.0.0.1`, bukan `localhost`: di Fedora `localhost` hanya
  me-resolve ke `::1`, sedangkan Mosquitto mendengarkan IPv4 `0.0.0.0`,
  sehingga koneksi MQTT gagal tanpa pesan error yang jelas.
- Jika broker berada di mesin lain, ubah host/port pada node broker tersebut.

Edit flow di editor selalu menulis ke `~/.node-red/flows.json`. Untuk
menyimpan perubahan ke repo:

```bash
cp ~/.node-red/flows.json node-red/flows.json
git add node-red/flows.json
git commit -m "chore: perbarui flow dashboard"
```

## 4. Topik MQTT

| Topik | Arah | Retained | Isi |
|---|---|---|---|
| `gas/rumah1/data` | alat → dashboard | tidak | `{"raw":3500,"avg":3480,"status":"ALARM"}`, dikirim tiap detik |
| `gas/rumah1/availability` | alat → dashboard | ya | `online` / `offline` (LWT mengirim `offline`) |
| `gas/rumah1/threshold/state` | alat → dashboard | ya | `{"on":3350,"off":3300}` |
| `gas/rumah1/threshold/set` | dashboard → alat | tidak | `{"on":3400,"off":3300}` |

`status` hanya `WARMUP`, `SAFE`, atau `ALARM`. Firmware mengabaikan threshold
yang tidak memenuhi `0 <= off < on <= 4095` dan mengirim ulang state terakhir.

## 5. Perilaku dashboard

- Kartu status: `WARMUP` kuning, `SAFE` hijau, `ALARM` merah.
- Gauge: rata-rata ADC 0–4095.
- Grafik: nilai rata-rata, jendela 15 menit, maksimum 900 titik, sumbu Y tetap
  0–4095 agar noise normal tidak tampak seperti lonjakan besar.
- Threshold: dua angka (`on` dan `off`) + tombol simpan. State dari alat
  mengisi form secara otomatis; bila belum ada state, form memakai default
  3350/3300 dengan keterangan "Belum tersinkron dengan alat".
- Riwayat alarm: maksimum 200 kejadian di context, 10 baris terbaru
  ditampilkan. Satu baris berisi mulai, selesai, durasi, puncak, dan
  keterangan (`normal`, `alat restart`, `koneksi terputus`). Riwayat bertahan
  setelah Node-RED restart; saat startup flow mengirim ulang events yang
  tersimpan ke tabel.

## 6. Firmware

```bash
cp include/secrets.example.h include/secrets.h
# isi WIFI_SSID, WIFI_PASS, MQTT_HOST (IP LAN broker), MQTT_PORT
~/.platformio/penv/bin/pio run -e esp32doit-devkit-v1 -t upload
~/.platformio/penv/bin/pio test -e native
```

`MQTT_HOST` harus IP broker yang bisa dijangkau ESP32, misalnya
`192.168.88.248`; jangan pakai `127.0.0.1` karena itu menunjuk ke alat itu
sendiri.

## 7. Checklist verifikasi perangkat nyata

Bagian ini **belum dijalankan** pada saat flow selesai dibuat karena board
belum terpasang dan `include/secrets.h` masih placeholder. Jalankan setelah
alat siap:

1. Upload firmware, lalu pantau semua topik:

   ```bash
   mosquitto_sub -h <ip-broker> -t 'gas/#' -v
   ```

   Harapan: `availability online`, `threshold/state` retained, `data` tiap detik.

2. Dari dashboard set `on=2500`, `off=2400`, klik **Simpan Threshold**.
   Amati Serial alat: threshold baru tercatat dan alarm menyala saat `avg`
   melewati 2500. Reboot alat: 2500/2400 tetap dipakai. Kembalikan ke
   3350/3300 setelah tes.

3. Beri paparan gas (atau pakai threshold rendah sementara) sampai `ALARM`.
   Harapan: buzzer berbunyi, LED merah, kartu status merah, dan satu baris
   riwayat muncul setelah kembali `SAFE`.

4. Saat alarm menyala, matikan WiFi/router sebentar. Harapan: buzzer tetap
   berbunyi (logika di alat), dashboard menampilkan `offline`, dan riwayat
   menutup kejadian dengan `koneksi terputus`.

## 8. Catatan migrasi ke VPS

- Ganti `MQTT_HOST`/`MQTT_PORT` di `include/secrets.h` ke alamat VPS, lalu
  flash ulang alat.
- Jangan buka Mosquitto tanpa autentikasi ke internet. Tambahkan
  `allow_anonymous false`, `password_file`, dan TLS di port 8883:

  ```conf
  listener 8883 0.0.0.0
  allow_anonymous false
  password_file /etc/mosquitto/passwd
  cafile   /etc/mosquitto/certs/ca.crt
  certfile /etc/mosquitto/certs/server.crt
  keyfile  /etc/mosquitto/certs/server.key
  ```

- Lindungi Node-RED dengan HTTPS dan autentikasi (reverse proxy + `adminAuth`
  atau middleware) sebelum dashboard dapat diakses publik. Jangan ekspos port
  1880 langsung.
- Sesuaikan node broker di flow ke alamat broker baru, atau pertahankan broker
  dan Node-RED di mesin yang sama.
- Pindahkan `flows.json` beserta `~/.node-red/context/` bila riwayat alarm
  ingin dibawa. Tanpa context, riwayat mulai kosong.
- Ganti `allow_anonymous true` hanya untuk pengujian lokal.

## 9. Troubleshooting

- `/ui` 404: belum ada `ui-base` yang di-deploy, atau flow belum di-import.
- MQTT tidak pernah terhubung: pastikan broker memakai `127.0.0.1` di node,
  cek `ss -tln | grep 1883`, dan pastikan listener IPv4 aktif.
- Dashboard kosong setelah restart: data non-retained (`data`) memang hilang;
  gauge/grafik akan terisi lagi saat alat mengirim data. Tabel riwayat dan
  threshold dipulihkan dari context/retained state.
- Form threshold kosong setelah restart: terjadi hanya jika tidak ada retained
  state dan context kosong; tunggu fallback 3 detik (default 3350/3300) atau
  sambungkan alat.
- Node-RED dijalankan manual dengan `nohup` hanya untuk pengembangan. Untuk
  operasional, jalankan sebagai service (systemd) agar otomatis restart.
