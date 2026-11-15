import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Per-frame tracking confidence strip. Segments are colored by state and a
// click seeks to the clicked frame; low-confidence runs get a one-click jump.
Item {
    id: root
    objectName: "confidence-band-root"

    // The context property only exists when the app (or a test host) installs
    // it; guard so loading Main.qml standalone still works.
    property var removal: (typeof Removal !== "undefined") ? Removal : null
    readonly property var band: removal !== null ? removal.confidenceBand : []
    readonly property int lowCount: removal !== null
                                    ? removal.lowConfidenceCount : 0
    readonly property real spanStart: band.length > 0 ? band[0].timelineUs : 0
    readonly property real spanEnd: band.length > 1
                                    ? band[band.length - 1].timelineUs
                                    : spanStart + 1

    implicitHeight: bar.height
                    + (lowJump.visible ? lowJump.implicitHeight + 4 : 0)
    Layout.fillWidth: true

    function segmentStart(i) {
        if (i <= 0)
            return 0
        return (band[i - 1].timelineUs - spanStart)
                / Math.max(1, spanEnd - spanStart) * bar.width
    }

    Rectangle {
        id: bar
        objectName: "confidence-band"
        width: root.width
        height: 6
        radius: 3
        color: "#333333"
        clip: true
        visible: root.band.length > 0

        Repeater {
            model: root.band
            delegate: Rectangle {
                required property int index
                required property var modelData
                x: root.segmentStart(index)
                width: index === root.band.length - 1
                       ? Math.max(1, bar.width - x)
                       : Math.max(1,
                                  (modelData.timelineUs - root.spanStart)
                                  / Math.max(1, root.spanEnd - root.spanStart)
                                  * bar.width - x)
                height: bar.height
                color: modelData.color
            }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: (mouse) => {
                if (root.removal === null || root.band.length === 0)
                    return
                const idx = Math.max(0, Math.min(
                    root.band.length - 1,
                    Math.floor(mouse.x / Math.max(1, width)
                               * root.band.length)))
                root.removal.jumpToConfidence(idx)
            }
        }
    }

    Button {
        id: lowJump
        objectName: "confidence-low-jump"
        anchors.top: bar.bottom
        anchors.topMargin: 4
        visible: root.lowCount > 0
        text: "第 " + root.lowCount + " 段低置信，跳到该帧"
        font.pixelSize: 10
        padding: 2
        onClicked: {
            if (root.removal === null)
                return
            for (let i = 0; i < root.band.length; ++i) {
                const state = root.band[i].state
                if (state === "low" || state === "missing") {
                    root.removal.jumpToConfidence(i)
                    return
                }
            }
        }
    }
}
