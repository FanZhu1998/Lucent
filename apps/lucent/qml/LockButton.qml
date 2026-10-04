import QtQuick
import QtQuick.Controls
import Lucent

// A lock toggle for a photographic control (blueprint section 2: "The photographer can lock controls
// before rerunning [Auto]"). Drawn as a small padlock: open and grey when unlocked, closed and
// accent-coloured when locked. Locks are stored with the recipe (a revision each); they do not
// change the photo.
AbstractButton {
    id: control

    // The control's name for the accessible label and tooltip ("Exposure").
    property string controlName

    implicitWidth: 22
    implicitHeight: 22
    checkable: true
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    Accessible.role: Accessible.CheckBox
    Accessible.name: qsTr("Lock %1").arg(controlName)
    Accessible.checkable: true
    Accessible.checked: checked

    ToolTip.visible: hovered
    ToolTip.text: checked ? qsTr("%1 is locked: Auto will not change it. Click to unlock.").arg(controlName)
                          : qsTr("Lock %1 so Auto will not change it").arg(controlName)
    ToolTip.delay: 800

    contentItem: Item {
        readonly property color ink: !control.enabled ? Theme.textTertiary
                                     : control.checked ? Theme.focusRing
                                     : control.hovered ? Theme.textSecondary : "#6a6a6a"

        // Shackle: closed over the body when locked, lifted and open when not.
        Rectangle {
            x: Math.round(parent.width / 2 - 4)
            y: control.checked ? 4 : 2
            width: 8
            height: 8
            radius: 4
            color: "transparent"
            border.width: 1.5
            border.color: parent.ink
        }

        Rectangle {
            visible: !control.checked
            x: Math.round(parent.width / 2 + 2)
            y: 6
            width: 3
            height: 4
            color: Theme.surface
        }

        Rectangle {
            x: Math.round(parent.width / 2 - 6)
            y: 10
            width: 12
            height: 8
            radius: 1.5
            color: parent.ink
        }
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.down ? Theme.controlPressed : control.hovered ? Theme.surfaceHover : "transparent"

        FocusRing {
            shown: control.visualFocus
        }
    }
}
