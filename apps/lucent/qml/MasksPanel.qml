pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Masks (blueprint section 2: "brush, linear-gradient, and radial masks with feathering for detailed
// local work"; MaskEditor, mask_editor.hpp): add a brush (B), linear gradient (G) or radial gradient
// (Shift+R) mask; the list of masks in the order they apply, each with enable, invert, move,
// duplicate and delete; the selected mask's opacity, feather and local controls (exposure, contrast,
// highlights, shadows, saturation, temperature, tint); the brush's size, feather and flow and
// Erase; and "Show mask" (O), which tints the selected mask's weights over the photo.
PanelSection {
    id: panel

    required property EditSession edit

    readonly property MaskEditor masks: edit.masks
    readonly property bool editable: edit.canEdit
    readonly property bool hasSelection: masks.selectedId > 0

    title: qsTr("Masks")

    actions: [
        StyledToolButton {
            objectName: "masksToolButton"
            text: panel.edit.tool === "masks" ? qsTr("Done") : qsTr("Edit")
            checkable: true
            checked: panel.edit.tool === "masks"
            enabled: panel.edit.tool === "masks" || (panel.editable && panel.masks.count > 0)
            onClicked: panel.edit.tool === "masks" ? panel.edit.finishTool() : panel.edit.setTool("masks")
            ToolTip.visible: hovered
            ToolTip.text: panel.edit.tool === "masks" ? qsTr("Close the masks tool (Enter)")
                                                      : qsTr("Show the masks' handles on the photo")
            ToolTip.delay: 800
        }
    ]

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 22
        spacing: 2

        StyledToolButton {
            objectName: "addBrushMask"
            text: qsTr("+ Brush")
            enabled: panel.masks.canAdd
            onClicked: panel.masks.add("brush")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Paint a mask (B)")
            ToolTip.delay: 800
        }

        StyledToolButton {
            objectName: "addLinearMask"
            text: qsTr("+ Linear")
            enabled: panel.masks.canAdd
            onClicked: panel.masks.add("linear")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("A linear gradient mask (G)")
            ToolTip.delay: 800
        }

        StyledToolButton {
            objectName: "addRadialMask"
            text: qsTr("+ Radial")
            enabled: panel.masks.canAdd
            onClicked: panel.masks.add("radial")
            ToolTip.visible: hovered
            ToolTip.text: qsTr("A radial gradient mask (Shift+R)")
            ToolTip.delay: 800
        }

        Item {
            Layout.fillWidth: true
        }
    }

    Text {
        visible: panel.masks.count === 0
        text: qsTr("No masks. A mask applies its own exposure, contrast and colour to part of the photo.")
        color: Theme.textTertiary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.leftMargin: 26
    }

    // ---- The masks, in the order they apply ----------------------------------------------------------

    Repeater {
        model: panel.masks.list

        delegate: Rectangle {
            id: row

            required property int index
            required property int maskId
            required property string name
            required property string kindLabel
            required property bool maskEnabled
            required property bool inverted
            required property real maskOpacity
            required property bool selected

            objectName: "maskRow_" + index
            Layout.fillWidth: true
            Layout.leftMargin: 22
            implicitHeight: 28
            radius: Theme.radius
            color: selected ? Theme.accentMuted : rowHover.hovered ? Theme.surfaceHover : "transparent"
            border.width: selected ? 1 : 0
            border.color: Theme.accent

            Accessible.role: Accessible.ListItem
            Accessible.name: qsTr("%1, %2%3").arg(name).arg(kindLabel).arg(maskEnabled ? "" : qsTr(", off"))
            Accessible.selected: selected

            HoverHandler {
                id: rowHover
            }

            TapHandler {
                onTapped: panel.masks.selectedId = row.maskId
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 2
                spacing: 0

                StyledCheckBox {
                    objectName: "maskEnabled_" + row.index
                    checked: row.maskEnabled
                    enabled: panel.editable
                    onToggled: panel.masks.setEnabled(row.maskId, checked)
                    Accessible.name: qsTr("Apply %1").arg(row.name)
                    Layout.preferredWidth: 24
                }

                Text {
                    text: row.name
                    color: row.maskEnabled ? Theme.textPrimary : Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                    font.weight: row.selected ? Font.DemiBold : Font.Normal
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }

                Text {
                    objectName: "maskOpacity_" + row.index
                    visible: row.maskOpacity < 100
                    text: qsTr("%1 %").arg(row.maskOpacity)
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                    Layout.rightMargin: 4
                    Accessible.name: qsTr("Opacity %1 %").arg(row.maskOpacity)
                }

                StyledToolButton {
                    objectName: "maskInvert_" + row.index
                    text: qsTr("Inv")
                    implicitHeight: 22
                    leftPadding: 4
                    rightPadding: 4
                    checkable: true
                    checked: row.inverted
                    enabled: panel.editable
                    onClicked: panel.masks.setInvert(row.maskId, checked)
                    Accessible.name: qsTr("Invert %1").arg(row.name)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Invert: apply outside the mask")
                    ToolTip.delay: 800
                }

                StyledToolButton {
                    text: "↑"
                    implicitWidth: 22
                    implicitHeight: 22
                    leftPadding: 2
                    rightPadding: 2
                    enabled: panel.editable && row.index > 0
                    onClicked: panel.masks.move(row.maskId, -1)
                    Accessible.name: qsTr("Move %1 up").arg(row.name)
                }

                StyledToolButton {
                    text: "↓"
                    implicitWidth: 22
                    implicitHeight: 22
                    leftPadding: 2
                    rightPadding: 2
                    enabled: panel.editable && row.index + 1 < panel.masks.count
                    onClicked: panel.masks.move(row.maskId, 1)
                    Accessible.name: qsTr("Move %1 down").arg(row.name)
                }

                StyledToolButton {
                    objectName: "maskDuplicate_" + row.index
                    text: "⧉"
                    implicitWidth: 22
                    implicitHeight: 22
                    leftPadding: 2
                    rightPadding: 2
                    enabled: panel.masks.canAdd
                    onClicked: panel.masks.duplicate(row.maskId)
                    Accessible.name: qsTr("Duplicate %1").arg(row.name)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Duplicate")
                    ToolTip.delay: 800
                }

                StyledToolButton {
                    objectName: "maskDelete_" + row.index
                    text: "×"
                    implicitWidth: 22
                    implicitHeight: 22
                    leftPadding: 2
                    rightPadding: 2
                    enabled: panel.editable
                    onClicked: panel.masks.remove(row.maskId)
                    Accessible.name: qsTr("Delete %1").arg(row.name)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Delete")
                    ToolTip.delay: 800
                }
            }
        }
    }

    // ---- The selected mask -----------------------------------------------------------------------------

    StyledCheckBox {
        objectName: "maskOverlayToggle"
        visible: panel.hasSelection
        text: qsTr("Show mask (O)")
        checked: panel.masks.overlay
        onToggled: panel.masks.overlay = checked
        Layout.leftMargin: 22
    }

    Repeater {
        model: panel.hasSelection ? ["maskOpacity"].concat(panel.masks.selectedKind === "brush" ? [] : ["maskFeather"])
                                  : []

        delegate: AdjustmentSlider {
            required property string modelData

            edit: panel.edit
            key: modelData
            showLock: false
            Layout.fillWidth: true
        }
    }

    Rectangle {
        visible: panel.hasSelection
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: Theme.divider
    }

    Repeater {
        model: panel.hasSelection ? ["maskExposure", "maskContrast", "maskHighlights", "maskShadows", "maskSaturation",
                                     "maskTemperature", "maskTint"]
                                  : []

        delegate: AdjustmentSlider {
            required property string modelData

            edit: panel.edit
            key: modelData
            showLock: false
            lowColor: modelData === "maskTemperature" ? "#3d6fb3" : modelData === "maskTint" ? "#4f9a4f" : "transparent"
            highColor: modelData === "maskTemperature" ? "#d9a441" : modelData === "maskTint" ? "#b25ab0" : "transparent"
            Layout.fillWidth: true
        }
    }

    // ---- The brush ----------------------------------------------------------------------------------------

    ColumnLayout {
        visible: panel.masks.selectedKind === "brush"
        Layout.fillWidth: true
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 26
            spacing: 6

            Text {
                text: qsTr("Brush")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                font.weight: Font.Medium
            }

            Text {
                text: qsTr("%1 px").arg(panel.masks.brushPixels)
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
            }

            Item {
                Layout.fillWidth: true
            }

            StyledToolButton {
                objectName: "brushErase"
                text: qsTr("Erase")
                implicitHeight: 24
                checkable: true
                checked: panel.masks.erase
                onClicked: panel.masks.erase = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Paint to remove the mask (or hold Alt while painting)")
                ToolTip.delay: 800
            }
        }

        Repeater {
            model: ["brushSize", "brushFeather", "brushFlow"]

            delegate: AdjustmentSlider {
                required property string modelData

                edit: panel.edit
                key: modelData
                showLock: false
                Layout.fillWidth: true
            }
        }
    }
}
