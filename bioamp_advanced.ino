// BioAMP EXG Pill - Advanced EEG Code with Noise Filtering & Brain-Wave Classification
// ────────────────────────────────────────────────────────────────────────────────────
// THREE-LAYER NOISE REDUCTION (targets ~80% noise rejection):
//   Layer 1 – Artifact rejection   : clips near-rail values (< MIN_VALID or > MAX_VALID)
//   Layer 2 – 3-point Median filter: kills isolated spike outliers
//   Layer 3 – Butterworth IIR BP   : band-pass 0.5–29.5 Hz removes all out-of-band noise
//
// BRAIN WAVE CLASSIFICATION via Zero-Crossing Rate (ZCR) over 256-sample window:
//   Delta  :  0.5 –  4 Hz  (deep sleep / recovery)
//   Theta  :  4   –  8 Hz  (drowsy / meditative)
//   Alpha  :  8   – 12 Hz  (relaxed, eyes closed)
//   Beta   : 12   – 30 Hz  (alert / focused / thinking)
//   Gamma  : 30   – 44 Hz  (high cognition / problem solving)
//              ↑ gamma limit kept at 44 Hz (Nyquist of 256 Hz ADC margin)
//
// Reference: https://github.com/upsidedownlabs/BioAmp-EXG-Pill
// Copyright (c) 2021 Upside Down Labs – MIT License.

#include <limits.h>

// ─── Hardware ────────────────────────────────────────────────────────────────
const int SENSOR_PIN = A1;
const int BAUD_RATE  = 115200;
#define SAMPLE_RATE  256           // Hz – must match filter design

// ─── Artifact rejection thresholds ───────────────────────────────────────────
// Raw ADC values outside this window are treated as electrode pop / saturation
#define MIN_VALID    20            // below this  → near-zero dropout
#define MAX_VALID    1003          // above this  → ADC rail saturation (1023 max)

// ─── Median filter buffer (3-point) ──────────────────────────────────────────
#define MED_N 3
float medBuf[MED_N] = {512, 512, 512};
int   medIdx = 0;

// ─── ZCR sliding window ───────────────────────────────────────────────────────
#define ZCR_WINDOW  SAMPLE_RATE    // 256 samples = 1 second
int   zcrCount     = 0;           // zero-crossings counted this window
int   zcrSamples   = 0;           // samples counted this window
float lastFiltered = 0;           // previous filtered value for crossing detect
// Mean of filtered signal (tracked as running average – used as "zero" reference)
float filtMean     = 0;
float filtMeanAlpha = 0.002f;     // slow IIR for mean tracking

// ─── Per-batch stats (256 samples = 1 s) ─────────────────────────────────────
float batchMin   =  1e30f;
float batchMax   = -1e30f;
float batchSum   = 0;
int   batchCount = 0;
int   rejectedCount = 0;          // artifact-rejected samples per batch

// ─── Global session stats ─────────────────────────────────────────────────────
float globalMin   =  1e30f;
float globalMax   = -1e30f;
float globalSum   = 0;
long  globalCount = 0;

