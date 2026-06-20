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
    db = SessionLocal()
    try:
        payload = json.loads(msg.payload.decode())
        device_id = msg.topic.split("/")[1]
        topic_type = msg.topic.split("/")[2]
        
        if topic_type == "alerts":
            from .db import Event
            z = payload.get("z_score", 0.0)
            alert_type = payload.get("alert_type", "thd_drift")
            if alert_type == "irms_drift":
                detail = f"Edge detected RMS current drift! load={payload.get('irms_a')}A"
            else:
                detail = f"Edge detected THD drift! THD={payload.get('thd_pct')}% at load={payload.get('irms_a')}A"
            event = Event(
                device_id=device_id,
                kind="anomaly",
                detail=detail,
                zscore=float(z)
            )
            db.add(event)
            db.commit()
            log.info("EVENT anomaly %s: %s", device_id, event.detail)
            return

        payload.setdefault("device_id", device_id)
        data = TelemetryIn(**payload)
        events = process_telemetry(db, data)
        for e in events:
            log.info("EVENT %s %s: %s", e.kind, e.device_id, e.detail)
    except (ValueError, ValidationError, IndexError, json.JSONDecodeError) as exc:
        log.warning("rejected payload on %s: %s", msg.topic, exc)
    finally:
        db.close()


def start_mqtt_thread() -> threading.Thread:
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_message = _on_message
    client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.subscribe("volmax/+/telemetry", qos=1)
    client.subscribe("volmax/+/alerts", qos=1)
    t = threading.Thread(target=client.loop_forever, daemon=True)
    t.start()
    log.info("MQTT subscribed to telemetry and alerts @ %s:%s", MQTT_HOST, MQTT_PORT)
    return t
