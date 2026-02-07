package driver

const (
	// DriverName is the CSI driver name registered with Kubernetes.
	DriverName = "stripefs.csi.example.com"

	// DriverVersion is the version of this CSI driver.
	DriverVersion = "0.1.0"

	// StripefsBinary is the path to the stripefs FUSE binary inside the container.
	StripefsBinary = "/usr/local/bin/stripefs"

	// DataRoot is where OST directories are created inside the container.
	DataRoot = "/data/volumes"

	// Default volume attributes.
	DefaultOSTCount   = "4"
	DefaultStripeSize = "1048576" // 1 MB
	DefaultCacheSize  = "128"    // MB
	DefaultTTL        = "30"     // seconds

	// Volume attribute keys (used in PV volumeAttributes).
	AttrOSTCount   = "ostCount"
	AttrStripeSize = "stripeSize"
	AttrCacheSize  = "cacheSize"
	AttrTTL        = "ttl"
)
