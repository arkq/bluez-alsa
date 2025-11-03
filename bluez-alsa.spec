Name:           bluez-alsa
Version:        4.3.1.b0dd89bd
Release:        %(date '+%Y%m%d')
Summary:        alsa bluetooth audio adaptor.
License:        MIT
Source0:        bluez-alsa-v4.3.1-b0dd89bd.tar.gz

BuildRequires:  automake autoconf make git automake libtool pkgconfig gcc python3-docutils alsa-lib-devel bluez-libs-devel dbus-glib-devel sbc-devel
Requires: sbc alsa-lib bluez-libs dbus-glib libtool

%prep
%setup -q -n bluez-alsa-v4.3.1-b0dd89bd
echo "prep"

# Don't build a debug package
%global debug_package %{nil}

%build
autoreconf --install
./configure --enable-systemd --with-systemdbluealsadargs=" -p a2dp-source -p hfp-ag -p hsp-ag -p a2dp-sink -p hsp-hs -p hfp-hf"
make -j $(nproc)

%pre

%description
Playback or record on bluetooth audio device with alsa APIs.

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot}

%post
libtool --finish /usr/lib
libtool --finish /usr/lib64/alsa-lib/
systemctl daemon-reload
systemctl enable bluealsa
systemctl start bluealsa

%clean
rm -rf %{buildroot}

%files
/etc/alsa/conf.d/20-bluealsa.conf
/usr/share/dbus-1/system.d/org.bluealsa.conf
/usr/share/alsa/alsa.conf.d/20-bluealsa.conf
/usr/lib/libbluealsad.a
/usr/lib/libbluealsad.so.0.0.0
/usr/lib/libbluealsad.la
/usr/lib/systemd/system/bluealsa.service
/usr/lib/systemd/system/bluealsa-aplay.service
/usr/lib64/alsa-lib/libasound_module_ctl_bluealsa.a
/usr/lib64/alsa-lib/libasound_module_pcm_bluealsa.so
/usr/lib64/alsa-lib/libasound_module_ctl_bluealsa.so
/usr/lib64/alsa-lib/libasound_module_pcm_bluealsa.a
/usr/lib64/alsa-lib/libasound_module_ctl_bluealsa.la
/usr/lib64/alsa-lib/libasound_module_pcm_bluealsa.la
/usr/include/bluealsad/shared/dbus-client-rfcomm.h
/usr/include/bluealsad/shared/nv.h
/usr/include/bluealsad/shared/bluetooth.h
/usr/include/bluealsad/shared/a2dp-codecs.h
/usr/include/bluealsad/shared/hex.h
/usr/include/bluealsad/shared/defs.h
/usr/include/bluealsad/shared/dbus-client.h
/usr/include/bluealsad/shared/dbus-client-pcm.h
/usr/include/bluealsad/shared/ffb.h
/usr/include/bluealsad/shared/rt.h
/usr/include/bluealsad/shared/log.h
/usr/bin/bluealsactl
/usr/bin/bluealsa-aplay
/usr/bin/bluealsad
/usr/lib/libbluealsad.so
/usr/lib/libbluealsad.so.0

%postun
libtool --finish /usr/lib
libtool --finish /usr/lib64/alsa-lib/
if [ "$1" -eq 0 ]; then
    systemctl stop bluealsa
    systemctl disable bluealsa
    systemctl daemon-reload
fi
