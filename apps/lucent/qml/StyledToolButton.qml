import QtQuick
import QtQuick.Controls
import Lucent

// A flat toolbar button in the Lucent theme; checkable ones show their checked state with a tinted
// background.
ToolButton {
    id: control

    implicitHeight: 30
    leftPadding: 10
    rightPadding: 10
    font.pixelSize: Theme.fontBody

    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.textTertiary : control.checked ? Theme.textPrimary : Theme.textSecondary
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 30
        implicitHeight: 30
        radius: Theme.radius
        color: control.checked ? Theme.accentMuted
            : control.down ? Theme.controlPressed : control.hovered ? Theme.surfaceHover : "transparent"
        border.width: control.checked ? 1 : 0
        border.color: Theme.accent

        FocusRing {
            shown: control.visualFocus
        }
    }
}
