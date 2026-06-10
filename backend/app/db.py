"""SQLAlchemy models: devices, telemetry, events.

SQLite by default (dev/tests); TimescaleDB/Postgres in docker-compose.
"""
from datetime import datetime, timezone

from sqlalchemy import (Boolean, DateTime, Float, ForeignKey, Integer,
                        String, create_engine)
from sqlalchemy.orm import (DeclarativeBase, Mapped, mapped_column,
                            sessionmaker)

from .config import DATABASE_URL

engine = create_engine(DATABASE_URL, connect_args={"check_same_thread": False}
                       if DATABASE_URL.startswith("sqlite") else {})
SessionLocal = sessionmaker(bind=engine, autoflush=False, expire_on_commit=False)


def utcnow() -> datetime:
    return datetime.now(timezone.utc)


class Base(DeclarativeBase):
    pass


class Device(Base):
    __tablename__ = "devices"
    id: Mapped[str] = mapped_column(String(64), primary_key=True)
    name: Mapped[str] = mapped_column(String(128), default="")
    fw_version: Mapped[str] = mapped_column(String(32), default="unknown")
    last_seen: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow)
    telemetry_interval_s: Mapped[int] = mapped_column(Integer, default=5)
    rms_alarm_threshold_a: Mapped[float] = mapped_column(Float, default=16.0)
    pending_ota_url: Mapped[str] = mapped_column(String(256), default="")


class Telemetry(Base):
    __tablename__ = "telemetry"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id"), index=True)
    ts: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, index=True)
    irms_a: Mapped[float] = mapped_column(Float)
    thd_pct: Mapped[float] = mapped_column(Float, default=0.0)
    p_est_w: Mapped[float] = mapped_column(Float, default=0.0)
    source: Mapped[str] = mapped_column(String(16), default="ct")  # ct | modbus


class Event(Base):
    __tablename__ = "events"
    id: Mapped[int] = mapped_column(Integer, primary_key=True, autoincrement=True)
    device_id: Mapped[str] = mapped_column(ForeignKey("devices.id"), index=True)
    ts: Mapped[datetime] = mapped_column(DateTime(timezone=True), default=utcnow, index=True)
    kind: Mapped[str] = mapped_column(String(32))  # anomaly | threshold | ota | offline
    detail: Mapped[str] = mapped_column(String(256), default="")
    zscore: Mapped[float] = mapped_column(Float, default=0.0)
    acknowledged: Mapped[bool] = mapped_column(Boolean, default=False)


def init_db() -> None:
    Base.metadata.create_all(engine)
