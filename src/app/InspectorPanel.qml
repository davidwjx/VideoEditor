import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Properties for the selected clip. Sliders only commit to the controller
// when the mouse is released (or on a keyboard step), so one drag is one
// undo step.
ScrollView {
    id: inspector

    function hasClip() {
        return Timeline.selectedClipId !== ""
    }

    function commitFades() {
        Timeline.setClipFades(Timeline.selectedClipId,
                              Math.round(fadeIn.value * 1e6),
                              Math.round(fadeOut.value * 1e6))
    }

    ColumnLayout {
        width: inspector.availableWidth
        spacing: 4

        Label {
            text: "片段属性"
            font.bold: true
            color: "#dddddd"
        }
        Label {
            text: inspector.hasClip() ? "已选择片段" : "未选择片段"
            color: "#999999"
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "变速"; color: "#cccccc" }
            ComboBox {
                id: speedBox
                Layout.fillWidth: true
                model: ["0.25x", "0.5x", "1x", "1.5x", "2x", "4x"]
                currentIndex: 2
                enabled: inspector.hasClip()
                onActivated: {
                    const speeds = [250, 500, 1000, 1500, 2000, 4000]
                    Timeline.setClipSpeed(Timeline.selectedClipId, speeds[index],
                                          preservePitch.checked)
                }
            }
        }
        CheckBox {
            id: preservePitch
            text: "保持音高"
            checked: true
            enabled: inspector.hasClip()
        }

        Label { text: "音量"; color: "#cccccc" }
        Slider {
            id: volume
            Layout.fillWidth: true
            from: 0
            to: 200
            value: 100
            enabled: inspector.hasClip()
            onMoved: if (!pressed && inspector.hasClip())
                          Timeline.setClipVolume(Timeline.selectedClipId,
                                                 Math.round(value * 10))
            onPressedChanged: if (!pressed && inspector.hasClip())
                                  Timeline.setClipVolume(Timeline.selectedClipId,
                                                         Math.round(value * 10))
        }

        Label { text: "声像"; color: "#cccccc" }
        Slider {
            id: pan
            Layout.fillWidth: true
            from: -100
            to: 100
            value: 0
            enabled: inspector.hasClip()
            onMoved: if (!pressed && inspector.hasClip())
                          Timeline.setClipPan(Timeline.selectedClipId,
                                              Math.round(value * 10))
            onPressedChanged: if (!pressed && inspector.hasClip())
                                  Timeline.setClipPan(Timeline.selectedClipId,
                                                      Math.round(value * 10))
        }

        Label { text: "淡入（秒）"; color: "#cccccc" }
        Slider {
            id: fadeIn
            Layout.fillWidth: true
            from: 0
            to: 5
            value: 0
            enabled: inspector.hasClip()
            onMoved: if (!pressed && inspector.hasClip())
                          commitFades()
            onPressedChanged: if (!pressed && inspector.hasClip())
                                  commitFades()
        }

        Label { text: "淡出（秒）"; color: "#cccccc" }
        Slider {
            id: fadeOut
            Layout.fillWidth: true
            from: 0
            to: 5
            value: 0
            enabled: inspector.hasClip()
            onMoved: if (!pressed && inspector.hasClip())
                          commitFades()
            onPressedChanged: if (!pressed && inspector.hasClip())
                                  commitFades()
        }

        Label { text: "画面缩放"; color: "#cccccc" }
        Slider {
            id: scale
            Layout.fillWidth: true
            from: 10
            to: 400
            value: 100
            enabled: inspector.hasClip()
            onMoved: if (!pressed && inspector.hasClip())
                          Timeline.setClipTransform(Timeline.selectedClipId,
                                                    Math.round(value * 10), 0, 0,
                                                    Math.round(opacity.value * 10))
            onPressedChanged: if (!pressed && inspector.hasClip())
                                  Timeline.setClipTransform(Timeline.selectedClipId,
                                                            Math.round(value * 10), 0, 0,
                                                            Math.round(opacity.value * 10))
        }

        Label { text: "不透明度"; color: "#cccccc" }
        Slider {
            id: opacity
            Layout.fillWidth: true
            from: 0
            to: 100
            value: 100
            enabled: inspector.hasClip()
            onMoved: if (!pressed && inspector.hasClip())
                          Timeline.setClipTransform(Timeline.selectedClipId,
                                                    Math.round(scale.value * 10), 0, 0,
                                                    Math.round(value * 10))
            onPressedChanged: if (!pressed && inspector.hasClip())
                                  Timeline.setClipTransform(Timeline.selectedClipId,
                                                            Math.round(scale.value * 10), 0, 0,
                                                            Math.round(value * 10))
        }
    }
}
