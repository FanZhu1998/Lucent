import QtQuick
import QtQuick.Controls
import QtQuick.Window
import Lucent

// The Edit view's image area: Fit or 100 % (one image pixel per device pixel), panning by dragging
// or scrolling when the image is larger than the view, and double-click to toggle the zoom at the
// clicked point.
//
// Image requests: in Fit mode the preview is requested at the displayed size in device pixels,
// rounded up to 256-pixel steps so that small window resizes do not reload it; at 100 % the full
// resolution is requested. While a different size of the same image loads, the previous one stays
// visible (quick preview, then refine). A different image (another photo or generation) never
// keeps the previous pixels: it shows the loading state until its own pixels arrive.
//
// Nothing is requested while `active` is false or before the view has a size, and deactivating
// releases the image: a preview is a render in the application services and can be large.
//
// While a photo's own preview is not there yet, `placeholderSource` (its thumbnail, usually the
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
    readonly property alias status: image.status
    // True while the preview (or a sharper size of it) is being produced.
    readonly property bool rendering: image.status === Image.Loading
    readonly property bool showingPlaceholder: placeholder.visible

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

    // The source actually loaded: empty while inactive or not yet laid out.
    readonly property string loadedSource: active && flick.width > 0 && flick.height > 0 ? source : ""

    onLoadedSourceChanged: {
        image.retainWhileLoading = false
        image.source = root.loadedSource
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
            source: root.loadedSource.length > 0 ? root.placeholderSource : ""
            // The filmstrip's request size, so Qt's pixmap cache already holds it.
            sourceSize: Qt.size(512, 512)
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            visible: !image.visible && image.status !== Image.Error && status === Image.Ready
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
            visible: status === Image.Ready || retainWhileLoading

            Accessible.role: Accessible.Graphic
            Accessible.name: root.accessibleName

            onStatusChanged: {
                // From now on, size changes of this image may keep showing it while they load.
                if (status === Image.Ready)
                    retainWhileLoading = true
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
        running: image.status === Image.Loading && !image.retainWhileLoading
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
        visible: image.status === Image.Loading
        implicitWidth: renderingText.implicitWidth + 16
        implicitHeight: 24
        radius: 3
        color: Theme.scrim

        Accessible.role: Accessible.StaticText
        Accessible.name: renderingText.text

        Text {
            id: renderingText

            anchors.centerIn: parent
            text: image.retainWhileLoading ? qsTr("Rendering a sharper preview…")
                : placeholder.visible && root.placeholderIsCameraPreview ? qsTr("Camera preview · Lucent is rendering…")
                : placeholder.visible ? qsTr("Lucent thumbnail · rendering the preview…")
                : qsTr("Lucent is rendering…")
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
        visible: root.incompleteRender && image.status === Image.Ready
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
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 12
        width: 28
        height: 28
        running: image.status === Image.Loading && image.retainWhileLoading
        visible: running
        Accessible.name: qsTr("Refining preview")
    }

    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 420)
        spacing: 8
        visible: root.source.length === 0 || image.status === Image.Error

        Text {
            width: parent.width
            text: root.source.length === 0 ? qsTr("No photo selected") : qsTr("Preview unavailable")
            color: Theme.textSecondary
            font.pixelSize: Theme.fontTitle
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }

        Text {
            width: parent.width
            visible: image.status === Image.Error && root.unavailableText.length > 0
            text: root.unavailableText
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
        visible: image.status === Image.Ready
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
        implicitWidth: clippingText.implicitWidth + 14
        implicitHeight: 22
        radius: 3
        color: Theme.scrim

        Text {
            id: clippingText
            anchors.centerIn: parent
            text: qsTr("Clipping overlay is not available yet")
            color: Theme.attention
            font.pixelSize: Theme.fontSmall
        }
    }
}
