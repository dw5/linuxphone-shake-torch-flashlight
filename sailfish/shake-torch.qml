// Settings > System > Shake for torch. Talks to the shake-torch service over D-Bus
// (local.ShakeTorch): QML here can't write the config file or the vibrator itself.
import QtQuick 2.0
import Sailfish.Silica 1.0
import Nemo.DBus 2.0

Page {
    id: page

    property bool ready: false     // settings came from the service
    property bool loading: false   // filling the controls in; don't save
    property bool sensing: false

    DBusInterface {
        id: service
        bus: DBus.SessionBus
        service: "local.ShakeTorch"
        path: "/local/ShakeTorch"
        iface: "local.ShakeTorch"
    }

    DBusInterface {
        id: systemd
        bus: DBus.SessionBus
        service: "org.freedesktop.systemd1"
        path: "/org/freedesktop/systemd1"
        iface: "org.freedesktop.systemd1.Manager"
    }

    function load() {
        service.typedCall("GetSettings", [], function (s) {
            loading = true
            enabledSwitch.checked = s["enabled"]
            threshold.value = s["threshold"]
            vibrate.checked = s["vibrate"]
            onMs.value = s["buzz-on-ms"]
            gapMs.value = s["buzz-gap-ms"]
            offMs.value = s["buzz-off-ms"]
            sensing = s["running"]
            loading = false
            ready = true
        }, function () {
            ready = false
        })
    }

    function set(key, value) {
        if (!loading && ready)
            service.typedCall("Set", [{ "type": "s", "value": key },
                                      { "type": "s", "value": String(value) }],
                              function () { sensingTimer.restart() }, function () { load() })
    }

    function tryBuzz(on) {
        service.typedCall("TryBuzz", [{ "type": "b", "value": on },
                                      { "type": "i", "value": Math.round(onMs.value) },
                                      { "type": "i", "value": Math.round(gapMs.value) },
                                      { "type": "i", "value": Math.round(offMs.value) }])
    }

    Component.onCompleted: load()

    // Look again while the service isn't there, e.g. after pressing Start.
    Timer {
        interval: 2000
        repeat: true
        running: !page.ready && page.status === PageStatus.Active
        onTriggered: page.load()
    }

    // After a change, read back whether the accelerometer is in use.
    Timer {
        id: sensingTimer
        interval: 500
        onTriggered: service.typedCall("GetSettings", [], function (s) { page.sensing = s["running"] })
    }

    // Save a slider shortly after it stops moving, not on every step.
    Timer {
        id: saveTimer
        interval: 400
        property var pending: ({})
        onTriggered: {
            for (var k in pending)
                page.set(k, pending[k])
            pending = {}
        }
        function later(key, value) {
            if (page.loading || !page.ready)
                return
            pending[key] = value
            restart()
        }
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: content.height + Theme.paddingLarge

        Column {
            id: content
            width: parent.width
            spacing: Theme.paddingMedium

            PageHeader { title: "Shake for torch" }

            Column {
                width: parent.width
                visible: !page.ready
                spacing: Theme.paddingLarge

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    wrapMode: Text.Wrap
                    color: Theme.highlightColor
                    text: "The shake-torch service isn't running."
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Start"
                    onClicked: systemd.typedCall("StartUnit",
                                                 [{ "type": "s", "value": "shake-torch.service" },
                                                  { "type": "s", "value": "replace" }])
                }
            }

            Column {
                width: parent.width
                visible: page.ready
                spacing: Theme.paddingMedium

                TextSwitch {
                    id: enabledSwitch
                    text: "Shake to switch the torch"
                    description: page.sensing
                                 ? "Shake the phone firmly for about half a second. Works with the screen off too."
                                 : "Off. The accelerometer isn't used."
                    onCheckedChanged: page.set("enabled", checked)
                }

                SectionHeader { text: "How hard to shake" }

                Slider {
                    id: threshold
                    width: parent.width
                    minimumValue: 0.8
                    maximumValue: 4.0
                    stepSize: 0.1
                    value: 1.4
                    label: "Strength"
                    valueText: value.toFixed(1) + " g"
                    onValueChanged: saveTimer.later("threshold", value.toFixed(1))
                }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryHighlightColor
                    text: "A shake is a swing one way and back, like a chop. Left: a gentle one is "
                          + "enough, but walking may set it off. Right: needs a firmer one. Measured "
                          + "on the FLX1s: walking up to 1.25 g, gentle shakes 1.5 to 2 g, normal "
                          + "ones 4 to 10 g."
                }

                SectionHeader { text: "Vibration" }

                TextSwitch {
                    id: vibrate
                    text: "Vibrate"
                    description: "Torch on: two buzzes with a pause. Torch off: one buzz. "
                                 + "Also when the phone is set to silent."
                    onCheckedChanged: page.set("vibrate", checked)
                }

                Slider {
                    id: onMs
                    width: parent.width
                    minimumValue: 10; maximumValue: 500; stepSize: 10; value: 250
                    label: "Torch on, each buzz"
                    valueText: Math.round(value) + " ms"
                    onValueChanged: saveTimer.later("buzz-on-ms", Math.round(value))
                }

                Slider {
                    id: gapMs
                    width: parent.width
                    minimumValue: 50; maximumValue: 800; stepSize: 10; value: 50
                    label: "Torch on, pause between"
                    valueText: Math.round(value) + " ms"
                    onValueChanged: saveTimer.later("buzz-gap-ms", Math.round(value))
                }

                Slider {
                    id: offMs
                    width: parent.width
                    minimumValue: 10; maximumValue: 1000; stepSize: 10; value: 275
                    label: "Torch off, buzz"
                    valueText: Math.round(value) + " ms"
                    onValueChanged: saveTimer.later("buzz-off-ms", Math.round(value))
                }

                ButtonLayout {
                    Button { text: "Try torch on"; onClicked: page.tryBuzz(true) }
                    Button { text: "Try torch off"; onClicked: page.tryBuzz(false) }
                }
            }
        }
        VerticalScrollDecorator {}
    }
}
