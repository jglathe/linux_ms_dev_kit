#!/bin/sh
make -j8 ubuntu_x1e_defconfig
scripts/config --undefine GDB_SCRIPTS
scripts/config --undefine DEBUG_INFO
scripts/config --undefine DEBUG_INFO_SPLIT
scripts/config --undefine DEBUG_INFO_REDUCED
scripts/config --undefine DEBUG_INFO_COMPRESSED
scripts/config --set-val DEBUG_INFO_NONE y
scripts/config --set-val DEBUG_INFO_DWARF5 n
scripts/config --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT
scripts/config --disable SYSTEM_TRUSTED_KEYS
scripts/config --disable SYSTEM_REVOCATION_KEYS
make -j8 menuconfig
