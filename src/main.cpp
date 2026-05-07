#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>

const char* WIFI_SSID = "Penny";
const char* WIFI_PASS = "foxglove2017";

#define SPEEDY_SERIAL Serial2
#define SPEEDY_BAUD   115200
#define SPEEDY_RX     27   // A2 pad on QT Py ESP32 Pico — receives from Speeduino
#define SPEEDY_TX     32   // unused TX pad

// Secondary serial 'r' command protocol
#define SPEEDY_CANID    0x00
#define SPEEDY_RCMD     0x30   // read command type

// Realtime data offsets (from Speeduino wiki / SpeedData.cpp)
#define OFF_MAP         4    // 2 bytes, MAP kPa
#define OFF_CLT         7    // 1 byte, coolant temp (raw + 40 = °C)
#define OFF_BATT        9    // 1 byte, volts * 10
#define OFF_RPM        14    // 2 bytes, RPM
#define OFF_TPS        24    // 1 byte, throttle %
#define OFF_ADVANCE    23    // 1 byte, timing advance

#define POLL_INTERVAL_MS 150
#define BROADCAST_MS     250   // send to browser at ~4 Hz (gauges can't keep up faster)
#define POLL_BACKOFF_MS  2000  // slow down when Speeduino isn't responding

bool g_speeduinoOnline = false;

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

int16_t g_rpm     = 0;
int8_t  g_advance = 0;
uint16_t g_map    = 0;
uint8_t  g_tps    = 0;
uint8_t  g_batt10 = 0;   // volts * 10

// Request a value from Speeduino secondary serial using 'r' command
// Returns number of data bytes received (0 on failure), fills resp[]
int speeduinoRequest(uint16_t offset, uint16_t len, uint8_t* resp) {
    uint8_t req[7] = {
        0x72,                          // 'r' command
        SPEEDY_CANID,                  // CAN ID
        SPEEDY_RCMD,                   // command type 0x30
        (uint8_t)(offset & 0xFF),      // offset LSB
        (uint8_t)(offset >> 8),        // offset MSB
        (uint8_t)(len & 0xFF),         // length LSB
        (uint8_t)(len >> 8)            // length MSB
    };

    // Drain any leftover bytes
    while (SPEEDY_SERIAL.available()) SPEEDY_SERIAL.read();

    SPEEDY_SERIAL.write(req, 7);

    // Wait for response: 'r' confirmation + type byte + data bytes
    int expected = 2 + len;
    uint8_t buf[8];
    uint32_t start = millis();
    int idx = 0;
    while (idx < expected) {
        if (millis() - start > 100) {
            Serial.printf("[REQ ] offset=%d len=%d TIMEOUT got %d/%d bytes\n", offset, len, idx, expected);
            if (idx > 0) {
                Serial.print("       raw: ");
                for (int i = 0; i < idx; i++) Serial.printf("%02X ", buf[i]);
                Serial.println();
            }
            return 0;
        }
        if (SPEEDY_SERIAL.available()) buf[idx++] = SPEEDY_SERIAL.read();
    }

    if (buf[0] != 0x72) {
        Serial.printf("[REQ ] offset=%d — bad confirm byte: 0x%02X (expected 0x72)\n", offset, buf[0]);
        return 0;
    }

    // Copy data bytes (skip confirmation + type)
    for (int i = 0; i < (int)len; i++) resp[i] = buf[2 + i];
    return len;
}

