# Testing Guide

This document describes the testing infrastructure for cxl-fmd, including the DMTF Redfish Service Validator, pytest test suite, and QEMU-based hardware emulation testing.

## Overview

cxl-fmd has three levels of testing:

| Level | Description | Hardware Required |
|-------|-------------|-------------------|
| **Unit/API Tests** | pytest tests against mock topologies | None |
| **DMTF Validator** | Schema compliance validation | None |
| **QEMU Tests** | Real CXL device emulation | None (uses QEMU) |

## Quick Start

```bash
# Run all meson tests
meson test -C build

# Run all pytest tests directly
pytest tests/ -v

# Run DMTF validator on default topology
python scripts/run_redfish_validator.py

# Run QEMU ioctl test (requires kernel with CXL support)
./scripts/qemu/qemu-cxl-test.sh --kernel /boot/vmlinuz-$(uname -r)
```

## Meson Test Integration

Tests are integrated with meson's test framework for easy CI integration.

### Prerequisites

Install Python dependencies:

```bash
pip install pytest requests
```

The DMTF Redfish Service Validator and Redfish-Protocol-Validator run from
their own virtual environment, `.venv-validator` in the source tree, at the
versions `tests/conftest.py` names (`VALIDATOR_VERSION`,
`PROTOCOL_VALIDATOR_VERSION`). The tests of each run when it is installed
there, or when asked for with `-Dvalidator=enabled`, in which case the
first of them installs what is missing, which needs network:

```bash
meson setup build -Dvalidator=enabled
# Shows: Test Dependencies
#   pytest                     : YES
#   requests                   : YES
#   redfish_service_validator  : YES
#   redfish_protocol_validator : YES
```

`-Dvalidator=disabled` leaves them out. CI runs them in a job of their own.

### Running Meson Tests

```bash
# Run all tests
meson test -C build

# Run with verbose output
meson test -C build -v

# Run specific test suite
meson test -C build --suite redfish
meson test -C build --suite api
meson test -C build --suite validator

# Run specific test by name
meson test -C build redfish-endpoints
meson test -C build topology-inventory
meson test -C build connections

# List available tests
meson test -C build --list
```

### Test Suites

| Suite | Tests | Description | Dependencies |
|-------|-------|-------------|--------------|
| `redfish` | redfish-endpoints | Basic endpoint accessibility | pytest |
| `api` | topology-inventory, orchestration, tasks, hot-removal, event-logs, concurrency, memory-lifecycle, connections, cxl-business-rules, error-handling, dcd, mock-bindings | Full API tests | pytest, requests |
| `security` | fuzzing, adversarial | Input fuzzing and security tests | pytest, requests |
| `unit` | discovery, mock-topologies, manpage, lock-file, docs, unit-topologies, redfish-validator-gate, redfish-protocol-validator-gate | The discovery harness (`discovery-tests`), the checks of the mock topologies, the man page against `--help`, one cxl-fmd per lock file (`test_lock_file.py`), statements the docs must keep (`test_docs.py`: one cxl-fmd per fabric, a lock file that fences one host only, the DCD `DELETE` and zero-Tag contracts, the deferred TLS, authentication and listen address), 7 combinations of attachment and device type (all but a Multi-Headed MLD on a switch), and what fails a run of either validator (below) | pytest, requests (the pytest ones) |
| `validator` | redfish-validator-sld, redfish-validator-mld, redfish-validator-event-logs, redfish-validator-hot-removal, redfish-validator-dcd, redfish-validator-dcd-perhost, redfish-validator-degraded, redfish-validator-all, redfish-protocol-validator | The DMTF Redfish Service Validator over `single-sld.json`, `mld-device.json`, `event-logs.json` (a switch, its VCSs, connections, tasks and log services), `hot-removal.json` after a device with a connection has been pulled out, `dcd-device.json`, `dcd-shared-perhost.json`, `degraded-device.json` (`Status.Conditions` and the `CxlHealth` registry), and (`redfish-validator-all`) every other topology in `mock/` and `mock/unit/`. Oem extensions are checked (no `--nooemcheck`), against cxl-fmd's Oem schema `Cxl_v1.xml`, which the validator fetches from `/redfish/v1/Schemas` through `$metadata` (the copy it saves in `schema/dmtf/csdl` is removed after each run). A run fails on any `FAIL`, and on any `WARN` but the deprecated `TemporaryThroughputReductionSupported` cxl-fmd still serves: the validator reports a bad Oem extension only as a `WARN`. `redfish-protocol-validator` runs the DMTF Redfish-Protocol-Validator over every topology in `mock/` and `mock/unit/` (see "DMTF Redfish-Protocol-Validator") | pytest, `.venv-validator` |

