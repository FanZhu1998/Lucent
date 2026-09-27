import QtQuick
import QtQuick.Controls
import Lucent

// A push button in the Lucent theme. `highlighted` marks the primary action of a view.
Button {
    id: control

    implicitHeight: 30
    leftPadding: 14
    rightPadding: 14
    topPadding: 4
    bottomPadding: 4
    font.pixelSize: Theme.fontBody

    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.textTertiary : control.highlighted ? Theme.textOnAccent : Theme.textPrimary
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 72
        implicitHeight: 30
        radius: Theme.radius
        opacity: control.enabled ? 1 : 0.55
        color: control.highlighted
            ? (control.down ? Theme.accentStrongPressed : control.hovered ? Theme.accentStrongHover : Theme.accentStrong)
            : (control.down ? Theme.controlPressed : control.hovered ? Theme.controlHover : Theme.control)
        border.width: control.highlighted ? 0 : 1
        border.color: Theme.controlBorder

        FocusRing {
            shown: control.visualFocus
        }
    }
}
