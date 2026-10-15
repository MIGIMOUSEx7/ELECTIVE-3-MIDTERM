#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <HTTPClient.h>

// ==========================================
// --- 1. NETWORK & EMQX CLOUD CONFIG ---
// ==========================================
const char* ssid        = "ALAWABALO";
const char* password    = "PRX21RB33";

const char* mqtt_server   = "k68f60c1.ala.asia-southeast1.emqxsl.com";
const int   mqtt_port     = 8883;
const char* mqtt_username = "Van";
const char* mqtt_password = "123qweasd";

const char* topic_telemetry = "elective3/gas_node/data";     // sensor values, every 1 s
const char* topic_alarm     = "elective3/gas_node/alarm";    // ALARM / CLEAR events
const char* topic_status    = "elective3/gas_node/status";   // NEW: warmup / calibrating / ready
const char* topic_commands  = "elective3/gas_node/cmd";      // recalibrate | mute | test

WiFiClientSecure espClient;
PubSubClient client(espClient);
unsigned long lastMqttPublish = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastStatusPub = 0;

// ==========================================
// --- 2. SENSORS & HARDWARE DEFINITIONS ---
// ==========================================
const int N = 3;
const int sensorPin[N]    = {34, 32, 39};   // MQ-2 = D34, MQ-7 = D32, MQ-135 = VN (GPIO39)
const char* sensorName[N] = {"MQ-2", "MQ-7", "MQ-135"};

// Set a sensor to false if it is not connected, so it can never false-alarm.
const bool SENSOR_ENABLED[N] = {true, true, true};

const int blueLed    = 33;
const int redLed     = 26;
const int buzzerPin  = 14;
const int bootButton = 0;

const bool USE_PASSIVE_BUZZER = true;    // true = passive buzzer, false = active buzzer
const bool BUZZER_ACTIVE_LOW  = false;   // set true if the buzzer works backwards
const int  BUZZER_FREQ        = 2700;    // passive buzzer only

// Passive buzzer only: a different pitch per sensor (ignored for active buzzers)
const int sensorTone[N] = {2700, 2000, 3500};

// --- TIMING ---
const unsigned long TICK_MS        = 100;
const unsigned long WARMUP_MS      = 90000;  // use 20000 while testing
const unsigned long CALIBRATE_MS   = 15000;
const unsigned long ALARM_HOLD_MS  = 8000;
const unsigned long CLEAR_QUIET_MS = 3000;

// --- SENSITIVITY (per sensor: MQ-2, MQ-7, MQ-135) ---
const int   ALARM_MIN[N]   = {100, 40, 100};    // lower = more sensitive (MQ-7 is lower: it reads at 11 dB)
const int   ALARM_MAX[N]   = {500, 500, 500};   // cap so a noisy calibration can't make it deaf
const float SIGMA_ALARM    = 6.0;               // threshold = clamp(6*sigma, MIN, MAX)
const int   HYSTERESIS     = 30;
const int   CONFIRM_COUNT  = 3;
const int   DRIFT_UP_DIV   = 400;
const int   DRIFT_DOWN_DIV = 40;
const int   SAMPLES        = 9;     // median of 9 samples
const int   WIN            = 15;    // smoothing window
const int   SATURATED      = 3900;
const int   DROPOUT_BELOW  = 30;

// --- ALARM PRIORITY (if several alarm at once, the first one here is sounded) ---
// MQ-7 (carbon monoxide) first, then MQ-2 (smoke/gas), then MQ-135 (air quality)
const int PRIORITY[N] = {1, 0, 2};

bool buzzerIsOn = false;
bool calibrated = false;
bool muted      = false;   // buzzer silenced from the app (LED keeps flashing)
bool testRequested = false;

bool  alarmOn[N];
int   upCount[N];
unsigned long alarmSince[N];
unsigned long lastHigh[N];

float buf[N][WIN];
int   bufCount = 0;
int   bufIdx   = 0;

