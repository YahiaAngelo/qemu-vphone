arm_common_ss.add(when: 'CONFIG_APPLE_SOC', if_true: tasn1)

# sep.c uses nettle's ECC (hogweed) and hashes (nettle). QEMU may build its own
# crypto on gnutls and leave both off the link line, so require them explicitly
# for the SoC sources; otherwise the final link fails with
# "nettle_ecc_point_set / nettle_sha384_digest ... DSO missing from command line".
sep_hogweed = dependency('hogweed', method: 'pkg-config', required: true)
sep_nettle = dependency('nettle', method: 'pkg-config', required: true)
sep_gmp = cc.find_library('gmp', required: true)  # hogweed bignum backend
arm_common_ss.add(when: 'CONFIG_APPLE_SOC',
                  if_true: [sep_hogweed, sep_nettle, sep_gmp])

# Optional: the agxemu library executes the jobs the GPU firmware kicks.
agxemu_dir = get_option('agxemu')
if agxemu_dir != ''
  agxemu = declare_dependency(
    dependencies: [cc.find_library('agxemu', dirs: agxemu_dir / 'build',
                                   static: true),
                   cc.find_library('m', required: false)],
    include_directories: include_directories(agxemu_dir / 'include'),
    compile_args: ['-DCONFIG_AGXEMU'])
  arm_common_ss.add(when: 'CONFIG_APPLE_SOC', if_true: agxemu)
endif
arm_common_ss.add(when: 'CONFIG_APPLE_DART', if_true: files('dart.c'),
                                             if_false: files('dart-stub.c'))
arm_common_ss.add(when: 'CONFIG_APPLE_SART', if_true: files('sart.c'))
arm_common_ss.add(when: 'CONFIG_APPLE_SOC', if_true: files(
    'a13.c',
    'a13_gxf.c',
    'a9.c',
    'boot.c',
    'dt.c',
    'mem.c',
    'mt-spi.c',
    'sep-sim.c',
    'sep.c',
    'gfx_asc_fw.c',
    'patcher.c',
    'kernel_patches.c',
    'lm-backlight.c',
    's8000.c',
    't8030.c',
))
