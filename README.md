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

Then on the phone, with `shake-torch`, `shake-torch.service` and `install.sh` copied over: `sh install.sh`. It puts the binary in `~/.local/bin` and runs it as a user service that starts with the phone. No root.

Remove: `systemctl --user disable --now shake-torch`, then delete `~/.local/bin/shake-torch` and `~/.config/systemd/user/shake-torch.service`.

## Settings

`~/.config/shake-torch.conf`, see `shake-torch.conf.example`. After a change: `systemctl --user reload shake-torch`.

## Tuning and testing

- `shake-torch --toggle`: switch the torch once, with the buzz.
- `shake-torch --monitor`: print how hard the phone is moving, four times a second.
- `shake-torch --record FILE`: also save every reading; `shake-torch --replay FILE` runs the detector over it and prints where it would switch, without touching the torch.
- `log-movement=true` in the settings logs every movement over 0.4 g. On FuriOS the user journal needs root to read: `sudo journalctl _SYSTEMD_USER_UNIT=shake-torch.service -b`.

## Not done yet

- **Screen off.** By default sensorfwd stops the accelerometer while the screen is off, so the gesture only works with the screen on. `screen-off=true` keeps it running (sensorfwd's standby override); whether the phone then sleeps anyway, and what it costs in battery, is not measured.
- **Sailfish:** not built yet. Needs a build against Sailfish's GLib (the Sailfish SDK, or the phone's copied root filesystem as a sysroot).
- A settings screen.
