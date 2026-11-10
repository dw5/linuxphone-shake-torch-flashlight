# Shake for the torch

Shake the phone to switch the torch on or off, like Motorola's gesture. Two buzzes when it goes on, one when it goes off, also when the phone is set to silent. Written for FuriOS (Furi FLX1s) and Sailfish OS from one C source.

## How it works

- **Motion:** both systems run sensorfwd. `shake-torch` asks it for an accelerometer session over D-Bus (`com.nokia.SensorService`, system bus) and reads the readings from `/run/sensord.sock`: 50 a second, in milli-g. The protocol is in the comment at the top of `shake-torch.c`. QtSensors isn't used: on FuriOS it has no sensorfw plugin.
- **Shake:** a swing one way and back, like Motorola's chop. Gravity is the average of the last second; what's left is the movement. Within the last 0.6 s, the strongest movement must reach the threshold (1.4 g) and the movement back along the same line 60% of it, within 200 ms. In every recorded shake, gentle or hard, the swing back came 60 to 140 ms after the swing; setting the phone down on a table is also a swing and back, but slow (480 to 600 ms), and switched the torch before this limit. After switching it is ready again once the phone is calm (under 0.8 g for a quarter second, at least 0.8 s) or after 1.2 s at the latest. The calm rule alone missed the user's turn-off shake: the hand kept moving between on and off, as it does with the Motorola.
- **Torch:** FuriOS `io.furios.Flashlightd` (`SetBrightness`), Sailfish `org.sailfish.flashlight.provider` (`toggleFlashlight`). Whichever answers.
- **Buzz:** through the Android vibrator device `/sys/class/leds/vibrator`, so it works in the "silent" profile too. Writable by the phone's user on both the FLX1s and the Jolla Phone (there through the `input` group). Phones without it get ngfd's `feedback_press` instead (untested).
- **Off means off:** with shaking turned off in the settings, the service lets go of the accelerometer.

Tuned on a 100 s recording of the user's hand on the FLX1s (2026-11-10): walking peaked at 1.25 g, gentle shakes at 1.5 to 2 g, normal ones (a chop) at 4 to 10 g, in bursts of about half a second. Two earlier methods failed on it: counting three alternating swings merged fast swings into one, and "how far the total is from 1 g" barely moves when the phone is shaken sideways (a 2 g sideways shake reads 1.2 g). Replayed through the current method: 25 of 36 separate bursts switched (the rest followed another within a second), nothing while walking.

## How Motorola does it

Full write-up, with the evidence and the commands to read it again: `../docs/motorola-chop.md`. In short, read on 2026-11-10 from the user's Motorola Edge 30 Pro (Android 14, Moto Actions 08.78.10) over adb, without root. The APK is kept privately in `../reference/moto-actions/` (proprietary, don't publish).

- **Detection isn't in the app.** It runs in the Qualcomm sensor hub as a wake-up sensor, `ChopChop Wakeup` (type `com.motorola.sensor.chopchop`), sampled at 1 kHz with the main processor asleep. That's why it reacts at once with the screen off. The app, `com.motorola.actions` (FlashOnChop), only listens for the event (value 1.0), then switches the torch through Android's camera torch API and buzzes.
- **Its parameters** are in `/vendor/etc/sensors/config/mot_chopchop.json`, readable by anyone. The names are Motorola's; the meanings are a reading of the names, since the firmware itself is closed:

  | Parameter | Value | Likely meaning |
  | --- | --- | --- |
  | `first_accel_threshold` | 32.0 | first chop at least 32 m/s² (3.3 g) |
  | `second_accel_threshold` | 30.0 | second chop at least 30 m/s² (3.1 g) |
  | `max_chop_duration_ms` | 350 | each chop over within 350 ms |
  | `max_gyro_rotation` | 108.0 | rejected if the phone turns faster than this (probably °/s): a twist of the wrist isn't a chop |
  | `min_magnitude_percentage` | 0.2 | unclear; maybe how strong the opposite swing must be |
  | `max_xy_percentage` | 0.65 | at most 65% of the movement across the phone's x/y plane, so the chop has to run mostly along one direction |

  So it is "chop twice", as Motorola's own name `chop_twice` says, and it uses the gyroscope as well as the accelerometer. shake-torch uses only the accelerometer.
- **Buzz**, from Android's vibration log (`dumpsys vibrator_manager`): torch on is 200 ms, 100 ms pause, 400 ms. Torch off is 300 ms. Both at full strength. The buzz starts 25 to 45 ms after the chop event.
- **Between switches:** in the user's tests the quickest on-then-off pairs were 1.7 to 2.1 s apart. shake-torch allows a switch again after 1.2 s.

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

The settings page is **Settings > System > Shake for torch**, under Gestures (`sailfish/shake-torch.qml`). QML there can't write files or the vibrator, so the page talks to the service over D-Bus: `local.ShakeTorch` on the session bus, with `GetSettings`, `Set(key, value)` and `TryBuzz` (see the comment in `shake-torch.c`). If the service isn't running the page says so and offers **Start**.

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

Works on the FLX1s (2026-11-10), checked by the user side by side with a Motorola: gentle and normal shakes switch it on and off, setting the phone on a table doesn't.

Not done yet:

- **Sailfish:** packaged (2026-11-10) but not yet tried on a phone. The D-Bus settings interface was tested in the container on a private bus; the QML page passed `qmllint` only. Open questions: whether Sailfish suspends with the screen off and so stops the sensor (the FLX1s never suspends), whether `org.sailfish.flashlight.provider` reports the new state from `toggleFlashlight`, and how the thresholds feel on the Jolla's accelerometer.
- **Battery over hours** with the screen off is not measured; processor time is (see above).
- A Codex review.
