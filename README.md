# **State of the repository**

This repository, originally set out to support the Windows Dev Kit 2023, has evolved somewhat. It does support it as best as possible IMO, and then some. It was a long time based on @jhovold's wip tree for x1e80100 with Support for the Lenovo Thinkpad X13s and T14s and followed the kernel development cycles of the upstream kernel (torvalds/linux). I added a selection of my own patches, from other repositories, from the kernel mailing list where I found adding the features/fixes beneficial. There are all upstream device trees for newer qualcomm SoCs, and some that are not yet upstreamed. Support focuses on the devices I have / can test with, naturally (listed in bold). This can be a tricky business.
Recently the base of the repository has changed to [arm64-laptops managed by Linaro](https://gitlab.com/Linaro/arm64-laptops/linux), also following the upstream cycle.

Support is now "as good as I can manage" for following devices:

### *based on SC8280XP*
- **Microsoft Windows Dev Kit 2023** (device tree is upstreamed)
- **Lenovo Thinkpad X13s** (minor tweaks to the upstream version)

### *based on X1E80100 (Hamoa) or binnings from it*
- **HP Omnibook X AI 14-fe0** (device tree is upstreamed)
- **Lenovo Thinkpad T14s** (minor tweaks from upstream)
- **Qualcomm Snapdragon Dev Kit for Windows** (derivatives from upstream)
- Acer Swift sf14-11
- Lenovo Yoga Slim 7x (minor tweaks from upstream)
- Asus Vivobook S15

### *based ox X1P42100 (Purwa) or binnings from it*
- Acer Swift Go sfg14-01
- Asus Vivobook S15 (x1p42100 version)
- HP Omnibook X AI 14-fe1 (x1p42100 version)
- **Lenovo Ideapad 5 2-in-1 14Q8X9 (83GH)**
- **Lenovo Ideapad Slim 5x 14Q8X9 (83HL)**
- **Lenovo Ideapad Slim 3x 15Q8X10 (83N3)**
- **Lenovo Thinkbook 16 G7 QOY (21NH)** 
- Microsoft SP12

The config used here is basically the one of Ubuntu Cocept X1E, with only minor additions. 

# **Install media**
There are a load of dedicated install media for certain devices (and some versions back) which are sort of all outdated now (<= Ubuntu 24.10). Creating them was a lot of manual work, therefore I'm happy that the current Ubuntu Concept 26.04 ISOs can boot all (well, most) of the X1 devices. But some of the dtbs are missing and need to be added to the ISO before it can boot. That is ongoing work. 

# **Kernel packages**
Since installing / removing kernels is now only a use of apt and dpkg, pre-built package sets are available [here](https://drive.google.com/drive/folders/1Lps5o3FXroAJFDiKj18vutJbC1uld49s?usp=drive_link). I publish them occasionally, after some testing here.

## **Acknowledgements**
The original tutorial from @chenguokai can be found [here](https://github.com/chenguokai/chenguokai/blob/master/tutorial-dev-kit-linux.md)
