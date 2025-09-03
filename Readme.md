# JSockD

JSockD provides a lightweight multi-threaded server for executing parameterized JavaScript commands.
The server receives commands over one or more UNIX domain sockets. The motivating use case is server-side rendering of React components.

- Uses the lightweight [QuickJS](https://bellard.org/quickjs/) JavaScript engine
- Loads precompiled bytecode for fast startup and execution
- Caches command bytecode
- Low memory usage
- Valgrind tests, fuzz tests
- Memory-safe [Fil-C](https://github.com/pizlonator/llvm-project-deluge/) build available on Linux/x86_64 for the safety conscious.

## 1. Getting started

### 1.1 Adding JSockD to your application

Applications should generally connect to JSockD via a client library that manages the JSockD server process. At the moment, this repo contains one example of such a library in `clients/elixir/jsockd_client`. At this level of abstraction, JSockD is a simple command execution service.
Steps to add JSockD to your application:

* Bundle all of the required Javascript library code into a single ES6 module (using e.g. [esbuild](https://esbuild.github.io/api/)).
* Generate an ED25519 public/private key pair for signing your code (see section 2).
* Compile the ES6 module into a QuickJS bytecode file using `jsockd -c` command (see section 2).
* Configure your client library with the path to the bytecode file and the public key used to sign it.
* Use the client library to send commands to the JSockD server.

### 1.2 What is a JSockD command?

A JSockD command is a JavaScript function that takes two arguments:

* the module loaded from your bundle (or `undefined` if no bundle was specified), and
* the parameter passed to the command.

The parameter can be any JSON-serializable value. A command should return either a value that can be serialized to JSON or a promise that resolves to such a value.

Commands are cached in the same sort of way that a SQL server caches queries. When a command is executed, the server first checks if the command has been executed before. If it has, the server executes the cached bytecode for that command with the specified parameter. If not, the server first compiles the command and caches the bytecode for future use.

Commands should not mutate global state. Global state may or may not persist across command executions. JSockD reserves the right to reset global state at any time.

The following is a typical example of a command in the context of React SSR. The bundled module exports some key React functions such as `renderToString` and `createElement`. The parameter supplies one of the props for the `UserDetails` component.

```javascript
(mod, userDetails) => {
  return mod.renderToString(
    mod.createElement(mod.UserDetails, { size: 'small', userDetails })
  )
}
````

### 1.2 Tips for SSR with React 19

**TODO: rough notes**

- Import `react-dom/server.edge` rather than `react-dom/server`.

## 2. The module compiler

### 2.1 Signing and compiling modules

JSockD provides a command-line tool for compiling ES6 modules into QuickJS bytecode files.
Bytecode must be signed using an ED25519 key to ensure that only trusted code is executed
by the server.

Generate a signing key as follows:

```sh
jsockd -k my_key_file
```

This command generates two files, `my_key_file.pubkey` (the public key) and `my_key_file.privkey` (the private key).

We can now compile an ES6 module to QuickJS bytecode and sign the bytecode using the key:

```sh
jsockd -c my_module.mjs my_module.quickjs_bytecode -k my_key_file.privkey
```

The public key is passed to the `jsockd` server process via the `JSOCKD_BYTECODE_MODULE_PUBLIC_KEY` environment variable:

```sh
export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat my_key_file.pubkey)
```

### 2.2 [optional] Using openssl to sign bytecode

_**On Mac you may need to install openssl via homebrew to get support for ED25519 signatures.**_

If you don't trust JSockD to generate keys and signatures, you can use openssl to sign your module bytecode.
Generate public and private keys as follows:

```sh
openssl genpkey -algorithm ed25519 -out private_signing_key.pem
openssl pkey -inform pem -pubout -outform der -in private_signing_key.pem | tail -c 32 | xxd -p | tr -d '\n' > public_signing_key_hex
```

Now compile the module without a key:

```sh
jsockd -c my_module.mjs my_module.quickjs_bytecode
```

The last 64 bytes of the bytecode file (usually occupied by the ED25519 signature) are now filled with zeros. The preceding 128 bytes contain the jsockd compiler version string. You therefore need to sign the file **excluding the last 192 bytes** and then replace the last 64 bytes of the file with the signature. The following shell one-liner accomplishes this:

```sh
BYTECODE_FILE=my_module.quickjs_bytecode BYTECODE_FILE_SIZE=$(wc -c $BYTECODE_FILE | awk '{print $1}') && ( head -c $(($BYTECODE_FILE_SIZE - 192)) $BYTECODE_FILE | openssl pkeyutl -sign -inkey private_signing_key.pem -rawin -in /dev/stdin | dd of=$BYTECODE_FILE bs=1 seek=$(($BYTECODE_FILE_SIZE - 64)) conv=notrunc )
```

Finally, set the environment variable to the hex-encoded public key before running jsockd:

```sh
export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat public_signing_key_hex)
jsockd -m my_module.quickjs_bytecode -s /tmp/sock
```

## 3. The JSockD server

### 3.1 Starting the server

**JSockD is not intended to run as a standalone server; it is designed to run as a subprocess of an application that needs to execute JavaScript commands. This section is intended as a reference for developers implementing JSockD client libraries.**

The server is started as follows:

```sh
export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=xxxxx # see previous section
jsockd -m <es6_module_bytecode_file> -s <socket1_path> [<socket2_path> ...]
```

The `-m` argument is the path to a precompiled ES6 module bytecode file. This module should contain all of the library code that you want commands to be able to reference. In many cases it will be a bundle generated by a build tool such as esbuild.

When the server is ready to start accepting commands on the specified UNIX domain sockets, it prints `READY <N>` to the standard output followed by `\n`. The integer N specifies the number of threads that the server is using to process commands. This may be less than the number of sockets specified, in which case only the first N sockets will be used for command processing.

### 3.2 Command line options

#### Get version

```sh
jsockd -v
```

#### Generate public and private keys

```sh
jsockd -k <key_file_prefix>
```

Outputs two files: `<key_file_prefix>.pub` (the public key) and `<key_file_prefix>.pem` (the private key).

#### Compile a module file

```sh
jsockd -c <module_file> <output_bytecode_file> [-k <private_key_file>]
```

Compiles the specified ES6 module file to a QuickJS bytecode file. If the `-k` option is not given, the module is not signed. Unsigned modules can be used only by debug builds of `jsockd` when the `JSOCKD_BYTECODE_MODULE_PUBLIC_KEY` env var is set to `dangerously_allow_invalid_signatures`.

#### Start the server

| Option      | Argument(s)                | Description                                                                  | Default       | Repeatable | Required |
|-------------|----------------------------|------------------------------------------------------------------------------|---------------|------------|----------|
| `-m`        | `<module_bytecode_file>`   | Path to ES6 module bytecode file.                                            |               | No         | No       |
| `-sm`       | `<source_map_file>`        | Path to source map file (e.g. `foo.js.map`). Can only be used with `-m`.     |               | No         | No       |
| `-t`        | `<microseconds>`           | Maximum command runtime in microseconds (must be integer > 0).               | 250000        | No         | No       |
| `-b`        | `<XX>`                     | Separator byte as two hex digits (e.g. `0A`).                                | `0A` (= `\n`) | No         | No       |
| `-s`        | `<socket1> [socket2 ...]`  | One or more socket file paths.                                               |               | Yes        | Yes      |
| `--`        | *(none)*                   | Indicates end of options for `-s` (allows socket paths starting with `-`).   |               | N/A        | No       |

### 3.3 The socket protocol

The server listens for commands on the specified UNIX domain sockets. Each command consists of three fields separated
by a separator byte:

```
<unique command ID>
----- separator byte -----
(module, param) => { ... }
----- separator byte -----
<JSON-encoded parameter>
----- separator byte -----
```

The separator byte is `\n` by default. It can be changed using the `-b XX` command line option, where `XX` is a two-digit hexadecimal value.
A useful value is `00`, as the null byte cannot be present in valid JSON or JavaScript
(assuming UTF-8 encoding).

A unique command ID is any non-empty sequence of 32 or fewer bytes that does not contain a space character or a separator byte.

The second field, the command, is a JavaScript expression evaluating to a function. The function is called with two arguments.
The first is the module that was loaded from the bytecode file (or `undefined` if none was given); the second is the parameter passed as the third field.

The third field is the JSON-encoded parameter value.

The server responds with a single line (terminated with `\n`) consisting of the command ID followed by a space and then either
* the JSON-encoded result of the command, if successful; or
* the string `exception` followed by a space and then a JSON-encoded error message and backtrace (see next subsection).

As the protocol is synchronous, command IDs are not strictly necessary. However, it is recommended to check that responses have the expected
command ID as a means of ensuring that the client code is working correctly.

The client may send either of the following commands at any point, terminated by the separator byte:

```
?reset
?quit
```

The `?reset` command resets the server's command parser to its initial state (so that it expects the next field to be a unique command ID).

The `?quit` command causes the server to exit immediately (closing all sockets, not just the socket on which the command was sent).

### 3.4 Error message and backtrace format

```javascript
{
  "raw": "..." // the raw QuickJS error message + stack trace
  "pretty": "..." // The formatted backtrace with source map info (if sourcemap provided)
  "errorMessage": "Error: foo", // the error message
  "trace": [
     {
        "functionName": "flubFoo", // the function containing this line in the backtrace
        "source": "foo.js",        // the source file (null if unavailable)
        "line": 1,                 // the line number (null if unavailable)
        "column": 26,              // the column number (null if unavailable)
        "mapped": {                // null if no source map available
          // the fields above for the function/location in the original source file
        }
     },
     ...
  ]
}
```

### 3.5 Source maps

JSockD supports source maps for error backtrace reporting. Use the `-sm <source_map.js.map>`
command line option to specify the path to a source map file for the bundle.

When a source map is provided, each entry in the `"trace"` array (see previous section) includes a `"mapped"` property with `"functionName"`, `"source"`, `"line"`, and `"column"` properties. These properties correspond to the original source code location of the error, as determined by the source map.

It is recommended to specify a source map only for development and testing purposes, as the code for computing source mapped back traces is not optimized for performance. As long as you have a source map for your bundle, you always have the option of manually resolving the backtrace entries when looking at errors in production.

## 4. Bundling your JavaScript code

JSockD can be used with any bundler that can output an ES6 module (or with no bundler at all if your JS code is contained in a single file). The following is an example of how to bundle you code using [esbuild](https://esbuild.github.io/). The `root_module.mjs` module should contain all the code that you want to execute in the JSockD server. It can import other modules as needed.=

```
esbuild root_module.mjs --bundle --outfile=bundle.mjs --sourcemap --format=esm
```

Example `root_module.mjs` contents:

```javascript
import { flubBar } from "./library1"
import { blagFoo } from "./library2"

export { flubBar, blagFoo }
````

## 5. Building from source

To build JSockD from source, you must first build QuickJS and then the JS server.

Tools necessary for the build are listed in `.tool-versions` and can be installed using [mise-en-place](https://mise.jdx.dev/) or [asdf](https://asdf-vm.com/).

### 5.1 Building QuickJS

QuickJS is built by running `./build_quickjs.sh`. This script downloads and builds the QuickJS library. The QuickJS build is kept separate from the main JSockD build because it needs to be run only once, and the QuickJS build system is a bit finicky to configure for different environments.

On systems where `make` is a non-GNU Make, set the `MAKE` env var to `gmake` when running the script.

### 5.2 Building the JS server

The JS server is built using CMake 4. The `mk.sh` wrapper script invokes CMake with the correct arguments for common use cases.

Run the following commands from within the `jsockd_server` directory:

```sh
./mk.sh Debug # Debug build
./mk.sh Release # Release build
```

The resulting `jsockd` binary can be found in the `build_Release` or `build_Debug` directory. As a convenience, the `mk.sh` script can also run the executable after compilation succeeds:

```sh
./mk.sh Debug run arg1 arg2 ...
```

Run unit tests can be as follows:

```sh
./mk.sh Debug test
```

### 5.3 Code formatting

The `format.sh` script in `jsockd_server` formats C source files using `clang-format`. Run `npm i` to install the appropriate version of `clang-format`.

### 5.4 Developing with Fil-C

**TODO: rough notes**

* `./build_quickjs.sh linux_x86_64_filc`
* [In `jsockd_server`] `TOOLCHAIN_FILE=TC-fil-c.cmake ./mk.sh Debug`

## 6. Releases

### 6.1 Creating a release

Pre-built binaries are available for download from the [GitHub releases page](https://github.com/addrummond/jsockd/releases). The following platforms are supported:

- Linux x86_64
- Linux ARM64
- macOS ARM64 (Apple Silicon)

To create a new release:

1. Tag the commit with a version tag starting with 'v' (e.g., `v1.0.0`)
2. Push the tag to GitHub: `git push origin v1.0.0`
3. The release workflow will automatically build binaries and create a GitHub release

Each release includes:

- Platform-specific tar.gz archives containing the `jsockd` binary
- SHA256 checksums for verification

### 6.2 New version checklist

* Update `@jsockd_version` in `clients/elixir/jsockd_client/mix.exs`.
* Update the example `deps` entry in `clients/elixir/jsockd_client/README.md`.
* Push to `main`.
* Tag with the version number and push the tag.
