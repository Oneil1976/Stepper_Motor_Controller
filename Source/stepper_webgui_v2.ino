/*
 * Ovládanie krokového motora (ENABLE / DIRECTION / PULSE) cez web GUI
 * Doska: WeMos D1 mini (ESP8266), Arduino SDK (ESP8266 core)
 *
 * WiFi AP:  SSID "StepperCTRL"  heslo "12345678"
 * GUI:      http://192.168.4.1
 *
 * Zapojenie (3.3 V logika!):
 *   D1 (GPIO5)  -> ENABLE  (ENA)
 *   D2 (GPIO4)  -> DIRECTION (DIR)
 *   D5 (GPIO14) -> PULSE (PUL/STEP)
 *   GND         -> spoločná zem s driverom
 *
 * Pulzy generuje hardvérový Timer1 (ISR) -> presná frekvencia
 * nezávislá od WiFi a webservera.
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

// ---------------- Konfigurácia ----------------
const char* AP_SSID = "StepperCTRL";
const char* AP_PASS = "12345678";      // min. 8 znakov, alebo "" pre otvorenú sieť

#define PIN_ENABLE   5     // D1
#define PIN_DIR      4     // D2
#define PIN_PULSE    14    // D5  (GPIO14)

// Polarita signálov - upraviť podľa drivera
#define ENABLE_ACTIVE_LEVEL  LOW    // úroveň, pri ktorej je driver zapnutý
                                    // (TB6600/DM542 často: LOW = enable, ak je zapojený ENA-)
#define DIR_CW_LEVEL         HIGH   // úroveň DIR pre smer CW

#define FREQ_MIN     1UL            // Hz
#define FREQ_MAX     20000UL        // Hz (obmedzenie kvôli ISR)
#define TIMER_TICKS_PER_SEC 5000000UL   // Timer1: 80 MHz / 16

// ---------------- Stav ----------------
ESP8266WebServer server(80);

volatile uint32_t stepsRemaining = 0;
volatile bool     infiniteMode   = false;
volatile bool     running        = false;
volatile bool     pulseHigh      = false;

bool     enableState = false;
bool     dirCW       = true;
uint32_t lastSteps   = 0;
uint32_t lastFreq    = 1000;

// ---------------- Web GUI ----------------
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="sk">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Stepper ovládanie</title>
<style>
  body{font-family:system-ui,Arial,sans-serif;background:#111827;color:#e5e7eb;margin:0;padding:16px;display:flex;justify-content:center}
  .card{width:100%;max-width:420px;background:#1f2937;border-radius:14px;padding:20px;box-shadow:0 4px 20px #0008}
  h1{font-size:1.3rem;margin:0 0 16px}
  .row{display:flex;justify-content:space-between;align-items:center;margin:16px 0}
  label{font-size:1rem}
  .sw{position:relative;width:64px;height:34px;flex:none}
  .sw input{opacity:0;width:0;height:0}
  .sl{position:absolute;inset:0;background:#4b5563;border-radius:34px;transition:.2s;cursor:pointer}
  .sl:before{content:"";position:absolute;height:26px;width:26px;left:4px;top:4px;background:#fff;border-radius:50%;transition:.2s}
  input:checked + .sl{background:#10b981}
  input:checked + .sl:before{transform:translateX(30px)}
  .dirsw input:checked + .sl{background:#3b82f6}
  input[type=number]{width:130px;padding:8px;border-radius:8px;border:1px solid #4b5563;background:#111827;color:#e5e7eb;font-size:1rem;text-align:right}
  button{width:48%;padding:12px;border:0;border-radius:10px;font-size:1rem;font-weight:600;cursor:pointer;color:#fff}
  #bStart{background:#10b981} #bStop{background:#ef4444}
  button:disabled{opacity:.4;cursor:not-allowed}
  .status{margin-top:18px;padding:12px;background:#111827;border-radius:10px;font-size:.95rem;line-height:1.6}
  small{color:#9ca3af}
  .pins{margin-top:18px;font-size:.9rem}
  .pins h2{font-size:1rem;margin:0 0 8px}
  .pins table{width:100%;border-collapse:collapse}
  .pins th,.pins td{text-align:left;padding:6px 8px;border-bottom:1px solid #374151}
  .pins th{color:#9ca3af;font-weight:600}
  .pins p{margin:8px 0 0;color:#9ca3af;font-size:.8rem}
</style>
</head>
<body>
<div class="card">
  <h1>Ovládanie krokového motora</h1>

  <div class="row">
    <label>ENABLE</label>
    <label class="sw"><input type="checkbox" id="enable"><span class="sl"></span></label>
  </div>

  <div class="row">
    <label>Smer: <b id="dirTxt">CW</b></label>
    <label class="sw dirsw"><input type="checkbox" id="dir"><span class="sl"></span></label>
  </div>
  <small>vypnuté = CW, zapnuté = CCW</small>

  <div class="row">
    <label>Počet krokov<br><small>0 = nepretržitý chod</small></label>
    <input type="number" id="steps" min="0" max="4000000000" value="1000">
  </div>

  <div class="row">
    <label>Rýchlosť [Hz]<br><small>1 – 20000 pulzov/s</small></label>
    <input type="number" id="freq" min="1" max="20000" value="1000">
  </div>

  <div class="row">
    <button id="bStart">START</button>
    <button id="bStop">STOP</button>
  </div>

  <div class="status" id="status">Pripájam…</div>

  <div class="pins">
    <h2>Zapojenie signálov</h2>
    <table>
      <tr><th>Signál</th><th>Pin D1 mini</th></tr>
      <tr><td>ENABLE</td><td>D1 (GPIO5)</td></tr>
      <tr><td>DIRECTION</td><td>D2 (GPIO4)</td></tr>
      <tr><td>PULSE</td><td>D5 (GPIO14)</td></tr>
    </table>
    <p>Logika 3,3 V, spoločná zem (GND) s driverom.</p>
  </div>
</div>

<script>
const $ = id => document.getElementById(id);

async function api(path){
  try{
    const r = await fetch(path);
    return await r.json();
  }catch(e){ return null; }
}

function render(s){
  if(!s){ $('status').textContent = 'Bez spojenia so zariadením'; return; }
  const act = document.activeElement;
  if(act !== $('enable')) $('enable').checked = s.enable;
  if(act !== $('dir'))    $('dir').checked = !s.cw;
  $('dirTxt').textContent = s.cw ? 'CW' : 'CCW';
  $('bStart').disabled = !s.enable;
  $('status').innerHTML =
    'Stav: <b>' + (s.running ? 'BEŽÍ' : 'STOJÍ') + '</b><br>' +
    'ENABLE: <b>' + (s.enable ? 'ON' : 'OFF') + '</b> | Smer: <b>' + (s.cw ? 'CW' : 'CCW') + '</b><br>' +
    (s.infinite ? 'Režim: nepretržitý' : 'Zostáva krokov: <b>' + s.remaining + '</b>') +
    '<br>Frekvencia: <b>' + s.freq + ' Hz</b>';
}

async function refresh(){ render(await api('/api/status')); }

$('enable').onchange = async () => render(await api('/api/enable?v=' + ($('enable').checked ? 1 : 0)));
$('dir').onchange    = async () => render(await api('/api/dir?cw=' + ($('dir').checked ? 0 : 1)));
$('bStart').onclick  = async () => {
  const steps = Math.max(0, parseInt($('steps').value) || 0);
  const freq  = Math.min(20000, Math.max(1, parseInt($('freq').value) || 1));
  $('freq').value = freq; $('steps').value = steps;
  render(await api('/api/start?steps=' + steps + '&freq=' + freq));
};
$('bStop').onclick   = async () => render(await api('/api/stop'));

refresh();
setInterval(refresh, 500);
</script>
</body>
</html>
)rawliteral";

// ---------------- ISR pre Timer1 ----------------
void ICACHE_RAM_ATTR onTimer1() {
  if (!pulseHigh) {
    GPOS = (1 << PIN_PULSE);          // PULSE -> HIGH
    pulseHigh = true;
  } else {
    GPOC = (1 << PIN_PULSE);          // PULSE -> LOW
    pulseHigh = false;
    if (!infiniteMode) {
      if (stepsRemaining > 0) stepsRemaining--;
      if (stepsRemaining == 0) {
        timer1_disable();
        running = false;
      }
    }
  }
}

// ---------------- Ovládanie motora ----------------
void applyEnable() {
  digitalWrite(PIN_ENABLE, enableState ? ENABLE_ACTIVE_LEVEL : !ENABLE_ACTIVE_LEVEL);
}

void applyDir() {
  digitalWrite(PIN_DIR, dirCW ? DIR_CW_LEVEL : !DIR_CW_LEVEL);
}

void stopPulses() {
  noInterrupts();
  timer1_disable();
  GPOC = (1 << PIN_PULSE);
  pulseHigh = false;
  running = false;
  stepsRemaining = 0;
  infiniteMode = false;
  interrupts();
}

void startPulses(uint32_t steps, uint32_t freq) {
  if (!enableState) return;
  if (freq < FREQ_MIN) freq = FREQ_MIN;
  if (freq > FREQ_MAX) freq = FREQ_MAX;

  stopPulses();
  lastSteps = steps;
  lastFreq  = freq;

  applyDir();
  delay(2);                            // DIR setup time pred prvým pulzom

  uint32_t halfTicks = TIMER_TICKS_PER_SEC / (2UL * freq);   // pol periódy

  noInterrupts();
  infiniteMode   = (steps == 0);
  stepsRemaining = steps;
  pulseHigh      = false;
  running        = true;
  timer1_write(halfTicks);
  timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP);
  interrupts();
}

// ---------------- HTTP handlery ----------------
void sendStatus() {
  uint32_t rem;
  bool inf, run;
  noInterrupts();
  rem = stepsRemaining; inf = infiniteMode; run = running;
  interrupts();

  String j = "{";
  j += "\"enable\":";   j += enableState ? "true" : "false";
  j += ",\"cw\":";      j += dirCW ? "true" : "false";
  j += ",\"running\":"; j += run ? "true" : "false";
  j += ",\"infinite\":";j += inf ? "true" : "false";
  j += ",\"remaining\":"; j += rem;
  j += ",\"freq\":";    j += lastFreq;
  j += "}";
  server.send(200, "application/json", j);
}

void handleRoot()   { server.send_P(200, "text/html", INDEX_HTML); }

void handleEnable() {
  enableState = server.arg("v") == "1";
  if (!enableState) stopPulses();      // pri vypnutí ENABLE zastav pulzy
  applyEnable();
  sendStatus();
}

void handleDir() {
  dirCW = server.arg("cw") == "1";
  applyDir();
  sendStatus();
}

void handleStart() {
  uint32_t steps = strtoul(server.arg("steps").c_str(), nullptr, 10);
  uint32_t freq  = strtoul(server.arg("freq").c_str(), nullptr, 10);
  startPulses(steps, freq);
  sendStatus();
}

void handleStop() {
  stopPulses();
  sendStatus();
}

// ---------------- Setup / loop ----------------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_ENABLE, OUTPUT);
  pinMode(PIN_DIR,    OUTPUT);
  pinMode(PIN_PULSE,  OUTPUT);
  digitalWrite(PIN_PULSE, LOW);
  applyEnable();                       // driver na štarte vypnutý
  applyDir();

  timer1_attachInterrupt(onTimer1);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/",           handleRoot);
  server.on("/api/status", sendStatus);
  server.on("/api/enable", handleEnable);
  server.on("/api/dir",    handleDir);
  server.on("/api/start",  handleStart);
  server.on("/api/stop",   handleStop);
  server.begin();
}

void loop() {
  server.handleClient();
}
