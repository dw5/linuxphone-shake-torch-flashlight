// The Shake Torch app: the same page as Settings > System > Shake for torch, in its
// own window, so it can be opened from the app grid. Run by sailfish-qml.
import QtQuick 2.0
import Sailfish.Silica 1.0

ApplicationWindow {
    initialPage: Component { ShakeTorchPage {} }
    cover: Component {
        CoverBackground {
            Label {
                anchors.centerIn: parent
                width: parent.width - 2 * Theme.paddingLarge
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: "Shake Torch"
            }
        }
    }
    allowedOrientations: defaultAllowedOrientations
}
