# Setting up VirTSC on Debian-based Linux
This guide provides a walkthrough on installing and configuring VirTSC on Debian-based Linux distributions (i.e. Ubuntu, Mint, RaspiOS). If you are not using a Debian-based Linux, do not follow this guide.

# Step 1: Prerequisites
We need to ensure we have all packages required in order to build VirTSC. Install the following:
```
sudo apt-get install -y bison bzip2 ca-certificates ccache findutils flex gcc git libc6-dev libfdt-dev libffi-dev libglib2.0-dev libpixman-1-dev locales make meson ninja-build pkg-config libosmesa6 libosmesa6-dev cmake libgtk-3-dev
```
With these installed, clone the **qemu-is1** and **virtsc** repositories:
```bash
git clone https://github.com/VirTSC/qemu-is1
git clone https://github.com/VirTSC/virtsc
```
Great. You should have everything you need at this point, aside from a valid IntelliSTAR 1 image. For ease of setup without deviations, an unmodified, raw image is recommended.
# Step 2: Configuring & Building
Now it's time to configure and build **qemu-is1**. Please be mindful of the fact that you are literally building qemu binaries - in other words, pay attention to which qemu instance you are using if you already have an existing build of qemu installed on your PATH.

Prepare **qemu-is1** by CD'ing into the directory you cloned the repository to, and create a **build** folder.
```bash
cd qemu-is1
mkdir -p build
cd build
```
Configure the build, relative to the build folder:
```bash
../configure --target-list=i386-softmmu --enable-osmesa --disable-opengl --enable-gtk --enable-sdl --enable-slirp
```
If it fails to configure, **ensure you installed all the packages as defined in step 1.** If the configuration succeeded without error, it's time to build. Run **make** in the **build** directory.
```bash
make
```
**Optional (but recommended):** Add the build folder to your bashrc so that the binaries are available in your system PATH. Do this by adding `export PATH="/home/YOUR-USERNAME/FOLDER-WHERE-YOU-CLONED-QEMU-IS1/build:$PATH"` to the bottom of your `~/.bashrc` file. Be sure to replace **YOUR-USERNAME** and **FOLDER-WHERE-YOU-CLONED-QEMU-IS1** accordingly, otherwise this will do nothing. *Only do this if you do not have a conflicting version of qemu installed on your machine.*

# Step 3: Creating the VM
This is rather involved, so I'll try to hold your hand as much as possible through this. 

First things first - have an unmodified, raw IntelliSTAR 1 disk image handy. Create a folder for maintaining your environment, anywhere you desire. In this example, I'll store it in my home folder:
```bash
mkdir ~/i1
```
Place your image in the folder you created. I'll use `~/i1/weatherscan.img` as the location of the disk image for this example.

### You now have a choice to make. You can use a raw disk image for the VM, or you can convert the disk image to a qcow2 snapshot file. 
If you want to use a raw disk image (EASIEST), proceed with **Step 3a.** If you would like to convert to a qcow2 file (snapshot-based instead of raw image), proceed with **Step 3b.** 

