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

## 3. Autentikasi editor dan dashboard

Ada dua lapis perlindungan, keduanya dikonfigurasi di `~/.node-red/settings.js`:

| Lapisan | Melindungi | Mekanisme |
|---|---|---|
| `adminAuth` | editor, Admin API (`/flows`, `/settings`, `/auth/token`) | sesi login + token |
| `dashboard.middleware` | `/ui`, aset statis, `/ui/_setup`, `/ui/_debug/*` | HTTP Basic + cookie |
| `dashboard.ioMiddleware` | `/ui/socket.io` (kanal realtime) | Basic/cookie yang sama |

Tanpa `dashboard.middleware`, `adminAuth` **tidak** melindungi `/ui` maupun
socket.io-nya, sehingga siapa pun di jaringan yang sama bisa membuka dashboard
dan menekan **Simpan Threshold**.

### 3.1 Editor dan Admin API (`adminAuth`)

Aktifkan di `~/.node-red/settings.js`:

```js
adminAuth: {
    type: "credentials",
    users: [{
        username: "admin",
        password: "<hash bcrypt>",
        permissions: "*"
    }]
},
```

Buat hash (password polos tidak pernah ditulis ke file):

```bash
node-red admin hash-pw
```

Ganti password kapan saja dengan mengulang perintah di atas, menempel hash
baru, lalu me-restart Node-RED. Di Node-RED 5.0.10 Admin API **tidak**
menerima HTTP Basic (`curl -u admin:pass` → 401); ambil token lewat
`POST /auth/token` seperti pada bagian 4.

### 3.2 Dashboard `/ui` (`middleware` + `ioMiddleware`)

Tambahkan helper berikut di bagian atas `settings.js`, sebelum
`module.exports`:

```js
const crypto = require("crypto")
const DASH_USER = "admin"
const DASH_SHA256 = "<sha256 dari user:password>"
const DASH_COOKIE = "gas_dash_auth"
const DASH_COOKIE_TOKEN = crypto.createHash("sha256")
    .update("gas-dash-cookie:" + DASH_SHA256).digest("hex")

function dashEquals(a, b) {
    const ba = Buffer.from(String(a))
    const bb = Buffer.from(String(b))
    return ba.length === bb.length && crypto.timingSafeEqual(ba, bb)
}

function dashPasswordOk(user, pass) {
    if (!DASH_SHA256) return false
    const hash = crypto.createHash("sha256").update(user + ":" + pass).digest("hex")
    return dashEquals(user, DASH_USER) && dashEquals(hash, DASH_SHA256)
}

function dashAuthorized(headers) {
    const auth = headers.authorization || ""
    if (auth.indexOf("Basic ") === 0) {
        const raw = Buffer.from(auth.slice(6), "base64").toString()
        const sep = raw.indexOf(":")
        if (sep > 0 && dashPasswordOk(raw.slice(0, sep), raw.slice(sep + 1))) {
            return true
        }
    }
    const cookie = String(headers.cookie || "")
    return cookie.split(";").map(part => part.trim())
        .indexOf(DASH_COOKIE + "=" + DASH_COOKIE_TOKEN) !== -1
}

function dashMiddleware(req, res, next) {
    if (dashAuthorized(req.headers)) {
        res.cookie(DASH_COOKIE, DASH_COOKIE_TOKEN, {
            httpOnly: true,
            sameSite: "lax",
            maxAge: 7 * 24 * 60 * 60 * 1000
        })
        return next()
    }
    res.set("WWW-Authenticate", 'Basic realm="Gas Dashboard", charset="UTF-8"')
    return res.status(401).send("Unauthorized")
}

function dashIoMiddleware(socket, next) {
    if (dashAuthorized(socket.request.headers)) return next()
    return next(new Error("unauthorized"))
}
```

lalu daftarkan di objek `module.exports`:

```js
dashboard: {
    middleware: dashMiddleware,
    ioMiddleware: dashIoMiddleware
},
```

