import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Edit: one photo with generous image space, the filmstrip, the adjustments, tone curve, history and
// details, and the export action (blueprint section 2, "Detailed individual editing"). While the
// Edit view shows a photo it has an edit session (AppController.edit): the image is the session's
// newest frame (a quick render on the cached proxy while settings change, then the final one), the
// histogram follows it, and every gesture becomes one revision of the photo's recipe.
//
// Keys: Left/Right previous/next photo (when the image has focus), Home/End first/last, Ctrl+0
// Fit, Ctrl+1 100 %, Z toggles the zoom, \ before/after, J clipping overlay, Ctrl+Z undo, Ctrl+Y
// (or Ctrl+Shift+Z) redo, Ctrl+Shift+C copy settings, Ctrl+Shift+V paste settings, Ctrl+Shift+A
// Auto (W8), Ctrl+E export, Esc returns to the library (Main.qml). In a slider: arrows step (Shift:
// coarse), Home resets.
FocusScope {
    id: root

    required property AppController controller

    readonly property CurrentItem current: controller.current
    readonly property EditSession edit: controller.edit

    signal exportRequested()

    function takeFocus() {
        viewer.forceActiveFocus(Qt.OtherFocusReason)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.toolbarHeight
                color: Theme.window

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: Theme.margin
                    spacing: Theme.spacing

                    StyledToolButton {
                        text: qsTr("‹ Library")
                        onClicked: root.controller.showView(AppController.Library)
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Back to the library (Esc)")
                        ToolTip.delay: 800
                    }

                    Text {
                        objectName: "editTitle"
                        text: root.current.fileName
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontTitle
                        font.weight: Font.DemiBold
                        elide: Text.ElideMiddle
                        Layout.maximumWidth: 280
                    }

                    Text {
                        text: root.current.valid
                            ? qsTr("%1 of %2").arg(root.current.row + 1).arg(root.controller.library.count) : ""
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontBody
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    StyledToolButton {
                        objectName: "undoToolButton"
                        text: qsTr("Undo")
                        enabled: root.edit.canUndo
                        onClicked: root.edit.undo()
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Undo the last change (Ctrl+Z)")
                        ToolTip.delay: 800
                    }

                    StyledToolButton {
                        objectName: "redoToolButton"
                        text: qsTr("Redo")
                        enabled: root.edit.canRedo
                        onClicked: root.edit.redo()
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Redo (Ctrl+Y)")
                        ToolTip.delay: 800
                    }

                    ToolSeparator {}

                    StyledToolButton {
                        objectName: "compareToggle"
                        text: qsTr("Before")
                        checkable: true
                        checked: root.edit.compare
                        enabled: root.edit.active
                        onToggled: root.edit.compare = checked
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Show the photo as shot (\\)")
                        ToolTip.delay: 800
                    }

                    StyledToolButton {
                        objectName: "splitToggle"
                        text: qsTr("Split")
                        checkable: true
                        checked: root.edit.split
                        enabled: root.edit.active
                        onToggled: root.edit.split = checked
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Before and after side by side; drag the divider")
                        ToolTip.delay: 800
                    }

                    ToolSeparator {}

                    StyledToolButton {
                        objectName: "fitButton"
                        text: qsTr("Fit")
                        checkable: true
                        checked: viewer.fitMode
                        onClicked: viewer.setFit(true)
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Fit the photo in the window (Ctrl+0)")
                        ToolTip.delay: 800
                    }

                    StyledToolButton {
                        objectName: "actualSizeButton"
                        text: qsTr("100 %")
                        checkable: true
                        checked: !viewer.fitMode
                        onClicked: viewer.setFit(false)
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("One image pixel per screen pixel (Ctrl+1)")
                        ToolTip.delay: 800
                    }

                    ToolSeparator {}

                    StyledToolButton {
                        objectName: "clippingToggle"
                        text: qsTr("Clipping")
                        checkable: true
                        checked: root.controller.clippingOverlay
                        onToggled: root.controller.clippingOverlay = checked
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Show where a colour channel clips: at white in red, at black in blue (J)")
                        ToolTip.delay: 800
                    }

                    StyledButton {
                        objectName: "exportButton"
                        text: qsTr("Export…")
                        highlighted: true
                        enabled: root.current.valid
                        onClicked: root.exportRequested()
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Export this photo as JPEG or 16-bit TIFF into a folder you choose (Ctrl+E)")
                        ToolTip.delay: 800
                    }
                }
            }

            ZoomableImage {
                id: viewer

                objectName: "previewViewer"
                Layout.fillWidth: true
                Layout.fillHeight: true
                activeFocusOnTab: true  // Tab back here to use Left/Right and the zoom keys
                focus: true
                // Previews are requested only while Edit is shown.
                active: root.visible
                source: root.current.previewSource
                pixelWidth: root.current.pixelWidth
                pixelHeight: root.current.pixelHeight
                accessibleName: qsTr("Preview of %1").arg(root.current.fileName)
                unavailableText: root.current.processingDetail.length > 0 ? root.current.processingDetail
                                                                          : root.current.supportDetail
                placeholderSource: root.current.thumbnailSource
                placeholderIsCameraPreview: root.current.thumbnailIsCameraPreview
                incompleteRender: root.current.renderIncomplete
                clippingOverlay: root.controller.clippingOverlay
                framesExpected: root.edit.active
                frameSource: root.edit.frameSource
                beforeSource: root.edit.beforeSource
                clippingSource: root.edit.clippingSource
                frameError: root.edit.frameError
                refining: root.edit.busy && root.edit.framePhase === "quick" && !root.edit.gestureActive
                compare: root.edit.compare
                split: root.edit.split
                onShownFrameChanged: root.edit.noteFrameShown(shownFrame)
                onFitModeChanged: root.edit.setFullResolution(!fitMode)
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.divider
            }

            Filmstrip {
                objectName: "filmstrip"
                Layout.fillWidth: true
                Layout.preferredHeight: 104
                controller: root.controller
            }
        }

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 1
            color: Theme.divider
        }

        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 344
            color: Theme.surface

            Flickable {
                id: panelFlick

                objectName: "editPanel"
                anchors.fill: parent
                contentHeight: sidePanel.implicitHeight + 2 * Theme.margin
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }

                ColumnLayout {
                    id: sidePanel

                    x: Theme.margin
                    y: Theme.margin - 4
                    width: parent.width - 2 * Theme.margin
                    spacing: 14

                    Histogram {
                        objectName: "histogram"
                        Layout.fillWidth: true
                        item: root.current
                        info: root.edit.hasFrame ? root.edit.frameInfo : root.current.renderInfo
                        rendering: viewer.rendering
                    }

                    // The recipe cannot be changed (made by a newer Lucent, or a read-only library).
                    Rectangle {
                        objectName: "readOnlyBanner"
                        visible: root.edit.readOnly
                        Layout.fillWidth: true
                        implicitHeight: readOnlyText.implicitHeight + 16
                        radius: Theme.radius
                        color: Theme.surfaceRaised
                        border.width: 1
                        border.color: Theme.attention

                        Accessible.role: Accessible.AlertMessage
                        Accessible.name: readOnlyText.text

                        Text {
                            id: readOnlyText

                            objectName: "readOnlyReason"
                            anchors.fill: parent
                            anchors.margins: 8
                            text: root.edit.readOnlyReason
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.Wrap
                        }
                    }

                    // The outcome of the last copy, paste or refused edit.
                    RowLayout {
                        objectName: "editMessage"
                        visible: root.edit.message.length > 0
                        Layout.fillWidth: true
                        spacing: 4

                        Text {
                            objectName: "editMessageText"
                            text: root.edit.message
                            color: Theme.attention
                            font.pixelSize: Theme.fontSmall
                            wrapMode: Text.Wrap
                            Layout.fillWidth: true
                            Accessible.role: Accessible.StaticText
                            Accessible.name: text
                        }

                        StyledToolButton {
                            text: "×"
                            implicitWidth: 22
                            implicitHeight: 22
                            onClicked: root.edit.clearMessage()
                            Accessible.name: qsTr("Dismiss the message")
                        }
                    }

                    // W8: Auto's progress, outcome and what it did to the current revision.
                    AutoSettingsPanel {
                        objectName: "autoSettingsPanel"
                        Layout.fillWidth: true
                        edit: root.edit
                    }

                    AdjustPanel {
                        objectName: "adjustPanel"
                        Layout.fillWidth: true
                        edit: root.edit
                        onCopyRequested: copyDialog.open()
                    }

                    PanelSection {
                        objectName: "toneCurveSection"
                        title: qsTr("Tone curve")
                        Layout.fillWidth: true

                        ToneCurveEditor {
                            Layout.fillWidth: true
                            edit: root.edit
                        }
                    }

                    PanelSection {
                        objectName: "historySection"
                        title: qsTr("History")
                        Layout.fillWidth: true

                        HistoryPanel {
                            objectName: "historyPanel"
                            Layout.fillWidth: true
                            edit: root.edit
                        }
                    }

                    MetadataPanel {
                        objectName: "metadataPanel"
                        Layout.fillWidth: true
                        item: root.current
                        controller: root.controller
                    }
                }
            }
        }
    }

    CopySettingsDialog {
        id: copyDialog

        edit: root.edit
    }

    Keys.onLeftPressed: root.controller.selectRelative(-1)
    Keys.onRightPressed: root.controller.selectRelative(1)
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Home) {
            root.controller.selectRelative(-root.controller.library.count)
            event.accepted = true
        } else if (event.key === Qt.Key_End) {
            root.controller.selectRelative(root.controller.library.count)
            event.accepted = true
        }
    }

    Shortcut {
        sequence: "Ctrl+0"
        enabled: root.visible
        onActivated: viewer.setFit(true)
    }

    Shortcut {
        sequence: "Ctrl+1"
        enabled: root.visible
        onActivated: viewer.setFit(false)
    }

    Shortcut {
        sequence: "Z"
        enabled: root.visible
        onActivated: viewer.setFit(!viewer.fitMode)
    }

    Shortcut {
        sequence: "J"
        enabled: root.visible
        onActivated: root.controller.clippingOverlay = !root.controller.clippingOverlay
    }

    Shortcut {
        sequence: "\\"
        enabled: root.visible && root.edit.active
        onActivated: root.edit.compare = !root.edit.compare
    }

    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: root.visible && root.edit.canUndo
        onActivated: root.edit.undo()
    }

    Shortcut {
        sequences: ["Ctrl+Y", "Ctrl+Shift+Z"]
        enabled: root.visible && root.edit.canRedo
        onActivated: root.edit.redo()
    }

    Shortcut {
        sequence: "Ctrl+Shift+C"
        enabled: root.visible && root.edit.ready && !root.edit.readOnly
        onActivated: copyDialog.open()
    }

    Shortcut {
        sequence: "Ctrl+Shift+V"
        enabled: root.visible && root.edit.hasClipboard && root.edit.canEdit
        onActivated: root.edit.pasteSettings()
    }

    Shortcut {
        sequence: "Ctrl+E"
        enabled: root.visible && root.current.valid
        onActivated: root.exportRequested()
    }

    Shortcut {
        sequence: "Ctrl+Shift+A"
        enabled: root.visible && root.edit.canRunAuto
        onActivated: root.edit.runAuto()
    }
}
