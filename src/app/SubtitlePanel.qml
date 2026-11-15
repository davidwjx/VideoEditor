import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Subtitle track editor: add/delete cues, import/export SRT, apply style
// presets and edit each cue's text in place.
ColumnLayout {
    id: panel
    spacing: 6

    Label {
        text: "字幕"
        font.bold: true
        color: "#dddddd"
    }

    RowLayout {
        Layout.fillWidth: true
        Button {
            text: "在播放头添加"
            onClicked: Timeline.addCueAtPlayhead()
        }
        Button {
            text: "删除选中"
            onClicked: Timeline.deleteSelectedCue()
        }
    }
    RowLayout {
        Layout.fillWidth: true
        Button {
            text: "导入 SRT"
            onClicked: srtImport.open()
        }
        Button {
            text: "导出 SRT"
            onClicked: srtExport.open()
        }
    }

    ComboBox {
        id: styleBox
        Layout.fillWidth: true
        model: App.subtitleStyles()
        textRole: "name"
        valueRole: "id"
        onActivated: Timeline.applySubtitleStyle(currentValue)
    }

    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        model: Timeline.timelineModel

        delegate: Rectangle {
            width: ListView.view.width
            visible: model.kind === "cue"
            height: visible ? 70 : 0
            color: model.selected ? "#3a3424" : "transparent"
            border.color: "#2a2a2a"

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Label {
                        text: (model.startUs / 1e6).toFixed(2) + "s"
                        color: "#999999"
                        font.pixelSize: 11
                    }
                    Item { Layout.fillWidth: true }
                    ComboBox {
                        Layout.preferredWidth: 110
                        model: App.subtitleStyles()
                        textRole: "name"
                        valueRole: "id"
                        currentIndex: Math.max(0, indexOfValue(model.styleId))
                        onActivated: Timeline.applySubtitleStyle(currentValue,
                                                                 model.itemId)
                    }
                    ToolButton {
                        text: "删"
                        font.pixelSize: 11
                        padding: 2
                        onClicked: {
                            Timeline.selectCue(model.itemId)
                            Timeline.deleteSelectedCue()
                        }
                    }
                }

                TextField {
                    Layout.fillWidth: true
                    text: model.text
                    placeholderText: "字幕文本"
                    onEditingFinished: {
                        if (text !== model.text)
                            Timeline.setCueText(model.itemId, text)
                    }
                    onActiveFocusChanged: {
                        if (activeFocus)
                            Timeline.selectCue(model.itemId)
                    }
                }
            }
        }
    }

    FileDialog {
        id: srtImport
        title: "导入 SRT"
        nameFilters: ["字幕文件 (*.srt)"]
        onAccepted: Timeline.importSrtFile(selectedFile)
    }
    FileDialog {
        id: srtExport
        title: "导出 SRT"
        nameFilters: ["字幕文件 (*.srt)"]
        fileMode: FileDialog.SaveFile
        onAccepted: Timeline.exportSrtFile(selectedFile)
    }
}
