import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// One export job: source, destination, state with progress, and its actions. The properties
// marked required are ExportJobModel roles.
Rectangle {
    id: row

    required property int index
    required property string sourceName
    required property string outputFolder
    required property string destinationPath
    required property string formatLabel
    required property string stateKey
    required property string stateLabel
    required property real progress
    required property string progressLabel
    required property string message
    required property bool canCancel
    required property bool canOpenFolder
    required property string accessibleLabel

    property bool selected: false
    property bool showFocus: false

    signal cancelRequested()
    signal openFolderRequested()
    signal clicked()

    implicitHeight: 78
    radius: Theme.radius
    color: selected ? Theme.surfaceRaised : Theme.surface

    Accessible.role: Accessible.ListItem
    Accessible.name: accessibleLabel
    Accessible.selected: selected

    TapHandler {
        onTapped: row.clicked()
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 14
        anchors.rightMargin: 12
        spacing: 14

        Rectangle {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: 10
            implicitHeight: 10
            radius: 5
            color: Theme.jobStateColor(row.stateKey)
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 3

            RowLayout {
                spacing: 10
                Layout.fillWidth: true

                Text {
                    text: row.sourceName
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.Medium
                    elide: Text.ElideMiddle
                    Layout.maximumWidth: 320
                }

                Text {
                    text: row.formatLabel
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                }

                Item {
                    Layout.fillWidth: true
                }
            }

            Text {
                text: row.destinationPath.length > 0 ? row.destinationPath : row.outputFolder
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }

            Text {
                visible: row.message.length > 0
                text: row.message
                color: row.stateKey === "failed" ? Theme.unsupported : Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        ColumnLayout {
            Layout.preferredWidth: 170
            spacing: 5

            Text {
                text: row.progressLabel.length > 0 ? row.stateLabel + " · " + row.progressLabel : row.stateLabel
                color: Theme.jobStateColor(row.stateKey)
                font.pixelSize: Theme.fontBody
                horizontalAlignment: Text.AlignRight
                Layout.fillWidth: true
            }

            ProgressBar {
                visible: row.stateKey === "running"
                from: 0
                to: 1
                value: row.progress
                Layout.fillWidth: true
                Accessible.name: qsTr("Progress of %1").arg(row.sourceName)
            }
        }

        StyledToolButton {
            text: qsTr("Cancel")
            visible: row.canCancel
            onClicked: row.cancelRequested()
            Accessible.name: qsTr("Cancel export of %1").arg(row.sourceName)
        }

        StyledButton {
            // Only exported jobs have something to show; the header opens the folder at any time.
            visible: row.stateKey === "completed"
            text: qsTr("Open output folder")
            enabled: row.canOpenFolder
            onClicked: row.openFolderRequested()
            Accessible.name: qsTr("Open the output folder of %1").arg(row.sourceName)
        }
    }

    // Drawn inside the row: the list clips anything outside it.
    FocusRing {
        anchors.margins: 1
        shown: row.showFocus
    }
}
