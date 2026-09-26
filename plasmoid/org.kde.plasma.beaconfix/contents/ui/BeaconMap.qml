pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.plasma.components as PC3
import "security.js" as Sec
import org.kde.kirigami as Kirigami

// Slippy map for the widget: tiles (served by the BeaconFix tray — night-filtered OSM,
// streets, satellite, topo — or Esri directly when it isn't running), the trip track, accuracy ring, beacons, and OpenStreetMap places.
// Drag to pan, wheel to zoom, double-click to zoom in, ◎ to follow the fix again.
Item {
    id: map
    clip: true

    required property var src                  // the PlasmoidItem holding the data
    property int  layerIndex: 0                // 0 dark · 1 streets · 2 satellite · 3 topo
    property var  hiddenCats: []
    property real phase: 0
    property bool showSsids: true              // Wi-Fi names beside the beacons
    property bool showEvents: true             // event animations + ticker
    signal layerPicked(int index)
    signal categoryToggled(string key, bool visible)
    signal allCategories(bool visible)
    signal ssidsToggled(bool on)
    signal eventsToggled(bool on)

    // ── events: what changed since the last read, drawn as motion ────────────
    property var  anims: []                    // running animations [{type, bssid, t0, dur, ...}]
    property int  animCount: 0
    property real animNow: 0
    property var  posBy: ({})                  // beacon screen positions from the last base paint (for the fx layer)
    property var  mePos: null
    property var  selPos: null
    property var  _lastAp: ({})                // bssid → last drawn geometry, so a lost beacon can animate out
    property var  labelHits: []                // [{x, y, w, h, ids}] label pills from the last paint
    property string selectedBeacon: ""         // bssid pinned in the card
    property real tickNow: Date.now()
    property int  _toastKey: 0

    property real cx: 0.5                      // view centre, Web-Mercator [0,1]
    property real cy: 0.5
    property real zoom: 15
    property real zoomTarget: 15
    property point zoomAnchor: Qt.point(width / 2, height / 2)
    property bool follow: true
    // ── cinematic mode: slow eased fly-ins on events, periodic zoom-outs to city / state ──
    property bool cinematic: false
    property int  tourMinutes: 2
    property bool flying: false
    property bool holding: false
    property real lastUserInput: 0
    property string caption: ""
    property var  _fly: null
    signal cinematicToggled(bool on)
    // ── security overlay ──
    property bool secFocus: false               // dim everything that is not insecure
    property bool secPanel: false
    property var  knownDevices: src.knownDevices || []
    readonly property var secSummary: Sec.summary(src.aps || [], function(a) { return a.homeWlan || null })
    function secOf(ap) { return Sec.classify(ap, ap.homeWlan || null) }
    function clientsOn(ssid) {
        var out = [], kd = knownDevices || []
        for (var i = 0; i < kd.length; i++) if (kd[i] && kd[i].network && ssid && kd[i].network === ssid) out.push(kd[i])
        return out
    }
    property bool autoZoom: true
    readonly property string tileBase: src.tileBase || ""     // tray's localhost tile server
    readonly property int  maxZ: tileBase ? (layerIndex === 3 ? 17 : 19) : (layerIndex === 0 ? 16 : 19)
    readonly property real ws: 256 * Math.pow(2, zoom)

    property int  selected: -1                 // POI index with a pinned card
    property var  hover: null                  // {kind: "poi"|"cluster"|"beacon", ids: [...]}
    property var  markers: []                  // POI markers after clustering
    property var  beaconHits: []               // [{x, y, ids}] from the last overlay paint

    // ── projection ──────────────────────────────────────────────────────────
    function merc(lat, lon) {
        var r = Math.max(-85.0511, Math.min(85.0511, lat)) * Math.PI / 180
        return Qt.point((lon + 180) / 360, (1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2)
    }
    function latOf(my) { return Math.atan(Math.sinh(Math.PI * (1 - 2 * my))) * 180 / Math.PI }
    function sx(mx) { var dx = mx - cx; if (dx > 0.5) dx -= 1; else if (dx < -0.5) dx += 1; return width / 2 + dx * ws }
    function sy(my) { return height / 2 + (my - cy) * ws }
    function toMerc(px, py) { return Qt.point(cx + (px - width / 2) / ws, cy + (py - height / 2) / ws) }
    function mpp() { return 40075016.686 * Math.cos(latOf(cy) * Math.PI / 180) / ws }
    function distText(m) { return m < 950 ? Math.round(m / 10) * 10 + " m" : m < 9950 ? (m / 1000).toFixed(1) + " km" : Math.round(m / 1000) + " km" }
    function compass(deg) { return ["N", "NE", "E", "SE", "S", "SW", "W", "NW"][Math.floor(((deg + 22.5) % 360 + 360) % 360 / 45) % 8] }

    // ── view control ────────────────────────────────────────────────────────
    function recenter() {
        if (!src.valid) return
        var m = merc(src.lat, src.lon); cx = m.x; cy = m.y
        follow = true; autoZoom = true
        fit()
    }
    function fitZoom() {
        if (!src.valid || width < 50) return zoom
        // Fit the beacons, not the fix's error: an IP fix is tens of km wide and would
        // zoom the map out to a region. Never fit wider than ~2.5 km around us.
        var maxM = Math.max(80, src.source === "ip" ? 80 : Math.min(src.accuracy, 2500))
        var aps = src.aps || []
        for (var i = 0; i < aps.length; i++) {
            var a = aps[i]
            if (a.kind === "ring") maxM = Math.max(maxM, a.r)
            else if (a.kind !== "none" && a.status === "used") {
                var d = haversine(src.lat, src.lon, a.lat, a.lon) + a.r
                if (d < 3000) maxM = Math.max(maxM, d)
            }
        }
        var target = Math.min(width, height) * 0.42
        var z = Math.log(target * 40075016.686 * Math.cos(src.lat * Math.PI / 180) / (256 * maxM)) / Math.LN2
        return Math.max(13, Math.min(18, Math.floor(z * 2) / 2, maxZ))
    }
    function fit() {
        if (!src.valid || width < 50 || flying) return
        zoom = zoomTarget = fitZoom()
    }
    // cinematic camera
    function userTouched() {
        lastUserInput = Date.now()
        if (flying) { _fly = null; flyTimer.stop(); flying = false }
        holdTimer.stop(); holding = false; caption = ""
    }
    function easeInOut(t) { return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2 }
    function flyTo(lat, lon, z, dur, then) {
        var m = merc(lat, lon)
        _fly = {t0: Date.now(), dur: Math.max(200, dur), z0: zoom, z1: Math.max(3, Math.min(z, maxZ)), x0: cx, y0: cy, x1: m.x, y1: m.y, then: then || null}
        flying = true; follow = false; autoZoom = false
        flyTimer.start()
    }
    function hold(ms, then) { holding = true; holdTimer.then = then || null; holdTimer.interval = ms; holdTimer.restart() }
    function homeView(dur, then) {
        if (!src.valid) { if (then) then(); return }
        flyTo(src.lat, src.lon, fitZoom(), dur, function() { follow = true; autoZoom = true; caption = ""; if (then) then() })
    }
    function placeParts() {
        var p = (src.place || "").split(",").map(function(x) { return x.trim() }).filter(function(x) { return x })
        return {city: p.length ? p[0] : "", region: p.length > 1 ? p[p.length - 1] : ""}
    }
    function overview(kind) {                 // 0: city · 1: city then state
        if (!src.valid || flying || holding) return
        var pp = placeParts()
        caption = pp.city || src.place || ""
        flyTo(src.lat, src.lon, 10.5, 6500, function() {
            hold(5000, function() {
                if (kind === 1 && pp.region) {
                    caption = pp.region
                    flyTo(src.lat, src.lon, 7.5, 5000, function() { hold(5000, function() { homeView(7000) }) })
                } else homeView(6500)
            })
        })
    }
    function spotlight(lat, lon) {            // an event: glide in, linger, glide back
        if (!cinematic || !visible || flying || holding || !src.valid) return
        if (Date.now() - lastUserInput < 45000) return
        flyTo(lat, lon, Math.min(maxZ, 17), 2800, function() { hold(3500, function() { homeView(2800) }) })
    }
    function focusOn(lat, lon, z) {
        var m = merc(lat, lon); cx = m.x; cy = m.y
        follow = false; autoZoom = false
        zoom = zoomTarget = Math.max(3, Math.min(20, z))
    }
    function applyZoom(z, ax, ay) {
        var m = toMerc(ax, ay)
        zoom = z
        cx = m.x - (ax - width / 2) / ws
        cy = Math.max(0, Math.min(1, m.y - (ay - height / 2) / ws))
        cx = cx - Math.floor(cx)
    }
    function zoomAt(delta, ax, ay) {
        userTouched()
        autoZoom = false
        if (Math.abs(ax - width / 2) > 1 || Math.abs(ay - height / 2) > 1) follow = false
        zoomTarget = Math.max(3, Math.min(20, zoomTarget + delta))
        zoomAnchor = Qt.point(ax, ay)
    }
    function haversine(la1, lo1, la2, lo2) {
        var R = 6371000, d2r = Math.PI / 180
        var dLa = (la2 - la1) * d2r, dLo = (lo2 - lo1) * d2r
        var a = Math.sin(dLa / 2) * Math.sin(dLa / 2) + Math.cos(la1 * d2r) * Math.cos(la2 * d2r) * Math.sin(dLo / 2) * Math.sin(dLo / 2)
        return 2 * R * Math.asin(Math.sqrt(a))
    }
    function bandOf(ap) { return ap.band ? ap.band : ap.freq >= 5925 ? "6" : ap.freq >= 4900 ? "5" : ap.freq > 0 ? "2.4" : "" }
    function project(g) {                       // stored geometry → screen point (rings re-anchor on the fix they were heard from)
        var m = merc(g.lat, g.lon)
        if (g.kind === "ring") { var ang = (g.bearing - 90) * Math.PI / 180, rr = g.r / mpp(); return Qt.point(sx(m.x) + Math.cos(ang) * rr, sy(m.y) + Math.sin(ang) * rr) }
        return Qt.point(sx(m.x), sy(m.y))
    }
    function progress(a) { return Math.max(0, Math.min(1, (animNow - a.t0) / a.dur)) }
    function easeOut(p) { return 1 - Math.pow(1 - p, 3) }
    function bounce(p) {
        var n1 = 7.5625, d1 = 2.75
        if (p < 1 / d1) return n1 * p * p
        if (p < 2 / d1) { p -= 1.5 / d1; return n1 * p * p + 0.75 }
        if (p < 2.5 / d1) { p -= 2.25 / d1; return n1 * p * p + 0.9375 }
        p -= 2.625 / d1; return n1 * p * p + 0.984375
    }
    function animFor(type, bssid) { for (var i = 0; i < anims.length; i++) if (anims[i].type === type && anims[i].bssid === bssid) return anims[i]; return null }
    function evGlyph(t) { return ({ap_new: "📡", ap_lost: "💨", ap_up: "▲", ap_down: "▼", ap_placed: "💎", fix: "◎", stop: "🚩", achievement: "🏆", region: "🗺", prefetch: "💾", error: "⚠"})[t] || "•" }
    function evColor(t) { return ({ap_new: "#35d6ff", ap_lost: "#8a93a6", ap_up: "#6cff8a", ap_down: "#ff9f43", ap_placed: "#ffd166", fix: "#35d6ff", stop: "#ff4f4f", achievement: "#ffd166", region: "#c9a0ff", prefetch: "#9fb0c8", error: "#ff4f4f"})[t] || "#e6edf7" }
    function evText(e) {
        if (e.text) return e.text
        var n = e.ssid || (e.bssid ? e.bssid : "")
        switch (e.type) {
        case "ap_new": return "Heard " + (n || "a beacon")
        case "ap_lost": return "Lost " + (n || "a beacon")
        case "ap_up": return (n || "beacon") + " +" + (e.delta || 0) + " dB"
        case "ap_down": return (n || "beacon") + " " + (e.delta || 0) + " dB"
        case "ap_placed": return "Placed " + (n || "a beacon") + " on the map"
        case "fix": return "New fix"
        case "stop": return "New stop"
        default: return e.type || "event"
        }
    }
    function ageText(s) { return s < 5 ? "now" : s < 60 ? Math.floor(s) + " s" : Math.floor(s / 60) + " min" }
    function onEvents(list, animate) {
        var t = Date.now(), pushed = 0, spot = null
        for (var i = 0; i < list.length; i++) {
            var e = list[i] || {}, type = e.type || ""
            if (animate && !spot && e.lat !== undefined && e.lon !== undefined && (type === "ap_placed" || type === "stop" || type === "fix" || type === "ap_new")) spot = e
            var when = e.time ? (Date.parse(e.time) || t) : t
            tickerModel.insert(0, {glyph: evGlyph(type), line: evText(e), t: when, col: evColor(type)})
            while (tickerModel.count > 5) tickerModel.remove(tickerModel.count - 1)
            if (!animate || !showEvents) continue
            var a = {type: type, bssid: e.bssid || "", ssid: e.ssid || "", t0: t + pushed * 120, dur: 2500, lat: e.lat, lon: e.lon, delta: e.delta || 0}
            switch (type) {
            case "ap_new": a.dur = 2500; break
            case "ap_lost":
                a.dur = 2000
                a.ghost = _lastAp[a.bssid] || ((e.lat !== undefined && e.lon !== undefined) ? {kind: "pt", lat: e.lat, lon: e.lon, ssid: e.ssid, col: "#8a93a6"} : null)
                if (!a.ghost) continue
                break
            case "ap_up": case "ap_down": a.dur = 3000; break
            case "ap_placed": a.dur = 1500; var g = _lastAp[a.bssid]; a.fromPos = (g && g.kind === "ring") ? g : null; break
            case "fix": a.dur = 1800; a.from = (e.fromLat !== undefined && e.fromLon !== undefined) ? {lat: e.fromLat, lon: e.fromLon} : null; break
            case "stop": a.dur = 4200; break
            case "achievement": case "region": case "prefetch": case "error":
                toastModel.append({glyph: evGlyph(type), line: evText(e), col: evColor(type), key: ++_toastKey})
                while (toastModel.count > 3) toastModel.remove(0)
                continue
            default: continue
            }
            anims.push(a); pushed++
        }
        animCount = anims.length
        animNow = t
        fx.requestPaint()
        if (spot) spotlight(spot.lat, spot.lon)
    }
    Timer {                                     // one clock for every animation (and the selected-beacon pulse)
        id: animClock
        interval: 33; repeat: true                  // 30 fps for ripples and chevrons
        running: (map.animCount > 0 || map.selectedBeacon !== "") && map.visible
        onTriggered: {
            map.animNow = Date.now()
            if (map.anims.length) {
                var keep = []
                for (var i = 0; i < map.anims.length; i++) if (map.progress(map.anims[i]) < 1) keep.push(map.anims[i])
                if (keep.length !== map.anims.length) { map.anims = keep; map.animCount = keep.length }
            }
            fx.requestPaint()
        }
    }
    Timer {                                     // ticker ageing; hovering it pauses the fade
        interval: 1000; repeat: true; running: tickerModel.count > 0
        onTriggered: {
            map.tickNow = Date.now()
            if (tickerHover.hovered) return
            for (var i = tickerModel.count - 1; i >= 0; i--) if (map.tickNow - tickerModel.get(i).t > 60000) tickerModel.remove(i)
        }
    }
    ListModel { id: tickerModel }
    ListModel { id: toastModel }

    Timer {                                     // cinematic flight: 60 fps target, only while flying;
        id: flyTimer                            // the overlay paints in "lite" mode (no labels) meanwhile
        interval: 16; repeat: true; running: false
        onTriggered: {
            var f = map._fly
            if (!f) { stop(); map.flying = false; return }
            var p = Math.min(1, (Date.now() - f.t0) / f.dur), e = map.easeInOut(p)
            var dx = f.x1 - f.x0; if (dx > 0.5) dx -= 1; else if (dx < -0.5) dx += 1
            var nx = f.x0 + dx * e; map.cx = nx - Math.floor(nx)
            map.cy = f.y0 + (f.y1 - f.y0) * e
            map.zoom = map.zoomTarget = f.z0 + (f.z1 - f.z0) * e
            if (p >= 1) { var then = f.then; map._fly = null; stop(); map.flying = false; overlay.requestPaint(); if (then) then() }
        }
    }
    Timer { id: holdTimer; property var then: null; onTriggered: { map.holding = false; var t = then; then = null; if (t) t() } }
    Timer {                                     // tour scheduler: an overview every tourMinutes, alternating city / city+state
        interval: 5000; repeat: true; running: map.cinematic && map.visible
        property real nextAt: 0
        property int  variant: 0
        onRunningChanged: nextAt = 0
        onTriggered: {
            var now = Date.now()
            if (nextAt === 0) { nextAt = now + map.tourMinutes * 60000; return }
            if (map.flying || map.holding || !map.src.valid) return
            if (now - map.lastUserInput < 45000) return
            if (now >= nextAt) { nextAt = now + map.tourMinutes * 60000; map.overview(variant++ % 2) }
        }
    }
    Timer {                                     // eased, cursor-anchored zoom
        interval: 16; repeat: true
        running: !map.flying && Math.abs(map.zoomTarget - map.zoom) > 0.002
        onTriggered: {
            var d = map.zoomTarget - map.zoom
            map.applyZoom(Math.abs(d) < 0.01 ? map.zoomTarget : map.zoom + d * 0.3, map.zoomAnchor.x, map.zoomAnchor.y)
        }
    }

    // ── tiles ───────────────────────────────────────────────────────────────
    function tileUrl(z, x, y, labels) {
        // OSM blocks QML's generic User-Agent, so OSM-based layers come through the tray
        if (tileBase) return `${tileBase}/t/${labels ? "L" : layerIndex}/${z}/${x}/${y}.png`
        if (labels) return `https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/${z}/${y}/${x}`
        if (layerIndex === 2) return `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/${z}/${y}/${x}`
        if (layerIndex === 3) return `https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/${z}/${y}/${x}`
        if (layerIndex === 1) return `https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/${z}/${y}/${x}`
        return `https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Dark_Gray_Base/MapServer/tile/${z}/${y}/${x}`
    }

    component TileLayer: Item {
        id: tl
        property int  zOffset: 0
        property bool labels: false
        property string rangeKey: ""
        anchors.fill: parent
        function refresh() {
            if (map.width <= 0 || map.height <= 0) return
            var z = Math.max(1, Math.min(map.maxZ, Math.round(map.zoom)) - zOffset)
            var n = 1 << z
            var a = map.toMerc(0, 0), b = map.toMerc(map.width, map.height)
            var x0 = Math.floor(a.x * n), x1 = Math.floor(b.x * n)
            var y0 = Math.max(0, Math.floor(a.y * n)), y1 = Math.min(n - 1, Math.floor(b.y * n))
            var key = [map.layerIndex, map.tileBase, z, x0, x1, y0, y1].join(",")
            if (key === rangeKey) return
            rangeKey = key
            var need = {}
            for (var x = x0; x <= x1; x++)
                for (var y = y0; y <= y1; y++) need[z + "/" + x + "/" + y] = [x, y]
            var lk = map.layerIndex + "," + map.tileBase
            for (var i = tiles.count - 1; i >= 0; i--) {
                var t = tiles.get(i)
                if (t.lk === lk && need[t.k] !== undefined) delete need[t.k]
                else tiles.remove(i)
            }
            for (var k in need) {
                var tx = need[k][0], ty = need[k][1]
                tiles.append({k: k, lk: lk, tz: z, tx: tx, ty: ty, url: map.tileUrl(z, ((tx % n) + n) % n, ty, labels)})
            }
        }
        ListModel { id: tiles }
        Repeater {
            model: tiles
            delegate: Image {
                required property int tz
                required property int tx
                required property int ty
                required property string url
                readonly property real n: Math.pow(2, tz)
                readonly property real ox: map.width / 2 + (tx / n - map.cx) * map.ws
                readonly property real oy: map.height / 2 + (ty / n - map.cy) * map.ws
                x: Math.floor(ox); y: Math.floor(oy)
                width: Math.ceil(ox + map.ws / n) - x; height: Math.ceil(oy + map.ws / n) - y
                source: url
                asynchronous: true; cache: true; smooth: true
                fillMode: Image.Stretch
            }
        }
    }
    function refreshTiles() { bgTiles.refresh(); fgTiles.refresh(); labelTiles.refresh() }
    onCxChanged: { refreshTiles(); overlay.requestPaint(); fx.requestPaint() }
    onCyChanged: { refreshTiles(); overlay.requestPaint(); fx.requestPaint() }
    onZoomChanged: { refreshTiles(); clusterTimer.restart(); overlay.requestPaint(); fx.requestPaint() }
    onWidthChanged: { if (autoZoom) fit(); refreshTiles() }
    onHeightChanged: { if (autoZoom) fit(); refreshTiles() }
    onLayerIndexChanged: { if (zoomTarget > maxZ + 2) zoom = zoomTarget = maxZ + 2; refreshTiles() }
    onTileBaseChanged: refreshTiles()
    onHiddenCatsChanged: cluster()

    Rectangle { anchors.fill: parent; color: "#0b101a" }
    Item {
        id: base
        anchors.fill: parent
        TileLayer { id: bgTiles; zOffset: 2 }      // coarse layer underneath: no black holes while loading
        TileLayer { id: fgTiles }
    }
    Rectangle { anchors.fill: parent; color: "#000000"; opacity: map.layerIndex === 2 ? 0.22 : 0 }
    TileLayer { id: labelTiles; labels: true; visible: map.layerIndex === 2 }

    // ── overlay: track, accuracy ring, beacons ──────────────────────────────
    Canvas {
        id: overlay
        anchors.fill: parent
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            var src = map.src
            if (!src.valid) return
            var mpp = map.mpp()
            var me = map.merc(src.lat, src.lon), mx = map.sx(me.x), my = map.sy(me.y)
            // trip track
            var tr = src.track || []
            for (var i = 1; i < tr.length; i++) {
                var a = map.merc(tr[i - 1].lat, tr[i - 1].lon), b = map.merc(tr[i].lat, tr[i].lon)
                var coarse = tr[i - 1].source === "ip" || tr[i].source === "ip"
                ctx.strokeStyle = coarse ? "rgba(159,176,200,0.45)" : "rgba(53,214,255,0.6)"
                ctx.lineWidth = coarse ? 1.5 : 2.5
                ctx.setLineDash(coarse ? [5, 5] : [])
                ctx.beginPath(); ctx.moveTo(map.sx(a.x), map.sy(a.y)); ctx.lineTo(map.sx(b.x), map.sy(b.y)); ctx.stroke()
            }
            ctx.setLineDash([])
            for (var j = 0; j + 1 < tr.length; j++) {
                var s = map.merc(tr[j].lat, tr[j].lon)
                ctx.beginPath(); ctx.arc(map.sx(s.x), map.sy(s.y), 4, 0, Math.PI * 2)
                ctx.strokeStyle = tr[j].source === "ip" ? "#9fb0c8" : "#ffffff"; ctx.lineWidth = 1.4; ctx.stroke()
                if (tr[j].source !== "ip") { ctx.fillStyle = "#1e8fae"; ctx.fill() }
            }
            // accuracy ring
            if (src.accuracy > 0) {
                var r = Math.max(8, Math.min(src.accuracy / mpp, 20000))
                ctx.beginPath(); ctx.arc(mx, my, r, 0, Math.PI * 2)
                ctx.fillStyle = src.source === "ip" ? "rgba(53,214,255,0.05)" : "rgba(53,214,255,0.09)"; ctx.fill()
                ctx.setLineDash(src.source === "ip" ? [6, 5] : [])
                ctx.strokeStyle = "rgba(53,214,255,0.5)"; ctx.lineWidth = 1.5; ctx.stroke()
                ctx.setLineDash([])
            }
            // beacons: RSSI orbits, uncertainty rings, dots (grouped when they coincide)
            var aps = src.aps || [], pts = [], atMe = [], posBy = {}, now = map.animNow, last = map._lastAp
            var selIdx = -1
            map.selPos = null
            for (var k = 0; k < aps.length; k++) {
                var ap = aps[k]
                if (ap.kind === "none") continue
                if (map.selectedBeacon && ap.bssid === map.selectedBeacon) selIdx = k
                var col = ap.status === "used" ? (ap.kind === "ring" ? "#35d6ff" : "#ffd166")
                        : ap.status === "active" ? "#6cff8a" : ap.status === "travelling" ? "#ff4fd8" : "#8a93a6"
                var px, py
                if (ap.kind === "ring") {
                    var rr = ap.r / mpp
                    if (rr < 14) {
                        atMe.push(k)
                        last[ap.bssid] = {kind: "pt", lat: src.lat, lon: src.lon, ssid: ap.ssid, col: col, status: ap.status, dbm: ap.dbm}
                        continue
                    }
                    ctx.beginPath(); ctx.arc(mx, my, rr, 0, Math.PI * 2)
                    ctx.strokeStyle = "rgba(53,214,255,0.10)"; ctx.lineWidth = 1; ctx.setLineDash([2, 3]); ctx.stroke(); ctx.setLineDash([])
                    var ang = (ap.bearing - 90) * Math.PI / 180
                    px = mx + Math.cos(ang) * rr; py = my + Math.sin(ang) * rr
                    last[ap.bssid] = {kind: "ring", lat: src.lat, lon: src.lon, r: ap.r, bearing: ap.bearing, ssid: ap.ssid, col: col, status: ap.status, dbm: ap.dbm}
                } else {
                    var m2 = map.merc(ap.lat, ap.lon); px = map.sx(m2.x); py = map.sy(m2.y)
                    var ur = ap.r / mpp
                    if (ur > 6) { ctx.beginPath(); ctx.arc(px, py, ur, 0, Math.PI * 2); ctx.strokeStyle = "rgba(255,209,102,0.3)"; ctx.setLineDash([4, 4]); ctx.stroke(); ctx.setLineDash([]) }
                    last[ap.bssid] = {kind: ap.kind, lat: ap.lat, lon: ap.lon, r: ap.r, ssid: ap.ssid, col: col, status: ap.status, dbm: ap.dbm}
                }
                // just placed on the map: the marker eases in from the orbit spot it used to sit on
                var placing = map.animFor("ap_placed", ap.bssid)
                if (placing && placing.fromPos) {
                    var op = map.project(placing.fromPos), pp = map.easeOut(map.progress(placing))
                    px = op.x + (px - op.x) * pp; py = op.y + (py - op.y) * pp
                }
                posBy[ap.bssid] = {x: px, y: py, col: col}
                var sc = map.secOf(ap)
                var merged = false
                for (var q = 0; q < pts.length; q++)
                    if (Math.abs(pts[q].x - px) < 9 && Math.abs(pts[q].y - py) < 9) {
                        pts[q].ids.push(k)
                        if (sc.rank > pts[q].secRank) { pts[q].secRank = sc.rank; pts[q].secCol = sc.color; pts[q].secGlyph = sc.glyph }
                        if (ap.dbm > pts[q].dbm) { pts[q].ssid = ap.ssid; pts[q].dbm = ap.dbm; pts[q].status = ap.status; pts[q].bssid = ap.bssid; pts[q].band = map.bandOf(ap); pts[q].col = col }
                        merged = true; break
                    }
                if (!merged) pts.push({x: px, y: py, ids: [k], col: col, wigle: ap.kind === "wigle", big: ap.status === "used" || ap.status === "active",
                                       ssid: ap.ssid || "", dbm: ap.dbm, status: ap.status, bssid: ap.bssid, band: map.bandOf(ap),
                                       secRank: sc.rank, secCol: sc.color, secGlyph: sc.glyph})
            }
            for (var p = 0; p < pts.length; p++) {
                var pt = pts[p], rad = pt.big ? 4.5 : 3
                var insecure = pt.secRank >= 3
                ctx.globalAlpha = map.secFocus && !insecure ? 0.05 : 0.25; ctx.fillStyle = pt.col
                ctx.beginPath(); ctx.arc(pt.x, pt.y, rad * 2.6, 0, Math.PI * 2); ctx.fill()
                ctx.globalAlpha = 1
                if (selIdx >= 0 && pt.ids.indexOf(selIdx) >= 0) map.selPos = {x: pt.x, y: pt.y}   // halo is drawn by the fx layer
                ctx.beginPath()
                if (pt.wigle) { ctx.moveTo(pt.x, pt.y - 7); ctx.lineTo(pt.x + 7, pt.y); ctx.lineTo(pt.x, pt.y + 7); ctx.lineTo(pt.x - 7, pt.y); ctx.closePath() }
                else ctx.arc(pt.x, pt.y, rad, 0, Math.PI * 2)
                ctx.fillStyle = pt.col
                if (map.secFocus && !insecure) ctx.globalAlpha = 0.25
                ctx.fill(); ctx.strokeStyle = "rgba(255,255,255,0.85)"; ctx.lineWidth = 1; ctx.stroke()
                if (insecure) {                     // warning ring + glyph in the grade colour
                    ctx.beginPath(); ctx.arc(pt.x, pt.y, rad + 5, 0, Math.PI * 2)
                    ctx.strokeStyle = pt.secCol; ctx.lineWidth = 1.6; ctx.setLineDash(pt.secRank >= 4 ? [] : [3, 2]); ctx.stroke(); ctx.setLineDash([])
                    ctx.fillStyle = pt.secCol; ctx.font = "bold 10px sans-serif"; ctx.textAlign = "center"; ctx.fillText(pt.secGlyph, pt.x - 9, pt.y - 7)
                }
                ctx.globalAlpha = 1
                if (pt.ids.length > 1) {
                    ctx.fillStyle = pt.col; ctx.beginPath(); ctx.arc(pt.x + 8, pt.y - 8, 7, 0, Math.PI * 2); ctx.fill()
                    ctx.fillStyle = "#0b101a"; ctx.font = "bold 9px sans-serif"; ctx.textAlign = "center"; ctx.fillText(pt.ids.length, pt.x + 8, pt.y - 5)
                }
            }

            // ── Wi-Fi names: pills beside the beacons, strongest first, no overlaps ──
            // (skipped while a cinematic flight is running: text layout is the expensive part)
            var hits = []
            if (!map.flying && map.showSsids && map.zoom >= 13) {
                var lbl = pts.slice().sort(function(a, b) { return b.dbm - a.dbm })
                var limit = map.zoom >= 15 ? lbl.length : 12, taken = [], made = 0, h = 16
                ctx.textBaseline = "middle"; ctx.textAlign = "left"
                for (var li = 0; li < lbl.length && made < limit; li++) {
                    var L = lbl[li]
                    var name = L.ssid ? (L.ssid.length > 18 ? L.ssid.slice(0, 17) + "…" : L.ssid) : "(hidden)"
                    if (L.ids.length > 1) name += " +" + (L.ids.length - 1)
                    var nameFont = L.ssid ? "10px sans-serif" : "italic 10px sans-serif"
                    ctx.font = nameFont
                    var tw = ctx.measureText(name).width
                    var badge = map.zoom >= 16 && L.band ? L.band : ""
                    ctx.font = "bold 8px sans-serif"
                    var bw = badge ? ctx.measureText(badge).width + 6 : 0
                    var w = tw + 12 + (badge ? bw + 3 : 0)
                    var intro = map.animFor("ap_new", L.bssid), ip = intro ? map.easeOut(map.progress(intro)) : 1
                    var cands = [[L.x + 9 + (1 - ip) * 14, L.y - 8], [L.x - w / 2, L.y - 27], [L.x - w / 2, L.y + 10]]
                    var at = null
                    for (var ci = 0; ci < cands.length && !at; ci++) {
                        var rx = cands[ci][0], ry = cands[ci][1], ok = true
                        for (var ti = 0; ti < taken.length && ok; ti++) { var tk = taken[ti]; if (rx < tk.x + tk.w && rx + w > tk.x && ry < tk.y + h && ry + h > tk.y) ok = false }
                        for (var pi = 0; pi < pts.length && ok; pi++) { var o = pts[pi]; if (o !== L && o.x > rx - 6 && o.x < rx + w + 6 && o.y > ry - 6 && o.y < ry + h + 6) ok = false }
                        if (ok) at = {x: rx, y: ry}
                    }
                    if (!at) continue
                    taken.push({x: at.x, y: at.y, w: w, h: h}); made++
                    var tcol = L.status === "used" ? L.col : L.status === "active" ? "#6cff8a" : L.status === "travelling" ? "#ff4fd8" : "#8a93a6"
                    ctx.globalAlpha = ip
                    if (map.secFocus && L.secRank < 3) ctx.globalAlpha = ip * 0.25
                    ctx.beginPath(); ctx.roundedRect(at.x, at.y, w, h, 4, 4); ctx.fillStyle = "rgba(8,13,20,0.8)"; ctx.fill()
                    if (L.secRank >= 3) { ctx.strokeStyle = L.secCol; ctx.lineWidth = 1; ctx.stroke() }
                    ctx.fillStyle = tcol; ctx.fillRect(at.x, at.y + 3, 2.5, h - 6)
                    ctx.font = nameFont; ctx.fillStyle = tcol
                    ctx.fillText(name, at.x + 7, at.y + h / 2)
                    if (badge) {
                        var bx = at.x + 7 + tw + 3
                        ctx.beginPath(); ctx.roundedRect(bx, at.y + 3, bw, h - 6, 3, 3); ctx.fillStyle = "rgba(230,237,247,0.18)"; ctx.fill()
                        ctx.font = "bold 8px sans-serif"; ctx.fillStyle = "#e6edf7"; ctx.fillText(badge, bx + 3, at.y + h / 2)
                    }
                    ctx.globalAlpha = 1
                    hits.push({x: at.x, y: at.y, w: w, h: h, ids: L.ids})
                }
                ctx.textBaseline = "alphabetic"
            }
            map.labelHits = hits

            map.posBy = posBy
            map.mePos = {x: mx, y: my}
            if (map.animCount > 0 || map.selectedBeacon) fx.requestPaint()

            if (atMe.length) pts.push({x: mx, y: my, ids: atMe})
            map.beaconHits = pts
            meBadge.count = atMe.length
        }
    }

    // ── fx: animations + pinned halo only. Repainted by the clock; the base overlay is not. ──
    Canvas {
        id: fx
        anchors.fill: parent
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            var src = map.src
            if (!src.valid) return
            var posBy = map.posBy || {}, now = map.animNow
            var me = map.merc(src.lat, src.lon)
            if (map.selPos && map.selectedBeacon) {
                ctx.beginPath(); ctx.arc(map.selPos.x, map.selPos.y, 10 + 3 * Math.sin(now / 250), 0, Math.PI * 2)
                ctx.strokeStyle = "#ffffff"; ctx.lineWidth = 2; ctx.stroke()
            }
            // ── events as motion ──
            for (var ai = 0; ai < map.anims.length; ai++) {
                var an = map.anims[ai], pr = map.progress(an)
                if (pr <= 0) continue
                var pos = posBy[an.bssid]
                if (an.type === "ap_new") {
                    if (!pos) continue
                    ctx.strokeStyle = pos.col; ctx.lineWidth = 2
                    ctx.globalAlpha = 1 - pr
                    ctx.beginPath(); ctx.arc(pos.x, pos.y, 6 + 34 * map.easeOut(pr), 0, Math.PI * 2); ctx.stroke()
                    if (pr > 0.3) { ctx.globalAlpha = (1 - pr) * 0.6; ctx.beginPath(); ctx.arc(pos.x, pos.y, 6 + 26 * map.easeOut((pr - 0.3) / 0.7), 0, Math.PI * 2); ctx.stroke() }
                    ctx.globalAlpha = 1
                } else if (an.type === "ap_lost") {
                    var gp = map.project(an.ghost), sc = 1 - pr
                    ctx.globalAlpha = sc; ctx.fillStyle = an.ghost.col || "#8a93a6"
                    ctx.beginPath(); ctx.arc(gp.x, gp.y, 4.5 * sc + 0.5, 0, Math.PI * 2); ctx.fill()
                    ctx.beginPath(); ctx.arc(gp.x, gp.y, 6 + 12 * pr, 0, Math.PI * 2)
                    ctx.strokeStyle = an.ghost.col || "#8a93a6"; ctx.lineWidth = 1; ctx.setLineDash([2, 3]); ctx.stroke(); ctx.setLineDash([])
                    ctx.font = "italic 10px sans-serif"; ctx.textAlign = "left"; ctx.fillStyle = "#c9d4e5"
                    ctx.fillText((an.ghost.ssid || "(hidden)") + " gone", gp.x + 8, gp.y - 8 - 14 * pr)
                    ctx.globalAlpha = 1
                } else if (an.type === "ap_up" || an.type === "ap_down") {
                    if (!pos) continue
                    var up = an.type === "ap_up"
                    var bob = (up ? -3 : 3) * Math.abs(Math.sin(now / 220))
                    ctx.globalAlpha = Math.min(1, (1 - pr) * 3) * (0.55 + 0.45 * Math.sin(now / 160))
                    ctx.fillStyle = up ? "#6cff8a" : "#ff9f43"; ctx.font = "bold 11px sans-serif"; ctx.textAlign = "left"
                    ctx.fillText((up ? "▲ " : "▼ ") + (an.delta > 0 ? "+" : "") + an.delta + " dB", pos.x + 10, pos.y + 12 + bob)
                    ctx.globalAlpha = 1
                } else if (an.type === "ap_placed") {
                    if (!pos) continue
                    if (an.fromPos) {
                        var fo = map.project(an.fromPos)
                        ctx.globalAlpha = 1 - pr; ctx.strokeStyle = "rgba(255,209,102,0.6)"; ctx.lineWidth = 1; ctx.setLineDash([3, 3])
                        ctx.beginPath(); ctx.moveTo(fo.x, fo.y); ctx.lineTo(pos.x, pos.y); ctx.stroke(); ctx.setLineDash([])
                    }
                    ctx.globalAlpha = 1 - pr; ctx.strokeStyle = "#ffd166"; ctx.lineWidth = 2.5
                    ctx.beginPath(); ctx.arc(pos.x, pos.y, 8 + 30 * map.easeOut(pr), 0, Math.PI * 2); ctx.stroke()
                    ctx.globalAlpha = 1
                } else if (an.type === "fix") {
                    var to = (an.lat !== undefined && an.lon !== undefined) ? map.merc(an.lat, an.lon) : me
                    var tx = map.sx(to.x), ty = map.sy(to.y)
                    if (an.from) {
                        var fm = map.merc(an.from.lat, an.from.lon), fx = map.sx(fm.x), fy = map.sy(fm.y)
                        var e4 = map.easeOut(Math.min(1, pr / 0.7)), ex = fx + (tx - fx) * e4, ey = fy + (ty - fy) * e4
                        ctx.globalAlpha = pr < 0.7 ? 1 : 1 - (pr - 0.7) / 0.3
                        ctx.strokeStyle = "#35d6ff"; ctx.lineWidth = 2; ctx.setLineDash([6, 4])
                        ctx.beginPath(); ctx.moveTo(fx, fy); ctx.lineTo(ex, ey); ctx.stroke(); ctx.setLineDash([])
                        var ah = Math.atan2(ey - fy, ex - fx)
                        ctx.fillStyle = "#35d6ff"; ctx.beginPath(); ctx.moveTo(ex, ey)
                        ctx.lineTo(ex - 10 * Math.cos(ah - 0.5), ey - 10 * Math.sin(ah - 0.5)); ctx.lineTo(ex - 10 * Math.cos(ah + 0.5), ey - 10 * Math.sin(ah + 0.5)); ctx.closePath(); ctx.fill()
                        ctx.globalAlpha = 1
                    }
                    var pb = Math.min(1, pr / 0.66)
                    ctx.globalAlpha = (1 - pb) * 0.9; ctx.strokeStyle = "#35d6ff"; ctx.lineWidth = 3
                    ctx.beginPath(); ctx.arc(tx, ty, 14 + 46 * map.easeOut(pb), 0, Math.PI * 2); ctx.stroke()
                    ctx.globalAlpha = 1
                } else if (an.type === "stop") {
                    var sm = (an.lat !== undefined && an.lon !== undefined) ? map.merc(an.lat, an.lon) : me
                    var sxp = map.sx(sm.x), syp = map.sy(sm.y)
                    var drop = Math.min(1, pr * 3.5), yoff = -40 * (1 - map.bounce(drop))
                    ctx.globalAlpha = pr > 0.75 ? (1 - pr) / 0.25 : 1
                    ctx.font = "20px sans-serif"; ctx.textAlign = "center"; ctx.fillStyle = "#ff4f4f"
                    ctx.fillText("🚩", sxp, syp - 2 + yoff)
                    if (drop >= 1) {
                        var dr = ((now - an.t0 - 1200) / 900) % 1
                        ctx.globalAlpha *= (1 - dr); ctx.strokeStyle = "#ff4f4f"; ctx.lineWidth = 1.5
                        ctx.beginPath(); ctx.arc(sxp, syp, 4 + 18 * dr, 0, Math.PI * 2); ctx.stroke()
                    }
                    ctx.globalAlpha = 1
                }
            }
            ctx.textAlign = "left"
        }
    }
    Connections {
        target: map.src
        function onApsChanged() { if (map.follow && map.autoZoom) map.fit(); overlay.requestPaint() }
        function onTrackChanged() { overlay.requestPaint() }
        function onPoisChanged() { map.selected = -1; map.cluster() }
        function onLatChanged() { if (map.follow) map.recenter(); overlay.requestPaint() }
        function onLonChanged() { if (map.follow) map.recenter(); overlay.requestPaint() }
        function onAccuracyChanged() { if (map.follow && map.autoZoom) map.fit(); overlay.requestPaint() }
        function onNewEvents(list, animate) { map.onEvents(list, animate) }
    }
    onShowSsidsChanged: overlay.requestPaint()
    onShowEventsChanged: if (!showEvents) { anims = []; animCount = 0; tickerModel.clear(); toastModel.clear(); overlay.requestPaint() }

    // ── places: clustered in world space so panning doesn't reshuffle them ──
    Timer { id: clusterTimer; interval: 140; onTriggered: map.cluster() }
    function cluster() {
        var pois = src.pois || [], out = []
        var hidden = {}
        for (var h = 0; h < hiddenCats.length; h++) hidden[hiddenCats[h]] = true
        var cell = zoom < 13 ? 56 : zoom < 15.5 ? 40 : 0
        var groups = {}, order = []
        for (var i = 0; i < pois.length; i++) {
            var pt = pois[i]
            if (hidden[pt.cat]) continue
            var m = merc(pt.lat, pt.lon)
            if (cell > 0) {
                var key = Math.floor(m.x * ws / cell) + ":" + Math.floor(m.y * ws / cell)
                if (!groups[key]) { groups[key] = {ids: [], mx: 0, my: 0, cats: {}}; order.push(key) }
                var g = groups[key]; g.ids.push(i); g.mx += m.x; g.my += m.y; g.cats[pt.cat] = (g.cats[pt.cat] || 0) + 1
            } else {
                out.push({ids: [i], mx: m.x, my: m.y, icon: pt.icon, color: pt.color, count: 1, name: pt.name, wifi: !!pt.wifi && pt.cat !== "wifi", d: pt.d || 0})
            }
        }
        for (var o = 0; o < order.length; o++) {
            var gr = groups[order[o]], top = "", best = 0
            for (var c in gr.cats) if (gr.cats[c] > best) { best = gr.cats[c]; top = c }
            var first = pois[gr.ids[0]]
            for (var t = 0; t < gr.ids.length; t++) if (pois[gr.ids[t]].cat === top) { first = pois[gr.ids[t]]; break }
            var mixed = Object.keys(gr.cats).length > 1
            out.push({ids: gr.ids, mx: gr.mx / gr.ids.length, my: gr.my / gr.ids.length, icon: first.icon, color: first.color,
                      count: gr.ids.length, badge: mixed ? "#e6edf7" : first.color, name: gr.ids.length === 1 ? first.name : "", wifi: false, d: first.d || 0})
        }
        // Name labels for the nearest single places that don't collide (world px, pan-invariant)
        var taken = [], labelled = 0
        var byDist = out.slice().sort(function(a, b) { return a.d - b.d })
        for (var s = 0; s < byDist.length; s++) {
            var mk = byDist[s]
            mk.showLabel = false
            if (zoom < 15.5 || mk.count > 1 || !mk.name || labelled >= 18) continue
            var wx = mk.mx * ws + 16, wy = mk.my * ws - 9, w = Math.min(150, mk.name.length * 6.5 + 12), hh = 18
            var hit = false
            for (var u = 0; u < out.length && !hit; u++) {
                var ox = out[u].mx * ws, oy = out[u].my * ws
                if (out[u] !== mk && ox > wx - 12 && ox < wx + w + 12 && oy > wy - 12 && oy < wy + hh + 12) hit = true
            }
            for (var v = 0; v < taken.length && !hit; v++) {
                var r = taken[v]
                if (wx < r.x + r.w && wx + w > r.x && wy < r.y + r.h && wy + hh > r.y) hit = true
            }
            if (hit) continue
            taken.push({x: wx, y: wy, w: w, h: hh}); mk.showLabel = true; labelled++
        }
        markers = out
    }

    // ── gestures ────────────────────────────────────────────────────────────
    function hitAt(x, y) {                      // beacon label pill or marker under a point → {kind: "beacon", ids}
        for (var l = 0; l < labelHits.length; l++) {
            var r = labelHits[l]
            if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) return {kind: "beacon", ids: r.ids}
        }
        var best = null, bd = 12
        for (var i = 0; i < beaconHits.length; i++) {
            var h = beaconHits[i], d = Math.hypot(h.x - x, h.y - y)
            if (d < bd) { bd = d; best = h }
        }
        return best ? {kind: "beacon", ids: best.ids} : null
    }
    function pinBeacon(ids) {                   // pin the strongest of a group in the card
        var aps = src.aps || [], top = -1
        for (var i = 0; i < ids.length; i++) if (top < 0 || (aps[ids[i]] && aps[top] && aps[ids[i]].dbm > aps[top].dbm)) top = ids[i]
        selectedBeacon = top >= 0 && aps[top] ? (aps[top].bssid || "") : ""
        selected = -1
        overlay.requestPaint()
    }
    function openContext(x, y) {
        var hit = hitAt(x, y)
        if (hit) pinBeacon(hit.ids)
        ctxMenu.px = x; ctxMenu.py = y
        ctxMenu.popup(map, x, y)
    }
    MouseArea {
        id: pan
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        property point start
        property point startC
        property bool moved: false
        cursorShape: pressed && moved ? Qt.ClosedHandCursor : (map.hover && map.hover.kind === "beacon" ? Qt.PointingHandCursor : Qt.ArrowCursor)
        onPressed: mouse => { map.userTouched(); start = Qt.point(mouse.x, mouse.y); startC = Qt.point(map.cx, map.cy); moved = false }
        onPositionChanged: mouse => {
            if (pressedButtons & Qt.LeftButton) {
                var dx = mouse.x - start.x, dy = mouse.y - start.y
                if (!moved && Math.abs(dx) + Math.abs(dy) < 4) return
                moved = true; map.follow = false
                map.cx = startC.x - dx / map.ws
                map.cy = Math.max(0, Math.min(1, startC.y - dy / map.ws))
                return
            }
            // hover beacons and their name pills (they live on the canvas, not as items)
            var nh = map.hitAt(mouse.x, mouse.y)
            if (JSON.stringify(nh) !== JSON.stringify(map.hover && map.hover.kind === "beacon" ? map.hover : null))
                if (nh || (map.hover && map.hover.kind === "beacon")) map.hover = nh
        }
        onClicked: mouse => {
            if (mouse.button === Qt.RightButton) { map.openContext(mouse.x, mouse.y); return }
            if (moved) return
            var hit = map.hitAt(mouse.x, mouse.y)
            if (hit) map.pinBeacon(hit.ids)
            else { map.selected = -1; if (map.selectedBeacon) { map.selectedBeacon = ""; overlay.requestPaint(); fx.requestPaint() } }
        }
        onDoubleClicked: mouse => { if (mouse.button === Qt.LeftButton) map.zoomAt(1, mouse.x, mouse.y) }
        onExited: if (map.hover && map.hover.kind === "beacon") map.hover = null
        onWheel: wheel => { map.zoomAt(wheel.angleDelta.y / 120 * 0.5, wheel.x, wheel.y); wheel.accepted = true }

        // Touch / touchpad: pinch zooms about the pinch centre; press-and-hold opens the context menu.
        // Handlers sit on the MouseArea so they see the points first; the tap handler is passive
        // (drag threshold policy) and cancels as soon as a pan starts.
        PinchHandler {
            id: pinch
            target: null
            property real z0: 15
            onActiveChanged: if (active) { z0 = map.zoom; map.autoZoom = false; map.follow = false }
            onActiveScaleChanged: if (active) {
                var z = Math.max(3, Math.min(20, z0 + Math.log(activeScale) / Math.LN2))
                map.zoomTarget = z
                map.applyZoom(z, centroid.position.x, centroid.position.y)
            }
        }
        TapHandler {
            acceptedButtons: Qt.LeftButton
            gesturePolicy: TapHandler.DragThreshold
            onLongPressed: map.openContext(point.position.x, point.position.y)
        }
    }

    // ── me ──────────────────────────────────────────────────────────────────
    Item {
        id: meItem
        visible: map.src.valid
        readonly property point m: map.merc(map.src.lat, map.src.lon)
        x: map.sx(m.x); y: map.sy(m.y)
        Rectangle {
            readonly property real r: 10 + 22 * map.phase
            x: -r; y: -r; width: 2 * r; height: 2 * r; radius: r
            color: "transparent"; border.color: "#35d6ff"; border.width: 2; opacity: 1 - map.phase
        }
        Rectangle { x: -14; y: -14; width: 28; height: 28; radius: 14; color: "#35d6ff"; opacity: 0.22 }
        Rectangle { x: -7; y: -7; width: 14; height: 14; radius: 7; color: "#35d6ff"; border.color: "white"; border.width: 1.5 }
        Rectangle { x: -2.5; y: -2.5; width: 5; height: 5; radius: 2.5; color: "white" }
        Rectangle {
            id: meBadge
            property int count: 0
            visible: count > 0
            x: 8; y: 6; height: 15; radius: 7.5; width: meBadgeText.implicitWidth + 10
            color: "#35d6ff"; border.color: "#0b101a"; border.width: 1.5
            PC3.Label { id: meBadgeText; anchors.centerIn: parent; text: meBadge.count + " 📶"; color: "#0b101a"; font.bold: true; font.pixelSize: 10 }
        }
    }

    // ── place markers ───────────────────────────────────────────────────────
    Repeater {
        model: map.markers
        delegate: Item {
            id: mk
            required property var modelData
            readonly property bool hot: pinArea.containsMouse || (modelData.count === 1 && map.selected === modelData.ids[0])
            x: map.sx(modelData.mx); y: map.sy(modelData.my)
            visible: x > -40 && x < map.width + 40 && y > -40 && y < map.height + 40
            z: hot ? 10 : 1
            Rectangle {
                readonly property real r: mk.hot ? 13 : 11
                x: -r; y: -r; width: 2 * r; height: 2 * r; radius: r
                color: "#0c111c"; border.color: mk.modelData.color; border.width: mk.hot ? 2.5 : 2
                Text { anchors.centerIn: parent; text: mk.modelData.icon; font.pixelSize: 12; color: "white" }
            }
            Rectangle {                          // advertises Wi-Fi
                visible: mk.modelData.wifi
                x: 5; y: -11; width: 6; height: 6; radius: 3; color: "#35d6ff"
            }
            Rectangle {                          // cluster count
                visible: mk.modelData.count > 1
                x: 4; y: -18; height: 15; radius: 7.5; width: Math.max(15, cnt.implicitWidth + 8)
                color: mk.modelData.badge || mk.modelData.color; border.color: "#0b101a"; border.width: 1.5
                PC3.Label { id: cnt; anchors.centerIn: parent; text: mk.modelData.count; color: "#0b101a"; font.bold: true; font.pixelSize: 10 }
            }
            Rectangle {                          // name
                visible: mk.modelData.showLabel === true && !mk.hot
                x: 15; y: -9; height: 18; radius: 4
                width: Math.min(150, nameText.implicitWidth + 12)
                color: Qt.rgba(0.03, 0.05, 0.08, 0.78)
                Rectangle { x: 0; y: 3; width: 2.5; height: parent.height - 6; radius: 1; color: mk.modelData.color }
                PC3.Label {
                    id: nameText
                    x: 6; anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, 138)
                    text: mk.modelData.name || ""; elide: Text.ElideRight
                    color: "#e6edf7"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                }
            }
            MouseArea {
                id: pinArea
                x: -14; y: -14; width: 28; height: 28
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onContainsMouseChanged: map.hover = containsMouse ? {kind: mk.modelData.count > 1 ? "cluster" : "poi", ids: mk.modelData.ids}
                                                                  : (map.hover && map.hover.kind !== "beacon" ? null : map.hover)
                onClicked: {
                    if (mk.modelData.count > 1) map.zoomAt(2, mk.x, mk.y)
                    else map.selected = mk.modelData.ids[0]
                }
                onWheel: wheel => { map.zoomAt(wheel.angleDelta.y / 120 * 0.5, mk.x, mk.y) }
            }
        }
    }

    // ── controls ────────────────────────────────────────────────────────────
    component MapButton: PC3.ToolButton {
        id: mb
        property string tip: ""
        width: Kirigami.Units.gridUnit * 1.7; height: width
        display: QQC2.AbstractButton.IconOnly
        icon.color: "#e6edf7"
        QQC2.ToolTip.text: tip || text; QQC2.ToolTip.visible: hovered; QQC2.ToolTip.delay: 500
        background: Rectangle {
            radius: 6
            color: mb.hovered ? Qt.rgba(0.1, 0.14, 0.22, 0.95) : Qt.rgba(0.03, 0.05, 0.08, 0.8)
            border.color: mb.checked ? "#35d6ff" : Qt.rgba(0.21, 0.84, 1, 0.3)
        }
    }
    Column {
        anchors { top: parent.top; right: parent.right; margins: Kirigami.Units.smallSpacing }
        spacing: 4
        MapButton { icon.name: "zoom-in"; text: "Zoom in"; onClicked: map.zoomAt(1, map.width / 2, map.height / 2) }
        MapButton { icon.name: "zoom-out"; text: "Zoom out"; onClicked: map.zoomAt(-1, map.width / 2, map.height / 2) }
        MapButton { icon.name: "mark-location"; text: "Follow my position"; checkable: false; checked: map.follow; onClicked: map.recenter() }
        MapButton { icon.name: "map-flat"; text: "Map style"; onClicked: layerMenu.popup() }
        MapButton { icon.name: "view-filter"; text: "Places to show"; onClicked: placesMenu.popup() }
        MapButton {
            text: "Aa"; tip: "Wi-Fi names on the map"; display: QQC2.AbstractButton.TextOnly
            checkable: true; checked: map.showSsids
            contentItem: PC3.Label { text: "Aa"; color: "#e6edf7"; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            onToggled: map.ssidsToggled(checked)
        }
        MapButton {
            icon.name: "view-history"; text: "Events"; tip: "Event animations and ticker"
            checkable: true; checked: map.showEvents
            onToggled: map.eventsToggled(checked)
        }
        MapButton {
            icon.name: "security-low"; text: "Security"; tip: "Highlight insecure beacons (open, WEP, WPA1/TKIP, WPA2-PSK without PMF) and open the security notes"
            checkable: true; checked: map.secFocus
            onToggled: { map.secFocus = checked; map.secPanel = checked }
        }
        MapButton {
            icon.name: "media-playback-start"; text: "Cinematic"; tip: "Cinematic mode: glide to events, and every few minutes zoom out to show the city and state"
            checkable: true; checked: map.cinematic
            onToggled: map.cinematicToggled(checked)
        }
    }
    Rectangle {                                 // security summary chip (top-left); click → panel
        id: secChip
        anchors { left: parent.left; top: parent.top; margins: Kirigami.Units.smallSpacing }
        visible: map.secSummary.total > map.secSummary.unknown && !map.caption; z: 6   // hidden until the scanner reports RSN flags
        radius: 6; color: Qt.rgba(0.03, 0.05, 0.08, 0.85)
        border.color: map.secSummary.critical ? "#ff4d4d" : map.secSummary.weak ? "#ff9f43" : "#6cff8a"; border.width: 1
        implicitWidth: secLabel.implicitWidth + 16; implicitHeight: secLabel.implicitHeight + 8
        PC3.Label { id: secLabel; anchors.centerIn: parent; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; color: "#e6edf7"
                    text: (map.secSummary.critical ? "☠ " : map.secSummary.weak ? "⚠ " : "🛡 ") + Sec.summaryText(map.secSummary) }
        TapHandler { onTapped: { map.secPanel = !map.secPanel; if (map.secPanel) map.secFocus = true } }
        HoverHandler { cursorShape: Qt.PointingHandCursor }
    }
    Rectangle {                                 // security panel: every beacon graded, worst first, in full nerdspeak
        id: secPanelBox
        visible: map.secPanel; z: 7
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom; margins: Kirigami.Units.smallSpacing; rightMargin: 44 }
        width: Math.min(parent.width * 0.62, Kirigami.Units.gridUnit * 24)
        radius: 8; color: Qt.rgba(0.03, 0.05, 0.08, 0.94); border.color: Qt.rgba(0.21, 0.84, 1, 0.35); border.width: 1
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 8; spacing: 4
            RowLayout {
                Layout.fillWidth: true
                PC3.Label { text: "Radio security audit"; font.bold: true; color: "#e6edf7"; Layout.fillWidth: true }
                PC3.Label { text: `${map.secSummary.total} BSS · ${map.secSummary.critical} insecure · ${map.secSummary.weak} weak · ${map.secSummary.strong} strong`; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; color: "#9fb0c8" }
                PC3.ToolButton { icon.name: "window-close"; onClicked: { map.secPanel = false; map.secFocus = false } }
            }
            ListView {
                id: secList
                Layout.fillWidth: true; Layout.fillHeight: true
                clip: true; spacing: 6
                model: {
                    var aps = map.src.aps || [], rows = []
                    for (var i = 0; i < aps.length; i++) {
                        var a = aps[i]; if (!a || a.kind === "none") continue
                        var c = map.secOf(a); rows.push({ap: a, sec: c, idx: i})
                    }
                    rows.sort(function(x, y) { return y.sec.rank - x.sec.rank || y.ap.dbm - x.ap.dbm })
                    return rows
                }
                delegate: Rectangle {
                    required property var modelData
                    width: secList.width; radius: 6
                    color: Qt.rgba(1, 1, 1, 0.04); border.color: modelData.sec.color; border.width: modelData.sec.rank >= 3 ? 1 : 0
                    implicitHeight: rowCol.implicitHeight + 12
                    ColumnLayout {
                        id: rowCol
                        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 6 }
                        spacing: 2
                        RowLayout {
                            Layout.fillWidth: true
                            PC3.Label { text: modelData.sec.glyph; color: modelData.sec.color; font.bold: true }
                            PC3.Label { text: modelData.ap.ssid || "(hidden)"; font.bold: true; color: "#e6edf7"; elide: Text.ElideRight; Layout.fillWidth: true }
                            PC3.Label { text: Sec.secName(modelData.sec.security) + " · " + modelData.ap.dbm + " dBm" + (map.bandOf(modelData.ap) ? " · " + map.bandOf(modelData.ap) + " GHz" : ""); color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize }
                        }
                        PC3.Label { text: modelData.ap.bssid + (modelData.ap.home ? " · ours (" + (modelData.ap.homeSsid || modelData.ap.ssid) + ")" : ""); color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize }
                        Repeater {
                            model: modelData.sec.issues
                            delegate: ColumnLayout {
                                required property var modelData
                                Layout.fillWidth: true; spacing: 0
                                PC3.Label { text: "▸ " + modelData.title; color: modelData.severity === "critical" ? "#ff4d4d" : modelData.severity === "weak" ? "#ff9f43" : "#e6edf7"; font.bold: true; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; wrapMode: Text.Wrap; Layout.fillWidth: true }
                                PC3.Label { text: modelData.nerd; color: "#c9d4e5"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            }
                        }
                        PC3.Label {
                            readonly property var cl: map.clientsOn(modelData.ap.ssid)
                            visible: cl.length > 0
                            text: "Our devices attached: " + cl.map(function(d) { return (d.name || d.mac) + (d.online ? "" : " (offline)") }).join(", ") + " — every weakness above applies to them."
                            color: "#ffd166"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; wrapMode: Text.Wrap; Layout.fillWidth: true
                        }
                        PC3.Label {
                            visible: !!modelData.ap.homeWlan
                            text: modelData.ap.homeWlan ? `UniFi WLAN: ${modelData.ap.homeWlan.wpa_mode || "?"}/${modelData.ap.homeWlan.wpa_enc || "?"} · PMF ${modelData.ap.homeWlan.pmf_mode || "?"} · WPA3 ${modelData.ap.homeWlan.wpa3_support ? "on" : "off"}${modelData.ap.homeWlan.wpa3_transition ? " (transition)" : ""}` : ""
                            color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; wrapMode: Text.Wrap; Layout.fillWidth: true
                        }
                    }
                    TapHandler { onTapped: { map.selectedBeacon = modelData.ap.bssid; overlay.requestPaint(); fx.requestPaint() } }
                }
            }
        }
    }
    Rectangle {                                 // overview caption (city / state) while touring
        anchors { horizontalCenter: parent.horizontalCenter; top: parent.top; topMargin: Kirigami.Units.largeSpacing }
        visible: opacity > 0; opacity: map.caption ? 1 : 0; z: 6
        Behavior on opacity { NumberAnimation { duration: 600 } }
        radius: 8; color: Qt.rgba(0.03, 0.05, 0.08, 0.78)
        border.color: Qt.rgba(0.21, 0.84, 1, 0.5); border.width: 1
        implicitWidth: capLabel.implicitWidth + 28; implicitHeight: capLabel.implicitHeight + 12
        PC3.Label { id: capLabel; anchors.centerIn: parent; text: map.caption; color: "#e6edf7"; font.bold: true; font.pixelSize: Kirigami.Theme.defaultFont.pixelSize * 1.3 }
    }
    PC3.Menu {                                  // right-click / long-press
        id: ctxMenu
        property real px: 0
        property real py: 0
        PC3.MenuItem { text: "Centre here"; icon.name: "zoom-fit-best"; onTriggered: { var m = map.toMerc(ctxMenu.px, ctxMenu.py); map.follow = false; map.cx = m.x - Math.floor(m.x); map.cy = Math.max(0, Math.min(1, m.y)) } }
        PC3.MenuItem { text: "Zoom in here"; icon.name: "zoom-in"; onTriggered: map.zoomAt(1, ctxMenu.px, ctxMenu.py) }
        PC3.MenuItem { text: "Follow my position"; icon.name: "mark-location"; onTriggered: map.recenter() }
        PC3.MenuSeparator {}
        PC3.MenuItem { text: "Wi-Fi names"; checkable: true; checked: map.showSsids; onTriggered: map.ssidsToggled(checked) }
        PC3.MenuItem { text: "Event animations & ticker"; checkable: true; checked: map.showEvents; onTriggered: map.eventsToggled(checked) }
        PC3.MenuSeparator {}
        PC3.MenuItem { text: "Map style…"; icon.name: "map-flat"; onTriggered: layerMenu.popup() }
        PC3.MenuItem { text: "Places to show…"; icon.name: "view-filter"; onTriggered: placesMenu.popup() }
    }

    // ── toasts (milestones, regions, offline saves, errors) ─────────────────
    Column {
        anchors { left: parent.left; top: parent.top; margins: Kirigami.Units.smallSpacing }
        width: Math.min(map.width * 0.7, Kirigami.Units.gridUnit * 20)
        spacing: 4
        z: 20
        Repeater {
            model: toastModel
            delegate: Rectangle {
                id: toast
                required property string glyph
                required property string line
                required property string col
                required property int key
                width: parent.width; height: 26; radius: 6
                color: Qt.rgba(0.03, 0.05, 0.08, 0.92)
                border.color: col; border.width: 1
                opacity: 0
                SequentialAnimation on opacity {
                    running: true
                    NumberAnimation { to: 1; duration: 250 }
                    PauseAnimation { duration: 3050 }
                    NumberAnimation { to: 0; duration: 700 }
                }
                Timer { interval: 4000; running: true; onTriggered: { for (var i = 0; i < toastModel.count; i++) if (toastModel.get(i).key === toast.key) { toastModel.remove(i); break } } }
                PC3.Label { id: tg; x: 8; anchors.verticalCenter: parent.verticalCenter; text: toast.glyph; color: toast.col; font.pixelSize: 13 }
                PC3.Label { x: 28; width: parent.width - 36; anchors.verticalCenter: parent.verticalCenter; text: toast.line; color: "#e6edf7"; font.bold: true; elide: Text.ElideRight; font.pixelSize: Kirigami.Theme.smallFont.pixelSize }
            }
        }
    }

    // ── event ticker: the last few things that happened, fading out ─────────
    Column {
        id: ticker
        visible: map.showEvents && tickerModel.count > 0
        anchors { left: parent.left; bottom: scaleBox.top; margins: Kirigami.Units.smallSpacing }
        // Collapsed: only the newest line (with a "+N" badge); hover to unfold the last five.
        width: Math.min(map.width * 0.5, Kirigami.Units.gridUnit * 16)
        spacing: 2
        z: 5
        HoverHandler { id: tickerHover }
        Repeater {
            model: tickerModel
            delegate: Rectangle {
                id: tick
                required property int index
                required property string glyph
                required property string line
                required property real t
                required property string col
                readonly property real age: (map.tickNow - t) / 1000
                readonly property bool unfolded: tickerHover.hovered
                width: ticker.width; height: 18; radius: 4
                color: Qt.rgba(0.03, 0.05, 0.08, unfolded ? 0.88 : 0.7)
                opacity: unfolded ? 1 : (age < 25 ? 0.95 : Math.max(0, 1 - (age - 25) / 10))
                visible: opacity > 0 && (index === 0 || unfolded)
                Rectangle { x: 0; y: 3; width: 2.5; height: parent.height - 6; radius: 1; color: tick.col }
                PC3.Label { id: tg2; x: 7; anchors.verticalCenter: parent.verticalCenter; text: tick.glyph; color: tick.col; font.pixelSize: 10; font.bold: true }
                PC3.Label { id: ta; anchors.right: parent.right; anchors.rightMargin: 6; anchors.verticalCenter: parent.verticalCenter
                            text: (tick.index === 0 && !tick.unfolded && tickerModel.count > 1 ? "+" + (tickerModel.count - 1) + " · " : "") + map.ageText(tick.age)
                            color: "#9fb0c8"; font.pixelSize: 9 }
                PC3.Label { x: 24; width: ta.x - 30; anchors.verticalCenter: parent.verticalCenter; text: tick.line; color: "#e6edf7"; elide: Text.ElideRight; font.pixelSize: Kirigami.Theme.smallFont.pixelSize }
            }
        }
    }
    PC3.Menu {
        id: layerMenu
        Instantiator {
            model: ["Dark", "Streets", "Satellite", "Topographic"]
            delegate: PC3.MenuItem {
                required property string modelData
                required property int index
                text: modelData; checkable: true; checked: map.layerIndex === index
                onTriggered: map.layerPicked(index)
            }
            onObjectAdded: (index, object) => layerMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => layerMenu.removeItem(object)
        }
    }
    PC3.Menu {
        id: placesMenu
        PC3.MenuItem { text: "Show all"; onTriggered: map.allCategories(true) }
        PC3.MenuItem { text: "Hide all"; onTriggered: map.allCategories(false) }
        PC3.MenuSeparator {}
        Instantiator {
            model: map.src.poiCategories || []
            delegate: PC3.MenuItem {
                required property var modelData
                text: {
                    var n = 0, p = map.src.pois || []
                    for (var i = 0; i < p.length; i++) if (p[i].cat === modelData.key) n++
                    return `${modelData.icon}  ${modelData.label}  (${n})`
                }
                checkable: true
                checked: map.hiddenCats.indexOf(modelData.key) < 0
                onTriggered: map.categoryToggled(modelData.key, checked)
            }
            onObjectAdded: (index, object) => placesMenu.insertItem(index + 3, object)
            onObjectRemoved: (index, object) => placesMenu.removeItem(object)
        }
    }

    // ── scale + attribution ─────────────────────────────────────────────────
    Rectangle {
        id: scaleBox
        readonly property var step: {
            var steps = [5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000]
            var m = map.mpp(), best = steps[0]
            for (var i = 0; i < steps.length; i++) if (steps[i] / m <= 80) best = steps[i]
            return {m: best, px: best / m}
        }
        anchors { left: parent.left; bottom: parent.bottom; margins: Kirigami.Units.smallSpacing }
        width: step.px + scaleText.implicitWidth + 20; height: 18; radius: 4
        color: Qt.rgba(0.03, 0.05, 0.08, 0.78)
        Rectangle { x: 6; y: 11; width: scaleBox.step.px; height: 1.5; color: "#e6edf7" }
        Rectangle { x: 6; y: 6; width: 1.5; height: 6; color: "#e6edf7" }
        Rectangle { x: 6 + scaleBox.step.px - 1.5; y: 6; width: 1.5; height: 6; color: "#e6edf7" }
        PC3.Label {
            id: scaleText
            x: scaleBox.step.px + 12; anchors.verticalCenter: parent.verticalCenter
            text: scaleBox.step.m >= 1000 ? scaleBox.step.m / 1000 + " km" : scaleBox.step.m + " m"
            color: "#e6edf7"; font.pixelSize: 10
        }
    }
    PC3.Label {
        anchors { right: parent.right; bottom: parent.bottom; margins: 3 }
        text: !map.tileBase ? "© Esri · OpenStreetMap" : "© OpenStreetMap" + (map.layerIndex === 2 ? " · Esri" : map.layerIndex === 3 ? " · OpenTopoMap" : "")
        color: Qt.rgba(0.9, 0.93, 0.97, 0.55); font.pixelSize: 9
    }

    // ── info card: hovered marker / beacon, or the pinned place ─────────────
    Rectangle {
        id: card
        readonly property var info: {
            var src = map.src, h = map.hover, pois = src.pois || [], aps = src.aps || []
            if (h && h.kind === "poi") return map.poiInfo(pois[h.ids[0]], false)
            if (h && h.kind === "cluster") {
                var l = []
                for (var i = 0; i < Math.min(8, h.ids.length); i++) { var p = pois[h.ids[i]]; l.push(p.icon + " " + (p.name || p.label)) }
                if (h.ids.length > 8) l.push("… and " + (h.ids.length - 8) + " more")
                return {title: h.ids.length + " places — click to zoom in", lines: l, color: "#e6edf7", poi: null}
            }
            if (h && h.kind === "beacon") {
                var ids = h.ids.slice().sort(function(a, b) { return aps[b].dbm - aps[a].dbm })
                if (ids.length === 1) return map.beaconInfo(aps[ids[0]], false)
                var ll = []
                for (var j = 0; j < Math.min(8, ids.length); j++) { var b = aps[ids[j]]; ll.push(`${b.ssid || "(hidden)"}  ${b.dbm} dBm · ${b.status}`) }
                if (ids.length > 8) ll.push("… and " + (ids.length - 8) + " more")
                return {title: ids.length + " beacons here — click to pin the strongest", lines: ll, color: "#35d6ff", poi: null}
            }
            if (map.selectedBeacon) for (var s = 0; s < aps.length; s++) if (aps[s].bssid === map.selectedBeacon) return map.beaconInfo(aps[s], true)
            if (map.selected >= 0 && map.selected < pois.length) return map.poiInfo(pois[map.selected], true)
            return null
        }
        visible: info !== null
        anchors { left: parent.left; right: parent.right; bottom: ticker.visible ? ticker.top : scaleBox.top; margins: Kirigami.Units.smallSpacing }
        anchors.rightMargin: Kirigami.Units.gridUnit * 2.4
        height: cardCol.implicitHeight + 14
        radius: 7
        color: Qt.rgba(0.03, 0.05, 0.08, 0.93)
        border.color: info ? info.color : "transparent"; border.width: 1
        Column {
            id: cardCol
            x: 10; y: 7; width: parent.width - 20
            spacing: 1
            PC3.Label { width: parent.width; text: card.info ? card.info.title : ""; font.bold: true; color: "white"; elide: Text.ElideRight }
            Repeater {
                model: card.info ? card.info.lines : []
                delegate: PC3.Label {
                    required property string modelData
                    width: cardCol.width; text: modelData; elide: Text.ElideRight
                    color: "#c9d4e5"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                }
            }
            Row {
                visible: !!(card.info && card.info.poi && card.info.pinned)
                spacing: 4
                PC3.ToolButton {
                    icon.name: "go-next"; text: "Directions"
                    onClicked: Qt.openUrlExternally(`https://www.openstreetmap.org/directions?engine=fossgis_osrm_car&route=${map.src.lat},${map.src.lon};${card.info.poi.lat},${card.info.poi.lon}`)
                }
                PC3.ToolButton { icon.name: "internet-web-browser"; text: "OSM"; onClicked: Qt.openUrlExternally(card.info.poi.osm) }
                PC3.ToolButton {
                    visible: !!(card.info && card.info.poi && card.info.poi.website)
                    icon.name: "globe"; text: "Website"; onClicked: Qt.openUrlExternally(card.info.poi.website)
                }
            }
        }
    }
    function beaconInfo(a, pinned) {
        if (!a) return null
        var how = a.kind === "wigle" ? "mapped position (WiGLE / Apple)" : a.kind === "centroid" ? `multilaterated from ${a.vantage || 2} places, ±${map.distText(a.r)}`
                : `~${map.distText(a.r)} away by signal — direction unknown`
        var band = map.bandOf(a) ? map.bandOf(a) + " GHz" : "", ch = a.ch ? ` ch ${a.ch}` : ""
        var st = a.status === "used" ? "used for the fix" : a.status === "active" ? "connected · travels with you"
               : a.status === "travelling" ? "travels with you" : a.status === "ignored" ? "ignored" : (a.status || "")
        var lines = [`${a.bssid}${band ? " · " + band + ch : ""} · ${a.dbm} dBm`, st, how]
        var sc = map.secOf(a)
        lines.push(`${sc.glyph} ${Sec.secName(sc.security)} · ${sc.label.toUpperCase()}`)
        var n = pinned ? sc.issues.length : Math.min(2, sc.issues.length)
        for (var i = 0; i < n; i++) lines.push((pinned ? "▸ " : "· ") + sc.issues[i].title + (pinned ? " — " + sc.issues[i].nerd : ""))
        var cl = map.clientsOn(a.ssid)
        if (cl.length) lines.push("Our devices on it: " + cl.map(function(d) { return d.name || d.mac }).join(", ") + " — they inherit the above")
        lines.push(pinned ? "Pinned — click the map to dismiss" : "Click to pin (full security notes)")
        var col = sc.rank >= 3 ? sc.color : a.status === "used" ? "#ffd166" : a.status === "active" ? "#6cff8a" : a.status === "travelling" ? "#ff4fd8" : "#8a93a6"
        return {title: (sc.rank >= 3 ? sc.glyph + " " : "📶 ") + (a.ssid || "(hidden network)"), lines: lines, color: col, poi: null}
    }
    function poiInfo(p, pinned) {
        if (!p) return null
        var lines = [`${p.label} · ${distText(p.d || 0)} ${compass(p.brg || 0)}` + (src.source === "ip" ? " (from IP estimate)" : "")]
        if (p.detail) lines.push(p.detail)
        if (p.hours) lines.push("🕑 " + p.hours)
        if (p.phone) lines.push("☎ " + p.phone)
        if (!pinned) lines.push("Click for directions")
        return {title: `${p.icon} ${p.name || p.label}`, lines: lines, color: p.color, poi: p, pinned: pinned}
    }

    Component.onCompleted: { recenter(); cluster(); refreshTiles() }
}
