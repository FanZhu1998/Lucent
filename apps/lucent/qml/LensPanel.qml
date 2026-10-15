pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// Lens corrections (blueprint section 2, "lens corrections"; section 5 step 4; ADR 0023;
// LensCorrections, lens_corrections.hpp): the camera and lens the file records, the lens database's
// match with its quality (exact, similar profiles only, none) and the reasons, the profile in use,
// "Automatic profile" (the exact match only; never a guessed profile) or a profile chosen from the
// database, and separate switches for distortion, chromatic aberration and vignetting with what each
// did. A correction the file already makes itself (DNG opcodes) is never applied twice: its switch is
// off and disabled, with a note. Every change is one revision.
PanelSection {
    id: panel

    required property EditSession edit

    readonly property LensCorrections lens: edit.lens
    readonly property bool active: lens.available && !lens.legacy

    title: qsTr("Lens corrections")

    actions: [
        Badge {
            objectName: "lensMatchBadge"
            visible: panel.active && panel.lens.known
            text: panel.lens.matchLabel
            dotColor: panel.lens.matchKind === "exact" ? Theme.verified
                    : panel.lens.matchKind === "fuzzy" ? Theme.limited : Theme.unverified
            color: Theme.surfaceRaised
            detail: panel.lens.matchSummary
        }
    ]

    // One correction: its switch and what the newest render did with it.
    component CorrectionRow: RowLayout {
        id: row

        required property int index
        readonly property var entry: panel.lens.corrections[index]

        spacing: 6
        visible: panel.active
        Layout.fillWidth: true
        Layout.leftMargin: 22

        StyledCheckBox {
            id: toggle

            objectName: "lensCorrection_" + row.entry.key
            text: row.entry.label
            checked: row.entry.checked
            enabled: row.entry.editable
            onToggled: {
                panel.lens.setCorrection(row.entry.key, checked)
                checked = Qt.binding(function() { return row.entry.checked })  // the settings decide
            }
            // What the render did and why (the engine's words), for keyboard and screen-reader users
            // too: the status text's tooltip is shown while the switch has keyboard focus.
            Accessible.description: row.entry.detail.length > 0 ? row.entry.status + ". " + row.entry.detail
                                                                : row.entry.status
        }

        Item {
            Layout.fillWidth: true
        }

        Text {
            objectName: "lensStatus_" + row.entry.key
            text: row.entry.status
            color: row.entry.status === qsTr("Applied") ? Theme.verified : Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignRight
            Layout.maximumWidth: 150

            HoverHandler {
                id: statusHover
            }

            ToolTip.visible: (statusHover.hovered || toggle.visualFocus) && row.entry.detail.length > 0
            ToolTip.text: row.entry.detail
            ToolTip.delay: 600
        }
    }

    // No lens database: nothing can be corrected, and why (or, W12, that it is still loading).
    Text {
        objectName: "lensUnavailable"
        visible: !panel.lens.available
        text: panel.lens.unavailableReason
        color: panel.lens.loading ? Theme.textTertiary : Theme.attention
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.leftMargin: 26
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    // The camera and lens as the file records them.
    ColumnLayout {
        visible: panel.lens.available && panel.lens.known
        spacing: 1
        Layout.fillWidth: true
        Layout.leftMargin: 26

        Text {
            objectName: "lensCamera"
            text: panel.lens.cameraText
            visible: text.length > 0
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        Text {
            objectName: "lensLens"
            text: panel.lens.lensText
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }

    // What the profile choice means for this photo, in one sentence.
    Text {
        objectName: "lensSummary"
        visible: panel.lens.available
        text: panel.lens.matchSummary
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.leftMargin: 26
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    // A recipe made before Lucent had lens corrections.
    StyledButton {
        objectName: "lensTurnOn"
        visible: panel.lens.available && panel.lens.legacy
        text: qsTr("Turn on lens corrections")
        enabled: panel.lens.editable
        onClicked: panel.lens.turnOn()
        Layout.leftMargin: 26
        ToolTip.visible: hovered
        ToolTip.text: qsTr("Correct this photo with its lens profile, as new photos are")
        ToolTip.delay: 800
    }

    RowLayout {
        visible: panel.active
        spacing: 6
        Layout.fillWidth: true
        Layout.leftMargin: 26

        Text {
            text: qsTr("Profile")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
        }

        Text {
            objectName: "lensProfile"
            text: panel.lens.profileText.length > 0 ? panel.lens.profileText : qsTr("None")
            color: panel.lens.profileText.length > 0 ? Theme.textPrimary : Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }

    RowLayout {
        visible: panel.active
        spacing: 6
        Layout.fillWidth: true
        Layout.leftMargin: 22

        StyledCheckBox {
            id: automaticToggle

            objectName: "lensAutomatic"
            text: qsTr("Automatic profile")
            checked: panel.lens.automatic && !panel.lens.picking
            enabled: panel.lens.editable
            onToggled: {
                panel.lens.setAutomatic(checked)
                checked = Qt.binding(function() { return panel.lens.automatic && !panel.lens.picking })
            }
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Use the lens database's profile for exactly this camera and lens, when it has one")
            ToolTip.delay: 800
        }

        Item {
            Layout.fillWidth: true
        }

        StyledToolButton {
            objectName: "lensChooseProfile"
            visible: !panel.lens.picking
            text: qsTr("Choose…")
            enabled: panel.lens.editable
            implicitHeight: 26
            onClicked: panel.lens.openPicker()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Choose a profile from the lens database")
            ToolTip.delay: 800
        }
    }

    // The manual profile choice: suggestions (the match's similar profiles) or a search of the
    // database by maker and model. Esc closes it from any of its controls (Main.qml's Esc, which
    // leaves the Edit view, is off while it is open); it opens with the search field focused.
    ColumnLayout {
        id: picker

        objectName: "lensPicker"
        visible: panel.active && panel.lens.picking
        spacing: 4
        Layout.fillWidth: true
        Layout.leftMargin: 26

        onVisibleChanged: {
            if (visible) {
                search.forceActiveFocus()
            }
        }

        // An open tool's Esc (EditView) comes first: two enabled Esc shortcuts would both be ignored.
        Shortcut {
            sequence: "Esc"
            enabled: picker.visible && !panel.edit.toolActive
            onActivated: panel.lens.closePicker()
        }

        TextField {
            id: search

            objectName: "lensSearch"
            placeholderText: qsTr("Search by maker or model")
            text: panel.lens.searchText
            font.pixelSize: Theme.fontSmall
            color: Theme.textPrimary
            selectByMouse: true
            Layout.fillWidth: true
            onTextEdited: panel.lens.searchText = text
            Accessible.name: qsTr("Search lens profiles")
            // Esc closes the choice rather than leaving the Edit view.
            Keys.onShortcutOverride: event => { event.accepted = event.key === Qt.Key_Escape }
            Keys.onEscapePressed: panel.lens.closePicker()
        }

        Text {
            objectName: "lensPickerHint"
            visible: panel.lens.profiles.count === 0
            text: panel.lens.searchText.length > 0 ? qsTr("No profile matches.")
                                                   : qsTr("Type a maker or model, for example \"Sigma 50mm\".")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        ListView {
            id: profileList

            objectName: "lensProfileList"
            visible: count > 0
            model: panel.lens.profiles
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            implicitHeight: Math.min(contentHeight, 220)
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight

            ScrollBar.vertical: ScrollBar {
                policy: ScrollBar.AsNeeded
            }

            delegate: AbstractButton {
                id: choice

                required property int index
                required property string label
                required property string detail
                required property bool compatible
                required property bool current

                // The state the row's colour and weight show, in words.
                readonly property string stateText: (current ? qsTr("In use") + ". " : "")
                    + (compatible ? qsTr("Fits this camera's mount") : qsTr("For another mount")) + ". "

                width: ListView.view.width
                implicitHeight: choiceColumn.implicitHeight + 8
                focusPolicy: Qt.StrongFocus
                onClicked: panel.lens.chooseProfile(index)
                Accessible.role: Accessible.Button
                Accessible.name: label
                Accessible.description: stateText + detail
                // The whole detail (the line below the name is elided at the panel's width).
                ToolTip.visible: (hovered || visualFocus) && detail.length > 0
                ToolTip.text: stateText + detail
                ToolTip.delay: 600

                background: Rectangle {
                    color: choice.hovered || choice.visualFocus ? Theme.surfaceHover
                         : choice.current ? Theme.accentMuted : "transparent"
                    radius: Theme.radius
                }

                contentItem: ColumnLayout {
                    id: choiceColumn

                    spacing: 1

                    Text {
                        text: choice.label
                        color: choice.compatible ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: Theme.fontSmall
                        font.weight: choice.current ? Font.DemiBold : Font.Normal
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.leftMargin: 4
                    }

                    Text {
                        text: choice.detail
                        visible: text.length > 0
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontSmall - 1
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                        Layout.leftMargin: 4
                    }
                }
            }
        }

        StyledToolButton {
            objectName: "lensPickerCancel"
            text: qsTr("Cancel")
            implicitHeight: 24
            Layout.alignment: Qt.AlignRight
            onClicked: panel.lens.closePicker()
        }
    }

    Text {
        visible: panel.active
        text: qsTr("Corrections")
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        font.weight: Font.Medium
        Layout.leftMargin: 26
        Layout.topMargin: 2
    }

    CorrectionRow {
        index: 0
    }

    CorrectionRow {
        index: 1
    }

    CorrectionRow {
        index: 2
    }

    // What the file corrects itself (those switches stay off).
    Text {
        objectName: "lensCarriedNote"
        visible: panel.active && text.length > 0
        text: panel.lens.carriedNote
        color: Theme.attention
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.leftMargin: 26
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    // Why the database matched as it did (the engine's words), on demand.
    StyledToolButton {
        id: whyToggle

        objectName: "lensWhy"
        visible: panel.lens.available && panel.lens.known && panel.lens.reasons.length > 0
        text: checked ? qsTr("Hide details") : qsTr("Why this profile?")
        checkable: true
        implicitHeight: 24
        Layout.leftMargin: 22
    }

    Text {
        objectName: "lensReasons"
        visible: whyToggle.visible && whyToggle.checked
        text: panel.lens.reasons.join("\n")
        color: Theme.textTertiary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
        Layout.leftMargin: 26
    }
}
