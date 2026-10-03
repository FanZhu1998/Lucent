import QtQuick
import QtQuick.Controls
import Lucent

// One contact-sheet cell: thumbnail, processing status (once Lucent has started on the photo),
// support status, a "Camera preview" label while the thumbnail is the camera's embedded JPEG
// (blueprint section 2), badges for the file (Missing, Changed, Duplicate, Removable) and the pick,
// file name, camera, capture time and flags. The properties marked required are LibraryModel
// roles.
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

    property bool selected: false
    property bool showFocus: false

    signal clicked()
    signal activated()

    Accessible.role: Accessible.ListItem
    Accessible.name: accessibleLabel
    Accessible.selected: selected

    HoverHandler {
        id: hover
    }

    TapHandler {
        onTapped: card.clicked()
        onDoubleTapped: card.activated()
    }

    Rectangle {
        id: frame

        anchors.fill: parent
        anchors.margins: 6
        radius: Theme.radius
        color: card.selected ? Theme.surfaceRaised : (hover.hovered ? Theme.surfaceHover : Theme.surface)
        border.width: card.selected ? 2 : 0
        border.color: Theme.accent

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

            Text {
                width: parent.width
                text: card.fileName
                color: Theme.textPrimary
                font.pixelSize: Theme.fontBody
                font.weight: Font.Medium
                elide: Text.ElideMiddle
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
