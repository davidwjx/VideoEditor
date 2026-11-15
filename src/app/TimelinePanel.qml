import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Scrollable multi-track timeline: fixed header column, ruler, playhead and
// one delegate per clip/cue. Drags only change the delegate's local x; the
// controller receives exactly one command when the mouse is released.
Item {
    id: root
    property real trackHeight: 44
    property real rulerHeight: 28
    property real headerWidth: 160
    property real pixelsPerSecond: Timeline.pixelsPerSecond
    property real contentWidth: Math.max(width - headerWidth,
                                         Timeline.durationUs / 1e6 * pixelsPerSecond + 400)
    // Adaptive ruler: keep at least ~64 px between ticks instead of creating
    // one Repeater item per second (a long timeline at low zoom would create
    // thousands of items).
    property real rulerStepSeconds: {
        const steps = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800]
        for (let i = 0; i < steps.length; ++i) {
            if (steps[i] * pixelsPerSecond >= 64)
                return steps[i]
        }
        return 3600
    }
    property int rulerTickCount: Math.ceil(contentWidth
                                           / Math.max(1, pixelsPerSecond)
                                           / rulerStepSeconds) + 1

    function seekTo(timeUs) {
        App.seekTimeline(Math.max(0, Math.round(timeUs)))
    }

    function rulerLabel(seconds) {
        if (seconds < 60)
            return seconds + "s"
        const total = Math.round(seconds)
        const h = Math.floor(total / 3600)
        const m = Math.floor((total % 3600) / 60)
        const s = total % 60
        return h > 0 ? h + ":" + String(m).padStart(2, "0") + ":"
                       + String(s).padStart(2, "0")
                     : m + ":" + String(s).padStart(2, "0")
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Track headers stay fixed horizontally but follow vertical scrolling
        // so they remain aligned with their lanes.
        Item {
            id: headerArea
            Layout.preferredWidth: root.headerWidth
            Layout.fillHeight: true
            clip: true

            Column {
                id: headerColumn
                width: headerArea.width
                y: -flick.contentY

                Item { width: 1; height: root.rulerHeight }
                Repeater {
                    model: Timeline.trackModel
                    delegate: TrackHeaderDelegate {
                        width: root.headerWidth
                        height: root.trackHeight
                        trackId: model.trackId
                        name: model.name
                        typeLabel: model.typeLabel
                        trackType: model.type
                        muted: model.muted
                        solo: model.solo
                        locked: model.locked
                    }
                }
            }
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: root.contentWidth
            // Height follows the real track count so extra tracks stay
            // reachable; 44 px keeps the M2 six-track project visible.
            contentHeight: Math.max(flick.height, headerColumn.implicitHeight)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.horizontal: ScrollBar {}
            ScrollBar.vertical: ScrollBar {}

            Item {
                width: flick.contentWidth
                height: flick.contentHeight

                // Track lanes: one band per row so clips have visible rails.
                Repeater {
                    model: Timeline.trackModel
                    delegate: Rectangle {
                        x: 0
                        y: root.rulerHeight + index * root.trackHeight
                        width: parent.width
                        height: root.trackHeight
                        color: model.type === "subtitle"
                               ? (index % 2 === 0 ? "#242018" : "#292418")
                               : (index % 2 === 0 ? "#1d1d1d" : "#222222")
                        border.color: "#101010"
                    }
                }

                // Ruler with one tick and label per second.
                Rectangle {
                    id: ruler
                    width: parent.width
                    height: root.rulerHeight
                    color: "#202020"

                    Repeater {
                        objectName: "ruler-ticks"
                        model: root.rulerTickCount
                        Item {
                            x: index * root.rulerStepSeconds * root.pixelsPerSecond
                            width: root.rulerStepSeconds * root.pixelsPerSecond
                            height: ruler.height
                            Rectangle {
                                width: 1
                                height: 7
                                color: "#666666"
                            }
                            Text {
                                x: 3
                                y: 8
                                text: root.rulerLabel(index * root.rulerStepSeconds)
                                color: "#8a8a8a"
                                font.pixelSize: 10
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        onClicked: (mouse) => root.seekTo(
                            mouse.x / Math.max(1, root.pixelsPerSecond) * 1e6)
                    }
                }

                // Clips and cues share one model; each Repeater filters the
                // row type through its delegate's visibility.
                Repeater {
                    model: Timeline.timelineModel
                    delegate: TimelineClipDelegate {
                        visible: model.kind === "clip"
                        trackId: model.trackId
                        itemId: model.itemId
                        assetId: model.assetId
                        name: model.name
                        startUs: model.startUs
                        durationUs: model.durationUs
                        sourceInUs: model.sourceInUs
                        sourceOutUs: model.sourceOutUs
                        speedMilli: model.speedMilli
                        linked: model.linked
                        trackType: model.trackType
                        selected: model.selected
                        y: root.rulerHeight + model.trackIndex * root.trackHeight
                        height: root.trackHeight - 4
                        timeline: root
                    }
                }

                Repeater {
                    model: Timeline.timelineModel
                    delegate: TimelineCueDelegate {
                        visible: model.kind === "cue"
                        trackId: model.trackId
                        itemId: model.itemId
                        text: model.text
                        startUs: model.startUs
                        durationUs: model.durationUs
                        selected: model.selected
                        y: root.rulerHeight + model.trackIndex * root.trackHeight
                        height: 36
                        timeline: root
                    }
                }

                // Playhead is drawn last so it stays above clips.
                Rectangle {
                    id: playhead
                    x: Math.round(Timeline.positionUs / 1e6 * root.pixelsPerSecond)
                    y: 0
                    width: 2
                    height: parent.height
                    color: "#ff5b5b"
                    z: 20

                    Rectangle {
                        x: -4
                        width: 10
                        height: 8
                        radius: 2
                        color: "#ff5b5b"
                    }
                }
            }
        }
    }
}
