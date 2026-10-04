pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Copy settings by category (blueprint section 2: "Copy/paste adjustments offers specific
// categories"). Paste applies the copied categories to the photo shown; camera-specific white
// balance is never pasted to another camera model (the result message says what was not pasted).
Dialog {
    id: dialog

    required property EditSession edit

    objectName: "copySettingsDialog"
    title: qsTr("Copy settings")
    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(420, Overlay.overlay ? Overlay.overlay.width - 64 : 420)
    padding: 20
    topPadding: 12

    // Keys of the checked categories.
    readonly property var checkedKeys: {
        const keys = []
        for (let i = 0; i < boxes.count; ++i) {
            const box = boxes.itemAt(i)
            if (box && box.checked)
                keys.push(box.key)
        }
        return keys
    }

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

    onAccepted: edit.copySettings(checkedKeys)

    contentItem: ColumnLayout {
        spacing: 6

        Text {
            text: qsTr("Choose what to copy from this photo:")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontBody
            Layout.fillWidth: true
            Layout.bottomMargin: 4
        }

        Repeater {
            id: boxes

            model: dialog.edit.categories()

            delegate: StyledCheckBox {
                required property var modelData

                readonly property string key: modelData.key

                objectName: "copyCategory_" + modelData.key
                text: modelData.label
                checked: true
            }
        }

        Text {
            text: qsTr("A white balance specific to this camera is only pasted to photos from the same camera model.")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
            Layout.topMargin: 6
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
                objectName: "copyCancel"
                text: qsTr("Cancel")
                onClicked: dialog.reject()
            }

            StyledButton {
                objectName: "copyConfirm"
                text: qsTr("Copy")
                highlighted: true
                enabled: dialog.checkedKeys.length > 0
                onClicked: dialog.accept()
            }
        }
    }
}
