"""Telemetry ingest pipeline — shared by MQTT subscriber and REST ingest.

Validates payload, upserts device, stores telemetry, runs anomaly
detection and threshold alarms, emits events.
"""
from sqlalchemy.orm import Session

from .anomaly import RollingZScore
from .config import ANOMALY_THRESHOLD, ANOMALY_WINDOW
from .db import Device, Event, Telemetry, utcnow
from .schemas import TelemetryIn

_detectors: dict[str, RollingZScore] = {}


def get_detector(device_id: str) -> RollingZScore:
    if device_id not in _detectors:
        _detectors[device_id] = RollingZScore(ANOMALY_WINDOW, ANOMALY_THRESHOLD)
    return _detectors[device_id]


def reset_detectors() -> None:
    _detectors.clear()


def process_telemetry(db: Session, data: TelemetryIn) -> list[Event]:
    """Store one telemetry sample; return any events raised."""
    device = db.get(Device, data.device_id)
    if device is None:
        device = Device(id=data.device_id, name=data.device_id)
        db.add(device)
        db.flush()  # apply column defaults before reading them below
    device.last_seen = utcnow()
    device.fw_version = data.fw_version

    db.add(Telemetry(device_id=data.device_id, irms_a=data.irms_a,
                     thd_pct=data.thd_pct, p_est_w=data.p_est_w,
                     z_score=data.z_score, learn_status=data.learn_status,
                     source=data.source))

    events: list[Event] = []
    res = get_detector(data.device_id).update(data.irms_a)
    if res.is_anomaly:
        z = max(min(res.zscore, 999.0), -999.0)  # clamp inf for JSON/DB
        events.append(Event(device_id=data.device_id, kind="anomaly",
                            detail=f"Irms {data.irms_a:.2f} A deviates from "
                                   f"rolling mean {res.mean:.2f} A",
                            zscore=round(z, 2)))
    if data.irms_a > device.rms_alarm_threshold_a:
        events.append(Event(device_id=data.device_id, kind="threshold",
                            detail=f"Irms {data.irms_a:.2f} A > limit "
                                   f"{device.rms_alarm_threshold_a:.2f} A"))
    for e in events:
        db.add(e)
    db.commit()
    return events
