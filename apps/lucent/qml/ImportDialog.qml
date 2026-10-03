import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Lucent

// Import options for the photos and folders just chosen (blueprint section 2, "Import and
// organize"): the selected source location, Include subfolders, and Copy into library folder with
// a destination the photographer chooses. It opens whenever the controller has a pending import
// (Add photos, Add folder, drag and drop) and closes when that is confirmed or cancelled.
Dialog {
    id: dialog

    required property AppController controller

    readonly property var paths: controller.pendingImport
    readonly property bool hasDestination: controller.copyDestination.toString().length > 0

    objectName: "importDialog"
    title: qsTr("Add to library")
    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, Overlay.overlay ? Overlay.overlay.width - 64 : 560)
    padding: 20
    topPadding: 12
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: Theme.surface
        radius: Theme.radius
        border.width: 1
        border.color: Theme.border
    }

    header: Text {
        text: dialog.title
        color: Theme.textPrimary
        font.pixelSize: Theme.fontTitle
        font.weight: Font.DemiBold
        leftPadding: 20
        topPadding: 18
        Accessible.role: Accessible.Heading
        Accessible.name: text
    }

    Connections {
        target: dialog.controller

        function onPendingImportChanged() {
            if (dialog.controller.pendingImport.length > 0)
                dialog.open()
            else
                dialog.close()
        }
    }

    onOpened: addButton.forceActiveFocus(Qt.OtherFocusReason)
    // Esc or a click on Cancel: nothing is imported.
    onRejected: controller.cancelImport()
    onClosed: {
        if (controller.pendingImport.length > 0)
            controller.cancelImport()
    }

    contentItem: ColumnLayout {
        spacing: 12

        Text {
            text: dialog.paths.length === 1 ? qsTr("Selected location")
                                            : qsTr("%1 selected items").arg(dialog.paths.length)
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
        }

        // The selected source location (first few paths).
        Column {
            objectName: "importSources"
            spacing: 2
            Layout.fillWidth: true

            Repeater {
                model: dialog.paths.slice(0, 4)

                delegate: Text {
                    required property string modelData

                    width: parent ? parent.width : 0
                    text: dialog.controller.nativePath(modelData)
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    elide: Text.ElideMiddle
                }
            }

            Text {
                visible: dialog.paths.length > 4
                text: qsTr("and %1 more").arg(dialog.paths.length - 4)
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
            }
        }

        StyledCheckBox {
            objectName: "importIncludeSubfolders"
            text: qsTr("Include subfolders")
            checked: dialog.controller.includeSubfolders
            onToggled: dialog.controller.includeSubfolders = checked
        }

        StyledCheckBox {
            objectName: "importCopyIntoLibrary"
            text: qsTr("Copy into library folder")
            checked: dialog.controller.copyIntoLibrary
            onToggled: dialog.controller.copyIntoLibrary = checked
        }

        RowLayout {
            enabled: dialog.controller.copyIntoLibrary
            opacity: enabled ? 1 : 0.55
            spacing: 8
            Layout.fillWidth: true
            Layout.leftMargin: 24

            Text {
                objectName: "importCopyDestination"
                text: dialog.hasDestination ? dialog.controller.displayPath(dialog.controller.copyDestination)
                                            : qsTr("No library folder chosen")
                color: dialog.hasDestination ? Theme.textPrimary : Theme.textTertiary
                font.pixelSize: Theme.fontBody
                elide: Text.ElideMiddle
                Layout.fillWidth: true
            }

            StyledButton {
                objectName: "importChooseDestination"
                text: qsTr("Choose…")
                onClicked: destinationPicker.open()
            }
        }

        Text {
            objectName: "importExplanation"
            text: dialog.controller.copyIntoLibrary
                  ? qsTr("Each photo is copied into the library folder, below a folder named after its source, and checked against the original before the copy is added. The originals are only read.")
                  : qsTr("Photos are added where they are. Lucent only reads them; a photo on a memory card stays marked as removable.")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.topMargin: 4
        }
    }

    footer: Item {
        implicitHeight: buttons.implicitHeight + 36

        RowLayout {
            id: buttons

            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            StyledButton {
                objectName: "importCancel"
                text: qsTr("Cancel")
                onClicked: dialog.reject()
            }

            StyledButton {
                id: addButton

                objectName: "importConfirm"
                text: dialog.controller.copyIntoLibrary ? qsTr("Copy and add") : qsTr("Add")
                highlighted: true
                enabled: !dialog.controller.copyIntoLibrary || dialog.hasDestination
                onClicked: {
                    if (dialog.controller.confirmImport()) {
                        dialog.controller.showView(AppController.Library)
                        dialog.close()
                    }
                }
            }
        }
    }

    FolderDialog {
        id: destinationPicker

        title: qsTr("Choose the library folder for the copies")
        currentFolder: dialog.controller.copyDestination
        onAccepted: dialog.controller.copyDestination = selectedFolder
    }
}
