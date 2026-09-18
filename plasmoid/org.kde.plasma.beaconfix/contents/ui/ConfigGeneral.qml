import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.FormLayout {
    property alias cfg_pollSeconds:      pollSpin.value
    property alias cfg_showPlaceInPanel: placeCheck.checked
    property alias cfg_binary:           binField.text

    RowLayout {
        Kirigami.FormData.label: "Read state every:"
        QQC2.SpinBox { id: pollSpin; from: 10; to: 3600; stepSize: 10 }
        QQC2.Label { text: "sec" }
    }
    QQC2.CheckBox {
        id: placeCheck
        Kirigami.FormData.label: "Panel:"
        text: "Show place name next to the icon"
    }
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
