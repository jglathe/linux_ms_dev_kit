#!/usr/bin/env bash

set -ex

if ! diff -u debian/changelog debian.qcom-x1e/changelog
then
    fakeroot debian/rules clean
fi

dh_clean

rm -rf debian/build/build-qcom-x1e/_____________________________________dkms/

export LLVM=1

# Detect if we're on ARM64/AArch64
if uname -m | grep -q 'aarch64\|arm64'; then
  echo "Native ARM64 compilation detected"
  time dpkg-buildpackage -d -b -k'jens.glathe@oldschoolsolutions.biz'
else
  echo "Cross-compiling to ARM64"
  time dpkg-buildpackage -d -b -aarm64 -k'jens.glathe@oldschoolsolutions.biz'
fi

#time debuild --no-lintian -k'jens.glathe@oldschoolsolutions.biz'
