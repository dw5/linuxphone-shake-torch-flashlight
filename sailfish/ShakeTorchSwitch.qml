// Top menu toggle: shaking on or off. Off also lets go of the sensors, so the idle
// service costs nothing. Add it in Settings > Top menu.
import QtQuick 2.0
import Sailfish.Silica 1.0
import com.jolla.settings 1.0
import Nemo.DBus 2.0

SettingsToggle {
    id: root

    property bool running: false

    name: "Shake for torch"
    icon.source: "image://theme/icon-m-gesture"
    checked: false

    onToggled: {
        var on = !checked
        if (!running) {
            systemd.typedCall("StartUnit", [{ "type": "s", "value": "shake-torch.service" },
                                            { "type": "s", "value": "replace" }],
                              function () { startTimer.start() })
            return
        }
        service.typedCall("Set", [{ "type": "s", "value": "enabled" },
                                  { "type": "s", "value": on ? "true" : "false" }],
                          function () { root.checked = on })
    }

    function refresh() {
        service.typedCall("GetSettings", [], function (s) {
            root.running = true
            root.checked = s["enabled"]
        }, function () {
            root.running = false
            root.checked = false
        })
    }

    menu: ContextMenu {
        SettingsMenuItem {
            onClicked: settingsApp.call("showPage", ["system_settings/look_and_feel/shake_torch"])
        }
    }

    Component.onCompleted: refresh()

    // After starting the service, switch shaking on once it answers.
    Timer {
        id: startTimer
        interval: 1000
        onTriggered: service.typedCall("Set", [{ "type": "s", "value": "enabled" },
                                               { "type": "s", "value": "true" }],
                                       function () { root.refresh() }, function () { root.refresh() })
    }

    // The top menu stays loaded: pick up changes made in the settings page.
    Timer {
        interval: 5000
        repeat: true
        running: true
        onTriggered: root.refresh()
    }

    DBusInterface {
        id: service
        bus: DBus.SessionBus
        service: "local.ShakeTorch"
        path: "/local/ShakeTorch"
        iface: "local.ShakeTorch"
    }

    DBusInterface {
        id: settingsApp
        bus: DBus.SessionBus
        service: "com.jolla.settings"
        path: "/com/jolla/settings/ui"
        iface: "com.jolla.settings.ui"
    }

    DBusInterface {
        id: systemd
        bus: DBus.SessionBus
        service: "org.freedesktop.systemd1"
        path: "/org/freedesktop/systemd1"
        iface: "org.freedesktop.systemd1.Manager"
    }
}
