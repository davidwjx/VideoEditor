import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import VideoEditor 1.0

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
            Button { text: "打开工程"; onClicked: openProjectDialog.open() }
            Button { text: "保存工程"; onClicked: saveProjectDialog.open() }
            Button { text: "撤销"; enabled: App.canUndo; onClicked: App.undo() }
            Button { text: "重做"; enabled: App.canRedo; onClicked: App.redo() }
            Item { Layout.fillWidth: true }
            Label { text: "M1 媒体基础"; color: "#cccccc" }
        }
    }

    footer: RowLayout {
        spacing: 8
        ToolButton {
            text: Preview.playing ? "暂停" : "播放"
            onClicked: Preview.togglePlay()
        }
        ToolButton { text: "◀"; onClicked: Preview.stepFrames(-1) }
        ToolButton { text: "▶"; onClicked: Preview.stepFrames(1) }
        Label {
            text: Preview.timecode
            color: "white"
            font.family: "Consolas"
        }
        Slider {
            Layout.fillWidth: true
            from: 0
            to: Math.max(1, Preview.durationMs)
            value: Preview.positionMs
            onMoved: Preview.seekMs(value)
        }
        CheckBox {
            text: "使用代理"
            checked: App.useProxy
            onToggled: App.useProxy = checked
        }
        Label { text: Preview.decodeMode; color: "#cccccc" }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 1

        Rectangle {
            Layout.preferredWidth: 320
            Layout.fillHeight: true
            color: "#1b1b1b"

            ListView {
                id: mediaList
                anchors.fill: parent
                model: App.mediaModel
                delegate: ItemDelegate {
                    width: ListView.view.width
                    text: model.name + "\n" + model.codec + " · " + model.resolution
                          + " · " + model.duration
                    onClicked: App.openAsset(model.id)
                }

                Label {
                    anchors.centerIn: parent
                    visible: mediaList.count === 0
                    text: "媒体库为空，点击“导入视频”开始"
                    color: "#888888"
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Rectangle {
                anchors.fill: parent
                color: "black"
            }

            VideoPreview {
                id: previewItem
                anchors.fill: parent
            }

            Component.onCompleted: Preview.item = previewItem

            Label {
                anchors.centerIn: parent
                visible: !Preview.mediaName
                text: "选择左侧素材开始预览"
                color: "#888888"
            }
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

    FileDialog {
        id: openProjectDialog
        title: "打开工程"
        nameFilters: ["VideoEditor 工程 (*.veproj)"]
        fileMode: FileDialog.OpenFile
        onAccepted: App.openProject(selectedFile)
    }

    FileDialog {
        id: saveProjectDialog
        title: "保存工程"
        nameFilters: ["VideoEditor 工程 (*.veproj)"]
        fileMode: FileDialog.SaveFile
        onAccepted: App.saveProject(selectedFile)
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