Hitung hash dashboard (berbeda dari hash bcrypt `adminAuth`):

```bash
printf '%s' 'admin:password-dashboard' | sha256sum
```

Tempel 64 karakter hex hasilnya ke `DASH_SHA256`, lalu restart Node-RED.
Saat mengganti password, **dua** nilai harus diperbarui: hash bcrypt
(`adminAuth`) dan `DASH_SHA256`.

Perilaku setelah aktif:

- Request tanpa kredensial ke `/ui`, `/ui/_setup`, dan `/ui/_debug/*` dibalas
  `401` + `WWW-Authenticate`, sehingga browser menampilkan dialog login.
- Login sukses memasang cookie `gas_dash_auth` (httpOnly, SameSite=Lax,
  berlaku 7 hari), jadi reload tidak meminta password lagi.
- Handshake socket.io tetap membalas `200`, tetapi frame `CONNECT` ditolak
  dengan `44{"message":"unauthorized"}` bila tidak ada kredensial. Dengan
  kredensial benar server mengirim `40{"sid":...}` diikuti `ui-config`.

Verifikasi:

```bash
curl -s -o /dev/null -w 'tanpa kredensial: %{http_code}\n' \
  http://127.0.0.1:1880/ui/monitor                 # 401
curl -s -o /dev/null -w 'dengan kredensial: %{http_code}\n' \
  -u 'admin:password-dashboard' \
  http://127.0.0.1:1880/ui/monitor                 # 200
```

Catatan keamanan: HTTP Basic mengirim `user:password` sebagai base64, bukan
enkripsi. Di LAN ini masih dapat diterima; untuk akses dari internet wajib
HTTPS (bagian 9).

### 3.3 Akses dari perangkat lain

Gunakan IP LAN mesin Node-RED, bukan `localhost`:

```text
http://<IP-LAN-host>:1880/ui/monitor
```

`localhost` di laptop lain menunjuk ke laptop itu sendiri. Pastikan port 1880
dibuka di firewall host:

```bash
sudo firewall-cmd --permanent --add-port=1880/tcp
sudo firewall-cmd --reload
```

## 4. Import flow

Flow ada di `node-red/flows.json`.

- Lewat editor: buka `http://localhost:1880`, login sebagai `admin`, menu
  **Import**, pilih `node-red/flows.json`, lalu **Deploy**.
- Atau lewat Admin API (butuh token bila `adminAuth` aktif):

  ```bash
  TOKEN=$(curl -s -X POST http://localhost:1880/auth/token \
    -H 'Content-Type: application/x-www-form-urlencoded' \
    -d 'client_id=node-red-admin' -d 'grant_type=password' -d 'scope=*' \
    -d 'username=admin' --data-urlencode "password=$DASHBOARD_PASS" \
    | python3 -c 'import json,sys;print(json.load(sys.stdin)["access_token"])')

  python3 -c 'import json;json.dump({"flows":json.load(open("node-red/flows.json"))},open("/tmp/flows-body.json","w"))'

  curl -X POST http://localhost:1880/flows \
    -H "Authorization: Bearer $TOKEN" \
    -H 'Content-Type: application/json' \
    -H 'Node-RED-API-Version: v2' \
    --data-binary @/tmp/flows-body.json
  ```

  Body harus berupa objek `{"flows":[...]}`, bukan array polos; array polos
  ditolak dengan error `Cannot read properties of undefined (reading
  'forEach')`.

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

## 5. Topik MQTT

| Topik | Arah | Retained | Isi |
|---|---|---|---|
| `gas/rumah1/data` | alat → dashboard | tidak | `{"raw":3500,"avg":3480,"status":"ALARM"}`, dikirim tiap detik |
| `gas/rumah1/availability` | alat → dashboard | ya | `online` / `offline` (LWT mengirim `offline`) |
| `gas/rumah1/threshold/state` | alat → dashboard | ya | `{"on":3350,"off":3300}` |
| `gas/rumah1/threshold/set` | dashboard → alat | tidak | `{"on":3400,"off":3300}` |

