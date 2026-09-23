# Running VirTSC
This guide sets up the scripts that start and connect to your IntelliSTAR 1 VM. You should have finished building VirTSC for your platform first ([Debian](build/DEBAIN.md) or [Windows](build/WINDOWS.md)).

This is rather involved, so I'll try to hold your hand as much as possible through this. 

Each step explains what to do, then gives the commands for each platform. **Expand the section for your platform.**

# Step 1: Creating the VM folder
First things first - have an unmodified, raw IntelliSTAR 1 disk image handy. You need a folder to hold the image, the scripts, and the VM's logs. In this guide we use `~/i1` on Linux and `C:\IS1` on Windows, with the image at `weatherscan.img` inside it.

<details>
<summary><b>Debian</b></summary>

Create the folder, and place your image at `~/i1/weatherscan.img`:
```bash
mkdir ~/i1
```
Create FIFO files for input video and audio:
```bash
mkfifo ~/i1/is1-in-v ~/i1/is1-in-a
```
</details>

<details>
<summary><b>Windows</b></summary>

The build guide already created `C:\IS1`. Place your image at `C:\IS1\weatherscan.img`.

The build is finished, so **everything from here runs from a normal, non-elevated PowerShell window** unless a step says otherwise. The Windows build does **not** support the input FIFOs used on Linux, so there are no `is1-in-v` or `is1-in-a` files to create.

