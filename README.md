Current tip of the development: [jg/wdk2023-gunyah-6.7-rc8](https://github.com/jglathe/linux_ms_dev_kit/tree/jg/wdk2023-gunyah-6.7-rc8), now based on [steev/linux](https://github.com/steev/linux/), cross checked against [jhovold/linux](https://github.com/jhovold/linux.git) and [torvalds/linux](https://github.com/torvalds/linux.git)

# **Recent Changes**
* [Gunyah Hypervior](https://github.com/quic/gunyah-hypervisor) support is now integrated into the kernel, so that it can be made aware of it running (it should, in EL2) - and maybe start a VM eventually. Still trying to figure out the documentation.
* Power Management issues are resolved as it seems, having a clean and hardware-bound Windows installation on the local SSD appears to be required. I observed that you can repair unruly fan behaviour by booting into Windows, and afterwards rebooting to Linux. Still investigating to find the actual binding. Fan speed readout would be nice, too :grin:
* DP suspend/resume behaviour has improved, even suspend/resume on my very odd Iiyama ProLite X83288UHSU works now - most of the time without changing VTs.
* Display via USB-C works, actually. But only on USB-C #0 (the rearmost socket). With a display on USB-C #1 it won't boot (some BIOS stuff) or get a signal later. The mode used is USB-C alt mode (DP over USB-C) which has only one lane left for USB bus operation, limiting speeds to USB-2.
* Docker and LXD both working nicely.
* Bluetooth now has a default MAC address set up via the dtb - quite an improvement, it can now work out of the box. To get a unique MAC (the one stored in the machine is unknown) you can use [bootmac](https://gitlab.com/jglathe/bootmac/-/commits/jg/wdk2023) started from crontab. This should be in `/var/spool/cron/crontabs/root`, my command line is `@reboot sleep 7 && /usr/bin/bootmac -b -p AD5A`.

# **Bootable Image**
A bootable image can be downloaded [here](https://drive.google.com/drive/folders/1sc_CpqOMTJNljfvRyLG-xdwB0yduje_O?usp=sharing). Some more details are in [this discussion](https://github.com/jglathe/linux_ms_dev_kit/discussions/1#discussioncomment-6907710). Now that the WDK is bootable from an USB stick (or SSD) I will take the previous tutorial on booting up the WDK with Linux offline. It will be replaced with a tutorial on how to install on SSD soon. [There is a short description on how to do this with the image.](https://github.com/linux-surface/surface-pro-x/issues/43#issuecomment-1705395207) I would recommend to read the discussions, though, before embarking on the install on the local SSD. Especially [here](https://github.com/jglathe/linux_ms_dev_kit/discussions/1#discussioncomment-7038835) regarding USB-C support.

# **How to build and install your own kernel when you're up and running on the WDK2023**
To build the kernel, you need to install the necessary tools:

`sudo apt install git bc bison flex libssl-dev make libc6-dev libncurses5-dev build-essential`

Next, clone the git repo, check out the desired branch. In the example I use `jg/wdk2023-6.5.4`:

you@yourwdk:~/src$ `git clone --branch jg/wdk2023-6.5.4 https://github.com/jglathe/linux_ms_dev_kit.git`

Before compiling you need to configure for the wdk target. steev/linux maintains its own laptop_defconfig, which is IMO better fitting. I'm out of my depth with this config thing.

you@yourwdk:~/src/linux_ms_dev_kit.git$ `make -j8 laptop_config`

This generates a .config file with the configuration for the kernel to build. Afterwards, you can change the configuration with `make menuconfig`. The .config always appends a '+' for a locally compiled kernel, so the version would be `6.5.4+`.

Time for doing changes / hacking.

Afterwards, compile the kernel: `time make -j8 Image.gz dtbs modules`
'time' just measures how long it takes, I find it useful. A full run should take ~25mins. This command builds all we need to do an install.

The actual installation is a few steps:

- If it's a new kernel version, we need to create the dtb target path: `sudo mkdir -p /boot/dtbs/6.5.4+/`.
- copy the dtb to the target path: you@yourwdk:~/src/linux_ms_dev_kit.git$ `sudo cp arch/arm64/boot/dts/qcom/sc8280xp-microsoft-dev-kit-2023.dtb /boot/dtb-6.5.4+/`
- if it doesn't exist yet, we need to set a symlink named `dtb-<version>` to the dtb
```
cd /boot
sudo ln -s dtbs/6.5.4+/sc8280xp-microsoft-dev-kit-2023.dtb dtb-6.5.4+
```
- install the kernel modules: you@yourwdk:~/src/linux_ms_dev_kit.git$ `sudo make -j8 modules_install`
- finally, install the kernel. This will invoke install scripts which also updates grub: you@yourwdk:~/src/linux_ms_dev_kit.git$ `sudo make install`

On the next reboot, you can boot the new kernel.

If you're stuck and need to clean up, this command is helpful: you@yourwdk:~/src/linux_ms_dev_kit.git$ `make -j8 mrproper`

## **Acknowledgements**
The original tutorial from @chenguokai can be found [here](https://github.com/chenguokai/chenguokai/blob/master/tutorial-dev-kit-linux.md)
