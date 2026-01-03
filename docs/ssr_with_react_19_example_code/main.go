package main

import (
	"bytes"
	"fmt"
	"html/template"
	"net/http"
	"os"

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

	fmt.Fprintf(os.Stderr, "Starting server on :8080\n")
	http.ListenAndServe(":8080", nil)
}
