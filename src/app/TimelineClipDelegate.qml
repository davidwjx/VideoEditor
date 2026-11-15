import QtQuick
import QtQuick.Controls

// One video/audio clip. Moving and trimming mutate the local x/geometry while
// the mouse is down; the controller is called once on release, producing one
// undo step per gesture.
Rectangle {
    id: clipItem
    property var timeline
    property string trackId
    property string itemId
    property string assetId
    property string name
    property real startUs
    property real durationUs
    property int speedMilli
    property bool linked
    property string trackType
    property bool selected: false

    property string waveformUrl: "image://waveform/" + assetId + "/"
                                 + Math.max(1, Math.round(width))
    property bool dragging: false

    x: startUs / 1e6 * timeline.pixelsPerSecond
    width: Math.max(8, durationUs / 1e6 * timeline.pixelsPerSecond)
    radius: 3
    color: selected ? "#3d6ea8" : (trackType === "audio" ? "#2b5d3f" : "#3a3a55")
    border.color: selected ? "#8fc4ff" : "#111111"
    border.width: 1
    clip: true
    z: selected ? 2 : 1
    opacity: dragging ? 0.85 : 1.0

    // Waveform is generated on a worker thread. If the peaks arrive after the
    // delegate was created, re-issue the image request once they are ready.
    Image {
        id: wave
        visible: clipItem.trackType === "audio"
        anchors.fill: parent
        anchors.topMargin: 14
        asynchronous: true
        cache: false
        fillMode: Image.Stretch
        source: visible ? clipItem.waveformUrl : ""

        Connections {
            target: App.waveforms
            function onReady(readyAssetId) {
                if (readyAssetId !== clipItem.assetId)
                    return
                wave.source = ""
                Qt.callLater(function() {
                    wave.source = Qt.binding(function() {
                        return clipItem.trackType === "audio"
                                ? clipItem.waveformUrl : ""
                    })
                })
            }
        }
    }

    Text {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 4
        width: parent.width - 20
        elide: Text.ElideRight
        text: (speedMilli !== 1000 ? (speedMilli / 1000).toFixed(2) + "x · " : "")
              + (linked ? "链接 · " : "")
              + (clipItem.name ? clipItem.name : "")
        color: "#e8e8e8"
        font.pixelSize: 11
    }

    // Drag body. No controller call until release.
    MouseArea {
        id: body
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        property real pressSceneX: 0
        property real originalX: 0

        onPressed: (mouse) => {
            // The delegate moves under the pointer, so mouse.x is relative to a
            // moving item. Scene coordinates keep the delta stable.
            pressSceneX = mouse.sceneX
            originalX = clipItem.x
            clipItem.dragging = true
            Timeline.selectClip(clipItem.itemId)
        }
        onPositionChanged: (mouse) => {
            if (pressed)
                clipItem.x = Math.max(0, originalX + mouse.sceneX - pressSceneX)
        }
        onReleased: (mouse) => {
            clipItem.dragging = false
            const proposed = Math.round(
                clipItem.x / Math.max(1, timeline.pixelsPerSecond) * 1e6)
            Timeline.moveClip(clipItem.itemId, Timeline.snapUs(proposed))
            clipItem.x = Qt.binding(function() {
                return clipItem.startUs / 1e6 * timeline.pixelsPerSecond
            })
        }
        onCanceled: {
            clipItem.dragging = false
            clipItem.x = Qt.binding(function() {
                return clipItem.startUs / 1e6 * timeline.pixelsPerSecond
            })
        }
    }

    // Trim handles: commit the new edge on release only.
    Rectangle {
        width: 8
        height: parent.height
        color: "#8fc4ff"
        opacity: selected ? 0.8 : 0.35

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.SizeHorCursor
            property real pressX: 0
            onPressed: (mouse) => {
                pressX = mouse.x
                Timeline.selectClip(clipItem.itemId)
            }
            onReleased: (mouse) => {
                const deltaUs = Math.round(
                    (mouse.x - pressX)
                    / Math.max(1, timeline.pixelsPerSecond) * 1e6)
                Timeline.trimStart(clipItem.itemId,
                                   Timeline.snapUs(clipItem.startUs + deltaUs))
            }
        }
    }

    Rectangle {
        anchors.right: parent.right
        width: 8
        height: parent.height
        color: "#8fc4ff"
        opacity: selected ? 0.8 : 0.35

        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.SizeHorCursor
            property real pressX: 0
            onPressed: (mouse) => {
                pressX = mouse.x
                Timeline.selectClip(clipItem.itemId)
            }
            onReleased: (mouse) => {
                const deltaUs = Math.round(
                    (mouse.x - pressX)
                    / Math.max(1, timeline.pixelsPerSecond) * 1e6)
                Timeline.trimEnd(clipItem.itemId,
                                 Timeline.snapUs(clipItem.startUs
                                                 + clipItem.durationUs + deltaUs))
            }
        }
    }
}
