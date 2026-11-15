import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import VideoEditor 1.0

ApplicationWindow {
    id: root
    width: 1440
    height: 900
    visible: true
    title: "VideoEditor " + appVersion
    color: "#111111"

    function zoomBy(factor) {
        Timeline.pixelsPerSecond = Math.max(20, Math.min(400,
                                            Timeline.pixelsPerSecond * factor))
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            spacing: 2

            Button { text: "导入素材"; onClicked: fileDialog.open() }
            Button { text: "打开工程"; onClicked: openProjectDialog.open() }
            Button { text: "保存工程"; onClicked: saveProjectDialog.open() }
            Button { text: "撤销"; enabled: App.canUndo; onClicked: App.undo() }
            Button { text: "重做"; enabled: App.canRedo; onClicked: App.redo() }

            ToolSeparator {}
            Button { text: "分割"; onClicked: Timeline.splitAtPlayhead() }
            Button { text: "删除"; onClicked: Timeline.deleteSelected(false) }
            Button { text: "波纹删除"; onClicked: Timeline.deleteSelected(true) }
            Button { text: "复制"; onClicked: Timeline.copySelected() }
            Button { text: "粘贴"; onClicked: Timeline.pasteAtPlayhead() }
            Button {
                text: "链接"
                enabled: Timeline.selectedClipId !== ""
                onClicked: Timeline.linkSelectedClip()
            }
            Button {
                text: "解链"
                enabled: Timeline.selectedClipId !== ""
                onClicked: Timeline.unlinkSelectedClip()
            }

            ToolButton {
                id: trackMenuButton
                objectName: "track-menu-button"
                text: "轨道"
                onClicked: trackMenu.open()

                // Add/reorder/remove lanes. New tracks become the target so
                // the next double-clicked media item lands there.
                Menu {
                    id: trackMenu
                    MenuItem {
                        objectName: "track-add-video"
                        text: "添加视频轨"
                        onClicked: Timeline.addTrack(0)
                    }
                    MenuItem {
                        objectName: "track-add-audio"
                        text: "添加音频轨"
                        onClicked: Timeline.addTrack(1)
                    }
                    MenuItem {
                        objectName: "track-add-subtitle"
                        text: "添加字幕轨"
                        onClicked: Timeline.addTrack(2)
                    }
                    MenuSeparator {}
                    MenuItem {
                        objectName: "track-move-up"
                        text: "上移当前目标轨"
                        enabled: Timeline.targetTrackId !== ""
                        onClicked: Timeline.moveTrackUp(Timeline.targetTrackId)
                    }
                    MenuItem {
                        objectName: "track-move-down"
                        text: "下移当前目标轨"
                        enabled: Timeline.targetTrackId !== ""
                        onClicked: Timeline.moveTrackDown(Timeline.targetTrackId)
                    }
                    MenuItem {
                        objectName: "track-remove"
                        text: "删除当前目标轨"
                        enabled: Timeline.targetTrackId !== ""
                        onClicked: Timeline.removeTrack(Timeline.targetTrackId)
                    }
                }
            }

            ToolSeparator {}
            CheckBox {
                text: "吸附"
                checked: Timeline.snapEnabled
                onToggled: Timeline.snapEnabled = checked
            }
            Slider {
                id: zoomSlider
                Layout.preferredWidth: 130
                from: 20
                to: 400
                onMoved: Timeline.pixelsPerSecond = value
            }

            Item { Layout.fillWidth: true }
            Label {
                text: Preview.timelineMode ? Preview.decodeMode
                                           : (Preview.decodeMode.length > 0
                                              ? Preview.decodeMode : "M2 时间线编辑")
                color: "#cccccc"
            }

            // Primary export entry stays in the top-right corner, matching the
            // CapCut-style "finish the edit, then export" flow.
            ToolSeparator {}
            Button {
                objectName: "exportButton"
                text: "导出"
                highlighted: true
                onClicked: exportDialog.openDialog()
            }
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
            text: Preview.timelineMode ? Timeline.timecode : Preview.timecode
            color: "white"
            font.family: "Consolas"
        }
        TextField {
            id: timecodeJump
            objectName: "timecode-jump"
            implicitWidth: 120
            placeholderText: "00:00:00:00"
            font.family: "Consolas"
            onEditingFinished: {
                if (!Timeline.seekTimecode(text))
                    text = Timeline.timecode
                focus = false
            }
            Binding {
                target: timecodeJump
                property: "text"
                value: Preview.timelineMode ? Timeline.timecode : Preview.timecode
                when: !timecodeJump.activeFocus
            }
        }
        Slider {
            id: transport
            Layout.fillWidth: true
            from: 0
            to: Math.max(1, Preview.timelineMode ? Timeline.durationUs / 1000
                                                 : Preview.durationMs)
            onMoved: {
                if (Preview.timelineMode)
                    App.seekTimeline(Math.round(value) * 1000)
                else
                    Preview.seekMs(value)
            }
            Binding {
                target: transport
                property: "value"
                value: Preview.timelineMode ? Timeline.positionUs / 1000
                                            : Preview.positionMs
                when: !transport.pressed
            }
        }
        CheckBox {
            text: "使用代理"
            checked: App.useProxy
            onToggled: App.useProxy = checked
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 1

        // Media library.
        Rectangle {
            Layout.preferredWidth: 250
            Layout.fillHeight: true
            color: "#1b1b1b"

            ListView {
                id: mediaList
                anchors.fill: parent
                model: App.mediaModel
                delegate: ItemDelegate {
                    objectName: "media-item-" + model.id
                    width: ListView.view.width
                    text: model.name + "\n" + model.codec + " · " + model.resolution
                          + " · " + model.duration
                    onClicked: App.openAsset(model.id)
                    // Add to the lane selected in the track headers (or the
                    // default matching lane when none is selected).
                    onDoubleClicked: App.addAssetToTimeline(
                                         model.id, Timeline.targetTrackId)
                }

                Label {
                    anchors.centerIn: parent
                    width: parent.width - 16
                    visible: mediaList.count === 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: "媒体库为空\n点击“导入素材”开始\n双击素材添加到时间线"
                    color: "#888888"
                }
            }
        }

        // Preview on top, timeline below.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 1

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
                Label {
                    anchors.centerIn: parent
                    visible: !Preview.mediaName
                    text: "选择左侧素材开始预览"
                    color: "#888888"
                }
                Component.onCompleted: Preview.item = previewItem
            }

            Rectangle {
                Layout.preferredHeight: 300
                Layout.fillWidth: true
                color: "#151515"
                TimelinePanel { anchors.fill: parent }
            }
        }

        // Inspector + subtitles.
        Rectangle {
            Layout.preferredWidth: 310
            Layout.fillHeight: true
            color: "#1b1b1b"

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 6

                InspectorPanel {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 470
                }
                SubtitlePanel {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
            }
        }
    }

    Binding {
        target: zoomSlider
        property: "value"
        value: Timeline.pixelsPerSecond
        when: !zoomSlider.pressed
    }

    Shortcut {
        sequence: "S"
        onActivated: Timeline.splitAtPlayhead()
    }
    Shortcut {
        sequence: "Delete"
        onActivated: Timeline.deleteSelected(false)
    }
    Shortcut {
        sequence: "Shift+Delete"
        onActivated: Timeline.deleteSelected(true)
    }
    Shortcut {
        sequence: "Ctrl+C"
        onActivated: Timeline.copySelected()
    }
    Shortcut {
        sequence: "Ctrl+V"
        onActivated: Timeline.pasteAtPlayhead()
    }
    Shortcut {
        sequence: "+"
        onActivated: root.zoomBy(1.25)
    }
    Shortcut {
        sequence: "="
        onActivated: root.zoomBy(1.25)
    }
    Shortcut {
        sequence: "-"
        onActivated: root.zoomBy(0.8)
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
        title: "导入素材"
        nameFilters: ["媒体文件 (*.mp4 *.wmv *.mov *.mkv *.mp3 *.wav *.flac *.m4a)",
                      "所有文件 (*)"]
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

    ExportDialog {
        id: exportDialog
    }
}
