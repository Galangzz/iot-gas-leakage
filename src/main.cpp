#include <Arduino.h>

// --- Pin ---
const int RED_PIN = 2;
const int GREEN_PIN = 4;
const int BUZZER_PIN = 5;
const int SENSOR_GAS = 34; // VP / ADC1_CH0

// --- Kalibrasi MQ-5 @ 3.3V ---
const int THRESHOLD_ON = 2200;
const int THRESHOLD_OFF = 2150;

// --- Waktu ---
const unsigned long WARMUP_MS = 60000UL; // 2 menit
const unsigned long SENSOR_INTERVAL = 1000UL;
const unsigned long NOTE_GAP = 20UL; // jeda antar nada

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
// 5. 1 1 1 1 7 1 2 | 5. 2 2 2 2 1 2 3 | 1 3 5 6 6 5 | 4 4 3 3 2 2 1
const Note MELODY[] = {
    {N_G5, 400}, {N_C6, 100}, {N_C6, 100}, {N_C6, 100}, {N_C6, 100}, {N_B5, 200}, {N_C6, 200}, {N_D6, 400}, {N_G5, 400}, {N_D6, 100}, {N_D6, 100}, {N_D6, 100}, {N_D6, 100}, {N_C6, 200}, {N_D6, 200}, {N_E6, 400}, {N_C6, 200}, {N_E6, 200}, {N_G6, 400}, {N_A6, 200}, {N_A6, 200}, {N_G6, 400}, {N_F6, 200}, {N_F6, 200}, {N_E6, 200}, {N_E6, 200}, {N_D6, 200}, {N_D6, 200}, {N_C6, 400}};
const int MELODY_LEN = sizeof(MELODY) / sizeof(MELODY[0]);

// --- State ---
bool alarmActive = false;
bool notePlaying = false;
int melodyIndex = 0;
unsigned long noteStart = 0;
unsigned long gapUntil = 0;
unsigned long bootTime = 0;
unsigned long lastSensorRead = 0;

void stopMelody()
{
  noTone(BUZZER_PIN);
  notePlaying = false;
  melodyIndex = 0;
  gapUntil = 0;
}

void updateMelody(unsigned long now)
{
  if (!alarmActive)
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

void setup()
{
  Serial.begin(115200);

  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(RED_PIN, LOW);
  digitalWrite(GREEN_PIN, LOW);

  sampleSum = 0;
  for (int i = 0; i < SAMPLE_COUNT; i++)
  {
    samples[i] = analogRead(SENSOR_GAS);
    sampleSum += samples[i];
  }

  bootTime = millis();
  lastSensorRead = bootTime;
  Serial.println("MQ-5 warm-up 2 menit...");
}

void loop()
{
  unsigned long now = millis();

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

    if (!warmingUp)
    {
      if (!alarmActive && gasAvg > THRESHOLD_ON)
      {
        alarmActive = true;
        stopMelody();
      }
      else if (alarmActive && gasAvg < THRESHOLD_OFF)
      {
        alarmActive = false;
        stopMelody();
      }
    }
    else
    {
      alarmActive = false;
      stopMelody();
    }

    if (alarmActive)
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
    if (warmingUp)
      Serial.println("WARMUP");
    else if (alarmActive)
      Serial.println("ALARM");
    else
      Serial.println("SAFE");
  }

  updateMelody(now);
  delay(5);
}