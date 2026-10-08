#include "webdashboard.h"

namespace WebDashboard {

QByteArray html()
{
    return QByteArrayLiteral(R"html(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>BeaconFix · ALPR & Surveillance Radar</title>
  <link rel="icon" type="image/png" sizes="32x32" href="/favicon.ico">
  <link rel="apple-touch-icon" href="/api/v1/icon/medium">
  <link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css" integrity="sha256-p4NxAoJBhIIN+hmNHrzRCf9tD/miZyoHS5obTRR9BMY=" crossorigin=""/>
  <script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js" integrity="sha256-20nQCchB9co0qIjJZRGuk2/Z9VM+kNiyxNV1lvTlZBo=" crossorigin=""></script>
  <style>
    :root {
      --bg: #090d16;
      --card-bg: #131c2e;
      --card-border: #1e293b;
      --text: #f1f5f9;
      --text-muted: #94a3b8;
      --accent: #38bdf8;
      --accent-glow: rgba(56, 189, 248, 0.25);
      --danger: #ef4444;
      --danger-glow: rgba(239, 68, 68, 0.35);
      --warning: #f59e0b;
      --success: #10b981;
      --success-glow: rgba(16, 185, 129, 0.25);
      --surface: #1e293b;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      background: var(--bg);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
    }
    header {
      background: rgba(19, 28, 46, 0.85);
      backdrop-filter: blur(12px);
      border-bottom: 1px solid var(--card-border);
      padding: 12px 24px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      position: sticky;
      top: 0;
      z-index: 1000;
    }
    .brand {
      display: flex;
      align-items: center;
      gap: 12px;
    }
    .brand-icon {
      width: 32px;
      height: 32px;
      background: linear-gradient(135deg, #0284c7, #38bdf8);
      border-radius: 8px;
      display: flex;
      align-items: center;
      justify-content: center;
      box-shadow: 0 0 15px var(--accent-glow);
    }
    .brand-icon svg { width: 20px; height: 20px; fill: white; }
    .brand h1 { font-size: 1.15rem; font-weight: 700; letter-spacing: -0.02em; }
    .brand span { font-size: 0.75rem; color: var(--accent); background: rgba(56,189,248,0.12); padding: 2px 8px; border-radius: 9999px; margin-left: 8px; font-weight: 600; }
    .nav-tabs {
      display: flex;
      gap: 8px;
      background: rgba(15, 23, 42, 0.6);
      padding: 4px;
      border-radius: 10px;
      border: 1px solid var(--card-border);
    }
    .nav-btn {
      background: transparent;
      border: none;
      color: var(--text-muted);
      padding: 7px 16px;
      border-radius: 7px;
      font-size: 0.85rem;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.15s ease;
      display: flex;
      align-items: center;
      gap: 6px;
    }
    .nav-btn.active {
      background: var(--surface);
      color: var(--text);
      box-shadow: 0 2px 8px rgba(0,0,0,0.3);
    }
    .nav-btn:hover:not(.active) { color: var(--text); background: rgba(255,255,255,0.05); }
    .header-actions {
      display: flex;
      align-items: center;
      gap: 10px;
    }
    .btn {
      background: var(--surface);
      color: var(--text);
      border: 1px solid var(--card-border);
      padding: 7px 14px;
      border-radius: 7px;
      font-size: 0.82rem;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.15s ease;
      display: inline-flex;
      align-items: center;
      gap: 6px;
    }
    .btn:hover { background: #273549; border-color: #334155; }
    .btn-primary { background: #0284c7; border-color: #0369a1; color: white; }
    .btn-primary:hover { background: #0369a1; }
    .btn-danger { background: #dc2626; border-color: #b91c1c; color: white; }
    .btn-danger:hover { background: #b91c1c; }
    .badge {
      display: inline-flex;
      align-items: center;
      gap: 4px;
      padding: 3px 8px;
      border-radius: 6px;
      font-size: 0.75rem;
      font-weight: 600;
    }
    .badge-danger { background: rgba(239, 68, 68, 0.18); color: #fca5a5; border: 1px solid rgba(239, 68, 68, 0.3); }
    .badge-success { background: rgba(16, 185, 129, 0.18); color: #6ee7b7; border: 1px solid rgba(16, 185, 129, 0.3); }
    .badge-accent { background: rgba(56, 189, 248, 0.18); color: #7dd3fc; border: 1px solid rgba(56, 189, 248, 0.3); }
    .badge-warning { background: rgba(245, 158, 11, 0.18); color: #fcd34d; border: 1px solid rgba(245, 158, 11, 0.3); }

    /* Alert Banner */
    #alertBanner {
      display: none;
      background: linear-gradient(90deg, #991b1b, #dc2626);
      color: white;
      padding: 10px 24px;
      font-size: 0.9rem;
      font-weight: 600;
      align-items: center;
      justify-content: space-between;
      animation: slideDown 0.3s ease;
      box-shadow: 0 4px 15px rgba(220, 38, 38, 0.4);
    }
    @keyframes slideDown { from { transform: translateY(-100%); } to { transform: translateY(0); } }

    /* Stat Cards */
    .stats-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
      gap: 14px;
      padding: 18px 24px 8px 24px;
    }
    .stat-card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 10px;
      padding: 14px 18px;
      display: flex;
      flex-direction: column;
      gap: 4px;
      position: relative;
      overflow: hidden;
    }
    .stat-card::before {
      content: "";
      position: absolute;
      top: 0; left: 0; right: 0; height: 2px;
      background: var(--card-border);
    }
    .stat-card.alert::before { background: var(--danger); box-shadow: 0 0 10px var(--danger); }
    .stat-card.active-plate::before { background: var(--accent); box-shadow: 0 0 10px var(--accent); }
    .stat-card.success::before { background: var(--success); }
    .stat-label { font-size: 0.78rem; text-transform: uppercase; letter-spacing: 0.05em; color: var(--text-muted); font-weight: 600; }
    .stat-value { font-size: 1.65rem; font-weight: 700; color: var(--text); }
    .stat-sub { font-size: 0.75rem; color: var(--text-muted); }

    /* Content Area */
    main { flex: 1; padding: 16px 24px 24px 24px; display: flex; flex-direction: column; gap: 16px; }
    .tab-content { display: none; flex-direction: column; gap: 16px; flex: 1; }
    .tab-content.active { display: flex; }

    /* Map Tab */
    #mapContainer {
      position: relative;
      height: 600px;
      border-radius: 12px;
      overflow: hidden;
      border: 1px solid var(--card-border);
      box-shadow: 0 8px 24px rgba(0,0,0,0.4);
    }
    #map { width: 100%; height: 100%; }
    .map-overlay-card {
      position: absolute;
      top: 16px;
      right: 16px;
      z-index: 800;
      background: rgba(19, 28, 46, 0.92);
      backdrop-filter: blur(10px);
      border: 1px solid var(--card-border);
      border-radius: 10px;
      padding: 14px;
      max-width: 320px;
      font-size: 0.82rem;
      box-shadow: 0 4px 16px rgba(0,0,0,0.5);
    }
    .map-legend { display: flex; flex-direction: column; gap: 8px; margin-top: 8px; }
    .legend-item { display: flex; align-items: center; gap: 8px; }
    .legend-dot { width: 12px; height: 12px; border-radius: 50%; }

    /* Tables */
    .table-container {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 10px;
      overflow: hidden;
      box-shadow: 0 4px 12px rgba(0,0,0,0.2);
    }
    .table-header {
      padding: 14px 18px;
      border-bottom: 1px solid var(--card-border);
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .table-header h2 { font-size: 1.05rem; font-weight: 700; }
    table { width: 100%; border-collapse: collapse; text-align: left; font-size: 0.85rem; }
    th {
      background: rgba(15, 23, 42, 0.7);
      color: var(--text-muted);
      font-weight: 600;
      padding: 10px 16px;
      border-bottom: 1px solid var(--card-border);
    }
    td { padding: 12px 16px; border-bottom: 1px solid rgba(30, 41, 59, 0.6); }
    tr:last-child td { border-bottom: none; }
    tr:hover td { background: rgba(255, 255, 255, 0.02); }

    /* Plates Grid */
    .plates-grid {
      display: grid;
      grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
      gap: 16px;
    }
    .plate-card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 12px;
      padding: 18px;
      display: flex;
      flex-direction: column;
      gap: 12px;
      position: relative;
    }
    .plate-card.active {
      border-color: #0284c7;
      box-shadow: 0 0 20px rgba(2, 132, 199, 0.2);
    }
    .plate-card-header {
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .plate-emblem {
      background: #1e293b;
      border: 2px solid #cbd5e1;
      color: #0f172a;
      border-radius: 6px;
      padding: 6px 14px;
      display: inline-flex;
      flex-direction: column;
      align-items: center;
      font-weight: 800;
      letter-spacing: 0.08em;
      box-shadow: 0 3px 6px rgba(0,0,0,0.3);
      position: relative;
      background: linear-gradient(180deg, #f8fafc, #e2e8f0);
    }
    .plate-state { font-size: 0.65rem; color: #475569; text-transform: uppercase; margin-bottom: -2px; }
    .plate-number { font-size: 1.25rem; font-family: monospace; font-weight: 900; }
    .vehicle-details { font-size: 0.85rem; color: var(--text-muted); display: flex; flex-direction: column; gap: 4px; }
    .vehicle-title { font-size: 1rem; font-weight: 700; color: var(--text); }
    .plate-actions { display: flex; gap: 8px; margin-top: auto; padding-top: 8px; border-top: 1px solid var(--card-border); }

    /* Modal */
    .modal-overlay {
      display: none;
      position: fixed;
      top: 0; left: 0; right: 0; bottom: 0;
      background: rgba(0, 0, 0, 0.7);
      backdrop-filter: blur(5px);
      z-index: 2000;
      align-items: center;
      justify-content: center;
    }
    .modal-card {
      background: var(--card-bg);
      border: 1px solid var(--card-border);
      border-radius: 12px;
      width: 100%;
      max-width: 480px;
      padding: 24px;
      box-shadow: 0 12px 32px rgba(0,0,0,0.6);
      display: flex;
      flex-direction: column;
      gap: 16px;
    }
    .form-group { display: flex; flex-direction: column; gap: 6px; }
    .form-group label { font-size: 0.8rem; font-weight: 600; color: var(--text-muted); }
    .form-group input, .form-group textarea, .form-group select {
      background: #0f172a;
      border: 1px solid var(--card-border);
      color: var(--text);
      padding: 9px 12px;
      border-radius: 7px;
      font-size: 0.88rem;
    }
    .form-group input:focus, .form-group textarea:focus {
      outline: none;
      border-color: var(--accent);
      box-shadow: 0 0 0 2px var(--accent-glow);
    }
    .form-row { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }

    /* Custom Leaflet Marker Styling */
    .cam-pin {
      width: 24px;
      height: 24px;
      border-radius: 50%;
      background: #0284c7;
      border: 2px solid white;
      box-shadow: 0 0 10px rgba(0,0,0,0.6);
      display: flex;
      align-items: center;
      justify-content: center;
      color: white;
      font-size: 10px;
      font-weight: bold;
    }
    .cam-pin.passed {
      background: #dc2626;
      border-color: #fca5a5;
      box-shadow: 0 0 14px rgba(220, 38, 38, 0.8);
      animation: pulseAlert 2s infinite;
    }
    @keyframes pulseAlert {
      0% { box-shadow: 0 0 0 0 rgba(220, 38, 38, 0.7); }
      70% { box-shadow: 0 0 0 12px rgba(220, 38, 38, 0); }
      100% { box-shadow: 0 0 0 0 rgba(220, 38, 38, 0); }
    }
    .user-pin {
      width: 16px;
      height: 16px;
      border-radius: 50%;
      background: #38bdf8;
      border: 2px solid white;
      box-shadow: 0 0 12px #38bdf8;
    }
  </style>
</head>
<body>

  <!-- Top Real-time Alert Banner -->
  <div id="alertBanner">
    <div style="display:flex; align-items:center; gap:10px;">
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>
      <span id="alertBannerText">Camera Encounter Detected</span>
    </div>
    <button class="btn" style="padding:4px 10px; font-size:0.75rem; background:rgba(0,0,0,0.3);" onclick="dismissAlert()">Dismiss</button>
  </div>

  <header>
    <div class="brand">
      <img src="/api/v1/icon/medium" alt="BeaconFix" style="width:38px; height:38px; border-radius:50%; box-shadow:0 0 12px rgba(56,189,248,0.5); object-fit:contain; flex-shrink:0;">
      <div>
        <h1>BeaconFix · ALPR Intelligence</h1>
      </div>
      <span>v3.9</span>
    </div>

    <nav class="nav-tabs">
      <button class="nav-btn active" onclick="switchTab('mapTab', this)">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polygon points="1 6 1 22 8 18 16 22 23 18 23 2 16 6 8 2 1 6"/><line x1="8" y1="2" x2="8" y2="18"/><line x1="16" y1="6" x2="16" y2="22"/></svg>
        Map
      </button>
      <button class="nav-btn" onclick="switchTab('platesTab', this)">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="2" y="5" width="20" height="14" rx="2"/><line x1="2" y1="10" x2="22" y2="10"/></svg>
        Vehicles & Plates
      </button>
      <button class="nav-btn" onclick="switchTab('encountersTab', this)">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 14 14"/></svg>
        Pass Encounters
      </button>
      <button class="nav-btn" onclick="switchTab('auditsTab', this)">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/><line x1="16" y1="13" x2="8" y2="13"/><line x1="16" y1="17" x2="8" y2="17"/></svg>
        Plate Events
      </button>
      <button class="nav-btn" onclick="switchTab('telegramTab', this)">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="22" y1="2" x2="11" y2="13"/><polygon points="22 2 15 22 11 13 2 9 22 2"/></svg>
        Telegram
      </button>
      <button class="nav-btn" onclick="switchTab('nodesTab', this); loadEspPorts();">
        <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20"/></svg>
        ESP32 Nodes
      </button>
    </nav>

    <div class="header-actions">
      <button class="btn" id="audioToggleBtn" onclick="toggleAudio()">
        <span id="audioIcon">🔊</span> Audio: ON
      </button>
      <button class="btn" id="syncUsBtn" onclick="triggerSyncUs()" title="Download and synchronize all accessible Flock and ALPR cameras across the entire United States">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>
        <span id="syncUsBtnText">Sync Nationwide (US)</span>
      </button>
      <button class="btn" onclick="triggerRecalculate()" title="Recompute passes using your stored route points">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/></svg>
        Recalculate Passes
      </button>
      <button class="btn btn-primary" onclick="triggerCrossref()" title="Cross-reference with open ALPR and transparency databases">
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="8"/><line x1="21" y1="21" x2="16.65" y2="16.65"/></svg>
        Cross-Ref Open DB
      </button>
    </div>
  </header>

  <!-- Summary Stats Bar -->
  <section class="stats-grid">
    <div class="stat-card alert">
      <span class="stat-label">Cameras Passed</span>
      <span class="stat-value" id="statPassedCams">0</span>
      <span class="stat-sub" id="statPassedSub">Unique ALPRs encountered</span>
    </div>
    <div class="stat-card">
      <span class="stat-label">Total Passes Logged</span>
      <span class="stat-value" id="statTotalEncounters">0</span>
      <span class="stat-sub">Recorded drive-by passes</span>
    </div>
    <div class="stat-card active-plate">
      <span class="stat-label">Active Vehicle</span>
      <span class="stat-value" style="font-size:1.3rem; font-family:monospace;" id="statActivePlate">—</span>
      <span class="stat-sub" id="statActiveVehicle">no plate registered</span>
    </div>
    <div class="stat-card">
      <span class="stat-label">Public Cameras Tracked</span>
      <span class="stat-value" id="statTotalCams">0</span>
      <span class="stat-sub" id="statVettedCams">0 vetted cameras</span>
    </div>
  </section>

  <main>
    <!-- Tab 1: Interactive Map -->
    <div id="mapTab" class="tab-content active">
      <div id="mapContainer">
        <div id="map"></div>
        <div class="map-overlay-card">
          <h3 style="font-size:0.95rem; font-weight:700; margin-bottom:4px;">Surveillance Camera Map</h3>
          <p style="color:var(--text-muted); font-size:0.75rem;">Showing ALPR cameras, capture zones, and your real-time vehicle route.</p>
          <div class="map-legend">
            <div class="legend-item">
              <div class="legend-dot" style="background:#dc2626; box-shadow:0 0 8px #dc2626;"></div>
              <span><strong>Encountered ALPR</strong> (Pass Count &gt; 0)</span>
            </div>
            <div class="legend-item">
              <div class="legend-dot" style="background:#0284c7;"></div>
              <span>Open Database / DeFlock Camera</span>
            </div>
            <div class="legend-item">
              <div class="legend-dot" style="background:#38bdf8; box-shadow:0 0 8px #38bdf8;"></div>
              <span>Current GPS Location</span>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Tab 2: Vehicle & License Plate Management -->
    <div id="platesTab" class="tab-content">
      <div style="display:flex; justify-content:space-between; align-items:center;">
        <div>
          <h2 style="font-size:1.15rem; font-weight:700;">Registered Vehicles & License Plates</h2>
          <p style="font-size:0.8rem; color:var(--text-muted); margin-top:2px;">
            Manage the license plates cross-referenced against public camera sightings and route passes.
          </p>
        </div>
        <button class="btn btn-primary" onclick="openAddPlateModal()">
          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>
          Add License Plate
        </button>
      </div>

      <div class="plates-grid" id="platesGrid">
        <!-- Rendered dynamically -->
      </div>
    </div>

    <!-- Tab 3: ALPR Pass Encounters -->
    <div id="encountersTab" class="tab-content">
      <div class="table-container">
        <div class="table-header">
          <div>
            <h2>Camera Pass Encounters</h2>
            <p style="font-size:0.78rem; color:var(--text-muted); margin-top:2px;">
              Every logged drive-by pass within camera capture proximity (65 m threshold).
            </p>
          </div>
          <button class="btn" onclick="fetchEncounters()">
            <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="23 4 23 10 17 10"/><polyline points="1 20 1 14 7 14"/><path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"/></svg>
            Refresh
          </button>
        </div>
        <table>
          <thead>
            <tr>
              <th>Time</th>
              <th>Camera Operator & Model</th>
              <th>Pass Number</th>
              <th>Distance</th>
              <th>Plate & Vehicle</th>
              <th>Coordinates</th>
            </tr>
          </thead>
          <tbody id="encountersTableBody">
            <tr><td colspan="6" style="text-align:center; color:var(--text-muted);">Loading encounters...</td></tr>
          </tbody>
        </table>
      </div>
    </div>

    <!-- Tab 4: Plate events (docs/SIGHTINGS.md): ALPR passes and plate searches -->
    <div id="auditsTab" class="tab-content">
      <div class="table-container">
        <div class="table-header">
          <div>
            <h2>Plate events</h2>
            <p style="font-size:0.78rem; color:var(--text-muted); margin-top:2px;">
              Camera passes from your own route (near an ALPR your plate was likely read; traffic cameras do not read plates) and
              searches for your plate in Flock audit logs released through public-records requests (via HaveIBeenFlocked).
            </p>
          </div>
          <div style="display:flex; gap:8px;">
            <button class="btn btn-primary" onclick="triggerCrossref()">Check audit logs now</button>
            <button class="btn" onclick="fetchAudits()">Refresh</button>
          </div>
        </div>
        <table>
          <thead>
            <tr>
              <th>Time</th>
              <th>Kind</th>
              <th>Plate</th>
              <th>Camera / agency</th>
              <th>Confidence</th>
              <th>What it means</th>
            </tr>
          </thead>
          <tbody id="auditsTableBody">
            <tr><td colspan="6" style="text-align:center; color:var(--text-muted);">Loading plate events...</td></tr>
          </tbody>
        </table>
      </div>
    </div>

    <!-- Tab 5: Telegram bot (own token, paired by a one-time code) -->
    <div id="telegramTab" class="tab-content">
      <div class="table-container">
        <div class="table-header">
          <div>
            <h2>Telegram Alerts</h2>
            <p style="font-size:0.78rem; color:var(--text-muted); margin-top:2px;">
              Camera-pass alerts and /status on your own bot. Create one with @BotFather, paste its token, then pair your chat.
            </p>
          </div>
          <button class="btn" onclick="fetchTelegram()">Refresh</button>
        </div>
        <div style="padding:16px 18px; display:flex; flex-direction:column; gap:14px; font-size:0.85rem;">
          <div><strong>Status:</strong> <span id="tgStatus">Loading...</span></div>
          <div id="tgPair" style="display:none; background:rgba(56,189,248,0.08); border:1px solid var(--card-border); border-radius:8px; padding:10px 12px;">
            Pair your chat: send <code id="tgPairCmd" style="font-size:0.95rem; font-weight:700;"></code> to
            <a id="tgPairLink" target="_blank" rel="noopener" style="color:#38bdf8;">your bot</a> from your Telegram account.
            Messages from any other chat are ignored.
          </div>
          <form onsubmit="saveTelegramToken(event)" style="display:flex; gap:10px; align-items:flex-end; flex-wrap:wrap;">
            <div class="form-group" style="flex:1; min-width:240px;">
              <label>Bot token</label>
              <input type="password" id="tgToken" autocomplete="off" placeholder="123456:ABC... (leave empty to switch the bot off)">
            </div>
            <button type="submit" class="btn btn-primary">Save Token</button>
            <button type="button" class="btn btn-danger" id="tgUnpairBtn" style="display:none;" onclick="unpairTelegram()">Unpair Chat</button>
          </form>
        </div>
      </div>
    </div>

    <!-- ESP32 Nodes Tab -->
    <div id="nodesTab" class="tab-content" style="padding: 20px; max-width: 1000px; margin: 0 auto; width: 100%;">
      <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom: 20px; flex-wrap:wrap; gap:12px;">
        <div>
          <h2 style="font-size:1.4rem; font-weight:700;">ESP32 Sniffer & Monitor Nodes</h2>
          <p style="font-size:0.85rem; color:var(--text-muted); margin-top:4px;">Manage wireless promiscuous monitor nodes, 3.7V vape battery telemetry, and network flashing.</p>
        </div>
        <button class="btn btn-primary" onclick="flashEspNode()" id="btnFlashEsp" style="display:flex; align-items:center; gap:8px;">
          ⚡ Flash Connected ESP32
        </button>
      </div>

      <div style="display:grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 16px; margin-bottom: 24px;">
        <div class="card" style="padding:16px;">
          <h3 style="font-size:1rem; font-weight:600; margin-bottom:12px; color:var(--accent);">Connected Serial Ports</h3>
          <div id="espPortsList" style="font-family:monospace; font-size:0.85rem; color:var(--text-muted);">Scanning ports...</div>
          <button class="btn" style="margin-top:12px; font-size:0.8rem; padding:4px 10px;" onclick="loadEspPorts()">🔄 Refresh Ports</button>
        </div>
        <div class="card" style="padding:16px;">
          <h3 style="font-size:1rem; font-weight:600; margin-bottom:12px; color:var(--success);">Telemetry & Auto-Naming</h3>
          <p style="font-size:0.85rem; color:var(--text-muted); line-height:1.6;">
            • <b>Reddit-Style Name:</b> e.g. <code>SwiftFalconRanger4821</code> (auto-seeded from MAC, customizable in NVS)<br>
            • <b>Wi-Fi SoftAP:</b> SSID matches Node Name @ <code>192.168.4.1</code><br>
            • <b>Web OTA & Config:</b> <a href="http://192.168.4.1/" target="_blank" style="color:var(--accent);">http://192.168.4.1/</a><br>
            • <b>UDP Broadcast:</b> Port <code>47824</code> | <b>BLE NUS:</b> UUID <code>6E400001-...</code><br>
            • <b>Vape Battery:</b> Pin GPIO 35 (2:1 divider, 100kΩ/100kΩ)
          </p>
        </div>
      </div>

      <!-- Node Battery Capacity & Runtime Estimation Card -->
      <div class="card" style="padding:18px; margin-bottom:24px;">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:12px; flex-wrap:wrap; gap:10px;">
          <div>
            <h3 style="font-size:1.1rem; font-weight:700; color:var(--success);">🔋 Node Battery Volume & Runtime Estimation</h3>
            <p style="font-size:0.82rem; color:var(--text-muted); margin-top:2px;">
              Configure battery volume (mAh) for each node (e.g. 240 mAh for Heltec V3) to compute accurate runtime estimation and drain telemetry.
            </p>
          </div>
          <button class="btn" onclick="loadNodeBatteries()" style="font-size:0.8rem; padding:4px 10px;">🔄 Refresh Batteries</button>
        </div>
        <div id="nodeBatteriesList" style="display:grid; grid-template-columns:repeat(auto-fit, minmax(280px, 1fr)); gap:12px;">
          <div style="color:var(--text-muted); font-size:0.85rem;">Scanning active nodes...</div>
        </div>
      </div>

      <!-- 3D Nameplate Generator & Node Minting Card (NAMEPLATE_SPEC.md) -->
      <div class="card" style="padding:18px; margin-bottom:24px;">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:12px; flex-wrap:wrap; gap:10px;">
          <div>
            <h3 style="font-size:1.1rem; font-weight:700; color:var(--accent);">🏷 3D Nameplate Generator & Node Minting (NAMEPLATE_SPEC.md)</h3>
            <p style="font-size:0.82rem; color:var(--text-muted); margin-top:2px;">
              Generate slide-and-clip 3D-printable nameplate STLs for Heltec V3 and ESP32 enclosure retention rails.
            </p>
          </div>
          <button class="btn btn-primary" onclick="mintNewNodeWeb()" style="display:flex; align-items:center; gap:6px;">
            ✨ Mint New Node & 3D STL
          </button>
        </div>

        <div style="background:#090d16; border:1px solid var(--card-border); border-radius:8px; padding:14px; margin-bottom:16px;">
          <div style="display:grid; grid-template-columns:repeat(auto-fit, minmax(180px, 1fr)); gap:12px; align-items:end;">
            <div>
              <label style="display:block; font-size:0.75rem; color:var(--text-muted); margin-bottom:4px;">Node / Plate Text</label>
              <input type="text" id="nameplateInputName" placeholder="e.g. CosmicFalconMaster9463" style="width:100%; padding:8px 12px; background:var(--surface); border:1px solid var(--card-border); border-radius:6px; color:#fff; font-family:monospace; font-size:0.85rem;">
            </div>
            <div>
              <label style="display:block; font-size:0.75rem; color:var(--text-muted); margin-bottom:4px;">Rail Width / Units</label>
              <select id="nameplateInputUnits" style="width:100%; padding:8px 12px; background:var(--surface); border:1px solid var(--card-border); border-radius:6px; color:#fff; font-size:0.85rem;">
                <option value="4" selected>4U (31.7 mm) — Full Span</option>
                <option value="3">3U (23.7 mm) — 3-Unit Tile</option>
                <option value="2">2U (15.7 mm) — 2-Unit Tile</option>
                <option value="1">1U (7.7 mm) — 1-Unit Tile</option>
              </select>
            </div>
            <div>
              <label style="display:block; font-size:0.75rem; color:var(--text-muted); margin-bottom:4px;">Text Style</label>
              <select id="nameplateInputMode" style="width:100%; padding:8px 12px; background:var(--surface); border:1px solid var(--card-border); border-radius:6px; color:#fff; font-size:0.85rem;">
                <option value="false" selected>Full Name (Debossed)</option>
                <option value="true">Short Callsign (e.g. CF94)</option>
              </select>
            </div>
            <div>
              <button class="btn" onclick="generateNameplateWeb()" style="width:100%; padding:8px 14px; display:flex; align-items:center; justify-content:center; gap:6px;">
                🖨 Generate Nameplate STL
              </button>
            </div>
          </div>
          <div id="nameplateResult" style="display:none; margin-top:12px; padding:10px 14px; background:rgba(56,189,248,0.1); border:1px solid rgba(56,189,248,0.3); border-radius:6px; font-size:0.82rem;"></div>
        </div>

        <div>
          <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:8px;">
            <h4 style="font-size:0.9rem; font-weight:600; color:var(--text);">Minted 3D Nameplates Library</h4>
            <button class="btn" style="font-size:0.75rem; padding:3px 8px;" onclick="loadNameplatesWeb()">🔄 Refresh STLs</button>
          </div>
          <div id="nameplatesList" style="display:grid; grid-template-columns:repeat(auto-fill, minmax(280px, 1fr)); gap:10px;">
            <div style="color:var(--text-muted); font-size:0.8rem;">Loading nameplates...</div>
          </div>
        </div>
      </div>

      <!-- Hardware & Wiring Guide Card -->
      <div class="card" style="padding:18px; margin-bottom:24px;">
        <h3 style="font-size:1.1rem; font-weight:700; color:var(--accent); margin-bottom:8px;">📐 Hardware Wiring & Pinout Safety Matrix</h3>
        <p style="font-size:0.82rem; color:var(--text-muted); margin-bottom:14px;">
          Where to wire and <b>where NOT to wire</b> external LEDs and 3.7V vape Li-ion batteries.
        </p>

        <table style="width:100%; border-collapse:collapse; margin-bottom:16px; font-size:0.82rem;">
          <thead>
            <tr style="border-bottom:1px solid var(--card-border); text-align:left; color:var(--text-muted);">
              <th style="padding:8px;">Category</th>
              <th style="padding:8px;">GPIO Pins</th>
              <th style="padding:8px;">Designated Function</th>
              <th style="padding:8px;">Safety Instructions</th>
            </tr>
          </thead>
          <tbody>
            <tr style="border-bottom:1px solid rgba(255,255,255,0.05);">
              <td style="padding:8px; color:var(--success); font-weight:600;">✅ Safe LED Outputs</td>
              <td style="padding:8px; font-family:monospace;">GPIO 2, 4, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33</td>
              <td style="padding:8px;">Single LEDs & 4-Pin RGB (R, G, B)</td>
              <td style="padding:8px;">Always use 220Ω - 330Ω current limiting resistor!</td>
            </tr>
            <tr style="border-bottom:1px solid rgba(255,255,255,0.05);">
              <td style="padding:8px; color:#f59e0b; font-weight:600;">⚡ Input-Only (ADC)</td>
              <td style="padding:8px; font-family:monospace;">GPIO 34, 35, 36 (VP), 39 (VN)</td>
              <td style="padding:8px;">Battery Voltage Monitoring (GPIO 35 default)</td>
              <td style="padding:8px;"><b>Input ONLY!</b> Cannot output high/low for LEDs.</td>
            </tr>
            <tr style="border-bottom:1px solid rgba(255,255,255,0.05);">
              <td style="padding:8px; color:#ef4444; font-weight:600;">❌ Boot Strapping</td>
              <td style="padding:8px; font-family:monospace;">GPIO 0, 12, 15</td>
              <td style="padding:8px;">Internal Boot Control</td>
              <td style="padding:8px;">Do not pull LOW at boot or board enters ROM download mode.</td>
            </tr>
            <tr style="border-bottom:1px solid rgba(255,255,255,0.05);">
              <td style="padding:8px; color:#ef4444; font-weight:600;">❌ USB Serial Port</td>
              <td style="padding:8px; font-family:monospace;">GPIO 1 (TX0), GPIO 3 (RX0)</td>
              <td style="padding:8px;">USB CP2102 Serial Bridge</td>
              <td style="padding:8px;">Wiring LEDs here interferes with flashing & telemetry!</td>
            </tr>
            <tr>
              <td style="padding:8px; color:#ef4444; font-weight:600;">❌ SPI Flash Memory</td>
              <td style="padding:8px; font-family:monospace;">GPIO 6, 7, 8, 9, 10, 11</td>
              <td style="padding:8px;">Internal SPI Flash Bus</td>
              <td style="padding:8px;"><b>NEVER CONNECT!</b> Causes instant board freeze or flash brick.</td>
            </tr>
          </tbody>
        </table>

        <!-- Visual Wiring ASCII Schematics -->
        <div style="display:grid; grid-template-columns:repeat(auto-fit, minmax(280px, 1fr)); gap:12px;">
          <div style="background:#090d16; border:1px dashed var(--card-border); border-radius:8px; padding:12px; font-family:monospace; font-size:0.75rem; color:#38bdf8; overflow-x:auto;">
<b>🔋 3.7V Vape Battery Divider (GPIO 35)</b>
[3.7V Vape Pouch Cell]
  (+) Red Wire   ──> TP4056 B+ ──> OUT+ ──> ESP32 VIN (5V)
  (-) Black Wire ──> TP4056 B- ──> OUT- ──> ESP32 GND

[100kΩ/100kΩ Precision Divider]:
  Vape (+) ──[100kΩ]──┬──> ESP32 GPIO 35
                      │
                  [100kΩ]
                      │
  ESP32 GND ──────────┴─────────────────
          </div>

          <div style="background:#090d16; border:1px dashed var(--card-border); border-radius:8px; padding:12px; font-family:monospace; font-size:0.75rem; color:#38bdf8; overflow-x:auto;">
<b>💡 Single LED (Waterproof Case Satellite)</b>
ESP32 GPIO 4 ──[ 330Ω Resistor ]──> Anode (+) [Long leg]
                                    Cathode (-) [Flat edge]
ESP32 GND    ─────────────────────> Ground

<b>Multi-LED Roles:</b>
• <b>Mirror:</b> Mirrors internal status/heartbeat
• <b>Battery-Only:</b> Full-time battery level gauge
• <b>Sniffer:</b> Wi-Fi probe/beacon strobe
• <b>Tracker:</b> BLE AirTag/Tile alarm
          </div>

          <div style="background:#090d16; border:1px dashed var(--card-border); border-radius:8px; padding:12px; font-family:monospace; font-size:0.75rem; color:#38bdf8; overflow-x:auto;">
<b>🌈 4-Pin RGB LED (Common Cathode / Anode)</b>
[Common Cathode (GND Common)]:
  Pin 1 (Red)   ──[ 330Ω ]──> ESP32 GPIO 18
  Pin 2 (Common)────────────> ESP32 GND
  Pin 3 (Green) ──[ 220Ω ]──> ESP32 GPIO 19
  Pin 4 (Blue)  ──[ 220Ω ]──> ESP32 GPIO 23

[RGB Full-Time Battery Indication]:
  • Full: Solid Green  • Dying: Rapid Red
  • Half: Solid Yellow • Charge: Cyan Breath
  • USB (No Batt): Soft Ice Blue Dual-Tick
          </div>
        </div>
      </div>

      <!-- Battery Diagnostic Troubleshooter Card -->
      <div class="card" style="padding:18px; margin-bottom:24px;">
        <h3 style="font-size:1.1rem; font-weight:700; color:var(--accent); margin-bottom:8px;">🔍 Battery Diagnostics & Troubleshooter</h3>
        <p style="font-size:0.82rem; color:var(--text-muted); margin-bottom:12px;">
          Troubleshoot "Why can't the ESP see my battery?" when wiring recovered 3.7V vape pouch cells.
        </p>
        <div style="background:#090d16; border:1px solid var(--card-border); border-radius:8px; padding:14px; font-size:0.82rem; line-height:1.6;">
          <b style="color:var(--accent);">Diagnosis & Checklist:</b>
          <ol style="margin:8px 0 0 18px; color:var(--text-muted);">
            <li><b style="color:#f8fafc;">Common Ground (GND):</b> The ESP32 GND and TP4056 GND <i>must</i> be physically tied together. If grounds are isolated, the ADC has no voltage reference and floats near 0V.</li>
            <li><b style="color:#f8fafc;">Precision Voltage Divider:</b> Check resistor 1 (100kΩ) between Battery Positive (+) and GPIO 35, and resistor 2 (100kΩ) between GPIO 35 and ESP32 GND.</li>
            <li><b style="color:#f8fafc;">Vape Cell Protection Tripped:</b> Single-cell vape batteries have safety protection chips that disconnect the cell if depleted below ~2.5V. Plug the TP4056 into USB-C for 15 minutes to awaken the cell.</li>
            <li><b style="color:#f8fafc;">USB-Only Operation:</b> If the node is powered via USB 5V without a battery attached, the firmware automatically detects this (< 1.2V), sets <code>batt_state: "no_battery"</code>, and plays a calm dual-tick pattern instead of false dying alarms.</li>
          </ol>
        </div>
      </div>

      <div class="card" style="padding:16px;">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:10px;">
          <h3 style="font-size:1rem; font-weight:600;">Flashing & Telemetry Console</h3>
          <span id="flashStatusBadge" class="badge" style="display:none; background:var(--surface); padding:2px 8px; border-radius:4px; font-size:0.75rem;"></span>
        </div>
        <pre id="espConsole" style="background:#090d16; border:1px solid var(--card-border); border-radius:8px; padding:12px; font-family:monospace; font-size:0.8rem; color:#38bdf8; max-height:280px; overflow-y:auto; white-space:pre-wrap;">Ready to flash or monitor ESP32 nodes.</pre>
      </div>
    </div>
  </main>

  <!-- Add License Plate Modal -->
  <div class="modal-overlay" id="addPlateModal">
    <div class="modal-card">
      <div style="display:flex; justify-content:space-between; align-items:center;">
        <h3 style="font-size:1.1rem; font-weight:700;">Add License Plate</h3>
        <button style="background:transparent; border:none; color:var(--text-muted); font-size:1.2rem; cursor:pointer;" onclick="closeAddPlateModal()">&times;</button>
      </div>
      <form id="addPlateForm" onsubmit="submitAddPlate(event)">
        <div class="form-row">
          <div class="form-group">
            <label>State / Region</label>
            <input type="text" id="inputState" placeholder="e.g. TX or Texas" required value="Texas">
          </div>
          <div class="form-group">
            <label>License Plate #</label>
            <input type="text" id="inputPlate" placeholder="e.g. ABC-1234" required style="font-family:monospace; text-transform:uppercase;">
          </div>
        </div>
        <div class="form-row">
          <div class="form-group">
            <label>Make</label>
            <input type="text" id="inputMake" placeholder="e.g. Jeep">
          </div>
          <div class="form-group">
            <label>Model</label>
            <input type="text" id="inputModel" placeholder="e.g. Corolla">
          </div>
        </div>
        <div class="form-row">
          <div class="form-group">
            <label>Color</label>
            <input type="text" id="inputColor" placeholder="e.g. Grey">
          </div>
          <div class="form-group">
            <label>Vehicle Description</label>
            <input type="text" id="inputDesc" placeholder="e.g. Blue Toyota Corolla">
          </div>
        </div>
        <div class="form-group">
          <label>Notes</label>
          <input type="text" id="inputNotes" placeholder="Optional notes">
        </div>
        <div style="display:flex; align-items:center; gap:8px; margin-top:4px;">
          <input type="checkbox" id="inputActive" checked style="width:16px; height:16px;">
          <label for="inputActive" style="font-size:0.85rem; font-weight:600; cursor:pointer;">Set as Primary Active Vehicle</label>
        </div>
        <div style="display:flex; justify-content:flex-end; gap:10px; margin-top:12px;">
          <button type="button" class="btn" onclick="closeAddPlateModal()">Cancel</button>
          <button type="submit" class="btn btn-primary">Save Vehicle</button>
        </div>
      </form>
    </div>
  </div>

  <script>
    let map = null;
    let cameraLayer = null;
    let userMarker = null;
    let audioEnabled = true;
    let lastEncounterCount = -1;
    let audioCtx = null;

    function initAudio() {
      if (!audioCtx) {
        audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      }
    }

    function playRadarChime() {
      if (!audioEnabled) return;
      try {
        initAudio();
        if (audioCtx.state === 'suspended') audioCtx.resume();
        const now = audioCtx.currentTime;
        
        // High ping
        const osc1 = audioCtx.createOscillator();
        const gain1 = audioCtx.createGain();
        osc1.type = 'sine';
        osc1.frequency.setValueAtTime(880, now);
        osc1.frequency.exponentialRampToValueAtTime(1760, now + 0.15);
        gain1.gain.setValueAtTime(0.3, now);
        gain1.gain.exponentialRampToValueAtTime(0.001, now + 0.3);
        osc1.connect(gain1);
        gain1.connect(audioCtx.destination);
        osc1.start(now);
        osc1.stop(now + 0.3);

        // Confirmation tone
        const osc2 = audioCtx.createOscillator();
        const gain2 = audioCtx.createGain();
        osc2.type = 'triangle';
        osc2.frequency.setValueAtTime(1320, now + 0.18);
        gain2.gain.setValueAtTime(0.25, now + 0.18);
        gain2.gain.exponentialRampToValueAtTime(0.001, now + 0.45);
        osc2.connect(gain2);
        gain2.connect(audioCtx.destination);
        osc2.start(now + 0.18);
        osc2.stop(now + 0.45);
      } catch (e) {
        console.warn('Audio chime error:', e);
      }
    }

    function toggleAudio() {
      audioEnabled = !audioEnabled;
      const btn = document.getElementById('audioToggleBtn');
      if (audioEnabled) {
        btn.innerHTML = '<span id="audioIcon">🔊</span> Audio: ON';
        initAudio();
      } else {
        btn.innerHTML = '<span id="audioIcon">🔇</span> Audio: OFF';
      }
    }

    function switchTab(tabId, el) {
      document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
      document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
      document.getElementById(tabId).classList.add('active');
      if (el) el.classList.add('active');
      if (tabId === 'mapTab' && map) {
        setTimeout(() => map.invalidateSize(), 150);
      }
      if (tabId === 'telegramTab') fetchTelegram();
    }

    function showAlert(text) {
      const banner = document.getElementById('alertBanner');
      document.getElementById('alertBannerText').textContent = text;
      banner.style.display = 'flex';
      playRadarChime();
    }

    function dismissAlert() {
      document.getElementById('alertBanner').style.display = 'none';
    }

    function initMap() {
      map = L.map('map', { zoomControl: true, preferCanvas: true }).setView([32.7767, -96.7970], 11);
      // CARTO basemaps: attribution required; free for non-commercial use only (carto.com/basemaps)
      L.tileLayer('https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png', {
        attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors &copy; <a href="https://carto.com/attributions">CARTO</a>',
        subdomains: 'abcd',
        maxZoom: 20
      }).addTo(map);

      cameraLayer = L.layerGroup().addTo(map);
      // The camera data's licences ask for credit: DeFlock's OpenStreetMap-derived ALPR list ODbL, flocklocations.com community reports CC BY 4.0
      map.attributionControl.addAttribution('Cameras: <a href="https://deflock.me">DeFlock</a> / <a href="https://www.openstreetmap.org/copyright">OpenStreetMap contributors</a> (ODbL), <a href="https://flocklocations.com">flocklocations.com</a> community reports (CC BY 4.0)');
      map.on('moveend', () => fetchCameras(false));
    }

    // Keys as MapDb::alprSummary sends them: passedCameras, totalPasses, totalCameras, vettedCameras, plates[] (active flag)
    async function fetchSummary() {
      try {
        const res = await fetch('/api/v1/flock/summary');
        if (!res.ok) return;
        const data = await res.json();
        const passes = data.totalPasses || 0;

        document.getElementById('statPassedCams').textContent = (data.passedCameras || 0).toLocaleString();
        document.getElementById('statTotalEncounters').textContent = passes.toLocaleString();
        document.getElementById('statTotalCams').textContent = (data.totalCameras || 0).toLocaleString();
        document.getElementById('statVettedCams').textContent = (data.vettedCameras || 0).toLocaleString() + ' vetted cameras';

        const ap = (data.plates || []).find(p => p.active);
        document.getElementById('statActivePlate').textContent = ap ? ((ap.state ? ap.state.toUpperCase() + ' · ' : '') + (ap.displayPlate || ap.plate)) : 'None';
        document.getElementById('statActiveVehicle').textContent = ap ? (ap.vehicleDesc || [ap.color, ap.make, ap.model].filter(Boolean).join(' ')) : 'No active vehicle set';

        // A new pass since the last poll
        if (lastEncounterCount !== -1 && passes > lastEncounterCount) {
          const delta = passes - lastEncounterCount;
          showAlert('Camera pass recorded! ' + delta + (delta === 1 ? ' new pass' : ' new passes') + ' logged.');
          fetchEncounters();
          fetchAudits();
        }
        lastEncounterCount = passes;
      } catch (e) {
        console.error('fetchSummary failed:', e);
      }
    }

    async function fetchLocation() {
      try {
        const res = await fetch('/api/v1/location');
        if (!res.ok) return;
        const data = await res.json();
        if (data.valid && data.lat && data.lon) {
          const pos = [data.lat, data.lon];
          if (!userMarker) {
            const icon = L.divIcon({
              className: 'user-pin',
              iconSize: [16, 16],
              iconAnchor: [8, 8]
            });
            userMarker = L.marker(pos, { icon }).addTo(map);
            userMarker.bindPopup('');
            map.setView(pos, 13);
          } else {
            userMarker.setLatLng(pos);
          }
          // The place name is Nominatim / IP-geolocation text (OSM names are editable by anyone): escaped
          userMarker.setPopupContent('<strong>Your Current Position</strong><br>' + esc(data.place || ''));
        }
      } catch (e) {
        console.error('fetchLocation failed:', e);
      }
    }

    // Camera fields come from OSM tags and community datasets: escaped before they go into HTML
    function esc(v) {
      return String(v == null ? '' : v).replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
    }

    function camPopup(cam) {
      const isPassed = (cam.passCount && cam.passCount > 0);
      return `
            <div style="font-family:sans-serif; min-width:200px; color:#0f172a;">
              <h4 style="margin:0 0 4px 0; font-size:14px; color:${isPassed ? '#b91c1c' : '#0369a1'};">
                ${esc(cam.model || (cam.manufacturer ? cam.manufacturer + ' camera' : 'Camera'))}
              </h4>
              <p style="margin:0 0 6px 0; font-size:12px; font-weight:600;">${esc(cam.operator || 'Unknown Agency')}</p>
              ${isPassed ? `<div style="background:#fee2e2; color:#991b1b; padding:4px 8px; border-radius:4px; font-weight:bold; font-size:12px; margin-bottom:6px;">⚠️ Encountered ${esc(cam.passCount)} time(s)</div>` : ''}
              <div style="font-size:11px; color:#475569; line-height:1.4;">
                <strong>Camera ID:</strong> ${esc(cam.id || 'N/A')}<br>
                <strong>Coordinates:</strong> ${cam.lat.toFixed(5)}, ${cam.lon.toFixed(5)}<br>
                <strong>Source:</strong> ${esc(cam.source || 'DeFlock')}<br>
                <strong>Confidence:</strong> ${esc(cam.confidence)}%
              </div>
            </div>
          `;
    }

    // After a nationwide sync there are ~136k cameras: one DOM marker each froze the page. Only the cameras in
    // view (padded) are drawn, plain ones as canvas dots, and redrawn when the map stops moving.
    let allCameras = [];
    function drawCameras() {
      if (!map || !cameraLayer) return;
      cameraLayer.clearLayers();
      const view = map.getBounds().pad(0.25);
      let plain = 0;
      for (const cam of allCameras) {
        if (!view.contains([cam.lat, cam.lon])) continue;
        const isPassed = (cam.passCount && cam.passCount > 0);
        let m;
        if (isPassed) {
          m = L.marker([cam.lat, cam.lon], { icon: L.divIcon({ className: 'cam-pin passed', html: esc(cam.passCount) + 'x', iconSize: [26, 26], iconAnchor: [13, 13] }) });
        } else {
          if (++plain > 4000) continue;
          m = L.circleMarker([cam.lat, cam.lon], { radius: 5, color: '#38bdf8', weight: 1.5, fillColor: '#0369a1', fillOpacity: 0.85 });
        }
        m.bindPopup(() => camPopup(cam));
        cameraLayer.addLayer(m);
      }
    }

    // Fetched by area (the padded view): the server keeps ~136k after a nationwide sync, far too many to ship.
    // A move inside the last area only redraws; a truncated answer (dense area, zoomed out) refetches on every move.
    // Only the latest request may land: a new one aborts the one in flight, and a stale answer that still
    // resolves (seq moved on) is dropped, so an older area's cameras never replace the current view's.
    let camBox = null, camSeq = 0, camAbort = null;
    async function fetchCameras(force = true) {
      if (!map) return;
      if (!force && camBox && camBox.contains(map.getBounds())) {
        if (camAbort) { camAbort.abort(); camAbort = null; ++camSeq; }   // a fetch for an area we left must not land
        drawCameras(); return;
      }
      const seq = ++camSeq;
      if (camAbort) camAbort.abort();
      const ctl = camAbort = new AbortController();
      try {
        const b = map.getBounds().pad(0.5);
        const q = [b.getSouth(), b.getWest(), b.getNorth(), b.getEast()].map(v => v.toFixed(5)).join(',');
        const res = await fetch('/api/v1/flock?limit=8000&bbox=' + q, { signal: ctl.signal });
        if (!res.ok || seq !== camSeq) return;
        const data = await res.json();
        if (seq !== camSeq || !data.cameras) return;
        allCameras = data.cameras.filter(cam => cam.lat && cam.lon);
        camBox = data.truncated ? null : b;
        drawCameras();
      } catch (e) {
        if (e.name !== 'AbortError') console.error('fetchCameras failed:', e);
      } finally {
        if (camAbort === ctl) camAbort = null;
      }
    }

    async function fetchPlates() {
      try {
        const res = await fetch('/api/v1/plates');
        if (!res.ok) return;
        const data = await res.json();
        const grid = document.getElementById('platesGrid');
        grid.innerHTML = '';

        if (!data.plates || data.plates.length === 0) {
          grid.innerHTML = '<p style="color:var(--text-muted);">No license plates registered yet.</p>';
          return;
        }

        data.plates.forEach(p => {
          const card = document.createElement('div');
          card.className = 'plate-card' + (p.active ? ' active' : '');
          card.innerHTML = `
            <div class="plate-card-header">
              <div class="plate-emblem">
                <span class="plate-state">${esc(p.state || 'TEXAS')}</span>
                <span class="plate-number">${esc(p.displayPlate || p.plate)}</span>
              </div>
              ${p.active ? '<span class="badge badge-success">ACTIVE VEHICLE</span>' : '<button class="btn" data-act="active" style="padding:3px 8px; font-size:0.75rem;">Set Active</button>'}
            </div>
            <div class="vehicle-details">
              <span class="vehicle-title">${esc(p.vehicleDesc || [p.make, p.model].filter(Boolean).join(' '))}</span>
              <span><strong>Make/Model:</strong> ${esc(p.make || '-')} ${esc(p.model || '-')} (${esc(p.color || '-')})</span>
              <span><strong>Added:</strong> ${p.addedAt ? new Date(p.addedAt).toLocaleDateString() : 'Active'}</span>
              ${p.notes ? `<span><strong>Notes:</strong> ${esc(p.notes)}</span>` : ''}
            </div>
            <div class="plate-actions">
              <button class="btn btn-danger" data-act="delete" style="margin-left:auto; padding:4px 10px; font-size:0.75rem;">
                Delete
              </button>
            </div>
          `;
          // Handlers get the plate as a value, not spliced into inline JS
          const act = card.querySelector('[data-act="active"]');
          if (act) act.addEventListener('click', () => setActivePlate(p.plate));
          card.querySelector('[data-act="delete"]').addEventListener('click', () => deletePlate(p.plate));
          grid.appendChild(card);
        });
      } catch (e) {
        console.error('fetchPlates failed:', e);
      }
    }

    async function fetchEncounters() {
      try {
        const res = await fetch('/api/v1/flock/encounters');
        if (!res.ok) return;
        const data = await res.json();
        const tbody = document.getElementById('encountersTableBody');
        tbody.innerHTML = '';

        if (!data.encounters || data.encounters.length === 0) {
          tbody.innerHTML = '<tr><td colspan="6" style="text-align:center; color:var(--text-muted); padding:24px;">No pass encounters logged yet. Historical passes will be calculated automatically when recalculating.</td></tr>';
          return;
        }

        data.encounters.forEach(enc => {
          const tr = document.createElement('tr');
          const timeStr = enc.time ? new Date(enc.time).toLocaleString() : '-';
          const distStr = enc.distanceM ? enc.distanceM.toFixed(1) + ' m' : '-';
          tr.innerHTML = `
            <td>${timeStr}</td>
            <td><strong>${esc(enc.cameraId || 'ALPR')}</strong></td>
            <td><span class="badge badge-danger">Encounter #${esc(enc.encounterNum || 1)}</span></td>
            <td>${distStr}</td>
            <td><strong>${esc(enc.plate || '—')}</strong> <span style="color:var(--text-muted); font-size:0.78rem;">(${esc(enc.vehicleDesc || '')})</span></td>
            <td style="font-family:monospace; font-size:0.8rem;">${Number(enc.lat || 0).toFixed(5)}, ${Number(enc.lon || 0).toFixed(5)}</td>
          `;
          tbody.appendChild(tr);
        });
      } catch (e) {
        console.error('fetchEncounters failed:', e);
      }
    }

    async function fetchAudits() {
      try {
        const res = await fetch('/api/v1/plate-events?latest=1&limit=200');
        if (!res.ok) return;
        const data = await res.json();
        const tbody = document.getElementById('auditsTableBody');
        tbody.innerHTML = '';

        if (!data.events || data.events.length === 0) {
          tbody.innerHTML = '<tr><td colspan="6" style="text-align:center; color:var(--text-muted); padding:24px;">No plate events yet. Passes come from your route history; plate searches from released Flock audit logs.</td></tr>';
          return;
        }

        data.events.forEach(e => {
          const tr = document.createElement('tr');
          const timeStr = e.time ? new Date(e.time).toLocaleString() : '-';
          const search = e.kind === 'plate_search';
          const alpr = e.camera_type === 'alpr';
          const kind = search ? '<span class="badge badge-warning">Plate search</span>'
                     : alpr ? '<span class="badge badge-danger">ALPR pass</span>' : '<span class="badge">' + esc((e.camera_type || 'camera') + ' (no plate reading)') + '</span>';
          const who = search ? (e.agency || 'Agency') : [e.operator, e.model].filter(Boolean).join(' ');
          const url = /^https?:\/\//.test(e.source_url || '') ? e.source_url : '';
          const link = url ? ` <a href="${esc(url)}" target="_blank" rel="noopener noreferrer" title="${esc(e.source_name || 'source')}">source</a>` : '';
          tr.innerHTML = `
            <td>${timeStr}</td>
            <td>${kind}</td>
            <td><span class="badge badge-accent">${esc(e.plate || '')}</span></td>
            <td><strong>${esc(who)}</strong>${search ? '' : ` <span style="font-size:0.75rem; color:var(--text-muted);">(${esc(Math.round(e.distance_m || 0))} m)</span>`}</td>
            <td>${esc(e.confidence)}%</td>
            <td>${esc(e.details || '')}${link}</td>
          `;
          tbody.appendChild(tr);
        });
      } catch (e) {
        console.error('fetchAudits failed:', e);
      }
    }

    async function triggerCrossref() {
      try {
        const res = await fetch('/api/v1/flock/crossref', { method: 'POST' });
        const data = await res.json();
        alert('Audit-log check started (HaveIBeenFlocked, hashed plate prefixes only). Plate searches stored so far: ' + (data.matches || 0) + '.');
        fetchAudits();
        fetchSummary();
      } catch (e) {
        alert('Audit-log check failed: ' + e);
      }
    }

    async function triggerSyncUs() {
      const btn = document.getElementById('syncUsBtn');
      const txt = document.getElementById('syncUsBtnText');
      if (btn) btn.disabled = true;
      if (txt) txt.textContent = 'Starting Sync...';
      try {
        const res = await fetch('/api/v1/flock/sync-us', { method: 'POST' });
        const data = await res.json();
        pollSyncUs();
      } catch (e) {
        if (btn) btn.disabled = false;
        if (txt) txt.textContent = 'Sync Nationwide (US)';
        alert('Failed to start nationwide sync: ' + e);
      }
    }

    function pollSyncUs() {
      const timer = setInterval(async () => {
        try {
          const res = await fetch('/api/v1/flock/sync-us');
          const data = await res.json();
          const btn = document.getElementById('syncUsBtn');
          const txt = document.getElementById('syncUsBtnText');
          if (data.active) {
            if (txt) txt.textContent = `Syncing (${(data.sector || 0) + 1}/${data.totalSectors || 17})...`;
          } else {
            clearInterval(timer);
            if (btn) btn.disabled = false;
            if (txt) txt.textContent = 'Sync Nationwide (US)';
            alert(data.status || 'Nationwide US ALPR camera sync finished!');
            fetchSummary();
            fetchCameras();
            fetchEncounters();
            fetchAudits();
          }
        } catch (e) {
          clearInterval(timer);
          const btn = document.getElementById('syncUsBtn');
          const txt = document.getElementById('syncUsBtnText');
          if (btn) btn.disabled = false;
          if (txt) txt.textContent = 'Sync Nationwide (US)';
        }
      }, 2000);
    }

    async function triggerRecalculate() {
      try {
        const res = await fetch('/api/v1/flock/recalculate', { method: 'POST' });
        const data = await res.json();
        alert('Route history pass recalculation completed! Correlated ' + (data.encounters || 0) + ' passes.');
        fetchSummary();
        fetchCameras();
        fetchEncounters();
      } catch (e) {
        alert('Recalculation failed: ' + e);
      }
    }

    function openAddPlateModal() {
      document.getElementById('addPlateModal').style.display = 'flex';
    }

    function closeAddPlateModal() {
      document.getElementById('addPlateModal').style.display = 'none';
      document.getElementById('addPlateForm').reset();
    }

    async function submitAddPlate(e) {
      e.preventDefault();
      const state = document.getElementById('inputState').value.trim();
      const plate = document.getElementById('inputPlate').value.trim();
      const make = document.getElementById('inputMake').value.trim();
      const model = document.getElementById('inputModel').value.trim();
      const color = document.getElementById('inputColor').value.trim();
      const desc = document.getElementById('inputDesc').value.trim();
      const notes = document.getElementById('inputNotes').value.trim();
      const active = document.getElementById('inputActive').checked;

      const body = {
        plate: plate,
        displayPlate: plate,
        state: state,
        make: make,
        model: model,
        color: color,
        vehicleDesc: desc || (color + ' ' + make + ' ' + model),
        notes: notes,
        active: active
      };

      try {
        const res = await fetch('/api/v1/plates', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(body)
        });
        if (res.ok) {
          closeAddPlateModal();
          fetchPlates();
          fetchSummary();
        } else {
          alert('Failed to save license plate.');
        }
      } catch (err) {
        alert('Error: ' + err);
      }
    }

    async function setActivePlate(plate) {
      try {
        await fetch('/api/v1/plates', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ plate: plate, active: true })
        });
        fetchPlates();
        fetchSummary();
      } catch (e) {
        console.error('setActivePlate failed:', e);
      }
    }

    async function deletePlate(plate) {
      if (!confirm('Remove license plate ' + plate + '?')) return;
      try {
        await fetch('/api/v1/plates/' + encodeURIComponent(plate), { method: 'DELETE' });
        fetchPlates();
        fetchSummary();
      } catch (e) {
        console.error('deletePlate failed:', e);
      }
    }

    function renderTelegram(t) {
      document.getElementById('tgStatus').textContent = !t.configured ? 'Off (no bot token)'
        : (t.paired ? 'Paired with chat ' + t.chatId : 'Waiting for pairing')
          + (t.botUsername ? ' · @' + t.botUsername : '') + (t.polling ? '' : ' · not polling (token rejected?)');
      const pair = document.getElementById('tgPair');
      pair.style.display = t.pairCode ? 'block' : 'none';
      if (t.pairCode) {
        document.getElementById('tgPairCmd').textContent = '/start ' + t.pairCode;
        const a = document.getElementById('tgPairLink');
        a.textContent = t.botUsername ? '@' + t.botUsername : 'your bot';
        if (t.pairLink) a.href = t.pairLink; else a.removeAttribute('href');
      }
      document.getElementById('tgUnpairBtn').style.display = t.paired ? '' : 'none';
    }

    async function fetchTelegram() {
      try {
        const res = await fetch('/api/v1/telegram');
        if (!res.ok) { document.getElementById('tgStatus').textContent = 'Unavailable (HTTP ' + res.status + ')'; return; }
        renderTelegram(await res.json());
      } catch (e) {
        console.error('fetchTelegram failed:', e);
      }
    }

    async function postTelegram(body) {
      const res = await fetch('/api/v1/telegram', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
      if (!res.ok) { alert('Telegram settings were not saved (HTTP ' + res.status + ').'); return; }
      renderTelegram(await res.json());
      setTimeout(fetchTelegram, 1500);                 // the bot's @username arrives from Telegram a moment later
    }

    async function saveTelegramToken(e) {
      e.preventDefault();
      const input = document.getElementById('tgToken');
      const token = input.value.trim();
      if (!token && !confirm('Switch the Telegram bot off (remove its token)?')) return;
      try { await postTelegram({ token }); input.value = ''; } catch (err) { alert('Error: ' + err); }
    }

    async function unpairTelegram() {
      if (!confirm('Unpair the Telegram chat? It stops getting alerts until it pairs again with the new code.')) return;
      try { await postTelegram({ unpair: true }); } catch (err) { alert('Error: ' + err); }
    }

    async function loadEspPorts() {
      const el = document.getElementById('espPortsList');
      if (!el) return;
      try {
        const res = await fetch('/api/v1/esp/ports');
        if (!res.ok) throw new Error('HTTP ' + res.status);
        const data = await res.json();
        if (!data.ports || data.ports.length === 0) {
          el.innerHTML = '<span style="color:var(--warning);">No ESP32 serial devices detected.</span>';
        } else {
          el.innerHTML = data.ports.map(p => `<div>🔌 <b>${p.id}</b> → <span style="color:var(--accent);">${p.target || p.path}</span></div>`).join('');
        }
      } catch (e) {
        el.innerHTML = `<span style="color:var(--danger);">Error loading ports: ${e.message}</span>`;
      }
    }

    async function flashEspNode() {
      const btn = document.getElementById('btnFlashEsp');
      const con = document.getElementById('espConsole');
      const badge = document.getElementById('flashStatusBadge');
      if (btn) btn.disabled = true;
      if (badge) { badge.style.display = 'inline'; badge.textContent = 'Flashing...'; badge.style.color = 'var(--warning)'; }
      if (con) con.textContent = 'Starting firmware flash...\nUploading signed binary to connected ESP32...';

      try {
        const res = await fetch('/api/v1/esp/flash', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({})
        });
        const data = await res.json();
        if (data.ok) {
          if (badge) { badge.textContent = 'Flashed Successfully'; badge.style.color = 'var(--success)'; }
          if (con) con.textContent = '⚡ ESP32 flashed successfully!\n\n' + (data.output || 'Upload verified.');
        } else {
          if (badge) { badge.textContent = 'Flash Failed'; badge.style.color = 'var(--danger)'; }
          if (con) con.textContent = '❌ Flash error: ' + (data.error || 'Failed') + '\n\n' + (data.details || '');
        }
      } catch (e) {
        if (badge) { badge.textContent = 'Error'; badge.style.color = 'var(--danger)'; }
        if (con) con.textContent = 'Network error: ' + e.message;
      } finally {
        if (btn) btn.disabled = false;
        loadEspPorts();
      }
    }

    async function loadNodeBatteries() {
      const el = document.getElementById('nodeBatteriesList');
      if (!el) return;
      try {
        const res = await fetch('/api/v1/nodes');
        const data = await res.json();
        const nodes = data.nodes || [];
        if (nodes.length === 0) {
          el.innerHTML = '<div style="color:var(--text-muted); font-size:0.85rem;">No mesh nodes discovered yet.</div>';
          return;
        }
        // Names go through esc() and data- attributes: USB/bridge names aren't limited to letters and digits
        el.innerHTML = nodes.map((n, i) => {
          const mah = n.battMah || 240;
          const seen = n.ageS >= 0;
          const state = n.online ? '<span style="color:var(--success);">● online</span>'
                      : seen ? `<span style="color:#ff5555;">● offline · ${Math.round(n.ageS / 60)} min ago</span>`
                      : '<span style="color:var(--text-muted);">● never seen</span>';
          // A node without a battery still reports 0 % and a floating ~0.3 V
          let power;
          if (!seen) power = '—';
          else if (!n.hasBattery) power = 'No battery (USB 5V)';
          else {
            power = n.battPct >= 0 ? `<span style="color:#fff; font-weight:600;">${n.battPct}%</span>` : '';
            if (n.battMv > 0) power += ` (${n.battMv} mV)`;
            power += ' · ' + (n.estRuntimeMins > 0 ? `<span style="color:var(--success);">~${(n.estRuntimeMins/60).toFixed(1)}h remaining</span>` : (n.charging ? 'Charging' : ''));
          }
          return `
            <div style="background:#090d16; border:1px solid var(--card-border); border-radius:8px; padding:12px;">
              <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:6px;">
                <b style="color:var(--accent); font-family:monospace; font-size:0.85rem;">${esc(n.name)}</b>
                <span style="font-size:0.75rem; background:rgba(255,255,255,0.06); padding:2px 6px; border-radius:4px;">${n.role === 'base_station' ? 'base' : esc(n.role || 'node')}</span>
              </div>
              <div style="font-size:0.8rem; color:var(--text-muted); margin-bottom:4px;">${state}${n.link && seen ? ' · ' + esc(n.link) : ''}${n.fwVersion ? ' · v' + esc(n.fwVersion) : ''}</div>
              <div style="font-size:0.8rem; color:var(--text-muted); margin-bottom:8px;">Power: ${power}</div>
              <div style="display:flex; gap:6px; align-items:center;">
                <input type="number" id="battInput_${i}" value="${mah}" min="50" max="50000" step="10" style="width:90px; padding:4px 8px; background:var(--surface); border:1px solid var(--card-border); border-radius:4px; color:#fff; font-size:0.8rem;">
                <span style="font-size:0.75rem; color:var(--text-muted);">mAh</span>
                <button class="btn" style="padding:4px 8px; font-size:0.75rem;" data-name="${esc(n.name)}" onclick="saveNodeBattery(this.dataset.name, document.getElementById('battInput_${i}').value)">Save</button>
                <button class="btn" style="padding:4px 8px; font-size:0.75rem;" data-name="${esc(n.name)}" onclick="saveNodeBattery(this.dataset.name, 240)" title="Set Heltec V3 stock LiPo capacity">240mAh</button>
              </div>
            </div>
          `;
        }).join('');
      } catch (e) {
        el.innerHTML = '<div style="color:var(--danger); font-size:0.85rem;">Error loading nodes: ' + e.message + '</div>';
      }
    }

    async function saveNodeBattery(name, mah) {
      try {
        const res = await fetch('/api/v1/nodes/battery', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ name: name, mah: parseInt(mah, 10) })
        });
        const data = await res.json();
        if (data.ok) {
          loadNodeBatteries();
        } else {
          alert('Failed to set battery capacity: ' + (data.error || 'error'));
        }
      } catch (e) {
        alert('Network error: ' + e.message);
      }
    }

    async function loadNameplatesWeb() {
      const el = document.getElementById('nameplatesList');
      if (!el) return;
      try {
        const res = await fetch('/api/v1/nodes/nameplate');
        const data = await res.json();
        const plates = data.nameplates || [];
        if (plates.length === 0) {
          el.innerHTML = '<div style="color:var(--text-muted); font-size:0.8rem;">No nameplates generated yet. Click "Mint New Node" or "Generate" above!</div>';
          return;
        }
        el.innerHTML = plates.map(p => {
          const kb = (p.sizeBytes / 1024).toFixed(0);
          return `
            <div style="background:#090d16; border:1px solid var(--card-border); border-radius:6px; padding:10px; display:flex; justify-content:space-between; align-items:center;">
              <div>
                <b style="font-size:0.8rem; font-family:monospace; color:#fff;">${p.fileName}</b>
                <div style="font-size:0.72rem; color:var(--text-muted);">${kb} KB · 45° retention rail STL</div>
              </div>
              <a href="/api/v1/nodes/nameplate/download?file=${encodeURIComponent(p.fileName)}" class="btn" style="padding:4px 10px; font-size:0.75rem; text-decoration:none;" download>
                ⬇ STL
              </a>
            </div>
          `;
        }).join('');
      } catch (e) {
        el.innerHTML = '<div style="color:var(--danger); font-size:0.8rem;">Error loading nameplates: ' + e.message + '</div>';
      }
    }

    async function generateNameplateWeb() {
      const name = document.getElementById('nameplateInputName').value.trim();
      const units = parseInt(document.getElementById('nameplateInputUnits').value, 10);
      const shortMode = document.getElementById('nameplateInputMode').value === 'true';
      const resEl = document.getElementById('nameplateResult');

      if (resEl) { resEl.style.display = 'block'; resEl.textContent = 'Generating 3D nameplate STL with OpenSCAD...'; }
      try {
        const res = await fetch('/api/v1/nodes/nameplate', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ name: name, units: units, short: shortMode })
        });
        const data = await res.json();
        if (data.ok) {
          if (resEl) {
            resEl.innerHTML = `✅ Generated <b>${data.stlFileName}</b> (${data.width_mm}×${data.height_mm}×${data.thickness_mm} mm) · Plate text: <code>${data.text}</code> — <a href="/api/v1/nodes/nameplate/download?file=${encodeURIComponent(data.stlFileName)}" style="color:var(--accent); font-weight:600;" download>Download STL</a>`;
          }
          loadNameplatesWeb();
        } else {
          if (resEl) resEl.textContent = '❌ Generation failed: ' + (data.error || 'Unknown error');
        }
      } catch (e) {
        if (resEl) resEl.textContent = '❌ Error: ' + e.message;
      }
    }

    async function mintNewNodeWeb() {
      const resEl = document.getElementById('nameplateResult');
      if (resEl) { resEl.style.display = 'block'; resEl.textContent = 'Minting new node name from dictionaries and building 3D nameplate STL...'; }
      try {
        const res = await fetch('/api/v1/nodes/mint', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ role: 'mobile', units: 4 })
        });
        const data = await res.json();
        if (data.ok) {
          if (resEl) {
            resEl.innerHTML = `🎉 Minted node <b>${data.name}</b> (Callsign: <code>${data.callsign}</code>)! 3D Nameplate saved: <b>${data.stlFileName}</b> — <a href="/api/v1/nodes/nameplate/download?file=${encodeURIComponent(data.stlFileName)}" style="color:var(--accent); font-weight:600;" download>Download STL</a>`;
          }
          document.getElementById('nameplateInputName').value = data.name;
          loadNameplatesWeb();
          loadNodeBatteries();
        } else {
          if (resEl) resEl.textContent = '❌ Minting failed: ' + (data.error || 'Unknown error');
        }
      } catch (e) {
        if (resEl) resEl.textContent = '❌ Error: ' + e.message;
      }
    }

    // Startup
    window.addEventListener('DOMContentLoaded', () => {
      initMap();
      fetchSummary();
      fetchLocation();
      fetchCameras();
      fetchPlates();
      fetchEncounters();
      fetchAudits();
      loadNodeBatteries();
      loadNameplatesWeb();

      // Poll every 4 seconds for real-time updates
      setInterval(() => {
        fetchSummary();
        fetchLocation();
        if (document.getElementById('telegramTab').classList.contains('active')) fetchTelegram();
      }, 4000);
    });
  </script>
</body>
</html>
)html");
}

} // namespace WebDashboard
