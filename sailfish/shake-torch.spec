# Sailfish OS package, built from the files already made by `make` (see `make rpm`).
%global debug_package %{nil}
%global __os_install_post %{nil}
%define _binary_payload w9.gzdio

Name:       shake-torch
Version:    %{?version}%{!?version:0.2}
Release:    1
Summary:    Shake the phone to switch the torch
License:    Personal use
Requires:   sensorfw-qt5
Requires:   nemo-qml-plugin-dbus-qt5
Requires:   jolla-settings
Requires:   libsailfishapp-launcher

%description
Shake the phone to switch the torch on or off, with two buzzes for on and one for off,
also in silent mode. Settings are in Settings > System > Shake for torch.

%install
install -Dm755 %{_sourcedir}/shake-torch %{buildroot}%{_bindir}/shake-torch
install -Dm644 %{_sourcedir}/shake-torch.service %{buildroot}%{_userunitdir}/shake-torch.service
mkdir -p %{buildroot}%{_userunitdir}/user-session.target.wants
ln -s ../shake-torch.service %{buildroot}%{_userunitdir}/user-session.target.wants/shake-torch.service
install -Dm644 %{_sourcedir}/shake-torch.json %{buildroot}%{_datadir}/jolla-settings/entries/shake-torch.json
install -Dm644 %{_sourcedir}/ShakeTorchPage.qml %{buildroot}%{_datadir}/shake-torch/qml/ShakeTorchPage.qml
install -Dm644 %{_sourcedir}/shake-torch-app.qml %{buildroot}%{_datadir}/shake-torch/qml/shake-torch.qml
install -Dm644 %{_sourcedir}/ShakeTorchSwitch.qml %{buildroot}%{_datadir}/jolla-settings/pages/shake-torch/ShakeTorchSwitch.qml
install -Dm644 %{_sourcedir}/shake-torch.desktop %{buildroot}%{_datadir}/applications/shake-torch.desktop
for size in 86 108 128 172; do
    install -Dm644 %{_sourcedir}/shake-torch-$size.png %{buildroot}%{_datadir}/icons/hicolor/${size}x${size}/apps/shake-torch.png
done

%post
# Pick up the new program in running sessions (systemctl-user is Sailfish's helper for this).
systemctl-user daemon-reload || :
systemctl-user try-restart shake-torch.service || :

%preun
if [ "$1" -eq 0 ]; then
    systemctl-user stop shake-torch.service || :
fi

%postun
systemctl-user daemon-reload || :

%files
%{_bindir}/shake-torch
%{_userunitdir}/shake-torch.service
%{_userunitdir}/user-session.target.wants/shake-torch.service
%{_datadir}/jolla-settings/entries/shake-torch.json
%{_datadir}/jolla-settings/pages/shake-torch
%{_datadir}/shake-torch
%{_datadir}/applications/shake-torch.desktop
%{_datadir}/icons/hicolor/*/apps/shake-torch.png
