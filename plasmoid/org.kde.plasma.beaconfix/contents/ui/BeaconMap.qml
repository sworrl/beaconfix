pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Window
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
    property int  satSource: 0                 // satellite imagery: 0 Esri · 1 Esri Clarity · 2 USGS (US) · 3 NASA VIIRS (yesterday)
    signal satSourcePicked(int index)
    property bool showContours: true           // satellite hybrid: USGS contour lines (US; elevation in feet)
    signal contoursToggled(bool on)
    // Pinpointing an AP to under a foot needs zoom 22-23 (~2.3-4.6 cm a pixel here): the imagery stops at its own
    // deepest level (19 for Esri) and is scaled past it; the overlay, heat tiles and the scale bar stay sharp
    readonly property int maxZoomView: 23
    // The tray draws the route heat map as tiles (off the shell's thread, GPU-scaled while zooming); the Canvas
    // heat map is only the fallback for a tray without them
    readonly property bool heatTiles: !!tileBase && !!(src && src.heatGen)
    property var  hiddenCats: []
    property real phase: 0
    property bool showSsids: true              // Wi-Fi names beside the beacons
    property bool showEvents: true             // event animations + ticker
    property bool showHeatmap: true            // cumulative route heatmap
    property real heatmapOpacity: (Plasmoid.configuration.heatmapOpacity !== undefined && Plasmoid.configuration.heatmapOpacity > 0) ? Plasmoid.configuration.heatmapOpacity : 0.45
    property bool showCameras: true            // Flock Safety & surveillance cameras
    property var  routeSegments: []            // precomputed route polylines: fast, stutter-free, spike-free
                                               // (rebuilt by the src Connections below: routeFixes, else the track)
    // Cameras in a spatial grid, rebuilt only when the list changes (the desktop sends every camera it knows,
    // 100k+ nationwide): a paint walks the cells around the view instead of projecting every camera.
    property var  camIndex: null               // {levels: {6, 8, 10, 12: {n, cells: {key: cell}, keys: [...]}}, count}
    // On screen: the map tab is current and its window (the panel popup, the desktop) is shown. A collapsed
    // popup keeps its items `visible`; only the window says it is hidden.
    readonly property bool onScreen: visible && Window.visibility !== Window.Hidden
    signal layerPicked(int index)
    signal categoryToggled(string key, bool visible)
    function groupToggled(group, visible) { var cats = map.src.poiCategories || []; for (var i = 0; i < cats.length; i++) if ((cats[i].group || "services") === group) map.categoryToggled(cats[i].key, visible) }
    signal allCategories(bool visible)
    signal ssidsToggled(bool on)
    signal eventsToggled(bool on)

    // ── events: what changed since the last read, drawn as motion ────────────
    property var  anims: []                    // running animations [{type, bssid, t0, dur, ...}]
    property int  animCount: 0
    property real animNow: 0
    property var  posBy: ({})                  // beacon screen positions from the last base paint (for the fx layer)
    property var  pillBy: ({})                 // bssid → its name pill {x, y, w, h, col} from the last base paint
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
    // ── cinematic mode: slow eased fly-ins on significant events, occasional zoom-outs to city / state ──
    property bool cinematic: false
    property int  tourMinutes: 20               // 0: no overviews
    property int  spotlightMinutes: 10          // at most one glide to an event per this many minutes (0: never)
    property bool hostHovered: false            // the pointer is over the widget (set by main.qml)
    property bool flying: false
    property bool holding: false
    readonly property bool zooming: Math.abs(zoomTarget - zoom) > 0.002
    property real lastUserInput: 0
    property real lastSpotlightAt: 0
    property real _autoAt: 0                    // when the last automatic zoom (tour, spotlight, follow re-fit) started
    // One budget shared by every automatic zoom: at most one per 10 minutes, whatever the overview / spotlight
    // intervals (a shorter one only means "as often as the budget allows"), so tours, spotlights and re-fits
    // never add up. The Cinematic help text and the settings page promise exactly this.
    readonly property real autoGapMs: 600000
    property real _hoverEndAt: 0
    property bool _placed: false                // the view has been put on the fix once
    property var  _homeView: null               // {z, auto}: the view an automatic sequence returns to (non-null while one runs)
    property var  _fitVote: null                // {dir, n, since, z}: the fitted zoom has sat a whole level away for n polls
    property var  _fitAt: null                  // {lat, lon}: the fix the zoom was last fitted for (placement, Follow, a re-zoom)
    property real _fitSide: 0                   // the map's shorter side at that fit
    property real _placedAt: 0
    property bool _settleSpent: false           // the one re-zoom allowed without moving, after a placement, has happened
    readonly property var _bestAcc: ({})        // bssid → tightest accuracy any desktop refit reported (mutated in place)
    onHostHoveredChanged: if (!hostHovered) _hoverEndAt = Date.now()
    onTourMinutesChanged: tourTimer.nextAt = 0
    // Switching cinematic off stops a running sequence at once and puts the view back where it was
    onCinematicChanged: if (!cinematic) { _fitVote = null; if (inSequence()) abortToHome() }
    property string caption: ""
    property var  _fly: null
    property bool _flyZooms: false              // the current flight changes the zoom (a pure pan keeps the labels)
    signal cinematicToggled(bool on)
    // ── security overlay ──
    property bool secFocus: false               // dim everything that is not insecure
    property bool secPanel: false
    property bool showDevices: true             // linked devices (phone, laptop) on the map
    readonly property var linked: src.linkedDevices || []
    function deviceGlyph(k, role) {
        if (k === "esp32-node" || k === "node" || k === "mesh") {
            return (role === "base_station" || role === "base") ? "🏠" : "📡"
        }
        return k === "android" ? "📱" : k === "laptop" ? "💻" : k === "desktop" ? "🖥" : "📍"
    }

    function placeMeshNode(name, lat, lon) {
        var a = {
            id: "esp32-" + String(name).toLowerCase(),
            name: name,
            kind: "esp32-node",
            lat: lat,
            lon: lon,
            heightM: 1.0,
            accM: 1.0,
            bssids: [],
            rv: false,
            placedBy: "widget",
            placedAt: new Date().toISOString(),
            source: "map-pick"
        }
        src.saveAnchor(a)
    }

    // ── surveyed antenna anchors: placed with the picker, ground truth for the maths ──
    readonly property var antennas: src.antennaAnchors || []
    readonly property bool antennasSupported: !!src.anchorsSupported
    property var  editAnchor: null              // being placed or edited: {id, name, kind, lat, lon, heightM, rv, bssids, accM, isNew}
    property bool draggingAnchor: false
    readonly property var anchorKinds: [
        {k: "this-computer", t: "This computer's Wi-Fi antenna"},
        {k: "esp32-node",    t: "Mesh Node / Base Station"},
        {k: "wifi-ap",       t: "A Wi-Fi access point / router"},
        {k: "rtt-responder", t: "A Wi-Fi RTT responder"},
        {k: "ble",           t: "A Bluetooth device"},
        {k: "custom",        t: "Something else"}]
    function lonOf(mx) { var x = mx - Math.floor(mx); return x * 360 - 180 }
    function anchorColor(k) { return k === "this-computer" ? "#7cf2c4" : k === "esp32-node" ? "#ffd166" : k === "wifi-ap" ? "#35d6ff" : k === "rtt-responder" ? "#ffd166" : k === "ble" ? "#c9a0ff" : "#e6edf7" }
    function anchorScreen(a) { var m = merc(a.lat, a.lon); return Qt.point(sx(m.x), sy(m.y)) }
    function anchorAt(px, py) {
        var list = antennas.slice(); if (editAnchor && editAnchor.isNew) list.push(editAnchor)
        for (var i = list.length - 1; i >= 0; i--) {
            var a = (editAnchor && list[i].id === editAnchor.id) ? editAnchor : list[i], p = anchorScreen(a)
            if (Math.abs(p.x - px) <= 14 && Math.abs(p.y - py) <= 14) return a
        }
        return null
    }
    function pickAcc() { return Math.max(0.2, Math.round(4 * mpp() * 10) / 10) }   // four pixels of pointing precision at this zoom
    function newUuid() {
        var h = "0123456789abcdef", s = ""
        for (var i = 0; i < 32; i++) s += h[Math.floor(Math.random() * 16)]
        return s.substr(0, 8) + "-" + s.substr(8, 4) + "-4" + s.substr(13, 3) + "-" + h[8 + Math.floor(Math.random() * 4)] + s.substr(17, 3) + "-" + s.substr(20, 12)
    }
    function startAnchor(px, py) { var m = toMerc(px, py); startAnchorAt(latOf(m.y), lonOf(m.x)) }
    function startAnchorAt(lat, lon) {
        var hasPc = antennas.some(function(a) { return a.kind === "this-computer" })
        follow = false; autoZoom = false; userTouched()
        editAnchor = {isNew: true, id: newUuid(), name: hasPc ? "" : "Wi-Fi antenna", kind: hasPc ? "wifi-ap" : "this-computer",
                      lat: lat, lon: lon, heightM: 1.0, rv: true, bssids: [], accM: pickAcc()}
        overlay.requestPaint()
    }
    function editExisting(a) { follow = false; autoZoom = false; userTouched(); editAnchor = Object.assign({}, a, {isNew: false}); overlay.requestPaint() }
    function patchAnchor(o) { if (!editAnchor) return; editAnchor = Object.assign({}, editAnchor, o); overlay.requestPaint() }
    function toggleBssid(b) {
        if (!editAnchor) return
        var l = (editAnchor.bssids || []).slice(), i = l.indexOf(b)
        if (i >= 0) l.splice(i, 1); else l.push(b)
        patchAnchor({bssids: l})
    }
    function selectHomeRadios() {                 // the RV router: every radio the desktop classes as home
        var l = [], aps = src.aps || []
        for (var i = 0; i < aps.length; i++) if (aps[i] && (aps[i].home || aps[i].status === "home") && l.indexOf(String(aps[i].bssid).toUpperCase()) < 0) l.push(String(aps[i].bssid).toUpperCase())
        patchAnchor({bssids: l, name: editAnchor && editAnchor.name ? editAnchor.name : "RV router"})
    }
    function saveAnchor() {
        var a = editAnchor; if (!a) return
        var out = {id: a.id, name: a.name || (a.kind === "this-computer" ? "Wi-Fi antenna" : "Antenna"), kind: a.kind, lat: a.lat, lon: a.lon,
                   heightM: a.heightM, accM: a.accM, bssids: (a.bssids || []).map(function(b) { return String(b).toUpperCase() }), rv: !!a.rv,
                   ref: a.kind === "this-computer", placedBy: "widget", placedAt: new Date().toISOString(), source: "map-pick"}
        src.saveAnchor(out)
        editAnchor = null; overlay.requestPaint()
    }
    function deleteAnchor() { if (!editAnchor) return; if (!editAnchor.isNew) src.removeAnchor(editAnchor.id); editAnchor = null; overlay.requestPaint() }
    onAntennasChanged: overlay.requestPaint()
    property var  knownDevices: src.knownDevices || []
    readonly property var secSummary: Sec.summary(src.aps || [], function(a) { return a.homeWlan || null })
    // classification per beacon, cached for the current aps array (the overlay asks for every beacon on every paint)
    // (a plain object mutated in place: no change signals, so bindings calling secOf() do not loop)
    readonly property var _secMemo: ({aps: null, byId: {}})
    function secOf(ap) {
        var memo = _secMemo
        if (memo.aps !== src.aps) { memo.aps = src.aps; memo.byId = {} }
        var k = ap.bssid || "", c = k ? memo.byId[k] : undefined
        if (c === undefined) { c = Sec.classify(ap, ap.homeWlan || null); if (k) memo.byId[k] = c }
        return c
    }
    function clientsOn(ssid) {
        var out = [], kd = knownDevices || []
        for (var i = 0; i < kd.length; i++) if (kd[i] && kd[i].network && ssid && kd[i].network === ssid) out.push(kd[i])
        return out
    }
    property bool autoZoom: true
    readonly property string tileBase: src.tileBase || ""     // tray's localhost tile server
    readonly property int  maxZ: layerIndex === 2 ? [19, 19, 16, 9][satSource] || 19
                               : tileBase ? (layerIndex === 3 ? 17 : 19) : (layerIndex === 0 ? 16 : 19)
    readonly property real ws: 256 * Math.pow(2, zoom)

    // ── view pipeline ───────────────────────────────────────────────────────
    // Camera writes (cx, cy, zoom) are coalesced into one viewUpdate() per frame. The tiles, places and "me"
    // are laid out for a reference camera (iref), the overlay canvas was painted for another (pref); both
    // follow the live camera through an exact similarity transform p' = s·p + t, so a motion frame moves
    // textures instead of re-running hundreds of bindings and a canvas paint. The items rebase every 0.3
    // zoom levels, the canvas re-renders (lite, no text pills) at most every 250 ms, and everything is
    // rebuilt at full quality once the camera has been still for 150 ms.
    property var  iref: ({cx: 0.5, cy: 0.5, zoom: 15, ws: 256 * 32768})
    property var  pref: ({cx: 0.5, cy: 0.5, zoom: 15, ws: 256 * 32768})
    property var  iX: ({s: 1, tx: 0, ty: 0})     // live transform of the item layers
    property var  pX: ({s: 1, tx: 0, ty: 0})     // live transform of the painted overlay
    property real invS: 1                        // 1 / iX.s: markers keep their size while the layer scales
    property bool moving: false                  // the camera changed within the last 150 ms
    // Paint without the Wi-Fi name pills only while the zoom changes: a pure pan (drag, follow glide) moves the
    // painted texture, pills included, and only re-renders when it nears the edge of the painted margin
    readonly property bool lite: zooming || draggingAnchor || pinch.active || (flying && _flyZooms)
    // Painted beyond the edges, for pans and zoom-outs: half the width left and right, half the height above and
    // below (at most 512 px) still cover the view at half scale, a whole zoom level out; viewUpdate() repaints at
    // once when the scale leaves [0.8, 1.25], so a fast zoom-out no longer uncovers the edges between paints.
    readonly property int  ovMarginX: Math.round(Math.min(width * 0.5, 512))
    readonly property int  ovMarginY: Math.round(Math.min(height * 0.5, 512))
    property real _paintAt: 0
    property real _tilesAt: 0
    property bool _zoomDirty: false
    property bool _viewQueued: false
    Matrix4x4 { id: itemM }
    Matrix4x4 { id: paintM }
    function camNow() { return {cx: cx, cy: cy, zoom: zoom, ws: ws, w: width, h: height} }
    // Reference camera r (view w0 × h0) → live camera: x = W/2 + wrap(mx − cx)·ws with mx from x0 gives
    // x = s·x0 + W/2 − s·w0/2 + wrap(r.cx − cx)·ws, s = 2^(zoom − r.zoom); the same for y without the wrap.
    function camXform(r, w0, h0) {
        var s = Math.pow(2, zoom - r.zoom)
        var dmx = r.cx - cx; if (dmx > 0.5) dmx -= 1; else if (dmx < -0.5) dmx += 1
        return {s: s, tx: width / 2 - s * w0 / 2 + dmx * ws, ty: height / 2 - s * h0 / 2 + (r.cy - cy) * ws}
    }
    function setMatrix(m, x) { m.matrix = Qt.matrix4x4(x.s, 0, 0, x.tx, 0, x.s, 0, x.ty, 0, 0, 1, 0, 0, 0, 0, 1) }
    function rebaseItems() {
        iref = camNow(); iX = {s: 1, tx: 0, ty: 0}; setMatrix(itemM, iX)
        if (invS !== 1) invS = 1
    }
    function applyXforms() {
        var x = camXform(iref, width, height)          // the items' bindings already use the current size
        // Re-laying-out every tile and marker costs 30-80 ms: mid-gesture the transform carries them further (the
        // settle rebases anyway); at rest a 0.3-level drift already rebases, for crisp, pixel-snapped tiles
        if (Math.abs(zoom - iref.zoom) > (lite ? 2.5 : 0.3) || Math.abs(iref.cx - cx) > 0.25 || Math.abs(x.tx) > 4 * width || Math.abs(x.ty) > 4 * height) rebaseItems()
        else { iX = x; setMatrix(itemM, x); var inv = 1 / x.s; if (Math.abs(invS - inv) > 1e-9) invS = inv }
        pX = camXform(pref, pref.w || width, pref.h || height); setMatrix(paintM, pX)
    }
    // The canvas is about to render for the live camera: bring the item layers to it in the same frame too
    // (a data paint can land after a camera write whose viewUpdate() is still queued; the tiles would lag a tick)
    function rebasePaint() { pref = camNow(); applyXforms(); _paintAt = Date.now() }
    function paintToView(p) { return {x: p.x * pX.s + pX.tx, y: p.y * pX.s + pX.ty, col: p.col} }
    function viewToPaint(x, y) { return {x: (x - pX.tx) / pX.s, y: (y - pX.ty) / pX.s} }
    function itemToView(x, y) { return Qt.point(x * iX.s + iX.tx, y * iX.s + iX.ty) }
    function rsx(mx) { var dx = mx - iref.cx; if (dx > 0.5) dx -= 1; else if (dx < -0.5) dx += 1; return width / 2 + dx * iref.ws }
    function rsy(my) { return height / 2 + (my - iref.cy) * iref.ws }
    function rmpp() { return 40075016.686 * Math.cos(latOf(iref.cy) * Math.PI / 180) / iref.ws }
    function viewChanged(zoomed) {
        if (zoomed) _zoomDirty = true
        if (!_viewQueued) { _viewQueued = true; Qt.callLater(viewUpdate) }
    }
    function viewUpdate() {                     // once per frame while the camera moves
        _viewQueued = false
        var now = Date.now()
        if (!moving) moving = true
        settleTimer.restart()
        applyXforms()
        if (now - _tilesAt >= 120) { _tilesAt = now; refreshTiles() }
        // While a zoom is in progress (wheel glide, pinch, a zooming flight) the painted texture is only scaled by
        // the GPU: a Canvas re-render rasterises on this thread (40-130 ms for the overlay, the heat-map strokes
        // most of it), which held a zoom to ~7 fps. One full paint lands when it settles; only a zoom-out deep
        // enough to bare the margins' edges (scale < 0.45) repaints mid-gesture. Pans: 4 paints a second, and at
        // once when the scale leaves [0.8, 1.25] (edges popping in late / blurry).
        if (overlayStale()) {
            if (lite) { if (pX.s < 0.45 || pX.s > 6) overlay.requestPaint() }
            else if (now - _paintAt >= 250 || pX.s < 0.8 || pX.s > 1.25) overlay.requestPaint()
        }
        if (fxAnimating() || (fxActive() && !lite)) fx.requestPaint()   // static marks are hidden mid-gesture (onLiteChanged)
        if (_zoomDirty) { _zoomDirty = false; clusterTimer.restart() }
    }
    // The painted overlay still covers the view: same zoom, and the pan has not eaten most of the margin
    function overlayStale() { return Math.abs(pX.s - 1) > 1e-6 || Math.abs(pX.tx) > 0.7 * ovMarginX || Math.abs(pX.ty) > 0.7 * ovMarginY }
    Timer {                                     // the camera stopped: full-quality tiles, layout and paint
        id: settleTimer
        interval: 150
        onTriggered: {
            if (map.flying || map.zooming || pinch.active) { restart(); return }
            map.moving = false
            map._tilesAt = Date.now(); map.refreshTiles()
            map.rebaseItems(); map.applyXforms()
            overlay.requestPaint()
            if (map.fxActive()) fx.requestPaint()
        }
    }

    property int  selected: -1                 // POI index with a pinned card
    property var  hover: null                  // {kind: "poi"|"cluster"|"beacon", ids: [...]}
    property var  markers: []                  // POI markers after clustering, by delegate slot (null / hid: slot unused)
    property var  markerLabels: []             // slot → its name label shows
    property int  markerPool: 0                // place delegates kept alive: grow in steps, never shrink
    readonly property var _noMarker: ({ids: [], mx: 0, my: 0, icon: "", color: "#000000", count: 0, name: "", wifi: false, d: 0, hid: true})
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
    function recenter() {                      // the user's "follow my position" (and first placement): immediate
        if (!src.valid) return
        stopCinema()
        var m = merc(src.lat, src.lon); cx = m.x; cy = m.y
        follow = true; autoZoom = true; _placed = true
        _placedAt = Date.now(); _settleSpent = false
        fit()
    }
    // Data-driven follow (new fix, accuracy or beacons), coalesced to one step per poll.
    // Pan: whenever Follow is on and the fix has left a dead-band around the centre (fix noise never moves
    //      the map); only an antenna edit, a press, a drag, a pinch or an open context menu holds it back.
    // Re-zoom: only in cinematic mode, only once the fix has really gone somewhere since the zoom was last
    //      fitted (parked, the fit swings with signal noise alone), only once the fitted zoom has sat a whole
    //      level away for 3 polls (and 40 s), within the shared automatic-zoom budget, and never while someone
    //      is at the map (mayAutoMove).
    function scheduleFollow() { Qt.callLater(followUpdate) }
    function inSequence() { return _homeView !== null }
    function followUpdate() {
        if (!src.valid || width < 50) return
        if (!_placed) { recenter(); return }
        if (!follow) { _fitVote = null; return }
        if (inSequence() || holding) return        // a tour / spotlight comes home to the fix by itself
        if (flying || zooming || editAnchor || draggingAnchor || pan.pressed || pinch.active || ctxMenu.visible) { followRetry.restart(); return }
        var m = merc(src.lat, src.lon), z = zoom, now = Date.now()
        if (autoZoom) {
            var vz = rezoomVote(now)
            if (vz !== zoom && cinematic && (!onScreen || (mayAutoMove() && now - _autoAt >= autoGapMs))) z = vz
        } else _fitVote = null
        if (z !== zoom) { _fitVote = null; _fitAt = {lat: src.lat, lon: src.lon}; _settleSpent = true; if (onScreen) _autoAt = now }
        if (!onScreen) { cx = m.x; cy = m.y; if (z !== zoom) zoom = zoomTarget = z; return }   // another tab / closed popup: just be there
        var dx = m.x - cx; if (dx > 0.5) dx -= 1; else if (dx < -0.5) dx += 1
        var dpx = Math.hypot(dx * ws, (m.y - cy) * ws)
        if (z === zoom && dpx <= followDeadPx()) return
        if (dpx > 3 * Math.max(width, height)) { cx = m.x; cy = m.y; zoom = zoomTarget = z; return }   // across the map: nothing to glide over
        var az = autoZoom
        flyTo(src.lat, src.lon, z, 600, function() { map.follow = true; map.autoZoom = az }, "follow")
    }
    Timer { id: followRetry; interval: 700; onTriggered: map.followUpdate() }   // the user's zoom / press / glide in the way: once it is over
    function followDeadPx() {                  // "me" may wander this far off-centre before the camera follows
        var side = Math.min(width, height), accPx = src.accuracy > 0 ? src.accuracy / mpp() : 0     // still inside its accuracy ring
        return Math.min(0.3 * side, Math.max(0.12 * side, accPx))
    }
    // → the zoom to re-fit to, or the current zoom while the fit has not settled a whole level away.
    // The re-fit goes to the most conservative fit of the run, so one noisy poll cannot overshoot it.
    function rezoomVote(now) {
        var fz = fitZoom(), d = fz - zoom
        if (Math.abs(d) < 1 || !mayRefit(now)) { _fitVote = null; return zoom }
        var dir = d > 0 ? 1 : -1, v = _fitVote
        if (!v || v.dir !== dir) v = _fitVote = {dir: dir, n: 0, since: now, z: fz}
        v.n++
        if (Math.abs(d) < Math.abs(v.z - zoom)) v.z = fz
        return v.n >= 3 && now - v.since >= 40000 ? v.z : zoom
    }
    // Parked, the fitted zoom still swings a level or more with signal noise alone (weak rings on the cut-off,
    // far "used" peers dropping in and out, the fix's accuracy breathing): re-zooming on that just undoes the
    // previous re-zoom. So a re-zoom needs the fix to have left the spot where the zoom was last fitted, by more
    // than its own error; the only exception is one correction of the first fit in the 10 minutes after the map
    // was placed (that fit came from a single poll).
    function refitMoveM() { return Math.max(200, Math.min(2 * Math.max(0, src.accuracy), 1500)) }
    function mayRefit(now) {
        var a = _fitAt
        if (!a || haversine(a.lat, a.lon, src.lat, src.lon) > refitMoveM()) return true
        return !_settleSpent && now - _placedAt < 600000
    }
    // One guard for every automatic camera sequence and re-zoom (spotlights, tours, follow re-fits). With Follow
    // off (the user panned, centred or zoomed somewhere else, or picked a place) the camera stays where it was
    // left until Follow is switched back on. (A popup counts from the moment it starts opening: `visible`, not
    // `opened`, which is false during its enter and exit transitions.)
    function pointerOver() { return mapHover.hovered || hostHovered }
    function mayAutoMove() {
        var now = Date.now()
        return follow && onScreen && src.valid && !editAnchor && !draggingAnchor && !secPanel && !ctxMenu.visible
               && !pointerOver() && now - _hoverEndAt > 20000
               && now - lastUserInput > 180000 && !pan.pressed && !pinch.active
    }
    function fitZoom() {
        if (!src.valid || width < 50) return zoom
        // Fit the beacons, not the fix's error: an IP fix is tens of km wide and would
        // zoom the map out to a region. Never fit wider than ~2.5 km around us.
        var maxM = Math.max(80, src.source === "ip" ? 80 : Math.min(src.accuracy, 2500))
        var aps = src.aps || []
        for (var i = 0; i < aps.length; i++) {
            var a = aps[i]
            // A ring is a range guessed from signal strength: below -80 dBm that guess swings by hundreds of
            // metres from one scan to the next (and weak beacons come and go), so only strong or used rings count.
            if (a.kind === "ring") { if (a.status === "used" || a.dbm >= -80) maxM = Math.max(maxM, a.r) }
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
        _fitAt = {lat: src.lat, lon: src.lon}; _fitSide = Math.min(width, height)
    }
    // A resize re-fits only when the map really changed size (a popup opening, the widget resized by a quarter
    // or more), not when a line of text above it comes and goes: that re-fit is an instant, unbudgeted zoom.
    function resized() {
        if (!_placed) { scheduleFollow(); return }        // a fix that came while the map was too small: place it now
        if (autoZoom && follow && !inSequence() && Math.abs(Math.min(width, height) - _fitSide) > 0.25 * _fitSide) fit()
    }
    // cinematic camera
    function stopCinema() {
        if (flying) {
            var f = _fly; _fly = null; flyTimer.stop(); flying = false
            if (f && f.kind === "follow") { follow = true; autoZoom = f.az }   // a follow glide cut short is still following
        }
        holdTimer.stop(); holdTimer.then = null; holding = false; caption = ""
        _homeView = null
    }
    // Any hand on the map or one of its controls (a press, not a hover): a running tour / spotlight / glide stops
    // where it is and nothing automatic starts for 3 minutes. Called on press by the pan area, place markers,
    // map buttons (not Cinematic: switching it off flies home by itself), the security chip and panel, the card.
    function userTouched() {
        lastUserInput = Date.now()
        stopCinema()
    }
    function easeInOut(t) { return t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2 }
    function flyTo(lat, lon, z, dur, then, kind) {
        var m = merc(lat, lon), z1 = Math.max(3, Math.min(z, maxZ))
        _fly = {t0: Date.now(), dur: Math.max(200, dur), z0: zoom, z1: z1, x0: cx, y0: cy, x1: m.x, y1: m.y, then: then || null, kind: kind || "", az: autoZoom}
        _flyZooms = Math.abs(z1 - zoom) > 0.01
        flying = true
        if (kind !== "follow") { follow = false; autoZoom = false }    // a follow glide is still following (the button stays lit)
        flyTimer.start()
    }
    function hold(ms, then) { holding = true; holdTimer.then = then || null; holdTimer.interval = ms; holdTimer.restart() }
    function beginSequence() { if (!_homeView) _homeView = {z: zoom, auto: autoZoom} }
    // Back to the fix at the zoom the sequence started from (not a fresh fit: that would be a re-zoom of
    // its own, outside the follow hysteresis). Following again, auto-zoom as it was.
    function homeView(dur, then) {
        var hv = _homeView || {z: zoom, auto: autoZoom}
        if (!src.valid) { _homeView = null; caption = ""; if (then) then(); return }
        flyTo(src.lat, src.lon, hv.z, dur, function() { map._homeView = null; map.follow = true; map.autoZoom = hv.auto; map.caption = ""; if (then) then() })
    }
    function abortToHome() {                   // cinematic switched off mid-sequence: straight back, no glide
        var hv = _homeView
        stopCinema()
        if (!src.valid) return
        var m = merc(src.lat, src.lon); cx = m.x; cy = m.y
        if (hv) { zoom = zoomTarget = hv.z; autoZoom = hv.auto }
        follow = true
    }
    function placeParts() {
        var p = (src.place || "").split(",").map(function(x) { return x.trim() }).filter(function(x) { return x })
        return {city: p.length ? p[0] : "", region: p.length > 1 ? p[p.length - 1] : ""}
    }
    function overview(kind) {                 // 0: city · 1: city then state
        if (!src.valid || flying || holding || !mayAutoMove()) return
        var pp = placeParts()
        _autoAt = Date.now(); _fitVote = null
        beginSequence()
        caption = pp.city || src.place || ""
        flyTo(src.lat, src.lon, 10.5, 6500, function() {
            hold(5000, function() {
                if (kind === 1 && pp.region && !map.pointerOver()) {    // someone came to look: straight home instead
                    map.caption = pp.region
                    map.flyTo(map.src.lat, map.src.lon, 7.5, 5000, function() { map.hold(5000, function() { map.homeView(7000) }) })
                } else map.homeView(6500)
            })
        })
    }
    // Which events are worth moving the camera for: a newly placed beacon, a fix or stop that really moved,
    // a linked device that moved well beyond its own error, and a desktop refit that beat the best fit that
    // beacon ever had by 40 % or more. Never routine scan churn (ap_new / ap_lost / ap_up / ap_down), refits
    // the widget synthesised, a stop re-logged in place (a fix-source change), or a Pi agent's link / power note.
    function accOfEvent(e) {
        if (e.acc > 0) return e.acc
        if (e.accuracy > 0) return e.accuracy
        var m = /±\s*(\d+(?:\.\d+)?)\s*m/.exec(e.text || "")
        return m ? Number(m[1]) : (src.accuracy > 0 ? src.accuracy : 0)
    }
    function spotWorthy(e) {
        if (!e || e.lat === undefined || e.lon === undefined || e.synthetic) return false
        switch (e.type) {
        case "ap_placed": return true
        case "fix": case "stop":                 // the desktop only sets from* when the move beat the new fix's accuracy
            if (e.fromLat === undefined || e.fromLon === undefined) return false
            return haversine(e.fromLat, e.fromLon, e.lat, e.lon) > Math.max(50, 2 * accOfEvent(e))
        case "device":                           // agent link / power notes carry no movedM; GNSS jitter is within its acc
            return e.movedM !== undefined && e.movedM > Math.max(100, 2 * (e.acc > 0 ? e.acc : 0))
        case "ap_refit": {
            if (!(e.acc > 0 && e.prevAcc > 0)) return false
            var best = e.bssid ? _bestAcc[e.bssid] : undefined     // a fit that swings ±20 ↔ ±85 m is not news twice
            return e.acc <= 0.6 * (best > 0 ? Math.min(best, e.prevAcc) : e.prevAcc)
        }
        default: return false
        }
    }
    function noteRefit(e) {                   // remember the tightest fit each beacon has had
        if (!e || e.type !== "ap_refit" || e.synthetic || !e.bssid) return
        var lo = Math.min(e.acc > 0 ? e.acc : Infinity, e.prevAcc > 0 ? e.prevAcc : Infinity), b = _bestAcc[e.bssid]
        if (lo < Infinity && !(b > 0 && b <= lo)) _bestAcc[e.bssid] = lo
    }
    property real _spotHold: 3500               // how long a spotlight lingers (long enough for the animations it waited for)
    function spotlight(lat, lon, holdMs) {    // an event: glide in, linger, glide back — rate-limited
        if (!cinematic || !showEvents || spotlightMinutes <= 0 || flying || holding || !mayAutoMove()) return false
        var now = Date.now()
        if (lastSpotlightAt > 0 && now - lastSpotlightAt < spotlightMinutes * 60000) return false
        if (_autoAt > 0 && now - _autoAt < autoGapMs) return false      // a tour or re-fit used the budget
        lastSpotlightAt = now; _autoAt = now; _fitVote = null
        beginSequence()
        _spotHold = holdMs || 3500
        flyTo(lat, lon, Math.min(maxZ, 17), 2800, function() { map.hold(map._spotHold, function() { map.homeView(2800) }) })
        return true
    }
    function focusOn(lat, lon, z) {           // Nearby → "show on map": the user's choice, so it counts as input
        userTouched()
        var m = merc(lat, lon); cx = m.x; cy = m.y
        follow = false; autoZoom = false
        zoom = zoomTarget = Math.max(3, Math.min(maxZoomView, z))
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
        zoomTarget = Math.max(3, Math.min(maxZoomView, zoomTarget + delta))
        zoomAnchor = Qt.point(ax, ay)
    }
    function haversine(la1, lo1, la2, lo2) {
        var R = 6371000, d2r = Math.PI / 180
        var dLa = (la2 - la1) * d2r, dLo = (lo2 - lo1) * d2r
        var a = Math.sin(dLa / 2) * Math.sin(dLa / 2) + Math.cos(la1 * d2r) * Math.cos(la2 * d2r) * Math.sin(dLo / 2) * Math.sin(dLo / 2)
        return 2 * R * Math.asin(Math.sqrt(a))
    }
    function updateRouteSegments() {
        var raw = (src && src.routeFixes && src.routeFixes.length) ? src.routeFixes : ((src && src.track) || [])
        if (!raw || raw.length === 0) {
            routeSegments = []
            return
        }
        var segments = []
        var curPts = []
        var minMx = 1, maxMx = 0, minMy = 1, maxMy = 0
        var lastValid = null
        var lastAdded = null

        for (var i = 0; i < raw.length; i++) {
            var pt = raw[i]
            if (!pt || pt.lat === undefined || pt.lon === undefined) continue
            if (pt.lat === 0 && pt.lon === 0) continue
            var pAcc = pt.acc !== undefined ? pt.acc : pt.accuracy      // routeFixes / track carry "acc"
            if (pAcc !== undefined && pAcc > 500) continue

            var t = pt.time ? Date.parse(pt.time) : NaN

            // Lookahead: drop single-point spike jumps (jump > 30m out and back within 18m)
            if (lastValid && i < raw.length - 1) {
                var nextPt = raw[i + 1]
                if (nextPt && nextPt.lat !== undefined && nextPt.lon !== undefined && nextPt.lat !== 0) {
                    var d1 = haversine(lastValid.lat, lastValid.lon, pt.lat, pt.lon)
                    var d2 = haversine(pt.lat, pt.lon, nextPt.lat, nextPt.lon)
                    var dChord = haversine(lastValid.lat, lastValid.lon, nextPt.lat, nextPt.lon)
                    if (d1 > 30 && d2 > 30 && dChord < 18) {
                        continue
                    }
                }
            }

            if (lastValid) {
                var dtSec = (!isNaN(t) && !isNaN(lastValid.t)) ? Math.abs((t - lastValid.t) / 1000) : 0
                var dist = haversine(lastValid.lat, lastValid.lon, pt.lat, pt.lon)

                // Skip false Wi-Fi / teleportation spikes (>35 m/s or ~78 mph within 300s)
                if (dtSec > 0 && dtSec < 300) {
                    var speed = dist / dtSec
                    if (speed > 35.0 && dist > 200) continue
                }

                // Break discontinuous trips (>15 min gap or >4000m distance gap)
                if (dtSec > 900 || dist > 4000) {
                    if (curPts.length > 1) {
                        segments.push({ minMx: minMx, maxMx: maxMx, minMy: minMy, maxMy: maxMy, pts: curPts })
                    }
                    curPts = []
                    minMx = 1; maxMx = 0; minMy = 1; maxMy = 0
                    lastAdded = null
                } else if (lastAdded) {
                    // Compress stationary points within 20 meters (or tagged phone-stationary)
                    var isStationary = (pt.source === "phone-stationary")
                    var dFromLast = haversine(lastAdded.lat, lastAdded.lon, pt.lat, pt.lon)
                    if ((dFromLast < 20 || (isStationary && dFromLast < 35)) && i < raw.length - 1) {
                        lastValid = {lat: pt.lat, lon: pt.lon, t: t}
                        continue
                    }
                }
            }

            // merc() inlined: a Qt.point per fix is the slow part of this loop in V4
            var pr = Math.max(-85.0511, Math.min(85.0511, pt.lat)) * Math.PI / 180
            var pmx = (pt.lon + 180) / 360, pmy = (1 - Math.log(Math.tan(pr) + 1 / Math.cos(pr)) / Math.PI) / 2
            if (pmx < minMx) minMx = pmx
            if (pmx > maxMx) maxMx = pmx
            if (pmy < minMy) minMy = pmy
            if (pmy > maxMy) maxMy = pmy

            curPts.push({mx: pmx, my: pmy})
            lastValid = {lat: pt.lat, lon: pt.lon, t: t}
            lastAdded = {lat: pt.lat, lon: pt.lon}
        }

        if (curPts.length > 1) {
            segments.push({ minMx: minMx, maxMx: maxMx, minMy: minMy, maxMy: maxMy, pts: curPts })
        }
        routeSegments = segments
    }
    // ── cameras: projected once per list, bucketed per grid level (cells of 2^-L of the world) ──
    readonly property var camLevels: [6, 8, 10, 12]
    readonly property var _compassDeg: ({N: 0, NB: 0, NE: 45, E: 90, EB: 90, SE: 135, S: 180, SB: 180, SW: 225, W: 270, WB: 270, NW: 315})
    // "121", "NB", "SW", "338-23" → degrees clockwise from north, else NaN (the dashed ring). Whole-string
    // numbers only: parseFloat read "0-360" as 0 and "338-23" as 338. A range is its circular midpoint
    // unless it spans more than half the compass (near-omni); a ";" list (several heads) has no one heading.
    function camHeading(dir, named) {
        var d = String(dir || "").trim().toUpperCase()
        if (!d) return NaN
        if (/^-?\d+(\.\d+)?$/.test(d)) return ((parseFloat(d) % 360) + 360) % 360
        var r = /^(-?\d+(?:\.\d+)?)\s*-\s*(-?\d+(?:\.\d+)?)$/.exec(d)
        if (r) {
            var a = parseFloat(r[1]), b = parseFloat(r[2])
            if (Math.abs(b - a) >= 360) return NaN
            var span = ((b - a) % 360 + 360) % 360
            return span > 180 ? NaN : ((a + span / 2) % 360 + 360) % 360
        }
        return named[d] !== undefined ? named[d] : NaN
    }
    // One pass, the projection inlined: merc() returns a Qt.point, and 136k of those took 1.7 s in V4
    // (inlined: ~0.1 s). Integer cell keys in plain objects (a Map was 20x slower in V4).
    function updateCameraIndex() {
        var cams = (src && src.flockCameras) || []
        if (!cams.length) { camIndex = null; return }
        var levels = camLevels, nL = levels.length, lv = {}, gs = [], L, i
        for (L = 0; L < nL; L++) { var g0 = {n: 1 << levels[L], cells: {}, keys: []}; lv[levels[L]] = g0; gs.push(g0) }
        var d2r = Math.PI / 180, count = 0
        for (i = 0; i < cams.length; i++) {
            var c = cams[i]
            if (!c) continue
            var la = c.lat, lo = c.lon
            if (la === undefined || lo === undefined || (la === 0 && lo === 0)) continue
            var r = Math.max(-85.0511, Math.min(85.0511, la)) * d2r
            var mx = (lo + 180) / 360, my = (1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2
            mx = mx - Math.floor(mx)
            var passes = c.passCount > 0 ? c.passCount : 0, vetted = !!c.vetted
            // rank: 2 passed (red) > 1 vetted (cyan) > 0 other (orange): the colour a cell's dot takes
            var it = {mx: mx, my: my, cam: c, heading: undefined, passes: passes, vetted: vetted, rank: passes > 0 ? 2 : vetted ? 1 : 0}
            count++
            for (L = 0; L < nL; L++) {
                var g = gs[L], n = g.n, gx = Math.min(n - 1, Math.floor(mx * n)), gy = Math.max(0, Math.min(n - 1, Math.floor(my * n)))
                var key = gx * n + gy, cell = g.cells[key]
                if (!cell) { cell = g.cells[key] = {gx: gx, gy: gy, rep: it, items: L === nL - 1 ? [] : null}; g.keys.push(key) }
                else if (it.rank > cell.rep.rank) cell.rep = it
                if (cell.items) cell.items.push(it)
            }
        }
        camIndex = count ? {levels: lv, count: count} : null
    }
    // The cells of one level that intersect the mercator box [x0, x1] × [y0, y1] (x may run past 0 or 1: wraps)
    function camCells(L, x0, x1, y0, y1) {
        var g = camIndex.levels[L], n = g.n, out = []
        var cx0 = Math.floor(x0 * n), cx1 = Math.floor(x1 * n), cy0 = Math.max(0, Math.floor(y0 * n)), cy1 = Math.min(n - 1, Math.floor(y1 * n))
        if (cx1 - cx0 >= n) { cx0 = 0; cx1 = n - 1 }
        if ((cx1 - cx0 + 1) * (cy1 - cy0 + 1) > g.keys.length) {      // a wide view: walk the occupied cells instead
            for (var k = 0; k < g.keys.length; k++) {
                var c = g.cells[g.keys[k]]
                if (c.gy < cy0 || c.gy > cy1) continue
                var inX = false
                for (var w = -1; w <= 1 && !inX; w++) if (c.gx + w * n >= cx0 && c.gx + w * n <= cx1) inX = true
                if (inX) out.push(c)
            }
            return out
        }
        for (var x = cx0; x <= cx1; x++) {
            var wx = ((x % n) + n) % n
            for (var y = cy0; y <= cy1; y++) { var cc = g.cells[wx * n + y]; if (cc) out.push(cc) }
        }
        return out
    }
    function bandOf(ap) { return ap.band ? ap.band : ap.freq >= 5925 ? "6" : ap.freq >= 4900 ? "5" : ap.freq > 0 ? "2.4" : "" }

    // ── graded estimates (desktop 3.9+): grade A–F for a fix, R region only, M mobile; absent: ungraded ──
    // Okabe–Ito (colour-blind safe). Looked up per beacon on every paint: a constant table, no allocation.
    readonly property var gradeColors: ({A: "#009E73", B: "#56B4E9", C: "#F0E442", D: "#E69F00", E: "#D55E00", F: "#CC79A7", R: "#8A93A6", M: "#0072B2"})
    readonly property var gradeWords: [["A", "excellent"], ["B", "good"], ["C", "fair"], ["D", "weak"], ["E", "poor"], ["F", "unreliable"], ["R", "region only"], ["M", "mobile"]]
    property bool showLegend: false
    function gradeOf(a) {
        if (!a) return ""
        var g = a.grade || (a.fit && a.fit.grade) || ""
        if (!g) { var k = a.fit && a.fit.kind; g = a.kind === "region" || k === "region" ? "R" : a.kind === "mobile" || k === "mobile" ? "M" : "" }
        return g
    }
    function gradeColor(g) { return gradeColors[g] || "#e6edf7" }
    function plural(n, w) { return n + " " + w + (n === 1 ? "" : "s") }
    function hasFlag(f, name) { return !!(f && f.flags && f.flags.indexOf(name) >= 0) }
    // "B · 72% within 25 m · 9 places · 3 devices" / "R · region ±140 m · 4 places" / "M · travels with you"
    function gradeLine(a) {
        var g = gradeOf(a), f = (a && a.fit) || {}
        if (!g) return ""
        if (g === "M") return "M · travels with you"
        if (g === "R") return `R · region ±${Math.round(a.r95 || f.r95 || a.r || 0)} m · ${plural(f.vantage || 0, "place")}`
        var parts = [g]
        if (f.pWithin25 !== undefined) parts.push(Math.round(f.pWithin25 * 100) + "% within 25 m")
        if (f.vantage !== undefined) parts.push(plural(f.vantage, "place"))
        if (f.devices !== undefined) parts.push(plural(f.devices, "device"))
        return parts.join(" · ")
    }
    // The "sample here next" target of the hovered (else the pinned) beacon, drawn by the fx layer
    property var sugTarget: null                // {lat, lon, slat, slon, col} or null
    function updateSuggest() {
        var aps = src.aps || [], h = hover, a = null
        if (h && h.kind === "beacon" && h.ids.length === 1) a = aps[h.ids[0]] || null
        if (!(a && a.fit && a.fit.suggest) && selectedBeacon)
            for (var i = 0; i < aps.length; i++) if (aps[i] && aps[i].bssid === selectedBeacon) { a = aps[i]; break }
        var s = a && a.fit && a.fit.suggest && a.lat !== undefined && a.fit.suggest.lat !== undefined ? a.fit.suggest : null
        var o = sugTarget
        if (!s) { if (o) { sugTarget = null; fx.requestPaint() } return }
        if (o && o.lat === a.lat && o.lon === a.lon && o.slat === s.lat && o.slon === s.lon) return
        sugTarget = {lat: a.lat, lon: a.lon, slat: s.lat, slon: s.lon, col: gradeColor(gradeOf(a))}
        fx.requestPaint()
    }
    onHoverChanged: updateSuggest()
    onSelectedBeaconChanged: updateSuggest()
    function project(g) {                       // stored geometry → screen point (rings re-anchor on the fix they were heard from)
        var m = merc(g.lat, g.lon)
        if (g.kind === "ring") { var ang = (g.bearing - 90) * Math.PI / 180, rr = g.r / mpp(); return Qt.point(sx(m.x) + Math.cos(ang) * rr, sy(m.y) + Math.sin(ang) * rr) }
        return Qt.point(sx(m.x), sy(m.y))
    }
    function progress(a) { return Math.max(0, Math.min(1, (animNow - a.t0) / a.dur)) }
    function easeOut(p) { return 1 - Math.pow(1 - p, 3) }
    property var lastRefit: null
    // When the event carries no vantage points (older desktops, or the widget synthesised it),
    // use our own position plus recent track points near the beacon.
    function refitFallbackPoints(lat, lon) {
        var pts = [], tr = src.track || [], ld = map.linked
        if (src.valid) pts.push({lat: src.lat, lon: src.lon, device: "me"})
        for (var li = 0; li < ld.length && pts.length < 4; li++) if (ld[li] && ld[li].lat !== undefined && haversine(lat, lon, ld[li].lat, ld[li].lon) < 1500) pts.push({lat: ld[li].lat, lon: ld[li].lon, device: ld[li].device, kind: ld[li].kind})
        for (var i = tr.length - 1; i >= 0 && pts.length < 5; i--)
            if (tr[i].source !== "ip" && haversine(lat, lon, tr[i].lat, tr[i].lon) < 600 && (!pts.length || haversine(pts[0].lat, pts[0].lon, tr[i].lat, tr[i].lon) > 25)) pts.push({lat: tr[i].lat, lon: tr[i].lon})
        return pts
    }
    function replayRefit() {
        if (!lastRefit) return
        var a = JSON.parse(JSON.stringify(lastRefit)); a.t0 = Date.now()
        var l = anims.slice(); l.push(a); anims = l; animCount = l.length; animNow = a.t0; fx.requestPaint()
    }
    function bounce(p) {
        var n1 = 7.5625, d1 = 2.75
        if (p < 1 / d1) return n1 * p * p
        if (p < 2 / d1) { p -= 1.5 / d1; return n1 * p * p + 0.75 }
        if (p < 2.5 / d1) { p -= 2.25 / d1; return n1 * p * p + 0.9375 }
        p -= 2.625 / d1; return n1 * p * p + 0.984375
    }
    function animFor(type, bssid) { for (var i = 0; i < anims.length; i++) if (anims[i].type === type && anims[i].bssid === bssid) return anims[i]; return null }
    function evGlyph(t) { return ({ap_new: "📡", ap_lost: "💨", ap_up: "▲", ap_down: "▼", ap_placed: "💎", ap_refit: "🎯", fix: "◎", stop: "🚩", achievement: "🏆", region: "🗺", prefetch: "💾", error: "⚠"})[t] || "•" }
    function evColor(t) { return ({ap_new: "#35d6ff", ap_lost: "#8a93a6", ap_up: "#6cff8a", ap_down: "#ff9f43", ap_placed: "#ffd166", ap_refit: "#7cf2c4", fix: "#35d6ff", stop: "#ff4f4f", achievement: "#ffd166", region: "#c9a0ff", prefetch: "#9fb0c8", error: "#ff4f4f"})[t] || "#e6edf7" }
    function evText(e) {
        if (e.text) return e.text
        var n = e.ssid || (e.bssid ? e.bssid : "")
        switch (e.type) {
        case "ap_new": return "Heard " + (n || "a beacon")
        case "ap_lost": return "Lost " + (n || "a beacon")
        case "ap_up": return (n || "beacon") + " +" + (e.delta || 0) + " dB"
        case "ap_down": return (n || "beacon") + " " + (e.delta || 0) + " dB"
        case "ap_placed": return "Placed " + (n || "a beacon") + " on the map"
        case "ap_refit": return "Refined " + (n || "a beacon") + (e.prevAcc && e.acc ? `: ±${Math.round(e.prevAcc)} → ±${Math.round(e.acc)} m` : "") + (e.n ? ` (${e.n} samples)` : "")
        case "fix": return "New fix"
        case "stop": return "New stop"
        default: return e.type || "event"
        }
    }
    function ageText(s) { return s < 5 ? "now" : s < 60 ? Math.floor(s) + " s" : s < 3600 ? Math.floor(s / 60) + " min" : s < 172800 ? Math.floor(s / 3600) + " h" : Math.floor(s / 86400) + " d" }
    function onEvents(list, animate) {
        var t = Date.now(), pushed = 0, spot = null
        if (animate) for (var s = 0; s < list.length; s++) if (spotWorthy(list[s])) spot = list[s]   // the newest significant one
        for (var r = 0; r < list.length; r++) noteRefit(list[r])
        if (!animate) { var hist = src.events || []; for (var hi = 0; hi < hist.length; hi++) noteRefit(hist[hi]) }   // first read: learn the history
        var spotted = spot ? spotlight(spot.lat, spot.lon, 3500) : false
        // While the camera flies (the glide that just started, a tour leg, a follow glide) the animations wait
        // for it to land: they then play in a still view, instead of repainting the fx layer every flight frame.
        var start = flying && _fly ? Math.max(t, _fly.t0 + _fly.dur + 150) : t
        // animClock (the only other pruner) stops off screen, e.g. a collapsed popup: drop what has played,
        // and queue nothing new there, or a panel left shut for hours piles up every poll's events
        if (anims.length) { animNow = t; anims = anims.filter(x => progress(x) < 1) }
        for (var i = 0; i < list.length; i++) {
            var e = list[i] || {}, type = e.type || ""
            var when = e.time ? (Date.parse(e.time) || t) : t
            tickerModel.insert(0, {glyph: evGlyph(type), line: evText(e), t: when, col: evColor(type)})
            while (tickerModel.count > 5) tickerModel.remove(tickerModel.count - 1)
            if (!animate || !showEvents) continue
            var a = {type: type, bssid: e.bssid || "", ssid: e.ssid || "", t0: start + pushed * 120, dur: 2500, lat: e.lat, lon: e.lon, delta: e.delta || 0}
            switch (type) {
            case "ap_new": a.dur = 2500; break
            case "ap_lost":
                a.dur = 2000
                a.ghost = _lastAp[a.bssid] || ((e.lat !== undefined && e.lon !== undefined) ? {kind: "pt", lat: e.lat, lon: e.lon, ssid: e.ssid, col: "#8a93a6"} : null)
                if (!a.ghost) continue
                break
            case "ap_up": case "ap_down": a.dur = 3000; break
            case "ap_placed": a.dur = 1500; var g = _lastAp[a.bssid]; a.fromPos = (g && g.kind === "ring") ? g : null; break
            case "ap_refit":
                a.dur = 3200
                a.from = (e.fromLat !== undefined && e.fromLon !== undefined) ? {lat: e.fromLat, lon: e.fromLon} : null
                a.acc = e.acc || 30; a.prevAcc = e.prevAcc || a.acc * 3; a.n = e.n || 0
                a.points = (e.vantagePoints && e.vantagePoints.length) ? e.vantagePoints.slice(0, 6) : refitFallbackPoints(e.lat, e.lon)
                lastRefit = a
                break
            case "fix": a.dur = 1800; a.from = (e.fromLat !== undefined && e.fromLon !== undefined) ? {lat: e.fromLat, lon: e.fromLon} : null; break
            case "stop": a.dur = 4200; break
            case "achievement": case "region": case "prefetch": case "error":
                toastModel.append({glyph: evGlyph(type), line: evText(e), col: evColor(type), key: ++_toastKey})
                while (toastModel.count > 3) toastModel.remove(0)
                continue
            default: continue
            }
            if (!onScreen) continue
            anims.push(a); pushed++
        }
        if (spotted && pushed) {                // linger until the batch has played (at most 6 s), then glide back
            var landAt = _fly.t0 + _fly.dur, endAt = landAt
            for (var k = anims.length - pushed; k < anims.length; k++) endAt = Math.max(endAt, anims[k].t0 + anims[k].dur)
            _spotHold = Math.min(6000, Math.max(3500, endAt - landAt + 200))
        }
        animCount = anims.length
        animNow = t
        if (fxActive()) fx.requestPaint()
    }
    function fxActive() {                       // anything for the fx layer to draw now (animations waiting for a glide do not count)
        return !!(selectedBeacon || sugTarget) || fxAnimating()
    }
    function fxAnimating() {
        var now = Date.now()
        for (var i = 0; i < anims.length; i++) if (anims[i].t0 <= now) return true
        return false
    }
    // Mid-gesture the fx layer drops its static marks (selection ring, "sample here") rather than re-render each
    // frame to follow the camera; they come back with the settle paint
    onLiteChanged: if (selectedBeacon || sugTarget) fx.requestPaint()
    Timer {                                     // one clock for every event animation; idle otherwise
        id: animClock                           // (a selected beacon's ring is static: pulsing it repainted the whole fx
        interval: 33; repeat: true              // canvas 30×/s for as long as anything was selected — on the desktop, forever)
        running: map.animCount > 0 && map.onScreen
        onTriggered: {
            map.animNow = Date.now()
            var changed = false
            if (map.anims.length) {
                var keep = []
                for (var i = 0; i < map.anims.length; i++) if (map.progress(map.anims[i]) < 1) keep.push(map.anims[i])
                if (keep.length !== map.anims.length) { map.anims = keep; map.animCount = keep.length; changed = true }
            }
            if (changed || map.fxActive()) fx.requestPaint()
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

    FrameAnimation {                            // cinematic flight: one step per rendered frame, only while flying;
        id: flyTimer                            // the view pipeline moves the layers (the overlay is only scaled)
        running: false
        onTriggered: {
            var f = map._fly
            if (!f) { stop(); map.flying = false; return }
            var p = Math.min(1, (Date.now() - f.t0) / f.dur), e = map.easeInOut(p)
            var dx = f.x1 - f.x0; if (dx > 0.5) dx -= 1; else if (dx < -0.5) dx += 1
            var nx = f.x0 + dx * e; map.cx = nx - Math.floor(nx)
            map.cy = f.y0 + (f.y1 - f.y0) * e
            map.zoom = map.zoomTarget = f.z0 + (f.z1 - f.z0) * e
            if (p >= 1) { var then = f.then; map._fly = null; stop(); map.flying = false; if (then) then() }
        }
    }
    Timer { id: holdTimer; property var then: null; onTriggered: { map.holding = false; var t = then; then = null; if (t) t() } }
    Timer {                                     // tour scheduler: an overview every tourMinutes, alternating city / city+state
        id: tourTimer                           // (not while anyone is at the widget, nor within 3 min of input or autoGapMs of another automatic zoom)
        interval: 5000; repeat: true; running: map.cinematic && map.onScreen && map.tourMinutes > 0
        property real nextAt: 0
        property int  variant: 0
        onRunningChanged: nextAt = 0
        onTriggered: {
            var now = Date.now()
            if (nextAt === 0) { nextAt = now + map.tourMinutes * 60000; return }
            if (now < nextAt || map.flying || map.holding || !map.mayAutoMove()) return
            if (now - map._autoAt < map.autoGapMs) return        // the shared automatic-zoom budget
            nextAt = now + map.tourMinutes * 60000
            map.overview(variant++ % 2)
        }
    }
    FrameAnimation {                            // eased, cursor-anchored zoom: stepped once per rendered frame (vsync), the
        running: !map.flying && Math.abs(map.zoomTarget - map.zoom) > 0.002   // easing scaled by the frame time
        onTriggered: {                          // (0.22 of the gap per 60 Hz frame, whatever the refresh rate)
            var d = map.zoomTarget - map.zoom, k = 1 - Math.pow(0.78, Math.min(4, frameTime * 60))
            map.applyZoom(Math.abs(d) < 0.006 ? map.zoomTarget : map.zoom + d * k, map.zoomAnchor.x, map.zoomAnchor.y)
        }
    }

    // ── tiles ───────────────────────────────────────────────────────────────
    function tileUrl(z, x, y, labels) {
        // OSM blocks QML's generic User-Agent, so OSM-based layers come through the tray (tileBase, from
        // `beaconfix --json`). Without the tray (not running, or an old desktop) the widget falls back to Esri's
        // keyless raster services directly, which accept any client: the same imagery and labels the tray
        // proxies, and Esri's own street / topographic / dark-grey basemaps in place of OSM and OpenTopoMap.
        // (Esri keeps those three raster basemaps in mature support, no updates, until their retirement,
        // announced for December 2029; World_Imagery and the reference labels are current.)
        if (tileBase) return `${tileBase}/t/${labels ? labels : layerIndex === 2 ? ["2", "C", "U", "V"][satSource] || "2" : layerIndex}/${z}/${x}/${y}.png`
        if (labels === "R") return `https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Transportation/MapServer/tile/${z}/${y}/${x}`
        if (labels === "K") {                   // USGS contours: a dynamic service, asked for the tile's Web-Mercator box
            var R = 20037508.342789244, w = 2 * R / Math.pow(2, z)
            return `https://carto.nationalmap.gov/arcgis/rest/services/contours/MapServer/export?bbox=${(-R + x * w).toFixed(2)},${(R - (y + 1) * w).toFixed(2)},${(-R + (x + 1) * w).toFixed(2)},${(R - y * w).toFixed(2)}&bboxSR=3857&imageSR=3857&size=256,256&format=png32&transparent=true&f=image`
        }
        if (labels) return `https://server.arcgisonline.com/ArcGIS/rest/services/Reference/World_Boundaries_and_Places/MapServer/tile/${z}/${y}/${x}`
        if (layerIndex === 2 && satSource === 1) return `https://clarity.maptiles.arcgis.com/arcgis/rest/services/World_Imagery/MapServer/tile/${z}/${y}/${x}`
        if (layerIndex === 2 && satSource === 2) return `https://basemap.nationalmap.gov/arcgis/rest/services/USGSImageryOnly/MapServer/tile/${z}/${y}/${x}`
        if (layerIndex === 2 && satSource === 3) return `https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/VIIRS_NOAA20_CorrectedReflectance_TrueColor/default/${viirsDate()}/GoogleMapsCompatible_Level9/${z}/${y}/${x}.jpg`
        if (layerIndex === 2) return `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/${z}/${y}/${x}`
        if (layerIndex === 3) return `https://server.arcgisonline.com/ArcGIS/rest/services/World_Topo_Map/MapServer/tile/${z}/${y}/${x}`
        if (layerIndex === 1) return `https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/${z}/${y}/${x}`
        return `https://server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Dark_Gray_Base/MapServer/tile/${z}/${y}/${x}`
    }

    function viirsDate() { return new Date(Date.now() - 86400000).toISOString().slice(0, 10) }   // yesterday (UTC): today's pass is incomplete
    function heatUrl(z, x, y) { return `${tileBase}/h/${src.heatGen}/${z}/${x}/${y}.png` }

    component TileLayer: Item {
        id: tl
        property int  zOffset: 0
        property bool labels: false
        property string ovKey: labels ? "L" : ""   // satellite hybrid overlays: L labels, R roads, K contours
        property bool heat: false
        property string rangeKey: ""
        anchors.fill: parent
        function refresh() {
            if (map.width <= 0 || map.height <= 0) return
            if ((ovKey && map.layerIndex !== 2) || (ovKey === "K" && !map.showContours)
                || (heat && !(map.showHeatmap && map.heatTiles))) { if (tiles.count) tiles.clear(); rangeKey = ""; return }   // hidden: load nothing
            // heat tiles are drawn for any zoom (sharp to 22); the overlays exist to 19; imagery to its source's depth
            var z = Math.max(1, Math.min(heat || ovKey === "K" ? 22 : ovKey ? 19 : map.maxZ, Math.round(map.zoom)) - zOffset)
            var n = 1 << z
            var a = map.toMerc(0, 0), b = map.toMerc(map.width, map.height)
            var x0 = Math.floor(a.x * n), x1 = Math.floor(b.x * n)
            var y0 = Math.max(0, Math.floor(a.y * n)), y1 = Math.min(n - 1, Math.floor(b.y * n))
            var lk = heat ? "h" + map.src.heatGen : ovKey ? ovKey + "," + map.tileBase : map.layerIndex + "," + map.satSource + "," + map.tileBase
            var key = [lk, z, x0, x1, y0, y1].join(",")
            if (key === rangeKey) return
            rangeKey = key
            var need = {}, have = {}                // `have` instead of deleting from `need` (see Radar.qml's flashes)
            for (var x = x0; x <= x1; x++)
                for (var y = y0; y <= y1; y++) need[z + "/" + x + "/" + y] = [x, y]
            // Tiles of another zoom level stay (marked stale, underneath) until every tile of
            // the new level has loaded, so a zoom step no longer flashes to the coarse layer.
            for (var i = tiles.count - 1; i >= 0; i--) {
                var t = tiles.get(i)
                if (t.lk !== lk) { if (heat) tiles.setProperty(i, "stale", true); else tiles.remove(i); continue }   // a new route: the old heat stays until the new loads
                // a tile of this level marked stale by a zoom step that was undone is current again: prune() would
                // otherwise drop it, and with the same view refresh() stops early, so the map stayed blank
                if (t.tz === z) { if (need[t.k] !== undefined) { have[t.k] = true; if (t.stale) tiles.setProperty(i, "stale", false) } else tiles.remove(i) }
                else {                          // other level: keep only while it still covers the view
                    var on = Math.pow(2, t.tz), ax = map.toMerc(0, 0), bx = map.toMerc(map.width, map.height)
                    var vis = (t.tx + 1) / on > ax.x && t.tx / on < bx.x && (t.ty + 1) / on > ax.y && t.ty / on < bx.y
                    if (!vis) tiles.remove(i); else tiles.setProperty(i, "stale", true)
                }
            }
            for (var k in need) {
                if (have[k]) continue
                var tx = need[k][0], ty = need[k][1]
                tiles.append({k: k, lk: lk, tz: z, tx: tx, ty: ty, stale: false, ready: false,
                              url: heat ? map.heatUrl(z, ((tx % n) + n) % n, ty) : map.tileUrl(z, ((tx % n) + n) % n, ty, ovKey)})
            }
            tl.currentZ = z
            prune()
        }
        property int currentZ: 0
        function prune() {                      // drop stale tiles once the current level is fully loaded
            var allReady = true
            for (var i = 0; i < tiles.count; i++) { var t = tiles.get(i); if (t.tz === tl.currentZ && !t.ready) { allReady = false; break } }
            if (!allReady) return
            for (var j = tiles.count - 1; j >= 0; j--) if (tiles.get(j).stale) tiles.remove(j)
        }
        ListModel { id: tiles }
        Repeater {
            model: tiles
            delegate: Image {
                required property int index
                required property int tz
                required property int tx
                required property int ty
                required property bool stale
                required property string url
                z: stale ? 0 : 1
                onStatusChanged: if (status === Image.Ready || status === Image.Error) { tiles.setProperty(index, "ready", true); tl.prune() }
                readonly property real n: Math.pow(2, tz)
                // laid out for the reference camera; the layer's transform follows the live one
                readonly property real ox: map.width / 2 + (tx / n - map.iref.cx) * map.iref.ws
                readonly property real oy: map.height / 2 + (ty / n - map.iref.cy) * map.iref.ws
                x: Math.floor(ox); y: Math.floor(oy)
                width: Math.ceil(ox + map.iref.ws / n) - x; height: Math.ceil(oy + map.iref.ws / n) - y
                source: url
                asynchronous: true; cache: true; smooth: true
                fillMode: Image.Stretch
            }
        }
    }
    function refreshTiles() { bgTiles.refresh(); fgTiles.refresh(); contourTiles.refresh(); roadTiles.refresh(); labelTiles.refresh(); heatLayer.refresh() }
    onCxChanged: viewChanged(false)
    onCyChanged: viewChanged(false)
    onZoomChanged: viewChanged(true)
    onWidthChanged: { resized(); refreshTiles(); viewChanged(false); overlay.requestPaint() }
    onHeightChanged: { resized(); refreshTiles(); viewChanged(false); overlay.requestPaint() }
    // A coarse source (NASA VIIRS stops at 9) pulls the view out to where it still means something
    onLayerIndexChanged: { if (maxZ < 15 && zoomTarget > maxZ + 3) zoom = zoomTarget = maxZ + 3; refreshTiles() }
    onSatSourceChanged: { if (maxZ < 15 && zoomTarget > maxZ + 3) zoom = zoomTarget = maxZ + 3; refreshTiles() }
    onShowContoursChanged: contourTiles.refresh()
    onHeatTilesChanged: { heatLayer.refresh(); overlay.requestPaint() }
    onShowHeatmapChanged: heatLayer.refresh()
    Connections { target: map.src; function onHeatGenChanged() { heatLayer.refresh() } }
    onTileBaseChanged: refreshTiles()
    onHiddenCatsChanged: cluster()

    Rectangle { anchors.fill: parent; color: "#0b101a" }
    Item {
        id: base
        anchors.fill: parent
        transform: itemM
        TileLayer { id: bgTiles; zOffset: 2 }      // coarse layer underneath: no black holes while loading
        TileLayer { id: fgTiles }
    }
    Rectangle { anchors.fill: parent; color: "#000000"; opacity: 0.15; visible: map.layerIndex === 2 }   // overlays and pills read on bright imagery
    TileLayer { id: contourTiles; ovKey: "K"; visible: map.layerIndex === 2 && map.showContours; opacity: 0.7; transform: itemM }
    TileLayer { id: roadTiles; ovKey: "R"; visible: map.layerIndex === 2; transform: itemM }
    TileLayer { id: labelTiles; labels: true; visible: map.layerIndex === 2; transform: itemM }
    TileLayer { id: heatLayer; heat: true; visible: map.showHeatmap && map.heatTiles; opacity: map.heatmapOpacity; transform: itemM }

    // ── overlay: track, accuracy ring, beacons ──────────────────────────────
    // Painted for the camera in `pref` (plus a margin beyond the edges) and carried along by paintM between
    // re-renders. Its text is QML Text (markLayer): Canvas fillText costs 0.6–1 ms a call, a Text item ~0.
    FontMetrics { id: fmName; font.family: "sans-serif"; font.pixelSize: 10 }
    FontMetrics { id: fmItalic; font.family: "sans-serif"; font.pixelSize: 10; font.italic: true }
    FontMetrics { id: fmBadge; font.family: "sans-serif"; font.pixelSize: 8; font.bold: true }
    property var marks: []                      // slots {x, y, t, c, px, b(old), i(talic), o(utline), a(lpha), h(centred), v(baseline y), hid(den)}
    property var umarks: []                     // the same, under the canvas (device / antenna names: pills cover them, as before)
    property int markPool: 0                    // Text items kept alive: grow in steps, never shrink (no delegate churn)
    property int umarkPool: 0
    function setMarks(l, u) {
        var sl = slotMarks(marks, l), su = slotMarks(umarks, u)
        if (sl.length > markPool) markPool = Math.ceil(sl.length / 16) * 16
        if (su.length > umarkPool) umarkPool = Math.ceil(su.length / 8) * 8
        marks = sl; umarks = su
    }
    // Each mark goes back to the Text item that showed the same text last time, else to a free item of the same
    // font, else to an unused one: an item never changes font, and one with nothing to show keeps its text,
    // hidden. A repaint then mostly moves items. Re-shaping a string, and above all switching an item's font
    // (40-50 ms for three badges in the harness), is what made the poll and settle repaints cost 50-90 ms.
    function markKey(m) { return m.t + "\u0001" + markFont(m) }
    function markFont(m) { return (m.px || 0) + (m.b ? "b" : "") + (m.i ? "i" : "") + (m.o ? "o" : "") }
    function slotMarks(prev, l) {
        var out = [], byKey = {}, byFont = {}, rest = [], i, s
        for (i = 0; i < prev.length; i++) { out.push(null); if (prev[i]) { var k = markKey(prev[i]); (byKey[k] || (byKey[k] = [])).push(i) } }
        for (i = 0; i < l.length; i++) { var c = byKey[markKey(l[i])]; if (c && c.length) out[c.shift()] = l[i]; else rest.push(l[i]) }
        for (i = 0; i < prev.length; i++) if (prev[i] && !out[i]) { var f = markFont(prev[i]); (byFont[f] || (byFont[f] = [])).push(i) }
        for (i = 0; i < rest.length; i++) {
            var same = byFont[markFont(rest[i])]
            if (same && same.length) { out[same.shift()] = rest[i]; continue }
            for (s = 0; s < out.length && (out[s] || prev[s]); s++) {}
            if (s < out.length) out[s] = rest[i]; else out.push(rest[i])
        }
        for (i = 0; i < prev.length; i++) if (!out[i] && prev[i]) out[i] = prev[i].hid ? prev[i] : Object.assign({}, prev[i], {hid: 1})
        return out
    }
    component MarkText: Text {
        required property var m
        visible: m !== null && !m.hid
        text: m ? m.t : ""
        color: m ? m.c : "#ffffff"
        opacity: m && m.a !== undefined ? m.a : 1
        font.family: "sans-serif"
        font.pixelSize: m ? m.px : 10
        font.bold: !!(m && m.b)
        font.italic: !!(m && m.i)
        style: m && m.o ? Text.Outline : Text.Normal
        styleColor: "#c0000000"
        x: m ? (m.h ? m.x - implicitWidth / 2 : m.x) : 0
        y: m ? (m.v ? m.y - baselineOffset : m.y - implicitHeight / 2) : 0
    }
    Item {
        id: ovBox
        anchors.fill: parent
        transform: paintM
        Item {                                  // text under the canvas, in the same (painted) coordinates
            anchors.fill: parent
            Repeater {
                model: map.umarkPool
                delegate: MarkText {
                    required property int index
                    m: index < map.umarks.length ? map.umarks[index] : null
                }
            }
        }
        Canvas {
            id: overlay
            x: -map.ovMarginX; y: -map.ovMarginY
            width: map.width + 2 * map.ovMarginX; height: map.height + 2 * map.ovMarginY
            renderStrategy: Canvas.Immediate     // rendered in the frame that resets the transform: no one-frame jump
            onPaint: {
                map.rebasePaint()
                var ctx = getContext("2d")
                ctx.reset()
                var src = map.src, marks = [], umarks = []
                if (!src.valid) { map.setMarks(marks, umarks); return }
                ctx.translate(map.ovMarginX, map.ovMarginY)   // draw in map coordinates
                var lite = map.lite
                var mpp = map.mpp()
                var me = map.merc(src.lat, src.lon), mx = map.sx(me.x), my = map.sy(me.y)
                // route density heatmap (precomputed segments, viewport culled). One path, built once and stroked
                // twice (glow, then core); a point within ~1.5 px of the last one drawn is skipped, so a zoomed-out
                // view of thousands of fixes is a few hundred segments, and nothing is allocated per point.
                if (map.showHeatmap && !map.heatTiles && map.routeSegments.length > 0) {
                    ctx.save()
                    ctx.lineCap = "round"
                    ctx.lineJoin = "round"

                    var m0 = map.toMerc(-map.ovMarginX, -map.ovMarginY)
                    var m1 = map.toMerc(map.width + map.ovMarginX, map.height + map.ovMarginY)
                    var vMinX = Math.min(m0.x, m1.x), vMaxX = Math.max(m0.x, m1.x)
                    var vMinY = Math.min(m0.y, m1.y), vMaxY = Math.max(m0.y, m1.y)

                    // Lines wholly outside the painted area (one side of it, with the glow's half-width to spare) become
                    // moveTo: zoomed in, a segment touching the view still spans kilometres off it, and the stroker
                    // outlined all of that (the settle paint after a zoom took 60-300 ms, nearly all heat-map).
                    var rSegs = map.routeSegments, nVis = 0
                    var cL = -map.ovMarginX - 10, cR = map.width + map.ovMarginX + 10, cT = -map.ovMarginY - 10, cB = map.height + map.ovMarginY + 10
                    ctx.beginPath()
                    for (var rsi = 0; rsi < rSegs.length; rsi++) {
                        var rs = rSegs[rsi], rpts = rs.pts
                        if (rs.maxMx < vMinX || rs.minMx > vMaxX || rs.maxMy < vMinY || rs.minMy > vMaxY) continue
                        if (!rpts || rpts.length < 2) continue
                        var lx = map.sx(rpts[0].mx), ly = map.sy(rpts[0].my), down = false
                        nVis++
                        for (var rpi = 1; rpi < rpts.length; rpi++) {
                            var qx = map.sx(rpts[rpi].mx), qy = map.sy(rpts[rpi].my)
                            if (rpi < rpts.length - 1 && Math.abs(qx - lx) + Math.abs(qy - ly) < 2) continue
                            if ((lx < cL && qx < cL) || (lx > cR && qx > cR) || (ly < cT && qy < cT) || (ly > cB && qy > cB)) down = false
                            else { if (!down) { ctx.moveTo(lx, ly); down = true } ctx.lineTo(qx, qy) }
                            lx = qx; ly = qy
                        }
                    }

                    if (nVis > 0) {
                        // Continuous density heatmap scaled by user-selected heatmapOpacity
                        var hAlpha = Math.max(0.05, Math.min(1.0, map.heatmapOpacity))
                        ctx.strokeStyle = "rgba(53, 214, 255, " + (0.15 * hAlpha).toFixed(3) + ")"
                        ctx.lineWidth = 18; ctx.stroke()
                        ctx.strokeStyle = "rgba(108, 255, 138, " + (0.35 * hAlpha).toFixed(3) + ")"
                        ctx.lineWidth = 9; ctx.stroke()
                        ctx.strokeStyle = "rgba(255, 145, 0, " + (0.50 * hAlpha).toFixed(3) + ")"
                        ctx.lineWidth = 4; ctx.stroke()
                        ctx.strokeStyle = "rgba(255, 255, 255, " + (0.75 * hAlpha).toFixed(3) + ")"
                        ctx.lineWidth = 1.5; ctx.stroke()
                    }
                    ctx.restore()
                }
                // trip track: straight stop-to-stop lines cut across everything, so they only stand in for the route when
                // the heat route is off; the stops themselves go in close-up work (zoom ≥ 17), where they hide beacons
                var tr = map.zoom >= 17 ? [] : (src.track || [])
                for (var i = (map.showHeatmap && map.heatTiles) ? tr.length : 1; i < tr.length; i++) {
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
                    ctx.beginPath(); ctx.arc(map.sx(s.x), map.sy(s.y), 3, 0, Math.PI * 2)
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
                // linked devices: where the phone / laptop / mesh node last reported itself
                if (map.showDevices) {
                    var ld = map.linked
                    for (var di = 0; di < ld.length; di++) {
                        var dv = ld[di]; if (!dv || dv.lat === undefined) continue
                        if (Math.abs(dv.lat) < 0.0001 && Math.abs(dv.lon) < 0.0001) continue
                        var dm = map.merc(dv.lat, dv.lon), dx = map.sx(dm.x), dy = map.sy(dm.y)
                        var dAcc = Math.max(6, (dv.acc || 30) / mpp), stale = (dv.ageS !== undefined && dv.ageS < 0) || (dv.ageS || 0) > 3600
                        var dAlpha = stale ? 0.35 : dv.online ? 1 : 0.7
                        var isNode = (dv.kind === "esp32-node" || dv.kind === "node" || dv.kind === "mesh")
                        var isBase = (dv.role === "base_station" || dv.role === "base")
                        var nodeCol = (isNode && !dv.online) ? "#9fb0c8" : isBase ? "#ffd166" : (isNode ? "#35d6ff" : "#7cf2c4")
                        ctx.globalAlpha = dAlpha
                        var dist = map.haversine(src.lat, src.lon, dv.lat, dv.lon)
                        if (dist < 2000) { ctx.setLineDash([4, 4]); ctx.strokeStyle = isNode ? "rgba(53,214,255,0.4)" : "rgba(124,242,196,0.5)"; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(mx, my); ctx.lineTo(dx, dy); ctx.stroke(); ctx.setLineDash([]) }
                        ctx.beginPath(); ctx.arc(dx, dy, dAcc, 0, Math.PI * 2); ctx.fillStyle = isNode ? "rgba(53,214,255,0.06)" : "rgba(124,242,196,0.08)"; ctx.fill(); ctx.strokeStyle = isNode ? "rgba(53,214,255,0.45)" : "rgba(124,242,196,0.45)"; ctx.setLineDash([3, 3]); ctx.stroke(); ctx.setLineDash([])
                        ctx.beginPath(); ctx.arc(dx, dy, 7, 0, Math.PI * 2); ctx.fillStyle = "#0b101a"; ctx.fill(); ctx.strokeStyle = nodeCol; ctx.lineWidth = 1.5; ctx.stroke()
                        ctx.globalAlpha = 1
                        marks.push({x: dx, y: dy + 0.5, t: map.deviceGlyph(dv.kind, dv.role), c: "#e6edf7", px: 10, h: 1, a: dAlpha})
                        // name, role, battery %, voltage, runtime, and how old
                        if (!stale || (dv.ageS || 0) < 21600) {
                            // A node without a battery still reports 0 % and a floating ~0.3 V: say nothing about battery
                            var hasBatt = !isNode || dv.hasBattery === true
                            var bInfo = (hasBatt && dv.battPct !== undefined && dv.battPct >= 0) ? (" · 🔋" + dv.battPct + "%") : ""
                            if (hasBatt && dv.battMv) bInfo += " · " + dv.battMv + "mV"
                            if (hasBatt && isNode && dv.battMah) bInfo += " · " + dv.battMah + "mAh"
                            if (hasBatt && dv.estRuntimeMins !== undefined && dv.estRuntimeMins > 0) {
                                var rtH = Math.floor(dv.estRuntimeMins / 60)
                                var rtM = dv.estRuntimeMins % 60
                                bInfo += " · " + rtH + "h " + rtM + "m left"
                            }
                            var rInfo = isBase ? " · Base" : ""
                            var dl = (dv.device || dv.identityName || "device") + rInfo + bInfo + (dv.ageS !== undefined && dv.ageS >= 600 ? " · " + map.ageText(dv.ageS) : "")
                            umarks.push({x: dx + 10, y: dy - 9, t: dl, c: nodeCol, px: 10, b: 1, o: 1, a: dAlpha})
                        }
                    }
                }
                // surveyed antenna anchors: crosshair pins (the one being edited is white and draggable)
                var ancs = map.antennas.slice(); if (map.editAnchor && map.editAnchor.isNew) ancs.push(map.editAnchor)
                for (var qa = 0; qa < ancs.length; qa++) {
                    var an2 = (map.editAnchor && ancs[qa].id === map.editAnchor.id) ? map.editAnchor : ancs[qa]
                    var ap2 = map.anchorScreen(an2), editing = !!(map.editAnchor && an2.id === map.editAnchor.id), col2 = map.anchorColor(an2.kind)
                    var accR = (an2.accM || 1) / mpp
                    if (accR > 4) { ctx.beginPath(); ctx.arc(ap2.x, ap2.y, accR, 0, Math.PI * 2); ctx.fillStyle = "rgba(255,255,255,0.06)"; ctx.fill(); ctx.strokeStyle = "rgba(255,255,255,0.35)"; ctx.lineWidth = 1; ctx.setLineDash([2, 3]); ctx.stroke(); ctx.setLineDash([]) }
                    ctx.strokeStyle = "rgba(0,0,0,0.7)"; ctx.lineWidth = editing ? 5 : 4
                    for (var pass2 = 0; pass2 < 2; pass2++) {
                        ctx.beginPath(); ctx.arc(ap2.x, ap2.y, 9, 0, Math.PI * 2)
                        ctx.moveTo(ap2.x - 15, ap2.y); ctx.lineTo(ap2.x - 4, ap2.y); ctx.moveTo(ap2.x + 4, ap2.y); ctx.lineTo(ap2.x + 15, ap2.y)
                        ctx.moveTo(ap2.x, ap2.y - 15); ctx.lineTo(ap2.x, ap2.y - 4); ctx.moveTo(ap2.x, ap2.y + 4); ctx.lineTo(ap2.x, ap2.y + 15)
                        ctx.stroke()
                        ctx.strokeStyle = editing ? "#ffffff" : col2; ctx.lineWidth = editing ? 2.4 : 1.7
                    }
                    ctx.beginPath(); ctx.arc(ap2.x, ap2.y, 2, 0, Math.PI * 2); ctx.fillStyle = editing ? "#ffffff" : col2; ctx.fill()
                    var lab2 = (an2.name || (editing ? "new antenna" : "antenna")) + (an2.rv ? " · RV" : "") + (an2.headingAssumed ? " · re-place?" : "")
                    umarks.push({x: ap2.x + 12, y: ap2.y + 15, t: lab2, c: editing ? "#ffffff" : col2, px: 10, b: 1, o: 1})
                }
                // beacons: RSSI orbits, uncertainty rings, dots (grouped when they coincide)
                var aps = src.aps || [], pts = [], atMe = [], posBy = {}, last = map._lastAp
                // Area grouping: beacons this close on screen share one marker with a count, so a street-level view
                // shows where they are by the block, not a heap of overlapping dots; it splits as you zoom in (one
                // marker per beacon from zoom 19). A click on a group zooms into it.
                var groupPx = map.zoom < 15 ? 44 : map.zoom < 17 ? 30 : map.zoom < 19 ? 18 : 9
                var selIdx = -1
                map.selPos = null
                // "region" beacons (grade R): one faint disc of radius R95 each, underneath everything else. There can
                // be hundreds, so all of them go into one path, filled once and stroked once (overlaps do not stack up).
                var nReg = 0
                ctx.beginPath()
                for (var rk = 0; rk < aps.length; rk++) {
                    var ra = aps[rk]
                    if (!ra || ra.kind !== "region" || ra.lat === undefined) continue
                    var rpx = (ra.r95 || ra.r || 0) / mpp
                    if (rpx < 2 || rpx > 20000) continue
                    var rm = map.merc(ra.lat, ra.lon), rcx = map.sx(rm.x), rcy = map.sy(rm.y)
                    ctx.moveTo(rcx + rpx, rcy); ctx.arc(rcx, rcy, rpx, 0, Math.PI * 2)
                    nReg++
                }
                if (nReg) {
                    if (!lite) { ctx.globalAlpha = map.secFocus ? 0.08 : 0.40; ctx.strokeStyle = map.gradeColors.R; ctx.lineWidth = 0.8; ctx.stroke() }
                    ctx.globalAlpha = 1
                }
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
                        var ur = ap.r / mpp, apFit = ap.fit
                        if (apFit && apFit.kind === "fix" && apFit.semiMajor > 0) {
                            // 95 % error ellipse: 1-σ semi axes × 2.4477 (√χ²₂(0.95)), major axis along `orient`
                            // (bearing, clockwise from north). Screen y points down: north is −y, so the major axis
                            // is (sin θ, −cos θ) and the minor axis (cos θ, sin θ).
                            var ea = Math.min(20000, apFit.semiMajor * 2.4477 / mpp), eb = Math.min(20000, (apFit.semiMinor > 0 ? apFit.semiMinor : apFit.semiMajor) * 2.4477 / mpp)
                            var gcol = map.gradeColor(map.gradeOf(ap))
                            var dashed = map.hasFlag(apFit, "extrapolated") || map.hasFlag(apFit, "ambiguous")
                            if (ea > 3) {
                                var th = (apFit.orient || 0) * Math.PI / 180, ux = Math.sin(th), uy = -Math.cos(th)
                                ctx.beginPath()
                                for (var es = 0; es <= 40; es++) {
                                    var et = es * Math.PI / 20, ca = Math.cos(et) * ea, sb = Math.sin(et) * eb
                                    var ex = px + ca * ux - sb * uy, ey = py + ca * uy + sb * ux
                                    if (es === 0) ctx.moveTo(ex, ey); else ctx.lineTo(ex, ey)
                                }
                                ctx.closePath()
                                var isSel = selIdx >= 0 && k === selIdx
                                if (isSel) { ctx.globalAlpha = 0.30; ctx.fillStyle = gcol; ctx.fill() }
                                ctx.globalAlpha = isSel ? 0.95 : (map.secFocus ? 0.16 : 0.22)
                                ctx.strokeStyle = gcol
                                ctx.lineWidth = isSel ? 2.0 : 0.9
                                if (dashed || !isSel) ctx.setLineDash(isSel ? [5, 4] : [3, 3])
                                ctx.stroke(); ctx.setLineDash([])
                                ctx.globalAlpha = 1
                            }
                            var mt = apFit.metrics
                            if (map.hasFlag(apFit, "ambiguous") && mt && mt.altLat !== undefined && mt.altLon !== undefined) {
                                // the other solution the data allows (mirror ambiguity): a hollow ghost, faintly tied to the estimate
                                var am = map.merc(mt.altLat, mt.altLon), gx2 = map.sx(am.x), gy2 = map.sy(am.y)
                                ctx.globalAlpha = 0.35; ctx.strokeStyle = gcol; ctx.lineWidth = 1; ctx.setLineDash([2, 3])
                                ctx.beginPath(); ctx.moveTo(px, py); ctx.lineTo(gx2, gy2); ctx.stroke()
                                ctx.globalAlpha = 0.8; ctx.setLineDash([2, 2]); ctx.lineWidth = 1.2
                                ctx.beginPath(); ctx.arc(gx2, gy2, 4.5, 0, Math.PI * 2); ctx.stroke(); ctx.setLineDash([])
                                ctx.globalAlpha = 1
                            }
                        } else if (ur > 6 && ap.kind !== "region" && ap.kind !== "mobile") {
                            var isSelCirc = selIdx >= 0 && k === selIdx
                            ctx.beginPath(); ctx.arc(px, py, ur, 0, Math.PI * 2)
                            ctx.strokeStyle = isSelCirc ? "rgba(255,209,102,0.90)" : "rgba(255,209,102,0.18)"
                            ctx.lineWidth = isSelCirc ? 2.0 : 0.9
                            ctx.setLineDash([3, 3]); ctx.stroke(); ctx.setLineDash([])
                        }
                        last[ap.bssid] = {kind: ap.kind, lat: ap.lat, lon: ap.lon, r: ap.r, ssid: ap.ssid, col: col, status: ap.status, dbm: ap.dbm}
                    }
                    // A beacon just placed on the map is drawn where it now is: this canvas is not repainted by the
                    // animation clock, so a glide baked in here would freeze wherever the paint caught it (for a
                    // deferred batch, at the old orbit spot). The fx layer draws the glide in from that spot.
                    posBy[ap.bssid] = {x: px, y: py, col: col}
                    var sc = map.secOf(ap)
                    var merged = false, mob = ap.kind === "mobile"     // a mobile beacon keeps its own "M" marker
                    for (var q = 0; q < pts.length && !mob; q++)
                        if (!pts[q].mob && Math.abs(pts[q].x - px) < groupPx && Math.abs(pts[q].y - py) < groupPx) {
                            pts[q].ids.push(k)
                            if (sc.rank > pts[q].secRank) { pts[q].secRank = sc.rank; pts[q].secCol = sc.color; pts[q].secGlyph = sc.glyph }
                            if (ap.dbm > pts[q].dbm) { pts[q].ssid = ap.ssid; pts[q].dbm = ap.dbm; pts[q].status = ap.status; pts[q].bssid = ap.bssid; pts[q].band = map.bandOf(ap); pts[q].col = col }
                            merged = true; break
                        }
                    if (!merged) pts.push({x: px, y: py, ids: [k], col: col, wigle: ap.kind === "wigle", mob: mob, big: ap.status === "used" || ap.status === "active",
                                           ssid: ap.ssid || "", dbm: ap.dbm, status: ap.status, bssid: ap.bssid, band: map.bandOf(ap),
                                           secRank: sc.rank, secCol: sc.color, secGlyph: sc.glyph})
                }
                for (var p = 0; p < pts.length; p++) {
                    var pt = pts[p], rad = pt.big ? 4.5 : 3
                    var insecure = pt.secRank >= 3
                    if (pt.mob) {                       // mobile (grade M): a small "M" disc where it was last heard
                        if (selIdx >= 0 && pt.ids.indexOf(selIdx) >= 0) map.selPos = {x: pt.x, y: pt.y}
                        ctx.globalAlpha = map.secFocus && !insecure ? 0.25 : 1
                        ctx.beginPath(); ctx.arc(pt.x, pt.y, 6.5, 0, Math.PI * 2); ctx.fillStyle = map.gradeColors.M; ctx.fill()
                        ctx.strokeStyle = "rgba(255,255,255,0.85)"; ctx.lineWidth = 1; ctx.stroke()
                        marks.push({x: pt.x, y: pt.y + 3.5, t: "M", c: "#ffffff", px: 9, b: 1, h: 1, v: 1, a: ctx.globalAlpha})
                        if (insecure) {
                            ctx.beginPath(); ctx.arc(pt.x, pt.y, 10.5, 0, Math.PI * 2)
                            ctx.strokeStyle = pt.secCol; ctx.lineWidth = 1.6; ctx.setLineDash(pt.secRank >= 4 ? [] : [3, 2]); ctx.stroke(); ctx.setLineDash([])
                            marks.push({x: pt.x - 11, y: pt.y - 8, t: pt.secGlyph, c: pt.secCol, px: 10, b: 1, h: 1, v: 1})
                        }
                        ctx.globalAlpha = 1
                        continue
                    }
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
                        marks.push({x: pt.x - 9, y: pt.y - 7, t: pt.secGlyph, c: pt.secCol, px: 10, b: 1, h: 1, v: 1})
                    }
                    ctx.globalAlpha = 1
                    if (pt.ids.length > 1 && groupPx > 9) {   // an area group: a disc sized by its count, the count inside
                        var gr = 8 + 2.5 * Math.log(pt.ids.length) / Math.LN2
                        ctx.beginPath(); ctx.arc(pt.x, pt.y, gr, 0, Math.PI * 2); ctx.fillStyle = "rgba(8,13,20,0.82)"; ctx.fill()
                        ctx.strokeStyle = pt.col; ctx.lineWidth = 2; ctx.stroke()
                        marks.push({x: pt.x, y: pt.y + 3.5, t: String(pt.ids.length), c: "#ffffff", px: 10, b: 1, h: 1, v: 1})
                        pt.area = true
                    } else if (pt.ids.length > 1) {
                        ctx.fillStyle = pt.col; ctx.beginPath(); ctx.arc(pt.x + 8, pt.y - 8, 7, 0, Math.PI * 2); ctx.fill()
                        marks.push({x: pt.x + 8, y: pt.y - 5, t: String(pt.ids.length), c: "#0b101a", px: 9, b: 1, h: 1, v: 1})
                    }
                }

                // surveillance & flock cameras, from the spatial index (camIndex): only the grid cells around the
                // painted area are visited. From zoom 12 each camera in full (view wedge, rings, badge, pass count;
                // its name from 13); zoomed out, a dot per camera, and below zoom 9 a dot per grid cell (~8-30 px)
                // in the colour of the most notable camera in it (passed, else vetted, else other).
                if (map.showCameras && map.camIndex) {
                    var cMinX = -map.ovMarginX - 40, cMaxX = map.width + map.ovMarginX + 40
                    var cMinY = -map.ovMarginY - 40, cMaxY = map.height + map.ovMarginY + 40
                    var cb0 = map.toMerc(cMinX, cMinY), cb1 = map.toMerc(cMaxX, cMaxY)
                    if (map.zoom < 12) {
                        var camDot = ["rgba(255, 145, 0, 0.9)", "rgba(0, 229, 255, 0.9)", "rgba(255, 42, 75, 0.95)"]
                        var perCell = map.zoom < 9
                        var dotCells = map.camCells(!perCell || map.zoom >= 7 ? 12 : map.zoom >= 5 ? 10 : map.zoom >= 3 ? 8 : 6, cb0.x, cb1.x, cb0.y, cb1.y)   // a cell is 256·2^(zoom−L) px: 8–32
                        for (var crk = 0; crk < 3; crk++) {     // one path per colour; the most notable drawn last (on top)
                            var nDots = 0
                            ctx.beginPath()
                            for (var dci = 0; dci < dotCells.length; dci++) {
                                var dcl = dotCells[dci], dlist = perCell ? [dcl.rep] : dcl.items
                                for (var dli = 0; dli < dlist.length; dli++) {
                                    var dit = dlist[dli]
                                    if (dit.rank !== crk) continue
                                    var dcx = map.sx(dit.mx), dcy = map.sy(dit.my)
                                    if (dcx < cMinX || dcx > cMaxX || dcy < cMinY || dcy > cMaxY) continue
                                    ctx.moveTo(dcx + 3, dcy); ctx.arc(dcx, dcy, 3, 0, Math.PI * 2); nDots++
                                }
                            }
                            if (nDots) { ctx.strokeStyle = "rgba(12, 16, 23, 0.85)"; ctx.lineWidth = 1.5; ctx.stroke(); ctx.fillStyle = camDot[crk]; ctx.fill() }
                        }
                    } else {
                        var vcells = map.camCells(12, cb0.x, cb1.x, cb0.y, cb1.y)
                        for (var cci = 0; cci < vcells.length; cci++) {
                            var citems = vcells[cci].items
                            for (var ci = 0; ci < citems.length; ci++) {
                                var cit = citems[ci], cam = cit.cam
                                var camX = map.sx(cit.mx), camY = map.sy(cit.my)
                                if (camX < cMinX || camX > cMaxX || camY < cMinY || camY > cMaxY) continue
                                var isVetted = cit.vetted
                                var passes = cit.passes
                                var isPassed = passes > 0
                                var camColor = isPassed ? "#ff2a4b" : isVetted ? "#00e5ff" : "#ff9100"

                                // Directional FOV wedge on the road surface
                                if (cit.heading === undefined) cit.heading = map.camHeading(cam.direction, map._compassDeg)   // parsed once, when first drawn
                                var heading = cit.heading
                                if (!isNaN(heading)) {
                                    var hRad = (heading - 90) * Math.PI / 180
                                    var spread = 24 * Math.PI / 180
                                    var fovLen = 38
                                    ctx.beginPath()
                                    ctx.moveTo(camX, camY)
                                    ctx.lineTo(camX + fovLen * Math.cos(hRad - spread), camY + fovLen * Math.sin(hRad - spread))
                                    ctx.arc(camX, camY, fovLen, hRad - spread, hRad + spread)
                                    ctx.closePath()
                                    ctx.fillStyle = isPassed ? "rgba(255, 42, 75, 0.35)" : isVetted ? "rgba(0, 229, 255, 0.28)" : "rgba(255, 145, 0, 0.25)"
                                    ctx.fill()
                                    ctx.strokeStyle = camColor
                                    ctx.lineWidth = isPassed ? 2.0 : 1.6
                                    ctx.stroke()
                                } else {
                                    ctx.beginPath()
                                    ctx.arc(camX, camY, 22, 0, Math.PI * 2)
                                    ctx.strokeStyle = isPassed ? "rgba(255, 42, 75, 0.60)" : isVetted ? "rgba(0, 229, 255, 0.45)" : "rgba(255, 145, 0, 0.40)"
                                    ctx.lineWidth = 1.2
                                    ctx.setLineDash([4, 3])
                                    ctx.stroke()
                                    ctx.setLineDash([])
                                }

                                // Outer radar ring
                                ctx.beginPath()
                                ctx.arc(camX, camY, isPassed ? 18 : 14, 0, Math.PI * 2)
                                ctx.fillStyle = isPassed ? "rgba(255, 42, 75, 0.25)" : isVetted ? "rgba(0, 229, 255, 0.20)" : "rgba(255, 145, 0, 0.20)"
                                ctx.fill()
                                ctx.strokeStyle = camColor
                                ctx.lineWidth = isPassed ? 1.5 : 1
                                ctx.stroke()

                                // High-contrast shield badge
                                ctx.beginPath()
                                ctx.arc(camX, camY, 7, 0, Math.PI * 2)
                                ctx.fillStyle = "#0c1017"
                                ctx.fill()
                                ctx.lineWidth = 2.0
                                ctx.strokeStyle = camColor
                                ctx.stroke()

                                // Inner camera lens
                                ctx.beginPath()
                                ctx.arc(camX, camY, 3, 0, Math.PI * 2)
                                ctx.fillStyle = camColor
                                ctx.fill()

                                // Pass count badge top-right of camera icon
                                if (isPassed) {
                                    ctx.beginPath()
                                    ctx.arc(camX + 9, camY - 9, 7.5, 0, Math.PI * 2)
                                    ctx.fillStyle = "#ff1744"
                                    ctx.fill()
                                    ctx.strokeStyle = "#ffffff"
                                    ctx.lineWidth = 1.2
                                    ctx.stroke()
                                    marks.push({
                                        x: camX + 9, y: camY - 6,
                                        t: String(passes),
                                        c: "#ffffff", px: 9, b: 1, h: 1, v: 1
                                    })
                                }

                                if (map.zoom >= 13) {
                                    var camLabel = "📷 " + (cam.model || "Flock")
                                    if (isPassed) camLabel += " · " + passes + (passes === 1 ? " pass" : " passes")
                                    else if (isVetted) camLabel += " [vetted]"
                                    marks.push({
                                        x: camX + 11, y: camY - 5,
                                        t: camLabel,
                                        c: camColor, px: 11, b: true, o: true
                                    })
                                }
                            }
                        }
                    }
                }

                // ── Wi-Fi names: pills beside the beacons, strongest first, no overlaps ──
                // (not while the camera moves: the layout is re-done once it settles)
                // (a new beacon's pill is drawn in its final state too; the fx layer highlights it while it is new)
                var hits = [], pillBy = {}
                if (!lite && map.showSsids && map.zoom >= 13) {
                    var lbl = pts.filter(function(p) { return !p.area }).sort(function(a, b) { return b.dbm - a.dbm })
                    var limit = map.zoom >= 18 ? lbl.length : map.zoom >= 15 ? 24 : 12, taken = [], made = 0, h = 16
                    for (var li = 0; li < lbl.length && made < limit; li++) {
                        var L = lbl[li]
                        var name = L.ssid ? (L.ssid.length > 18 ? L.ssid.slice(0, 17) + "…" : L.ssid) : "(hidden)"
                        if (L.ids.length > 1) name += " +" + (L.ids.length - 1)
                        var tw = (L.ssid ? fmName : fmItalic).advanceWidth(name)
                        var badge = map.zoom >= 16 && L.band ? L.band : ""
                        var bw = badge ? fmBadge.advanceWidth(badge) + 6 : 0
                        var w = tw + 12 + (badge ? bw + 3 : 0)
                        var cands = [[L.x + 9, L.y - 8], [L.x - w / 2, L.y - 27], [L.x - w / 2, L.y + 10]]
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
                        var la = map.secFocus && L.secRank < 3 ? 0.25 : 1
                        ctx.globalAlpha = la
                        ctx.beginPath(); ctx.roundedRect(at.x, at.y, w, h, 4, 4); ctx.fillStyle = "rgba(8,13,20,0.8)"; ctx.fill()
                        if (L.secRank >= 3) { ctx.strokeStyle = L.secCol; ctx.lineWidth = 1; ctx.stroke() }
                        ctx.fillStyle = tcol; ctx.fillRect(at.x, at.y + 3, 2.5, h - 6)
                        marks.push({x: at.x + 7, y: at.y + h / 2, t: name, c: tcol, px: 10, i: L.ssid ? 0 : 1, a: la})
                        if (badge) {
                            var bx = at.x + 7 + tw + 3
                            ctx.beginPath(); ctx.roundedRect(bx, at.y + 3, bw, h - 6, 3, 3); ctx.fillStyle = "rgba(230,237,247,0.18)"; ctx.fill()
                            marks.push({x: bx + 3, y: at.y + h / 2, t: badge, c: "#e6edf7", px: 8, b: 1, a: la})
                        }
                        ctx.globalAlpha = 1
                        hits.push({x: at.x, y: at.y, w: w, h: h, ids: L.ids})
                        for (var pb = 0; pb < L.ids.length; pb++) pillBy[aps[L.ids[pb]].bssid] = {x: at.x, y: at.y, w: w, h: h, col: tcol}
                    }
                }
                map.labelHits = hits
                map.pillBy = pillBy

                map.posBy = posBy
                map.mePos = {x: mx, y: my}
                if (map.fxActive()) fx.requestPaint()

                if (atMe.length) pts.push({x: mx, y: my, ids: atMe})
                map.beaconHits = pts
                meBadge.count = atMe.length
                map.setMarks(marks, umarks)
            }
        }
        Item {                                  // the overlay's text above it (glyphs, counts, Wi-Fi names)
            id: markLayer
            anchors.fill: parent
            Repeater {
                model: map.markPool
                delegate: MarkText {
                    required property int index
                    m: index < map.marks.length ? map.marks[index] : null
                }
            }
        }
    }

    // ── fx: animations + pinned halo only. Repainted by the clock; the base overlay is not. ──
    // Not transformed: it paints in live view coordinates, mapping the overlay's painted positions
    // (posBy, selPos) through the overlay's transform, and is only repainted while something animates.
    // Its text is QML Text too (fxLayer): a canvas fillText per frame (an emoji especially) costs milliseconds.
    // One pool per text style, so a Text item never changes font from one frame to the next (a font switch
    // re-shapes the text, with emoji fallback lookups: tens of ms): t bold 11 · i italic 10 · e 20 px emoji · g 10 px glyph.
    property var fxMarks: ({t: [], i: [], e: [], g: []})
    property var fxPool: ({t: 0, i: 0, e: 0, g: 0})
    property int _fxCount: 0
    function setFxMarks(l) {
        if (!l.length && !_fxCount) return
        var by = {t: [], i: [], e: [], g: []}, grow = false, pool = fxPool
        for (var n = 0; n < l.length; n++) by[l[n].s].push(l[n])
        for (var k in by) if (by[k].length > pool[k]) grow = true
        for (var k2 in by) by[k2] = slotMarks(fxMarks[k2], by[k2])
        if (grow) { var np = {}; for (var q in by) np[q] = Math.max(pool[q], Math.ceil(by[q].length / 4) * 4); fxPool = np }
        _fxCount = l.length
        fxMarks = by
    }
    component FxText: Text {
        required property var m
        visible: m !== null && !m.hid
        text: m ? m.t : ""
        color: m ? m.c : "#ffffff"
        opacity: m && m.a !== undefined ? m.a : 1
        font.family: "sans-serif"
        x: m ? (m.h ? m.x - implicitWidth / 2 : m.x) : 0
        y: m ? m.y - baselineOffset : 0          // canvas convention: y is the baseline
    }
    Canvas {
        id: fx
        anchors.fill: parent
        onPaint: {
            if (map._viewQueued) map.applyXforms()      // draws for the live camera: the layers must be there in this frame too
            var ctx = getContext("2d")
            ctx.reset()
            var src = map.src, fxText = []
            if (!src.valid) { map.setFxMarks(fxText); return }
            var posBy = map.posBy || {}, now = map.animNow
            var me = map.merc(src.lat, src.lon)
            if (map.selPos && map.selectedBeacon && !map.lite) {
                var sp = map.paintToView(map.selPos)
                ctx.beginPath(); ctx.arc(sp.x, sp.y, 11, 0, Math.PI * 2)
                ctx.strokeStyle = "rgba(0,0,0,0.55)"; ctx.lineWidth = 4; ctx.stroke()
                ctx.strokeStyle = "#ffffff"; ctx.lineWidth = 2; ctx.stroke()
            }
            // "sample here next" for the hovered / pinned beacon: a dotted line from its estimate to a small target
            var sg = map.lite ? null : map.sugTarget
            if (sg) {
                var sa0 = map.merc(sg.lat, sg.lon), sa1 = map.merc(sg.slat, sg.slon)
                var sx0 = map.sx(sa0.x), sy0 = map.sy(sa0.y), sx1 = map.sx(sa1.x), sy1 = map.sy(sa1.y)
                ctx.strokeStyle = sg.col; ctx.lineWidth = 1.5; ctx.globalAlpha = 0.8
                ctx.setLineDash([1.5, 3.5]); ctx.beginPath(); ctx.moveTo(sx0, sy0); ctx.lineTo(sx1, sy1); ctx.stroke(); ctx.setLineDash([])
                ctx.globalAlpha = 1; ctx.beginPath()
                ctx.moveTo(sx1 + 8, sy1); ctx.arc(sx1, sy1, 8, 0, Math.PI * 2)
                ctx.moveTo(sx1 + 3.5, sy1); ctx.arc(sx1, sy1, 3.5, 0, Math.PI * 2)
                ctx.moveTo(sx1 - 12, sy1); ctx.lineTo(sx1 - 9, sy1); ctx.moveTo(sx1 + 9, sy1); ctx.lineTo(sx1 + 12, sy1)
                ctx.moveTo(sx1, sy1 - 12); ctx.lineTo(sx1, sy1 - 9); ctx.moveTo(sx1, sy1 + 9); ctx.lineTo(sx1, sy1 + 12)
                ctx.strokeStyle = "rgba(0,0,0,0.6)"; ctx.lineWidth = 3.5; ctx.stroke()
                ctx.strokeStyle = sg.col; ctx.lineWidth = 1.6; ctx.stroke()
                fxText.push({s: "t", x: sx1, y: sy1 + 25, t: "sample here", c: sg.col, h: 1})
            }
            // ── events as motion ──
            for (var ai = 0; ai < map.anims.length; ai++) {
                var an = map.anims[ai], pr = map.progress(an)
                if (pr <= 0) continue
                var pos = posBy[an.bssid] ? map.paintToView(posBy[an.bssid]) : undefined
                if (an.type === "ap_new") {
                    if (!pos) continue
                    ctx.strokeStyle = pos.col; ctx.lineWidth = 2
                    ctx.globalAlpha = 1 - pr
                    ctx.beginPath(); ctx.arc(pos.x, pos.y, 6 + 34 * map.easeOut(pr), 0, Math.PI * 2); ctx.stroke()
                    if (pr > 0.3) { ctx.globalAlpha = (1 - pr) * 0.6; ctx.beginPath(); ctx.arc(pos.x, pos.y, 6 + 26 * map.easeOut((pr - 0.3) / 0.7), 0, Math.PI * 2); ctx.stroke() }
                    var pill = map.pillBy[an.bssid]
                    if (pill) {                         // its name pill (drawn final by the overlay) glows while it is new
                        var pa = map.paintToView(pill), pw = pill.w * map.pX.s, ph = pill.h * map.pX.s
                        ctx.beginPath(); ctx.roundedRect(pa.x - 1.5, pa.y - 1.5, pw + 3, ph + 3, 5, 5)
                        ctx.globalAlpha = 0.22 * (1 - pr); ctx.fillStyle = pos.col; ctx.fill()
                        ctx.globalAlpha = 0.9 * (1 - pr); ctx.lineWidth = 1.5; ctx.stroke()
                    }
                    ctx.globalAlpha = 1
                } else if (an.type === "ap_lost") {
                    var gp = map.project(an.ghost), sc = 1 - pr
                    ctx.globalAlpha = sc; ctx.fillStyle = an.ghost.col || "#8a93a6"
                    ctx.beginPath(); ctx.arc(gp.x, gp.y, 4.5 * sc + 0.5, 0, Math.PI * 2); ctx.fill()
                    ctx.beginPath(); ctx.arc(gp.x, gp.y, 6 + 12 * pr, 0, Math.PI * 2)
                    ctx.strokeStyle = an.ghost.col || "#8a93a6"; ctx.lineWidth = 1; ctx.setLineDash([2, 3]); ctx.stroke(); ctx.setLineDash([])
                    fxText.push({s: "i", x: gp.x + 8, y: gp.y - 8 - 14 * pr, t: (an.ghost.ssid || "(hidden)") + " gone", c: "#c9d4e5", a: sc})
                    ctx.globalAlpha = 1
                } else if (an.type === "ap_up" || an.type === "ap_down") {
                    if (!pos) continue
                    var up = an.type === "ap_up"
                    var bob = (up ? -3 : 3) * Math.abs(Math.sin(now / 220))
                    fxText.push({s: "t", x: pos.x + 10, y: pos.y + 12 + bob, t: (up ? "▲ " : "▼ ") + (an.delta > 0 ? "+" : "") + an.delta + " dB", c: up ? "#6cff8a" : "#ff9f43",
                             a: Math.min(1, (1 - pr) * 3) * (0.55 + 0.45 * Math.sin(now / 160))})
                } else if (an.type === "ap_placed") {
                    if (!pos) continue
                    if (an.fromPos) {                   // the marker glides in from the orbit spot it used to sit on
                        var fo = map.project(an.fromPos), ge = map.easeOut(Math.min(1, pr / 0.6))
                        var glx = fo.x + (pos.x - fo.x) * ge, gly = fo.y + (pos.y - fo.y) * ge
                        ctx.globalAlpha = 1 - pr; ctx.strokeStyle = "rgba(255,209,102,0.6)"; ctx.lineWidth = 1; ctx.setLineDash([3, 3])
                        ctx.beginPath(); ctx.moveTo(fo.x, fo.y); ctx.lineTo(glx, gly); ctx.stroke(); ctx.setLineDash([])
                        if (ge < 1) {
                            ctx.globalAlpha = 1; ctx.beginPath(); ctx.arc(glx, gly, 4.5, 0, Math.PI * 2); ctx.fillStyle = "#ffd166"; ctx.fill()
                            ctx.strokeStyle = "rgba(255,255,255,0.85)"; ctx.lineWidth = 1; ctx.stroke()
                        }
                    }
                    ctx.globalAlpha = 1 - pr; ctx.strokeStyle = "#ffd166"; ctx.lineWidth = 2.5
                    ctx.beginPath(); ctx.arc(pos.x, pos.y, 8 + 30 * map.easeOut(pr), 0, Math.PI * 2); ctx.stroke()
                    ctx.globalAlpha = 1
                } else if (an.type === "ap_refit") {
                    // Triangulation: range rings from each vantage point converge on the answer,
                    // the marker glides there while its uncertainty shrinks, then a lock-on.
                    var rt = map.merc(an.lat, an.lon), rtx = map.sx(rt.x), rty = map.sy(rt.y), mppR = map.mpp()
                    var pA = Math.min(1, pr / 0.5), eA = map.easeOut(pA)
                    var vp = an.points || []
                    ctx.setLineDash([4, 3]); ctx.lineWidth = 1.2
                    for (var vi = 0; vi < vp.length; vi++) {
                        var vm = map.merc(vp[vi].lat, vp[vi].lon), vx = map.sx(vm.x), vy = map.sy(vm.y)
                        var vd = Math.hypot(rtx - vx, rty - vy)
                        var vcol = vp[vi].device && vp[vi].device !== "me" ? (vp[vi].kind === "android" ? "#c9a0ff" : "#ffd166") : "#7cf2c4"
                        ctx.globalAlpha = Math.min(1, pr * 4) * (pr < 0.8 ? 1 : 1 - (pr - 0.8) / 0.2)
                        ctx.fillStyle = vcol; ctx.beginPath(); ctx.moveTo(vx, vy - 5); ctx.lineTo(vx + 5, vy + 4); ctx.lineTo(vx - 5, vy + 4); ctx.closePath(); ctx.fill()
                        if (vp[vi].device && vp[vi].device !== "me") fxText.push({s: "g", x: vx, y: vy - 9, t: map.deviceGlyph(vp[vi].kind), c: "#e6edf7", h: 1, a: ctx.globalAlpha})
                        ctx.globalAlpha *= 0.85; ctx.strokeStyle = vcol
                        ctx.beginPath(); ctx.arc(vx, vy, Math.max(1, vd * eA), 0, Math.PI * 2); ctx.stroke()
                    }
                    ctx.setLineDash([])
                    var pB = Math.max(0, Math.min(1, (pr - 0.3) / 0.45)), eB = map.easeOut(pB)
                    var fx0 = rtx, fy0 = rty
                    if (an.from) { var rfm = map.merc(an.from.lat, an.from.lon); fx0 = map.sx(rfm.x); fy0 = map.sy(rfm.y) }
                    var gx = fx0 + (rtx - fx0) * eB, gy = fy0 + (rty - fy0) * eB
                    if (an.from && pB > 0) {
                        ctx.globalAlpha = 0.6; ctx.strokeStyle = "#ffd166"; ctx.lineWidth = 1; ctx.setLineDash([3, 3])
                        ctx.beginPath(); ctx.moveTo(fx0, fy0); ctx.lineTo(gx, gy); ctx.stroke(); ctx.setLineDash([])
                    }
                    var rr0 = Math.max(4, (an.prevAcc + (an.acc - an.prevAcc) * eB) / mppR)
                    ctx.globalAlpha = 0.7; ctx.strokeStyle = "#ffd166"; ctx.lineWidth = 1.5
                    ctx.beginPath(); ctx.arc(gx, gy, rr0, 0, Math.PI * 2); ctx.stroke()
                    ctx.globalAlpha = 0.08; ctx.fillStyle = "#ffd166"; ctx.fill()
                    ctx.globalAlpha = 1; ctx.beginPath(); ctx.arc(gx, gy, 5, 0, Math.PI * 2); ctx.fillStyle = "#ffd166"; ctx.fill()
                    if (pr > 0.7) {
                        var pC = (pr - 0.7) / 0.3, pulse = 0.5 + 0.5 * Math.sin(pC * Math.PI * 4), arm = 10 + 6 * pulse
                        ctx.globalAlpha = 0.25 + 0.75 * (1 - pC); ctx.strokeStyle = "#7cf2c4"; ctx.lineWidth = 2
                        ctx.beginPath()
                        ctx.moveTo(rtx - arm, rty); ctx.lineTo(rtx - arm / 2, rty); ctx.moveTo(rtx + arm / 2, rty); ctx.lineTo(rtx + arm, rty)
                        ctx.moveTo(rtx, rty - arm); ctx.lineTo(rtx, rty - arm / 2); ctx.moveTo(rtx, rty + arm / 2); ctx.lineTo(rtx, rty + arm)
                        ctx.stroke()
                        ctx.fillStyle = "#e6edf7"
                        for (var sk = 0; sk < 6; sk++) {
                            var sa = sk * Math.PI / 3 + pC, sd = 8 + 18 * pC
                            ctx.globalAlpha = 1 - pC; ctx.beginPath(); ctx.arc(rtx + Math.cos(sa) * sd, rty + Math.sin(sa) * sd, 1.5, 0, Math.PI * 2); ctx.fill()
                        }
                        fxText.push({s: "t", x: rtx, y: rty - 18 - 10 * pC, t: "±" + Math.round(an.acc) + " m" + (an.n ? " · " + an.n + " samples" : ""), c: "#7cf2c4", h: 1, a: 1 - pC * 0.7})
                    }
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
                    fxText.push({s: "e", x: sxp, y: syp - 2 + yoff, t: "🚩", c: "#ff4f4f", h: 1, a: ctx.globalAlpha})
                    if (drop >= 1) {
                        var dr = ((now - an.t0 - 1200) / 900) % 1
                        ctx.globalAlpha *= (1 - dr); ctx.strokeStyle = "#ff4f4f"; ctx.lineWidth = 1.5
                        ctx.beginPath(); ctx.arc(sxp, syp, 4 + 18 * dr, 0, Math.PI * 2); ctx.stroke()
                    }
                    ctx.globalAlpha = 1
                }
            }
            map.setFxMarks(fxText)
        }
    }
    Item {                                      // the fx layer's text, in the same live view coordinates
        id: fxLayer
        anchors.fill: parent
        // Laid out once at load (never seen): the fonts and glyphs an animation needs are ready before its
        // first frame, instead of costing 70-120 ms of font set-up in the middle of it
        Repeater {
            model: [{t: "▲ ▼ +-0123456789 dB ±m · samples here", px: 11, b: true, i: false}, {t: "(hidden) gone", px: 10, b: false, i: true},
                    {t: "🚩", px: 20, b: false, i: false}, {t: "📱💻🖥📍", px: 10, b: false, i: false}]
            delegate: Text {
                required property var modelData
                opacity: 0; text: modelData.t
                font.family: "sans-serif"; font.pixelSize: modelData.px; font.bold: modelData.b; font.italic: modelData.i
            }
        }
        Repeater { model: map.fxPool.t; delegate: FxText { required property int index; m: index < map.fxMarks.t.length ? map.fxMarks.t[index] : null; font.pixelSize: 11; font.bold: true } }
        Repeater { model: map.fxPool.i; delegate: FxText { required property int index; m: index < map.fxMarks.i.length ? map.fxMarks.i[index] : null; font.pixelSize: 10; font.italic: true } }
        Repeater { model: map.fxPool.e; delegate: FxText { required property int index; m: index < map.fxMarks.e.length ? map.fxMarks.e[index] : null; font.pixelSize: 20 } }
        Repeater { model: map.fxPool.g; delegate: FxText { required property int index; m: index < map.fxMarks.g.length ? map.fxMarks.g[index] : null; font.pixelSize: 10 } }
    }
    Connections {
        target: map.src
        // data changes: one coalesced, guarded follow step per poll (lat and lon arrive separately)
        function onApsChanged() { map.scheduleFollow(); overlay.requestPaint(); map.updateSuggest() }
        // the heatmap draws routeFixes, else (an older desktop) the track: rebuilt once per change of either
        function onTrackChanged() { if (!(map.src.routeFixes && map.src.routeFixes.length)) map.updateRouteSegments(); overlay.requestPaint() }
        function onRouteFixesChanged() { map.updateRouteSegments(); overlay.requestPaint() }
        function onFlockCamerasChanged() { map.updateCameraIndex(); overlay.requestPaint() }
        function onPoisChanged() { map.selected = -1; map.cluster() }
        function onValidChanged() { map.scheduleFollow(); overlay.requestPaint() }
        function onLatChanged() { map.scheduleFollow(); overlay.requestPaint() }
        function onLonChanged() { map.scheduleFollow(); overlay.requestPaint() }
        function onAccuracyChanged() { map.scheduleFollow(); overlay.requestPaint() }
        function onNewEvents(list, animate) { map.onEvents(list, animate) }
    }
    onShowSsidsChanged: overlay.requestPaint()
    onOnScreenChanged: if (onScreen) { refreshTiles(); overlay.requestPaint() }   // catch up after a hidden spell
    onLinkedChanged: overlay.requestPaint()
    onShowEventsChanged: if (!showEvents) { anims = []; animCount = 0; tickerModel.clear(); toastModel.clear(); overlay.requestPaint(); fx.requestPaint() }

    // ── places: clustered in world space so panning doesn't reshuffle them ──
    // The place delegates are a fixed pool: a zoom that only changes which names show toggles label visibility,
    // and a re-cluster moves markers between existing delegates. (Reassigning a JS-array model made the Repeater
    // destroy and rebuild every delegate, ~95 with two Labels each, 130-210 ms, on every zoom step.)
    Timer { id: clusterTimer; interval: 140; onTriggered: map.cluster() }
    function cluster() {
        var pois = src.pois || [], out = []
        var hidden = {}
        for (var h = 0; h < hiddenCats.length; h++) hidden[hiddenCats[h]] = true
        // Clusters at the zoom rounded down to a half level: a small zoom step keeps the same groups
        var zq = Math.floor(zoom * 2) / 2, wq = 256 * Math.pow(2, zq)
        var cell = zq < 13 ? 56 : zq < 15.5 ? 40 : 0
        var groups = {}, order = []
        for (var i = 0; i < pois.length; i++) {
            var pt = pois[i]
            if (hidden[pt.cat]) continue
            var m = merc(pt.lat, pt.lon)
            if (cell > 0) {
                var key = Math.floor(m.x * wq / cell) + ":" + Math.floor(m.y * wq / cell)
                if (!groups[key]) { groups[key] = {ids: [], mx: 0, my: 0, cats: {}}; order.push(key) }
                var g = groups[key]; g.ids.push(i); g.mx += m.x; g.my += m.y; g.cats[pt.cat] = (g.cats[pt.cat] || 0) + 1
            } else {
                out.push({ids: [i], mx: m.x, my: m.y, icon: pt.icon, color: pt.color, count: 1, name: pt.name || "", wifi: !!pt.wifi && pt.cat !== "wifi", d: pt.d || 0, pri: helpPriority(pt)})
            }
        }
        // A cluster wears the icon of the most urgent help in it (pediatric ER, then an ER, then police / fire),
        // else of its most common category: help must not hide behind a cluster of cafés.
        for (var o = 0; o < order.length; o++) {
            var gr = groups[order[o]], top = "", best = 0
            for (var c in gr.cats) if (gr.cats[c] > best) { best = gr.cats[c]; top = c }
            var first = null, fp = 0
            for (var f = 0; f < gr.ids.length; f++) { var fpi = helpPriority(pois[gr.ids[f]]); if (fpi > fp) { fp = fpi; first = pois[gr.ids[f]] } }
            if (!first) {
                first = pois[gr.ids[0]]
                for (var t = 0; t < gr.ids.length; t++) if (pois[gr.ids[t]].cat === top) { first = pois[gr.ids[t]]; break }
            }
            var mixed = Object.keys(gr.cats).length > 1
            out.push({ids: gr.ids, mx: gr.mx / gr.ids.length, my: gr.my / gr.ids.length, icon: first.icon, color: first.color,
                      count: gr.ids.length, badge: mixed ? "#e6edf7" : first.color, name: gr.ids.length === 1 ? (first.name || "") : "", wifi: false, d: first.d || 0, pri: fp})
        }
        var slots = slotMarkers(markers, out)
        var js = JSON.stringify(slots)
        if (js !== _markersJson) {
            _markersJson = js
            if (slots.length > markerPool) markerPool = Math.ceil(slots.length / 16) * 16
            markers = slots
        }
        // Name labels for the nearest single places that don't collide (world px, pan-invariant). Laid out at the
        // zoom rounded down to a quarter level: zoomed in further, the places only spread apart, so still no overlap.
        var zl = Math.floor(zoom * 4) / 4, wl = 256 * Math.pow(2, zl)
        var taken = [], labelled = 0, shown = [], live = []
        for (var s = 0; s < slots.length; s++) { shown.push(false); if (slots[s] && !slots[s].hid) live.push(s) }
        var byDist = live.slice().sort(function(a, b) { return slots[a].d - slots[b].d })
        for (var q = 0; q < byDist.length && zl >= 15.5 && labelled < 18; q++) {
            var mk = slots[byDist[q]]
            if (mk.count > 1 || !mk.name) continue
            var wx = mk.mx * wl + 16, wy = mk.my * wl - 9, w = Math.min(150, mk.name.length * 6.5 + 12), hh = 18
            var hit = false
            for (var u = 0; u < live.length && !hit; u++) {
                var ou = slots[live[u]], ox = ou.mx * wl, oy = ou.my * wl
                if (ou !== mk && ox > wx - 12 && ox < wx + w + 12 && oy > wy - 12 && oy < wy + hh + 12) hit = true
            }
            for (var v = 0; v < taken.length && !hit; v++) {
                var r = taken[v]
                if (wx < r.x + r.w && wx + w > r.x && wy < r.y + r.h && wy + hh > r.y) hit = true
            }
            if (hit) continue
            taken.push({x: wx, y: wy, w: w, h: hh}); shown[byDist[q]] = true; labelled++
        }
        var lj = JSON.stringify(shown)
        if (lj !== _labelsJson) { _labelsJson = lj; markerLabels = shown }
    }
    function helpPriority(p) { return !p ? 0 : p.cat === "peds_er" ? 3 : p.cat === "health" && p.emergency ? 2 : p.cat === "police" || p.cat === "fire" ? 1 : 0 }
    // A marker keeps the delegate that showed it last time (same places); a new one takes a free delegate that
    // showed the same icon (its emoji is already laid out), else any free one; the pool only grows when full.
    // A delegate with nothing to show keeps its last marker, hidden, so its texts stay laid out.
    function slotMarkers(prev, list) {
        var out = [], byKey = {}, byIcon = {}, rest = [], i, k
        for (i = 0; i < prev.length; i++) { out.push(null); if (prev[i]) { k = prev[i].ids.join(","); (byKey[k] || (byKey[k] = [])).push(i) } }
        for (i = 0; i < list.length; i++) {
            var c = byKey[list[i].ids.join(",")]
            if (c && c.length) out[c.shift()] = list[i]; else rest.push(list[i])
        }
        for (i = 0; i < prev.length; i++) if (prev[i] && !out[i]) (byIcon[prev[i].icon] || (byIcon[prev[i].icon] = [])).push(i)
        var free = 0
        for (i = 0; i < rest.length; i++) {
            var same = byIcon[rest[i].icon]
            while (same && same.length && out[same[0]]) same.shift()
            if (same && same.length) { out[same.shift()] = rest[i]; continue }
            while (free < out.length && out[free]) free++
            if (free < out.length) out[free] = rest[i]; else out.push(rest[i])
        }
        for (i = 0; i < prev.length; i++) if (!out[i] && prev[i]) out[i] = prev[i].hid ? prev[i] : Object.assign({}, prev[i], {hid: true})
        return out
    }
    property string _markersJson: ""
    property string _labelsJson: ""

    // ── gestures ────────────────────────────────────────────────────────────
    function hitAt(vx, vy) {                    // beacon label pill or marker under a point → {kind: "beacon", ids}
        // the hit geometry is from the last paint: test in its coordinates, i.e. against what is on screen
        var q = viewToPaint(vx, vy), x = q.x, y = q.y
        for (var l = 0; l < labelHits.length; l++) {
            var r = labelHits[l]
            if (x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h) return {kind: "beacon", ids: r.ids}
        }
        var best = null, bd = 12 / pX.s
        for (var i = 0; i < beaconHits.length; i++) {
            var h = beaconHits[i], d = Math.hypot(h.x - x, h.y - y), reach = h.area ? Math.max(12, 8 + 2.5 * Math.log(h.ids.length) / Math.LN2 + 2) / pX.s : bd
            if (d < Math.max(bd, reach) && d < (best ? best.d : Infinity)) { best = h; best.d = d }
        }
        return best ? {kind: "beacon", ids: best.ids, area: !!best.area} : null
    }
    function pinBeacon(ids) {                   // pin the strongest of a group in the card
        var aps = src.aps || [], top = -1
        for (var i = 0; i < ids.length; i++) if (top < 0 || (aps[ids[i]] && aps[top] && aps[ids[i]].dbm > aps[top].dbm)) top = ids[i]
        selectedBeacon = top >= 0 && aps[top] ? (aps[top].bssid || "") : ""
        selected = -1
        overlay.requestPaint()
    }
    // The menu acts on the spot that was clicked, kept on the map (Mercator), not as a screen pixel: the view can
    // still move under an open menu (a zoom tween finishing), and a pixel would then point somewhere else.
    function openContext(x, y) {
        var hit = hitAt(x, y)
        if (hit) pinBeacon(hit.ids)
        var m = toMerc(x, y)
        ctxMenu.mx = m.x - Math.floor(m.x); ctxMenu.my = Math.max(0, Math.min(1, m.y))
        ctxMenu.popup(map, x, y)
    }
    function ctxPoint() { return Qt.point(sx(ctxMenu.mx), sy(ctxMenu.my)) }     // the clicked spot, where it is on screen now
    function ctxPlaceAntenna() { startAnchorAt(latOf(ctxMenu.my), lonOf(ctxMenu.mx)) }
    function ctxCentre() { follow = false; cx = ctxMenu.mx; cy = ctxMenu.my }
    function ctxZoomIn() { var p = ctxPoint(); zoomAt(1, p.x, p.y) }
    MouseArea {
        id: pan
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        property point start
        property point startC
        property bool moved: false
        cursorShape: map.draggingAnchor ? Qt.SizeAllCursor : pressed && moved ? Qt.ClosedHandCursor : (map.hover && map.hover.kind === "beacon" ? Qt.PointingHandCursor : Qt.ArrowCursor)
        onPressed: mouse => {
            map.userTouched(); start = Qt.point(mouse.x, mouse.y); startC = Qt.point(map.cx, map.cy); moved = false
            if (mouse.button === Qt.LeftButton && map.editAnchor) {
                var p = map.anchorScreen(map.editAnchor)
                map.draggingAnchor = Math.abs(p.x - mouse.x) <= 16 && Math.abs(p.y - mouse.y) <= 16
            }
        }
        onReleased: { if (map.draggingAnchor) { map.draggingAnchor = false; map.patchAnchor({accM: map.pickAcc()}) } }
        onPositionChanged: mouse => {
            if ((pressedButtons & Qt.LeftButton) && map.draggingAnchor) {
                var am = map.toMerc(mouse.x, mouse.y); moved = true
                map.patchAnchor({lat: map.latOf(am.y), lon: map.lonOf(am.x)})
                return
            }
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
            var anc = map.anchorAt(mouse.x, mouse.y)
            if (anc) { if (!(map.editAnchor && map.editAnchor.id === anc.id)) map.editExisting(anc); return }
            var hit = map.hitAt(mouse.x, mouse.y)
            if (hit && hit.area) map.zoomAt(2, mouse.x, mouse.y)      // an area group: zoom in until it splits
            else if (hit) map.pinBeacon(hit.ids)
            else { map.selected = -1; if (map.selectedBeacon) { map.selectedBeacon = ""; overlay.requestPaint(); fx.requestPaint() } }
        }
        onDoubleClicked: mouse => { if (mouse.button === Qt.LeftButton) map.zoomAt(1, mouse.x, mouse.y) }
        onExited: if (map.hover && map.hover.kind === "beacon") map.hover = null
        onWheel: wheel => { map.zoomAt(wheel.angleDelta.y / 120 * 0.35, wheel.x, wheel.y); wheel.accepted = true }

        // Touch / touchpad: pinch zooms about the pinch centre; press-and-hold opens the context menu.
        // Handlers sit on the MouseArea so they see the points first; the tap handler is passive
        // (drag threshold policy) and cancels as soon as a pan starts.
        PinchHandler {
            id: pinch
            target: null
            property real z0: 15
            onActiveChanged: if (active) { map.userTouched(); z0 = map.zoom; map.autoZoom = false; map.follow = false }
            onActiveScaleChanged: if (active) {
                var z = Math.max(3, Math.min(map.maxZoomView, z0 + Math.log(activeScale) / Math.LN2))
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

    HoverHandler { id: mapHover; onHoveredChanged: if (!hovered) map._hoverEndAt = Date.now() }

    // ── me + places: laid out for the reference camera, moved by the layer transform (itemM);
    // each item counter-scales (invS) so it keeps its size while the layer zooms ──
    Item {
        id: placeLayer
        anchors.fill: parent
        transform: itemM
        Item {
            id: meItem
            visible: map.src.valid
            readonly property point m: map.merc(map.src.lat, map.src.lon)
            x: map.rsx(m.x); y: map.rsy(m.y)
            scale: map.invS
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
            model: map.markerPool                    // a fixed pool (see cluster()): changing markers never rebuilds it
            delegate: Item {
                id: mk
                required property int index
                readonly property var md: (index < map.markers.length && map.markers[index]) || map._noMarker
                readonly property bool live: !md.hid
                readonly property bool hot: live && (pinArea.containsMouse || (md.count === 1 && map.selected === md.ids[0]))
                x: map.rsx(md.mx); y: map.rsy(md.my)
                scale: map.invS
                // generous bounds: the layer is only re-laid-out every 0.3 zoom levels while moving
                visible: live && x > -map.width * 0.6 && x < map.width * 1.6 && y > -map.height * 0.6 && y < map.height * 1.6
                z: hot ? 10 : md.pri === 3 ? 2 : 1       // a pediatric ER is drawn over its neighbours
                // the delegate now shows another place (or none) under the pointer: the card follows
                onMdChanged: if (pinArea.containsMouse) map.hover = !md.hid ? {kind: md.count > 1 ? "cluster" : "poi", ids: md.ids} : null
                Rectangle {
                    readonly property real r: mk.hot ? 13 : 11
                    x: -r; y: -r; width: 2 * r; height: 2 * r; radius: r
                    color: "#0c111c"; border.color: mk.md.color; border.width: mk.hot ? 2.5 : 2
                    Text { anchors.centerIn: parent; text: mk.md.icon; font.pixelSize: 12; color: "white" }
                }
                Rectangle {                          // advertises Wi-Fi
                    visible: mk.md.wifi
                    x: 5; y: -11; width: 6; height: 6; radius: 3; color: "#35d6ff"
                }
                Rectangle {                          // cluster count
                    visible: mk.md.count > 1
                    x: 4; y: -18; height: 15; radius: 7.5; width: Math.max(15, cnt.implicitWidth + 8)
                    color: mk.md.badge || mk.md.color; border.color: "#0b101a"; border.width: 1.5
                    PC3.Label { id: cnt; anchors.centerIn: parent; text: mk.md.count; color: "#0b101a"; font.bold: true; font.pixelSize: 10 }
                }
                Rectangle {                          // name
                    visible: map.markerLabels[mk.index] === true && !mk.hot
                    x: 15; y: -9; height: 18; radius: 4
                    width: Math.min(150, nameText.implicitWidth + 12)
                    color: Qt.rgba(0.03, 0.05, 0.08, 0.78)
                    Rectangle { x: 0; y: 3; width: 2.5; height: parent.height - 6; radius: 1; color: mk.md.color }
                    PC3.Label {
                        id: nameText
                        x: 6; anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, 138)
                        text: mk.md.name || ""; elide: Text.ElideRight
                        color: "#e6edf7"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                    }
                }
                MouseArea {
                    id: pinArea
                    x: -14; y: -14; width: 28; height: 28
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onPressed: map.userTouched()             // a place click during a tour / spotlight stops it there
                    onContainsMouseChanged: map.hover = containsMouse ? {kind: mk.md.count > 1 ? "cluster" : "poi", ids: mk.md.ids}
                                                                      : (map.hover && map.hover.kind !== "beacon" ? null : map.hover)
                    onClicked: {
                        var v = map.itemToView(mk.x, mk.y)
                        if (mk.md.count > 1) map.zoomAt(2, v.x, v.y)
                        else map.selected = mk.md.ids[0]
                    }
                    onWheel: wheel => { var v = map.itemToView(mk.x, mk.y); map.zoomAt(wheel.angleDelta.y / 120 * 0.5, v.x, v.y) }
                }
            }
        }
    }

    // ── controls ────────────────────────────────────────────────────────────
    component MapButton: PC3.ToolButton {
        id: mb
        property string tip: ""
        property bool touches: true              // a press counts as input (stops a tour / spotlight)
        onPressed: if (touches) map.userTouched()
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
            text: "A–F"; tip: "Position grades legend (how sure each beacon's place is)"; display: QQC2.AbstractButton.TextOnly
            touches: false
            checkable: true; checked: map.showLegend
            contentItem: PC3.Label { text: "A–F"; color: "#e6edf7"; font.bold: true; font.pixelSize: 9; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            onToggled: map.showLegend = checked
        }
        MapButton {
            icon.name: "media-playback-start"; text: "Cinematic"; tip: "Cinematic mode: glide to significant events, now and then zoom out to show the city and state, and re-fit the zoom once you have moved somewhere new, never while parked (at most one automatic zoom every 10 minutes, none while Follow is off). Off: the map only pans to keep you in view"
            touches: false                          // switching it off mid-tour flies home (onCinematicChanged)
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
        TapHandler { onTapped: { map.userTouched(); map.secPanel = !map.secPanel; if (map.secPanel) map.secFocus = true } }
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
                PC3.ToolButton { icon.name: "window-close"; onPressed: map.userTouched(); onClicked: { map.secPanel = false; map.secFocus = false } }
            }
            ListView {
                id: secList
                Layout.fillWidth: true; Layout.fillHeight: true
                clip: true; spacing: 6
                onMovementStarted: map.userTouched()
                model: {
                    // Built only while the panel is open: a hidden ListView still re-creates its delegates
                    // (each with its issue list) on every model reassignment, i.e. on every poll
                    if (!map.secPanel) return []
                    var aps = map.src.aps || [], rows = []
                    for (var i = 0; i < aps.length; i++) {
                        var a = aps[i]; if (!a || a.kind === "none") continue
                        var c = map.secOf(a); rows.push({ap: a, sec: c, idx: i})
                    }
                    rows.sort(function(x, y) { return y.sec.rank - x.sec.rank || y.ap.dbm - x.ap.dbm })
                    return rows
                }
                delegate: Rectangle {
                    id: secRow
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
                            Rectangle {                  // position grade badge (A–F, R, M)
                                id: gradeBadge
                                readonly property string g: map.gradeOf(secRow.modelData.ap)
                                visible: g !== ""
                                Layout.preferredWidth: 16; Layout.preferredHeight: 16; radius: 3
                                color: "transparent"; border.color: map.gradeColor(g); border.width: 1
                                PC3.Label { anchors.centerIn: parent; text: gradeBadge.g; color: map.gradeColor(gradeBadge.g); font.bold: true; font.pixelSize: 10 }
                                QQC2.ToolTip.text: map.gradeLine(secRow.modelData.ap); QQC2.ToolTip.visible: gradeHover.hovered
                                HoverHandler { id: gradeHover }
                            }
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
                    TapHandler { onTapped: { map.userTouched(); map.selectedBeacon = modelData.ap.bssid; overlay.requestPaint(); fx.requestPaint() } }
                }
            }
        }
    }
    Rectangle {                                 // position grades legend (toggled by the A–F button)
        id: gradeLegend
        visible: map.showLegend && !map.secPanel && map.editAnchor === null; z: 6
        anchors { right: parent.right; top: parent.top; margins: Kirigami.Units.smallSpacing; rightMargin: 44 }
        width: legendCol.implicitWidth + 16; height: legendCol.implicitHeight + 12
        radius: 7; color: Qt.rgba(0.03, 0.05, 0.08, 0.9); border.color: Qt.rgba(0.21, 0.84, 1, 0.3); border.width: 1
        Column {
            id: legendCol
            x: 8; y: 6; spacing: 2
            PC3.Label { text: "Beacon position grade"; color: "#e6edf7"; font.bold: true; font.pixelSize: Kirigami.Theme.smallFont.pixelSize }
            Repeater {
                model: map.gradeWords
                delegate: Row {
                    id: lgRow
                    required property var modelData
                    readonly property string g: lgRow.modelData[0]
                    spacing: 6
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14; height: lgRow.g === "M" || lgRow.g === "R" ? 14 : 10; radius: height / 2
                        color: Qt.alpha(map.gradeColor(lgRow.g), lgRow.g === "R" ? 0.25 : lgRow.g === "M" ? 1 : 0.2)
                        border.color: map.gradeColor(lgRow.g); border.width: lgRow.g === "M" ? 0 : 1.3
                        Text { visible: lgRow.g === "M"; anchors.centerIn: parent; text: "M"; color: "white"; font.bold: true; font.pixelSize: 8 }
                    }
                    PC3.Label { text: lgRow.g; color: map.gradeColor(lgRow.g); font.bold: true; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; width: 14 }
                    PC3.Label { text: lgRow.modelData[1]; color: "#c9d4e5"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize }
                }
            }
            PC3.Label { text: "- - dashed = extrapolated / ambiguous"; color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize }
            PC3.Label { text: "ellipse = 95 % area · ◎ = sample here next"; color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize }
        }
    }
    Rectangle {                                 // antenna anchor editor
        id: anchorPanel
        visible: map.editAnchor !== null; z: 9
        anchors { right: parent.right; top: parent.top; margins: Kirigami.Units.smallSpacing; rightMargin: 44 }
        width: Math.min(parent.width * 0.62, Kirigami.Units.gridUnit * 19)
        height: Math.min(parent.height - Kirigami.Units.smallSpacing * 2, anchorCol.implicitHeight + 16)
        radius: 8; color: Qt.rgba(0.03, 0.05, 0.08, 0.95); border.color: Qt.rgba(0.49, 0.95, 0.77, 0.55); border.width: 1
        readonly property var ea: map.editAnchor || ({})
        readonly property bool wantsRadios: ea.kind === "wifi-ap" || ea.kind === "rtt-responder"
        ColumnLayout {
            id: anchorCol
            anchors.fill: parent; anchors.margins: 8; spacing: 5
            RowLayout {
                Layout.fillWidth: true
                PC3.Label { text: anchorPanel.ea.isNew ? "Place an antenna" : "Edit antenna"; font.bold: true; color: "#e6edf7"; Layout.fillWidth: true }
                PC3.ToolButton { icon.name: "window-close"; onClicked: { map.editAnchor = null; overlay.requestPaint() } }
            }
            PC3.Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                text: "Drag the white crosshair onto the antenna. Pointing precision at this zoom: ±" + (anchorPanel.ea.accM || 1) + " m" + (map.zoom < 19 ? " — zoom in, satellite view helps." : ".")
            }
            PC3.TextField {
                Layout.fillWidth: true; placeholderText: "Name, e.g. Wi-Fi antenna"
                text: anchorPanel.ea.name || ""
                onTextEdited: map.patchAnchor({name: text})
            }
            QQC2.ComboBox {
                Layout.fillWidth: true
                model: map.anchorKinds; textRole: "t"
                currentIndex: { for (var i = 0; i < map.anchorKinds.length; i++) if (map.anchorKinds[i].k === anchorPanel.ea.kind) return i; return 0 }
                onActivated: index => map.patchAnchor({kind: map.anchorKinds[index].k, name: anchorPanel.ea.name || (map.anchorKinds[index].k === "this-computer" ? "Wi-Fi antenna" : "")})
            }
            RowLayout {
                Layout.fillWidth: true
                PC3.Label { text: "Height above ground"; color: "#c9d4e5"; Layout.fillWidth: true }
                QQC2.SpinBox {
                    from: 0; to: 300; stepSize: 1; editable: true
                    value: Math.round((anchorPanel.ea.heightM !== undefined ? anchorPanel.ea.heightM : 1) * 10)
                    textFromValue: function(v) { return (v / 10).toFixed(1) + " m" }
                    valueFromText: function(t) { return Math.round(parseFloat(t) * 10) || 0 }
                    onValueModified: map.patchAnchor({heightM: value / 10})
                }
            }
            QQC2.CheckBox {
                text: "Moves with the RV"; checked: !!anchorPanel.ea.rv
                onToggled: map.patchAnchor({rv: checked})
                PC3.ToolTip.text: "Kept relative to this computer's antenna, so it follows the RV to the next site"
                PC3.ToolTip.visible: hovered
            }
            RowLayout {
                visible: anchorPanel.wantsRadios
                Layout.fillWidth: true
                PC3.Label { text: "Radios on this antenna (" + (anchorPanel.ea.bssids || []).length + ")"; color: "#c9d4e5"; Layout.fillWidth: true }
                PC3.Button { text: "The RV router's radios"; onClicked: map.selectHomeRadios() }
            }
            ListView {
                id: radioList
                visible: anchorPanel.wantsRadios
                Layout.fillWidth: true; Layout.preferredHeight: Math.min(150, contentHeight); clip: true
                model: !anchorPanel.wantsRadios ? [] : (map.src.aps || []).slice().sort(function(a, b) { return b.dbm - a.dbm }).slice(0, 40)
                delegate: QQC2.CheckDelegate {
                    required property var modelData
                    width: radioList.width; height: 24; padding: 2
                    background: Rectangle { color: parent.hovered ? Qt.rgba(1, 1, 1, 0.08) : "transparent"; radius: 4 }
                    checked: (map.editAnchor && map.editAnchor.bssids || []).indexOf(String(modelData.bssid).toUpperCase()) >= 0
                    onClicked: map.toggleBssid(String(modelData.bssid).toUpperCase())
                    contentItem: PC3.Label {
                        text: (modelData.ssid || "(hidden)") + "  ·  " + modelData.bssid + "  ·  " + modelData.dbm + " dBm" + ((modelData.home || modelData.status === "home") ? "  · home" : "")
                        elide: Text.ElideRight; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; color: "#e6edf7"; leftPadding: 26; verticalAlignment: Text.AlignVCenter
                    }
                }
            }
            PC3.Label {
                Layout.fillWidth: true; color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; wrapMode: Text.Wrap
                text: (anchorPanel.ea.lat !== undefined ? anchorPanel.ea.lat.toFixed(6) + ", " + anchorPanel.ea.lon.toFixed(6) : "")
                      + (map.antennasSupported ? "" : "\nSaved in this widget; it reaches BeaconFix as soon as the desktop app is updated.")
            }
            RowLayout {
                Layout.fillWidth: true
                PC3.Button { text: "Save"; icon.name: "document-save"; onClicked: map.saveAnchor() }
                PC3.Button { text: "Satellite, zoom in"; icon.name: "zoom-in"; onClicked: { map.layerPicked(2); var p = map.anchorScreen(anchorPanel.ea); map.zoomAt(Math.max(0, 19.5 - map.zoom), p.x, p.y) } }
                Item { Layout.fillWidth: true }
                PC3.Button { visible: !anchorPanel.ea.isNew; text: "Delete"; icon.name: "edit-delete"; onClicked: map.deleteAnchor() }
            }
        }
    }
    // ── Unset Mesh Nodes alert banner ─────────────────────────────────────
    Rectangle {
        id: unsetNodesBanner
        visible: (map.src.unsetNodes || []).length > 0
        anchors {
            horizontalCenter: parent.horizontalCenter
            top: parent.top
            topMargin: Kirigami.Units.smallSpacing + 6
        }
        z: 22
        radius: 14
        color: Qt.rgba(0.08, 0.12, 0.20, 0.95)
        border.color: "#ffd166"
        border.width: 1.2
        implicitWidth: unsetRow.implicitWidth + 24
        implicitHeight: 28

        RowLayout {
            id: unsetRow
            anchors.centerIn: parent
            spacing: 8
            PC3.Label {
                text: "📡"
                font.pixelSize: 12
            }
            PC3.Label {
                text: {
                    var un = map.src.unsetNodes || []
                    if (un.length === 1) return `${un[0].name} (Position Unset)`
                    return `${un.length} Nodes Online (Position Unset)`
                }
                color: "#ffd166"
                font.bold: true
                font.pixelSize: Kirigami.Theme.smallFont.pixelSize
            }
            Rectangle {
                width: 1; height: 14; color: Qt.rgba(1, 1, 1, 0.2)
            }
            PC3.Label {
                text: "Click to Place at View Center"
                color: "#35d6ff"
                font.bold: true
                font.pixelSize: Kirigami.Theme.smallFont.pixelSize
            }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            hoverEnabled: true
            onClicked: {
                var un = map.src.unsetNodes || []
                if (un.length > 0) {
                    var node = un[0]
                    var lat = map.latOf(map.cy)
                    var lon = map.lonOf(map.cx)
                    map.placeMeshNode(node.name, lat, lon)
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
        property real mx: 0.5                   // the clicked spot, Web-Mercator
        property real my: 0.5
        PC3.MenuItem { text: "Replay last refinement"; enabled: map.lastRefit !== null; onTriggered: map.replayRefit() }
        PC3.MenuItem { text: "Linked devices"; checkable: true; checked: map.showDevices; onTriggered: { map.showDevices = !map.showDevices; overlay.requestPaint() } }
        PC3.MenuItem {
            text: "📡 Place Mesh Node Here…"
            icon.name: "network-wireless"
            visible: (map.src.unsetNodes || []).length > 0 || (map.src.meshNodes || []).length > 0
            onTriggered: {
                var un = map.src.unsetNodes || []
                var nodeToPlace = un.length > 0 ? un[0].name : "Mesh Node"
                var lat = map.latOf(ctxMenu.my), lon = map.lonOf(ctxMenu.mx)
                map.placeMeshNode(nodeToPlace, lat, lon)
            }
        }
        PC3.MenuItem { text: "Place an antenna here…"; icon.name: "network-wireless"; onTriggered: map.ctxPlaceAntenna() }
        PC3.MenuSeparator {}
        PC3.MenuItem { text: "Centre here"; icon.name: "zoom-fit-best"; onTriggered: map.ctxCentre() }
        PC3.MenuItem { text: "Zoom in here"; icon.name: "zoom-in"; onTriggered: map.ctxZoomIn() }
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
        PC3.MenuSeparator {}                    // satellite sources: flat items (a nested PC3.Menu warns in Plasma's delegate)
        PC3.MenuItem { text: "Satellite · Esri World Imagery (newest high-res)"; checkable: true; checked: map.layerIndex === 2 && map.satSource === 0; onTriggered: map.satSourcePicked(0) }
        PC3.MenuItem { text: "Satellite · Esri Clarity (sharper, can be older)"; checkable: true; checked: map.layerIndex === 2 && map.satSource === 1; onTriggered: map.satSourcePicked(1) }
        PC3.MenuItem { text: "Satellite · USGS National Map (US, public domain)"; checkable: true; checked: map.layerIndex === 2 && map.satSource === 2; onTriggered: map.satSourcePicked(2) }
        PC3.MenuItem { text: "Satellite · NASA VIIRS (yesterday's pass, coarse)"; checkable: true; checked: map.layerIndex === 2 && map.satSource === 3; onTriggered: map.satSourcePicked(3) }
        PC3.MenuItem { text: "Contour lines (US)"; checkable: true; checked: map.showContours; enabled: map.layerIndex === 2; onTriggered: map.contoursToggled(!map.showContours) }
        PC3.MenuSeparator {}
        PC3.MenuItem {
            text: "Route Heatmap"
            checkable: true
            checked: map.showHeatmap
            onTriggered: { map.showHeatmap = !map.showHeatmap; overlay.requestPaint() }
        }
        PC3.MenuItem {
            text: `Heatmap Opacity: ${Math.round(map.heatmapOpacity * 100)}%`
            enabled: map.showHeatmap
            onTriggered: {
                var ops = [0.25, 0.35, 0.45, 0.60, 0.75, 1.0]
                var curIdx = 2
                for (var i = 0; i < ops.length; i++) {
                    if (Math.abs(ops[i] - map.heatmapOpacity) < 0.04) { curIdx = i; break }
                }
                var nextOp = ops[(curIdx + 1) % ops.length]
                map.heatmapOpacity = nextOp
                Plasmoid.configuration.heatmapOpacity = nextOp
                overlay.requestPaint()
            }
        }
        PC3.MenuItem {
            text: "Flock & Surveillance Cameras"
            checkable: true
            checked: map.showCameras
            onTriggered: { map.showCameras = !map.showCameras; overlay.requestPaint() }
        }
    }
    PC3.Menu {
        id: placesMenu
        // Each group submenu is wrapped in an item made from this menu's delegate before that item has a parent;
        // Plasma's default delegate (width: parent.width) then logged "Menu.qml:30:26: TypeError: Cannot read
        // property 'width' of null" once per submenu at every start. The same delegate, null-safe.
        delegate: PC3.MenuItem {
            width: parent ? parent.width : implicitWidth
            onImplicitWidthChanged: placesMenu.contentItem.contentItem.childrenChanged()
        }
        PC3.MenuItem { text: "Show all"; onTriggered: map.allCategories(true) }
        PC3.MenuItem { text: "Hide all"; onTriggered: map.allCategories(false) }
        PC3.MenuSeparator {}
        // One submenu per group (Emergency & civic · Kids & fun · Services), each with show/hide-all
        Instantiator {
            model: ["civic", "kids", "services"]
            delegate: PC3.Menu {
                id: groupMenu
                required property string modelData
                title: modelData === "civic" ? "🚔 Emergency & civic" : modelData === "kids" ? "🛝 Kids & fun" : "⛽ Services"
                PC3.MenuItem { text: "Show all in this group"; onTriggered: map.groupToggled(groupMenu.modelData, true) }
                PC3.MenuItem { text: "Hide all in this group"; onTriggered: map.groupToggled(groupMenu.modelData, false) }
                PC3.MenuSeparator {}
                Instantiator {
                    id: groupItems
                    readonly property string grp: groupMenu.modelData
                    model: (map.src.poiCategories || []).filter(function(c) { return (c.group || "services") === groupItems.grp })
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
                    onObjectAdded: (index, object) => groupMenu.insertItem(index + 3, object)
                    onObjectRemoved: (index, object) => groupMenu.removeItem(object)
                }
            }
            onObjectAdded: (index, object) => placesMenu.insertMenu(index + 3, object)
            onObjectRemoved: (index, object) => placesMenu.removeMenu(object)
        }
    }

    // ── scale + attribution ─────────────────────────────────────────────────
    Rectangle {
        id: scaleBox
        readonly property var step: {             // follows the layers' reference camera: not re-built every frame
            var steps = [0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000]
            var m = map.rmpp(), best = steps[0]
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
            // metres and feet: pinpointing works in both (1 ft = 30.48 cm)
            text: (scaleBox.step.m >= 1000 ? scaleBox.step.m / 1000 + " km" : scaleBox.step.m >= 1 ? scaleBox.step.m + " m" : Math.round(scaleBox.step.m * 100) + " cm")
                  + " · " + (scaleBox.step.m >= 1609.344 ? (scaleBox.step.m / 1609.344).toFixed(scaleBox.step.m >= 16093 ? 0 : 1) + " mi"
                           : scaleBox.step.m >= 0.3048 ? Math.round(scaleBox.step.m / 0.3048 * 10) / 10 + " ft" : (scaleBox.step.m / 0.0254).toFixed(1) + " in")
            color: "#e6edf7"; font.pixelSize: 10
        }
    }
    // Attribution for the layer actually shown: short on the map, the providers' full credit lines (their
    // services' copyrightText) on hover, a click opens the licence / terms page.
    readonly property var attribution: {
        var osm = "© OpenStreetMap contributors"
        var esriLabels = "Labels: Esri, HERE, Garmin, © OpenStreetMap contributors, and the GIS user community"
        var hybrid = "Roads: Esri, HERE, Garmin, © OpenStreetMap contributors" + (showContours ? "\nContours: USGS The National Map (public domain)" : "")
        esriLabels += "\n" + hybrid
        var imagery = {t: "Powered by Esri · Esri, Vantor, Earthstar Geographics" + (showContours ? " · USGS" : ""),
                       full: "Imagery: Esri, Vantor, Earthstar Geographics, and the GIS User Community\n" + esriLabels,
                       url: "https://developers.arcgis.com/documentation/esri-and-data-attribution/"}
        if (layerIndex === 2 && satSource === 2) return {t: "USGS The National Map: orthoimagery (public domain)", full: "Imagery: U.S. Geological Survey, The National Map (USDA NAIP and others), public domain\n" + esriLabels,
                                                         url: "https://www.usgs.gov/programs/national-geospatial-program/national-map"}
        if (layerIndex === 2 && satSource === 3) return {t: "NASA EOSDIS GIBS · VIIRS NOAA-20, " + viirsDate(), full: "Imagery: NASA Worldview / GIBS, part of NASA's Earth Science Data and Information System (ESDIS); VIIRS on NOAA-20, corrected reflectance, " + viirsDate() + "\n" + esriLabels,
                                                         url: "https://nasa-gibs.github.io/gibs-api-docs/"}
        if (layerIndex === 2) return imagery
        if (tileBase) {
            if (layerIndex === 3) return {t: "© OpenStreetMap contributors, SRTM · © OpenTopoMap (CC-BY-SA)",
                                          full: "Map data: © OpenStreetMap contributors, SRTM\nMap style: © OpenTopoMap (CC-BY-SA 3.0)",
                                          url: "https://opentopomap.org/about"}
            return {t: osm, full: "Map data © OpenStreetMap contributors (ODbL)" + (layerIndex === 0 ? ", night-filtered by BeaconFix" : ""),
                    url: "https://www.openstreetmap.org/copyright"}
        }
        if (layerIndex === 3) return {t: "Powered by Esri · Esri, HERE, Garmin, USGS, © OpenStreetMap contributors",
                                      full: "Sources: Esri, HERE, Garmin, Intermap, INCREMENT P, GEBCO, USGS, FAO, NPS, NRCAN, GeoBase, IGN, Kadaster NL, Ordnance Survey, Esri Japan, METI, Esri China (Hong Kong), © OpenStreetMap contributors, and the GIS User Community",
                                      url: "https://developers.arcgis.com/documentation/esri-and-data-attribution/"}
        if (layerIndex === 1) return {t: "Powered by Esri · Esri, HERE, Garmin, USGS, © OpenStreetMap contributors",
                                      full: "Sources: Esri, HERE, Garmin, USGS, Intermap, INCREMENT P, NRCan, Esri Japan, METI, Esri China (Hong Kong), Esri Korea, Esri (Thailand), NGCC, © OpenStreetMap contributors, and the GIS User Community",
                                      url: "https://developers.arcgis.com/documentation/esri-and-data-attribution/"}
        return {t: "Powered by Esri · Esri, HERE, Garmin, © OpenStreetMap contributors",
                full: "Sources: Esri, HERE, Garmin, © OpenStreetMap contributors, and the GIS User Community",
                url: "https://developers.arcgis.com/documentation/esri-and-data-attribution/"}
    }
    PC3.Label {
        id: attrLabel
        anchors { right: parent.right; bottom: parent.bottom; margins: 3 }
        width: Math.min(implicitWidth, Math.max(60, map.width - scaleBox.width - 3 * Kirigami.Units.smallSpacing))
        horizontalAlignment: Text.AlignRight
        elide: Text.ElideLeft
        text: map.attribution.t
        color: Qt.rgba(0.9, 0.93, 0.97, attrHover.containsMouse ? 0.9 : 0.55); font.pixelSize: 9
        MouseArea {                             // takes the press itself: a (passive) TapHandler let the click through to pan too
            id: attrHover
            anchors.fill: parent
            hoverEnabled: true; acceptedButtons: Qt.LeftButton; cursorShape: Qt.PointingHandCursor
            onClicked: Qt.openUrlExternally(map.attribution.url)
        }
        QQC2.ToolTip.text: map.attribution.full; QQC2.ToolTip.visible: attrHover.containsMouse; QQC2.ToolTip.delay: 400
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
                for (var j = 0; j < Math.min(8, ids.length); j++) { var b = aps[ids[j]]; var bg = map.gradeOf(b); ll.push(`${b.ssid || "(hidden)"}  ${b.dbm} dBm · ${b.status}` + (bg ? " · " + bg : "")) }
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
                    onPressed: map.userTouched()
                    onClicked: Qt.openUrlExternally(map.directionsUrl(card.info.poi))
                }
                PC3.ToolButton { visible: !!(card.info && card.info.poi && card.info.poi.osm); icon.name: "internet-web-browser"; text: "OSM"; onPressed: map.userTouched(); onClicked: Qt.openUrlExternally(card.info.poi.osm) }
                PC3.ToolButton {
                    visible: !!(card.info && card.info.poi && card.info.poi.website)
                    icon.name: "globe"; text: "Website"; onPressed: map.userTouched(); onClicked: Qt.openUrlExternally(card.info.poi.website)
                }
            }
        }
    }
    // openstreetmap.org directions from the fix to a place: route=<lat>,<lon>;<lat>,<lon>, encoded as the site does
    // itself (%2C, %3B). toFixed() never uses the locale's decimal comma; 6 decimals are ~0.1 m. Without a fix
    // the start is left empty for the user to fill in (not 0,0 in the Gulf of Guinea).
    function directionsUrl(p) {
        var from = src.valid ? Number(src.lat).toFixed(6) + "," + Number(src.lon).toFixed(6) : ""
        return "https://www.openstreetmap.org/directions?engine=fossgis_osrm_car&route="
               + encodeURIComponent(from + ";" + Number(p.lat).toFixed(6) + "," + Number(p.lon).toFixed(6))
    }
    function beaconInfo(a, pinned) {
        if (!a) return null
        var f = a.fit || null
        var how = a.kind === "wigle" ? "mapped position (WiGLE / Apple)" : a.kind === "centroid" ? `multilaterated from ${a.vantage || 2} places, ±${map.distText(a.r)}`
                : a.kind === "region" ? `somewhere in this region, ±${map.distText(a.r95 || a.r || 0)} (95 %)`
                : a.kind === "mobile" ? "moves around — shown where it was last heard"
                : f && f.kind === "fix" && a.kind !== "ring" ? `fitted from ${map.plural(f.vantage || f.n || 0, "place")}, ±${map.distText(a.r95 || f.r95 || a.r)} (95 %)`
                : `~${map.distText(a.r)} away by signal — direction unknown`
        var band = map.bandOf(a) ? map.bandOf(a) + " GHz" : "", ch = a.ch ? ` ch ${a.ch}` : ""
        var st = a.status === "used" ? "used for the fix" : a.status === "active" ? "connected · travels with you"
               : a.status === "travelling" ? "travels with you" : a.status === "ignored" ? "ignored" : (a.status || "")
        var lines = [`${a.bssid}${band ? " · " + band + ch : ""} · ${a.dbm} dBm`, st, how]
        var gl = map.gradeLine(a)
        if (gl) lines.splice(1, 0, gl)
        if (f && pinned) {                          // the fit's caveats and what would help, in full when pinned
            var fl = (f.flags || []).map(function(x) { return x === "extrapolated" ? "outside the places it was heard from" : x === "ambiguous" ? "a second solution fits too (hollow ghost)"
                                                           : x === "moved" ? "may have moved" : x === "fragile" ? "rests on a few samples" : x === "rangeScale" ? "range scale uncertain" : x })
            if (fl.length) lines.push("⚠ " + fl.join(" · "))
            if (f.pendingGrade && f.pendingGrade !== map.gradeOf(a)) lines.push("Grade moving towards " + f.pendingGrade)
            if (f.suggest) lines.push("◎ Sampling at the target on the map would tighten it most")
        }
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
    // Desktop 3.8 fields (each optional; an older desktop's detail line already says "emergency dept." / "no ER"):
    // er "yes" | "no", peds tier 1-4, campusEr, driveS / driveEst.
    function erStatus(p) {
        if (!p || (p.er === undefined && p.peds === undefined && p.cat !== "peds_er" && p.cat !== "peds_urgent")) return ""
        if (p.cat === "peds_urgent" || p.peds === 4) return "Not an ER"
        if (p.er === "no") return "No ER"
        if (p.er === "yes") return p.peds === 3 ? "ER · pediatrics dept." : "ER"
        if (p.cat === "peds_er") return p.campusEr ? `ER on campus: ${p.campusEr} — call ahead` : "ER not confirmed — call ahead"
        return ""
    }
    function driveText(p) {
        if (!p || !(p.driveS > 0)) return ""
        var m = Math.max(1, Math.round(p.driveS / 60))
        return "~" + (m < 60 ? m + " min" : Math.floor(m / 60) + " h" + (m % 60 ? " " + (m % 60) + " min" : "")) + (p.driveEst === false ? " drive" : " drive (est.)")
    }
    function poiInfo(p, pinned) {
        if (!p) return null
        var eta = driveText(p)
        var lines = [`${p.label} · ${distText(p.d || 0)} ${compass(p.brg || 0)}` + (eta ? " · " + eta : "") + (src.source === "ip" ? " (from IP estimate)" : "")]
        var st = erStatus(p)
        if (st && (p.detail || "").toLowerCase().indexOf(st.toLowerCase()) < 0) lines.push((/call ahead|not an er|no er/i.test(st) ? "⚠ " : "🏥 ") + st)
        if (p.detail) lines.push(p.detail)
        if (p.hours) lines.push("🕑 " + p.hours)
        if (p.phone) lines.push("☎ " + p.phone)
        if (p.address) lines.push("📍 " + p.address)
        if (!pinned) lines.push("Click for directions")
        return {title: `${p.icon} ${p.name || p.label}`, lines: lines, color: p.color, poi: p, pinned: pinned}
    }

    Component.onCompleted: { updateRouteSegments(); updateCameraIndex(); recenter(); cluster(); rebaseItems(); refreshTiles() }
}
