# Cross-builds custom_surface for the Mazda CMU (32-bit ARM, glibc 2.11).
#
#   docker build --output out .        ->  out/custom_surface
#
# Debian 11 (bullseye) from archive.debian.org: a frozen snapshot, so the build is
# reproducible. bullseye's glibc 2.31 matters: anything linked against glibc >= 2.34
# requires __libc_start_main@GLIBC_2.34, which the car does not have.
FROM debian:bullseye AS build

# Toolchain + link-time sysroot. The sysroot holds Debian's armel builds of the open-source
# libraries the firmware ships (same sonames; cairo headers from libcairo2-dev). They are
# unpacked without their dependencies and never run: on the car the firmware's own copies
# (cairo 1.10, Wayland ~1.4) are loaded.
RUN echo 'deb http://archive.debian.org/debian bullseye main' > /etc/apt/sources.list \
 && dpkg --add-architecture armel \
 && apt-get -o Acquire::Check-Valid-Until=false update -qq \
 && apt-get install -y -qq --no-install-recommends gcc-arm-linux-gnueabi libc6-dev-armel-cross \
 && cd /tmp \
 && apt-get download -qq libcairo2-dev:armel libcairo2:armel libfreetype6:armel libwayland-client0:armel \
 && for f in *.deb; do dpkg -x "$f" /sysroot; done \
 && rm -rf /tmp/*.deb /var/lib/apt/lists/*

ENV L=/sysroot/usr/lib/arm-linux-gnueabi
WORKDIR /src

# The firmware's proprietary IVI client libraries: stand-ins with the same sonames.
COPY ivi_stubs.c ./
RUN arm-linux-gnueabi-gcc -shared -nostdlib -Wl,-soname,libwayland-ivi-client.so.0 \
        -o $L/libwayland-ivi-client.so.0 ivi_stubs.c \
 && arm-linux-gnueabi-gcc -shared -nostdlib -Wl,-soname,libwayland-ivi-shell-client.so.1 -DIVI_SHELL \
        -o $L/libwayland-ivi-shell-client.so.1 ivi_stubs.c

# -march/-mfpu/-mfloat-abi=softfp: VFP instructions, but doubles passed in core registers —
#   the calling convention of the firmware libraries.
# -no-pie, no stack protector / fortify: the binary needs only GLIBC_2.4 symbols.
# --allow-shlib-undefined: the sysroot libraries' own dependencies are not unpacked.
COPY app.h app.c plat_wayland.c ./
RUN arm-linux-gnueabi-gcc -O2 -Wall -march=armv7-a -mfloat-abi=softfp -mfpu=vfpv3-d16 -marm -no-pie \
        -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
        -I/sysroot/usr/include/cairo \
        -o custom_surface app.c plat_wayland.c \
        -L$L -Wl,--allow-shlib-undefined \
        -l:libwayland-client.so.0 -l:libwayland-ivi-client.so.0 -l:libwayland-ivi-shell-client.so.1 \
        -l:libcairo.so.2 -l:libfreetype.so.6 -lm

FROM scratch
COPY --from=build /src/custom_surface /
