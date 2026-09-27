pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Library: the contact sheet (blueprint section 2, "Import and organize"). Arrow keys move the
// selection, Enter opens the selected photo in Edit, Home/End jump to the first/last photo.
FocusScope {
    id: root

    required property AppController controller

    signal addPhotosRequested()
    signal addFolderRequested()

    function takeFocus() {
        if (grid.count > 0)
            grid.forceActiveFocus(Qt.OtherFocusReason)
        else
            emptyState.takeFocus()
    }

    // When the first photos arrive the empty state disappears; if the keyboard focus was in this
    // view (or nowhere), it moves to the contact sheet. Focus elsewhere (the header) stays put.
    function refocusAfterImport() {
        if (!root.visible || grid.count === 0 || grid.activeFocus)
            return
        let item = root.Window.activeFocusItem
        while (item && item !== root)
            item = item.parent
        if (!root.Window.activeFocusItem || item === root || !root.Window.activeFocusItem.visible)
            grid.forceActiveFocus(Qt.OtherFocusReason)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.toolbarHeight
            visible: grid.count > 0
            color: Theme.window

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.margin
                anchors.rightMargin: Theme.margin
                spacing: Theme.spacing

                Label {
                    text: grid.count === 1 ? qsTr("1 photo") : qsTr("%1 photos").arg(grid.count)
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }

                Item {
                    Layout.fillWidth: true
                }

                StyledCheckBox {
                    objectName: "toolbarIncludeSubfolders"
                    text: qsTr("Include subfolders")
                    checked: root.controller.includeSubfolders
                    onToggled: root.controller.includeSubfolders = checked
                }

                StyledButton {
                    text: qsTr("Add folder…")
                    onClicked: root.addFolderRequested()
                }

                StyledButton {
                    text: qsTr("Add photos…")
                    highlighted: true
                    onClicked: root.addPhotosRequested()
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            GridView {
                id: grid

                objectName: "contactSheet"
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                visible: count > 0
                clip: true
                focus: true
                model: root.controller.library
                boundsBehavior: Flickable.StopAtBounds
                keyNavigationEnabled: true
                activeFocusOnTab: true
                highlightFollowsCurrentItem: false
                currentIndex: -1

                readonly property int columns: Math.max(1, Math.floor(width / 230))
                cellWidth: Math.floor(width / columns)
                // Image well (3:2 inside the frame margins) plus four lines of text.
                cellHeight: Math.round((cellWidth - 28) * 2 / 3) + 108
                cacheBuffer: cellHeight * 2

                Accessible.role: Accessible.List
                Accessible.name: qsTr("Contact sheet")

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }

                delegate: ThumbnailCard {
                    width: grid.cellWidth
                    height: grid.cellHeight
                    selected: GridView.isCurrentItem
                    showFocus: GridView.isCurrentItem && grid.activeFocus
                    onClicked: {
                        grid.currentIndex = index
                        grid.forceActiveFocus(Qt.MouseFocusReason)
                    }
                    onActivated: root.controller.openItem(index)
                }

                Component.onCompleted: currentIndex = root.controller.currentIndex
                onCountChanged: Qt.callLater(root.refocusAfterImport)
                onCurrentIndexChanged: {
                    if (currentIndex >= 0)
                        positionViewAtIndex(currentIndex, GridView.Contain)
                    if (currentIndex !== root.controller.currentIndex)
                        root.controller.currentIndex = currentIndex
                }
                onActiveFocusChanged: {
                    if (activeFocus && currentIndex < 0 && count > 0)
                        currentIndex = 0
                }

                Keys.onReturnPressed: root.controller.openItem(currentIndex)
                Keys.onEnterPressed: root.controller.openItem(currentIndex)
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Home && count > 0) {
                        currentIndex = 0
                        event.accepted = true
                    } else if (event.key === Qt.Key_End && count > 0) {
                        currentIndex = count - 1
                        event.accepted = true
                    }
                }
            }

            Connections {
                target: root.controller

                function onCurrentIndexChanged() {
                    if (grid.currentIndex !== root.controller.currentIndex)
                        grid.currentIndex = root.controller.currentIndex
                }
            }

            LibraryEmptyState {
                id: emptyState

                objectName: "emptyState"
                anchors.fill: parent
                visible: grid.count === 0
                controller: root.controller
                onAddPhotosRequested: root.addPhotosRequested()
                onAddFolderRequested: root.addFolderRequested()
            }
        }
    }
}
