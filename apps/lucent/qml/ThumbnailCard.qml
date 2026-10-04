import QtQuick
import QtQuick.Controls
import Lucent

// One contact-sheet cell: thumbnail, processing status (once Lucent has started on the photo),
// support status, a "Camera preview" label while the thumbnail is the camera's embedded JPEG
// (blueprint section 2), an "Edited" badge for photos with develop settings (W6: their thumbnail
// is Lucent's render of the recipe), a "Review" badge for automatic results Lucent recommends checking
// (W8), badges for the file (Missing, Changed, Duplicate, Removable) and the pick,
// file name, camera, capture time and flags. W7: photos selected for a batch (Ctrl+click toggles,
// Shift+click selects a range) are outlined, with a check mark while several are selected. The
// properties marked required are LibraryModel roles.
Item {
    id: card

    required property int index
    required property string fileName
    required property string cameraLabel
    required property string captureTimeLabel
    required property string supportKey
    required property string supportLabel
    required property string supportShortLabel
    required property string supportDetail
    required property string processingKey
    required property string processingLabel
    required property string processingDetail
    required property var flagLabels
    required property string thumbnailSource
    required property bool thumbnailIsCameraPreview
    required property string accessibleLabel
    required property var flagBadges
    required property string pickKey
    required property string pickLabel
    required property bool missing
    required property bool edited
    required property bool inSelection
    required property bool review

    property bool selected: false  // the current photo
    property bool showFocus: false
    property bool multipleSelected: false

    signal clicked()
    signal activated()
    signal toggleRequested()  // Ctrl+click
    signal rangeRequested()   // Shift+click

    Accessible.role: Accessible.ListItem
    Accessible.name: accessibleLabel
    // The batch selection; the current photo alone is not "selected" for assistive technology.
    Accessible.selected: inSelection

    HoverHandler {
        id: hover
    }

    TapHandler {
        acceptedModifiers: Qt.NoModifier
        onTapped: card.clicked()
        onDoubleTapped: card.activated()
    }

    TapHandler {
        acceptedModifiers: Qt.ControlModifier
        onTapped: card.toggleRequested()
    }

    TapHandler {
        acceptedModifiers: Qt.ShiftModifier
        onTapped: card.rangeRequested()
    }

    Rectangle {
        id: frame

        anchors.fill: parent
        anchors.margins: 6
        radius: Theme.radius
        // Selected photos are filled and outlined in the accent; the current photo outside the
        // selection (after Ctrl+click or Ctrl+Arrow) only gets a thin neutral outline.
        color: card.inSelection ? Theme.surfaceRaised : (hover.hovered ? Theme.surfaceHover : Theme.surface)
        border.width: card.inSelection ? 2 : (card.selected ? 1 : 0)
        border.color: card.inSelection ? Theme.accent : Theme.indicatorBorder

        Rectangle {
            id: well

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 8
            height: Math.round(width * 2 / 3)
            color: Theme.imageBackground
            clip: true

            Image {
                id: thumbnail

                anchors.fill: parent
                source: card.thumbnailSource
                // A fixed request keeps resizing the window from reloading every thumbnail.
                sourceSize: Qt.size(512, 512)
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                smooth: true
                mipmap: true
                // A missing file keeps its cached thumbnail, dimmed.
                opacity: card.missing ? 0.45 : 1
                Accessible.ignored: true
            }

            // The file's state and the pick, most urgent first (blueprint section 2: a removable
            // card stays visibly identified; missing files stay listed; duplicates are flagged).
            Row {
                objectName: "sourceBadges"
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 6
                spacing: 4
                layoutDirection: Qt.RightToLeft

                Repeater {
                    model: card.flagBadges

                    delegate: Badge {
                        required property var modelData

                        objectName: "badge_" + modelData.key
                        text: modelData.label
                        dotColor: Theme.sourceColor(modelData.key)
                        detail: modelData.detail
                    }
                }

                Badge {
                    objectName: "editedBadge"
                    visible: card.edited
                    text: qsTr("Edited")
                    dotColor: Theme.accent
                    detail: qsTr("This photo has develop settings; the thumbnail shows them.")
                }

                // W8: an automatic result Lucent recommends checking (low confidence, a fallback, locks).
                Badge {
                    objectName: "reviewBadge"
                    visible: card.review
                    text: qsTr("Review")
                    dotColor: Theme.attention
                    detail: qsTr("Lucent recommends checking this photo's automatic settings. Open it in Edit to see why.")
                }

                Badge {
                    objectName: "pickBadge"
                    visible: card.pickKey !== "none"
                    text: card.pickLabel
                    dotColor: card.pickKey === "picked" ? Theme.verified : Theme.textTertiary
                }
            }

            Text {
                anchors.centerIn: parent
                visible: thumbnail.status !== Image.Ready
                text: thumbnail.status === Image.Error ? qsTr("No preview")
                    : card.thumbnailSource.length > 0 ? qsTr("Loading…") : qsTr("No preview")
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
            }

            Badge {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 6
                text: card.supportShortLabel
                dotColor: Theme.supportColor(card.supportKey)
                detail: card.supportDetail.length > 0 ? card.supportLabel + " — " + card.supportDetail : card.supportLabel
            }

            // Lucent's own processing (blueprint section 2: the contact sheet shows processing
            // status); nothing while the photo has not been processed.
            Badge {
                objectName: "processingBadge"
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: 6
                visible: card.processingKey !== "notprocessed"
                // Compact text; the tooltip has the full label and the reason.
                text: card.processingKey === "failed" ? qsTr("Failed")
                    : card.processingKey === "incomplete" ? qsTr("Incomplete") : card.processingLabel
                dotColor: Theme.processingColor(card.processingKey)
                detail: card.processingDetail.length > 0 ? card.processingLabel + " — " + card.processingDetail
                                                         : card.processingLabel
            }

            Rectangle {
                objectName: "cameraPreviewLabel"
                visible: card.thumbnailIsCameraPreview && thumbnail.status === Image.Ready
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 6
                implicitWidth: previewText.implicitWidth + 12
                implicitHeight: 20
                radius: 3
                color: Theme.scrim

                Text {
                    id: previewText
                    anchors.centerIn: parent
                    text: qsTr("Camera preview")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSmall
                }

                HoverHandler {
                    id: previewHover
                }

                ToolTip.visible: previewHover.hovered
                ToolTip.text: qsTr("The camera's embedded JPEG, shown until Lucent's own rendering replaces it.")
                ToolTip.delay: 500
            }
        }

        Column {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: well.bottom
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            anchors.topMargin: 8
            spacing: 2

            Row {
                width: parent.width
                spacing: 6

                Rectangle {
                    objectName: "selectionMark"
                    visible: card.inSelection && card.multipleSelected
                    width: 16
                    height: 16
                    radius: 8
                    color: Theme.accentStrong
                    anchors.verticalCenter: parent.verticalCenter

                    Text {
                        anchors.centerIn: parent
                        text: "✓"
                        color: Theme.textOnAccent
                        font.pixelSize: 11
                        font.bold: true
                    }
                }

                Text {
                    width: parent.width - (card.inSelection && card.multipleSelected ? 22 : 0)
                    text: card.fileName
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.Medium
                    elide: Text.ElideMiddle
                }
            }

            Text {
                width: parent.width
                text: card.cameraLabel
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }

            Text {
                width: parent.width
                text: card.captureTimeLabel
                color: Theme.textTertiary
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }

            Text {
                width: parent.width
                text: card.flagLabels.join(" · ")
                color: Theme.attention
                font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }
        }

        FocusRing {
            shown: card.showFocus
        }
    }
}
