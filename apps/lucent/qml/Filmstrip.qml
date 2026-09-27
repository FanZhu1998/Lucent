pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import Lucent

// The Edit view's filmstrip: every library photo in order, the current one outlined, and a
// "Camera" tag on thumbnails that are still the camera's embedded JPEG (blueprint section 2: labelled
// until replaced by Lucent's own render). Left/Right move through it when it has keyboard focus; a
// click selects.
ListView {
    id: strip

    required property AppController controller

    orientation: ListView.Horizontal
    model: controller.library
    spacing: 6
    leftMargin: 10
    rightMargin: 10
    topMargin: 8
    bottomMargin: 8
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    keyNavigationEnabled: true
    activeFocusOnTab: true
    highlightFollowsCurrentItem: false
    currentIndex: -1

    Accessible.role: Accessible.List
    Accessible.name: qsTr("Filmstrip")

    ScrollBar.horizontal: ScrollBar {
        policy: ScrollBar.AsNeeded
    }

    delegate: Item {
        id: cell

        required property int index
        required property string thumbnailSource
        required property bool thumbnailIsCameraPreview
        required property string accessibleLabel

        readonly property bool isCurrent: ListView.isCurrentItem

        height: strip.height - strip.topMargin - strip.bottomMargin
        width: Math.round(height * 1.5)

        Accessible.role: Accessible.ListItem
        Accessible.name: accessibleLabel
        Accessible.selected: isCurrent

        Rectangle {
            anchors.fill: parent
            radius: 3
            color: Theme.imageBackground
            border.width: cell.isCurrent ? 2 : 1
            border.color: cell.isCurrent ? Theme.accent : Theme.border

            Image {
                id: thumb

                anchors.fill: parent
                anchors.margins: 3
                source: cell.thumbnailSource
                // Same request size as the contact sheet, so both share Qt's pixmap cache.
                sourceSize: Qt.size(512, 512)
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true
                mipmap: true
                Accessible.ignored: true
            }

            Rectangle {
                objectName: "filmstripCameraTag"
                visible: cell.thumbnailIsCameraPreview && thumb.status === Image.Ready
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 5
                implicitWidth: cameraText.implicitWidth + 8
                implicitHeight: 16
                radius: 2
                color: Theme.scrim

                Text {
                    id: cameraText
                    anchors.centerIn: parent
                    text: qsTr("Camera")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSmall - 1
                }

                HoverHandler {
                    id: tagHover
                }

                ToolTip.visible: tagHover.hovered
                ToolTip.text: qsTr("Camera preview: the camera's embedded JPEG, shown until Lucent's own rendering replaces it.")
                ToolTip.delay: 500
            }
        }

        FocusRing {
            shown: cell.isCurrent && strip.activeFocus
        }

        TapHandler {
            onTapped: {
                strip.currentIndex = cell.index
                strip.forceActiveFocus(Qt.MouseFocusReason)
            }
        }
    }

    Component.onCompleted: currentIndex = controller.currentIndex
    onCurrentIndexChanged: {
        if (currentIndex >= 0)
            positionViewAtIndex(currentIndex, ListView.Contain)
        if (currentIndex !== controller.currentIndex)
            controller.currentIndex = currentIndex
    }

    Connections {
        target: strip.controller

        function onCurrentIndexChanged() {
            if (strip.currentIndex !== strip.controller.currentIndex)
                strip.currentIndex = strip.controller.currentIndex
        }
    }
}
