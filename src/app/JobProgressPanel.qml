import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Floating progress panel for the running AI removal job. Editing stays
// available: AiJobManager only processes transport messages on the UI thread.
Rectangle {
    id: root
    objectName: "job-progress-panel"

    property var removal: (typeof Removal !== "undefined") ? Removal : null
    visible: removal !== null && removal.generating
    color: "#202020"
    border.color: "#3a3a3a"
    border.width: 1
    radius: 6
    implicitHeight: layout.implicitHeight + 12

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: 6
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: root.removal !== null ? root.removal.stageText
                                            : ""
                color: "#e8e8e8"
                font.pixelSize: 11
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                objectName: "job-progress-paused"
                visible: root.removal !== null && root.removal.paused
                text: "已暂停"
                color: "#e6c15a"
                font.pixelSize: 10
            }
            Label {
                text: root.removal !== null ? root.removal.etaText : ""
                color: "#9a9a9a"
                font.pixelSize: 10
            }
        }

        ProgressBar {
            objectName: "job-progress-bar"
            Layout.fillWidth: true
            from: 0
            to: 100
            value: root.removal !== null ? root.removal.progress : 0
        }

        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "job-pause-button"
                text: root.removal !== null && root.removal.paused
                      ? "继续" : "暂停"
                font.pixelSize: 10
                padding: 3
                onClicked: {
                    if (root.removal === null)
                        return
                    if (root.removal.paused)
                        root.removal.resume()
                    else
                        root.removal.pause()
                }
            }
            Button {
                objectName: "job-cancel-button"
                text: "取消"
                font.pixelSize: 10
                padding: 3
                onClicked: if (root.removal !== null) root.removal.cancel()
            }
            Item { Layout.fillWidth: true }
            Label {
                text: Math.round(root.removal !== null
                                 ? root.removal.progress : 0) + "%"
                color: "#cfcfcf"
                font.pixelSize: 10
            }
        }
    }
}
