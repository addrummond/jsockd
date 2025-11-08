package main

import (
	"fmt"
	"os"

	rand "math/rand/v2"

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

	client, err := jsockdclient.InitJSockDClient(config, os.Args[1])
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error initializing client: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()

	for i := 0; i < 100000; i++ {
		n := rand.IntN(1000)
		cmd := fmt.Sprintf("(_m, p) => p + %v", n)
		fmt.Printf("Sending command %s with param %v\n", cmd, n)
		resp, err := jsockdclient.SendCommand[int](client, cmd, n)
		if err != nil {
			fmt.Fprintf(os.Stderr, "Error sending command: %v\n", err)
			os.Exit(1)
		}
		if resp.Exception {
			fmt.Fprintf(os.Stderr, "Received exception: %v\n", resp.Exception)
			os.Exit(1)
		}
		if resp.Result != 2*n {
			fmt.Fprintf(os.Stderr, "Unexpected result for command (_m, p) => p + %v: got %v, expected %v\n", n, resp.Result, 2*n)
			os.Exit(1)
		}
	}
}
