# SPDX-License-Identifier: BSD-3-Clause
# (c) Copyright 2023 Advanced Micro Devices, Inc.

ARG FEDORA_VERSION=latest
ARG UBI9_VERSION=latest

FROM registry.fedoraproject.org/fedora:${FEDORA_VERSION} AS builder
WORKDIR /src

# git used to derive version number when run from a git clone
RUN dnf -y install git tar bzip2 redhat-rpm-config make gcc libmnl-devel libcap-devel gpsd-devel

# Use build flags from redhat-rpm-config to select the same optimisations
# and hardening chosen by base platform
RUN echo \
  export CFLAGS=\"$(rpm --eval %{__global_cflags})\" \
  export LDFLAGS=\"$(rpm --eval %{__global_ldflags})\" \
  > /build.env

# Build sfptpd
COPY . /src/sfptpd
WORKDIR /src/sfptpd
RUN make patch_version
RUN . /build.env && DESTDIR=/staging INST_INITS= make NO_GPS= install

FROM registry.access.redhat.com/ubi9-minimal:${UBI9_VERSION} AS runtime
RUN microdnf install -y libmnl
COPY --from=builder /staging /
COPY --from=builder /usr/lib64/libgps.so* /usr/lib64/libdbus-1.so* /usr/lib64
WORKDIR /var/lib/sfptpd

# Override any 'daemon' setting in selected configuration.
# Select a default configuration that can be overriden by runtime arguments.
ENTRYPOINT ["/usr/sbin/sfptpd", "--no-daemon", "-f", "/usr/share/doc/sfptpd/config/default.cfg" ]

# Send output to the console if running default config without arguments
CMD ["--console"]
