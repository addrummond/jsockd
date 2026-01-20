# Python JSockD client inspired by the Go client
# - Starts the jsockd process
# - Connects via multiple unix domain sockets (one per thread)
# - Sends commands with per-connection workers
# - Supports message handlers (bidirectional messaging during a command)
# - Parses jsockd logs on stderr and forwards to a Python logger callback
# - Optional auto-download of jsockd with ed25519 signature verification
#
# Requires Python 3.10+
from __future__ import annotations

import contextlib
import dataclasses
import enum
import io
import itertools
import json
import os
import platform
import queue
import random
import re
import shutil
import socket
import stat
import subprocess
import tarfile
import tempfile
import threading
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Generic, Optional, TypeVar

# Optional runtime deps for auto download. These are declared in pyproject.
try:
    import requests
    from nacl import bindings as nacl_bindings
except Exception:  # pragma: no cover - only needed for auto-download
    requests = None  # type: ignore[assignment]
    nacl_bindings = None  # type: ignore[assignment]

__all__ = [
    "JSOCKD_VERSION",
    "Config",
    "DefaultConfig",
    "JSockDClient",
    "init_jsockd_client",
    "init_jsockd_client_via_auto_download",
    "JSockDClientError",
    "JSockDJSException",
]

# Mirror the Go client
JSOCKD_VERSION = "0.0.139"

# Protocol tokens
_MESSAGE_HANDLER_INTERNAL_ERROR = "internal_error"

# Log line regex (same as Go)
_LOG_LINE_RE = re.compile(r"(\*|\$) jsockd ([^ ]+) \[([^][]+)\] (.*)")

T = TypeVar("T")
MsgT = TypeVar("MsgT")
MsgRespT = TypeVar("MsgRespT")


class JSockDClientError(Exception):
    pass


class JSockDJSException(JSockDClientError):
    """Raised when the JS command throws an exception on the server side."""

    def __init__(self, result_json: str) -> None:
        super().__init__("JavaScript exception")
        self.result_json = result_json


@dataclass(slots=True)
class Config:
    # The number of threads that JSockD should use. If 0, number of CPU cores is used.
    NThreads: int
    # The filename of the bytecode module to load, or "" if none.
    BytecodeModuleFile: str = ""
    # Hex-encoded public key used to verify the signature of the bytecode module, or "" if none.
    BytecodeModulePublicKey: str = ""
    # The filename of the source map to load, or "" if none.
    SourceMap: str = ""
    # The maximum time in microseconds a connection is allowed to be idle before JSockD shuts down its associated QuickJS instance. If 0, no idle timeout is applied.
    MaxIdleTimeUs: int = 0
    # The maximum time in microseconds a command is allowed to run (0 for default max time)
    MaxCommandRuntimeUs: int = 0
    # The timeout in microseconds when communicating with JSockD
    TimeoutUs: int = 15_000_000  # 15 seconds
    # If true, the JSockD server version is not checked against the version expected by this client. This should always be set to 'false' in production systems.
    SkipJSockDVersionCheck: bool = False
    # If set, this function is called for each log message sent by JSockD.
    # timestamp can be None if parsing fails
    Logger: Optional[Callable[[Optional[datetime], str, str], None]] = None
    # The maximum number of times that the jsockd process will be restarted within one minute if it crashes. If 0, it will never be restarted.
    MaxRestartsPerMinute: int = 1
    # Value for JSOCKD_LOG_PREFIX env var
    LogPrefix: str = ""


def DefaultConfig() -> Config:
    nthreads = os.cpu_count() or 1
    return Config(
        NThreads=nthreads,
        Logger=lambda ts, level, msg: print(f"[jsockd] {msg}"),
    )


@dataclass(slots=True)
class RawResponse:
    Exception: bool
    ResultJson: str


@dataclass(slots=True)
class Response(Generic[T]):
    Exception: bool
    Result: T
    RawResponse: RawResponse


