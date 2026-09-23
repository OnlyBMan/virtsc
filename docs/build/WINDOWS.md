# Building VirTSC on Windows

This guide provides a walkthrough on building VirTSC directly on **64-bit Windows**. If you are not using Windows, do not follow this guide.

We will be using **MSYS2 MINGW64** to build the Windows binaries and **Mesa OSMesa** for software rendering through llvmpipe/softpipe. This does **not** use WSL, a Linux build VM, EGL, or the host OpenGL driver for `is1gl`.

# Step 1: Prerequisites

Clone the **qemu-is1** and **virtsc** repositories:
```bash
git clone https://github.com/VirTSC/qemu-is1
git clone https://github.com/VirTSC/virtsc
```

We need to ensure we have everything required to build VirTSC. First, download the **x86-64 MSYS2 installer** from the [MSYS2 website](https://www.msys2.org/docs/installer/) and install it in **`C:\msys64`**.

Find **MSYS2 MINGW64** in the Start menu, right-click it, and select **Run as administrator**. Keep using this elevated MINGW64 shell for all compilation steps. **Do not use the plain MSYS shell, UCRT64, Git Bash, WSL, or an ordinary Command Prompt to build.**

### The following build commands should be performed in MSYS2 MINGW64, running as Administrator.

Confirm that you opened the correct environment:

```bash
echo "$MSYSTEM"
```

It must print:

```text
MINGW64
```

Now, update MSYS2:

```bash
pacman -Syu
```

If you are asked to close the terminal, do so. Reopen **MSYS2 MINGW64 as Administrator** and run the update again:

```bash
pacman -Syu
```

With MSYS2 updated, install the required packages:

```bash
pacman -S --needed \
  base-devel git curl tar xz flex bison \
  mingw-w64-x86_64-toolchain \
  mingw-w64-x86_64-gcc \
  mingw-w64-x86_64-clang \
  mingw-w64-x86_64-llvm \
  mingw-w64-x86_64-meson \
  mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf \
  mingw-w64-x86_64-python \
  mingw-w64-x86_64-python-mako \
  mingw-w64-x86_64-python-yaml \
  mingw-w64-x86_64-glib2 \
  mingw-w64-x86_64-pixman \
  mingw-w64-x86_64-SDL2 \
  mingw-w64-x86_64-gtk3 \
  mingw-w64-x86_64-dtc
```

Accept the default selection when `pacman` asks which members of the **`mingw-w64-x86_64-toolchain`** group to install.

Make sure the native MinGW tools are first on your `PATH`:

```bash
which gcc meson ninja python pkg-config
gcc --version
```

The paths should begin with **`/mingw64/bin/`**.

## Preparing the source folders

For this walkthrough, we will use the following locations:

```text
C:\msys64                     MSYS2 installation
C:\Users\i1\qemu-is1          QEMU source
C:\Users\i1\virtsc            VirTSC guest source
C:\vtsc-build\mesa-24.3.4     Mesa source
C:\vtsc-build\mesa-build      Mesa build directory
C:\vtsc-build\osmesa          Mesa installation
C:\vtsc-build\qemu-build      QEMU build directory
C:\vtsc-build\qemu-install    QEMU installation
C:\IS1                        disk image and VM logs
```

The two source paths are the examples used in the Windows build instructions. **Replace `C:\Users\i1\qemu-is1` and `C:\Users\i1\virtsc` throughout this guide if your adjusted source folders are elsewhere.** Their MINGW64 equivalents begin with **`/c/Users/i1/`**. Keep installation, source, and build paths short, ASCII-only, and free of spaces.

Check that the source folders exist, then create the build and VM folders:

```bash
test -d /c/Users/i1/qemu-is1
test -d /c/Users/i1/virtsc
mkdir -p /c/vtsc-build /c/IS1
```

Both source-folder checks should succeed before you continue. Running MINGW64 as Administrator also allows QEMU's normal Python installer to create its symbolic links; no symlink-copy source workaround is used.

The recorded test environment used **Windows x64**, **MSYS2 MINGW64**, **GCC 16.2.0**, **Meson 1.12.0**, **Ninja 1.13.2**, **LLVM 22.1.8**, **Mesa 24.3.4**, and **QEMU 11.1.1** with the `is1gl` and `thunderstorm` devices.

Great. You should now have the prerequisites in place, aside from a valid **IntelliSTAR 1 disk image**. An unmodified image is recommended and is **not included** with VirTSC or this guide.

# Step 2: Configuring & Building

Now it's time to build **Mesa OSMesa** and **qemu-is1**. Please be mindful of the fact that you are building QEMU binaries: pay attention to which QEMU installation you are using if another version is already on your `PATH`.

**Keep using MSYS2 MINGW64 as Administrator for this entire step.**

## Building Mesa OSMesa

We need a local Mesa build to provide **`osmesa.dll`** and the software renderers. This procedure is pinned to **Mesa 24.3.4**. **Do not substitute a newer version**; this is the version used by the Windows build instructions for the required OSMesa frontend.

Download and extract it:

```bash
cd /c/vtsc-build
curl -LO https://archive.mesa3d.org/mesa-24.3.4.tar.xz
tar -xf mesa-24.3.4.tar.xz
```

Configure a release build with the Windows Gallium OSMesa frontend and the llvmpipe/softpipe software renderers:

```bash
meson setup /c/vtsc-build/mesa-build /c/vtsc-build/mesa-24.3.4 \
  --buildtype=release \
  --prefix=C:/vtsc-build/osmesa \
  --default-library=shared \
  -Dplatforms=windows \
  -Dgallium-drivers=llvmpipe,softpipe \
  -Dvulkan-drivers= \
  -Dosmesa=true \
  -Degl=disabled \
  -Dglx=disabled \
  -Dgles1=disabled \
  -Dgles2=disabled \
  -Dllvm=enabled \
  -Dshared-llvm=enabled \
  -Dshared-glapi=disabled \
  -Dvideo-codecs= \
  -Dlibunwind=disabled \
  -Dvalgrind=disabled \
  -Dbuild-tests=false \
  -Dtools=
```

If configuration succeeded, build and install Mesa:

```bash
meson compile -C /c/vtsc-build/mesa-build
meson install -C /c/vtsc-build/mesa-build
```

Check that the important files were installed and that `pkg-config` can find OSMesa:

```bash
test -f /c/vtsc-build/osmesa/bin/osmesa.dll
test -f /c/vtsc-build/osmesa/include/GL/osmesa.h
test -f /c/vtsc-build/osmesa/lib/pkgconfig/osmesa.pc
PKG_CONFIG_PATH=C:/vtsc-build/osmesa/lib/pkgconfig \
  pkg-config --modversion osmesa
```

The version should be **`8.0.0`**. The Mesa configuration summary should report:

```text
Platform: windows
Drivers: llvmpipe softpipe
Off-screen rendering (OSMesa): libosmesa
LLVM: enabled
```

## Building qemu-is1

Great. Now make the OSMesa installation visible to the build tools and runtime probes:

```bash
export PATH="/c/vtsc-build/osmesa/bin:$PATH"
export PKG_CONFIG_PATH="C:/vtsc-build/osmesa/lib/pkgconfig;C:/msys64/mingw64/lib/pkgconfig;C:/msys64/mingw64/share/pkgconfig"
```

Create a clean, separate QEMU build folder and change into it:

```bash
mkdir -p /c/vtsc-build/qemu-build
cd /c/vtsc-build/qemu-build
```

Configure the native Windows build:

```bash
/c/Users/i1/qemu-is1/configure \
  --target-list=x86_64-softmmu \
  --enable-whpx \
  --enable-osmesa \
  --enable-gtk \
  --enable-sdl \
  --enable-dsound \
  --enable-slirp \
  --enable-download \
  --disable-werror \
  --prefix=/c/vtsc-build/qemu-install
```

**Yes, the target is `x86_64-softmmu`, not `i386-softmmu`.** The Windows build instructions use **`qemu-system-x86_64.exe`** to expose WHPX while still running the required 32-bit Pentium III guest.

The **`--enable-download`** option allows QEMU to obtain its bundled slirp subproject when a compatible system libslirp is unavailable.

Before building, check the configuration summary. It should include:

```text
OSMesa support: YES
WHPX support: YES
TCG support: YES
SDL support: YES
GTK support: YES
slirp support: YES
```

If configuration fails, **check the packages from Step 1 and the OSMesa installation above**. If it succeeds, build and install QEMU:

```bash
ninja
ninja install
```

If the build reports a missing symlink privilege, close every MSYS2 window, reopen **MSYS2 MINGW64 as Administrator**, and configure in a new build directory. This procedure keeps QEMU's normal symlink behavior.

## Checking the build

Keep the OSMesa and MinGW DLL directories on your `PATH` while testing:

```bash
export PATH="/c/vtsc-build/osmesa/bin:/c/vtsc-build/qemu-install:/mingw64/bin:$PATH"
```

Check the executables and available accelerators:

```bash
/c/vtsc-build/qemu-install/qemu-system-x86_64.exe --version
/c/vtsc-build/qemu-install/qemu-img.exe --version
/c/vtsc-build/qemu-install/qemu-system-x86_64.exe -accel help
```

The accelerator list should contain **`whpx`** and **`tcg`**.

Next, make sure the custom devices are present:

```bash
/c/vtsc-build/qemu-install/qemu-system-x86_64.exe -device help | \
  grep -E 'is1gl|thunderstorm'
```

You should see:

```text
is1gl
thunderstorm
```

Finally, check OSMesa once more:

```bash
pkg-config --modversion osmesa
```

The expected version is **`24.3.4`**. If these checks pass, you're ready to create the VM.

### Next: [Running the VM](../RUN.md)

