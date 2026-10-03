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
    // tel: wants the bare number: OSM phone tags carry spaces, dashes, dots, brackets and sometimes several
    // numbers ("+1 555-0100;+1 555-0101"); dial the first
    function telUrl(ph) { return "tel:" + String(ph || "").split(/[;,]/)[0].replace(/[^0-9+*#]/g, "") }
    // openstreetmap.org directions, route=<lat>,<lon>;<lat>,<lon> encoded as the site does; toFixed() is never
    // localised (no decimal comma). Without a fix the start stays empty instead of 0,0.
    function directionsUrl(p) {
        var from = nearby.src.valid ? Number(nearby.src.lat).toFixed(6) + "," + Number(nearby.src.lon).toFixed(6) : ""
        return "https://www.openstreetmap.org/directions?engine=fossgis_osrm_car&route="
               + encodeURIComponent(from + ";" + Number(p.lat).toFixed(6) + "," + Number(p.lon).toFixed(6))
    }
    function distText(m) { return m < 950 ? Math.round(m / 10) * 10 + " m" : m < 9950 ? (m / 1000).toFixed(1) + " km" : Math.round(m / 1000) + " km" }

    // ── pediatric help (desktop 3.8+). Every field is optional: an older desktop just shows the rows it has. ──
    property bool pedsOnly: false               // the Kids ER chip: pediatric ERs and pediatric urgent care only
    readonly property bool pedsKnown: !!src.pedsSupported || (src.poiCategories || []).some(function(c) { return c.key === "peds_er" })
    readonly property var helpRows: [["police", "🚔 Police"], ["fire", "🚒 Fire"], ["hospital", "🏥 ER"], ["pediatric", "🧸 Pediatric ER"],
                                     ["pediatricCloser", "🧸 Closer"], ["urgent", "🩺 Urgent care"], ["pediatricUrgent", "🩹 Pediatric urgent care"]]
    function durText(s) { var m = Math.max(1, Math.round(s / 60)); return m < 60 ? m + " min" : Math.floor(m / 60) + " h" + (m % 60 ? " " + (m % 60) + " min" : "") }
    // driveS is the desktop's estimate (straight line x 1.4 at 70 km/h) unless it says driveEst: false (a road route)
    function etaText(q) { return q && q.driveS > 0 ? "~" + durText(q.driveS) + (q.driveEst === false ? " drive" : " drive (est.)") : "" }
    function tierText(q) {                      // how sure we are this is a pediatric ER: always shown, never ranked
        if (!q) return ""
        switch (q.tier !== undefined ? q.tier : q.peds) {
        case 1: return "Dedicated pediatric ER"
        case 2: return "Children's hospital · " + (q.campusEr ? "ER on campus: " + q.campusEr + " — call ahead" : "ER not confirmed — call ahead")
        case 3: return "ER with a pediatrics department"
        case 4: return "Not an ER"
        default: return q.er === "yes" ? "ER" : q.er === "no" ? "No ER" : ""
        }
    }
    function helpDetail(key, q) {
        if (!q) return ""
        if (key === "pediatric" || key === "pediatricCloser") return tierText(q)
        if (key === "pediatricUrgent") return ["Not an ER", q.hours ? "🕑 " + q.hours : ""].filter(function(s) { return !!s }).join(" · ")
        return ""
    }

    readonly property var rows: {
        var p = src.pois || [], out = [], q = filter.text.trim().toLowerCase(), peds = pedsOnly
        for (var i = 0; i < p.length; i++) {
            var x = p[i]
            if (peds && x.cat !== "peds_er" && x.cat !== "peds_urgent") continue
            if (hiddenCats.indexOf(x.cat) >= 0 && !q && !peds) continue
            if (q && (x.name + " " + x.label + " " + (x.detail || "") + " " + (x.group || "") + " " + (x.address || "")).toLowerCase().indexOf(q) < 0) continue
            out.push({i: i, p: x})
        }
        out.sort(function(a, b) { return (a.p.d || 0) - (b.p.d || 0) })
        return out
    }

    // ── nearest help: police / fire / ER / pediatric ER / urgent care + the local emergency number ──
    // The nearest general ER always keeps its row: a pediatric ER is listed after it, never instead of it.
    Rectangle {
        id: emergencyCard
        readonly property var e: nearby.src.emergency || null
        visible: e !== null && !!(e.police || e.fire || e.hospital || e.urgent || e.number || e.pediatric || e.pediatricUrgent || e.pediatricNote)
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
                model: emergencyCard.e ? nearby.helpRows.filter(function(k) { return !!emergencyCard.e[k[0]] }) : []
                delegate: RowLayout {
                    id: helpRow
                    required property var modelData
                    readonly property var q: emergencyCard.e[modelData[0]]
                    readonly property string sub: nearby.helpDetail(modelData[0], q)
                    readonly property string eta: nearby.etaText(q)
                    Layout.fillWidth: true; spacing: 4
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 0
                        PC3.Label {
                            Layout.fillWidth: true
                            text: `${helpRow.modelData[1]}: ${helpRow.q.name}`
                                  + (helpRow.q.d !== undefined ? ` · ${nearby.distText(helpRow.q.d)} ${nearby.compass(helpRow.q.brg || 0)}` : "")
                                  + (helpRow.eta ? ` · ${helpRow.eta}` : "") + (helpRow.q.address ? ` · ${helpRow.q.address}` : "")
                            color: "#e6edf7"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; elide: Text.ElideRight
                        }
                        PC3.Label {
                            visible: helpRow.sub !== ""
                            Layout.fillWidth: true
                            text: helpRow.sub
                            color: /call ahead|not an er|no er/i.test(helpRow.sub) ? "#ffd166" : "#9fb0c8"
                            font.pixelSize: Kirigami.Theme.smallFont.pixelSize; elide: Text.ElideRight
                        }
                    }
                    PC3.ToolButton {
                        visible: !!helpRow.q.phone; icon.name: "call-start"; text: helpRow.q.phone || ""; display: QQC2.AbstractButton.TextBesideIcon
                        font.pixelSize: Kirigami.Theme.smallFont.pixelSize
                        onClicked: Qt.openUrlExternally(nearby.telUrl(helpRow.q.phone))
                        QQC2.ToolTip.text: "Call " + (helpRow.q.phone || ""); QQC2.ToolTip.visible: hovered
                    }
                }
            }
            PC3.Label {                          // "No pediatric ER mapped within 150 km — go to the nearest ER", "Saved 4 d ago …"
                visible: !!(emergencyCard.e && emergencyCard.e.pediatricNote)
                Layout.fillWidth: true
                text: emergencyCard.e ? (emergencyCard.e.pediatricNote || "") : ""
                color: "#ffd166"; font.italic: true; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; wrapMode: Text.Wrap
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
                onClicked: { filter.text = checked ? modelData[0] : ""; nearby.pedsOnly = false }
            }
        }
        PC3.ToolButton {
            visible: nearby.pedsKnown
            text: "🧸 Kids ER"; checkable: true; checked: nearby.pedsOnly
            QQC2.ToolTip.text: "Pediatric ERs and pediatric urgent care only (urgent care is not an ER)"; QQC2.ToolTip.visible: hovered
            onClicked: { nearby.pedsOnly = checked; if (checked) filter.text = "" }
        }
    }
    PC3.Label {
        Layout.fillWidth: true
        text: nearby.src.poiNote ? nearby.src.poiNote
              : nearby.pedsOnly ? `${nearby.rows.length} pediatric ER & urgent care · OpenStreetMap`
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
                        text: [row.modelData.p.label, row.modelData.p.detail, row.modelData.p.hours, nearby.etaText(row.modelData.p)].filter(function(s) { return !!s }).join(" · ")
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
                    onClicked: Qt.openUrlExternally(nearby.telUrl(row.modelData.p.phone))
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
                    onClicked: Qt.openUrlExternally(nearby.directionsUrl(row.modelData.p))
                }
            }
        }
        PC3.Label {
            anchors.centerIn: parent
            visible: list.count === 0
            text: !(nearby.src.pois || []).length ? "No places loaded yet"
                  : nearby.pedsOnly ? "No pediatric ER or pediatric urgent care in the list — the nearest ER is above" : "Nothing matches"
            color: "#9fb0c8"
        }
    }
}
