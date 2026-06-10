"""MQTT subscriber: volmax/<device_id>/telemetry -> ingest pipeline.

Runs as a thread next to the API (started from run.py).
Device polls GET /devices/<id> for pending_ota_url and config —
simple pull model keeps the firmware logic trivial and robust.
"""
import json
import logging
import threading

import paho.mqtt.client as mqtt
from pydantic import ValidationError

from .config import MQTT_HOST, MQTT_PORT, MQTT_TOPIC
from .db import SessionLocal
from .ingest import process_telemetry
from .schemas import TelemetryIn

log = logging.getLogger("mqtt_ingest")


def _on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        device_id = msg.topic.split("/")[1]
        payload.setdefault("device_id", device_id)
        data = TelemetryIn(**payload)
    except (ValueError, ValidationError, IndexError) as exc:
        log.warning("rejected payload on %s: %s", msg.topic, exc)
        return
    db = SessionLocal()
    try:
        events = process_telemetry(db, data)
        for e in events:
            log.info("EVENT %s %s: %s", e.kind, e.device_id, e.detail)
    finally:
        db.close()


def start_mqtt_thread() -> threading.Thread:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message = _on_message
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.subscribe(MQTT_TOPIC, qos=1)
    t = threading.Thread(target=client.loop_forever, daemon=True)
    t.start()
    log.info("MQTT subscribed to %s @ %s:%s", MQTT_TOPIC, MQTT_HOST, MQTT_PORT)
    return t
