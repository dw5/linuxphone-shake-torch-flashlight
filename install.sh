#!/bin/sh
# Install shake-torch for the current user and start it. Run on the phone, next to the
# built binary (see README.md). No root needed.
set -e
cd "$(dirname "$0")"
install -D -m 755 shake-torch ~/.local/bin/shake-torch
install -D -m 644 shake-torch.service ~/.config/systemd/user/shake-torch.service
install -D -m 755 shake-torch-settings ~/.local/bin/shake-torch-settings
# The session may not have ~/.local/bin on PATH, so point at the installed copy.
mkdir -p ~/.local/share/applications
sed "s|^Exec=.*|Exec=$HOME/.local/bin/shake-torch-settings|" local.ShakeTorch.Settings.desktop \
    > ~/.local/share/applications/local.ShakeTorch.Settings.desktop
# Before 0.4 the app was local.ShakeTorch, now the service's D-Bus name.
rm -f ~/.local/share/applications/local.ShakeTorch.desktop
install -D -m 644 shake-torch.svg ~/.local/share/icons/hicolor/scalable/apps/shake-torch.svg
gtk-update-icon-cache -q ~/.local/share/icons/hicolor 2>/dev/null || true
update-desktop-database -q ~/.local/share/applications 2>/dev/null || true
systemctl --user daemon-reload
systemctl --user enable --now shake-torch.service
systemctl --user restart shake-torch.service
echo "Running. Shake the phone to switch the torch. Settings: the Shake Torch app."
