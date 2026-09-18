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
    readonly property string bin:       Plasmoid.configuration.binary || "beaconfix"

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

    readonly property string sourceName:
        source === "starlink" ? "Starlink dish GPS" :
        source === "wifi"     ? "BeaconDB Wi-Fi" :
        source === "ip"       ? "IP geolocation (approximate)" : "no fix"
    readonly property string sourceIcon:
        source === "starlink" ? "gps" :
        source === "wifi"     ? "mark-location" :
        source === "ip"       ? "network-server" : "dialog-warning"

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

    fullRepresentation: ColumnLayout {
        Layout.preferredWidth:  Kirigami.Units.gridUnit * 22
        Layout.minimumWidth:    Kirigami.Units.gridUnit * 18
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true
            Kirigami.Icon { source: root.sourceIcon; implicitWidth: Kirigami.Units.iconSizes.medium; implicitHeight: implicitWidth }
            PC3.Label {
                text: root.valid ? root.place : "No location yet"
                font.bold: true; font.pixelSize: Kirigami.Theme.defaultFont.pixelSize * 1.3
                wrapMode: Text.Wrap; Layout.fillWidth: true
            }
        }
        PC3.Label {
            visible: root.valid
            text: `${root.lat.toFixed(5)}, ${root.lon.toFixed(5)}`
            font.family: "monospace"
        }
        PC3.Label {
            text: root.valid
                  ? `±${Math.round(root.accuracy)} m · ${root.sourceName}\nUpdated ${root.ageText()}` + (root.apCount ? ` · ${root.apCount} APs seen` : "")
                  : (root.error || "Waiting for the BeaconFix tray app…")
            opacity: 0.75; wrapMode: Text.Wrap; Layout.fillWidth: true
        }
        PC3.Label {
            visible: root.valid && root.error !== ""
            text: "Last attempt failed: " + root.error
            opacity: 0.6; wrapMode: Text.Wrap; Layout.fillWidth: true
            font.pixelSize: Kirigami.Theme.smallFont.pixelSize
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
