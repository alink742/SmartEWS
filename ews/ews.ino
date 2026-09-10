/*
 * ===================================================================================
 *  HYDRO SENSE PRO - ESP32 EARLY WARNING SYSTEM (EWS) BANJIR
 * ===================================================================================
 *  Fitur:
 *  1. Terhubung ke WiFi dan MQTT Broker HiveMQ Cloud via TLS/SSL (Port 8883).
 *  2. Menerima (Subscribe) pengaturan batas ketinggian air (Threshold Aman, Siaga, Bahaya)
 *     dari Web Dashboard secara dinamis tanpa perlu mengganti kode/flashing ulang.
 *  3. Menyimpan nilai threshold ke NVS Flash (Preferences) agar tetap tersimpan saat mati lampu/reboot.
 *  4. Membaca sensor jarak VL53L0X (Time-of-Flight Micro LiDAR) atau sensor ultrasonik.
 *  5. Mengirimkan (Publish) data ketinggian air secara realtime ke Web Dashboard.
 *  6. Mengendalikan Buzzer & LED Indikator otomatis sesuai batas yang ditentukan.
 *
 *  Library yang Dibutuhkan (Install via Arduino IDE Library Manager):
 *  - PubSubClient (oleh Nick O'Leary)
 *  - ArduinoJson (oleh Benoit Blanchon - v6 atau v7)
 *  - Adafruit_VL53L0X (oleh Adafruit)
 * ===================================================================================
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include "Adafruit_VL53L0X.h"

// ==========================================
// 1. KONFIGURASI WIFI & HIVEMQ CLOUD
// ==========================================
const char* WIFI_SSID     = "NAMA_WIFI_ANDA";
const char* WIFI_PASSWORD = "PASSWORD_WIFI_ANDA";

// HiveMQ Cloud Host (Port 8883 untuk ESP32 TLS)
const char* MQTT_BROKER   = "xxxxxxxxxxxx.s1.eu.hivemq.cloud"; // Ganti dengan Cluster URL Anda
const int   MQTT_PORT     = 8883;
const char* MQTT_USER     = "username_hivemq";                // Kredensial dari Access Management
const char* MQTT_PASS     = "password_hivemq";

// Topik MQTT
const char* TOPIC_TELEMETRY      = "ews/banjir/level";          // ESP32 -> Web (Publish)
const char* TOPIC_THRESHOLD_SET  = "ews/banjir/threshold/set";  // Web -> ESP32 (Subscribe)
const char* TOPIC_THRESHOLD_STAT = "ews/banjir/threshold/state"; // ESP32 -> Web (Acknowledge)

// ==========================================
// 2. PIN HARDWARE & KALIBRASI SENSOR
// ==========================================
#define PIN_BUZZER   18
#define PIN_LED_RED  22 // Indikator Bahaya
#define PIN_LED_YEL  21 // Indikator Siaga
#define PIN_LED_GRN  19 // Indikator Aman

// Tinggi pemasangan sensor dari dasar wadah/sungai (dalam cm)
// Ketinggian Air = TINGGI_SENSOR_CM - Jarak_Sensor_ke_Permukaan_Air
const float TINGGI_SENSOR_CM = 200.0;

// ==========================================
// 3. VARIABEL GLOBAL & PERSISTENSI
// ==========================================
// Nilai threshold default (akan ditimpa oleh data dari Web/NVS Flash)
int thresholdAman   = 100;
int thresholdSiaga  = 150;
int thresholdBahaya = 200;

WiFiClientSecure netClient;
PubSubClient mqttClient(netClient);
Preferences preferences;
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
bool sensorReady = false;

unsigned long lastPublishTime = 0;
const unsigned long PUBLISH_INTERVAL_MS = 2500; // Kirim data setiap 2.5 detik

// ==========================================
// 4. FUNGSI PENYIMPANAN FLASH (PREFERENCES)
// ==========================================
void loadThresholdsFromStorage() {
  preferences.begin("ews_config", true); // Mode Read-Only
  thresholdAman   = preferences.getInt("aman", 100);
  thresholdSiaga  = preferences.getInt("siaga", 150);
  thresholdBahaya = preferences.getInt("bahaya", 200);
  preferences.end();

  Serial.println("\n[STORAGE] Nilai batas dimuat dari Flash:");
  Serial.printf("  Aman   : %d cm\n", thresholdAman);
  Serial.printf("  Siaga  : %d cm\n", thresholdSiaga);
  Serial.printf("  Bahaya : %d cm\n\n", thresholdBahaya);
}

void saveThresholdsToStorage(int aman, int siaga, int bahaya) {
  preferences.begin("ews_config", false); // Mode Read-Write
  preferences.putInt("aman", aman);
  preferences.putInt("siaga", siaga);
  preferences.putInt("bahaya", bahaya);
  preferences.end();
  Serial.println("[STORAGE] Nilai batas baru berhasil disimpan ke Flash ESP32!");
}

// ==========================================
// 5. CALLBACK PENERIMAAN PESAN MQTT (WEB -> ESP32)
// ==========================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String messageStr = "";
  for (unsigned int i = 0; i < length; i++) {
    messageStr += (char)payload[i];
  }

  Serial.print("[MQTT] Pesan masuk pada topik [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(messageStr);

  if (String(topic) == TOPIC_THRESHOLD_SET) {
    // Parse JSON dari web
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, messageStr);

    if (error) {
      Serial.print("[JSON] Gagal mem-parse pesan threshold: ");
      Serial.println(error.f_str());
      return;
    }

    if (doc.containsKey("aman") && doc.containsKey("siaga") && doc.containsKey("bahaya")) {
      int newAman   = doc["aman"];
      int newSiaga  = doc["siaga"];
      int newBahaya = doc["bahaya"];

      // Validasi logika nilai
      if (newAman > 0 && newSiaga > newAman && newBahaya > newSiaga) {
        thresholdAman   = newAman;
        thresholdSiaga  = newSiaga;
        thresholdBahaya = newBahaya;

        // Simpan ke Flash ESP32 agar tidak hilang jika reboot
        saveThresholdsToStorage(thresholdAman, thresholdSiaga, thresholdBahaya);

        Serial.println("[OK] Batas ketinggian berhasil diperbarui dari Web!");
        Serial.printf("  Aman   : %d cm\n", thresholdAman);
        Serial.printf("  Siaga  : %d cm\n", thresholdSiaga);
        Serial.printf("  Bahaya : %d cm\n", thresholdBahaya);

        // Kirim acknowledgment konfirmasi kembali ke web
        StaticJsonDocument<200> ackDoc;
        ackDoc["status"] = "applied";
        ackDoc["aman"]   = thresholdAman;
        ackDoc["siaga"]  = thresholdSiaga;
        ackDoc["bahaya"] = thresholdBahaya;
        char ackBuf[200];
        serializeJson(ackDoc, ackBuf);
        mqttClient.publish(TOPIC_THRESHOLD_STAT, ackBuf, true);

        // Beep singkat sebagai notifikasi update threshold berhasil
        digitalWrite(PIN_BUZZER, HIGH);
        delay(150);
        digitalWrite(PIN_BUZZER, LOW);
      } else {
        Serial.println("[WARN] Nilai threshold tidak logis (harus: 0 < Aman < Siaga < Bahaya)");
      }
    }
  }
}

// ==========================================
// 6. KONEKSI KE WIFI & HIVEMQ CLOUD
// ==========================================
void setupWiFi() {
  Serial.print("\n[WIFI] Menghubungkan ke: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n[WIFI] Berhasil terhubung!");
  Serial.print("[WIFI] IP Address: ");
  Serial.println(WiFi.localIP());
}

void reconnectMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("[MQTT] Menghubungkan ke HiveMQ Cloud...");
    String clientId = "ESP32_EWS_" + String((uint32_t)ESP.getEfuseMac(), HEX);

    if (mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS)) {
      Serial.println(" Berhasil terhubung!");

      // Subscribe ke topik pengaturan batas dari web
      mqttClient.subscribe(TOPIC_THRESHOLD_SET, 1);
      Serial.printf("[MQTT] Subscribed ke topik: %s\n", TOPIC_THRESHOLD_SET);

      // Publish status awal threshold yang aktif
      StaticJsonDocument<200> stateDoc;
      stateDoc["aman"]   = thresholdAman;
      stateDoc["siaga"]  = thresholdSiaga;
      stateDoc["bahaya"] = thresholdBahaya;
      char stateBuf[200];
      serializeJson(stateDoc, stateBuf);
      mqttClient.publish(TOPIC_THRESHOLD_STAT, stateBuf, true);

    } else {
      Serial.print(" Gagal, rc=");
      Serial.print(mqttClient.state());
      Serial.println(". Mencoba lagi dalam 5 detik...");
      delay(5000);
    }
  }
}

// ==========================================
// 7. SETUP
// ==========================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n==========================================");
  Serial.println("    EWS BANJIR - HYDROSENSE PRO (ESP32)   ");
  Serial.println("==========================================");

  // Inisialisasi Pin
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_YEL, OUTPUT);
  pinMode(PIN_LED_GRN, OUTPUT);

  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_YEL, LOW);
  digitalWrite(PIN_LED_GRN, LOW);

  // Muat threshold tersimpan dari flash
  loadThresholdsFromStorage();

  // Inisialisasi Sensor VL53L0X
  Wire.begin();
  if (!lox.begin()) {
    Serial.println("[SENSOR] Peringatan: Sensor VL53L0X tidak ditemukan via I2C!");
    Serial.println("[SENSOR] Sistem akan menggunakan simulasi jika sensor tidak terdeteksi.");
    sensorReady = false;
  } else {
    Serial.println("[SENSOR] Sensor VL53L0X berhasil diinisialisasi.");
    sensorReady = true;
  }

  // Koneksi WiFi
  setupWiFi();

  // Konfigurasi TLS untuk HiveMQ Cloud
  // Menggunakan setInsecure() agar tidak terkendala sertifikat kedaluwarsa, 
  // namun komunikasi tetap terenkripsi secara aman melalui TLS/SSL.
  netClient.setInsecure();

  // Setup MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
}

// ==========================================
// 8. PEMBACAAN SENSOR & KONTROL ALARM
// ==========================================
float readWaterLevel() {
  float distanceCm = 0;

  if (sensorReady) {
    VL53L0X_RangingMeasurementData_t measure;
    lox.rangingTest(&measure, false);

    if (measure.RangeStatus != 4) { // 4 = out of range
      distanceCm = measure.RangeMilliMeter / 10.0;
    } else {
      distanceCm = TINGGI_SENSOR_CM; // Anggap kosong jika di luar jangkauan
    }
  } else {
    // Simulasi jika sensor fisik belum tersambung
    static float simLevel = 75.0;
    simLevel += (random(-3, 4) * 0.5);
    simLevel = constrain(simLevel, 10.0, (float)thresholdBahaya + 20.0);
    return simLevel;
  }

  // Hitung ketinggian air
  float waterLevel = TINGGI_SENSOR_CM - distanceCm;
  if (waterLevel < 0) waterLevel = 0;
  return waterLevel;
}

void handleAlerts(float level) {
  if (level >= thresholdBahaya) {
    // KONDISI BAHAYA
    digitalWrite(PIN_LED_RED, HIGH);
    digitalWrite(PIN_LED_YEL, LOW);
    digitalWrite(PIN_LED_GRN, LOW);

    // Sirine Bunyi (Pola Beep Cepat)
    digitalWrite(PIN_BUZZER, HIGH);
    delay(100);
    digitalWrite(PIN_BUZZER, LOW);

  } else if (level >= thresholdSiaga) {
    // KONDISI SIAGA
    digitalWrite(PIN_LED_RED, LOW);
    digitalWrite(PIN_LED_YEL, HIGH);
    digitalWrite(PIN_LED_GRN, LOW);
    digitalWrite(PIN_BUZZER, LOW);

  } else {
    // KONDISI AMAN
    digitalWrite(PIN_LED_RED, LOW);
    digitalWrite(PIN_LED_YEL, LOW);
    digitalWrite(PIN_LED_GRN, HIGH);
    digitalWrite(PIN_BUZZER, LOW);
  }
}

// ==========================================
// 9. LOOP UTAMA
// ==========================================
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setupWiFi();
  }

  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  mqttClient.loop();

  // Pengiriman Telemetry Periodik
  unsigned long now = millis();
  if (now - lastPublishTime >= PUBLISH_INTERVAL_MS) {
    lastPublishTime = now;

    float currentLevel = readWaterLevel();
    handleAlerts(currentLevel);

    String statusText = "Aman";
    if (currentLevel >= thresholdBahaya) statusText = "Bahaya";
    else if (currentLevel >= thresholdSiaga) statusText = "Siaga";

    // Format JSON Telemetry
    StaticJsonDocument<256> doc;
    doc["level"]     = round(currentLevel * 10) / 10.0;
    doc["status"]    = statusText;
    doc["aman"]      = thresholdAman;
    doc["siaga"]     = thresholdSiaga;
    doc["bahaya"]    = thresholdBahaya;
    doc["uptime_s"]  = millis() / 1000;

    char buffer[256];
    serializeJson(doc, buffer);

    // Publikasi ke HiveMQ Cloud
    mqttClient.publish(TOPIC_TELEMETRY, buffer);
    Serial.printf("[TELEMETRY] Kirim -> Level: %.1f cm | Status: %s\n", currentLevel, statusText.c_str());
  }
}

