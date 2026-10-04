import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// One batch in the Exports view (blueprint section 2, step 5): its state, live counts (exported,
// remaining, failed, skipped, for review), the current stage, an ETA when the service has measured
// one, the actions, and an attention banner with the service's actionable text and Resume. A batch
// of one photo (a single export) is a compact row. W8: an automatic batch is tagged ("Auto",
// "Analyse only"), counts analysed photos when it only analyses, and shows the related-shot
// pre-pass. The properties marked required are BatchListModel roles.
Rectangle {
    id: row

    required property int index
    required property string name
    required property bool single
    required property string selection
    required property string folder
    required property string stateKey
    required property string stateLabel
    required property int total
    required property int completed
    required property int remaining
    required property int failed
    required property int skipped
    required property int leftOut
    required property int review
    required property real progress
    required property string progressLabel
    required property string stage
    required property string eta
    required property bool hasAttention
    required property string attention
    required property bool canPause
    required property bool canResume
    required property bool canUseFolder
    required property bool canCancel
    required property bool canRetry
    required property bool canOpenFolder
    required property bool canOpenReport
    required property string outputPath
    required property string outcomeReason
    required property string timesLabel
    required property bool isActive
    required property string accessibleLabel
    required property string automaticKey
    required property string automaticLabel
    required property string completedLabel
    required property string analysis

    property bool selected: false
    property bool showFocus: false

    signal clicked()
    signal pauseRequested()
    signal resumeRequested()
    signal useFolderRequested()
    signal cancelRequested()
    signal retryRequested()
    signal openFolderRequested()
    signal openReportRequested()

    readonly property bool running: stateKey === "running" || stateKey === "waiting"

    implicitHeight: content.implicitHeight + 24
    radius: Theme.radius
    color: selected ? Theme.surfaceRaised : Theme.surface
    border.width: hasAttention ? 1 : 0
    border.color: Theme.attention

    Accessible.role: Accessible.ListItem
    Accessible.name: accessibleLabel
    Accessible.selected: selected

    TapHandler {
        onTapped: row.clicked()
    }

    ColumnLayout {
        id: content

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin: 14
        anchors.rightMargin: 12
        anchors.topMargin: 12
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: 10
                implicitHeight: 10
                radius: 5
                color: Theme.batchStateColor(row.stateKey)
            }

            Text {
                text: row.name
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.Medium
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }

            // W8: an automatic batch (Auto, or Analyse only).
            Badge {
                objectName: "batchAutomaticTag"
                visible: row.automaticKey !== "off"
                text: row.automaticLabel
                dotColor: Theme.accent
                detail: row.automaticKey === "analyzeOnly"
                        ? qsTr("Each photo gets an automatic revision to review in Edit; nothing is exported.")
                        : qsTr("Each photo is analysed, saved as an automatic revision, checked at full size and exported.")
            }

            // A single export's outputs can be recommended for review too.
            Text {
                objectName: "batchReviewMark"
                visible: row.single && row.review > 0
                text: qsTr("Review")
                color: Theme.attention
                font.pixelSize: Theme.fontSmall
            }

            Text {
                objectName: "batchStateLabel"
                text: row.single || row.stateKey === "completed" || !row.isActive
                      ? row.stateLabel
                      : row.stateLabel + " · " + row.progressLabel
                color: Theme.batchStateColor(row.stateKey)
                font.pixelSize: Theme.fontBody
            }
        }

        // Where the outputs go (a single export: its file once written).
        Text {
            text: row.single && row.outputPath.length > 0 ? row.outputPath : row.folder
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            elide: Text.ElideMiddle
            Layout.fillWidth: true
            Layout.leftMargin: 20
        }

        // A single export that ended without its photo: why.
        Text {
            objectName: "batchOutcomeReason"
            visible: row.single && row.outcomeReason.length > 0
            text: row.outcomeReason
            color: row.stateKey === "cancelled" || row.stateKey === "failed" ? Theme.unsupported : Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.leftMargin: 20
        }

        // Live counts: exported, remaining, failed, skipped, for review.
        Flow {
            objectName: "batchCounts"
            visible: !row.single
            spacing: 14
            Layout.fillWidth: true
            Layout.leftMargin: 20

            Repeater {
                model: [
                    { key: "exported", label: row.completedLabel, shown: true },
                    { key: "remaining", label: qsTr("%1 remaining").arg(row.remaining), shown: row.remaining > 0 },
                    { key: "failed", label: qsTr("%1 failed").arg(row.failed), shown: row.failed > 0 },
                    { key: "skipped", label: qsTr("%1 skipped").arg(row.skipped), shown: row.skipped > 0 },
                    { key: "leftOut", label: qsTr("%1 left out").arg(row.leftOut), shown: row.leftOut > 0 },
                    { key: "review", label: qsTr("%1 for review").arg(row.review), shown: row.review > 0 }
                ]

                delegate: Text {
                    required property var modelData

                    visible: modelData.shown
                    text: modelData.label
                    color: modelData.key === "failed" ? Theme.unsupported
                         : modelData.key === "review" ? Theme.attention
                         : modelData.key === "exported" ? Theme.textPrimary : Theme.textSecondary
                    font.pixelSize: Theme.fontSmall
                }
            }
        }

        RowLayout {
            visible: row.isActive
            Layout.fillWidth: true
            Layout.leftMargin: 20
            spacing: 12

            ProgressBar {
                objectName: "batchProgress"
                visible: !row.single || row.running
                from: 0
                to: 1
                value: row.progress
                Layout.fillWidth: true
                Layout.maximumWidth: 420
                Accessible.name: qsTr("Progress of %1").arg(row.name)
            }

            Text {
                objectName: "batchEta"
                visible: row.eta.length > 0
                text: row.eta
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
            }

            Item {
                Layout.fillWidth: true
            }
        }

        // W8: the related-shot pre-pass (every photo is analysed before any is exported).
        Text {
            objectName: "batchAnalysis"
            visible: row.analysis.length > 0
            text: row.analysis
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.leftMargin: 20
        }

        // The current stage of the running photos.
        Text {
            objectName: "batchStage"
            visible: row.stage.length > 0
            text: row.stage
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            elide: Text.ElideRight
            Layout.fillWidth: true
            Layout.leftMargin: 20
        }

        // Attention: the service paused the batch (disk full, drive removed, permission).
        Rectangle {
            objectName: "batchAttention"
            visible: row.hasAttention
            color: Theme.attentionBackground
            radius: Theme.radius
            implicitHeight: attentionRow.implicitHeight + 16
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.topMargin: 4

            Accessible.role: Accessible.AlertMessage
            Accessible.name: row.attention

            RowLayout {
                id: attentionRow

                anchors.fill: parent
                anchors.margins: 8
                spacing: 10

                Text {
                    text: "!"
                    color: Theme.attention
                    font.pixelSize: Theme.fontTitle
                    font.bold: true
                    Accessible.ignored: true
                }

                Text {
                    text: row.attention
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                StyledButton {
                    objectName: "batchAttentionUseFolder"
                    visible: row.canUseFolder
                    text: qsTr("Use this folder")
                    highlighted: true
                    onClicked: row.useFolderRequested()
                    Accessible.name: qsTr("Continue %1 in the folder now at its output path").arg(row.name)
                }

                StyledButton {
                    objectName: "batchAttentionResume"
                    text: qsTr("Resume")
                    highlighted: !row.canUseFolder
                    onClicked: row.resumeRequested()
                    Accessible.name: qsTr("Resume %1").arg(row.name)
                }
            }
        }

        // Actions.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.topMargin: 2
            spacing: 6

            Text {
                visible: !row.isActive && row.timesLabel.length > 0
                text: row.timesLabel
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            Item {
                visible: row.isActive || row.timesLabel.length === 0
                Layout.fillWidth: true
            }

            StyledToolButton {
                objectName: "batchPause"
                visible: row.canPause
                text: qsTr("Pause")
                onClicked: row.pauseRequested()
                Accessible.name: qsTr("Pause %1").arg(row.name)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Photos being exported finish; nothing new starts until you resume.")
                ToolTip.delay: 800
            }

            StyledToolButton {
                objectName: "batchResume"
                visible: row.canResume && !row.hasAttention
                text: qsTr("Resume")
                onClicked: row.resumeRequested()
                Accessible.name: qsTr("Resume %1").arg(row.name)
            }

            StyledToolButton {
                objectName: "batchCancel"
                visible: row.canCancel
                text: qsTr("Cancel")
                onClicked: row.cancelRequested()
                Accessible.name: qsTr("Cancel %1").arg(row.name)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Photos not exported yet are cancelled. Files already exported stay.")
                ToolTip.delay: 800
            }

            StyledToolButton {
                objectName: "batchRetry"
                visible: row.canRetry
                text: qsTr("Retry failed")
                onClicked: row.retryRequested()
                Accessible.name: qsTr("Retry the failed photos of %1").arg(row.name)
            }

            StyledToolButton {
                objectName: "batchOpenReport"
                visible: row.canOpenReport && !row.single  // a single export's is in its report pane
                text: qsTr("Open report")
                onClicked: row.openReportRequested()
                Accessible.name: qsTr("Open the report of %1").arg(row.name)
            }

            StyledButton {
                objectName: "batchOpenFolder"
                visible: row.canOpenFolder && (row.completed > 0 || !row.isActive)
                text: qsTr("Open output folder")
                onClicked: row.openFolderRequested()
                Accessible.name: qsTr("Open the output folder of %1").arg(row.name)
            }
        }
    }

    // Drawn inside the row: the list clips anything outside it.
    FocusRing {
        anchors.margins: 1
        shown: row.showFocus
    }
}
