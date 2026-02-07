#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CSI_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_ROOT="$(cd "$CSI_DIR/../.." && pwd)"
MANIFEST_DIR="$CSI_DIR/manifests"
IMAGE_NAME="stripefs-csi-driver:latest"

echo "=== stripefs CSI Driver Deployment ==="
echo "Project root: $PROJECT_ROOT"
echo "CSI dir:      $CSI_DIR"
echo ""

# 1. Ensure minikube is running.
if ! minikube status &>/dev/null; then
    echo "Starting minikube..."
    minikube start --driver=docker
fi
echo "Minikube is running."

# 2. Build the image inside minikube's Docker daemon.
echo ""
echo "Building CSI driver image inside minikube..."
eval $(minikube docker-env)
docker build -t "$IMAGE_NAME" -f "$CSI_DIR/Dockerfile" "$PROJECT_ROOT"
echo "Image built: $IMAGE_NAME"

# 3. Apply Kubernetes manifests in order.
echo ""
echo "Applying Kubernetes manifests..."

echo "  CSIDriver..."
kubectl apply -f "$MANIFEST_DIR/csi-driver.yaml"

echo "  RBAC..."
kubectl apply -f "$MANIFEST_DIR/rbac.yaml"

echo "  DaemonSet..."
kubectl apply -f "$MANIFEST_DIR/daemonset.yaml"

echo "  StorageClass..."
kubectl apply -f "$MANIFEST_DIR/storageclass.yaml"

echo "  PersistentVolume..."
kubectl apply -f "$MANIFEST_DIR/example-pv.yaml"

echo "  PersistentVolumeClaim..."
kubectl apply -f "$MANIFEST_DIR/example-pvc.yaml"

# 4. Wait for DaemonSet rollout.
echo ""
echo "Waiting for DaemonSet rollout..."
kubectl -n kube-system rollout status daemonset/stripefs-csi-driver --timeout=120s

# 5. Wait for PVC to bind.
echo "Waiting for PVC to bind..."
for i in $(seq 1 30); do
    STATUS=$(kubectl get pvc stripefs-pvc -o jsonpath='{.status.phase}' 2>/dev/null || echo "Pending")
    if [ "$STATUS" = "Bound" ]; then
        break
    fi
    sleep 2
done

echo ""
echo "  PersistentVolume status:"
kubectl get pv stripefs-pv
echo ""
echo "  PersistentVolumeClaim status:"
kubectl get pvc stripefs-pvc

# 6. Deploy the test pod.
echo ""
echo "  Test pod..."
kubectl apply -f "$MANIFEST_DIR/example-pod.yaml"

echo ""
echo "Waiting for test pod to be running..."
kubectl wait --for=condition=Ready pod/stripefs-test-pod --timeout=120s

echo ""
echo "=== Deployment Complete ==="
echo ""
echo "Verify with:"
echo "  kubectl get csidrivers"
echo "  kubectl -n kube-system get pods -l app=stripefs-csi-driver"
echo "  kubectl get pv,pvc"
echo "  kubectl exec stripefs-test-pod -- bash -c 'echo hello > /mnt/striped/test.txt && cat /mnt/striped/test.txt'"
