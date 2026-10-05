pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// The photo's revisions (blueprint section 2: "undo/redo, and reset to as-shot or an earlier
// recipe"): Undo, Redo and Reset to as shot, and every revision, newest first, with what it did and
// when. Clicking a revision shows it (it becomes the current one; nothing is deleted, and an undone
// branch stays listed, dimmed).
ColumnLayout {
    id: panel

    required property EditSession edit

    spacing: 6

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        StyledButton {
            objectName: "undoButton"
            text: qsTr("Undo")
            enabled: panel.edit.canUndo
            onClicked: panel.edit.undo()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Undo (Ctrl+Z)")
            ToolTip.delay: 800
        }

        StyledButton {
            objectName: "redoButton"
            text: qsTr("Redo")
            enabled: panel.edit.canRedo
            onClicked: panel.edit.redo()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Redo (Ctrl+Y)")
            ToolTip.delay: 800
        }

        Item {
            Layout.fillWidth: true
        }

        StyledButton {
            objectName: "resetAsShotButton"
            text: qsTr("Reset to as shot")
            enabled: panel.edit.historyAvailable && panel.edit.ready && (panel.edit.edited || panel.edit.revisionNumber > 0)
            onClicked: panel.edit.resetToAsShot()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Back to the camera's settings, as a new revision that Undo takes back")
            ToolTip.delay: 800
        }
    }

    ListView {
        id: list

        objectName: "historyList"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(contentHeight, 220)
        clip: true
        model: panel.edit.history
        currentIndex: panel.edit.history.currentRow
        keyNavigationEnabled: true
        activeFocusOnTab: true
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height

        Accessible.role: Accessible.List
        Accessible.name: qsTr("Revisions")

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        Keys.onReturnPressed: panel.edit.selectRevision(currentIndex)
        Keys.onEnterPressed: panel.edit.selectRevision(currentIndex)
        // Enter selects the revision, also while a tool whose Enter shortcut would close it is open.
        Keys.onShortcutOverride: event => {
            event.accepted = event.key === Qt.Key_Return || event.key === Qt.Key_Enter
        }

        delegate: Rectangle {
            id: entry

            required property int index
            required property string title
            required property string detail
            required property bool current
            required property bool onCurrentPath
            required property string accessibleLabel

            width: ListView.view.width
            height: 34
            radius: Theme.radius
            color: current ? Theme.accentMuted : hover.hovered ? Theme.surfaceHover : "transparent"
            opacity: onCurrentPath ? 1 : 0.6

            Accessible.role: Accessible.ListItem
            Accessible.name: accessibleLabel
            Accessible.selected: current

            HoverHandler {
                id: hover

                enabled: panel.edit.historyAvailable
            }

            TapHandler {
                enabled: panel.edit.historyAvailable
                onTapped: panel.edit.selectRevision(entry.index)
            }

            Column {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 1

                Text {
                    width: parent.width
                    text: entry.title
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSmall
                    font.weight: entry.current ? Font.DemiBold : Font.Normal
                    elide: Text.ElideRight
                }

                Text {
                    width: parent.width
                    text: entry.detail
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontSmall - 1
                    elide: Text.ElideRight
                }
            }

            FocusRing {
                shown: list.activeFocus && list.currentIndex === entry.index
            }
        }
    }
}
