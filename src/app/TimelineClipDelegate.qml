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
    property real sourceInUs
    property real sourceOutUs
    property int speedMilli
    property bool linked
    property string trackType
    property bool selected: false

    // Range-aware waveform: trimmed clips show only their source interval.
    property string waveformUrl: "image://waveform/" + assetId + "/"
                                 + Math.max(1, Math.round(width)) + "/"
                                 + Math.round(sourceInUs) + "/"
                                 + Math.round(sourceOutUs)
    property bool dragging: false
    property bool trimming: false

    // Mask keyframes are read lazily from the removal controller; the document
    // signal refreshes them after strokes, undo/redo and layer deletion.
    property var removal: (typeof Removal !== "undefined") ? Removal : null
    property var maskKeyframes: removal !== null
                                ? removal.maskKeyframesForClip(itemId) : []

    Connections {
        target: clipItem.removal
        function onLayerChanged() {
            clipItem.maskKeyframes = clipItem.removal !== null
                    ? clipItem.removal.maskKeyframesForClip(clipItem.itemId)
                    : []
        }
    }

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
    border.width: selected ? 2 : 1
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
            Timeline.moveClip(clipItem.itemId,
                             Timeline.frameQuantizeUs(Timeline.snapUs(proposed)))
            clipItem.restoreGeometry()
        }
        onCanceled: clipItem.restoreGeometry()
    }

    // Right-click context menu: every professional clip action stays one
    // gesture away without occupying the primary bar.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: {
            Timeline.selectClip(clipItem.itemId)
            timeline.openClipMenu(clipItem.itemId, clipItem.linked)
        }
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
                Timeline.trimStart(clipItem.itemId,
                                Timeline.frameQuantizeUs(Timeline.snapUs(proposed)))
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
                Timeline.trimEnd(clipItem.itemId,
                              Timeline.frameQuantizeUs(Timeline.snapUs(proposedEnd)))
                clipItem.restoreGeometry()
            }
            onCanceled: clipItem.restoreGeometry()
        }
    }

    // Mask keyframe diamonds: one per keyframe of the clip's removal layer,
    // positioned by source time inside the trimmed range. A click seeks there.
    Repeater {
        model: clipItem.maskKeyframes
        delegate: Rectangle {
            required property int index
            required property var modelData

            objectName: "mask-keyframe-" + clipItem.itemId + "-" + index
            width: 6
            height: 6
            radius: 0
            rotation: 45
            color: "#ffb347"
            border.color: "#2a1a00"
            border.width: 1
            y: clipItem.height - height - 3
            x: {
                const span = Math.max(1, clipItem.sourceOutUs
                                      - clipItem.sourceInUs)
                const fraction = (modelData.timeUs - clipItem.sourceInUs) / span
                return Math.max(0, Math.min(clipItem.width - width,
                                            fraction * clipItem.width))
            }
            z: 3

            MouseArea {
                anchors.fill: parent
                anchors.margins: -3
                onClicked: {
                    const speed = Math.max(0.001, clipItem.speedMilli / 1000)
                    App.seekTimeline(Math.round(
                        clipItem.startUs
                        + (modelData.timeUs - clipItem.sourceInUs) / speed))
                    Timeline.selectClip(clipItem.itemId)
                }
            }
        }
    }
}
