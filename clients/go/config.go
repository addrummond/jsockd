package jsockdclient

import (
	"runtime"
	"time"
)

// The configuration for the JSockDClient. The zero value is not a sensible
// configuration, so use DefaultConfig() and then modify the desired fields.
type Config struct {
	// The number of threads that JSockD should use. If 0, the number of CPU cores is used.
	NThreads int
	// The filename of the bytecode module to load, or "" if no bytecode module should be loaded.
	BytecodeModuleFile string
	// The hex-encoded public key used to verify the signature of the bytecode module, or "" if none.
	BytecodeModulePublicKey string
	// The filename of the source map to load, or "" if no source map should be loaded.
	SourceMap string
	// The maximum time in microseconds a connection is allowed to be idle before JSockD shuts down its associated QuickJS instance. If 0, no idle timeout is applied.
	MaxIdleTimeUs int
	// The maximum time in microseconds a command is allowed to run
	MaxCommandRuntimeUs int
	// The timeout in microseconds when communicating with JSockD
	TimeoutUs int
	// If true, the JSockD server version is not checked against the version expected by this client
	SkipJSockDVersionCheck bool
	// If set, this function is called for each log message sent by JSockD.
	Logger func(timestamp time.Time, level string, message string)
}

// DefaultConfig returns the default JSockD client configuration.
func DefaultConfig() Config {
	return Config{
		NThreads:                runtime.NumCPU(),
		BytecodeModuleFile:      "",
		BytecodeModulePublicKey: "",
		SourceMap:               "",
		MaxIdleTimeUs:           -1,
		MaxCommandRuntimeUs:     -1,
		TimeoutUs:               15000000, // 15 seconds
	}
}
