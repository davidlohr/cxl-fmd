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
# Optional: for DMTF validator tests
pip install redfish-service-validator
```

Meson will warn about missing dependencies during configuration:

```bash
meson setup build
# Shows: Test Dependencies
#   pytest                    : YES
#   requests                  : YES
#   redfish_service_validator : YES/NO
```

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
| `api` | topology-inventory, memory-lifecycle, connections, cxl-business-rules, error-handling | Full API tests | pytest, requests |
| `security` | fuzzing, adversarial | Input fuzzing and security tests | pytest, requests |
| `unit` | unit-topologies | All 8 CXL topology combinations | pytest, requests |
| `validator` | redfish-validator-sld, redfish-validator-mld | DMTF schema validation | pytest, redfish-service-validator |

### Test Timeouts

| Suite | Timeout |
|-------|---------|
| redfish | 120s |
| api | 180s |
| validator | 300s |

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

## pytest Test Suite

The pytest test suite provides comprehensive API testing against mock topologies.

### Test Files

| File | Description | Tests |
|------|-------------|-------|
| `test_redfish_validator.py` | Basic endpoint accessibility | 7 |
| `test_topology_inventory.py` | Device/switch/host/MHD discovery | 49 |
| `test_memory_lifecycle.py` | Memory chunk CRUD operations | 21 |
| `test_connections.py` | Connection management | 16 |
| `test_cxl_business_rules.py` | CXL-specific rules (VCS, LD limits, health) | 20 |
| `test_error_handling.py` | Error responses (404, 400, 409) | 31 |
| `test_fuzzing.py` | Input fuzzing and boundary tests | 29 |
| `test_adversarial.py` | Security and adversarial tests | 59 |
| `test_unit_topologies.py` | All 8 CXL topology combinations | 74 |
| **Total** | | **316** |

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
- POST connections with initiator/target endpoints
- Connection appears in collection
- DELETE connections
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

### Recommended CI Pipeline

```yaml
test:
  stages:
    - build
    - test-unit
    - test-validator
    - test-asan

  build:
    script:
      - meson setup build
      - meson compile -C build

  test-unit:
    script:
      - pytest tests/ -v --junitxml=results.xml

  test-validator:
    script:
      - python scripts/run_redfish_validator.py --output-dir logs
    artifacts:
      paths:
        - logs/

  test-asan:
    script:
      - meson setup build-asan -Db_sanitize=address,undefined
      - meson compile -C build-asan
      - pytest tests/ -v
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
