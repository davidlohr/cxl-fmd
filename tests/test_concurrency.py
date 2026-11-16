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

    def test_one_host_gets_the_sld(self, cxl_fmd_daemon, api_client):
        """Two hosts ask for the same SLD, eight times each, all at once. An
        SLD port is bound to one VCS: one host has it, with one Bind vPPB,
        and its other seven requests are answered with that Connection; the
        other host is refused every time."""
        base = api_client.base_url
        chunk = chunk_of(api_client, "sld_0")

        def ask(host):
            def job():
                r = requests.post(base + CONNECTIONS, json=body(base, host, "sld_0", chunk),
                                  timeout=30)
                return host, r.status_code, r.headers.get("Location")
            return job

        hosts = ["switch_0_host0", "switch_0_host1"]
        results = together([ask(hosts[i % 2]) for i in range(CLIENTS)])

        won = {host for host, status, _ in results if status in (200, 201)}
        assert len(won) == 1, results
        winner = won.pop()
        mine = [(status, where) for host, status, where in results if host == winner]
        assert sorted(status for status, _ in mine) == [200] * 7 + [201], results
        assert len({where for _, where in mine}) == 1
        for host, status, _ in results:
            if host != winner:
                assert status == 409, results

        now = bound(bindings(api_client, "switch_0"))
        assert list(now.values()) == [("BoundPhysicalPort", 2, None)]
        assert next(iter(now))[0] == ("0" if winner == hosts[0] else "1")
        assert cxl_fmd_daemon["output"]().count("mock bind vPPB") == 1

    def test_eight_posts_of_one_target(self, cxl_fmd_daemon, api_client):
        """One host asks for sld_0 eight times at once: one Connection, made
        with one Bind vPPB (5201h)."""
        base = api_client.base_url
        chunk = chunk_of(api_client, "sld_0")

        def ask():
            r = requests.post(base + CONNECTIONS,
                              json=body(base, "switch_0_host0", "sld_0", chunk), timeout=30)
            return r.status_code, r.headers.get("Location")

        results = together([ask] * 8)
        assert sorted(status for status, _ in results) == [200] * 7 + [201]
        assert len({where for _, where in results}) == 1
        status, data = api_client.get(CONNECTIONS)
        assert [m["@odata.id"] for m in data["Members"]] == [results[0][1]]
        assert cxl_fmd_daemon["output"]().count("mock bind vPPB") == 1


@pytest.mark.mock_topology("mock/background-bind.json")
class TestRaceInTheBackground:

    def test_eight_posts_of_one_target_one_task(self, cxl_fmd_daemon, api_client):
        """While the switch binds in the background, the vPPB reads 01h: the
        record of the bind under way is what the other seven requests find.
        All eight are answered with the one task, and 5201h is sent once."""
        base = api_client.base_url
        chunk = chunk_of(api_client, "sld_0")

        def ask():
            r = requests.post(base + CONNECTIONS,
                              json=body(base, "switch_0_host0", "sld_0", chunk), timeout=30)
            return r.status_code, r.json().get("@odata.id")

        results = together([ask] * 8)
        assert [status for status, _ in results] == [202] * 8
        assert len({task for _, task in results}) == 1
        assert cxl_fmd_daemon["output"]().count("mock bind vPPB") == 1

    def test_distinct_targets_distinct_vppbs(self, cxl_fmd_daemon, api_client):
        """Two targets for one host at once: the first is being bound when
        the second is asked for. The two choose different vPPBs, and the
        switch, which runs one background command at a time, refuses the
        second as Busy (503). On the mock the vPPB being bound reads 01h,
        so this does not show that the record of the bind under way keeps
        another request off a vPPB that still reads 00h: discovery-tests'
        test_post_delete_on_a_switch does, with a switch on a CCI."""
        base = api_client.base_url

        def ask(dev):
            return lambda: requests.post(
                base + CONNECTIONS, timeout=30,
                json=body(base, "switch_0_host0", dev, chunk_of(api_client, dev))).status_code

        results = together([ask("sld_0"), ask("sld_1")])
        assert sorted(results) == [202, 503]
        out = cxl_fmd_daemon["output"]()
        assert out.count("mock bind vPPB 1 in VCS 0") == 1
        assert out.count("mock bind vPPB 2 in VCS 0") == 1


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


