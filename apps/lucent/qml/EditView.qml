import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Edit: one photo with generous image space, the filmstrip, details, and the export action
// (blueprint section 2, "Detailed individual editing"). The preview and its histogram are Lucent's
// own render; editing controls arrive in W6 and the clipping overlay is a placeholder until then.
//
// Keys: Left/Right previous/next photo, Home/End first/last, Ctrl+0 Fit, Ctrl+1 100 %, Z toggles
// the zoom, J toggles the clipping overlay, Ctrl+E exports, Esc returns to the library (Main.qml).
FocusScope {
    id: root

    required property AppController controller

    readonly property CurrentItem current: controller.current

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
                        Layout.maximumWidth: 360
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
                        ToolTip.text: qsTr("Highlight clipped shadows and highlights (J). Not available yet.")
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
            Layout.preferredWidth: 300
            color: Theme.surface

            Flickable {
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
                    y: Theme.margin
                    width: parent.width - 2 * Theme.margin
                    spacing: 22

                    Histogram {
                        objectName: "histogram"
                        Layout.fillWidth: true
                        item: root.current
                        rendering: viewer.rendering
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
        sequence: "Ctrl+E"
        enabled: root.visible && root.current.valid
        onActivated: root.exportRequested()
    }
}