static const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Speeduino Dashboard</title>
  <script src="https://cdn.jsdelivr.net/npm/canvas-gauges@2.1.7/gauge.min.js"></script>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: #111;
      color: #eee;
      font-family: sans-serif;
      display: flex;
      flex-direction: column;
      align-items: center;
      min-height: 100vh;
      padding: 20px;
    }
    h1 { margin-bottom: 24px; font-size: 1.3rem; letter-spacing: 3px; color: #666; }
    #row1, #row2 { display: flex; flex-wrap: wrap; gap: 24px; justify-content: center; margin-bottom: 24px; }
    .gauge-wrap { display: flex; flex-direction: column; align-items: center; gap: 6px; }
    .gauge-wrap.large canvas { width: 220px !important; height: 220px !important; }
    .gauge-wrap.small canvas { width: 160px !important; height: 160px !important; }
    .gauge-label { font-size: .75rem; letter-spacing: 2px; color: #888; text-transform: uppercase; }
    .gauge-value { font-size: 1.4rem; font-weight: bold; color: #fff; min-width: 70px; text-align: center; }
    .gauge-wrap.small .gauge-value { font-size: 1.1rem; }
    #batt-wrap { display: flex; flex-direction: column; align-items: center; gap: 6px; }
    #batt-bar-bg {
      width: 140px; height: 18px; background: #222; border-radius: 9px; overflow: hidden;
    }
    #batt-bar { height: 100%; width: 0%; background: #4c4; border-radius: 9px; transition: width .2s; }
    #batt-val { font-size: 1.1rem; font-weight: bold; color: #fff; }
    #batt-label { font-size: .75rem; letter-spacing: 2px; color: #888; text-transform: uppercase; }
    #status { font-size: .7rem; color: #444; }
    #status.connected { color: #4c4; }
    #status.error { color: #c44; }
  </style>
</head>
<body>
  <h1>SPEEDUINO DASH</h1>

  <div id="row1">
    <div class="gauge-wrap large">
      <span class="gauge-label">RPM</span>
      <canvas id="gaugeRpm"></canvas>
      <span class="gauge-value" id="valRpm">0</span>
    </div>
    <div class="gauge-wrap large">
      <span class="gauge-label">Advance</span>
      <canvas id="gaugeAdv"></canvas>
      <span class="gauge-value" id="valAdv">0&deg;</span>
    </div>
  </div>

  <div id="row2">
    <div class="gauge-wrap small">
      <span class="gauge-label">MAP</span>
      <canvas id="gaugeMap"></canvas>
      <span class="gauge-value" id="valMap">0 kPa</span>
    </div>
    <div class="gauge-wrap small">
      <span class="gauge-label">TPS</span>
      <canvas id="gaugeTps"></canvas>
      <span class="gauge-value" id="valTps">0%</span>
    </div>
    <div id="batt-wrap">
      <span id="batt-label">Battery</span>
      <div id="batt-bar-bg"><div id="batt-bar"></div></div>
      <span id="batt-val">-.-- V</span>
    </div>
  </div>

  <div id="status">connecting...</div>

  <script>
    const BASE = {
      colorPlate:'#1a1a1a', colorMajorTicks:'#ccc', colorMinorTicks:'#555',
      colorNumbers:'#ccc', valueBox:false, animationDuration:80, animationRule:'linear',
      strokeTicks:true
    };

    const gaugeRpm = new RadialGauge(Object.assign({}, BASE, {
      renderTo:'gaugeRpm', width:220, height:220, units:'RPM',
      minValue:0, maxValue:7000,
      majorTicks:['0','1000','2000','3000','4000','5000','6000','7000'],
      minorTicks:4,
      highlights:[{from:5500, to:7000, color:'rgba(200,0,0,.75)'}],
      colorNeedle:'rgba(240,128,0,1)', colorNeedleEnd:'rgba(200,80,0,.9)'
    })).draw();

    const gaugeAdv = new RadialGauge(Object.assign({}, BASE, {
      renderTo:'gaugeAdv', width:220, height:220, units:'Degrees',
      minValue:-10, maxValue:45,
      majorTicks:['-10','0','10','20','30','40','45'],
      minorTicks:2,
      highlights:[{from:35, to:45, color:'rgba(200,120,0,.6)'}],
      colorNeedle:'rgba(0,180,240,1)', colorNeedleEnd:'rgba(0,120,200,.9)'
    })).draw();

    const gaugeMap = new RadialGauge(Object.assign({}, BASE, {
      renderTo:'gaugeMap', width:160, height:160, units:'kPa',
      minValue:0, maxValue:120,
      majorTicks:['0','20','40','60','80','100','120'],
      minorTicks:2,
      highlights:[{from:100, to:120, color:'rgba(200,120,0,.5)'}],
      colorNeedle:'rgba(160,220,80,1)', colorNeedleEnd:'rgba(100,180,40,.9)'
    })).draw();

    const gaugeTps = new RadialGauge(Object.assign({}, BASE, {
      renderTo:'gaugeTps', width:160, height:160, units:'%',
      minValue:0, maxValue:100,
      majorTicks:['0','25','50','75','100'],
      minorTicks:4,
      highlights:[{from:85, to:100, color:'rgba(200,80,0,.5)'}],
      colorNeedle:'rgba(200,160,40,1)', colorNeedleEnd:'rgba(180,120,20,.9)'
    })).draw();

    const status  = document.getElementById('status');
    const valRpm  = document.getElementById('valRpm');
    const valAdv  = document.getElementById('valAdv');
    const valMap  = document.getElementById('valMap');
    const valTps  = document.getElementById('valTps');
    const battVal = document.getElementById('batt-val');
    const battBar = document.getElementById('batt-bar');

    function battColor(v) {
      if (v < 11.5) return '#c44';
      if (v < 12.5) return '#ca4';
      return '#4c4';
    }

    function connect() {
      const ws = new WebSocket('ws://' + location.host + '/ws');
      ws.onopen  = () => { status.textContent = 'connected'; status.className = 'connected'; };
      ws.onclose = () => { status.textContent = 'disconnected — retrying...'; status.className = 'error'; setTimeout(connect, 2000); };
      ws.onerror = () => { status.textContent = 'connection error'; status.className = 'error'; };
      ws.onmessage = (e) => {
        const d = JSON.parse(e.data);
        gaugeRpm.value = d.rpm;   valRpm.textContent = d.rpm;
        gaugeAdv.value = d.adv;   valAdv.textContent = d.adv + '°';
        gaugeMap.value = d.map;   valMap.textContent = d.map + ' kPa';
        gaugeTps.value = d.tps;   valTps.textContent = d.tps + '%';
        const v = d.batt / 10.0;
        battVal.textContent = v.toFixed(1) + ' V';
        const pct = Math.min(100, Math.max(0, (v - 10) / 6 * 100));
        battBar.style.width = pct + '%';
        battBar.style.background = battColor(v);
      };
    }
    connect();
  </script>
</body>
</html>
)rawhtml";

bool pollSpeeduino() {
    uint8_t resp[2];
    int ok = 0;

    // RPM — 2 bytes at offset 14
    if (speeduinoRequest(OFF_RPM, 2, resp)) {
        g_rpm = (resp[1] << 8) | resp[0];  // little-endian
        ok++;
    }
    // MAP — 2 bytes at offset 4
    if (speeduinoRequest(OFF_MAP, 2, resp)) {
        g_map = (resp[1] << 8) | resp[0];
        ok++;
    }
    // Battery — 1 byte at offset 9
    if (speeduinoRequest(OFF_BATT, 1, resp)) {
        g_batt10 = resp[0];
        ok++;
    }
    // TPS — 1 byte at offset 24
    if (speeduinoRequest(OFF_TPS, 1, resp)) {
        g_tps = resp[0];
        ok++;
    }
    // Advance — 1 byte at offset 23
    if (speeduinoRequest(OFF_ADVANCE, 1, resp)) {
        g_advance = (int8_t)resp[0];
        ok++;
    }

    Serial.printf("[POLL ] %d/5 OK — RPM=%d MAP=%d BATT=%d(%.1fV) TPS=%d ADV=%d\n",
        ok, g_rpm, g_map, g_batt10, g_batt10 / 10.0, g_tps, g_advance);

    return ok > 0;
}

void broadcastData() {
    static uint32_t lastBcast = 0;
    if (!ws.count()) return;
    if (millis() - lastBcast < BROADCAST_MS) return;
    lastBcast = millis();
    char buf[96];
    snprintf(buf, sizeof(buf),
        "{\"rpm\":%d,\"adv\":%d,\"map\":%d,\"tps\":%d,\"batt\":%d}",
        g_rpm, g_advance, g_map, g_tps, g_batt10);
    ws.textAll(buf);
}

void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType, void*, uint8_t*, size_t) {}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\nSpeeduino dash starting...");

    SPEEDY_SERIAL.begin(SPEEDY_BAUD, SERIAL_8N1, SPEEDY_RX, SPEEDY_TX);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to WiFi");
    uint32_t wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
        delay(500);
        Serial.print('.');
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nIP: http://%s/\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.printf("\nWiFi failed. Status=%d\n", (int)WiFi.status());
    }

    // Drain and log any stray bytes already in the RX buffer
    int stray = 0;
    while (SPEEDY_SERIAL.available()) { SPEEDY_SERIAL.read(); stray++; }
    Serial.printf("Drained %d stray bytes from Speeduino serial\n", stray);
    Serial.printf("Speeduino serial: RX=%d TX=%d @ %d baud\n", SPEEDY_RX, SPEEDY_TX, SPEEDY_BAUD);

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", INDEX_HTML);
    });
    server.begin();
    Serial.println("Server started.");
}

void loop() {
    static uint32_t lastPoll = 0;
    uint32_t interval = g_speeduinoOnline ? POLL_INTERVAL_MS : POLL_BACKOFF_MS;
    if (millis() - lastPoll >= interval) {
        lastPoll = millis();
        g_speeduinoOnline = pollSpeeduino();
        if (g_speeduinoOnline) broadcastData();
        ws.cleanupClients();
    }
    yield();
}
