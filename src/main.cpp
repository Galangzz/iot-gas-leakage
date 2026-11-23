#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include "gas_logic.h"
#include "secrets.h"

// --- Pin ---
const int RED_PIN = 2;
const int GREEN_PIN = 4;
const int BUZZER_PIN = 5;
const int SENSOR_GAS = 34; // VP / ADC1_CH0

// --- Threshold default ---
const int DEFAULT_THRESHOLD_ON = 3350;
const int DEFAULT_THRESHOLD_OFF = 3300;

int thresholdOn = DEFAULT_THRESHOLD_ON;
int thresholdOff = DEFAULT_THRESHOLD_OFF;

// --- Waktu ---
const unsigned long WARMUP_MS = 60000UL;
const unsigned long SENSOR_INTERVAL = 1000UL;
const unsigned long NOTE_GAP = 20UL;
const unsigned long WIFI_RETRY_MS = 10000UL;
const unsigned long MQTT_RETRY_MS = 5000UL;

// --- Moving average ---
const int SAMPLE_COUNT = 5;
int samples[SAMPLE_COUNT];
int sampleIndex = 0;
long sampleSum = 0;

// --- Frekuensi nada (1 = C6) ---
#define N_G5 784
#define N_B5 988 // 7 (leading tone di bawah do)
#define N_C6 1047
#define N_D6 1175
#define N_E6 1319
#define N_F6 1397
#define N_G6 1568
#define N_A6 1760

struct Note
{
  uint16_t freq;     // 0 = diam
  uint16_t duration; // ms
};

// Melodi "Wo De Peng You Zai Na Li"
const Note MELODY[] = {
    {N_G5, 400}, {N_C6, 100}, {N_C6, 100}, {N_C6, 100}, {N_C6, 100}, {N_B5, 200}, {N_C6, 200}, {N_D6, 400}, {N_G5, 400}, {N_D6, 100}, {N_D6, 100}, {N_D6, 100}, {N_D6, 100}, {N_C6, 200}, {N_D6, 200}, {N_E6, 400}, {N_C6, 200}, {N_E6, 200}, {N_G6, 400}, {N_A6, 200}, {N_A6, 200}, {N_G6, 400}, {N_F6, 200}, {N_F6, 200}, {N_E6, 200}, {N_E6, 200}, {N_D6, 200}, {N_D6, 200}, {N_C6, 400}};
const int MELODY_LEN = sizeof(MELODY) / sizeof(MELODY[0]);

// --- State ---
GasState gasState = {GasStatus::WARMUP, false};
bool notePlaying = false;
int melodyIndex = 0;
unsigned long noteStart = 0;
unsigned long gapUntil = 0;
unsigned long bootTime = 0;
unsigned long lastSensorRead = 0;

// --- Jaringan ---
const char *TOPIC_DATA = "gas/rumah1/data";
const char *TOPIC_AVAIL = "gas/rumah1/availability";
const char *TOPIC_THRESHOLD_STATE = "gas/rumah1/threshold/state";
const char *TOPIC_THRESHOLD_SET = "gas/rumah1/threshold/set";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;
String clientId;
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;

void stopMelody()
{
  noTone(BUZZER_PIN);
  notePlaying = false;
  melodyIndex = 0;
  gapUntil = 0;
}

void updateMelody(unsigned long now)
{
  if (!gasState.alarmActive)
  {
    if (notePlaying)
      stopMelody();
    return;
  }

  if (notePlaying)
  {
    if (now - noteStart >= MELODY[melodyIndex].duration)
    {
      noTone(BUZZER_PIN);
      notePlaying = false;
      melodyIndex++;
      gapUntil = now + NOTE_GAP;
    }
    return;
  }

  if (now < gapUntil)
    return;

  if (melodyIndex >= MELODY_LEN)
    melodyIndex = 0;

  uint16_t f = MELODY[melodyIndex].freq;
  if (f == 0)
    noTone(BUZZER_PIN);
  else
    tone(BUZZER_PIN, f);

  noteStart = now;
  notePlaying = true;
}

void publishThresholdState()
{
  char buf[64];
  snprintf(buf, sizeof(buf), "{\"on\":%d,\"off\":%d}", thresholdOn, thresholdOff);
  mqtt.publish(TOPIC_THRESHOLD_STATE, buf, true);
}

