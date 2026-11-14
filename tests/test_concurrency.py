# SPDX-License-Identifier: LGPL-2.1
"""
Several orchestrators at once.

A request reads the fabric, decides, changes it and reads it again; two of
them deciding on the same state would bind one port twice or lose a
connection. The HTTP server has a pool of threads. Requests that only read
run together; one that changes the fabric has it to itself (fabric_lock).
These tests send many at once to see that the outcome is one that some order
of them could have produced.

Run them on a build with -Db_sanitize=thread as well: a daemon that reports
a data race exits with a failure, which fails the test.
"""

import socket
import subprocess
import threading
import time
from pathlib import Path

import pytest
import requests

from test_orchestration import bindings, bound, chunk_of

CONNECTIONS = "/redfish/v1/Fabrics/CXL/Connections"
CLIENTS = 16


def body(base, host, dev, chunk):
    return {
        "ConnectionType": "Memory",
        "Links": {
            "InitiatorEndpoints": [{"@odata.id": f"/redfish/v1/Fabrics/CXL/Endpoints/{host}"}],
            "TargetEndpoints": [{"@odata.id": f"/redfish/v1/Fabrics/CXL/Endpoints/{dev}"}]},
        "MemoryChunkInfo": [{"MemoryChunk": {"@odata.id": chunk}}]}


def together(jobs):
    """Run every job at the same moment, each on a connection of its own."""
    barrier = threading.Barrier(len(jobs))
    results = [None] * len(jobs)

    def run(i, job):
        barrier.wait()
        try:
            results[i] = job()
        except Exception as exc:        # a refused or reset connection
            results[i] = exc

    threads = [threading.Thread(target=run, args=(i, job)) for i, job in enumerate(jobs)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=60)
    assert not any(thread.is_alive() for thread in threads), "a request never returned"
    for result in results:
        assert not isinstance(result, Exception), result
    return results


@pytest.mark.mock_topology("mock/fm-boots-first.json")
class TestRaceForOneDevice:

    def test_one_host_gets_the_sld(self, api_client):
        """Two hosts ask for the same SLD, eight times each, all at once. An
        SLD port is bound to one VCS: one host has it, the other is refused
        every time, and one vPPB is bound."""
        base = api_client.base_url
        chunk = chunk_of(api_client, "sld_0")

        def ask(host):
            return lambda: (host, requests.post(
                base + CONNECTIONS, json=body(base, host, "sld_0", chunk),
                timeout=30).status_code)

        hosts = ["switch_0_host0", "switch_0_host1"]
        results = together([ask(hosts[i % 2]) for i in range(CLIENTS)])

        won = {host for host, status in results if status == 201}
        assert len(won) == 1, results
        winner = won.pop()
        for host, status in results:
            assert status == (201 if host == winner else 409), results

        now = bound(bindings(api_client, "switch_0"))
        assert list(now.values()) == [("BoundPhysicalPort", 2, None)]
        assert next(iter(now))[0] == ("0" if winner == hosts[0] else "1")


