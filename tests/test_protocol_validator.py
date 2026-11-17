# SPDX-License-Identifier: LGPL-2.1
"""
The DMTF Redfish-Protocol-Validator over every mock topology (review
F18.6).

The Service Validator (test_redfish_validator.py) checks each resource
against its schema; this one checks the protocol of DSP0266: methods,
request and response headers, status codes, error responses, and the
security requirements. It is run as a user would run it, from the
.venv-validator the Service Validator is in (conftest.py), at the
version conftest.py names.

A run fails on any FAIL or WARN that EXPECTED below does not name, and on
an entry of EXPECTED that the run no longer reports, so that the list
cannot outlive what it excuses. Each entry is a requirement cxl-fmd does
not meet because it has no authentication, sessions, accounts, roles or
TLS (deferred by the maintainer: docs/Redfish-API.md, "Authentication"),
or one the validator tests only through them. Each is matched on its
assertion, method, status, URI and message, not on its assertion alone.
NOT_TESTED results are not failures: docs/testing.md lists why each is
not tested.
"""

import csv
import re
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent

TOPOLOGIES = sorted(
    str(p.relative_to(ROOT)) for p in
    list((ROOT / "mock").glob("*.json")) + list((ROOT / "mock" / "unit").glob("*.json")))

# (result, assertion, method, status, URI, message): regular expressions
# matched in full against a FAIL or WARN row of the validator's report; then
# why it is expected. The requirement each fails is quoted in docs/testing.md.
SESSIONS = "/redfish/v1/SessionService/Sessions"
NO_AUTH = "deferred: cxl-fmd has no authentication (TLS, authentication and the listen address are deferred)"
NO_SESSIONS = "deferred: cxl-fmd has no login sessions; its Sessions collection is empty and takes no POST"
EXPECTED = [
    # "Services shall support the POST, GET, PATCH, and DELETE HTTP methods."
    # The validator POSTs only to the Sessions and Accounts collections;
    # cxl-fmd takes POST on its Connections and MemoryChunks collections and
    # on actions (test_protocol.py, test_connections.py, test_memory_lifecycle.py)
    (("FAIL", "PROTO_HTTP_SUPPORTED_METHODS", "POST", "", "",
      "No POST requests had a successful response"), NO_SESSIONS),
    # "Submitting a POST request to a resource collection is equivalent to
    # submitting the same request to the Members property ..."; tried on
    # Sessions only. cxl-fmd's collections that take POST take it on Members
    # too (test_protocol.py)
    (("FAIL", "REQ_POST_CREATE_TO_MEMBERS_PROP", "POST", "404", SESSIONS + "/Members",
      r"POST to Members property URI .* failed with status 404; .*"), NO_SESSIONS),
    # "To create a resource, services shall support the POST method on
    # resource collections." Tried on Sessions only
    (("FAIL", "REQ_POST_CREATE_VIA_COLLECTION", "POST", "405", SESSIONS,
      r"POST request to collections uri .* failed with status 405"), NO_SESSIONS),
    # "Service shall provide login sessions that conform with this specification."
    (("FAIL", "SEC_REQUIRE_LOGIN_SESSIONS", "POST", "405", SESSIONS,
      r"POST request to .* to create a login session failed with status 405"), NO_SESSIONS),
    # "All requests that use HTTP Basic authentication shall require HTTPS."
    (("FAIL", "SEC_BASIC_AUTH_OVER_HTTPS", "GET", "200", SESSIONS,
      r"GET request to .* using basic auth over HTTP succeeded with status 200 but did "
      r"not redirect to HTTPS; .*"), NO_AUTH),
    # "Services shall support the Administrator, Operator, and ReadOnly
    # standard roles."
    (("FAIL", "SEC_PRIV_SUPPORT_PREDEFINED_ROLES", "GET", "", "",
      r"Predefined role (Administrator|Operator|ReadOnly) not found"),
     "deferred: cxl-fmd has no AccountService, accounts or roles"),
    # "Resources shall not be available as unauthenticated, with some exceptions."
    (("FAIL", "SEC_READ_REQUIRES_AUTH", "GET", "200", r"/redfish/v1/.+",
      r"GET request to /redfish/v1/.+ with no authentication succeeded with status 200"),
     NO_AUTH),
    # "Services shall authenticate all write requests to Redfish resources,
    # with some exceptions." Tried as a POST to Sessions, which is 405
    (("FAIL", "SEC_WRITE_REQUIRES_AUTH", "POST", "405", SESSIONS,
      r"POST request to .* with no authentication failed with status 405"), NO_AUTH),
]


def report_rows(report_dir):
    """The rows of the validator's TSV report in @report_dir"""
    reports = list(Path(report_dir).glob("RedfishProtocolValidationReport_*.tsv"))
    assert len(reports) == 1, reports
    with reports[0].open(newline="") as f:
        return list(csv.DictReader(f, delimiter="\t"))


def classify(rows, expected=EXPECTED):
    """
    (unexpected, unmet): each FAIL or WARN of @rows that no entry of
    @expected matches, and each entry that matches none of them
    """
    keys = ("Result", "Assertion", "Method", "Status code", "URI", "Message")
    unexpected, met = [], set()
    for row in rows:
        if row["Result"] not in ("FAIL", "WARN"):
            continue
        hit = [i for i, (pattern, _) in enumerate(expected)
               if all(re.fullmatch(p, row[k] or "") for p, k in zip(pattern, keys))]
        if hit:
            met.update(hit)
        else:
            unexpected.append(" | ".join(row[k] or "" for k in keys))
    unmet = [expected[i][0] for i in range(len(expected)) if i not in met]
    return unexpected, unmet


