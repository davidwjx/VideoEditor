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
    readonly property bool ready: removal !== null && removal.featureEnabled
    readonly property bool hasKeyframes: removal !== null
                                         && removal.maskKeyframes.length > 0
    readonly property string actionDisabledReason: {
        if (removal === null)
            return "AI 消除未初始化。"
        if (!removal.featureEnabled)
            return removal.featureDisabledReason
        if (removal.generating)
            return "AI 任务运行中，请等待完成或取消。"
        if (!hasKeyframes)
            return "请先在画面上涂抹要消除的物体；第一笔会自动创建关键帧。"
        return ""
    }
    readonly property string disclaimer:
        "内部技术预览：模型未取得商业许可，仅用于内部技术验证，禁止分发。"

    function timecodeLabel(us) {
        return (us / 1e6).toFixed(2) + "s"
    }

    function modeIndex(mode) {
        return mode === "quality" ? 1 : 0
    }

    function heightIndex(height) {
        if (height >= 1080)
            return 2
        if (height >= 720)
            return 1
        return 0
    }

    function stabilizationIndex(value) {
        if (value <= 0)
            return 0
        if (value <= 30)
            return 1
        if (value <= 60)
            return 2
        return 3
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
        objectName: "removal-gate-reason"
        visible: root.removal !== null && !root.removal.featureEnabled
        text: root.removal !== null ? root.removal.featureDisabledReason : ""
        color: "#e67e22"
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        Layout.fillWidth: true
    }

    Label {
        objectName: "removal-technical-preview"
        // The disclaimer text is always present so the source-tree QML test can
        // assert it; only the approved internal run makes it visible.
        visible: root.removal !== null && root.removal.technicalPreview
        text: root.disclaimer
        color: "#e6c15a"
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        Layout.fillWidth: true
    }

    // ------------------------------------------------------------------
    // GPU live preview. It repairs the current frame/window in memory and is
    // not a replacement for "生成修复", which writes the on-disk cache.
    // ------------------------------------------------------------------
    CheckBox {
        id: livePreviewToggle
        objectName: "removal-live-preview-toggle"
        visible: root.removal !== null
        text: "实时预览"
        enabled: root.removal !== null
                 && (root.removal.livePreview
                     || (root.removal.featureEnabled && root.removal.hasLayer
                         && root.removal.livePreviewAvailable))
        onToggled: if (root.removal !== null)
                       root.removal.setLivePreview(checked)
        Binding {
            target: livePreviewToggle
            property: "checked"
            value: root.removal !== null && root.removal.livePreview
            when: !livePreviewToggle.pressed
        }
        ToolTip.visible: hovered
        ToolTip.text: root.removal !== null && root.removal.livePreviewAvailable
                    ? "实时预览当前帧及附近窗口；不写入磁盘缓存，也不能替代“生成修复”。"
                    : "需要先完成 AI 跟踪。仅实时预览当前帧及附近窗口，"
                      + "不会写入磁盘缓存，不能替代“生成修复”。"
                      + "模型可用时使用 DirectML，否则回退 CPU（较慢）。"
    }

    Label {
        objectName: "removal-live-preview-status"
        visible: root.removal !== null
        text: root.removal !== null ? root.removal.livePreviewStatus : ""
        color: "#9e9e9e"
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        Layout.fillWidth: true
    }

    // ------------------------------------------------------------------
    // Layer enablement stays reachable outside mask mode so a tracked layer
    // can be reviewed and toggled without re-entering the editor.
    // ------------------------------------------------------------------
    Label {
        objectName: "removal-layer-disabled-chip"
        visible: root.removal !== null && root.removal.hasLayer
                 && !root.removal.layerEnabled
        text: "消除层已停用"
        color: "#e67e22"
        font.pixelSize: 11
        Layout.fillWidth: true
    }

    CheckBox {
        id: layerEnabled
        objectName: "removal-layer-enabled"
        visible: root.removal !== null && root.removal.hasLayer
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
    // Confidence, cache status and low-confidence count stay visible while
    // the panel is open.
    // ------------------------------------------------------------------
    ConfidenceBand {
        objectName: "removal-confidence-band"
        // Confidence data belongs to the layer, not to mask mode; keep it
        // visible whenever the selected clip has a removal layer.
        visible: root.removal !== null && root.removal.hasLayer
        Layout.fillWidth: true
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: root.removal !== null
        Label {
            objectName: "removal-tracking-state"
            visible: root.removal !== null && root.removal.hasLayer
            text: "跟踪：" + (root.removal !== null
                             ? root.removal.trackingStateText : "")
            color: "#bdbdbd"
            font.pixelSize: 11
        }
        Label {
            objectName: "removal-cache-status"
            text: "缓存：" + (root.removal !== null
                             ? root.removal.cacheStatusText : "")
            color: "#bdbdbd"
            font.pixelSize: 11
            Layout.fillWidth: true
            elide: Text.ElideRight
        }
        Button {
            objectName: "removal-continue-button"
            visible: root.removal !== null
                     && root.removal.canContinueGeneration
            text: "继续生成"
            font.pixelSize: 10
            padding: 3
            enabled: root.ready && !root.removal.generating
            onClicked: root.removal.generate()
        }
    }

    Label {
        objectName: "removal-low-confidence-count"
        visible: root.removal !== null && root.removal.lowConfidenceCount > 0
        text: "有 " + (root.removal !== null
                       ? root.removal.lowConfidenceCount : 0) + " 帧低置信"
        color: "#F44336"
        font.pixelSize: 11
        Layout.fillWidth: true
    }

    // ------------------------------------------------------------------
    // Compare controls (original / repair / split).
    // ------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: 4
        visible: root.removal !== null && root.removal.hasLayer

        Label { text: "对比"; color: "#cccccc"; font.pixelSize: 11 }
        ComboBox {
            id: compareCombo
            objectName: "removal-compare-toggle"
            Layout.fillWidth: true
            model: [
                { text: "修复画面", value: "repair" },
                { text: "原片", value: "original" },
                { text: "分割对比", value: "split" }
            ]
            textRole: "text"
            valueRole: "value"
            function syncCompare() {
                if (root.removal === null)
                    return
                const mode = root.removal.compareMode
                const idx = mode === "original" ? 1
                          : mode === "split" ? 2 : 0
                if (currentIndex !== idx)
                    currentIndex = idx
            }
            Component.onCompleted: syncCompare()
            onActivated: if (root.removal !== null)
                             root.removal.setCompareMode(currentValue)
            Connections {
                target: root.removal
                function onCompareModeChanged() { compareCombo.syncCompare() }
            }
        }
    }

    Slider {
        id: splitSlider
        objectName: "removal-split-slider"
        Layout.fillWidth: true
        visible: root.removal !== null && root.removal.hasLayer
                 && root.removal.compareMode === "split"
        from: 0
        to: 1000
        stepSize: 1
        onMoved: if (root.removal !== null)
                     root.removal.setSplitPermille(Math.round(value))
        Binding {
            target: splitSlider
            property: "value"
            value: root.removal !== null ? root.removal.splitPermille : 500
            when: !splitSlider.pressed
        }
    }
    Label {
        objectName: "removal-split-value"
        visible: splitSlider.visible
        text: Math.round(splitSlider.value / 10) + "%"
        color: "#9e9e9e"
        font.pixelSize: 11
        Layout.fillWidth: true
    }

    // ------------------------------------------------------------------
    // Mask mode toolbar.
    // ------------------------------------------------------------------
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 5
        visible: root.active

        // First-run guidance: the first stroke creates the first keyframe, so
        // there is no dead "add keyframe" step for a new layer.
        Label {
            objectName: "removal-first-run-hint"
            visible: root.removal !== null
                     && (!root.hasKeyframes || root.removal.currentMaskEmpty)
            text: "在预览画面上涂抹要消除的物体；第一笔会自动创建关键帧；"
                + "移动播放头可再画一个"
            color: "#e6c15a"
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            Layout.fillWidth: true
        }

        // Compact action row stays at the top of the mask section above the
        // brush/keyframe controls so track/generate are never below the fold.
        RowLayout {
            objectName: "removal-action-row"
            Layout.fillWidth: true
            spacing: 4

            Button {
                objectName: "removal-track-button"
                text: "跟踪遮罩"
                Layout.fillWidth: true
                enabled: root.ready && root.hasKeyframes
                         && !root.removal.generating
                onClicked: root.removal.track()
            }
            Button {
                objectName: "removal-generate-button"
                text: root.removal !== null
                      && root.removal.canContinueGeneration
                      ? "继续生成修复" : "生成修复"
                Layout.fillWidth: true
                highlighted: true
                enabled: root.ready && root.hasKeyframes
                         && !root.removal.generating
                onClicked: root.removal.generate()
            }
        }
        Label {
            objectName: "removal-action-disabled-reason"
            visible: root.actionDisabledReason.length > 0
            text: root.actionDisabledReason
            color: "#e67e22"
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            Layout.fillWidth: true
        }

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

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "画笔大小"
                color: "#cccccc"
                font.pixelSize: 11
                Layout.fillWidth: true
            }
            Label {
                objectName: "removal-brush-value"
                text: (root.removal !== null ? root.removal.brushSize : 24)
                      + " px"
                color: "#9e9e9e"
                font.pixelSize: 11
            }
        }
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

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: "羽化（像素）"
                color: "#cccccc"
                font.pixelSize: 11
                Layout.fillWidth: true
            }
            Label {
                objectName: "removal-feather-value"
                text: (root.removal !== null ? root.removal.featherPx : 4)
                      + " px"
                color: "#9e9e9e"
                font.pixelSize: 11
            }
        }
        Slider {
            id: featherSlider
            objectName: "removal-feather-slider"
            Layout.fillWidth: true
            from: 0
            to: 64
            stepSize: 1
            property int pendingValue: 0
            // Mirror the inspector's slider pattern: seed on press so a tap
            // without a drag commits the current value (a controller no-op)
            // instead of resetting feather to 0.
            onMoved: {
                pendingValue = Math.round(value)
                if (!pressed && root.removal !== null)
                    root.removal.setFeatherPx(pendingValue)
            }
            onPressedChanged: {
                if (pressed) {
                    pendingValue = Math.round(value)
                } else if (root.removal !== null) {
                    root.removal.setFeatherPx(pendingValue)
                }
            }
            // Keyboard arrows also emit moved(); the !pressed branch commits
            // that gesture immediately. Same-value commits are controller
            // no-ops, so a tap without a drag cannot create an undo step.
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
                // An all-zero mask cannot become a keyframe; the first stroke
                // creates one automatically, so this is only for extra frames.
                enabled: root.removal !== null && root.removal.maskHasPixels
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
            Layout.preferredHeight: Math.max(24, Math.min(96, contentHeight))
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

        Button {
            objectName: "removal-redo-button"
            Layout.fillWidth: true
            text: "从此关键帧重做"
            enabled: root.ready && root.removal.hasLayer
                     && root.hasKeyframes && !root.removal.generating
            onClicked: root.removal.repropagateFromCurrentKeyframe()
            ToolTip.visible: hovered
            ToolTip.text: "从当前播放头附近的关键帧重新跟踪并修复到层结尾"
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
        text: root.removal !== null && root.removal.advancedOpen
              ? "收起 AI 高级设置 ▴" : "AI 高级设置 ▾"
        onClicked: if (root.removal !== null)
                       root.removal.advancedOpen = !root.removal.advancedOpen
    }

    ColumnLayout {
        objectName: "removal-advanced-panel"
        Layout.fillWidth: true
        spacing: 4
        visible: root.active && root.removal !== null
                 && root.removal.advancedOpen

        Label {
            objectName: "removal-advanced-invalidation-warning"
            text: "修改模型/分辨率/稳定化会使已生成的修复缓存失效，"
                + "需要重新跟踪与生成。"
            color: "#e67e22"
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "模型"; color: "#cccccc"; font.pixelSize: 11 }
            ComboBox {
                id: modeCombo
                objectName: "removal-advanced-mode"
                Layout.fillWidth: true
                enabled: root.removal !== null && !root.removal.generating
                model: [
                    { text: "快速（MI-GAN）", value: "fast" },
                    { text: "高质量（LaMa）", value: "quality" }
                ]
                textRole: "text"
                valueRole: "value"
                function syncMode() {
                    if (root.removal === null)
                        return
                    const idx = root.modeIndex(root.removal.advancedMode)
                    if (currentIndex !== idx)
                        currentIndex = idx
                }
                Component.onCompleted: syncMode()
                onActivated: if (root.removal !== null)
                                 root.removal.setAdvancedMode(currentValue)
                Connections {
                    target: root.removal
                    function onLayerChanged() { modeCombo.syncMode() }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "处理分辨率"; color: "#cccccc"; font.pixelSize: 11 }
            ComboBox {
                id: heightCombo
                objectName: "removal-advanced-height"
                Layout.fillWidth: true
                enabled: root.removal !== null && !root.removal.generating
                model: ["540p", "720p", "1080p"]
                function syncHeight() {
                    if (root.removal === null)
                        return
                    const idx = root.heightIndex(root.removal.processingHeight)
                    if (currentIndex !== idx)
                        currentIndex = idx
                }
                Component.onCompleted: syncHeight()
                onActivated: if (root.removal !== null)
                                 root.removal.setProcessingHeight(
                                     [540, 720, 1080][currentIndex])
                Connections {
                    target: root.removal
                    function onLayerChanged() { heightCombo.syncHeight() }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label { text: "时域稳定化"; color: "#cccccc"; font.pixelSize: 11 }
            ComboBox {
                id: stabilizationCombo
                objectName: "removal-advanced-stabilization"
                Layout.fillWidth: true
                enabled: root.removal !== null && !root.removal.generating
                model: ["关", "弱", "标准", "强"]
                function syncStabilization() {
                    if (root.removal === null)
                        return
                    const idx = root.stabilizationIndex(
                        root.removal.stabilizationStrength)
                    if (currentIndex !== idx)
                        currentIndex = idx
                }
                Component.onCompleted: syncStabilization()
                onActivated: if (root.removal !== null)
                                 root.removal.setStabilizationStrength(
                                     [0, 30, 60, 100][currentIndex])
                Connections {
                    target: root.removal
                    function onLayerChanged() { stabilizationCombo.syncStabilization() }
                }
            }
        }

        Label {
            objectName: "removal-chunk-size"
            text: "分块大小：30 帧（M4 固定）"
            color: "#8a8a8a"
            font.pixelSize: 11
            Layout.fillWidth: true
        }
    }
}