## Step 3a: VM with raw image
We need to modify the file permissions of the image:
```bash
chmod 666 ~/i1/weatherscan.img
```
Create FIFO files for input video and audio:
```bash
mkfifo ~/i1/is1-in-v ~/is1-in-a
```
Great. Now we're going to create a shell script that you can easily launch your IntelliSTAR 1 VM with. Be sure to replace `~/i1/weatherscan.img` on **-drive file=** with the path to your image if you used a different path. You need to change `qemu-system-i386` to its complete path, if you did not add the **qemu-is1/build** folder to your system's PATH.
```bash
echo "qemu-system-i386 \
  -name IS1 \
  -machine pc,acpi=off \
  -global i440FX.agp=on -global i440FX.agp-aperture-size=128M \
  -global piix3-ide.force-bus-master=on \
  -accel kvm -cpu pentium3 -m 512 -smp 1 \
  -drive file=~/i1/weatherscan.img,format=raw,if=ide,cache=writeback -boot c \
  -vga cirrus \
  -netdev user,id=net0,net=10.100.102.0/24,host=10.100.102.1 \
  -device i82557b,netdev=net0 \
  -netdev user,id=net1,net=10.0.2.0/24,host=10.0.2.2,hostfwd=tcp:127.0.0.1:2222-10.0.2.15:22 \
  -device e1000-82545em,netdev=net1 \
  -rtc base=utc,clock=host \
  -serial file:./serial.log \
  -qmp unix:./qmp.sock,server,nowait \
  -device thunderstorm,id=tsc0,present=on,version=0x011a0012,\
input=bars,output=./is1-output,\
input-pipe=./is1-in-v,input-audio=./is1-in-a,\
stamp=host-ns,tstamp=host-s,timecode=utc,audio=silence \
  -device is1gl,id=is1gl0,mmio=0xfed10000,iobase=0x520 \
  -display gtk,zoom-to-fit=on,gl=off" > ~\i1\run.sh
  ```
You should now have a **run.sh** script in your folder. Let's make it executable.
```bash
chmod +x ~\i1\run.sh
```
Okay! Proceed to **step 4.**

## Step 3b: VM with snapshot (qcow2) file
***STOP!*** If you just completed **Step 3a,** you did not pay attention. The following instructions do not apply to you.

**TO-DO.** (In Allen Jackson voice:) Uhh...

# Step 4: IntelliSTAR 1 Setup
Time to start the IS1! Be ready though, because we need to interrupt the boot sequence to make some modifications.

Run your startup script:
```bash
~\i1\run.sh
```

### The following instructions should be performed on the VM, not your host.

As soon as you are prompted with the boot countdown, interrupt the boot sequence with any key **other than ENTER.** You should see `ok`. If you see `ok`, type `boot -s` and **hit ENTER**. It will ask your for the shell you wish to use, just hit **ENTER** to use /bin/sh.

Excellent! If you have a shell prompt, proceed with mounting the drive:
```bash
fsck -p
mount -u -o rw /
mount -a
```
Modify the firewall to allow all inbound/outbound traffic:
```bash
/sbin/ipfw add 1 allow ip from any to any
```
Bring the **em0** interface online:
```bash
ifconfig em0 inet 10.0.2.15 netmask 255.255.255.0
```
Reset your **root** user password, if you don't know what it is (in this example, "password"):
```bash
echo password | pw usermod root -h 0
```
We need to modify **/etc/rc.conf** to contain the IP address for **em0**. I will walk you through this using **ee** instead of **vi**, since **vi** sucks. 
```bash
ee /etc/rc.conf
```
Look for `ifconfig_em0`. If you see an existing definition, change it to the line below. If you do NOT see it, add the following line to the bottom of the file:
```bash
ifconfig_em0="inet 10.0.2.15 netmask 255.255.255.0"
```
Press **ESC** to bring up the editor menu. Press **A** to exit. Press **A** again to save changes. You should see that it wrote changes to **/etc/rc.conf**.

Set device permissions for the X server:
```bash
chmod 666 /dev/agpgart /dev/mem
```

We need to delete the barred root login so that we can use it later. ***THIS IS NECESSARY*** otherwise you will not be able to login to root externally:
```bash
ee /etc/login.access
```
**Delete or comment out the last line in the file** (the one that says `-:root:ALL EXCEPT LOCAL`). Press **ESC** and then press **A** twice to save.

Okay! Now we need to get some files on the VM. To do this, all services need to be running (we need SSH). Type `exit` and hit **ENTER**. This will continue the boot process beyond single-user mode. Once you get to the login prompt, you're ready to copy the necessary files to the VM.

### The following instructions should be performed on your host machine, NOT the VM.

