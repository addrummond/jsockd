# SSR with Go and React 19

This example demonstrates using the Go JSockD client for server-side rendering (SSR) of a simple React 19 component  The example includes setting up a basic HTTP server in Go that serves the rendered HTML and a client-side bundle to hydrate the React component on the client.

The example code is presented here via `cat <<END` commands for easy copy-pasting into a terminal, but you can also find the complete code in the
[`ssr_with_react_19_example_code`](https://github.com/addrummond/jsockd/tree/main/docs/ssr_with_react_19_example_code) directory.

## Creating the React project

Install the latest version of `jsockd` in your `PATH`.

Create project dir and install dependencies:

```sh
mkdir react-ssr-example && cd react-ssr-example
npm init -y
npm install react@19 react-dom@19 esbuild
```

Create the React component and the code to render it on the server:

```sh
cat > counter.jsx <<"END"
import React from "react";

export function Counter({ initialValue }) {
  const [count, setCount] = React.useState(initialValue);

  return (
    <div className="counter">
      <p>You clicked {count} times</p>
      <button onClick={() => setCount(count + 1)}>Click me</button>
    </div>
  );
}
END
```

```sh
cat > server.jsx <<"END"
import React from "react";
import { renderToString } from "react-dom/server.edge";
import { Counter } from "./counter.jsx";

export function renderCounter(props) {
  return renderToString(<Counter {...props} />);
}
END
```

## Bundling, compiling and signing

Create a signing key for the module bytecode:

```sh
jsockd -k mykey
```

Bundle the application using esbuild and then compile and sign the bytecode:

```sh
npx esbuild ./server.jsx --bundle --outfile=server-bundle.mjs --sourcemap --format=esm
jsockd -k mykey.privkey -c server-bundle.mjs server-bundle.quickjs_bytecode
```

Use the `-e` (eval) option of `jsockd` to test rendering without using a client:

```sh
export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat mykey.pubkey)
jsockd -m server-bundle.quickjs_bytecode -sm server-bundle.mjs.map -e "M.renderCounter({ initialValue: 42 })"
```

(`M` here is the global variable containing the loaded module that is initialized automatically when the `-e` option is used.)

## Setting up the server

```sh
go mod init react-ssr-example
go get github.com/addrummond/jsockd/clients/go/jsockdclient
```

The code for the server is as follows. **_Note that this code uses `InitJsockDClientViaAutoDownload`, which is not recommended for production use (use `InitJsockDClient` instead)._**

```sh
cat >main.go <<"END"
package main

import (
	"bytes"
	"fmt"
	"html/template"
	"net/http"

	"github.com/addrummond/jsockd/clients/go/jsockdclient"
)

const initialCounterValue = 99

const pageTemplate = `
<!DOCTYPE html>
<html lang="en">
<head>
<title>SSR example</title>
<script type="module" src="client-bundle.mjs"></script>
</head>
<body>
<h1>Server-side rendered counter</h1>
<div id="counter-container" data-props='{"initialValue": {{.InitialValue}}}'>{{.CounterHTML}}</div>
</body>
</html>
`

func main() {
	config := jsockdclient.DefaultConfig()
	config.BytecodeModuleFile = "server-bundle.quickjs_bytecode"
	config.SourceMap = "server-bundle.mjs.map"
	client, err := jsockdclient.InitJsockDClientViaAutoDownload(config)
	if err != nil {
		panic(err)
	}
	defer client.Close()

	t, err := template.New(pageTemplate).Parse(pageTemplate)
	if err != nil {
		panic(err)
	}

	http.HandleFunc("/client-bundle.mjs", func(w http.ResponseWriter, r *http.Request) {
		http.ServeFile(w, r, "client-bundle.mjs")
	})
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		response, err := jsockdclient.SendCommand[string](client, "(m, props) => m.renderCounter(props)", map[string]any{"initialValue": initialCounterValue})
		if err != nil {
			http.Error(w, "Internal Server Error", http.StatusInternalServerError)
			return
		}
		if response.Exception {
			http.Error(w, fmt.Sprintf("JavaScript Exception: %v", response.RawResponse.ResultJson), http.StatusInternalServerError)
		} else {
			var buf bytes.Buffer
			err = t.Execute(&buf, map[string]any{
				"CounterHTML":  template.HTML(response.Result),
				"InitialValue": initialCounterValue,
			})
			if err != nil {
				http.Error(w, "Internal Server Error", http.StatusInternalServerError)
				return
			}
			fmt.Fprintf(w, "%s", buf.Bytes())
		}
	})

	http.ListenAndServe(":8080", nil)
}
END
```

Create the client bundle:

```sh
cat >client.jsx <<"END"
import React from "react";
import { hydrateRoot } from "react-dom/client";
import { Counter } from "./counter.jsx";

function hydrate() {
  const counterElement = document.getElementById("counter-container");
  if (counterElement) {
    hydrateRoot(counterElement, <Counter {...JSON.parse(counterElement.dataset.props)} />);
    console.log("Counter hydrated");
  } else {
    console.error("Counter container not found");
  }
}

if (document.readyState === "loading")
  document.addEventListener("DOMContentLoaded", hydrate);
else hydrate();
END
```

```sh
npx esbuild ./client.jsx --bundle --outfile=client-bundle.mjs --sourcemap --format=esm
```

## Running the server

```sh
go run main.go
```

Connect to `http://localhost:8080` in your web browser. You should see the server-side rendered counter with an initial value of 99. Clicking the button will increment the counter, demonstrating that the React component has been successfully hydrated on the client side.