**Optional: enabling WHPX acceleration.** The launcher tries **WHPX** first and falls back to **TCG** if WHPX is unavailable. To enable WHPX, open **PowerShell as Administrator** and run:
```powershell
DISM.exe /Online /Enable-Feature /FeatureName:HypervisorPlatform /All
```
**Reboot Windows after enabling the feature.** The [QEMU WHPX documentation](https://www.qemu.org/docs/master/system/whpx.html) is available for reference.
</details>

# Step 2: Choosing a disk format
### You now have a choice to make. You can use the raw disk image for the VM, or you can convert it to a qcow2 file.

- **Raw image (EASIEST):** the VM writes straight to your image.
- **qcow2 snapshot file:** the VM writes to a compressed copy, and the original raw image is kept read-only as a recovery copy.

Converting may take a while. If it fails, there's a good chance the raw disk image has bad sectors on it; use the raw image instead. *If you know what you're doing,* you can also boot an existing **vmdk** image by setting the format to `vmdk` in Step 3.

<details>
<summary><b>Debian</b></summary>

**Raw:** make sure the image is writable:
```bash
chmod 666 ~/i1/weatherscan.img
```
**qcow2:** make the raw image read-only, then convert it using the **qemu-img** binary from your build folder:
```bash
chmod 444 ~/i1/weatherscan.img
qemu-img convert -p -f raw -O qcow2 -c ~/i1/weatherscan.img ~/i1/weatherscan.qcow2
```
</details>

<details>
<summary><b>Windows</b></summary>

**Raw:** nothing to do.

**qcow2:** make sure the VM is **stopped**, then convert the image using **`qemu-img.exe`** from your build and make the original read-only:
```powershell
$env:PATH = 'C:\msys64\mingw64\bin;C:\vtsc-build\qemu-install;' + $env:PATH

qemu-img.exe convert -p -f raw -O qcow2 -c `
  C:\IS1\weatherscan.img `
  C:\IS1\weatherscan.qcow2

Set-ItemProperty C:\IS1\weatherscan.img -Name IsReadOnly -Value $true
```
</details>

# Step 3: Creating the startup script
Great. Now we're going to create a shell script that you can easily launch your IntelliSTAR 1 VM with. **Before saving it, check the two settings at the top:** You need to change `qemu-system-i386` to its complete path, if you did not add the **qemu-is1/build** folder to your system's PATH.

- The **image path**. If you converted to qcow2, point it at `weatherscan.qcow2`.
- The **format**: `raw`, `qcow2`, or `vmdk`.

If you change your mind about the disk format later, just edit those two settings.

<details>
<summary><b>Debian</b></summary>

Change `qemu-system-i386` to its complete path if you did not add the **qemu-is1/build** folder to your system's PATH.
```bash
cat > ~/i1/run.sh <<'EOF'
#!/bin/sh
IMG="$HOME/i1/weatherscan.img"
FORMAT=raw

# The logs, QMP socket and FIFOs live next to this script.
cd "$(dirname "$0")"
exec qemu-system-i386 \
  -name IS1 \
  -machine pc,acpi=off \
  -global i440FX.agp=on -global i440FX.agp-aperture-size=128M \
  -global piix3-ide.force-bus-master=on \
  -accel kvm -cpu pentium3 -m 512 -smp 1 \
  -drive file=$IMG,format=raw,if=ide,cache=writeback -boot c \
  -vga cirrus \
  -netdev user,id=net0,net=10.100.102.0/24,host=10.100.102.1 \
  -device i82557b,netdev=net0 \
  -netdev user,id=net1,net=10.0.2.0/24,host=10.0.2.2,hostfwd=tcp:127.0.0.1:2222-10.0.2.15:22 \
  -device e1000-82545em,netdev=net1 \
  -rtc base=utc,clock=host \
  -serial file:./serial.log \
  -qmp unix:./qmp.sock,server,nowait \
  -audiodev sdl,id=audio0 \
  -device thunderstorm,id=tsc0,present=on,version=0x011a0012,\
input=bars,input-pipe=./is1-in-v,input-audio=./is1-in-a,\
stamp=host-ns,tstamp=host-s,timecode=utc,audio=silence,\
audiodev=audio0 \
  -device is1gl,id=is1gl0,mmio=0xfed10000,iobase=0x520 \
  -display gtk,zoom-to-fit=on,gl=off
EOF
```

You should now have a **run.sh** script in your folder. Let's make it executable.
```bash
chmod +x ~/i1/run.sh
```

</details>

<details>
<summary><b>Windows</b></summary>

Save the following as **`C:\IS1\Run-VirTSC.ps1`**:
```powershell
$BuildRoot = 'C:\vtsc-build'
$DiskImage = 'C:\IS1\weatherscan.img'
$DiskFormat = 'raw'
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
The two **`-accel`** entries tell QEMU to try WHPX and continue with TCG if WHPX is unavailable. QMP listens on **`127.0.0.1:4444`**.
</details>

# Step 4: Creating the SSH scripts
The IS1's SSH server is old, so connecting to it needs a handful of legacy options. We'll make two small scripts so you never have to type them: one to log in, and one to copy the guest files from **virtsc** onto the VM. SSH is forwarded to the VM through **`127.0.0.1:2222`**.

Before saving the copy script, change the **virtsc** path in it to the folder where you cloned **virtsc**.

<details>
<summary><b>Debian</b></summary>

The SSH script:
```bash
cat > ~/i1/ssh.sh <<'EOF'
#!/bin/sh
exec ssh -p 2222 \
    -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1 \
    -o HostKeyAlgorithms=+ssh-rsa,ssh-dss \
    -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o Ciphers=+aes128-cbc,3des-cbc \
    -o MACs=+hmac-sha1 \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    root@127.0.0.1
EOF
chmod +x ~/i1/ssh.sh
```
The copy script:
```bash
cat > ~/i1/copy-guest.sh <<'EOF'
#!/bin/sh
VIRTSC="$HOME/virtsc"

cd "$VIRTSC" || exit 1
exec scp -P 2222 \
    -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1 \
    -o HostKeyAlgorithms=+ssh-rsa,ssh-dss \
    -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o Ciphers=+aes128-cbc,3des-cbc \
    -o MACs=+hmac-sha1 \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    src/agp/libagpnv.c \
    src/glfix/libglfix.c \
    src/is1gl/is1gl.c \
    src/is1gl/is1gl_ring.h \
    src/is1gl/is1gl_ops.h \
    src/is1gl/is1gl_gen_guest.h \
    resources/XF86Config-4.qemu-cirrus \
    root@127.0.0.1:/usr/local/src/
EOF
chmod +x ~/i1/copy-guest.sh
```
</details>

<details>
<summary><b>Windows</b></summary>

Make sure **`ssh.exe`** and **`scp.exe`** are available. If they are missing, install the **Windows OpenSSH Client** optional feature before continuing.

Save the following as **`C:\IS1\SSH-VirTSC.ps1`**:
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
Save the following as **`C:\IS1\Copy-Guest.ps1`**:
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

& scp.exe @SshOptions @GuestFiles 'root@127.0.0.1:/usr/local/src/'
```
</details>

You won't be able to use these until the guest is set up for SSH, which is the next guide.

# Step 5: Starting the VM
Time to start the IS1! Be ready though, because on the first boot we need to interrupt the boot sequence to make some modifications.

<details>
<summary><b>Debian</b></summary>

```bash
~/i1/run.sh
```
</details>

<details>
<summary><b>Windows</b></summary>

Every script in `C:\IS1` is run the same way, with `powershell.exe -NoProfile -ExecutionPolicy Bypass -File`:
```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File C:\IS1\Run-VirTSC.ps1
```
</details>

Then head straight to the guest setup.

### Next: [Setting up the guest](GUEST.md)
