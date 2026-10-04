import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// One photographic control of the Edit view: lock, label, slider, numeric field and reset
// (blueprint section 2). Values and their ranges come from EditSession (control keys,
// edit_session.hpp).
//
// Gestures: a drag is ONE revision (pressing begins it, releasing commits it); arrow keys move by a
// step and Shift+arrow or Page Up/Down by a coarse step, and holding a key down is one revision too
// (EditSession.nudge, committed on release). Double-click the slider, press Home, or use the reset
// button to return to the default; type a value and press Enter to set it exactly.
//
// Accessibility: the slider runs in the control's display units (exposure in EV, the others -100 to
// +100), so assistive technology reads the value the field shows; temperature, whose slider is
// linear in reciprocal kelvin, reads its position and states the kelvin value in its description.
Item {
    id: row

    required property EditSession edit
    required property string key
    property bool showLock: true
    // Optional track colours, low to high (white balance).
    property color lowColor: "transparent"
    property color highColor: "transparent"

    readonly property var spec: edit.control(key)
    readonly property string label: spec.label !== undefined ? spec.label : key
    readonly property bool editable: edit.canEdit
    readonly property bool linear: spec.reciprocal !== true

    objectName: "adjust_" + key
    implicitHeight: 30
    implicitWidth: 300

    RowLayout {
        anchors.fill: parent
        spacing: 4

        LockButton {
            objectName: "lock_" + row.key
            visible: row.showLock
            controlName: row.label
            enabled: row.editable
            checked: { row.edit.settingsRevision; return row.edit.isLocked(row.spec.lockKey) }
            onClicked: row.edit.setLocked(row.spec.lockKey, checked)
        }

        Item {
            visible: !row.showLock
            implicitWidth: 22
        }

        Text {
            text: row.label
            color: row.editable ? Theme.textSecondary : Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            elide: Text.ElideRight
            Layout.preferredWidth: 80
            Accessible.ignored: true
        }

        Slider {
            id: slider

            objectName: "slider_" + row.key
            from: row.linear ? row.spec.minimum : 0
            to: row.linear ? row.spec.maximum : 1
            live: true
            enabled: row.editable
            focusPolicy: Qt.StrongFocus
            value: {
                row.edit.settingsRevision
                return row.linear ? row.edit.value(row.key) : row.edit.position(row.key)
            }
            Layout.fillWidth: true
            Layout.preferredHeight: 24

            onPressedChanged: pressed ? row.edit.beginGesture() : row.edit.endGesture()
            onMoved: row.linear ? row.edit.setValue(row.key, value) : row.edit.setPosition(row.key, value)

            Accessible.name: row.label
            Accessible.description: {
                row.edit.settingsRevision
                const unit = row.spec.unit !== undefined && row.spec.unit.length > 0 ? " " + row.spec.unit : ""
                return row.edit.valueText(row.key) + unit
            }

            Keys.onPressed: event => {
                let steps = 0
                let coarse = (event.modifiers & Qt.ShiftModifier) !== 0
                switch (event.key) {
                case Qt.Key_Left:
                case Qt.Key_Down:
                    steps = -1
                    break
                case Qt.Key_Right:
                case Qt.Key_Up:
                    steps = 1
                    break
                case Qt.Key_PageDown:
                    steps = -1
                    coarse = true
                    break
                case Qt.Key_PageUp:
                    steps = 1
                    coarse = true
                    break
                case Qt.Key_Home:
                    if (!event.isAutoRepeat)
                        row.edit.resetValue(row.key)
                    event.accepted = true
                    return
                default:
                    return
                }
                row.edit.nudge(row.key, steps, coarse)
                event.accepted = true
            }
            Keys.onReleased: event => {
                if (event.isAutoRepeat)
                    return
                switch (event.key) {
                case Qt.Key_Left:
                case Qt.Key_Right:
                case Qt.Key_Up:
                case Qt.Key_Down:
                case Qt.Key_PageUp:
                case Qt.Key_PageDown:
                    row.edit.endNudge()
                    event.accepted = true
                    break
                }
            }

            background: Rectangle {
                x: slider.leftPadding
                y: slider.topPadding + slider.availableHeight / 2 - height / 2
                implicitWidth: 120
                implicitHeight: 4
                width: slider.availableWidth
                height: 4
                radius: 2
                color: Theme.control
                gradient: row.lowColor.a > 0 ? trackGradient : null

                Gradient {
                    id: trackGradient

                    orientation: Gradient.Horizontal
                    GradientStop {
                        position: 0
                        color: row.lowColor
                    }
                    GradientStop {
                        position: 1
                        color: row.highColor
                    }
                }

                // The default (zero) of signed controls.
                Rectangle {
                    visible: row.spec.minimum < 0 && row.lowColor.a === 0
                    x: Math.round(parent.width / 2) - 1
                    y: -2
                    width: 1
                    height: 8
                    color: Theme.indicatorBorder
                }
            }

            handle: Rectangle {
                visible: row.edit.settingsKnown  // no position for values that cannot be read
                x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
                y: slider.topPadding + slider.availableHeight / 2 - height / 2
                implicitWidth: 12
                implicitHeight: 12
                radius: 6
                color: !slider.enabled ? Theme.control : slider.pressed ? Theme.focusRing : Theme.textPrimary
                border.width: slider.visualFocus ? 2 : 0
                border.color: Theme.focusRing
            }

            TapHandler {
                onDoubleTapped: row.edit.resetValue(row.key)
            }
        }

        TextField {
            id: field

            objectName: "field_" + row.key
            enabled: row.editable
            text: { row.edit.settingsRevision; return row.edit.valueText(row.key) }
            horizontalAlignment: TextInput.AlignRight
            selectByMouse: true
            font.pixelSize: Theme.fontSmall
            color: Theme.textPrimary
            leftPadding: 4
            rightPadding: 4
            topPadding: 2
            bottomPadding: 2
            Layout.preferredWidth: 50
            Layout.preferredHeight: 24
            onAccepted: {
                row.edit.setValueText(row.key, text)
                slider.forceActiveFocus(Qt.OtherFocusReason)
            }
            onActiveFocusChanged: {
                if (!activeFocus && text !== row.edit.valueText(row.key))
                    row.edit.setValueText(row.key, text)
            }

            Accessible.name: qsTr("%1 value").arg(row.label)

            background: Rectangle {
                radius: Theme.radius
                color: field.activeFocus ? Theme.surfaceRaised : "transparent"
                border.width: field.activeFocus ? 1 : 0
                border.color: Theme.focusRing
            }
        }

        StyledToolButton {
            objectName: "reset_" + row.key
            text: "↺"
            enabled: row.editable
            implicitWidth: 24
            implicitHeight: 24
            leftPadding: 2
            rightPadding: 2
            onClicked: row.edit.resetValue(row.key)
            Accessible.name: qsTr("Reset %1").arg(row.label)
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Reset %1 (or double-click the slider)").arg(row.label)
            ToolTip.delay: 800
        }
    }
}
