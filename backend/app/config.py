"""Runtime configuration via environment variables."""
import os

DATABASE_URL = os.getenv("DATABASE_URL", "sqlite:///./edge.db")
MQTT_HOST = os.getenv("MQTT_HOST", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "volmax/+/telemetry")
ANOMALY_WINDOW = int(os.getenv("ANOMALY_WINDOW", "120"))
ANOMALY_THRESHOLD = float(os.getenv("ANOMALY_THRESHOLD", "4.0"))
OTA_DIR = os.getenv("OTA_DIR", "/data/ota")
