# Setting up the IntelliSTAR 1 guest
This guide prepares the IntelliSTAR 1 itself to run VirTSC. It is the same on every host platform. You should have your VM started by following **[Running VirTSC](RUN.md)**.

The few steps that happen on the host use the scripts you made in the run guide:

| | Debian and macOS | Windows |
|---|---|---|
| Copy the guest files | `~/i1/copy-guest.sh` | `C:\IS1\Copy-Guest.ps1` |
| SSH into the VM | `~/i1/ssh.sh` | `C:\IS1\SSH-VirTSC.ps1` |

On a Mac keyboard, the **Ctrl+Alt** shortcuts in this guide are **Control+Option**.

# Step 1: Single-user mode
### The following instructions should be performed on the VM, not your host.

As soon as you are prompted with the boot countdown, interrupt the boot sequence with any key **other than ENTER.** You should see `ok`. If you see `ok`, type `boot -s` and **hit ENTER**. It will ask you for the shell you wish to use, just hit **ENTER** to use /bin/sh.

Excellent! If you have a shell prompt, check and mount the filesystem:
```sh
fsck -p
mount -u -o rw /
mount -a
```
Modify the firewall to allow all inbound/outbound traffic:
```sh
/sbin/ipfw add 1 allow ip from any to any
```
Bring the **em0** interface online:
```sh
ifconfig em0 inet 10.0.2.15 netmask 255.255.255.0
```
Reset the **root** password. Replace **`password`** below with the password you wish to use:
```sh
echo password | pw usermod root -h 0
```
We need to modify **/etc/rc.conf** to contain the IP address for **em0**. I will walk you through this using **ee** instead of **vi**, since **vi** sucks.
```sh
ee /etc/rc.conf
```
Look for `ifconfig_em0`. If you see an existing definition, change it to the line below. If you do NOT see it, add the following line to the bottom of the file:
```sh
ifconfig_em0="inet 10.0.2.15 netmask 255.255.255.0"
```
Press **ESC** to bring up the editor menu. Press **A** to exit. Press **A** again to save changes. You should see that it wrote changes to **/etc/rc.conf**.

Set device permissions for the X server. This needs to run each boot before X starts. There are a billion ways to do this, but this one works fine
```sh
ee /twc/util/startup.sh
```
Add this line after the first
```
chmod 666 /dev/agpgart /dev/mem
```
Press **ESC** and then press **A** twice to save.

We need to delete the barred root login so that we can use it later. ***THIS IS NECESSARY*** otherwise you will not be able to login to root externally:
```sh
ee /etc/login.access
```
**Delete or comment out the last line in the file** (the one that says `-:root:ALL EXCEPT LOCAL`). Press **ESC** and then press **A** twice to save.

Next, we need to enable the RSA SSH key, otherwise you will not be able to connect from a modern SSH client
```sh
ee /etc/ssh/sshd_config
```
Add the following line after the other HostKey comments
```
HostKey /etc/ssh/ssh_host_rsa_key
```
Press **ESC** and then press **A** twice to save.

We need to make a directory in preparation of the upcoming file transfer:
```sh
mkdir -p /usr/local/src
```
Okay! Now we need to get some files on the VM. To do this, all services need to be running (we need SSH). Continue the boot process beyond single-user mode:
```sh
exit
```
Once you get to the login prompt, you're ready to copy the necessary files to the VM.

# Step 2: Copying the guest files
### The following instructions should be performed on your host machine, NOT the VM.

Run your **copy script** (see the table at the top). Authenticate with the **root** password of the VM. If successful, this copies the guest libraries, their headers, and the XF86 config to `/usr/local/src/` on the VM.

Now run your **SSH script** and log in as **root**. If successful, we can now make our lives a little bit easier thanks to a great feature known as ***COPY AND PASTE***.

# Step 3: Building the guest libraries
### The following instructions should be performed on the VM (SSH or qemu VGA), not your host.

We need to build some libraries on the IS1 itself. If the copy from earlier was successful, run the following:
```sh
cd /usr/local/src

# Strip any Windows (CRLF) line endings from the copied sources.
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

# Keep the original Mesa libGL, even if this is run a second time.
if [ ! -e /usr/X11R6/lib/libGL.so.1.mesa ]; then
    mv /usr/X11R6/lib/libGL.so.1 \
       /usr/X11R6/lib/libGL.so.1.mesa
fi

cp /usr/local/lib/libis1gl.so /usr/X11R6/lib/libGL.so.1
ldconfig -m /usr/X11R6/lib
```
The **`tr`** loop prevents the guest GCC error **`is1gl_ops.h:62: syntax error before string constant`**, which happens when CRLF line endings break a backslash-continued macro. `.gitattributes` and the header generator already keep these files LF, but keep this step anyway: it costs nothing and also covers older checkouts.

***Finally,*** replace the XF86Config with the one we copied:
```sh
cp /usr/local/src/XF86Config-4.qemu-cirrus /etc/X11/XF86Config-4
```
**Reboot the VM.**
```sh
reboot
```

# Step 4: Checking installation
Once the VM returns and starts the X server, the **renderd** window should be solid black, with nothing displayed. Don't freak out! This is intentional.

To check that VirTSC is working, press **`Ctrl+Alt+2`** in the QEMU window (on Debian and macOS, you can also open the **View** menu and choose **tsc0**). If everything was installed correctly, you should see the Thunderstorm program output at full frame rate, with its audio playing through your host's speakers. There is nothing else to build or launch.

Keep these shortcuts handy:

- **`Ctrl+Alt+1`**: switch to the IntelliSTAR VGA display.
- **`Ctrl+Alt+2`**: switch to the Thunderstorm `tsc0` program output.
- **`Ctrl+Alt+G`**: release or capture the keyboard and mouse.
- **`Ctrl+Alt+F`**: toggle full screen.

From now on, you only need your platform's startup script to bring the IS1 back up.

### Run a flavor, and enjoy.
