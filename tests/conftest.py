# SPDX-License-Identifier: LGPL-2.1
"""pytest fixtures for cxl-fmd Redfish testing."""


def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line(
        "markers", "mock_topology(path): specify mock topology JSON file for test"
    )
    config.addinivalue_line(
        "markers", "port(num): specify port number for daemon"
    )
    config.addinivalue_line(
        "markers", "slow: marks tests as slow (deselect with '-m \"not slow\"')"
    )
    config.addinivalue_line(
        "markers", "daemon_args(*args): more command line arguments for the daemon, "
        "such as --allow-mctp-restricted"
    )


import os
import subprocess
import tempfile
import sys
import time
import urllib.request
import urllib.error
import venv
from pathlib import Path

import pytest
import requests

import redfish_messages


@pytest.fixture(autouse=True)
def error_responses_checked(monkeypatch):
    """
    Every error response a test gets through requests, with a JSON error
    body, is checked against the DMTF Base registry (redfish_messages.py):
    each test is a test of the errors it meets (review F18 claim 2).
    """
    send = requests.adapters.HTTPAdapter.send

    def checked_send(self, request, *args, **kwargs):
        response = send(self, request, *args, **kwargs)
        if response.status_code >= 400:
            try:
                body = response.json()
            except ValueError:
                body = None
            if isinstance(body, dict) and "error" in body:
                try:
                    redfish_messages.check_error(body)
                except (AssertionError, KeyError, TypeError) as e:
                    pytest.fail(f"{request.method} {request.url}: "
                                f"{response.status_code} error not as the "
                                f"Base registry has it: {e!r}\n{response.text}")
        return response

    monkeypatch.setattr(requests.adapters.HTTPAdapter, "send", checked_send)

# Project root (parent of tests/)
PROJECT_ROOT = Path(__file__).parent.parent.resolve()

# Default startup timeout
STARTUP_TIMEOUT = 30

# Virtual environment for validator
VENV_DIR = PROJECT_ROOT / ".venv-validator"

# the version of the DMTF Redfish Service Validator that is installed there:
# one that a test passed with, so that a new release does not change what
# passes without anything here having changed
VALIDATOR_VERSION = "3.1.8"

# the version of the DMTF Redfish-Protocol-Validator installed next to it
# (test_protocol_validator.py), pinned for the same reason. It needs
# redfish_service_validator>=3.1.6, which the pin above meets.
PROTOCOL_VALIDATOR_VERSION = "1.3.2"


class ValidatorVenv:
    """Manages virtual environment for Redfish validator."""

    def __init__(self, venv_path):
        self.venv_path = Path(venv_path)
        self.python = self.venv_path / "bin" / "python"
        self.validator = self.venv_path / "bin" / "rf_service_validator"
        self.protocol_validator = self.venv_path / "bin" / "rf_protocol_validator"

    def exists(self):
        """Check if venv exists with validator installed."""
        return self.python.exists() and self.validator.exists()

    def install(self):
        """Install both validators, at their pinned versions."""
        print("Installing redfish-service-validator and redfish-protocol-validator...")
        subprocess.run(
            [str(self.python), "-m", "pip", "install", "-q",
             "redfish-service-validator==" + VALIDATOR_VERSION,
             "redfish-protocol-validator==" + PROTOCOL_VALIDATOR_VERSION],
            check=True,
        )

    def create(self):
        """Create virtual environment and install the validators."""
        print(f"Creating virtual environment at {self.venv_path}")
        venv.create(self.venv_path, with_pip=True)
        self.install()

    def ensure(self):
        """Ensure venv exists with validator installed."""
        if not self.exists():
            self.create()
        return self

    def ensure_protocol(self):
        """Ensure the Protocol Validator is installed too: a venv made
        before it was is given it (which needs network)."""
        self.ensure()
        if not self.protocol_validator.exists():
            self.install()
        return self


@pytest.fixture(scope="session")
def project_root():
    """Return project root directory."""
    return PROJECT_ROOT


@pytest.fixture(scope="session")
def schema_dir(project_root):
    """Return CSDL/XML schema directory for validator."""
    return project_root / "schema" / "dmtf" / "csdl"


@pytest.fixture(scope="session")
def validator_venv():
    """Ensure validator virtual environment is set up and return it."""
    venv_mgr = ValidatorVenv(VENV_DIR)
    venv_mgr.ensure()
    return venv_mgr


@pytest.fixture(scope="session")
def protocol_validator_venv():
    """Ensure the Protocol Validator is installed in the validator virtual
    environment and return it."""
    return ValidatorVenv(VENV_DIR).ensure_protocol()


