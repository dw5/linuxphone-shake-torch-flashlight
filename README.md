# Shake for the torch

Shake the phone to switch the torch on or off, like Motorola's gesture. Two buzzes when it goes on, one when it goes off, also when the phone is set to silent. Written for FuriOS (Furi FLX1s) and Sailfish OS from one C source.

## How it works

- **Motion:** both systems run sensorfwd. `shake-torch` asks it for an accelerometer session over D-Bus (`com.nokia.SensorService`, system bus) and reads the readings from `/run/sensord.sock`: 50 a second, in milli-g. The protocol is in the comment at the top of `shake-torch.c`. QtSensors isn't used: on FuriOS it has no sensorfw plugin.
- **Shake:** a swing one way and back, like Motorola's chop. Gravity is the average of the last second; what's left is the movement. Within the last 0.6 s, the strongest movement must reach the threshold (1.4 g) and the movement back along the same line 60% of it, within 200 ms. In every recorded shake, gentle or hard, the swing back came 60 to 140 ms after the swing; setting the phone down on a table is also a swing and back, but slow (480 to 600 ms), and switched the torch before this limit. After switching nothing happens for 1.7 s, the Motorola's quickest on-then-off in the user's hands. With a shorter wait the end of the same shake could switch it straight back off. Then it is ready once the phone is calm (under 0.8 g for a quarter second) or after 2 s at the latest. The calm rule alone missed the user's turn-off shake: the hand kept moving between on and off, as it does with the Motorola.
- **Twists don't count:** like Motorola's `max_gyro_rotation`, the gyroscope (also through sensorfwd, where the phone has one) adds up how far the phone turned in the 350 ms before the swing. More than 108° is a flick of the wrist, not a chop, and is ignored. Measured on the FLX1s: accepted shakes turned 25 to 108°, rejected movements 110 to 224°. `max-rotation=0` turns the check off; without a gyroscope there is none.
- **Torch:** FuriOS `io.furios.Flashlightd` (`SetBrightness`), Sailfish `org.sailfish.flashlight.provider` (`toggleFlashlight`). Whichever answers.
- **Buzz:** through the Android vibrator device `/sys/class/leds/vibrator`, so it works in the "silent" profile too. Writable by the phone's user on both the FLX1s and the Jolla Phone (there through the `input` group). Phones without it get ngfd's `feedback_press` instead (untested).
- **Off means off:** with shaking turned off in the settings, the service lets go of the sensors.
- **Notification:** while a shake has the torch on, "Flashlight is on / Tap to turn off the flashlight.", as on the Motorola. Tapping it turns the torch off; it goes away by itself when the torch is turned off another way. Standard desktop notifications (Phosh, and Sailfish's home screen, which also gets a D-Bus action `TorchOff`).
- **Buzz pattern:** Motorola's by default: on is 200 ms, 100 ms pause, 400 ms; off is 300 ms.

Tuned on a 100 s recording of the user's hand on the FLX1s (2026-11-10): walking peaked at 1.25 g, gentle shakes at 1.5 to 2 g, normal ones (a chop) at 4 to 10 g, in bursts of about half a second. Two earlier methods failed on it: counting three alternating swings merged fast swings into one, and "how far the total is from 1 g" barely moves when the phone is shaken sideways (a 2 g sideways shake reads 1.2 g). Replayed through the current method: 25 of 36 separate bursts switched (the rest followed another within a second), nothing while walking.

## How Motorola does it

Spec sheet: `../docs/motorola-chop.md`, read from the user's Motorola Edge 30 Pro on 2026-11-10. In short: the chop is recognised by the phone's sensor hub, not by the app, as "two chops" of about 3 g, each within 350 ms, with the gyroscope rejecting turns over 108. The app then switches the torch, buzzes (on: 200 ms, 100 ms pause, 400 ms; off: 300 ms) and shows "Flashlight is on, tap to turn off". shake-torch now copies the gyroscope check, the buzz, the notification and the 1.7 s between switches.

## Build and install

### FuriOS

Built in the `furilabs` container (cross compiler and `libglib2.0-dev:arm64`):

```
ws run -- make          # in this folder: an arm64 binary for FuriOS
```

Then on the phone, with `shake-torch`, `shake-torch-settings`, `shake-torch.service`, `local.ShakeTorch.desktop`, `shake-torch.svg` and `install.sh` copied over: `sh install.sh`. It puts the programs in `~/.local/bin`, adds the app to the app list, and runs the service as a user service that starts with the phone. No root.

Remove: `systemctl --user disable --now shake-torch`, then delete `~/.local/bin/shake-torch*`, `~/.config/systemd/user/shake-torch.service`, `~/.local/share/applications/local.ShakeTorch.desktop` and `~/.local/share/icons/hicolor/scalable/apps/shake-torch.svg`.

### Sailfish OS

The same arm64 binary runs on the Jolla Phone: Sailfish 5.2 has glibc 2.41 and GLib 2.86, and the binary needs glibc 2.34 and GLib 2.84 at most. It is packaged as an RPM with a user service and a page in the Settings app:

```
ws run -- make rpm      # shake-torch-0.2-1.aarch64.rpm (needs rpm in the container)
```

On the phone, as root (`devel-su`): `pkcon install-local shake-torch-0.2-1.aarch64.rpm`, or `rpm -U` it. Then, as the phone's user, start it without a reboot: `systemctl --user daemon-reload; systemctl --user start shake-torch`. It starts with the phone after that (the package links it into `user-session.target`).

The settings page is **Settings > System > Shake for torch**, under Gestures (`sailfish/shake-torch.qml`). QML there can't write files or the vibrator, so the page talks to the service over D-Bus: `local.ShakeTorch` on the session bus, with `GetSettings`, `Set(key, value)` and `TryBuzz` (see the comment in `shake-torch.c`). If the service isn't running the page says so and offers **Start**. The switch at the top turns shaking off; the service then lets go of the sensors and only waits, at no cost. There is also a **Top menu** toggle, "Shake for torch": add it in Settings > Top menu. It starts the service if it isn't running.

The package restarts the service when it is updated and stops it when removed (`systemctl-user`, Sailfish's helper for package scripts). Before 0.5 an update needed `systemctl --user restart shake-torch`.

Remove: `devel-su rpm -e shake-torch`, then `systemctl --user stop shake-torch`.

## Settings

On FuriOS, the **Shake Torch** app (`shake-torch-settings`, GTK): on or off, how hard to shake, and the buzz lengths, with **Try torch on** / **Try torch off** buttons to feel them first. Changes save at once and the service rereads them.

Underneath it's `~/.config/shake-torch.conf`, see `shake-torch.conf.example`. After editing by hand: `systemctl --user reload shake-torch`.

## Tuning and testing

- `shake-torch --toggle`: switch the torch once, with the buzz.
- `shake-torch --monitor`: print how hard the phone is moving, four times a second.
- `shake-torch --record FILE`: also save every reading; `shake-torch --replay FILE` runs the detector over it and prints where it would switch, without touching the torch.
- `log-movement=true` in the settings logs every movement over 0.4 g. On FuriOS the user journal needs root to read: `sudo journalctl _SYSTEMD_USER_UNIT=shake-torch.service -b`.

## Status

- **FLX1s** (2026-11-10): works, checked by the user side by side with a Motorola: gentle and normal shakes switch it on and off, setting the phone on a table doesn't. The gyroscope check, the 1.7 s wait, Motorola's buzz and the notification are installed. The user found it good enough, with fine-tuning left for later.
- **Jolla Phone, Sailfish 5.2:** package 0.3 installed and working with the screen on. With the screen off it reacts later and needs harder shakes, and the buzz waited for the torch. 0.3 buzzes first; the sleeping is still open. The Jolla's vibrator is stronger than the FLX1s's. Package 0.4 has the FLX1s changes; the notification on Sailfish is untested.

Not done yet:

- **Screen off on Sailfish:** measure with `--record` (it now records when each reading arrived), then decide between keeping the processor awake while the phone moves and accepting the delay.
- **Twist limit:** 108° is Motorola's number. The FLX1s recording wasn't labelled shake by shake, so it isn't tuned to the user's twists yet.
- **More of Motorola's checks,** if false triggers show up: direction (`max_xy_percentage`), and ignoring the phone in a pocket (proximity).
- **Battery over hours** with the screen off is not measured; processor time is (see above).
- A Codex review.