float baseline[N];
float calSum[N];
float calSumSq[N];
float sigma[N];
int   effAlarm[N];
int   calN     = 0;
unsigned long calStart = 0;
unsigned long lastTick = 0;
unsigned long lastWarmPrint = 0;
unsigned long lastSatPrint  = 0;
int dropCount = 0;

// ==========================================
// --- PHONE NOTIFICATIONS (ntfy.sh) ---
// ==========================================
// 1. Install the free "ntfy" app on your phone (Android / iPhone).
// 2. In the app, subscribe to the SAME topic name as below.
// Anyone who knows the topic name can read it, so make it long and random.
const bool  NTFY_ENABLED = false;   // set true if you also want ntfy
const char* ntfy_topic   = "safehome-CHANGE-THIS-to-something-long-and-random";
const unsigned long NTFY_COOLDOWN_MS = 30000;   // at most one phone alert per sensor every 30 s
unsigned long lastNtfy[N];

void sendPhoneAlert(int i) {
  if (!NTFY_ENABLED || WiFi.status() != WL_CONNECTED) return;
  if (lastNtfy[i] != 0 && millis() - lastNtfy[i] < NTFY_COOLDOWN_MS) return;
  lastNtfy[i] = millis();

  static const char* what[N] = {"Smoke / gas detected", "Carbon monoxide detected", "Poor air quality detected"};

  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  String url = String("https://ntfy.sh/") + ntfy_topic;
  if (http.begin(tls, url)) {
    http.setTimeout(3000);                       // never freeze the alarm for more than ~3 s
    http.addHeader("Title", "SafeHome ALERT");
    http.addHeader("Priority", "urgent");
    http.addHeader("Tags", "rotating_light");
    int code = http.POST(String(what[i]) + " (" + sensorName[i] + "). Check your home now.");
    Serial.printf("(phone alert %s -> HTTP %d)\n", sensorName[i], code);
    http.end();
  } else {
    Serial.println("(phone alert: could not start request)");
  }
}

// ==========================================
// --- TELEGRAM BOT ALERTS ---
// ==========================================
// 1. Create a bot with @BotFather in Telegram and copy the token it gives you.
// 2. Send your bot a message, then get your chat ID (see the setup guide).
// KEEP THE TOKEN SECRET: anyone who has it can send messages as your bot.
const bool  TELEGRAM_ENABLED   = true;
const bool  TELEGRAM_BOOT_MSG  = true;    // sends "node is online" at startup, handy for testing
const char* telegram_token     = "8967123004:AAGzkQU2lBblV5lOFnyHAKoBXiS3j5ts3H0";
const char* telegram_chat_id   = "7399246302";           // a group's ID is a negative number
const unsigned long TELEGRAM_COOLDOWN_MS = 30000;        // at most one alert per sensor every 30 s
unsigned long lastTelegram[N];

// Returns the HTTP code (200 = sent). -1 = could not connect, -2 = token/chat ID not filled in.
int telegramSend(const String& text) {
  if (!TELEGRAM_ENABLED || WiFi.status() != WL_CONNECTED) return -1;
  if (String(telegram_token).startsWith("PASTE") || String(telegram_chat_id).startsWith("PASTE")) return -2;

  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + telegram_token + "/sendMessage";
  int code = -1;
  if (http.begin(tls, url)) {
    http.setTimeout(3000);                       // never freeze the alarm for more than ~3 s
    http.addHeader("Content-Type", "application/json");
    String body = String("{\"chat_id\":\"") + telegram_chat_id + "\",\"text\":\"" + text + "\"}";
    code = http.POST(body);
    http.end();
  }
  return code;
}

void sendTelegramAlert(int i) {
  if (!TELEGRAM_ENABLED) return;
  if (lastTelegram[i] != 0 && millis() - lastTelegram[i] < TELEGRAM_COOLDOWN_MS) return;
  lastTelegram[i] = millis();

  static const char* what[N] = {"Smoke / gas detected", "Carbon monoxide detected", "Poor air quality detected"};
  int code = telegramSend(String("🚨 SafeHome ALERT\\n") + what[i] + " (" + sensorName[i] + ").\\nCheck your home now.");
  Serial.printf("(telegram alert %s -> HTTP %d)\n", sensorName[i], code);
}

