pragma Singleton

import Quickshell
import QtQuick

QtObject {
    readonly property var targets: [
        "network", "bluetooth", "power-profile", "audio", "notifications", "launcher",
        "shortcuts", "system-monitor", "maintenance", "tray"
    ]

    function closeExcept(activeTarget) {
        for (const target of targets) {
            if (target !== activeTarget)
                Quickshell.execDetached(["quickshell", "ipc", "call", target, "close"]);
        }
    }
}
