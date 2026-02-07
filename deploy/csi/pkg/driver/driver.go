package driver

import (
	"context"
	"fmt"
	"net"
	"net/url"
	"os"
	"sync"

	"github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc"
	"k8s.io/klog/v2"
)

// Driver implements the CSI Identity and Node services for stripefs.
type Driver struct {
	name     string
	version  string
	nodeID   string
	endpoint string

	srv *grpc.Server
	mu  sync.Mutex
}

// NewDriver creates a new CSI driver instance.
func NewDriver(nodeID, endpoint string) *Driver {
	return &Driver{
		name:     DriverName,
		version:  DriverVersion,
		nodeID:   nodeID,
		endpoint: endpoint,
	}
}

// Run starts the gRPC server and blocks until stopped.
func (d *Driver) Run() error {
	u, err := url.Parse(d.endpoint)
	if err != nil {
		return fmt.Errorf("parsing endpoint %q: %w", d.endpoint, err)
	}

	var addr string
	if u.Scheme == "unix" {
		addr = u.Path
		if err := os.Remove(addr); err != nil && !os.IsNotExist(err) {
			return fmt.Errorf("removing existing socket %q: %w", addr, err)
		}
	} else {
		return fmt.Errorf("unsupported endpoint scheme %q (only unix:// is supported)", u.Scheme)
	}

	listener, err := net.Listen("unix", addr)
	if err != nil {
		return fmt.Errorf("listening on %q: %w", addr, err)
	}

	d.mu.Lock()
	d.srv = grpc.NewServer(grpc.UnaryInterceptor(logInterceptor))
	csi.RegisterIdentityServer(d.srv, &IdentityServer{
		name:    d.name,
		version: d.version,
	})
	csi.RegisterNodeServer(d.srv, &NodeServer{
		nodeID: d.nodeID,
	})
	d.mu.Unlock()

	klog.Infof("CSI driver %s %s listening on %s", d.name, d.version, d.endpoint)
	return d.srv.Serve(listener)
}

// Stop gracefully stops the gRPC server.
func (d *Driver) Stop() {
	d.mu.Lock()
	defer d.mu.Unlock()
	if d.srv != nil {
		klog.Info("Stopping CSI driver")
		d.srv.GracefulStop()
	}
}

// logInterceptor logs every gRPC call for debugging.
func logInterceptor(
	ctx context.Context,
	req interface{},
	info *grpc.UnaryServerInfo,
	handler grpc.UnaryHandler,
) (interface{}, error) {
	klog.V(2).Infof("gRPC call: %s", info.FullMethod)
	resp, err := handler(ctx, req)
	if err != nil {
		klog.Errorf("gRPC error: %s: %v", info.FullMethod, err)
	}
	return resp, err
}
