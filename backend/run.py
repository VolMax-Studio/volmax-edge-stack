"""Entry point: API + MQTT subscriber in one process."""
import logging

import uvicorn

from app.db import init_db
from app.main import app
from app.mqtt_ingest import start_mqtt_thread

logging.basicConfig(level=logging.INFO)

if __name__ == "__main__":
    init_db()
    try:
        start_mqtt_thread()
    except Exception as exc:  # broker may not be up yet in dev
        logging.warning("MQTT not connected (%s) — API only mode", exc)
    uvicorn.run(app, host="0.0.0.0", port=8000)
