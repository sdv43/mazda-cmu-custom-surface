# Custom window on the Mazda Connect CMU

A native 800×480 Wayland window drawn on top of the stock Mazda Connect HMI.
It shows and hides on demand, draws with cairo, receives touch and the commander
knob/buttons, and gives the keyboard focus back to the HMI when it is done.

Tested on CMU firmware **70.00.335 EU N**.

The `-d` flag starts the built-in demo, an input test. It shows the last pressed key and key
history, knob rotation, multi-touch points with trails, and touch-accuracy targets.

## Build

All you need is Docker. The toolchain runs inside the container, so the host OS
doesn't matter.

```sh
git clone <this repository>
cd <repository>
docker build --output out .
```

The result is `out/custom_surface`, a 32-bit ARM executable. It links dynamically against
libraries already on the head unit (Wayland and IVI client libraries, cairo, FreeType),
so the target needs nothing else.

## Deploy and run over SSH

If the head unit is reachable over SSH, `cmu.sh` builds the binary, copies it, and starts or
stops it:

```sh
export CAR_HOST=192.168.1.50     # <- replace with the IP address of YOUR head unit

./cmu.sh deploy                  # build + copy to /tmp/custom_surface on the head unit
./cmu.sh start -d                # start (options as below); prints the first log lines
./cmu.sh stop                    # hide, give the keyboard back to the HMI, exit
```

> **Note:** `CAR_HOST` has no default. You must set it to the IP address your head unit
> has on your network, or the script stops with an error. The SSH user is `jci` (override
> with `CAR_USER`). You're asked for the password once per command.

The binary goes to `/tmp` (tmpfs), so it disappears after a head-unit reboot.

## Run

Run it on the head unit from a root shell:

```sh
./custom_surface -d          # demo: input test
./custom_surface -t 60       # plain window, hides and exits after 60 s
```

```
custom_surface [-d] [-z ZORDER] [-a ALPHA] [-k] [-H] [-t SECONDS]
  -d          demo / input test (implies -k)
  -z ZORDER   z-order when shown (default 1000, above the HMI)
  -a ALPHA    opacity when shown, 0..255 (default 255)
  -k          take the keyboard focus while shown, give it back to JCI_OPERA_PRIMARY on hide/exit
  -H          start hidden (show later with SIGUSR1)
  -t SECONDS  hide and exit after N seconds
env: WAYLAND_IVI_SURFACE_ID=name (default MY_CUSTOM_SURFACE)
```

| Signal                            | Action                                        |
| --------------------------------- | --------------------------------------------- |
| `SIGUSR1`                         | show (and take the keyboard with `-k` / `-d`) |
| `SIGUSR2`                         | hide (and give the keyboard back to the HMI)  |
| `SIGTERM`, `SIGINT`, `-t` timeout | hide, give the keyboard back, exit            |

In demo mode the window also exits on the on-screen **Exit** button, after any key is held
for 3 s, or after 2 minutes without input. The log goes to stderr.

**Recovery.** The program changes nothing on the system, and a head-unit reboot restores the
stock state. If the process was killed with `SIGKILL` and the knob no longer controls the
HMI, run:

```sh
ivi-shell -a MY_CUSTOM_SURFACE:0      # hide the surface
ivi-shell -f JCI_OPERA_PRIMARY        # keyboard focus back to the HMI
```

## Input

Keys arrive as evdev codes (`linux/input.h`). Codes observed from the control panel:

| Code                  | evdev                                | Control                                |
| --------------------- | ------------------------------------ | -------------------------------------- |
| 49 / 50               | `KEY_N` / `KEY_M`                    | knob rotation (one code per direction) |
| 103 / 108 / 105 / 106 | `KEY_UP` / `DOWN` / `LEFT` / `RIGHT` | knob tilt                              |
| 28                    | `KEY_ENTER`                          | knob press (probably)                  |
| 14                    | `KEY_BACKSPACE`                      | Back button (probably)                 |
| 102                   | `KEY_HOME`                           | Home button (probably)                 |
| 18 / 19 / 20          | `KEY_E` / `KEY_R` / `KEY_T`          | shortcut buttons                       |

The compositor sends no auto-repeat, and the system handles volume itself. Touch arrives as
`wl_touch` in screen pixels (verified with up to 4 fingers). Only the visible window receives it.

## Code

| File             | Contents                                                                                                                                  |
| ---------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `app.c`, `app.h` | the application: drawing (cairo) and input logic. `draw_static()` is the place for your own UI; `app_key()` / `app_touch()` receive input |
| `plat_wayland.c` | the platform layer: Wayland connection, shm buffers, IVI registration, show/hide, keyboard focus, input events, signals, event loop       |
| `ivi_stubs.c`    | link-time stand-ins for the firmware's proprietary IVI client libraries                                                                   |
| `Dockerfile`     | cross-compilation                                                                                                                         |
| `cmu.sh`         | helper: build, deploy over SSH, start, stop                                                                                               |

## How it works

- The compositor (`wayland-viv-compositor`) creates new surfaces **invisible**
  (`initial-visibility = hide` in `/etc/wayland/compositor.conf`). The window becomes visible
  only after it gets an alpha and a z-order above the HMI.
- The firmware has Wayland ~1.4 and no headers. All requests go through `wl_proxy_marshal*()`
  with opcodes taken from the interface tables of the firmware libraries:
  - `wl_ivi.register_surface(surface, name)`: op 0
  - `wl_ivi_shell.create_animation_group`: op 0
  - `wl_ivi_shell.set_keyboard_focus(seat, name)`: op 1
  - animation group: `animate_scalar(name, prop, t0, t1, value, 0, 0)` op 5, `start` op 0,
    `destroy` op 3; `prop` is zorder 1, alpha 2 (value `a/255` as `wl_fixed`).
  This is the same thing `/usr/bin/ivi-shell -z/-a/-f` does.
- The knob and buttons go to the one surface that has the keyboard focus, normally
  `JCI_OPERA_PRIMARY` (the HMI). **The HMI never takes the focus back by itself**, so the
  window always returns it on hide and exit.
- This libwayland's `wl_display_dispatch()` restarts `poll()` on `EINTR`, so signals can't
  interrupt it. The program uses its own `prepare_read / poll / read_events` loop instead.
- Drawing: cairo **1.10** from the firmware, CPU rendering into an `XRGB8888` shm buffer
  (~11 ms per demo frame). Text uses the HMI font Tipperary (`/jci/fonts`) through FreeType.
  This cairo is built without fontconfig, so fonts are loaded from files.
- Build: `arm-linux-gnueabi` with `-mfloat-abi=softfp` (the firmware's calling convention),
  `-no-pie`, and no stack protector or fortify. The result needs only `GLIBC_2.4` symbols, and
  the head unit runs glibc 2.11. Use only the cairo API up to 1.10: the build links against a
  newer cairo, so newer calls compile but fail to load on the car.

## Disclaimer

Not affiliated with Mazda. This runs on a real car's head unit: use at your own risk, and
don't use it while driving.
