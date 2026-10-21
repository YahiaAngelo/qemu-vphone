# qemu-vphone

QEMU with the machine model and changes that
[vphone-linux](https://github.com/YahiaAngelo/vphone-linux) needs to run vphone-cli's virtual
iPhone (an iOS guest on Apple's PCC `vresearch101` virtual platform) on a Linux x86_64 host.

This is a fork of [ChefKissInc/QEMUAppleSilicon](https://github.com/ChefKissInc/QEMUAppleSilicon),
itself derived from [QEMU](https://www.qemu.org). It is not affiliated with, or endorsed by, either
project, and, as the upstream branding notice requires, it does not use that project's name or
artwork.

## What it adds

On the `vphone` branch:

- `-M vresearch101`: Apple's virtual research platform (the board Virtualization.framework
  presents to an iOS guest), with its SEP coprocessor, GIC, AVP devices, NVRAM and firmware store.
- `-cpu apple-gxf`: an AArch64 CPU with Apple's GXF (guarded execution) extension.
- `usb-touchscreen` (a Virtualization.framework-compatible HID touchscreen), xHCI and hub fixes,
  and vhost-vsock queue sizing for iOS's virtio drivers.
- The `apple-gfx-mmio` paravirtual GPU device, backed by
  [reims-vgpu](https://github.com/steelbrain/reims-vgpu), which links into QEMU as a static library.
- TCG performance work for iOS guests (16 KiB target pages, jump cache and lookup changes, TLB
  tuning), each switchable by an environment variable documented in vphone-linux's
  `docs/PERFORMANCE.md`.

Build and usage instructions are in vphone-linux; this repository is consumed there as a submodule.

## License

QEMU as a whole is licensed under the GNU General Public License, version 2. The upstream project
this fork is based on is licensed under the GNU General Public License, version 3, with its own code
under the GNU Affero General Public License, version 3. Parts of QEMU licensed under "version 2 or
later" keep their original license and copyright holders. Individual files carry their own license
headers. See `LICENSE` for the full notices.

## Disclaimer

This software is provided "as is", without warranty of any kind. It is intended for lawful
research and development only: you are responsible for complying with all applicable laws,
licenses and agreements, including Apple's software license agreements. This project does not
include, and you must not use it to distribute, Apple firmware or software.

QEMU is a trademark of Fabrice Bellard. iOS and Apple are trademarks of Apple Inc. Neither is
affiliated with this project.
