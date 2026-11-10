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

%description
Shake the phone to switch the torch on or off, with two buzzes for on and one for off,
also in silent mode. Settings are in Settings > System > Shake for torch.

%install
install -Dm755 %{_sourcedir}/shake-torch %{buildroot}%{_bindir}/shake-torch
install -Dm644 %{_sourcedir}/shake-torch.service %{buildroot}%{_userunitdir}/shake-torch.service
mkdir -p %{buildroot}%{_userunitdir}/user-session.target.wants
ln -s ../shake-torch.service %{buildroot}%{_userunitdir}/user-session.target.wants/shake-torch.service
install -Dm644 %{_sourcedir}/shake-torch.json %{buildroot}%{_datadir}/jolla-settings/entries/shake-torch.json
install -Dm644 %{_sourcedir}/shake-torch.qml %{buildroot}%{_datadir}/jolla-settings/pages/shake-torch/shake-torch.qml

%files
%{_bindir}/shake-torch
%{_userunitdir}/shake-torch.service
%{_userunitdir}/user-session.target.wants/shake-torch.service
%{_datadir}/jolla-settings/entries/shake-torch.json
%{_datadir}/jolla-settings/pages/shake-torch
