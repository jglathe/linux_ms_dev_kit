#!/usr/bin/env bash

set -ex

if ! diff -u debian/changelog debian.qcom-x1e/changelog
then
    fakeroot debian/rules clean
fi

dh_clean

rm -rf debian/build/build-qcom-x1e/_____________________________________dkms/

export LLVM=1

time dpkg-buildpackage -d -b -aarm64 -k'jens.glathe@oldschoolsolutions.biz'

#time debuild --no-lintian -k'jens.glathe@oldschoolsolutions.biz'