// ==========================================
// --- 3. HARDWARE CONTROL FUNCTIONS ---
// ==========================================

// Tells the app what phase the node is in (the app can show "Warming up 84s")
void publishStatus(const char* phase, long left, bool force = false) {
  if (!client.connected()) return;
  if (!force && millis() - lastStatusPub < 2000) return;
  lastStatusPub = millis();
  char msg[96];
  snprintf(msg, sizeof(msg), "{\"phase\":\"%s\",\"left\":%ld}", phase, left);
  client.publish(topic_status, msg);
}

void buzzerOn() {
  if (buzzerIsOn) return;
  buzzerIsOn = true;
  if (USE_PASSIVE_BUZZER) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(buzzerPin, BUZZER_FREQ, 8);
    ledcWriteTone(buzzerPin, BUZZER_FREQ);
#else
    ledcSetup(0, BUZZER_FREQ, 8);
    ledcAttachPin(buzzerPin, 0);
    ledcWriteTone(0, BUZZER_FREQ);
#endif
  } else {
    digitalWrite(buzzerPin, BUZZER_ACTIVE_LOW ? LOW : HIGH);
  }
}

void buzzerOff() {
  if (!buzzerIsOn) return;
  buzzerIsOn = false;
  if (USE_PASSIVE_BUZZER) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(buzzerPin, 0);
    ledcDetach(buzzerPin);
#else
    ledcWriteTone(0, 0);
    ledcDetachPin(buzzerPin);
#endif
    pinMode(buzzerPin, OUTPUT);
    digitalWrite(buzzerPin, LOW);
  } else {
    digitalWrite(buzzerPin, BUZZER_ACTIVE_LOW ? HIGH : LOW);
  }
}

void setBuzzerFreq(int f) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(buzzerPin, f);
#else
  ledcWriteTone(0, f);
#endif
}

void beep(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    buzzerOn();
    delay(onMs);
    buzzerOff();
    delay(offMs);
  }
}

void setLeds(bool blue, bool red) {
  digitalWrite(blueLed, blue);
  digitalWrite(redLed, red);
}

// --- One beep pattern per sensor ---
//  MQ-2   (smoke / gas)   : two short beeps
//  MQ-7   (carbon monoxide): one long beep
//  MQ-135 (air quality)   : three quick beeps
bool patternOn(int s) {
  unsigned long t = millis() - alarmSince[s];   // pattern starts right when this alarm starts
  unsigned long p;
  switch (s) {
    case 0:   // MQ-2
      p = t % 1000;
      return (p < 150) || (p >= 250 && p < 400);
    case 1:   // MQ-7
      p = t % 1200;
      return (p < 800);
    default:  // MQ-135
      p = t % 1500;
      return (p < 100) || (p >= 200 && p < 300) || (p >= 400 && p < 500);
  }
}

int activeSensor() {
  for (int k = 0; k < N; k++) {
    if (alarmOn[PRIORITY[k]]) return PRIORITY[k];
  }
  return -1;
}

void updateOutputs() {
  if (!calibrated) return;
  int s = activeSensor();
  if (s >= 0) {
    bool flash = (millis() / 250) % 2 == 0;
    setLeds(LOW, flash);
    if (!muted && patternOn(s)) {
      buzzerOn();
      if (USE_PASSIVE_BUZZER) setBuzzerFreq(sensorTone[s]);
    } else {
      buzzerOff();
    }
  } else {
    muted = false;            // everything is clear, so un-mute for the next alarm
    setLeds(HIGH, LOW);
    buzzerOff();
  }
}

void playPreviewSounds() {
  Serial.println("Sound preview: MQ-2 (two short beeps)");
  beep(2, 150, 100);
  delay(700);
  Serial.println("Sound preview: MQ-7 (one long beep)");
  beep(1, 800, 0);
  delay(700);
  Serial.println("Sound preview: MQ-135 (three quick beeps)");
  beep(3, 100, 100);
}

