package driver

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"time"

	"github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"k8s.io/klog/v2"
	"k8s.io/mount-utils"
)

// NodeServer implements the CSI Node service for stripefs FUSE mounts.
type NodeServer struct {
	csi.UnimplementedNodeServer
	nodeID string
}

func (s *NodeServer) NodePublishVolume(
	_ context.Context,
	req *csi.NodePublishVolumeRequest,
) (*csi.NodePublishVolumeResponse, error) {
	volumeID := req.GetVolumeId()
	targetPath := req.GetTargetPath()

	if volumeID == "" {
		return nil, status.Error(codes.InvalidArgument, "volume ID is required")
	}
	if targetPath == "" {
		return nil, status.Error(codes.InvalidArgument, "target path is required")
	}

	klog.Infof("NodePublishVolume: volume=%s target=%s", volumeID, targetPath)

	// Check idempotency — if already mounted, return OK.
	mounter := mount.New("")
	mounted, err := isMounted(mounter, targetPath)
	if err != nil {
		return nil, status.Errorf(codes.Internal, "checking mount status: %v", err)
	}
	if mounted {
		klog.Infof("NodePublishVolume: %s already mounted, returning OK", targetPath)
		return &csi.NodePublishVolumeResponse{}, nil
	}

	// Read volume attributes with defaults.
	attrs := req.GetVolumeContext()
	ostCount := attrOrDefault(attrs, AttrOSTCount, DefaultOSTCount)
	stripeSize := attrOrDefault(attrs, AttrStripeSize, DefaultStripeSize)
	cacheSize := attrOrDefault(attrs, AttrCacheSize, DefaultCacheSize)
	ttl := attrOrDefault(attrs, AttrTTL, DefaultTTL)

	ostCountInt, err := strconv.Atoi(ostCount)
	if err != nil || ostCountInt < 1 {
		return nil, status.Errorf(codes.InvalidArgument, "invalid ostCount %q", ostCount)
	}

	// Create OST directories.
	volumeDataDir := filepath.Join(DataRoot, volumeID)
	ostPaths := make([]string, ostCountInt)
	for i := 0; i < ostCountInt; i++ {
		ostPaths[i] = filepath.Join(volumeDataDir, fmt.Sprintf("ost%d", i))
		if err := os.MkdirAll(ostPaths[i], 0755); err != nil {
			return nil, status.Errorf(codes.Internal, "creating OST dir %s: %v", ostPaths[i], err)
		}
	}

	// Create target mount directory.
	if err := os.MkdirAll(targetPath, 0755); err != nil {
		return nil, status.Errorf(codes.Internal, "creating target dir %s: %v", targetPath, err)
	}

	// Build stripefs command.
	args := []string{
		"--mount", targetPath,
		"--stripe-size", stripeSize,
		"--cache-size", cacheSize,
		"--ttl", ttl,
		"--foreground",
	}
	for _, ost := range ostPaths {
		args = append(args, "--ost", ost)
	}

	klog.Infof("NodePublishVolume: launching %s %v", StripefsBinary, args)

	cmd := exec.Command(StripefsBinary, args...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr

	if err := cmd.Start(); err != nil {
		return nil, status.Errorf(codes.Internal, "starting stripefs: %v", err)
	}

	klog.Infof("NodePublishVolume: stripefs PID %d", cmd.Process.Pid)

	// Poll for mount to appear (up to 5 seconds).
	if err := waitForMount(mounter, targetPath, 5*time.Second); err != nil {
		// Kill the process if mount never appeared.
		_ = cmd.Process.Kill()
		return nil, status.Errorf(codes.Internal, "stripefs mount did not appear at %s: %v", targetPath, err)
	}

	klog.Infof("NodePublishVolume: mount confirmed at %s", targetPath)
	return &csi.NodePublishVolumeResponse{}, nil
}

func (s *NodeServer) NodeUnpublishVolume(
	_ context.Context,
	req *csi.NodeUnpublishVolumeRequest,
) (*csi.NodeUnpublishVolumeResponse, error) {
	targetPath := req.GetTargetPath()
	if targetPath == "" {
		return nil, status.Error(codes.InvalidArgument, "target path is required")
	}

	klog.Infof("NodeUnpublishVolume: target=%s", targetPath)

	// Attempt fusermount3 -u first, fall back to umount.
	if err := unmount(targetPath); err != nil {
		return nil, status.Errorf(codes.Internal, "unmounting %s: %v", targetPath, err)
	}

	// Clean up the mount directory.
	if err := os.Remove(targetPath); err != nil && !os.IsNotExist(err) {
		klog.Warningf("NodeUnpublishVolume: failed to remove %s: %v", targetPath, err)
	}

	klog.Infof("NodeUnpublishVolume: unmounted %s", targetPath)
	return &csi.NodeUnpublishVolumeResponse{}, nil
}

func (s *NodeServer) NodeGetCapabilities(
	_ context.Context,
	_ *csi.NodeGetCapabilitiesRequest,
) (*csi.NodeGetCapabilitiesResponse, error) {
	return &csi.NodeGetCapabilitiesResponse{
		Capabilities: []*csi.NodeServiceCapability{},
	}, nil
}

func (s *NodeServer) NodeGetInfo(
	_ context.Context,
	_ *csi.NodeGetInfoRequest,
) (*csi.NodeGetInfoResponse, error) {
	return &csi.NodeGetInfoResponse{
		NodeId: s.nodeID,
	}, nil
}

// --- Helpers ---

func attrOrDefault(attrs map[string]string, key, defaultVal string) string {
	if v, ok := attrs[key]; ok && v != "" {
		return v
	}
	return defaultVal
}

func isMounted(mounter mount.Interface, path string) (bool, error) {
	// If path doesn't exist, it's not mounted.
	if _, err := os.Stat(path); os.IsNotExist(err) {
		return false, nil
	}

	mountPoints, err := mounter.List()
	if err != nil {
		return false, fmt.Errorf("listing mounts: %w", err)
	}
	for _, mp := range mountPoints {
		if mp.Path == path {
			return true, nil
		}
	}
	return false, nil
}

func waitForMount(mounter mount.Interface, path string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		mounted, err := isMounted(mounter, path)
		if err != nil {
			return err
		}
		if mounted {
			return nil
		}
		time.Sleep(200 * time.Millisecond)
	}
	return fmt.Errorf("timed out after %s", timeout)
}

func unmount(path string) error {
	// Try fusermount3 first.
	out, err := exec.Command("fusermount3", "-u", path).CombinedOutput()
	if err == nil {
		return nil
	}
	klog.V(2).Infof("fusermount3 failed: %s (trying umount)", string(out))

	// Fall back to umount.
	out, err = exec.Command("umount", path).CombinedOutput()
	if err != nil {
		return fmt.Errorf("umount failed: %s: %w", string(out), err)
	}
	return nil
}
