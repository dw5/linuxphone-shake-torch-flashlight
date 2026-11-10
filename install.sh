#!/bin/sh
# Install shake-torch for the current user and start it. Run on the phone, next to the
# built binary (see README.md). No root needed.
set -e
cd "$(dirname "$0")"
install -D -m 755 shake-torch ~/.local/bin/shake-torch
install -D -m 644 shake-torch.service ~/.config/systemd/user/shake-torch.service
systemctl --user daemon-reload
systemctl --user enable --now shake-torch.service
systemctl --user restart shake-torch.service
echo "Running. Shake the phone to switch the torch; settings: ~/.config/shake-torch.conf"
