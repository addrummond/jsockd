package main

import (
	"fmt"
	"os"

	"github.com/addrummond/jsockd/clients/go/jsockdclient"
)

func main() {
	config := jsockdclient.DefaultConfig()
	config.NThreads = 50
	config.SkipJSockDVersionCheck = true

	socketNames := make([]string, config.NThreads)
	for i := range config.NThreads {
		socketNames[i] = fmt.Sprintf("/tmp/jsockd.%d.sock", i)
	}

	client, err := jsockdclient.InitJSockDClient(config, os.Args[1], socketNames)
	if client.Err != nil {

	}
}