@dataclasses.dataclass(slots=True)
class _Command:
    id: str
    query: str
    param_json: str
    response_q: "queue.Queue[RawResponse]"
    message_handler: Optional[Callable[[str], str]]


@dataclasses.dataclass(slots=True)
class _InternalClient:
    process: subprocess.Popen[str]
    sockets: list[socket.socket]
    socket_paths: list[str]
    cmd_queues: list["queue.Queue[Optional[_Command]]"]
    config: Config
    socket_tmpdir: str
    fatal_error: Optional[BaseException]
    fatal_error_lock: threading.Lock
    quit_event: threading.Event
    next_cmd_id: itertools.count
    choose_index: itertools.count
    restarts_in_window: int
    last_restart_time: float
    restart_guard: threading.Lock
    auto_downloaded_exec: Optional[str]


def _parse_rfc3339nano(ts: str) -> Optional[datetime]:
    # Try best-effort parse of RFC3339 with up to nanoseconds
    # Normalize 'Z' to '+00:00'
    try:
        if ts.endswith("Z"):
            ts = ts[:-1] + "+00:00"
        # If fractional seconds > 6 digits, trim to microseconds
        if "." in ts:
            head, tail = ts.split(".", 1)
            if "+" in tail:
                frac, tz = tail.split("+", 1)
                frac = (frac + "000000")[:6]
                ts2 = f"{head}.{frac}+{tz}"
            elif "-" in tail:
                frac, tz = tail.split("-", 1)
                frac = (frac + "000000")[:6]
                ts2 = f"{head}.{frac}-{tz}"
            else:
                # no tz (unlikely here)
                frac = (tail + "000000")[:6]
                ts2 = f"{head}.{frac}"
        else:
            ts2 = ts
        return datetime.fromisoformat(ts2)
    except Exception:
        return None


def _stream_stderr_logs(stderr: io.TextIOBase, config: Config) -> None:
    zero_time: Optional[datetime] = None
    current_line: list[str] = []
    last_was_dollar = True
    for raw in stderr:
        line = raw.strip()
        m = _LOG_LINE_RE.match(line)
        if not m:
            if config.Logger:
                config.Logger(zero_time, "INFO", line)
            continue
        prefix, timestamp, level, msg = m.groups()
        t = _parse_rfc3339nano(timestamp)
        if prefix == "*":
            if current_line:
                current_line.append("\n")
            current_line.append(msg)
        elif prefix == "$":
            if not last_was_dollar:
                current_line.append("\n")
            last_was_dollar = False
            current_line.append(msg)
            if config.Logger:
                config.Logger(t, level, "".join(current_line))
            current_line.clear()
        else:
            if config.Logger:
                config.Logger(zero_time, "INFO", line)


def _read_ready_from_stdout(
    stdout: io.TextIOBase, ready_q: "queue.Queue[int]", err_q: "queue.Queue[BaseException]", config: Config
) -> None:
    try:
        for raw in stdout:
            line = raw.strip()
            if line.startswith("READY "):
                parts = line.split(" ", 2)
                if len(parts) == 3:
                    try:
                        n = int(parts[1].strip())
                    except Exception:
                        err_q.put(JSockDClientError(f"malformed READY line: {line!r}"))
                        return
                    version = parts[2]
                    if not config.SkipJSockDVersionCheck and version != JSOCKD_VERSION:
                        err_q.put(
                            JSockDClientError(f"version mismatch: client {JSOCKD_VERSION!r}, server {version!r}")
                        )
                        return
                    ready_q.put(n)
                    return
                err_q.put(JSockDClientError(f"malformed READY line: {line!r}"))
                return
        # EOF
        err_q.put(JSockDClientError("stdout closed before READY line; check jsockd logs for error"))
    except Exception as e:
        err_q.put(e)


def _prepare_cmd_args(config: Config, sockets: list[str]) -> list[str]:
    args: list[str] = ["-b", "00"]
    if config.BytecodeModuleFile:
        args += ["-m", config.BytecodeModuleFile]
    if config.SourceMap:
        args += ["-sm", config.SourceMap]
    if config.MaxIdleTimeUs:
        args += ["-i", str(config.MaxIdleTimeUs)]
    if config.MaxCommandRuntimeUs:
        args += ["-t", str(config.MaxCommandRuntimeUs)]
    args += ["-s", "--"]
    args.extend(sockets)
    return args


