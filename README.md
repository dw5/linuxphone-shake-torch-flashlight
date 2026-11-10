# Shake for the torch

Shake the phone to switch the torch on or off, like Motorola's gesture. Two buzzes when it goes on, one when it goes off, also when the phone is set to silent. Written for FuriOS (Furi FLX1s) and Sailfish OS from one C source.

## How it works

- **Motion:** both systems run sensorfwd. `shake-torch` asks it for an accelerometer session over D-Bus (`com.nokia.SensorService`, system bus) and reads the readings from `/run/sensord.sock`: 50 a second, in milli-g. The protocol is in the comment at the top of `shake-torch.c`. QtSensors isn't used: on FuriOS it has no sensorfw plugin.
- **Shake:** how far the total acceleration is from 1 g. That is zero at rest however the phone is held, and large while it's shaken. When it stays over the threshold (1 g) for at least half the readings in half a second, the torch switches; then 1.5 s of quiet. A step or a bump is a short spike and doesn't last.
- **Torch:** FuriOS `io.furios.Flashlightd` (`SetBrightness`), Sailfish `org.sailfish.flashlight.provider` (`toggleFlashlight`). Whichever answers.
- **Buzz:** on FuriOS through the vibrator device `/sys/class/leds/vibrator` (writable by the phone's user), so it works in feedbackd's "silent" profile too. On Sailfish through ngfd (untested).

Measured on the FLX1s (2026-11-10), strongest point of each movement: walking and handling 0.5 to 1.25 g, firm shakes 1.8 to 9 g.

## Build and install

Built in the `furilabs` container (cross compiler and `libglib2.0-dev:arm64`):

```
ws run -- make          # in this folder: an arm64 binary for FuriOS
```

Then on the phone, with `shake-torch`, `shake-torch-settings`, `shake-torch.service`, `local.ShakeTorch.desktop`, `shake-torch.svg` and `install.sh` copied over: `sh install.sh`. It puts the programs in `~/.local/bin`, adds the app to the app list, and runs the service as a user service that starts with the phone. No root.

Remove: `systemctl --user disable --now shake-torch`, then delete `~/.local/bin/shake-torch*`, `~/.config/systemd/user/shake-torch.service`, `~/.local/share/applications/local.ShakeTorch.desktop` and `~/.local/share/icons/hicolor/scalable/apps/shake-torch.svg`.

## Settings

The **Shake Torch** app (`shake-torch-settings`, GTK, FuriOS): on or off, how hard to shake, and the buzz lengths, with **Try torch on** / **Try torch off** buttons to feel them first. Changes save at once and the service rereads them.

Underneath it's `~/.config/shake-torch.conf`, see `shake-torch.conf.example`. After editing by hand: `systemctl --user reload shake-torch`.

## Tuning and testing

- `shake-torch --toggle`: switch the torch once, with the buzz.
- `shake-torch --monitor`: print how hard the phone is moving, four times a second.
- `shake-torch --record FILE`: also save every reading; `shake-torch --replay FILE` runs the detector over it and prints where it would switch, without touching the torch.
- `log-movement=true` in the settings logs every movement over 0.4 g. On FuriOS the user journal needs root to read: `sudo journalctl _SYSTEMD_USER_UNIT=shake-torch.service -b`.

## Not done yet

- **Battery with the screen off.** It works with the screen off: FuriOS on the FLX1s never deep-sleeps (0 suspends; `batman` throttles the processor instead), and its sensorfwd kept sending all readings with the screen locked, with or without `screen-off` (sensorfwd's standby override, on by default for Sailfish's sake). Processor time while listening: about 250 ms a minute for `shake-torch` and 1.3 s a minute for sensorfwd (0.4% and 2% of one core). Battery drain over hours is not measured yet.
- **Sailfish:** not built yet. Needs a build against Sailfish's GLib (the Sailfish SDK, or the phone's copied root filesystem as a sysroot).
- Settings app on Sailfish (a QML page).
