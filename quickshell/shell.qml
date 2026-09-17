import Quickshell
import QtQuick
import "."

ShellRoot {
    Variants {
        model: Quickshell.screens
        delegate: Component {
            Bar {
            }
        }
    }

    NetworkPanel {
    }

    Shortcuts {
    }
}