void testHardware() {
  Serial.println("=== HARDWARE TEST ===");
  setLeds(HIGH, LOW);
  delay(500);
  setLeds(LOW, HIGH);
  delay(500);
  setLeds(LOW, LOW);
  playPreviewSounds();
  Serial.println("=== TEST DONE ===");
}

// Median of SAMPLES readings: throws away spikes instead of averaging them in.
int readSensor(int i) {
  int v[SAMPLES];
  for (int s = 0; s < SAMPLES; s++) {
    v[s] = analogRead(sensorPin[i]);
    delay(2);
  }
  for (int a = 1; a < SAMPLES; a++) {
    int key = v[a], b = a - 1;
    while (b >= 0 && v[b] > key) { v[b + 1] = v[b]; b--; }
    v[b + 1] = key;
  }
  updateOutputs();
  return v[SAMPLES / 2];
}

void recalibrate() {
  calibrated = false;
  calStart   = 0;
  calN       = 0;
  bufCount   = 0;
  bufIdx     = 0;
  dropCount  = 0;
  muted      = false;
  for (int i = 0; i < N; i++) {
    calSum[i] = 0;
    calSumSq[i] = 0;
    alarmOn[i] = false;
    upCount[i] = 0;
  }
  buzzerOff();
  publishStatus("calibrating", 0, true);
}

void checkButton() {
  if (digitalRead(bootButton) != LOW) return;
  while (digitalRead(bootButton) == LOW) delay(20);
  delay(100);
  Serial.println(">>> Button: re-learning baseline (keep smoke away)");
  beep(1, 100, 0);
  recalibrate();
}

// ==========================================
// --- 4. SECURE MQTT & NETWORK LOGIC ---
// ==========================================
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 8000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi connection timed out. Will retry in loop.");
  }
}

// Commands the app can send to topic elective3/gas_node/cmd:
//   recalibrate  - re-learn the baseline
//   mute         - silence the buzzer until the alarm clears
//   test         - play the three alarm sounds
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim();
  Serial.printf("MQTT Ingest [%s]: %s\n", topic, message.c_str());

  if (message == "recalibrate") {
    Serial.println(">>> Remote command: recalibrating...");
    recalibrate();
  } else if (message == "mute") {
    if (activeSensor() >= 0) {
      muted = true;
      Serial.println(">>> Remote command: buzzer muted");
    }
  } else if (message == "test") {
    testRequested = true;   // played from loop(), not inside the callback
  }
}

void maintainMqtt() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      Serial.print("Attempting Secure MQTT connection...");

      String clientId = "ESP32-GasNode-" + String(random(0xffff), HEX);

      if (client.connect(clientId.c_str(), mqtt_username, mqtt_password)) {
        Serial.println("connected!");
        client.subscribe(topic_commands);
      } else {
        Serial.printf("failed, rc=%d. Will try again in 5s\n", client.state());
      }
    }
  } else {
    client.loop();
  }
}

// ==========================================
// --- 5. ARDUINO SETUP & MAIN LOOP ---
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(500);

  analogReadResolution(12);
  for (int i = 0; i < N; i++) analogSetPinAttenuation(sensorPin[i], ADC_0db);  // MQ-2, MQ-135 (and default)
  analogSetPinAttenuation(sensorPin[1], ADC_11db);                              // MQ-7 needs the wider range

  pinMode(blueLed, OUTPUT);
  pinMode(redLed, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(bootButton, INPUT_PULLUP);

  setLeds(LOW, LOW);
  digitalWrite(buzzerPin, BUZZER_ACTIVE_LOW ? HIGH : LOW);

  for (int i = 0; i < N; i++) effAlarm[i] = ALARM_MIN[i];

  testHardware();
  recalibrate();

  setup_wifi();

  if (TELEGRAM_BOOT_MSG) {
    int code = telegramSend("✅ SafeHome node is online");
    Serial.printf("(telegram test message -> HTTP %d)\n", code);
  }

  espClient.setInsecure();   // skips certificate check (fine for a school prototype)

  client.setServer(mqtt_server, mqtt_port);
  client.setBufferSize(512);   // IMPORTANT: default is 256 bytes, too small for the telemetry JSON
  client.setCallback(mqttCallback);

  lastTick = millis();
}

