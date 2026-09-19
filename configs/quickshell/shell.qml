//@ pragma UseQApplication
import Quickshell
import QtQuick
import "."

ShellRoot {
    NotificationCenter {
        id: notifications
    }

    Variants {
        model: Quickshell.screens
        delegate: Component {
            Bar {
                notificationCenter: notifications
            }
        }
    }

    Launcher {
    }

    Shortcuts {
    }
}
