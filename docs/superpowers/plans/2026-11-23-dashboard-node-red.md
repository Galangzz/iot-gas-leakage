# Dashboard Node-RED untuk Deteksi Kebocoran Gas — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Menambahkan dashboard Node-RED yang menampilkan kondisi gas, mengatur dua threshold yang benar-benar mengubah perilaku alat, dan mencatat riwayat alarm.

**Architecture:** ESP32 tetap memegang logika alarm dan menyimpan threshold di NVS. Node-RED menerima data lewat MQTT, menampilkan dashboard, mengirim threshold baru, dan menyimpan riwayat alarm di context file. Mosquitto menjadi broker lokal.

**Tech Stack:** ESP32 (Arduino/PlatformIO), PubSubClient, ArduinoJson 7, Mosquitto, Node-RED 5.0.10, Dashboard 2.0 (`@flowfuse/node-red-dashboard` 1.37.0), Unity (tes native).

**Spec:** `docs/superpowers/specs/2026-11-23-dashboard-node-red-design.md`

## Global Constraints

- Environment firmware: `esp32doit-devkit-v1`, framework Arduino.
- Prefix topik: `gas/rumah1/`. Topik: `data`, `availability`, `threshold/state`, `threshold/set`.
- Payload `data`: `{"raw":<int>,"avg":<int>,"status":"WARMUP|SAFE|ALARM"}`.
- Payload `threshold/state` dan `threshold/set`: `{"on":<int>,"off":<int>}`.
- Retained hanya `availability` dan `threshold/state`. `data` dan `threshold/set` tidak retained.
- Validasi threshold di kedua sisi: `0 <= off < on <= 4095`.
- `WARMUP_MS` tetap 60000 ms; perilaku alarm, hysteresis, moving average, dan melodi tidak berubah.
- Data dipublikasikan tiap 1000 ms.
- Riwayat alarm maksimum 200 kejadian; grafik maksimum 900 titik (15 menit pada 1 detik).
- Label UI berbahasa Indonesia.
- `include/secrets.h` tidak boleh masuk git.
- PlatformIO CLI: `~/.platformio/penv/bin/pio` (tidak ada di PATH).
- Mesin: Fedora 44, Node.js 24.16.0, Node-RED 5.0.10, firewalld aktif.
- Repo git sudah diinisialisasi dan terhubung ke `origin` (`Galangzz/iot-gas-leakage`). Jangan jalankan `git init` lagi.

## Review Focus

- Threshold tidak valid (on ≤ off, di luar 0–4095, payload bukan JSON): alat harus menolak, tetap memakai nilai lama, dan state retained tidak berubah.
- Alat reboot saat alarm menyala: kejadian alarm harus ditutup dengan keterangan "alat restart", bukan menggantung.
- Node-RED restart saat alarm menyala: riwayat tetap ada dan kejadian yang terbuka tetap konsisten.
- Alat putus WiFi saat alarm: kejadian ditutup dengan "koneksi terputus"; alarm lokal tetap berbunyi.
- Nilai tepat di batas (`avg == on`, `avg == off`): hysteresis tidak boleh berkedip.

---

### Task 1: Konfigurasi rahasia dan ignore

**Files:**
- Modify: `.gitignore`
- Create: `include/secrets.example.h`

**Interfaces:**
- Produces: makro `WIFI_SSID`, `WIFI_PASS`, `MQTT_HOST`, `MQTT_PORT` yang dipakai Task 4.

- [ ] **Step 1: Tambahkan ignore untuk secrets dan credential flow**

```bash
cd /home/galangzzz/Development/iot/gas-leakge/gas-leakage
printf '\ninclude/secrets.h\nnode-red/flows_cred.json\n' >> .gitignore
```

Expected: `git diff .gitignore` menampilkan dua baris baru; repo sudah punya `origin/main`, tidak perlu `git init`.

- [ ] **Step 2: Buat file contoh secrets**

Isi `include/secrets.example.h`:

