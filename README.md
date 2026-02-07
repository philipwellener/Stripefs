# stripefs

A FUSE3 filesystem that stripes file data across multiple storage targets, modeled on Lustre's architecture.

![C](https://img.shields.io/badge/language-C11-blue)
![FUSE](https://img.shields.io/badge/interface-FUSE3-orange)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey)
![Build](https://img.shields.io/badge/build-gcc%20--std%3Dc11-brightgreen)
![Kubernetes](https://img.shields.io/badge/deploy-Kubernetes%20CSI-326CE5)

stripefs decomposes files into fixed-size chunks and distributes them round-robin across independent storage directories (acting as Lustre-style OSTs). Reads reassemble the chunks transparently. An LRU cache with TTL expiration and write-through invalidation sits in front of the stripe engine, and a stats endpoint exposes live telemetry. The whole thing runs in userspace via FUSE3, with a CSI driver for Kubernetes deployment.

## Architecture

```
┌─────────────────────────────────────┐
│           User Applications          │
│         (ls, cat, cp, etc.)          │
└──────────────┬──────────────────────┘
               │  POSIX calls
               ▼
┌─────────────────────────────────────┐
│          Linux VFS / FUSE           │
│        (kernel ↔ userspace)         │
└──────────────┬──────────────────────┘
               │  FUSE callbacks
               ▼
┌──────────────────────────────────────────────────┐
│                    stripefs                        │
│  ┌─────────┐  ┌──────────────────┐  ┌──────────┐ │
│  │ LRU     │  │ FUSE Operations  │  │ Stripe   │ │
│  │ Cache   │  │ (getattr, read,  │  │ Engine   │ │
│  │ Layer   │  │  readdir, open,  │  │ (split/  │ │
│  │         │  │  write, etc.)    │  │ reassem) │ │
│  └────┬────┘  └───────┬──────────┘  └────┬─────┘ │
│       │               │                  │        │
│  ┌────┴───────────────┴──────────────────┴─────┐  │
│  │         Stripe Distribution Layer            │  │
│  │  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐    │  │
│  │  │ OST0 │  │ OST1 │  │ OST2 │  │ OST3 │    │  │
│  │  └──────┘  └──────┘  └──────┘  └──────┘    │  │
│  └─────────────────────────────────────────────┘  │
│  ┌─────────────────────────────────────────────┐  │
│  │  Stats / Telemetry Engine                    │  │
│  └─────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────┘
```

## Striping

Files are divided into fixed-size chunks (default 1 MB) and assigned to OSTs by `chunk_index % num_osts`. This is the same round-robin policy Lustre uses by default.

### Concrete example

A 4.5 MB file with a 1 MB stripe size across 4 OSTs:

```
File (4.5 MB total):
 ┌────────┬────────┬────────┬────────┬────────┐
 │Chunk 0 │Chunk 1 │Chunk 2 │Chunk 3 │Chunk 4 │
 │ 1 MB   │ 1 MB   │ 1 MB   │ 1 MB   │ 0.5 MB │
 └────────┴────────┴────────┴────────┴────────┘

Round-robin assignment (chunk_index % num_osts):
  Chunk 0  →  0 % 4 = OST0   (1 MB)
  Chunk 1  →  1 % 4 = OST1   (1 MB)
  Chunk 2  →  2 % 4 = OST2   (1 MB)
  Chunk 3  →  3 % 4 = OST3   (1 MB)
  Chunk 4  →  4 % 4 = OST0   (0.5 MB)

Result:
  OST0: Chunks 0 and 4  (1.5 MB)
  OST1: Chunk 1          (1 MB)
  OST2: Chunk 2          (1 MB)
  OST3: Chunk 3          (1 MB)
```

On read, the stripe engine computes which chunks cover the requested byte range, reads each chunk from the appropriate OST directory, and reassembles them into a contiguous buffer. On write, the incoming data is split into chunks and written to the correct OSTs, then the stripe layout metadata is updated.

## Caching

An LRU cache (bounded, default 128 MB) sits between the FUSE ops layer and the stripe engine. Lookups are O(1) via uthash; eviction is O(1) via a doubly-linked list. Each entry carries a TTL timestamp. Writes invalidate the corresponding cache entry immediately (write-through). The cache is protected by a `pthread_rwlock_t` so concurrent reads don't block each other.

```
Read request arrives
        │
        ▼
   ┌─────────┐    cache hit     ┌──────────────┐
   │  Check   │ ──────────────► │ Check TTL    │
   │  Cache   │                 │ (expired?)   │
   └─────────┘                  └──────┬───────┘
        │                         yes/  \no
   cache miss                    /      \
        │                       ▼        ▼
        ▼                   Evict &    Return
   ┌──────────┐             re-fetch   cached
   │  Stripe   │                       data
   │  Engine   │
   │  (read    │
   │  chunks)  │
   └─────┬────┘
         │
         ▼
   Insert into cache
   (evict LRU if full)
```

## Building and Running

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt-get install -y libfuse3-dev gcc make pkg-config

# Debug build (includes sanitizers and debug symbols)
make debug

# Release build (optimized, stripped)
make release
```

### Run

```bash
# Create OST backing directories and a mount point
mkdir -p /tmp/ost0 /tmp/ost1 /tmp/ost2 /tmp/ost3
mkdir -p /tmp/stripefs_mount

# Mount the filesystem with 4 OSTs
./stripefs \
    --mount /tmp/stripefs_mount \
    --ost /tmp/ost0 \
    --ost /tmp/ost1 \
    --ost /tmp/ost2 \
    --ost /tmp/ost3 \
    --stripe-size 1048576 \
    --foreground

# In another terminal, use it like a normal filesystem
echo "hello world" > /tmp/stripefs_mount/test.txt
cat /tmp/stripefs_mount/test.txt

# Unmount when done
fusermount3 -u /tmp/stripefs_mount
```

### Docker

```bash
docker build -t stripefs .
docker run --privileged stripefs
```

## Configuration

| Flag | Default | Description |
|---|---|---|
| `--mount` | *(required)* | FUSE mount point directory |
| `--ost` | *(required, repeatable)* | OST backing directory |
| `--stripe-size` | `1048576` (1 MB) | Stripe chunk size in bytes |
| `--cache-size` | `128` | Maximum cache size in MB |
| `--ttl` | `30` | Cache entry TTL in seconds |
| `--verbose` | `off` | Verbose logging to stderr |
| `--foreground` | `off` | Run in foreground (don't daemonize) |
| `--help` | -- | Show usage and exit |

## Observability

A virtual file at `/.stripefs_stats` inside the mount returns live JSON telemetry:

```bash
cat /tmp/stripefs_mount/.stripefs_stats
```

```json
{
  "cache": {
    "hits": 14823,
    "misses": 3241,
    "hit_rate_pct": 82.06,
    "evictions": 587,
    "bytes_served": 312475648
  },
  "io": {
    "total_read_ops": 18064,
    "total_write_ops": 8391,
    "bytes_read_from_osts": 332398592,
    "bytes_written_to_osts": 167772160
  },
  "osts": [
    { "ost_index": 0, "bytes_read": 83886080, "bytes_written": 41943040 },
    { "ost_index": 1, "bytes_read": 82837504, "bytes_written": 41943040 },
    { "ost_index": 2, "bytes_read": 82837504, "bytes_written": 41943040 },
    { "ost_index": 3, "bytes_read": 82837504, "bytes_written": 41943040 }
  ]
}
```

Tracks cache hit rate, per-OST byte counters (useful for verifying balanced distribution), operation counts, and eviction rate.

## Testing

```bash
make test          # Unit tests — stripe engine, LRU cache, stats engine
make integration   # Mounts a real instance, exercises full POSIX path
make stress        # Concurrent readers/writers, consistency checks
```

Unit tests validate chunk index calculation, eviction ordering, TTL expiration, and thread safety. Integration tests verify end-to-end correctness through the FUSE mount. Stress tests run concurrent workloads and check for data corruption.

## Kubernetes CSI Deployment

stripefs includes a CSI driver that delivers FUSE-mounted striped storage to Kubernetes pods. Pods requesting a stripefs PVC get a live FUSE mount backed by the stripefs binary.

```
┌──────────────────────────────────────────────────────────┐
│                    Kubernetes Node                         │
│                                                            │
│  ┌────────────────┐       ┌──────────────────────────┐    │
│  │  Consumer Pod   │       │  CSI DaemonSet Pod        │    │
│  │                 │       │                            │    │
│  │  /mnt/striped ──┼───────┼─► stripefs (FUSE)         │    │
│  │  (PVC mount)    │       │     │                      │    │
│  └────────────────┘       │     ├─► /data/.../ost0     │    │
│                            │     ├─► /data/.../ost1     │    │
│                            │     ├─► /data/.../ost2     │    │
│                            │     └─► /data/.../ost3     │    │
│                            │                            │    │
│                            │  stripefs-csi-driver (Go)  │    │
│                            │  node-driver-registrar     │    │
│                            └──────────────────────────┘    │
└──────────────────────────────────────────────────────────┘
```

The driver implements Identity and Node services only — FUSE mounts are node-local, so there's no Controller. `NodePublishVolume` creates OST directories and launches stripefs; `NodeUnpublishVolume` tears down the mount.

### Quick Start (Minikube)

```bash
# Deploy (starts minikube if needed, builds image, applies manifests)
./deploy/csi/scripts/deploy.sh

# Verify
kubectl get csidrivers
kubectl -n kube-system get pods -l app=stripefs-csi-driver
kubectl get pv,pvc

# Write and read through the striped mount
kubectl exec stripefs-test-pod -- bash -c 'echo "hello stripefs" > /mnt/striped/test.txt'
kubectl exec stripefs-test-pod -- cat /mnt/striped/test.txt

# Check striping
kubectl exec stripefs-test-pod -- dd if=/dev/urandom of=/mnt/striped/big.bin bs=1M count=8
kubectl exec stripefs-test-pod -- cat /mnt/striped/.stripefs_stats

# Tear down
./deploy/csi/scripts/teardown.sh
```

Volume parameters are set via `volumeAttributes` on the PV: `ostCount` (default 4), `stripeSize` (default 1048576), `cacheSize` (default 128 MB), `ttl` (default 30s).

## Future Work

- MDS separation — split metadata ops into a standalone component
- Distributed lock manager for multi-client cache coherency
- Parallel stripe I/O via thread pool
- Read-ahead prefetching for sequential access patterns