def test_classify():
    """What fails a run: a FAIL or WARN not expected, on any field, and an
    expected one that is not reported"""
    def row(result, assertion, method, status, uri, message):
        return {"Result": result, "Assertion": assertion, "Method": method,
                "Status code": status, "URI": uri, "Message": message}

    expected = [
        row("FAIL", a, m, s, u, msg) for a, m, s, u, msg in [
            ("SEC_READ_REQUIRES_AUTH", "GET", "200", "/redfish/v1/Chassis",
             "GET request to /redfish/v1/Chassis with no authentication succeeded "
             "with status 200"),
            ("PROTO_HTTP_SUPPORTED_METHODS", "POST", "", "",
             "No POST requests had a successful response"),
            ("REQ_POST_CREATE_TO_MEMBERS_PROP", "POST", "404", SESSIONS + "/Members",
             "POST to Members property URI /redfish/v1/SessionService/Sessions/Members "
             "failed with status 404; extended error: x"),
            ("REQ_POST_CREATE_VIA_COLLECTION", "POST", "405", SESSIONS,
             "POST request to collections uri /redfish/v1/SessionService/Sessions "
             "failed with status 405"),
            ("SEC_REQUIRE_LOGIN_SESSIONS", "POST", "405", SESSIONS,
             "POST request to /redfish/v1/SessionService/Sessions to create a login "
             "session failed with status 405"),
            ("SEC_BASIC_AUTH_OVER_HTTPS", "GET", "200", SESSIONS,
             "GET request to /redfish/v1/SessionService/Sessions using basic auth over "
             "HTTP succeeded with status 200 but did not redirect to HTTPS; final URL "
             "was http://localhost:1/redfish/v1/SessionService/Sessions"),
            ("SEC_PRIV_SUPPORT_PREDEFINED_ROLES", "GET", "", "",
             "Predefined role Operator not found"),
            ("SEC_WRITE_REQUIRES_AUTH", "POST", "405", SESSIONS,
             "POST request to /redfish/v1/SessionService/Sessions with no "
             "authentication failed with status 405"),
        ]]
    passing = row("PASS", "RESP_HEADERS_CACHE_CONTROL", "GET", "200", "/redfish/v1/",
                  "Test passed for header Cache-Control: no-store")
    untested = row("NOT_TESTED", "SERV_SSE_SUCCESSFUL_RESPONSE", "", "", "",
                   "No ServerSentEventUri available; unable to test this assertion")
    assert classify(expected + [passing, untested]) == ([], [])

    # the same assertion with another status, another URI's write, or a
    # WARN of what is expected as a FAIL
    for bad in [row("FAIL", "SEC_WRITE_REQUIRES_AUTH", "POST", "201", SESSIONS,
                    "POST request to /redfish/v1/SessionService/Sessions with no "
                    "authentication failed with status 201"),
                row("FAIL", "REQ_POST_CREATE_VIA_COLLECTION", "POST", "405",
                    "/redfish/v1/Fabrics/CXL/Connections", "POST request to collections "
                    "uri /redfish/v1/Fabrics/CXL/Connections failed with status 405"),
                row("WARN", "RESP_HEADERS_CACHE_CONTROL", "GET", "200", "/redfish/v1/",
                    "anything"),
                row("WARN", "SEC_PRIV_SUPPORT_PREDEFINED_ROLES", "GET", "", "",
                    "Predefined role Operator not found"),
                row("FAIL", "REQ_HEADERS_ODATA_VERSION", "GET", "200", "/redfish/v1/",
                    "Request with unsupported header OData-Version: 4.1 returned status "
                    "200; expected status 412")]:
        unexpected, unmet = classify(expected + [bad])
        assert len(unexpected) == 1 and not unmet, bad

    # an entry the run no longer reports
    unexpected, unmet = classify(expected[1:])
    assert not unexpected
    assert [u[1] for u in unmet] == ["SEC_READ_REQUIRES_AUTH"]


@pytest.mark.slow
@pytest.mark.parametrize("topology", TOPOLOGIES)
def test_protocol_validation(cxl_fmd_executable, protocol_validator_venv, topology):
    """The validator over @topology: nothing fails but what EXPECTED names,
    and the daemon exits cleanly after it"""
    from conftest import _find_free_port, _wait_for_service

    port = _find_free_port()
    with tempfile.TemporaryFile() as log, tempfile.TemporaryDirectory() as work:
        proc = subprocess.Popen([str(cxl_fmd_executable), "--mock", str(ROOT / topology),
                                 "--port", str(port)],
                                stdout=log, stderr=subprocess.STDOUT)
        try:
            url = f"http://localhost:{port}"
            assert _wait_for_service(url + "/redfish/v1"), topology
            # run in an empty directory: it reads config.ini from the
            # current one
            result = subprocess.run(
                [str(protocol_validator_venv.protocol_validator), "--rhost", url,
                 "--user", "none", "--password", "none",
                 "--report-dir", work, "--report-type", "tsv"],
                cwd=work, capture_output=True, text=True, timeout=600)
            print(result.stdout, result.stderr)
            rows = report_rows(work)
            # 1 if anything failed, else 0 (its console_scripts.py); else it
            # did not finish
            assert result.returncode == int(any(r["Result"] == "FAIL" for r in rows)), \
                result.stderr
            unexpected, unmet = classify(rows)
        finally:
            died = proc.poll()
            proc.terminate()
            rc = proc.wait(timeout=10)
            log.seek(0)
            output = log.read().decode(errors="replace")
        assert died is None, f"cxl-fmd exited during the run ({died}):\n{output[-4000:]}"
        assert rc == 0, f"cxl-fmd did not shut down cleanly ({rc}):\n{output[-4000:]}"
    assert not unexpected, "\n".join(unexpected)
    assert not unmet, f"expected, and no longer reported: {unmet}"
