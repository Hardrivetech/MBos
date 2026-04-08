FROM debian:bookworm-slim

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies needed to produce the ISO
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
    build-essential \
    gcc-multilib \
    nasm \
    grub-pc-bin \
    xorriso \
    ca-certificates \
    git \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /work

# Copy repository into the image
COPY . /work

# Build MBos inside the container
RUN make clean && make

# Drop to a shell by default
CMD ["/bin/bash"]
