import QtQuick
import QtQuick.Controls
import QtQuick.Window
import Lucent

// The Edit view's image area: Fit or 100 % (one image pixel per device pixel), panning by dragging
// or scrolling when the image is larger than the view, and double-click to toggle the zoom at the
// clicked point.
//
// Two sources of pixels:
//   * Edit-session frames (W6, `framesExpected`): `frameSource` is the newest frame of the photo's
//     edit session (EditSession.frameSource; Quick frames while settings change, then the Final
//     one). Each frame is a new URL; the previous frame stays visible until the next one is loaded,
//     so a drag never flickers. `beforeSource` is the as-shot render for before/after (`compare`:
//     the whole image; `split`: the left part up to the divider), `clippingSource` the clipping
//     overlay drawn over the current frame: red where a colour channel reaches white, blue where
//     one reaches black (a deep shadow, or a saturated colour outside the display's gamut). Frames are shown at the
//     display size by the GPU (no sourceSize), so a proxy frame at 100 % appears upscaled until the
//     full-resolution Final frame arrives.
//   * Otherwise, `source` (image://preview, the W3 route): in Fit mode the preview is requested at
//     the displayed size in device pixels, rounded up to 256-pixel steps so that small window
//     resizes do not reload it; at 100 % the full resolution is requested. While a different size
//     of the same image loads, the previous one stays visible. A different image (another photo or
//     generation) never keeps the previous pixels.
//
// Nothing is requested while `active` is false or before the view has a size, and deactivating
// releases the image: a preview is a render in the application services and can be large.
//
// While a photo's own pixels are not there yet, `placeholderSource` (its thumbnail, usually the
// camera's embedded JPEG, already loaded for the filmstrip) is shown in its place, labelled, with
// a visible rendering state: a camera preview is never presented as Lucent's render.
//
// `incompleteRender` marks a preview rendered without a mandatory interpretation step (an
// Unsupported file) with a banner on the image: it is never presented as an accurate result
// (blueprint section 3).
FocusScope {
    id: root

    property string source
    property bool active: true
    property int pixelWidth   // full-resolution upright size; 0 when unknown
    property int pixelHeight
    property bool fitMode: true
    property string accessibleName
    property string unavailableText  // why the preview is unavailable, when known
    property bool clippingOverlay: false
    property string placeholderSource  // shown, labelled, until the preview is ready
    property bool placeholderIsCameraPreview: false
    property bool incompleteRender: false

    // W6: edit-session frames (see the file comment).
    property bool framesExpected: false
    property string frameSource
    property string beforeSource
    property string clippingSource
    property string frameError  // why the session cannot render the photo; empty when it can
    property bool refining: false  // a Final frame is on its way
    property bool compare: false
    property bool split: false
    property real splitPosition: 0.5

    // The newest frame that finished loading and is on screen (for latency measurements).
    property string shownFrame

    readonly property real devicePixelRatio: Screen.devicePixelRatio > 0 ? Screen.devicePixelRatio : 1
    readonly property bool hasSize: pixelWidth > 0 && pixelHeight > 0
    readonly property real naturalWidth: hasSize ? pixelWidth / devicePixelRatio : Math.max(1, flick.width)
    readonly property real naturalHeight: hasSize ? pixelHeight / devicePixelRatio : Math.max(1, flick.height)
    readonly property real fitScale: Math.min(1, flick.width / naturalWidth, flick.height / naturalHeight)
    readonly property real zoom: fitMode ? fitScale : 1
    readonly property real displayWidth: Math.max(1, Math.round(naturalWidth * zoom))
    readonly property real displayHeight: Math.max(1, Math.round(naturalHeight * zoom))
    readonly property size requestSize: fitMode || !hasSize
        ? Qt.size(roundUp(displayWidth * devicePixelRatio), roundUp(displayHeight * devicePixelRatio))
        : Qt.size(pixelWidth, pixelHeight)
    readonly property bool laidOut: active && flick.width > 0 && flick.height > 0
    readonly property bool frameVisible: framesExpected && frameImage.shownOnce
    // QQuickImageBase status of what is shown: the frame with an edit session, else the preview.
    readonly property int status: framesExpected
        ? (frameError.length > 0 && !frameVisible ? Image.Error : frameVisible ? Image.Ready : frameImage.status)
        : image.status
    // True while the preview (or a sharper size of it) is being produced.
    readonly property bool rendering: framesExpected ? (!frameVisible && frameError.length === 0) || refining
                                                     : image.status === Image.Loading
    readonly property bool showingPlaceholder: placeholder.visible
    readonly property bool showingBefore: framesExpected && (compare || split) && beforeImage.status === Image.Ready

    function roundUp(value) {
        return Math.max(256, Math.ceil(value / 256) * 256)
    }

    // Switches between Fit and 100 %, keeping the image point under (anchorX, anchorY) (view
    // coordinates; the centre by default) where it is.
    function setFit(fit, anchorX, anchorY) {
        if (fit === fitMode)
            return
        const ax = anchorX === undefined ? flick.width / 2 : anchorX
        const ay = anchorY === undefined ? flick.height / 2 : anchorY
        const relX = (flick.contentX + ax - image.x) / displayWidth
        const relY = (flick.contentY + ay - image.y) / displayHeight
        fitMode = fit
        flick.contentX = Math.max(0, Math.min(flick.contentWidth - flick.width, image.x + relX * displayWidth - ax))
        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, image.y + relY * displayHeight - ay))
    }

    // The preview source actually loaded: empty while inactive, not yet laid out, or showing frames.
    readonly property string loadedSource: laidOut && !framesExpected ? source : ""

    onLoadedSourceChanged: {
        image.retainWhileLoading = false
        image.source = root.loadedSource
    }
    onFrameSourceChanged: {
        if (frameSource.length === 0)
            frameImage.shownOnce = false
    }
    Component.onCompleted: image.source = root.loadedSource

    Rectangle {
        anchors.fill: parent
        color: Theme.imageBackground
    }

    Flickable {
        id: flick

        anchors.fill: parent
        clip: true
        contentWidth: Math.max(width, root.displayWidth)
        contentHeight: Math.max(height, root.displayHeight)
        interactive: !root.fitMode
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.horizontal: ScrollBar {
            policy: root.fitMode ? ScrollBar.AlwaysOff : ScrollBar.AsNeeded
        }
        ScrollBar.vertical: ScrollBar {
            policy: root.fitMode ? ScrollBar.AlwaysOff : ScrollBar.AsNeeded
        }

        Image {
            id: placeholder

            objectName: "previewPlaceholder"
            x: image.x
            y: image.y
            width: image.width
            height: image.height
            source: root.laidOut ? root.placeholderSource : ""
            // The filmstrip's request size, so Qt's pixmap cache already holds it.
            sourceSize: Qt.size(512, 512)
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            visible: status === Image.Ready
                     && (root.framesExpected ? !root.frameVisible && root.frameError.length === 0
                                             : !image.visible && image.status !== Image.Error)
            opacity: 0.85
            Accessible.ignored: true
        }

        Image {
            id: image

            x: Math.round((flick.contentWidth - width) / 2)
            y: Math.round((flick.contentHeight - height) / 2)
            width: root.displayWidth
            height: root.displayHeight
            sourceSize: root.requestSize
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: false
            smooth: true
            mipmap: root.fitMode
            visible: !root.framesExpected && (status === Image.Ready || retainWhileLoading)

            Accessible.role: Accessible.Graphic
            Accessible.name: root.accessibleName

            onStatusChanged: {
                // From now on, size changes of this image may keep showing it while they load.
                if (status === Image.Ready)
                    retainWhileLoading = true
            }
        }

        // The edit session's newest frame. The previous frame stays while the next one loads.
        Image {
            id: frameImage

            property bool shownOnce: false

            objectName: "frameImage"
            x: image.x
            y: image.y
            width: image.width
            height: image.height
            source: root.laidOut && root.framesExpected ? root.frameSource : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: false
            smooth: true
            mipmap: root.fitMode
            retainWhileLoading: true
            visible: root.framesExpected && shownOnce

            Accessible.role: Accessible.Graphic
            Accessible.name: root.accessibleName

            onStatusChanged: {
                if (status === Image.Ready && source.toString().length > 0) {
                    shownOnce = true
                    root.shownFrame = source.toString()
                }
            }
        }

        Image {
            id: clippingImage

            objectName: "clippingImage"
            x: image.x
            y: image.y
            width: image.width
            height: image.height
            source: root.laidOut && root.framesExpected && root.clippingOverlay ? root.clippingSource : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: false
            smooth: false
            retainWhileLoading: true
            visible: frameImage.visible && root.clippingOverlay && status === Image.Ready
                     && !(root.compare && !root.split)
            Accessible.ignored: true
        }

        // As shot: the whole image (compare) or the part left of the divider (split).
        Item {
            id: beforeClip

            x: image.x
            y: image.y
            width: root.split ? Math.round(image.width * root.splitPosition) : image.width
            height: image.height
            clip: true
            visible: root.showingBefore

            Image {
                id: beforeImage

                objectName: "beforeImage"
                width: image.width
                height: image.height
                source: root.laidOut && root.framesExpected && (root.compare || root.split) ? root.beforeSource : ""
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: false
                smooth: true
                mipmap: root.fitMode
                retainWhileLoading: true
                Accessible.role: Accessible.Graphic
                Accessible.name: qsTr("As shot: %1").arg(root.accessibleName)
            }
        }

        // The split divider: drag it, or focus it (Tab) and use the arrow keys (Shift: larger
        // steps, Home/End: the edges), to compare another part.
        Rectangle {
            id: divider

            objectName: "splitDivider"
            visible: root.showingBefore && root.split
            x: image.x + Math.round(image.width * root.splitPosition) - 1
            y: image.y
            width: 2
            height: image.height
            color: activeFocus ? Theme.focusRing : Theme.textPrimary
            opacity: 0.85
            activeFocusOnTab: true  // only while visible: hidden items are not in the tab chain
            Accessible.role: Accessible.Splitter
            Accessible.name: qsTr("Before and after divider")
            Accessible.description: qsTr("%1 % of the width shows the photo as shot")
                .arg(Math.round(root.splitPosition * 100))
            Accessible.focusable: true

            function moveBy(fraction) {
                root.splitPosition = Math.max(0.02, Math.min(0.98, root.splitPosition + fraction))
            }

            Keys.onPressed: event => {
                const step = (event.modifiers & Qt.ShiftModifier) !== 0 ? 0.1 : 0.02
                switch (event.key) {
                case Qt.Key_Left:
                    divider.moveBy(-step)
                    break
                case Qt.Key_Right:
                    divider.moveBy(step)
                    break
                case Qt.Key_Home:
                    divider.moveBy(-1)
                    break
                case Qt.Key_End:
                    divider.moveBy(1)
                    break
                default:
                    return
                }
                event.accepted = true
            }

            Rectangle {
                anchors.centerIn: parent
                width: 18
                height: 36
                radius: 4
                color: Theme.scrim
                border.width: 1
                border.color: Theme.textPrimary

                Text {
                    anchors.centerIn: parent
                    text: "⇔"
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSmall
                }
            }

            MouseArea {
                anchors.fill: parent
                anchors.leftMargin: -10
                anchors.rightMargin: -10
                cursorShape: Qt.SplitHCursor
                preventStealing: true
                onPositionChanged: event => {
                    if (!pressed)
                        return
                    const x = mapToItem(image, event.x, event.y).x
                    root.splitPosition = Math.max(0.02, Math.min(0.98, x / image.width))
                }
            }
        }
    }

    TapHandler {
        onDoubleTapped: eventPoint => root.setFit(!root.fitMode, eventPoint.position.x, eventPoint.position.y)
    }

    HoverHandler {
        cursorShape: root.fitMode ? Qt.ArrowCursor : Qt.OpenHandCursor
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: root.framesExpected ? root.laidOut && !root.frameVisible && root.frameError.length === 0
                                     : image.status === Image.Loading && !image.retainWhileLoading
        visible: running
        Accessible.name: qsTr("Rendering the preview")
    }

    // What is on screen while Lucent renders: the camera's JPEG (or an earlier Lucent thumbnail)
    // is labelled as such, never passed off as the render.
    Rectangle {
        objectName: "renderingLabel"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 12
        visible: root.framesExpected ? root.laidOut && !root.frameVisible && root.frameError.length === 0
                                     : image.status === Image.Loading
        implicitWidth: renderingText.implicitWidth + 16
        implicitHeight: 24
        radius: 3
        color: Theme.scrim

        Accessible.role: Accessible.StaticText
        Accessible.name: renderingText.text

        Text {
            id: renderingText

            anchors.centerIn: parent
            text: !root.framesExpected && image.retainWhileLoading ? qsTr("Rendering a sharper preview…")
                : placeholder.visible && root.placeholderIsCameraPreview ? qsTr("Camera preview · Lucent is rendering…")
                : placeholder.visible ? qsTr("Lucent thumbnail · rendering the preview…")
                : qsTr("Lucent is rendering…")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSmall
        }
    }

    // Before/after labels.
    Rectangle {
        objectName: "beforeLabel"
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 12
        visible: root.showingBefore
        implicitWidth: beforeText.implicitWidth + 16
        implicitHeight: 24
        radius: 3
        color: Theme.scrim

        Text {
            id: beforeText

            anchors.centerIn: parent
            text: qsTr("Before · as shot")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSmall
        }
    }

    Rectangle {
        objectName: "afterLabel"
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        anchors.rightMargin: 52
        visible: root.showingBefore && root.split
        implicitWidth: afterText.implicitWidth + 16
        implicitHeight: 24
        radius: 3
        color: Theme.scrim

        Text {
            id: afterText

            anchors.centerIn: parent
            text: qsTr("After")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSmall
        }
    }

    Rectangle {
        objectName: "incompleteRenderBanner"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 12
        width: Math.min(parent.width - 24, incompleteText.implicitWidth + 36)
        height: incompleteText.implicitHeight + 12
        visible: root.incompleteRender && root.status === Image.Ready
        radius: 3
        color: Theme.scrim

        Accessible.role: Accessible.StaticText
        Accessible.name: incompleteText.text

        Rectangle {
            id: incompleteDot
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 8
            height: 8
            radius: 4
            color: Theme.unsupported
        }

        Text {
            id: incompleteText
            anchors.left: incompleteDot.right
            anchors.leftMargin: 8
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Unsupported file: rendered without a required processing step. Not an accurate result.")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
        }
    }

    BusyIndicator {
        objectName: "refiningIndicator"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 12
        width: 28
        height: 28
        running: root.framesExpected ? root.refining && root.frameVisible
                                     : image.status === Image.Loading && image.retainWhileLoading
        visible: running
        Accessible.name: qsTr("Refining preview")
    }

    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 420)
        spacing: 8
        visible: root.framesExpected ? root.frameError.length > 0 && !root.frameVisible
                                     : root.source.length === 0 || image.status === Image.Error

        Text {
            width: parent.width
            text: !root.framesExpected && root.source.length === 0 ? qsTr("No photo selected")
                                                                   : qsTr("Preview unavailable")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontTitle
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Text {
            readonly property string reason: root.framesExpected ? root.frameError : root.unavailableText

            width: parent.width
            visible: reason.length > 0 && (root.framesExpected || image.status === Image.Error)
            text: reason
            color: Theme.textTertiary
            font.pixelSize: Theme.fontBody
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 12
        visible: root.status === Image.Ready
        implicitWidth: zoomText.implicitWidth + 14
        implicitHeight: 22
        radius: 3
        color: Theme.scrim

        Text {
            id: zoomText
            anchors.centerIn: parent
            text: root.fitMode ? qsTr("Fit · %1 %").arg(Math.round(root.zoom * 100)) : qsTr("100 %")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSmall
        }
    }

    Rectangle {
        objectName: "clippingOverlayNotice"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        visible: root.clippingOverlay
        implicitWidth: clippingRow.implicitWidth + 16
        implicitHeight: 22
        radius: 3
        color: Theme.scrim

        Row {
            id: clippingRow

            anchors.centerIn: parent
            spacing: 6

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 9
                height: 9
                color: "#ff2828"
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Highlights clipped")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSmall
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 9
                height: 9
                color: "#2878ff"
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Shadows or colours clipped")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSmall
            }
        }
    }
}
