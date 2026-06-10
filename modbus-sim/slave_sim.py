"""Modbus TCP slave simulator — stands in for an industrial meter/inverter.

Exposes holding registers the gateway reads:
  HR0: current x100 (A)   HR1: voltage x10 (V)
  HR2: power (W)          HR3: frequency x100 (Hz)
Values drift realistically so dashboards look alive.

Run:  pip install -r requirements.txt && python slave_sim.py
"""
import math
import random
import threading
import time

from pymodbus.datastore import (ModbusSequentialDataBlock, ModbusServerContext,
                                ModbusSlaveContext)
from pymodbus.server import StartTcpServer

block = ModbusSequentialDataBlock(0, [0] * 10)
context = ModbusServerContext(slaves=ModbusSlaveContext(hr=block), single=True)


def drift():
    t = 0.0
    while True:
        irms = 4.0 + 1.5 * math.sin(t / 30) + random.uniform(-0.2, 0.2)
        volt = 230.0 + random.uniform(-2, 2)
        freq = 50.0 + random.uniform(-0.02, 0.02)
        block.setValues(0, [int(irms * 100), int(volt * 10),
                            int(irms * volt), int(freq * 100)])
        t += 1
        time.sleep(1)


if __name__ == "__main__":
    threading.Thread(target=drift, daemon=True).start()
    print("Modbus slave on 0.0.0.0:5020 (HR0..HR3)")
    StartTcpServer(context=context, address=("0.0.0.0", 5020))
