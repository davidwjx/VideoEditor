import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Fixed track header. Clicking the header makes this lane the insertion target;
// the M/S toggles are only offered on audio tracks (they are meaningful audio
// controls), while every track can be locked or reordered/removed from the
// overflow menu. Each action is one controller command and one undo step.
Rectangle {
    id: header
    objectName: "track-header-" + trackId
    property string trackId
    property string name
    property string typeLabel
    property string trackType
    property bool muted
    property bool solo
    property bool locked
    property bool isTarget: Timeline.targetTrackId === trackId

    color: isTarget ? "#2b3442" : "#202020"
    border.color: isTarget ? "#8fc4ff" : "#101010"
    border.width: isTarget ? 1 : 0

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        onClicked: Timeline.targetTrackId = header.trackId
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 3
        spacing: 1

        RowLayout {
            Layout.fillWidth: true
            spacing: 3

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
                font.pixelSize: 9
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 1

            ToolButton {
                id: muteButton
                objectName: "track-mute-" + header.trackId
                visible: header.trackType === "audio"
                text: "M"
                checkable: true
                font.pixelSize: 10
                padding: 1
                Layout.preferredWidth: 22
                onClicked: Timeline.setTrackMuted(header.trackId, !header.muted)
                ToolTip.visible: hovered
                ToolTip.text: "静音轨道"
                Binding {
                    target: muteButton
                    property: "checked"
                    value: header.muted
                    when: !muteButton.pressed
                }
            }
            ToolButton {
                id: soloButton
                objectName: "track-solo-" + header.trackId
                visible: header.trackType === "audio"
                text: "S"
                checkable: true
                font.pixelSize: 10
                padding: 1
                Layout.preferredWidth: 22
                onClicked: Timeline.setTrackSolo(header.trackId, !header.solo)
                ToolTip.visible: hovered
                ToolTip.text: "独奏轨道"
                Binding {
                    target: soloButton
                    property: "checked"
                    value: header.solo
                    when: !soloButton.pressed
                }
            }
            ToolButton {
                id: lockButton
                objectName: "track-lock-" + header.trackId
                text: "锁"
                checkable: true
                font.pixelSize: 10
                padding: 1
                Layout.preferredWidth: 24
                onClicked: Timeline.setTrackLocked(header.trackId, !header.locked)
                ToolTip.visible: hovered
                ToolTip.text: "锁定轨道"
                Binding {
                    target: lockButton
                    property: "checked"
                    value: header.locked
                    when: !lockButton.pressed
                }
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                id: menuButton
                objectName: "track-menu-" + header.trackId
                text: "⋮"
                font.pixelSize: 12
                padding: 1
                Layout.preferredWidth: 20
                onClicked: trackMenu.open()

                Menu {
                    id: trackMenu
                    onOpened: App.beginPopup()
                    onClosed: App.endPopup()
                    MenuItem {
                        objectName: "track-up-" + header.trackId
                        text: "上移轨道"
                        onClicked: Timeline.moveTrackUp(header.trackId)
                    }
                    MenuItem {
                        objectName: "track-down-" + header.trackId
                        text: "下移轨道"
                        onClicked: Timeline.moveTrackDown(header.trackId)
                    }
                    MenuSeparator {}
                    MenuItem {
                        objectName: "track-remove-" + header.trackId
                        text: "删除轨道"
                        onClicked: Timeline.removeTrack(header.trackId)
                    }
                }
            }
        }
    }
}
