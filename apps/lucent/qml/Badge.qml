import QtQuick
import QtQuick.Controls

// A compact status label: a coloured dot and short text on a dark chip, readable on top of images.
Rectangle {
    id: badge

    property string text
    property color dotColor: Theme.textTertiary
    property string detail  // tooltip text; empty for none

    implicitWidth: row.implicitWidth + 12
    implicitHeight: 20
    radius: height / 2
    color: Theme.scrim

    Accessible.role: Accessible.StaticText
    Accessible.name: detail.length > 0 ? text + ": " + detail : text

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 5

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 7
            height: 7
            radius: 3.5
            color: badge.dotColor
        }

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: badge.text
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSmall
        }
    }

    HoverHandler {
        id: hover
        enabled: badge.detail.length > 0
    }

    ToolTip.visible: hover.hovered
    ToolTip.text: badge.detail
    ToolTip.delay: 500
}
