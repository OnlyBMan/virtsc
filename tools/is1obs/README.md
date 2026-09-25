# IntelliStar TSC source for OBS

This native OBS source consumes QEMU's ITS2 FIFO directly. It sends 720×480
BGRA fill/key frames and the card's 48 kHz stereo audio to OBS without an SDL
window, screen capture, network transport, or another encoder. It does not
change QEMU, the qcow2 image, or IS1GL.

Build against the public headers for the installed OBS version (currently
32.2.2). The Makefile expects an OBS source checkout at
`/tmp/obs-studio-32.2.2-sdk` and SIMDe headers at `/tmp/is1obs-simde`.
Those temporary checkouts can be recreated with:

```sh
git clone --depth 1 --filter=blob:none --sparse --branch 32.2.2 \
  https://github.com/obsproject/obs-studio.git /tmp/obs-studio-32.2.2-sdk
git -C /tmp/obs-studio-32.2.2-sdk sparse-checkout set libobs
git clone --depth 1 https://github.com/simd-everywhere/simde.git /tmp/is1obs-simde
```

Run `make` in this directory. The result is `build/is1obs.plugin`. Copy that
bundle into your OBS plugin directory and restart OBS. Add an
**IntelliStar TSC (ITS2 FIFO)** source; its default path is the current QEMU
`build/is1-output` FIFO and can be changed in the source's properties.

Only one process should read the FIFO. Close `is1view` before using this OBS
source; two readers split the bytes between them. The source preserves the
TSC alpha/key channel.
