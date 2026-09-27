# Building VirTSC on Fedora (and downstream distros like RHEL, Alma and Rocky)
This guide provides a walkthrough on building VirTSC on Fedora (and downstream distros like RHEL, Alma and Rocky). If you are not using these distros, do not follow this guide.

An important note is that these instructions were written and tested on Rocky Linux 10, and some dependancies may have changed with the upstream dist ributions.

# Step 1: Prerequisites
If you are running RHEL/Rocky/Alma, we have some repositories to enable:
Fedora users can ignore this section.
```
Rocky/Alma:
sudo dnf config-manager --set-enabled crb
sudo dnf install -y https://dl.fedoraproject.org/pub/epel/epel-release-latest-$(rpm -E '%{rhel}').noarch.rpm

RHEL:
sudo subscription-manager repos --enable codeready-builder-for-rhel-$(rpm -E '%{rhel}')-$(arch)-rpms
sudo dnf install -y https://dl.fedoraproject.org/pub/epel/epel-release-latest-$(rpm -E '%{rhel}').noarch.rpm
```

Now, we need to ensure we have all packages required in order to build VirTSC. Install the following:
```
sudo dnf install -y bison bzip2 ca-certificates ccache findutils flex gcc git glibc-devel libfdt-devel libffi-devel glib2-devel pixman-devel glibc-locale-source make meson ninja-build pkgconf-pkg-config mesa-compat-libOSMesa-devel cmake gtk3-devel SDL2-devel libslirp-devel

```
With these installed, clone the **qemu-is1** and **virtsc** repositories:
```bash
git clone https://github.com/VirTSC/qemu-is1
git clone https://github.com/VirTSC/virtsc
```
Great. You should have everything you need at this point, aside from a valid IntelliSTAR 1 image. For ease of setup without deviations, an unmodified, raw image is recommended. If you plan on using an image that was previously used on a different VM platform/rendering engine (i.e VMware + ReRenderD), you most likely need to re-enable the TSC card in the renderD config or enable renderD execution in istard.

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
make -j$(nproc)
```
**Optional (but recommended):** Add the build folder to your bashrc so that the binaries are available in your system PATH. Do this by adding `export PATH="/home/YOUR-USERNAME/FOLDER-WHERE-YOU-CLONED-QEMU-IS1/build:$PATH"` to the bottom of your `~/.bashrc` file. Be sure to replace **YOUR-USERNAME** and **FOLDER-WHERE-YOU-CLONED-QEMU-IS1** accordingly, otherwise this will do nothing. *Only do this if you do not have a conflicting version of qemu installed on your machine.*

### Next: [Running the VM](../RUN.md)
