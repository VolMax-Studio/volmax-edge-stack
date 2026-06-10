"""Modbus -> backend bridge: polls the slave, posts to /ingest as
source=modbus. In the field this loop runs on the ESP32 gateway;
here it doubles as a host-side demo so the full chain works without
a second physical device.
"""
import time

import httpx
from pymodbus.client import ModbusTcpClient

BACKEND = "http://localhost:8000"

if __name__ == "__main__":
    client = ModbusTcpClient("localhost", port=5020)
    client.connect()
    print("bridging Modbus :5020 ->", BACKEND)
    while True:
        rr = client.read_holding_registers(address=0, count=4)
        if not rr.isError():
            irms = rr.registers[0] / 100
            power = rr.registers[2]
            httpx.post(f"{BACKEND}/ingest", json={
                "device_id": "modbus-meter-01", "irms_a": irms,
                "p_est_w": power, "fw_version": "sim", "source": "modbus",
            }, timeout=5)
        time.sleep(5)
