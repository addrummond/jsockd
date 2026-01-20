# Python JSockD client

A Python client for communicating with a JSockD server (QuickJS socket daemon), designed with a Pythonic API.

- Starts a `jsockd` process
- Connects over multiple Unix domain sockets (one per JS thread)
- Runs JavaScript functions with optional bidirectional message handling
- Streams and parses `jsockd` logs
- Optional auto-download of `jsockd` with ed25519 signature verification

Note: This client targets POSIX systems (macOS, Linux). Windows users can use WSL.

## Install (uv)

If you’re using uv:

```sh
# From your project directory
uv add jsockdclient
```

This will install the Python client and its dependencies (requests, PyNaCl) for the auto-download feature.

## Quick start

```python
from jsockdclient import DefaultConfig, init_jsockd_client_via_auto_download, JSockDJSException

config = DefaultConfig()
# Optional: attach a logger for jsockd stderr lines
config.Logger = lambda ts, level, msg: print(f"{ts} [{level}] {msg}")

# Optionally provide a source map that matches your bytecode module (if using one)
# config.SourceMap = "example_module.mjs.map"

client = init_jsockd_client_via_auto_download(config)
try:
    # The query is a JS function of the form (m, p) => ...
    # - m is an internal object with JSockD helpers
    # - p is your parameter (JSON value)
    result = client.run("(_, name) => 'Hello ' + name", "World")
    print(result)  # "Hello World"
except JSockDJSException as e:
    # e.result_json contains the server-side JS error details (JSON)
    print("JavaScript exception:", e.result_json)
finally:
    client.close()
```

## Using a local jsockd executable

If you already have the `jsockd` binary on disk, initialize with its path:

```python
from jsockdclient import DefaultConfig, init_jsockd_client

config = DefaultConfig()
client = init_jsockd_client(config, "/path/to/jsockd")
try:
    result = client.run("(_, n) => n + 1", 99)
    print(result)  # 100
finally:
    client.close()
```

## Running raw vs typed

- Typed (Python values in/out):
  - `client.run(query: str, param: Any = None, message_handler: Optional[Callable[[Any], Any]] = None) -> Any`

- Raw (JSON strings in/out):
  - `client.run_raw(query: str, json_param: str, message_handler: Optional[Callable[[str], str]] = None) -> str`

```python
# Typed
result = client.run("(_, n) => n + 1", 41)
assert result == 42

# Raw
raw_json = client.run_raw("(_, n) => n + 1", "41")
assert raw_json.strip() == "42"
```

## Message handlers (bidirectional messaging)

JSockD commands can send messages back to the client and await replies.

Typed handler (Python objects in/out):
```python
def on_message(msg):
    # msg is a Python value decoded from JSON
    # Return any JSON-serializable Python value
    if msg == "foo":
        return "ack-1"
    return "ack-2"

result = client.run(
    "(m, p) => { JSockD.sendMessage('foo'); return JSockD.sendMessage('bar'); }",
    99,
    message_handler=on_message,
)
assert result == "ack-2"
```

Raw handler (JSON strings in/out):
```python
def on_message_raw(msg_json: str) -> str:
    # msg_json is a JSON string (e.g., "\"foo\"")
    # Return a JSON string (e.g., "\"ack-1\"")
    if msg_json.strip() == "\"foo\"":
        return "\"ack-1\""
    return "\"ack-2\""

raw_json = client.run_raw(
    "(m, p) => { JSockD.sendMessage('foo'); return JSockD.sendMessage('bar'); }",
    "99",
    message_handler=on_message_raw,
)
assert raw_json == "\"ack-2\""
```

If your handler raises or returns invalid JSON, the client terminates the session with a clear error.

## Configuration

`DefaultConfig()` populates sensible defaults:

- `NThreads`: number of JS threads (defaults to CPU cores)
- `BytecodeModuleFile`: path to a QuickJS bytecode module (optional)
- `BytecodeModulePublicKey`: hex-encoded public key to verify the module (optional). Exported as env `JSOCKD_BYTECODE_MODULE_PUBLIC_KEY`.
- `SourceMap`: source map filename (optional, helps logging)
- `MaxIdleTimeUs`: kill idle QuickJS instances after this time (microseconds)
- `MaxCommandRuntimeUs`: per-command max runtime (microseconds)
- `TimeoutUs`: client timeout for starting/dialing/responding (microseconds; default 15s)
- `SkipJSockDVersionCheck`: if true, skip protocol version check (avoid in production)
- `Logger`: callback `(timestamp: Optional[datetime], level: str, message: str) -> None`
- `MaxRestartsPerMinute`: not currently used for restarts in the Python client
- `LogPrefix`: sets `JSOCKD_LOG_PREFIX` for jsockd logs

