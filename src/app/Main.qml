import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    width: 1280
    height: 800
    visible: true
    title: "VideoEditor " + appVersion
    color: "#111111"

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            Button { text: "导入视频"; onClicked: fileDialog.open() }
            Button { text: "撤销"; enabled: App.canUndo; onClicked: App.undo() }
            Button { text: "重做"; enabled: App.canRedo; onClicked: App.redo() }
            Item { Layout.fillWidth: true }
            Label { text: "M1 媒体基础"; color: "#cccccc" }
        }
    }

    ListView {
        id: mediaList
        anchors.fill: parent
        model: App.mediaModel
        delegate: ItemDelegate {
            width: ListView.view.width
            text: model.name + "\n" + model.codec + " · " + model.resolution
                  + " · " + model.duration
        }

        Label {
            anchors.centerIn: parent
            visible: mediaList.count === 0
            text: "媒体库为空，点击“导入视频”开始"
            color: "#888888"
        }
    }

    Rectangle {
        visible: App.lastError.length > 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 72
        color: "#7a1f1f"
        z: 10

        Label {
            anchors.fill: parent
            anchors.margins: 8
            text: App.lastError
            color: "white"
            wrapMode: Text.WordWrap
        }

        MouseArea {
            anchors.fill: parent
            onClicked: App.clearError()
        }
    }

    FileDialog {
        id: fileDialog
        title: "导入视频"
        nameFilters: ["视频文件 (*.mp4 *.wmv)", "所有文件 (*)"]
        fileMode: FileDialog.OpenFiles
        onAccepted: App.importUrls(selectedFiles)
    }

    Dialog {
        id: recoveryDialog
        visible: App.recoveryAvailable
        title: "发现未保存的工程"
        modal: true
        standardButtons: Dialog.NoButton

        RowLayout {
            Button {
                text: "恢复"
                onClicked: App.restoreRecovery()
            }
            Button {
                text: "丢弃"
                onClicked: App.discardRecovery()
            }
        }
    }
}