```cpp
#pragma once

#define WIFI_SSID "isi-ssid-wifi"
#define WIFI_PASS "isi-password-wifi"
#define MQTT_HOST "192.168.88.248"
#define MQTT_PORT 1883
```

- [ ] **Step 3: Verifikasi secrets diabaikan**

```bash
cp include/secrets.example.h include/secrets.h
git status --short | grep 'include/secrets.h'
```

Expected: tidak ada output (file diabaikan). File `include/secrets.h` dibiarkan ada untuk Task 4.

- [ ] **Step 4: Commit**

```bash
git add .gitignore include/secrets.example.h
git commit -m "chore: ignore secrets dan tambah contoh konfigurasi"
```

---

### Task 2: Broker Mosquitto

**Files:** tidak ada file repo; konfigurasi sistem.

**Interfaces:**
- Produces: broker di `0.0.0.0:1883`, tanpa autentikasi, untuk dipakai Task 4–9.

- [ ] **Step 1: Install Mosquitto**

```bash
sudo dnf install -y mosquitto
```

Catatan Fedora: client (`mosquitto_pub`, `mosquitto_sub`) sudah termasuk dalam paket `mosquitto`; tidak ada paket `mosquitto-clients`.

- [ ] **Step 2: Tulis konfigurasi listener**

Paket Fedora menyediakan `/etc/mosquitto/mosquitto.conf` kosong dan tidak menyediakan `conf.d`. Tulis langsung:

```bash
sudo tee -a /etc/mosquitto/mosquitto.conf >/dev/null <<'EOF'
listener 1883 0.0.0.0
allow_anonymous true
EOF
```

- [ ] **Step 3: Aktifkan service dan buka firewall**

```bash
sudo systemctl enable --now mosquitto
sudo firewall-cmd --permanent --add-port=1883/tcp
sudo firewall-cmd --reload
```

- [ ] **Step 4: Verifikasi broker**

```bash
ss -tln | grep ':1883'
mosquitto_sub -h localhost -t 'uji/#' -v &
mosquitto_pub -h localhost -t 'uji/tes' -m 'halo 123'
sleep 1
```

Expected: port 1883 listening, dan subscriber mencetak `uji/tes halo 123`.

---

### Task 3: Logika gas murni + tes native

**Files:**
- Create: `include/gas_logic.h`
- Create: `test/test_logic/test_logic.cpp`
- Modify: `platformio.ini`

**Interfaces:**
- Produces:
  - `enum class GasStatus : uint8_t { WARMUP, SAFE, ALARM };`
  - `struct GasState { GasStatus status; bool alarmActive; };`
  - `bool thresholdsValid(int thresholdOn, int thresholdOff);`
  - `GasState nextGasState(GasState prev, int avg, bool warmingUp, int thresholdOn, int thresholdOff);`
  - `const char* statusName(GasStatus status);`
- Dikonsumsi Task 4.

- [ ] **Step 1: Tulis tes yang gagal**

Isi `test/test_logic/test_logic.cpp`:

