pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Automatic settings (blueprint section 2, "Detailed individual editing": "Auto creates a new
// editable revision ... The optimizer may change only unlocked controls and must report when the
// locks prevent its usual technical targets"; W8). While Auto runs: its stage, progress and Cancel.
// Afterwards, for a current revision Auto (or an automatic batch) made: the confidence, whether a
// review is recommended and why, what changed, what the locks kept and prevented, why a conservative
// fallback replaced the first choice after a failed full-size check, the related-shot group, and the
// other findings. Hidden when there is nothing to say.
PanelSection {
    id: panel

    required property EditSession edit
    readonly property AutoSettingsModel automatic: edit.automatic

    title: qsTr("Automatic settings")
    visible: automatic.running || automatic.hasSummary || automatic.lastResultWorthShowing

    actions: [
        StyledToolButton {
            objectName: "autoCancelButton"
            visible: panel.automatic.running
            enabled: !panel.automatic.cancelling
            text: qsTr("Cancel")
            onClicked: panel.edit.cancelAuto()
            Accessible.name: qsTr("Cancel Auto")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Stop Auto; nothing is changed")
            ToolTip.delay: 800
        }
    ]

    component Line: Text {
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    // ---- A run in progress ----------------------------------------------------------------------
    ColumnLayout {
        objectName: "autoProgressRow"
        visible: panel.automatic.running
        spacing: 4
        Layout.fillWidth: true

        RowLayout {
            spacing: 8
            Layout.fillWidth: true

            BusyIndicator {
                running: panel.automatic.running
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
            }

            Text {
                objectName: "autoStage"
                text: panel.automatic.stageText
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        ProgressBar {
            objectName: "autoProgress"
            from: 0
            to: 1
            value: panel.automatic.progress
            Layout.fillWidth: true

            Accessible.name: panel.automatic.stageText
        }
    }

    // ---- The last run's outcome when it says more than the summary (nothing saved, or a branch) ----
    RowLayout {
        objectName: "autoResultRow"
        visible: !panel.automatic.running && panel.automatic.lastResultWorthShowing
        spacing: 4
        Layout.fillWidth: true

        Text {
            objectName: "autoResult"
            text: panel.automatic.lastResult
            color: panel.automatic.lastResultFailed ? Theme.attention : Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        StyledToolButton {
            text: "×"
            implicitWidth: 22
            implicitHeight: 22
            onClicked: panel.automatic.clearLastResult()
            Accessible.name: qsTr("Dismiss")
        }
    }

    // ---- What Auto did to the current revision ---------------------------------------------------
    ColumnLayout {
        objectName: "autoSummary"
        visible: panel.automatic.hasSummary
        spacing: 6
        Layout.fillWidth: true

        RowLayout {
            spacing: 8
            Layout.fillWidth: true

            Text {
                objectName: "autoOrigin"
                text: panel.automatic.origin
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Text {
                objectName: "autoConfidence"
                visible: text.length > 0
                text: panel.automatic.confidenceText
                color: panel.automatic.confidenceLevel === "high" ? Theme.verified
                     : panel.automatic.confidenceLevel === "medium" ? Theme.textPrimary : Theme.attention
                font.pixelSize: Theme.fontSmall
                font.weight: Font.Medium
            }
        }

        // A review is recommended: why.
        Rectangle {
            objectName: "autoReview"
            visible: panel.automatic.review
            color: Theme.attentionBackground
            radius: Theme.radius
            implicitHeight: reviewColumn.implicitHeight + 12
            Layout.fillWidth: true

            Accessible.role: Accessible.AlertMessage
            Accessible.name: qsTr("Review recommended: %1").arg(panel.automatic.reviewReasons.join(" "))

            ColumnLayout {
                id: reviewColumn

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 6
                anchors.leftMargin: 8
                spacing: 2

                Text {
                    text: qsTr("Review recommended")
                    color: Theme.attention
                    font.pixelSize: Theme.fontSmall
                    font.weight: Font.DemiBold
                }

                Repeater {
                    model: panel.automatic.reviewReasons

                    delegate: Line {
                        required property string modelData

                        text: modelData
                        color: Theme.textPrimary
                    }
                }
            }
        }

        Line {
            objectName: "autoChanged"
            text: qsTr("Changed: %1").arg(panel.automatic.changedText)
            color: Theme.textPrimary
        }

        Line {
            objectName: "autoLocked"
            visible: text.length > 0
            text: panel.automatic.lockedText
        }

        Repeater {
            objectName: "autoLockNotes"
            model: panel.automatic.lockNotes

            delegate: Line {
                required property string modelData

                text: modelData
                color: panel.automatic.locksPreventedTargets ? Theme.attention : Theme.textSecondary
            }
        }

        // Why the first choice was replaced (the review box says that it was): plain text, so it can
        // be read and reached without a pointer.
        Line {
            objectName: "autoFallback"
            visible: text.length > 0
            text: panel.automatic.fallbackText
            color: Theme.textTertiary
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Line {
            objectName: "autoGroup"
            visible: text.length > 0
            text: panel.automatic.groupText
        }

        Repeater {
            objectName: "autoNotes"
            model: panel.automatic.notes

            delegate: Line {
                required property string modelData

                text: modelData
                color: Theme.textTertiary
            }
        }
    }
}
