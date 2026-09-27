import QtQuick
import QtQuick.Controls
import Lucent

// A check box in the Lucent theme with an indicator outline that stays visible on dark surfaces.
CheckBox {
    id: control

    spacing: 8
    font.pixelSize: Theme.fontBody

    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: 3
        color: control.checked ? Theme.accentStrong : "transparent"
        border.width: control.checked ? 0 : 1
        border.color: control.hovered ? Theme.textSecondary : Theme.indicatorBorder

        Text {
            anchors.centerIn: parent
            visible: control.checked
            text: "✓"
            color: Theme.textOnAccent
            font.pixelSize: 12
            font.bold: true
        }

        FocusRing {
            shown: control.visualFocus
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: control.enabled ? Theme.textPrimary : Theme.textTertiary
        verticalAlignment: Text.AlignVCenter
    }
}
