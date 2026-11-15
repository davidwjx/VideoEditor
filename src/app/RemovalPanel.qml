import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// AI removal entry point. The first screen only offers "enter mask mode"; the
// brush/box/erase toolbar, keyframes and layer switches appear once the user is
// editing a mask, mirroring the inspector's progressive-disclosure language.
ColumnLayout {
    id: root
    objectName: "removal-panel"
    spacing: 5

    // The context property only exists when the app (or a test host) installs
    // it; guard so loading Main.qml standalone still works.
    property var removal: (typeof Removal !== "undefined") ? Removal : null
    readonly property bool active: removal !== null && removal.maskMode
    readonly property bool hasClip: Timeline.selectedClipId !== ""
    property bool advancedOpen: false

    function timecodeLabel(us) {
        return (us / 1e6).toFixed(2) + "s"
    }

    Label {
        text: "AI 消除"
        font.bold: true
        color: "#cfcfcf"
        Layout.topMargin: 2
    }

    Button {
        objectName: "removal-enter-button"
        Layout.fillWidth: true
        text: root.active ? "遮罩编辑中…" : "AI 消除"
        enabled: root.removal !== null && root.hasClip && !root.active
        onClicked: root.removal.beginMask(Timeline.selectedClipId)
        ToolTip.visible: hovered
        ToolTip.text: "为选中的视频片段绘制消除遮罩"
    }

    Label {
        objectName: "removal-error"
        visible: root.removal !== null && root.removal.error.length > 0
        text: root.removal !== null ? root.removal.error : ""
        color: "#ff8a80"
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        Layout.fillWidth: true
        MouseArea {
            anchors.fill: parent
            onClicked: if (root.removal !== null) root.removal.clearError()
        }
    }

    // ------------------------------------------------------------------
    // Mask mode toolbar.
    // ------------------------------------------------------------------
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 5
        visible: root.active

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Button {
                objectName: "removal-brush-button"
                text: "画笔"
                highlighted: root.removal !== null
                             && !root.removal.boxMode
                             && !root.removal.eraseMode
                onClicked: {
                    root.removal.boxMode = false
                    root.removal.eraseMode = false
                }
            }
            Button {
                objectName: "removal-box-button"
                text: "框选"
                highlighted: root.removal !== null && root.removal.boxMode
                onClicked: {
                    root.removal.boxMode = true
                    root.removal.eraseMode = false
                }
            }
            Button {
                objectName: "removal-erase-button"
                text: "擦除"
                highlighted: root.removal !== null && root.removal.eraseMode
                onClicked: {
                    root.removal.eraseMode = true
                    root.removal.boxMode = false
                }
            }
        }

        Label { text: "画笔大小"; color: "#cccccc"; font.pixelSize: 11 }
        Slider {
            id: brushSlider
            objectName: "removal-brush-size"
            Layout.fillWidth: true
            from: 1
            to: 200
            stepSize: 1
            onMoved: if (root.removal !== null)
                          root.removal.brushSize = Math.round(value)
            Binding {
                target: brushSlider
                property: "value"
                value: root.removal !== null ? root.removal.brushSize : 24
                when: !brushSlider.pressed
            }
        }

        Label { text: "羽化（像素）"; color: "#cccccc"; font.pixelSize: 11 }
        Slider {
            id: featherSlider
            objectName: "removal-feather-slider"
            Layout.fillWidth: true
            from: 0
            to: 64
            stepSize: 1
            property int pendingValue: 0
            onMoved: pendingValue = Math.round(value)
            onPressedChanged: {
                if (!pressed && root.removal !== null)
                    root.removal.setFeatherPx(pendingValue)
            }
            Binding {
                target: featherSlider
                property: "value"
                value: root.removal !== null ? root.removal.featherPx : 4
                when: !featherSlider.pressed
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4

            Button {
                objectName: "removal-add-keyframe"
                text: "添加关键帧"
                onClicked: root.removal.addMaskKeyframeAtPlayhead()
            }
            Button {
                objectName: "removal-delete-keyframe"
                text: "删除关键帧"
                enabled: root.removal !== null
                         && root.removal.maskKeyframes.length > 1
                onClicked: root.removal.removeMaskKeyframe(
                               root.removal.maskKeyframes.length - 1)
            }
            Button {
                objectName: "removal-exit-button"
                text: "退出遮罩"
                onClicked: root.removal.endMask()
            }
        }

        ListView {
            id: keyframeList
            objectName: "removal-keyframe-list"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(96, contentHeight)
            clip: true
            model: root.removal !== null ? root.removal.maskKeyframes : []
            spacing: 2

            delegate: RowLayout {
                required property int index
                required property var modelData

                width: keyframeList.width
                spacing: 4

                Label {
                    text: root.timecodeLabel(modelData.timeUs)
                    color: "#cccccc"
                    font.family: "Consolas"
                    font.pixelSize: 11
                }
                Label {
                    text: "羽化 " + modelData.featherPx
                    color: "#8a8a8a"
                    font.pixelSize: 11
                    Layout.fillWidth: true
                }
                ToolButton {
                    objectName: "removal-keyframe-delete-" + index
                    text: "删"
                    padding: 2
                    font.pixelSize: 11
                    enabled: root.removal.maskKeyframes.length > 1
                    onClicked: root.removal.removeMaskKeyframe(index)
                }
            }

            Label {
                anchors.centerIn: parent
                visible: keyframeList.count === 0
                text: "暂无关键帧"
                color: "#777777"
                font.pixelSize: 11
            }
        }

        CheckBox {
            id: layerEnabled
            objectName: "removal-layer-enabled"
            text: "启用消除层"
            enabled: root.removal !== null && root.removal.hasLayer
            onToggled: if (root.removal !== null)
                           root.removal.setLayerEnabled(checked)
            Binding {
                target: layerEnabled
                property: "checked"
                value: root.removal !== null && root.removal.layerEnabled
                when: !layerEnabled.pressed
            }
        }

        Button {
            objectName: "removal-delete-layer"
            Layout.fillWidth: true
            text: "删除消除层"
            enabled: root.removal !== null && root.removal.hasLayer
            onClicked: root.removal.deleteLayer()
        }
    }

    Button {
        objectName: "removal-advanced-toggle"
        Layout.fillWidth: true
        visible: root.active
        text: root.advancedOpen ? "收起高级 ▴" : "高级 ▾"
        onClicked: root.advancedOpen = !root.advancedOpen
    }

    ColumnLayout {
        objectName: "removal-advanced-panel"
        Layout.fillWidth: true
        spacing: 4
        visible: root.active && root.advancedOpen

        Label {
            text: "模型、分辨率与稳定化将在后续任务中开放。"
            color: "#8a8a8a"
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            Layout.fillWidth: true
        }
    }
}
