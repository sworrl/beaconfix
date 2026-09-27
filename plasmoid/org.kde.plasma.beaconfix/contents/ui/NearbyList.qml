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
    // Clipboard without a C++ helper: a hidden TextEdit does it
    TextEdit { id: clip; visible: false }
    function copyText(t) { clip.text = t; clip.selectAll(); clip.copy() }
    function distText(m) { return m < 950 ? Math.round(m / 10) * 10 + " m" : m < 9950 ? (m / 1000).toFixed(1) + " km" : Math.round(m / 1000) + " km" }

    readonly property var rows: {
        var p = src.pois || [], out = [], q = filter.text.trim().toLowerCase()
        for (var i = 0; i < p.length; i++) {
            var x = p[i]
            if (hiddenCats.indexOf(x.cat) >= 0 && !q) continue
            if (q && (x.name + " " + x.label + " " + (x.detail || "") + " " + (x.group || "") + " " + (x.address || "")).toLowerCase().indexOf(q) < 0) continue
            out.push({i: i, p: x})
        }
        out.sort(function(a, b) { return (a.p.d || 0) - (b.p.d || 0) })
        return out
    }

    // ── nearest help: police / fire / ER / urgent care + the local emergency number ──
    Rectangle {
        id: emergencyCard
        readonly property var e: nearby.src.emergency || null
        visible: e !== null && (e.police || e.fire || e.hospital || e.urgent || e.number)
        Layout.fillWidth: true
        radius: 6; color: Qt.rgba(1, 0.3, 0.3, 0.10); border.color: Qt.rgba(1, 0.3, 0.3, 0.45); border.width: 1
        implicitHeight: emCol.implicitHeight + 12
        ColumnLayout {
            id: emCol
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 6 }
            spacing: 2
            PC3.Label {
                text: emergencyCard.e ? `🚨 Emergency number here: ${emergencyCard.e.number || "112"}` : ""
                color: "#ff6b6b"; font.bold: true; font.pixelSize: Kirigami.Theme.smallFont.pixelSize
            }
            Repeater {
                model: emergencyCard.e ? [["police", "🚔 Police"], ["fire", "🚒 Fire"], ["hospital", "🏥 ER"], ["urgent", "🩺 Urgent care"]].filter(function(k) { return !!emergencyCard.e[k[0]] }) : []
                delegate: RowLayout {
                    required property var modelData
                    readonly property var q: emergencyCard.e[modelData[0]]
                    Layout.fillWidth: true; spacing: 4
                    PC3.Label {
                        Layout.fillWidth: true
                        text: `${modelData[1]}: ${q.name}` + (q.d !== undefined ? ` · ${nearby.distText(q.d)} ${nearby.compass(q.brg || 0)}` : "") + (q.address ? ` · ${q.address}` : "")
                        color: "#e6edf7"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; elide: Text.ElideRight
                    }
                    PC3.ToolButton {
                        visible: !!q.phone; icon.name: "call-start"; text: q.phone || ""; display: QQC2.AbstractButton.TextBesideIcon
                        font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                        onClicked: Qt.openUrlExternally("tel:" + (q.phone || "").replace(/ /g, ""))
                        QQC2.ToolTip.text: "Call " + (q.phone || ""); QQC2.ToolTip.visible: hovered
                    }
                }
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing
        PC3.TextField {
            id: filter
            Layout.fillWidth: true
            placeholderText: "Filter — police, playground, dog park, diesel, laundry…"
            clearButtonShown: true
        }
        Repeater {
            model: [["civic", "🚔"], ["kids", "🛝"], ["services", "⛽"]]
            delegate: PC3.ToolButton {
                required property var modelData
                text: modelData[1]; checkable: true; checked: filter.text.trim().toLowerCase() === modelData[0]
                QQC2.ToolTip.text: modelData[0] === "civic" ? "Emergency & civic" : modelData[0] === "kids" ? "Kids & fun" : "Services"; QQC2.ToolTip.visible: hovered
                onClicked: filter.text = checked ? modelData[0] : ""
            }
        }
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
            height: Kirigami.Units.gridUnit * (row.modelData.p.address || row.modelData.p.phone ? 2.9 : 2.3)
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
                    PC3.Label {
                        visible: !!(row.modelData.p.address || row.modelData.p.phone)
                        Layout.fillWidth: true
                        text: [row.modelData.p.phone ? "☎ " + row.modelData.p.phone : "", row.modelData.p.address].filter(function(s) { return !!s }).join(" · ")
                        color: "#c9d4e5"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; elide: Text.ElideRight
                    }
                }
                PC3.ToolButton {
                    visible: !!row.modelData.p.phone
                    icon.name: "call-start"; display: QQC2.AbstractButton.IconOnly; text: "Call"
                    QQC2.ToolTip.text: "Call " + (row.modelData.p.phone || "") + " (right-click: copy)"; QQC2.ToolTip.visible: hovered; QQC2.ToolTip.delay: 500
                    onClicked: Qt.openUrlExternally("tel:" + (row.modelData.p.phone || "").replace(/ /g, ""))
                    TapHandler { acceptedButtons: Qt.RightButton; onTapped: nearby.copyText(row.modelData.p.phone || "") }
                }
                PC3.ToolButton {
                    visible: !!row.modelData.p.address
                    icon.name: "edit-copy"; display: QQC2.AbstractButton.IconOnly; text: "Copy address"
                    QQC2.ToolTip.text: "Copy address"; QQC2.ToolTip.visible: hovered; QQC2.ToolTip.delay: 500
                    onClicked: nearby.copyText(row.modelData.p.address || "")
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
