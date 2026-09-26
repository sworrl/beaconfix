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
    readonly property bool   showAccuracy: Plasmoid.configuration.showAccuracyInPanel
    readonly property bool   showMotion:   Plasmoid.configuration.showMotionInPanel
    readonly property bool   showSsids:    Plasmoid.configuration.showSsids
    readonly property bool   showEvents:   Plasmoid.configuration.showEvents
    readonly property var    tabKeys: {
        var k = []
        if (Plasmoid.configuration.showMapTab) k.push("map")
        if (Plasmoid.configuration.showNearbyTab) k.push("nearby")
        if (Plasmoid.configuration.showRadarTab) k.push("radar")
        if (Plasmoid.configuration.showTripTab) k.push("trip")
        return k.length ? k : ["map"]
    }
    readonly property string bin:
        'PATH="$HOME/.local/bin:/usr/local/bin:$PATH" ' + (Plasmoid.configuration.binary || "beaconfix")

    property bool   valid:    false
    property double lat:      0
    property double lon:      0
    property double accuracy: -1
    property string source:   ""
    property string provider: ""
    property string place:    ""
    property string time:     ""
    property int    apCount:  0
    property string error:    ""
    property bool   busy:     false
    property var    aps:      []
    property var    stats:    ({})
    property var    pois:     []
    property var    track:    []
    property var    knownDevices: []
    property var    securitySummary: null
    property var    poiCategories: []
    property string poiNote:  ""
    property string tileBase: ""
    property string _poisJson: ""
    property string _trackJson: ""
    property var    elevation: null
    property string elevationNote: ""
    property var    sun: null
    property var    share: null
    property var    events:   []                // recent things that happened (newest last)
    property int    lastEventId: 0
    property int    liveScanSeconds: 0
    property int    _seenEventId: -1            // -1 until the first read: never animate history
    property string _eventsJson: ""
    signal newEvents(var list, bool animate)    // events not seen before; animate=false on the first read
    readonly property var hiddenCats: Plasmoid.configuration.hiddenCategories || []

    function setCategory(key, visible) {
        var l = []
        for (var i = 0; i < hiddenCats.length; i++) if (hiddenCats[i] !== key) l.push(hiddenCats[i])
        if (!visible) l.push(key)
        Plasmoid.configuration.hiddenCategories = l
    }
    function setAllCategories(visible) {
        var l = []
        if (!visible) for (var i = 0; i < poiCategories.length; i++) l.push(poiCategories[i].key)
        Plasmoid.configuration.hiddenCategories = l
    }

    readonly property string sourceName:
        source === "starlink" ? "Starlink dish GPS" :
        source === "wifi" ? (provider === "apple" ? "Apple Wi-Fi" : "BeaconDB Wi-Fi") :
        source === "ip"       ? "IP geolocation (approximate)" : "no fix"
    readonly property string sourceIcon: "beaconfix"
    readonly property color  sourceColor:
        source === "starlink" ? "#6cff8a" : source === "wifi" ? "#35d6ff" : source === "ip" ? "#ffd166" : "#ff4f4f"

    Plasmoid.icon: sourceIcon
    toolTipMainText: valid ? place : "BeaconFix"
    toolTipSubText:  valid ? `±${Math.round(accuracy)} m · ${sourceName} · ${ageText()}`
                               + (stats && stats.rank ? `\n${stats.rank} · Lv ${stats.rankLevel} · today ${(stats.distanceTodayKm || 0).toFixed(1)} km · trip ${(stats.distanceTripKm || 0).toFixed(0)} km` : "")
                               + (elevation !== null ? `\n⛰ ${elevText()}` : "") + (sun && sun.sunrise ? ` · ☀ ${hm(sun.sunrise)} – ${hm(sun.sunset)}` : "")
                           : (error || "No location yet")

    function ageText() {
        if (!time) return "never"
        var s = (Date.now() - Date.parse(time)) / 1000
        if (s < 90) return "just now"
        if (s < 3600) return `${Math.floor(s / 60)} min ago`
        if (s < 86400) return `${Math.floor(s / 3600)} h ago`
        return new Date(time).toLocaleString(Qt.locale(), "ddd d MMM HH:mm")
    }

    // Header click → KWin "Show Desktop" (windows step aside, the widget stays; click again to restore).
    Plasma5Support.DataSource {
        id: shellRunner
        engine: "executable"
        connectedSources: []
        onNewData: function(sourceName) { disconnectSource(sourceName) }
    }
    function showDesktop() { shellRunner.connectSource('qdbus6 org.kde.kglobalaccel /component/kwin org.kde.kglobalaccel.Component.invokeShortcut "Show Desktop"') }

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
                root.source = d.source || ""; root.provider = d.provider || ""; root.place = d.place || ""; root.time = d.time || ""
                root.apCount = d.apCount || 0; root.error = d.error || ""; root.busy = !!d.busy
                root.aps = d.aps || []; root.stats = d.stats || {}
                // Only reassign the big arrays when they changed: that re-clusters and repaints the map
                var pj = JSON.stringify(d.pois || [])
                if (pj !== root._poisJson) { root._poisJson = pj; root.pois = d.pois || [] }
                var tj = JSON.stringify(d.track || [])
                if (tj !== root._trackJson) { root._trackJson = tj; root.track = d.track || [] }
                if (!root.poiCategories.length) root.poiCategories = d.poiCategories || []
                root.poiNote = d.poiNote || ""
                root.tileBase = d.tileBase || ""
                root.knownDevices = d.knownDevices || []
                root.securitySummary = d.securitySummary || null
                root.elevation = (d.elevation === undefined) ? null : d.elevation
                root.elevationNote = d.elevationNote || ""
                root.sun = d.sun || null
                root.share = d.share || null
                root.liveScanSeconds = d.liveScanSeconds || 0
                var ev = d.events || []
                var ej = JSON.stringify(ev)
                if (ej !== root._eventsJson) {
                    root._eventsJson = ej; root.events = ev
                    var fresh = [], maxId = root._seenEventId, first = root._seenEventId < 0
                    for (var i = 0; i < ev.length; i++) {
                        var id = (ev[i] && ev[i].id !== undefined) ? ev[i].id : i
                        if (id > maxId) maxId = id
                        if (!first && id > root._seenEventId) fresh.push(ev[i])
                    }
                    root._seenEventId = maxId
                    root.lastEventId = d.lastEventId !== undefined ? d.lastEventId : maxId
                    var out = first ? ev.slice(-5) : fresh
                    if (out.length) root.newEvents(out, !first)
                }
            } catch(e) { root.error = "beaconfix --json failed" }
        }
    }
    function poll()    { exec.connectSource(`${root.bin} --json`) }
    function refresh() { exec.connectSource(`${root.bin} --refresh`); refreshFollow.restart() }
    function openApp() { exec.connectSource(`${root.bin}`) }
    function copy(what) { exec.connectSource(`${root.bin} --copy ${what}`) }
    function saveGpx() { exec.connectSource(`${root.bin} --gpx "$HOME/Documents/beaconfix-trip.gpx"`) }
    function newTrip() { exec.connectSource(`${root.bin} --new-trip`); refreshFollow.restart() }
    function prefetch() { exec.connectSource(`${root.bin} --prefetch`) }
    function elevText() { return elevation === null ? "" : `${Math.round(elevation)} m` }
    function hm(iso) { return iso ? new Date(iso).toLocaleTimeString(Qt.locale(), "HH:mm") : "—" }

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
            Rectangle {
                visible: root.showAccuracy && !compact.vertical && root.valid
                radius: 7; height: 14; width: accChip.implicitWidth + 10; color: root.sourceColor
                PC3.Label { id: accChip; anchors.centerIn: parent; color: "#0b101a"; font.bold: true; font.pixelSize: Kirigami.Theme.smallFont.pixelSize - 1
                            text: (root.source === "starlink" ? "GPS " : root.source === "wifi" ? "WI-FI " : root.source === "ip" ? "IP " : "")
                                  + (root.accuracy >= 1000 ? Math.round(root.accuracy / 1000) + " km" : Math.round(root.accuracy) + " m") }
            }
            PC3.Label {
                visible: root.showMotion && !compact.vertical && root.valid && root.stats && root.stats.rank !== undefined
                text: root.stats.moving && root.stats.speedKmh >= 0 ? `→ ${Math.round(root.stats.speedKmh)} km/h ${root.stats.heading || ""}`
                      : root.elevation !== null ? `⛰ ${root.elevText()}` : ""
                color: root.stats.moving ? "#ff9f43" : Kirigami.Theme.textColor
                font.pixelSize: Kirigami.Theme.smallFont.pixelSize
            }
        }
    }

    fullRepresentation: Item {
        id: fullRep
        Layout.preferredWidth:  Kirigami.Units.gridUnit * 30
        Layout.preferredHeight: Kirigami.Units.gridUnit * 32
        Layout.minimumWidth:    Kirigami.Units.gridUnit * 20
        Layout.minimumHeight:   Kirigami.Units.gridUnit * 22

        // Pulse + sweep: a 2.4 s cycle stepped at 12 fps by a timer, not a NumberAnimation.
        // An infinite NumberAnimation makes Qt Quick re-render the whole desktop at 60 fps for
        // as long as the widget is on it, which on this VM's GL path costs most of a core.
        // …and on the desktop it only runs while the pointer is over the widget or for two
        // minutes after the last interaction or event, so an idle desktop stays idle.
        property real phase: 0
        property bool  awake: true
        readonly property bool onDesktop: Plasmoid.location === PlasmaCore.Types.Floating || Plasmoid.formFactor === PlasmaCore.Types.Planar
        HoverHandler { id: repHover; onHoveredChanged: if (hovered) fullRep.wake() }
        function wake() { awake = true; idleTimer.restart() }
        Timer { id: idleTimer; interval: 120000; onTriggered: if (!repHover.hovered) { fullRep.awake = false; fullRep.phase = 0 } }
        Connections { target: root; function onNewEvents(list, animate) { if (animate && list.length) fullRep.wake() } }
        Component.onCompleted: wake()
        Timer {
            interval: 83; repeat: true
            running: fullRep.visible && (root.expanded || (fullRep.onDesktop && fullRep.awake))
            onTriggered: fullRep.phase = (Date.now() % 2400) / 2400
        }

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
                TapHandler { acceptedButtons: Qt.LeftButton; onTapped: root.showDesktop() }
                HoverHandler { id: headerHover; cursorShape: Qt.PointingHandCursor }
                PC3.ToolTip { visible: headerHover.hovered; delay: 900; text: "Click: show the desktop (windows step aside). Click again to bring them back." }
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
                      ? `${root.lat.toFixed(5)}, ${root.lon.toFixed(5)}  ·  ±${root.accuracy >= 1000 ? (root.accuracy / 1000).toFixed(0) + " km" : Math.round(root.accuracy) + " m"}  ·  ${root.ageText()}`
                      : (root.error || "Waiting for BeaconFix…")
                color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                elide: Text.ElideRight; Layout.fillWidth: true
            }
            PC3.Label {
                visible: !!(root.valid && (root.elevation !== null || !!root.sun || (root.stats && root.stats.moving)))
                text: [root.elevation !== null ? `⛰ ${root.elevText()} · ${Math.round(root.elevation * 3.28084)} ft` : "",
                       root.sun && root.sun.sunrise ? `☀ ${root.hm(root.sun.sunrise)} – ${root.hm(root.sun.sunset)}` + (root.sun.goldenEveningStart ? ` · golden ${root.hm(root.sun.goldenEveningStart)}` : "") : (root.sun && root.sun.polarDay ? "☀ up all day" : root.sun && root.sun.polarNight ? "☀ down all day" : ""),
                       root.stats && root.stats.moving && root.stats.speedKmh >= 0 ? `→ ${Math.round(root.stats.speedKmh)} km/h ${root.stats.heading || ""}` : (root.stats && root.stats.dwellSecs > 0 ? `here ${Math.floor(root.stats.dwellSecs / 3600)} h ${Math.floor((root.stats.dwellSecs % 3600) / 60)} min` : "")
                      ].filter(function(s) { return !!s }).join("  ·  ")
                color: "#ffd166"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                elide: Text.ElideRight; Layout.fillWidth: true
            }

            PC3.TabBar {
                id: tabs
                Layout.fillWidth: true
                currentIndex: Math.min(Plasmoid.configuration.startTab, root.tabKeys.length - 1)
                onCurrentIndexChanged: Plasmoid.configuration.startTab = currentIndex
                Repeater {
                    model: root.tabKeys
                    delegate: PC3.TabButton {
                        required property string modelData
                        text: modelData === "map" ? "Map" : modelData === "nearby" ? `Nearby (${root.pois.length})` : modelData === "radar" ? "Radar" : "Trip"
                        icon.name: modelData === "map" ? "map-flat" : modelData === "nearby" ? "find-location" : modelData === "radar" ? "network-wireless" : "flag"
                    }
                }
            }

            StackLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                currentIndex: Math.max(0, ["map", "nearby", "radar", "trip"].indexOf(root.tabKeys[tabs.currentIndex] || "map"))
                Item {
                    Rectangle { anchors.fill: parent; radius: 6; color: "#0b101a" }
                    BeaconMap {
                        id: beaconMap
                        anchors.fill: parent
                        src: root
                        phase: fullRep.phase
                        layerIndex: Plasmoid.configuration.mapLayer
                        hiddenCats: root.hiddenCats
                        showSsids: root.showSsids
                        showEvents: root.showEvents
                        cinematic: Plasmoid.configuration.animatedMap
                        tourMinutes: Plasmoid.configuration.tourMinutes
                        onCinematicToggled: on => Plasmoid.configuration.animatedMap = on
                        onSsidsToggled: on => Plasmoid.configuration.showSsids = on
                        onEventsToggled: on => Plasmoid.configuration.showEvents = on
                        onLayerPicked: index => Plasmoid.configuration.mapLayer = index
                        onCategoryToggled: (key, visible) => root.setCategory(key, visible)
                        onAllCategories: visible => root.setAllCategories(visible)
                    }
                }
                NearbyList {
                    src: root
                    hiddenCats: root.hiddenCats
                    onShowOnMap: index => {
                        var p = root.pois[index]
                        if (root.hiddenCats.indexOf(p.cat) >= 0) root.setCategory(p.cat, true)
                        var mi = root.tabKeys.indexOf("map")
                        if (mi >= 0) tabs.currentIndex = mi
                        beaconMap.focusOn(p.lat, p.lon, Math.max(beaconMap.zoom, 16.5))
                        beaconMap.selected = index
                    }
                }
                Radar { src: root; phase: fullRep.phase; showSsids: root.showSsids }
                TripView { src: root }
            }

            // ── HUD line ───────────────────────────────────────────────────
            PC3.Label {
                Layout.fillWidth: true
                color: "#ffd166"; font.bold: true; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                text: root.stats && root.stats.rank
                      ? `${root.stats.rank.toUpperCase()} · Lv ${root.stats.rankLevel}${root.stats.rankCount ? "/" + root.stats.rankCount : ""} · ${root.stats.beaconsTotal} beacons logged${root.stats.nextRankAt ? " · next at " + root.stats.nextRankAt : ""} · ${root.stats.achievementsUnlocked || 0}/${root.stats.achievementsTotal || 0} milestones`
                      : ""
                elide: Text.ElideRight
            }
            PC3.Label {
                Layout.fillWidth: true
                color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                text: root.stats && root.stats.rank
                      ? `In range ${root.stats.beaconsNow} · used ${root.stats.usedNow} · located ${root.stats.locatedNow} · with you ${root.stats.travellingNow} · ${root.stats.stops} stops · ${(root.stats.distanceKm || 0).toFixed(0)} km` + (root.liveScanSeconds > 0 ? ` · live scan ${root.liveScanSeconds} s` : "")
                      : ""
                elide: Text.ElideRight
            }

            RowLayout {
                Layout.fillWidth: true
                PC3.Button { icon.name: "view-refresh"; text: "Re-check"; enabled: !root.busy; onClicked: root.refresh() }
                PC3.Button { icon.name: "internet-web-browser"; text: "OSM"; enabled: root.valid
                    onClicked: Qt.openUrlExternally(`https://www.openstreetmap.org/?mlat=${root.lat}&mlon=${root.lon}#map=15/${root.lat}/${root.lon}`) }
                PC3.Button {
                    id: shareBtn
                    icon.name: "document-share"; text: "Share"; enabled: root.valid
                    onClicked: shareMenu.visible ? shareMenu.close() : shareMenu.popup(shareBtn, 0, -shareMenu.height)
                    PC3.Menu {
                        id: shareMenu
                        PC3.MenuItem { text: "Copy coordinates"; icon.name: "edit-copy"; onTriggered: root.copy("coords") }
                        PC3.MenuItem { text: "Copy geo: URI"; icon.name: "edit-copy"; onTriggered: root.copy("geo") }
                        PC3.MenuItem { text: "Copy place + link"; icon.name: "edit-copy"; onTriggered: root.copy("text") }
                        PC3.MenuItem { text: "Copy Google Maps link"; icon.name: "edit-copy"; onTriggered: root.copy("google") }
                        PC3.MenuItem { text: "Copy Apple Maps link"; icon.name: "edit-copy"; onTriggered: root.copy("apple") }
                        PC3.MenuSeparator {}
                        PC3.MenuItem { text: "Open in Google Maps"; icon.name: "internet-web-browser"; onTriggered: if (root.share) Qt.openUrlExternally(root.share.google) }
                        PC3.MenuItem { text: "Open in Apple Maps"; icon.name: "internet-web-browser"; onTriggered: if (root.share) Qt.openUrlExternally(root.share.apple) }
                        PC3.MenuSeparator {}
                        PC3.MenuItem { text: "Save trip as GPX (~/Documents/beaconfix-trip.gpx)"; icon.name: "document-export"; onTriggered: root.saveGpx() }
                        PC3.MenuItem { text: "Save map around here for offline"; icon.name: "document-save"; onTriggered: root.prefetch() }
                        PC3.MenuItem { text: "Start a new trip here"; icon.name: "flag"; onTriggered: root.newTrip() }
                    }
                }
                Item { Layout.fillWidth: true }
                PC3.Button { icon.name: "window"; text: "Open app"; onClicked: root.openApp() }
            }
        }
    }
}
