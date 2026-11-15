import QtQuick
import QtQuick.Controls

// One subtitle cue on the subtitle track. Single click selects; double click
// opens an inline editor; dragging re-times the cue with one undo step per
// gesture (snap and frame quantize match clip dragging).
Rectangle {
    id: cueItem
    objectName: "cue-" + itemId
    property var timeline
    property string trackId
    property string itemId
    property string text
    property real startUs
    property real durationUs
    property bool selected: false

    property bool dragging: false

    function restoreGeometry() {
        cueItem.x = Qt.binding(function() {
            return cueItem.startUs / 1e6 * cueItem.timeline.pixelsPerSecond
        })
        cueItem.dragging = false
    }

    function openEditor() {
        editor.text = cueItem.text
        editor.visible = true
        editor.forceActiveFocus()
        editor.selectAll()
    }

    radius: 3
    color: selected ? "#6b5a2b" : "#3d3520"
    border.color: selected ? "#d8b86a" : "#111111"
    border.width: selected ? 2 : 1
    x: startUs / 1e6 * timeline.pixelsPerSecond
    width: Math.max(8, durationUs / 1e6 * timeline.pixelsPerSecond)
    clip: true
    z: selected ? 2 : 1
    opacity: dragging ? 0.85 : 1.0

    Text {
        anchors.fill: parent
        anchors.margins: 4
        text: cueItem.text
        color: "#f0e6c8"
        elide: Text.ElideRight
        font.pixelSize: 12
    }

    MouseArea {
        id: body
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        preventStealing: true
        property real pressSceneX: 0
        property real pressItemX: 0
        property bool moved: false

        onPressed: (mouse) => {
            pressSceneX = cueItem.mapToItem(null, mouse.x, mouse.y).x
            pressItemX = cueItem.x
            moved = false
            Timeline.selectCue(cueItem.itemId)
        }
        onPositionChanged: (mouse) => {
            if (!pressed)
                return
            const sceneX = cueItem.mapToItem(null, mouse.x, mouse.y).x
            const delta = sceneX - pressSceneX
            if (Math.abs(delta) > 3)
                moved = true
            if (moved) {
                cueItem.dragging = true
                cueItem.x = Math.max(0, pressItemX + delta)
            }
        }
        onReleased: (mouse) => {
            if (moved) {
                const proposed = Math.round(
                    cueItem.x / Math.max(1, timeline.pixelsPerSecond) * 1e6)
                const start = Timeline.frameQuantizeUs(Timeline.snapUs(proposed))
                Timeline.setCueTiming(cueItem.itemId, start,
                                      start + cueItem.durationUs)
            }
            cueItem.restoreGeometry()
        }
        onCanceled: cueItem.restoreGeometry()
        onClicked: Timeline.selectCue(cueItem.itemId)
        onDoubleClicked: cueItem.openEditor()
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: {
            Timeline.selectCue(cueItem.itemId)
            timeline.openCueMenu(cueItem.itemId, cueItem.trackId)
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: editor.visible
        color: "#111111"
        border.color: "#d8b86a"
    }

    TextArea {
        id: editor
        anchors.fill: parent
        visible: false
        wrapMode: TextArea.Wrap
        selectByMouse: true
        color: "#f5ecd0"
        selectionColor: "#8fc4ff"
        selectedTextColor: "#111111"

        onEditingFinished: {
            if (text !== cueItem.text)
                Timeline.setCueText(cueItem.itemId, text)
            visible = false
        }

        Keys.onEscapePressed: (event) => {
            text = cueItem.text
            visible = false
            event.accepted = true
        }
    }
}
