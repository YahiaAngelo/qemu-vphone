common_ss.add(files('accel-common.c'))
specific_ss.add(files('accel-target.c'))
# vphone-linux: guest mappings outside system memory (the SEP's address space)
specific_ss.add(when: 'CONFIG_KVM', if_true: files('accel-private.c'))
system_ss.add(files('accel-system.c', 'accel-blocker.c', 'accel-qmp.c'))

subdir('tcg')
if have_system
  subdir('hvf')
  subdir('kvm')
  subdir('xen')
  subdir('stubs')
endif
