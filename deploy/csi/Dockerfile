# Stage 1: Build Go CSI driver
FROM golang:1.22-bookworm AS go-builder

WORKDIR /build
COPY deploy/csi/go.mod deploy/csi/go.sum ./
RUN go mod download

COPY deploy/csi/ ./
RUN CGO_ENABLED=0 GOOS=linux go build -o /stripefs-csi-driver ./cmd/stripefs-csi-driver

# Stage 2: Build C stripefs binary
FROM ubuntu:22.04 AS c-builder

RUN apt-get update && apt-get install -y \
    build-essential \
    pkg-config \
    libfuse3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY Makefile ./
COPY src/ ./src/
RUN make release

# Stage 3: Runtime image
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    fuse3 \
    libfuse3-3 \
    && rm -rf /var/lib/apt/lists/*

# Copy binaries from builder stages.
COPY --from=go-builder /stripefs-csi-driver /usr/local/bin/stripefs-csi-driver
COPY --from=c-builder /app/stripefs /usr/local/bin/stripefs

# Create data directory for OST volumes.
RUN mkdir -p /data/volumes

ENTRYPOINT ["/usr/local/bin/stripefs-csi-driver"]
