# Common Problems & Troubleshooting 
Followed the guide ***exactly to a tee*** and not seeing a picture-perfect output on tsc0? Check out some of these common issues and their resolutions below:

## Solid black output on tsc0, no color bars, no graphical content, or no window shown on X.
If you completed the [Guest Setup](GUEST.md) as instructed (meaning you got all the files transferred to the VM and ran that long bash command to compile the GL modules) yet still have nothing but a solid black output on the **tsc0** view in the QEMU window, then there's a good chance that the Thunderstorm output was previously disabled for compatibility with another VM host like VMware (this is the common case especially if the image was a pre-built VMDK). Try the steps below:

Check out the **renderd** config file located at **/twc/conf/renderd.py**. Make sure that the **config.activateTStormCard** line is set to a 1, not a 0, as seen below:
```python
...

config.activateTStormCard(1)
config.activateAsiOutput(1)
```
If it's set to 0, then change it to 1. Save the file and **exit the X server** or **reboot the VM**. When it returns, check the **tsc0** output view again in the QEMU window. You should see color bars by default until graphical processes are running.

Another thing you should check is **/twc/conf/istard.py** to make sure renderD is launched, as some replacement graphics engines require renderD to be disabled.

## "Flashing Thunderstorm card to update firmware," reboot looping and never starting the shell
If you have attempted to start the VM and it continues to reboot in an attempt to flash the Thunderstorm card to a different version, **take note of the version it attempts to upgrade it to (can also be a downgrade)**. This is the version we'll need to set the Thunderstorm device to in our QEMU run script. ***When the VM tries to reboot again, interrupt it before it tries to boot kernel.*** When you get the "ok" prompt, it should be safe to power off the VM or close the QEMU window at this point.

Now, open up your VM's **run.sh** script in your favorite text editor. Go to the line which defines the **thunderstorm** device:
```bash
-device thunderstorm,id=tsc0,present=on,version=0x011a0012,
```
Notice the **version** argument. The version string is a hexadecimal byte sequence, which translates to a set of decimals. It breaks down like this:
```
In the example of '0x011a0012'

0x01 = 1
Major release (always 1 - 0x01)

1a = 26
Minor release

00 = 0
This portion always remains 00, seemingly unused

12 = 18
Patch release (most likely to be different)
```
So **0x011a0012** equals **1.26.18**. Since the segments are hexadecimal translated to decimal, it's easy to adjust the version. For example, if you need to change the patch release to 17, **1.26.17**, your hexadecimal string will be **0x011a0011**.

Change the version string in your startup script to the one your recorded earlier in the VM's console (the one that it tries to "upgrade" the card to) and it should no longer attempt to upgrade the firmware.

## The renderd window on the desktop is solid black and I see nothing!
So, if you actually know how to ***READ*** you would have seen that this is expected and intentional. If you'd like the technical explanation, the **renderd** window on the X11 desktop shows nothing when VirTSC is running. This is because on stock IntelliStar systems, the GPU draws the graphics in the same buffer space that the display uses... but with VirTSC, we utilize GL on the host machine to draw frames, meaning that we'd have to copy those frames *back* into the VM's memory just to display it on the **renderd** window, which consumes valuable CPU cycles, costing us valuable performance. That is why the window does nothing, and you are supposed to view the **tsc0** output on the host's **QEMU** window.

TL;DR - Switch to the '**tsc0**' view on your running QEMU window, either using the GUI or the hotkey.

## Fault code "Supervisor read, page not present" when setting up TAP (bridged) networking

The Intel i82557b emulated by QEMU can be hit or miss when it comes to TAP (bridged) networking, especially in regards to emulating the IntelliStar. If you want to bridge the VM to your host and you keep getting this error, you can change net0's device in your run.sh script to `e1000-82545em`.
```
  -device e1000-82545em,netdev=net0
```
Do note that you will also need to alter the `/etc/rc.conf` on the guest to match it. You will need to follow the same steps as if you were setting up QEMU for the first time [(booting into single-user mode)](GUEST.md#step-1-single-user-mode), except you'll only be changing one file:
```
ee /etc/rc.conf
```
Look for `ifconfig_em0`, then change em0 to **em1**.
```
ifconfig_em1="inet 10.0.2.15 netmask 255.255.255.0"
```
Then, add a new line formatted similarly to the previous one, except this will be for **em0**. Make sure `<IP_ADDR>` is the address of your network bridge. Alternatively, you can also change the bridge's static IP address to match whatever you put in here.
```
ifconfig_em0="inet <IP_ADDR> netmask 255.255.255.0"
```
Press **ESC** to bring up the editor menu. Press **A** to exit, then press **A** one more time to save changes. You may now continue booting from here.

# Unresolved or new issues?
Was your problem not described or solved in this document? Let us know by **creating an issue** on this repository, pending that your problem meets the following criteria:
- You have followed every instruction in the setup and configuration guides for your system. (It never hurts to sanity check and verify that you didn't miss something!!!)
- Your problem is not related to specific operational tasks, products, graphics, or files on the IntelliStar system. (In other words, make sure this is an issue with VirTSC and not something like "music won't play on Weatherscan")
- You have made your best effort to troubleshoot and verify that your configuration is correct and mistake-free.

We can help, but we've got lives of our own so please have some patience <3
