#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1
"""
DMTF Redfish Service Validator runner for cxl-fmd.

This script:
1. Sets up a virtual environment with required packages
2. Starts cxl-fmd with a mock topology
3. Waits for service readiness
4. Runs the Redfish Service Validator
5. Reports results and exits with appropriate code

Usage:
    python scripts/run_redfish_validator.py
    python scripts/run_redfish_validator.py --mock-topology mock/mld-device.json
    python scripts/run_redfish_validator.py --build-dir build --output-dir logs
"""

import argparse
import os
import subprocess
import sys
import time
import socket
import venv
from pathlib import Path
from contextlib import contextmanager
import urllib.request
import urllib.error

# Default configuration
DEFAULT_PORT = 8080
DEFAULT_TIMEOUT = 300  # 5 minutes for full validation
STARTUP_TIMEOUT = 30   # 30 seconds for daemon startup

PROJECT_ROOT = Path(__file__).parent.parent.resolve()
DEFAULT_MOCK_TOPOLOGY = PROJECT_ROOT / "mock" / "single-sld.json"
SCHEMA_DIR = PROJECT_ROOT / "schema" / "dmtf" / "csdl"  # CSDL/XML schemas for validator
VENV_DIR = PROJECT_ROOT / ".venv-validator"


class VenvManager:
    """Manages virtual environment for validator."""

    def __init__(self, venv_path):
        self.venv_path = Path(venv_path)
        self.python = self.venv_path / "bin" / "python"
        self.validator = self.venv_path / "bin" / "rf_service_validator"

    def exists(self):
        """Check if venv exists with validator installed."""
        return self.python.exists() and self.validator.exists()

    def create(self):
        """Create virtual environment and install validator."""
        print(f"Creating virtual environment at {self.venv_path}")
        venv.create(self.venv_path, with_pip=True)

        print("Installing redfish-service-validator...")
        subprocess.run(
            [str(self.python), "-m", "pip", "install", "-q",
             "redfish-service-validator"],
            check=True,
        )

    def ensure(self):
        """Ensure venv exists with validator installed."""
        if not self.exists():
            self.create()
        return self


class DaemonManager:
    """Manages cxl-fmd daemon lifecycle."""

    def __init__(self, executable, mock_topology, port, verbose=False):
        self.executable = executable
        self.mock_topology = mock_topology
        self.port = port
        self.verbose = verbose
        self.process = None

    def start(self):
        """Start the daemon process."""
        cmd = [
            str(self.executable),
            "--mock", str(self.mock_topology),
            "--port", str(self.port),
        ]
        if self.verbose:
            cmd.append("-v")

        self.process = subprocess.Popen(
            cmd,
            stdout=None if self.verbose else subprocess.PIPE,
            stderr=None if self.verbose else subprocess.STDOUT,
        )
        return self.process

    def wait_ready(self, timeout=STARTUP_TIMEOUT):
        """Wait for the service to become ready."""
        start = time.time()
        url = f"http://localhost:{self.port}/redfish/v1"

        while time.time() - start < timeout:
            # Check if process died
            if self.process.poll() is not None:
                stdout = self.process.stdout.read() if self.process.stdout else b""
                raise RuntimeError(
                    f"Daemon exited prematurely with code {self.process.returncode}\n"
                    f"Output: {stdout.decode()}"
                )

            try:
                req = urllib.request.Request(url)
                with urllib.request.urlopen(req, timeout=2) as resp:
                    if resp.status == 200:
                        return True
            except (urllib.error.URLError, socket.timeout):
                pass

            time.sleep(0.5)

        raise TimeoutError(f"Daemon did not become ready within {timeout}s")

    def stop(self):
        """Stop the daemon gracefully."""
        if self.process and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()


@contextmanager
def managed_daemon(executable, mock_topology, port, verbose=False):
    """Context manager for daemon lifecycle."""
    daemon = DaemonManager(executable, mock_topology, port, verbose)
    try:
        daemon.start()
        daemon.wait_ready()
        yield daemon
    finally:
        daemon.stop()


def run_validator(venv_mgr, port, schema_dir=None, output_dir=None,
                  timeout=DEFAULT_TIMEOUT):
    """Run the Redfish Service Validator."""
    cmd = [
        str(venv_mgr.validator),
        "-r", f"http://localhost:{port}",
        "--user", "none", "--password", "none", "--authtype", "Basic",
        "--nooemcheck",
    ]

    if schema_dir and schema_dir.exists():
        cmd.extend(["--schema_directory", str(schema_dir)])

    if output_dir:
        cmd.extend(["--logdir", str(output_dir)])

    result = subprocess.run(
        cmd,
        timeout=timeout,
    )

    return result


def find_executable(build_dir=None):
    """Find the cxl-fmd executable."""
    search_paths = []

    if build_dir:
        search_paths.append(Path(build_dir) / "cxl-fmd")

    # Common build directory locations
    search_paths.extend([
        PROJECT_ROOT / "build" / "cxl-fmd",
        PROJECT_ROOT / "builddir" / "cxl-fmd",
        PROJECT_ROOT / "_build" / "cxl-fmd",
    ])

    for path in search_paths:
        if path.exists() and path.is_file():
            return path

    raise FileNotFoundError(
        f"Could not find cxl-fmd executable. Searched: {search_paths}"
    )


def find_free_port():
    """Find an available port."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('', 0))
        return s.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(
        description="Run DMTF Redfish Service Validator against cxl-fmd"
    )
    parser.add_argument(
        "--build-dir", "-b",
        help="Build directory containing cxl-fmd executable"
    )
    parser.add_argument(
        "--mock-topology", "-m",
        type=Path,
        default=DEFAULT_MOCK_TOPOLOGY,
        help="Mock topology JSON file"
    )
    parser.add_argument(
        "--port", "-p",
        type=int,
        default=0,
        help="Port for Redfish service (0 = auto)"
    )
    parser.add_argument(
        "--output-dir", "-o",
        type=Path,
        help="Output directory for validator logs"
    )
    parser.add_argument(
        "--timeout", "-t",
        type=int,
        default=DEFAULT_TIMEOUT,
        help="Validation timeout in seconds"
    )
    parser.add_argument(
        "--verbose", "-v",
        action="store_true",
        help="Verbose output"
    )
    parser.add_argument(
        "--venv-dir",
        type=Path,
        default=VENV_DIR,
        help="Virtual environment directory"
    )

    args = parser.parse_args()

    # Find free port if not specified
    port = args.port if args.port else find_free_port()

    try:
        # Ensure virtual environment is set up
        venv_mgr = VenvManager(args.venv_dir).ensure()

        executable = find_executable(args.build_dir)
        print(f"Using executable: {executable}")
        print(f"Using mock topology: {args.mock_topology}")
        print(f"Using schema directory: {SCHEMA_DIR}")
        print(f"Using port: {port}")

        with managed_daemon(executable, args.mock_topology, port, args.verbose):
            print(f"Daemon started on port {port}")
            print("Running Redfish Service Validator...")
            print()

            result = run_validator(
                venv_mgr,
                port,
                schema_dir=SCHEMA_DIR,
                output_dir=args.output_dir,
                timeout=args.timeout,
            )

            # Validator exit codes:
            # 0 = pass
            # 1 = fail (validation errors)
            # Other = tool error
            sys.exit(result.returncode)

    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(2)
    except TimeoutError as e:
        print(f"Timeout: {e}", file=sys.stderr)
        sys.exit(3)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(4)


if __name__ == "__main__":
    main()
