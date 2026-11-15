import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import VideoEditor 1.0

ApplicationWindow {
    id: root
    width: 1440
    height: 900
    minimumWidth: 1024
    minimumHeight: 700
    visible: true
    title: "VideoEditor " + appVersion
    color: "#111111"

    // Visible persistent ripple mode. Shift+Delete remains a one-shot command;
    // this chip makes the destructive variant explicit for the Delete key.
    property bool rippleMode: false

    // Editing shortcuts must never steal keystrokes from a text editor. Every
    // TextInput/TextEdit (including TextField/TextArea) exposes cursorPosition.
    readonly property bool textEditing: {
        const item = activeFocusItem
        return item !== null && item !== undefined
               && item.cursorPosition !== undefined
    }

    readonly property bool hasTimelineContent: Timeline.durationUs > 0
    readonly property bool clipSelected: Timeline.selectedClipId !== ""
    readonly property bool cueSelected: Timeline.selectedCueId !== ""

    function zoomBy(factor) {
        Timeline.pixelsPerSecond = Math.max(20, Math.min(400,
                                            Timeline.pixelsPerSecond * factor))
    }

    // Closing the window during an export would drop the user into a silent
    // partial export; keep it open and let the dialog's cancel path finish.
    onClosing: function(close) {
        if (Export.running)
            close.accepted = false
    }

    // ---------------------------------------------------------------------
    // Primary bar: import, project menu, undo/redo and the fixed export entry.
    // Everything else lives in the selection bar, the timeline status strip,
    // context menus or the inspector's advanced section.
    // ---------------------------------------------------------------------
    header: ToolBar {
        RowLayout {
            objectName: "primary-bar"
            anchors.fill: parent
            spacing: 4

            Button {
                objectName: "import-button"
                text: "⤓ 导入素材"
                onClicked: fileDialog.open()
                ToolTip.visible: hovered
                ToolTip.text: "导入素材 (Ctrl+I)"
            }

            ToolButton {
                objectName: "project-menu-button"
                text: "项目 ▾"
                onClicked: projectMenu.open()
                ToolTip.visible: hovered
                ToolTip.text: "打开/保存工程"

                Menu {
                    id: projectMenu
                    MenuItem {
                        objectName: "project-open"
                        text: "打开工程…  Ctrl+O"
                        onClicked: openProjectDialog.open()
                    }
                    MenuItem {
                        objectName: "project-save"
                        text: "保存工程…  Ctrl+S"
                        onClicked: saveProjectDialog.open()
                    }
                }
            }

            ToolSeparator {}

            ToolButton {
                objectName: "undo-button"
                text: "↶"
                enabled: App.canUndo
                onClicked: App.undo()
                ToolTip.visible: hovered
                ToolTip.text: "撤销 (Ctrl+Z)"
            }
            ToolButton {
                objectName: "redo-button"
                text: "↷"
                enabled: App.canRedo
                onClicked: App.redo()
                ToolTip.visible: hovered
                ToolTip.text: "重做 (Ctrl+Y)"
            }

            Item { Layout.fillWidth: true }

            Label {
                text: root.hasTimelineContent ? "时间线编辑" : "空工程"
                color: "#9a9a9a"
                font.pixelSize: 11
            }

            Button {
                objectName: "exportButton"
                text: "导出"
                highlighted: true
                enabled: root.hasTimelineContent
                onClicked: exportDialog.openDialog()
                ToolTip.visible: hovered
                ToolTip.text: root.hasTimelineContent
                              ? "导出视频 (Ctrl+E)"
                              : "时间线为空，先添加素材"
            }
        }
    }

    // ---------------------------------------------------------------------
    // Transport: playback, frame step, timecode scrub and proxy state.
    // ---------------------------------------------------------------------
    footer: RowLayout {
        spacing: 8

        ToolButton {
            objectName: "play-button"
            text: Preview.playing ? "⏸" : "▶"
            onClicked: Preview.togglePlay()
            ToolTip.visible: hovered
            ToolTip.text: Preview.playing ? "暂停 (空格)" : "播放 (空格)"
        }
        ToolButton {
            text: "⏮"
            onClicked: Preview.stepFrames(-1)
            ToolTip.visible: hovered
            ToolTip.text: "上一帧 (←)"
        }
        ToolButton {
            text: "⏭"
            onClicked: Preview.stepFrames(1)
            ToolTip.visible: hovered
            ToolTip.text: "下一帧 (→)"
        }
        Label {
            objectName: "transport-timecode"
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
            ToolTip.visible: hovered
            ToolTip.text: "跳到时间码"
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
    }

    RowLayout {
        anchors.fill: parent
        spacing: 1

        // Media library: single click previews, double-click (or +) adds to the
        // current target lane at the playhead.
        Rectangle {
            Layout.preferredWidth: 250
            Layout.fillHeight: true
            color: "#1b1b1b"

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    Layout.margins: 8
                    text: "媒体库"
                    font.bold: true
                    color: "#dddddd"
                }

                ListView {
                    id: mediaList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: App.mediaModel
                    delegate: ItemDelegate {
                        id: mediaDelegate
                        objectName: "media-item-" + model.id
                        width: ListView.view.width
                        text: model.name + "\n" + model.codec + " · "
                              + model.resolution + " · " + model.duration
                        onClicked: App.openAsset(model.id)
                        onDoubleClicked: App.addAssetToTimeline(
                                             model.id, Timeline.targetTrackId)

                        // Right-click menu mirrors the visible + button and
                        // keeps advanced library actions off the first screen.
                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            onClicked: mediaMenu.popup()
                        }
                        ToolButton {
                            objectName: "media-add-" + model.id
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            text: "＋"
                            onClicked: App.addAssetToTimeline(
                                           model.id, Timeline.targetTrackId)
                            ToolTip.visible: hovered
                            ToolTip.text: "加入时间线（播放头）"
                        }
                        Menu {
                            id: mediaMenu
                            MenuItem {
                                objectName: "media-menu-add"
                                text: "加入时间线"
                                onClicked: App.addAssetToTimeline(
                                               model.id, Timeline.targetTrackId)
                            }
                            MenuItem {
                                objectName: "media-menu-preview"
                                text: "在源监视器打开"
                                onClicked: App.openAsset(model.id)
                            }
                        }
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
        }

        // Preview on top, selection context bar, then the full-width timeline.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 1

            Item {
                id: previewArea
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

                // Inline, non-blocking error toast: it floats over the preview
                // instead of covering the transport or the lower timeline lanes.
                Rectangle {
                    objectName: "error-toast"
                    visible: App.lastError.length > 0
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 12
                    width: Math.min(parent.width - 40, 560)
                    height: toastLabel.implicitHeight + 16
                    radius: 6
                    color: "#7a1f1f"
                    border.color: "#b05050"
                    z: 30

                    Label {
                        id: toastLabel
                        anchors.fill: parent
                        anchors.margins: 8
                        text: App.lastError
                        color: "white"
                        wrapMode: Text.WordWrap
                        verticalAlignment: Text.AlignVCenter
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: App.clearError()
                    }
                }

                Timer {
                    id: errorToastTimer
                    interval: 4000
                    repeat: false
                    onTriggered: App.clearError()
                }
                Connections {
                    target: App
                    function onLastErrorChanged() {
                        if (App.lastError.length > 0)
                            errorToastTimer.restart()
                    }
                }
            }

            // Selection-scoped context bar. Hidden when nothing is selected so
            // the first screen stays small.
            Rectangle {
                objectName: "selection-context-bar"
                visible: root.clipSelected || root.cueSelected
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                color: "#1c1c1c"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 6

                    Label {
                        text: root.cueSelected ? "字幕" : "片段"
                        color: "#d8b86a"
                        font.bold: true
                    }

                    Button {
                        objectName: "context-split"
                        visible: root.clipSelected
                        text: "分割  Ctrl+B"
                        onClicked: Timeline.splitAtPlayhead()
                    }
                    Button {
                        objectName: "context-delete"
                        text: "删除  Del"
                        onClicked: Timeline.deleteSelection(root.rippleMode)
                    }
                    Button {
                        objectName: "context-ripple"
                        visible: root.clipSelected
                        text: "波纹删除"
                        onClicked: Timeline.deleteSelection(true)
                    }
                    Button {
                        objectName: "context-edit-text"
                        visible: root.cueSelected
                        text: "编辑文本"
                        onClicked: subtitlePanel.focusSelectedText()
                    }
                    ToolButton {
                        objectName: "context-more-button"
                        visible: root.clipSelected
                        text: "更多 ▾"
                        onClicked: contextMoreMenu.open()

                        Menu {
                            id: contextMoreMenu
                            MenuItem {
                                objectName: "context-menu-link"
                                text: "链接音视频"
                                onClicked: Timeline.linkSelectedClip()
                            }
                            MenuItem {
                                objectName: "context-menu-unlink"
                                text: "解除链接"
                                onClicked: Timeline.unlinkSelectedClip()
                            }
                            MenuSeparator {}
                            MenuItem {
                                objectName: "context-menu-copy"
                                text: "复制  Ctrl+C"
                                onClicked: Timeline.copySelected()
                            }
                            MenuItem {
                                objectName: "context-menu-paste"
                                text: "粘贴到播放头  Ctrl+V"
                                onClicked: Timeline.pasteAtPlayhead()
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.preferredHeight: 330
                Layout.fillWidth: true
                color: "#151515"

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    TimelinePanel {
                        id: timelinePanel
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        onCueEditRequested: subtitlePanel.focusSelectedText()
                    }

                    // Timeline status strip: state chips and moderate-frequency
                    // actions, all outside the primary bar.
                    Rectangle {
                        objectName: "timeline-status-bar"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        color: "#1b1b1b"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 6
                            anchors.rightMargin: 6
                            spacing: 4

                            ToolButton {
                                objectName: "add-track-button"
                                text: "＋ 轨道"
                                onClicked: addTrackMenu.open()
                                ToolTip.visible: hovered
                                ToolTip.text: "添加视频/音频/字幕轨"

                                Menu {
                                    id: addTrackMenu
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
                                }
                            }

                            CheckBox {
                                id: snapChip
                                objectName: "snap-toggle"
                                text: "吸附"
                                checked: Timeline.snapEnabled
                                onToggled: Timeline.snapEnabled = checked
                                ToolTip.visible: hovered
                                ToolTip.text: "吸附到播放头和片段边缘"
                                Binding {
                                    target: snapChip
                                    property: "checked"
                                    value: Timeline.snapEnabled
                                    when: !snapChip.pressed
                                }
                            }
                            CheckBox {
                                id: proxyChip
                                objectName: "proxy-toggle"
                                text: "代理"
                                checked: App.useProxy
                                onToggled: App.useProxy = checked
                                ToolTip.visible: hovered
                                ToolTip.text: "使用代理媒体预览"
                                Binding {
                                    target: proxyChip
                                    property: "checked"
                                    value: App.useProxy
                                    when: !proxyChip.pressed
                                }
                            }
                            CheckBox {
                                objectName: "ripple-mode-toggle"
                                text: "波纹"
                                checked: root.rippleMode
                                onToggled: root.rippleMode = checked
                                ToolTip.visible: hovered
                                ToolTip.text: "波纹删除模式：删除后自动前移后续片段"
                            }
                            CheckBox {
                                objectName: "auto-follow-toggle"
                                text: "跟随"
                                checked: timelinePanel.followPlayhead
                                onToggled: timelinePanel.followPlayhead = checked
                                ToolTip.visible: hovered
                                ToolTip.text: "播放时自动滚动时间线跟随播放头"
                            }

                            Item { Layout.fillWidth: true }

                            ToolButton {
                                objectName: "keyboard-help-button"
                                text: "?"
                                onClicked: shortcutHelp.open()
                                ToolTip.visible: hovered
                                ToolTip.text: "快捷键帮助"
                            }
                            ToolButton {
                                objectName: "zoom-out-button"
                                text: "−"
                                onClicked: root.zoomBy(0.8)
                                ToolTip.visible: hovered
                                ToolTip.text: "缩小时间线 (-)"
                            }
                            ToolButton {
                                objectName: "zoom-in-button"
                                text: "＋"
                                onClicked: root.zoomBy(1.25)
                                ToolTip.visible: hovered
                                ToolTip.text: "放大时间线 (+)"
                            }
                        }
                    }
                }
            }
        }

        // Single scrollable right column: inspector sections and subtitles.
        // At 1024x700 the advanced/subtitle controls stay reachable by scroll.
        Flickable {
            id: rightScroll
            Layout.preferredWidth: 340
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: rightColumn.implicitHeight + 16
            clip: true
            ScrollBar.vertical: ScrollBar {}
            boundsBehavior: Flickable.StopAtBounds

            ColumnLayout {
                id: rightColumn
                x: 8
                y: 8
                width: rightScroll.width - 16
                spacing: 8

                InspectorPanel {
                    Layout.fillWidth: true
                }
                SubtitlePanel {
                    id: subtitlePanel
                    Layout.fillWidth: true
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // Shortcuts. Editing keys are disabled while a text editor has focus so
    // typing "s", "-" or pasting text never edits the timeline.
    // ---------------------------------------------------------------------
    Shortcut {
        sequence: "Ctrl+I"
        enabled: !root.textEditing
        onActivated: fileDialog.open()
    }
    Shortcut {
        sequence: "Ctrl+O"
        enabled: !root.textEditing
        onActivated: openProjectDialog.open()
    }
    Shortcut {
        sequence: "Ctrl+S"
        enabled: !root.textEditing
        onActivated: saveProjectDialog.open()
    }
    Shortcut {
        sequence: "Ctrl+E"
        enabled: !root.textEditing
        onActivated: {
            if (root.hasTimelineContent)
                exportDialog.openDialog()
        }
    }

    Shortcut {
        sequence: "Ctrl+Z"
        enabled: !root.textEditing && App.canUndo
        onActivated: App.undo()
    }
    Shortcut {
        sequence: "Ctrl+Y"
        enabled: !root.textEditing && App.canRedo
        onActivated: App.redo()
    }
    Shortcut {
        sequence: "Ctrl+B"
        enabled: !root.textEditing
        onActivated: Timeline.splitAtPlayhead()
    }
    Shortcut {
        sequence: "Delete"
        enabled: !root.textEditing
        onActivated: Timeline.deleteSelection(root.rippleMode)
    }
    Shortcut {
        sequence: "Shift+Delete"
        enabled: !root.textEditing
        onActivated: Timeline.deleteSelection(true)
    }
    Shortcut {
        sequence: "Space"
        enabled: !root.textEditing
        onActivated: Preview.togglePlay()
    }
    Shortcut {
        sequence: "Left"
        enabled: !root.textEditing
        onActivated: Timeline.stepFrames(-1)
    }
    Shortcut {
        sequence: "Right"
        enabled: !root.textEditing
        onActivated: Timeline.stepFrames(1)
    }
    Shortcut {
        sequence: "Home"
        enabled: !root.textEditing
        onActivated: Timeline.seekUs(0)
    }
    Shortcut {
        sequence: "End"
        enabled: !root.textEditing
        onActivated: Timeline.seekUs(Timeline.durationUs)
    }
    Shortcut {
        sequence: "+"
        enabled: !root.textEditing
        onActivated: root.zoomBy(1.25)
    }
    Shortcut {
        sequence: "="
        enabled: !root.textEditing
        onActivated: root.zoomBy(1.25)
    }
    Shortcut {
        sequence: "-"
        enabled: !root.textEditing
        onActivated: root.zoomBy(0.8)
    }
    Shortcut {
        sequence: "Ctrl+C"
        enabled: !root.textEditing
        onActivated: Timeline.copySelected()
    }
    Shortcut {
        sequence: "Ctrl+V"
        enabled: !root.textEditing
        onActivated: Timeline.pasteAtPlayhead()
    }

    Dialog {
        id: shortcutHelp
        title: "快捷键"
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Close
        ColumnLayout {
            spacing: 4
            Label { text: "空格  播放/暂停" }
            Label { text: "Ctrl+B  在播放头分割" }
            Label { text: "Delete / Shift+Delete  删除 / 波纹删除" }
            Label { text: "Ctrl+Z / Ctrl+Y  撤销 / 重做" }
            Label { text: "← / →  逐帧     Home / End  开头 / 结尾" }
            Label { text: "+ / -  缩放时间线" }
            Label { text: "Ctrl+C / Ctrl+V  复制 / 粘贴到播放头" }
            Label { text: "Ctrl+E  导出     Ctrl+I  导入     Ctrl+S / Ctrl+O  保存 / 打开" }
            Label {
                text: "参考剪映操作习惯，快捷键与布局为独立设计。"
                color: "#9a9a9a"
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 420
            }
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
