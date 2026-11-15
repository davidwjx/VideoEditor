import QtQuick
import QtQuick.Controls

// One video/audio clip. Moving updates the local x and trimming updates the
// local x/width for live feedback while the mouse is down; the controller is
// called exactly once on release, producing one undo step per gesture.
Rectangle {
    id: clipItem
    objectName: "clip-" + itemId
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
    property bool trimming: false

    x: startUs / 1e6 * timeline.pixelsPerSecond
    width: Math.max(8, durationUs / 1e6 * timeline.pixelsPerSecond)
    function restoreGeometry() {
        clipItem.x = Qt.binding(function() {
            return clipItem.startUs / 1e6 * clipItem.timeline.pixelsPerSecond
        })
        clipItem.width = Qt.binding(function() {
            return Math.max(8, clipItem.durationUs / 1e6
                              * clipItem.timeline.pixelsPerSecond)
        })
        clipItem.dragging = false
        clipItem.trimming = false
    }
    radius: 3
    color: selected ? "#3d6ea8" : (trackType === "audio" ? "#2b5d3f" : "#3a3a55")
    border.color: selected ? "#8fc4ff" : "#111111"
    border.width: 1
    clip: true
    z: selected ? 2 : 1
    opacity: (dragging || trimming) ? 0.85 : 1.0

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
        // The parent Flickable must not steal the gesture once the drag starts
        // (long, zoomed-in timelines overflow the viewport).
        preventStealing: true
        property real pressSceneX: 0
        property real originalX: 0

        onPressed: (mouse) => {
            // The delegate moves under the pointer, so mouse.x is relative to a
            // moving item. Map the local point back to the scene to keep the
            // drag delta stable.
            pressSceneX = clipItem.mapToItem(null, mouse.x, mouse.y).x
            originalX = clipItem.x
            clipItem.dragging = true
            Timeline.selectClip(clipItem.itemId)
        }
        onPositionChanged: (mouse) => {
            if (pressed) {
                const sceneX = clipItem.mapToItem(null, mouse.x, mouse.y).x
                clipItem.x = Math.max(0, originalX + sceneX - pressSceneX)
            }
        }
        onReleased: (mouse) => {
            const proposed = Math.round(
                clipItem.x / Math.max(1, timeline.pixelsPerSecond) * 1e6)
            Timeline.moveClip(clipItem.itemId, Timeline.snapUs(proposed))
            clipItem.restoreGeometry()
        }
        onCanceled: clipItem.restoreGeometry()
    }

    // Trim handles: live local geometry while dragging, one command on release.
    Rectangle {
        objectName: "trim-start-" + clipItem.itemId
        width: 8
        height: parent.height
        color: "#8fc4ff"
        opacity: selected ? 0.8 : 0.35

        MouseArea {
            id: trimStartArea
            anchors.fill: parent
            cursorShape: Qt.SizeHorCursor
            preventStealing: true
            property real pressSceneX: 0
            property real originalX: 0
            property real originalWidth: 0

            onPressed: (mouse) => {
                pressSceneX = trimStartArea.mapToItem(null, mouse.x, mouse.y).x
                originalX = clipItem.x
                originalWidth = clipItem.width
                clipItem.trimming = true
                Timeline.selectClip(clipItem.itemId)
            }
            onPositionChanged: (mouse) => {
                if (!pressed || originalWidth <= 0)
                    return
                const sceneX = trimStartArea.mapToItem(null, mouse.x, mouse.y).x
                const newX = Math.max(0, Math.min(
                    originalX + originalWidth - 8,
                    originalX + sceneX - pressSceneX))
                clipItem.x = newX
                clipItem.width = originalX + originalWidth - newX
            }
            onReleased: (mouse) => {
                const proposed = Math.round(
                    clipItem.x / Math.max(1, timeline.pixelsPerSecond) * 1e6)
                Timeline.trimStart(clipItem.itemId, Timeline.snapUs(proposed))
                clipItem.restoreGeometry()
            }
            onCanceled: clipItem.restoreGeometry()
        }
    }

    Rectangle {
        objectName: "trim-end-" + clipItem.itemId
        anchors.right: parent.right
        width: 8
        height: parent.height
        color: "#8fc4ff"
        opacity: selected ? 0.8 : 0.35

        MouseArea {
            id: trimEndArea
            anchors.fill: parent
            cursorShape: Qt.SizeHorCursor
            preventStealing: true
            property real pressSceneX: 0
            property real originalWidth: 0

            onPressed: (mouse) => {
                pressSceneX = trimEndArea.mapToItem(null, mouse.x, mouse.y).x
                originalWidth = clipItem.width
                clipItem.trimming = true
                Timeline.selectClip(clipItem.itemId)
            }
            onPositionChanged: (mouse) => {
                if (!pressed || originalWidth <= 0)
                    return
                const sceneX = trimEndArea.mapToItem(null, mouse.x, mouse.y).x
                clipItem.width = Math.max(
                    8, originalWidth + sceneX - pressSceneX)
            }
            onReleased: (mouse) => {
                const proposedEnd = clipItem.startUs + Math.round(
                    clipItem.width / Math.max(1, timeline.pixelsPerSecond) * 1e6)
                Timeline.trimEnd(clipItem.itemId, Timeline.snapUs(proposedEnd))
                clipItem.restoreGeometry()
            }
            onCanceled: clipItem.restoreGeometry()
        }
    }
}