void loop() {
  maintainMqtt();
  checkButton();

  if (testRequested) {
    testRequested = false;
    if (activeSensor() < 0) {
      Serial.println(">>> Remote command: playing test sounds");
      playPreviewSounds();
    }
  }

  updateOutputs();

  if (millis() - lastTick < TICK_MS) return;
  lastTick = millis();

  int   raw[N];
  float avg[N];
  for (int i = 0; i < N; i++) raw[i] = SENSOR_ENABLED[i] ? readSensor(i) : 0;

  // --- WARM-UP: don't learn or alarm until the sensor heaters settle ---
  if (millis() < WARMUP_MS) {
    setLeds((millis() / 500) % 2, LOW);   // blue blinks while warming up
    publishStatus("warmup", (long)((WARMUP_MS - millis()) / 1000));
    if (millis() - lastWarmPrint >= 5000) {
      lastWarmPrint = millis();
      Serial.printf("(warming up... %lus left) MQ-2=%d MQ-7=%d MQ-135=%d\n",
                    (WARMUP_MS - millis()) / 1000, raw[0], raw[1], raw[2]);
    }
    return;
  }

  // --- MQ-2 dropout: skip bad samples, only reset if it stays dead ---
  if (SENSOR_ENABLED[0] && raw[0] < DROPOUT_BELOW) {
    if (++dropCount >= 30) {
      Serial.println("(MQ-2 dead: check AO/VCC/GND wiring)");
      recalibrate();
    }
    return;
  }
  dropCount = 0;

  // --- Update smoothing window ---
  for (int i = 0; i < N; i++) buf[i][bufIdx] = raw[i];
  bufIdx = (bufIdx + 1) % WIN;
  if (bufCount < WIN) bufCount++;
  for (int i = 0; i < N; i++) {
    float s = 0;
    for (int k = 0; k < bufCount; k++) s += buf[i][k];
    avg[i] = s / bufCount;
  }

  for (int i = 0; i < N; i++) {
    if (SENSOR_ENABLED[i] && raw[i] >= SATURATED && millis() - lastSatPrint > 5000) {
      lastSatPrint = millis();
      Serial.printf("(%s saturated: signal is at the top of the ADC range)\n", sensorName[i]);
    }
  }

  // --- CALIBRATION ---
  if (!calibrated) {
    setLeds(HIGH, LOW);
    publishStatus("calibrating", 0);
    if (bufCount < WIN) {
      if (bufCount % 5 == 0) Serial.printf("(filling window %d/%d)\n", bufCount, WIN);
      return;
    }
    if (calStart == 0) calStart = millis();
    for (int i = 0; i < N; i++) {
      calSum[i]   += avg[i];
      calSumSq[i] += avg[i] * avg[i];
    }
    calN++;
    if (millis() - calStart > CALIBRATE_MS && calN > 10) {
      for (int i = 0; i < N; i++) {
        baseline[i] = calSum[i] / calN;
        float var = calSumSq[i] / calN - baseline[i] * baseline[i];
        sigma[i] = var > 0 ? sqrt(var) : 1.0;
        effAlarm[i] = constrain((int)(SIGMA_ALARM * sigma[i]), ALARM_MIN[i], ALARM_MAX[i]);
        Serial.printf("%s: base=%.0f sigma=%.1f -> alarm>%d%s\n", sensorName[i], baseline[i],
                      sigma[i], effAlarm[i], SENSOR_ENABLED[i] ? "" : " (disabled)");
        if (SENSOR_ENABLED[i] && sigma[i] > 40)
          Serial.printf("WARNING: %s is very noisy, check wiring/power\n", sensorName[i]);
      }
      calibrated = true;
      publishStatus("ready", 0, true);
    }
    return;
  }

  // --- DETECTION (each sensor works on its own) ---
  int rise[N];
  for (int i = 0; i < N; i++) rise[i] = (int)(avg[i] - baseline[i]);

  for (int i = 0; i < N; i++) {
    if (!SENSOR_ENABLED[i]) continue;

    bool trigger   = rise[i] > effAlarm[i];
    bool stillHigh = rise[i] > (effAlarm[i] - HYSTERESIS);

    if (!alarmOn[i]) {
      // baseline follows slow drift, but not a fast rise
      if (rise[i] < 0) baseline[i] += (avg[i] - baseline[i]) / DRIFT_DOWN_DIV;
      else if (rise[i] < effAlarm[i] / 2) baseline[i] += (avg[i] - baseline[i]) / DRIFT_UP_DIV;

      upCount[i] = trigger ? upCount[i] + 1 : 0;
      if (upCount[i] >= CONFIRM_COUNT) {
        alarmOn[i] = true;
        alarmSince[i] = millis();
        lastHigh[i] = millis();
        upCount[i] = 0;
        muted = false;   // a new alarm always sounds, even if an earlier one was muted
        Serial.printf("*** ALARM *** %s detected\n", sensorName[i]);

        if (client.connected()) {
          char msg[96];
          snprintf(msg, sizeof(msg), "{\"status\":\"ALARM\",\"source\":\"%s\"}", sensorName[i]);
          client.publish(topic_alarm, msg);
        }

        sendTelegramAlert(i);   // Telegram message (works even if the app is closed)
        sendPhoneAlert(i);      // ntfy notification (only if NTFY_ENABLED is true)
      }
    } else {
      if (stillHigh) lastHigh[i] = millis();
      if (millis() - alarmSince[i] >= ALARM_HOLD_MS && millis() - lastHigh[i] >= CLEAR_QUIET_MS) {
        alarmOn[i] = false;
        Serial.printf("--- %s clear ---\n", sensorName[i]);

        if (client.connected()) {
          char msg[96];
          snprintf(msg, sizeof(msg), "{\"status\":\"CLEAR\",\"source\":\"%s\"}", sensorName[i]);
          client.publish(topic_alarm, msg);
        }
      }
    }
  }

  // --- Serial status line ---
  String st = "";
  for (int i = 0; i < N; i++) if (alarmOn[i]) st += String(sensorName[i]) + " ";
  if (st == "") st = "safe";
  Serial.printf("MQ-2 r=%d(>%d) | MQ-7 r=%d(>%d) | MQ-135 r=%d(>%d) | %s%s\n",
                rise[0], effAlarm[0], rise[1], effAlarm[1], rise[2], effAlarm[2], st.c_str(),
                muted ? "(muted)" : "");

  // --- MQTT telemetry (once per second) ---
  if (millis() - lastMqttPublish >= 1000) {
    lastMqttPublish = millis();
    if (client.connected()) {
      bool anyAlarm = (activeSensor() >= 0);
      char payload[448];
      snprintf(payload, sizeof(payload),
               "{\"calibrated\":%s,\"alarm\":%s,\"muted\":%s,"
               "\"mq2\":{\"raw\":%d,\"avg\":%.1f,\"rise\":%d,\"thresh\":%d,\"alarm\":%s},"
               "\"mq7\":{\"raw\":%d,\"avg\":%.1f,\"rise\":%d,\"thresh\":%d,\"alarm\":%s},"
               "\"mq135\":{\"raw\":%d,\"avg\":%.1f,\"rise\":%d,\"thresh\":%d,\"alarm\":%s}}",
               calibrated ? "true" : "false",
               anyAlarm ? "true" : "false",
               muted ? "true" : "false",
               raw[0], avg[0], rise[0], effAlarm[0], alarmOn[0] ? "true" : "false",
               raw[1], avg[1], rise[1], effAlarm[1], alarmOn[1] ? "true" : "false",
               raw[2], avg[2], rise[2], effAlarm[2], alarmOn[2] ? "true" : "false");

      bool ok = client.publish(topic_telemetry, payload);
      if (!ok) Serial.println("(telemetry publish failed)");
    }
  }

  updateOutputs();
}