import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// The Edit view's adjustments (blueprint section 2, "Detailed individual editing"): white balance
// (temperature and tint where the camera's calibration defines a colour temperature, otherwise the
// camera's own channel balance, with an explanation), exposure, contrast, highlights, shadows,
// saturation and vibrance. The header holds Auto (W8: a new revision of settings Lucent chooses,
// keeping locked controls; the Automatic settings panel shows its progress and outcome), Copy and
// Paste of settings by category.
PanelSection {
    id: panel

    required property EditSession edit

    signal copyRequested()

    title: qsTr("Adjust")

    actions: [
        Item {
            implicitWidth: autoButton.implicitWidth
            implicitHeight: autoButton.implicitHeight

            StyledToolButton {
                id: autoButton

                objectName: "autoButton"
                text: qsTr("Auto")
                enabled: panel.edit.canRunAuto
                onClicked: panel.edit.runAuto()
                Accessible.description: autoHover.tip
            }

            // A disabled button gets no hover events: the tooltip hangs on its frame instead.
            HoverHandler {
                id: autoHover

                readonly property string tip: panel.edit.automatic.running
                    ? qsTr("Auto is choosing this photo's settings")
                    : panel.edit.readOnly ? panel.edit.readOnlyReason
                    : qsTr("Let Lucent choose this photo's settings as a new revision; locked controls stay as they are (Ctrl+Shift+A)")
            }

            ToolTip.visible: autoHover.hovered
            ToolTip.text: autoHover.tip
            ToolTip.delay: 500
        },
        StyledToolButton {
            objectName: "copySettingsButton"
            text: qsTr("Copy…")
            enabled: panel.edit.ready && !panel.edit.readOnly
            onClicked: panel.copyRequested()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Copy settings by category (Ctrl+Shift+C)")
            ToolTip.delay: 800
        },
        StyledToolButton {
            objectName: "pasteSettingsButton"
            text: qsTr("Paste")
            enabled: panel.edit.hasClipboard && panel.edit.canEdit
            onClicked: panel.edit.pasteSettings()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Paste the copied settings into this photo (Ctrl+Shift+V)")
            ToolTip.delay: 800
        }
    ]

    // ---- White balance ---------------------------------------------------------------------------

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        LockButton {
            objectName: "lock_whiteBalance"
            controlName: qsTr("White balance")
            enabled: panel.edit.canEdit
            checked: { panel.edit.settingsRevision; return panel.edit.isLocked("whiteBalance") }
            onClicked: panel.edit.setLocked("whiteBalance", checked)
        }

        Text {
            text: qsTr("White balance")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            font.weight: Font.Medium
        }

        Item {
            Layout.fillWidth: true
        }

        Text {
            objectName: "whiteBalanceMode"
            text: panel.edit.whiteBalanceMode === "asShot" ? qsTr("As shot")
                : panel.edit.whiteBalanceMode === "kelvin" ? qsTr("Custom")
                : panel.edit.whiteBalanceMode === "unknown" ? qsTr("Unknown")
                : qsTr("Custom (camera channels)")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
        }

        StyledToolButton {
            objectName: "whiteBalanceAsShot"
            text: qsTr("Use as shot")
            implicitHeight: 24
            visible: panel.edit.whiteBalanceMode !== "asShot" && panel.edit.settingsKnown
            enabled: panel.edit.canEdit
            onClicked: panel.edit.resetWhiteBalance()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Use the camera's white balance")
            ToolTip.delay: 800
        }
    }

    AdjustmentSlider {
        edit: panel.edit
        key: "temperature"
        showLock: false
        lowColor: "#3d6fb3"
        highColor: "#d9a441"
        visible: panel.edit.whiteBalanceControls === "kelvin"
        Layout.fillWidth: true
    }

    AdjustmentSlider {
        edit: panel.edit
        key: "tint"
        showLock: false
        lowColor: "#4f9a4f"
        highColor: "#b25ab0"
        visible: panel.edit.whiteBalanceControls === "kelvin"
        Layout.fillWidth: true
    }

    AdjustmentSlider {
        edit: panel.edit
        key: "redBlue"
        showLock: false
        lowColor: "#3d6fb3"
        highColor: "#d9a441"
        visible: panel.edit.whiteBalanceControls === "neutral"
        Layout.fillWidth: true
    }

    AdjustmentSlider {
        edit: panel.edit
        key: "greenMagenta"
        showLock: false
        lowColor: "#4f9a4f"
        highColor: "#b25ab0"
        visible: panel.edit.whiteBalanceControls === "neutral"
        Layout.fillWidth: true
    }

    Text {
        objectName: "whiteBalanceNote"
        visible: text.length > 0
        text: panel.edit.whiteBalanceNote
        color: Theme.textTertiary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.leftMargin: 26
        Layout.bottomMargin: 4
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        Layout.topMargin: 2
        Layout.bottomMargin: 2
        color: Theme.divider
    }

    // ---- Tone and colour ---------------------------------------------------------------------------

    Repeater {
        model: ["exposure", "contrast", "highlights", "shadows", "saturation", "vibrance"]

        delegate: AdjustmentSlider {
            required property string modelData

            edit: panel.edit
            key: modelData
            Layout.fillWidth: true
        }
    }
}