`status` hanya `WARMUP`, `SAFE`, atau `ALARM`. Firmware mengabaikan threshold
yang tidak memenuhi `0 <= off < on <= 4095` dan mengirim ulang state terakhir.

## 6. Perilaku dashboard

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
  tersimpan ke tabel. Bila alarm berlanjut setelah koneksi terputus, baris
  lanjutannya menampilkan `tidak diketahui` di kolom mulai karena waktu mulai
  sebenarnya tidak pernah teramati.

## 7. Firmware

```bash
cp include/secrets.example.h include/secrets.h
# isi WIFI_SSID, WIFI_PASS, MQTT_HOST (IP LAN broker), MQTT_PORT
~/.platformio/penv/bin/pio run -e esp32doit-devkit-v1 -t upload
~/.platformio/penv/bin/pio test -e native
```

`MQTT_HOST` harus IP broker yang bisa dijangkau ESP32, misalnya
`192.168.88.248`; jangan pakai `127.0.0.1` karena itu menunjuk ke alat itu
sendiri.

## 8. Checklist verifikasi perangkat nyata

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

## 9. Catatan migrasi ke VPS

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

- `adminAuth` dan middleware dashboard sudah aktif (bagian 3), tetapi masih
  memakai HTTP polos sehingga kredensial Basic bisa disadap di jaringan yang
  sama. Sebelum dibuka publik, pasang HTTPS (reverse proxy atau opsi `https`
  di `settings.js`) dan jangan ekspos port 1880 langsung.
- Sesuaikan node broker di flow ke alamat broker baru, atau pertahankan broker
  dan Node-RED di mesin yang sama.
- Pindahkan `flows.json` beserta `~/.node-red/context/` bila riwayat alarm
  ingin dibawa. Tanpa context, riwayat mulai kosong.
- Ganti `allow_anonymous true` hanya untuk pengujian lokal.

## 10. Troubleshooting

- `/ui` 404: belum ada `ui-base` yang di-deploy, atau flow belum di-import.
- MQTT tidak pernah terhubung: pastikan broker memakai `127.0.0.1` di node,
  cek `ss -tln | grep 1883`, dan pastikan listener IPv4 aktif.
- Dashboard kosong setelah restart: data non-retained (`data`) memang hilang;
  gauge/grafik akan terisi lagi saat alat mengirim data. Tabel riwayat dan
  threshold dipulihkan dari context/retained state.
- Form threshold kosong setelah restart: terjadi hanya jika tidak ada retained
  state dan context kosong; tunggu fallback 3 detik (default 3350/3300) atau
  sambungkan alat.
- `401 Unauthorized` saat `curl` ke Admin API: `adminAuth` aktif. Ambil token
  lewat `/auth/token` (bagian 4); HTTP Basic tidak didukung di Node-RED 5.0.10.
- `/ui` menampilkan dialog login atau halaman `Unauthorized`: memang auth
  dashboard aktif (bagian 3). Masuk dengan user `admin` dan password dashboard.
- Editor menerima password tetapi dashboard menolak: `DASH_SHA256` adalah
  sha256 dari `user:password`, nilai yang berbeda dari hash bcrypt
  `adminAuth`; perbarui keduanya saat mengganti password.
- Dialog login muncul terus padahal password benar: cookie diblokir (mode
  privat atau cookie dimatikan). Jangan mengandalkan `http://user:pass@host`
  karena browser modern membatasi kredensial di URL.
- Lupa password editor: buat hash baru dengan `node-red admin hash-pw`, ganti
  `password` di blok `adminAuth`, restart Node-RED. Backup konfigurasi lama ada
  di `~/.node-red/settings.js.bak`.
- Node-RED dijalankan manual dengan `nohup` hanya untuk pengembangan. Untuk
  operasional, jalankan sebagai service (systemd) agar otomatis restart.
