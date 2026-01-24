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
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Generic, Optional, TypeVar, cast

import requests
from dateutil.parser import isoparse
from nacl import (
    bindings as nacl_bindings,
)

del enum


__all__ = [
    "JSOCKD_VERSION",
    "Config",
    "JSockDClient",
    "JSockDClientError",
    "JSockDJSException",
]

# __jsockd_version__ <- this comment triggers a CI check to keep the version in sync
JSOCKD_VERSION = "0.0.139"

# Protocol tokens
_MESSAGE_HANDLER_INTERNAL_ERROR = "internal_error"

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
    """Configuration for the JSockD client.

    Fields:
      - n_threads (int): Number of worker threads; defaults to the number of CPU cores (>= 1).
      - bytecode_module_file (str): Path to the bytecode module to load, or "" for none.
      - bytecode_module_public_key (str): Hex-encoded public key used to verify the module signature, or "" for none.
      - souce_map (str): Path to the source map to load, or "" for none.
      - max_idle_time_us (int): Maximum idle time per connection in microseconds before its QuickJS instance is shut down; 0 disables idle timeout.
      - max_command_runtime_us (int): Maximum allowed runtime for a command in microseconds; 0 uses the default max.
      - timeout_us (int): I/O timeout in microseconds when communicating with JSockD.
      - skip_jsockd_version_check (bool): If true, skip checking server version against the client’s expected version. Keep false in production.
      - logger (Optional[Callable[[Optional[datetime], str, str], None]]): Callback for JSockD log messages; timestamp may be None if parsing fails.
      - max_restarts_per_minute (int): Max times the jsockd process will be restarted within one minute if it crashes; 0 disables restarts.
      - log_prefix (str): Value for the JSOCKD_LOG_PREFIX environment variable.

    Use Config() for sensible defaults and override fields as needed.
    """

    n_threads: int = field(default_factory=lambda: os.cpu_count() or 1)
    bytecode_module_file: str = ""
    bytecode_module_public_key: str = ""
    souce_map: str = ""
    max_idle_time_us: int = 0
    max_command_runtime_us: int = 0
    timeout_us: int = 15_000_000  # 15 seconds
    skip_jsockd_version_check: bool = False
    logger: Optional[Callable[[Optional[datetime], str, str], None]] = field(
        default_factory=lambda: (lambda ts, level, msg: print(f"[jsockd] {msg}"))
    )
    max_restarts_per_minute: int = 1
    log_prefix: str = ""


@dataclass(slots=True)
class RawResponse:
    """
    Raw response from JSockD."""

    exception: bool
    result_json: str


@dataclass(slots=True)
class Response(Generic[T]):
    """
    Parsed response from JSockD. Check raw_response for information about the error if Exception is True."""

    exception: bool
    result: T
    raw_response: RawResponse


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
    next_cmd_id: itertools.count[int]
    choose_index: itertools.count[int]
    restart_count: int
    last_restart_time: float
    restart_guard: threading.Lock
    auto_downloaded_exec: Optional[str]


def _stream_stderr_logs(stderr: io.TextIOBase, config: Config) -> None:
    zero_time: Optional[datetime] = None
    current_line: list[str] = []
    last_was_dollar = True
    for raw in stderr:
        line = raw.strip()
        m = _LOG_LINE_RE.match(line)
        if not m:
            if config.logger:
                config.logger(zero_time, "INFO", line)
            continue
        prefix, timestamp, level, msg = m.groups()
        t = isoparse(timestamp)
        if prefix == "*":
            if current_line:
                current_line.append("\n")
            current_line.append(msg)
        elif prefix == "$":
            if not last_was_dollar:
                current_line.append("\n")
            last_was_dollar = False
            current_line.append(msg)
            if config.logger:
                config.logger(t, level, "".join(current_line))
            current_line.clear()
        else:
            if config.logger:
                config.logger(zero_time, "INFO", line)


