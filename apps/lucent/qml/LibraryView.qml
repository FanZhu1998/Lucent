pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Lucent

// Library: the contact sheet (blueprint section 2, "Import and organize"). Arrow keys move the
// selection, Enter opens the selected photo in Edit, Home/End jump to the first/last photo. The
// toolbar offers Relink when photos are missing, and the Library menu backs up the catalog. W7:
// Ctrl+click adds or removes a photo, Shift+click selects a range, Ctrl+A selects every photo, and
// Export batch exports the selection, every listed photo or a folder.
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

                Label {
                    objectName: "selectionSummary"
                    visible: root.controller.library.selectedCount > 1
                    text: qsTr("· %1 selected").arg(root.controller.library.selectedCount)
                    color: Theme.accent
                    font.pixelSize: Theme.fontBody
                }

                Label {
                    objectName: "missingSummary"
                    visible: root.controller.library.missingCount > 0
                    text: root.controller.library.missingCount === 1
                          ? qsTr("· 1 missing")
                          : qsTr("· %1 missing").arg(root.controller.library.missingCount)
                    color: Theme.unsupported
                    font.pixelSize: Theme.fontBody
                }

                Item {
                    Layout.fillWidth: true
                }

                StyledButton {
                    objectName: "relinkMissingButton"
                    visible: root.controller.canRelink && root.controller.library.missingCount > 0
                    text: qsTr("Relink…")
                    onClicked: relinkPicker.open()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Find missing photos in another folder. A file is relinked only when its content matches.")
                    ToolTip.delay: 600
                }

                StyledButton {
                    id: libraryMenuButton

                    objectName: "libraryMenuButton"
                    visible: root.controller.canBackUp
                    text: qsTr("Library ▾")
                    onClicked: libraryMenu.popup(libraryMenuButton, 0, libraryMenuButton.height)

                    Menu {
                        id: libraryMenu

                        objectName: "libraryMenu"

                        MenuItem {
                            objectName: "backUpCatalogItem"
                            text: qsTr("Back up catalog")
                            onTriggered: root.controller.backUpCatalog()
                        }
                        MenuItem {
                            text: qsTr("Open backup folder")
                            onTriggered: root.controller.openBackupFolder()
                        }
                        MenuItem {
                            enabled: root.controller.canRelink && root.controller.library.missingCount > 0
                            text: qsTr("Relink missing photos…")
                            onTriggered: relinkPicker.open()
                        }
                        MenuItem {
                            objectName: "exportFolderBatchItem"
                            enabled: root.controller.batchesAvailable
                            text: qsTr("Export a folder…")
                            onTriggered: batchFolderPicker.open()
                        }
                    }
                }

                StyledButton {
                    objectName: "exportBatchButton"
                    visible: root.controller.batchesAvailable
                    text: qsTr("Export batch…")
                    onClicked: root.controller.openBatchExport()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Export the selected photos, all photos or a folder into a folder you choose (Ctrl+Shift+E)")
                    ToolTip.delay: 600
                }

                StyledButton {
                    objectName: "toolbarAddFolder"
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

                // True while a Ctrl or Shift click moves the current photo: the selection is theirs.
                property bool keepSelection: false

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
                    multipleSelected: root.controller.library.selectedCount > 1
                    onClicked: {
                        grid.currentIndex = index
                        root.controller.library.selectOnly(index)
                        grid.forceActiveFocus(Qt.MouseFocusReason)
                    }
                    onToggleRequested: {
                        grid.keepSelection = true
                        grid.currentIndex = index
                        grid.keepSelection = false
                        root.controller.library.toggleSelected(index)
                        grid.forceActiveFocus(Qt.MouseFocusReason)
                    }
                    onRangeRequested: {
                        grid.keepSelection = true
                        grid.currentIndex = index
                        grid.keepSelection = false
                        root.controller.library.selectRange(index)
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
                    // Moving with the keyboard (or opening another photo) selects just that photo.
                    if (!keepSelection && currentIndex >= 0)
                        root.controller.library.selectOnly(currentIndex)
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
                    } else if (event.key === Qt.Key_A && (event.modifiers & Qt.ControlModifier)) {
                        root.controller.library.selectAll()
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

            FolderDialog {
                id: batchFolderPicker

                title: qsTr("Choose the folder of photos to export")
                onAccepted: root.controller.openFolderBatchExport(selectedFolder)
            }

            FolderDialog {
                id: relinkPicker

                title: qsTr("Find missing photos in a folder")
                onAccepted: root.controller.relinkMissing(selectedFolder)
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
