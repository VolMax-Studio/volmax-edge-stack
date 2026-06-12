# VolMax Edge Stack

![CI](https://github.com/VolMax-Studio/volmax-edge-stack/actions/workflows/ci.yml/badge.svg)

**Production-style edge telemetry stack: ESP32-S3 field device → MQTT → time-series backend → Grafana, with OTA fleet updates, remote configuration and Modbus integration.**

> Demo video: *(link goes here — real CT clamp, live dashboard, router-kill recovery, live OTA)*

Built by an electrician with 20+ years of field work who got tired of monitoring systems designed by people who never stood in front of a live panel. The system is deployed at my own house — running as a three-node fleet measuring two separate phases (household and HVAC) alongside the physical neutral wire imbalance with several days of real-world mains load logging.

## Architecture

```
[SCT-013 CT clamp] ──► [ESP32-S3 · C++]
        on-device RMS + THD (Goertzel), watchdog, NVS config
                 │ MQTT
                 ▼
        [Mosquitto] ──► [FastAPI ingest] ──► [TimescaleDB]
                              │  rolling z-score anomaly engine
                              ▼
                         [Grafana]
        live waveform stats · fleet status · alarms

[Fleet control — pull model]  device polls REST API:
        remote config (interval, thresholds) · OTA firmware updates
[Modbus TCP]  slave simulator + bridge → same pipeline (source=modbus)
```

## Features

- **On-device DSP** — windowed RMS and THD-F (Goertzel, harmonics 2–8) computed on the ESP32; the broker carries numbers, not raw waveforms
- **Self-healing connectivity** — hardware watchdog, WiFi/MQTT reconnect; kill the router and the device walks back on its own
- **Fleet management** — OTA firmware updates and remote config over a pull-based REST model (no inbound connections to devices)
- **Anomaly detection** — streaming rolling z-score per device plus configurable hard thresholds, raised as queryable events
- **Multiprotocol** — native CT sampling and Modbus TCP feed the same ingest pipeline
- **Production hygiene** — CI on every push (backend tests + firmware build), provisioned-as-code Grafana, Docker compose up in one command
- **23/23 backend tests** · analytics logic shared with [power_signal_tools](https://github.com/VolMax-Studio/power_signal_tools)

## Quick start

```bash
docker compose up -d          # broker + DB + backend + Grafana
# Grafana:  http://localhost:3000  (dashboard: VolMax Fleet Overview)
# API docs: http://localhost:8000/docs

# no hardware? run the simulated meter:
cd modbus-sim && pip install -r requirements.txt
python slave_sim.py &         # Modbus slave on :5020
python bridge.py              # polls slave → POST /ingest
```

Flash the device: edit WiFi/broker constants in `firmware/src/main.cpp`, then `pio run -e esp32s3 -t upload`.

## Fleet operations

```bash
# change reporting interval remotely (device applies within 10 s)
curl -X PATCH localhost:8000/devices/<id>/config \
     -H 'Content-Type: application/json' -d '{"telemetry_interval_s": 2}'

# queue an OTA update (device pulls, flashes, reboots, acks)
curl -X POST localhost:8000/devices/<id>/ota \
     -H 'Content-Type: application/json' -d '{"url": "http://<host>:8000/ota/fw.bin"}'
```

## Calibration & validation

The CT channel is calibrated against a Gossen Metrawatt clamp meter on real loads — procedure and results in [docs/calibration.md](docs/calibration.md). Field accuracy is a measurement problem before it is a software problem.

## Design decisions

**Why pre-process on the edge?** Raw 2 kHz waveforms over MQTT waste bandwidth and broker capacity; RMS/THD at 0.2 Hz carries the signal that matters. **Why Goertzel over FFT?** Eight harmonic bins, O(N) each, zero dependencies. **Why pull-based OTA/config?** Devices behind NAT need no inbound ports, no broker RPC, no certificate dance — they ask, the fleet API answers. **Why Grafana?** It is what plant operators already run; provisioning it from JSON keeps the repo reproducible.

## Roadmap

- Voltage channel → true active power and power factor
- Appliance-level load identification (see [NILM portfolio](https://github.com/VolMax-Studio/NILM_Disaggregation_Portfolio))
- Device-level baseline fingerprinting: learn each device's normal RMS/THD signature, persist it, and track long-term drift to surface gradual degradation before failure (condition monitoring)
- TLS on MQTT + per-device credentials
- Fleet dashboard: OTA rollout status across N devices

## Related work

[Battery health (NASA PCoE)](https://github.com/VolMax-Studio/Battery_Health_Portfolio) · [Power quality classifier](https://github.com/VolMax-Studio/PowerQuality_Classifier_Portfolio) · [Grid frequency dynamics](https://github.com/VolMax-Studio/Grid_Frequency_Analysis) · [MCSA motor diagnostics](https://github.com/VolMax-Studio/MCSA_Fault_Diagnostics_Portfolio)

---

**VolMax Studio Lab d.o.o.** · Titel, Serbia · volmax.core@gmail.com
