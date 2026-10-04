import QtQuick
import QtQuick.Controls
import Lucent

// The opening screen of an empty library: Add photos, Add folder and the Include subfolders
// option (blueprint section 2, "Import and organize"), and W7's Export a folder (a folder batch
// adds the folder's photos first).
FocusScope {
    id: root

    required property AppController controller

    signal addPhotosRequested()
    signal addFolderRequested()
    signal exportFolderRequested()

    function takeFocus() {
        addPhotosButton.forceActiveFocus(Qt.OtherFocusReason)
    }

    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 480)
        spacing: 14

        Text {
            width: parent.width
            text: qsTr("Add photos to your library")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontHeading
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }

        Text {
            width: parent.width
            text: qsTr("Lucent works with your RAW files where they are. Originals are only read, never changed.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            lineHeight: 1.15
        }

        Item {
            width: parent.width
            height: 6
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 10

            StyledButton {
                id: addPhotosButton
                objectName: "addPhotosButton"
                text: qsTr("Add photos…")
                highlighted: true
                focus: true
                onClicked: root.addPhotosRequested()
            }

            StyledButton {
                objectName: "addFolderButton"
                text: qsTr("Add folder…")
                onClicked: root.addFolderRequested()
            }

            StyledButton {
                objectName: "emptyExportFolderButton"
                visible: root.controller.batchesAvailable
                text: qsTr("Export a folder…")
                onClicked: root.exportFolderRequested()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Add a folder's photos to the library and export them into a folder you choose")
                ToolTip.delay: 600
            }
        }

        StyledCheckBox {
            objectName: "includeSubfoldersOption"
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Include subfolders")
            checked: root.controller.includeSubfolders
            onToggled: root.controller.includeSubfolders = checked
        }

        Text {
            width: parent.width
            text: qsTr("Ctrl+O adds photos · Ctrl+Shift+O adds a folder")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }
}
