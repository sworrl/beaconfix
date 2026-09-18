import QtQuick
import QtQuick.Layouts
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami
import org.kde.plasma.plasma5support as Plasma5Support

PlasmoidItem {
    id: root

    readonly property int    pollSecs:  Plasmoid.configuration.pollSeconds
    readonly property bool   showPlace: Plasmoid.configuration.showPlaceInPanel
    readonly property string bin:
        'PATH="$HOME/.local/bin:/usr/local/bin:$PATH" ' + (Plasmoid.configuration.binary || "beaconfix")

    property bool   valid:    false
    property double lat:      0
    property double lon:      0
    property double accuracy: -1
    property string source:   ""
    property string place:    ""
    property string time:     ""
    property int    apCount:  0
    property string error:    ""
    property bool   busy:     false
    property var    aps:      []
    property var    stats:    ({})

    readonly property string sourceName:
        source === "starlink" ? "Starlink dish GPS" :
        source === "wifi"     ? "BeaconDB Wi-Fi" :
        source === "ip"       ? "IP geolocation (approximate)" : "no fix"
    readonly property string sourceIcon: "beaconfix"
    readonly property color  sourceColor:
        source === "starlink" ? "#6cff8a" : source === "wifi" ? "#35d6ff" : source === "ip" ? "#ffd166" : "#ff4f4f"

    Plasmoid.icon: sourceIcon
    toolTipMainText: valid ? place : "BeaconFix"
    toolTipSubText:  valid ? `±${Math.round(accuracy)} m · ${sourceName} · ${ageText()}` : (error || "No location yet")

    function ageText() {
        if (!time) return "never"
        var s = (Date.now() - Date.parse(time)) / 1000
        if (s < 90) return "just now"
        if (s < 3600) return `${Math.floor(s / 60)} min ago`
        if (s < 86400) return `${Math.floor(s / 3600)} h ago`
        return new Date(time).toLocaleString(Qt.locale(), "ddd d MMM HH:mm")
    }

    Plasma5Support.DataSource {
        id: exec
        engine: "executable"
        connectedSources: []
        onNewData: function(sourceName, data) {
            disconnectSource(sourceName)
            if (sourceName.indexOf("--json") < 0) return
            try {
                var d = JSON.parse((data["stdout"] || "").trim())
                root.valid = !!d.valid; root.lat = d.lat || 0; root.lon = d.lon || 0
                root.accuracy = d.accuracy !== undefined ? d.accuracy : -1
                root.source = d.source || ""; root.place = d.place || ""; root.time = d.time || ""
                root.apCount = d.apCount || 0; root.error = d.error || ""; root.busy = !!d.busy
                root.aps = d.aps || []; root.stats = d.stats || {}
                radar.requestPaint()
            } catch(e) { root.error = "beaconfix --json failed" }
        }
    }
    function poll()    { exec.connectSource(`${root.bin} --json`) }
    function refresh() { exec.connectSource(`${root.bin} --refresh`); refreshFollow.restart() }
    function openApp() { exec.connectSource(`${root.bin}`) }

    Timer { interval: root.pollSecs * 1000; running: true; repeat: true; triggeredOnStart: true; onTriggered: root.poll() }
    // After a manual refresh, poll a few times quickly so the new fix shows up promptly
    Timer { id: refreshFollow; interval: 4000; repeat: true; property int n: 0
            onRunningChanged: n = 0
            onTriggered: { root.poll(); if (++n >= 6) stop() } }
    Timer { interval: 30000; running: true; repeat: true; onTriggered: root.timeChanged() }   // re-evaluate age text

    compactRepresentation: MouseArea {
        id: compact
        readonly property bool vertical: Plasmoid.formFactor === PlasmaCore.Types.Vertical
        implicitWidth:  row.implicitWidth + Kirigami.Units.smallSpacing * 2
        implicitHeight: row.implicitHeight
        Layout.minimumWidth: implicitWidth
        onClicked: root.expanded = !root.expanded
        RowLayout {
            id: row
            anchors.centerIn: parent
            spacing: Kirigami.Units.smallSpacing
            Kirigami.Icon {
                source: root.sourceIcon
                implicitWidth:  Kirigami.Units.iconSizes.smallMedium
                implicitHeight: Kirigami.Units.iconSizes.smallMedium
                opacity: root.busy ? 0.5 : 1
            }
            PC3.Label {
                visible: root.showPlace && !compact.vertical && root.valid
                text: root.place.split(",")[0]
                elide: Text.ElideRight
                Layout.maximumWidth: Kirigami.Units.gridUnit * 12
            }
        }
    }

    fullRepresentation: Item {
        id: fullRep
        Layout.preferredWidth:  Kirigami.Units.gridUnit * 24
        Layout.preferredHeight: Kirigami.Units.gridUnit * 26
        Layout.minimumWidth:    Kirigami.Units.gridUnit * 18
        Layout.minimumHeight:   Kirigami.Units.gridUnit * 20

        // Pulse + sweep animation while the popup is open
        property real phase: 0
        NumberAnimation on phase { from: 0; to: 1; duration: 2400; loops: Animation.Infinite; running: root.expanded }
        onPhaseChanged: radar.requestPaint()

        Rectangle {
            anchors.fill: parent
            radius: Kirigami.Units.cornerRadius * 2
            color: "#0b101a"
            border.color: Qt.rgba(0.21, 0.84, 1, 0.35); border.width: 1
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.smallSpacing

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                Rectangle {
                    width: 10; height: 10; radius: 5; color: root.sourceColor
                    Rectangle { anchors.centerIn: parent; width: 10 + 16 * fullRep.phase; height: width; radius: width / 2
                                color: "transparent"; border.color: root.sourceColor; border.width: 1.5; opacity: 1 - fullRep.phase }
                }
                PC3.Label {
                    text: root.valid ? root.place : "Listening for beacons…"
                    color: "#e6edf7"; font.bold: true; font.pixelSize: Kirigami.Theme.defaultFont.pixelSize * 1.25
                    elide: Text.ElideRight; Layout.fillWidth: true
                }
                Rectangle {
                    visible: root.valid
                    radius: 9; color: root.sourceColor; height: 18; width: chip.implicitWidth + 16
                    PC3.Label { id: chip; anchors.centerIn: parent; color: "#0b101a"; font.bold: true
                                font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                                text: root.source === "starlink" ? "GPS" : root.source === "wifi" ? "WI-FI" : root.source === "ip" ? "IP" : "" }
                }
            }
            PC3.Label {
                text: root.valid
                      ? `${root.lat.toFixed(5)}, ${root.lon.toFixed(5)}  ·  ±${Math.round(root.accuracy)} m  ·  ${root.ageText()}`
                      : (root.error || "Waiting for BeaconFix…")
                color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                elide: Text.ElideRight; Layout.fillWidth: true
            }

            // ── Radar: beacons by distance, ring = RSSI distance, gold = located ──
            Canvas {
                id: radar
                Layout.fillWidth: true; Layout.fillHeight: true
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.reset()
                    var w = width, h = height, cx = w / 2, cy = h / 2
                    var R = Math.min(w, h) / 2 - 8
                    var aps = root.aps || []
                    var maxR = 40
                    for (var i = 0; i < aps.length; i++) maxR = Math.max(maxR, aps[i].r || 0)
                    maxR = Math.min(maxR, 600) * 1.08
                    var scale = R / maxR
                    // rings with distance labels
                    ctx.strokeStyle = "rgba(53,214,255,0.18)"; ctx.lineWidth = 1
                    ctx.fillStyle = "rgba(159,176,200,0.6)"; ctx.font = (Kirigami.Theme.smallFont.pixelSize - 1) + "px sans-serif"
                    var steps = [0.25, 0.5, 0.75, 1.0]
                    for (var s = 0; s < steps.length; s++) {
                        var rr = R * steps[s]
                        ctx.beginPath(); ctx.arc(cx, cy, rr, 0, Math.PI * 2); ctx.stroke()
                        ctx.fillText(Math.round(maxR * steps[s] / 1.08) + " m", cx + 4, cy - rr + 11)
                    }
                    // sweep
                    var a = fullRep.phase * Math.PI * 2
                    var g = ctx.createConicalGradient ? null : null
                    ctx.save(); ctx.beginPath(); ctx.moveTo(cx, cy); ctx.arc(cx, cy, R, a - 0.9, a); ctx.closePath()
                    ctx.fillStyle = "rgba(53,214,255,0.10)"; ctx.fill(); ctx.restore()
                    ctx.strokeStyle = "rgba(53,214,255,0.5)"; ctx.beginPath(); ctx.moveTo(cx, cy)
                    ctx.lineTo(cx + Math.cos(a) * R, cy + Math.sin(a) * R); ctx.stroke()
                    // beacons
                    for (var k = 0; k < aps.length; k++) {
                        var ap = aps[k]
                        var col = ap.status === "used" ? (ap.kind === "ring" ? "#35d6ff" : "#ffd166")
                                : ap.status === "active" ? "#6cff8a"
                                : ap.status === "travelling" ? "#ff4fd8" : "#8a93a6"
                        var dist, ang
                        if (ap.kind === "ring") { dist = ap.r; ang = (ap.bearing - 90) * Math.PI / 180 }
                        else {
                            // real/estimated position: true bearing + distance from us
                            var dLat = (ap.lat - root.lat) * 111320
                            var dLon = (ap.lon - root.lon) * 111320 * Math.cos(root.lat * Math.PI / 180)
                            dist = Math.sqrt(dLat * dLat + dLon * dLon); ang = Math.atan2(-dLat, dLon)
                        }
                        var px = cx + Math.cos(ang) * Math.min(dist, maxR) * scale
                        var py = cy + Math.sin(ang) * Math.min(dist, maxR) * scale
                        var rad = ap.status === "used" || ap.status === "active" ? 4 : 2.5
                        ctx.beginPath(); ctx.fillStyle = col.replace(")", "") ; ctx.globalAlpha = 0.25
                        ctx.fillStyle = col; ctx.arc(px, py, rad * 2.6, 0, Math.PI * 2); ctx.fill()
                        ctx.globalAlpha = 1
                        ctx.beginPath(); ctx.arc(px, py, rad, 0, Math.PI * 2); ctx.fill()
                        if (ap.kind !== "ring") { ctx.strokeStyle = "#ffffff"; ctx.lineWidth = 1; ctx.stroke() }
                    }
                    // us
                    ctx.beginPath(); ctx.fillStyle = "rgba(53,214,255,0.25)"; ctx.arc(cx, cy, 10 + 14 * fullRep.phase, 0, Math.PI * 2); ctx.fill()
                    ctx.beginPath(); ctx.fillStyle = "#35d6ff"; ctx.arc(cx, cy, 5, 0, Math.PI * 2); ctx.fill()
                    ctx.beginPath(); ctx.fillStyle = "#ffffff"; ctx.arc(cx, cy, 2, 0, Math.PI * 2); ctx.fill()
                }
            }

            // ── HUD line ───────────────────────────────────────────────────
            PC3.Label {
                Layout.fillWidth: true
                color: "#ffd166"; font.bold: true; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                text: root.stats && root.stats.rank
                      ? `${root.stats.rank.toUpperCase()} · Lv ${root.stats.rankLevel} · ${root.stats.beaconsTotal} beacons logged`
                      : ""
            }
            PC3.Label {
                Layout.fillWidth: true
                color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                text: root.stats && root.stats.rank
                      ? `In range ${root.stats.beaconsNow} · used ${root.stats.usedNow} · located ${root.stats.locatedNow} · travelling ${root.stats.travellingNow} · stops ${root.stats.stops} · ${(root.stats.distanceKm || 0).toFixed(1)} km`
                      : ""
                elide: Text.ElideRight
            }

            RowLayout {
                Layout.fillWidth: true
                PC3.Button { icon.name: "view-refresh"; text: "Re-check"; enabled: !root.busy; onClicked: root.refresh() }
                PC3.Button { icon.name: "internet-web-browser"; text: "Map"; enabled: root.valid
                    onClicked: Qt.openUrlExternally(`https://www.openstreetmap.org/?mlat=${root.lat}&mlon=${root.lon}#map=15/${root.lat}/${root.lon}`) }
                Item { Layout.fillWidth: true }
                PC3.Button { icon.name: "window"; text: "Open app"; onClicked: root.openApp() }
            }
        }
    }
}