def _read_ready_from_stdout(
    stdout: io.TextIOBase,
    ready_q: "queue.Queue[int]",
    err_q: "queue.Queue[BaseException]",
    config: Config,
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
                    if (
                        not config.skip_jsockd_version_check
                        and version != JSOCKD_VERSION
                    ):
                        err_q.put(
                            JSockDClientError(
                                f"version mismatch: client {JSOCKD_VERSION!r}, server {version!r}"
                            )
                        )
                        return
                    ready_q.put(n)
                    return
                err_q.put(JSockDClientError(f"malformed READY line: {line!r}"))
                return
        # EOF
        err_q.put(
            JSockDClientError(
                "stdout closed before READY line; check jsockd logs for error"
            )
        )
    except Exception as e:
        err_q.put(e)


def _prepare_cmd_args(config: Config, sockets: list[str]) -> list[str]:
    args: list[str] = ["-b", "00"]
    if config.bytecode_module_file:
        args += ["-m", config.bytecode_module_file]
    if config.souce_map:
        args += ["-sm", config.souce_map]
    if config.max_idle_time_us:
        args += ["-i", str(config.max_idle_time_us)]
    if config.max_command_runtime_us:
        args += ["-t", str(config.max_command_runtime_us)]
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


def _conn_handler(
    conn: socket.socket,
    cmd_q: "queue.Queue[Optional[_Command]]",
    iclient: _InternalClient,
) -> None:
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
                _set_fatal_error(
                    iclient,
                    JSockDClientError(f"malformed or mismatched response: {rec!r}"),
                )
                return

            rest = parts[1]
            if rest.startswith("exception "):
                cmd.response_q.put(
                    RawResponse(
                        exception=True,
                        result_json=rest[len("exception ") :].rstrip("\n"),
                    )
                )
                continue
            elif rest.startswith("ok "):
                cmd.response_q.put(
                    RawResponse(
                        exception=False, result_json=rest[len("ok ") :].rstrip("\n")
                    )
                )
                continue
            elif rest.startswith("message "):
                # Handle message exchange loop
                message_rest = rest[len("message ") :]
                while True:
                    response_json = "null"
                    handler_err: Optional[BaseException] = JSockDClientError(
                        "no message handler"
                    )
                    if cmd.message_handler is not None:
                        try:
                            response_json = cmd.message_handler(
                                message_rest.rstrip("\n")
                            )
                            handler_err = None
                        except BaseException as e:
                            handler_err = e
                    if handler_err is not None:
                        _set_fatal_error(
                            iclient,
                            JSockDClientError(f"message handler error: {handler_err}"),
                        )
                        try:
                            wfile.write(
                                f"{cmd.id}\x00{_MESSAGE_HANDLER_INTERNAL_ERROR}\x00".encode(
                                    "utf-8"
                                )
                            )
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
                            JSockDClientError(
                                f"mismatched or malformed message response: {mresp!r}, want id {cmd.id!r}"
                            ),
                        )
                        return
                    if mparts[1].startswith("ok "):
                        cmd.response_q.put(
                            RawResponse(
                                exception=False, result_json=mparts[1][3:].rstrip("\n")
                            )
                        )
                        break
                    elif mparts[1].startswith("exception "):
                        cmd.response_q.put(
                            RawResponse(
                                exception=True,
                                result_json=mparts[1][len("exception ") :].rstrip("\n"),
                            )
                        )
                        break
                    elif mparts[1].startswith("message "):
                        message_rest = mparts[1][len("message ") :]
                        continue
                    else:
                        _set_fatal_error(
                            iclient,
                            JSockDClientError(
                                f"malformed message response from JSockD: {mresp!r}"
                            ),
                        )
                        return
            else:
                _set_fatal_error(
                    iclient,
                    JSockDClientError(
                        f"malformed command response from JSockD: {rec!r}"
                    ),
                )
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


