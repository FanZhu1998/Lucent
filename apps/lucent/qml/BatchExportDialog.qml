pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import Lucent

// Export batch (blueprint section 2, "Automatic batch development" steps 1 to 4; section 7):
// the photos (the Library's selection, every listed photo, or a folder), the output folder, the
// preset (JPEG / sRGB / quality 95, or 16-bit TIFF with a profile), the naming pattern with its
// tokens, mirrored subfolders, what happens when a name is taken (a number is added by default;
// replacing existing files needs an explicit confirmation and never applies to originals), photos
// already exported, the metadata (capture details, artist and copyright, GPS off unless chosen) and
// the automatic settings (W8: Off, Auto or Analyse only, and Keep related shots consistent).
// Example destination paths, counts, free space, warnings and what the analysis adds come from the
// service's plan and follow every change.
Dialog {
    id: dialog

    required property AppController controller
    readonly property BatchSetupModel setup: controller.batchSetup

    objectName: "batchExportDialog"
    title: qsTr("Export batch")
    modal: true
    focus: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(820, Overlay.overlay ? Overlay.overlay.width - 48 : 820)
    height: Math.min(implicitHeight, Overlay.overlay ? Overlay.overlay.height - 32 : 800)
    padding: 0
    topPadding: 8
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: Theme.surface
        radius: Theme.radius
        border.width: 1
        border.color: Theme.border
    }

    header: Text {
        text: dialog.title
        color: Theme.textPrimary
        font.pixelSize: Theme.fontTitle
        font.weight: Font.DemiBold
        leftPadding: 20
        topPadding: 18
        Accessible.role: Accessible.Heading
        Accessible.name: text
    }

    Connections {
        target: dialog.controller

        function onBatchExportRequested() {
            dialog.open()
        }
    }

    onOpened: {
        setup.active = true
        startButton.forceActiveFocus(Qt.OtherFocusReason)
    }
    onClosed: setup.active = false

    component SectionLabel: Text {
        color: Theme.textTertiary
        font.pixelSize: Theme.fontBody
        Layout.alignment: Qt.AlignTop
        Layout.topMargin: 6
        Layout.preferredWidth: 130
    }

    component Note: Text {
        color: Theme.textSecondary
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }

    component Choice: RadioButton {
        id: choice

        font.pixelSize: Theme.fontBody
        contentItem: Text {
            leftPadding: choice.indicator.width + choice.spacing
            text: choice.text
            font: choice.font
            color: choice.enabled ? Theme.textPrimary : Theme.textTertiary
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    // The settings scroll; the plan (example paths, counts, space, warnings) stays in view below them.
    contentItem: ColumnLayout {
        spacing: 8

        ScrollView {
            id: scroller

            objectName: "batchDialogScroll"
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            // Visible whenever the settings do not fit, so that the ones below are found.
            ScrollBar.vertical.policy: contentHeight > height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: grid.implicitHeight
            Layout.minimumHeight: 120

            GridLayout {
                id: grid

                width: scroller.availableWidth - 40
                x: 20
                columns: 2
                columnSpacing: 16
                rowSpacing: 12

                // --- Photos -------------------------------------------------------------------------
                SectionLabel {
                    text: qsTr("Photos")
                }

                ColumnLayout {
                    spacing: 6
                    Layout.fillWidth: true

                    RowLayout {
                        spacing: 4

                        ButtonGroup {
                            id: scopeGroup
                        }

                        StyledToolButton {
                            objectName: "batchScopeSelection"
                            text: qsTr("Selected (%1)").arg(dialog.setup.selectionCount)
                            enabled: dialog.setup.selectionCount > 0
                            checkable: true
                            checked: dialog.setup.scope === BatchSetupModel.Selection
                            ButtonGroup.group: scopeGroup
                            onClicked: dialog.setup.scope = BatchSetupModel.Selection
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("The photos selected in the library (Ctrl+click, Shift+click, Ctrl+A)")
                            ToolTip.delay: 800
                        }

                        StyledToolButton {
                            objectName: "batchScopeAll"
                            text: qsTr("All photos (%1)").arg(dialog.setup.allCount)
                            enabled: dialog.setup.allCount > 0
                            checkable: true
                            checked: dialog.setup.scope === BatchSetupModel.AllPhotos
                            ButtonGroup.group: scopeGroup
                            onClicked: dialog.setup.scope = BatchSetupModel.AllPhotos
                        }

                        StyledToolButton {
                            objectName: "batchScopeFolder"
                            text: qsTr("A folder…")
                            checkable: true
                            checked: dialog.setup.scope === BatchSetupModel.Folder
                            ButtonGroup.group: scopeGroup
                            onClicked: {
                                dialog.setup.scope = BatchSetupModel.Folder
                                if (dialog.setup.inputFolder.toString().length === 0)
                                    inputPicker.open()
                            }
                        }
                    }

                    RowLayout {
                        visible: dialog.setup.scope === BatchSetupModel.Folder
                        spacing: 8
                        Layout.fillWidth: true

                        Text {
                            objectName: "batchInputFolder"
                            text: dialog.setup.inputFolder.toString().length > 0 ? dialog.setup.inputFolderText()
                                                                                 : qsTr("No folder chosen")
                            color: dialog.setup.inputFolder.toString().length > 0 ? Theme.textPrimary : Theme.textTertiary
                            font.pixelSize: Theme.fontBody
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }

                        StyledButton {
                            text: qsTr("Choose…")
                            onClicked: inputPicker.open()
                        }
                    }

                    StyledCheckBox {
                        visible: dialog.setup.scope === BatchSetupModel.Folder
                        text: qsTr("Include subfolders")
                        checked: dialog.setup.includeSubfolders
                        onToggled: dialog.setup.includeSubfolders = checked
                    }
                }

                // --- Automatic settings (W8) --------------------------------------------------------
                SectionLabel {
                    text: qsTr("Automatic settings")
                }

                ColumnLayout {
                    objectName: "batchAutomaticSettings"
                    spacing: 6
                    Layout.fillWidth: true

                    RowLayout {
                        spacing: 4

                        ButtonGroup {
                            id: automaticGroup
                        }

                        StyledToolButton {
                            objectName: "batchAutomaticOff"
                            text: qsTr("Off")
                            checkable: true
                            checked: dialog.setup.automatic === BatchSetupModel.AutomaticOff
                            ButtonGroup.group: automaticGroup
                            onClicked: dialog.setup.automatic = BatchSetupModel.AutomaticOff
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Export each photo with its own current edit")
                            ToolTip.delay: 800
                        }

                        StyledToolButton {
                            objectName: "batchAutomaticAuto"
                            text: qsTr("Auto")
                            checkable: true
                            checked: dialog.setup.automatic === BatchSetupModel.AutomaticAuto
                            ButtonGroup.group: automaticGroup
                            onClicked: dialog.setup.automatic = BatchSetupModel.AutomaticAuto
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Lucent chooses each photo's settings (Natural), checks them and exports")
                            ToolTip.delay: 800
                        }

                        StyledToolButton {
                            objectName: "batchAutomaticAnalyze"
                            text: qsTr("Analyse only")
                            checkable: true
                            checked: dialog.setup.automatic === BatchSetupModel.AutomaticAnalyzeOnly
                            ButtonGroup.group: automaticGroup
                            onClicked: dialog.setup.automatic = BatchSetupModel.AutomaticAnalyzeOnly
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Lucent saves each photo's automatic settings to review in Edit; nothing is exported")
                            ToolTip.delay: 800
                        }
                    }

                    Note {
                        objectName: "batchAutomaticNote"
                        text: dialog.setup.automaticNote
                        color: Theme.textTertiary
                    }

                    StyledCheckBox {
                        objectName: "batchKeepConsistent"
                        text: qsTr("Keep related shots consistent")
                        enabled: dialog.setup.automatic !== BatchSetupModel.AutomaticOff
                        checked: dialog.setup.keepConsistent && enabled
                        onToggled: dialog.setup.keepConsistent = checked
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Photos taken together in similar light get a similar look; each is still checked on its own")
                        ToolTip.delay: 800
                    }
                }

                // --- Destination --------------------------------------------------------------------
                SectionLabel {
                    text: qsTr("Output folder")
                }

                RowLayout {
                    spacing: 8
                    Layout.fillWidth: true

                    Text {
                        objectName: "batchOutputFolder"
                        text: dialog.setup.outputFolder.toString().length > 0 ? dialog.setup.outputFolderText()
                                                                              : qsTr("No folder chosen")
                        color: dialog.setup.outputFolder.toString().length > 0 ? Theme.textPrimary : Theme.textTertiary
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }

                    StyledButton {
                        objectName: "batchChooseOutput"
                        text: qsTr("Choose…")
                        onClicked: outputPicker.open()
                    }
                }

                SectionLabel {
                    text: qsTr("File names")
                }

                ColumnLayout {
                    spacing: 6
                    Layout.fillWidth: true

                    TextField {
                        id: patternField

                        objectName: "batchNamePattern"
                        text: dialog.setup.namePattern
                        font.pixelSize: Theme.fontBody
                        color: Theme.textPrimary
                        selectByMouse: true
                        Layout.fillWidth: true
                        onTextEdited: dialog.setup.namePattern = text
                        Accessible.name: qsTr("Name pattern")

                        function insertToken(token) {
                            const at = cursorPosition
                            insert(at, token)
                            cursorPosition = at + token.length
                            dialog.setup.namePattern = text
                            forceActiveFocus()
                        }
                    }

                    Flow {
                        spacing: 4
                        Layout.fillWidth: true

                        Repeater {
                            model: [
                                { token: "{original-stem}", label: qsTr("+ Original name") },
                                { token: "{revision}", label: qsTr("+ Edit number") },
                                { token: "{sequence}", label: qsTr("+ Sequence") },
                                { token: "{image}", label: qsTr("+ Image of file") }
                            ]

                            delegate: StyledToolButton {
                                required property var modelData

                                text: modelData.label
                                onClicked: patternField.insertToken(modelData.token)
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Insert %1").arg(modelData.token)
                                ToolTip.delay: 600
                            }
                        }

                        StyledToolButton {
                            text: qsTr("Default")
                            onClicked: dialog.setup.resetNamePattern()
                        }
                    }

                    Text {
                        objectName: "batchPatternProblem"
                        visible: dialog.setup.patternProblem.length > 0
                        text: dialog.setup.patternProblem
                        color: Theme.unsupported
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    StyledCheckBox {
                        objectName: "batchMirror"
                        text: qsTr("Mirror the photos' subfolders in the output folder")
                        checked: dialog.setup.mirrorSubfolders
                        onToggled: dialog.setup.mirrorSubfolders = checked
                    }
                }

                // --- Format -------------------------------------------------------------------------
                SectionLabel {
                    text: qsTr("Format")
                }

                ColumnLayout {
                    spacing: 6
                    Layout.fillWidth: true

                    RowLayout {
                        spacing: 4

                        ButtonGroup {
                            id: presetGroup
                        }

                        StyledToolButton {
                            objectName: "batchPresetJpeg"
                            text: qsTr("JPEG · sRGB · quality 95")
                            checkable: true
                            checked: dialog.setup.preset === BatchSetupModel.Jpeg
                            ButtonGroup.group: presetGroup
                            onClicked: dialog.setup.preset = BatchSetupModel.Jpeg
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("For sharing and the web")
                            ToolTip.delay: 800
                        }

                        StyledToolButton {
                            objectName: "batchPresetTiff"
                            text: qsTr("16-bit TIFF")
                            checkable: true
                            checked: dialog.setup.preset === BatchSetupModel.Tiff16
                            ButtonGroup.group: presetGroup
                            onClicked: dialog.setup.preset = BatchSetupModel.Tiff16
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("For further editing in other programs")
                            ToolTip.delay: 800
                        }
                    }

                    RowLayout {
                        visible: dialog.setup.preset === BatchSetupModel.Tiff16
                        spacing: 4

                        Text {
                            text: qsTr("Profile")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontBody
                            Layout.rightMargin: 6
                        }

                        ButtonGroup {
                            id: profileGroup
                        }

                        StyledToolButton {
                            objectName: "batchProfileSrgb"
                            text: qsTr("sRGB")
                            checkable: true
                            checked: !dialog.setup.displayP3
                            ButtonGroup.group: profileGroup
                            onClicked: dialog.setup.displayP3 = false
                        }

                        StyledToolButton {
                            objectName: "batchProfileP3"
                            text: qsTr("Display P3")
                            checkable: true
                            checked: dialog.setup.displayP3
                            ButtonGroup.group: profileGroup
                            onClicked: dialog.setup.displayP3 = true
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("A wider gamut for wide-gamut displays and further editing")
                            ToolTip.delay: 800
                        }
                    }
                }

                // --- Existing files -----------------------------------------------------------------
                SectionLabel {
                    text: qsTr("If a name is taken")
                }

                ColumnLayout {
                    spacing: 2
                    Layout.fillWidth: true

                    // Not an exclusive group: the model decides (replacing needs the confirmation first).
                    Choice {
                        objectName: "batchUniqueNames"
                        text: qsTr("Add a number to the new file's name (recommended)")
                        autoExclusive: false
                        checked: !dialog.setup.overwrite
                        Layout.fillWidth: true
                        onClicked: {
                            dialog.setup.useUniqueNames()
                            checked = Qt.binding(() => !dialog.setup.overwrite)
                        }
                    }

                    Choice {
                        objectName: "batchOverwrite"
                        text: qsTr("Replace the existing file…")
                        autoExclusive: false
                        checked: dialog.setup.overwrite
                        Layout.fillWidth: true
                        onClicked: {
                            checked = Qt.binding(() => dialog.setup.overwrite)
                            if (!dialog.setup.overwrite)
                                overwriteConfirm.open()
                        }
                    }

                    Note {
                        visible: dialog.setup.overwrite
                        text: qsTr("Existing files in the output folder with an output's name will be replaced. Original photos and this batch's own outputs are never replaced.")
                        color: Theme.attention
                    }
                }

                SectionLabel {
                    text: qsTr("Already exported")
                }

                ColumnLayout {
                    spacing: 2
                    Layout.fillWidth: true

                    ButtonGroup {
                        id: existingGroup
                    }

                    // An automatic batch makes new revisions: their exports are new by definition.
                    Choice {
                        objectName: "batchSkipExisting"
                        text: qsTr("Skip photos whose identical export is already in the folder")
                        enabled: dialog.setup.automatic === BatchSetupModel.AutomaticOff
                        checked: dialog.setup.skipExisting
                        ButtonGroup.group: existingGroup
                        onClicked: dialog.setup.skipExisting = true
                        Layout.fillWidth: true
                    }

                    Choice {
                        objectName: "batchExportAgain"
                        text: qsTr("Export them again")
                        enabled: dialog.setup.automatic === BatchSetupModel.AutomaticOff
                        checked: !dialog.setup.skipExisting
                        ButtonGroup.group: existingGroup
                        onClicked: dialog.setup.skipExisting = false
                        Layout.fillWidth: true
                    }

                    Note {
                        visible: dialog.setup.automatic !== BatchSetupModel.AutomaticOff
                        text: qsTr("With automatic settings every photo gets a new revision, so nothing counts as already exported.")
                        color: Theme.textTertiary
                    }
                }

                // --- Metadata -----------------------------------------------------------------------
                SectionLabel {
                    text: qsTr("Metadata")
                }

                ColumnLayout {
                    spacing: 4
                    Layout.fillWidth: true

                    StyledCheckBox {
                        objectName: "batchIncludeCapture"
                        text: qsTr("Capture details (camera, lens, exposure, date and time)")
                        checked: dialog.setup.includeCapture
                        onToggled: dialog.setup.includeCapture = checked
                    }

                    StyledCheckBox {
                        objectName: "batchIncludeRights"
                        text: qsTr("Artist and copyright")
                        checked: dialog.setup.includeRights
                        onToggled: dialog.setup.includeRights = checked
                    }

                    GridLayout {
                        enabled: dialog.setup.includeRights
                        opacity: enabled ? 1 : 0.55
                        columns: 2
                        columnSpacing: 8
                        rowSpacing: 4
                        Layout.fillWidth: true
                        Layout.leftMargin: 24

                        Text {
                            text: qsTr("Artist")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontBody
                        }

                        TextField {
                            objectName: "batchArtist"
                            text: dialog.setup.artist
                            placeholderText: qsTr("Empty: keep what the camera recorded")
                            font.pixelSize: Theme.fontBody
                            color: Theme.textPrimary
                            selectByMouse: true
                            Layout.fillWidth: true
                            onTextEdited: dialog.setup.artist = text
                            Accessible.name: qsTr("Artist")
                        }

                        Text {
                            text: qsTr("Copyright")
                            color: Theme.textSecondary
                            font.pixelSize: Theme.fontBody
                        }

                        TextField {
                            objectName: "batchCopyright"
                            text: dialog.setup.copyright
                            placeholderText: qsTr("Empty: keep what the camera recorded")
                            font.pixelSize: Theme.fontBody
                            color: Theme.textPrimary
                            selectByMouse: true
                            Layout.fillWidth: true
                            onTextEdited: dialog.setup.copyright = text
                            Accessible.name: qsTr("Copyright")
                        }
                    }

                    StyledCheckBox {
                        objectName: "batchIncludeGps"
                        text: qsTr("Include GPS location")
                        checked: dialog.setup.includeGps
                        onToggled: dialog.setup.includeGps = checked
                    }

                    Note {
                        leftPadding: 24
                        text: dialog.setup.includeGps
                              ? qsTr("Where each photo was taken is written into the exported files. Anyone you share them with can read it.")
                              : qsTr("Off: locations are left out of the exported files.")
                        color: dialog.setup.includeGps ? Theme.attention : Theme.textTertiary
                    }
                }
            }
        }

        // --- Before you start: the plan -----------------------------------------------------
        Rectangle {
            objectName: "batchPlanPanel"
            color: Theme.surfaceRaised
            radius: Theme.radius
            implicitHeight: planColumn.implicitHeight + 24
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20

            ColumnLayout {
                id: planColumn

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 12
                spacing: 6

                RowLayout {
                    spacing: 8
                    Layout.fillWidth: true

                    Text {
                        text: dialog.setup.analyzeOnly ? qsTr("Before you start") : qsTr("Example files")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.Medium
                        Accessible.role: Accessible.Heading
                        Accessible.name: text
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    Text {
                        objectName: "batchPlanning"
                        visible: dialog.setup.planning
                        text: qsTr("Checking…")
                        color: Theme.textTertiary
                        font.pixelSize: Theme.fontSmall
                    }
                }

                Repeater {
                    objectName: "batchExamples"
                    model: dialog.setup.examples

                    delegate: ColumnLayout {
                        required property var modelData

                        spacing: 0
                        Layout.fillWidth: true

                        Text {
                            text: modelData.destination
                            // An output already in the folder is not written again.
                            color: modelData.skipped ? Theme.textSecondary : Theme.textPrimary
                            font.pixelSize: Theme.fontBody
                            font.family: "Consolas"
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }

                        Text {
                            text: modelData.note.length > 0 ? qsTr("from %1 · %2").arg(modelData.source).arg(modelData.note)
                                                            : qsTr("from %1").arg(modelData.source)
                            color: modelData.note.length > 0 ? Theme.attention : Theme.textTertiary
                            font.pixelSize: Theme.fontSmall
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                        }
                    }
                }

                Note {
                    objectName: "batchPlanCounts"
                    visible: text.length > 0
                    text: dialog.setup.countsText
                    color: Theme.textPrimary
                }

                Note {
                    objectName: "batchPlanSpace"
                    visible: text.length > 0
                    text: dialog.setup.spaceText
                }

                // What the automatic settings add: their cost, what is (not) written.
                Repeater {
                    objectName: "batchAnalysisNotes"
                    model: dialog.setup.analysisNotes

                    delegate: Note {
                        required property string modelData

                        text: modelData
                        color: Theme.textPrimary
                    }
                }

                Repeater {
                    model: dialog.setup.warnings

                    delegate: Note {
                        required property string modelData

                        text: modelData
                        color: Theme.attention
                    }
                }

                Repeater {
                    objectName: "batchExclusions"
                    model: dialog.setup.exclusions.slice(0, 4)

                    delegate: Note {
                        required property var modelData

                        text: qsTr("Left out: %1 — %2").arg(modelData.source).arg(modelData.reason)
                        color: Theme.textSecondary
                    }
                }

                Note {
                    visible: dialog.setup.excludedCount > 4
                    text: qsTr("and %1 more left out").arg(dialog.setup.excludedCount - 4)
                }

                Note {
                    objectName: "batchPlanError"
                    visible: !dialog.setup.planning && dialog.setup.startHint.length > 0
                    text: dialog.setup.startHint
                    color: Theme.unsupported
                }
            }
        }
    }

    footer: Item {
        implicitHeight: buttons.implicitHeight + 32

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 1
            color: Theme.divider
        }

        RowLayout {
            id: buttons

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 20
            anchors.rightMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            Text {
                text: qsTr("Originals are only read. Every file is verified before it appears in the folder.")
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
                Layout.fillWidth: true
            }

            StyledButton {
                objectName: "batchCancel"
                text: qsTr("Cancel")
                onClicked: dialog.reject()
            }

            StyledButton {
                id: startButton

                objectName: "batchStart"
                text: dialog.setup.analyzeOnly ? qsTr("Start analysis") : qsTr("Start export")
                highlighted: true
                enabled: dialog.setup.canStart
                onClicked: {
                    if (dialog.controller.startBatch())
                        dialog.close()
                }
            }
        }
    }

    FolderDialog {
        id: outputPicker

        title: qsTr("Choose the output folder")
        currentFolder: dialog.setup.outputFolder
        onAccepted: dialog.setup.outputFolder = selectedFolder
    }

    FolderDialog {
        id: inputPicker

        title: qsTr("Choose the folder of photos to export")
        currentFolder: dialog.setup.inputFolder
        onAccepted: {
            dialog.setup.inputFolder = selectedFolder
            dialog.setup.scope = BatchSetupModel.Folder
        }
    }

    // Replacing files is never the default and is confirmed for the chosen folder each time.
    Dialog {
        id: overwriteConfirm

        objectName: "batchOverwriteConfirm"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(520, parent ? parent.width - 64 : 520)
        modal: true
        title: qsTr("Replace existing files?")
        padding: 20
        closePolicy: Popup.CloseOnEscape

        background: Rectangle {
            color: Theme.surfaceRaised
            radius: Theme.radius
            border.width: 1
            border.color: Theme.attention
        }

        header: Text {
            text: overwriteConfirm.title
            color: Theme.textPrimary
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
            leftPadding: 20
            topPadding: 18
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }

        onOpened: keepButton.forceActiveFocus(Qt.OtherFocusReason)

        contentItem: Text {
            text: qsTr("Files in %1 that have the same name as an exported photo will be permanently replaced by the new export.\n\nYour original photos are never replaced, and neither are other files of this batch: those get a number added instead.")
                .arg(dialog.setup.outputFolder.toString().length > 0 ? dialog.setup.outputFolderText() : qsTr("the output folder"))
            color: Theme.textPrimary
            font.pixelSize: Theme.fontBody
            wrapMode: Text.Wrap
        }

        footer: Item {
            implicitHeight: confirmButtons.implicitHeight + 32

            RowLayout {
                id: confirmButtons

                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                StyledButton {
                    id: keepButton

                    objectName: "batchOverwriteKeep"
                    text: qsTr("Keep existing files")
                    highlighted: true
                    onClicked: overwriteConfirm.reject()
                }

                StyledButton {
                    objectName: "batchOverwriteReplace"
                    text: qsTr("Replace existing files")
                    onClicked: {
                        dialog.setup.confirmOverwrite()
                        overwriteConfirm.accept()
                    }
                }
            }
        }
    }
}
