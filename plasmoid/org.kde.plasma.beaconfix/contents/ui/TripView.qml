import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.plasma.components as PC3
import org.kde.kirigami as Kirigami

// Trip intelligence: distances, moving/stopped time, speed & heading, elevation,
// sun, places visited, milestones and the stop list with dwell times.
Flickable {
    id: trip
    required property var src
    clip: true
    contentWidth: width
    contentHeight: col.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    QQC2.ScrollBar.vertical: PC3.ScrollBar {}

    readonly property var st: src.stats || ({})
    readonly property var sun: src.sun || null

    function dur(secs) {
        if (secs === undefined || secs === null || secs < 0) return "—"
        if (secs < 60) return Math.round(secs) + " s"
        if (secs < 3600) return Math.floor(secs / 60) + " min"
        if (secs < 86400) return Math.floor(secs / 3600) + " h " + Math.floor((secs % 3600) / 60) + " min"
        return Math.floor(secs / 86400) + " d " + Math.floor((secs % 86400) / 3600) + " h"
    }
    function km(v, d) { return (v === undefined || v === null) ? "—" : Number(v).toFixed(d === undefined ? (v < 10 ? 1 : 0) : d) + " km" }
    function hm(iso) { return iso ? new Date(iso).toLocaleTimeString(Qt.locale(), "HH:mm") : "—" }
    function dmy(iso) { return iso ? new Date(iso).toLocaleString(Qt.locale(), "ddd d MMM HH:mm") : "" }

    component Tile: Rectangle {
        property string label
        property string value
        property color accent: "#35d6ff"
        Layout.fillWidth: true
        implicitHeight: Kirigami.Units.gridUnit * 3.2
        radius: 6; color: "#111827"; border.color: Qt.rgba(accent.r, accent.g, accent.b, 0.35); border.width: 1
        ColumnLayout {
            anchors.fill: parent; anchors.margins: 6; spacing: 0
            PC3.Label { text: value; color: accent; font.bold: true; font.pixelSize: Kirigami.Theme.defaultFont.pixelSize * 1.3; elide: Text.ElideRight; Layout.fillWidth: true }
            PC3.Label { text: label; color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; elide: Text.ElideRight; Layout.fillWidth: true }
        }
    }

    ColumnLayout {
        id: col
        width: trip.width - Kirigami.Units.smallSpacing
        spacing: Kirigami.Units.smallSpacing

        RowLayout {
            Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
            Tile { label: "today"; value: trip.km(trip.st.distanceTodayKm) }
            Tile { label: "this trip · day " + (trip.st.tripDays || 0); value: trip.km(trip.st.distanceTripKm, 0); accent: "#ffd166" }
            Tile { label: "all time · " + (trip.st.stops || 0) + " stops"; value: trip.km(trip.st.distanceAllKm, 0); accent: "#6cff8a" }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
            Tile {
                label: trip.st.moving ? "moving · heading " + (trip.st.heading || "—") : "at this stop"
                value: trip.st.moving && trip.st.speedKmh >= 0 ? Math.round(trip.st.speedKmh) + " km/h" : trip.dur(trip.st.dwellSecs)
                accent: trip.st.moving ? "#ff9f43" : "#35d6ff"
            }
            Tile { label: "moving / stopped (trip)"; value: trip.dur(trip.st.movingTripSecs) + " / " + trip.dur(trip.st.stoppedTripSecs); accent: "#a29bfe" }
            Tile {
                label: trip.src.elevationNote ? trip.src.elevationNote : "elevation"
                value: trip.src.elevation !== null && trip.src.elevation !== undefined ? Math.round(trip.src.elevation) + " m · " + Math.round(trip.src.elevation * 3.28084) + " ft" : "—"
                accent: "#7bed9f"
            }
        }
        // Sun
        Rectangle {
            visible: !!trip.sun
            Layout.fillWidth: true; implicitHeight: sunRow.implicitHeight + 12
            radius: 6; color: "#111827"; border.color: Qt.rgba(1, 0.82, 0.4, 0.35); border.width: 1
            RowLayout {
                id: sunRow
                anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 6 }
                spacing: Kirigami.Units.largeSpacing
                PC3.Label { text: "☀"; color: "#ffd166"; font.pixelSize: Kirigami.Theme.defaultFont.pixelSize * 1.4 }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 0
                    PC3.Label {
                        Layout.fillWidth: true; color: "#e6edf7"; font.bold: true; elide: Text.ElideRight
                        text: !trip.sun ? "" : trip.sun.polarDay ? "Sun up all day" : trip.sun.polarNight ? "Sun below the horizon all day"
                              : `Sunrise ${trip.hm(trip.sun.sunrise)} · sunset ${trip.hm(trip.sun.sunset)} · ${trip.dur(trip.sun.dayLengthSecs)} of daylight`
                    }
                    PC3.Label {
                        Layout.fillWidth: true; color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; elide: Text.ElideRight
                        visible: !!trip.sun && !trip.sun.polarDay && !trip.sun.polarNight
                        text: !trip.sun ? "" : `Golden hour until ${trip.hm(trip.sun.goldenMorningEnd)} and from ${trip.hm(trip.sun.goldenEveningStart)} · civil dawn ${trip.hm(trip.sun.civilDawn)} · dusk ${trip.hm(trip.sun.civilDusk)} · solar noon ${trip.hm(trip.sun.solarNoon)}`
                    }
                }
                PC3.Label { text: trip.sun && trip.sun.isDay ? "day" : "night"; color: trip.sun && trip.sun.isDay ? "#ffd166" : "#a29bfe"; font.bold: true }
            }
        }
        // Places visited
        PC3.Label {
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#e6edf7"
            font.pixelSize: Kirigami.Theme.smallFont.pixelSize
            text: {
                var c = trip.st.cities || [], r = trip.st.regions || [], n = trip.st.countries || []
                if (!c.length && !r.length) return "<b>Places visited</b> (precise fixes): none yet"
                var s = `<b>Places visited</b> (precise fixes): ${c.length} cities · ${r.length} states/provinces · ${n.length} countries`
                if (r.length) s += "<br>" + r.join(", ")
                if (n.length) s += " — " + n.join(", ")
                return s
            }
        }
        // Milestones
        RowLayout {
            Layout.fillWidth: true
            PC3.Label { text: `Milestones ${trip.st.achievementsUnlocked || 0} / ${trip.st.achievementsTotal || 0}`; color: "#ffd166"; font.bold: true }
            PC3.Label { text: `${trip.st.rank || ""} · level ${trip.st.rankLevel || 0} of ${trip.st.rankCount || 0}${trip.st.nextRankAt ? " · next at " + trip.st.nextRankAt : ""}`
                        color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; Layout.fillWidth: true; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight }
        }
        Flow {
            Layout.fillWidth: true; spacing: 4
            Repeater {
                model: trip.st.achievements || []
                delegate: Rectangle {
                    required property var modelData
                    width: chipRow.implicitWidth + 14; height: chipRow.implicitHeight + 8; radius: height / 2
                    color: modelData.unlocked ? Qt.rgba(1, 0.82, 0.4, 0.18) : "#111827"
                    border.color: modelData.unlocked ? "#ffd166" : Qt.rgba(0.62, 0.69, 0.78, 0.25); border.width: 1
                    opacity: modelData.unlocked ? 1 : 0.55
                    QQC2.ToolTip.text: modelData.desc + (modelData.at ? "\n" + trip.dmy(modelData.at) : "")
                    QQC2.ToolTip.visible: chipHover.hovered; QQC2.ToolTip.delay: 400
                    HoverHandler { id: chipHover }
                    RowLayout {
                        id: chipRow; anchors.centerIn: parent; spacing: 4
                        Text { text: modelData.icon; font.pixelSize: Kirigami.Theme.smallFont.pixelSize + 1 }
                        PC3.Label { text: modelData.title; color: modelData.unlocked ? "#e6edf7" : "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; font.bold: modelData.unlocked }
                    }
                }
            }
        }
        // Stops
        PC3.Label { text: "Stops"; color: "#35d6ff"; font.bold: true }
        Repeater {
            model: {
                var t = trip.src.track || [], out = []
                for (var i = t.length - 1; i >= 0 && out.length < 40; i--) out.push(t[i])
                return out
            }
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true; spacing: Kirigami.Units.smallSpacing
                Rectangle { width: 8; height: 8; radius: 4; color: modelData.source === "ip" ? "#9fb0c8" : modelData.source === "starlink" ? "#6cff8a" : "#35d6ff" }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 0
                    PC3.Label { Layout.fillWidth: true; text: modelData.place; color: "#e6edf7"; font.bold: modelData.source !== "ip"; elide: Text.ElideRight
                                opacity: modelData.source === "ip" ? 0.6 : 1 }
                    PC3.Label {
                        Layout.fillWidth: true; color: "#9fb0c8"; font.pixelSize: Kirigami.Theme.smallFont.pixelSize; elide: Text.ElideRight
                        text: [trip.dmy(modelData.time),
                               modelData.dwellSecs !== undefined ? "stayed " + trip.dur(modelData.dwellSecs) : "",
                               modelData.legKm !== undefined ? "leg " + trip.km(modelData.legKm) + (modelData.legSecs > 0 ? " in " + trip.dur(modelData.legSecs) : "") : "",
                               modelData.elev !== undefined ? Math.round(modelData.elev) + " m" : ""].filter(function(s) { return !!s }).join(" · ")
                    }
                }
            }
        }
        PC3.Label { visible: !(trip.src.track || []).length; text: "No stops logged yet"; color: "#9fb0c8" }
        Item { implicitHeight: Kirigami.Units.smallSpacing }
    }
}