def _read_record(fp: io.BufferedReader) -> str:
    line = fp.readline()
    if not line:
        raise JSockDClientError("connection closed while reading record")
    try:
        return line.decode("utf-8", errors="strict")
    except Exception as e:
        raise JSockDClientError(f"decode record: {e}") from e


def _conn_handler(conn: socket.socket, cmd_q: "queue.Queue[Optional[_Command]]", iclient: _InternalClient) -> None:
    try:
        rfile = conn.makefile("rb")
        wfile = conn.makefile("wb")
        while not iclient.quit_event.is_set():
            try:
                cmd = cmd_q.get(timeout=0.1)
            except queue.Empty:
                continue
            if cmd is None:
                break

            payload = f"{cmd.id}\x00{cmd.query}\x00{cmd.param_json}\x00".encode("utf-8")
            try:
                wfile.write(payload)
                wfile.flush()
            except Exception as e:
                _set_fatal_error(iclient, e)
                return

            try:
                rec = _read_record(rfile)
            except Exception as e:
                _set_fatal_error(iclient, e)
                return

            parts = rec.split(" ", 1)
            if len(parts) != 2 or parts[0] != cmd.id:
                _set_fatal_error(iclient, JSockDClientError(f"malformed or mismatched response: {rec!r}"))
                return

            rest = parts[1]
            if rest.startswith("exception "):
                cmd.response_q.put(RawResponse(Exception=True, ResultJson=rest[len("exception ") :].rstrip("\n")))
                continue
            elif rest.startswith("ok "):
                cmd.response_q.put(RawResponse(Exception=False, ResultJson=rest[len("ok ") :].rstrip("\n")))
                continue
            elif rest.startswith("message "):
                # Handle message exchange loop
                message_rest = rest[len("message ") :]
                while True:
                    response_json = "null"
                    handler_err: Optional[BaseException] = JSockDClientError("no message handler")
                    if cmd.message_handler is not None:
                        try:
                            response_json = cmd.message_handler(message_rest.rstrip("\n"))
                            handler_err = None
                        except BaseException as e:
                            handler_err = e
                    if handler_err is not None:
                        _set_fatal_error(iclient, JSockDClientError(f"message handler error: {handler_err}"))
                        try:
                            wfile.write(f"{cmd.id}\x00{_MESSAGE_HANDLER_INTERNAL_ERROR}\x00".encode("utf-8"))
                            wfile.flush()
                        except Exception:
                            pass
                        return
                    try:
                        wfile.write(f"{cmd.id}\x00{response_json}\x00".encode("utf-8"))
                        wfile.flush()
                    except Exception as e:
                        _set_fatal_error(iclient, e)
                        return
                    try:
                        mresp = _read_record(rfile)
                    except Exception as e:
                        _set_fatal_error(iclient, e)
                        return
                    mparts = mresp.split(" ", 1)
                    if len(mparts) != 2 or mparts[0] != cmd.id:
                        _set_fatal_error(
                            iclient,
                            JSockDClientError(f"mismatched or malformed message response: {mresp!r}, want id {cmd.id!r}"),
                        )
                        return
                    if mparts[1].startswith("ok "):
                        cmd.response_q.put(RawResponse(Exception=False, ResultJson=mparts[1][3:].rstrip("\n")))
                        break
                    elif mparts[1].startswith("exception "):
                        cmd.response_q.put(RawResponse(Exception=True, ResultJson=mparts[1][len("exception ") :].rstrip("\n")))
                        break
                    elif mparts[1].startswith("message "):
                        message_rest = mparts[1][len("message ") :]
                        continue
                    else:
                        _set_fatal_error(iclient, JSockDClientError(f"malformed message response from JSockD: {mresp!r}"))
                        return
            else:
                _set_fatal_error(iclient, JSockDClientError(f"malformed command response from JSockD: {rec!r}"))
                return
    finally:
        try:
            conn.close()
        except Exception:
            pass


