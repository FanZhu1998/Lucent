import QtQuick
import QtQuick.Controls

// A view switcher tab in the window header: text with an accent underline when it is the current
// view, a hover tint, and a focus ring for keyboard users.
AbstractButton {
    id: control

    property string badgeText  // e.g. a count of running exports; empty for none
    property bool badgeAttention: false  // something there needs the user (W7: a paused batch)

    checkable: false  // "checked" is bound to the current view by the header
    focusPolicy: Qt.StrongFocus
    implicitWidth: content.implicitWidth + 28
    implicitHeight: Theme.toolbarHeight

    Accessible.role: Accessible.PageTab
    Accessible.name: (badgeText.length > 0 ? text + ", " + badgeText : text)
                     + (badgeAttention ? ", " + qsTr("needs attention") : "")

    contentItem: Item {
        Row {
            id: content
            anchors.centerIn: parent
            spacing: 6

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: control.text
                color: control.checked ? Theme.textPrimary : Theme.textSecondary
                font.pixelSize: Theme.fontBody
                font.weight: control.checked ? Font.DemiBold : Font.Normal
            }

            Rectangle {
                objectName: "navBadge"
                anchors.verticalCenter: parent.verticalCenter
                visible: control.badgeText.length > 0 || control.badgeAttention
                implicitWidth: badgeLabel.implicitWidth + 10
                implicitHeight: 16
                radius: 8
                color: control.badgeAttention ? Theme.attention : Theme.accentMuted

                Text {
                    id: badgeLabel
                    anchors.centerIn: parent
                    text: control.badgeAttention ? "! " + control.badgeText : control.badgeText
                    // Dark text on the amber attention badge keeps 4.5:1.
                    color: control.badgeAttention ? Theme.window : Theme.textPrimary
                    font.pixelSize: Theme.fontSmall
                    font.weight: control.badgeAttention ? Font.DemiBold : Font.Normal
                }
            }
        }
    }

    background: Item {
        Rectangle {
            anchors.fill: parent
            anchors.margins: 4
            radius: Theme.radius
            color: control.hovered && !control.checked ? Theme.surfaceHover : "transparent"
        }

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width - 16
            height: 2
            color: Theme.accent
            visible: control.checked
        }

        FocusRing {
            anchors.margins: 3
            shown: control.visualFocus
        }
    }
}
