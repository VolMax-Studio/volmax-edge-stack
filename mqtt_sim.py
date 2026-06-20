#!/usr/bin/env mode-python
import sys
import time
import json
import random
import threading
import httpx
import paho.mqtt.client as mqtt

# Konfiguracija
MQTT_HOST = "localhost"
MQTT_PORT = 1883
BACKEND_URL = "http://localhost:8000"
DEVICE_ID = "esp32-sim-01"

# Welford statistika (identična firmware logici)
class DriftDetector:
    def __init__(self, threshold=3.0, min_samples=10):
        self.threshold = threshold
        self.min_samples = min_samples
        self.n = 0
        self.mean = 0.0
        self.M2 = 0.0
        self.std_dev = 0.0
        self.is_locked = False

    def process(self, x, learn):
        if learn and not self.is_locked:
            self.n += 1
            delta = x - self.mean
            self.mean += delta / self.n
            self.M2 += delta * (x - self.mean)
            if self.n > 1:
                self.std_dev = (self.M2 / (self.n - 1)) ** 0.5

        if self.is_locked:
            if self.std_dev > 0.0001:
                return abs(x - self.mean) / self.std_dev
            return 0.0
        return -1.0

class BinnedDriftDetector:
    def __init__(self, threshold=3.0, min_samples=10):
        self.threshold = threshold
        self.min_samples = min_samples
        self.bin_edges = [1.0, 5.0, 10.0, 20.0]
        self.bins = [DriftDetector(threshold, min_samples) for _ in range(5)]

    def get_bin(self, irms):
        for i, edge in enumerate(self.bin_edges):
            if irms < edge:
                return i
        return 4

    def process(self, irms, thd, learn):
        if irms < self.bin_edges[0]:
            return -1.0
        idx = self.get_bin(irms)
        return self.bins[idx].process(thd, learn)

    def lock_all(self):
        for b in self.bins:
            if b.n >= b.min_samples:
                b.is_locked = True

    def reset_all(self):
        for b in self.bins:
            b.n = 0
            b.mean = 0.0
            b.M2 = 0.0
            b.std_dev = 0.0
            b.is_locked = False

# Globalne instance
thd_detector = BinnedDriftDetector(threshold=3.0, min_samples=10)
irms_detector = DriftDetector(threshold=3.0, min_samples=15)
learning_mode = True
anomaly_type = "none"  # none | thd | irms

def poll_device_config():
    global learning_mode
    try:
        r = httpx.get(f"{BACKEND_URL}/devices/{DEVICE_ID}")
        if r.status_code == 200:
            config = r.json()
            learn = config.get("learning_mode", True)
            if learn != learning_mode:
                learning_mode = learn
                if not learn:
                    thd_detector.lock_all()
                    irms_detector.is_locked = True
                    print("\n[Edge Sim] -> LOCK baseline! Prelazak u MONITORING režim.")
                else:
                    thd_detector.reset_all()
                    irms_detector.n = 0
                    irms_detector.is_locked = False
                    print("\n[Edge Sim] -> RESET baseline! Početak novog učenja (LEARNING).")
    except Exception as e:
        pass

def listen_keyboard():
    global anomaly_type
    print("\nInteraktivne komande za simulaciju anomalija:")
    print("  Pritisni 't' + Enter -> simuliraj THD% kvar (magnetizacijska struja)")
    print("  Pritisni 'r' + Enter -> simuliraj RMS strujni kvar (curenje namotaja)")
    print("  Pritisni 'n' + Enter -> vrati u normalno stanje")
    print("-" * 50)
    
    while True:
        try:
            line = sys.stdin.readline().strip().lower()
            if line == 't':
                anomaly_type = "thd"
                print("\n[KOMANDA] Injektujem THD kvar (THD skače na 18%)...")
            elif line == 'r':
                anomaly_type = "irms"
                print("\n[KOMANDA] Injektujem RMS strujni kvar (Struja skače sa 3A na 9A)...")
            elif line == 'n':
                anomaly_type = "none"
                print("\n[KOMANDA] Vraćam parametre u normalne opsege...")
        except Exception:
            break

def main():
    global anomaly_type
    
    # Inicijalizacija MQTT klijenta
    client = mqtt.Client()
    try:
        client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
        client.loop_start()
    except Exception as e:
        print(f"Greška pri povezivanju na MQTT broker: {e}")
        print("Da li je pokrenut Docker compose?")
        sys.exit(1)

    print(f"Simuliram ESP32-S3 IoT uređaj: '{DEVICE_ID}'")
    print(f"Šaljem telemetriju na topic 'volmax/{DEVICE_ID}/telemetry'...")
    
    # Pokretanje keyboard listener-a u pozadinskom thread-u
    t = threading.Thread(target=listen_keyboard, daemon=True)
    t.start()

    step = 0
    while True:
        poll_device_config()
        
        # Generisanje podataka ovisno o tipu anomalije
        if anomaly_type == "none":
            irms = random.normalvariate(3.2, 0.1)  # Normalna struja ~ 3.2A (Bin 1)
            thd = random.normalvariate(4.0, 0.2)   # Normalan THD ~ 4.0%
        elif anomaly_type == "thd":
            irms = random.normalvariate(3.2, 0.1)  # Struja normalna
            thd = random.normalvariate(18.0, 0.5)  # THD abnormalno visok
        elif anomaly_type == "irms":
            irms = random.normalvariate(9.0, 0.2)  # Struja visoka (Bin 2)
            thd = random.normalvariate(5.0, 0.2)   # THD normalan za taj režim

        # Proračun Z-score-a na čipu (firmware logika)
        z_score = thd_detector.process(irms, thd, learning_mode)
        irms_z = irms_detector.process(irms, learning_mode)

        # Telemetrijski paket
        telemetry = {
            "device_id": DEVICE_ID,
            "irms_a": round(irms, 3),
            "thd_pct": round(thd, 1),
            "p_est_w": round(irms * 230, 0),
            "z_score": round(z_score, 2) if z_score >= 0.0 else None,
            "irms_z": round(irms_z, 2) if irms_z >= 0.0 else None,
            "learn_status": "learning" if learning_mode else ("monitoring" if (z_score >= 0.0 or irms_z >= 0.0) else "uncharacterized"),
            "fw_version": "1.0.5",
            "source": "ct"
        }

        # Slanje telemetrije
        client.publish(f"volmax/{DEVICE_ID}/telemetry", json.dumps(telemetry))
        
        # Prikaz stanja na konzoli
        status_line = (f"Uzorak #{step:03d} | Mod: {telemetry['learn_status']:15s} | "
                       f"Irms: {irms:.2f}A (z={telemetry['irms_z']}) | "
                       f"THD: {thd:.1f}% (z={telemetry['z_score']})")
        print(status_line)

        # Detekcija alarma i slanje na alerts topic
        is_anomaly = (z_score > 3.0) and not learning_mode and (z_score >= 0.0)
        is_irms_anomaly = (irms_z > 3.0) and not learning_mode and (irms_z >= 0.0)

        if is_anomaly or is_irms_anomaly:
            alert = {
                "device_id": DEVICE_ID,
                "irms_a": round(irms, 3),
                "thd_pct": round(thd, 1),
                "z_score": round(z_score if is_anomaly else irms_z, 2),
                "alert_type": "thd_drift" if is_anomaly else "irms_drift"
            }
            client.publish(f"volmax/{DEVICE_ID}/alerts", json.dumps(alert))
            print(f"  >>> [ALERT POSLAT] Tip: {alert['alert_type']} | Z: {alert['z_score']}")

        step += 1
        time.sleep(3)

if __name__ == "__main__":
    main()
