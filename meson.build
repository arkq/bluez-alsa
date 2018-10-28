project('bluez-alsa', 'c',  version: '1.3.1', license: 'MIT',
        meson_version: '>= 0.37.0')

# Setup path for instalation
ba_prefix = get_option('prefix')
ba_bindir = join_paths(ba_prefix, get_option('bindir'))
ba_datadir = join_paths(ba_prefix, get_option('datadir'))
ba_localstatedir = join_paths(ba_prefix, get_option('localstatedir'))
ba_runstatedir = join_paths(ba_localstatedir, 'run')
ba_sysconfdir = join_paths(ba_prefix, get_option('sysconfdir'))

# Configuration data for config.h
conf = configuration_data()
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('RUN_STATE_DIR', ba_runstatedir)
conf.set10('ENABLE_DEBUG', get_option('debug'))
conf.set10('ENABLE_DEBUG_TIME', get_option('debug-time'))
conf.set10('ENABLE_PAYLOADCHECK', get_option('payloadcheck'))

# Needed when using config.h
add_global_arguments('-DHAVE_CONFIG_H', language: 'c')

# Setup include directories
top_inc = include_directories('.')
src_inc = include_directories('./src')

# Core dependencies
cc = meson.get_compiler('c')

if not cc.has_function('register_printf_specifier')
    error('function register_printf_specifier not found')
endif

math = cc.find_library('m', required : true)
clock = cc.find_library('rt', required: true)
threading = dependency('threads', required: true)
alsa = dependency('alsa', required: true)
bluez = dependency('bluez', version: '>= 5.0', required: true)
glib = dependency('glib-2.0', version: '>= 2.26', required: true)
gio = dependency('gio-unix-2.0', required: true)
sbc = dependency('sbc', version: '>= 1.2', required: true)

# set alsa directories to correct paths
alsalibdir = join_paths(alsa.get_pkgconfig_variable('libdir'), 'alsa-lib')

if get_option('alsaconfdir') != ''
    alsaconfdir = get_option('alsaconfdir')
elif alsa.version() >= '1.1.7'
    alsaconfdir = join_paths(ba_sysconfdir, 'alsa/conf.d')
else
    alsaconfdir = join_paths(ba_datadir, 'alsa/alsa.conf.d')
endif

# AAC Support
aac = dependency('fdk-aac', version: '>= 0.1.1', required: get_option('aac'))
conf.set10('ENABLE_AAC', get_option('aac'))

# apt-X support
aptx = dependency('openaptx', version: '>= 1.0.0', required: get_option('aptx'))
conf.set10('ENABLE_APTX', get_option('aptx'))

# LDAC support
ldac = dependency('libldacBT', version: '>= 2.0.0', required: get_option('ldac'))
ldac_abr = dependency('libldacBT_abr', version: '>= 2.0.0', required: get_option('ldac'))
conf.set10('ENABLED_LDAC', get_option('ldac'))

# hcitop deps
libbsd = dependency('libbsd', required: get_option('hcitop'))
ncurses = dependency('ncurses', required: get_option('hcitop'))

# rfcomm deps
readline = cc.find_library('readline', required: get_option('rfcomm'))

configure_file(output : 'config.h', configuration : conf)

subdir('src')
subdir('src/asound')
subdir('test')
subdir('utils')

if get_option('aptx')
    message('             *** aptX encoder support ***')
    message('You have enabled support for aptX encoding. Note, that')
    message('by default bluez-alsa will use openaptx library, which')
    message('does NOT perform any encoding/decoding.\n')
endif

if alsa.version() > '1.1.2'
    message('             *** alsa-lib >= 1.1.2 ***')
    message('Starting from alsa-lib 1.1.2, it is possible to enable')
    message('thread-safe API functions. Unfortunately, this feature')
    message('is not mature enough - software plugins may experience')
    message('random deadlocks (bluez-alsa PCM plugin is affected')
    message('Either compile alsa-lib without this feature or export')
    message('LIBASOUND_THREAD_SAFE=0 while using bluealsa PCM.\n')
endif
