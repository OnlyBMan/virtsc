# Setting up VirTSC on Windows

This guide provides a walkthrough on installing and configuring VirTSC directly on **64-bit Windows**. If you are not using Windows, do not follow this guide.

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

The version should be **`24.3.4`**. The Mesa configuration summary should report:

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

# Step 3: Creating the VM

First things first: have an **unmodified IntelliSTAR 1 disk image** handy. We created **`C:\IS1`** earlier to hold the image, startup scripts, and logs. For this example, place your raw image at **`C:\IS1\weatherscan.img`**.

The build is finished, so **the VM can run from a normal, non-elevated PowerShell window**.

## Optional: Enabling WHPX acceleration

The launcher will attempt **WHPX** first and automatically fall back to **TCG** if WHPX is unavailable.

To enable WHPX, open **PowerShell as Administrator** and run:

```powershell
DISM.exe /Online /Enable-Feature /FeatureName:HypervisorPlatform /All
```

**Reboot Windows after enabling the feature.** You can then return to a normal PowerShell window for the remaining VM steps. The [QEMU WHPX documentation](https://www.qemu.org/docs/master/system/whpx.html) is available for reference.

### You now have a choice to make: use a raw disk image, or convert it to qcow2.

For a **raw disk image (EASIEST)**, follow **[Step 3a](#step-3a-vm-with-raw-image)**. To convert the image to **qcow2**, follow **[Step 3b](#step-3b-vm-with-qcow2-image)**. **Complete only one of these two sections.**

Unlike the Linux setup, this Windows build does **not** support the POSIX input FIFOs. There are no `is1-in-v` or `is1-in-a` files to create.

## Step 3a: VM with raw image

Great. Now we're going to create a **PowerShell script** that you can use to launch the IntelliSTAR 1 VM.

Save the following as **`C:\IS1\Run-VirTSC.ps1`**. Change **`$DiskImage`** if you used a different image path:

```powershell
$BuildRoot = 'C:\vtsc-build'
$DiskImage = 'C:\IS1\weatherscan.img'
$DiskFormat = 'raw' # use 'qcow2' or 'vmdk' when appropriate
$VmDirectory = Split-Path -Parent $DiskImage

$env:PATH = "$BuildRoot\osmesa\bin;C:\msys64\mingw64\bin;$BuildRoot\qemu-install;$env:PATH"

$Qemu = "$BuildRoot\qemu-install\qemu-system-x86_64.exe"
$QemuArgs = @(
    '-L', "$BuildRoot\qemu-install\share",
    '-name', 'IntelliSTAR 1',
    '-machine', 'pc,acpi=off',
    '-global', 'i440FX.agp=on',
    '-global', 'i440FX.agp-aperture-size=128M',
    '-global', 'piix3-ide.force-bus-master=on',
    '-accel', 'whpx',
    '-accel', 'tcg',
    '-cpu', 'pentium3',
    '-m', '512',
    '-smp', '1',
    '-drive', "file=$DiskImage,format=$DiskFormat,if=ide,cache=writeback",
    '-boot', 'c',
    '-vga', 'cirrus',
    '-netdev', 'user,id=net0,net=10.100.102.0/24,host=10.100.102.1',
    '-device', 'i82557b,netdev=net0',
    '-netdev', 'user,id=net1,net=10.0.2.0/24,host=10.0.2.2,hostfwd=tcp:127.0.0.1:2222-10.0.2.15:22',
    '-device', 'e1000-82545em,netdev=net1',
    '-rtc', 'base=utc,clock=host',
    '-serial', "file:$VmDirectory\serial.log",
    '-qmp', 'tcp:127.0.0.1:4444,server=on,wait=off',
    '-audiodev', 'sdl,id=audio0',
    '-device', 'thunderstorm,id=tsc0,present=on,version=0x011a0012,input=bars,stamp=host-ns,tstamp=host-s,timecode=utc,audio=silence,program-display=on,program-audio=on,audiodev=audio0',
    '-device', 'is1gl,id=is1gl0,mmio=0xfed10000,iobase=0x520',
    '-display', 'sdl,gl=off'
)

Push-Location $VmDirectory
try {
    & $Qemu @QemuArgs
} finally {
    Pop-Location
}
```

The two **`-accel`** entries tell QEMU to try WHPX and continue with TCG if WHPX is unavailable. QMP listens on **`127.0.0.1:4444`**, and SSH is forwarded through **`127.0.0.1:2222`**.

You should now have a **`Run-VirTSC.ps1`** script in your VM folder. Proceed to **[Step 4](#step-4-intellistar-1-setup)**; you do **not** need Step 3b.

## Step 3b: VM with qcow2 image

**Only follow this section if you chose qcow2.** If you already completed Step 3a, continue to **[Step 4](#step-4-intellistar-1-setup)** instead.

Make sure the VM is **stopped** before converting its disk. In **PowerShell**, convert the raw image using the **`qemu-img.exe`** binary from your Windows build:

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;C:\vtsc-build\qemu-install;' + $env:PATH

qemu-img.exe convert -p -f raw -O qcow2 -c `
  C:\IS1\weatherscan.img `
  C:\IS1\weatherscan.qcow2
```

Once conversion is complete, keep the original raw image **read-only as a recovery copy**.

Now save the following startup script as **`C:\IS1\Run-VirTSC.ps1`**. It is the same Windows launcher, but **`$DiskImage`** points to the converted file and **`$DiskFormat`** is set to **`qcow2`**:

```powershell
$BuildRoot = 'C:\vtsc-build'
$DiskImage = 'C:\IS1\weatherscan.qcow2'
$DiskFormat = 'qcow2'
$VmDirectory = Split-Path -Parent $DiskImage

$env:PATH = "$BuildRoot\osmesa\bin;C:\msys64\mingw64\bin;$BuildRoot\qemu-install;$env:PATH"

$Qemu = "$BuildRoot\qemu-install\qemu-system-x86_64.exe"
$QemuArgs = @(
    '-L', "$BuildRoot\qemu-install\share",
    '-name', 'IntelliSTAR 1',
    '-machine', 'pc,acpi=off',
    '-global', 'i440FX.agp=on',
    '-global', 'i440FX.agp-aperture-size=128M',
    '-global', 'piix3-ide.force-bus-master=on',
    '-accel', 'whpx',
    '-accel', 'tcg',
    '-cpu', 'pentium3',
    '-m', '512',
    '-smp', '1',
    '-drive', "file=$DiskImage,format=$DiskFormat,if=ide,cache=writeback",
    '-boot', 'c',
    '-vga', 'cirrus',
    '-netdev', 'user,id=net0,net=10.100.102.0/24,host=10.100.102.1',
    '-device', 'i82557b,netdev=net0',
    '-netdev', 'user,id=net1,net=10.0.2.0/24,host=10.0.2.2,hostfwd=tcp:127.0.0.1:2222-10.0.2.15:22',
    '-device', 'e1000-82545em,netdev=net1',
    '-rtc', 'base=utc,clock=host',
    '-serial', "file:$VmDirectory\serial.log",
    '-qmp', 'tcp:127.0.0.1:4444,server=on,wait=off',
    '-audiodev', 'sdl,id=audio0',
    '-device', 'thunderstorm,id=tsc0,present=on,version=0x011a0012,input=bars,stamp=host-ns,tstamp=host-s,timecode=utc,audio=silence,program-display=on,program-audio=on,audiodev=audio0',
    '-device', 'is1gl,id=is1gl0,mmio=0xfed10000,iobase=0x520',
    '-display', 'sdl,gl=off'
)

Push-Location $VmDirectory
try {
    & $Qemu @QemuArgs
} finally {
    Pop-Location
}
```

Be sure to change **`$DiskImage`** if your converted image is elsewhere. For an existing VMDK image, the Windows launcher also allows the matching image path and **`$DiskFormat = 'vmdk'`**.

You should now have a **`Run-VirTSC.ps1`** script in your VM folder. Let's start the IS1.

# Step 4: IntelliSTAR 1 Setup

Time to start the IS1! Be ready, because we need to interrupt the boot sequence to make some modifications.

Run your startup script from **PowerShell**:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File C:\IS1\Run-VirTSC.ps1
```

### The following instructions should be performed on the VM, not your Windows host.

As soon as you see the boot countdown, interrupt it with any key **other than ENTER**. You should see an **`ok`** prompt. At that prompt, type:

```text
boot -s
```

Press **ENTER**. When asked which shell to use, press **ENTER** again to accept **`/bin/sh`**.

Excellent! Once you have a shell prompt, check and mount the filesystem:

```sh
fsck -p
mount -u -o rw /
mount -a
```

Modify the firewall to allow inbound and outbound traffic:

```sh
/sbin/ipfw add 1 allow ip from any to any
```

Bring the **`em0`** interface online:

```sh
ifconfig em0 inet 10.0.2.15 netmask 255.255.255.0
```

Reset the **root** password. Replace **`password`** below with the password you wish to use:

```sh
echo password | pw usermod root -h 0
```

We also need to save the IP address for **`em0`** in **`/etc/rc.conf`**. Open it with **`ee`**:

```sh
ee /etc/rc.conf
```

Look for **`ifconfig_em0`**. Replace its existing definition, or add the following line if it is missing:

```sh
ifconfig_em0="inet 10.0.2.15 netmask 255.255.255.0"
```

Press **ESC** to open the editor menu, then **A** to exit and **A** again to save.

Set the device permissions needed by the X server:

```sh
chmod 666 /dev/agpgart /dev/mem
```

Next, remove the restriction that prevents root from logging in remotely:

```sh
ee /etc/login.access
```

**Delete or comment out the final line**, which reads:

```text
-:root:ALL EXCEPT LOCAL
```

Press **ESC**, then **A** twice to leave and save. **Do not skip this change**, or you will not be able to log in remotely as root.

Create the folder for the files we are about to copy:

```sh
mkdir -p /usr/local/src
```

Now continue the boot process so the services, including SSH, can start:

```sh
exit
```

Once the VM reaches its login prompt, you're ready to copy the guest components.

### The following instructions should be performed in PowerShell on your Windows host, NOT the VM.

Make sure **`ssh.exe`** and **`scp.exe`** are available. If they are missing, install the **Windows OpenSSH Client** optional feature before continuing.

We'll copy the guest libraries, generated headers, and XF86 configuration from your **virtsc** folder. Change **`$Virtsc`** if the source folder is elsewhere.

First, prepare the file list and connection options:

```powershell
$Virtsc = 'C:\Users\i1\virtsc'
$GuestFiles = @(
    "$Virtsc\src\agp\libagpnv.c",
    "$Virtsc\src\glfix\libglfix.c",
    "$Virtsc\src\is1gl\is1gl.c",
    "$Virtsc\src\is1gl\is1gl_ring.h",
    "$Virtsc\src\is1gl\is1gl_ops.h",
    "$Virtsc\src\is1gl\is1gl_gen_guest.h",
    "$Virtsc\resources\XF86Config-4.qemu-cirrus"
)

$SshOptions = @(
    '-P', '2222',
    '-o', 'KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1',
    '-o', 'HostKeyAlgorithms=+ssh-rsa,ssh-dss',
    '-o', 'PubkeyAcceptedAlgorithms=+ssh-rsa',
    '-o', 'Ciphers=+aes128-cbc,3des-cbc',
    '-o', 'MACs=+hmac-sha1',
    '-o', 'StrictHostKeyChecking=no',
    '-o', 'UserKnownHostsFile=NUL'
)
```

**Optional:** If you are using an SSH key, add these entries **before running the SCP command**:

```powershell
$SshOptions += @('-i', "$HOME\.ssh\is1_rsa", '-o', 'IdentitiesOnly=yes')
```

Now copy the seven files:

```powershell
& scp.exe @SshOptions @GuestFiles 'root@127.0.0.1:/usr/local/src/'
```

Authenticate with the VM's **root** password, or with your configured key. A successful copy places the required files in **`/usr/local/src/`** on the guest.

Next, connect to the VM over SSH. You can run this directly in PowerShell or save it as **`C:\IS1\SSH-VirTSC.ps1`**:

```powershell
ssh.exe -p 2222 `
  -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1 `
  -o HostKeyAlgorithms=+ssh-rsa,ssh-dss `
  -o PubkeyAcceptedAlgorithms=+ssh-rsa `
  -o Ciphers=+aes128-cbc,3des-cbc `
  -o MACs=+hmac-sha1 `
  -o StrictHostKeyChecking=no `
  -o UserKnownHostsFile=NUL `
  root@127.0.0.1
```

If you saved the script, run it with:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File C:\IS1\SSH-VirTSC.ps1
```

Once connected, you can use your SSH terminal for the remaining guest commands.

### The following instructions should be performed on the VM, through SSH or the QEMU VGA console, not your host.

We need to build some libraries on the IS1 itself. The commands below also normalize the copied C files and headers to **Unix LF line endings**, preventing problems with files that passed through a Windows checkout.

If the earlier SCP copy succeeded, run:

```sh
cd /usr/local/src

# Safe one-time normalization for files that passed through a Windows checkout.
for f in *.c *.h; do
    tr -d '\015' < "$f" > "$f.lf" && mv "$f.lf" "$f"
done

gcc -O2 -fPIC -shared \
    -o /usr/local/lib/libagpnv.so libagpnv.c

gcc -O2 -fPIC -shared -I/usr/X11R6/include \
    -o /usr/local/lib/libglfix.so libglfix.c \
    -L/usr/X11R6/lib -lGL

gcc -O2 -fPIC -shared -I. -I/usr/X11R6/include \
    -o /usr/local/lib/libis1gl.so is1gl.c \
    -L/usr/X11R6/lib -lX11

if [ ! -e /usr/X11R6/lib/libGL.so.1.mesa ]; then
    mv /usr/X11R6/lib/libGL.so.1 \
       /usr/X11R6/lib/libGL.so.1.mesa
fi

cp /usr/local/lib/libis1gl.so /usr/X11R6/lib/libGL.so.1
ldconfig -m /usr/X11R6/lib
```

The **`tr`** loop prevents the old guest GCC error **`is1gl_ops.h:62: syntax error before string constant`**, caused by CRLF line endings breaking a backslash-continued macro. Keep this step even with the adjusted generator so older Windows checkouts are handled too.

***Finally,*** replace the XF86 configuration with the one we copied:

```sh
cp /usr/local/src/XF86Config-4.qemu-cirrus /etc/X11/XF86Config-4
```

**Reboot the VM:**

```sh
reboot
```

# Step 5: Checking installation

Once the VM returns and starts the X server, the main render window may be solid black. Don't freak out! **This is expected.**

To check that VirTSC is working, press **`Ctrl+Alt+2`** in the QEMU SDL window. This switches to the **`tsc0`** console, where you should see the raw Thunderstorm program output.

Press **`Ctrl+Alt+1`** to return to the IntelliSTAR VGA display.

# Step 6: Picture-perfect

This is the final stretch, and there is nothing else to build or launch. The startup script already enables **`program-display=on`** and **`program-audio=on`**. Thunderstorm sends program video to QEMU's second console and program audio directly to the **SDL audio backend**.

Keep these SDL shortcuts handy:

- **`Ctrl+Alt+1`**: switch to the IntelliSTAR VGA display.
- **`Ctrl+Alt+2`**: switch to the Thunderstorm `tsc0` program output.
- **`Ctrl+Alt+G`**: release or capture the keyboard and mouse.
- **`Ctrl+Alt+F`**: toggle full screen.

### Run a flavor, and enjoy.
