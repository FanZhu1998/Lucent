pragma ComponentBehavior: Bound

import QtQuick
import Lucent

// The crop tool's overlay over the displayed image (blueprint section 2, "crop, rotate, straighten";
// CropTool, crop_tool.hpp). While the tool is open the image is shown without its crop; this draws
// the crop rectangle (rule of thirds while it moves), dims what it leaves out and offers eight handles
// and the inside to drag. Positions are fractions of the displayed image (CropTool.rect) placed
// through `imageRect`, the image in this item's coordinates (ZoomableImage.imageRect), so they stay
// on the image at any zoom. Each drag is one revision.
//
// Levelling: in `levelMode`, or with Ctrl held, dragging draws a horizon; on release the photo is
// straightened so that line becomes level (or upright, whichever is nearer).
Item {
    id: overlay

    required property CropTool crop
    property rect imageRect
    property bool levelMode: false

    // The crop in this item's coordinates.
    readonly property rect cropRect: Qt.rect(imageRect.x + crop.rect.x * imageRect.width,
                                             imageRect.y + crop.rect.y * imageRect.height,
                                             crop.rect.width * imageRect.width,
                                             crop.rect.height * imageRect.height)
    readonly property bool levelling: levelArea.drawing

    signal levelled()

    clip: true

    function uOf(item, x) {
        const p = item.mapToItem(overlay, x, 0)
        return (p.x - imageRect.x) / Math.max(1, imageRect.width)
    }
    function vOf(item, y) {
        const p = item.mapToItem(overlay, 0, y)
        return (p.y - imageRect.y) / Math.max(1, imageRect.height)
    }

    // What the crop leaves out, dimmed.
    Rectangle {
        color: "#a6000000"
        x: overlay.imageRect.x
        y: overlay.imageRect.y
        width: overlay.imageRect.width
        height: Math.max(0, overlay.cropRect.y - overlay.imageRect.y)
    }
    Rectangle {
        color: "#a6000000"
        x: overlay.imageRect.x
        y: overlay.cropRect.y + overlay.cropRect.height
        width: overlay.imageRect.width
        height: Math.max(0, overlay.imageRect.y + overlay.imageRect.height - y)
    }
    Rectangle {
        color: "#a6000000"
        x: overlay.imageRect.x
        y: overlay.cropRect.y
        width: Math.max(0, overlay.cropRect.x - overlay.imageRect.x)
        height: overlay.cropRect.height
    }
    Rectangle {
        color: "#a6000000"
        x: overlay.cropRect.x + overlay.cropRect.width
        y: overlay.cropRect.y
        width: Math.max(0, overlay.imageRect.x + overlay.imageRect.width - x)
        height: overlay.cropRect.height
    }

    // The crop frame, with the rule of thirds.
    Item {
        id: frame

        objectName: "cropFrame"
        x: overlay.cropRect.x
        y: overlay.cropRect.y
        width: overlay.cropRect.width
        height: overlay.cropRect.height

        Repeater {
            model: 2

            delegate: Item {
                id: third

                required property int index

                anchors.fill: parent
                visible: overlay.crop.dragging

                Rectangle {
                    x: Math.round(frame.width * (third.index + 1) / 3)
                    width: 1
                    height: frame.height
                    color: "#99ffffff"
                }

                Rectangle {
                    y: Math.round(frame.height * (third.index + 1) / 3)
                    width: frame.width
                    height: 1
                    color: "#99ffffff"
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.width: 1
            border.color: "#f2ffffff"
        }

        // Dragging the inside moves the crop; with Ctrl it draws a horizon instead.
        MouseArea {
            id: moveArea

            anchors.fill: parent
            enabled: !overlay.levelMode
            preventStealing: true
            cursorShape: Qt.SizeAllCursor
            onPressed: mouse => {
                if ((mouse.modifiers & Qt.ControlModifier) !== 0) {
                    mouse.accepted = false  // the level area underneath takes it
                    return
                }
                overlay.crop.beginDrag("move", overlay.uOf(moveArea, mouse.x), overlay.vOf(moveArea, mouse.y))
            }
            onPositionChanged: mouse => overlay.crop.dragTo(overlay.uOf(moveArea, mouse.x),
                                                            overlay.vOf(moveArea, mouse.y))
            onReleased: overlay.crop.endDrag()
            onCanceled: overlay.crop.endDrag()
        }

        Repeater {
            model: [
                { name: "topLeft", fx: 0, fy: 0, cursor: Qt.SizeFDiagCursor },
                { name: "top", fx: 0.5, fy: 0, cursor: Qt.SizeVerCursor },
                { name: "topRight", fx: 1, fy: 0, cursor: Qt.SizeBDiagCursor },
                { name: "right", fx: 1, fy: 0.5, cursor: Qt.SizeHorCursor },
                { name: "bottomRight", fx: 1, fy: 1, cursor: Qt.SizeFDiagCursor },
                { name: "bottom", fx: 0.5, fy: 1, cursor: Qt.SizeVerCursor },
                { name: "bottomLeft", fx: 0, fy: 1, cursor: Qt.SizeBDiagCursor },
                { name: "left", fx: 0, fy: 0.5, cursor: Qt.SizeHorCursor }
            ]

            delegate: Rectangle {
                id: handle

                required property var modelData

                objectName: "cropHandle_" + modelData.name
                visible: !overlay.levelMode
                width: 12
                height: 12
                x: Math.round(frame.width * modelData.fx - width / 2)
                y: Math.round(frame.height * modelData.fy - height / 2)
                color: handleArea.pressed ? Theme.focusRing : "#ffffff"
                border.width: 1
                border.color: "#80000000"

                MouseArea {
                    id: handleArea

                    anchors.fill: parent
                    anchors.margins: -6
                    preventStealing: true
                    cursorShape: handle.modelData.cursor
                    onPressed: mouse => overlay.crop.beginDrag(handle.modelData.name, overlay.uOf(handleArea, mouse.x),
                                                               overlay.vOf(handleArea, mouse.y))
                    onPositionChanged: mouse => overlay.crop.dragTo(overlay.uOf(handleArea, mouse.x),
                                                                    overlay.vOf(handleArea, mouse.y))
                    onReleased: overlay.crop.endDrag()
                    onCanceled: overlay.crop.endDrag()
                }
            }
        }
    }

    // Levelling: a drawn horizon (Level button, or Ctrl+drag anywhere on the image).
    MouseArea {
        id: levelArea

        property bool drawing: false
        property point from
        property point to

        z: overlay.levelMode ? 1 : -1
        x: overlay.imageRect.x
        y: overlay.imageRect.y
        width: overlay.imageRect.width
        height: overlay.imageRect.height
        preventStealing: true
        cursorShape: overlay.levelMode ? Qt.CrossCursor : Qt.ArrowCursor
        onPressed: mouse => {
            if (!overlay.levelMode && (mouse.modifiers & Qt.ControlModifier) === 0) {
                mouse.accepted = false
                return
            }
            from = Qt.point(mouse.x, mouse.y)
            to = from
            drawing = true
        }
        onPositionChanged: mouse => {
            if (drawing)
                to = Qt.point(mouse.x, mouse.y)
        }
        onReleased: mouse => {
            if (!drawing)
                return
            drawing = false
            const w = Math.max(1, width)
            const h = Math.max(1, height)
            if (overlay.crop.level(from.x / w, from.y / h, mouse.x / w, mouse.y / h))
                overlay.levelled()
        }
        onCanceled: drawing = false

        // The horizon being drawn.
        Rectangle {
            visible: levelArea.drawing
            x: levelArea.from.x
            y: levelArea.from.y - 1
            width: Math.hypot(levelArea.to.x - levelArea.from.x, levelArea.to.y - levelArea.from.y)
            height: 2
            color: Theme.focusRing
            transformOrigin: Item.Left
            rotation: Math.atan2(levelArea.to.y - levelArea.from.y, levelArea.to.x - levelArea.from.x) * 180 / Math.PI
            antialiasing: true
        }
    }

    Rectangle {
        objectName: "cropHint"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        implicitWidth: hintText.implicitWidth + 16
        implicitHeight: 24
        radius: 3
        color: Theme.scrim

        Text {
            id: hintText

            anchors.centerIn: parent
            text: overlay.levelMode ? qsTr("Draw along the horizon to level the photo")
                                    : qsTr("Drag the frame or its handles · Ctrl+drag to level · Enter applies · Esc cancels")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSmall
        }
    }
}
