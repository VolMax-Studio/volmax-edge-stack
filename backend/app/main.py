"""FastAPI application — fleet REST API."""
from datetime import timedelta

from fastapi import Depends, FastAPI, HTTPException
from sqlalchemy import select
from sqlalchemy.orm import Session

from .db import Device, Event, SessionLocal, Telemetry, init_db, utcnow
from .ingest import process_telemetry
from .schemas import ConfigUpdate, DeviceOut, OtaRequest, TelemetryIn

ONLINE_WINDOW_S = 30

app = FastAPI(title="VolMax Edge Stack", version="1.0.0")


@app.on_event("startup")
def _startup() -> None:
    init_db()


def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()


def _device_out(d: Device) -> DeviceOut:
    last = d.last_seen
    now = utcnow()
    if last.tzinfo is None:
        last = last.replace(tzinfo=now.tzinfo)
    online = (now - last) < timedelta(seconds=ONLINE_WINDOW_S)
    return DeviceOut(id=d.id, name=d.name, fw_version=d.fw_version,
                     last_seen=d.last_seen, telemetry_interval_s=d.telemetry_interval_s,
                     rms_alarm_threshold_a=d.rms_alarm_threshold_a,
                     pending_ota_url=d.pending_ota_url, learning_mode=d.learning_mode,
                     bin_edges=d.bin_edges,
                     online=online)


@app.get("/health")
def health():
    return {"status": "ok"}


@app.post("/ingest", status_code=201)
def ingest(data: TelemetryIn, db: Session = Depends(get_db)):
    events = process_telemetry(db, data)
    return {"stored": True, "events_raised": len(events)}


@app.get("/devices", response_model=list[DeviceOut])
def list_devices(db: Session = Depends(get_db)):
    return [_device_out(d) for d in db.scalars(select(Device)).all()]


@app.get("/devices/{device_id}", response_model=DeviceOut)
def get_device(device_id: str, db: Session = Depends(get_db)):
    d = db.get(Device, device_id)
    if d is None:
        raise HTTPException(404, "device not found")
    return _device_out(d)


@app.get("/devices/{device_id}/telemetry")
def get_telemetry(device_id: str, limit: int = 100, db: Session = Depends(get_db)):
    if db.get(Device, device_id) is None:
        raise HTTPException(404, "device not found")
    rows = db.scalars(select(Telemetry).where(Telemetry.device_id == device_id)
                      .order_by(Telemetry.ts.desc()).limit(min(limit, 1000))).all()
    return [{"ts": r.ts, "irms_a": r.irms_a, "thd_pct": r.thd_pct,
             "p_est_w": r.p_est_w, "z_score": r.z_score, "irms_z": r.irms_z,
             "learn_status": r.learn_status, "source": r.source} for r in rows]


@app.get("/devices/{device_id}/events")
def get_events(device_id: str, limit: int = 50, db: Session = Depends(get_db)):
    if db.get(Device, device_id) is None:
        raise HTTPException(404, "device not found")
    rows = db.scalars(select(Event).where(Event.device_id == device_id)
                      .order_by(Event.ts.desc()).limit(min(limit, 500))).all()
    return [{"ts": r.ts, "kind": r.kind, "detail": r.detail,
             "zscore": r.zscore} for r in rows]


@app.patch("/devices/{device_id}/config", response_model=DeviceOut)
def update_config(device_id: str, cfg: ConfigUpdate, db: Session = Depends(get_db)):
    d = db.get(Device, device_id)
    if d is None:
        raise HTTPException(404, "device not found")
    for field, value in cfg.model_dump(exclude_none=True).items():
        setattr(d, field, value)
    db.commit()
    return _device_out(d)


@app.post("/devices/{device_id}/ota")
def trigger_ota(device_id: str, req: OtaRequest, db: Session = Depends(get_db)):
    d = db.get(Device, device_id)
    if d is None:
        raise HTTPException(404, "device not found")
    d.pending_ota_url = req.url
    db.add(Event(device_id=device_id, kind="ota", detail=f"OTA queued: {req.url}"))
    db.commit()
    return {"queued": True, "url": req.url}


@app.post("/devices/{device_id}/ota/ack")
def ack_ota(device_id: str, db: Session = Depends(get_db)):
    """Device calls this after a successful OTA to clear the pending flag."""
    d = db.get(Device, device_id)
    if d is None:
        raise HTTPException(404, "device not found")
    d.pending_ota_url = ""
    db.add(Event(device_id=device_id, kind="ota", detail="OTA applied"))
    db.commit()
    return {"cleared": True}
