FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    build-essential \
    pkg-config \
    libfuse3-dev \
    fuse3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .
RUN make release

# Create 4 OST backing directories
RUN mkdir -p /data/ost0 /data/ost1 /data/ost2 /data/ost3 /mnt/striped

# Container needs --privileged or --cap-add SYS_ADMIN --device /dev/fuse for FUSE
CMD ["./stripefs", "--mount", "/mnt/striped", \
     "--ost", "/data/ost0", "--ost", "/data/ost1", \
     "--ost", "/data/ost2", "--ost", "/data/ost3", "--foreground"]
