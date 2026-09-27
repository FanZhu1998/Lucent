pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Exports: every export job of this session with its state and progress, and Open output folder
// (blueprint section 2, "Automatic batch development" step 5). Up/Down move through the jobs.
FocusScope {
    id: root

    required property AppController controller

    function takeFocus() {
        if (jobs.count > 0)
            jobs.forceActiveFocus(Qt.OtherFocusReason)
        else
            openFolderButton.forceActiveFocus(Qt.OtherFocusReason)
    }

    readonly property string summary: {
        const exports = controller.exports
        const parts = []
        if (exports.activeCount > 0)
            parts.push(qsTr("%1 in progress").arg(exports.activeCount))
        if (exports.completedCount > 0)
            parts.push(qsTr("%1 exported").arg(exports.completedCount))
        if (exports.failedCount > 0)
            parts.push(qsTr("%1 failed").arg(exports.failedCount))
        return parts.join(" · ")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.toolbarHeight
            color: Theme.window

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.margin
                anchors.rightMargin: Theme.margin
                spacing: 14

                Text {
                    text: qsTr("Exports")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }

                Text {
                    objectName: "exportSummary"
                    text: root.summary
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }

                Item {
                    Layout.fillWidth: true
                }

                StyledButton {
                    id: openFolderButton

                    objectName: "openLastOutputFolder"
                    text: qsTr("Open output folder")
                    enabled: root.controller.lastOutputFolder.toString().length > 0
                    onClicked: root.controller.openOutputFolder(-1)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Open the folder of the most recent export")
                    ToolTip.delay: 800
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: jobs

                objectName: "exportJobs"
                anchors.fill: parent
                anchors.margins: 12
                visible: count > 0
                spacing: 6
                clip: true
                focus: true
                model: root.controller.exports
                keyNavigationEnabled: true
                activeFocusOnTab: true
                highlightFollowsCurrentItem: false
                boundsBehavior: Flickable.StopAtBounds

                Accessible.role: Accessible.List
                Accessible.name: qsTr("Export jobs")

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }

                delegate: ExportJobRow {
                    width: jobs.width
                    selected: ListView.isCurrentItem
                    showFocus: ListView.isCurrentItem && jobs.activeFocus
                    onClicked: {
                        jobs.currentIndex = index
                        jobs.forceActiveFocus(Qt.MouseFocusReason)
                    }
                    onCancelRequested: root.controller.cancelExport(index)
                    onOpenFolderRequested: root.controller.openOutputFolder(index)
                }
            }

            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - 64, 460)
                spacing: 10
                visible: jobs.count === 0

                Text {
                    width: parent.width
                    text: qsTr("No exports yet")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontHeading
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }

                Text {
                    width: parent.width
                    text: qsTr("Open a photo and choose Export to write it into a folder you pick. Existing files are never overwritten.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