### Test Timeouts

| Suite | Timeout |
|-------|---------|
| redfish | 120s |
| api | 180s |
| security | 180s (fuzzing), 300s (adversarial) |
| unit | 60s (manpage), 120s (mock-topologies, lock-file), 300s (unit-topologies) |
| validator | 600s; 1800s (redfish-validator-all, redfish-protocol-validator) |

Override timeouts:

```bash
meson test -C build --timeout-multiplier 2
```

## Discovery Tests

The pytest suites run the daemon in `--mock` mode, which builds the topology
straight from JSON and never executes `src/discovery.c` or any libcxlmi
command. `tests/discovery-tests.c` covers that code: each device is a libcxlmi
mock endpoint (`cxlmi_open_mock()`) that answers CCI commands with responses
the test queued, and `fmd_discover()` and the functions that act on its result
are the daemon's own.

```bash
meson test -C build discovery
./build/tests/discovery-tests
```

The responses are written byte by byte from the payload tables of the CXL
specification, with the table number next to each, so the decoding is checked
against the specification and not against libcxlmi's structures.

A check for a known defect is written with `XFAIL()`. It is reported as
`xfail` and does not fail the run. When the defect is fixed it reports `XPASS`,
which does fail, so that the check is turned into a `CHECK()`. The `xfail`
lines are therefore the list of discovery defects the tests currently pin down.

Limits, from the libcxlmi mock endpoint:

- It matches a command by opcode only, so the tunnel a command is sent through
  (to an LD, or through a switch port) is not verified.
- A queued response is used once. A command the code sends twice needs two
  responses; one that has none gets "Unsupported".
- It keeps only the last command it received.

## Mock Topologies

A mock topology is a JSON file in `mock/`. Whatever is tested on one only
means something if the topology could be a real CXL fabric, so two things
stand between a file and the daemon:

- **The file is read strictly** (`src/mock.c`). A value that is present and
  cannot be used as what it is meant to be is an error: a number that does not
  parse or does not fit its field, a capacity that is not a multiple of
  256 MB, more entries than a list can hold, a device on a switch or port the
  file does not have, an extent outside every region. None of these is rounded,
  truncated, dropped or replaced by a default.
- **The topology is checked against the CXL specification**
  (`src/validate.c`). A check that comes from CXL r4.0 names its section or
  table when it fails; the few that are consistency checks of cxl-fmd's own
  model, or its limits, say so. Among them: a port or an LD bound to more
  than one vPPB, an MLD port bound as a whole, an enabled VCS whose USP ID is
  not an upstream port or a Fabric Port, a device on a port whose Connected
  Device Type is another, capacities that do not add up, an MLD with no LDs
  or more than 16, a head of a Multi-Headed device with no LD mapped, DC
  regions and extents that are not aligned, extents of one host that
  overlap, a Shared Extent Sequence out of place, a tag two hosts hold
  through a region that is not Sharable, two devices with one serial
  number, vPPB 0 of an enabled VCS (the USP vPPB, Section 7.1.4, as
  cxl-fmd reads it) bound to anything but its USP. Some are only doubtful:
  logged as a warning, and the topology is still valid. Among them, those
  that give a binding no Connection, or an unresolved one: a vPPB bound in
  a VCS that is not enabled, a vPPB bound to an LD of a head that presents
  an MLD (which pool LD that is, is not known, Section 2.5.1), and a head of
  a Multi-Headed device on no switch port and no host cxl-fmd knows.

A topology that fails either is refused: the daemon exits instead of serving
it. To check one without serving it:

```bash
./build/cxl-fmd --mock mock/multi-host-vcs.json --check    # exit status 0: valid
```

`tests/test_mock_topologies.py` (`meson test -C build mock-topologies`) checks
that every topology in `mock/` is valid, and that each rule rejects a topology
that breaks it: every case takes a valid topology, breaks one rule, and
expects the refusal to name it. **A new check needs a case there**, or nothing
shows that it can fail.

The same checks run on a discovered topology, where a failure is a warning in
the log: it says something about the hardware or about discovery, and the
daemon still serves what it found.