```cpp
#include <unity.h>
#include "gas_logic.h"

void setUp() {}
void tearDown() {}

void test_thresholds_valid_accepts_default() {
    TEST_ASSERT_TRUE(thresholdsValid(3350, 3300));
}

void test_thresholds_rejects_equal() {
    TEST_ASSERT_FALSE(thresholdsValid(3300, 3300));
}

void test_thresholds_rejects_inverted() {
    TEST_ASSERT_FALSE(thresholdsValid(3300, 3350));
}

void test_thresholds_rejects_out_of_range() {
    TEST_ASSERT_FALSE(thresholdsValid(4096, 4000));
    TEST_ASSERT_FALSE(thresholdsValid(3350, -1));
}

void test_alarm_turns_on_above_threshold_on() {
    GasState s = nextGasState({GasStatus::SAFE, false}, 3360, false, 3350, 3300);
    TEST_ASSERT_TRUE(s.alarmActive);
    TEST_ASSERT_TRUE(s.status == GasStatus::ALARM);
}

void test_alarm_stays_on_between_thresholds() {
    GasState s = nextGasState({GasStatus::ALARM, true}, 3340, false, 3350, 3300);
    TEST_ASSERT_TRUE(s.alarmActive);
}

void test_alarm_turns_off_below_threshold_off() {
    GasState s = nextGasState({GasStatus::ALARM, true}, 3290, false, 3350, 3300);
    TEST_ASSERT_FALSE(s.alarmActive);
    TEST_ASSERT_TRUE(s.status == GasStatus::SAFE);
}

void test_alarm_ignores_boundary_values() {
    GasState on = nextGasState({GasStatus::SAFE, false}, 3350, false, 3350, 3300);
    TEST_ASSERT_FALSE(on.alarmActive);
    GasState off = nextGasState({GasStatus::ALARM, true}, 3300, false, 3350, 3300);
    TEST_ASSERT_TRUE(off.alarmActive);
}

void test_warmup_suppresses_alarm() {
    GasState s = nextGasState({GasStatus::ALARM, true}, 4000, true, 3350, 3300);
    TEST_ASSERT_FALSE(s.alarmActive);
    TEST_ASSERT_TRUE(s.status == GasStatus::WARMUP);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_thresholds_valid_accepts_default);
    RUN_TEST(test_thresholds_rejects_equal);
    RUN_TEST(test_thresholds_rejects_inverted);
    RUN_TEST(test_thresholds_rejects_out_of_range);
    RUN_TEST(test_alarm_turns_on_above_threshold_on);
    RUN_TEST(test_alarm_stays_on_between_thresholds);
    RUN_TEST(test_alarm_turns_off_below_threshold_off);
    RUN_TEST(test_alarm_ignores_boundary_values);
    RUN_TEST(test_warmup_suppresses_alarm);
    return UNITY_END();
}
```

- [ ] **Step 2: Tambahkan environment native**

Tambahkan ke `platformio.ini`:

```ini
[env:native]
platform = native
test_framework = unity
test_build_src = no
build_flags = -Iinclude
```

- [ ] **Step 3: Jalankan tes, pastikan gagal**

```bash
~/.platformio/penv/bin/pio test -e native
```

Expected: gagal kompilasi karena `gas_logic.h` belum ada.

- [ ] **Step 4: Implementasikan `include/gas_logic.h`**

Header-only, semua fungsi `inline`. Perilaku:

- `thresholdsValid`: `thresholdOff >= 0 && thresholdOn <= 4095 && thresholdOn > thresholdOff`.
- `nextGasState`: jika `warmingUp` → `{WARMUP, false}`. Jika tidak: alarm tetap seperti `prev.alarmActive`; nyalakan jika `!alarm && avg > thresholdOn`; matikan jika `alarm && avg < thresholdOff`. Status = `ALARM` bila alarm aktif, selain itu `SAFE`.
- `statusName`: `"WARMUP"`, `"SAFE"`, `"ALARM"`.

- [ ] **Step 5: Jalankan tes, pastikan lulus**

```bash
~/.platformio/penv/bin/pio test -e native
```

Expected: 9 tes PASS, exit code 0.

- [ ] **Step 6: Commit**

```bash
git add platformio.ini include/gas_logic.h test/test_logic/test_logic.cpp
git commit -m "feat: logika gas murni dengan tes native"
```

---

### Task 4: Firmware WiFi, MQTT, dan NVS

**Files:**
- Modify: `platformio.ini`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `thresholdsValid`, `nextGasState`, `statusName`, `GasStatus` dari Task 3; makro dari `include/secrets.h` (Task 1).
- Produces: publikasi `data`, `availability`, `threshold/state`; subscribes `threshold/set`.

- [ ] **Step 1: Tambahkan dependensi**

Tambahkan ke `platformio.ini` bagian `[env:esp32doit-devkit-v1]`:

