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

#define MS_CMD_REALTIME  0x41

// Speeduino realtime packet offsets — verify against speeduino/comms.cpp sendValues()
#define OFFSET_MAP_HI   4    // MAP kPa uint16 big-endian
#define OFFSET_MAP_LO   5
#define OFFSET_IAT      6    // intake air temp, subtract 40 for °C
#define OFFSET_CLT      7    // coolant temp, subtract 40 for °C
#define OFFSET_BATT     9    // battery * 10 (e.g. 142 = 14.2V)
#define OFFSET_RPM_HI   13
#define OFFSET_RPM_LO   14
#define OFFSET_TPS      15   // throttle position 0–100%
#define OFFSET_ADVANCE  23   // signed degrees

#define PACKET_LEN      125
#define POLL_INTERVAL_MS 100

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

int16_t g_rpm     = 0;
int8_t  g_advance = 0;
uint16_t g_map    = 0;
uint8_t  g_tps    = 0;
uint8_t  g_batt10 = 0;   // volts * 10

uint8_t pkt[PACKET_LEN];

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

bool readSpeeduinoPacket() {
    SPEEDY_SERIAL.write(MS_CMD_REALTIME);
    uint32_t start = millis();
    int idx = 0;
    while (idx < PACKET_LEN) {
        if (millis() - start > 200) return false;
        if (SPEEDY_SERIAL.available()) pkt[idx++] = SPEEDY_SERIAL.read();
    }
    return true;
}

void parsePacket() {
    g_rpm     = ((int16_t)pkt[OFFSET_RPM_HI] << 8) | pkt[OFFSET_RPM_LO];
    g_advance = (int8_t)pkt[OFFSET_ADVANCE];
    g_map     = ((uint16_t)pkt[OFFSET_MAP_HI] << 8) | pkt[OFFSET_MAP_LO];
    g_tps     = pkt[OFFSET_TPS];
    g_batt10  = pkt[OFFSET_BATT];
}

void broadcastData() {
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
    if (millis() - lastPoll >= POLL_INTERVAL_MS) {
        lastPoll = millis();
        if (readSpeeduinoPacket()) parsePacket();
        broadcastData();
        ws.cleanupClients();
    }
}
