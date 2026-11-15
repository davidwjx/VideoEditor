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
            id: muteButton
            text: "M"
            checkable: true
            font.pixelSize: 11
            padding: 2
            onClicked: Timeline.setTrackMuted(header.trackId, !header.muted)
            Binding {
                target: muteButton
                property: "checked"
                value: header.muted
                when: !muteButton.pressed
            }
        }
        ToolButton {
            id: soloButton
            text: "S"
            checkable: true
            font.pixelSize: 11
            padding: 2
            onClicked: Timeline.setTrackSolo(header.trackId, !header.solo)
            Binding {
                target: soloButton
                property: "checked"
                value: header.solo
                when: !soloButton.pressed
            }
        }
        ToolButton {
            id: lockButton
            text: "锁"
            checkable: true
            font.pixelSize: 11
            padding: 2
            onClicked: Timeline.setTrackLocked(header.trackId, !header.locked)
            Binding {
                target: lockButton
                property: "checked"
                value: header.locked
                when: !lockButton.pressed
            }
        }
    }
}