@pytest.mark.mock_topology("mock/large-fabric.json")
class TestManyAtOnce:

    def test_eight_lds_to_four_hosts(self, api_client):
        """LDs 8 to 15 of a_mld_0 are unbound. Give two to each of the four
        hosts at once: every request succeeds and every LD is bound once."""
        base = api_client.base_url
        chunks = {ld: chunk_of(api_client, "a_mld_0", ld) for ld in range(8, 16)}
        before = bound(bindings(api_client, "switch_a"))

        def ask(ld):
            host = "switch_a_host%d" % (ld % 4)
            return lambda: requests.post(
                base + CONNECTIONS, json=body(base, host, "a_mld_0", chunks[ld]),
                timeout=30).status_code

        assert together([ask(ld) for ld in range(8, 16)]) == [201] * 8

        now = bound(bindings(api_client, "switch_a"))
        new = sorted(v for k, v in now.items() if k not in before)
        assert new == [("BoundLD", 8, ld) for ld in range(8, 16)]
        # nothing is bound twice
        assert len(set(now.values())) == len(now)
        for (vcs, _), (_, port, ld) in now.items():
            if port == 8 and ld >= 8:
                assert vcs == str(ld % 4)

    def test_create_and_delete_together(self, api_client):
        """Connections made and removed at once leave as many bindings as
        there are connections."""
        base = api_client.base_url
        chunks = {ld: chunk_of(api_client, "a_mld_0", ld) for ld in range(8, 16)}
        before = bound(bindings(api_client, "switch_a"))

        first = []
        for ld in range(8, 12):
            response = requests.post(
                base + CONNECTIONS, timeout=30,
                json=body(base, "switch_a_host%d" % (ld % 4), "a_mld_0", chunks[ld]))
            assert response.status_code == 201
            first.append(response.json()["@odata.id"])

        def remove(uri):
            return lambda: requests.delete(base + uri, timeout=30).status_code

        def create(ld):
            host = "switch_a_host%d" % (ld % 4)
            return lambda: requests.post(
                base + CONNECTIONS, json=body(base, host, "a_mld_0", chunks[ld]),
                timeout=30).status_code

        results = together([remove(uri) for uri in first] +
                           [create(ld) for ld in range(12, 16)])
        assert results == [204] * 4 + [201] * 4

        now = bound(bindings(api_client, "switch_a"))
        new = sorted(v for k, v in now.items() if k not in before)
        assert new == [("BoundLD", 8, ld) for ld in range(12, 16)]

    def test_readers_during_changes(self, api_client):
        """Reads of the fabric while it changes all succeed."""
        base = api_client.base_url
        chunks = {ld: chunk_of(api_client, "a_mld_0", ld) for ld in range(8, 12)}

        def create(ld):
            host = "switch_a_host%d" % (ld % 4)
            return lambda: requests.post(
                base + CONNECTIONS, json=body(base, host, "a_mld_0", chunks[ld]),
                timeout=30).status_code

        def read(path):
            return lambda: requests.get(base + path, timeout=30).status_code

        paths = ["/redfish/v1/Fabrics/CXL/Switches/switch_a",
                 "/redfish/v1/Fabrics/CXL/Switches/switch_a/VCSs/0/VPPBs/3",
                 "/redfish/v1/Fabrics/CXL/Connections",
                 "/redfish/v1/Chassis/a_mld_0/PCIeDevices/a_mld_0"]
        results = together([create(ld) for ld in range(8, 12)] +
                           [read(paths[i % len(paths)]) for i in range(12)])
        assert results == [201] * 4 + [200] * 12

    def test_many_readers(self, api_client):
        """Every kind of resource read at once, several times over, with a
        change now and then: each read is answered, and the same resource
        reads the same when nothing touched it."""
        base = api_client.base_url
        chunks = {ld: chunk_of(api_client, "a_mld_0", ld) for ld in range(8, 10)}
        paths = [
            "/redfish/v1",
            "/redfish/v1/Chassis",
            "/redfish/v1/Chassis/a_sld_0",
            "/redfish/v1/Chassis/a_sld_0/Memory/a_sld_0",
            "/redfish/v1/Chassis/a_mld_0/PCIeDevices/a_mld_0",
            "/redfish/v1/Chassis/a_mld_0/PCIeDevices/a_mld_0/CXLLogicalDevices/3",
            "/redfish/v1/Chassis/a_mld_0/MemoryDomains",
            "/redfish/v1/Fabrics/CXL/Switches/switch_a/Ports",
            "/redfish/v1/Fabrics/CXL/Switches/switch_a/Ports/switch_a_dsp8",
            "/redfish/v1/Fabrics/CXL/Switches/switch_b/VCSs/0/VPPBs/1",
            "/redfish/v1/Fabrics/CXL/Endpoints",
            "/redfish/v1/Fabrics/CXL/Zones",
            "/redfish/v1/Systems",
        ]

        def read(path):
            def job():
                response = requests.get(base + path, timeout=30)
                return path, response.status_code, response.text
            return job

        def create(ld):
            host = "switch_a_host%d" % (ld % 4)
            return lambda: ("POST", requests.post(
                base + CONNECTIONS, json=body(base, host, "a_mld_0", chunks[ld]),
                timeout=30).status_code, None)

        results = together([read(path) for path in paths * 5] +
                           [create(ld) for ld in chunks])
        seen = {}
        for path, status, text in results:
            if path == "POST":
                assert status == 201
                continue
            assert status == 200, path
            # switch_b and the SLDs are not touched by the two binds
            if "switch_b" in path or "a_sld_0" in path or path == "/redfish/v1":
                assert seen.setdefault(path, text) == text, path


ROOT = Path(__file__).resolve().parent.parent


class TestThreadsOption:
    """-N, --threads: how many threads serve requests."""

    @pytest.mark.parametrize("value", ["0", "-1", "257", "many", ""])
    def test_a_number_from_1_to_256(self, cxl_fmd_executable, value):
        proc = subprocess.run(
            [str(cxl_fmd_executable), "--mock", str(ROOT / "mock/single-sld.json"),
             "-N", value], capture_output=True, text=True, timeout=30)
        assert proc.returncode != 0
        assert "Invalid number of threads" in proc.stderr

    @pytest.mark.parametrize("threads", [1, 2, 32])
    def test_serves_on_that_many(self, cxl_fmd_executable, threads):
        """One thread is the server without a pool; any number answers
        requests sent together."""
        with socket.socket() as sock:
            sock.bind(("", 0))
            port = sock.getsockname()[1]
        proc = subprocess.Popen(
            [str(cxl_fmd_executable), "--mock", str(ROOT / "mock/large-fabric.json"),
             "--port", str(port), "--threads", str(threads)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            base = f"http://localhost:{port}"
            for _ in range(100):
                try:
                    if requests.get(base + "/redfish/v1", timeout=2).status_code == 200:
                        break
                except requests.ConnectionError:
                    time.sleep(0.1)
            else:
                pytest.fail("the daemon did not start")

            def read():
                return requests.get(base + "/redfish/v1/Chassis", timeout=30).status_code

            assert together([read] * 16) == [200] * 16
        finally:
            proc.terminate()
            output, _ = proc.communicate(timeout=10)
        assert proc.returncode == 0, output
        assert ("Serving requests on %d thread" % threads) in output
