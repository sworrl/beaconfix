import QtQuick
import org.kde.kirigami as Kirigami

// Beacons by distance around us: ring = RSSI distance (bearing unknown),
// gold = real/estimated position with its true bearing.
Canvas {
    id: radar
    required property var src
    property real phase: 0
    property bool showSsids: true
    property var  flashes: ({})                 // bssid → when it lit up (new / louder)
    property int  flashCount: 0
    property real now: 0
    function flash(bssid) { if (!bssid) return; flashes[bssid] = Date.now(); flashCount++; now = Date.now() }
    Timer {
        interval: 33; repeat: true; running: radar.flashCount > 0
        onTriggered: {
            radar.now = Date.now()
            var live = 0
            for (var b in radar.flashes) { if (radar.now - radar.flashes[b] > 1600) delete radar.flashes[b]; else live++ }
            radar.flashCount = live
            radar.requestPaint()
        }
    }
    Connections {
        target: radar.src
        function onNewEvents(list, animate) {
            if (!animate) return
            for (var i = 0; i < list.length; i++) if (list[i] && (list[i].type === "ap_new" || list[i].type === "ap_up")) radar.flash(list[i].bssid)
        }
    }
    // The sweep follows the shared 2.4 s pulse, which ticks every frame; repaint at ≤ 20 fps and only when shown.
    property bool _paintQueued: false
    Timer { id: sweepThrottle; interval: 50; onTriggered: { radar._paintQueued = false; if (radar.visible) radar.requestPaint() } }
    onPhaseChanged: if (visible && !_paintQueued) { _paintQueued = true; sweepThrottle.start() }
    onVisibleChanged: if (visible) requestPaint()
    onShowSsidsChanged: requestPaint()
    onPaint: {
        var ctx = getContext("2d")
        ctx.reset()
        var w = width, h = height, cx = w / 2, cy = h / 2
        var R = Math.min(w, h) / 2 - 8
        var aps = src.aps || [], blips = [], now = radar.now
        var maxR = 40
        for (var i = 0; i < aps.length; i++) if (aps[i].kind === "ring") maxR = Math.max(maxR, aps[i].r || 0)
        maxR = Math.min(maxR, 600) * 1.08
        var scale = R / maxR
        ctx.strokeStyle = "rgba(53,214,255,0.18)"; ctx.lineWidth = 1
        ctx.fillStyle = "rgba(159,176,200,0.6)"; ctx.font = (Kirigami.Theme.smallFont.pixelSize - 1) + "px sans-serif"
        var steps = [0.25, 0.5, 0.75, 1.0]
        for (var s = 0; s < steps.length; s++) {
            var rr = R * steps[s]
            ctx.beginPath(); ctx.arc(cx, cy, rr, 0, Math.PI * 2); ctx.stroke()
            ctx.fillText(Math.round(maxR * steps[s] / 1.08) + " m", cx + 4, cy - rr + 11)
        }
        var a = phase * Math.PI * 2
        ctx.beginPath(); ctx.moveTo(cx, cy); ctx.arc(cx, cy, R, a - 0.9, a); ctx.closePath()
        ctx.fillStyle = "rgba(53,214,255,0.10)"; ctx.fill()
        ctx.strokeStyle = "rgba(53,214,255,0.5)"; ctx.beginPath(); ctx.moveTo(cx, cy)
        ctx.lineTo(cx + Math.cos(a) * R, cy + Math.sin(a) * R); ctx.stroke()
        for (var k = 0; k < aps.length; k++) {
            var ap = aps[k]
            if (ap.kind === "none") continue
            var col = ap.status === "used" ? (ap.kind === "ring" ? "#35d6ff" : "#ffd166")
                    : ap.status === "active" ? "#6cff8a" : ap.status === "travelling" ? "#ff4fd8" : "#8a93a6"
            var dist, ang
            if (ap.kind === "ring") { dist = ap.r; ang = (ap.bearing - 90) * Math.PI / 180 }
            else {
                var dLat = (ap.lat - src.lat) * 111320
                var dLon = (ap.lon - src.lon) * 111320 * Math.cos(src.lat * Math.PI / 180)
                dist = Math.sqrt(dLat * dLat + dLon * dLon); ang = Math.atan2(-dLat, dLon)
            }
            var px = cx + Math.cos(ang) * Math.min(dist, maxR) * scale
            var py = cy + Math.sin(ang) * Math.min(dist, maxR) * scale
            var rad = ap.status === "used" || ap.status === "active" ? 4 : 2.5
            ctx.fillStyle = col; ctx.globalAlpha = 0.25
            ctx.beginPath(); ctx.arc(px, py, rad * 2.6, 0, Math.PI * 2); ctx.fill()
            ctx.globalAlpha = 1
            ctx.beginPath(); ctx.arc(px, py, rad, 0, Math.PI * 2); ctx.fill()
            if (ap.kind !== "ring") { ctx.strokeStyle = "#ffffff"; ctx.lineWidth = 1; ctx.stroke() }
            var f = radar.flashes[ap.bssid]
            if (f !== undefined && now - f < 1600) {                 // just heard / got louder: a ring bursts out of the blip
                var fp = (now - f) / 1600
                ctx.globalAlpha = 1 - fp; ctx.strokeStyle = col; ctx.lineWidth = 2
                ctx.beginPath(); ctx.arc(px, py, 4 + 22 * (1 - Math.pow(1 - fp, 3)), 0, Math.PI * 2); ctx.stroke()
                ctx.globalAlpha = 1
            }
            blips.push({x: px, y: py, dbm: ap.dbm, ssid: ap.ssid || "", col: col})
        }
        // names for the strongest blips when there is room
        if (radar.showSsids && Math.min(w, h) >= 220) {
            blips.sort(function(a, b) { return b.dbm - a.dbm })
            var taken = []
            ctx.textBaseline = "middle"; ctx.textAlign = "left"
            for (var n = 0, made = 0; n < blips.length && made < 8; n++) {
                var bl = blips[n]
                var name = bl.ssid ? (bl.ssid.length > 16 ? bl.ssid.slice(0, 15) + "…" : bl.ssid) : "(hidden)"
                ctx.font = bl.ssid ? "10px sans-serif" : "italic 10px sans-serif"
                var tw = ctx.measureText(name).width + 8, th = 14
                var lx = bl.x + 7, ly = bl.y - 7
                if (lx + tw > w - 2) lx = bl.x - 7 - tw
                var ok = true
                for (var t = 0; t < taken.length && ok; t++) { var r = taken[t]; if (lx < r.x + r.w && lx + tw > r.x && ly < r.y + th && ly + th > r.y) ok = false }
                if (!ok) continue
                taken.push({x: lx, y: ly, w: tw, h: th}); made++
                ctx.beginPath(); ctx.roundedRect(lx, ly, tw, th, 3, 3); ctx.fillStyle = "rgba(8,13,20,0.75)"; ctx.fill()
                ctx.fillStyle = bl.col; ctx.fillText(name, lx + 4, ly + th / 2)
            }
            ctx.textBaseline = "alphabetic"
        }
        ctx.beginPath(); ctx.fillStyle = "rgba(53,214,255,0.25)"; ctx.arc(cx, cy, 10 + 14 * phase, 0, Math.PI * 2); ctx.fill()
        ctx.beginPath(); ctx.fillStyle = "#35d6ff"; ctx.arc(cx, cy, 5, 0, Math.PI * 2); ctx.fill()
        ctx.beginPath(); ctx.fillStyle = "#ffffff"; ctx.arc(cx, cy, 2, 0, Math.PI * 2); ctx.fill()
    }
}