```ini
lib_deps =
    knolleary/PubSubClient@^2.8
    bblanchon/ArduinoJson@^7
```

- [ ] **Step 2: Integrasikan jaringan dan NVS di `src/main.cpp`**

Ketentuan implementasi:

- Include `WiFi.h`, `PubSubClient.h`, `Preferences.h`, `ArduinoJson.h`, `secrets.h`, `gas_logic.h`.
- Konstanta topik: `TOPIC_DATA "gas/rumah1/data"`, `TOPIC_AVAIL "gas/rumah1/availability"`, `TOPIC_THRESHOLD_STATE "gas/rumah1/threshold/state"`, `TOPIC_THRESHOLD_SET "gas/rumah1/threshold/set"`.
- Default threshold tetap 3350/3300. Ganti konstanta `THRESHOLD_ON`/`THRESHOLD_OFF` menjadi variabel `int thresholdOn`, `int thresholdOff`.
- NVS: `Preferences prefs;` namespace `"gas"`, key `"th_on"` dan `"th_off"`. Saat boot baca; jika `!thresholdsValid` pakai default.
- WiFi mode STA, `WiFi.setAutoReconnect(true)`. Di `loop()`, jika terputus coba `WiFi.reconnect()` maksimal tiap 10000 ms.
- MQTT: `PubSubClient mqtt(wifiClient)`, server `MQTT_HOST:MQTT_PORT`. Client ID unik dari MAC, mis. `"esp32-gas-" + String((uint32_t)ESP.getEfuseMac(), HEX)`.
- Saat connect: gunakan LWT topik `TOPIC_AVAIL`, QoS 0, retained, payload `"offline"`; lalu publish `"online"` retained; publish `threshold/state` retained; subscribe `TOPIC_THRESHOLD_SET` QoS 0.
- Reconnect MQTT maksimal tiap 5000 ms, tidak blocking.
- Ganti perhitungan alarm inline dengan `GasState` + `nextGasState`, lalu pakai `statusName()` untuk Serial dan payload.
- Publish `data` tiap `SENSOR_INTERVAL` (1000 ms) hanya jika `mqtt.connected()`, payload `{"raw":%d,"avg":%d,"status":"%s"}`.
- Callback `threshold/set`: parse JSON dengan ArduinoJson; ambil `on` dan `off`. Jika `thresholdsValid` → simpan ke NVS, perbarui variabel, publish `threshold/state` retained. Jika tidak valid → tulis peringatan ke Serial dan publish state lama. Jangan pernah mengubah alarm berdasarkan payload tidak valid.
- Logika warm-up, moving average, LED, buzzer, dan melodi tetap sama; hanya sumber status yang berubah.

- [ ] **Step 3: Build firmware**

```bash
~/.platformio/penv/bin/pio run -e esp32doit-devkit-v1
```

Expected: `SUCCESS`, tidak ada error kompilasi.

- [ ] **Step 4: Verifikasi runtime (butuh board terpasang)**

Jika board tidak terpasang, tandai langkah ini blocked dan lanjut ke Task 5; verifikasi runtime diulang di Task 9.

```bash
~/.platformio/penv/bin/pio run -e esp32doit-devkit-v1 -t upload
mosquitto_sub -h localhost -t 'gas/#' -v
```

Expected: `availability online`, `threshold/state {"on":3350,"off":3300}` (retained), lalu `data` tiap detik dengan status WARMUP sampai 60 detik.

- [ ] **Step 5: Verifikasi threshold turun-naik**

```bash
mosquitto_pub -h localhost -t 'gas/rumah1/threshold/set' -m '{"on":3400,"off":3300}'
mosquitto_pub -h localhost -t 'gas/rumah1/threshold/set' -m '{"on":3200,"off":3300}'
mosquitto_pub -h localhost -t 'gas/rumah1/threshold/set' -m 'bukan-json'
```

