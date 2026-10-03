import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// The tone curve (blueprint section 2: "a monotone tone curve"), drawn over the histogram of the
// current frame. Points stay monotone (EditSession.moveCurvePoint). Mouse: press on the curve to add
// a point and drag it, drag a point to move it, double-click a point to remove it; a drag is ONE
// revision. Keyboard (when the curve has focus): [ and ] select the previous or next point, the
// arrow keys move it (Shift: further; holding a key is one revision), Delete removes it, Insert adds
// one in the widest gap.
ColumnLayout {
    id: editor

    required property EditSession edit

    spacing: 6

    Rectangle {
        id: frame

        objectName: "toneCurve"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.round(width * 0.62)
        radius: Theme.radius
        color: Theme.imageBackground
        border.width: plotArea.activeFocus ? 2 : 1
        border.color: plotArea.activeFocus ? Theme.focusRing : Theme.divider

        ToneCurvePlot {
            id: plot

            objectName: "toneCurvePlot"
            anchors.fill: parent
            anchors.margins: 2
            enabled: editor.edit.canEdit
            points: editor.edit.curvePoints
            samples: editor.edit.curveSamples
            histogram: editor.edit.frameInfo
            selectedIndex: editor.edit.selectedCurvePoint
        }

        FocusScope {
            id: plotArea

            anchors.fill: plot
            activeFocusOnTab: true

            Accessible.role: Accessible.Graphic
            Accessible.name: qsTr("Tone curve")
            Accessible.description: {
                const points = editor.edit.curvePoints
                const index = editor.edit.selectedCurvePoint
                let text = qsTr("%1 points").arg(points.length)
                if (index >= 0 && index < points.length)
                    text += qsTr(", point %1 selected at input %2, output %3").arg(index + 1)
                        .arg(Math.round(points[index].x * 100)).arg(Math.round(points[index].y * 100))
                return text
            }

            function selectNext(delta) {
                const count = editor.edit.curvePoints.length
                if (count === 0)
                    return
                const current = editor.edit.selectedCurvePoint
                const next = current < 0 ? (delta > 0 ? 0 : count - 1) : Math.max(0, Math.min(count - 1, current + delta))
                editor.edit.selectedCurvePoint = next
            }

            function addInWidestGap() {
                const points = editor.edit.curvePoints
                let best = -1
                let width = 0
                for (let i = 0; i + 1 < points.length; ++i) {
                    if (points[i + 1].x - points[i].x > width) {
                        width = points[i + 1].x - points[i].x
                        best = i
                    }
                }
                if (best >= 0)
                    editor.edit.addCurvePoint((points[best].x + points[best + 1].x) / 2)
            }

            Keys.onPressed: event => {
                const index = editor.edit.selectedCurvePoint
                const step = (event.modifiers & Qt.ShiftModifier) ? 0.05 : 0.01
                switch (event.key) {
                case Qt.Key_BracketLeft:
                    selectNext(-1)
                    break
                case Qt.Key_BracketRight:
                    selectNext(1)
                    break
                case Qt.Key_Left:
                    editor.edit.nudgeCurvePoint(index, -step, 0)
                    break
                case Qt.Key_Right:
                    editor.edit.nudgeCurvePoint(index, step, 0)
                    break
                case Qt.Key_Up:
                    editor.edit.nudgeCurvePoint(index, 0, step)
                    break
                case Qt.Key_Down:
                    editor.edit.nudgeCurvePoint(index, 0, -step)
                    break
                case Qt.Key_Delete:
                case Qt.Key_Backspace:
                    editor.edit.removeCurvePoint(index)
                    break
                case Qt.Key_Insert:
                    addInWidestGap()
                    break
                default:
                    return
                }
                event.accepted = true
            }
            Keys.onReleased: event => {
                if (!event.isAutoRepeat && (event.key === Qt.Key_Left || event.key === Qt.Key_Right
                                            || event.key === Qt.Key_Up || event.key === Qt.Key_Down)) {
                    editor.edit.endNudge()
                    event.accepted = true
                }
            }

            MouseArea {
                id: mouse

                property int dragIndex: -1

                anchors.fill: parent
                enabled: editor.edit.canEdit
                hoverEnabled: true
                cursorShape: dragIndex >= 0 ? Qt.ClosedHandCursor
                    : plot.pointAt(mouseX, mouseY, 9) >= 0 ? Qt.OpenHandCursor : Qt.CrossCursor

                onPressed: event => {
                    plotArea.forceActiveFocus(Qt.MouseFocusReason)
                    let index = plot.pointAt(event.x, event.y, 9)
                    if (index < 0) {
                        // Only on (or near) the curve: a click elsewhere selects nothing.
                        const at = plot.toCurve(event.x, event.y)
                        const onCurve = plot.toItem(at.x, editor.edit.curveValueAt(at.x))
                        if (Math.abs(onCurve.y - event.y) > 14) {
                            editor.edit.selectedCurvePoint = -1
                            return
                        }
                        editor.edit.beginGesture()
                        index = editor.edit.addCurvePoint(at.x)
                    } else {
                        editor.edit.beginGesture()
                    }
                    dragIndex = index
                    editor.edit.selectedCurvePoint = index
                }
                onPositionChanged: event => {
                    if (dragIndex < 0)
                        return
                    const at = plot.toCurve(event.x, event.y)
                    editor.edit.moveCurvePoint(dragIndex, at.x, at.y)
                }
                onReleased: {
                    dragIndex = -1
                    editor.edit.endGesture()
                }
                onCanceled: {
                    dragIndex = -1
                    editor.edit.endGesture()
                }
                onDoubleClicked: event => {
                    const index = plot.pointAt(event.x, event.y, 9)
                    if (index > 0)
                        editor.edit.removeCurvePoint(index)
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4

        LockButton {
            objectName: "lock_toneCurve"
            controlName: qsTr("Tone curve")
            enabled: editor.edit.canEdit
            checked: { editor.edit.settingsRevision; return editor.edit.isLocked("toneCurve") }
            onClicked: editor.edit.setLocked("toneCurve", checked)
        }

        Text {
            text: qsTr("Click the curve to add a point; drag to move; double-click to remove.")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        StyledToolButton {
            objectName: "removeCurvePoint"
            text: qsTr("Remove")
            implicitHeight: 24
            enabled: editor.edit.canEdit && editor.edit.selectedCurvePoint > 0
                     && editor.edit.selectedCurvePoint < editor.edit.curvePoints.length - 1
            onClicked: editor.edit.removeCurvePoint(editor.edit.selectedCurvePoint)
            Accessible.name: qsTr("Remove the selected curve point")
        }

        StyledToolButton {
            objectName: "resetCurve"
            text: qsTr("Reset")
            implicitHeight: 24
            enabled: editor.edit.canEdit && !editor.edit.curveIsIdentity
            onClicked: editor.edit.resetCurve()
            Accessible.name: qsTr("Reset the tone curve")
        }
    }
}
