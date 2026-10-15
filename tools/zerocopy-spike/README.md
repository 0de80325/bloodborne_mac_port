# Zero-copy spike (Phase 0)

Proves `VK_EXT_external_memory_host` buffer import through MoltenVK on Apple
Silicon, the primitive Phase 1 needs to serve `BufferCache` ranges straight
from the pool entry instead of the arena mirror.

- `spike0.c`: extension present? `minImportedHostPointerAlignment`?
- `spike1.c`: import a 16 MiB host range as a `VkBuffer`, verify bytes both
  directions (mapped read matches host pattern; write through mapping visible
  through the host pointer).

Build (arm64, from repo root):

```sh
export PATH="/opt/homebrew/bin:$PATH"
export PKG_CONFIG_LIBDIR=$PWD/deps/arm64/lib/pkgconfig:$PWD/deps/arm64/share/pkgconfig
unset PKG_CONFIG_PATH
/usr/bin/clang -arch arm64 -mmacosx-version-min=15.0 -O2 $(pkg-config --cflags vulkan) \
  tools/zerocopy-spike/spike0.c -o /tmp/spike0 $(pkg-config --libs vulkan) \
  -Wl,-rpath,$PWD/deps/arm64/lib
VK_DRIVER_FILES=$PWD/deps/arm64/share/vulkan/icd.d/MoltenVK_icd.json /tmp/spike0
# same for spike1.c
```

Result on M2 Pro (2026-10-15): extension YES, alignment 0x4000, IMPORT-OK.
