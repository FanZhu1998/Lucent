import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Help > About (W10, ADR 0018): Lucent's version, the camera support database the session uses
// (its version and qualification date, or why none is in use) and what the four support states
// mean (blueprint section 3, "User-visible compatibility states"). W12 (ADR 0022): "Check for
// updates" (UpdatePanel). W13 (ADR 0023): the lens database the lens corrections use, with its
// licence (CC BY-SA 3.0, the Lensfun project's data).
Popup {
    id: about

    required property AppController controller

    readonly property var support: controller.cameraSupport
    readonly property var lenses: controller.lensDatabase

    objectName: "aboutDialog"
    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
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
                color: about.lenses.available ? Theme.textSecondary : Theme.unsupported
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
