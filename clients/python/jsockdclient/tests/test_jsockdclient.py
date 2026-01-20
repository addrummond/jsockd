import os
import platform
import sys
from pathlib import Path
from typing import Optional

import pytest

# Ensure the package src/ is on sys.path for tests
THIS_DIR = Path(__file__).resolve().parent
PKG_ROOT = THIS_DIR.parent
SRC_DIR = PKG_ROOT / "src"
if str(SRC_DIR) not in sys.path:
    sys.path.insert(0, str(SRC_DIR))

# Import the python client
try:
    from jsockdclient import (
        DefaultConfig,
        JSockDClient,
        JSockDClientError,
        JSockDJSException,
        init_jsockd_client,
        init_jsockd_client_via_auto_download,
    )
except Exception as e:  # pragma: no cover
    pytest.skip(f"Failed to import jsockdclient: {e}", allow_module_level=True)


def _has_auto_download_deps() -> bool:
    try:
        import requests  # noqa: F401
        from nacl import bindings as _  # noqa: F401
        return True
    except Exception:
        return False


def _make_client() -> Optional[JSockDClient]:
    """
    Create a JSockD client either from JSOCKD env var or via auto-download.
    Returns None if we should skip the test (e.g., missing deps or unsupported OS).
    """
    # Only support POSIX (Unix domain sockets)
    if platform.system().lower().startswith("win"):
        return None

    config = DefaultConfig()
    config.SkipJSockDVersionCheck = True
    config.NThreads = 1

    jsockd = os.getenv("JSOCKD")
    if jsockd:
        try:
            return init_jsockd_client(config, jsockd)
        except Exception:
            return None

    # Try auto-download if deps available
    if not _has_auto_download_deps():
        return None

    try:
        return init_jsockd_client_via_auto_download(config)
    except Exception:
        return None


@pytest.fixture(scope="module")
def client():
    c = _make_client()
    if c is None:
        pytest.skip(
            "JSockD not available. Set JSOCKD env var to a local jsockd binary or allow auto-download with dependencies."
        )
    try:
        yield c
    finally:
        try:
            c.close()
        except JSockDClientError:
            # It's okay if the process already exited unexpectedly; tests are done.
            pass
        except Exception:
            pass


def test_send_raw_command_good(client: JSockDClient):
    raw_json = client.run_raw("(m, p) => p+1", "99")
    assert raw_json.strip() == "100"


def test_send_raw_command_with_message_handler(client: JSockDClient):
    messages = []

    def on_message(msg_json: str) -> str:
        messages.append(msg_json)
        if len(messages) == 1:
            # Expect "foo"
            assert msg_json == "\"foo\""
            return "\"ack-1\""
        elif len(messages) == 2:
            # Expect "bar"
            assert msg_json == "\"bar\""
            return "\"ack-2\""
        # Should not happen
        return "null"

    raw_json = client.run_raw(
        "(m, p) => { JSockD.sendMessage(\"foo\"); return JSockD.sendMessage(\"bar\"); }",
        "99",
        message_handler=on_message,
    )
    assert len(messages) == 2
    assert raw_json == "\"ack-2\""


def test_send_raw_command_bad(client: JSockDClient):
    with pytest.raises(JSockDJSException):
        client.run_raw("(m, p) => p.foo()", "99")


def test_send_command_good(client: JSockDClient):
    result = client.run("(m, p) => p+1", 99)
    assert result == 100


def test_send_command_bad(client: JSockDClient):
    with pytest.raises(JSockDJSException):
        client.run("(m, p) => p.foo()", 99)
