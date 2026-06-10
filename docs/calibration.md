# SCT-013 Calibration & Validation

## Hardware chain
SCT-013-030 (30 A : 1 V) -> midpoint bias divider (2x 10 kΩ between
3.3 V and GND, 10 µF across the lower leg) -> ESP32-S3 ADC1 (GPIO4,
12-bit, 11 dB attenuation).

## Why bias removal in software
The CT output swings around the 1.65 V midpoint. Each acquisition
window subtracts its own mean instead of trusting the divider —
this cancels divider tolerance and ADC offset drift in one step.

## Calibration procedure
1. Resistive load (heater, 2 kW class) on the measured line.
2. Reference reading with a calibrated power quality analyzer
   (MAVOWATT 45) clamped on the same conductor.
3. `calGain = I_ref / I_measured`, written to NVS (`calgain`).
4. Repeat at low load (~0.5 A) to confirm linearity within ±2 %.

## Validation results
(filled in during the 24 h home-monitoring run — table of
reference vs. device readings at 3 load points goes here)

## Known limitations
- ESP32 ADC nonlinearity at the extremes — keep CT sized so the
  expected max stays inside ~10–90 % of range.
- Power is estimated as Irms x 230 V (apparent, fixed V); a true
  power measurement needs a voltage channel — on the roadmap.
