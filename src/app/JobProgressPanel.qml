import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Floating progress panel for the running AI removal job. Editing stays
// available: AiJobManager only processes transport messages on the UI thread.
Rectangle {
    id: root
    objectName: "job-progress-panel"

    property var removal: (typeof Removal !== "undefined") ? Removal : null
    // A modal dialog above the window swallows every click outside it while
    // still leaving this panel looking alive. Main.qml suppresses the panel in
    // that case; the dialog itself owns the job progress and a reachable
    // cancel, so a click can never land on a dead-looking control.
    property bool suppressed: false
    visible: !suppressed && removal !== null && removal.generating
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
                objectName: "job-progress-stage"
                // Once cancel is requested the stage must stay "正在取消…"
                // even though the engine keeps sending tracking progress until
                // it stops; otherwise the label flickers back and the click
                // looks ignored.
                text: root.removal !== null
                      ? (root.removal.cancelling
                         ? "正在取消…" : root.removal.stageText)
                      : ""
                color: root.removal !== null && root.removal.cancelling
                       ? "#e6c15a" : "#e8e8e8"
                font.pixelSize: 11
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                objectName: "job-progress-paused"
                visible: root.removal !== null && root.removal.paused
                         && !root.removal.cancelling
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
            // 44 px tall targets: the previous 28x23 px buttons were easy to
            // miss, and a missed click looks exactly like a dead cancel.
            Button {
                objectName: "job-pause-button"
                text: root.removal !== null && root.removal.paused
                      ? "继续" : "暂停"
                font.pixelSize: 11
                padding: 6
                implicitHeight: 30
                implicitWidth: 64
                enabled: root.removal !== null && !root.removal.cancelling
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
                text: root.removal !== null && root.removal.cancelling
                      ? "取消中…" : "取消"
                font.pixelSize: 11
                padding: 6
                implicitHeight: 30
                implicitWidth: 72
                enabled: root.removal !== null && !root.removal.cancelling
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
