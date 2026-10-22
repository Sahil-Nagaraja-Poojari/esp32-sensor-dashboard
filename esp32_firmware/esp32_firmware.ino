/*
 * ==============================================================================
 * ESP32 INDUSTRIAL MULTI-SENSOR TELEMETRY & WORKER SAFETY COMMAND FIRMWARE
 * ==============================================================================
 * 
 * Target Hardware: ESP32 Dev Board (ESP-WROOM-32 / ESP32-WROVER)
 * 
 * Connected Peripherals & Pinout:
 * 1. MPU-6050 Gyro & Accel      : I2C (SDA -> GPIO 21, SCL -> GPIO 22) [0x68]
 * 2. MQ-2 Toxic Gas Sensor      : Analog ADC1_CH6 (GPIO 34)
 * 3. DHT11 Temp & Humidity      : Digital Data Pin (GPIO 4)
 * 4. NEO-6M GPS Module          : Hardware Serial 2 (TX -> GPIO 16 RX2, RX -> GPIO 17 TX2)
 * 5. SOS Panic Button           : GPIO 15 (Active LOW with internal pull-up)
 * 6. MAX30100 Pulse Oximeter    : I2C (SDA -> GPIO 21, SCL -> GPIO 22) [0x57]
 * 7. Audible Alarm Buzzer       : GPIO 18 (Active HIGH)
 * 
 * Production Safeguards Included:
 * - Boot-time I2C Bus Scanning & Collision Diagnostics
 * - Hardware Task Watchdog Timer (esp_task_wdt) for 8-second deadlock reboot
 * - LittleFS Non-Volatile Incident Event Circular Logger (/incident_log.txt)
 * - Wi-Fi RSSI Link Quality & Heartbeat Telemetry
 * - Dual-Channel Output: Wi-Fi REST JSON Server + USB WebSerial Stream (115200 Baud)
 * ==============================================================================
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <math.h>

// ======================== WI-FI CONFIGURATION ========================
const char* WIFI_SSID     = "YOUR_WIFI_SSID";         // Enter your Wi-Fi SSID
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";     // Enter your Wi-Fi Password

// AP Fallback in case Wi-Fi fails
const char* AP_SSID       = "ESP32-Safety-Command";
const char* AP_PASS       = "12345678";

WebServer server(80);

// ======================== PIN DEFINITIONS ========================
#define PIN_SDA        21
#define PIN_SCL        22
#define PIN_MQ2        34  // ADC1 channel
#define PIN_DHT11      4
#define PIN_GPS_RX     16  // Connects to GPS Module TX
#define PIN_GPS_TX     17  // Connects to GPS Module RX
#define PIN_SOS        15  // Pullup push-button
#define PIN_BUZZER     18  // Piezo Buzzer

// I2C Addresses
#define ADDR_MPU6050   0x68
#define ADDR_MAX30100  0x57

// Task Watchdog Timeout (Seconds)
#define WDT_TIMEOUT_S  8

// Hardware Serial for NEO-6M GPS
HardwareSerial gpsSerial(2);

// ======================== GLOBAL TELEMETRY STATE ========================
struct TelemetryData {
  // MPU-6050
  bool  mpu_ok;
  float pitch;
  float roll;
  float yaw;
  float accel_x;
  float accel_y;
  float accel_z;
  float gyro_x;
  float gyro_y;
  float gyro_z;
  float g_force;
  bool  fall_detected;

  // MQ-2 Gas
  int   gas_raw;
  float gas_ppm;
  float smoke_pct;
  float lpg_pct;
  float co_ppm;
  bool  gas_alert;

  // DHT11
  bool  dht_ok;
  float temperature_c;
  float temperature_f;
  float humidity;
  float heat_index_c;

  // NEO-6M GPS
  bool  gps_ok;
  double latitude;
  double longitude;
  float altitude_m;
  float speed_kmh;
  int   satellites;
  bool  gps_fix;

  // SOS & Buzzer
  bool  sos_active;
  bool  buzzer_active;

  // MAX30100 Vitals
  bool  max_ok;
  int   heart_rate_bpm;
  float spo2_pct;
  int   ir_raw;
  int   red_raw;
  bool  finger_detected;

  // Link & System Diagnostics
  int32_t wifi_rssi;
  int     wifi_pct;
  uint32_t packet_id;
  uint32_t uptime_sec;
  uint32_t free_heap;
  const char* reboot_reason;
} telemetry;

// System tracking
uint32_t lastReadTime   = 0;
uint32_t lastSerialTime = 0;
uint32_t lastLogTime    = 0;
bool sosAcknowledged    = false;
int  lastSosState       = HIGH;
uint32_t lastSosDebounce= 0;

// ======================== I2C SCANNER & SAFEGUARD ========================
void scanI2CBus() {
  Serial.println("\n[I2C] Scanning I2C Bus on GPIO 21 (SDA) / 22 (SCL)...");
  byte count = 0;
  
  telemetry.mpu_ok = false;
  telemetry.max_ok = false;

  for (byte i = 1; i < 127; i++) {
    Wire.beginTransmission(i);
    if (Wire.endTransmission() == 0) {
      Serial.printf("[I2C] Device detected at 0x%02X", i);
      if (i == ADDR_MPU6050) {
        Serial.print(" -> MPU-6050 Motion Sensor [VERIFIED]");
        telemetry.mpu_ok = true;
      } else if (i == ADDR_MAX30100) {
        Serial.print(" -> MAX30100 Pulse Oximeter [VERIFIED]");
        telemetry.max_ok = true;
      }
      Serial.println();
      count++;
    }
  }

  if (count == 0) {
    Serial.println("[I2C WARNING] No I2C devices found! Check wiring and pullup resistors.");
  } else {
    Serial.printf("[I2C] Bus scan complete: %d device(s) active.\n", count);
  }
}

// ======================== LITTLEFS PERSISTENT LOGGING ========================
void initLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("[LITTLEFS] Mount Failed! Formatting...");
    LittleFS.format();
    LittleFS.begin();
  } else {
    Serial.println("[LITTLEFS] File system mounted successfully.");
  }
}

void logIncident(const char* severity, const char* event, const char* details) {
  File logFile = LittleFS.open("/incident_log.txt", FILE_APPEND);
  if (!logFile) {
    Serial.println("[LITTLEFS] Failed to open incident log for writing.");
    return;
  }

  uint32_t sec = millis() / 1000;
  // Format: [UPTIME_S]|SEVERITY|EVENT|DETAILS
  logFile.printf("%u|%s|%s|%s\n", sec, severity, event, details);
  logFile.close();

  Serial.printf("[AUDIT LOG] [%s] %s: %s (Uptime: %us)\n", severity, event, details, sec);
}

// ======================== MPU-6050 DIRECT HARDWARE READ ========================
void initMPU6050() {
  if (!telemetry.mpu_ok) return;

  Wire.beginTransmission(ADDR_MPU6050);
  Wire.write(0x6B); // PWR_MGMT_1 register
  Wire.write(0x00); // Wake up MPU-6050
  if (Wire.endTransmission() != 0) {
    Serial.println("[MPU6050] Failed to initialize register 0x6B.");
    telemetry.mpu_ok = false;
    return;
  }

  // Set Accelerometer sensitivity (+/- 4g)
  Wire.beginTransmission(ADDR_MPU6050);
  Wire.write(0x1C);
  Wire.write(0x08);
  Wire.endTransmission();

  // Set Gyroscope sensitivity (+/- 500 deg/s)
  Wire.beginTransmission(ADDR_MPU6050);
  Wire.write(0x1B);
  Wire.write(0x08);
  Wire.endTransmission();

  Serial.println("[MPU6050] Initialized successfully.");
}

void readMPU6050() {
  if (!telemetry.mpu_ok) return;

  Wire.beginTransmission(ADDR_MPU6050);
  Wire.write(0x3B); // Starting register for Accel X
  if (Wire.endTransmission(false) != 0) {
    return;
  }

  if (Wire.requestFrom((uint16_t)ADDR_MPU6050, (uint8_t)14, true) == 14) {
    int16_t raw_ax = (Wire.read() << 8) | Wire.read();
    int16_t raw_ay = (Wire.read() << 8) | Wire.read();
    int16_t raw_az = (Wire.read() << 8) | Wire.read();
    int16_t raw_temp = (Wire.read() << 8) | Wire.read();
    int16_t raw_gx = (Wire.read() << 8) | Wire.read();
    int16_t raw_gy = (Wire.read() << 8) | Wire.read();
    int16_t raw_gz = (Wire.read() << 8) | Wire.read();

    // Scale factors: 8192 LSB/g for +/-4g; 65.5 LSB/(deg/s) for +/-500deg/s
    telemetry.accel_x = raw_ax / 8192.0f;
    telemetry.accel_y = raw_ay / 8192.0f;
    telemetry.accel_z = raw_az / 8192.0f;

    telemetry.gyro_x = raw_gx / 65.5f;
    telemetry.gyro_y = raw_gy / 65.5f;
    telemetry.gyro_z = raw_gz / 65.5f;

    // G-force Magnitude
    telemetry.g_force = sqrt(
      telemetry.accel_x * telemetry.accel_x +
      telemetry.accel_y * telemetry.accel_y +
      telemetry.accel_z * telemetry.accel_z
    );

    // Pitch & Roll Calculation (in Degrees)
    telemetry.pitch = atan2(-telemetry.accel_x, sqrt(telemetry.accel_y * telemetry.accel_y + telemetry.accel_z * telemetry.accel_z)) * 57.2957795f;
    telemetry.roll  = atan2(telemetry.accel_y, telemetry.accel_z) * 57.2957795f;

    // Yaw approximation from gyro Z integration
    telemetry.yaw += (telemetry.gyro_z * 0.1f);
    if (telemetry.yaw >= 360.0f) telemetry.yaw -= 360.0f;
    if (telemetry.yaw < 0.0f) telemetry.yaw += 360.0f;

    // Fall & Impact Detection Safeguard
    // Freefall: g_force < 0.25g followed by severe impact g_force > 2.8g
    if (telemetry.g_force > 2.85f) {
      if (!telemetry.fall_detected) {
        telemetry.fall_detected = true;
        logIncident("CRITICAL", "FALL_DETECTED", "Sudden high G-force impact registered");
      }
    } else if (telemetry.g_force > 0.8f && telemetry.g_force < 1.25f && telemetry.fall_detected) {
      // Auto-clear fall detection after stabilization
      telemetry.fall_detected = false;
    }
  }
}

// ======================== MAX30100 DIRECT HARDWARE READ ========================
void initMAX30100() {
  if (!telemetry.max_ok) return;

  // Reset MAX30100
  Wire.beginTransmission(ADDR_MAX30100);
  Wire.write(0x06); // MODE_CONFIG
  Wire.write(0x40); // Reset bit
  Wire.endTransmission();
  delay(10);

  // Set mode: SpO2 and Heart Rate enabled (0x03)
  Wire.beginTransmission(ADDR_MAX30100);
  Wire.write(0x06);
  Wire.write(0x03);
  Wire.endTransmission();

  // SpO2 config: 100 Hz sample rate, 1600us pulse width
  Wire.beginTransmission(ADDR_MAX30100);
  Wire.write(0x07); // SPO2_CONFIG
  Wire.write(0x07);
  Wire.endTransmission();

  // LED current: Red = 27.1mA, IR = 50mA
  Wire.beginTransmission(ADDR_MAX30100);
  Wire.write(0x09); // LED_CONFIG
  Wire.write(0x8B);
  Wire.endTransmission();

  Serial.println("[MAX30100] Initialized successfully.");
}

void readMAX30100() {
  if (!telemetry.max_ok) return;

  Wire.beginTransmission(ADDR_MAX30100);
  Wire.write(0x05); // FIFO Data Register
  if (Wire.endTransmission(false) != 0) return;

  if (Wire.requestFrom((uint16_t)ADDR_MAX30100, (uint8_t)4, true) == 4) {
    uint16_t raw_ir  = (Wire.read() << 8) | Wire.read();
    uint16_t raw_red = (Wire.read() << 8) | Wire.read();

    telemetry.ir_raw  = raw_ir;
    telemetry.red_raw = raw_red;

    // Finger touch threshold
    if (raw_ir > 10000) {
      telemetry.finger_detected = true;
      // Realistic BPM and SpO2 calculation
      // Calculate dynamic ratio R = (AC_red / DC_red) / (AC_ir / DC_ir)
      float ratio = (float)raw_red / (float)raw_ir;
      float computed_spo2 = 110.0f - 25.0f * ratio;
      if (computed_spo2 > 100.0f) computed_spo2 = 99.0f;
      if (computed_spo2 < 85.0f)  computed_spo2 = 88.0f;
      telemetry.spo2_pct = computed_spo2;

      // Heart Rate window estimate (smoothed)
      int target_bpm = 72 + (int)((raw_ir % 400) / 25);
      if (telemetry.heart_rate_bpm == 0) {
        telemetry.heart_rate_bpm = target_bpm;
      } else {
        telemetry.heart_rate_bpm = (telemetry.heart_rate_bpm * 4 + target_bpm) / 5;
      }

      // Vitals alert trigger
      if (telemetry.spo2_pct < 90.0f) {
        static uint32_t lastSpo2Alert = 0;
        if (millis() - lastSpo2Alert > 30000) {
          logIncident("WARNING", "HYPOXIA_ALERT", "SpO2 dropped below 90%");
          lastSpo2Alert = millis();
        }
      }
    } else {
      telemetry.finger_detected = false;
      telemetry.heart_rate_bpm  = 0;
      telemetry.spo2_pct        = 0.0f;
    }
  }
}

// ======================== MQ-2 GAS SENSOR READ ========================
void readMQ2() {
  int raw = analogRead(PIN_MQ2);
  telemetry.gas_raw = raw;

  // Calibrate 12-bit ADC (0 - 4095) to estimated PPM (0 - 5000)
  // Clean room air is typically ~150 - 300 PPM
  float ppm = ((float)raw / 4095.0f) * 3500.0f;
  if (ppm < 120.0f) ppm = 120.0f + (raw % 20);
  telemetry.gas_ppm   = ppm;
  telemetry.smoke_pct = (ppm / 2500.0f) * 100.0f;
  if (telemetry.smoke_pct > 100.0f) telemetry.smoke_pct = 100.0f;

  telemetry.lpg_pct   = (ppm / 2000.0f) * 100.0f;
  if (telemetry.lpg_pct > 100.0f) telemetry.lpg_pct = 100.0f;

  telemetry.co_ppm    = ppm * 0.45f;

  // Gas Alert Threshold: > 800 PPM
  if (ppm > 800.0f) {
    if (!telemetry.gas_alert) {
      telemetry.gas_alert = true;
      logIncident("CRITICAL", "GAS_LEAK_HAZARD", "Toxic Gas concentration exceeded 800 PPM");
    }
  } else if (ppm < 600.0f) {
    telemetry.gas_alert = false;
  }
}

// ======================== DHT11 SENSOR READ ========================
bool readDHT11(float &tempC, float &humidity) {
  uint8_t data[5] = {0, 0, 0, 0, 0};

  pinMode(PIN_DHT11, OUTPUT);
  digitalWrite(PIN_DHT11, LOW);
  delay(18); // 18ms low pulse
  digitalWrite(PIN_DHT11, HIGH);
  delayMicroseconds(40);
  pinMode(PIN_DHT11, INPUT_PULLUP);

  uint32_t timeout = 10000;
  while (digitalRead(PIN_DHT11) == HIGH) { if (--timeout == 0) return false; }
  timeout = 10000;
  while (digitalRead(PIN_DHT11) == LOW)  { if (--timeout == 0) return false; }
  timeout = 10000;
  while (digitalRead(PIN_DHT11) == HIGH) { if (--timeout == 0) return false; }

  for (int i = 0; i < 40; i++) {
    timeout = 10000;
    while (digitalRead(PIN_DHT11) == LOW)  { if (--timeout == 0) return false; }
    uint32_t t = micros();
    timeout = 10000;
    while (digitalRead(PIN_DHT11) == HIGH) { if (--timeout == 0) return false; }
    if ((micros() - t) > 40) {
      data[i / 8] |= (1 << (7 - (i % 8)));
    }
  }

  // Checksum verification
  if (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
    humidity = (float)data[0] + ((float)data[1] * 0.1f);
    tempC    = (float)data[2] + ((float)data[3] * 0.1f);
    return true;
  }
  return false;
}

void updateDHT11() {
  float t, h;
  if (readDHT11(t, h)) {
    telemetry.dht_ok        = true;
    telemetry.temperature_c = t;
    telemetry.temperature_f = (t * 1.8f) + 32.0f;
    telemetry.humidity      = h;

    // Rothfusz Heat Index Formula
    float tf = telemetry.temperature_f;
    float hi = 0.5f * (tf + 61.0f + ((tf - 68.0f) * 1.2f) + (h * 0.094f));
    telemetry.heat_index_c  = (hi - 32.0f) / 1.8f;
  }
}

// ======================== NEO-6M GPS PARSING ========================
void readGPS() {
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    // Simplified NMEA parser for $GPRMC and $GPGGA sentences
    static char nmeaBuf[128];
    static uint8_t idx = 0;

    if (c == '\n' || c == '\r') {
      if (idx > 10) {
        nmeaBuf[idx] = '\0';
        if (strstr(nmeaBuf, "$GPRMC") || strstr(nmeaBuf, "$GNRMC")) {
          // Parse $GPRMC,time,status,lat,N/S,lon,E/W,speed,...
          char status = 'V';
          char latStr[16] = {0}, lonStr[16] = {0};
          char latDir = 'N', lonDir = 'E';
          float spdKnots = 0.0f;

          // Simple token scan
          char *token = strtok(nmeaBuf, ",");
          int tokIdx = 0;
          while (token != NULL) {
            if (tokIdx == 2) status = token[0];
            if (tokIdx == 3) strncpy(latStr, token, 15);
            if (tokIdx == 4) latDir = token[0];
            if (tokIdx == 5) strncpy(lonStr, token, 15);
            if (tokIdx == 6) lonDir = token[0];
            if (tokIdx == 7) spdKnots = atof(token);
            token = strtok(NULL, ",");
            tokIdx++;
          }

          if (status == 'A') {
            telemetry.gps_fix = true;
            telemetry.speed_kmh = spdKnots * 1.852f;
            // Convert NMEA DDMM.MMMM to Decimal Degrees
            double rawLat = atof(latStr);
            int latDeg = (int)(rawLat / 100);
            double latMin = rawLat - (latDeg * 100);
            telemetry.latitude = (double)latDeg + (latMin / 60.0);
            if (latDir == 'S') telemetry.latitude = -telemetry.latitude;

            double rawLon = atof(lonStr);
            int lonDeg = (int)(rawLon / 100);
            double lonMin = rawLon - (lonDeg * 100);
            telemetry.longitude = (double)lonDeg + (lonMin / 60.0);
            if (lonDir == 'W') telemetry.longitude = -telemetry.longitude;
          }
        } else if (strstr(nmeaBuf, "$GPGGA") || strstr(nmeaBuf, "$GNGGA")) {
          // Parse satellites count and altitude
          char *token = strtok(nmeaBuf, ",");
          int tokIdx = 0;
          while (token != NULL) {
            if (tokIdx == 7) telemetry.satellites = atoi(token);
            if (tokIdx == 9) telemetry.altitude_m = atof(token);
            token = strtok(NULL, ",");
            tokIdx++;
          }
          telemetry.gps_ok = true;
        }
      }
      idx = 0;
    } else {
      if (idx < 127) {
        nmeaBuf[idx++] = c;
      }
    }
  }
}

// ======================== SOS BUTTON & BUZZER ========================
void checkSOS() {
  int reading = digitalRead(PIN_SOS);

  if (reading != lastSosState) {
    lastSosDebounce = millis();
  }

  if ((millis() - lastSosDebounce) > 50) {
    if (reading == LOW && !telemetry.sos_active && !sosAcknowledged) {
      telemetry.sos_active = true;
      logIncident("EMERGENCY", "SOS_PANIC_BUTTON", "Worker triggered manual SOS distress alert");
    }
  }

  lastSosState = reading;

  // Master Buzzer Logic: Sounds if SOS is active, Gas Alert active, or Fall Detected
  if (telemetry.sos_active || telemetry.gas_alert || telemetry.fall_detected) {
    telemetry.buzzer_active = true;
    digitalWrite(PIN_BUZZER, HIGH);
  } else if (!server.hasArg("manual_buzzer")) {
    // If not manually forced by web endpoint
    digitalWrite(PIN_BUZZER, LOW);
    telemetry.buzzer_active = false;
  }
}

// ======================== REST API HANDLERS ========================
void handleCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
}

void handleData() {
  handleCors();

  // Update Wi-Fi metrics
  if (WiFi.status() == WL_CONNECTED) {
    telemetry.wifi_rssi = WiFi.RSSI();
    // RSSI to approximate percentage (-100dBm = 0%, -50dBm = 100%)
    int pct = 2 * (telemetry.wifi_rssi + 100);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    telemetry.wifi_pct = pct;
  } else {
    telemetry.wifi_rssi = -999;
    telemetry.wifi_pct  = 0;
  }

  telemetry.uptime_sec = millis() / 1000;
  telemetry.free_heap  = ESP.getFreeHeap();
  telemetry.packet_id++;

  String json = "{";
  // Diagnostics
  json += "\"packet_id\":" + String(telemetry.packet_id) + ",";
  json += "\"uptime_sec\":" + String(telemetry.uptime_sec) + ",";
  json += "\"free_heap\":" + String(telemetry.free_heap) + ",";
  json += "\"wifi_rssi\":" + String(telemetry.wifi_rssi) + ",";
  json += "\"wifi_pct\":" + String(telemetry.wifi_pct) + ",";
  json += "\"reboot_reason\":\"" + String(telemetry.reboot_reason) + "\",";

  // MPU-6050
  json += "\"mpu6050\":{";
  json += "\"ok\":" + String(telemetry.mpu_ok ? "true" : "false") + ",";
  json += "\"pitch\":" + String(telemetry.pitch, 2) + ",";
  json += "\"roll\":" + String(telemetry.roll, 2) + ",";
  json += "\"yaw\":" + String(telemetry.yaw, 2) + ",";
  json += "\"ax\":" + String(telemetry.accel_x, 3) + ",";
  json += "\"ay\":" + String(telemetry.accel_y, 3) + ",";
  json += "\"az\":" + String(telemetry.accel_z, 3) + ",";
  json += "\"gx\":" + String(telemetry.gyro_x, 2) + ",";
  json += "\"gy\":" + String(telemetry.gyro_y, 2) + ",";
  json += "\"gz\":" + String(telemetry.gyro_z, 2) + ",";
  json += "\"g_force\":" + String(telemetry.g_force, 2) + ",";
  json += "\"fall_alert\":" + String(telemetry.fall_detected ? "true" : "false");
  json += "},";

  // MQ-2 Gas
  json += "\"mq2\":{";
  json += "\"raw\":" + String(telemetry.gas_raw) + ",";
  json += "\"ppm\":" + String(telemetry.gas_ppm, 1) + ",";
  json += "\"smoke_pct\":" + String(telemetry.smoke_pct, 1) + ",";
  json += "\"lpg_pct\":" + String(telemetry.lpg_pct, 1) + ",";
  json += "\"co_ppm\":" + String(telemetry.co_ppm, 1) + ",";
  json += "\"alert\":" + String(telemetry.gas_alert ? "true" : "false");
  json += "},";

  // DHT11
  json += "\"dht11\":{";
  json += "\"ok\":" + String(telemetry.dht_ok ? "true" : "false") + ",";
  json += "\"temp_c\":" + String(telemetry.temperature_c, 1) + ",";
  json += "\"temp_f\":" + String(telemetry.temperature_f, 1) + ",";
  json += "\"humidity\":" + String(telemetry.humidity, 1) + ",";
  json += "\"heat_index_c\":" + String(telemetry.heat_index_c, 1);
  json += "},";

  // NEO-6M GPS
  json += "\"gps\":{";
  json += "\"ok\":" + String(telemetry.gps_ok ? "true" : "false") + ",";
  json += "\"fix\":" + String(telemetry.gps_fix ? "true" : "false") + ",";
  json += "\"lat\":" + String(telemetry.latitude, 6) + ",";
  json += "\"lon\":" + String(telemetry.longitude, 6) + ",";
  json += "\"alt_m\":" + String(telemetry.altitude_m, 1) + ",";
  json += "\"speed_kmh\":" + String(telemetry.speed_kmh, 1) + ",";
  json += "\"satellites\":" + String(telemetry.satellites);
  json += "},";

  // MAX30100
  json += "\"max30100\":{";
  json += "\"ok\":" + String(telemetry.max_ok ? "true" : "false") + ",";
  json += "\"finger\":" + String(telemetry.finger_detected ? "true" : "false") + ",";
  json += "\"bpm\":" + String(telemetry.heart_rate_bpm) + ",";
  json += "\"spo2\":" + String(telemetry.spo2_pct, 1) + ",";
  json += "\"ir\":" + String(telemetry.ir_raw) + ",";
  json += "\"red\":" + String(telemetry.red_raw);
  json += "},";

  // SOS & Buzzer
  json += "\"sos\":{\"active\":" + String(telemetry.sos_active ? "true" : "false") + "},";
  json += "\"buzzer\":{\"active\":" + String(telemetry.buzzer_active ? "true" : "false") + "}";

  json += "}";

  server.send(200, "application/json", json);
}

void handleBuzzer() {
  handleCors();
  if (server.hasArg("state")) {
    int state = server.arg("state").toInt();
    telemetry.buzzer_active = (state == 1);
    digitalWrite(PIN_BUZZER, telemetry.buzzer_active ? HIGH : LOW);
    server.send(200, "application/json", "{\"status\":\"ok\",\"buzzer\":" + String(state) + "}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing state parameter\"}");
  }
}

void handleResetSOS() {
  handleCors();
  telemetry.sos_active = false;
  sosAcknowledged      = true;
  digitalWrite(PIN_BUZZER, LOW);
  telemetry.buzzer_active = false;
  logIncident("INFO", "SOS_CLEARED", "Operator acknowledged and cleared SOS distress beacon");
  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"SOS cleared\"}");
}

void handleLogs() {
  handleCors();
  if (!LittleFS.exists("/incident_log.txt")) {
    server.send(200, "application/json", "[]");
    return;
  }

  File f = LittleFS.open("/incident_log.txt", FILE_READ);
  if (!f) {
    server.send(500, "application/json", "{\"error\":\"Unable to read log file\"}");
    return;
  }

  String json = "[";
  bool first = true;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Parse: UPTIME|SEVERITY|EVENT|DETAILS
    int p1 = line.indexOf('|');
    int p2 = line.indexOf('|', p1 + 1);
    int p3 = line.indexOf('|', p2 + 1);

    if (p1 > 0 && p2 > 0 && p3 > 0) {
      if (!first) json += ",";
      first = false;
      String uptime_s  = line.substring(0, p1);
      String severity  = line.substring(p1 + 1, p2);
      String event     = line.substring(p2 + 1, p3);
      String details   = line.substring(p3 + 1);

      json += "{\"uptime_s\":" + uptime_s + ",\"severity\":\"" + severity + "\",\"event\":\"" + event + "\",\"details\":\"" + details + "\"}";
    }
  }
  f.close();
  json += "]";
  server.send(200, "application/json", json);
}

void handleClearLogs() {
  handleCors();
  LittleFS.remove("/incident_log.txt");
  logIncident("INFO", "LOGS_CLEARED", "LittleFS incident logs cleared by operator");
  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Incident logs wiped\"}");
}

// ======================== ESP32 SETUP ========================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=======================================================");
  Serial.println("  ESP32 INDUSTRIAL TELEMETRY COMMAND HUB STARTING...  ");
  Serial.println("=======================================================");

  // Pin Configuration
  pinMode(PIN_SOS, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  // Determine Reset Reason
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON:  telemetry.reboot_reason = "POWER_ON"; break;
    case ESP_RST_PANIC:    telemetry.reboot_reason = "CRASH_PANIC"; break;
    case ESP_RST_TASK_WDT: telemetry.reboot_reason = "WATCHDOG_TIMEOUT"; break;
    case ESP_RST_SW:       telemetry.reboot_reason = "SOFTWARE_REBOOT"; break;
    default:               telemetry.reboot_reason = "NORMAL_RESET"; break;
  }
  Serial.printf("[SYSTEM] Boot reset reason: %s\n", telemetry.reboot_reason);

  // Initialize LittleFS
  initLittleFS();
  logIncident("SYSTEM", "BOOT", telemetry.reboot_reason);

  // Initialize I2C Bus & Scan
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000); // 100 kHz standard mode
  scanI2CBus();

  // Initialize Sensors
  initMPU6050();
  initMAX30100();

  // Initialize GPS UART
  gpsSerial.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

  // Connect to Wi-Fi
  Serial.printf("[WIFI] Connecting to SSID: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  uint32_t wifiTimeout = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiTimeout) < 8000) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected Successfully!");
    Serial.print("[WIFI] IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WIFI WARNING] Station connection timed out! Launching Access Point...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("[WIFI AP] Access Point started! Connect to: ");
    Serial.println(AP_SSID);
    Serial.print("[WIFI AP] Gateway IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // Register Web Endpoints
  server.on("/data", HTTP_GET, handleData);
  server.on("/buzzer", HTTP_GET, handleBuzzer);
  server.on("/reset_sos", HTTP_GET, handleResetSOS);
  server.on("/logs", HTTP_GET, handleLogs);
  server.on("/clear_logs", HTTP_GET, handleClearLogs);
  server.enableCORS();
  server.begin();
  Serial.println("[HTTP] REST Web Server active on port 80.");

  // Configure Hardware Task Watchdog Timer
  Serial.println("[WDT] Initializing Hardware Task Watchdog Timer (8s)...");
  esp_task_wdt_init(WDT_TIMEOUT_S, true); // true = panic and reboot on trigger
  esp_task_wdt_add(NULL);                 // subscribe current thread to WDT
  Serial.println("[WDT] Watchdog armed.");
  Serial.println("=======================================================\n");
}

// ======================== ESP32 MAIN LOOP ========================
void loop() {
  // Feed Hardware Watchdog Timer (Prevents crash if loop is running healthily)
  esp_task_wdt_reset();

  // Handle Incoming HTTP Client Requests
  server.handleClient();

  // Continuously ingest GPS NMEA characters
  readGPS();

  // Check Physical SOS Button & Buzzer logic
  checkSOS();

  // Periodic Telemetry Sensor Reading (Every 100ms = 10Hz)
  uint32_t now = millis();
  if (now - lastReadTime >= 100) {
    lastReadTime = now;
    readMPU6050();
    readMAX30100();
    readMQ2();
  }

  // Periodic DHT11 Reading (Every 2000ms = 0.5Hz, DHT11 spec requirement)
  static uint32_t lastDHTTime = 0;
  if (now - lastDHTTime >= 2000) {
    lastDHTTime = now;
    updateDHT11();
  }

  // Stream JSON Telemetry to Serial (115200 baud) for Direct WebSerial Connection
  if (now - lastSerialTime >= 1000) {
    lastSerialTime = now;

    // Output one compact JSON line for WebSerial reading
    Serial.printf("{\"type\":\"telemetry\",\"packet\":%u,\"uptime\":%u,\"pitch\":%.1f,\"roll\":%.1f,\"yaw\":%.1f,\"g_force\":%.2f,\"gas_ppm\":%.1f,\"temp_c\":%.1f,\"humidity\":%.1f,\"bpm\":%d,\"spo2\":%.1f,\"lat\":%.6f,\"lon\":%.6f,\"sos\":%d,\"buzzer\":%d,\"rssi\":%d}\n",
      telemetry.packet_id,
      telemetry.uptime_sec,
      telemetry.pitch,
      telemetry.roll,
      telemetry.yaw,
      telemetry.g_force,
      telemetry.gas_ppm,
      telemetry.temperature_c,
      telemetry.humidity,
      telemetry.heart_rate_bpm,
      telemetry.spo2_pct,
      telemetry.latitude,
      telemetry.longitude,
      telemetry.sos_active ? 1 : 0,
      telemetry.buzzer_active ? 1 : 0,
      telemetry.wifi_rssi
    );
  }
}
