#!/usr/bin/env bash
set -euxo pipefail

FLAVOUR=qcom-x1e
ARCH=$(dpkg-architecture -qDEB_HOST_ARCH)

# Ensure we start clean enough for packaging, but keep the source tree usable
if ! diff -q debian/changelog debian.qcom-x1e/changelog >/dev/null 2>&1; then
    fakeroot debian/rules clean
fi
dh_clean

# Drop any previous half-staged package dirs that might confuse the second pass
rm -rf debian/linux-*-${FLAVOUR}* debian/build/build-${FLAVOUR}/_____________________________________dkms/ || true

mkdir -p ../build-stubble ../build-nostubble

export LLVM=1

build_and_collect() {
    local dest="$1"
    shift
    # Remaining args are extra make variables / dpkg-buildpackage flags
    if uname -m | grep -q 'aarch64\|arm64'; then
        dpkg-buildpackage -d -b -k'jens.glathe@oldschoolsolutions.biz' "$@"
    else
        dpkg-buildpackage -d -b -aarm64 -k'jens.glathe@oldschoolsolutions.biz' "$@"
    fi

    # Move everything that looks like a package for this flavour (and common ones)
    shopt -s nullglob
    mv ../*-${FLAVOUR}_*.deb ../*-${FLAVOUR}_*.ddeb \
       ../linux-bpf-dev_*.deb ../linux-bpf-dev_*.ddeb \
       ../linux-libc-dev_*.deb \
       "$dest"/ 2>/dev/null || true
    # Also catch any other leftover debs from this run if you want them
    cp ../*.deb ../*.ddeb "$dest"/ 2>/dev/null || true
    cp ../*.buildinfo ../*.changes "$dest"/ 2>/dev/null || true
    shopt -u nullglob
}

echo "=== Building stubble variant ==="
build_and_collect ../build-stubble

echo "=== Building nostubble variant from existing objects ==="
# Clear only the packaging stamps/staging so the install step re-runs,
# but leave debian/build/build-${FLAVOUR} intact.
rm -f debian/stamps/stamp-install-${FLAVOUR} debian/stamps/stamp-build-${FLAVOUR} || true
rm -rf debian/linux-*-${FLAVOUR}* || true

# do_full_build=false preserves the object tree; do_stubble=false skips ukify
if uname -m | grep -q 'aarch64\|arm64'; then
    fakeroot debian/rules do_stubble=false do_full_build=false binary-${FLAVOUR}
else
    # Cross case: dpkg-buildpackage still works, but we force the rules vars
    dpkg-buildpackage -d -b -aarm64 -k'jens.glathe@oldschoolsolutions.biz' \
        --rules-file=debian/rules -R 'debian/rules do_stubble=false do_full_build=false'
fi

# Collect the second set
shopt -s nullglob
mv ../*-${FLAVOUR}_*.deb ../*-${FLAVOUR}_*.ddeb \
   ../linux-bpf-dev_*.deb ../linux-bpf-dev_*.ddeb \
   ../linux-libc-dev_*.deb \
   ../*.buildinfo ../*.changes \
   ../build-nostubble/ 2>/dev/null || true
mv ../*.deb ../*.ddeb ../build-nostubble/ 2>/dev/null || true
shopt -u nullglob

echo "Done."
echo "Stubble packages:   ../build-stubble/"
echo "No-stubble packages: ../build-nostubble/"
