#!/usr/bin/env python3
"""
ESP32 Telemetry Command Hub - Local Test & Host Server
Serves static dashboard files and provides simulated /data, /logs, /buzzer endpoints.
"""

import http.server
import socketserver
import json
import time
import math
import random
import os
import sys

PORT = 8000
DIRECTORY = os.path.dirname(os.path.abspath(__file__))

# Simulated Telemetry State
state = {
    "packet_id": 100,
    "uptime_sec": 0,
    "free_heap": 218450,
    "wifi_rssi": -58,
    "wifi_pct": 84,
    "reboot_reason": "POWER_ON",
    "mpu6050": {
        "ok": True,
        "pitch": 0.0,
        "roll": 0.0,
        "yaw": 0.0,
        "ax": 0.0,
        "ay": 0.0,
        "az": 1.0,
        "gx": 0.0,
        "gy": 0.0,
        "gz": 0.0,
        "g_force": 1.0,
        "fall_alert": False
    },
    "mq2": {
        "raw": 320,
        "ppm": 240.5,
        "smoke_pct": 9.2,
        "lpg_pct": 12.0,
        "co_ppm": 108.2,
        "alert": False
    },
    "dht11": {
        "ok": True,
        "temp_c": 28.5,
        "temp_f": 83.3,
        "humidity": 62.0,
        "heat_index_c": 30.1
    },
    "gps": {
        "ok": True,
        "fix": True,
        "lat": 19.076090,
        "lon": 72.877426,
        "alt_m": 42.5,
        "speed_kmh": 2.4,
        "satellites": 9
    },
    "max30100": {
        "ok": True,
        "finger": True,
        "bpm": 75,
        "spo2": 98.6,
        "ir": 18200,
        "red": 14200
    },
    "sos": {"active": False},
    "buzzer": {"active": False}
}

incident_logs = [
    {"uptime_s": 1, "severity": "SYSTEM", "event": "BOOT", "details": "POWER_ON (Watchdog 8s Armed, LittleFS Mounted)"},
    {"uptime_s": 3, "severity": "INFO", "event": "I2C_SCAN", "details": "Found MPU-6050 (0x68) and MAX30100 (0x57)"},
    {"uptime_s": 24, "severity": "INFO", "event": "WIFI_LINK", "details": "Connected to Wi-Fi RSSI -58 dBm"}
]

class IndustrialServerHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

    def end_headers(self):
        # Enable CORS
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.end_headers()

    def do_GET(self):
        path = self.path.split("?")[0]

        if path == "/data":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()

            # Dynamic updates
            t = time.time()
            state["packet_id"] += 1
            state["uptime_sec"] += 1
            state["mpu6050"]["pitch"] = round(math.sin(t * 0.8) * 5.0, 2)
            state["mpu6050"]["roll"]  = round(math.cos(t * 0.6) * 4.0, 2)
            state["mq2"]["ppm"] = round(230.0 + math.sin(t * 0.3) * 20.0 + random.uniform(-2, 2), 1)
            state["max30100"]["bpm"] = 72 + int(math.sin(t * 0.2) * 5)
            state["gps"]["lat"] += (random.random() - 0.5) * 0.00002
            state["gps"]["lon"] += (random.random() - 0.5) * 0.00002

            self.wfile.write(json.dumps(state).encode("utf-8"))
            return

        elif path == "/buzzer":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            if "state=1" in self.path:
                state["buzzer"]["active"] = True
            else:
                state["buzzer"]["active"] = False
            self.wfile.write(json.dumps({"status": "ok", "buzzer": state["buzzer"]["active"]}).encode("utf-8"))
            return

        elif path == "/reset_sos":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            state["sos"]["active"] = False
            state["buzzer"]["active"] = False
            incident_logs.append({
                "uptime_s": state["uptime_sec"],
                "severity": "INFO",
                "event": "SOS_CLEARED",
                "details": "Operator cleared SOS distress"
            })
            self.wfile.write(json.dumps({"status": "ok", "message": "SOS cleared"}).encode("utf-8"))
            return

        elif path == "/logs":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(incident_logs).encode("utf-8"))
            return

        elif path == "/clear_logs":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            incident_logs.clear()
            self.wfile.write(json.dumps({"status": "ok", "message": "Logs cleared"}).encode("utf-8"))
            return

        # Default static file handler
        return super().do_GET()

def run_server():
    print("=" * 65)
    print("  ESP32 INDUSTRIAL TELEMETRY COMMAND HUB - LOCAL SERVER")
    print("=" * 65)
    print(f"[*] Serving Directory: {DIRECTORY}")
    print(f"[*] Dashboard URL:     http://localhost:{PORT}")
    print(f"[*] Telemetry API:     http://localhost:{PORT}/data")
    print(f"[*] LittleFS Logs API: http://localhost:{PORT}/logs")
    print("=" * 65)
    print("[*] Press Ctrl+C to terminate.")

    with socketserver.TCPServer(("", PORT), IndustrialServerHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[*] Server shutdown cleanly.")

if __name__ == "__main__":
    run_server()
