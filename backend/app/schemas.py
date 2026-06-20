"""Pydantic schemas for ingest validation and API responses."""
from datetime import datetime

from pydantic import BaseModel, Field


class TelemetryIn(BaseModel):
    device_id: str = Field(min_length=1, max_length=64)
    irms_a: float = Field(ge=0, le=1000)
    thd_pct: float = Field(default=0.0, ge=0, le=1000)
    p_est_w: float = Field(default=0.0, ge=0)
    z_score: float = Field(default=0.0)
    learn_status: str = Field(default="learning", max_length=16)
    fw_version: str = "unknown"
    source: str = Field(default="ct", pattern="^(ct|modbus)$")
 
 
class DeviceOut(BaseModel):
    id: str
    name: str
    fw_version: str
    last_seen: datetime
    telemetry_interval_s: int
    rms_alarm_threshold_a: float
    pending_ota_url: str
    learning_mode: bool
    online: bool


class ConfigUpdate(BaseModel):
    telemetry_interval_s: int | None = Field(default=None, ge=1, le=3600)
    rms_alarm_threshold_a: float | None = Field(default=None, gt=0, le=1000)
    learning_mode: bool | None = Field(default=None)
    name: str | None = Field(default=None, max_length=128)


class OtaRequest(BaseModel):
    url: str = Field(min_length=8, max_length=256)