## DMTF Redfish Service Validator

The [DMTF Redfish Service Validator](https://github.com/DMTF/Redfish-Service-Validator) validates that cxl-fmd's Redfish responses conform to the official DMTF schemas.

### Running the Validator

The `run_redfish_validator.py` script handles everything automatically:
- Creates a Python virtual environment
- Installs the validator package
- Starts cxl-fmd with a mock topology
- Runs validation against all endpoints
- Reports results

```bash
# Basic usage (uses mock/single-sld.json)
python scripts/run_redfish_validator.py

# Test specific topology
python scripts/run_redfish_validator.py --mock-topology mock/mld-device.json
python scripts/run_redfish_validator.py --mock-topology mock/switch-attached.json

# Save detailed logs
python scripts/run_redfish_validator.py --output-dir ./validator-logs

# Verbose daemon output
python scripts/run_redfish_validator.py --verbose

# Custom build directory
python scripts/run_redfish_validator.py --build-dir build-debug
```

### Command Line Options

| Option | Description |
|--------|-------------|
| `--mock-topology, -m` | Mock topology JSON file (default: mock/single-sld.json) |
| `--build-dir, -b` | Build directory containing cxl-fmd |
| `--output-dir, -o` | Output directory for validator logs |
| `--port, -p` | Port for Redfish service (0 = auto) |
| `--timeout, -t` | Validation timeout in seconds (default: 300) |
| `--verbose, -v` | Show daemon output |

### Validating All Topologies

```bash
# Validate all mock topologies
for f in mock/*.json mock/unit/*.json; do
    echo "=== Validating: $f ==="
    python scripts/run_redfish_validator.py --mock-topology "$f"
done
```

### Exit Codes

| Code | Meaning |
|------|---------|
| 0 | Validation passed |
| 1 | Validation failed (schema errors) |
| 2 | Executable not found |
| 3 | Daemon startup timeout |
| 4 | Other error |

## DMTF Redfish-Protocol-Validator

The [DMTF Redfish-Protocol-Validator](https://github.com/DMTF/Redfish-Protocol-Validator)
checks the protocol of DSP0266: methods, request and response headers,
status codes, error responses, and the security requirements, each
reported as an assertion that passes, fails, warns or is not tested.
`tests/test_protocol_validator.py` runs it, at `PROTOCOL_VALIDATOR_VERSION`,
over each topology in `mock/` and `mock/unit/` (`redfish-protocol-validator`
in the `validator` suite, and the CI validator job). A run takes about 20
seconds; all 23 about 8 minutes.

A run fails on any `FAIL` or `WARN` the table below does not name, and on
an entry of the table the run no longer reports, so that the list cannot
outlive what it excuses (`redfish-protocol-validator-gate` tests this).
Each entry is matched on its assertion, method, status, URI and message.
It fails each because cxl-fmd has no authentication, login sessions,
accounts, roles or TLS, which the maintainer has deferred (see
"Authentication" in [Redfish-API.md](Redfish-API.md)), or because the
validator tests the requirement only through them. The requirement is the
validator's text of it:

| Assertion | Requirement | What cxl-fmd does |
|-----------|-------------|-------------------|
| `PROTO_HTTP_SUPPORTED_METHODS` (POST) | "Services shall support the POST, GET, PATCH, and DELETE HTTP methods." | The validator POSTs only to the Sessions and Accounts collections. cxl-fmd takes POST on its Connections and MemoryChunks collections and on actions (`test_connections.py`, `test_memory_lifecycle.py`, `test_protocol.py`) |
| `REQ_POST_CREATE_TO_MEMBERS_PROP` | "Submitting a POST request to a resource collection is equivalent to submitting the same request to the Members property of that resource collection. Services that support the addition of Members to a resource collection shall support both forms." | Tried on `SessionService/Sessions/Members` only (`404`): cxl-fmd has no sessions. Its own collections that take POST take it on `Members` too (`test_protocol.py`) |
| `REQ_POST_CREATE_VIA_COLLECTION` | "To create a resource, services shall support the POST method on resource collections." | Tried on `SessionService/Sessions` only, which is `405`: no sessions |
| `SEC_REQUIRE_LOGIN_SESSIONS` | "Service shall provide login sessions that conform with this specification." | No sessions: a POST to `SessionService/Sessions` is `405` |
| `SEC_BASIC_AUTH_OVER_HTTPS` | "All requests that use HTTP Basic authentication shall require HTTPS." | No TLS, and no authentication: a request with Basic credentials is served over HTTP |
| `SEC_PRIV_SUPPORT_PREDEFINED_ROLES` (three: Administrator, Operator, ReadOnly) | "Services shall support the Administrator, Operator, and ReadOnly standard roles." | No `AccountService`, accounts or roles |
| `SEC_READ_REQUIRES_AUTH` (each URI it tries) | "Resources shall not be available as unauthenticated, with some exceptions." | No authentication: every GET is served |
| `SEC_WRITE_REQUIRES_AUTH` | "Services shall authenticate all write requests to Redfish resources, with some exceptions." | No authentication; tried as a POST to `SessionService/Sessions`, `405` |

What it reports `NOT_TESTED` does not fail a run. On a mock topology that
is, by what it says it lacks:

- Sessions, accounts and their ETags (deferred): the `PROTO_ETAG_*`
  assertions ("Failed to create an account"), `REQ_HEADERS_IF_MATCH`,
  `RESP_HEADERS_ETAG`, `RESP_HEADERS_X_AUTH_TOKEN`,
  `REQ_POST_CREATE_NOT_IDEMPOTENT`, `REQ_POST_CREATE_URI_IN_LOCATION_HDR`,
  `RESP_HEADERS_LOCATION`, `RESP_HEADERS_WWW_AUTHENTICATE`, and the `SEC_*`
  assertions of sessions, accounts, passwords, privileges, cookies and
  certificates (`SEC_CERTS_CONFORM_X509V3`: the service is not HTTPS).
  `REQ_GET_COLLECTION_COUNT_PROP_*`, `REQ_HEADERS_X_AUTH_TOKEN` and
  `SEC_BOTH_AUTH_TYPES` say no successful GET of
  `/redfish/v1/SessionService/Sessions` was found: the validator GETs
  that collection only through the service root's `SessionService`, which
  cxl-fmd does not serve (it serves the empty collection the service
  root's `Links.Sessions` names, answered `200`).
- The PATCH and DELETE it would make, of an account it could not create:
  `PROTO_HTTP_SUPPORTED_METHODS` (PATCH, DELETE), `REQ_DELETE_METHOD_REQUIRED`,
  `REQ_PATCH_*`, `REQ_HEADERS_CONTENT_TYPE`, `REQ_DATA_MOD_ERRORS`; and
  `RESP_STATUS_BAD_REQUEST` and `RESP_STATUS_INTERNAL_SERVER_ERROR`, which
  look for a `400` or a `500` among the responses to its own requests and
  found none. cxl-fmd's PATCH, DELETE and errors are tested in the api
  suite.
- Optional features cxl-fmd does not provide: `HEAD` ("Services may
  support the HEAD method", DSP0266 Section 7.4: `REQ_HEAD_DIFFERS_FROM_GET`,
  and the `HEAD` half of `RESP_HEADERS_LINK_REL_DESCRIBED_BY`); an OpenAPI
  document (`REQ_HEADERS_ACCEPT`, `RESP_HEADERS_CONTENT_TYPE` for
  `openapi.yaml`); query parameters (`REQ_QUERY_PROTOCOL_FEATURES_SUPPORTED`:
  the service root has no `ProtocolFeaturesSupported`); an EventService,
  with subscriptions and Server-Sent Events (`SERV_EVENT_*`, `SERV_SSE_*`,
  `SEC_SESSION_TERMINATION_SIDE_EFFECTS`); SSDP (`SERV_SSDP_*`: the
  service root has no `UUID`); HTTP redirects
  (`PROTO_REDIRECT_ENFORCES_TARGET_PRIVS`).

The headers it checks that cxl-fmd did not send, and the `OData-Version`
it did not check, are fixed (`Cache-Control`, `Allow` and `Link` on a
`GET`, `412` for an `OData-Version` other than `4.0`; see "Headers" in
[Redfish-API.md](Redfish-API.md)), as is a POST to a collection's
`Members`; `test_protocol.py` tests each without the validator.

## pytest Test Suite

The pytest test suite provides comprehensive API testing against mock topologies.

### Test Files

| File | Description | Tests |
|------|-------------|-------|
| `test_redfish_validator.py` | Basic endpoint accessibility, and the DMTF validator runs | 32 |
| `test_topology_inventory.py` | Device/switch/host/MHD discovery | 99 |
| `test_orchestration.py` | Provisioning on the larger mock topologies | 15 |
| `test_tasks.py` | Binds and unbinds in the background | 8 |
| `test_hot_removal.py` | Devices pulled out of a switch port | 3 |
| `test_event_logs.py` | Event records as log entries | 8 |
| `test_concurrency.py` | Requests served on several threads | 24 |
| `test_memory_lifecycle.py` | Memory chunk CRUD operations | 67 |
| `test_connections.py` | Connection management | 54 |
| `test_cxl_business_rules.py` | CXL-specific rules (VCS, LD limits, health) | 34 |
| `test_error_handling.py` | Error responses (404, 400, 409) | 31 |
| `test_fuzzing.py` | Input fuzzing and boundary tests | 28 |
| `test_adversarial.py` | Security and adversarial tests | 59 |
| `test_dcd.py` | Dynamic capacity, the tasks of an add or a release, and when each DC read was made | 111 |
| `test_mock_topologies.py` | The mock topologies, and the checker refusing invalid ones | 97 |
| `test_manpage.py` | The man page against `--help` | 5 |
| `test_lock_file.py` | A second cxl-fmd with the same lock file exits; the default lock file, taken before discovery | 9 |
| `test_unit_topologies.py` | 7 combinations of attachment and device type | 83 |
| `test_mock_bindings.py` | The mock switch's bind and unbind outcomes, and binds made outside cxl-fmd | 17 |
| `test_conditions.py` | `Status.Conditions` from Get Health Info, and the `CxlHealth` registry | 20 |
| `test_oem_schema.py` | Every `Oem.Cxl` object and Oem action against cxl-fmd's Oem schema `Cxl_v1.xml` | 7 |
| `test_protocol_validator.py` | The DMTF Redfish-Protocol-Validator over every mock topology, and what fails a run of it | 24 |
| **Total** | | **835** |

### Running Tests

```bash
# Run all tests
pytest tests/ -v

# Run specific test file
pytest tests/test_connections.py -v

# Run specific test class
pytest tests/test_topology_inventory.py::TestSwitchInventory -v

# Run tests matching pattern
pytest tests/ -k "MHD" -v
pytest tests/ -k "connection" -v

# Run with specific topology marker
pytest tests/ -m "mock_topology" -v

# Skip slow tests
pytest tests/ -m "not slow" -v

# Show test durations
pytest tests/ -v --durations=10
```

### Test Fixtures

The `conftest.py` provides these fixtures:

#### `cxl_fmd_daemon`

Starts cxl-fmd with a mock topology and provides connection info:

```python
def test_example(cxl_fmd_daemon):
    port = cxl_fmd_daemon['port']
    url = cxl_fmd_daemon['url']
    # daemon auto-starts and stops
```

#### `api_client`

HTTP client with JSON handling:

```python
def test_get_chassis(api_client):
    status, data = api_client.get("/redfish/v1/Chassis")
    assert status == 200
    assert "Members" in data

def test_create_chunk(api_client):
    status, data, location = api_client.post(
        "/redfish/v1/Chassis/dev0/MemoryDomains/dom0/MemoryChunks",
        {"AddressRangeType": "PMEM", "MemoryChunkSizeMiB": 1024}
    )
    assert status == 201
```

### Custom Markers

Use markers to customize test behavior:

```python
@pytest.mark.mock_topology("mock/mld-device.json")
def test_mld_specific(api_client):
    # Uses MLD topology instead of default
    pass

@pytest.mark.slow
def test_expensive_operation(api_client):
    # Can be skipped with -m "not slow"
    pass
```

### Test Categories

#### Discovery Tests
Verify correct resource hierarchy after topology loading:
- Chassis collection and members
- PCIe devices and CXL logical devices
- Switch ports and VCS configuration
- Host systems and endpoints

#### Memory Lifecycle Tests
Test dynamic memory management:
- Initial chunk discovery
- DELETE existing chunks
- POST new chunks with various sizes
- Capacity constraint enforcement
- InterleaveSets for MLD chunks

#### Connection Tests
Test host-to-device binding:
- Connections of vPPBs bound before cxl-fmd started, and of direct-attached
  links, with their IDs, targets, chunks and host MemoryDomains
- POST of a free target binds (201, or 202 and a Task); a repeated POST is
  200 with the same Connection, or 202 with the same Task while it binds
- POST validation: one initiator and one target, chunks of the target on
  one LD, `AccessCapabilities` `Read` and `Write` only (400)
- DELETE unbinds whoever made the binding (option 1h), a direct-attached
  Connection is 405, and `#Cxl.ForceUnbind` sends option 2h
- Elsewhere: a stale ID is 404 after a bind made outside cxl-fmd
  (`test_mock_bindings.py`), and IDs and `Oem.Cxl.Origin` across a
  restart of the daemon (`test_orchestration.py`)
- System MemoryDomains updates
- Error handling for invalid endpoints

#### Error Handling Tests
Verify proper error responses:
- 404 for non-existent resources
- 400 for malformed requests
- 409 for capacity conflicts
- Proper error JSON structure

#### Adversarial Tests
Security and robustness testing:
- Concurrent request handling
- Long strings and buffer limits
- Special characters (null, unicode, format strings)
- HTTP protocol abuse
- JSON edge cases
- Resource exhaustion
- Integer overflow
- Malformed resource IDs

## QEMU Testing

QEMU testing validates cxl-fmd against emulated CXL hardware without requiring physical devices.

### Prerequisites

- QEMU with CXL support (cxl-type3 device)
- Linux kernel with CXL support
- KVM enabled

### ioctl Transport Test

Tests cxl-fmd's ioctl-based discovery against QEMU CXL Type3 devices.

```bash
./scripts/qemu/qemu-cxl-test.sh --kernel /boot/vmlinuz-$(uname -r)
```

#### What It Does

1. Builds cxl-fmd
2. Creates minimal initramfs with cxl-fmd and test tools
3. Boots QEMU with:
   - Two CXL Type3 memory devices (256MB + 512MB)
   - CXL host bridge and root ports
4. Runs cxl-fmd with `--ioctl` discovery
5. Executes Redfish API tests
6. Reports PASS/FAIL

#### Options

| Option | Description |
|--------|-------------|
| `--kernel, -k` | Path to kernel bzImage (required) |
| `--qemu, -q` | Path to QEMU binary |
| `--timeout, -t` | Test timeout in seconds (default: 120) |
| `--verbose, -v` | Verbose output |

#### QEMU Topology

```
pxb-cxl (CXL host bridge)
├── cxl-rp (root port 0)
│   └── cxl-type3 (256MB volatile)
└── cxl-rp (root port 1)
    └── cxl-type3 (512MB volatile)
```

### MCTP Transport Test

Tests cxl-fmd's MCTP-based discovery against QEMU CXL switch topology with I2C MCTP transport.

```bash
./scripts/qemu/qemu-mctp-test.sh --kernel /boot/vmlinuz-$(uname -r)
```

#### Additional Requirements

- QEMU with CXL switch support (cxl-upstream, cxl-downstream)
- QEMU with i2c_mctp_cxl device support
- Kernel with MCTP and aspeed-i2c support:
  ```
  CONFIG_MCTP=y
  CONFIG_MCTP_TRANSPORT_I2C=y
  CONFIG_I2C_ASPEED=y
  ```
- mctp-tools (mctp, mctpd)
- D-Bus (dbus-daemon)

#### What It Does

1. Builds cxl-fmd with codeconstruct mctpd support
2. Creates initramfs with MCTP tools and D-Bus
3. Boots QEMU with:
   - CXL switch with upstream port
   - Two CXL Type3 devices on downstream ports
   - I2C MCTP transport via aspeed-i2c
4. Configures MCTP network and endpoint IDs
5. Runs cxl-fmd with `--mctp` discovery
6. Executes Redfish API tests
7. Reports PASS/FAIL

#### QEMU Topology

```
pxb-cxl (CXL host bridge)
└── cxl-rp (root port)
    └── cxl-upstream (switch)
        ├── cxl-downstream port 0 → cxl-type3 (256MB)
        ├── cxl-downstream port 1 → virtio-rng
        └── cxl-downstream port 2 → cxl-type3 (512MB)

aspeed.i2c.bus.0 (MCTP transport)
├── i2c_mctp_cxl @ 0x4 → switch (EID 8)
├── i2c_mctp_cxl @ 0x5 → type3-1 (EID 9)
└── i2c_mctp_cxl @ 0x6 → type3-2 (EID 10)
```

#### MCTP Configuration

| Component | I2C Address | EID |
|-----------|-------------|-----|
| Local (cxl-fmd) | - | 50 |
| Switch | 0x4 | 8 |
| Type3 Device 1 | 0x5 | 9 |
| Type3 Device 2 | 0x6 | 10 |

## Memory Safety Testing

### Address Sanitizer (ASan)

Build with ASan to detect memory errors:

```bash
# Configure with sanitizers
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false

# Build
meson compile -C build-asan

# Run tests (ASan is automatic)
pytest tests/ -v
```

ASan detects:
- Buffer overflows
- Use-after-free
- Memory leaks
- Undefined behavior

### Valgrind

Run with Valgrind for detailed memory analysis:

```bash
# Start daemon under Valgrind
valgrind --leak-check=full --show-leak-kinds=all \
    ./build/cxl-fmd --mock mock/single-sld.json -v &

# Run tests
pytest tests/test_redfish_validator.py -v

# Stop daemon and check output
kill %1
```

### Valgrind Test Script

```bash
#!/bin/bash
for topology in mock/unit/*.json; do
    name=$(basename "$topology" .json)
    echo "Testing: $name"

    valgrind --leak-check=full --error-exitcode=1 \
        ./build/cxl-fmd --mock "$topology" &
    PID=$!
    sleep 2

    curl -s http://localhost:8080/redfish/v1 > /dev/null
    kill $PID 2>/dev/null
    wait $PID 2>/dev/null

    [ $? -eq 0 ] && echo "  OK" || echo "  LEAK DETECTED"
done
```

## Continuous Integration

`.github/workflows/ci.yml` runs on every push to `main` and on pull requests.
Nothing in it needs hardware:

| Job | What it runs |
|-----|--------------|
| Build and test (gcc, clang) | A warnings-as-errors build, `--check` on every topology in `mock/`, then `meson test`: the discovery harness, the mock topology checks and the pytest suites |
| ASan and UBSan | The same `meson test` on a build with `-Db_sanitize=address,undefined` |
| TSan | The same `meson test` on a build with `-Db_sanitize=thread` |

The pytest fixture that starts the daemon fails the test if the daemon exits
during it, or does not shut down with status 0 afterwards. A crash, a failed
assertion or a sanitizer report in the daemon is therefore a test failure,
even when every request the test made was answered; under the sanitizer job
that includes memory the daemon still holds at exit.

The HTTP server serves requests on a pool of threads (`-N`, `--threads`,
8 by default). Requests that only read
(GET) run together; a request that changes the fabric, and the event loop when
it brings dynamic capacity up to date, has it exclusively (`fabric_lock` in
`struct fmd_ctx`). `tests/test_concurrency.py` sends requests at once, and
under the TSan job a data race anywhere in the daemon fails the test that was
running. Its `TestStress` and `TestStressInTheBackground` send rounds of
POSTs and DELETEs of the same and of different targets at once, and check
that each 201 sent one Bind vPPB and each 204 one Unbind vPPB, that no LD or
vPPB is bound twice, and that no bind or unbind is left under way.

To run what CI runs:

```bash
meson setup build -Dtests=enabled -Dwerror=true && meson test -C build
CC=clang meson setup build-asan -Dtests=enabled \
    -Db_sanitize=address,undefined -Db_lundef=false
ASAN_OPTIONS=detect_leaks=1 meson test -C build-asan --timeout-multiplier 4
```

## Troubleshooting

### Daemon Won't Start

```bash
# Check if port is in use
lsof -i :8080

# Try different port
./build/cxl-fmd --mock mock/single-sld.json --port 8081

# Enable verbose logging
./build/cxl-fmd --mock mock/single-sld.json -vv
```

### Tests Timeout

```bash
# Increase timeout
pytest tests/ -v --timeout=120

# Run single test for debugging
pytest tests/test_connections.py::TestConnectionLifecycle::test_create_basic_connection -v -s
```

### QEMU Test Fails

```bash
# Check QEMU CXL support
qemu-system-x86_64 -device help | grep cxl

# Check kernel CXL support
zcat /proc/config.gz | grep CXL

# Run with verbose output
./scripts/qemu/qemu-cxl-test.sh --kernel /boot/vmlinuz -v
```

### Validator Reports Failures

```bash
# Save detailed logs
python scripts/run_redfish_validator.py --output-dir ./logs

# Check specific endpoint
curl -s http://localhost:8080/redfish/v1/Chassis | jq

# Compare against schema
cat schema/dmtf/json-schema/Chassis.v1_25_0.json | jq
```
