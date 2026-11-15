import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Preset-first export dialog: the common choices stay visible, while codec
// tuning, subtitles and container options live behind "高级". Progress,
// remaining time, cancel and resume are always reachable without scrolling.
Dialog {
    id: root
    objectName: "exportDialog"
    title: "导出视频"
    modal: true
    width: 660
    anchors.centerIn: parent
    standardButtons: Dialog.NoButton
    // Esc must not dismiss the dialog while an export is running.
    closePolicy: Export.running ? Popup.NoAutoClose : Popup.CloseOnEscape

    property bool advancedOpen: false

    function openDialog() {
        Export.refreshCapabilities()
        open()
    }

    ColumnLayout {
        width: 620
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Label { text: "预设" }
            ComboBox {
                id: presetCombo
                objectName: "presetCombo"
                Layout.fillWidth: true
                // Keep the dialog to the four common presets; the MPEG-4
                // compatibility preset remains reachable through 视频编码.
                model: Export.presets.slice(0, 4)
                textRole: "name"
                valueRole: "id"
                currentIndex: 0
                // Imperative sync instead of a currentIndex binding: user
                // activation and controller updates then never fight.
                function syncFromController() {
                    const presets = Export.presets
                    for (let i = 0; i < presets.length && i < 4; ++i) {
                        if (presets[i].id === Export.presetId) {
                            if (currentIndex !== i)
                                currentIndex = i
                            return
                        }
                    }
                }
                Component.onCompleted: syncFromController()
                onActivated: Export.applyPreset(currentValue)
                Connections {
                    target: Export
                    function onPresetIdChanged() { presetCombo.syncFromController() }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "视频编码" }
            ComboBox {
                id: codecCombo
                objectName: "codecCombo"
                Layout.preferredWidth: 220
                model: [
                    { text: "H.264（MP4）", value: "h264" },
                    { text: "H.265/HEVC（仅硬件）", value: "h265" },
                    { text: "MPEG-4 Part 2（兼容）", value: "mpeg4" }
                ]
                textRole: "text"
                valueRole: "value"
                currentIndex: 0
                function syncFromController() {
                    const index = Export.codec === "h264" ? 0
                                : Export.codec === "h265" ? 1 : 2
                    if (currentIndex !== index)
                        currentIndex = index
                }
                Component.onCompleted: syncFromController()
                onActivated: Export.codec = currentValue
                Connections {
                    target: Export
                    function onCodecChanged() { codecCombo.syncFromController() }
                }
            }
            Label {
                objectName: "h265Reason"
                visible: Export.codec === "h265" && !Export.h265Available
                text: Export.h265UnavailableReason
                color: "#e67e22"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 12
            rowSpacing: 6

            Label { text: "分辨率" }
            Label { text: Export.width + " × " + Export.height }
            Label { text: "帧率" }
            Label {
                objectName: "fpsValue"
                text: Export.fpsDen === 1
                      ? Export.fpsNum + " fps"
                      : Export.fpsNum + "/" + Export.fpsDen + " fps"
            }
            Label { text: "码率" }
            Label { text: Export.bitrateKbps + " kbps" }
            Label { text: "编码器" }
            Label {
                text: Export.encoderName.length > 0 ? Export.encoderName : "自动选择"
                Layout.fillWidth: true
                elide: Text.ElideRight
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "advancedToggle"
                text: root.advancedOpen ? "收起高级 ▴" : "高级 ▾"
                onClicked: root.advancedOpen = !root.advancedOpen
            }
            Item { Layout.fillWidth: true }
            Label {
                text: "预设会决定分辨率、帧率和码率；需要细调时展开高级设置。"
                color: "#9a9a9a"
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
        }

        ColumnLayout {
            objectName: "advancedPanel"
            Layout.fillWidth: true
            visible: root.advancedOpen
            spacing: 8

            MenuSeparator {
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                Label { text: "质量档位" }
                ComboBox {
                    id: qualityCombo
                    objectName: "qualityCombo"
                    Layout.preferredWidth: 160
                    model: ["流畅", "标准", "高质量"]
                    currentIndex: 1
                    function syncFromController() {
                        const index = Math.max(0, Math.min(2, Export.quality))
                        if (currentIndex !== index)
                            currentIndex = index
                    }
                    Component.onCompleted: syncFromController()
                    onActivated: Export.quality = currentIndex
                    Connections {
                        target: Export
                        function onQualityChanged() {
                            qualityCombo.syncFromController()
                        }
                    }
                }
                Label { text: "码率 (kbps)" }
                TextField {
                    id: bitrateField
                    objectName: "bitrateField"
                    Layout.preferredWidth: 140
                    text: ""
                    validator: IntValidator { bottom: 100; top: 200000 }
                    function syncFromController(force) {
                        if (force || !activeFocus)
                            text = String(Export.bitrateKbps)
                    }
                    Component.onCompleted: syncFromController(true)
                    onEditingFinished: {
                        Export.bitrateKbps = parseInt(text) || 0
                        syncFromController(true)
                    }
                    Connections {
                        target: Export
                        function onBitrateKbpsChanged() {
                            bitrateField.syncFromController(false)
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                CheckBox {
                    text: "优先硬件编码"
                    checked: Export.hardwarePreferred
                    onToggled: Export.hardwarePreferred = checked
                }
                CheckBox {
                    text: "烧录字幕"
                    checked: Export.burnSubtitles
                    onToggled: Export.burnSubtitles = checked
                }
                CheckBox {
                    text: "同时导出 SRT"
                    checked: Export.writeSrt
                    onToggled: Export.writeSrt = checked
                }
            }

            Label {
                text: "音频固定为 AAC-LC 48 kHz 双声道；H.265 仅在硬件可用时可选。"
                color: "#9a9a9a"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "输出文件" }
            TextField {
                id: outputField
                objectName: "outputPathField"
                Layout.fillWidth: true
                text: ""
                function syncFromController(force) {
                    if (force || !activeFocus)
                        text = Export.outputPath
                }
                Component.onCompleted: syncFromController(true)
                onEditingFinished: {
                    Export.setOutputPath(text)
                    syncFromController(true)
                }
                Connections {
                    target: Export
                    function onOutputPathChanged() {
                        outputField.syncFromController(false)
                    }
                }
            }
            Button {
                text: "浏览…"
                onClicked: saveDialog.open()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            // Only the live run owns the progress bar; a finished export's
            // 100 % must not linger when the dialog is reopened.
            visible: Export.running
            Label { text: Export.stageText }
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 1
                value: Export.progress
            }
            Label {
                text: Math.round(Export.progress * 100) + "%"
                Layout.preferredWidth: 46
                horizontalAlignment: Text.AlignRight
            }
        }

        Label {
            Layout.fillWidth: true
            visible: Export.statusText.length > 0
            text: Export.statusText
            wrapMode: Text.WordWrap
            color: "#cfcfcf"
        }
        Label {
            Layout.fillWidth: true
            visible: Export.error.length > 0
            text: Export.error
            color: "#e74c3c"
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: Export.validationSummary.length > 0 && !Export.running
            text: Export.validationSummary
            color: "#7fbf7f"
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: Export.validationError.length > 0
                     && Export.codec !== "h265" && !Export.running
            text: "无法开始导出：" + Export.validationError
            color: "#e74c3c"
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            Button {
                objectName: "startExportButton"
                text: "开始导出"
                highlighted: true
                enabled: !Export.running && Export.validationError.length === 0
                onClicked: Export.startExport()
            }
            Button {
                objectName: "resumeExportButton"
                text: "继续上次导出"
                visible: !Export.running && Export.canResume
                onClicked: Export.resumeExport()
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "cancelExportButton"
                text: "取消"
                visible: Export.running && Export.canCancel
                onClicked: Export.cancelExport()
            }
            Button {
                text: "关闭"
                enabled: !Export.running
                onClicked: root.close()
            }
        }
    }

    FileDialog {
        id: saveDialog
        title: "选择导出文件"
        fileMode: FileDialog.SaveFile
        nameFilters: ["MP4 视频 (*.mp4)"]
        defaultSuffix: "mp4"
        onAccepted: Export.setOutputPath(selectedFile.toString())
    }
}
