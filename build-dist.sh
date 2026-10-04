#!/bin/bash
# Build qemu-system-arm and qemu-system-aarch64 from this tree and bundle a
# relocatable distribution under ./dist.
#
# What each bundle carries:
#
#   Linux    nothing. The runtime libraries come from the distribution, which
#            also keeps them patched -- bundling would freeze versions, and it
#            would not remove the glibc floor anyway since glibc is never
#            bundled. It would only make the bundle look more portable than it
#            is. Install libglib2.0-0 and libpixman-1-0 to run it.
#   macOS    nothing, but third-party libraries are linked statically
#            (build-static-deps.sh) so only system libraries remain. Without
#            that the binaries would carry absolute Homebrew paths.
#   Windows  the MSYS2 DLLs, copied next to the executables. There is no
#            package manager on a stock Windows machine to get them from.
#
# Usage: bash build-dist.sh
#
# Prerequisites:
#   Debian/Ubuntu: sudo apt install build-essential ninja-build pkg-config \
#                       python3-venv patchelf libglib2.0-dev libpixman-1-dev
#   macOS:         brew install pkg-config ninja meson
#                  (libraries are built from source -- see build-static-deps.sh)
#   MSYS2:         pacman -S mingw-w64-x86_64-{gcc,ninja,meson,pkgconf,glib2,pixman,python}
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
DIST_DIR="${SCRIPT_DIR}/dist"

case "$(uname -s)" in
    Linux)                PLATFORM=linux ;;
    Darwin)               PLATFORM=macos ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM=windows ;;
    *) echo "unsupported host: $(uname -s)" >&2; exit 1 ;;
esac
echo "=== Host: $(uname -s) $(uname -m) -> ${PLATFORM} ==="

# meson 1.5+ is required and most distributions are behind it, so fall back to
# a venv the way build-static-deps.sh does not have to (macOS ships a new
# enough one through Homebrew).
if command -v meson >/dev/null && command -v ninja >/dev/null; then
    PYTHON="$(command -v python3 || command -v python)"
else
    VENV_DIR="${SCRIPT_DIR}/.venv"
    if [ ! -d "${VENV_DIR}" ]; then
        python3 -m venv "${VENV_DIR}"
        "${VENV_DIR}/bin/pip" install --quiet meson ninja distlib tomli
    fi
    export PATH="${VENV_DIR}/bin:$PATH"
    PYTHON="${VENV_DIR}/bin/python3"
fi

# A lean emulator needs almost nothing: no display backend, no networking, no
# block layer, no accelerators but TCG. Every dependency switched off here is
# one fewer library to bundle below.
CONFIGURE_ARGS=(
    --target-list=arm-softmmu,aarch64-softmmu
    --python="${PYTHON}"
    --disable-docs
    --disable-tools
    --disable-guest-agent
    --disable-werror
    # display
    --disable-gtk
    --disable-sdl
    --disable-vnc
    --disable-curses
    --disable-opengl
    --disable-dbus-display
    --disable-spice
    --disable-vte
    # networking
    --disable-slirp
    --disable-libssh
    --disable-curl
    # block
    --disable-glusterfs
    --disable-libiscsi
    --disable-libnfs
    --disable-rbd
    --disable-blkio
    --disable-bzip2
    --disable-lzo
    --disable-snappy
    --disable-zstd
    # accelerators other than TCG
    --disable-kvm
    --disable-hvf
    --disable-whpx
    --disable-xen
    # misc
    --disable-capstone
    --disable-gio
    --disable-libdw
    --disable-modules
    --disable-numa
    --disable-plugins
    --disable-seccomp
    --disable-tpm
    --disable-install-blobs
)

# Linux-only options; passing them elsewhere would be rejected outright.
if [ "${PLATFORM}" = linux ]; then
    CONFIGURE_ARGS+=(--disable-linux-aio --disable-linux-io-uring)
fi

if [ "${PLATFORM}" = macos ]; then
    # Link third-party libraries statically from a pinned staging prefix so the
    # shipped binaries depend only on macOS system libraries. PKG_CONFIG_LIBDIR
    # (not _PATH) hides every other pkg-config tree -- Homebrew included.
    DEPS_PREFIX="${BUILD_DIR}/static-deps"
    bash "${SCRIPT_DIR}/build-static-deps.sh" --prefix "${DEPS_PREFIX}"
    export PKG_CONFIG_LIBDIR="${DEPS_PREFIX}/lib/pkgconfig"
    # PKG_CONFIG_PATH is searched *before* LIBDIR, so an inherited value would
    # shadow the staging prefix.
    unset PKG_CONFIG_PATH

    # pkg-config must be queried with --static so that Libs.private (system
    # frameworks, -lintl, ...) reaches the link line. QEMU's own --static
    # cannot be used: it puts -static in LDFLAGS, which macOS does not support
    # since there is no static libSystem. meson honours a $PKG_CONFIG wrapper.
    mkdir -p "${BUILD_DIR}"
    cat > "${BUILD_DIR}/pkg-config-static" <<'EOF'
#!/bin/sh
exec pkg-config --static "$@"
EOF
    chmod +x "${BUILD_DIR}/pkg-config-static"
    export PKG_CONFIG="${BUILD_DIR}/pkg-config-static"
