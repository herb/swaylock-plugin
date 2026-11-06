#!/usr/bin/env bash
# Build and test swaylock-plugin inside a Fedora distrobox.
#
# This mirrors how the dotfiles repository builds the binary for the host: the
# source is compiled in a Fedora container whose major version matches the
# host, then the resulting dynamically linked executable is copied out. The
# distrobox shares the host home directory, so the checkout and the build
# output stay on the host filesystem.
#
# Environment:
#   DISTROBOX_NAME    container name (default: swaylock-plugin-ci)
#   DISTROBOX_IMAGE   container image (default: registry.fedoraproject.org/fedora:45)
#   MESON_BUILD_DIR   build directory relative to the repository (default: build)
#
# The binary is left at "$repo_root/$MESON_BUILD_DIR/swaylock-plugin".
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
distrobox_name="${DISTROBOX_NAME:-swaylock-plugin-ci}"
distrobox_image="${DISTROBOX_IMAGE:-registry.fedoraproject.org/fedora:45}"
build_dir="${MESON_BUILD_DIR:-build}"

if ! command -v distrobox >/dev/null 2>&1; then
	echo "error: distrobox is required (https://distrobox.it)" >&2
	exit 1
fi
if ! command -v podman >/dev/null 2>&1; then
	echo "error: podman is required by distrobox" >&2
	exit 1
fi

build_deps=(
	meson
	ninja-build
	git
	gcc
	wayland-devel
	wayland-protocols-devel
	libxkbcommon-devel
	cairo-devel
	gdk-pixbuf2-devel
	pam-devel
	systemd-devel
	scdoc
)

if ! distrobox list --no-color | awk 'NR > 1 { print $2 }' | grep -qx "${distrobox_name}"; then
	echo "creating distrobox ${distrobox_name} from ${distrobox_image}" >&2
	distrobox create --name "${distrobox_name}" --image "${distrobox_image}" --yes
fi

echo "installing build dependencies in ${distrobox_name}" >&2
distrobox enter --no-tty "${distrobox_name}" -- \
	sudo -n dnf -y install "${build_deps[@]}"

quoted_repo="$(printf '%q' "${repo_root}")"
quoted_build="$(printf '%q' "${build_dir}")"

echo "building and testing in ${distrobox_name}" >&2
# Start from a clean build directory so stale meson state from a previous
# checkout can never change the result.
distrobox enter --no-tty "${distrobox_name}" -- bash -lc \
	"cd ${quoted_repo} && rm -rf ${quoted_build} && meson setup ${quoted_build} --prefix /usr && ninja -C ${quoted_build} && meson test -C ${quoted_build} --print-errorlogs"

binary="${repo_root}/${build_dir}/swaylock-plugin"
if [[ ! -x "${binary}" ]]; then
	echo "error: expected build output at ${binary}" >&2
	exit 1
fi
watcher="${repo_root}/${build_dir}/swaylock-sleep-watcher"
if [[ ! -x "${watcher}" ]]; then
	echo "error: expected build output at ${watcher}" >&2
	exit 1
fi

# The binary links the box's Fedora system libraries, so verify the release
# identity and linkage inside the same box rather than on the host/runner.
echo "verifying ${binary} in ${distrobox_name}" >&2
distrobox enter --no-tty "${distrobox_name}" -- bash -lc \
	"set -euo pipefail
	cd ${quoted_repo}
	. /etc/os-release
	echo \"distrobox OS: \${ID} \${VERSION_ID}\"
	${quoted_build}/swaylock-plugin --version
	${quoted_build}/swaylock-plugin --version | grep -q '1\\.8\\.7'
	if ldd ${quoted_build}/swaylock-plugin | grep 'not found'; then exit 1; fi"

echo "built ${binary}"
