package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"

	rand "math/rand/v2"

	"github.com/addrummond/jsockd/clients/go/jsockdclient"
)

func main() {
	nClientThreads := runtime.NumCPU()
	if val, ok := os.LookupEnv("NCLIENTTHREADS"); ok {
		n, err := strconv.Atoi(val)
		if err == nil {
			nClientThreads = n
		}
	}
	nServerThreads := 8
	if val, ok := os.LookupEnv("NSERVERTHREADS"); ok {
		n, err := strconv.Atoi(val)
		if err == nil {
			nServerThreads = n
		}
	}

	fmt.Fprintf(os.Stderr, "Using %v client threads and %v server threads\n", nClientThreads, nServerThreads)

	config := jsockdclient.DefaultConfig()
	config.NThreads = nServerThreads
	config.SkipJSockDVersionCheck = true
	config.MaxRestartsPerMinute = 10000
	config.TimeoutUs = 1000000 // 1 second

	// uncomment for debugging assistance
	// config.Logger = func(timestamp time.Time, level string, message string) {
	// 	fmt.Printf("[%s] %s: %s\n", timestamp.Format("2006-01-02 15:04:05"), level, message)
	// }

	client, err := jsockdclient.InitJSockDClient(config, os.Args[1])
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error initializing client: %v\n", err)
		os.Exit(1)
	}
	defer client.Close()

	var wg sync.WaitGroup

	var exitCode atomic.Int32

	for i := 0; i < nClientThreads; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < 10000; j++ {
				n := rand.IntN(1000)
				cmd := fmt.Sprintf("(_m, p) => { const x = p + %v; return JSockD.sendMessage(x) + 1; }", n)
				//fmt.Printf("Sending command: %v\n", cmd)
				resp, err := jsockdclient.SendCommandWithMessageHandler[int](client, cmd, n, func(msg int) (int, error) {
					return msg, nil
				})
				if err != nil {
					exitCode.Store(1)
					fmt.Fprintf(os.Stderr, "Error sending command: %v\n", err)
					//os.Exit(1)
					continue
				}
				if resp.Exception {
					exitCode.Store(1)
					fmt.Fprintf(os.Stderr, "Received exception: %v\n", resp.Exception)
				}
				if resp.Result != (2*n)+1 {
					exitCode.Store(1)
					fmt.Fprintf(os.Stderr, "Unexpected result for command (_m, p) => p + %v: got %v, expected %v\n", n, resp.Result, 2*n)
				}

				// TODO: separate test where we kill the process and test restarts
				/*if i == 1 && (j+1)%8 == 0 {
				fmt.Fprintf(os.Stderr, "\n\n****Killing JSockD\n\n")
				err := client.GetJSockDProcess().Kill()
				if err != nil {
					fmt.Fprintf(os.Stderr, "Error killing JSockD process: %v\n", err)
				}
				}*/
			}
		}()
	}

	wg.Wait()
	if ec := exitCode.Load(); ec != 0 {
		os.Exit(int(ec))
	}
}
