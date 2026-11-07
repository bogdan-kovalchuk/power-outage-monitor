#ifndef WEB_UI_H
#define WEB_UI_H

#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>Power Outage Monitor</title>
  <style>
    :root {
      --bg: #0d1117;
      --card-bg: #161b22;
      --card-border: #30363d;
      --text: #f0f6fc;
      --text-muted: #8b949e;
      --green: #238636;
      --green-glow: rgba(35, 134, 54, 0.4);
      --red: #da3633;
      --red-glow: rgba(218, 54, 51, 0.4);
      --amber: #d29922;
      --blue: #1f6feb;
      --purple: #8957e5;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; }
    body { background-color: var(--bg); color: var(--text); padding: 16px; min-height: 100vh; display: flex; flex-direction: column; align-items: center; }
    .container { width: 100%; max-width: 520px; display: flex; flex-direction: column; gap: 14px; }
    header { display: flex; justify-content: space-between; align-items: center; padding-bottom: 8px; border-bottom: 1px solid var(--card-border); order: -3; }
    .brand { display: flex; align-items: center; gap: 10px; }
    .brand h1 { font-size: 1.1rem; font-weight: 700; letter-spacing: -0.3px; }
    .mode-pill { font-size: 0.75rem; font-weight: 700; padding: 4px 10px; border-radius: 12px; text-transform: uppercase; }
    .pill-armed { background: var(--green); color: #fff; }
    .pill-disarmed { background: #30363d; color: var(--text-muted); }
    .pill-snoozed { background: var(--blue); color: #fff; }
    .warn-banner {
      display: none;
      background: rgba(210, 153, 34, 0.15);
      border: 1px solid var(--amber);
      color: #f0c674;
      border-radius: 10px;
      padding: 10px 12px;
      font-size: 0.82rem;
      line-height: 1.35;
      order: -2;
    }
    .info-banner {
      display: none;
      background: rgba(31, 111, 235, 0.15);
      border: 1px solid #58a6ff;
      color: #cce3ff;
      border-radius: 8px;
      padding: 10px 12px;
      font-size: 0.82rem;
      line-height: 1.35;
      margin-bottom: 10px;
    }
    .status-card {
      background: var(--card-bg);
      border: 2px solid var(--card-border);
      border-radius: 16px;
      padding: 22px 18px;
      text-align: center;
      transition: all 0.3s ease;
      box-shadow: 0 8px 24px rgba(0,0,0,0.4);
    }
    .status-card.online { border-color: var(--green); box-shadow: 0 0 25px var(--green-glow); }
    .status-card.offline { border-color: var(--red); box-shadow: 0 0 25px var(--red-glow); animation: pulse-red 2s infinite; }
    @keyframes pulse-red {
      0% { box-shadow: 0 0 10px var(--red-glow); }
      50% { box-shadow: 0 0 35px var(--red-glow); }
      100% { box-shadow: 0 0 10px var(--red-glow); }
    }
    .status-icon { font-size: 2.8rem; margin-bottom: 6px; }
    .status-title { font-size: 1.4rem; font-weight: 800; margin-bottom: 4px; }
    .status-desc { font-size: 0.85rem; color: var(--text-muted); margin-bottom: 14px; }
    .timer-badge {
      display: inline-block;
      background: #0d1117;
      border: 1px solid var(--card-border);
      padding: 8px 16px;
      border-radius: 8px;
      font-size: 1.2rem;
      font-weight: 700;
      letter-spacing: 1px;
      color: #58a6ff;
    }
    .notify-strip {
      margin-top: 12px;
      display: flex;
      flex-wrap: wrap;
      justify-content: center;
      gap: 8px;
    }
    .notify-badge {
      display: inline-block;
      font-size: 0.78rem;
      font-weight: 600;
      padding: 4px 10px;
      border-radius: 999px;
      border: 1px solid var(--card-border);
      background: #0d1117;
      color: var(--text-muted);
    }
    .notify-badge.ok {
      border-color: rgba(46, 160, 67, 0.55);
      color: #3fb950;
      background: rgba(35, 134, 54, 0.14);
    }
    .notify-badge.fail {
      border-color: rgba(248, 81, 73, 0.55);
      color: #f85149;
      background: rgba(218, 54, 51, 0.14);
    }
    .adv-subhead {
      font-size: 0.78rem;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.5px;
      color: var(--text-muted);
      margin-top: 4px;
      padding-top: 8px;
      border-top: 1px solid var(--card-border);
    }
    .section-title { font-size: 0.82rem; font-weight: 600; text-transform: uppercase; color: var(--text-muted); letter-spacing: 0.5px; margin-bottom: 6px; }
    .controls-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    .controls-note { font-size: 0.76rem; color: var(--text-muted); line-height: 1.35; margin-top: 8px; }
    button {
      padding: 12px 14px;
      font-size: 0.9rem;
      font-weight: 700;
      border-radius: 10px;
      border: 1px solid transparent;
      cursor: pointer;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 6px;
      transition: transform 0.12s ease;
      color: #fff;
    }
    button:active { transform: scale(0.97); }
    button:disabled { opacity: 0.55; cursor: default; }
    .btn-arm { background: var(--green); border-color: #2ea043; }
    .btn-disarm { background: #21262d; border-color: #363b42; color: #c9d1d9; }
    .btn-snooze { background: var(--blue); border-color: #388bfd; }
    .btn-test { background: var(--purple); border-color: #a371f7; }
    .btn-save { background: #1f6feb; width: 100%; margin-top: 6px; }
    .btn-sec { background: #21262d; border-color: #363b42; color: #c9d1d9; padding: 8px 10px; font-size: 0.8rem; }
    .btn-danger { background: #21262d; border-color: var(--red); color: #f85149; width: 100%; }
    .btn-sm { padding: 7px 10px; font-size: 0.78rem; border-radius: 7px; }
    .card { background: var(--card-bg); border: 1px solid var(--card-border); border-radius: 12px; padding: 14px 16px; }
    .card.ap-top { order: -1; border-color: #58a6ff; }
    .metrics-list { display: flex; flex-direction: column; gap: 8px; font-size: 0.85rem; }
    .metric-row { display: flex; justify-content: space-between; align-items: center; gap: 8px; }
    .metric-label { color: var(--text-muted); }
    .metric-val { font-weight: 600; text-align: right; word-break: break-all; }
    .collapsible-trigger {
      display: flex;
      justify-content: space-between;
      align-items: center;
      cursor: pointer;
      padding: 4px 0;
      font-weight: 700;
      font-size: 0.92rem;
    }
    .card-body { display: none; flex-direction: column; gap: 11px; margin-top: 12px; padding-top: 12px; border-top: 1px solid var(--card-border); }
    .card-body.open { display: flex; }
    .form-group { display: flex; flex-direction: column; gap: 4px; }
    .form-group label { font-size: 0.75rem; color: var(--text-muted); font-weight: 600; }
    .form-row-2 { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    .field-hint { font-size: 0.74rem; color: var(--text-muted); line-height: 1.3; }
    .field-err { display: none; font-size: 0.76rem; color: #f85149; line-height: 1.3; }
    .field-err.show { display: block; }
    input[type="text"], input[type="password"], input[type="number"], select {
      background: #0d1117;
      border: 1px solid var(--card-border);
      border-radius: 6px;
      padding: 9px 10px;
      color: var(--text);
      font-size: 0.88rem;
      width: 100%;
    }
    input[type="file"] {
      background: #0d1117;
      border: 1px solid var(--card-border);
      border-radius: 6px;
      padding: 8px 10px;
      color: var(--text-muted);
      font-size: 0.84rem;
      width: 100%;
    }
    input:focus, select:focus { border-color: #58a6ff; outline: none; }
    input.invalid { border-color: var(--red); }
    .wifi-slot { background: #0d1117; border: 1px solid var(--card-border); border-radius: 8px; padding: 10px; display: flex; flex-direction: column; gap: 8px; }
    .wifi-slot-head { display: flex; justify-content: space-between; align-items: center; font-size: 0.78rem; color: var(--text-muted); font-weight: 700; }
    .wifi-slot-controls { display: flex; justify-content: space-between; align-items: center; gap: 8px; flex-wrap: wrap; }
    .chk-label { display: inline-flex; align-items: center; gap: 6px; font-size: 0.8rem; color: var(--text-muted); cursor: pointer; }
    .form-group label.switch-row {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 10px;
      margin-top: 6px;
      padding: 8px 10px;
      background: #0d1117;
      border: 1px solid var(--card-border);
      border-radius: 8px;
      cursor: pointer;
    }
    .switch-text { font-size: 0.84rem; font-weight: 600; color: var(--text); }
    .switch { position: relative; display: inline-block; width: 42px; height: 24px; flex-shrink: 0; }
    .switch input { position: absolute; inset: 0; width: 100%; height: 100%; opacity: 0; margin: 0; cursor: pointer; z-index: 1; }
    .switch-slider {
      position: absolute;
      inset: 0;
      background: #30363d;
      border-radius: 24px;
      transition: background 0.2s ease;
    }
    .switch-slider::before {
      content: "";
      position: absolute;
      width: 18px;
      height: 18px;
      left: 3px;
      top: 3px;
      background: #f0f6fc;
      border-radius: 50%;
      transition: transform 0.2s ease;
    }
    .switch input:checked + .switch-slider { background: var(--green); }
    .switch input:checked + .switch-slider::before { transform: translateX(18px); }
    .switch input:focus-visible + .switch-slider { outline: 2px solid #58a6ff; outline-offset: 2px; }
    .scan-list { display: none; flex-direction: column; gap: 6px; max-height: 160px; overflow-y: auto; background: #0d1117; border: 1px solid var(--card-border); border-radius: 8px; padding: 8px; }
    .scan-list.show { display: flex; }
    .scan-item { display: flex; justify-content: space-between; align-items: center; padding: 6px 8px; border-radius: 6px; background: #161b22; cursor: pointer; font-size: 0.82rem; border: 1px solid transparent; }
    .scan-item:hover { border-color: #58a6ff; }
    .action-stack { display: flex; flex-direction: column; gap: 8px; margin-top: 6px; }
    .progress-wrap { display: none; flex-direction: column; gap: 5px; }
    .progress-wrap.show { display: flex; }
    .progress-head { display: flex; justify-content: space-between; font-size: 0.78rem; color: var(--text-muted); }
    .progress-track { width: 100%; height: 8px; background: #0d1117; border: 1px solid var(--card-border); border-radius: 4px; overflow: hidden; }
    .progress-bar { width: 0%; height: 100%; background: var(--blue); transition: width 0.15s ease; }
    #toast {
      position: fixed;
      bottom: 24px;
      background: #238636;
      color: #fff;
      padding: 10px 20px;
      border-radius: 20px;
      font-size: 0.85rem;
      font-weight: 600;
      box-shadow: 0 4px 12px rgba(0,0,0,0.5);
      opacity: 0;
      transform: translateY(20px);
      transition: all 0.3s ease;
      pointer-events: none;
      z-index: 1000;
      max-width: 90vw;
      text-align: center;
    }
    #toast.show { opacity: 1; transform: translateY(0); }
  </style>
</head>
<body>
  <div class="container">
    <header>
      <div class="brand">
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="#58a6ff" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><path d="M13 2L3 14h9l-1 8 10-12h-9l1-8z"/></svg>
        <h1>Power Outage Monitor</h1>
      </div>
      <div id="modePill" class="mode-pill pill-armed">ARMED</div>
    </header>

    <div id="adminWarn" class="warn-banner">No admin password set &#x2013; anyone on your Wi-Fi or the setup AP can access settings, Wi-Fi scan, setup AP control, reboot, factory reset and firmware update.</div>

    <div id="statusCard" class="status-card">
      <div id="statusIcon" class="status-icon">&#x23F3;</div>
      <div id="statusTitle" class="status-title">CHECKING POWER</div>
      <div id="statusDesc" class="status-desc">Checking mains power...</div>
      <div id="timerBadge" class="timer-badge">Starting up...</div>
      <div class="notify-strip">
        <div id="phoneNotifyStatus" class="notify-badge">Phone: not configured</div>
        <div id="cloudNotifyStatus" class="notify-badge" style="display:none;">Cloud: off</div>
      </div>
    </div>

    <div>
      <div class="section-title">System Controls</div>
      <div class="controls-grid">
        <button class="btn-arm" onclick="sendAction('/api/arm')">Arm</button>
        <button class="btn-disarm" onclick="sendAction('/api/disarm')">Disarm</button>
        <button id="btnSnooze" class="btn-snooze" onclick="snoozeAction()">Snooze</button>
        <button class="btn-test" onclick="sendAction('/api/test')">Test Alarm</button>
      </div>
      <div id="controlsNote" class="controls-note">Arm, disarm, snooze and test are open to anyone on the local network by design.</div>
    </div>

    <div class="card">
      <div class="section-title" style="margin-bottom:8px;">Diagnostics</div>
      <div class="metrics-list">
        <div class="metric-row"><span class="metric-label">Wi-Fi Network:</span><span id="diagWifi" class="metric-val">-</span></div>
        <div class="metric-row"><span class="metric-label">Signal (RSSI):</span><span id="diagRssi" class="metric-val">-</span></div>
        <div class="metric-row"><span class="metric-label">IP / Hostname:</span><span id="diagIp" class="metric-val">-</span></div>
        <div class="metric-row"><span class="metric-label">Setup AP:</span><span id="diagAp" class="metric-val">-</span></div>
        <div class="metric-row"><span class="metric-label">Uptime:</span><span id="diagUptime" class="metric-val">-</span></div>
        <div class="metric-row"><span class="metric-label">Free RAM:</span><span id="diagHeap" class="metric-val">-</span></div>
        <div class="metric-row"><span class="metric-label">Firmware:</span><span id="diagFw" class="metric-val">-</span></div>
      </div>
    </div>

    <!-- Wi-Fi Network Card (Primary Slot 1) -->
    <div id="wifiCard" class="card">
      <div class="collapsible-trigger" onclick="toggleCard('wifi')">
        <span id="wifiCardTitle">Wi-Fi Network</span>
        <span id="arrow_wifi">&#x25BC;</span>
      </div>
      <div id="body_wifi" class="card-body">
        <div id="apHintBanner" class="info-banner">Setup AP mode: add your Wi-Fi network below and tap Save.</div>
        <div id="apSavedBanner" class="info-banner"></div>
        <div style="display:flex;justify-content:space-between;align-items:center;gap:8px;">
          <span style="font-size:0.78rem;color:var(--text-muted);">Primary Wi-Fi network (backup slots 2&#x2013;5 are under Advanced).</span>
          <button type="button" id="btnScan" class="btn-sec btn-sm" onclick="scanNetworks()">Scan</button>
        </div>
        <div id="scanResults" class="scan-list"></div>
        <datalist id="scanList"></datalist>
        <div id="wifiErr" style="display:none;color:#f85149;font-size:0.8rem;"></div>
        <div id="wifiRows" style="display:flex;flex-direction:column;gap:8px;"></div>
        <button type="button" class="btn-save" onclick="saveWifi(0, 1)">Save Wi-Fi</button>
      </div>
    </div>

    <!-- Device Settings Card -->
    <div id="devCard" class="card">
      <div class="collapsible-trigger" onclick="toggleCard('dev')">
        <span>Device Settings</span>
        <span id="arrow_dev">&#x25BC;</span>
      </div>
      <form id="body_dev" class="card-body" onsubmit="saveDevice(event)" novalidate>
        <div class="form-group">
          <label for="cfg_local_url">Local Notify URL (Phone)</label>
          <input type="text" id="cfg_local_url" placeholder="http://192.168.1.60:8080/{event}">
          <div class="field-hint">{event} becomes power_off, power_on, test_alarm or alarm_stop; used for a phone on the same Wi-Fi (for example MacroDroid HTTP Server).</div>
          <div id="err_cfg_local_url" class="field-err"></div>
        </div>
        <div class="form-group">
          <label for="cfg_webhook">Cloud Webhook URL</label>
          <input type="text" id="cfg_webhook" placeholder="https://ntfy.sh/your_secret_topic">
          <div class="field-hint">Cloud target (ntfy.sh or any HTTPS endpoint); supports {event} placeholder.</div>
          <div id="err_cfg_webhook" class="field-err"></div>
          <label class="switch-row" for="cfg_cloud_enabled">
            <span class="switch-text">Internet alerts (ntfy) &#x2013; <span id="cloudSwitchState">Off</span></span>
            <span class="switch">
              <input type="checkbox" id="cfg_cloud_enabled" onchange="updateCloudSwitchState()">
              <span class="switch-slider"></span>
            </span>
          </label>
          <div class="field-hint">Turn on when you are away from home; at home the phone on Wi-Fi is alerted directly.</div>
        </div>
        <div class="form-group">
          <label for="cfg_alert_mode">Alert Tracking Policy</label>
          <select id="cfg_alert_mode">
            <option value="0">Outage only (no internet alert on power return)</option>
            <option value="1">Both (Alert on cut + Notification on restoration)</option>
          </select>
        </div>
        <button type="submit" class="btn-save">Save Device Settings</button>
      </form>
    </div>

    <!-- Setup Access Card -->
    <div id="secCard" class="card">
      <div class="collapsible-trigger" onclick="toggleCard('sec')">
        <span>Setup Access &amp; Maintenance</span>
        <span id="arrow_sec">&#x25BC;</span>
      </div>
      <form id="body_sec" class="card-body" onsubmit="saveAccess(event)" novalidate>
        <div class="form-group">
          <label for="cfg_adm_pass" id="lbl_cfg_adm_pass">Admin Password</label>
          <input type="password" id="cfg_adm_pass" placeholder="Not set" autocomplete="new-password">
          <div id="err_cfg_adm_pass" class="field-err"></div>
        </div>
        <label id="clearAdmWrap" class="chk-label" style="display:none;">
          <input type="checkbox" id="cfg_clear_adm" onchange="toggleClearAdm()"> Clear admin password (disable auth)
        </label>
        <div class="form-group">
          <label for="cfg_ap_ssid" id="lbl_cfg_ap_ssid">Setup AP SSID</label>
          <input type="text" id="cfg_ap_ssid">
          <div id="err_cfg_ap_ssid" class="field-err"></div>
        </div>
        <div class="form-group">
          <label for="cfg_ap_pass" id="lbl_cfg_ap_pass">Setup AP Password</label>
          <input type="password" id="cfg_ap_pass" placeholder="Password" autocomplete="new-password">
          <div class="field-hint">The default password is public &#x2013; change it.</div>
          <div id="err_cfg_ap_pass" class="field-err"></div>
        </div>
        <div class="form-group">
          <label for="cfg_ap_win" id="lbl_cfg_ap_win">Boot Setup Window</label>
          <input type="number" id="cfg_ap_win">
          <div id="err_cfg_ap_win" class="field-err"></div>
        </div>
        <button type="submit" class="btn-save">Save Setup Access</button>
        <div class="action-stack">
          <button type="button" class="btn-sec" onclick="sendAction('/api/setup-ap')">Open setup AP for 5 min</button>
          <button type="button" id="btnCloseAp" class="btn-sec" style="display:none;" onclick="sendAction('/api/setup-ap/close')">Close setup AP</button>
          <button type="button" class="btn-danger" onclick="factoryReset()">Factory reset</button>
          <button type="button" class="btn-danger" onclick="rebootDevice()">Reboot</button>
        </div>
      </form>
    </div>

    <!-- Firmware Update Card -->
    <div id="fwCard" class="card">
      <div class="collapsible-trigger" onclick="toggleCard('fw')">
        <span>Firmware Update</span>
        <span id="arrow_fw">&#x25BC;</span>
      </div>
      <div id="body_fw" class="card-body">
        <div class="metric-row">
          <span class="metric-label">Running version:</span>
          <span id="fwCurrentVer" class="metric-val">-</span>
        </div>
        <div id="fwDisabledHint" class="warn-banner" style="display:none;order:0;margin:0;">Set an admin password to enable firmware updates</div>
        <div class="form-group">
          <label for="fw_file">Firmware binary (.bin)</label>
          <input type="file" id="fw_file" accept=".bin" disabled>
        </div>
        <div id="fwProgressWrap" class="progress-wrap">
          <div class="progress-head">
            <span id="fwProgressLabel">Uploading...</span>
            <span id="fwProgressPct">0%</span>
          </div>
          <div class="progress-track">
            <div id="fwProgressBar" class="progress-bar"></div>
          </div>
        </div>
        <div id="fwStatusMsg" class="field-hint" style="display:none;"></div>
        <button type="button" id="btnFwUpload" class="btn-save" onclick="uploadFirmware()" disabled>Upload firmware</button>
      </div>
    </div>

    <!-- Advanced Card (collapsed by default) -->
    <div id="advCard" class="card">
      <div class="collapsible-trigger" onclick="toggleCard('adv')">
        <span>Advanced</span>
        <span id="arrow_adv">&#x25BC;</span>
      </div>
      <div id="body_adv" class="card-body">
        <form id="form_adv" style="display:flex;flex-direction:column;gap:11px;" onsubmit="saveAdvanced(event)" novalidate>
          <div class="form-group">
            <label for="cfg_name">Device Name (mDNS hostname)</label>
            <input type="text" id="cfg_name">
            <div id="hint_cfg_name" class="field-hint"></div>
            <div id="err_cfg_name" class="field-err"></div>
          </div>
          <div class="form-row-2">
            <div class="form-group">
              <label for="cfg_debounce" id="lbl_cfg_debounce">Debounce</label>
              <input type="number" id="cfg_debounce">
              <div id="err_cfg_debounce" class="field-err"></div>
            </div>
            <div class="form-group">
              <label for="cfg_cooldown" id="lbl_cfg_cooldown">Restore Cooldown</label>
              <input type="number" id="cfg_cooldown">
              <div id="err_cfg_cooldown" class="field-err"></div>
            </div>
          </div>
          <div class="form-group">
            <label for="cfg_snooze" id="lbl_cfg_snooze">Default Snooze</label>
            <input type="number" id="cfg_snooze">
            <div id="err_cfg_snooze" class="field-err"></div>
          </div>
          <button type="submit" class="btn-save">Save Advanced Settings</button>
        </form>
        <div class="adv-subhead">Backup Wi-Fi Networks (Slots 2&#x2013;5)</div>
        <div id="advWifiErr" style="display:none;color:#f85149;font-size:0.8rem;"></div>
        <div id="advWifiRows" style="display:flex;flex-direction:column;gap:8px;"></div>
        <button type="button" class="btn-save" onclick="saveWifi(1, wifiSlotCount)">Save Backup Wi-Fi</button>
      </div>
    </div>
  </div>

  <div id="toast">Settings updated</div>

  <script>
    const LIMITS = {
      ssidMaxBytes: 32,
      apSsidMinBytes: 1,
      apSsidMaxBytes: 32,
      passMinBytes: 8,
      passMaxBytes: 63,
      deviceNameMin: 1,
      deviceNameMax: 32,
      deviceNamePattern: '[a-z0-9]([a-z0-9-]*[a-z0-9])?',
      deviceNameDefault: 'power-monitor',
      urlMaxBytes: 256,
      debounceMs: { min: 50, max: 10000, def: 500 },
      restoreCooldownMs: { min: 0, max: 60000, def: 5000 },
      snoozeDefaultMin: { min: 1, max: 1440, def: 30 },
      setupWindowS: { min: 0, max: 600, def: 60 },
      alertModeDef: 0,
      scanTimeoutMs: 16000,
      scanPollIntervalMs: 1000
    };

    let cfg = null;
    let cfgPromise = null;
    let cfgReloadQueued = false;
    let cfgQueuedForce = null;
    let cfgQueuedSuppressAuth = false;
    let lastStatus = null;
    let liveOutageSec = 0;
    let isOffline = false;
    let wifiSlotCount = 0;
    let activeSsidIdx = 0;
    let apExpandedOnce = false;
    let toastTimer = null;
    let snoozeDefMin = LIMITS.snoozeDefaultMin.def;
    let dirtyWifi = false;
    let dirtyAdvWifi = false;
    let dirtyDev = false;
    let dirtyAdv = false;
    let dirtySec = false;
    let otaBusy = false;
    let apSavedSsids = [];

    function defVal(v, d) {
      return (v !== undefined && v !== null) ? v : d;
    }

    function utf8ByteLen(s) {
      if (typeof TextEncoder !== 'undefined') {
        return new TextEncoder().encode(s).length;
      }
      return unescape(encodeURIComponent(s)).length;
    }

    function apiFetch(url, opts) {
      const o = opts || {};
      const h = Object.assign({ 'X-Requested-With': 'pm-ui' }, o.headers || {});
      return fetch(url, Object.assign({}, o, { headers: h }));
    }

    function applyLimitsToDom() {
      const nameIn = document.getElementById('cfg_name');
      nameIn.maxLength = LIMITS.deviceNameMax;
      nameIn.pattern = LIMITS.deviceNamePattern;
      nameIn.value = LIMITS.deviceNameDefault;
      document.getElementById('hint_cfg_name').textContent =
        LIMITS.deviceNameMin + '-' + LIMITS.deviceNameMax +
        ' chars: lowercase a-z, digits 0-9, hyphens (not at start or end).';

      document.getElementById('cfg_webhook').maxLength = LIMITS.urlMaxBytes;
      document.getElementById('cfg_local_url').maxLength = LIMITS.urlMaxBytes;

      const numFields = [
        { id: 'cfg_debounce', lbl: 'lbl_cfg_debounce', title: 'Debounce', spec: LIMITS.debounceMs, unit: ' ms' },
        { id: 'cfg_cooldown', lbl: 'lbl_cfg_cooldown', title: 'Restore Cooldown', spec: LIMITS.restoreCooldownMs, unit: ' ms' },
        { id: 'cfg_snooze', lbl: 'lbl_cfg_snooze', title: 'Default Snooze', spec: LIMITS.snoozeDefaultMin, unit: ' min' },
        { id: 'cfg_ap_win', lbl: 'lbl_cfg_ap_win', title: 'Boot Setup Window', spec: LIMITS.setupWindowS, unit: ' s, 0 = off' }
      ];
      numFields.forEach(f => {
        const el = document.getElementById(f.id);
        el.min = f.spec.min;
        el.max = f.spec.max;
        el.value = f.spec.def;
        document.getElementById(f.lbl).textContent =
          f.title + ' (' + f.spec.min + '-' + f.spec.max + f.unit + ')';
      });

      document.getElementById('cfg_ap_ssid').maxLength = LIMITS.apSsidMaxBytes;
      document.getElementById('lbl_cfg_ap_ssid').textContent =
        'Setup AP SSID (' + LIMITS.apSsidMinBytes + '-' + LIMITS.apSsidMaxBytes + ' bytes)';

      document.getElementById('cfg_ap_pass').maxLength = LIMITS.passMaxBytes;
      document.getElementById('lbl_cfg_ap_pass').textContent =
        'Setup AP Password (' + LIMITS.passMinBytes + '-' + LIMITS.passMaxBytes + ' bytes)';

      document.getElementById('cfg_adm_pass').maxLength = LIMITS.passMaxBytes;
      document.getElementById('lbl_cfg_adm_pass').textContent =
        'Admin Password (user: admin, ' + LIMITS.passMinBytes + '-' + LIMITS.passMaxBytes + ' bytes)';

      document.getElementById('btnSnooze').textContent = 'Snooze ' + snoozeDefMin + 'm';
    }

    function setFieldErr(id, msg) {
      const inp = document.getElementById(id);
      const err = document.getElementById('err_' + id);
      if (inp) inp.classList.toggle('invalid', !!msg);
      if (err) {
        err.textContent = msg || '';
        err.classList.toggle('show', !!msg);
      }
    }

    function clearFieldErrors(ids) {
      ids.forEach(id => setFieldErr(id, ''));
    }

    function parseIntField(id, spec) {
      const raw = document.getElementById(id).value.trim();
      if (!/^-?\d+$/.test(raw)) {
        return { ok: false, error: 'Enter a whole number (' + spec.min + '-' + spec.max + ')' };
      }
      const val = parseInt(raw, 10);
      if (!isFinite(val) || val < spec.min || val > spec.max) {
        return { ok: false, error: 'Must be between ' + spec.min + ' and ' + spec.max };
      }
      return { ok: true, value: val };
    }

    function validateUrlField(url) {
      if (url === '') return '';
      if (utf8ByteLen(url) > LIMITS.urlMaxBytes) {
        return 'Must be at most ' + LIMITS.urlMaxBytes + ' bytes';
      }
      if (url.indexOf('http://') !== 0 && url.indexOf('https://') !== 0) {
        return 'Must start with http:// or https://';
      }
      return '';
    }

    function showWifiErrBox(boxId, msg) {
      const el = document.getElementById(boxId);
      if (!el) return;
      if (msg) {
        el.textContent = msg;
        el.style.display = 'block';
      } else {
        el.textContent = '';
        el.style.display = 'none';
      }
    }

    function showWifiErr(msg) {
      showWifiErrBox('wifiErr', msg);
    }

    function showAdvWifiErr(msg) {
      showWifiErrBox('advWifiErr', msg);
    }

    function formatAge(ageS) {
      if (ageS < 60) return ageS + ' s ago';
      const m = Math.floor(ageS / 60);
      if (m < 60) return m + ' min ago';
      const h = Math.floor(m / 60);
      return h + ' h ago';
    }

    function formatNotifyEvent(ev) {
      if (ev === 'TEST_ALARM') return 'test';
      if (ev === 'POWER_OFF') return 'power_off';
      if (ev === 'POWER_ON') return 'power_on';
      if (ev === 'ALARM_STOP') return 'alarm_stop';
      return (ev || '').toLowerCase();
    }

    function setNotifyPill(el, label, rec, isConfigured, showWhenUnconfigured) {
      if (!el) return;
      if (!isConfigured && !rec) {
        if (!showWhenUnconfigured) {
          el.style.display = 'none';
          return;
        }
        el.style.display = 'inline-block';
        el.className = 'notify-badge';
        el.textContent = label + ': not configured';
        return;
      }
      el.style.display = 'inline-block';
      if (!rec) {
        el.className = 'notify-badge';
        el.textContent = label + ': not tested yet';
        return;
      }
      const evTxt = formatNotifyEvent(rec.event);
      const ageTxt = formatAge(defVal(rec.age_s, 0));
      if (rec.ok) {
        el.className = 'notify-badge ok';
        el.textContent = label + ': OK (' + evTxt + ' ' + ageTxt + ')';
      } else {
        el.className = 'notify-badge fail';
        const detail = rec.reason ? rec.reason : ('HTTP ' + rec.code);
        el.textContent = label + ': last delivery failed (' + evTxt + ', ' + detail + ', ' + ageTxt + ')';
      }
    }

    function renderNotifySummary() {
      const notify = (lastStatus && lastStatus.notify) ? lastStatus.notify : {};
      const phoneCfgKnown = !!cfg;
      const phoneConfigured = phoneCfgKnown ? !!(cfg.local_url && cfg.local_url.length > 0) : !!(notify.local);
      const phoneEl = document.getElementById('phoneNotifyStatus');
      if (phoneEl) {
        if (!phoneCfgKnown && !notify.local) {
          phoneEl.style.display = 'inline-block';
          phoneEl.className = 'notify-badge';
          phoneEl.textContent = 'Phone: not tested yet';
        } else {
          setNotifyPill(phoneEl, 'Phone', notify.local || null, phoneConfigured, true);
        }
      }
      const cloudEl = document.getElementById('cloudNotifyStatus');
      if (cloudEl) {
        if (notify.cloud) {
          setNotifyPill(cloudEl, 'Cloud', notify.cloud, true, true);
        } else if (cfg) {
          if (!cfg.cloud_enabled) {
            cloudEl.style.display = 'inline-block';
            cloudEl.className = 'notify-badge';
            cloudEl.textContent = 'Cloud: off';
          } else {
            const cloudConfigured = !!(cfg.webhook_url && cfg.webhook_url.length > 0);
            setNotifyPill(cloudEl, 'Cloud', null, cloudConfigured, true);
          }
        } else {
          cloudEl.style.display = 'none';
        }
      }
    }

    function updateCloudSwitchState() {
      const on = !!document.getElementById('cfg_cloud_enabled').checked;
      document.getElementById('cloudSwitchState').textContent = on ? 'On' : 'Off';
    }

    function formatTime(sec) {
      const h = Math.floor(sec / 3600);
      const m = Math.floor((sec % 3600) / 60);
      const s = Math.floor(sec % 60);
      return [h, m, s].map(v => v < 10 ? '0' + v : v).join(':');
    }

    function showToast(msg, isErr = false) {
      const t = document.getElementById('toast');
      t.textContent = msg;
      t.style.background = isErr ? '#da3633' : '#238636';
      t.classList.add('show');
      if (toastTimer) clearTimeout(toastTimer);
      toastTimer = setTimeout(() => {
        t.classList.remove('show');
        toastTimer = null;
      }, 3000);
    }

    function isApHost() {
      return !!(lastStatus && lastStatus.setup_ap && lastStatus.setup_ap.ip &&
        location.hostname === lastStatus.setup_ap.ip);
    }

    function setCardOpen(name, open) {
      const body = document.getElementById('body_' + name);
      const arrow = document.getElementById('arrow_' + name);
      body.classList.toggle('open', open);
      arrow.innerHTML = open ? '&#x25B2;' : '&#x25BC;';
    }

    async function toggleCard(name) {
      const body = document.getElementById('body_' + name);
      const willOpen = !body.classList.contains('open');
      setCardOpen(name, willOpen);
      if (willOpen && !cfg && name !== 'fw') {
        await loadConfig();
      }
    }

    function buildWifiRows(count) {
      const wrap = document.getElementById('wifiRows');
      const advWrap = document.getElementById('advWifiRows');
      wrap.textContent = '';
      if (advWrap) advWrap.textContent = '';
      wifiSlotCount = count;
      activeSsidIdx = 0;
      document.getElementById('wifiCardTitle').textContent = 'Wi-Fi Network';

      for (let i = 0; i < count; i++) {
        const isPrimary = (i === 0);
        const slot = document.createElement('div');
        slot.className = 'wifi-slot';

        const head = document.createElement('div');
        head.className = 'wifi-slot-head';
        const lbl = document.createElement('span');
        lbl.textContent = isPrimary ? 'Slot 1 (Primary)' : ('Slot ' + (i + 1));
        const delBtn = document.createElement('button');
        delBtn.type = 'button';
        delBtn.className = 'btn-sec btn-sm';
        delBtn.textContent = 'Delete';
        delBtn.onclick = () => clearWifiRow(i);
        head.appendChild(lbl);
        head.appendChild(delBtn);

        const ssidIn = document.createElement('input');
        ssidIn.type = 'text';
        ssidIn.id = 'w_ssid_' + i;
        ssidIn.maxLength = LIMITS.ssidMaxBytes;
        ssidIn.placeholder = 'SSID (empty = unused)';
        ssidIn.setAttribute('list', 'scanList');
        ssidIn.onfocus = () => { activeSsidIdx = i; };

        const passIn = document.createElement('input');
        passIn.type = 'password';
        passIn.id = 'w_pass_' + i;
        passIn.maxLength = LIMITS.passMaxBytes;
        passIn.autocomplete = 'new-password';
        passIn.placeholder = 'Password';

        const ctrl = document.createElement('div');
        ctrl.className = 'wifi-slot-controls';
        const chkLbl = document.createElement('label');
        chkLbl.className = 'chk-label';
        const openChk = document.createElement('input');
        openChk.type = 'checkbox';
        openChk.id = 'w_open_' + i;
        openChk.onchange = () => {
          if (isPrimary) {
            dirtyWifi = true;
            showWifiErr('');
          } else {
            dirtyAdvWifi = true;
            showAdvWifiErr('');
          }
          passIn.disabled = openChk.checked;
          if (openChk.checked) {
            passIn.value = '';
            passIn.placeholder = 'Open network (no password)';
          } else {
            const hasP = cfg && cfg.wifi && cfg.wifi[i] && cfg.wifi[i].has_pass;
            passIn.placeholder = hasP ? 'unchanged' : 'Password';
          }
        };
        chkLbl.appendChild(openChk);
        chkLbl.appendChild(document.createTextNode('Open network'));
        ctrl.appendChild(chkLbl);

        slot.appendChild(head);
        slot.appendChild(ssidIn);
        slot.appendChild(passIn);
        slot.appendChild(ctrl);
        (isPrimary || !advWrap ? wrap : advWrap).appendChild(slot);
      }
    }

    function clearWifiRow(i) {
      if (i === 0) {
        dirtyWifi = true;
        showWifiErr('');
      } else {
        dirtyAdvWifi = true;
        showAdvWifiErr('');
      }
      document.getElementById('w_ssid_' + i).value = '';
      const passIn = document.getElementById('w_pass_' + i);
      passIn.value = '';
      passIn.disabled = false;
      passIn.placeholder = 'Password';
      document.getElementById('w_open_' + i).checked = false;
    }

    function applyWifiSlotFromCfg(c, i) {
      const w = (c.wifi && c.wifi[i]) ? c.wifi[i] : { ssid: '', has_pass: false };
      const ssidIn = document.getElementById('w_ssid_' + i);
      const passIn = document.getElementById('w_pass_' + i);
      const openChk = document.getElementById('w_open_' + i);
      ssidIn.value = w.ssid || '';
      passIn.value = '';
      const isOpen = (w.ssid && !w.has_pass);
      openChk.checked = isOpen;
      passIn.disabled = isOpen;
      passIn.placeholder = isOpen ? 'Open network (no password)' : (w.has_pass ? 'unchanged' : 'Password');
    }

    function applyConfigToForm(c, forceCard) {
      cfg = c;
      const incomingCount = Array.isArray(c.wifi) ? c.wifi.length : 0;
      if (incomingCount !== wifiSlotCount) {
        buildWifiRows(incomingCount);
      }
      if (!dirtyWifi || forceCard === 'wifi' || forceCard === 'all') {
        if (wifiSlotCount > 0) applyWifiSlotFromCfg(c, 0);
        showWifiErr('');
        dirtyWifi = false;
      }
      if (!dirtyAdvWifi || forceCard === 'advWifi' || forceCard === 'all') {
        for (let i = 1; i < wifiSlotCount; i++) {
          applyWifiSlotFromCfg(c, i);
        }
        showAdvWifiErr('');
        dirtyAdvWifi = false;
      }

      if (!dirtyDev || forceCard === 'dev' || forceCard === 'all') {
        document.getElementById('cfg_alert_mode').value = String(defVal(c.alert_mode, LIMITS.alertModeDef));
        document.getElementById('cfg_cloud_enabled').checked = !!c.cloud_enabled;
        updateCloudSwitchState();
        document.getElementById('cfg_webhook').value = c.webhook_url || '';
        document.getElementById('cfg_local_url').value = c.local_url || '';
        clearFieldErrors(['cfg_webhook', 'cfg_local_url']);
        dirtyDev = false;
      }

      if (!dirtyAdv || forceCard === 'adv' || forceCard === 'all') {
        document.getElementById('cfg_name').value = c.device_name || LIMITS.deviceNameDefault;
        document.getElementById('cfg_debounce').value = defVal(c.debounce_ms, LIMITS.debounceMs.def);
        document.getElementById('cfg_cooldown').value = defVal(c.restore_cooldown_ms, LIMITS.restoreCooldownMs.def);
        document.getElementById('cfg_snooze').value = defVal(c.snooze_default_min, LIMITS.snoozeDefaultMin.def);
        clearFieldErrors(['cfg_name', 'cfg_debounce', 'cfg_cooldown', 'cfg_snooze']);
        dirtyAdv = false;
      }
      snoozeDefMin = defVal(c.snooze_default_min, LIMITS.snoozeDefaultMin.def);
      document.getElementById('btnSnooze').textContent = 'Snooze ' + snoozeDefMin + 'm';
      renderNotifySummary();

      if (!dirtySec || forceCard === 'sec' || forceCard === 'all') {
        document.getElementById('cfg_ap_ssid').value = c.ap_ssid || '';
        const apPass = document.getElementById('cfg_ap_pass');
        apPass.value = '';
        apPass.placeholder = c.has_ap_pass ? 'unchanged' : 'Password';
        document.getElementById('cfg_ap_win').value = defVal(c.setup_window_s, LIMITS.setupWindowS.def);

        const admPass = document.getElementById('cfg_adm_pass');
        admPass.value = '';
        admPass.disabled = false;
        admPass.placeholder = c.has_admin_pass
          ? 'unchanged'
          : ('Not set (optional, ' + LIMITS.passMinBytes + '-' + LIMITS.passMaxBytes + ' bytes)');
        document.getElementById('cfg_clear_adm').checked = false;
        document.getElementById('clearAdmWrap').style.display = c.has_admin_pass ? 'inline-flex' : 'none';
        clearFieldErrors(['cfg_ap_ssid', 'cfg_ap_pass', 'cfg_ap_win', 'cfg_adm_pass']);
        dirtySec = false;
      }
    }

    function toggleClearAdm() {
      dirtySec = true;
      setFieldErr('cfg_adm_pass', '');
      const clr = document.getElementById('cfg_clear_adm').checked;
      const inp = document.getElementById('cfg_adm_pass');
      inp.disabled = clr;
      if (clr) inp.value = '';
    }

    function loadConfig(forceCard, suppressAuthErr) {
      if (cfgPromise) {
        cfgReloadQueued = true;
        if (suppressAuthErr) cfgQueuedSuppressAuth = true;
        if (forceCard) {
          if (cfgQueuedForce === null) cfgQueuedForce = forceCard;
          else if (cfgQueuedForce !== forceCard) cfgQueuedForce = 'all';
        }
        return cfgPromise;
      }
      cfgPromise = (async () => {
        try {
          let nextForce = forceCard;
          let nextSuppress = !!suppressAuthErr;
          while (true) {
            try {
              const res = await apiFetch('/api/config');
              if (!res.ok) {
                if (nextSuppress && res.status === 401) {
                  if (cfg) applyConfigToForm(cfg, nextForce);
                } else {
                  showToast('Auth required or failed to load config', true);
                }
              } else {
                const data = await res.json();
                applyConfigToForm(data, nextForce);
              }
            } catch (e) {
              showToast('Failed to load settings', true);
            }
            if (!cfgReloadQueued) break;
            nextForce = cfgQueuedForce || undefined;
            nextSuppress = cfgQueuedSuppressAuth;
            cfgReloadQueued = false;
            cfgQueuedForce = null;
            cfgQueuedSuppressAuth = false;
          }
        } finally {
          cfgPromise = null;
        }
      })();
      return cfgPromise;
    }

    async function postConfig(payload, savedCard, opts) {
      const o = opts || {};
      try {
        const res = await apiFetch('/api/config', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(payload)
        });
        const text = await res.text();
        let data = {};
        try { data = JSON.parse(text); } catch (e) {}
        if (!res.ok) {
          showToast((data && data.error) ? data.error : (text || 'Validation error'), true);
          return null;
        }
        if (cfg) {
          if (payload.admin_pass !== undefined) cfg.has_admin_pass = true;
          else if (payload.clear_admin_pass) cfg.has_admin_pass = false;
        }
        await loadConfig(savedCard, !!o.suppressAuthErr);
        return data;
      } catch (e) {
        showToast('Failed to save configuration', true);
        return null;
      }
    }

    function updateApSavedBanner() {
      const b = document.getElementById('apSavedBanner');
      if (!b || apSavedSsids.length === 0) return;
      const devName = (cfg && cfg.device_name) ? cfg.device_name : LIMITS.deviceNameDefault;
      const staIp = (lastStatus && lastStatus.wifi && lastStatus.wifi.ip) ? lastStatus.wifi.ip : '';
      const target = staIp
        ? ('http://' + staIp + '/ (or http://' + devName + '.local/)')
        : ('the device IP from your router (or http://' + devName + '.local/)');
      b.textContent = 'Saved for ' + apSavedSsids.join(', ') + '; reconnect to your home Wi-Fi and open ' + target + '.';
      b.style.display = 'block';
    }

    async function saveWifi(startIdx, endIdx) {
      const from = (startIdx === undefined) ? 0 : startIdx;
      const to = (endIdx === undefined) ? wifiSlotCount : Math.min(endIdx, wifiSlotCount);
      const isBackup = (from > 0);
      const errFn = isBackup ? showAdvWifiErr : showWifiErr;
      const cardTag = isBackup ? 'advWifi' : 'wifi';

      errFn('');
      if (!cfg) {
        await loadConfig(cardTag);
        if (!cfg) return;
      }
      const changed = [];
      const changedSsids = [];
      for (let i = from; i < to; i++) {
        const orig = (cfg.wifi && cfg.wifi[i]) ? cfg.wifi[i] : { ssid: '', has_pass: false };
        const ssid = document.getElementById('w_ssid_' + i).value;
        const pass = document.getElementById('w_pass_' + i).value;
        const isOpen = document.getElementById('w_open_' + i).checked;
        const ssidChanged = (ssid !== (orig.ssid || ''));
        const ssidBytes = utf8ByteLen(ssid);
        const passBytes = utf8ByteLen(pass);

        if (ssidBytes > LIMITS.ssidMaxBytes) {
          const msg = 'Slot ' + (i + 1) + ': SSID must be at most ' + LIMITS.ssidMaxBytes + ' bytes';
          errFn(msg);
          showToast(msg, true);
          return;
        }
        if (ssid !== '' && ssidChanged && pass.length === 0 && !isOpen) {
          const msg = 'Slot ' + (i + 1) + ': enter a password or check "Open network"';
          errFn(msg);
          showToast(msg, true);
          return;
        }
        if (ssid !== '' && !isOpen && pass.length > 0 && (passBytes < LIMITS.passMinBytes || passBytes > LIMITS.passMaxBytes)) {
          const msg = 'Slot ' + (i + 1) + ': password must be ' + LIMITS.passMinBytes + '-' + LIMITS.passMaxBytes + ' bytes';
          errFn(msg);
          showToast(msg, true);
          return;
        }

        const entry = { slot: i };
        let dirty = false;
        if (ssidChanged) {
          entry.ssid = ssid;
          dirty = true;
          if (ssid !== '') changedSsids.push(ssid);
        }
        if (ssid === '') {
          if (orig.ssid) dirty = true;
        } else if (isOpen) {
          if (orig.has_pass || ssidChanged) {
            entry.clear_pass = true;
            dirty = true;
          }
        } else if (pass.length > 0) {
          entry.pass = pass;
          dirty = true;
          if (changedSsids.indexOf(ssid) === -1) changedSsids.push(ssid);
        }
        if (dirty) changed.push(entry);
      }

      if (changed.length === 0) {
        showToast(isBackup ? 'No backup Wi-Fi changes to save' : 'No Wi-Fi changes to save');
        return;
      }
      const res = await postConfig({ wifi: changed }, cardTag);
      if (res) {
        showToast(isBackup ? 'Backup Wi-Fi settings saved' : 'Wi-Fi settings saved');
        if (isApHost() && lastStatus && lastStatus.setup_ap && lastStatus.setup_ap.active && changedSsids.length > 0) {
          apSavedSsids = changedSsids.slice();
          updateApSavedBanner();
        }
      }
    }

    async function saveDevice(e) {
      e.preventDefault();
      if (!cfg) { await loadConfig('dev'); if (!cfg) return; }
      clearFieldErrors(['cfg_webhook', 'cfg_local_url']);

      let hasErr = false;
      const webhook = document.getElementById('cfg_webhook').value.trim();
      const webhookErr = validateUrlField(webhook);
      if (webhookErr) { setFieldErr('cfg_webhook', webhookErr); hasErr = true; }

      const localUrl = document.getElementById('cfg_local_url').value.trim();
      const localUrlErr = validateUrlField(localUrl);
      if (localUrlErr) { setFieldErr('cfg_local_url', localUrlErr); hasErr = true; }

      if (hasErr) {
        showToast('Please fix highlighted fields', true);
        return;
      }

      const alertMode = parseInt(document.getElementById('cfg_alert_mode').value, 10);
      const cloudEnabled = !!document.getElementById('cfg_cloud_enabled').checked;
      const p = {};
      if (cloudEnabled !== !!cfg.cloud_enabled) p.cloud_enabled = cloudEnabled;
      if (alertMode !== defVal(cfg.alert_mode, LIMITS.alertModeDef)) p.alert_mode = alertMode;
      if (webhook !== (cfg.webhook_url || '')) p.webhook_url = webhook;
      if (localUrl !== (cfg.local_url || '')) p.local_url = localUrl;

      if (Object.keys(p).length === 0) {
        showToast('No device changes to save');
        return;
      }
      const res = await postConfig(p, 'dev');
      if (res) {
        showToast('Device settings saved');
        fetchStatus();
      }
    }

    async function saveAdvanced(e) {
      e.preventDefault();
      if (!cfg) { await loadConfig('adv'); if (!cfg) return; }
      clearFieldErrors(['cfg_name', 'cfg_debounce', 'cfg_cooldown', 'cfg_snooze']);

      let hasErr = false;
      const name = document.getElementById('cfg_name').value.trim();
      const nameRe = new RegExp('^' + LIMITS.deviceNamePattern + '$');
      if (name.length < LIMITS.deviceNameMin || name.length > LIMITS.deviceNameMax || !nameRe.test(name)) {
        setFieldErr('cfg_name', 'Use ' + LIMITS.deviceNameMin + '-' + LIMITS.deviceNameMax + ' chars [a-z0-9-], no leading or trailing hyphen');
        hasErr = true;
      }

      const deb = parseIntField('cfg_debounce', LIMITS.debounceMs);
      if (!deb.ok) { setFieldErr('cfg_debounce', deb.error); hasErr = true; }

      const cool = parseIntField('cfg_cooldown', LIMITS.restoreCooldownMs);
      if (!cool.ok) { setFieldErr('cfg_cooldown', cool.error); hasErr = true; }

      const snz = parseIntField('cfg_snooze', LIMITS.snoozeDefaultMin);
      if (!snz.ok) { setFieldErr('cfg_snooze', snz.error); hasErr = true; }

      if (hasErr) {
        showToast('Please fix highlighted fields', true);
        return;
      }

      const p = {};
      if (name !== cfg.device_name) p.device_name = name;
      if (deb.value !== defVal(cfg.debounce_ms, LIMITS.debounceMs.def)) p.debounce_ms = deb.value;
      if (cool.value !== defVal(cfg.restore_cooldown_ms, LIMITS.restoreCooldownMs.def)) p.restore_cooldown_ms = cool.value;
      if (snz.value !== defVal(cfg.snooze_default_min, LIMITS.snoozeDefaultMin.def)) p.snooze_default_min = snz.value;

      if (Object.keys(p).length === 0) {
        showToast('No advanced changes to save');
        return;
      }
      const res = await postConfig(p, 'adv');
      if (res) showToast('Advanced settings saved');
    }

    async function saveAccess(e) {
      e.preventDefault();
      if (!cfg) { await loadConfig('sec'); if (!cfg) return; }
      clearFieldErrors(['cfg_ap_ssid', 'cfg_ap_pass', 'cfg_ap_win', 'cfg_adm_pass']);

      let hasErr = false;
      const apSsid = document.getElementById('cfg_ap_ssid').value;
      const apSsidBytes = utf8ByteLen(apSsid);
      if (apSsidBytes < LIMITS.apSsidMinBytes || apSsidBytes > LIMITS.apSsidMaxBytes) {
        setFieldErr('cfg_ap_ssid', 'Must be ' + LIMITS.apSsidMinBytes + '-' + LIMITS.apSsidMaxBytes + ' bytes');
        hasErr = true;
      }

      const apPass = document.getElementById('cfg_ap_pass').value;
      if (apPass.length > 0) {
        const apPassBytes = utf8ByteLen(apPass);
        if (apPassBytes < LIMITS.passMinBytes || apPassBytes > LIMITS.passMaxBytes) {
          setFieldErr('cfg_ap_pass', 'Must be ' + LIMITS.passMinBytes + '-' + LIMITS.passMaxBytes + ' bytes');
          hasErr = true;
        }
      }

      const apWin = parseIntField('cfg_ap_win', LIMITS.setupWindowS);
      if (!apWin.ok) {
        setFieldErr('cfg_ap_win', apWin.error);
        hasErr = true;
      }

      const admPass = document.getElementById('cfg_adm_pass').value;
      const clrAdm = document.getElementById('cfg_clear_adm').checked;
      if (!clrAdm && admPass.length > 0) {
        const admPassBytes = utf8ByteLen(admPass);
        if (admPassBytes < LIMITS.passMinBytes || admPassBytes > LIMITS.passMaxBytes) {
          setFieldErr('cfg_adm_pass', 'Must be ' + LIMITS.passMinBytes + '-' + LIMITS.passMaxBytes + ' bytes');
          hasErr = true;
        }
      }

      if (hasErr) {
        showToast('Please fix highlighted fields', true);
        return;
      }

      const p = {};
      if (apSsid !== (cfg.ap_ssid || '')) p.ap_ssid = apSsid;
      if (apPass.length > 0) p.ap_pass = apPass;
      if (apWin.value !== defVal(cfg.setup_window_s, LIMITS.setupWindowS.def)) p.setup_window_s = apWin.value;
      if (clrAdm) {
        p.clear_admin_pass = true;
      } else if (admPass.length > 0) {
        p.admin_pass = admPass;
      }

      if (Object.keys(p).length === 0) {
        showToast('No access changes to save');
        return;
      }
      const apCredsChanged = (p.ap_ssid !== undefined || p.ap_pass !== undefined);
      const authChanged = (p.admin_pass !== undefined || p.clear_admin_pass === true);
      const res = await postConfig(p, 'sec', { suppressAuthErr: authChanged });
      if (res) {
        showToast(apCredsChanged
          ? 'Setup access saved; setup AP restarts with the new credentials'
          : 'Setup access saved');
        fetchStatus();
      }
    }

    async function scanNetworks() {
      const btn = document.getElementById('btnScan');
      const box = document.getElementById('scanResults');
      const dl = document.getElementById('scanList');
      btn.disabled = true;
      btn.textContent = 'Scanning...';
      let completed = false;
      let failed = false;
      const startMs = Date.now();
      const maxAttempts = Math.ceil(LIMITS.scanTimeoutMs / LIMITS.scanPollIntervalMs);
      try {
        for (let attempt = 0; attempt < maxAttempts; attempt++) {
          const res = await apiFetch('/api/scan');
          if (!res.ok) {
            failed = true;
            box.classList.remove('show');
            showToast('Scan failed', true);
            break;
          }
          const data = await res.json();
          if (data.networks) {
            dl.textContent = '';
            box.textContent = '';
            if (data.networks.length === 0) {
              const empty = document.createElement('div');
              empty.style.fontSize = '0.8rem';
              empty.style.color = 'var(--text-muted)';
              empty.textContent = 'No networks found';
              box.appendChild(empty);
            } else {
              data.networks.forEach(n => {
                const opt = document.createElement('option');
                opt.value = n.ssid;
                dl.appendChild(opt);

                const item = document.createElement('div');
                item.className = 'scan-item';
                const left = document.createElement('span');
                left.textContent = n.ssid + (n.secure ? ' [WPA]' : ' [Open]');
                const right = document.createElement('span');
                right.style.color = 'var(--text-muted)';
                right.textContent = n.rssi + ' dBm (ch ' + n.channel + ')';
                item.appendChild(left);
                item.appendChild(right);
                item.onclick = () => selectScannedNetwork(n);
                box.appendChild(item);
              });
            }
            box.classList.add('show');
            completed = true;
            break;
          }
          if (Date.now() - startMs >= LIMITS.scanTimeoutMs || attempt + 1 >= maxAttempts) {
            break;
          }
          await new Promise(r => setTimeout(r, LIMITS.scanPollIntervalMs));
        }
        if (!completed && !failed) {
          box.textContent = '';
          const timedOut = document.createElement('div');
          timedOut.style.fontSize = '0.8rem';
          timedOut.style.color = 'var(--text-muted)';
          timedOut.textContent = 'Scan timed out, try again';
          box.appendChild(timedOut);
          box.classList.add('show');
        }
      } catch (e) {
        box.classList.remove('show');
        showToast('Scan error', true);
      } finally {
        btn.disabled = false;
        btn.textContent = 'Scan';
      }
    }

    function selectScannedNetwork(n) {
      if (wifiSlotCount <= 0) return;
      let idx = activeSsidIdx < wifiSlotCount ? activeSsidIdx : 0;
      const curVal = document.getElementById('w_ssid_' + idx).value;
      if (curVal !== '') {
        for (let i = 0; i < wifiSlotCount; i++) {
          if (document.getElementById('w_ssid_' + i).value === '') {
            idx = i;
            break;
          }
        }
      }
      if (idx === 0) {
        dirtyWifi = true;
        showWifiErr('');
      } else {
        dirtyAdvWifi = true;
        showAdvWifiErr('');
        setCardOpen('adv', true);
      }
      document.getElementById('w_ssid_' + idx).value = n.ssid;
      const openChk = document.getElementById('w_open_' + idx);
      const passIn = document.getElementById('w_pass_' + idx);
      openChk.checked = !n.secure;
      passIn.disabled = !n.secure;
      if (!n.secure) {
        passIn.value = '';
        passIn.placeholder = 'Open network (no password)';
      } else {
        passIn.placeholder = 'Password';
        passIn.focus();
      }
    }

    function setFwStatus(msg, isErr) {
      const el = document.getElementById('fwStatusMsg');
      if (!msg) {
        el.textContent = '';
        el.style.display = 'none';
        return;
      }
      el.textContent = msg;
      el.style.color = isErr ? '#f85149' : 'var(--text-muted)';
      el.style.display = 'block';
    }

    function updateFwProgress(pct, label) {
      document.getElementById('fwProgressWrap').classList.add('show');
      document.getElementById('fwProgressPct').textContent = pct + '%';
      document.getElementById('fwProgressLabel').textContent = label;
      document.getElementById('fwProgressBar').style.width = pct + '%';
    }

    function updateOtaAvailability(s) {
      if (otaBusy) return;
      const canOta = !!(s && s.admin_protected);
      document.getElementById('fwDisabledHint').style.display = canOta ? 'none' : 'block';
      document.getElementById('fw_file').disabled = !canOta;
      document.getElementById('btnFwUpload').disabled = !canOta;
    }

    async function waitForDeviceAfterOta(prevVersion, prevUptime) {
      const startedAt = Date.now();
      let sawDisconnect = false;
      await new Promise(r => setTimeout(r, 1200));
      while (Date.now() - startedAt < 60000) {
        try {
          const res = await apiFetch('/api/status');
          if (res.ok) {
            const s = await res.json();
            const verChanged = (s.fw_version && prevVersion && s.fw_version !== prevVersion);
            const uptimeReset = (typeof prevUptime === 'number' && s.uptime_s < prevUptime);
            if (sawDisconnect || verChanged || uptimeReset || (Date.now() - startedAt >= 5000)) {
              await fetchStatus();
              const verText = s.fw_version ? (' (version ' + s.fw_version + ')') : '';
              setFwStatus('Device is back online' + verText, false);
              showToast('Firmware update complete' + verText);
              document.getElementById('fw_file').value = '';
              otaBusy = false;
              updateOtaAvailability(lastStatus);
              return;
            }
          } else {
            sawDisconnect = true;
          }
        } catch (e) {
          sawDisconnect = true;
        }
        await new Promise(r => setTimeout(r, 1500));
      }
      setFwStatus('Device did not answer after reboot; reload the page manually.', true);
      otaBusy = false;
      updateOtaAvailability(lastStatus);
    }

    function uploadFirmware() {
      if (otaBusy) return;
      if (!(lastStatus && lastStatus.admin_protected)) {
        const msg = 'Set an admin password to enable firmware updates';
        setFwStatus(msg, true);
        showToast(msg, true);
        return;
      }
      const fileIn = document.getElementById('fw_file');
      if (!fileIn.files || fileIn.files.length === 0) {
        const msg = 'Select a .bin firmware file first';
        setFwStatus(msg, true);
        showToast(msg, true);
        return;
      }

      const prevVersion = lastStatus ? lastStatus.fw_version : null;
      const prevUptime = lastStatus ? lastStatus.uptime_s : null;
      otaBusy = true;
      fileIn.disabled = true;
      document.getElementById('btnFwUpload').disabled = true;
      setFwStatus('', false);
      updateFwProgress(0, 'Uploading...');

      const form = new FormData();
      form.append('firmware', fileIn.files[0], fileIn.files[0].name);

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/update', true);
      xhr.setRequestHeader('X-Requested-With', 'pm-ui');
      xhr.upload.onprogress = (ev) => {
        if (ev.lengthComputable && ev.total > 0) {
          const pct = Math.min(100, Math.round((ev.loaded / ev.total) * 100));
          updateFwProgress(pct, 'Uploading...');
        }
      };
      xhr.onload = () => {
        let data = {};
        try { data = JSON.parse(xhr.responseText); } catch (e) {}
        if (xhr.status >= 200 && xhr.status < 300) {
          updateFwProgress(100, 'Rebooting...');
          setFwStatus('Firmware written, device is rebooting', false);
          showToast('Firmware written, device is rebooting');
          waitForDeviceAfterOta(prevVersion, prevUptime);
        } else {
          const errMsg = (data && data.error) ? data.error : (xhr.responseText || ('HTTP ' + xhr.status));
          setFwStatus(errMsg, true);
          showToast(errMsg, true);
          otaBusy = false;
          updateOtaAvailability(lastStatus);
        }
      };
      xhr.onerror = () => {
        const errMsg = 'Upload failed (network error)';
        setFwStatus(errMsg, true);
        showToast(errMsg, true);
        otaBusy = false;
        updateOtaAvailability(lastStatus);
      };
      xhr.send(form);
    }

    async function sendAction(endpoint) {
      try {
        const res = await apiFetch(endpoint, { method: 'POST' });
        const text = await res.text();
        let data = {};
        try { data = JSON.parse(text); } catch (e) {}
        if (!res.ok) {
          const errMsg = (data && data.error) ? data.error : (text || 'Action failed');
          showToast(errMsg, true);
          return;
        }
        showToast(data.message || 'OK');
        fetchStatus();
        if (endpoint === '/api/test') setTimeout(fetchStatus, 300);
      } catch (e) {
        showToast('Command failed', true);
      }
    }

    function snoozeAction() {
      sendAction('/api/snooze?min=' + snoozeDefMin);
    }

    async function rebootDevice() {
      if (!confirm('Reboot the Power Outage Monitor?')) return;
      try {
        const res = await apiFetch('/api/reboot', { method: 'POST' });
        const text = await res.text();
        let data = {};
        try { data = JSON.parse(text); } catch (e) {}
        if (!res.ok) {
          showToast((data && data.error) ? data.error : (text || 'Reboot failed'), true);
          return;
        }
        showToast('Device rebooting...');
        setTimeout(() => location.reload(), 4000);
      } catch (e) {
        showToast('Reboot request failed', true);
      }
    }

    async function factoryReset() {
      if (!confirm('Factory reset will erase ALL Wi-Fi credentials and settings. Continue?')) return;
      if (!confirm('Are you sure? The device will reboot into setup AP mode.')) return;
      try {
        const res = await apiFetch('/api/factory-reset', { method: 'POST' });
        const text = await res.text();
        let data = {};
        try { data = JSON.parse(text); } catch (e) {}
        if (!res.ok) {
          showToast((data && data.error) ? data.error : (text || 'Factory reset failed'), true);
          return;
        }
        showToast('Factory reset complete. Rebooting...');
        setTimeout(() => location.reload(), 4000);
      } catch (e) {
        showToast('Factory reset request failed', true);
      }
    }

    async function fetchStatus() {
      try {
        const res = await apiFetch('/api/status');
        if (!res.ok) return;
        const s = await res.json();
        lastStatus = s;

        if (s.snooze_default_min) {
          snoozeDefMin = s.snooze_default_min;
          document.getElementById('btnSnooze').textContent = 'Snooze ' + snoozeDefMin + 'm';
        }

        document.getElementById('adminWarn').style.display = s.admin_protected ? 'none' : 'block';
        updateOtaAvailability(s);

        const fwVer = s.fw_version || '-';
        document.getElementById('diagFw').textContent = fwVer;
        document.getElementById('fwCurrentVer').textContent = fwVer;

        const card = document.getElementById('statusCard');
        const icon = document.getElementById('statusIcon');
        const title = document.getElementById('statusTitle');
        const desc = document.getElementById('statusDesc');
        const timer = document.getElementById('timerBadge');

        if (s.grid === 'ONLINE') {
          isOffline = false;
          card.className = 'status-card online';
          icon.innerHTML = '&#x26A1;';
          title.textContent = 'GRID ONLINE';
          desc.textContent = 'Mains power present \u2013 Normal';
          timer.textContent = (s.outage_duration_s > 0) ? ('Last outage: ' + formatTime(s.outage_duration_s)) : 'Active & Protected';
        } else if (s.grid === 'OFFLINE') {
          isOffline = true;
          liveOutageSec = s.outage_duration_s;
          card.className = 'status-card offline';
          icon.innerHTML = '&#x1F6A8;';
          title.textContent = 'POWER OUTAGE DETECTED';
          desc.textContent = 'Mains power lost';
          timer.textContent = 'Outage: ' + formatTime(liveOutageSec);
        } else {
          isOffline = false;
          card.className = 'status-card';
          icon.innerHTML = '&#x23F3;';
          title.textContent = 'CHECKING POWER';
          desc.textContent = 'Checking mains power...';
          timer.textContent = 'Starting up...';
        }

        const pill = document.getElementById('modePill');
        if (s.mode === 'ARMED') {
          pill.className = 'mode-pill pill-armed';
          pill.textContent = 'ARMED';
        } else if (s.mode === 'SNOOZED') {
          pill.className = 'mode-pill pill-snoozed';
          pill.textContent = 'SNOOZE (' + Math.ceil(s.snooze_remaining_s / 60) + 'm)';
        } else {
          pill.className = 'mode-pill pill-disarmed';
          pill.textContent = 'DISARMED';
        }

        const slotTxt = (typeof s.wifi.slot === 'number' && s.wifi.slot >= 0) ? (' (slot ' + (s.wifi.slot + 1) + ')') : '';
        document.getElementById('diagWifi').textContent = s.wifi.connected ? (s.wifi.ssid + slotTxt) : 'Not connected';
        document.getElementById('diagRssi').textContent = s.wifi.connected ? (s.wifi.rssi + ' dBm') : 'N/A';
        const apInfo = s.setup_ap || {};
        const shownIp = (s.wifi && s.wifi.ip) ? s.wifi.ip : (apInfo.ip ? apInfo.ip : '-');
        document.getElementById('diagIp').textContent = shownIp + ' (' + s.device + '.local)';
        document.getElementById('diagAp').textContent = apInfo.active
          ? (apInfo.ssid + ' (' + apInfo.clients + ' client' + (apInfo.clients === 1 ? '' : 's') + (apInfo.closes_in_s > 0 ? ', ' + apInfo.closes_in_s + 's' : '') + ')')
          : 'Off';
        document.getElementById('btnCloseAp').style.display = apInfo.active ? 'inline-flex' : 'none';
        document.getElementById('diagUptime').textContent = formatTime(s.uptime_s);
        document.getElementById('diagHeap').textContent = Math.round(s.free_heap / 1024) + ' KB';
        renderNotifySummary();

        const apView = isApHost() && apInfo.active;
        document.getElementById('wifiCard').classList.toggle('ap-top', apView);
        document.getElementById('apHintBanner').style.display = apView ? 'block' : 'none';
        if (apSavedSsids.length > 0 && document.getElementById('apSavedBanner').style.display === 'block') {
          updateApSavedBanner();
        }
        if (apView && !apExpandedOnce) {
          apExpandedOnce = true;
          setCardOpen('wifi', true);
          if (!cfg && s.admin_protected) loadConfig();
        }

        if (!cfg && !s.admin_protected) {
          loadConfig();
        }
      } catch (err) {
        console.error('Status fetch error:', err);
      }
    }

    applyLimitsToDom();
    document.getElementById('wifiRows').addEventListener('input', () => { dirtyWifi = true; showWifiErr(''); });
    document.getElementById('advWifiRows').addEventListener('input', () => { dirtyAdvWifi = true; showAdvWifiErr(''); });
    document.getElementById('body_dev').addEventListener('input', (ev) => {
      dirtyDev = true;
      if (ev.target && ev.target.id) setFieldErr(ev.target.id, '');
    });
    document.getElementById('body_dev').addEventListener('change', () => { dirtyDev = true; });
    document.getElementById('form_adv').addEventListener('input', (ev) => {
      dirtyAdv = true;
      if (ev.target && ev.target.id) setFieldErr(ev.target.id, '');
    });
    document.getElementById('form_adv').addEventListener('change', () => { dirtyAdv = true; });
    document.getElementById('body_sec').addEventListener('input', (ev) => {
      dirtySec = true;
      if (ev.target && ev.target.id) setFieldErr(ev.target.id, '');
    });
    document.getElementById('body_sec').addEventListener('change', () => { dirtySec = true; });
    setInterval(() => {
      if (isOffline) {
        liveOutageSec++;
        const timer = document.getElementById('timerBadge');
        if (timer) timer.textContent = 'Outage: ' + formatTime(liveOutageSec);
      }
    }, 1000);
    fetchStatus();
    setInterval(fetchStatus, 2000);
  </script>
</body>
</html>
)rawliteral";

#endif // WEB_UI_H
