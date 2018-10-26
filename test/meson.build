deps = [threading, alsa, bluez, gio, glib, aac, aptx, ldac, ldac_abr, sbc, math]

test_at = executable('test-at', 'test-at.c', dependencies: deps,
                     include_directories: [top_inc, src_inc])

test_io = executable('test-io', 'test-io.c', dependencies: deps,
                     include_directories: [top_inc, src_inc])

test_utils = executable('test-utils', 'test-utils.c', dependencies: deps,
                        include_directories: [top_inc, src_inc])

test_pcm = executable('test-pcm', 'test-pcm.c', dependencies: deps,
                      include_directories: [top_inc, src_inc])

server_mock = executable('server-mock', 'server-mock.c', dependencies: deps,
                         include_directories: [top_inc, src_inc])

test('Testing at', test_at)
test('Testing io', test_io)
test('Testing utils', test_utils)

# these test require patched alsa
if get_option('pcm-test')
    test('Test server', server_mock)
    test('Testing pcm', test_pcm)
endif
