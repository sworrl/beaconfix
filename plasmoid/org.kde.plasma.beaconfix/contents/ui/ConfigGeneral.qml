import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    property alias cfg_pollSeconds:      pollSpin.value
    property alias cfg_showPlaceInPanel: placeCheck.checked
    property alias cfg_binary:           binField.text
    property alias cfg_mapLayer:         layerCombo.currentIndex
    property alias cfg_showAccuracyInPanel: accCheck.checked
    property alias cfg_showMotionInPanel:   motionCheck.checked
    property alias cfg_showMapTab:       mapTab.checked
    property alias cfg_showNearbyTab:    nearbyTab.checked
    property alias cfg_showRadarTab:     radarTab.checked
    property alias cfg_showTripTab:      tripTab.checked
    property alias cfg_showSsids:        ssidCheck.checked
    property alias cfg_showEvents:       eventsCheck.checked
    property alias cfg_animatedMap: cineCheck.checked
    property alias cfg_tourMinutes: tourSpin.value
    property alias cfg_spotlightMinutes: spotSpin.value

    RowLayout {
        Kirigami.FormData.label: "Read state every:"
        QQC2.SpinBox { id: pollSpin; from: 5; to: 3600; stepSize: 5 }
        QQC2.Label { text: "sec" }
    }
    QQC2.ComboBox {
        id: layerCombo
        Kirigami.FormData.label: "Map style:"
        model: ["Dark (OpenStreetMap, night filter)", "Streets (OpenStreetMap)", "Satellite (Esri)", "Topographic (OpenTopoMap)"]
    }
    QQC2.CheckBox {
        id: placeCheck
        Kirigami.FormData.label: "Panel:"
        text: "Show place name next to the icon"
    }
    QQC2.CheckBox { id: accCheck; text: "Show source + accuracy chip" }
    QQC2.CheckBox { id: motionCheck; text: "Show speed & heading while moving (elevation when stopped)" }
    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Map" }
    QQC2.CheckBox { id: ssidCheck;   Kirigami.FormData.label: "Show:"; text: "Wi-Fi names beside the beacons (all from zoom 15, the 12 strongest from 13)" }
    QQC2.CheckBox { id: cineCheck; text: "Cinematic map — glide to significant events (a real move, a beacon placed, a much better beacon fit), now and then zoom out to show the city and state, and re-fit the zoom once you have moved somewhere new (never while parked). At most one automatic zoom every 10 minutes, never while the pointer is over the widget or for 3 minutes after you use the map. Off: the map only pans to keep your position in view." }
    RowLayout {
        Kirigami.FormData.label: "Overview every:"
        enabled: cineCheck.checked
        QQC2.SpinBox { id: tourSpin; from: 0; to: 180 }
        QQC2.Label { text: tourSpin.value === 0 ? "min (never)" : "min" }
    }
    RowLayout {
        Kirigami.FormData.label: "Glide to an event at most every:"
        enabled: cineCheck.checked
        QQC2.SpinBox { id: spotSpin; from: 0; to: 240 }
        QQC2.Label { text: spotSpin.value === 0 ? "min (never)" : "min" }
    }
    QQC2.CheckBox { id: eventsCheck; text: "Events as motion — ripples for new beacons, fade-outs for lost ones, signal arrows, fix arrows, stop pins — plus the ticker" }
    Kirigami.Separator { Kirigami.FormData.isSection: true; Kirigami.FormData.label: "Tabs" }
    QQC2.CheckBox { id: mapTab;    Kirigami.FormData.label: "Show:"; text: "Map — the fix, beacons and places" }
    QQC2.CheckBox { id: nearbyTab; text: "Nearby — places list" }
    QQC2.CheckBox { id: radarTab;  text: "Radar — beacons by distance" }
    QQC2.CheckBox { id: tripTab;   text: "Trip — distances, time, sun, elevation, places visited, milestones, stops" }
    QQC2.TextField {
        id: binField
        Kirigami.FormData.label: "beaconfix binary:"
        placeholderText: "beaconfix"
    }
    QQC2.Label {
        text: "The widget reads the fix from the BeaconFix tray app (D-Bus org.sworrl.BeaconFix) and starts it on demand.\nHow often BeaconFix itself re-checks is set in the app."
        font.pixelSize: Kirigami.Theme.smallFont.pixelSize
        opacity: 0.7
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
}
