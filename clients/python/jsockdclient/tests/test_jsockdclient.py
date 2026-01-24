import os
import platform
import sys
from pathlib import Path
from typing import Iterator, Optional

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
        Config,
        JSockDClient,
        JSockDClientError,
        JSockDJSException,
    )
except Exception as e:  # pragma: no cover
    pytest.skip(f"Failed to import jsockdclient: {e}", allow_module_level=True)


# Optional dependency check removed: requests and pynacl are now required and installed via uv


def _make_client() -> Optional[JSockDClient]:
    """
    Create a JSockD client either from JSOCKD env var or via auto-download.
    Returns None if we should skip the test (e.g., missing deps or unsupported OS).
    """
    # Only support POSIX (Unix domain sockets)
    if platform.system().lower().startswith("win"):
        return None

    config = Config()
    config.skip_jsockd_version_check = True
    config.n_threads = 1

    jsockd = os.getenv("JSOCKD")
    if jsockd:
        try:
            return JSockDClient(config, jsockd_exec=jsockd)
        except Exception:
            return None

    # Always try auto-download

    try:
        return JSockDClient(config, autodownload=True)
    except Exception:
        return None


@pytest.fixture(scope="module")
def client() -> Iterator[JSockDClient]:
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
    response = client.send_raw_command("(m, p) => p+1", "99")
    assert not response.exception
    assert response.result_json.strip() == "100"


def test_send_raw_command_with_message_handler(client: JSockDClient):
    messages: list[str] = []

    def on_message(msg_json: str) -> str:
        messages.append(msg_json)
        if len(messages) == 1:
            # Expect "foo"
            assert msg_json == '"foo"'
            return '"ack-1"'
        elif len(messages) == 2:
            # Expect "bar"
            assert msg_json == '"bar"'
            return '"ack-2"'
        # Should not happen
        return "null"

    response = client.send_raw_command(
        '(m, p) => { JSockD.sendMessage("foo"); return JSockD.sendMessage("bar"); }',
        "99",
        message_handler=on_message,
    )
    assert len(messages) == 2
    assert not response.exception
    assert response.result_json == '"ack-2"'


def test_send_raw_command_bad(client: JSockDClient):
    result = client.send_raw_command("(m, p) => p.foo()", "99")
    assert result.exception
    assert "errorMessage" in result.result_json


def test_send_command_good(client: JSockDClient):
    result = client.send_command("(m, p) => p+1", 99)
    assert not result.exception
    assert result.result == 100


def test_send_command_bad(client: JSockDClient):
    result = client.send_command("(m, p) => p.foo()", 99)
    assert result.exception
    assert "errorMessage" in result.raw_response.result_json