Example:

```python
config = DefaultConfig()
config.NThreads = 2
config.BytecodeModuleFile = "example_module.qjsbc"
config.BytecodeModulePublicKey = "deadbeef..."  # hex
config.SourceMap = "example_module.mjs.map"
config.MaxIdleTimeUs = 0
config.MaxCommandRuntimeUs = 0
config.TimeoutUs = 15_000_000
config.SkipJSockDVersionCheck = False
config.LogPrefix = "myapp"
```

## Version compatibility

This client expects JSockD server version `0.0.139`. On startup, the client parses the `READY` line from jsockd and validates the version unless `SkipJSockDVersionCheck` is `True`.

## Auto-download details

`init_jsockd_client_via_auto_download(config)`:
- Downloads `jsockd` from GitHub Releases based on OS/arch
- Downloads and verifies the signature against the embedded ed25519 public key
- Extracts the `jsockd` binary to a temp folder
- Launches `jsockd` with your config

Dependencies required (installed automatically with the package):
- `requests`
- `PyNaCl` (used for ed25519 verification)

If you prefer not to enable network access or want to manage upgrades yourself, use `init_jsockd_client(config, "/path/to/jsockd")`.

## Logging

`config.Logger` receives parsed log lines from `jsockd` stderr. The timestamp is parsed as RFC3339 (to microsecond precision). If parsing fails, `timestamp` may be `None`.

```python
from datetime import datetime

def logger(ts: datetime | None, level: str, msg: str) -> None:
    when = ts.isoformat() if ts else "unknown-time"
    print(f"{when} [{level}] {msg}")

config.Logger = logger
```

## Process and sockets

- The client creates a temporary directory and requests one Unix domain socket per JS thread.
- It connects to as many sockets as `jsockd` reports in its `READY N <version>` line.
- Commands are distributed across these connections.

## Error handling

- `JSockDClientError`: client/transport/process errors (start-up failures, timeouts, I/O, malformed protocol, unexpected process exit).
- `JSockDJSException`: raised when the JavaScript function throws on the server side. Inspect `e.result_json` for server-provided error details (JSON).

## Cleanup

Always close your client:

```python
client.close()
```

This:
- Signals worker threads to stop
- Closes Unix sockets
- Terminates the `jsockd` process
- Cleans up temp directories and any auto-downloaded `jsockd` binary

## API summary

- `DefaultConfig() -> Config`
- `init_jsockd_client(config: Config, jsockd_exec: str) -> JSockDClient`
- `init_jsockd_client_via_auto_download(config: Config) -> JSockDClient`
- `JSockDClient`:
  - `run(query: str, param: Any = None, message_handler: Optional[Callable[[Any], Any]] = None) -> Any`
  - `run_raw(query: str, json_param: str, message_handler: Optional[Callable[[str], str]] = None) -> str`
  - `close() -> None`
- Exceptions:
  - `JSockDClientError` (client/transport errors)
  - `JSockDJSException` (JavaScript exceptions; `result_json` holds details)

## SSR with React 19

See the main JSockD documentation for an example of using JSockD for server-side rendering with React 19:
- https://github.com/addrummond/jsockd/blob/main/docs/ssr_with_react_19.md
- https://github.com/addrummond/jsockd/tree/main/docs/ssr_with_react_19_example_code

## Development (uv)

The following commands assume you have uv installed and available on your PATH.

- Create and activate a virtual environment:
```sh
uv venv
# macOS/Linux:
source .venv/bin/activate
# Windows (PowerShell):
# .venv\Scripts\Activate.ps1
```

- Install the package in editable mode with development dependencies:
```sh
uv pip install -e .[dev]
```

- Run tests:
```sh
uv run pytest -q
```

- Lint and format:
```sh
uv run ruff check .
uv run ruff format
```

- Type-check:
```sh
uv run mypy
```

- Build distributions (wheel and sdist):
```sh
uv build
```

## License

MIT (same as the main JSockD repository).