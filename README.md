# swaylock-plugin

This is a fork of [`swaylock`](https://github.com/swaywm/swaylock), a screen
locking utility for Wayland compositors. With `swaylock-plugin`, you can for
your lockscreen background display the animated output from any wallpaper program
that implements the `wlr-layer-shell-unstable-v1` protocol. All you have to do
is run `swaylock-plugin --command 'my-wallpaper ...'`, where `my-wallpaper ...`
is replaced by your desired program. Examples:

* [`swaybg`](https://github.com/swaywm/swaybg), which displays regular background images
* [`mpvpaper`](https://github.com/GhostNaN/mpvpaper), which lets you play videos
* [`shaderbg`](https://git.sr.ht/~mstoeckl/shaderbg), renders OpenGL shaders
* [`rwalkbg`](https://git.sr.ht/~mstoeckl/rwalkbg), a very slow animation
* [`wscreensaver`](https://git.sr.ht/~mstoeckl/wscreensaver), an experiment in porting
   a few xscreensaver hacks to Wayland. Best with the `--command-each` flag.
* [`windowtolayer`](https://gitlab.freedesktop.org/mstoeckl/windowtolayer), a tool that
   can be used to run normally windowed applications, like terminals, as wallpapers.
   Requires `--command-each` flag. For example:
   ```
   swaylock-plugin --command-each 'windowtolayer -- termite -e neo-matrix'
   swaylock-plugin --command-each 'windowtolayer -- alacritty -e asciiquarium'
   ```
* You can rotate between wallpapers in a folder by setting the following script
  as the command; e.g.: `swaylock-plugin --command './example_rotate.sh /path/to/folder'`.
  (This works by periodically killing the wallpaper program, after which
  `swaylock-plugin` automatically restarts it.)
    ```
    #!/bin/sh
    file=`ls $1 | shuf -n 1`
    delay=60.
    echo "Runnning swaybg for $delay secs on: $1/$file"
    timeout $delay swaybg -i $1/$file
    ```
* Running X11 animation programs under `swaylock-plugin` is trickier, but
  can be done using `windowtolayer`, `Xwayland`, and a script
  ([`example_xwayland_wrapper.py`](example_xwayland_wrapper.py)) that runs a
  program under `Xwayland`. For example,
  ```
  swaylock-plugin --command-each \
    'windowtolayer example_xwayland_wrapper.py /usr/lib/xscreensaver/abstractile -root'
  ```


## Herb fork, releases, and prebuilt images

This repository is Herb's fork of `mstoeckl/swaylock-plugin`. All fork work
lives on the `herb/main` branch, which is based on upstream tag `v1.8.7`
(commit `117ed3fe`). `main` tracks upstream and is not modified.

Fork releases use tags of the form `v1.8.7-herb.1`. A release is built inside a
Fedora 44 distrobox by `ci/distrobox-build.sh` (the same recipe runs in GitHub
Actions and locally), so the resulting dynamically linked binary matches a
Fedora 44 host's system libraries. The binary is published as a minimal
`FROM scratch` OCI image at:

```
ghcr.io/herb/swaylock-plugin:<git tag>            # e.g. v1.8.7-herb.1
ghcr.io/herb/swaylock-plugin:<git tag>-fc45-x86_64
```

The image is a transport and provenance artifact, not a runnable container: it
contains only `/usr/bin/swaylock-plugin` and `/LICENSE`. Consumers pin the image
by digest (`ghcr.io/herb/swaylock-plugin@sha256:...`), copy the binary out with
`podman create` + `podman cp`, and install it against the host's PAM and shared
libraries. Builds use `GITHUB_TOKEN` for `packages: write` and publish
`actions/attest-build-provenance` attestations (`id-token: write`,
`attestations: write`). Pull requests only build and test; they never push.

To build locally:

```
./ci/distrobox-build.sh
build/swaylock-plugin --version
```

`DISTROBOX_NAME` and `DISTROBOX_IMAGE` override the default
`swaylock-plugin-ci` container and `registry.fedoraproject.org/fedora:45`
image.

` swaylock-plugin` requires that the Wayland compositor implement the `ext-session-lock-v1` protocol.

This is experimental software, so if something fails to work it's probably a bug
in this program -- report it at https://github.com/mstoeckl/swaylock-plugin .

As this fork is not nearly as well tested as the original swaylock, before using this
program, ensure that you can recover from both an unresponsive lockscreen and one
that has crashed. (For example, in Sway, by creating a `--locked` bindsym to kill and
restart swaylock-plugin; or by switching to a different virtual terminal, running
`killall swaylock-plugin` and running swaylock-plugin, and restarting with e.g. `WAYLAND_DISPLAY=wayland-1 swaylock-plugin` .)

See the man page, [`swaylock-plugin(1)`](swaylock.1.scd), for instructions on using swaylock-plugin.

## Grace period

`swaylock-plugin` adds a grace period feature; unlike the original `swaylock`, it
is not practical to emulate one using a separate program (like `chayang`) because
any animated backgrounds would be interrupted. With the `--grace` flag, it is
possible to unlock the screen without a password for the first few seconds after
the screen locker starts with either a key press or significant mouse motion.

This feature requires logind (systemd or elogind) support to automatically end the
grace period just before the computer goes to sleep. The grace period also ends on
receipt of the signal SIGUSR2.

### Example

Sway can be made to lock the screen with a grace period and the custom wallpaper
program specified in the script `lock-bg-command.sh` with the following configuration:

```
exec swayidle \
    timeout 300 'swaylock-plugin --grace 30sec --pointer-hysteresis 25.0 --command-each lock-bg-command.sh' \
    timeout 600 'swaymsg "output * dpms off"' \
       resume 'swaymsg "output * dpms on"' \
       before-sleep 'swaylock-plugin --command-each lock-bg-command.sh'
bindsym --locked Ctrl+Alt+L exec \
    'killall -SIGUSR2 swaylock-plugin; \
    swaylock-plugin --command-each lock-bg-command.sh'
```

This will, after 5 minutes of inactivity, start `swaylock-plugin`; for the next
30 seconds, one can easily unlock the screen by pressing any key or moving the
mouse more than 25 pixels in a one second period; afterwards, authentication
will be required. When the computer goes to sleep, the screen will lock for
certain. (If `swaylock-plugin` was running and in the grace period, the grace
period will end; in case `swaylock-plugin` was not running, a new instance will
be started without a grace period, that locks the screen if it was not already
locked.) One can also immediately lock the screen with a keybinding (or use the
keybinding to restart the lock screen, if it crashed.) Any screens will be turned
off after 10 minutes of inactivity.

## Fingerprint unlock

If `/etc/pam.d/swaylock-plugin-fprint` exists, `swaylock-plugin` starts a
separate PAM backend that authenticates fingerprints concurrently with the
password backend. Typing a password is never delayed by fingerprint
authentication, and a recognized finger unlocks the screen without typing
anything first. A failed match shows the usual "invalid" indicator and retries
after a short delay; a missing sensor or no enrolled prints backs off from 30
seconds up to two minutes instead of polling in a tight loop.

The fingerprint backend never receives password data: it sends only a fixed
success or failure byte over its own pipe, and the main process only accepts a
successful unlock from that pipe. The backend is killed and reaped on unlock
and exit (it also exits if the locker dies). It is left running across suspend,
since fprintd manages the device through the sleep: on resume it is respawned if
it has exited, and again if it exits within 30 seconds of resume.

No child is started while a suspend is under way. A locker started during a
suspend (for example from before-sleep) asks the sleep watcher at startup, and
the child is started on resume rather than into the suspend. A child that was
started less than a second before a suspend began is stopped and respawned on
resume.

The PAM service is not installed by `meson install`; create it yourself, for
example:

```
auth     required   pam_fprintd.so
account  include    login
```

Without that file (or on machines with no fingerprint reader) no fingerprint
backend is started and nothing changes.

## Installation

Install dependencies:

* meson \*
* wayland
* wayland-protocols \*
* libxkbcommon
* cairo
* gdk-pixbuf2
* pam (optional)
* systemd or elogind (optional)
* [scdoc](https://git.sr.ht/~sircmpwn/scdoc) (optional: man pages) \*
* git \*
* swaybg

_\* Compile-time dep_  

Run these commands:

    meson build
    ninja -C build
    sudo ninja -C build install

##### Without PAM

On systems without PAM, `swaylock-plugin` uses `shadow.h`.

Systems which rely on a tcb-like setup (either via musl's native support or via
glibc+[tcb]), require no further action.

[tcb]: https://www.openwall.com/tcb/

For most other systems, where passwords for all users are stored in `/etc/shadow`,
`swaylock-plugin` needs to be installed suid:

    sudo chmod a+s /usr/local/bin/swaylock-plugin

Optionally, on systems where the file `/etc/shadow` is owned by the `shadow`
group, the binary can be made sgid instead:

    sudo chgrp shadow /usr/local/bin/swaylock-plugin
    sudo chmod g+s /usr/local/bin/swaylock-plugin

`swaylock-plugin` will drop root permissions shortly after startup.
