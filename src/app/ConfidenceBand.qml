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
                                      + Math.max(1,
                                                 band[band.length - 1].timelineUs
                                                 - band[band.length - 2].timelineUs)
                                    : spanStart + 1

    implicitHeight: bar.height
                    + (lowJump.visible ? lowJump.implicitHeight + 4 : 0)
    Layout.fillWidth: true

    // Segment i spans [band[i].timelineUs, band[i+1].timelineUs); the last
    // segment runs to the bar end.
    function segmentStart(i) {
        if (i <= 0 || band.length <= 1)
            return 0
        return (band[i].timelineUs - spanStart)
                / Math.max(1, spanEnd - spanStart) * bar.width
    }

    function segmentEnd(i) {
        if (i >= band.length - 1)
            return bar.width
        return segmentStart(i + 1)
    }

    // Converts a click x to the segment containing that time. The mapping is
    // time-based, not a uniform index, so non-uniform keyframe spacing works.
    function indexAtX(x) {
        if (band.length === 0)
            return -1
        const clamped = Math.max(0, Math.min(bar.width, x))
        const t = spanStart + (clamped / Math.max(1, bar.width))
                               * Math.max(1, spanEnd - spanStart)
        let index = 0
        for (let i = 0; i < band.length; ++i) {
            if (t >= band[i].timelineUs)
                index = i
            else
                break
        }
        return index
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
                objectName: "confidence-segment-" + index
                x: root.segmentStart(index)
                width: Math.max(1,
                                root.segmentEnd(index)
                                - root.segmentStart(index))
                height: bar.height
                color: modelData.color
            }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: (mouse) => {
                if (root.removal === null || root.band.length === 0)
                    return
                const idx = root.indexAtX(mouse.x)
                if (idx >= 0)
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
