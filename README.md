Current tip of the development: [jg/ubuntu-blackrock-v6.10.y](https://github.com/jglathe/linux_ms_dev_kit/tree/jg/ubuntu-blackrock-v6.10.y), now based on [steev/linux](https://github.com/steev/linux/), cross checked against [jhovold/linux](https://github.com/jhovold/linux.git) and [torvalds/linux](https://github.com/torvalds/linux.git)

# **Recent Changes**
* Build system has changed to Ubuntu-Mainline, including devkit_defconfig adapted to Ubuntu distro standards. Latest is 6.10.0, working nicely.
* There is a branch for [el2](https://github.com/jglathe/linux_ms_dev_kit/tree/jg/ubuntu-el2-blackrock-v6.10.y) (providing /dev/kvm suport), running 24/7 on another wdk.
* There is also a branch for [pop-os](https://github.com/jglathe/linux_ms_dev_kit/tree/jg/pop-blackrock-v6.10.y) using the pop build system. Since 24.04 is still pre-alpha, running when I want to see the state of things 😁 I also have a tree for the [ISO builder](https://github.com/jglathe/pop-os-iso/tree/jg/arm64-x13s), giving a bootable installer ISO for Pop!_OS.
* flash-kernel is now supported. Windows Dev Kit 2023 is not in the database, the required entries are in the [wdk2023-syshacks](https://github.com/jglathe/wdk2023_syshacks) repository. This works really well IMO, it makes you forget that there is some dtb handling required.It also works in combination with grub and systemd-boot (although systemd-bootmight be quite a PITA on the WDK).
* Wireless (WCN6855) firmware has been [updated](https://github.com/jglathe/wdk2023_syshacks/tree/wlan) to .41, with a new board-2.bin to also include a working calibration set for the WDK. This is newer than what linux-firmware has. I'm trying to get the calibration upstream. 

# **Bootable Image**
Preinstalled desktop images live [here](https://drive.google.com/drive/folders/1sc_CpqOMTJNljfvRyLG-xdwB0yduje_O?usp=drive_link). The latest is Ubuntu 24.04 with kernel 6.10 for wdk2023. Sometimes images for the Lenovo Thinkpad X13s are available, too. They can be written with the disks utility or Balena Etcher or Rufus (or dd for the adventurous) onto an USB stick or external SSD, and booted. 

There are some special properties:

- The first boot will try to [copy](https://github.com/jglathe/wdk2023_fw_fetch) device-specific firmwares (*8280.mbn) from the Windows installation of the internal nvme if it is accessible. Aferwards, it reboots once.
- The image comes up with the Ubiquity installer. Should work well enough.

The image is tested for the use case that the local Windows installation is accessible, might balk a lot if this is not the case. For operation reasons I would recommend to keep the installation. It helps with resetting the power management after a failed boot with Linux, and you get the newest firmware when available through Windows Update.

# **Kernel packages**
Since installing / removing kernels is now only a use of apt and dpkg, pre-built package sets are available [here](https://drive.google.com/drive/folders/1Lps5o3FXroAJFDiKj18vutJbC1uld49s?usp=drive_link). I publish them occasionally, after some testing here.

## **Acknowledgements**
The original tutorial from @chenguokai can be found [here](https://github.com/chenguokai/chenguokai/blob/master/tutorial-dev-kit-linux.md)
