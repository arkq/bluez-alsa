install_data('20-bluealsa.conf', install_dir: alsaconfdir)

ctl_sources = [
    '../shared/ctl-client.c',
    '../shared/log.c',
    'bluealsa-ctl.c',
]

pcm_sources = [
    '../shared/ctl-client.c',
    '../shared/log.c',
    '../shared/rt.c',
    'bluealsa-pcm.c',
]

ctldeps = [threading, alsa]
pcmdeps = [threading, alsa, bluez, glib]
include = [top_inc, src_inc]

shared_library('asound_module_ctl_bluealsa', ctl_sources, dependencies: ctldeps,
               include_directories: include, install: true, c_args: '-DPIC',
               install_dir: alsalibdir)

shared_library('asound_module_pcm_bluealsa', pcm_sources, dependencies: pcmdeps,
               include_directories: include, install: true, c_args: '-DPIC',
               install_dir: alsalibdir)
