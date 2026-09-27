import QtQuick

// Keyboard-focus indicator for custom items: a ring drawn just outside the parent's bounds.
Rectangle {
    property bool shown: false

    anchors.fill: parent
    anchors.margins: -Theme.focusRingWidth - 1
    visible: shown
    color: "transparent"
    radius: Theme.radius + 2
    border.width: Theme.focusRingWidth
    border.color: Theme.focusRing
}
