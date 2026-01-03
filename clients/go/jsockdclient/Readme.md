# Go JSockD client

## Add JSockD to your Go project

```sh
go get github.com/addrummond/jsockd/clients/go/jsockdclient
```

## Example usage

```go
package main
import (
	"fmt"
	"log"

	"github.com/addrummond/jsockd/clients/go/jsockdclient"
)

func main() {
	config := jsockdclient.DefaultConfig()
	config.SourceMap = "example_module.mjs.map"
	client, err := jsockdclient.InitJsockDClientViaAutoDownload(config)
	if err != nil {
		log.Fatalf("Failed to initialize JSockD client: %v", err)
	}
	defer client.Close()

	response, err := jsockdclient.SendCommand[string](client, "(_, name) => 'Hello ' + name", "World")
	if err != nil {
		log.Fatalf("Failed to send command: %v", err)
	}

	fmt.Println(response) // Should print: Hello, World!
}
```

## Example of SSR with React 19

See [this documentation](https://github.com/addrummond/jsockd/blob/main/docs/ssr_with_react_19.md)
and the [associated example code](https://github.com/addrummond/jsockd/tree/main/docs/ssr_with_react_19_example_code)
for an example of using JSockD for server-side rendering with React 19.
