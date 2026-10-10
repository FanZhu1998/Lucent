import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Help > About: "Check for updates" (W12, ADR 0022; blueprint section 8: "a signed full installer and a
// graphical Check for updates flow"). Lucent contacts its update server only when the photographer
// clicks Check for updates; a build without an update channel says so and offers nothing. A release
// is downloaded and verified first; Install closes Lucent and starts Setup, and only between jobs.
ColumnLayout {
    id: panel

    required property UpdateModel updates

    objectName: "updatePanel"
    spacing: 8

    Text {
        text: qsTr("Updates")
        color: Theme.textPrimary
        font.pixelSize: Theme.fontBody
        font.weight: Font.DemiBold
        Layout.topMargin: 6
        Accessible.role: Accessible.Heading
        Accessible.name: text
    }

    Text {
        objectName: "updateChannel"
        text: panel.updates.channelText
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    Text {
        objectName: "updateStatus"
        visible: text.length > 0
        text: panel.updates.statusText
        color: panel.updates.state === UpdateModel.Failed ? Theme.attention : Theme.textPrimary
        font.pixelSize: Theme.fontBody
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    ProgressBar {
        objectName: "updateProgress"
        visible: panel.updates.working
        from: 0
        to: 1
        indeterminate: panel.updates.progress < 0
        value: Math.max(0, panel.updates.progress)
        Layout.fillWidth: true
        Accessible.name: panel.updates.statusText
    }

    // The release notes are plain text from the signed release manifest (never markup).
    ScrollView {
        objectName: "updateNotes"
        visible: panel.updates.releaseNotes.length > 0 && panel.updates.state !== UpdateModel.UpToDate
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(notes.implicitHeight + 12, 140)
        clip: true

        background: Rectangle {
            color: Theme.surfaceRaised
            radius: Theme.radius
        }

        TextArea {
            id: notes

            text: panel.updates.releaseNotes
            textFormat: TextEdit.PlainText
            readOnly: true
            wrapMode: TextEdit.Wrap
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            background: null
            Accessible.name: qsTr("Release notes of Lucent %1").arg(panel.updates.version)
        }
    }

    RowLayout {
        visible: panel.updates.configured
        spacing: 8
        Layout.fillWidth: true

        StyledButton {
            objectName: "updateCheckButton"
            visible: panel.updates.canCheck || panel.updates.state === UpdateModel.Checking
            text: qsTr("Check for updates")
            enabled: panel.updates.canCheck
            highlighted: panel.updates.canCheck
            onClicked: panel.updates.check()
            Accessible.description: qsTr("Contacts the update server now")
        }

        StyledButton {
            objectName: "updateDownloadButton"
            visible: panel.updates.canDownload
            text: qsTr("Download Lucent %1").arg(panel.updates.version)
            highlighted: true
            onClicked: panel.updates.download()
        }

        StyledButton {
            objectName: "updateInstallButton"
            visible: panel.updates.state === UpdateModel.Ready || panel.updates.state === UpdateModel.Installing
            text: qsTr("Install and restart")
            enabled: panel.updates.canInstall
            highlighted: panel.updates.canInstall
            onClicked: panel.updates.install()
            Accessible.description: panel.updates.installBlocker.length > 0
                                    ? panel.updates.installBlocker
                                    : qsTr("Closes Lucent, backs up your library and starts Setup")
        }

        StyledButton {
            objectName: "updateCancelButton"
            visible: panel.updates.canCancel
            text: qsTr("Cancel")
            onClicked: panel.updates.cancel()
        }

        Item {
            Layout.fillWidth: true
        }
    }
}
