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
    property var recent: []        // the last switches by shaking, newest first
    property string marksInfo: ""

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
            rotation.value = s["max-rotation"]
            notifySwitch.checked = s["notify"]
            vibrate.checked = s["vibrate"]
            onMs.value = s["buzz-on-ms"]
            secondMs.value = s["buzz-second-ms"]
            gapMs.value = s["buzz-gap-ms"]
            offMs.value = s["buzz-off-ms"]
            sensing = s["running"]
            marksInfo = s["marks"] + " saved in " + s["marks-dir"]
            loading = false
            ready = true
            loadRecent()
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
                                      { "type": "i", "value": Math.round(secondMs.value) },
                                      { "type": "i", "value": Math.round(gapMs.value) },
                                      { "type": "i", "value": Math.round(offMs.value) }])
    }

    function loadRecent() {
        service.typedCall("GetRecent", [], function (r) { recent = r })
    }

    // The switches, marks and whether it's sensing; not the sliders, which may be moving.
    function refresh() {
        service.typedCall("GetSettings", [], function (s) {
            sensing = s["running"]
            marksInfo = s["marks"] + " saved in " + s["marks-dir"]
        }, function () { ready = false })
        loadRecent()
    }

    // Pressing a switch's marked button again takes the mark back.
    function mark(when, kind) {
        service.typedCall("Mark", [{ "type": "x", "value": when }, { "type": "s", "value": kind }],
                          function () { refresh() }, function () { marksInfo = "Couldn't save the mark" })
    }

    function ago(when) {
        var s = Math.max(0, Math.round(Date.now() / 1000 - when))
        return s < 60 ? s + " s ago" : s < 3600 ? Math.floor(s / 60) + " min ago"
                                                : Math.floor(s / 3600) + " h ago"
    }

    Component.onCompleted: load()

    // Keep the switches and their times current while the page is shown.
    Timer {
        interval: 3000
        repeat: true
        running: page.ready && page.status === PageStatus.Active
        onTriggered: page.refresh()
    }

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

                TextSwitch {
                    id: notifySwitch
                    text: "Notification while it's on"
                    description: "\u201cFlashlight is on\u201d, tap to turn it off"
                    onCheckedChanged: page.set("notify", checked)
                }

                SectionHeader { text: "Wrong switches" }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryHighlightColor
                    text: "Did a shake switch the torch when you didn't mean it, or did a shake do "
                          + "nothing? Mark it here. The readings around it are saved, to tune the "
                          + "settings later. The last 5 switches since the service started."
                }

                Label {
                    x: Theme.horizontalPageMargin
                    visible: page.recent.length === 0
                    text: "No switches by shaking yet"
                    color: Theme.highlightColor
                }

                Repeater {
                    model: page.recent
                    delegate: Item {
                        width: parent.width
                        height: Math.max(info.height, buttons.height) + Theme.paddingSmall

                        Column {
                            id: info
                            x: Theme.horizontalPageMargin
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - 2 * x - buttons.width - Theme.paddingMedium
                            Label {
                                text: modelData["on"] ? "Torch on" : "Torch off"
                                color: Theme.highlightColor
                            }
                            Label {
                                width: parent.width
                                truncationMode: TruncationMode.Fade
                                font.pixelSize: Theme.fontSizeExtraSmall
                                color: Theme.secondaryHighlightColor
                                text: Qt.formatTime(new Date(modelData["when"] * 1000), "hh:mm:ss")
                                      + ", " + page.ago(modelData["when"])
                            }
                        }
                        Row {
                            id: buttons
                            anchors { right: parent.right; rightMargin: Theme.horizontalPageMargin
                                      verticalCenter: parent.verticalCenter }
                            spacing: Theme.paddingSmall
                            Button {
                                preferredWidth: Theme.buttonWidthExtraSmall
                                text: modelData["mark"] === "bad" ? "Wrong \u2713" : "Wrong"
                                highlighted: down || modelData["mark"] === "bad"
                                onClicked: page.mark(modelData["when"], modelData["mark"] === "bad" ? "" : "bad")
                            }
                            Button {
                                preferredWidth: Theme.buttonWidthExtraSmall
                                text: modelData["mark"] === "good" ? "Right \u2713" : "Right"
                                highlighted: down || modelData["mark"] === "good"
                                onClicked: page.mark(modelData["when"], modelData["mark"] === "good" ? "" : "good")
                            }
                        }
                    }
                }

                Item {
                    width: parent.width
                    height: missedButton.height + Theme.paddingSmall
                    Label {
                        x: Theme.horizontalPageMargin
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 2 * x - missedButton.width - Theme.paddingMedium
                        wrapMode: Text.Wrap
                        text: "A shake did nothing"
                        color: Theme.highlightColor
                    }
                    Button {
                        id: missedButton
                        anchors { right: parent.right; rightMargin: Theme.horizontalPageMargin
                                  verticalCenter: parent.verticalCenter }
                        preferredWidth: Theme.buttonWidthExtraSmall
                        text: "Save"
                        onClicked: service.typedCall("MarkMissed", [], function () { page.refresh() },
                                                     function () { page.marksInfo = "Couldn't save" })
                    }
                }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryHighlightColor
                    text: "Saves the last 15 s; do it soon after. " + page.marksInfo
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

                SectionHeader { text: "Ignore twists" }

                Slider {
                    id: rotation
                    width: parent.width
                    minimumValue: 0; maximumValue: 360; stepSize: 10; value: 108
                    label: "Most it may turn"
                    valueText: value > 0 ? Math.round(value) + "\u00b0" : "Off"
                    onValueChanged: saveTimer.later("max-rotation", Math.round(value))
                }

                Label {
                    x: Theme.horizontalPageMargin
                    width: parent.width - 2 * x
                    wrapMode: Text.Wrap
                    font.pixelSize: Theme.fontSizeExtraSmall
                    color: Theme.secondaryHighlightColor
                    text: "Like Motorola: a chop moves the phone along a line, a flick of the wrist "
                          + "turns it. A shake that turns the phone more than this doesn't count. "
                          + "Motorola uses 108\u00b0."
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
                    minimumValue: 10; maximumValue: 500; stepSize: 10; value: 200
                    label: "Torch on, first buzz"
                    valueText: Math.round(value) + " ms"
                    onValueChanged: saveTimer.later("buzz-on-ms", Math.round(value))
                }

                Slider {
                    id: secondMs
                    width: parent.width
                    minimumValue: 10; maximumValue: 500; stepSize: 10; value: 400
                    label: "Torch on, second buzz"
                    valueText: Math.round(value) + " ms"
                    onValueChanged: saveTimer.later("buzz-second-ms", Math.round(value))
                }

                Slider {
                    id: gapMs
                    width: parent.width
                    minimumValue: 50; maximumValue: 800; stepSize: 10; value: 100
                    label: "Torch on, pause between"
                    valueText: Math.round(value) + " ms"
                    onValueChanged: saveTimer.later("buzz-gap-ms", Math.round(value))
                }

                Slider {
                    id: offMs
                    width: parent.width
                    minimumValue: 10; maximumValue: 1000; stepSize: 10; value: 300
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