fi

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"
"${SCRIPT_DIR}/configure" "${CONFIGURE_ARGS[@]}"

JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
ninja -j"${JOBS}" qemu-system-arm qemu-system-aarch64

echo
echo "=== Bundling distributable ==="
rm -rf "${DIST_DIR}"
mkdir -p "${DIST_DIR}/bin" "${DIST_DIR}/lib"

EXE=""
[ "${PLATFORM}" = windows ] && EXE=".exe"
for target in arm aarch64; do
    cp "${BUILD_DIR}/qemu-system-${target}${EXE}" "${DIST_DIR}/bin/"
    strip "${DIST_DIR}/bin/qemu-system-${target}${EXE}" 2>/dev/null || true
done
BINARIES=("${DIST_DIR}/bin/qemu-system-arm${EXE}"
          "${DIST_DIR}/bin/qemu-system-aarch64${EXE}")

case "${PLATFORM}" in
linux)
    # Nothing bundled, deliberately.
    #
    # Copying the shared objects in would freeze their versions and take them
    # out of the distribution's security updates -- pcre2 and zlib have both
    # had CVEs -- and it would not remove the glibc floor, because glibc is
    # never bundled. All it buys is not having to install two packages, at the
    # cost of a bundle that looks more portable than it is.
    #
    # The floor is still worth reporting, since it is the thing that actually
    # decides where this runs and nothing in the bundle makes it visible.
    floor=""
    for bin in "${BINARIES[@]}"; do
        v="$(objdump -T "${bin}" 2>/dev/null \
             | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -V | tail -1)"
        [ -n "${v}" ] || continue
        if [ -z "${floor}" ] || [ "$(printf '%s\n%s\n' "${floor}" "${v}" \
                                     | sort -V | tail -1)" = "${v}" ]; then
            floor="${v}"
        fi
    done
    # Read the list off the binaries rather than naming packages here: a hand
    # written list drifts the moment a configure flag changes.
    deps=""
    for bin in "${BINARIES[@]}"; do
        deps="${deps}$(readelf -d "${bin}" 2>/dev/null \
            | awk '/NEEDED/ {gsub(/[\[\]]/,"",$NF); print $NF}')"$'\n'
    done
    deps="$(echo "${deps}" | sort -u \
            | grep -vE '^(libc|libm|libpthread|libdl|librt|libgcc_s|ld-linux)' \
            | tr '\n' ' ')"

    rmdir "${DIST_DIR}/lib"
    echo "  no libraries bundled; the distribution provides these:"
    echo "    ${deps}"
    echo "  and it needs ${floor:-glibc} or newer: the build host's version"
    echo "  decides that, and nothing in the artifact changes it."
    ;;
macos)
    # Nothing to bundle: build-static-deps.sh linked everything statically.
    # Anything outside the system trees means a dependency leaked in
    # dynamically and the bundle would not run on a machine without it.
    for bin in "${BINARIES[@]}"; do
        chmod u+w "${bin}"
        leftover="$(otool -L "${bin}" | tail -n +2 | awk '{print $1}' \
            | grep -Ev '^(/usr/lib/|/System/)' || true)"
        if [ -n "${leftover}" ]; then
            echo "ERROR: ${bin} references non-system libraries:" >&2
            echo "${leftover}" >&2
            exit 1
        fi
        codesign --force --sign - "${bin}"
    done
    rmdir "${DIST_DIR}/lib"
    echo "  statically linked; re-signed"
    ;;
windows)
    for bin in "${BINARIES[@]}"; do
        while read -r dll; do
            [ -n "${dll}" ] || continue
            cp -L "${dll}" "${DIST_DIR}/bin/"
        done < <(ldd "${bin}" 2>/dev/null | awk '/=>/ {print $3}' \
                     | grep -i '^/mingw64' | sort -u)
    done

    # Same reasoning as the Linux branch: MSYS2 is on the runner's PATH, so a
    # binary started here finds its DLLs whether or not they were copied.
    # What says the bundle is self-contained is that every DLL it names from
    # /mingw64 is sitting next to it.
    missing=""
    for bin in "${BINARIES[@]}"; do
        while read -r name path; do
            case "${path}" in
                /mingw64/*)
                    [ -e "${DIST_DIR}/bin/${name}" ] || missing="${missing} ${name}" ;;
            esac
        done < <(ldd "${bin}" 2>/dev/null | awk '/=>/ {print $1, $3}')
    done
    if [ -n "${missing}" ]; then
        echo "ERROR: bundle is missing DLLs:${missing}" >&2
        exit 1
    fi
    rmdir "${DIST_DIR}/lib"
    echo "  bundled $(ls -1 "${DIST_DIR}"/bin/*.dll 2>/dev/null | wc -l) DLLs, closure complete"
    ;;
esac

# pc-bios keymaps are only read by display backends, all of which are off; the
# binaries locate data relative to themselves, so keep the layout they expect
# in case one is ever enabled.
mkdir -p "${DIST_DIR}/share/qemu"
cp -r "${SCRIPT_DIR}/pc-bios/keymaps" "${DIST_DIR}/share/qemu/" 2>/dev/null || true

echo
echo "=== Distributable ready: ${DIST_DIR} ==="
find "${DIST_DIR}" -type f | sed "s|${SCRIPT_DIR}/||"