def _get_fatal_error(iclient: _InternalClient) -> Optional[BaseException]:
    with iclient.fatal_error_lock:
        return iclient.fatal_error


def _set_fatal_error(iclient: _InternalClient, err: BaseException) -> None:
    with iclient.fatal_error_lock:
        if iclient.fatal_error is None:
            iclient.fatal_error = err


def _start_jsockd_process(jsockd_exec: str, config: Config, sockets: list[str]) -> subprocess.Popen[str]:
    args = [jsockd_exec] + _prepare_cmd_args(config, sockets)
    env = os.environ.copy()
    if config.LogPrefix:
        env["JSOCKD_LOG_PREFIX"] = config.LogPrefix
    if config.BytecodeModulePublicKey:
        env["JSOCKD_BYTECODE_MODULE_PUBLIC_KEY"] = config.BytecodeModulePublicKey

    # Use text mode for stdout/stderr to simplify parsing
    proc = subprocess.Popen(
        args,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
    )
    return proc


def _dial_unix(path: str, timeout_us: int) -> socket.socket:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout_us / 1_000_000.0 if timeout_us > 0 else None)
    s.connect(path)
    # Avoid per-op timeouts; rely on handler threads and overall client timeout.
    s.settimeout(None)
    return s


class JSockDClient:
    def __init__(self, iclient: _InternalClient):
        self._iclient = iclient
        self._close_lock = threading.Lock()
        self._closed = False

    @property
    def process(self) -> subprocess.Popen[str]:
        return self._iclient.process

    def get_jsockd_process(self) -> subprocess.Popen[str]:
        return self._iclient.process

    def run_raw(
        self,
        query: str,
        json_param: str,
        message_handler: Optional[Callable[[str], str]] = None,
    ) -> str:
        raw = self.send_raw_command(query, json_param, message_handler)
        if raw.Exception:
            raise JSockDJSException(raw.ResultJson)
        return raw.ResultJson

    def run(
        self,
        query: str,
        param: Any = None,
        message_handler: Optional[Callable[[Any], Any]] = None,
    ) -> Any:
        if message_handler is None:
            resp = self.send_command(query, param)
        else:
            resp = self.send_command_with_message_handler(query, param, message_handler)
        if resp.Exception:
            raise JSockDJSException(resp.RawResponse.ResultJson)
        return resp.Result

    def close(self) -> None:
        with self._close_lock:
            if self._closed:
                return
            self._closed = True

            ic = self._iclient
            ic.quit_event.set()

            # Stop cmd workers
            for q in ic.cmd_queues:
                try:
                    q.put_nowait(None)
                except Exception:
                    pass

            # Close sockets
            for s in ic.sockets:
                try:
                    s.close()
                except Exception:
                    pass

            # Terminate process
            try:
                timeout_s = ic.config.TimeoutUs / 1_000_000.0 if ic.config.TimeoutUs > 0 else 5.0
                ic.process.terminate()
                try:
                    ic.process.wait(timeout=timeout_s)
                except subprocess.TimeoutExpired:
                    ic.process.kill()
                    ic.process.wait(timeout=timeout_s)
            except Exception:
                pass

            # Cleanup sockets dir and temp exec if any
            try:
                if ic.socket_tmpdir:
                    shutil.rmtree(ic.socket_tmpdir, ignore_errors=True)
            except Exception:
                pass
            try:
                if ic.auto_downloaded_exec:
                    with contextlib.suppress(Exception):
                        os.remove(ic.auto_downloaded_exec)
            except Exception:
                pass

            # Bubble up fatal if recorded
            fe = _get_fatal_error(ic)
            if fe:
                raise JSockDClientError(str(fe)) from fe

    def send_raw_command(
        self,
        query: str,
        json_param: str,
        message_handler: Optional[Callable[[str], str]] = None,
    ) -> RawResponse:
        ic = self._iclient
        fe = _get_fatal_error(ic)
        if fe:
            raise JSockDClientError(str(fe)) from fe
        if ic.quit_event.is_set():
            raise JSockDClientError("jsockd client has quit")

        cmd_id = str(next(ic.next_cmd_id))
        resp_q: "queue.Queue[RawResponse]" = queue.Queue()
        cmd = _Command(
            id=cmd_id,
            query=query,
            param_json=json_param,
            response_q=resp_q,
            message_handler=message_handler,
        )
        nconns = len(ic.cmd_queues)
        if nconns == 0:
            raise JSockDClientError("no connections available")

        idx = (next(ic.choose_index)) % nconns
        try:
            ic.cmd_queues[idx].put_nowait(cmd)
        except Exception:
            # Fallback to random enqueue
            random.choice(ic.cmd_queues).put(cmd)

        timeout_s = ic.config.TimeoutUs / 1_000_000.0 if ic.config.TimeoutUs > 0 else None
        try:
            return resp_q.get(timeout=timeout_s)
        except queue.Empty:
            raise JSockDClientError("timeout waiting for response")

    def send_raw_command_with_message_handler(
        self,
        query: str,
        json_param: str,
        message_handler: Callable[[str], str],
    ) -> RawResponse:
        return self.send_raw_command(query, json_param, message_handler)

    def send_command(self, query: str, json_param: Any) -> Response[Any]:
        payload = json.dumps(json_param, separators=(",", ":"))
        raw = self.send_raw_command(query, payload)
        if raw.Exception:
            return Response(Exception=True, Result=None, RawResponse=raw)
        try:
            result = json.loads(raw.ResultJson)
        except Exception as e:
            raise JSockDClientError(f"json unmarshal: {e}") from e
        return Response(Exception=False, Result=result, RawResponse=raw)

    def send_command_with_message_handler(
        self,
        query: str,
        json_param: Any,
        message_handler: Callable[[Any], Any],
    ) -> Response[Any]:
        msg_handler_err: list[Optional[BaseException]] = [None]

        def _wrapped(json_message: str) -> str:
            try:
                message = json.loads(json_message)
            except Exception as e:
                msg_handler_err[0] = e
                raise
            try:
                response = message_handler(message)
            except Exception as e:
                msg_handler_err[0] = e
                raise
            try:
                return json.dumps(response, separators=(",", ":"))
            except Exception as e:
                msg_handler_err[0] = e
                raise

        payload = json.dumps(json_param, separators=(",", ":"))
        try:
            raw = self.send_raw_command(query, payload, _wrapped)
        except Exception as cmd_err:
            if msg_handler_err[0] is not None:
                raise JSockDClientError(f"message handler error: {msg_handler_err[0]}; command error: {cmd_err}") from cmd_err
            raise
        if msg_handler_err[0] is not None:
            raise JSockDClientError(f"message handler error: {msg_handler_err[0]}")
        if raw.Exception:
            return Response(Exception=True, Result=None, RawResponse=raw)
        try:
            result = json.loads(raw.ResultJson)
        except Exception as e:
            raise JSockDClientError(f"json unmarshal: {e}") from e
        return Response(Exception=False, Result=result, RawResponse=raw)