def _start_jsockd_process(
    jsockd_exec: str, config: Config, sockets: list[str]
) -> subprocess.Popen[str]:
    args = [jsockd_exec] + _prepare_cmd_args(config, sockets)
    env = os.environ.copy()
    if config.log_prefix:
        env["JSOCKD_LOG_PREFIX"] = config.log_prefix
    if config.bytecode_module_public_key:
        env["JSOCKD_BYTECODE_MODULE_PUBLIC_KEY"] = config.bytecode_module_public_key

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
    def __init__(
        self,
        config: Config,
        jsockd_exec: Optional[str] = None,
        autodownload: bool = False,
    ):
        """Create a JSockD client. Either provide jsockd_exec path or set autodownload=True to download the appropriate binary."""
        if config.n_threads <= 0:
            raise JSockDClientError("NThreads must be greater than zero.")

        # Create short socket dir to avoid UDS path len issues
        socket_tmpdir = tempfile.mkdtemp(prefix="jsd_")
        sockets: list[str] = [
            str(Path(socket_tmpdir) / f"jsd_{i}.sock") for i in range(config.n_threads)
        ]

        # Resolve jsockd executable (explicit or autodownload)
        if autodownload:
            jsockd_exec_resolved = _download_and_verify_jsockd()
            auto_downloaded: Optional[str] = jsockd_exec_resolved
        else:
            if not jsockd_exec:
                raise JSockDClientError("Provide jsockd_exec or set autodownload=True")
            jsockd_exec_resolved = jsockd_exec
            auto_downloaded = None

        # Start process
        proc = _start_jsockd_process(jsockd_exec_resolved, config, sockets)

        # Setup readers
        assert proc.stdout is not None and proc.stderr is not None
        ready_q: "queue.Queue[int]" = queue.Queue()
        err_q: "queue.Queue[BaseException]" = queue.Queue()

        t_ready = threading.Thread(
            target=_read_ready_from_stdout,
            args=(proc.stdout, ready_q, err_q, config),
            name="jsockd-ready",
            daemon=True,
        )
        t_ready.start()

        t_logs = threading.Thread(
            target=_stream_stderr_logs,
            args=(proc.stderr, config),
            name="jsockd-logs",
            daemon=True,
        )
        t_logs.start()

        # Wait for READY or error/timeout
        ready_count = 0
        try:
            timeout_s = (
                config.timeout_us / 1_000_000.0 if config.timeout_us > 0 else 15.0
            )
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
                    raise JSockDClientError(
                        f"timeout ({timeout_s:.3f}s) waiting for READY line"
                    )
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
                s = _dial_unix(sockets[i], config.timeout_us)
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
            restart_count=0,
            last_restart_time=0.0,
            restart_guard=threading.Lock(),
            auto_downloaded_exec=auto_downloaded,
        )

        # Start connection workers
        for i in range(ready_count):
            threading.Thread(
                target=_conn_handler,
                args=(conns[i], ic.cmd_queues[i], ic),
                daemon=True,
                name=f"jsockd-conn-{i}",
            ).start()

        # Monitor process exit with bounded auto-restart
        def _reaper() -> None:
            nonlocal proc
            while not ic.quit_event.is_set():
                rc = proc.wait()
                if ic.quit_event.is_set():
                    break
                if rc == 0:
                    # Clean shutdown; propagate quit and cleanup
                    ic.quit_event.set()
                    try:
                        shutil.rmtree(ic.socket_tmpdir, ignore_errors=True)
                    except Exception:
                        pass
                    break

                # Unexpected exit; try bounded restart
                now = time.time()
                with ic.restart_guard:
                    # Rolling budget: allow up to MaxRestartsPerMinute per minute elapsed since last_restart_time.
                    minutes_passed = (
                        int((now - ic.last_restart_time) / 60.0)
                        if ic.last_restart_time > 0
                        else 0
                    )
                    effective_count = ic.restart_count - (
                        minutes_passed * ic.config.max_restarts_per_minute
                    )
                    if effective_count >= ic.config.max_restarts_per_minute:
                        _set_fatal_error(
                            ic,
                            JSockDClientError(
                                f"jsockd crashed (code {rc}); exceeded MaxRestartsPerMinute={ic.config.max_restarts_per_minute}"
                            ),
                        )
                        ic.quit_event.set()
                        try:
                            shutil.rmtree(ic.socket_tmpdir, ignore_errors=True)
                        except Exception:
                            pass
                        break

                    ic.restart_count += 1
                    ic.last_restart_time = now

                # Attempt restart
                try:
                    # Close existing sockets
                    for s in ic.sockets:
                        with contextlib.suppress(Exception):
                            s.close()
                    ic.sockets.clear()

                    # Start new process
                    new_proc = _start_jsockd_process(
                        jsockd_exec_resolved, ic.config, ic.socket_paths
                    )

                    # Setup readers
                    assert new_proc.stdout is not None and new_proc.stderr is not None
                    ready_q: "queue.Queue[int]" = queue.Queue()
                    err_q: "queue.Queue[BaseException]" = queue.Queue()

                    threading.Thread(
                        target=_read_ready_from_stdout,
                        args=(new_proc.stdout, ready_q, err_q, ic.config),
                        name="jsockd-ready",
                        daemon=True,
                    ).start()

                    threading.Thread(
                        target=_stream_stderr_logs,
                        args=(new_proc.stderr, ic.config),
                        name="jsockd-logs",
                        daemon=True,
                    ).start()

                    # Wait for READY or error/timeout
                    ready_count = 0
                    timeout_s = (
                        ic.config.timeout_us / 1_000_000.0
                        if ic.config.timeout_us > 0
                        else 15.0
                    )
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
                            new_proc.kill()
                            new_proc.wait()
                            raise JSockDClientError(
                                f"waiting for READY (restart): {e}"
                            ) from e
                        except queue.Empty:
                            pass
                        if (time.time() - start) > timeout_s:
                            new_proc.kill()
                            new_proc.wait()
                            raise JSockDClientError(
                                f"timeout ({timeout_s:.3f}s) waiting for READY line during restart"
                            )
                        time.sleep(0.01)

                    if ready_count > len(ic.socket_paths):
                        new_proc.kill()
                        new_proc.wait()
                        raise JSockDClientError(
                            f"ready count ({ready_count}) exceeds number of sockets specified ({len(ic.socket_paths)}) during restart"
                        )

                    # Dial sockets
                    new_conns: list[socket.socket] = []
                    for i in range(ready_count):
                        s = _dial_unix(ic.socket_paths[i], ic.config.timeout_us)
                        new_conns.append(s)

                    # Replace process and sockets
                    ic.process = new_proc
                    ic.sockets = new_conns

                    # Recreate command queues (clear old queues to avoid stale workers)
                    ic.cmd_queues = [queue.Queue() for _ in range(ready_count)]

                    # Start new connection workers
                    for i in range(ready_count):
                        threading.Thread(
                            target=_conn_handler,
                            args=(new_conns[i], ic.cmd_queues[i], ic),
                            daemon=True,
                            name=f"jsockd-conn-restart-{i}",
                        ).start()

                    # Successfully restarted; continue monitoring
                    proc = new_proc
                    continue
                except Exception as e:
                    # Restart failed; record fatal and stop
                    _set_fatal_error(
                        iclient=ic, err=JSockDClientError(f"restart failed: {e}")
                    )
                    ic.quit_event.set()
                    try:
                        shutil.rmtree(ic.socket_tmpdir, ignore_errors=True)
                    except Exception:
                        pass
                    break

        threading.Thread(target=_reaper, daemon=True, name="jsockd-reaper").start()

        # Store internal client and locks
        self._iclient = ic
        self._close_lock = threading.Lock()
        self._closed = False

    @property
    def process(self) -> subprocess.Popen[str]:
        return self._iclient.process

    def get_jsockd_process(self) -> subprocess.Popen[str]:
        return self._iclient.process

    def set_auto_downloaded_exec(self, path: str) -> None:
        self._iclient.auto_downloaded_exec = path

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
                timeout_s = (
                    ic.config.timeout_us / 1_000_000.0
                    if ic.config.timeout_us > 0
                    else 5.0
                )
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
        """
        Send a raw JSON command to JSockD."""
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

        timeout_s = (
            ic.config.timeout_us / 1_000_000.0 if ic.config.timeout_us > 0 else None
        )
        try:
            return resp_q.get(timeout=timeout_s)
        except queue.Empty:
            raise JSockDClientError("timeout waiting for response")

    def send_command(
        self,
        query: str,
        json_param: Any,
        message_handler: Optional[Callable[[Any], Any]] = None,
    ) -> Response[Any]:
        """
        Send a command to JSockD with JSON marshalling/unmarshalling handled by json.dumps and json.loads."""
        msg_handler_err: list[Optional[BaseException]] = [None]

        def _wrapped(json_message: str) -> str:
            try:
                message = json.loads(json_message)
            except Exception as e:
                msg_handler_err[0] = e
                raise
            try:
                response = message_handler(message) if message_handler else None
            except Exception as e:
                msg_handler_err[0] = e
                raise
            try:
                return json.dumps(response, separators=(",", ":"))
            except Exception as e:
                msg_handler_err[0] = e
                raise

        payload = json.dumps(json_param, separators=(",", ":"))
        raw = self.send_raw_command(query, payload, _wrapped)
        if msg_handler_err[0] is not None:
            raise JSockDClientError(f"message handler error: {msg_handler_err[0]}")
        if raw.exception:
            return Response(exception=True, result=None, raw_response=raw)
        try:
            result = json.loads(raw.result_json)
        except Exception as e:
            raise JSockDClientError(f"json unmarshal: {e}") from e
        return Response(exception=False, result=result, raw_response=raw)


