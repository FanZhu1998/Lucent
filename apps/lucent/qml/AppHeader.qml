import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Window header: the three views of the application (blueprint section 2), background activity
// and the latest status message.
Rectangle {
    id: header

    required property AppController controller

    implicitHeight: Theme.toolbarHeight
    color: Theme.surface

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.divider
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.margin
        anchors.rightMargin: Theme.margin
        spacing: 2

        Text {
            text: "Lucent"
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
            Layout.rightMargin: 18
            Accessible.ignored: true
        }

        NavButton {
            objectName: "libraryTab"
            text: qsTr("Library")
            checked: header.controller.currentView === AppController.Library
            onClicked: header.controller.showView(AppController.Library)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Library (Ctrl+Alt+1)")
            ToolTip.delay: 800
        }

        NavButton {
            objectName: "editTab"
            text: qsTr("Edit")
            enabled: header.controller.library.count > 0
            checked: header.controller.currentView === AppController.Edit
            onClicked: header.controller.showView(AppController.Edit)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Edit (Ctrl+Alt+2)")
            ToolTip.delay: 800
        }

        NavButton {
            objectName: "exportsTab"
            text: qsTr("Exports")
            badgeText: header.controller.activeExportCount > 0 ? String(header.controller.activeExportCount) : ""
            checked: header.controller.currentView === AppController.Exports
            onClicked: header.controller.showView(AppController.Exports)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Exports (Ctrl+Alt+3)")
            ToolTip.delay: 800
        }

        Item {
            Layout.fillWidth: true
        }

        BusyIndicator {
            running: header.controller.busy
            visible: running
            Layout.preferredWidth: 22
            Layout.preferredHeight: 22
            Accessible.name: qsTr("Working")
        }

        // Background verification of the library's files (presence checks, content hashes).
        Label {
            objectName: "backgroundActivity"
            visible: text.length > 0
            text: header.controller.backgroundActivity
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            Layout.leftMargin: 4
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Label {
            objectName: "statusMessage"
            text: header.controller.statusMessage
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            elide: Text.ElideMiddle
            horizontalAlignment: Text.AlignRight
            Layout.fillWidth: true
            Layout.maximumWidth: 560
            Layout.leftMargin: 6
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        StyledToolButton {
            id: problemsButton

            objectName: "importProblemsButton"
            visible: header.controller.importProblems.length > 0
            text: qsTr("Details")
            onClicked: problemsPopup.open()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Why photos of the last import could not be added, or are limited or on removable media")
            ToolTip.delay: 800
        }

        // The library itself is unavailable or read-only (the catalog could not be opened).
        Rectangle {
            objectName: "libraryNotice"
            visible: header.controller.libraryNotice.length > 0
            implicitWidth: libraryNoticeText.implicitWidth + 16
            implicitHeight: 22
            radius: 11
            color: "transparent"
            border.width: 1
            border.color: Theme.unsupported
            Layout.leftMargin: 10

            Accessible.role: Accessible.StaticText
            Accessible.name: header.controller.libraryNotice

            Text {
                id: libraryNoticeText
                anchors.centerIn: parent
                text: qsTr("Library not saved")
                color: Theme.unsupported
                font.pixelSize: Theme.fontSmall
            }

            HoverHandler {
                id: libraryNoticeHover
            }

            ToolTip.visible: libraryNoticeHover.hovered
            ToolTip.text: header.controller.libraryNotice
            ToolTip.delay: 300
        }

        Rectangle {
            visible: header.controller.simulationNotice.length > 0
            implicitWidth: noticeText.implicitWidth + 16
            implicitHeight: 22
            radius: 11
            color: "transparent"
            border.width: 1
            border.color: Theme.attention
            Layout.leftMargin: 10

            Accessible.role: Accessible.StaticText
            Accessible.name: noticeText.text

            Text {
                id: noticeText
                anchors.centerIn: parent
                text: header.controller.simulationNotice
                color: Theme.attention
                font.pixelSize: Theme.fontSmall
            }

            HoverHandler {
                id: noticeHover
            }

            ToolTip.visible: noticeHover.hovered
            ToolTip.text: qsTr("Photos are not decoded. Metadata, thumbnails, previews and exports are simulated for development.")
            ToolTip.delay: 300
        }
    }

    // Per-file reasons of the last import: files that could not be read, and photos that are
    // Unsupported or Limited (blueprint section 3: skipped items get a specific explanation).
    Popup {
        id: problemsPopup

        objectName: "importProblemsPopup"
        parent: Overlay.overlay
        x: Math.round((parent.width - width) / 2)
        y: header.height + 8
        width: Math.min(680, parent.width - 64)
        height: Math.min(problemsList.contentHeight + 64, parent.height - header.height - 32)
        modal: true
        focus: true
        padding: 16

        background: Rectangle {
            color: Theme.surface
            radius: Theme.radius
            border.width: 1
            border.color: Theme.border
        }

        contentItem: ColumnLayout {
            spacing: 10

            Text {
                text: qsTr("Last import")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }

            ListView {
                id: problemsList

                objectName: "importProblemsList"
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: header.controller.importProblems
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Import details")

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }

                delegate: Text {
                    required property string modelData

                    width: ListView.view.width
                    text: modelData
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData
                }
            }
        }
    }
}
