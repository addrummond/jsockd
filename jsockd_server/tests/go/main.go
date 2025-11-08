package main

import (
	"fmt"
	"os"
	"runtime"
	"sync"

	rand "math/rand/v2"

	"github.com/addrummond/jsockd/clients/go/jsockdclient"
)

func main() {
	config := jsockdclient.DefaultConfig()
	config.NThreads = 50
	config.SkipJSockDVersionCheck = true

	// uncomment for debugging assitance
	//config.Logger = func(timestamp time.Time, level string, message string) {
	//	fmt.Printf("[%s] %s: %s\n", timestamp.Format("2006-01-02 15:04:05"), level, message)
	//}

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

	var wg sync.WaitGroup

	for i := 0; i < runtime.NumCPU(); i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 10000; j++ {
				n := rand.IntN(1000)
				cmd := fmt.Sprintf("(_m, p) => p + %v", n)
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
				//fmt.Printf("All good!\n")
			}
		}()
	}

	wg.Wait()
}
