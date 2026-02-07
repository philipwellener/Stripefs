package driver

import (
	"context"

	"github.com/container-storage-interface/spec/lib/go/csi"
	"k8s.io/klog/v2"
)

// IdentityServer implements the CSI Identity service.
type IdentityServer struct {
	csi.UnimplementedIdentityServer
	name    string
	version string
}

func (s *IdentityServer) GetPluginInfo(
	_ context.Context,
	_ *csi.GetPluginInfoRequest,
) (*csi.GetPluginInfoResponse, error) {
	klog.V(4).Info("GetPluginInfo called")
	return &csi.GetPluginInfoResponse{
		Name:          s.name,
		VendorVersion: s.version,
	}, nil
}

func (s *IdentityServer) GetPluginCapabilities(
	_ context.Context,
	_ *csi.GetPluginCapabilitiesRequest,
) (*csi.GetPluginCapabilitiesResponse, error) {
	klog.V(4).Info("GetPluginCapabilities called")
	// No Controller service — FUSE mounts are node-local.
	return &csi.GetPluginCapabilitiesResponse{
		Capabilities: []*csi.PluginCapability{},
	}, nil
}

func (s *IdentityServer) Probe(
	_ context.Context,
	_ *csi.ProbeRequest,
) (*csi.ProbeResponse, error) {
	klog.V(4).Info("Probe called")
	return &csi.ProbeResponse{}, nil
}
