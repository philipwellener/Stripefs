package main

import (
	"flag"
	"os"
	"os/signal"
	"syscall"

	"github.com/stripefs/csi-driver/pkg/driver"
	"k8s.io/klog/v2"
)

func main() {
	var (
		endpoint = flag.String("endpoint", "unix:///csi/csi.sock", "CSI gRPC endpoint")
		nodeID   = flag.String("node-id", "", "Kubernetes node name (defaults to NODE_NAME env)")
	)

	klog.InitFlags(nil)
	flag.Parse()

	if *nodeID == "" {
		*nodeID = os.Getenv("NODE_NAME")
	}
	if *nodeID == "" {
		klog.Fatal("--node-id or NODE_NAME environment variable is required")
	}

	klog.Infof("Starting stripefs CSI driver (node=%s, endpoint=%s)", *nodeID, *endpoint)

	d := driver.NewDriver(*nodeID, *endpoint)

	// Handle shutdown signals.
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		sig := <-sigCh
		klog.Infof("Received signal %v, shutting down", sig)
		d.Stop()
	}()

	if err := d.Run(); err != nil {
		klog.Fatalf("Failed to run driver: %v", err)
	}
}
