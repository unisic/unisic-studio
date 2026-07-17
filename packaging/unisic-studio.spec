Name:           unisic-studio
Version:        0.1.0
Release:        1%{?dist}
Summary:        Post-production for screen recordings on Linux Wayland

License:        GPL-3.0-or-later
URL:            https://github.com/unisic
Source0:        %{url}/unisic-studio/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  pkgconfig
# Qt6 (app + the bundled unisic-kit submodule).
BuildRequires:  cmake(Qt6Core)
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(Qt6Quick)
BuildRequires:  cmake(Qt6Qml)
BuildRequires:  cmake(Qt6QuickControls2)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Concurrent)
BuildRequires:  cmake(Qt6OpenGL)
BuildRequires:  cmake(Qt6Svg)
BuildRequires:  cmake(Qt6LinguistTools)
BuildRequires:  cmake(Qt6Test)
# Capture stack: PipeWire (kit grabber) + optional libinput/udev click capture.
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(libinput)
BuildRequires:  systemd-devel
# %%check validators.
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

# Fedora ships ffmpeg-free; use the file dep so either provider satisfies it.
Requires:       /usr/bin/ffmpeg
Requires:       pipewire
Requires:       qt6-qtsvg
Requires:       qt6-qtwayland
Requires:       qt6-qtdeclarative
# HARD dependency: the editor's live preview and the webcam overlay both
# `import QtMultimedia`, whose ffmpeg backend ships in qt6-qtmultimedia.
Requires:       qt6-qtmultimedia
Requires:       xdg-desktop-portal
Recommends:     (xdg-desktop-portal-kde or xdg-desktop-portal-gnome or xdg-desktop-portal-wlr)

%description
Unisic Studio turns a raw screen recording into a polished video: automatic
zoom and pan driven by the cursor and click tracks, styling (backgrounds,
padding, rounding, shadow, frames), non-destructive trim, and export to MP4,
WebM or GIF via ffmpeg. It is a sibling of Unisic and shares its foundation
library. Wayland-only, GPLv3, zero telemetry.

%prep
%autosetup -n %{name}-%{version}

%build
# A packaged build is a release build (a real build number flips the dev
# identity off).
export STUDIO_BUILD_NUMBER=%{release}
%cmake -G Ninja -DUNISIC_DEV_BUILD=OFF -DBUILD_TESTING=OFF
%cmake_build

%install
%cmake_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/app.unisic.UnisicStudio.desktop
appstream-util validate-relax --nonet \
    %{buildroot}%{_datadir}/metainfo/app.unisic.UnisicStudio.metainfo.xml || :

# Fedora refreshes the MIME and desktop databases automatically via file
# triggers from shared-mime-info / desktop-file-utils, so no %%post scriptlet is
# needed for the *.unisicstudio handler.

%files
%license LICENSE
%doc README.md
%{_bindir}/unisic-studio
%{_datadir}/applications/app.unisic.UnisicStudio.desktop
%{_datadir}/metainfo/app.unisic.UnisicStudio.metainfo.xml
%{_datadir}/mime/packages/app.unisic.UnisicStudio.xml
%{_datadir}/icons/hicolor/scalable/apps/app.unisic.UnisicStudio.svg

%changelog
* Thu Jul 16 2026 Unisic maintainers <unisic@debondor.com> - 0.1.0-1
- First preview: import, auto zoom/pan, styling, trim, and MP4/WebM/GIF export.
