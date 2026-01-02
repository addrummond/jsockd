# JSockD

JSockD provides a lightweight multi-threaded server for executing parameterized JavaScript commands.
The server receives commands over one or more UNIX domain sockets. The motivating use case is server-side rendering of React components.

- Uses the lightweight [QuickJS](https://bellard.org/quickjs/) JavaScript engine
- Elixir and Go client libraries included
- Runs on Linux, MacOS, FreeBSD, OpenBSD
- Multithreaded (one QuickJS engine per thread)
- Loads precompiled bytecode for fast startup and execution
- Caches command bytecode
- Guards against memory leaks in JS code
- Valgrind tests, fuzz tests
- Memory-safe [Fil-C](https://github.com/pizlonator/llvm-project-deluge/) build available on Linux/x86_64 for the safety conscious.

## 1. Getting started

### 1.1 Adding JSockD to your application

Applications should connect to JSockD via a client library that manages the JSockD server process. At the moment, this repo contains two examples of client libraries in `clients/elixir/jsockd_client` and `clients/go`.
Steps to add JSockD to your application:

* Bundle all of the required Javascript library code into a single ES6 module (using e.g. [esbuild](https://esbuild.github.io/api/)).
* Generate an ED25519 public/private key pair for signing your code (see section 2).
* Compile the ES6 module into a QuickJS bytecode file using the `jsockd -c` command (see section 2).
* Configure your client library with the path to the bytecode file and the public key used to sign it.
* Use the client library to send commands to the JSockD server.

---

**🚀 There's an example of using JSockD with React 19 in [`docs/ssr_with_react_19.md`](https://github.com/addrummond/jsockd/blob/main/docs/ssr_with_react_19.md). The example uses the Go client, but the essential steps are similar for other languages.🚀**

---

### 1.2 What is a JSockD command?

A JSockD command is a JavaScript function that takes two arguments:

* the module loaded from your bundle (or `undefined` if no bundle was specified), and
* the parameter passed to the command.

The parameter can be any JSON-serializable value. A command should return either a JSON-serializable value or a promise that resolves to such a value.

Commands are cached in the same sort of way that a SQL server caches queries. When a command is executed, the server first checks if the command has been executed before. If it has and the bytecode remains in the cache, then the server executes the cached bytecode with the specified parameter. If not, the server first compiles the command and caches the bytecode for future use.

Commands should not depend on persistent global state. Global state may or may not persist across command executions. JSockD reserves the right to reset global state at any time (except during command execution).

The following is a typical example of a command in the context of React SSR. The bundled module exports some key React functions such as `renderToString` and `createElement`. The parameter supplies one of the props for the `UserDetails` component.

```javascript
(mod, userDetails) => {
  return mod.renderToString(
    mod.createElement(mod.UserDetails, { size: 'small', userDetails })
  )
}
````

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

**_❗If you don't trust JSockD to generate keys and signatures, you can use openssl to sign your module bytecode.
See `docs/signing_with_openssl.md` for details.❗_**

### 2.3 Reducing the size of compiled modules

Minifying the module before compiling it will reduce the size of the compiled bytecode because the bytecode includes source information.

To further reduce bytecode size, you can use the `-ss` flag to strip source code from the bytecode file or the `-sd` flag to strip all debug info including source code. Note that stripping source code means that error backtraces will not include line numbers or source code snippets. It may also break some JavaScript code which relies on inspecting function source code.

For general use, I recommend minifiying your bundle but **not** using `-ss` or `-sd`. This way you can substantially reduce the size of the bytecode while still allowing useful error backtraces via source maps (see section 3.5).

## 3. The JSockD server

### 3.1 Starting the server

**_❗JSockD is not intended to run as a standalone server. It is designed to run as a subprocess of an application that needs to execute JavaScript commands. This section is intended as a reference for developers implementing JSockD client libraries.❗_**

The server is started as follows:

```sh
export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=xxxxx # see previous section
jsockd -m <es6_module_bytecode_file> -s <socket1_path> [<socket2_path> ...]
```

The `-m` argument is the path to a precompiled ES6 module bytecode file. This module should contain all of the library code that you want commands to be able to reference. In many cases it will be a bundle generated by a tool such as esbuild.

When the server is ready to start accepting commands on the specified UNIX domain sockets, it prints `READY <n> <jsockd_version>` to the standard output followed by `\n`. The integer n is ≥1 and specifies the number of threads that the server is using to process commands. This may be less than the number of sockets specified, in which case only the first N socket file arguments will be opened for command processing.

### 3.2 Command line options

#### Get version

```sh
jsockd -v
```

#### Generate public and private keys

```sh
jsockd -k <key_file_prefix>
```

Outputs two files: `<key_file_prefix>.pubkey` (the public key) and `<key_file_prefix>.privkey` (the private key).

#### Compile a module file

```sh
jsockd -c <module_file> <output_bytecode_file> [-k <private_key_file>] [-ss] [-sd]
```

Compiles the specified ES6 module file to a QuickJS bytecode file. If the `-k` option is not given, the module is not signed. Unsigned modules can be used only by debug builds of `jsockd` when the `JSOCKD_BYTECODE_MODULE_PUBLIC_KEY` env var is set to `dangerously_allow_invalid_signatures`.

The `-ss` and `-sd` options are mutually exclusive. Setting `-ss` strips all source code from the bytecode file, while `-sd` strips all debug info including source code.

#### Evaluate a Javascript expression

The `-e` option evaluates a JavaScript expression and prints the JSON-encoded result to standard output. If the argument ot `-e` is `-`, the expression is read from standard input.

If the `-m` option is given, then the global variable `M` is initialized to the specified module before evaluating the expression.

```sh
jsockd [-m <module_bytecode_file>] [-sm <source_map_file>] -e <javascript_expression>
jsockd [-m <module_bytecode_file>] [-sm <source_map_file>] -e -
```

Exit code is non-zero iff there is a parse error or an exception occurs during evaluation.

#### Run the server

```sh
jsockd -s <socket1> [<socket2> ...] [-m <module_bytecode_file>] [-sm <source_map_file>] [-t <microseconds>] [-i <microseconds>] [-b <XX>]
```

| Option      | Argument(s)                 | Description                                                                  | Default       | Repeatable | Required |
|-------------|-----------------------------|------------------------------------------------------------------------------|---------------|------------|----------|
| `-s`        | `<socket1> [<socket2> ...]` | One or more socket file paths. Use `-s -- <socket> ...` to permit file names beginning with `-`.                                              |               | Yes        | Yes      |
| `-m`        | `<module_bytecode_file>`    | Path to ES6 module bytecode file.                                            |               | No         | No       |
| `-sm`       | `<source_map_file>`         | Path to source map file (e.g. `foo.js.map`). Can only be used with `-m`.     |               | No         | No       |
| `-t`        | `<microseconds>`            | Maximum command runtime in microseconds (must be integer > 0).               | 250000        | No         | No       |
| `-i`        | `<microseconds>`            | Maximum time in microseconds that thread can remain idle before QuickJS runtime is shut down, or 0 for no idle timeout (must be integer ≥ 0). | 0             | No         | No       |
| `-b`        | `<XX>`                      | Separator byte as two hex digits (e.g. `0A`).                                | `0A` (= `\n`) | No         | No       |

### 3.3 Environment variables

* `JSOCKD_BYTECODE_MODULE_PUBLIC_KEY`: The hex-encoded ED25519 public key used to verify the signature of the module bytecode file specified with the `-m` option.
* `JSOCKD_LOG_PREFIX`: This string is prepended to all logged messages (unless it contains a carriage return or line feed, in which case it is ignored).

### 3.4 Error message and backtrace formats

```javascript
{
  "raw": "...", // the raw QuickJS error message + stack trace
  "pretty": "...", // The formatted backtrace with source map info (if sourcemap provided)
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

### 3.6 Load balancing

The client should request a number of sockets roughly in line with the number of avaiable CPU cores (or fewer if a light load is anticipated).

The `-i` command line option can be used to adjust the amount of time that a QuickJS runtime is permitted to remain idle before being shut down. By default, idle runtimes are never shut down. If `-i` is set to a non-zero value, then
if any socket except the first is unused for a significant period of time, the QuickJS runtime for that socket is shut down to free up memory. The next command received on that socket causes a new QuickJS runtime to be created. Thus the client may distribute commands over the first n sockets, with n rising and falling with increasing/decreasing load. A good rule of thumb is to route commands to socket n only if sockets 1..n-1 are all busy processing commands.

When an idle thread is woken, the typical time to initialize a new QuickJS runtime is on the order of a few milliseconds.

### 3.7 Server log format

When executed as a server (i.e. with the `-s` option), JSockD logs messages to standard error in the following format (all characters literal except `<VAR>` variables):

```
<PREFIX> jsockd <DATETIME> [<LOG_LEVEL>] <LOG MESSAGE>
```

The `<PREFIX>` is `$` for the last line of a log message and `*` for all other lines.

The `<DATETIME>` is in ISO 8601 format (or is the string `0000-00-00T00:00:00.000000Z` in the unlikely event of a failed call to `clock_gettime`).

The `<LOG_LEVEL>` is one of `INFO`, `WARN`, or `ERROR` in release builds, or also `DEBUG` in debug builds.

The `<DATETIME>` and `<LOG_LEVEL>` fields are guaranteed to be identical for all lines of a multi-line log message.

An example of a single line log message:

```
$ jsockd 2025-09-24T21:15:45.644776Z [DEBUG] Creating thread 0
```

An example of a multi-line log message:

```
* jsockd 2025-09-24T21:15:45.644776Z [INFO] Line 1
* jsockd 2025-09-24T21:15:45.644776Z [INFO] Line 2
$ jsockd 2025-09-24T21:15:45.644776Z [INFO] Line 3
```

### 3.8 Memory leak protection

JSockD tracks memory usage by each QuickJS runtime. If the memory used by a runtime continues to grow over multiple command executions then the runtime is reset to free up memory. (This is one reason why your JSockD commands should not depend on the persistence of global state, even if you route all commands to the same socket/runtime.)

The current logic for detecting memory leaks is as follows:

* For each QuickJS runtime:
  * Let U be the initial memory usage of the runtime.
  * Let C, the memory increase counter, be zero.
  * After each 100 command executions:
    * if current usage is higher than U, increment C and update U to the current usage;
    * otherwise reset C to zero.
    * If C = 3, reset the runtime and go to the first step; othwerwise, check again after another 100 command executions.
  

## 4. Bundling your JavaScript code

### 4.1 Using a bundler

JSockD can be used with any bundler that can output an ES6 module (or with no bundler at all if your JS code is contained in a single file). The following is an example of how to bundle your code using [esbuild](https://esbuild.github.io/). The `root_module.mjs` module should contain all the code that you want to execute in the JSockD server. It can import other modules as needed.

```
esbuild root_module.mjs --bundle --outfile=bundle.mjs --sourcemap --format=esm
```

Example `root_module.mjs` contents:

```javascript
import { flubBar } from "./library1"
import { blagFoo } from "./library2"

export { flubBar, blagFoo }
````

### 4.2 The JSockD runtime environment

* The [QuickJS standard library](https://bellard.org/quickjs/quickjs.html#Standard-library) is available.
* `TextEncoder` and `TextDecoder` are implemented.
* A polyfill is included for [Web Streams](https://developer.mozilla.org/en-US/docs/Web/API/Streams_API).
* `console.log`, `console.error`, etc. log to the JSockD server log (stderr).
* `setTimeout` and `setInterval` are not available. As JSockD does not support long-running commands, you would generally want to shim these if any of your library code depends on them.
* The global object is `globalThis`.
* The global `JSockD` is available with the following method:
  * `JSockD.sendMessage(message: any, replacer?: any, space?: any): any`: sends a JSON-serializable message to the client and synchronously waits for a response. The optional `replacer` and `space` arguments are passed to `JSON.stringify` when serializing the message. The return value is the response received from the client.

## 5. Building from source

To build JSockD from source, you must first build QuickJS and then the JS server.

The version of CMake required for the build is listed in `.tool-versions`, and can be installed using [mise-en-place](https://mise.jdx.dev/) or [asdf](https://asdf-vm.com/).

### 5.1 Building QuickJS

QuickJS is built by running `./build_quickjs.sh`. This script downloads and builds the QuickJS library. The QuickJS build is kept separate from the main JSockD build because it needs to be run only once, and the QuickJS build system is a bit finicky to configure for different environments.

On systems where `make` is a non-GNU Make, the script tries `gmake` by default. You can override this by setting the `MAKE` env var to the name of the GMake command on your system.

### 5.2 Building the JS server

The JS server is built using CMake 4. The `mk.sh` wrapper script invokes CMake with the correct arguments for common use cases.

Run the following commands from within the `jsockd_server` directory:

```sh
npm i # install esbuild and clang-format
./mk.sh Debug # Debug build
./mk.sh Release # Release build
```

The resulting `jsockd` binary can be found in the `build_Release` or `build_Debug` directory. As a convenience, the `mk.sh` script can also run the executable after compilation succeeds:

```sh
./mk.sh Debug run arg1 arg2 ...
```

Run unit tests as follows:

```sh
./mk.sh Debug test
```

### 5.3 Code formatting

The `format.sh` script in `jsockd_server` formats C source files using `clang-format`. Run `npm i` to install the appropriate version of `clang-format`.

### 5.4 Developing with Fil-C

**TODO: rough notes**

* `FILC_CLANG=/path/to/fil-c/clang ./build_quickjs.sh linux_x86_64_filc`
* [In `jsockd_server`] `TOOLCHAIN_FILE=TC-fil-c.cmake ./mk.sh Debug`

## 6. The socket protocol

**_❗The details of the socket protocol are relevant only if you are implementing a JSockD client library.❗_**

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
A useful value is `00`, as the null byte cannot be present in valid UTF-8-encoded JSON or JavaScript.

A unique command ID is any non-empty sequence of 32 or fewer bytes that does not contain a space character or a separator byte.

The second field, the command, is a JavaScript expression evaluating to a function. The function is called with two arguments.
The first is the module that was loaded from the bytecode file (or `undefined` if none was given); the second is the parameter passed as the third field.

The third field is the JSON-encoded parameter value.

The server responds with a single line:

```
<command id> <response type> <response data><newline=0xA>
```

The response type is either `ok`, `message` or `exception`. If it is `ok`, the response data is the JSON-encoded result of the command. If it is `message`, the response data is a JSON-encoded message sent by the command via `JSockD.sendMessage`. If it is `exception`, the response data is a JSON-encoded error message and backtrace (see next subsection).

In the `message` case, the client should respond as follows:

```
<command id>
----- separator byte -----
<response>
----- separator byte -----
```

The response is either the special string `internal_error` (if an error occured when the client tried to process the message) or a JSON-encoded response value. The server then responds either with another `message` (in which case the client should respond as before) or with an `ok` or `exception` response.

As the protocol is synchronous, command IDs are not strictly necessary. However, it is recommended to check that responses have the expected
command ID as a means of ensuring that the client code is working correctly.

The client may send either of the following commands at any point, terminated by the separator byte:

```
?reset
?quit
```

The `?reset` command resets the server's command parser to its initial state (so that it expects the next field to be a unique command ID).

The `?quit` command causes the server to exit immediately (closing all sockets, not just the socket on which the command was sent).

Clients may shut down the server gracefully by doing exactly one of the
following:

* Sending the `?quit` command and waiting for the server to respond with `quit`.
* Closing any one of the server's UNIX domain sockets.
* Sending a SIGTERM signal to the server process.

JSockD will attempt to remove socket files when it exits, so it is not necessary for clients to clean these up. However, if the client has created a temporary dir to hold the socket files, it is the client's responsibility to remove this dir after the server has exited.
