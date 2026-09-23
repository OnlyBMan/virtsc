# Building VirTSC on macOS
This guide provides a walkthrough on building VirTSC on macOS. It covers both Apple Silicon (M1 and later) and Intel Macs. If you are not using macOS, do not follow this guide.

What's different on macOS, in short:

- **No KVM.** `qemu-system-i386` on macOS runs only under **TCG** (software emulation), on both Apple Silicon and Intel. Hypervisor.framework (HVF) in this tree only accelerates `x86_64-softmmu`, not `i386-softmmu`. A Pentium III guest is still very usable under TCG on a modern Mac.
- **OSMesa is not in Homebrew.** The `is1gl` device needs OSMesa for its host GL context. Mesa removed OSMesa in 25.1, and Homebrew ships newer Mesa, so you build Mesa **25.0.x** yourself once (Step 2).
- **Use the native Cocoa display** instead of GTK. GTK works too, but it's optional.

# Step 1: Prerequisites
Install the Xcode Command Line Tools if you haven't already:
```bash
xcode-select --install
```
Install [Homebrew](https://brew.sh) if you don't have it, then install the build dependencies:
```bash
brew install git meson ninja pkgconf glib pixman libslirp sdl2 python bison flex llvm zstd
```
**Optional:** if you want the GTK display instead of the native Cocoa one, also run:
```bash
brew install gtk+3
```
Clone the **qemu-is1** and **virtsc** repositories:
```bash
git clone https://github.com/VirTSC/qemu-is1
git clone https://github.com/VirTSC/virtsc
```
Great. You should have everything you need at this point, aside from a valid IntelliSTAR 1 image. For ease of setup without deviations, an unmodified, raw image is recommended.

> **Heads up:** if you already have Homebrew's `qemu` installed (`brew list qemu`), its `qemu-system-i386` is **not** qemu-is1 and doesn't have the `thunderstorm` or `is1gl` devices. Keep an eye on which binary you're running.

# Step 2: Building OSMesa
`is1gl` replays the guest's OpenGL calls on the host using OSMesa, which renders off-screen in software. It's the only one of QEMU's GL backends that exists on macOS, since macOS has no EGL. If QEMU is built without OSMesa, the `is1gl` device falls back to a stub and the IS1's graphics acceleration won't work.

We'll build Mesa 25.0.x (the last series that includes OSMesa) with the **llvmpipe** driver and install it into `~/opt/osmesa`, so it stays separate from Homebrew.

Create a Python virtualenv with Mesa's build-time Python modules:
```bash
python3 -m venv ~/opt/mesa-venv
~/opt/mesa-venv/bin/pip install mako pyyaml packaging
```
Download and extract Mesa. Any 25.0.x release should work; 25.0.7 is used here:
```bash
mkdir -p ~/src && cd ~/src
curl -LO https://archive.mesa3d.org/mesa-25.0.7.tar.xz
tar xf mesa-25.0.7.tar.xz
cd mesa-25.0.7
```
Configure Mesa. Homebrew's `llvm`, `bison` and `flex` are keg-only, so they're added to `PATH` for this shell. The system `bison` is too old for Mesa.
```bash
export PATH="$HOME/opt/mesa-venv/bin:$(brew --prefix llvm)/bin:$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"

meson setup build \
  --prefix="$HOME/opt/osmesa" \
  --buildtype=release \
  -Dosmesa=true \
  -Dgallium-drivers=llvmpipe \
  -Dvulkan-drivers= \
  -Dplatforms= \
  -Dglx=disabled \
  -Degl=disabled \
  -Dgbm=disabled \
  -Dgles1=disabled \
  -Dgles2=disabled \
  -Dllvm=enabled \
  -Dshared-llvm=disabled \
  -Dc_link_args="-L$(brew --prefix)/lib" \
  -Dcpp_link_args="-L$(brew --prefix)/lib"
```
The `-L$(brew --prefix)/lib` link arguments are required. Statically linked LLVM pulls in `-lzstd`, `-lz` and `-lxml2`, and Apple's linker doesn't search `/opt/homebrew/lib` on its own. Without them, the build stops with `ld: library 'zstd' not found`. If you already ran `meson setup` without them, add them to the existing build with:
```bash
meson configure build -Dc_link_args="-L$(brew --prefix)/lib" -Dcpp_link_args="-L$(brew --prefix)/lib"
```
Build and install:
```bash
ninja -C build
ninja -C build install
```
Check that pkg-config can find it:
```bash
PKG_CONFIG_PATH="$HOME/opt/osmesa/lib/pkgconfig" pkg-config --modversion osmesa
```
This should print a version such as `8.0.0`.

> **If the LLVM build gives you trouble,** you can build with the slower **softpipe** driver, which doesn't need LLVM. Replace `-Dgallium-drivers=llvmpipe`, `-Dllvm=enabled` and `-Dshared-llvm=disabled` with `-Dgallium-drivers=softpipe -Dllvm=disabled`, then run `meson setup --wipe build ...` again.

# Step 3: Configuring & Building qemu-is1
Now it's time to configure and build **qemu-is1**. Go to the directory where you cloned **qemu-is1** and create a **build** folder:
```bash
cd qemu-is1
mkdir -p build
cd build
```
Configure the build. `PKG_CONFIG_PATH` is how QEMU finds the OSMesa you just built.
```bash
PKG_CONFIG_PATH="$HOME/opt/osmesa/lib/pkgconfig" \
../configure --target-list=i386-softmmu --enable-osmesa --disable-opengl --enable-cocoa --enable-sdl --enable-slirp
```
If you installed `gtk+3` and want the GTK display too, add `--enable-gtk` to the end.

`--enable-osmesa` makes configure **fail** if it can't find OSMesa, so you never end up with a stub `is1gl` by accident. If configure fails, check Step 1, then check the `pkg-config` command at the end of Step 2. In the configure summary, **TCG support** and **slirp support** should both say **YES**.

Build (this uses all of your CPU cores):
```bash
make -j"$(sysctl -n hw.ncpu)"
```
Check that the IntelliSTAR devices are in the binary:
```bash
./qemu-system-i386 -device help | grep -E 'thunderstorm|is1gl'
```
You should see both `thunderstorm` and `is1gl` listed.

**Optional (but recommended):** add the build folder to your `PATH`. macOS uses **zsh** by default, so this goes in `~/.zshrc`, not `~/.bashrc`:
```bash
echo 'export PATH="/Users/YOUR-USERNAME/FOLDER-WHERE-YOU-CLONED-QEMU-IS1/build:$PATH"' >> ~/.zshrc
source ~/.zshrc
```
Replace **YOUR-USERNAME** and **FOLDER-WHERE-YOU-CLONED-QEMU-IS1** with your own values. *Only do this if you don't have a conflicting QEMU (such as Homebrew's) earlier in your PATH.* Run `which qemu-system-i386` to check which one wins.

### Troubleshooting
- **`qemu-system-i386: -device thunderstorm: ... not found`.** You're running a different QEMU (probably Homebrew's). Use the full path to `qemu-is1/build/qemu-system-i386`.
- **`dyld: Library not loaded: .../libOSMesa...`.** The OSMesa install in `~/opt/osmesa` was moved or deleted. Reinstall it (Step 2), or rebuild QEMU if you changed the prefix.

### Next: [Running the VM](../RUN.md)
