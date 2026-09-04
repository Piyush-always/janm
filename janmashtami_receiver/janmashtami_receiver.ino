/*
  Janmashtami Jhula - Receiver (ESP32)
  Dual control path:
    - Local: ESP32 hosts its own page at http://jhula1.local/ -- lowest latency,
      works with zero internet/broker dependency when on the venue WiFi.
    - Remote: MQTT over TLS to a cloud broker (e.g. HiveMQ Cloud) -- QoS 1 publish
      (broker retries until acknowledged) and a Last Will so the broker tells
      subscribers immediately if this device's connection drops ungracefully.
  Both paths call the same setMotor() interlock -- one source of truth for
  relay safety regardless of which path issued the command.

  Required libraries (Arduino Library Manager):
    - "MQTT" by Joel Gaehwiler (256dpi/arduino-mqtt) -- NOT "PubSubClient";
      this one actually implements QoS 1 publish/ack.
  Bundled with the ESP32 board package (no separate install):
    - WiFi, WiFiClientSecure, WebServer, ESPmDNS, esp_task_wdt
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <MQTT.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_task_wdt.h>

// ---- Project config -------------------------------------------------------

const char* WIFI_SSID     = "test";
const char* WIFI_PASSWORD = "12345678";

const char* MDNS_NAME = "jhula1"; // local page at http://jhula1.local/

const char* MQTT_HOST = "46ae51f4f3d74f3ca138768beed1676d.s1.eu.hivemq.cloud";
const uint16_t MQTT_PORT = 8883; // TLS
const char* MQTT_USER = "hivemq.webclient.1788493600372";
const char* MQTT_PASS = ".y;,qk<9578EoAHjCpBD";
const char* MQTT_CLIENT_ID = "esp32-jhula1";

const char* CMD_TOPIC[2]   = { "janmashtami/motor1/cmd",   "janmashtami/motor2/cmd" };
const char* STATE_TOPIC[2] = { "janmashtami/motor1/state", "janmashtami/motor2/state" };
const char* STATUS_TOPIC   = "janmashtami/jhula1/status"; // "online" / "offline" (LWT)

// Most cheap opto-isolated relay modules trigger on LOW. Set to false if yours
// is active-HIGH (check: board LED/relay clicks when the input pin goes HIGH).
#define RELAY_ACTIVE_LOW true

// GPIOs chosen to avoid ESP32 boot-strapping pins (0,2,4,5,12,15) and
// input-only pins (34-39).
const uint8_t MOTOR1_FWD_PIN = 32; // relay 1 -> motor 1 up
const uint8_t MOTOR1_REV_PIN = 33; // relay 2 -> motor 1 down
const uint8_t MOTOR2_FWD_PIN = 25; // relay 1 -> motor 2 up
const uint8_t MOTOR2_REV_PIN = 26; // relay 2 -> motor 2 down

// Minimum time both relays for a motor must sit OFF before the new one
// energizes. Conservative margin pending your relay's datasheet release time.
const uint16_t RELAY_DEADTIME_MS = 300;

// Dead-man's switch: whichever page sent "fwd"/"rev" must keep re-sending it
// at least this often while the button is held (both local and MQTT pages do
// this). If no refresh arrives in time, the motor auto-stops. This is what
// actually guarantees the motor stops on a dropped connection -- independent
// of MQTT's LWT, which only informs the *website* that the device went dark.
const uint16_t CMD_TIMEOUT_MS = 800;

// Hardware watchdog: if loop() doesn't come back around within this many
// seconds (firmware hang), the chip force-reboots. Relays default OFF at
// boot, so a hang becomes "motors stop, device recovers" instead of "stuck
// forever until someone finds the power switch."
const uint8_t WDT_TIMEOUT_S = 8;

// ---- State -----------------------------------------------------------------

enum Direction { DIR_STOP, DIR_FWD, DIR_REV };

struct Motor {
  uint8_t fwdPin;
  uint8_t revPin;
  Direction current;
  unsigned long lastCmdMillis;
};

Motor motors[2] = {
  { MOTOR1_FWD_PIN, MOTOR1_REV_PIN, DIR_STOP, 0 },
  { MOTOR2_FWD_PIN, MOTOR2_REV_PIN, DIR_STOP, 0 },
};

WiFiClientSecure net;
MQTTClient mqtt(256);
WebServer server(80);

// ---- Relay drive ------------------------------------------------------------

static inline void relayWrite(uint8_t pin, bool energize) {
#if RELAY_ACTIVE_LOW
  digitalWrite(pin, energize ? LOW : HIGH);
#else
  digitalWrite(pin, energize ? HIGH : LOW);
#endif
}

// Break-before-make: always de-energize both relays for this motor first,
// wait out relay release time, then energize the requested one (if any).
// Called from the local HTTP handler or the MQTT message callback -- both
// run in main-loop context, so blocking delay() here is fine.
void setMotor(uint8_t motorIndex, Direction dir) {
  if (motorIndex >= 2) return;
  Motor& m = motors[motorIndex];

  relayWrite(m.fwdPin, false);
  relayWrite(m.revPin, false);

  if (dir == DIR_STOP) {
    m.current = DIR_STOP;
    Serial.print("[MOTOR] "); Serial.print(motorIndex + 1); Serial.println(" -> STOP (both relays off)");
    if (mqtt.connected()) mqtt.publish(STATE_TOPIC[motorIndex], "stop", true, 1);
    return;
  }

  delay(RELAY_DEADTIME_MS);

  if (dir == DIR_FWD) {
    relayWrite(m.fwdPin, true);
    Serial.print("[MOTOR] "); Serial.print(motorIndex + 1); Serial.println(" -> FWD (up)");
    if (mqtt.connected()) mqtt.publish(STATE_TOPIC[motorIndex], "fwd", true, 1);
  } else {
    relayWrite(m.revPin, true);
    Serial.print("[MOTOR] "); Serial.print(motorIndex + 1); Serial.println(" -> REV (down)");
    if (mqtt.connected()) mqtt.publish(STATE_TOPIC[motorIndex], "rev", true, 1);
  }
  m.current = dir;
  m.lastCmdMillis = millis();
}

void applyCommand(uint8_t motorIndex, const String& cmd) {
  if (cmd == "fwd") {
    motors[motorIndex].lastCmdMillis = millis();
    if (motors[motorIndex].current != DIR_FWD) setMotor(motorIndex, DIR_FWD);
  } else if (cmd == "rev") {
    motors[motorIndex].lastCmdMillis = millis();
    if (motors[motorIndex].current != DIR_REV) setMotor(motorIndex, DIR_REV);
  } else if (cmd == "stop") {
    setMotor(motorIndex, DIR_STOP);
  }
}

// ---- Local HTTP control (on-site, no cloud dependency) ----------------------

const char LOCAL_PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Jhula Local Control</title>
<style>
body{font-family:sans-serif;text-align:center;background:#0b1b3a;color:#f4f1e8}
.motors{display:flex;gap:16px;justify-content:center;margin-top:24px}
button{width:92px;height:80px;font-size:20px;margin:8px;border-radius:14px;border:1px solid #24356b;background:#1a2f5c;color:#f4f1e8}
button:active{background:#d4af37;color:#0b1b3a}
h2{color:#d4af37}
#log{margin:16px auto 0;max-width:340px;height:120px;overflow-y:auto;background:#0e1d40;border:1px solid #24356b;border-radius:10px;padding:8px 12px;text-align:left;font-family:monospace;font-size:0.72rem}
#log div{padding:2px 0;border-bottom:1px solid rgba(255,255,255,0.05)}
.ok{color:#22c55e}.err{color:#ef4444}
</style></head><body>
<h2>Jhula Local Control</h2>
<p>Direct on-site link -- no internet needed</p>
<div class="motors">
<div><button data-m="1" data-d="fwd">M1 UP</button><br><button data-m="1" data-d="rev">M1 DOWN</button></div>
<div><button data-m="2" data-d="fwd">M2 UP</button><br><button data-m="2" data-d="rev">M2 DOWN</button></div>
</div>
<div id="log"></div>
<script>
let hb=null;
function log(msg, cls){
  var el=document.getElementById('log');
  var t=new Date().toLocaleTimeString([],{hour12:false});
  var d=document.createElement('div');
  d.className=cls||'';
  d.textContent='['+t+'] '+msg;
  el.prepend(d);
  while (el.children.length>40) el.removeChild(el.lastChild);
}
function send(m,d){
  fetch('/cmd?m='+m+'&d='+d)
    .then(r=>{ if(r.ok) log('M'+m+' '+d+' -> ok','ok'); else log('M'+m+' '+d+' -> HTTP '+r.status,'err'); })
    .catch(e=>log('M'+m+' '+d+' -> FAILED: '+e.message,'err'));
}
function start(m,d){log('M'+m+' '+d+' pressed'); send(m,d);clearInterval(hb);hb=setInterval(()=>send(m,d),250);}
function stop(m){log('M'+m+' released'); clearInterval(hb);send(m,'stop');}
document.querySelectorAll('button').forEach(function(b){
  var m=b.dataset.m, d=b.dataset.d;
  b.addEventListener('mousedown', e=>{e.preventDefault();start(m,d);});
  b.addEventListener('touchstart', e=>{e.preventDefault();start(m,d);});
  b.addEventListener('mouseup', e=>{e.preventDefault();stop(m);});
  b.addEventListener('mouseleave', e=>{e.preventDefault();stop(m);});
  b.addEventListener('touchend', e=>{e.preventDefault();stop(m);});
  b.addEventListener('touchcancel', e=>{e.preventDefault();stop(m);});
});
log('Loaded. Press a button to test.');
</script></body></html>
)HTML";

void handleLocalRoot() {
  server.send_P(200, "text/html", LOCAL_PAGE);
}

void handleLocalCmd() {
  if (!server.hasArg("m") || !server.hasArg("d")) {
    Serial.println("[LOCAL] rejected: missing m/d");
    server.send(400, "text/plain", "missing m/d"); return;
  }
  int motorIndex = server.arg("m").toInt() - 1;
  String d = server.arg("d");
  if (motorIndex != 0 && motorIndex != 1) {
    Serial.print("[LOCAL] rejected: bad motor index "); Serial.println(motorIndex + 1);
    server.send(400, "text/plain", "bad m"); return;
  }
  Serial.print("[LOCAL] received m="); Serial.print(motorIndex + 1); Serial.print(" d="); Serial.println(d);
  applyCommand((uint8_t)motorIndex, d);
  server.send(200, "text/plain", "ok");
}

// ---- MQTT (remote/cloud path) ------------------------------------------------

void mqttMessageReceived(String &topic, String &payload) {
  int motorIndex = -1;
  for (int i = 0; i < 2; i++) {
    if (topic == CMD_TOPIC[i]) { motorIndex = i; break; }
  }
  if (motorIndex < 0) {
    Serial.print("[MQTT] ignored unknown topic: "); Serial.println(topic);
    return;
  }
  Serial.print("[MQTT] received m="); Serial.print(motorIndex + 1); Serial.print(" d="); Serial.println(payload);
  applyCommand((uint8_t)motorIndex, payload);
}

void mqttConnect() {
  mqtt.setWill(STATUS_TOPIC, "offline", true, 1);
  while (!mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
    Serial.print("MQTT connect failed, lastError=");
    Serial.print((int)mqtt.lastError());
    Serial.print(" returnCode=");
    Serial.println((int)mqtt.returnCode());
    esp_task_wdt_reset(); // avoid a watchdog reset during a slow broker retry
    delay(2000);
  }
  Serial.println("MQTT connected");
  mqtt.subscribe(CMD_TOPIC[0], 1);
  mqtt.subscribe(CMD_TOPIC[1], 1);
  mqtt.publish(STATUS_TOPIC, "online", true, 1);
}

// ---- Setup / loop --------------------------------------------------------

void setup() {
  Serial.begin(115200);

  const uint8_t pins[4] = { MOTOR1_FWD_PIN, MOTOR1_REV_PIN, MOTOR2_FWD_PIN, MOTOR2_REV_PIN };
  for (uint8_t p : pins) {
    pinMode(p, OUTPUT);
    relayWrite(p, false);
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP address: "); Serial.println(WiFi.localIP());
  Serial.print("RSSI: "); Serial.println(WiFi.RSSI());

  if (MDNS.begin(MDNS_NAME)) {
    Serial.print("Local control (try this first): http://"); Serial.print(MDNS_NAME); Serial.println(".local/");
  } else {
    Serial.println("mDNS failed to start");
  }
  // .local hostnames are unreliable on many Android browsers -- this IP
  // works everywhere regardless of mDNS support.
  Serial.print("Local control (always works): http://"); Serial.print(WiFi.localIP()); Serial.println("/");

  server.on("/", handleLocalRoot);
  server.on("/cmd", handleLocalCmd);
  server.begin();

  // Simplification to get running: skips broker certificate verification
  // (TLS is still encrypted, but the ESP32 won't detect a spoofed broker).
  // For production, replace with net.setCACert(...) using your broker's CA.
  net.setInsecure();

  mqtt.begin(MQTT_HOST, MQTT_PORT, net);
  mqtt.onMessage(mqttMessageReceived);
  // Default socket timeout in this library is short enough that a slow TLS
  // round-trip to a cloud broker can look like a dead connection and get
  // dropped right after connecting. keepAlive=60s, cleanSession=true,
  // socket timeout=5000ms.
  mqtt.setOptions(60, true, 5000);
  mqttConnect();

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();

  server.handleClient();
  mqtt.loop();
  if (!mqtt.connected()) {
    Serial.println("[MQTT] disconnected, reconnecting...");
    mqttConnect();
  }

  // Dead-man's switch: auto-stop any motor whose last "fwd"/"rev" command
  // (from either the local page or MQTT) wasn't refreshed in time.
  unsigned long now = millis();
  for (uint8_t i = 0; i < 2; i++) {
    if (motors[i].current != DIR_STOP && (now - motors[i].lastCmdMillis) > CMD_TIMEOUT_MS) {
      Serial.print("[SAFETY] motor "); Serial.print(i + 1); Serial.println(" dead-man's switch timeout -> auto-stop");
      setMotor(i, DIR_STOP);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[SAFETY] WiFi lost -> auto-stop both motors");
    setMotor(0, DIR_STOP);
    setMotor(1, DIR_STOP);
  }
}
