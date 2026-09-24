# Building VirTSC in WSL

This guide walks through building VirTSC on **Windows** inside **WSL 2** (Windows Subsystem for Linux). You get the same Linux build as the [Debian guide](DEBIAN.md), running under Windows, with **KVM** hardware acceleration instead of the software emulation used by the [native Windows build](WINDOWS.md).

If you are on Linux, use the [Debian guide](DEBIAN.md) instead. If you can't use WSL 2 (see below), use the [native Windows guide](WINDOWS.md).

> **Why WSL?** The native Windows build runs the IS1 under TCG, QEMU's software CPU, which is much slower than hardware acceleration. Inside WSL, the VM runs under KVM, just like the Debian build.

# Step 1: Prerequisites

## What you need

- **Windows 11.** WSL 2 needs to expose KVM to Linux ("nested virtualization"). Windows 11 supports this on **Intel** CPUs. Support for **AMD** CPUs arrived later and depends on your Windows build, so AMD users should check `/dev/kvm` below before going further.
- **Virtualization enabled in your BIOS/UEFI** (Intel VT-x or AMD-V).
- **Hyper-V gets switched on.** WSL 2 runs on Windows' hypervisor, so installing it turns that on. If you rely on another hypervisor that doesn't coexist with Hyper-V, keep that in mind.

## Installing WSL and Ubuntu

Open **PowerShell** and check whether WSL is already installed:

```powershell
wsl --version
```

If it prints a version, make sure it's current:

```powershell
wsl --update
```

If WSL isn't installed yet, install it with Ubuntu. Run this in **PowerShell as Administrator**, then **reboot** when it asks:

```powershell
wsl --install -d Ubuntu
```

After the reboot, open **Ubuntu** from the Start menu. The first launch asks you to create a Linux username and password. You'll need that password for `sudo`.

Check that your distribution is running under **WSL 2**, not WSL 1:

```powershell
wsl -l -v
```

The `VERSION` column should say **2**. If it says 1, convert it with `wsl --set-version Ubuntu 2`.

### From here on, commands run in the Ubuntu (WSL) terminal unless a step says PowerShell.

## Checking for KVM

This is the step that decides whether WSL can accelerate the IS1. In Ubuntu, run:

```bash
ls -l /dev/kvm
```

You should see something like:

```text
crw-rw---- 1 root kvm 10, 232 ... /dev/kvm
```

If you get **`No such file or directory`**, turn on nested virtualization. In PowerShell, create or edit **`%UserProfile%\.wslconfig`** so it contains:

```ini
[wsl2]
nestedVirtualization=true
```

Then restart WSL from PowerShell:

```powershell
wsl --shutdown
```

Reopen Ubuntu and check `/dev/kvm` again. If it's still missing, your CPU or Windows build doesn't support nested virtualization under WSL, and you should use the [native Windows guide](WINDOWS.md) instead.

## Installing the build packages

Update Ubuntu, then install everything the build needs:

```bash
sudo apt-get update
sudo apt-get install -y bison bzip2 ca-certificates ccache findutils flex gcc git \
  libc6-dev libfdt-dev libffi-dev libglib2.0-dev libpixman-1-dev locales make \
  meson ninja-build pkg-config libosmesa6 libosmesa6-dev cmake libgtk-3-dev \
  libsdl2-dev libslirp-dev python3-venv
```

This is the same as the [Debian](DEBIAN.md) package list.

## Allowing your user to use KVM

`/dev/kvm` belongs to the `kvm` group. Add yourself to it:

```bash
sudo usermod -aG kvm $USER
```

Group changes only apply to new sessions. From **PowerShell**, restart WSL:

```powershell
wsl --shutdown
```

Reopen Ubuntu and confirm:

```bash
id | grep -o kvm
test -w /dev/kvm && echo "KVM OK"
```

You should see `kvm` and `KVM OK`. If you skip this, QEMU fails later with `Could not access KVM kernel module: Permission denied`.

## Turning off EPT for KVM

**This step is required.** Inside WSL, KVM runs *nested* on top of Windows' Hyper-V. With KVM's default settings, the IS1's FreeBSD kernel panics a moment after `Booting [kernel]...`:

```text
Fatal trap 12: page fault while in vm86 mode
...
Stopped at      0xa00:  cli
db>
```

FreeBSD makes BIOS calls in virtual-8086 (vm86) mode while it boots, and Hyper-V appears to mishandle that mode for nested guests. It's the same panic the native Windows build gets under WHPX, which also runs on Hyper-V. Turning off KVM's **EPT** (hardware-assisted paging) makes KVM handle the guest's page tables and vm86 mode itself, which avoids the bug. The VM is somewhat slower this way, but still far faster than software emulation.

Make the setting permanent, so it survives `wsl --shutdown` and Windows restarts:

```bash
echo "options kvm_intel ept=0" | sudo tee /etc/modprobe.d/kvm-intel.conf
```

Apply it now by reloading the KVM module. **No VM can be running while you do this**:

```bash
sudo modprobe -r kvm_intel
sudo modprobe kvm_intel
```

Check that it took effect:

```bash
cat /sys/module/kvm_intel/parameters/ept
```

It must print **`N`**. If it prints `Y`, the IS1 will panic at boot. If `modprobe -r` reports that the module is in use, close any running QEMU and try again. Turning off EPT also turns off "unrestricted guest" mode (`/sys/module/kvm_intel/parameters/unrestricted_guest` shows `N`), which is expected.

These instructions are for Intel CPUs (`kvm_intel`). On AMD the module is `kvm_amd` and the equivalent setting is `npt=0`; that combination hasn't been tested.

## Cloning the repositories

**Clone inside the Linux filesystem, in your home directory, not under `/mnt/c`.** This is the most common source of trouble:

- Files under `/mnt/c` are reached through a translation layer that makes the build many times slower.
- A repository cloned with **Git for Windows** usually has Windows (CRLF) line endings. Shell scripts from it then fail with errors like `$'\r': command not found` or `/bin/sh^M: bad interpreter`.

Clone both repositories with Ubuntu's own `git`:

```bash
cd ~
git clone https://github.com/VirTSC/qemu-is1
git clone https://github.com/VirTSC/virtsc
```

Your Linux home folder is also reachable from Windows Explorer at **`\\wsl$\Ubuntu\home\<your-username>`**, if you want to browse the files.

# Step 2: Configuring & Building

Create a build folder inside **qemu-is1**:

```bash
cd ~/qemu-is1
mkdir -p build
cd build
```

Configure the build:

```bash
../configure --target-list=i386-softmmu --enable-osmesa --disable-opengl \
  --enable-gtk --enable-sdl --enable-slirp
```

Now build, using all your CPU cores:

```bash
make -j$(nproc)
```

## Checking the build

Check the binary, the accelerators and the custom devices:

```bash
./qemu-system-i386 --version
./qemu-system-i386 -accel help
```

The accelerator list should contain **`kvm`** and **`tcg`**.


**Optional (but recommended):** Add the build folder to your bashrc so that the binaries are available in your system PATH. Do this by adding `export PATH="/home/YOUR-USERNAME/FOLDER-WHERE-YOU-CLONED-QEMU-IS1/build:$PATH"` to the bottom of your `~/.bashrc` file. Be sure to replace **YOUR-USERNAME** and **FOLDER-WHERE-YOU-CLONED-QEMU-IS1** accordingly, otherwise this will do nothing. *Only do this if you do not have a conflicting version of qemu installed on your machine.*

### Next: [Setting up the guest](../GUEST.md)
