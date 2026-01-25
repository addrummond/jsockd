# Python JSockD client

A Python client for JSockD (QuickJS socket daemon).

⚠️⚠️ Status: Experimental ⚠️⚠️

## Installation (uv)

```sh
-# __jsockd_version__ <- ignore this comment, it's used by a CI check
-#uv add "git+https://github.com/addrummond/jsockd@v0.0.131#subdirectory=clients/python/jsockdclient"
-# TEMP while we're still on Python branch
-"git+https://github.com/addrummond/jsockd@python#subdirectory=clients/python/jsockdclient"
```

## Quick start

```python
from jsockdclient import Config, JSockDClient, JSockDClientError, JSockDJSException

# Configure client
config = Config()
config.n_threads = 1
config.skip_jsockd_version_check = True  # useful in development

# Optional: attach a logger for jsockd stderr lines
from datetime import datetime
def logger(ts: datetime | None, level: str, msg: str) -> None:
    when = ts.isoformat() if ts else "unknown-time"
    print(f"{when} [{level}] {msg}")
config.logger = logger

# Optionally provide a source map filename that matches your bytecode module (if using one)
# config.souce_map = "example_module.mjs.map"

# Autodownload jsockd binary and run it
client = JSockDClient(config, autodownload=True)
try:
    response = client.send_command("(_, name) => 'Hello ' + name", "World")
    if response.exception:
        print("JavaScript exception:", response.raw_response.result_json)
    else:
        print(response.result)  # "Hello World"
finally:
    client.close()
```

## Using a local jsockd executable

If you already have the `jsockd` binary on disk, initialize with its path:

```python
from jsockdclient import Config, JSockDClient

config = Config()
client = JSockDClient(config, jsockd_exec="/path/to/jsockd")
try:
    result = client.send_command("(_, n) => n + 1", 99)
    print(result.result)  # 100
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

response = client.send_command(
    "(m, p) => { JSockD.sendMessage('foo'); return JSockD.sendMessage('bar'); }",
    99,
    message_handler=on_message,
)
assert response.result == "ack-2"
```

Raw handler (JSON strings in/out):

```python
def on_message_raw(msg_json: str) -> str:
    # msg_json is a JSON string (e.g., "\"foo\"")
    # Return a JSON string (e.g., "\"ack-1\"")
    if msg_json.strip() == "\"foo\"":
        return "\"ack-1\""
    return "\"ack-2\""

raw = client.send_raw_command(
    "(m, p) => { JSockD.sendMessage('foo'); return JSockD.sendMessage('bar'); }",
    "99",
    message_handler=on_message_raw,
)
assert raw.result_json == "\"ack-2\""
```

If your handler raises or returns invalid JSON, the client terminates the session with a clear error.

## Configuration

`Config()` provides sensible defaults. Fields:

- `n_threads: int` — number of JS worker threads; defaults to CPU cores (>= 1)
- `bytecode_module_file: str` — path to a QuickJS bytecode module (optional, "" for none)
- `bytecode_module_public_key: str` — hex-encoded public key to verify the module signature (optional, "" for none)
- `souce_map: str` — source map filename (optional; helps logging)
- `max_idle_time_us: int` — idle timeout per connection (microseconds); 0 disables
- `max_command_runtime_us: int` — per-command max runtime (microseconds); 0 uses default
- `timeout_us: int` — I/O timeout for start/dial/respond (microseconds; default 15s)
- `skip_jsockd_version_check: bool` — skip protocol version check (avoid in production)
- `logger: Optional[Callable[[Optional[datetime], str, str], None]]` — stderr log callback
- `max_restarts_per_minute: int` — bounded auto-restart budget if jsockd crashes; 0 disables restarts
- `log_prefix: str` — sets `JSOCKD_LOG_PREFIX` for jsockd logs
