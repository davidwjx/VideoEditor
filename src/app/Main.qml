import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    width: 1280
    height: 800
    visible: true
    title: "VideoEditor " + appVersion
    color: "#111111"

    Label {
        anchors.centerIn: parent
        text: "媒体库为空，点击“导入视频”开始（M1 骨架）"
        color: "#dddddd"
    }
}