Expected: pesan pertama mengubah state retained menjadi `{"on":3400,"off":3300}`; dua pesan berikutnya tidak mengubah state dan tercatat sebagai peringatan di Serial. Setelah reboot board, state tetap 3400/3300.

- [ ] **Step 6: Commit**

```bash
git add platformio.ini src/main.cpp
git commit -m "feat: WiFi, MQTT, dan threshold persisten di firmware"
```

---

### Task 5: Dashboard 2.0 dan context storage

**Files:**
- Modify: `~/.node-red/settings.js` (di luar repo)

**Interfaces:**
- Produces: paket `@flowfuse/node-red-dashboard` aktif dan context default berbasis file untuk Task 8.

- [ ] **Step 1: Install paket dashboard**

```bash
cd ~/.node-red
npm install @flowfuse/node-red-dashboard
```

Expected: `@flowfuse/node-red-dashboard@1.37.x` masuk `dependencies`.

- [ ] **Step 2: Aktifkan context file di `settings.js`**

Di dalam `module.exports = { ... }`, tambahkan (atau ganti bila sudah ada):

```js
contextStorage: {
    default: {
        module: "localfilesystem"
    }
},
```

- [ ] **Step 3: Restart Node-RED**

Hentikan proses Node-RED yang sedang berjalan (proses yang mendengarkan port 1880), lalu jalankan ulang `node-red` dari terminal. Jangan membunuh proses lain.

- [ ] **Step 4: Verifikasi**

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://localhost:1880/ui
```

Expected: `200`.

---

### Task 6: Flow data live (status, gauge, grafik, availability)

**Files:**
- Modify: `~/.node-red/flows.json`
- Create: `node-red/flows.json` (salinan)

**Interfaces:**
- Consumes: topik `gas/rumah1/data` dan `gas/rumah1/availability`.
- Produces: flow tab `Gas Monitor`, halaman `Monitor Gas`, grup `Status`, `Grafik`, `Threshold`, `Riwayat Alarm`; widget status, gauge, raw, grafik, indikator koneksi.

- [ ] **Step 1: Buat tab dan konfigurasi dashboard**

Di editor Node-RED (`http://localhost:1880`):

- Tab `Gas Monitor`.
- `ui-base` name `Gas Dashboard`, path `/ui`.
- `ui-page` name `Monitor Gas`, path `/monitor`.
- Grup: `Status`, `Grafik`, `Threshold`, `Riwayat Alarm` (lebar penuh).

- [ ] **Step 2: Buat jalur data**

- `mqtt in`: broker `localhost:1883`, topic `gas/rumah1/data`, QoS 0, output as parsed JSON off.
- `json` node.
- `function` bernama `validasi data`: jika `msg.payload.avg` bukan angka atau `msg.payload.status` bukan salah satu `WARMUP|SAFE|ALARM`, `return null`. Selain itu teruskan.
- Output function ke: `ui_gauge`, `ui_text` raw, `ui_chart`, `ui_text` status, dan function `pencatat alarm` (dibuat di Task 8; sementara boleh belum disambung).

- [ ] **Step 3: Konfigurasi widget**

- `ui_text` status: label `Status Gas`, value `msg.payload.status`.
- Function `warnai status` sebelum `ui_text`: set `msg.ui_update = { class: 'status-' + msg.payload.status.toLowerCase() }` dan `msg.payload = msg.payload.status`.
- `ui_template` scope `CSS (All Pages)`:

```css
.status-safe .v-card-text { color: #2e7d32 !important; font-weight: 700; }
.status-alarm .v-card-text { color: #c62828 !important; font-weight: 700; }
.status-warmup .v-card-text { color: #f9a825 !important; font-weight: 700; }
```

- `ui_gauge`: label `Nilai Gas (avg)`, range 0–4095, value `msg.payload.avg`.
- `ui_text` label `Nilai Mentah (raw)`, value `msg.payload.raw`.
- `ui_chart`: label `Riwayat Nilai Gas`, tipe line, x-axis time, max 900 points (15 menit), value `msg.payload.avg`.
- `mqtt in` topic `gas/rumah1/availability` → `ui_text` label `Koneksi Alat`, value `msg.payload` (`online`/`offline`).

