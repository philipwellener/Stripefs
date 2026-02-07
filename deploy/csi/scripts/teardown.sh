#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CSI_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MANIFEST_DIR="$CSI_DIR/manifests"

echo "=== stripefs CSI Driver Teardown ==="
echo ""

# Delete in reverse order of creation.
echo "Deleting test pod..."
kubectl delete -f "$MANIFEST_DIR/example-pod.yaml" --ignore-not-found

echo "Deleting PersistentVolumeClaim..."
kubectl delete -f "$MANIFEST_DIR/example-pvc.yaml" --ignore-not-found

echo "Deleting PersistentVolume..."
kubectl delete -f "$MANIFEST_DIR/example-pv.yaml" --ignore-not-found

echo "Deleting StorageClass..."
kubectl delete -f "$MANIFEST_DIR/storageclass.yaml" --ignore-not-found

echo "Deleting DaemonSet..."
kubectl delete -f "$MANIFEST_DIR/daemonset.yaml" --ignore-not-found

echo "Deleting RBAC..."
kubectl delete -f "$MANIFEST_DIR/rbac.yaml" --ignore-not-found

echo "Deleting CSIDriver..."
kubectl delete -f "$MANIFEST_DIR/csi-driver.yaml" --ignore-not-found

echo ""
echo "=== Teardown Complete ==="
