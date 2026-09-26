import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami

// Places around the fix, nearest first, with a filter box.
ColumnLayout {
    id: nearby
    required property var src
    property var hiddenCats: []
    signal showOnMap(int index)
    spacing: Kirigami.Units.smallSpacing

    function compass(deg) { return ["N", "NE", "E", "SE", "S", "SW", "W", "NW"][Math.floor(((deg + 22.5) % 360 + 360) % 360 / 45) % 8] }
    function distText(m) { return m < 950 ? Math.round(m / 10) * 10 + " m" : m < 9950 ? (m / 1000).toFixed(1) + " km" : Math.round(m / 1000) + " km" }

    readonly property var rows: {
        var p = src.pois || [], out = [], q = filter.text.trim().toLowerCase()
        for (var i = 0; i < p.length; i++) {
            var x = p[i]
            if (hiddenCats.indexOf(x.cat) >= 0 && !q) continue
            if (q && (x.name + " " + x.label + " " + (x.detail || "")).toLowerCase().indexOf(q) < 0) continue
            out.push({i: i, p: x})
        }
        out.sort(function(a, b) { return (a.p.d || 0) - (b.p.d || 0) })
        return out
    }

    PC3.TextField {
        id: filter
        Layout.fillWidth: true
        placeholderText: "Filter — fuel, diesel, laundry, free, water…"
        clearButtonShown: true
    }
    PC3.Label {
        Layout.fillWidth: true
        text: nearby.src.poiNote ? nearby.src.poiNote
              : `${nearby.rows.length} of ${(nearby.src.pois || []).length} places · OpenStreetMap`
        color: nearby.src.poiNote ? "#ffd166" : "#9fb0c8"
        font.pixelSize: Kirigami.Theme.smallFont.pixelSize
        elide: Text.ElideRight
    }
    ListView {
        id: list
        Layout.fillWidth: true; Layout.fillHeight: true
        clip: true
        model: nearby.rows
        QQC2.ScrollBar.vertical: PC3.ScrollBar {}
        delegate: MouseArea {
            id: row
            required property var modelData
            width: list.width - Kirigami.Units.smallSpacing
            height: Kirigami.Units.gridUnit * 2.3
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: nearby.showOnMap(modelData.i)
            Rectangle {
                anchors.fill: parent; radius: 5
                color: row.containsMouse ? Qt.rgba(0.21, 0.84, 1, 0.10) : "transparent"
            }
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 4; anchors.rightMargin: 4
                spacing: Kirigami.Units.smallSpacing
                Rectangle {
                    Layout.preferredWidth: 24; Layout.preferredHeight: 24; radius: 12
                    color: "#0c111c"; border.color: row.modelData.p.color; border.width: 2
                    Text { anchors.centerIn: parent; text: row.modelData.p.icon; font.pixelSize: 12 }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    PC3.Label {
                        Layout.fillWidth: true
                        text: row.modelData.p.name || row.modelData.p.label
                        color: "#e6edf7"; font.bold: true; elide: Text.ElideRight
                    }
                    PC3.Label {
                        Layout.fillWidth: true
                        text: [row.modelData.p.label, row.modelData.p.detail, row.modelData.p.hours].filter(function(s) { return !!s }).join(" · ")
                        color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; elide: Text.ElideRight
                    }
                }
                PC3.Label {
                    text: `${nearby.distText(row.modelData.p.d || 0)} ${nearby.compass(row.modelData.p.brg || 0)}`
                    color: "#35d6ff"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                }
                PC3.ToolButton {
                    icon.name: "go-next"
                    display: QQC2.AbstractButton.IconOnly
                    text: "Directions"
                    QQC2.ToolTip.text: "Directions (OpenStreetMap)"; QQC2.ToolTip.visible: hovered; QQC2.ToolTip.delay: 500
                    onClicked: Qt.openUrlExternally(`https://www.openstreetmap.org/directions?engine=fossgis_osrm_car&route=${nearby.src.lat},${nearby.src.lon};${row.modelData.p.lat},${row.modelData.p.lon}`)
                }
            }
        }
        PC3.Label {
            anchors.centerIn: parent
            visible: list.count === 0
            text: (nearby.src.pois || []).length ? "Nothing matches" : "No places loaded yet"
            color: "#9fb0c8"
        }
    }
}
