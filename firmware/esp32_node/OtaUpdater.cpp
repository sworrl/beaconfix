#include "OtaUpdater.h"
#include "WiringSvg.h"

void OtaUpdater::setupWebEndpoints() {
    // ── 1. Web Portal HTML Dashboard ──────────────────────────────────────────
    m_webServer->on("/", HTTP_GET, [this]() {
        const char* nodeName = NodeConfig::instance().getName();
        uint32_t mv = BatteryMonitor::instance().getMilliVolts();
        uint8_t pct = BatteryMonitor::instance().getPercentage();
        const char* battState = BatteryMonitor::instance().getStateStr();
        bool hasBatt = BatteryMonitor::instance().hasBattery();

        String html = "<!DOCTYPE html><html lang='en'><head><meta charset='UTF-8'>"
                      "<title>BeaconFix Node - " + String(nodeName) + "</title>"
                      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                      "<style>"
                      ":root{--bg:#0f172a;--panel:#1e293b;--accent:#00e5ff;--accent2:#f59e0b;--danger:#ef4444;--success:#10b981;--text:#f8fafc;--sub:#94a3b8;--border:#334155}"
                      "*{box-sizing:border-box;margin:0;padding:0}body{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--text);padding:16px;line-height:1.5}"
                      ".container{max-width:850px;margin:auto}"
                      ".header{display:flex;flex-wrap:wrap;align-items:center;justify-content:space-between;border-bottom:2px solid var(--border);padding-bottom:16px;margin-bottom:20px}"
                      ".header h1{font-size:1.6rem;color:var(--accent);display:flex;align-items:center;gap:8px}"
                      ".badge{font-size:0.75rem;background:#0369a1;color:#fff;padding:3px 8px;border-radius:12px;font-weight:600}"
                      ".card{background:var(--panel);border:1px solid var(--border);border-radius:12px;padding:20px;margin-bottom:20px;box-shadow:0 4px 6px -1px rgba(0,0,0,0.3)}"
                      ".card h2{font-size:1.2rem;margin-bottom:12px;display:flex;align-items:center;gap:8px}"
                      ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:12px;margin-bottom:16px}"
                      ".stat-box{background:#0b1120;padding:12px;border-radius:8px;border-left:4px solid var(--accent)}"
                      ".stat-title{font-size:0.75rem;color:var(--sub);text-transform:uppercase}"
                      ".stat-val{font-size:1.2rem;font-weight:bold;margin-top:2px}"
                      "input,select,button{font-family:inherit;font-size:0.95rem;padding:10px 14px;border-radius:6px;border:1px solid var(--border);background:#0b1120;color:var(--text);outline:none}"
                      "button{background:var(--accent);color:#020617;font-weight:bold;cursor:pointer;border:none;transition:0.2s}"
                      "button:hover{filter:brightness(1.15)}"
                      ".btn-secondary{background:var(--border);color:var(--text)}"
                      ".btn-danger{background:var(--danger);color:#fff}"
                      ".form-row{display:flex;gap:10px;margin-top:10px;flex-wrap:wrap}"
                      "table{width:100%;border-collapse:collapse;margin:12px 0;font-size:0.9rem}"
                      "th,td{padding:10px;text-align:left;border-bottom:1px solid var(--border)}"
                      "th{background:#0b1120;color:var(--sub)}"
                      ".safe{color:var(--success);font-weight:bold}"
                      ".danger{color:var(--danger);font-weight:bold}"
                      ".diagram-box{background:#090d16;padding:16px;border-radius:8px;border:1px dashed var(--border);font-family:monospace;white-space:pre;overflow-x:auto;font-size:0.85rem;color:#38bdf8;line-height:1.35;margin-top:10px}"
                      "</style></head><body><div class='container'>"
                      "<div class='header'><div><h1>📡 " + String(nodeName) + " <span class='badge'>v2.4 Signed</span></h1>"
                      "<p style='color:var(--sub);font-size:0.85rem;margin-top:4px'>IP: " + WiFi.softAPIP().toString() + " | Monitor Mode Promiscuous Sniffer & Anchor Node</p></div></div>";

        // Card 1: Node Telemetry & Reddit Naming
        html += "<div class='card'><h2>⚡ Telemetry & Reddit-Style Node Name</h2>"
                "<div class='grid'>"
                "<div class='stat-box'><div class='stat-title'>Battery Level</div><div class='stat-val'>" +
                (hasBatt ? (String(pct) + "% (" + String(mv) + " mV)") : "<span style='color:var(--accent)'>USB Powered (No Batt)</span>") +
                "</div></div>"
                "<div class='stat-box'><div class='stat-title'>Power State</div><div class='stat-val' style='text-transform:capitalize'>" + String(battState) + "</div></div>"
                "<div class='stat-box'><div class='stat-title'>Free Memory</div><div class='stat-val'>" + String(ESP.getFreeHeap() / 1024) + " KB</div></div>"
                "<div class='stat-box'><div class='stat-title'>Wi-Fi Monitor</div><div class='stat-val'>Ch " + String(WifiMonitor::instance().getChannel()) + " (" + String(WifiMonitor::instance().getPps()) + " pps)</div></div>"
                "</div>"
                "<form action='/config/name' method='POST' class='form-row'>"
                "<input type='text' name='name' value='" + String(nodeName) + "' placeholder='Custom Node Name' maxlength='32' style='flex:1' required>"
                "<button type='submit'>💾 Save Name</button>"
                "<button type='button' class='btn-secondary' onclick=\"location.href='/config/name?reset=1'\">🎲 Reddit Auto-Name</button>"
                "</form></div>";

        // Card 1.5: Battery Diagnostics & Troubleshooter ("Why can't the ESP see my battery?")
        uint32_t rawMv = BatteryMonitor::instance().getRawMilliVolts();
        const char* diagVerdict = BatteryMonitor::instance().getDiagnosticVerdict();
        const char* diagAdvice = BatteryMonitor::instance().getDiagnosticAdvice();

        html += "<div class='card'><h2>🔍 Battery Diagnostics & Troubleshooter</h2>"
                "<div style='background:#0b1120;padding:14px;border-radius:8px;margin-bottom:12px;border-left:4px solid " +
                String(hasBatt ? "var(--success)" : "var(--accent)") + "'>"
                "<div style='display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px'>"
                "<div><b>Diagnostic Status:</b> <span class='badge' style='background:" +
                String(hasBatt ? "var(--success)" : "var(--accent)") + ";color:#020617;font-weight:bold'>" + String(diagVerdict) + "</span></div>"
                "<div style='font-family:monospace;font-size:0.85rem'>ADC Pin " + String(BatteryMonitor::instance().getPin()) + ": raw " + String(rawMv) + " mV | calc " + String(mv) + " mV (ratio " + String(BatteryMonitor::instance().getDivider(), 1) + "x)</div>"
                "</div>"
                "<p style='color:var(--text);font-size:0.9rem;margin-top:8px'><b>Recommendation:</b> " + String(diagAdvice) + "</p>"
                "</div>"

                "<details style='background:#0b1120;padding:12px;border-radius:8px;font-size:0.85rem;color:var(--sub)'>"
                "<summary style='cursor:pointer;font-weight:bold;color:var(--accent)'>❓ The ESP can't see my battery — What should I check?</summary>"
                "<ul style='margin:10px 0 0 18px;line-height:1.6'>"
                "<li><b>1. Common Ground (GND):</b> The ESP32 GND and TP4056 GND <i>must</i> be connected together. If grounds are separated, the ADC floats near 0V.</li>"
                "<li><b>2. Resistor Divider on GPIO 35:</b> Verify resistor 1 (100kΩ) is between Battery (+) and GPIO 35, and resistor 2 (100kΩ) is between GPIO 35 and GND.</li>"
                "<li><b>3. Recovered Vape Cell Deep Sleep:</b> If a recovered vape battery was left flat for days, its protection chip trips (<2.5V). Plug the TP4056 module into USB-C for 15 minutes to awaken the cell.</li>"
                "<li><b>4. Running on USB Only:</b> If you are intentionally powering the node via USB 5V without a battery attached, this is completely normal! The LED will play a quiet dual-tick instead of alarming.</li>"
                "</ul></details>"
                "<details " + String(hasBatt ? "" : "open ") + "style='background:#0b1120;padding:12px;border-radius:8px;margin-top:10px;font-size:0.85rem;color:var(--sub)'>"
                "<summary style='cursor:pointer;font-weight:bold;color:var(--accent)'>Wiring diagram: cell, TP4056 and the 100k/100k divider</summary>"
                "<div style='margin-top:10px;overflow-x:auto'><img src='/wiring.svg' alt='ESP32 battery wiring' loading='lazy' style='width:100%;min-width:640px;background:#fff;border-radius:8px'></div></details></div>";

        // Card 1.8: Home Wi-Fi Gateway Link
        bool staConn = (WiFi.status() == WL_CONNECTED);
        String savedSsid = NodeConfig::instance().getWifiSsid();
        html += "<div class='card'><h2>🌐 Home Wi-Fi & Gateway Uplink</h2>"
                "<div style='background:#0b1120;padding:14px;border-radius:8px;margin-bottom:12px;border-left:4px solid " +
                String(staConn ? "var(--success)" : "var(--accent)") + "'>"
                "<div style='display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px'>"
                "<div><b>Status:</b> " +
                String(staConn ? ("<span class='badge' style='background:var(--success);color:#020617;font-weight:bold'>Connected to " + WiFi.SSID() + " (" + String(WiFi.RSSI()) + " dBm)</span>") : "<span class='badge' style='background:var(--border);color:#fff'>Not Connected</span>") +
                "</div>"
                "<div style='font-family:monospace;font-size:0.85rem'>" +
                (staConn ? ("Home IP: " + WiFi.localIP().toString()) : (savedSsid.length() > 0 ? ("Configured: " + savedSsid) : "No network saved")) +
                "</div></div></div>"
                "<form action='/config/wifi' method='POST' class='form-row'>"
                "<input type='text' name='ssid' value='" + savedSsid + "' placeholder='Home Wi-Fi SSID' maxlength='32' style='flex:1' required>"
                "<input type='password' name='pass' placeholder='Password (or blank)' maxlength='64' style='flex:1'>"
                "<button type='submit'>🔗 Connect & Save</button>" +
                (savedSsid.length() > 0 ? "<button type='button' class='btn-danger' onclick=\"location.href='/config/wifi/clear'\">Disconnect & Clear</button>" : "") +
                "</form></div>";

        // Card 1.85: Station Role (Base Station vs Mobile)
        bool isBase = NodeConfig::instance().isBaseStation();
        html += "<div class='card'><h2>📡 Station Role: Base Station vs Mobile Node</h2>"
                "<div style='background:#0b1120;padding:14px;border-radius:8px;margin-bottom:12px;border-left:4px solid " +
                String(isBase ? "var(--accent)" : "var(--accent2)") + "'>"
                "<div style='display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px'>"
                "<div><b>Current Role:</b> <span class='badge' style='background:" +
                String(isBase ? "var(--accent)" : "var(--accent2)") + ";color:#020617;font-weight:bold'>" +
                String(isBase ? "🏠 BASE STATION (Root Gateway)" : "🚗 MOBILE NODE (Portable / Field)") + "</span> " +
                "<span style='color:var(--sub);font-size:0.85rem'>Mesh Hops: " +
                String(MeshEngine::instance().getHopsToGateway()) + "</span></div>"
                "<div style='display:flex;gap:6px'>" +
                String(!isBase ? "<button type='button' style='background:var(--accent);color:#020617;font-weight:bold' onclick=\"location.href='/config/mode?mode=base'\">🏠 Set as Base Station</button>" :
                                 "<button type='button' style='background:var(--accent2);color:#020617;font-weight:bold' onclick=\"location.href='/config/mode?mode=mobile'\">🚗 Set as Mobile Node</button>") +
                "</div></div></div>"
                "<div style='display:grid;grid-template-columns:1fr 1fr;gap:12px;font-size:0.85rem;color:var(--sub)'>"
                "<div style='background:#0f172a;padding:10px;border-radius:6px;border:1px solid " + String(isBase ? "var(--accent)" : "var(--border)") + "'>"
                "<b style='color:#fff'>🏠 Base Station Mode</b><br>"
                "Local to permanent desktop/server connection. Functions as Root Gateway (0 hops), Stratum 1 Master clock, relays all mobile telemetry to host."
                "</div>"
                "<div style='background:#0f172a;padding:10px;border-radius:6px;border:1px solid " + String(!isBase ? "var(--accent2)" : "var(--border)") + "'>"
                "<b style='color:#fff'>🚗 Mobile Node Mode</b><br>"
                "Field/portable node on vape battery. Store-and-forward caching when out of range, meshes back to Base Station."
                "</div></div>"
                "<p style='color:var(--sub);font-size:0.8rem;margin-top:10px'>Commands: <code>mode base</code>, <code>mode mobile</code>, <code>mode toggle</code>.</p>"
                "</div>";

        // Card 2: External LED Slots Configuration
        html += "<div class='card'><h2>💡 External LED Configuration (Satellite & Dedicated Gauges)</h2>"
                "<p style='color:var(--sub);font-size:0.9rem;margin-bottom:12px'>Configure single-color or 4-pin RGB LEDs. Use <b>Mirror</b> for an external case LED (e.g. waterproof enclosure), or assign a <b>Dedicated Full-Time Battery Gauge</b> that never timeshares with network packets.</p>";

        for (int i = 1; i < MAX_LED_SLOTS; i++) {
            const LedSlot& s = LedPatterns::instance().getSlot(i);
            html += "<div style='background:#0b1120;padding:14px;border-radius:8px;margin-bottom:10px;border-left:4px solid " +
                    String(s.enabled ? "var(--success)" : "var(--border)") + "'>"
                    "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:8px'>"
                    "<b>Slot " + String(i) + " " + (s.enabled ? ("(" + String(s.type == LED_TYPE_SINGLE ? "Single LED" : "4-Pin RGB") + ")") : "<span style='color:var(--sub)'>(Empty)</span>") + "</b>" +
                    (s.enabled ? "<a href='/config/led/clear?slot=" + String(i) + "' style='color:var(--danger);font-size:0.8rem;text-decoration:none;font-weight:bold'>[Delete Slot]</a>" : "") +
                    "</div>";

            html += "<form action='/config/led' method='POST' style='display:flex;flex-wrap:wrap;gap:8px;align-items:center'>"
                    "<input type='hidden' name='slot' value='" + String(i) + "'>"
                    "<select name='type' onchange=\"this.form.querySelectorAll('.rgb-only').forEach(e=>e.style.display=this.value==='rgb'?'inline-block':'none')\">"
                    "<option value='single'" + String(s.type == LED_TYPE_SINGLE ? " selected" : "") + ">Single Color</option>"
                    "<option value='rgb'" + String(s.type == LED_TYPE_RGB ? " selected" : "") + ">4-Pin RGB</option>"
                    "</select>"
                    "<input type='number' name='pinR' value='" + String(s.pinR ? s.pinR : 4) + "' min='2' max='33' style='width:80px' placeholder='Pin R/S' title='Safe GPIO pin' required>"
                    "<span class='rgb-only' style='display:" + String(s.type == LED_TYPE_RGB ? "inline-block" : "none") + "'>"
                    "<input type='number' name='pinG' value='" + String(s.pinG ? s.pinG : 16) + "' min='2' max='33' style='width:75px' placeholder='Pin G'>"
                    "<input type='number' name='pinB' value='" + String(s.pinB ? s.pinB : 17) + "' min='2' max='33' style='width:75px;margin-left:8px' placeholder='Pin B'>"
                    "</span>"
                    "<select name='role'>"
                    "<option value='0'" + String(s.role == ROLE_MIRROR ? " selected" : "") + ">Augment/Mirror Board (Waterproof Case)</option>"
                    "<option value='1'" + String(s.role == ROLE_BATTERY_ONLY ? " selected" : "") + ">Full-Time Battery Gauge (Dedicated)</option>"
                    "<option value='2'" + String(s.role == ROLE_SNIFFER_ONLY ? " selected" : "") + ">Wi-Fi Traffic Sniffer Strobe</option>"
                    "<option value='3'" + String(s.role == ROLE_TRACKER_ONLY ? " selected" : "") + ">BLE Tracker / AirTag Alarm</option>"
                    "<option value='4'" + String(s.role == ROLE_ALERT_ONLY ? " selected" : "") + ">Deauth Security Alert Strobe</option>"
                    "<option value='5'" + String(s.role == ROLE_OFF ? " selected" : "") + ">Disabled</option>"
                    "</select>"
                    "<label style='font-size:0.8rem;color:var(--sub);display:flex;align-items:center;gap:4px'>"
                    "<input type='checkbox' name='inv' value='1'" + String(s.inverted ? " checked" : "") + "> Invert / Common Anode</label>"
                    "<button type='submit' style='padding:8px 14px;margin-left:auto'>Apply</button>"
                    "</form></div>";
        }
        html += "</div>";

        // Card 3: Hardware Wiring Guide & Safe Pinout Matrix
        html += "<div class='card'><h2>📐 Hardware Wiring Guide & Pin Safety Matrix</h2>"
                "<p style='color:var(--sub);font-size:0.9rem'>Follow this strictly when soldering or breadboarding vape batteries and external LEDs.</p>"
                "<table><thead><tr><th>Pin Category</th><th>GPIO Numbers</th><th>Safe For</th><th>Critical Warning</th></tr></thead><tbody>"
                "<tr><td class='safe'>Safe Outputs</td><td>GPIO 2, 4, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33</td><td>Single LEDs, RGB R/G/B pins</td><td>Always use 220Ω - 330Ω resistor!</td></tr>"
                "<tr><td style='color:var(--accent2);font-weight:bold'>Input Only</td><td>GPIO 34, 35, 36 (VP), 39 (VN)</td><td>Battery ADC (GPIO 35 default)</td><td>CANNOT output HIGH/LOW! Do NOT connect LEDs!</td></tr>"
                "<tr><td class='danger'>Boot Strapping</td><td>GPIO 0, 2, 12, 15</td><td>Internal use only</td><td>Pulling GPIO 0 LOW stops boot; GPIO 12 sets flash voltage!</td></tr>"
                "<tr><td class='danger'>UART0 Flasher</td><td>GPIO 1 (TX0), GPIO 3 (RX0)</td><td>USB Serial / Flashing</td><td>Wiring LEDs here blocks USB flashing and telemetry!</td></tr>"
                "<tr><td class='danger'>SPI Flash Memory</td><td>GPIO 6, 7, 8, 9, 10, 11</td><td>NEVER CONNECT</td><td>Direct flash bus. Connecting anything causes instant crash/brick!</td></tr>"
                "</tbody></table>"

                "<h3 style='margin-top:16px;font-size:1rem;color:var(--accent)'>🔋 1. Single-Cell 3.7V Vape Li-ion Battery Wiring Diagram</h3>"
                "<div class='diagram-box'>"
                "+-----------------------------------------------------------------------------------+\n"
                "| [3.7V Vape Pouch Cell]                                                             |\n"
                "|   (+) Red Wire   ────────> [TP4056 B+]    [TP4056 OUT+] ───> ESP32 5V (VIN) pin    |\n"
                "|   (-) Black Wire ────────> [TP4056 B-]    [TP4056 OUT-] ───> ESP32 GND pin         |\n"
                "|                                                                                    |\n"
                "| [Precision Battery Voltage Divider for GPIO 35]:                                   |\n"
                "|   Vape Batt (+) ──[ 100kΩ Resistor ]──┬──> ESP32 GPIO 35 (ADC1_CH7)                |\n"
                "|                                       │                                            |\n"
                "|                                  [ 100kΩ Resistor ]                                |\n"
                "|                                       │                                            |\n"
                "|   ESP32 GND     ──────────────────────┴────────────────────────────────────────────|\n"
                "+-----------------------------------------------------------------------------------+"
                "</div>"

                "<h3 style='margin-top:16px;font-size:1rem;color:var(--accent)'>💡 2. Single-Color LED Wiring (Satellite Case Light)</h3>"
                "<div class='diagram-box'>"
                "+-----------------------------------------------------------------------------------+\n"
                "| ESP32 Safe GPIO (e.g. GPIO 4) ───[ 220Ω - 330Ω Resistor ]───> Anode (+) [Long Leg]|\n"
                "|                                                                                   |\n"
                "| ESP32 GND                     ─────────────────────────────> Cathode (-) [Flat]   |\n"
                "+-----------------------------------------------------------------------------------+"
                "</div>"

                "<h3 style='margin-top:16px;font-size:1rem;color:var(--accent)'>🌈 3. 4-Pin RGB LED Wiring (Common Cathode or Anode)</h3>"
                "<div class='diagram-box'>"
                "+-----------------------------------------------------------------------------------+\n"
                "| [Common Cathode RGB LED]:                                                          |\n"
                "|   Pin 1 (Red)    ───[ 330Ω Resistor ]───> ESP32 GPIO 18                            |\n"
                "|   Pin 2 (Common) ───────────────────────> ESP32 GND  (Longest Pin)                 |\n"
                "|   Pin 3 (Green)  ───[ 220Ω Resistor ]───> ESP32 GPIO 19                            |\n"
                "|   Pin 4 (Blue)   ───[ 220Ω Resistor ]───> ESP32 GPIO 23                            |\n"
                "|                                                                                    |\n"
                "| [Common Anode RGB LED]:                                                            |\n"
                "|   Pin 2 (Common) ───────────────────────> ESP32 3V3 Pin (Check 'Invert' box above) |\n"
                "+-----------------------------------------------------------------------------------+"
                "</div></div>";

        // Card 4: Signed OTA Firmware Updater
        html += "<div class='card'><h2>🔒 Cryptographically Signed OTA Update</h2>"
                "<p style='color:var(--sub);font-size:0.9rem;margin-bottom:12px'>Upload a newly compiled firmware binary (.bin) and host ECDSA secp256r1 signature (.sig). Unsigned or tampered binaries are automatically rejected by hardware crypto before flashing.</p>"
                "<form method='POST' action='/update' enctype='multipart/form-data'>"
                "<label style='font-size:0.85rem;color:var(--sub)'>Firmware Binary (.bin):</label><br>"
                "<input type='file' name='update' style='margin:6px 0 12px 0;width:100%' required><br>"
                "<label style='font-size:0.85rem;color:var(--sub)'>Cryptographic Signature (.sig):</label><br>"
                "<input type='file' name='signature' style='margin:6px 0 16px 0;width:100%' required><br>"
                "<button type='submit' style='width:100%;padding:12px'>🚀 Verify Signature & Flash Firmware</button>"
                "</form></div>";

        html += "</div></body></html>";
        m_webServer->send(200, "text/html", html);
    });

    // ── 2. REST API: Node Info & Telemetry ────────────────────────────────────
    m_webServer->on("/api/v1/info", HTTP_GET, [this]() {
        uint32_t mv = BatteryMonitor::instance().getMilliVolts();
        uint8_t pct = BatteryMonitor::instance().getPercentage();
        const char* battState = BatteryMonitor::instance().getStateStr();
        bool hasBatt = BatteryMonitor::instance().hasBattery();

        String json = "{";
        json += "\"node\":\"" + String(NodeConfig::instance().getName()) + "\",";
        json += "\"uptime\":" + String(millis() / 1000) + ",";
        json += "\"heap\":" + String(ESP.getFreeHeap()) + ",";
        json += "\"ch\":" + String(WifiMonitor::instance().getChannel()) + ",";
        json += "\"pps\":" + String(WifiMonitor::instance().getPps()) + ",";
        json += "\"batt_mv\":" + String(mv) + ",";
        json += "\"batt_pct\":" + String(pct) + ",";
        json += "\"batt_state\":\"" + String(battState) + "\",";
        json += "\"has_battery\":" + String(hasBatt ? "true" : "false") + ",";
        json += "\"charging\":" + String(BatteryMonitor::instance().isCharging() ? "true" : "false");
        json += "}";
        m_webServer->send(200, "application/json", json);
    });

    // The battery wiring diagram, straight from flash (docs/battery-wiring.svg)
    m_webServer->on("/wiring.svg", HTTP_GET, [this]() {
        m_webServer->sendHeader("Cache-Control", "max-age=86400");
        m_webServer->send_P(200, "image/svg+xml", WIRING_SVG);
    });

    // ── 3. Name Config Endpoint ───────────────────────────────────────────────
    m_webServer->on("/config/name", HTTP_POST, [this]() {
        if (m_webServer->hasArg("name")) {
            String newName = m_webServer->arg("name");
            NodeConfig::instance().setName(newName.c_str());
        }
        m_webServer->sendHeader("Location", "/");
        m_webServer->send(303);
    });

    m_webServer->on("/config/name", HTTP_GET, [this]() {
        if (m_webServer->hasArg("reset")) {
            NodeConfig::instance().resetToDefaultName();
        }
        m_webServer->sendHeader("Location", "/");
        m_webServer->send(303);
    });

    // ── 3.5. Home Wi-Fi Config Endpoints ─────────────────────────────────────
    m_webServer->on("/config/wifi", HTTP_POST, [this]() {
        if (m_webServer->hasArg("ssid")) {
            String ssid = m_webServer->arg("ssid");
            String pass = m_webServer->hasArg("pass") ? m_webServer->arg("pass") : "";
            ssid.trim();
            pass.trim();
            NodeConfig::instance().setWifiCreds(ssid.c_str(), pass.c_str());
            WiFi.mode(WIFI_AP_STA);
            WiFi.begin(ssid.c_str(), pass.c_str());
        }
        m_webServer->sendHeader("Location", "/");
        m_webServer->send(303);
    });

    m_webServer->on("/config/wifi/clear", HTTP_GET, [this]() {
        NodeConfig::instance().clearWifiCreds();
        WiFi.disconnect(true);
        m_webServer->sendHeader("Location", "/");
        m_webServer->send(303);
    });

    // ── 3.9. Station Role (Base Station vs Mobile) Endpoints ─────────────────
    m_webServer->on("/config/mode", HTTP_GET, [this]() {
        if (m_webServer->hasArg("mode")) {
            String m = m_webServer->arg("mode");
            if (m.equalsIgnoreCase("base") || m.equalsIgnoreCase("base_station") || m.equalsIgnoreCase("station")) {
                NodeConfig::instance().setOpMode(OP_MODE_BASE_STATION);
            } else if (m.equalsIgnoreCase("mobile")) {
                NodeConfig::instance().setOpMode(OP_MODE_MOBILE);
            }
        }
        m_webServer->sendHeader("Location", "/");
        m_webServer->send(303);
    });

    m_webServer->on("/api/v1/mode", HTTP_GET, [this]() {
        String json = "{";
        json += "\"mode\":\"" + String(NodeConfig::instance().getOpModeStr()) + "\",";
        json += "\"is_base_station\":" + String(NodeConfig::instance().isBaseStation() ? "true" : "false") + ",";
        json += "\"hops\":" + String(MeshEngine::instance().getHopsToGateway());
        json += "}";
        m_webServer->send(200, "application/json", json);
    });

    // ── 4. LED Config Endpoint ────────────────────────────────────────────────
    m_webServer->on("/config/led", HTTP_POST, [this]() {
        int slot = m_webServer->arg("slot").toInt();
        String type = m_webServer->arg("type");
        int pinR = m_webServer->arg("pinR").toInt();
        int roleInt = m_webServer->arg("role").toInt();
        LedRole role = (LedRole)roleInt;
        bool inv = m_webServer->hasArg("inv");

        if (type == "rgb") {
            int pinG = m_webServer->arg("pinG").toInt();
            int pinB = m_webServer->arg("pinB").toInt();
            LedPatterns::instance().configureRgb(slot, pinR, pinG, pinB, role, inv);
        } else {
            LedPatterns::instance().configureSingle(slot, pinR, role, inv);
        }

        m_webServer->sendHeader("Location", "/");
        m_webServer->send(303);
    });

    m_webServer->on("/config/led/clear", HTTP_GET, [this]() {
        int slot = m_webServer->arg("slot").toInt();
        LedPatterns::instance().clearSlot(slot);
        m_webServer->sendHeader("Location", "/");
        m_webServer->send(303);
    });

    // ── 5. Signed OTA Multipart Upload ────────────────────────────────────────
    m_webServer->on("/update", HTTP_POST, [this]() {
        bool verified = false;
        if (m_binReceived && m_sigReceived && m_sigLen > 0) {
            verified = verifySignature(m_sha256Hash, m_sigBuf, m_sigLen);
        } else {
            Serial.println("{\"type\":\"error\",\"component\":\"ota\",\"msg\":\"missing_firmware_or_signature\"}");
        }

        if (verified && !Update.hasError() && Update.end(true)) {
            Serial.println("{\"type\":\"ota\",\"status\":\"update_success_rebooting\"}");
            m_webServer->send(200, "text/plain", "SUCCESS: Cryptographic signature verified! Rebooting node...");
            delay(1000);
            ESP.restart();
        } else {
            Update.abort();
            Serial.println("{\"type\":\"error\",\"component\":\"ota\",\"msg\":\"signature_rejected_or_update_failed\"}");
            m_webServer->send(403, "text/plain", "FAILED: Cryptographic signature rejected or flash write error. Firmware NOT installed.");
        }
    }, [this]() {
        HTTPUpload& upload = m_webServer->upload();
        if (upload.status == UPLOAD_FILE_START) {
            if (upload.name == "update") {
                m_binReceived = true;
                mbedtls_sha256_init(&m_shaCtx);
                mbedtls_sha256_starts(&m_shaCtx, 0); // 0 = SHA-256
                Serial.printf("{\"type\":\"ota\",\"status\":\"binary_upload_start\",\"filename\":\"%s\"}\n", upload.filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.printError(Serial);
                }
            } else if (upload.name == "signature") {
                m_sigReceived = true;
                m_sigLen = 0;
                Serial.printf("{\"type\":\"ota\",\"status\":\"signature_upload_start\",\"filename\":\"%s\"}\n", upload.filename.c_str());
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (upload.name == "update") {
                mbedtls_sha256_update(&m_shaCtx, upload.buf, upload.currentSize);
                if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    Update.printError(Serial);
                }
            } else if (upload.name == "signature") {
                if (m_sigLen + upload.currentSize <= sizeof(m_sigBuf)) {
                    memcpy(m_sigBuf + m_sigLen, upload.buf, upload.currentSize);
                    m_sigLen += upload.currentSize;
                }
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (upload.name == "update") {
                mbedtls_sha256_finish(&m_shaCtx, m_sha256Hash);
                mbedtls_sha256_free(&m_shaCtx);
                Serial.printf("{\"type\":\"ota\",\"status\":\"binary_upload_end\",\"size\":%u}\n", upload.totalSize);
            } else if (upload.name == "signature") {
                Serial.printf("{\"type\":\"ota\",\"status\":\"signature_upload_end\",\"size\":%u}\n", m_sigLen);
            }
        }
    });
}
