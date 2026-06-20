import pytest
from pydantic import ValidationError

from app.schemas import ConfigUpdate, TelemetryIn


def test_telemetry_valid():
    t = TelemetryIn(device_id="esp32-01", irms_a=3.2, thd_pct=4.1)
    assert t.source == "ct"


def test_telemetry_negative_current_rejected():
    with pytest.raises(ValidationError):
        TelemetryIn(device_id="esp32-01", irms_a=-1.0)


def test_telemetry_bad_source_rejected():
    with pytest.raises(ValidationError):
        TelemetryIn(device_id="esp32-01", irms_a=1.0, source="hack")


def test_telemetry_empty_device_rejected():
    with pytest.raises(ValidationError):
        TelemetryIn(device_id="", irms_a=1.0)


def test_config_bounds():
    with pytest.raises(ValidationError):
        ConfigUpdate(telemetry_interval_s=0)
    assert ConfigUpdate(telemetry_interval_s=10).telemetry_interval_s == 10


def test_config_update_bin_edges():
    cfg = ConfigUpdate(bin_edges="1.0,3.0,7.0,12.0")
    assert cfg.bin_edges == "1.0,3.0,7.0,12.0"