- [ ] **Step 4: Deploy dan verifikasi**

```bash
mosquitto_pub -h localhost -t 'gas/rumah1/data' -m '{"raw":1200,"avg":1180,"status":"SAFE"}'
mosquitto_pub -h localhost -t 'gas/rumah1/data' -m '{"raw":3500,"avg":3480,"status":"ALARM"}'
mosquitto_pub -h localhost -t 'gas/rumah1/availability' -m 'offline' -r
```

Buka `http://localhost:1880/ui` dan konfirmasi: gauge menunjukkan 3480, kartu status berubah ALARM (merah), grafik bertambah, indikator koneksi `offline`. Deploy ulang dengan indikator `online`.

- [ ] **Step 5: Simpan salinan flow ke repo dan commit**

```bash
cp ~/.node-red/flows.json node-red/flows.json
git add node-red/flows.json
git commit -m "feat: flow dashboard data live"
```

---

### Task 7: Form threshold dan sinkronisasi state

**Files:**
- Modify: `~/.node-red/flows.json`, `node-red/flows.json`

**Interfaces:**
- Consumes: `gas/rumah1/threshold/state`, `gas/rumah1/threshold/set`.
- Produces: flow variable `thresholdSeen` (boolean), `inputOn`/`inputOff` (number).

- [ ] **Step 1: Buat input dan tombol**

- `ui_number_input` `Batas Nyala (on)`: min 0, max 4095, step 1, Passthrough off. Output → `function` `simpan input on`: `flow.set('inputOn', msg.payload); return null;`
- `ui_number_input` `Batas Mati (off)`: min 0, max 4095, step 1, Passthrough off. Output → `function` `simpan input off`: `flow.set('inputOff', msg.payload); return null;`
- `ui_button` `Simpan Threshold`, payload `"simpan"`, Passthrough off → `function` `validasi threshold`.

- [ ] **Step 2: Implementasikan `validasi threshold`**

Logika:

```js
const on = flow.get('inputOn');
const off = flow.get('inputOff');
const valid = Number.isInteger(on) && Number.isInteger(off)
    && off >= 0 && on <= 4095 && on > off;

if (!valid) {
    node.status({ fill: 'red', shape: 'ring', text: 'tidak valid' });
    msg.payload = 'Nilai tidak valid: pastikan 0 <= off < on <= 4095';
    msg.topic = 'threshold/error';
    return [null, msg];
}

node.status({ fill: 'green', shape: 'dot', text: `${on}/${off}` });
msg.payload = JSON.stringify({ on, off });
msg.topic = 'gas/rumah1/threshold/set';
return [msg, { payload: 'Menunggu konfirmasi alat...' }];
```

Output 1 → `mqtt out` topic `gas/rumah1/threshold/set` (kosongkan topic di node, pakai `msg.topic`), QoS 0, retain off. Output 2 → `ui_text` `status threshold`.

- [ ] **Step 3: Tangani `threshold/state`**

- `mqtt in` topic `gas/rumah1/threshold/state` → `json` → `function` `sinkron state`:
  - `flow.set('thresholdSeen', true)`.
  - Kirim `msg.payload = doc.on` ke `ui_number_input` on, `msg.payload = doc.off` ke input off (dua output terpisah).
  - Kirim teks `Threshold aktif: on=…, off=…` ke `ui_text` `status threshold`.
  - Simpan `flow.set('inputOn', doc.on)` dan `flow.set('inputOff', doc.off)`.
- `inject` sekali setelah 3 detik → `function` `fallback threshold`: jika `!flow.get('thresholdSeen')`, isi input dengan 3350/3300 dan teks `Belum tersinkron dengan alat`.

- [ ] **Step 4: Deploy dan verifikasi**

