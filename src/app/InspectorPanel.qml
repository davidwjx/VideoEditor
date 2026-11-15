import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Properties for the selected clip. Controls are initialized from
// Timeline.selectedClip and only commit to the controller on release, so one
// drag is one undo step and unrelated fields are preserved by the dedicated
// single-field setters.
ScrollView {
    id: inspector

    readonly property var clip: Timeline.selectedClip

    function hasClip() {
        return Timeline.selectedClipId !== ""
    }

    function clipField(name, fallback) {
        const map = Timeline.selectedClip
        const value = map ? map[name] : undefined
        return value === undefined ? fallback : value
    }

    function speedIndexFor(milli) {
        const speeds = [250, 500, 1000, 1500, 2000, 4000]
        const index = speeds.indexOf(milli)
        return index < 0 ? 2 : index
    }

    function currentSpeedMilli() {
        return [250, 500, 1000, 1500, 2000, 4000][
                    speedIndexFor(clipField("speedMilli", 1000))]
    }

    function commitFadeIn(seconds) {
        if (hasClip())
            Timeline.setClipFadeIn(Timeline.selectedClipId,
                                   Math.round(seconds * 1e6))
    }

    function commitFadeOut(seconds) {
        if (hasClip())
            Timeline.setClipFadeOut(Timeline.selectedClipId,
                                    Math.round(seconds * 1e6))
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
                enabled: inspector.hasClip()
                onActivated: {
                    const speeds = [250, 500, 1000, 1500, 2000, 4000]
                    Timeline.setClipSpeed(Timeline.selectedClipId, speeds[index],
                                          preservePitch.checked)
                }
            }
            Binding {
                target: speedBox
                property: "currentIndex"
                value: inspector.speedIndexFor(inspector.clipField("speedMilli", 1000))
                when: !speedBox.pressed
            }
        }
        CheckBox {
            id: preservePitch
            text: "保持音高"
            enabled: inspector.hasClip()
            onClicked: if (inspector.hasClip())
                           Timeline.setClipSpeed(Timeline.selectedClipId,
                                                 inspector.currentSpeedMilli(),
                                                 checked)
            Binding {
                target: preservePitch
                property: "checked"
                value: inspector.clipField("preservePitch", false) === true
                when: !preservePitch.pressed
            }
        }

        Label { text: "音量"; color: "#cccccc" }
        Slider {
            id: volume
            objectName: "inspector-volume"
            Layout.fillWidth: true
            from: 0
            to: 200
            enabled: inspector.hasClip()
            property real pendingValue: 100
            onMoved: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    Timeline.setClipVolume(Timeline.selectedClipId,
                                           Math.round(value * 10))
                }
            }
            onPressedChanged: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    Timeline.setClipVolume(Timeline.selectedClipId,
                                           Math.round(pendingValue * 10))
                }
            }
            Binding {
                target: volume
                property: "value"
                value: inspector.clipField("volumeMilli", 1000) / 10
                when: !volume.pressed
            }
        }

        Label { text: "声像"; color: "#cccccc" }
        Slider {
            id: pan
            objectName: "inspector-pan"
            Layout.fillWidth: true
            from: -100
            to: 100
            enabled: inspector.hasClip()
            property real pendingValue: 0
            onMoved: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    Timeline.setClipPan(Timeline.selectedClipId,
                                        Math.round(value * 10))
                }
            }
            onPressedChanged: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    Timeline.setClipPan(Timeline.selectedClipId,
                                        Math.round(pendingValue * 10))
                }
            }
            Binding {
                target: pan
                property: "value"
                value: inspector.clipField("panMilli", 0) / 10
                when: !pan.pressed
            }
        }

        Label { text: "淡入（秒）"; color: "#cccccc" }
        Slider {
            id: fadeIn
            objectName: "inspector-fadeIn"
            Layout.fillWidth: true
            from: 0
            to: 5
            enabled: inspector.hasClip()
            property real pendingValue: 0
            onMoved: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    inspector.commitFadeIn(value)
                }
            }
            onPressedChanged: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    inspector.commitFadeIn(pendingValue)
                }
            }
            Binding {
                target: fadeIn
                property: "value"
                value: inspector.clipField("fadeInUs", 0) / 1e6
                when: !fadeIn.pressed
            }
        }

        Label { text: "淡出（秒）"; color: "#cccccc" }
        Slider {
            id: fadeOut
            objectName: "inspector-fadeOut"
            Layout.fillWidth: true
            from: 0
            to: 5
            enabled: inspector.hasClip()
            property real pendingValue: 0
            onMoved: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    inspector.commitFadeOut(value)
                }
            }
            onPressedChanged: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    inspector.commitFadeOut(pendingValue)
                }
            }
            Binding {
                target: fadeOut
                property: "value"
                value: inspector.clipField("fadeOutUs", 0) / 1e6
                when: !fadeOut.pressed
            }
        }

        Label { text: "画面缩放"; color: "#cccccc" }
        Slider {
            id: scale
            objectName: "inspector-scale"
            Layout.fillWidth: true
            from: 10
            to: 400
            enabled: inspector.hasClip()
            property real pendingValue: 100
            onMoved: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    Timeline.setClipScale(Timeline.selectedClipId,
                                          Math.round(value * 10))
                }
            }
            onPressedChanged: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    Timeline.setClipScale(Timeline.selectedClipId,
                                          Math.round(pendingValue * 10))
                }
            }
            Binding {
                target: scale
                property: "value"
                value: inspector.clipField("scaleMilli", 1000) / 10
                when: !scale.pressed
            }
        }

        Label { text: "不透明度"; color: "#cccccc" }
        Slider {
            id: opacity
            objectName: "inspector-opacity"
            Layout.fillWidth: true
            from: 0
            to: 100
            enabled: inspector.hasClip()
            property real pendingValue: 100
            onMoved: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    Timeline.setClipOpacity(Timeline.selectedClipId,
                                            Math.round(value * 10))
                }
            }
            onPressedChanged: {
                if (pressed) {
                    pendingValue = value
                } else if (inspector.hasClip()) {
                    Timeline.setClipOpacity(Timeline.selectedClipId,
                                            Math.round(pendingValue * 10))
                }
            }
            Binding {
                target: opacity
                property: "value"
                value: inspector.clipField("opacityMilli", 1000) / 10
                when: !opacity.pressed
            }
        }
    }
}