// ─── Timing ───────────────────────────────────────────────────────────────────
static unsigned long pastMicros  = 0;
static long          timerMicros = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  Layer 3 – EEG Band-Pass Butterworth IIR filter
//  256 Hz sample rate | 0.5–29.5 Hz pass-band | Order 4 (biquad SOS)
//  Generated with filter_gen.py – Upside Down Labs / CMU 16-223
// ─────────────────────────────────────────────────────────────────────────────
float EEGFilter(float input) {
  float output = input;

  { static float z1, z2;
    float x = output - (-0.95391350f)*z1 - 0.25311356f*z2;
    output = 0.00735282f*x + 0.01470564f*z1 + 0.00735282f*z2;
    z2 = z1; z1 = x; }

  { static float z1, z2;
    float x = output - (-1.20596630f)*z1 - 0.60558332f*z2;
    output = 1.00000000f*x + 2.00000000f*z1 + 1.00000000f*z2;
    z2 = z1; z1 = x; }

  { static float z1, z2;
    float x = output - (-1.97690645f)*z1 - 0.97706395f*z2;
    output = 1.00000000f*x + (-2.00000000f)*z1 + 1.00000000f*z2;
    z2 = z1; z1 = x; }

  { static float z1, z2;
    float x = output - (-1.99071687f)*z1 - 0.99086813f*z2;
    output = 1.00000000f*x + (-2.00000000f)*z1 + 1.00000000f*z2;
    z2 = z1; z1 = x; }

  return output;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Layer 2 – 3-point Median Filter  (kills isolated spike outliers)
// ─────────────────────────────────────────────────────────────────────────────
float medianFilter(float newVal) {
  medBuf[medIdx] = newVal;
  medIdx = (medIdx + 1) % MED_N;

  // Sort 3 values and return middle
  float a = medBuf[0], b = medBuf[1], c = medBuf[2];
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;  // median
}

// ─────────────────────────────────────────────────────────────────────────────
//  Brain wave classifier from ZCR count over a 1-second window
//  ZCR ≈ 2 × dominant_frequency (signal crosses mean twice per cycle)
// ─────────────────────────────────────────────────────────────────────────────
const char* classifyBrainWave(int crossings) {
  // crossings per second → dominant Hz ≈ crossings / 2
  if (crossings <  8)  return F("DELTA  (0.5-4 Hz)  | Deep Sleep / Recovery");
  if (crossings < 16)  return F("THETA  (4-8 Hz)    | Drowsy / Meditative  ");
  if (crossings < 24)  return F("ALPHA  (8-12 Hz)   | Relaxed / Eyes Closed");
  if (crossings < 60)  return F("BETA   (12-30 Hz)  | Alert / Focused      ");
  return                      F("GAMMA  (30-44 Hz)  | High Cognition        ");
}

const char* brainWaveShort(int crossings) {
  if (crossings <  8)  return "DELTA ";
  if (crossings < 16)  return "THETA ";
  if (crossings < 24)  return "ALPHA ";
  if (crossings < 60)  return "BETA  ";
  return                      "GAMMA ";
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(BAUD_RATE);
  delay(2000);

  Serial.println(F("======================================================="));
  Serial.println(F("  BioAMP EXG Pill | EEG with 3-Layer Noise Filtering  "));
  Serial.println(F("======================================================="));
  Serial.println(F("  Layer 1 : Artifact rejection  (clip near-rail spikes)"));
  Serial.println(F("  Layer 2 : 3-point Median filter (spike outliers)     "));
  Serial.println(F("  Layer 3 : Butterworth BP 0.5-29.5 Hz (IIR, order 4) "));
  Serial.println(F("  Brain Waves: Delta | Theta | Alpha | Beta | Gamma    "));
  Serial.println(F("=======================================================\n"));
  Serial.println(F("Starting in 3 seconds..."));
  delay(3000);

  pastMicros  = micros();
  timerMicros = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  // Precise 256 Hz scheduler
  unsigned long now      = micros();
  unsigned long elapsed  = now - pastMicros;
  pastMicros   = now;
  timerMicros -= (long)elapsed;
  if (timerMicros >= 0) return;
  timerMicros += 1000000L / SAMPLE_RATE;

  // ── Read raw ADC ──────────────────────────────────────────────────────────
  int rawADC = analogRead(SENSOR_PIN);

  // ── LAYER 1: Artifact Rejection ───────────────────────────────────────────
  bool isArtifact = (rawADC < MIN_VALID || rawADC > MAX_VALID);
  float adcIn;
  if (isArtifact) {
    // Replace artifact sample with the median-buffer average (hold last good)
    adcIn = medBuf[(medIdx + MED_N - 1) % MED_N];  // last valid median value
    rejectedCount++;
  } else {
    adcIn = (float)rawADC;
  }

  // ── LAYER 2: Median Filter ────────────────────────────────────────────────
  float medFiltered = medianFilter(adcIn);

  // ── LAYER 3: Butterworth IIR Band-Pass ────────────────────────────────────
  float filtered = EEGFilter(medFiltered);

  // ── Update running mean (used as "zero" for ZCR) ──────────────────────────
  filtMean = filtMean + filtMeanAlpha * (filtered - filtMean);

  // ── Zero-Crossing Rate detection ─────────────────────────────────────────
  // A crossing = filtered signal crosses the running mean from either direction
  float centered     = filtered - filtMean;
  float lastCentered = lastFiltered - filtMean;
  if ((centered > 0) != (lastCentered > 0)) {   // sign flip = crossing
    zcrCount++;
  }
  lastFiltered = filtered;
  zcrSamples++;

  // ── Per-sample serial output ──────────────────────────────────────────────
  // Use tab-separated for easy Serial Plotter viewing
  Serial.print(filtered, 2);
  Serial.print(F("\t"));
  Serial.print(brainWaveShort(zcrCount));   // current window estimate
  Serial.print(F("\t| Raw:"));
  Serial.print(isArtifact ? -1 : rawADC);  // -1 flags rejected samples
  Serial.println();

  // ── Update global stats ───────────────────────────────────────────────────
  if (!isArtifact) {
    if (filtered < globalMin) globalMin = filtered;
    if (filtered > globalMax) globalMax = filtered;
    globalSum += filtered;
    globalCount++;
  }

  // ── Update batch stats ────────────────────────────────────────────────────
  if (!isArtifact) {
    if (filtered < batchMin) batchMin = filtered;
    if (filtered > batchMax) batchMax = filtered;
    batchSum += filtered;
  }
  batchCount++;

  // ── Every 256 samples (1 second) print full report ───────────────────────
  if (batchCount >= ZCR_WINDOW) {
    float batchAvg    = (batchCount - rejectedCount > 0)
                        ? batchSum / (batchCount - rejectedCount)
                        : 0;
    float noiseRejPct = (100.0f * rejectedCount) / batchCount;
    int   estHz       = zcrCount / 2;              // dominant frequency estimate

    Serial.println(F("\n╔══════════════════════════════════════════════════════╗"));
    Serial.println(F("║         1-SECOND EEG ANALYSIS REPORT                ║"));
    Serial.println(F("╠══════════════════════════════════════════════════════╣"));

    Serial.print  (F("║ Brain Wave Band : "));
    Serial.print  (classifyBrainWave(zcrCount));
    Serial.println(F(" ║"));

    Serial.print  (F("║ Dominant Freq   : ~"));
    Serial.print  (estHz);
    Serial.println(F(" Hz                                ║"));

    Serial.print  (F("║ Zero Crossings  : "));
    printPaddedI(zcrCount, 4);

    Serial.print  (F("║ Artifacts Reject: "));
    Serial.print  (rejectedCount);
    Serial.print  (F(" samples ("));
    Serial.print  (noiseRejPct, 1);
    Serial.println(F("%)                       ║"));

    Serial.println(F("╠══════════════════════════════════════════════════════╣"));
    Serial.print  (F("║ Filtered Min    : ")); printPaddedF(batchMin);
    Serial.print  (F("║ Filtered Max    : ")); printPaddedF(batchMax);
    Serial.print  (F("║ Filtered Avg    : ")); printPaddedF(batchAvg);
    Serial.print  (F("║ Filtered Range  : ")); printPaddedF(batchMax - batchMin);

    if (globalCount > 0) {
      Serial.println(F("╠══════════════════════════════════════════════════════╣"));
      Serial.print  (F("║ Session Min     : ")); printPaddedF(globalMin);
      Serial.print  (F("║ Session Max     : ")); printPaddedF(globalMax);
      Serial.print  (F("║ Session Avg     : ")); printPaddedF(globalSum / globalCount);
      Serial.print  (F("║ Total Samples   : ")); printPaddedI(globalCount, 7);
    }

    Serial.println(F("╚══════════════════════════════════════════════════════╝\n"));

    // Reset batch counters
    batchMin      =  1e30f;
    batchMax      = -1e30f;
    batchSum      = 0;
    batchCount    = 0;
    rejectedCount = 0;
    zcrCount      = 0;
    zcrSamples    = 0;
  }
}

// ─── Print helpers ────────────────────────────────────────────────────────────
void printPaddedF(float v) {
  char buf[10];
  dtostrf(v, 8, 2, buf);
  Serial.print(buf);
  Serial.println(F("                          ║"));
}

void printPaddedI(long v, int fieldWidth) {
  // Right-justify within fieldWidth chars
  char buf[12];
  ltoa(v, buf, 10);
  int len = strlen(buf);
  for (int i = len; i < fieldWidth; i++) Serial.print(' ');
  Serial.print(buf);
  Serial.println(F("                             ║"));
}

/*
══════════════════════════════════════════════════════════════════════════════
  HOW THE NOISE REDUCTION WORKS
══════════════════════════════════════════════════════════════════════════════

  OBSERVED NOISE IN YOUR DATA:
  ┌──────────────────┬────────────────────────────┬────────────────────┐
  │ Noise Type       │ Example values             │ Layer that kills it│
  ├──────────────────┼────────────────────────────┼────────────────────┤
  │ ADC dropout      │ 5, 6, 7  (≈0.03V)          │ Layer 1            │
  │ ADC saturation   │ 1017-1019 (≈4.97V)         │ Layer 1            │
  │ Isolated spikes  │ single-sample jump ±300    │ Layer 2 (median)   │
  │ 50/60 Hz mains   │ power line hum             │ Layer 3 (IIR BP)   │
  │ DC offset drift  │ slow baseline wander       │ Layer 3 (IIR BP)   │
  │ EMG/muscle noise │ >30 Hz content             │ Layer 3 (IIR BP)   │
  └──────────────────┴────────────────────────────┴────────────────────┘

  BRAIN WAVE BANDS (standard clinical EEG):
  ┌───────┬───────────┬─────────────────────────────────────────┐
  │ Band  │ Freq (Hz) │ Associated State                        │
  ├───────┼───────────┼─────────────────────────────────────────┤
  │ Delta │  0.5 – 4  │ Deep sleep, recovery, unconscious       │
  │ Theta │  4  – 8   │ Drowsy, meditative, creative flow       │
  │ Alpha │  8  – 12  │ Relaxed awake, eyes closed, calm focus  │
  │ Beta  │ 12  – 30  │ Active thinking, alert, problem solving │
  │ Gamma │ 30  – 44  │ High cognition, cross-modal perception  │
  └───────┴───────────┴─────────────────────────────────────────┘

  ZCR (Zero-Crossing Rate) classification:
  Crossings per second ÷ 2 ≈ dominant frequency in Hz

  SERIAL PLOTTER:
  Column 1 = filtered EEG signal (float)
  Column 2 = current brain wave band label
  Column 3 = raw ADC (-1 = rejected artifact)
*/