def connections(api_client):
    """{id: State} of every Connection."""
    status, data = api_client.get(CONNECTIONS)
    assert status == 200
    out = {}
    for member in data["Members"]:
        status, conn = api_client.get(member["@odata.id"])
        assert status == 200
        out[conn["Id"]] = conn["Status"]["State"]
    return out


def running_tasks(api_client):
    status, tasks = api_client.get("/redfish/v1/TaskService/Tasks")
    assert status == 200
    states = []
    for member in tasks["Members"]:
        status, task = api_client.get(member["@odata.id"])
        assert status == 200
        states.append(task["TaskState"])
    return [s for s in states if s not in ("Completed", "Exception", "Killed",
                                            "Cancelled")]


@pytest.mark.mock_topology("mock/large-fabric.json")
class TestStress:
    """Rounds of POSTs and DELETEs at once, of the same targets and of
    different ones, by the host that has a target and by one that does not.
    Whatever order fabric_lock puts them in, every bind and unbind is one
    a serial order could have made: each 201 is one Bind vPPB (5201h) and
    each 204 one Unbind vPPB (5202h), no LD and no vPPB is bound twice,
    the Connections are the bindings, and no bind or unbind is left under
    way. Run on the TSan build too."""

    LDS = range(8, 16)          # a_mld_0's LDs on port 8 that start unbound
    ROUNDS = 6

    def test_rounds_of_posts_and_deletes(self, cxl_fmd_daemon, api_client):
        base = api_client.base_url
        chunks = {ld: chunk_of(api_client, "a_mld_0", ld) for ld in self.LDS}
        owner = {ld: "switch_a_host%d" % (ld % 4) for ld in self.LDS}
        other = {ld: "switch_a_host%d" % ((ld + 1) % 4) for ld in self.LDS}
        before = bound(bindings(api_client, "switch_a"))
        log = cxl_fmd_daemon["output"]

        def post(ld, host):
            return lambda: ("POST", ld, host, requests.post(
                base + CONNECTIONS, json=body(base, host, "a_mld_0", chunks[ld]),
                timeout=30))

        def delete(ld, uri):
            return lambda: ("DELETE", ld, None, requests.delete(base + uri, timeout=30))

        held = {}               # ld: Connection URI, as the last round left it
        for rnd in range(self.ROUNDS):
            jobs = []
            for ld in self.LDS:
                if ld in held:
                    # unbind it, twice; on alternate rounds ask for it
                    # again in the same breath
                    jobs += [delete(ld, held[ld])] * 2
                    if (ld + rnd) % 2:
                        jobs += [post(ld, owner[ld])] * 2
                elif (ld + rnd) % 3:
                    # three of the owner and one of a host that is not
                    jobs += [post(ld, owner[ld])] * 3 + [post(ld, other[ld])]
            binds = log().count("mock bind vPPB")
            unbinds = log().count("mock unbind vPPB")
            was = {ld for ld in self.LDS if ld in held}
            results = together(jobs)

            created = deleted = 0
            winners = {}
            for method, ld, host, response in results:
                code = response.status_code
                if method == "POST":
                    assert code in (200, 201, 409), (rnd, ld, host, code, response.text)
                    if code in (200, 201):
                        winners.setdefault(ld, set()).add(host)
                    created += code == 201
                else:
                    assert code in (204, 404), (rnd, ld, code, response.text)
                    deleted += code == 204

            # one 5201h per 201 and one 5202h per 204, none other
            assert log().count("mock bind vPPB") - binds == created, rnd
            assert log().count("mock unbind vPPB") - unbinds == deleted, rnd
            # a target goes to one host in a round
            assert all(len(hosts) == 1 for hosts in winners.values()), winners

            now = bound(bindings(api_client, "switch_a"))
            new = {k: v for k, v in now.items() if k not in before or before[k] != v}
            lds = [v[2] for v in new.values()]
            # every LD bound once, each on a vPPB of its own
            assert len(lds) == len(set(lds)), new
            assert all(v[:2] == ("BoundLD", 8) for v in new.values()), new
            # a 201 binds and a 204 unbinds: what each LD is now adds up
            per = {}
            for method, ld, _, response in results:
                per[ld] = per.get(ld, 0) + {201: 1, 204: -1}.get(response.status_code, 0)
            for ld in self.LDS:
                assert (ld in lds) - (ld in was) == per.get(ld, 0), (rnd, ld, per, lds)

            # the Connections are those bindings, none still under way
            conns = connections(api_client)
            assert set(conns.values()) <= {"Enabled"}, conns
            assert running_tasks(api_client) == []
            held = {}
            for (vcs, vppb), (_, port, ld) in new.items():
                cid = "switch_a_vcs%s_vppb%s_p%d_ld%d" % (vcs, vppb, port, ld)
                assert cid in conns, (cid, conns)
                assert vcs == owner[ld][-1] or ld in winners, (vcs, ld)
                held[ld] = CONNECTIONS + "/" + cid

        # nothing was left behind: each target unbinds, and binds again
        for ld, uri in held.items():
            assert requests.delete(base + uri, timeout=30).status_code == 204
        for ld in self.LDS:
            response = requests.post(base + CONNECTIONS, timeout=30,
                                     json=body(base, owner[ld], "a_mld_0", chunks[ld]))
            assert response.status_code == 201, (ld, response.text)
            assert requests.delete(base + response.headers["Location"],
                                   timeout=30).status_code == 204
        assert bound(bindings(api_client, "switch_a")) == before