_JSOCKD_RELEASE_URL_TEMPL = "https://github.com/addrummond/jsockd/releases/download/vVERSION/jsockd-VERSION-OS-ARCH.tar.gz"
_JSOCKD_SIGNATURE_URL_TEMPL = "https://github.com/addrummond/jsockd/releases/download/vVERSION/ed25519_signatures.txt"
_JSOCKD_BINARY_PUBLIC_KEY = (
    "b136fca8fbfc42fe6dc95dedd035b0b50ad93b6a5d6fcaf8fc0552e9d29ee406"
)


class _DownloadError(JSockDClientError):
    pass


def _download_and_verify_jsockd() -> str:
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

    url = (
        _JSOCKD_RELEASE_URL_TEMPL.replace("VERSION", JSOCKD_VERSION)
        .replace("OS", os_name)
        .replace("ARCH", arch)
    )
    sig_url = _JSOCKD_SIGNATURE_URL_TEMPL.replace("VERSION", JSOCKD_VERSION)

    # Download signatures file
    try:
        sig_resp = requests.get(sig_url, timeout=30)
    except Exception as e:
        raise _DownloadError(f"failed to download file: {sig_url}: {e}") from e
    if sig_resp.status_code != 200:
        raise _DownloadError(
            f"failed to download file: unexpected HTTP status {sig_resp.status_code} for {sig_url}"
        )

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
        raise _DownloadError(
            f"failed to download file: unexpected HTTP status {resp.status_code} for {url}"
        )

    archive_data = resp.content

    # Verify signature (detached)
    try:
        pubkey = bytes.fromhex(_JSOCKD_BINARY_PUBLIC_KEY)
        ok = cast(Any, nacl_bindings).crypto_sign_verify_detached(
            signature_bytes, archive_data, pubkey
        )
    except Exception as e:
        raise JSockDClientError(f"signature verification error: {e}") from e
    if not ok:
        raise JSockDClientError("signature verification failed")

    # Extract jsockd binary to temp dir
    tmpdir = tempfile.mkdtemp(prefix="jsockd-")
    jsockd_path = os.path.join(tmpdir, "jsockd")

    try:
        with (
            io.BytesIO(archive_data) as bio,
            tarfile.open(fileobj=bio, mode="r:gz") as tf,
        ):
            member: Optional[tarfile.TarInfo] = None
            for ti in tf:
                if ti.isfile() and os.path.basename(ti.name) == "jsockd":
                    member = ti
                    break
            if member is None:
                raise JSockDClientError("jsockd binary not found in archive")
            src = tf.extractfile(member)
            if src is None:
                raise JSockDClientError("failed to extract jsockd binary")
            with open(jsockd_path, "wb") as dst:
                shutil.copyfileobj(src, dst)
            src.close()
        os.chmod(
            jsockd_path,
            os.stat(jsockd_path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH,
        )
    except Exception:
        shutil.rmtree(tmpdir, ignore_errors=True)
        raise

    return jsockd_path