@pytest.fixture(scope="session")
def cxl_fmd_executable(project_root):
    """Find and return cxl-fmd executable path."""
    # Check MESON_BUILD_ROOT env var first (set by meson test)
    build_root = os.environ.get("MESON_BUILD_ROOT")

    search_paths = []
    if build_root:
        search_paths.append(Path(build_root) / "cxl-fmd")

    search_paths.extend([
        project_root / "build" / "cxl-fmd",
        project_root / "builddir" / "cxl-fmd",
    ])

    for path in search_paths:
        if path.exists():
            return path

    pytest.skip(f"cxl-fmd executable not found in {search_paths}")


def _wait_for_service(url, timeout=STARTUP_TIMEOUT):
    """Wait for HTTP service to become available."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            with urllib.request.urlopen(url, timeout=2) as resp:
                if resp.status == 200:
                    return True
        except (urllib.error.URLError, Exception):
            pass
        time.sleep(0.5)
    return False


def _find_free_port():
    """Find an available port for testing."""
    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('', 0))
        return s.getsockname()[1]


@pytest.fixture
def cxl_fmd_daemon(cxl_fmd_executable, request):
    """
    Start cxl-fmd daemon with mock topology.

    Use markers to customize:
        @pytest.mark.mock_topology("mock/mld-device.json")
        @pytest.mark.port(8081)
        @pytest.mark.daemon_args("--allow-mctp-restricted")
    """
    # Get mock topology from test marker or use default
    marker = request.node.get_closest_marker("mock_topology")
    mock_file = marker.args[0] if marker else "mock/single-sld.json"
    mock_path = PROJECT_ROOT / mock_file

    # Get port from marker or find a free one
    port_marker = request.node.get_closest_marker("port")
    port = port_marker.args[0] if port_marker else _find_free_port()

    args_marker = request.node.get_closest_marker("daemon_args")
    extra = list(args_marker.args) if args_marker else []

    # Start daemon. Its output goes to a file: a pipe nobody reads fills up
    # and then blocks the daemon in the middle of a test.
    log = tempfile.TemporaryFile()
    process = subprocess.Popen(
        [
            str(cxl_fmd_executable),
            "--mock", str(mock_path),
            "--port", str(port),
            *extra,
        ],
        stdout=log,
        stderr=subprocess.STDOUT,
    )

    def output():
        log.seek(0)
        return log.read().decode(errors="replace")

    # Wait for ready
    url = f"http://localhost:{port}/redfish/v1"
    if not _wait_for_service(url):
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        text = output()
        log.close()
        pytest.fail(f"Daemon did not become ready. Output:\n{text}")

    yield {
        "process": process,
        "port": port,
        "url": f"http://localhost:{port}",
        "mock_topology": str(mock_path),
        "output": output,
    }

    # Cleanup. A daemon that died during the test, or that does not shut
    # down cleanly when asked to, fails the test: a crash, an assertion or
    # a sanitizer report is a failure even if every request was answered.
    died = process.poll()
    process.terminate()
    try:
        rc = process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        rc = "killed: it did not exit within 10 seconds of SIGTERM"
    text = output()
    log.close()

    if died is not None:
        pytest.fail(f"cxl-fmd exited during the test (status {died}). Output:\n{text[-4000:]}")
    if rc != 0:
        pytest.fail(f"cxl-fmd did not shut down cleanly (status {rc}). Output:\n{text[-4000:]}")


class RedfishClient:
    """HTTP client wrapper for Redfish API testing."""

    def __init__(self, base_url):
        self.base_url = base_url

    def get(self, path):
        """GET request returning (status_code, json_data)."""
        resp = requests.get(f"{self.base_url}{path}")
        try:
            data = resp.json() if resp.content else None
        except ValueError:
            data = None
        return resp.status_code, data

    def post(self, path, body, raw_body=None):
        """POST request returning (status_code, json_data, location_header).

        If raw_body is provided, it is sent as-is (for fuzzing malformed JSON).
        """
        if raw_body is not None:
            resp = requests.post(
                f"{self.base_url}{path}",
                data=raw_body,
                headers={"Content-Type": "application/json"}
            )
        else:
            resp = requests.post(f"{self.base_url}{path}", json=body)
        try:
            data = resp.json() if resp.content else None
        except ValueError:
            data = None
        return resp.status_code, data, resp.headers.get('Location')

    def delete(self, path):
        """DELETE request returning status_code."""
        return requests.delete(f"{self.base_url}{path}").status_code

    def patch(self, path, body):
        """PATCH request returning (status_code, json_data)."""
        resp = requests.patch(f"{self.base_url}{path}", json=body)
        try:
            data = resp.json() if resp.content else None
        except ValueError:
            data = None
        return resp.status_code, data


@pytest.fixture
def api_client(cxl_fmd_daemon):
    """
    HTTP client for Redfish API testing.

    Provides get(), post(), delete(), patch() methods with automatic
    JSON handling. Uses the daemon's base URL.
    """
    return RedfishClient(cxl_fmd_daemon['url'])
