def ingest(client, device="esp32-01", irms=2.0, **kw):
    return client.post("/ingest", json={"device_id": device, "irms_a": irms,
                                        "fw_version": "1.0.0", **kw})


def test_health(client):
    assert client.get("/health").json() == {"status": "ok"}


def test_ingest_creates_device(client):
    assert ingest(client).status_code == 201
    devices = client.get("/devices").json()
    assert len(devices) == 1
    assert devices[0]["id"] == "esp32-01"
    assert devices[0]["fw_version"] == "1.0.0"
    assert devices[0]["online"] is True


def test_ingest_invalid_payload_422(client):
    r = client.post("/ingest", json={"device_id": "x", "irms_a": "abc"})
    assert r.status_code == 422


def test_telemetry_listing(client):
    for i in range(5):
        ingest(client, irms=1.0 + i)
    rows = client.get("/devices/esp32-01/telemetry").json()
    assert len(rows) == 5
    assert rows[0]["irms_a"] == 5.0  # newest first


def test_unknown_device_404(client):
    assert client.get("/devices/ghost").status_code == 404
    assert client.get("/devices/ghost/telemetry").status_code == 404


def test_threshold_event(client):
    client.post("/ingest", json={"device_id": "d1", "irms_a": 1.0})
    client.patch("/devices/d1/config", json={"rms_alarm_threshold_a": 5.0})
    r = ingest(client, device="d1", irms=9.0)
    assert r.json()["events_raised"] >= 1
    events = client.get("/devices/d1/events").json()
    assert any(e["kind"] == "threshold" for e in events)


def test_config_update(client):
    ingest(client)
    r = client.patch("/devices/esp32-01/config",
                     json={"telemetry_interval_s": 30, "name": "Garage main"})
    assert r.status_code == 200
    assert r.json()["telemetry_interval_s"] == 30
    assert r.json()["name"] == "Garage main"


def test_config_partial_update_keeps_rest(client):
    ingest(client)
    client.patch("/devices/esp32-01/config", json={"name": "A"})
    d = client.get("/devices/esp32-01").json()
    assert d["telemetry_interval_s"] == 5  # default untouched


def test_ota_queue_and_ack(client):
    ingest(client)
    r = client.post("/devices/esp32-01/ota",
                    json={"url": "http://backend:8000/ota/fw-1.1.0.bin"})
    assert r.json()["queued"] is True
    assert client.get("/devices/esp32-01").json()["pending_ota_url"].endswith("1.1.0.bin")
    client.post("/devices/esp32-01/ota/ack")
    assert client.get("/devices/esp32-01").json()["pending_ota_url"] == ""
    kinds = [e["kind"] for e in client.get("/devices/esp32-01/events").json()]
    assert kinds.count("ota") == 2


def test_anomaly_event_via_api(client):
    for _ in range(30):
        ingest(client, device="d2", irms=2.0)
    ingest(client, device="d2", irms=14.0)
    events = client.get("/devices/d2/events").json()
    assert any(e["kind"] == "anomaly" for e in events)


def test_multiple_devices_isolated(client):
    ingest(client, device="a", irms=1.0)
    ingest(client, device="b", irms=2.0)
    assert len(client.get("/devices").json()) == 2
    assert len(client.get("/devices/a/telemetry").json()) == 1


def test_device_config_bin_edges(client):
    ingest(client, device="esp32-02")
    d = client.get("/devices/esp32-02").json()
    assert d["bin_edges"] == "1.0,5.0,10.0,20.0"
    
    r = client.patch("/devices/esp32-02/config", json={"bin_edges": "1.0,3.0,7.0,12.0"})
    assert r.status_code == 200
    assert r.json()["bin_edges"] == "1.0,3.0,7.0,12.0"


def test_ingest_with_zscore_and_irmsz(client):
    r = client.post("/ingest", json={
        "device_id": "esp32-03",
        "irms_a": 4.5,
        "thd_pct": 2.1,
        "z_score": 1.25,
        "irms_z": 0.84,
        "learn_status": "monitoring"
    })
    assert r.status_code == 201
    telemetry = client.get("/devices/esp32-03/telemetry").json()
    assert len(telemetry) == 1
    assert telemetry[0]["z_score"] == 1.25
    assert telemetry[0]["irms_z"] == 0.84
    assert telemetry[0]["learn_status"] == "monitoring"
