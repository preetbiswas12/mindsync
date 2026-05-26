// BioAMP EXG Pill - Web Dashboard Mode
// ─────────────────────────────────────────────────────────────────────────────
// OUTPUT: one filtered float per line at 256 Hz  →  connect to dashboard.html
// NOISE FILTER (3-layer):
//   Layer 1 – Artifact rejection  (ADC dropout / saturation)
//   Layer 2 – 3-point Median filter
//   Layer 3 – Butterworth IIR band-pass  0.5–29.5 Hz @ 256 Hz
// The browser decomposes the signal into 5 EEG bands via JS IIR filters.
// ─────────────────────────────────────────────────────────────────────────────

const int SENSOR_PIN = A1;
const int BAUD_RATE  = 115200;
#define SAMPLE_RATE  256

#define MIN_VALID 20
#define MAX_VALID 1003

// Layer 2 – 3-point median buffer
float medBuf[3] = {512.0f, 512.0f, 512.0f};
int   medIdx    = 0;

float medianFilter(float v) {
  medBuf[medIdx] = v;
  medIdx = (medIdx + 1) % 3;
  float a = medBuf[0], b = medBuf[1], c = medBuf[2];
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;
}

// Layer 3 – Butterworth IIR band-pass 0.5–29.5 Hz @ 256 Hz (order 4, biquad SOS)
float EEGFilter(float in) {
  float o = in;
  {static float z1,z2; float x=o-(-0.95391350f)*z1-0.25311356f*z2; o=0.00735282f*x+0.01470564f*z1+0.00735282f*z2; z2=z1; z1=x;}
  {static float z1,z2; float x=o-(-1.20596630f)*z1-0.60558332f*z2; o=1.0f*x+2.0f*z1+1.0f*z2;                      z2=z1; z1=x;}
  {static float z1,z2; float x=o-(-1.97690645f)*z1-0.97706395f*z2; o=1.0f*x+(-2.0f)*z1+1.0f*z2;                   z2=z1; z1=x;}
  {static float z1,z2; float x=o-(-1.99071687f)*z1-0.99086813f*z2; o=1.0f*x+(-2.0f)*z1+1.0f*z2;                   z2=z1; z1=x;}
  return o;
}

static unsigned long pastMicros = 0;
static long          timerMicros = 0;
float lastGood = 512.0f;

void setup() {
  Serial.begin(BAUD_RATE);
  delay(500);  // short delay – web app handles garbage lines gracefully
  pastMicros = micros();
}

void loop() {
  unsigned long now     = micros();
  timerMicros          -= (long)(now - pastMicros);
  pastMicros            = now;
  if (timerMicros >= 0) return;
  timerMicros += 1000000L / SAMPLE_RATE;

  int   raw      = analogRead(SENSOR_PIN);
  bool  artifact = (raw < MIN_VALID || raw > MAX_VALID);
  float adc      = artifact ? lastGood : (float)raw;
  if (!artifact) lastGood = adc;

  float med      = medianFilter(adc);
  float filtered = EEGFilter(med);

  // Send ONLY the float value – one per line – for the web dashboard
  Serial.println(filtered, 4);
}
