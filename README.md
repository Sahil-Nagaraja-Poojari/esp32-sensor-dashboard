# ESP32 Industrial Multi-Sensor Telemetry & Safety Command Hub

An industrial-grade telemetry and worker safety monitoring platform built for the **ESP32** microcontroller, featuring **Copperplate Gothic Bold** command-center typography, real-time acoustic/visual alarm sirens, instant notification logging, Leaflet.js GPS mapping, 3D artificial horizon attitude estimation, photoplethysmogram (PPG) vitals oscilloscope, and direct USB WebSerial & Wi-Fi REST connectivity.

---

## 📋 Hardware Bill of Materials (BOM) & Pinout

| Peripheral | Component Model | ESP32 Pin Assignment | Interface / Protocol | Primary Telemetry Metrics |
| :--- | :--- | :--- | :--- | :--- |
| **01. Gyroscope & Accel** | **MPU-6050** | `SDA -> GPIO 21`<br>`SCL -> GPIO 22` | I2C (Address `0x68`) | Pitch, Roll, Yaw, $X/Y/Z$ G-force, Fall Impact |
| **02. Toxic Gas Sensor** | **MQ-2** | `Analog Pin -> GPIO 34` | ADC1 CH6 (Analog) | Toxic Gas (PPM), Smoke %, LPG %, CO (PPM) |
| **03. Environment** | **DHT11** | `Data Pin -> GPIO 4` | 1-Wire Digital (Pull-up) | Temperature (°C/°F), Humidity (% RH), Heat Index |
| **04. GPS Receiver** | **NEO-6M** | `TX -> GPIO 16 (RX2)`<br>`RX -> GPIO 17 (TX2)` | HardwareSerial 2 (9600 Baud) | Latitude, Longitude, Altitude, Speed, Satellites |
| **05. Distress Panic Button** | **SOS Button** | `Pin -> GPIO 15`<br>`Other Pin -> GND` | GPIO (`INPUT_PULLUP`) | Manual Emergency Beacon State (Active / Idle) |
| **06. Pulse Oximeter** | **MAX30100** | `SDA -> GPIO 21`<br>`SCL -> GPIO 22` | I2C (Address `0x57`) | Heart Rate (BPM), Blood Oxygen (SpO2 %), PPG Trace |
| **07. Audible Alarm** | **Piezo Buzzer** | `Positive -> GPIO 18`<br>`Negative -> GND` | Digital Output | Hardware Siren (Active HIGH) & Remote Control |

---

## 🛡️ Industrial Production Safeguards

1. **Boot-Time I2C Bus Scanner & Collision Safeguard**:
   - The ESP32 scans the I2C bus at startup (`GPIO 21/22`).
   - Verifies communication with **MPU-6050** (`0x68`) and **MAX30100** (`0x57`).
   - If either device fails to respond or suffers wiring disconnects, it is flagged offline in telemetry and logged over Serial without locking up the boot sequence.

2. **Non-Volatile Event Logging (LittleFS)**:
   - Built-in flash memory partition stores critical incidents (`/incident_log.txt`) that survive browser refreshes and power resets.
   - Triggers on: SOS button press, gas spikes (>800 PPM), worker fall shock (>2.85G), hypoxia (<90% SpO2), and watchdog reboots.
   - REST endpoints: `/logs` (fetch JSON incident history) and `/clear_logs` (wipe flash storage).

3. **Link Quality (RSSI) & Heartbeat Telemetry**:
   - Transmits live Wi-Fi signal strength (`WiFi.RSSI()`), link quality percentage, and packet sequence ID.
   - The dashboard displays a 4-bar RSSI meter, heartbeat pulse LED, and a "Last Seen" chronometer with auto-stale warnings if data freezes for >3 seconds.

4. **Hardware Task Watchdog Timer (WDT)**:
   - Configured via `esp_task_wdt` with an 8-second timeout.
   - Resets automatically during normal loop execution; gracefully reboots the microcontroller if an I2C or Serial communication lockup occurs.

---

## 🚀 Quick Start Guide

### Option A: Local Testing & Simulation (Zero Hardware Needed)

1. Run the local Python server:
   ```bash
   python server.py
   ```
2. Open your web browser at:
   ```
   http://localhost:8000
   ```
3. Use the **Industrial Fault Injection Console** at the bottom of the screen to simulate:
   - Toxic Gas Leak (>900 PPM)
   - Worker Fall Impact Shock (3.25 G)
   - Tachycardia & Hypoxia
   - Manual SOS Panic Beacon

### Option B: Direct Browser USB WebSerial (No Server Needed!)

1. Connect your ESP32 to your PC using a USB cable.
2. In Google Chrome or Microsoft Edge, open `index.html`.
3. In the header **DATA SOURCE** dropdown, select **USB WEBSERIAL (115200 BAUD)**.
4. Click **CONNECT USB**, choose your ESP32 COM port from the prompt, and click **Connect**.
5. Telemetry streams directly from the ESP32 into your browser!

### Option C: Direct Wi-Fi REST Mode

1. In `esp32_firmware/esp32_firmware.ino`, configure your Wi-Fi credentials:
   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_SSID";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   ```
2. Upload the sketch using Arduino IDE (ensure you have installed the ESP32 Board package by Espressif).
3. Open Serial Monitor (115200 baud) to find the ESP32 IP address (e.g. `192.168.1.50`).
4. On the dashboard, switch **DATA SOURCE** to **ESP32 WI-FI REST API**, type the IP address, and watch the telemetry synchronize in real time.

---

## 🔊 Real-Time Alarm & Audio System

- **Web Audio API Acoustic Synthesizer**: The dashboard features an integrated dual-oscillator acoustic siren (880 Hz / 1760 Hz) that plays an authentic piezo alarm sound right through your computer speakers when an alarm occurs.
- **Audio Master Mute**: Toggle sound ON or MUTED anytime using the top header button.
- **Bi-Directional Buzzer Control**: Click **SOUND BUZZER** or **SILENCE** on the Buzzer card to remotely trigger or mute the physical ESP32 buzzer over the network.
