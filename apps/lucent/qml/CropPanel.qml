pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Crop and rotate (blueprint section 2, "crop, rotate, straighten"; CropTool, crop_tool.hpp): the
// crop tool (R; the image is shown without its crop under a frame to drag, Enter applies, Esc cancels),
// aspect-ratio presets with an orientation swap, the straighten slider and levelling by drawing a
// horizon, quarter turns and the mirror, "Constrain to image" and reset. Every change is one revision.
PanelSection {
    id: panel

    required property EditSession edit
    // The crop overlay draws a horizon instead of moving the crop (EditView owns the flag).
    property bool levelMode: false

    signal levelModeRequested(bool on)

    readonly property CropTool crop: edit.crop
    readonly property bool open: edit.tool === "crop"
    readonly property bool editable: edit.canEdit && crop.available

    title: qsTr("Crop and rotate")

    actions: [
        StyledToolButton {
            objectName: "cropToolButton"
            text: panel.open ? qsTr("Done") : qsTr("Crop")
            checkable: true
            checked: panel.open
            enabled: panel.open || panel.editable
            onClicked: panel.open ? panel.edit.finishTool() : panel.edit.setTool("crop")
            ToolTip.visible: hovered
            ToolTip.text: panel.open ? qsTr("Apply the crop and close the tool (Enter)")
                                     : qsTr("Crop and straighten the photo (R)")
            ToolTip.delay: 800
        }
    ]

    // ---- Aspect ratio ---------------------------------------------------------------------------------

    Text {
        text: qsTr("Aspect ratio")
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        font.weight: Font.Medium
        Layout.leftMargin: 26
    }

    Flow {
        Layout.fillWidth: true
        Layout.leftMargin: 26
        spacing: 4

        Repeater {
            model: panel.crop.aspectOptions()

            delegate: StyledToolButton {
                required property var modelData

                objectName: "cropAspect_" + modelData.key
                text: modelData.label
                implicitHeight: 26
                leftPadding: 6
                rightPadding: 6
                checkable: true
                checked: panel.crop.aspect === modelData.key
                enabled: panel.editable
                onClicked: panel.crop.setAspect(modelData.key)
                Accessible.name: qsTr("Aspect ratio %1").arg(modelData.label)
            }
        }

        StyledToolButton {
            objectName: "cropOrientation"
            text: panel.crop.portrait ? qsTr("Portrait") : qsTr("Landscape")
            implicitHeight: 26
            enabled: panel.editable && panel.crop.aspect !== "free"
            onClicked: panel.crop.swapOrientation()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Swap the aspect ratio between landscape and portrait")
            ToolTip.delay: 800
        }
    }

    RowLayout {
        visible: panel.crop.aspect === "custom"
        Layout.leftMargin: 26
        spacing: 4

        TextField {
            id: customWidth

            objectName: "cropCustomWidth"
            text: String(panel.crop.customWidth)
            enabled: panel.editable
            horizontalAlignment: TextInput.AlignRight
            font.pixelSize: Theme.fontSmall
            color: Theme.textPrimary
            Layout.preferredWidth: 48
            onAccepted: panel.crop.setCustomAspect(Number(customWidth.text), Number(customHeight.text))
            Accessible.name: qsTr("Custom aspect ratio width")
        }

        Text {
            text: ":"
            color: Theme.textSecondary
        }

        TextField {
            id: customHeight

            objectName: "cropCustomHeight"
            text: String(panel.crop.customHeight)
            enabled: panel.editable
            font.pixelSize: Theme.fontSmall
            color: Theme.textPrimary
            Layout.preferredWidth: 48
            onAccepted: panel.crop.setCustomAspect(Number(customWidth.text), Number(customHeight.text))
            Accessible.name: qsTr("Custom aspect ratio height")
        }

        StyledToolButton {
            text: qsTr("Set")
            implicitHeight: 26
            enabled: panel.editable
            onClicked: panel.crop.setCustomAspect(Number(customWidth.text), Number(customHeight.text))
        }
    }

    // ---- Straighten, turns, mirror --------------------------------------------------------------------

    AdjustmentSlider {
        edit: panel.edit
        key: "straighten"
        showLock: false
        Layout.fillWidth: true
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 22
        spacing: 2

        StyledToolButton {
            objectName: "cropLevel"
            text: qsTr("Level")
            checkable: true
            checked: panel.levelMode
            enabled: panel.open && panel.editable
            onClicked: panel.levelModeRequested(!panel.levelMode)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Draw along the horizon to straighten the photo (or Ctrl+drag on it)")
            ToolTip.delay: 800
        }

        StyledToolButton {
            objectName: "cropRotateLeft"
            text: "⟲"
            enabled: panel.editable
            onClicked: panel.crop.rotateLeft()
            Accessible.name: qsTr("Rotate left")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Rotate 90° counter-clockwise")
            ToolTip.delay: 800
        }

        StyledToolButton {
            objectName: "cropRotateRight"
            text: "⟳"
            enabled: panel.editable
            onClicked: panel.crop.rotateRight()
            Accessible.name: qsTr("Rotate right")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Rotate 90° clockwise")
            ToolTip.delay: 800
        }

        StyledToolButton {
            objectName: "cropFlip"
            text: "⇋"
            enabled: panel.editable
            onClicked: panel.crop.flip()
            Accessible.name: qsTr("Flip horizontally")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Mirror the photo left to right")
            ToolTip.delay: 800
        }

        Item {
            Layout.fillWidth: true
        }

        StyledToolButton {
            objectName: "cropReset"
            text: qsTr("Reset")
            enabled: panel.editable && panel.crop.modified
            onClicked: panel.crop.reset()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Remove the crop, straightening, turns and mirror")
            ToolTip.delay: 800
        }
    }

    StyledCheckBox {
        objectName: "cropConstrain"
        text: qsTr("Constrain to image")
        checked: panel.crop.constrain
        enabled: panel.editable
        onToggled: panel.crop.constrain = checked
        Layout.leftMargin: 22
        ToolTip.visible: hovered
        ToolTip.text: qsTr("Keep the crop inside the straightened photo, so no empty corners show")
        ToolTip.delay: 800
    }

    Text {
        objectName: "cropSize"
        visible: text.length > 0
        text: panel.crop.sizeText
        color: Theme.textTertiary
        font.pixelSize: Theme.fontSmall
        Layout.leftMargin: 26
    }
}
