import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Lucent

// Export of the photo shown in Edit (blueprint section 2 step 3 and section 7): file format,
// colour space, JPEG quality and the output folder, with the file name the export will get.
// Existing files are never overwritten: a number is added to the name instead. The options are
// the controller's export options, kept for the session.
Dialog {
    id: dialog

    required property AppController controller
    property url folder: controller.lastOutputFolder

    objectName: "exportDialog"
    title: qsTr("Export photo")
    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(520, Overlay.overlay ? Overlay.overlay.width - 64 : 520)
    padding: 20
    topPadding: 12

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

    onOpened: {
        folder = controller.lastOutputFolder
        jpegButton.forceActiveFocus(Qt.OtherFocusReason)
    }
    onAccepted: controller.exportCurrent(folder)

    contentItem: ColumnLayout {
        spacing: 12

        Text {
            text: dialog.controller.current.fileName
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            elide: Text.ElideMiddle
            Layout.fillWidth: true
        }

        GridLayout {
            columns: 2
            columnSpacing: 16
            rowSpacing: 10
            Layout.fillWidth: true

            Text {
                text: qsTr("Format")
                color: Theme.textTertiary
                font.pixelSize: Theme.fontBody
            }

            RowLayout {
                spacing: 4

                ButtonGroup {
                    id: formatGroup
                }

                StyledToolButton {
                    id: jpegButton

                    objectName: "exportFormatJpeg"
                    text: qsTr("JPEG")
                    checkable: true
                    checked: dialog.controller.exportFormat === AppController.Jpeg
                    ButtonGroup.group: formatGroup
                    onClicked: dialog.controller.exportFormat = AppController.Jpeg
                    Accessible.name: qsTr("JPEG, 8-bit")
                }

                StyledToolButton {
                    objectName: "exportFormatTiff"
                    text: qsTr("TIFF 16-bit")
                    checkable: true
                    checked: dialog.controller.exportFormat === AppController.Tiff16
                    ButtonGroup.group: formatGroup
                    onClicked: dialog.controller.exportFormat = AppController.Tiff16
                }
            }

            Text {
                text: qsTr("Colour space")
                color: Theme.textTertiary
                font.pixelSize: Theme.fontBody
            }

            RowLayout {
                spacing: 4

                ButtonGroup {
                    id: colorGroup
                }

                StyledToolButton {
                    objectName: "exportColorSrgb"
                    text: qsTr("sRGB")
                    checkable: true
                    checked: dialog.controller.exportColorSpace === AppController.Srgb
                    ButtonGroup.group: colorGroup
                    onClicked: dialog.controller.exportColorSpace = AppController.Srgb
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("For sharing and the web")
                    ToolTip.delay: 800
                }

                StyledToolButton {
                    objectName: "exportColorP3"
                    text: qsTr("Display P3")
                    checkable: true
                    checked: dialog.controller.exportColorSpace === AppController.DisplayP3
                    ButtonGroup.group: colorGroup
                    onClicked: dialog.controller.exportColorSpace = AppController.DisplayP3
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("A wider gamut for wide-gamut displays and further editing")
                    ToolTip.delay: 800
                }
            }

            Text {
                visible: dialog.controller.exportFormat === AppController.Jpeg
                text: qsTr("Quality")
                color: Theme.textTertiary
                font.pixelSize: Theme.fontBody
            }

            RowLayout {
                visible: dialog.controller.exportFormat === AppController.Jpeg
                spacing: 10

                Slider {
                    id: qualitySlider

                    objectName: "exportQuality"
                    from: 1
                    to: 100
                    stepSize: 1
                    snapMode: Slider.SnapAlways
                    value: dialog.controller.exportQuality
                    onMoved: dialog.controller.exportQuality = Math.round(value)
                    Layout.preferredWidth: 220
                    Accessible.name: qsTr("JPEG quality")
                }

                Text {
                    text: String(dialog.controller.exportQuality)
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    Layout.preferredWidth: 28
                }
            }

            Text {
                text: qsTr("Folder")
                color: Theme.textTertiary
                font.pixelSize: Theme.fontBody
            }

            RowLayout {
                spacing: 8
                Layout.fillWidth: true

                Text {
                    objectName: "exportFolderPath"
                    text: dialog.folder.toString().length > 0 ? dialog.controller.displayPath(dialog.folder)
                                                              : qsTr("No folder chosen")
                    color: dialog.folder.toString().length > 0 ? Theme.textPrimary : Theme.textTertiary
                    font.pixelSize: Theme.fontBody
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }

                StyledButton {
                    objectName: "exportChooseFolder"
                    text: qsTr("Choose…")
                    onClicked: folderPicker.open()
                }
            }
        }

        Text {
            objectName: "exportNamePreview"
            visible: dialog.controller.suggestedExportName.length > 0
            text: qsTr("Saved as %1. If that name is taken a number is added; existing files are never overwritten.")
                .arg(dialog.controller.suggestedExportName)
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
                objectName: "exportCancel"
                text: qsTr("Cancel")
                onClicked: dialog.reject()
            }

            StyledButton {
                objectName: "exportConfirm"
                text: qsTr("Export")
                highlighted: true
                enabled: dialog.folder.toString().length > 0 && dialog.controller.current.valid
                onClicked: dialog.accept()
            }
        }
    }

    FolderDialog {
        id: folderPicker

        title: qsTr("Choose the output folder")
        currentFolder: dialog.folder
        onAccepted: dialog.folder = selectedFolder
    }
}
