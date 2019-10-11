#!/usr/bin/env bash

set -ex

if ! diff -u debian/changelog debian.master/changelog
then
    fakeroot debian/rules clean
fi

dh_clean

rm -rf debian/build/build-generic/_____________________________________dkms/

export LLVM=1

time dpkg-buildpackage -b -aarm64 -k'jens.glathe@oldschoolsolutions.biz'

#time debuild --no-lintian -k'jens.glathe@oldschoolsolutions.biz'