def init_jsockd_client(config: Config, jsockd_exec: str) -> JSockDClient:
    if config.NThreads <= 0:
        raise JSockDClientError(
            "NThreads must be greater than zero. Did you forget to call DefaultConfig() to initialize Config?"
        )

    # Create short socket dir to avoid UDS path len issues
    socket_tmpdir = tempfile.mkdtemp(prefix="jsd_")
    sockets: list[str] = [str(Path(socket_tmpdir) / f"jsd_{i}.sock") for i in range(config.NThreads)]

    # Start process
    proc = _start_jsockd_process(jsockd_exec, config, sockets)

    # Setup readers
    assert proc.stdout is not None and proc.stderr is not None
    ready_q: "queue.Queue[int]" = queue.Queue()
    err_q: "queue.Queue[BaseException]" = queue.Queue()

    t_ready = threading.Thread(
        target=_read_ready_from_stdout, args=(proc.stdout, ready_q, err_q, config), name="jsockd-ready", daemon=True
    )
    t_ready.start()

    t_logs = threading.Thread(
        target=_stream_stderr_logs, args=(proc.stderr, config), name="jsockd-logs", daemon=True
    )
    t_logs.start()

    # Wait for READY or error/timeout
    ready_count = 0
    try:
        timeout_s = config.TimeoutUs / 1_000_000.0 if config.TimeoutUs > 0 else 15.0
        start = time.time()
        while True:
            try:
                n = ready_q.get_nowait()
                ready_count = n
                break
            except queue.Empty:
                pass
            try:
                e = err_q.get_nowait()
                proc.kill()
                proc.wait()
                shutil.rmtree(socket_tmpdir, ignore_errors=True)
                raise JSockDClientError(f"waiting for READY: {e}") from e
            except queue.Empty:
                pass
            if (time.time() - start) > timeout_s:
                proc.kill()
                proc.wait()
                shutil.rmtree(socket_tmpdir, ignore_errors=True)
                raise JSockDClientError(f"timeout ({timeout_s:.3f}s) waiting for READY line")
            time.sleep(0.01)
    except Exception:
        raise

    if ready_count > len(sockets):
        proc.kill()
        proc.wait()
        shutil.rmtree(socket_tmpdir, ignore_errors=True)
        raise JSockDClientError(
            f"ready count ({ready_count}) exceeds number of sockets specified ({len(sockets)})"
        )

    # Dial sockets
    conns: list[socket.socket] = []
    try:
        for i in range(ready_count):
            s = _dial_unix(sockets[i], config.TimeoutUs)
            conns.append(s)
    except Exception as e:
        proc.kill()
        proc.wait()
        shutil.rmtree(socket_tmpdir, ignore_errors=True)
        raise JSockDClientError(f"dial {sockets[len(conns)]}: {e}") from e

    # Build internal client
    ic = _InternalClient(
        process=proc,
        sockets=conns,
        socket_paths=sockets[:ready_count],
        cmd_queues=[queue.Queue() for _ in range(ready_count)],
        config=config,
        socket_tmpdir=socket_tmpdir,
        fatal_error=None,
        fatal_error_lock=threading.Lock(),
        quit_event=threading.Event(),
        next_cmd_id=itertools.count(1),
        choose_index=itertools.count(0),
        restarts_in_window=0,
        last_restart_time=0.0,
        restart_guard=threading.Lock(),
        auto_downloaded_exec=None,
    )

    # Start connection workers
    for i in range(ready_count):
        threading.Thread(
            target=_conn_handler, args=(conns[i], ic.cmd_queues[i], ic), daemon=True, name=f"jsockd-conn-{i}"
        ).start()

    # Monitor process exit
    def _reaper() -> None:
        rc = proc.wait()
        ic.quit_event.set()
        # Note: Go client restarts jsockd if it crashes, bounded by MaxRestartsPerMinute.
        # For simplicity, we only record a fatal error on unexpected exit.
        if rc != 0:
            _set_fatal_error(ic, JSockDClientError(f"jsockd process exited unexpectedly with code {rc}"))
        try:
            shutil.rmtree(ic.socket_tmpdir, ignore_errors=True)
        except Exception:
            pass

    threading.Thread(target=_reaper, daemon=True, name="jsockd-reaper").start()

    return JSockDClient(ic)


