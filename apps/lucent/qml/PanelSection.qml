import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Lucent

// A titled, collapsible section of the Edit view's side panel. Items placed in it form its body;
// `actions` go to the right of the title.
ColumnLayout {
    id: section

    property string title
    property bool expanded: true
    default property alias content: body.data
    property alias actions: actionRow.data

    spacing: 8

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        AbstractButton {
            id: toggle

            objectName: "sectionToggle"
            Layout.fillWidth: true
            implicitHeight: 26
            focusPolicy: Qt.StrongFocus
            onClicked: section.expanded = !section.expanded

            Accessible.role: Accessible.Button
            Accessible.name: section.title
            Accessible.description: section.expanded ? qsTr("Expanded") : qsTr("Collapsed")

            contentItem: Row {
                spacing: 6

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: section.expanded ? "▾" : "▸"
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontSmall
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: section.title
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                }
            }

            background: FocusRing {
                shown: toggle.visualFocus
            }
        }

        RowLayout {
            id: actionRow

            spacing: 4
        }
    }

    ColumnLayout {
        id: body

        visible: section.expanded
        Layout.fillWidth: true
        spacing: 4
    }
}
