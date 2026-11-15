import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Fixed track header with mute/solo/lock toggles. Each toggle is one
// controller command (and therefore one undo step).
Rectangle {
    id: header
    property string trackId
    property string name
    property string typeLabel
    property bool muted
    property bool solo
    property bool locked

    color: "#202020"
    border.color: "#101010"

    RowLayout {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 2

        Label {
            text: header.name
            color: "#dddddd"
            font.bold: true
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
        Label {
            text: header.typeLabel
            color: "#888888"
            font.pixelSize: 10
        }
        ToolButton {
            text: "M"
            checkable: true
            checked: header.muted
            font.pixelSize: 11
            padding: 2
            onClicked: Timeline.setTrackMuted(header.trackId, checked)
        }
        ToolButton {
            text: "S"
            checkable: true
            checked: header.solo
            font.pixelSize: 11
            padding: 2
            onClicked: Timeline.setTrackSolo(header.trackId, checked)
        }
        ToolButton {
            text: "锁"
            checkable: true
            checked: header.locked
            font.pixelSize: 11
            padding: 2
            onClicked: Timeline.setTrackLocked(header.trackId, checked)
        }
    }
}