@pytest.mark.mock_topology("mock/background-bind.json")
class TestStressInTheBackground:
    """The same with a switch that binds and unbinds in the background, one
    command at a time: while a bind or unbind of the target is under way,
    every request for it finds that op; once it is over, nothing of it is
    left."""

    ROUNDS = 3

    def test_rounds(self, cxl_fmd_daemon, api_client):
        from test_tasks import wait_for

        base = api_client.base_url
        chunk = chunk_of(api_client, "sld_0")
        log = cxl_fmd_daemon["output"]

        def ask():
            return ("POST", requests.post(
                base + CONNECTIONS, timeout=30,
                json=body(base, "switch_0_host0", "sld_0", chunk)))

        for rnd in range(self.ROUNDS):
            results = together([ask] * 8)
            assert [r.status_code for _, r in results] == [202] * 8, rnd
            monitors = {r.headers["Location"] for _, r in results}
            assert len(monitors) == 1
            done = wait_for(base, monitors.pop())
            assert done.status_code == 201
            uri = done.headers["Location"]
            assert connections(api_client) == {uri.rsplit("/", 1)[1]: "Enabled"}

            # unbind it eight times at once: one Unbind vPPB, the others
            # find it under way (503), or gone (404) if it is over
            results = together([lambda: ("DELETE", requests.delete(base + uri, timeout=30))] * 8)
            codes = sorted(r.status_code for _, r in results)
            assert codes.count(202) == 1 and set(codes) <= {202, 404, 503}, codes
            monitor = next(r.headers["Location"] for _, r in results if r.status_code == 202)
            assert wait_for(base, monitor).status_code in (200, 204)
            assert requests.get(base + uri, timeout=30).status_code == 404
            assert connections(api_client) == {}
            assert running_tasks(api_client) == []

            assert log().count("mock bind vPPB") == rnd + 1
            assert log().count("mock unbind vPPB") == rnd + 1

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


class TestRescanOption:
    """-r, --rescan: how often to look for new devices."""

    @pytest.mark.parametrize("value", ["-1", "soon", "", "86401"])
    def test_seconds(self, cxl_fmd_executable, value):
        proc = subprocess.run(
            [str(cxl_fmd_executable), "--mock", str(ROOT / "mock/single-sld.json"),
             "--check", "-r", value], capture_output=True, text=True, timeout=30)
        assert proc.returncode != 0
        assert "Invalid re-scan interval" in proc.stderr

    @pytest.mark.parametrize("value", ["0", "5"])
    def test_accepted(self, cxl_fmd_executable, value):
        proc = subprocess.run(
            [str(cxl_fmd_executable), "--mock", str(ROOT / "mock/single-sld.json"),
             "--check", "--rescan", value], capture_output=True, text=True, timeout=30)
        assert proc.returncode == 0
