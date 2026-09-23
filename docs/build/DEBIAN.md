# Building VirTSC on Debian-based Linux
This guide provides a walkthrough on building VirTSC on Debian-based Linux distributions (i.e. Ubuntu, Mint, RaspiOS). If you are not using a Debian-based Linux, do not follow this guide.

# Step 1: Prerequisites
We need to ensure we have all packages required in order to build VirTSC. Install the following:
```
sudo apt-get install -y bison bzip2 ca-certificates ccache findutils flex gcc git libc6-dev libfdt-dev libffi-dev libglib2.0-dev libpixman-1-dev locales make meson ninja-build pkg-config libosmesa6 libosmesa6-dev cmake libgtk-3-dev libsdl2-dev
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

### Next: [Running the VM](../RUN.md)
