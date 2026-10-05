import QtQuick
import QtQuick.Layouts
import Lucent

// Detail: chroma (colour) noise reduction (blueprint section 2: "Noise reduction starts with the
// guided chroma-noise method specified in section 5"; ADR 0015). Amount 0 is off (the default);
// the radius is in pixels of the full-resolution photo. Luminance detail is kept: the method only
// smooths colour.
PanelSection {
    id: panel

    required property EditSession edit

    title: qsTr("Detail")

    Text {
        text: qsTr("Colour noise reduction")
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        font.weight: Font.Medium
        Layout.leftMargin: 26
    }

    AdjustmentSlider {
        objectName: "adjust_noiseReduction"
        edit: panel.edit
        key: "noiseReduction"
        showLock: false
        Layout.fillWidth: true
    }

    AdjustmentSlider {
        objectName: "adjust_noiseRadius"
        edit: panel.edit
        key: "noiseRadius"
        showLock: false
        Layout.fillWidth: true
    }

    Text {
        text: qsTr("Smooths colour speckles while keeping brightness detail. Check fine coloured detail at 100 %.")
        color: Theme.textTertiary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.leftMargin: 26
    }
}