# Auto-download support (parity with Go client)
_JSOCKD_RELEASE_URL_TEMPL = (
    "https://github.com/addrummond/jsockd/releases/download/vVERSION/jsockd-VERSION-OS-ARCH.tar.gz"
)
_JSOCKD_SIGNATURE_URL_TEMPL = (
    "https://github.com/addrummond/jsockd/releases/download/vVERSION/ed25519_signatures.txt"
)
_JSOCKD_BINARY_PUBLIC_KEY = "b136fca8fbfc42fe6dc95dedd035b0b50ad93b6a5d6fcaf8fc0552e9d29ee406"


class _DownloadError(JSockDClientError):
    pass


def _download_and_verify_jsockd() -> str:
    if requests is None or nacl_bindings is None:
        raise JSockDClientError("Auto-download requires 'requests' and 'PyNaCl' dependencies installed")

    os_name = platform.system().lower()
    if os_name == "darwin":
        os_name = "macos"
    arch = platform.machine().lower()
    # Align with Go mapping: only change amd64 to x86_64, otherwise keep arch
    if arch == "amd64":
        arch = "x86_64"
    # Common normalize
    if arch == "x86-64":
        arch = "x86_64"

    url = _JSOCKD_RELEASE_URL_TEMPL.replace("VERSION", JSOCKD_VERSION).replace("OS", os_name).replace("ARCH", arch)
    sig_url = _JSOCKD_SIGNATURE_URL_TEMPL.replace("VERSION", JSOCKD_VERSION)

    # Download signatures file
    try:
        sig_resp = requests.get(sig_url, timeout=30)
    except Exception as e:
        raise _DownloadError(f"failed to download file: {sig_url}: {e}") from e
    if sig_resp.status_code != 200:
        raise _DownloadError(f"failed to download file: unexpected HTTP status {sig_resp.status_code} for {sig_url}")

    archive_filename = url.split("/")[-1]
    signature_bytes: Optional[bytes] = None
    for line in sig_resp.text.splitlines():
        parts = line.split("\t")
        if len(parts) != 2:
            continue
        if parts[1] == archive_filename:
            import base64

            try:
                signature_bytes = base64.b64decode(parts[0])
            except Exception as e:
                raise JSockDClientError(f"decode signature: {e}") from e
            break
    if signature_bytes is None:
        raise JSockDClientError(f"signature not found for {archive_filename}")

    # Download archive
    try:
        resp = requests.get(url, timeout=60)
    except Exception as e:
        raise JSockDClientError(f"download {url}: {e}") from e
    if resp.status_code != 200:
        raise _DownloadError(f"failed to download file: unexpected HTTP status {resp.status_code} for {url}")

    archive_data = resp.content

    # Verify signature (detached)
    try:
        pubkey = bytes.fromhex(_JSOCKD_BINARY_PUBLIC_KEY)
        ok = nacl_bindings.crypto_sign_verify_detached(signature_bytes, archive_data, pubkey)
    except Exception as e:
        raise JSockDClientError(f"signature verification error: {e}") from e
    if not ok:
        raise JSockDClientError("signature verification failed")

    # Extract jsockd binary to temp dir
    tmpdir = tempfile.mkdtemp(prefix="jsockd-")
    jsockd_path = os.path.join(tmpdir, "jsockd")

    try:
        with io.BytesIO(archive_data) as bio, tarfile.open(fileobj=bio, mode="r:gz") as tf:
            member: Optional[tarfile.TarInfo] = None
            for ti in tf:
                if ti.isfile() and os.path.basename(ti.name) == "jsockd":
                    member = ti
                    break
            if member is None:
                raise JSockDClientError("jsockd binary not found in archive")
            with tf.extractfile(member) as src, open(jsockd_path, "wb") as dst:
                if src is None:
                    raise JSockDClientError("failed to extract jsockd binary")
                shutil.copyfileobj(src, dst)
        os.chmod(jsockd_path, os.stat(jsockd_path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except Exception:
        shutil.rmtree(tmpdir, ignore_errors=True)
        raise

    return jsockd_path


def init_jsockd_client_via_auto_download(config: Config) -> JSockDClient:
    jsockd_path = _download_and_verify_jsockd()
    client = init_jsockd_client(config, jsockd_path)
    # Mark the exec path for cleanup on close
    client._iclient.auto_downloaded_exec = jsockd_path
    return client


# Optional conveniences mirroring Go-style free functions
