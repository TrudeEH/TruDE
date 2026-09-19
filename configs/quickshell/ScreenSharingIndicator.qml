import Quickshell.Io
import QtQuick
import "."

Rectangle {
    id: indicator

    property bool captureActive: false

    visible: captureActive
    width: visible ? 28 : 0
    height: 28
    radius: 0
    color: Theme.surfaceRaised
    border.color: "#ff7b72"
    border.width: 1

    function updateCaptureState(output) {
        try {
            const graph = JSON.parse(output);
            for (const object of graph) {
                if (object.type !== "PipeWire:Interface:Node" || !object.info
                        || object.info.state !== "running") continue;
                const properties = object.info.props || {};
                const mediaClass = properties["media.class"] || "";
                const nodeName = properties["node.name"] || "";
                const videoConsumer = mediaClass === "Stream/Input/Video"
                    || mediaClass === "Stream/Output/Video";
                const portalSource = mediaClass === "Video/Source"
                    && /portal|screencast|screen/i.test(nodeName);
                if (videoConsumer || portalSource) {
                    captureActive = true;
                    return;
                }
            }
            captureActive = false;
        } catch (error) {
            captureActive = false;
        }
    }

    AppText {
        anchors.centerIn: parent
        text: "󰑋"
        color: "#ff7b72"
        font.pixelSize: 14
    }

    Process {
        id: captureQuery
        command: ["pw-dump"]
        running: false
        stdout: StdioCollector {
            onStreamFinished: indicator.updateCaptureState(this.text)
        }
    }

    Timer {
        interval: 2000
        running: true
        repeat: true
        onTriggered: if (!captureQuery.running) captureQuery.running = true
    }

    Component.onCompleted: captureQuery.running = true
}