Go ahead and `cd` to the location where you cloned **virtsc**. We'll copy a few tools and the XF86 config via **scp** from this folder:
```bash
scp -P 2222 \
    -i ~/.ssh/is1_rsa \
    -o IdentitiesOnly=yes \
    -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1 \
    -o HostKeyAlgorithms=+ssh-rsa,ssh-dss \
    -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o Ciphers=+aes128-cbc,3des-cbc \
    -o MACs=+hmac-sha1 \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    ./tools/agp/libagpnv.c \
    ./tools/glfix/libglfix.c \
    ./tools/is1gl/is1gl.c \
    ./tools/is1gl/is1gl_ring.h \
    ./tools/is1gl/is1gl_ops.h \
    ./tools/is1gl/is1gl_gen_guest.h \
    ./resources/XF86Config-4.qemu-cirrus \
    root@127.0.0.1:/usr/local/src/
```
Authenticate with the **root** password of the VM. If successful, this should have copied all of the required files to the VM.

Now, we'll make a quick shell script so that you can easily SSH into the VM. We'll place it in the IS1 environment folder we created at the start of all this nonsense:
```bash
echo "ssh -p 2222 \
    -i ~/.ssh/is1_rsa \
    -o IdentitiesOnly=yes \
    -o KexAlgorithms=+diffie-hellman-group1-sha1,diffie-hellman-group-exchange-sha1 \
    -o HostKeyAlgorithms=+ssh-rsa,ssh-dss \
    -o PubkeyAcceptedAlgorithms=+ssh-rsa \
    -o Ciphers=+aes128-cbc,3des-cbc \
    -o MACs=+hmac-sha1 \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    root@127.0.0.1" > ~\i1\ssh.sh
```
Make it executable:
```bash
chmod +x ~/i1/ssh.sh
```

**Run it** and achieve SSH terminal access. If successful, we can now make our lives a little bit easier thanks to a great feature known as ***COPY AND PASTE***.

### The following instructions should be performed on the VM (SSH or qemu VGA), not your host.

We need to build some binaries on the IS1 VM. If the SCP copy from earlier was successful, run the following:
```bash
cd /usr/local/src
gcc -O2 -fPIC -shared -o /usr/local/lib/libagpnv.so libagpnv.c
gcc -O2 -fPIC -shared -I/usr/X11R6/include -o /usr/local/lib/libglfix.so libglfix.c -L/usr/X11R6/lib -lGL
gcc -O2 -fPIC -shared -I. -I/usr/X11R6/include -o /usr/local/lib/libis1gl.so is1gl.c -L/usr/X11R6/lib -lX11
mv /usr/X11R6/lib/libGL.so.1 /usr/X11R6/lib/libGL.so.1.mesa
cp /usr/local/lib/libis1gl.so /usr/X11R6/lib/libGL.so.1
ldconfig -m /usr/X11R6/lib
```

***Finally,*** replace the XF86Config with the one we copied via SCP.

```bash
cp /usr/local/src/XF86Config-4.qemu-cirrus /etc/X11/XF86Config-4
```

**Reboot the VM.**
```
reboot
```

# Step 5: Checking installation
Once the VM returns and starts the X server, the **renderd** window should be solid black, with nothing displayed. Don't freak out! This is intentional.

To check that VirTSC is working, **click on View in the qemu window**, and then **click tsc0**. If everything was installed correctly, you should see the raw TSC video output. **The framerate is slow in the display, and that's normal.** ***This is NOT how you will be viewing the complete output!!!***

That's what **is1view** is for.

# Step 6: Picture-perfect
This is the final stretch to viewing your glorious 30 FPS of video and stereo audio. All we need to do now is **build is1view.**

`cd` to the folder where you cloned **virtsc**, and then `cd` into **is1view**.

Build **is1view** (on your host machine, obviously):
```bash
cc -O2 -o is1view is1view.c $(pkg-config --cflags --libs sdl2)
```

Ready to view your IS1? **Run is1view** targeting the `is1-output` file in your VM's environment folder (remember, in this example, that's `~/i1`)
```bash
./is1view ~/i1/is1-output
```

### Run a flavor, and enjoy.