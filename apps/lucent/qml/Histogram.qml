import QtQuick
import QtQuick.Layouts
import Lucent

// The Edit view's histogram (blueprint section 2): red, green and blue code-value histograms of
// the preview Lucent rendered for the photo, with its luma outline, and the share of clipped
// shadows and highlights. Empty until Lucent has rendered the photo.
ColumnLayout {
    id: root

    required property CurrentItem item
    // True while the preview is being rendered.
    property bool rendering: false

    spacing: 8

    Text {
        text: qsTr("Histogram")
        color: Theme.textPrimary
        font.pixelSize: Theme.fontTitle
        font.weight: Font.DemiBold
        Accessible.role: Accessible.Heading
        Accessible.name: text
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: 112
        radius: Theme.radius
        color: Theme.imageBackground
        border.width: 1
        border.color: Theme.divider

        Accessible.role: Accessible.Graphic
        Accessible.name: plot.empty ? placeholder.text
                                    : qsTr("Histogram of the preview. Shadows clipped %1 %, highlights clipped %2 %.")
                                          .arg(root.percent(plot.shadowsClipped)).arg(root.percent(plot.highlightsClipped))

        Repeater {
            model: 3

            delegate: Rectangle {
                required property int index

                x: Math.round((index + 1) * parent.width / 4)
                y: 1
                width: 1
                height: parent.height - 2
                color: Theme.divider
            }
        }

        HistogramPlot {
            id: plot

            objectName: "histogramPlot"
            anchors.fill: parent
            anchors.margins: 2
            histogram: root.item.renderInfo
        }

        Text {
            id: placeholder

            anchors.centerIn: parent
            width: parent.width - 24
            visible: plot.empty
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: !root.item.valid ? qsTr("No photo selected")
                : root.rendering ? qsTr("Rendering…")
                : qsTr("Shown once Lucent has rendered the photo")
            color: Theme.textTertiary
            font.pixelSize: Theme.fontSmall
        }
    }

    Text {
        objectName: "clippingSummary"
        visible: !plot.empty
        text: qsTr("Shadows clipped %1 % · Highlights clipped %2 %")
            .arg(root.percent(plot.shadowsClipped)).arg(root.percent(plot.highlightsClipped))
        color: Theme.textTertiary
        font.pixelSize: Theme.fontSmall
    }

    function percent(fraction) {
        return fraction > 0 && fraction < 0.001 ? "<0.1" : (fraction * 100).toFixed(1)
    }
}
