# SSR with React 19 example code

To run:

* Ensure you have Go and Node.js installed.
* Ensure that the latest version of `jsockd` is in your `PATH`.
* Run `./run.sh` to start the server.

If you modify the JS code, run

```sh
npx esbuild ./server.jsx --bundle --outfile=server-bundle.mjs --sourcemap --format=esm
npx esbuild ./client.jsx --bundle --outfile=client-bundle.mjs --sourcemap --format=esm
```

and then restart the server:

```sh
go run main.go
```
