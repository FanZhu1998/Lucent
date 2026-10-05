pragma ComponentBehavior: Bound

import QtQuick
import Lucent

// The masks tool's overlay over the displayed image (blueprint section 2: "brush, linear-gradient,
// and radial masks with feathering"; MaskEditor, mask_editor.hpp): the selected mask's outline (the
// frame's own, or the engine's outline of the local settings while it moves), its handles, and the
// brush. Positions are fractions of the displayed image placed through `imageRect` (the image in this
// item's coordinates), so they stay on the image at any zoom.
//
// Brush: with a brush mask selected, dragging paints one stroke (one revision), Alt (or the panel's
// Erase) erases. The cursor is a circle of the brush's size on screen. Handles: a radial mask's
// centre (move), the end of its first semi-axis (size and angle) and of its second (size); a linear
// gradient's start, end and middle (moves both). Each drag is one revision.
Item {
    id: overlay

    required property MaskEditor masks
    property rect imageRect

    clip: true

    function uOf(item, x, y) {
        const p = item.mapToItem(overlay, x, y)
        return (p.x - imageRect.x) / Math.max(1, imageRect.width)
    }
    function vOf(item, x, y) {
        const p = item.mapToItem(overlay, x, y)
        return (p.y - imageRect.y) / Math.max(1, imageRect.height)
    }

    OverlayPathItem {
        objectName: "maskOutlines"
        anchors.fill: parent
        paths: overlay.masks.outlines
        imageRect: overlay.imageRect
    }

    // The stroke being painted: the only path that changes on a pointer move, on a layer of its own,
    // so the mask's earlier strokes are not redrawn while it grows.
    OverlayPathItem {
        objectName: "maskStrokeOutline"
        anchors.fill: parent
        visible: overlay.masks.painting
        paths: overlay.masks.strokeOutline
        imageRect: overlay.imageRect
    }

    // Brush painting: the whole view, so a stroke may start or end beyond the image.
    MouseArea {
        id: paintArea

        objectName: "maskPaintArea"
        anchors.fill: parent
        enabled: overlay.masks.brushActive
        hoverEnabled: true
        preventStealing: true
        cursorShape: enabled ? Qt.BlankCursor : Qt.ArrowCursor
        onPressed: mouse => overlay.masks.beginStroke(overlay.uOf(paintArea, mouse.x, mouse.y),
                                                      overlay.vOf(paintArea, mouse.x, mouse.y),
                                                      (mouse.modifiers & Qt.AltModifier) !== 0)
        onPositionChanged: mouse => {
            if (pressed)
                overlay.masks.extendStroke(overlay.uOf(paintArea, mouse.x, mouse.y),
                                           overlay.vOf(paintArea, mouse.x, mouse.y))
        }
        onReleased: overlay.masks.endStroke()
        onCanceled: overlay.masks.endStroke()
    }

    // The brush cursor: its size on screen.
    Rectangle {
        id: cursor

        readonly property real r: Math.max(2, overlay.masks.brushRadius * overlay.imageRect.width)

        objectName: "brushCursor"
        visible: overlay.masks.brushActive && paintArea.containsMouse
        x: paintArea.mouseX - r
        y: paintArea.mouseY - r
        width: 2 * r
        height: 2 * r
        radius: r
        color: "transparent"
        border.width: 1.5
        border.color: overlay.masks.erase ? "#ff8a80" : "#ffffff"

        Rectangle {
            anchors.fill: parent
            anchors.margins: -1
            radius: width / 2
            color: "transparent"
            border.width: 1
            border.color: "#99000000"
        }

        Text {
            anchors.centerIn: parent
            visible: overlay.masks.erase
            text: "−"
            color: "#ff8a80"
            font.pixelSize: Theme.fontTitle
        }
    }

    // A count, not the handle list: the delegates (and a handle being dragged) stay while the handles
    // move; each reads its position from the list.
    Repeater {
        model: overlay.masks.handles.length

        delegate: Rectangle {
            id: handle

            required property int index
            readonly property var modelData: overlay.masks.handles[index] ?? { key: "", x: 0, y: 0 }
            readonly property bool primary: modelData.key === "center" || modelData.key === "middle"

            objectName: "maskHandle_" + modelData.key
            width: primary ? 14 : 11
            height: width
            radius: width / 2
            x: Math.round(overlay.imageRect.x + modelData.x * overlay.imageRect.width - width / 2)
            y: Math.round(overlay.imageRect.y + modelData.y * overlay.imageRect.height - height / 2)
            color: handleArea.pressed ? Theme.focusRing : primary ? Theme.accent : "#ffffff"
            border.width: 1.5
            border.color: "#cc000000"

            MouseArea {
                id: handleArea

                anchors.fill: parent
                anchors.margins: -7
                preventStealing: true
                cursorShape: handle.primary ? Qt.SizeAllCursor : Qt.CrossCursor
                onPressed: mouse => overlay.masks.beginHandle(handle.modelData.key,
                                                              overlay.uOf(handleArea, mouse.x, mouse.y),
                                                              overlay.vOf(handleArea, mouse.x, mouse.y))
                onPositionChanged: mouse => overlay.masks.moveHandle(overlay.uOf(handleArea, mouse.x, mouse.y),
                                                                     overlay.vOf(handleArea, mouse.x, mouse.y))
                onReleased: overlay.masks.endHandle()
                onCanceled: overlay.masks.endHandle()
                Component.onDestruction: {
                    if (pressed)
                        overlay.masks.endHandle()
                }
            }
        }
    }

    Rectangle {
        objectName: "maskHint"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        visible: overlay.masks.selectedId > 0
        implicitWidth: maskHintText.implicitWidth + 16
        implicitHeight: 24
        radius: 3
        color: Theme.scrim

        Text {
            id: maskHintText

            anchors.centerIn: parent
            text: overlay.masks.brushActive
                ? qsTr("%1 · paint to add · Alt+paint to erase · brush %2 px").arg(overlay.masks.selectedName)
                      .arg(overlay.masks.brushPixels)
                : qsTr("%1 · drag the handles · O shows the mask").arg(overlay.masks.selectedName)
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSmall
        }
    }
}
