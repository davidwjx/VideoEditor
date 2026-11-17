import QtQuick

// Transparent mask-editing surface layered over the video preview. Pointer
// coordinates are mapped through VideoPreviewItem::contentRect, never the
// whole item, so letterbox bars are excluded. This overlay is preview-only and
// is never part of TimelineCompositor, so exports never include the red mask.
Item {
    id: overlay
    objectName: "mask-editor-overlay"

    // Set by Main.qml. Kept as a property because a component loaded from a
    // separate file cannot see the instantiating file's ids.
    property var previewItem: null
    // The context property only exists when the app installs it.
    property var removal: (typeof Removal !== "undefined") ? Removal : null

    readonly property bool editing: removal !== null && removal.maskMode
    readonly property rect content: {
        if (previewItem === null || previewItem === undefined
            || previewItem.contentRect === undefined)
            return Qt.rect(0, 0, width, height)
        const r = previewItem.contentRect
        const topLeft = previewItem.mapToItem(overlay, r.x, r.y)
        const bottomRight = previewItem.mapToItem(overlay,
                                                  r.x + r.width,
                                                  r.y + r.height)
        return Qt.rect(topLeft.x, topLeft.y,
                       bottomRight.x - topLeft.x,
                       bottomRight.y - topLeft.y)
    }

    property point boxStart: Qt.point(0, 0)
    property point lastPoint: Qt.point(0, 0)

    visible: editing
    enabled: editing
    z: 5

    function norm(point) {
        return Qt.point((point.x - content.x) / Math.max(1, content.width),
                        (point.y - content.y) / Math.max(1, content.height))
    }

    Image {
        id: maskImage
        x: overlay.content.x
        y: overlay.content.y
        width: overlay.content.width
        height: overlay.content.height
        fillMode: Image.Stretch
        cache: false
        smooth: true
        opacity: 0.85
        source: (overlay.removal !== null && overlay.editing
                 && overlay.removal.maskRevision > 0)
                ? "image://mask/" + overlay.removal.maskRevision
                : ""
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        cursorShape: Qt.CrossCursor

        onPressed: (mouse) => {
            if (overlay.removal === null)
                return
            const p = overlay.norm(Qt.point(mouse.x, mouse.y))
            overlay.boxStart = p
            overlay.lastPoint = p
            if (overlay.removal.boxMode)
                return
            overlay.removal.beginStroke(overlay.removal.brushSize,
                                        overlay.removal.eraseMode)
            overlay.removal.continueStroke(p, p)
        }

        onPositionChanged: (mouse) => {
            if (!pressed || overlay.removal === null)
                return
            const p = overlay.norm(Qt.point(mouse.x, mouse.y))
            if (!overlay.removal.boxMode)
                overlay.removal.continueStroke(overlay.lastPoint, p)
            overlay.lastPoint = p
        }

        onReleased: (mouse) => {
            if (overlay.removal === null)
                return
            const p = overlay.norm(Qt.point(mouse.x, mouse.y))
            if (overlay.removal.boxMode) {
                overlay.removal.paintBox(
                    Qt.rect(overlay.boxStart.x, overlay.boxStart.y,
                            p.x - overlay.boxStart.x,
                            p.y - overlay.boxStart.y),
                    overlay.removal.eraseMode)
            } else {
                overlay.removal.endStroke()
            }
        }

        onCanceled: {
            if (overlay.removal !== null && !overlay.removal.boxMode)
                overlay.removal.endStroke()
        }
    }
}
