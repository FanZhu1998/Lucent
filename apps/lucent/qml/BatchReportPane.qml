pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// The report of the batch selected in the Exports view (blueprint section 2, step 5): exported,
// skipped, failed and review-recommended photos with their reasons and suggested actions, filtered
// by outcome, with Open output folder and Open report.
Rectangle {
    id: pane

    required property AppController controller
    readonly property BatchReportModel report: controller.batchReport
    // The row of the selected batch in controller.batches (its actions).
    property int batchRow: -1
    property bool canOpenReport: false

    color: Theme.surface
    radius: Theme.radius

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 14
        spacing: 8
        visible: pane.report.batchId > 0

        Text {
            objectName: "batchReportTitle"
            text: pane.report.name.length > 0 ? pane.report.name : qsTr("Report")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            Layout.fillWidth: true
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }

        Text {
            visible: text.length > 0
            text: [pane.report.timesLabel, pane.report.format].filter(part => part.length > 0).join(" · ")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        Text {
            visible: pane.report.folder.length > 0
            text: pane.report.folder
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            elide: Text.ElideMiddle
            Layout.fillWidth: true
        }

        RowLayout {
            spacing: 6
            Layout.fillWidth: true

            StyledButton {
                objectName: "reportOpenFolder"
                text: qsTr("Open output folder")
                enabled: pane.batchRow >= 0
                onClicked: pane.controller.openBatchFolder(pane.batchRow)
            }

            StyledButton {
                objectName: "reportOpenFile"
                text: qsTr("Open report")
                visible: pane.canOpenReport
                onClicked: pane.controller.openBatchReport(pane.batchRow)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("The readable report saved in the output folder")
                ToolTip.delay: 800
            }

            Item {
                Layout.fillWidth: true
            }
        }

        // Outcome filters with their counts.
        Flow {
            objectName: "reportFilters"
            // A single export needs no filters (unless one is chosen: it can always be switched back).
            visible: pane.report.total > 1 || pane.report.filter !== "all"
            spacing: 4
            Layout.fillWidth: true
            Layout.topMargin: 4

            ButtonGroup {
                id: filterGroup
            }

            Repeater {
                model: [
                    { key: "all", label: qsTr("All %1").arg(pane.report.total), shown: true },
                    { key: "exported", label: qsTr("Exported %1").arg(pane.report.exported), shown: true },
                    { key: "skipped", label: qsTr("Skipped %1").arg(pane.report.skipped), shown: pane.report.skipped > 0 },
                    { key: "failed", label: qsTr("Failed %1").arg(pane.report.failed), shown: pane.report.failed > 0 },
                    { key: "review", label: qsTr("For review %1").arg(pane.report.review), shown: pane.report.review > 0 },
                    { key: "cancelled", label: qsTr("Cancelled %1").arg(pane.report.cancelled), shown: pane.report.cancelled > 0 }
                ]

                delegate: StyledToolButton {
                    required property var modelData

                    visible: modelData.shown || pane.report.filter === modelData.key
                    text: modelData.label
                    checkable: true
                    checked: pane.report.filter === modelData.key
                    ButtonGroup.group: filterGroup
                    onClicked: pane.report.filter = modelData.key
                }
            }
        }

        // Batch-level notes (removable destination, GPS written, files replaced, ...).
        Repeater {
            model: pane.report.notes

            delegate: Text {
                required property string modelData

                text: modelData
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
        }

        Text {
            visible: pane.report.error.length > 0
            text: pane.report.error
            color: Theme.unsupported
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        ListView {
            id: entries

            objectName: "reportEntries"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: pane.report
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Photos of the batch")

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            delegate: Rectangle {
                id: entry

                required property string sourceName
                required property string outcomeKey
                required property string outcomeLabel
                required property string output
                required property string outputName
                required property string reason
                required property string action
                required property var review
                required property bool hasReview
                required property string accessibleLabel

                width: ListView.view.width
                implicitHeight: entryColumn.implicitHeight + 12
                color: "transparent"
                Accessible.role: Accessible.ListItem
                Accessible.name: accessibleLabel

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: Theme.divider
                }

                ColumnLayout {
                    id: entryColumn

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    RowLayout {
                        spacing: 8
                        Layout.fillWidth: true

                        Text {
                            text: entry.sourceName
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }

                        Text {
                            visible: entry.hasReview
                            text: qsTr("Review")
                            color: Theme.attention
                            font.pixelSize: Theme.fontSmall
                        }

                        Text {
                            text: entry.outcomeLabel
                            color: Theme.batchStateColor(entry.outcomeKey)
                            font.pixelSize: Theme.fontSmall
                        }
                    }

                    Text {
                        visible: entry.outputName.length > 0
                        text: "→ " + entry.outputName
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true

                        TapHandler {
                            onDoubleTapped: pane.controller.showFileInFolder(entry.output)
                        }
                    }

                    // The outcome label already says "Exported" or "Waiting".
                    Text {
                        visible: entry.reason.length > 0 && entry.outcomeKey !== "exported"
                                 && entry.outcomeKey !== "pending"
                        text: entry.reason
                        color: entry.outcomeKey === "failed" ? Theme.unsupported : Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    Text {
                        visible: entry.action.length > 0
                        text: entry.action
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    Text {
                        visible: entry.hasReview
                        text: entry.review.join("\n")
                        color: Theme.attention
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: entries.count === 0 && !pane.report.loading && pane.report.hasReport
                text: qsTr("No photos in this group")
                color: Theme.textTertiary
                font.pixelSize: Theme.fontBody
            }
        }
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - 48
        visible: pane.report.batchId === 0
        text: qsTr("Select a batch to see its report")
        color: Theme.textTertiary
        font.pixelSize: Theme.fontBody
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
    }
}
