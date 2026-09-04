# Janmashtami Jhula Controller — Setup Steps

## 1. Create a cloud MQTT broker (for remote/off-site control)
1. Sign up at HiveMQ Cloud (or EMQX Cloud / AWS IoT Core) and create a free serverless cluster.
2. In the cluster console, create credentials (username + password) — this is what both the ESP32 and the website will log in as.
3. Note down: cluster host (e.g. `xxxxxxxx.s1.eu.hivemq.cloud`), TLS port `8883` (for the ESP32), WebSocket TLS port `8884` (for the website).

## 2. Arduino IDE setup
1. Install ESP32 board support (Boards Manager) if not already installed.
2. Library Manager -> install **"MQTT" by Joel Gaehwiler** (this is the `256dpi/arduino-mqtt` library — do NOT install "PubSubClient", it doesn't support QoS 1 publish).
3. `WiFi`, `WiFiClientSecure`, `WebServer`, `ESPmDNS`, `esp_task_wdt` are all bundled with the ESP32 board package — no separate install.

## 3. Configure and flash the firmware
1. Open `janmashtami_receiver.ino`.
2. Fill in: `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_HOST`, `MQTT_USER`, `MQTT_PASS`.
3. Flash to the ESP32, then open Serial Monitor at 115200 baud. You should see:
   - WiFi connect + IP address + RSSI
   - `Local control: http://jhula1.local/`
   - `MQTT connected`
4. **Verify (hardware):** confirm your relay module's trigger polarity matches `RELAY_ACTIVE_LOW`, and that the FWD relay for each motor actually drives it up, not down (swap the two relay wires per motor if backwards — same check we did earlier).

## 4. Configure the website
1. Open `index.html`.
2. Fill in `MQTT_WSS_URL` (same cluster host, port `8884`, path `/mqtt`), `MQTT_USER`, `MQTT_PASS`.
3. Host it somewhere reachable from anywhere — GitHub Pages is free and simplest — or just open the file locally for testing.

## 5. Test the local path (on-site, lowest latency)
1. Connect your phone to the **same WiFi** as the ESP32.
2. Browse to `http://jhula1.local/` (if your phone/browser doesn't resolve `.local` names, use the IP address printed in Serial Monitor instead).
3. Press and hold a button — relay should click almost instantly (~5-20ms).

## 6. Test the remote path (off-site)
1. Open the hosted `index.html` from **cellular data**, not the venue WiFi.
2. Confirm "Live" status and the device row shows "Online" (this comes from the MQTT status/LWT topic).
3. Press a button, confirm the relay engages — expect noticeably more latency than the local path (that's the internet round trip through the broker).

## 7. Test the safety mechanisms
1. **Dead-man's switch:** hold a button, then force-close the browser tab or turn off WiFi on your phone mid-press. The motor should stop on its own within ~1 second.
2. **Interlock:** rapidly alternate UP/DOWN presses on the same motor; confirm (via multimeter or the relay's own LED) that both relays for that motor are never on at once.
3. **Watchdog:** background safety net, nothing to actively test unless you want to deliberately hang the firmware — otherwise it's invisible until it's needed.

## 8. Before the actual event
- Test WiFi signal strength at the real venue, not your workbench (`RSSI` is printed to Serial on connect).
- Keep a plain physical override switch wired independently of the ESP32, so the swing can still be operated by hand if the internet or broker goes down mid-event.