```bash
mosquitto_sub -h localhost -t 'gas/rumah1/threshold/set' -v
```

- Isi on=3400, off=3300, klik Simpan → subscriber mencetak `{"on":3400,"off":3300}`.
- Isi on=3200, off=3300, klik Simpan → tidak ada publish, teks error muncul.
- Publish state manual: `mosquitto_pub -h localhost -t 'gas/rumah1/threshold/state' -m '{"on":3500,"off":3300}' -r` → kedua input dan teks aktif berubah.

- [ ] **Step 5: Commit**

```bash
cp ~/.node-red/flows.json node-red/flows.json
git add node-red/flows.json
git commit -m "feat: form threshold dua angka dan sinkronisasi state"
```

---

### Task 8: Riwayat kejadian alarm

**Files:**
- Modify: `~/.node-red/flows.json`, `node-red/flows.json`

**Interfaces:**
- Consumes: output `validasi data`, topik `gas/rumah1/availability`.
- Produces: flow context `lastStatus`, `openEvent`, `events` (array maks 200), dan tabel riwayat.

- [ ] **Step 1: Buat function `pencatat alarm`**

Sambungkan output `validasi data` dan `mqtt in` availability ke function ini. Isi:

```js
const now = Date.now();
let lastStatus = flow.get('lastStatus') || null;
let open = flow.get('openEvent') || null;
let events = flow.get('events') || [];

function closeOpen(end, reason) {
    if (!open) return;
    open.end = end;
    open.durationSec = Math.max(0, Math.round((end - open.start) / 1000));
    open.endReason = reason;
    events.unshift(open);
    if (events.length > 200) events.length = 200;
    open = null;
}

if (msg.topic === 'gas/rumah1/availability') {
    if (msg.payload === 'offline' && open) {
        closeOpen(open.lastSeen || now, 'koneksi terputus');
    }
} else {
    const status = msg.payload.status;
    const avg = msg.payload.avg;
    if (status === 'ALARM') {
        if (!open) {
            open = {
                start: now,
                lastSeen: now,
                peak: avg,
                startKnown: lastStatus !== null
            };
        }
        open.lastSeen = now;
        open.peak = Math.max(open.peak, avg);
    } else if (lastStatus === 'ALARM' && open) {
        if (status === 'WARMUP') {
            closeOpen(open.lastSeen, 'alat restart');
        } else {
            closeOpen(now, 'normal');
        }
    }
    flow.set('lastStatus', status);
}

flow.set('openEvent', open);
flow.set('events', events);
msg.payload = events;
return msg;
```

- [ ] **Step 2: Buat function `format riwayat`**

```js
function fmt(t) {
    return new Date(t).toLocaleString('id-ID', { timeZone: 'Asia/Jakarta' });
}

function dur(sec) {
    if (sec < 60) return `${sec} dtk`;
    return `${Math.floor(sec / 60)} mnt ${sec % 60} dtk`;
}

msg.payload = (msg.payload || []).map(e => ({
    mulai: e.startKnown === false ? 'tidak diketahui' : fmt(e.start),
    selesai: fmt(e.end),
    durasi: dur(e.durationSec),
    puncak: e.peak,
    keterangan: e.endReason
}));
return msg;
```

- [ ] **Step 3: Sambungkan tabel**

`pencatat alarm` → `format riwayat` → `ui_table` label `Riwayat Alarm`, Max Rows 10, Auto Columns on.

- [ ] **Step 4: Deploy dan verifikasi kejadian normal**

```bash
mosquitto_pub -h localhost -t 'gas/rumah1/data' -m '{"raw":1200,"avg":1180,"status":"SAFE"}'
mosquitto_pub -h localhost -t 'gas/rumah1/data' -m '{"raw":3500,"avg":3480,"status":"ALARM"}'
mosquitto_pub -h localhost -t 'gas/rumah1/data' -m '{"raw":3400,"avg":3400,"status":"ALARM"}'
mosquitto_pub -h localhost -t 'gas/rumah1/data' -m '{"raw":1200,"avg":1180,"status":"SAFE"}'
```

