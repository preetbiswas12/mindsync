# MindSync

Ultra-premium EEG dashboard and BioAMP EXG firmware for local simulation and live serial streaming.

## Contents
- `bioamp_web.ino` — Arduino firmware: reads ADC, applies median + 4-stage Butterworth band-pass, prints one filtered float per line at 256 Hz.
- `dashboard.html` — Browser dashboard: connects via Web Serial or loads CSV simulation files; decomposes incoming filtered samples into bands and visualizes them.
- `*.csv` — Exported simulation logs (supported CSV format described below).

## Quick Start

Prerequisites:
- Google Chrome or Microsoft Edge (v89+) for Web Serial support.
- Connect your Arduino board and select the correct serial port in the dashboard.

Run (serve locally so Web Serial works reliably):

```bash
# from project root
python -m http.server 8000
# then open http://localhost:8000/dashboard.html in Chrome/Edge
```

Live streaming (Arduino):
1. Open `bioamp_web.ino` in the Arduino IDE.
2. Select your board and port, compile and upload.
3. Open the dashboard in Chrome/Edge and click **Connect Arduino**.

Simulation (CSV):
- Use the **Load simulation file** button (Accepts `.csv` produced by the dashboard). The dashboard will detect CSV rows and ingest precomputed band columns when available.

Supported CSV format (header row expected):
```
Row,Timestamp_ms,RawEEG,Delta,Theta,Alpha,Beta,Gamma,DominantBand
```
- The dashboard accepts rows with these columns and will use the `RawEEG` value and the precomputed band columns (`Delta`..`Gamma`) when present.

## Why negative values appear in recordings
- `analogRead()` returns integer ADC values in `0..1023` (raw sensor). The firmware then applies a band-pass filter that removes DC/baseline and centers the signal around `0`. Filtered EEG is an AC signal and therefore naturally contains both positive and negative values. This is expected and correct for spectral analysis.

If you need non-negative recorded values (for display or legacy tools), choose one of:
- Record raw ADC samples (0..1023) instead of filtered output.
- Shift filtered output before sending: e.g. `shifted = filtered + 512.0` (then subtract when analyzing offline).
- Store both filtered and shifted values (recommended for analysis + compatibility).

Example: modify `bioamp_web.ino` to send a shifted column (pseudo-code):
```cpp
float filtered = EEGFilter(med);
float shifted = filtered + 512.0f; // non-negative for recording/display
Serial.print(filtered, 4); Serial.print(','); Serial.println(shifted, 4);
```

## Notes & Troubleshooting
- The dashboard parser differentiates two modes:
  - CSV mode: when header contains `RawEEG` and band names — it reads precomputed columns and will not apply artifact rejection to those rows.
  - Legacy raw text mode: single float per line (firmware live output). The dashboard applies the same median + IIR filters and artifact rejection logic to legacy raw-mode inputs.
- Web Serial requires a secure context: testing from `http://localhost:8000` is recommended.

## License & Contact
This repository is for private development. If you want changes (e.g., automatically sending raw + shifted columns or changing the CSV exporter), open an issue or ask me to update the code.
