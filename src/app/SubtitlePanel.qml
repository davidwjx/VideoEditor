import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Subtitle section. "+ 字幕" is the single frequent action: it creates a cue at
// the playhead, selects it, and focuses a text field so the user can type
// immediately without another click. SRT import/export live in the overflow
// menu; precise timing, style and position remain visible.
ColumnLayout {
    id: panel
    objectName: "subtitle-panel"
    spacing: 6

    function cueField(name, fallback) {
        const map = Timeline.selectedCue
        const value = map ? map[name] : undefined
        return value === undefined ? fallback : value
    }

    function selectedCueText() {
        return panel.cueField("text", "")
    }

    function resetTimingFields() {
        startField.text = (panel.cueField("startUs", 0) / 1e6).toFixed(3)
        endField.text = (panel.cueField("endUs", 0) / 1e6).toFixed(3)
    }

    function commitCueTiming() {
        if (Timeline.selectedCueId === "")
            return
        const start = parseFloat(startField.text) * 1e6
        const end = parseFloat(endField.text) * 1e6
        if (!isFinite(start) || !isFinite(end) || start < 0 || end <= start) {
            resetTimingFields()
            return
        }
        Timeline.setCueTiming(Timeline.selectedCueId, Math.round(start),
                              Math.round(end))
    }

    function commitTopText() {
        if (Timeline.selectedCueId === "")
            return
        if (topText.editBuffer !== panel.selectedCueText())
            Timeline.setCueText(Timeline.selectedCueId, topText.editBuffer)
    }

    function commitPositionX(permille) {
        if (Timeline.selectedCueId !== "")
            Timeline.setCuePosition(panel.cueField("trackId", ""),
                                    Timeline.selectedCueId,
                                    Math.round(permille),
                                    panel.cueField("positionYPermille", 880))
    }

    function commitPositionY(permille) {
        if (Timeline.selectedCueId !== "")
            Timeline.setCuePosition(panel.cueField("trackId", ""),
                                    Timeline.selectedCueId,
                                    panel.cueField("positionXPermille", 500),
                                    Math.round(permille))
    }

    function focusSelectedText() {
        if (Timeline.selectedCueId === "")
            return
        topText.forceActiveFocus()
        topText.selectAll()
    }

    RowLayout {
        Layout.fillWidth: true
        Label {
            text: "字幕"
            font.bold: true
            color: "#dddddd"
            Layout.fillWidth: true
        }
        ToolButton {
            objectName: "subtitle-overflow"
            text: "⋯"
            onClicked: subtitleMenu.open()
            ToolTip.visible: hovered
            ToolTip.text: "更多字幕操作"
            Menu {
                id: subtitleMenu
                MenuItem {
                    objectName: "subtitle-import-srt"
                    text: "导入 SRT…"
                    onClicked: srtImport.open()
                }
                MenuItem {
                    objectName: "subtitle-export-srt"
                    text: "导出 SRT…"
                    onClicked: srtExport.open()
                }
                MenuSeparator {}
                MenuItem {
                    objectName: "subtitle-delete"
                    text: "删除选中字幕"
                    enabled: Timeline.selectedCueId !== ""
                    onClicked: Timeline.deleteSelectedCue()
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        Button {
            objectName: "subtitle-add"
            Layout.fillWidth: true
            text: "＋ 字幕（播放头）"
            highlighted: true
            onClicked: {
                const id = Timeline.addCueAtPlayhead()
                if (id.length > 0) {
                    Qt.callLater(function() {
                        topText.forceActiveFocus()
                        topText.selectAll()
                    })
                }
            }
            ToolTip.visible: hovered
            ToolTip.text: "在播放头添加字幕并立即输入"
        }
        Button {
            objectName: "subtitle-delete-button"
            text: "删除"
            enabled: Timeline.selectedCueId !== ""
            onClicked: Timeline.deleteSelectedCue()
        }
    }

    // Direct editor for the selected cue. This is the one-click path: after
    // "+ 字幕" the text is selected so typing replaces the placeholder.
    TextField {
        id: topText
        objectName: "subtitle-text"
        Layout.fillWidth: true
        enabled: Timeline.selectedCueId !== ""
        placeholderText: Timeline.selectedCueId !== ""
                         ? "输入字幕文本" : "先添加或选中一条字幕"
        property string editBuffer: ""
        onTextChanged: {
            if (activeFocus)
                editBuffer = text
        }
        onEditingFinished: panel.commitTopText()
        onActiveFocusChanged: {
            if (activeFocus)
                editBuffer = text
        }
        Binding {
            target: topText
            property: "text"
            value: panel.selectedCueText()
            when: !topText.activeFocus
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Label { text: "样式"; color: "#cccccc"; font.pixelSize: 11 }
        ComboBox {
            id: styleBox
            objectName: "subtitle-style"
            Layout.fillWidth: true
            model: App.subtitleStyles()
            textRole: "name"
            valueRole: "id"
            enabled: Timeline.selectedCueId !== ""
            onActivated: Timeline.applySubtitleStyle(
                             currentValue, Timeline.selectedCueId)
        }
    }

    // Timing fields edit the selected cue in seconds and preserve its text,
    // style and position (the controller copies the whole cue).
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Label { text: "开始（秒）"; color: "#cccccc"; font.pixelSize: 11 }
        TextField {
            id: startField
            objectName: "subtitle-start"
            Layout.fillWidth: true
            enabled: Timeline.selectedCueId !== ""
            font.pixelSize: 11
            onEditingFinished: panel.commitCueTiming()
            Binding {
                target: startField
                property: "text"
                value: (panel.cueField("startUs", 0) / 1e6).toFixed(3)
                when: !startField.activeFocus
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        Label { text: "结束（秒）"; color: "#cccccc"; font.pixelSize: 11 }
        TextField {
            id: endField
            objectName: "subtitle-end"
            Layout.fillWidth: true
            enabled: Timeline.selectedCueId !== ""
            font.pixelSize: 11
            onEditingFinished: panel.commitCueTiming()
            Binding {
                target: endField
                property: "text"
                value: (panel.cueField("endUs", 0) / 1e6).toFixed(3)
                when: !endField.activeFocus
            }
        }
    }

    Label { text: "位置 X（‰）"; color: "#cccccc" }
    Slider {
        id: positionX
        objectName: "subtitle-position-x"
        Layout.fillWidth: true
        from: 0
        to: 1000
        enabled: Timeline.selectedCueId !== ""
        property real pendingValue: 500
        onMoved: {
            if (pressed) {
                pendingValue = value
            } else if (enabled) {
                panel.commitPositionX(value)
            }
        }
        onPressedChanged: {
            if (pressed) {
                pendingValue = value
            } else if (enabled) {
                panel.commitPositionX(pendingValue)
            }
        }
        Binding {
            target: positionX
            property: "value"
            value: panel.cueField("positionXPermille", 500)
            when: !positionX.pressed
        }
    }

    Label { text: "位置 Y（‰）"; color: "#cccccc" }
    Slider {
        id: positionY
        objectName: "subtitle-position-y"
        Layout.fillWidth: true
        from: 0
        to: 1000
        enabled: Timeline.selectedCueId !== ""
        property real pendingValue: 880
        onMoved: {
            if (pressed) {
                pendingValue = value
            } else if (enabled) {
                panel.commitPositionY(value)
            }
        }
        onPressedChanged: {
            if (pressed) {
                pendingValue = value
            } else if (enabled) {
                panel.commitPositionY(pendingValue)
            }
        }
        Binding {
            target: positionY
            property: "value"
            value: panel.cueField("positionYPermille", 880)
            when: !positionY.pressed
        }
    }

    ListView {
        id: cueList
        objectName: "subtitle-cue-list"
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(220, contentHeight)
        clip: true
        model: Timeline.timelineModel

        delegate: Rectangle {
            width: ListView.view.width
            visible: model.kind === "cue"
            height: visible ? 58 : 0
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
                    ToolButton {
                        objectName: "cue-delete-" + model.itemId
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
                    id: cueText
                    objectName: "cue-text-" + model.itemId
                    Layout.fillWidth: true
                    text: model.text
                    placeholderText: "字幕文本"
                    // Capture what the user typed before the focus-loss
                    // Binding below restores the model value.
                    property string editBuffer: model.text
                    onTextChanged: {
                        if (activeFocus)
                            editBuffer = text
                    }
                    onEditingFinished: {
                        if (editBuffer !== model.text)
                            Timeline.setCueText(model.itemId, editBuffer)
                    }
                    onActiveFocusChanged: {
                        if (activeFocus) {
                            editBuffer = text
                            Timeline.selectCue(model.itemId)
                        }
                    }
                    // Undo/external edits must win while the user is not
                    // typing in this field.
                    Binding {
                        target: cueText
                        property: "text"
                        value: model.text
                        when: !cueText.activeFocus
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
