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
    readonly property var speedValues: [250, 500, 1000, 1500, 2000, 4000]
    readonly property var presetSpeedLabels: ["0.25x", "0.5x", "1x", "1.5x",
                                              "2x", "4x"]
    // A custom speed gets its own row so the combo never displays a wrong
    // preset (previously any non-preset value showed as 1x).
    readonly property var speedModel: {
        const labels = presetSpeedLabels.slice()
        const actual = clipField("speedMilli", 1000)
        if (speedIndexFor(actual) < 0)
            labels.push("自定义 " + formatSpeed(actual))
        return labels
    }

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
        return speeds.indexOf(milli) // -1 keeps a custom speed visible
    }

    function formatSpeed(milli) {
        const value = milli / 1000
        return (Math.round(value * 100) / 100) + "x"
    }

    function currentSpeedMilli() {
        // Use the clip's actual speed so toggling preserve-pitch does not
        // silently reset a custom value to 1x.
        return clipField("speedMilli", 1000)
    }

    function isAudioClip() {
        return clipField("hasAudioControls", false) === true
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

    // The controller stores keyframe time relative to the clip start, while
    // the playhead is a timeline-global position.
    function addKeyframeAtPlayhead() {
        if (!hasClip())
            return
        const startUs = clipField("startUs", 0)
        Timeline.addVolumeKeyframe(clipField("trackId", ""),
                                   Timeline.selectedClipId,
                                   Math.round(Timeline.positionUs - startUs),
                                   clipField("volumeMilli", 1000))
    }

    function removeKeyframe(index) {
        if (!hasClip())
            return
        Timeline.removeVolumeKeyframe(clipField("trackId", ""),
                                      Timeline.selectedClipId, index)
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
                objectName: "inspector-speed"
                Layout.fillWidth: true
                editable: true
                model: inspector.speedModel
                enabled: inspector.hasClip()
                onActivated: {
                    if (index < inspector.speedValues.length) {
                        Timeline.setClipSpeed(Timeline.selectedClipId,
                                              inspector.speedValues[index],
                                              preservePitch.checked)
                    }
                }
                // Free-form entry keeps non-preset speeds editable instead of
                // displaying 1x for them.
                onAccepted: {
                    const parsed = parseFloat(text)
                    if (inspector.hasClip() && isFinite(parsed)
                        && parsed >= 0.25 && parsed <= 4) {
                        Timeline.setClipSpeed(Timeline.selectedClipId,
                                              Math.round(parsed * 1000),
                                              preservePitch.checked)
                    } else {
                        editText = inspector.formatSpeed(
                                       inspector.clipField("speedMilli", 1000))
                    }
                }
            }
            Binding {
                target: speedBox
                property: "currentIndex"
                value: {
                    const index = inspector.speedIndexFor(
                                      inspector.clipField("speedMilli", 1000))
                    return index < 0 ? speedBox.model.length - 1 : index
                }
                when: !speedBox.pressed && !speedBox.activeFocus
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

        Label {
            visible: inspector.hasClip() && !inspector.isAudioClip()
            text: "音频参数请选择链接的音频片段（A 轨）"
            color: "#b08a5a"
            font.pixelSize: 10
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
        }
        Label { text: "音量"; color: "#cccccc" }
        Slider {
            id: volume
            objectName: "inspector-volume"
            Layout.fillWidth: true
            from: 0
            to: 200
            enabled: inspector.hasClip() && inspector.isAudioClip()
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

        Label { text: "音量关键帧"; color: "#cccccc" }
        ListView {
            id: keyframeList
            objectName: "inspector-keyframe-list"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(96, contentHeight)
            clip: true
            model: inspector.clipField("keyframes", [])

            delegate: RowLayout {
                required property int index
                required property var modelData

                width: ListView.view.width
                spacing: 4

                Label {
                    text: (modelData.timeUs / 1e6).toFixed(2) + "s"
                    color: "#999999"
                    font.pixelSize: 11
                }
                Label {
                    text: Math.round(modelData.volumeMilli / 10) + "%"
                    color: "#cccccc"
                    font.pixelSize: 11
                    Layout.fillWidth: true
                }
                ToolButton {
                    objectName: "inspector-keyframe-delete-" + index
                    text: "删"
                    padding: 2
                    font.pixelSize: 11
                    onClicked: inspector.removeKeyframe(index)
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "inspector-add-keyframe"
                text: "添加关键帧"
                enabled: inspector.hasClip() && inspector.isAudioClip()
                onClicked: inspector.addKeyframeAtPlayhead()
            }
            Label {
                text: "共 " + inspector.clipField("keyframeCount", 0)
                color: "#999999"
                font.pixelSize: 11
            }
        }

        Label { text: "声像"; color: "#cccccc" }
        Slider {
            id: pan
            objectName: "inspector-pan"
            Layout.fillWidth: true
            from: -100
            to: 100
            enabled: inspector.hasClip() && inspector.isAudioClip()
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
            enabled: inspector.hasClip() && inspector.isAudioClip()
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
            enabled: inspector.hasClip() && inspector.isAudioClip()
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
