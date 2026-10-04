pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Lucent

// Exports (blueprint section 2, "Automatic batch development" step 5): every batch, newest first,
// with live counts, the current stage, an ETA when the service has measured one, pause, resume,
// cancel and retry failed, attention banners with Resume, and the selected batch's report with
// Open output folder and Open report. Single exports are batches of one. The newest batches and
// every unfinished one are listed; Show older exports pages further back. When this session cannot
// keep batches (the library is not saved), the session's in-process export jobs are listed instead.
// Up/Down move through the list.
FocusScope {
    id: root

    required property AppController controller

    readonly property bool batchesAvailable: controller.batchesAvailable

    function takeFocus() {
        const list = root.batchesAvailable ? batchList : jobs
        if (list.count > 0)
            list.forceActiveFocus(Qt.OtherFocusReason)
        else
            openFolderButton.forceActiveFocus(Qt.OtherFocusReason)
    }

    readonly property string jobSummary: {
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
                    text: root.batchesAvailable ? root.controller.batches.summary : root.jobSummary
                    color: root.controller.batches.attentionCount > 0 ? Theme.attention : Theme.textSecondary
                    font.pixelSize: Theme.fontBody
                }

                Item {
                    Layout.fillWidth: true
                }

                StyledButton {
                    objectName: "exportsFolderBatchButton"
                    visible: root.batchesAvailable
                    text: qsTr("Export a folder…")
                    onClicked: exportFolderPicker.open()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Add a folder's photos to the library and export them into a folder you choose")
                    ToolTip.delay: 800
                }

                StyledButton {
                    objectName: "exportsBatchButton"
                    visible: root.batchesAvailable
                    text: qsTr("Export batch…")
                    enabled: root.controller.library.count > 0
                    onClicked: root.controller.openBatchExport()
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

        // A batch being prepared: the folder is added to the library and the originals are
        // fingerprinted before the batch exists.
        Rectangle {
            objectName: "batchPreparing"
            visible: root.batchesAvailable && root.controller.batches.preparing
            color: Theme.surface
            radius: Theme.radius
            implicitHeight: preparingRow.implicitHeight + 20
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 8

            RowLayout {
                id: preparingRow

                anchors.fill: parent
                anchors.margins: 10
                anchors.leftMargin: 14
                spacing: 12

                BusyIndicator {
                    running: parent.visible
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                }

                ColumnLayout {
                    spacing: 2
                    Layout.fillWidth: true

                    Text {
                        text: qsTr("Preparing %1").arg(root.controller.batches.preparingName)
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Text {
                        objectName: "batchPreparingText"
                        text: root.controller.batches.preparingText
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                }

                ProgressBar {
                    visible: root.controller.batches.preparingProgress >= 0
                    from: 0
                    to: 1
                    value: Math.max(0, root.controller.batches.preparingProgress)
                    Layout.preferredWidth: 160
                }

                StyledToolButton {
                    text: qsTr("Cancel")
                    onClicked: root.controller.batches.cancelPreparing()
                    Accessible.name: qsTr("Cancel preparing the batch")
                }
            }
        }

        // --- Batches ----------------------------------------------------------------------------
        RowLayout {
            visible: root.batchesAvailable
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 12
            spacing: 12

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: batchList

                    // The newest batch while the selection was the newest (or none): a new batch is
                    // shown as soon as it exists.
                    property bool followNewest: true

                    objectName: "batchList"
                    anchors.fill: parent
                    visible: count > 0
                    spacing: 6
                    clip: true
                    focus: root.batchesAvailable
                    model: root.controller.batches
                    keyNavigationEnabled: true
                    activeFocusOnTab: true
                    highlightFollowsCurrentItem: false
                    boundsBehavior: Flickable.StopAtBounds

                    Accessible.role: Accessible.List
                    Accessible.name: qsTr("Batches")

                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AsNeeded
                    }

                    onCountChanged: {
                        if (count > 0 && (followNewest || currentIndex < 0))
                            currentIndex = 0
                        Qt.callLater(() => root.controller.showBatchReport(batchList.currentIndex))
                    }
                    onCurrentIndexChanged: root.controller.showBatchReport(currentIndex)
                    // Only the user's choice decides whether the newest batch is followed (a new batch
                    // shifts the current index without a choice).
                    Keys.onReleased: followNewest = currentIndex === 0

                    // Older batches are listed on request (every unfinished one is listed already).
                    footer: Item {
                        width: batchList.width
                        height: root.controller.batches.hasOlder ? olderButton.implicitHeight + 16 : 0
                        visible: root.controller.batches.hasOlder

                        StyledButton {
                            id: olderButton

                            objectName: "batchListOlder"
                            anchors.centerIn: parent
                            text: qsTr("Show older exports")
                            onClicked: root.controller.batches.loadOlder()
                        }
                    }

                    delegate: BatchRow {
                        width: batchList.width
                        selected: ListView.isCurrentItem
                        showFocus: ListView.isCurrentItem && batchList.activeFocus
                        onClicked: {
                            batchList.currentIndex = index
                            batchList.followNewest = index === 0
                            batchList.forceActiveFocus(Qt.MouseFocusReason)
                        }
                        onPauseRequested: root.controller.batches.pause(index)
                        onResumeRequested: root.controller.batches.resume(index)
                        onUseFolderRequested: root.controller.batches.useReplacedFolder(index)
                        onCancelRequested: root.controller.batches.cancel(index)
                        onRetryRequested: root.controller.batches.retryFailed(index)
                        onOpenFolderRequested: root.controller.openBatchFolder(index)
                        onOpenReportRequested: root.controller.openBatchReport(index)
                    }
                }

                Column {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 64, 460)
                    spacing: 10
                    visible: batchList.count === 0 && !root.controller.batches.preparing

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
                        text: qsTr("Choose Export batch in the library to export many photos into a folder you pick, or open a photo and choose Export. Exports continue after Lucent is closed and opened again; files are never overwritten unless you ask for it.")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                    }
                }
            }

            BatchReportPane {
                objectName: "batchReportPane"
                controller: root.controller
                visible: batchList.count > 0
                batchRow: batchList.currentIndex
                canOpenReport: batchList.currentItem ? batchList.currentItem.canOpenReport : false
                Layout.fillHeight: true
                Layout.preferredWidth: Math.max(320, Math.round(root.width * 0.4))
            }
        }

        FolderDialog {
            id: exportFolderPicker

            title: qsTr("Choose the folder of photos to export")
            onAccepted: root.controller.openFolderBatchExport(selectedFolder)
        }

        // --- In-process export jobs (no batches in this session) ---------------------------------
        Item {
            visible: !root.batchesAvailable
            Layout.fillWidth: true
            Layout.fillHeight: true

            Text {
                id: unavailableNotice

                objectName: "batchesUnavailable"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                visible: text.length > 0
                text: root.controller.batches.unavailableReason
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.Wrap
            }

            ListView {
                id: jobs

                objectName: "exportJobs"
                anchors.fill: parent
                anchors.margins: 12
                anchors.topMargin: unavailableNotice.visible ? unavailableNotice.height + 20 : 12
                visible: count > 0
                spacing: 6
                clip: true
                focus: !root.batchesAvailable
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