void onMqttMessage(char *topic, byte *payload, unsigned int length)
{
  if (strcmp(topic, TOPIC_THRESHOLD_SET) != 0)
    return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err)
  {
    Serial.printf("threshold/set bukan JSON valid: %s\n", err.c_str());
    publishThresholdState();
    return;
  }

  int on = doc["on"] | -1;
  int off = doc["off"] | -1;

  if (!thresholdsValid(on, off))
  {
    Serial.printf("threshold ditolak: on=%d off=%d\n", on, off);
    publishThresholdState();
    return;
  }

  thresholdOn = on;
  thresholdOff = off;
  prefs.putInt("th_on", thresholdOn);
  prefs.putInt("th_off", thresholdOff);

  Serial.printf("threshold baru: on=%d off=%d\n", thresholdOn, thresholdOff);
  publishThresholdState();
}

void loadThresholds()
{
  prefs.begin("gas", false);
  thresholdOn = prefs.getInt("th_on", DEFAULT_THRESHOLD_ON);
  thresholdOff = prefs.getInt("th_off", DEFAULT_THRESHOLD_OFF);

  if (!thresholdsValid(thresholdOn, thresholdOff))
  {
    thresholdOn = DEFAULT_THRESHOLD_ON;
    thresholdOff = DEFAULT_THRESHOLD_OFF;
    Serial.println("threshold NVS tidak valid, pakai default");
  }
}

void connectWifi(unsigned long now)
{
  if (WiFi.status() == WL_CONNECTED)
    return;
  if (now - lastWifiAttempt < WIFI_RETRY_MS)
    return;

  lastWifiAttempt = now;
  Serial.println("Menyambung ulang WiFi...");
  WiFi.reconnect();
}

void connectMqtt(unsigned long now)
{
  if (mqtt.connected())
    return;
  if (now - lastMqttAttempt < MQTT_RETRY_MS)
    return;

  lastMqttAttempt = now;
  if (WiFi.status() != WL_CONNECTED)
    return;

  if (mqtt.connect(clientId.c_str(), nullptr, nullptr, TOPIC_AVAIL, 0, true, "offline"))
  {
    Serial.println("MQTT terhubung");
    mqtt.publish(TOPIC_AVAIL, "online", true);
    publishThresholdState();
    mqtt.subscribe(TOPIC_THRESHOLD_SET, 0);
  }
  else
  {
    Serial.printf("MQTT gagal, state=%d\n", mqtt.state());
  }
}

void publishData(int raw, int avg)
{
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"raw\":%d,\"avg\":%d,\"status\":\"%s\"}",
           raw, avg, statusName(gasState.status));
  mqtt.publish(TOPIC_DATA, buf, false);
}

void setup()
{
  Serial.begin(115200);

  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(RED_PIN, LOW);
  digitalWrite(GREEN_PIN, LOW);

  loadThresholds();

  sampleSum = 0;
  for (int i = 0; i < SAMPLE_COUNT; i++)
  {
    samples[i] = analogRead(SENSOR_GAS);
    sampleSum += samples[i];
  }

  bootTime = millis();
  lastSensorRead = bootTime;

  clientId = "esp32-gas-" + String((uint32_t)ESP.getEfuseMac(), HEX);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiAttempt = millis();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setBufferSize(512);

  Serial.printf("MQ-5 warm-up 60 detik... threshold on=%d off=%d\n", thresholdOn, thresholdOff);
}

void loop()
{
  unsigned long now = millis();

  connectWifi(now);
  connectMqtt(now);
  mqtt.loop();

  if (now - lastSensorRead >= SENSOR_INTERVAL)
  {
    lastSensorRead = now;

    int gasRaw = analogRead(SENSOR_GAS);

    sampleSum -= samples[sampleIndex];
    samples[sampleIndex] = gasRaw;
    sampleSum += gasRaw;
    sampleIndex = (sampleIndex + 1) % SAMPLE_COUNT;
    int gasAvg = sampleSum / SAMPLE_COUNT;

    bool warmingUp = (now - bootTime) < WARMUP_MS;

    gasState = nextGasState(gasState, gasAvg, warmingUp, thresholdOn, thresholdOff);

    if (gasState.alarmActive)
    {
      digitalWrite(RED_PIN, HIGH);
      digitalWrite(GREEN_PIN, LOW);
    }
    else
    {
      digitalWrite(RED_PIN, LOW);
      digitalWrite(GREEN_PIN, HIGH);
    }

    Serial.print("raw=");
    Serial.print(gasRaw);
    Serial.print("  avg=");
    Serial.print(gasAvg);
    Serial.print("  status=");
    Serial.println(statusName(gasState.status));

    if (mqtt.connected())
      publishData(gasRaw, gasAvg);
  }

  updateMelody(now);
  delay(5);
}
