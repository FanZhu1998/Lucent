import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Help > About (W10, ADR 0018): Lucent's version, the camera support database the session uses
// (its version and qualification date, or why none is in use) and what the four support states
// mean (blueprint section 3, "User-visible compatibility states"). W12 (ADR 0022): "Check for
// updates" (UpdatePanel). W13 (ADR 0023): the lens database the lens corrections use, with its
// licence (CC BY-SA 3.0, the Lensfun project's data). W12 release phase (ADR 0025): Lucent's logo
// beside the title.
Popup {
    id: about

    required property AppController controller

    readonly property var support: controller.cameraSupport
    readonly property var lenses: controller.lensDatabase

    objectName: "aboutDialog"
    parent: Overlay.overlay
    // Centred on whole device pixels (the window's scale, read from the overlay's screen): at 125, 175
    // or 225 % a whole logical position can fall between device pixels, and the logo, drawn 1:1 from
    // the image of its scale's size, would be resampled and blurred. The padding is 20 logical pixels,
    // whole device pixels at every scale Windows offers.
    readonly property real devicePixelRatio: parent ? parent.Screen.devicePixelRatio : 1
    x: Math.round((parent.width - width) / 2 * devicePixelRatio) / devicePixelRatio
    y: Math.round((parent.height - height) / 2 * devicePixelRatio) / devicePixelRatio
    width: Math.min(620, parent.width - 64)
    height: Math.min(content.implicitHeight + 2 * padding, parent.height - 64)
    modal: true
    focus: true
    padding: 20

    background: Rectangle {
        color: Theme.surface
        radius: Theme.radius
        border.width: 1
        border.color: Theme.border
    }

    // Whether a downloaded update may be installed now follows the running exports and Auto.
    onOpened: about.controller.updates.refresh()

    contentItem: Flickable {
        clip: true
        contentWidth: width
        contentHeight: content.implicitHeight
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        ColumnLayout {
            id: content

            width: parent.width
            spacing: 10

            RowLayout {
                spacing: 14
                Layout.fillWidth: true

                // The logo at 64 logical pixels. sourceSize makes Qt Quick ask the "branding" provider
                // for 64 x the device pixel ratio it knows when the image loads, which the provider
                // serves from the pre-rendered image of that size (src/branding.hpp): drawn 1:1, sharp
                // at every scale. It loads only while the dialog is in a window, so that this ratio is
                // the window's: outside one (the closed dialog) Qt Quick takes the largest ratio of all
                // screens, and on a 125 % screen beside a 150 % one would scale a 96-pixel image down.
                // Its plate, not the image's margin, lines up with the text column below (review finding
                // BRD-11): the app-icon images keep the artwork's content box (alpha 128 and more)
                // round(size / 16) pixels from each edge (ADR 0025, decisions 6 and 21), so the image
                // starts that much further left, in whole device pixels. The margin lies left of the
                // column, where the Flickable clips it (at most a faint rim of the artwork reaches into it).
                // Decorative: the title beside it names the dialog.
                Image {
                    objectName: "aboutLogo"
                    source: Window.window ? "image://branding/app-icon" : ""
                    sourceSize: Qt.size(64, 64)
                    fillMode: Image.PreserveAspectFit
                    Layout.preferredWidth: 64
                    Layout.preferredHeight: 64
                    Layout.leftMargin: -Math.round(64 * about.devicePixelRatio / 16) / about.devicePixelRatio
                    Layout.alignment: Qt.AlignVCenter
                    Accessible.ignored: true
                }

                ColumnLayout {
                    spacing: 4
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter

                    Text {
                        text: qsTr("About Lucent")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontTitle
                        font.weight: Font.DemiBold
                        Accessible.role: Accessible.Heading
                        Accessible.name: text
                    }

                    Text {
                        objectName: "aboutVersion"
                        text: qsTr("Version %1").arg(about.controller.appVersion)
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontBody
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                }
            }

            Text {
                text: qsTr("Camera support database")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                Layout.topMargin: 6
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }

            Text {
                objectName: "aboutCameraSupport"
                text: about.support.summary
                color: about.support.available ? Theme.textSecondary : Theme.unsupported
                font.pixelSize: Theme.fontBody
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            Text {
                visible: about.support.available
                text: qsTr("Qualified with %1. Installed with Lucent in %2.")
                      .arg(about.support.decoder).arg(about.support.location)
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                Layout.fillWidth: true
            }

            Repeater {
                model: about.support.notes

                delegate: Text {
                    required property string modelData

                    text: modelData
                    color: Theme.attention
                    font.pixelSize: Theme.fontSmall
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    Layout.fillWidth: true
                }
            }

            Text {
                text: qsTr("Lens database")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                Layout.topMargin: 6
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }

            Text {
                objectName: "aboutLensDatabase"
                text: about.lenses.summary
                color: about.lenses.available || about.lenses.loading ? Theme.textSecondary : Theme.unsupported
                font.pixelSize: Theme.fontBody
                wrapMode: Text.Wrap
                Layout.fillWidth: true
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            Text {
                visible: about.lenses.available
                text: qsTr("Lens profiles from the Lensfun project, licensed CC BY-SA 3.0. Installed with Lucent in %1.")
                      .arg(about.lenses.location)
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                Layout.fillWidth: true
            }

            Text {
                text: qsTr("What the support states mean")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
                Layout.topMargin: 6
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }

            Repeater {
                objectName: "aboutSupportStates"
                model: about.controller.supportStates

                delegate: RowLayout {
                    required property var modelData

                    spacing: 10
                    Layout.fillWidth: true

                    Badge {
                        text: modelData.label
                        dotColor: Theme.supportColor(modelData.key)
                        color: Theme.surfaceRaised
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 170
                    }

                    Text {
                        text: modelData.meaning
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        Accessible.role: Accessible.StaticText
                        Accessible.name: modelData.label + ": " + text
                    }
                }
            }

            UpdatePanel {
                updates: about.controller.updates
                Layout.fillWidth: true
            }

            StyledButton {
                objectName: "aboutCloseButton"
                text: qsTr("Close")
                Layout.alignment: Qt.AlignRight
                Layout.topMargin: 6
                onClicked: about.close()
            }
        }
    }
}
