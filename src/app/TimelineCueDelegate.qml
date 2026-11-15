import QtQuick
import QtQuick.Controls

// One subtitle cue on the subtitle track. Single click selects it; double
// click opens an inline editor and commits the text as one command.
Rectangle {
    id: cueItem
    property var timeline
    property string trackId
    property string itemId
    property string text
    property real startUs
    property real durationUs
    property bool selected: false

    radius: 3
    color: selected ? "#6b5a2b" : "#3d3520"
    border.color: selected ? "#d8b86a" : "#111111"
    border.width: 1
    x: startUs / 1e6 * timeline.pixelsPerSecond
    width: Math.max(8, durationUs / 1e6 * timeline.pixelsPerSecond)
    clip: true
    z: selected ? 2 : 1

    Text {
        anchors.fill: parent
        anchors.margins: 4
        text: cueItem.text
        color: "#f0e6c8"
        elide: Text.ElideRight
        font.pixelSize: 12
    }

    MouseArea {
        anchors.fill: parent
        onClicked: Timeline.selectCue(cueItem.itemId)
        onDoubleClicked: {
            editor.text = cueItem.text
            editor.visible = true
            editor.forceActiveFocus()
            editor.selectAll()
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
