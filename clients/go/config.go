package jsockdclient

import (
	"runtime"
	"time"
)

type Config struct {
	NThreads                int
	BytecodeModuleFile      string
	BytecodeModulePublicKey string
	JSockDExec              string
	SourceMap               string
	MaxIdleTimeUs           int
	MaxCommandRuntimeUs     int
	UseFilCWhenAvailable    bool
	TimeoutUs               int
}

func DefaultConfig() Config {
	return Config{
		NThreads:                runtime.NumCPU(),
		BytecodeModuleFile:      "",
		BytecodeModulePublicKey: "",
		JSockDExec:              "",
		SourceMap:               "",
		MaxIdleTimeUs:           -1,
		MaxCommandRuntimeUs:     -1,
		UseFilCWhenAvailable:    false,
		TimeoutUs:               int(15 * time.Microsecond),
	}
}
