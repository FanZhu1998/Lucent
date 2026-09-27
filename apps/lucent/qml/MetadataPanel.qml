import QtQuick
import QtQuick.Layouts
import Lucent

// Details of the photo shown in the Edit view: file, camera, capture and exposure facts, and its
// compatibility state with the reason when there is one (blueprint section 3).
ColumnLayout {
    id: panel

    required property CurrentItem item

    readonly property var rows: [
        { label: qsTr("File"), value: item.fileName, path: true },
        { label: qsTr("Folder"), value: item.folder, path: true },
        { label: qsTr("Camera"), value: item.valid ? item.cameraLabel : "" },
        { label: qsTr("Lens"), value: item.lensLabel },
        { label: qsTr("Captured"), value: item.valid ? item.captureTimeLabel : "" },
        { label: qsTr("Exposure"), value: item.exposureLabel },
        { label: qsTr("Dimensions"), value: item.dimensionsLabel },
        { label: qsTr("File size"), value: item.fileSizeLabel },
        { label: qsTr("Status"), value: item.valid ? item.processingLabel : "" },
        { label: qsTr("Rendering"), value: item.renderLabel },
        { label: qsTr("Flags"), value: item.flagLabels.join(", ") }
    ].filter(row => row.value.length > 0)

    spacing: 10

    Text {
        text: qsTr("Details")
        color: Theme.textPrimary
        font.pixelSize: Theme.fontTitle
        font.weight: Font.DemiBold
        Accessible.role: Accessible.Heading
        Accessible.name: text
    }

    RowLayout {
        visible: panel.item.valid
        spacing: 8
        Layout.fillWidth: true

        Badge {
            text: panel.item.supportLabel
            dotColor: Theme.supportColor(panel.item.supportKey)
            detail: panel.item.supportDetail
            color: Theme.surfaceRaised
        }

        Item {
            Layout.fillWidth: true
        }
    }

    Text {
        visible: panel.item.valid && panel.item.supportDetail.length > 0
        text: panel.item.supportDetail
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Repeater {
        model: panel.rows

        delegate: RowLayout {
            id: detailRow

            required property var modelData

            spacing: 10
            Layout.fillWidth: true

            Accessible.role: Accessible.StaticText
            Accessible.name: modelData.label + ": " + modelData.value

            Text {
                text: detailRow.modelData.label
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                Layout.preferredWidth: 72
                Layout.alignment: Qt.AlignTop
            }

            Text {
                text: detailRow.modelData.value
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSmall
                // Paths have no spaces: break them anywhere; other values between words.
                wrapMode: detailRow.modelData.path ? Text.WrapAnywhere : Text.WrapAtWordBoundaryOrAnywhere
                Layout.fillWidth: true
            }
        }
    }

    Text {
        objectName: "processingDetail"
        visible: panel.item.valid && panel.item.processingDetail.length > 0
        text: panel.item.processingDetail
        color: Theme.unsupported
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    Repeater {
        model: panel.item.renderNotes

        delegate: Text {
            required property string modelData

            text: "• " + modelData
            color: Theme.attention
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }

    Text {
        visible: !panel.item.valid
        text: qsTr("No photo selected")
        color: Theme.textTertiary
        font.pixelSize: Theme.fontSmall
    }
}