Expected: satu baris riwayat, keterangan `normal`, puncak 3480, durasi masuk akal.

- [ ] **Step 5: Verifikasi restart alat dan offline**

```bash
mosquitto_pub -h localhost -t 'gas/rumah1/data' -m '{"raw":3500,"avg":3480,"status":"ALARM"}'
mosquitto_pub -h localhost -t 'gas/rumah1/data' -m '{"raw":500,"avg":500,"status":"WARMUP"}'
mosquitto_pub -h localhost -t 'gas/rumah1/data' -m '{"raw":3500,"avg":3480,"status":"ALARM"}'
mosquitto_pub -h localhost -t 'gas/rumah1/availability' -m 'offline' -r
```

Expected: dua baris baru, masing-masing berketerangan `alat restart` dan `koneksi terputus`.

- [ ] **Step 6: Verifikasi persistensi setelah Node-RED restart**

Restart Node-RED, buka `http://localhost:1880/ui`, konfirmasi tabel masih menampilkan baris yang sama. Periksa `ls ~/.node-red/context` menunjukkan file context terisi.

- [ ] **Step 7: Commit**

```bash
cp ~/.node-red/flows.json node-red/flows.json
git add node-red/flows.json
git commit -m "feat: riwayat kejadian alarm persisten"
```

---

### Task 9: Verifikasi end-to-end dan dokumentasi

**Files:**
- Create: `node-red/README.md`
- Modify: `node-red/flows.json` bila ada perbaikan

**Interfaces:**
- Consumes: seluruh hasil Task 1–8.

- [ ] **Step 1: Verifikasi perangkat nyata**

Dengan board terpasang dan `include/secrets.h` terisi:

```bash
~/.platformio/penv/bin/pio run -e esp32doit-devkit-v1 -t upload
mosquitto_sub -h localhost -t 'gas/#' -v
```

Expected: `availability online`, `threshold/state` retained, `data` tiap detik. Buka dashboard: gauge dan grafik bergerak.

- [ ] **Step 2: Verifikasi threshold mengubah perilaku alat**

Dari dashboard, set on=2500, off=2400, Simpan. Amati Serial alat: nilai threshold baru tercatat, dan alarm menyala saat `avg` melewati 2500. Reboot alat: nilai 2500/2400 tetap dipakai. Kembalikan ke 3350/3300 setelah tes.

- [ ] **Step 3: Verifikasi alarm dan riwayat**

Beri paparan gas (atau pakai threshold rendah sementara) sampai status ALARM. Expected: buzzer berbunyi, LED merah, kartu merah, satu baris riwayat muncul setelah kembali SAFE.

- [ ] **Step 4: Verifikasi fail-safe jaringan**

Matikan WiFi/router sebentar saat alarm menyala. Expected: buzzer tetap berbunyi, dashboard menunjukkan offline, riwayat menutup kejadian dengan `koneksi terputus`.

- [ ] **Step 5: Tulis `node-red/README.md`**

Isi minimal:

- Prasyarat: Node-RED 5.x, Node.js 24.x, Mosquitto.
- Langkah install Mosquitto dan konfigurasi listener.
- Perintah install `@flowfuse/node-red-dashboard`.
- Potongan `contextStorage` untuk `settings.js`.
- Cara import `node-red/flows.json` dan mengatur broker `localhost:1883`.
- Daftar topik MQTT dan artinya.
- Catatan migrasi VPS: ganti `MQTT_HOST`/`MQTT_PORT` di `secrets.h`, tambahkan autentikasi broker + TLS, dan lindungi akses dashboard sebelum dibuka ke publik.

- [ ] **Step 6: Commit akhir**

```bash
git add node-red/README.md node-red/flows.json
git commit -m "docs: panduan setup dashboard dan catatan migrasi VPS"
```
