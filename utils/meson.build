aplay_sources = [
    '../src/shared/ctl-client.c',
    '../src/shared/ffb.c',
    '../src/shared/log.c',
    'aplay.c',
]

aplay_deps = [threading, alsa, bluez, gio]

rfcomm_sources = [
    '../src/shared/ctl-client.c',
    '../src/shared/log.c',
    'rfcomm.c',
]

rfcomm_deps = [bluez, readline]

hcitop_sources = ['hcitop.c']

hcitop_deps = [bluez, libbsd, ncurses]

if get_option('aplay')
    executable('bluealsa-aplay', aplay_sources, dependencies: aplay_deps,
    install_dir: ba_bindir, include_directories: [top_inc, src_inc],
    install: true)
endif

if get_option('rfcomm')
    executable('bluealsa-rfcomm', rfcomm_sources, dependencies: rfcomm_deps,
    install_dir: ba_bindir, include_directories: [top_inc, src_inc],
    install: true)
endif

if get_option('hcitop')
    executable('hcitop', hcitop_sources, dependencies: hcitop_deps,
    install_dir: ba_bindir, include_directories: top_inc,
    install: true)
endif
