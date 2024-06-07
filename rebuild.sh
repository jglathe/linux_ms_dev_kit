#!/usr/bin/env bash

set -ex

if ! diff -u debian/changelog debian.master/changelog
then
    fakeroot debian/rules clean
fi

dh_clean

rm -rf debian/build/build-generic/_____________________________________dkms/

export LLVM=1

time debuild --no-lintian -b -d -k'6213DBA4733B87D63492CBFD5F4E4567BA878F66'
