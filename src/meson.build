sources = [
    'shared/ffb.c',
    'shared/log.c',
    'shared/rt.c',
    'at.c',
    'bluealsa.c',
    'bluez.c',
    'bluez-a2dp.c',
    'bluez-iface.c',
    'ctl.c',
    'io.c',
    'rfcomm.c',
    'transport.c',
    'utils.c',
    'main.c',
]

deps = [
    threading,
    math,
    clock,
    bluez,
    glib,
    gio,
    aac,
    aptx,
    ldac,
    ldac_abr,
    sbc
]

executable('bluealsa', sources: sources, dependencies: deps, install: true,
           install_dir: ba_bindir, include_directories: top_inc)
