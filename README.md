Current tip of the development: [jg/wdk2023-6.7-rc3](https://github.com/jglathe/linux_ms_dev_kit/tree/jg/wdk2023-6.7-rc3), now based on [steev/linux](https://github.com/steev/linux/)

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
