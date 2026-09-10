# HydroSense Pro - Early Warning System (EWS) Banjir IoT

Sistem monitoring ketinggian air dan peringatan dini banjir berbasis **ESP32** dan **Web Dashboard**, terhubung secara realtime melalui broker MQTT **HiveMQ Cloud**.

Fitur utama:
- **Komunikasi Dua Arah (Bidirectional)**:
  - **Menerima Data Sensor (Subscribe)**: Data ketinggian air ditampilkan secara realtime dalam bentuk angka, status siaga (Aman / Siaga / Bahaya), grafik telemetri interaktif (*Chart.js*), dan alarm audio.
  - **Mengirim Pengaturan Batas / Threshold (Publish)**: Pengguna dapat mengubah batas *Aman*, *Siaga*, dan *Bahaya* langsung melalui antarmuka Web Dashboard tanpa perlu mengubah kode atau mem-flash ulang ESP32.
- **Penyimpanan Permanen (Flash NVS)**: ESP32 menyimpan batas ketinggian terakhir ke memori flash (*Preferences*), sehingga pengaturan tidak hilang saat mati lampu atau reboot.
- **Dukungan HiveMQ Cloud**:
  - Web Dashboard: Menggunakan **Secure WebSockets (WSS)** pada port **8884** (path `/mqtt`).
  - ESP32: Menggunakan **TLS/SSL TCP** pada port **8883**.

---

## 1. Persiapan HiveMQ Cloud
1. Daftar atau masuk ke [HiveMQ Cloud](https://www.hivemq.com/cloud/).
2. Buat cluster gratis (Serverless).
3. Salin **Cluster URL**, contoh: `xxxxxxxxxxxx.s1.eu.hivemq.cloud`.
4. Buka tab **Access Management** di konsol HiveMQ Cloud:
   - Buat kredensial baru (**Username** dan **Password**).
   - Pastikan hak akses mengizinkan Publish dan Subscribe ke topik yang digunakan.

---

## 2. Menjalankan Web Dashboard
1. Buka file [index.html](file:///home/alings/ews%20banjir/index.html) langsung di browser (Google Chrome, Firefox, Edge, atau melalui Live Server).
2. Masuk ke tab/menu **MQTT Config**:
   - **Broker Host**: Masukkan Cluster URL Anda (contoh: `xxxxxxxxxxxx.s1.eu.hivemq.cloud`).
   - **Port**: `8884` (Default WSS).
   - **Path**: `/mqtt`.
   - **Username**: Username dari Access Management HiveMQ.
   - **Password**: Password dari Access Management HiveMQ.
   - **Topik Sensor (Subscribe)**: `ews/banjir/level`.
   - **Topik Batas (Publish)**: `ews/banjir/threshold/set`.
3. Klik tombol **Hubungkan ke HiveMQ**.
   - Indikator status di header akan berubah menjadi hijau (**Connected**).
   - Konfigurasi otomatis tersimpan di browser Anda (*localStorage*).

### Mengatur Batas Ketinggian dari Web
1. Pada kartu **Threshold Settings** di halaman Overview:
   - Tentukan batas **Aman (Safe) Max (cm)** (contoh: `100`).
   - Tentukan batas **Siaga (Warning) Max (cm)** (contoh: `150`).
   - Tentukan batas **Bahaya (Danger) Limit (cm)** (contoh: `200`).
2. Klik tombol **Simpan & Kirim ke MQTT**.
3. Nilai ini langsung dipublish ke topik `ews/banjir/threshold/set` dengan flag *retained*, sehingga ESP32 akan langsung menerimanya dan menerapkannya seketika.

---

## 3. Menyiapkan ESP32
Kode program ESP32 tersedia pada file [esp32/ews_banjir_esp32.ino](file:///home/alings/ews%20banjir/esp32/ews_banjir_esp32.ino).

### Library yang Diperlukan (Arduino IDE)
Buka menu **Sketch > Include Library > Manage Libraries...**, lalu pasang pustaka berikut:
- **PubSubClient** oleh Nick O'Leary
- **ArduinoJson** oleh Benoit Blanchon (versi 6 atau 7)
- **Adafruit_VL53L0X** oleh Adafruit (jika menggunakan sensor ToF VL53L0X)

### Skema Pin Hardware
| Komponen | Pin ESP32 | Keterangan |
| :--- | :--- | :--- |
| **Sensor VL53L0X (SDA)** | GPIO 21 | Jalur data I2C |
| **Sensor VL53L0X (SCL)** | GPIO 22 | Jalur clock I2C |
| **Buzzer** | GPIO 18 | Alarm suara (aktif saat Bahaya) |
| **LED Merah** | GPIO 22 (atau pin digital lain) | Indikator Bahaya |
| **LED Kuning** | GPIO 21 (atau pin digital lain) | Indikator Siaga |
| **LED Hijau** | GPIO 19 | Indikator Aman |

### Konfigurasi Kode ESP32
Buka [esp32/ews_banjir_esp32.ino](file:///home/alings/ews%20banjir/esp32/ews_banjir_esp32.ino), lalu sesuaikan bagian berikut:
```cpp
const char* WIFI_SSID     = "NAMA_WIFI_ANDA";
const char* WIFI_PASSWORD = "PASSWORD_WIFI_ANDA";

const char* MQTT_BROKER   = "xxxxxxxxxxxx.s1.eu.hivemq.cloud"; // Cluster URL HiveMQ Anda
const int   MQTT_PORT     = 8883;                              // Port TLS ESP32
const char* MQTT_USER     = "username_hivemq";
const char* MQTT_PASS     = "password_hivemq";
```
Upload kode ke board ESP32 Anda. Buka **Serial Monitor** pada baudrate `115200` untuk memantau status koneksi dan telemetri data.

---

## 4. Topik MQTT yang Digunakan
| Arah Komunikasi | Topik | Format Payload | Penjelasan |
| :--- | :--- | :--- | :--- |
| **ESP32 -> Web** | `ews/banjir/level` | `{"level": 85.5, "status": "Aman", "aman": 100, "siaga": 150, "bahaya": 200}` | Telemetri ketinggian air realtime |
| **Web -> ESP32** | `ews/banjir/threshold/set` | `{"aman": 100, "siaga": 150, "bahaya": 200}` | Mengirim konfigurasi batas baru dari web |
| **ESP32 -> Web** | `ews/banjir/threshold/state` | `{"status": "applied", "aman": 100, "siaga": 150, "bahaya": 200}` | Konfirmasi batas aktif yang tersimpan di ESP32 |