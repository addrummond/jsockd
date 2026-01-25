# Python JSockD client

A Python client for JSockD. 

⚠️ **_Experimental._** ⚠️

## Install (uv)

```sh
# __jsockd_version__ <- ignore this comment, it's used by a CI check
#uv add "git+https://github.com/addrummond/jsockd@v0.0.131#subdirectory=clients/python/jsockdclient"
# TEMP while we're still on Python branch
"git+https://github.com/addrummond/jsockd@python#subdirectory=clients/python/jsockdclient"
```

## Quick start

```python
from jsockdclient import DefaultConfig, init_jsockd_client_via_auto_download, JSockDJSException

config = DefaultConfig()
# Optional: attach a logger for jsockd stderr lines
config.Logger = lambda ts, level, msg: print(f"{ts} [{level}] {msg}")

# Optionally provide a source map that matches your bytecode module (if using one)
# config.SourceMap = "example_module.mjs.map"

client = nit_jsockd_client_via_auto_download(config)
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

`Config()` populates sensible defaults:

- `n_threads`: number of JS threads (defaults to CPU cores)
- `bytecode_module_file`: path to a QuickJS bytecode module (optional)
- `bytecode_module_public_key`: hex-encoded public key to verify the module (optional). Exported as env `JSOCKD_BYTECODE_MODULE_PUBLIC_KEY`.
- `source_map`: source map filename (optional, helps logging)
- `max_idle_time_us`: kill idle QuickJS instances after this time (microseconds)
- `max_command_runtime_us`: per-command max runtime (microseconds)
- `timeout_us`: client timeout for starting/dialing/responding (microseconds; default 15s)
- `skip_jsockd_version_check`: if true, skip protocol version check (avoid in production)
- `logger`: callback `(timestamp: Optional[datetime], level: str, message: str) -> None`
- `max_restarts_per_minute`: not currently used for restarts in the Python client
- `log_prefix`: sets `JSOCKD_LOG_PREFIX` for jsockd logs

Example:

```python
config = Config()
config.n_threads = 2
config.bytecode_module_file = "example_module.qjsbc"
config.bytecode_module_public_key = "deadbeef..."  # hex
config.source_map = "example_module.mjs.map"
```

## Auto-download details

`init_jsockd_client_via_auto_download(config)`:
- Downloads `jsockd` from GitHub Releases based on OS/arch
- Downloads and verifies the signature against the embedded ed25519 public key
- Extracts the `jsockd` binary to a temp folder
- Launches `jsockd` with your config

If you prefer not to enable network access or want to manage upgrades yourself, use `init_jsockd_client(config, "/path/to/jsockd")`.

## Logging

`config.Logger` receives parsed log lines from `jsockd` stderr. The timestamp is parsed as RFC3339. If parsing unexpectedly fails, `timestamp` is `None`.

```python
from datetime import datetime

def logger(ts: datetime | None, level: str, msg: str) -> None:
    when = ts.isoformat() if ts else "unknown-time"
    print(f"{when} [{level}] {msg}")

config.Logger = logger
```

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
