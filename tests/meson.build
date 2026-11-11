# SPDX-License-Identifier: LGPL-2.1
# cxl-fmd test configuration

# Find Python
python = find_program('python3', required: true)

# Check for pytest
pytest_check = run_command(python, '-c', 'import pytest', check: false)
pytest_available = pytest_check.returncode() == 0

# Check for requests (required for API tests)
requests_check = run_command(python, '-c', 'import requests', check: false)
requests_available = requests_check.returncode() == 0

# Check for redfish_service_validator
validator_check = run_command(python, '-c', 'import redfish_service_validator', check: false)
validator_available = validator_check.returncode() == 0

if not pytest_available
    warning('pytest not found - tests will be skipped')
endif

if not requests_available
    warning('requests not found - API tests will be skipped. Install with "pip install requests"')
endif

# Quick endpoint tests (always run if pytest available)
if pytest_available
    test(
        'redfish-endpoints',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            '-k', 'TestRedfishEndpoints',
            meson.current_source_dir(),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 120,
        suite: 'redfish',
    )
endif

# API tests (require requests module)
if pytest_available and requests_available
    # Topology inventory tests
    test(
        'topology-inventory',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_topology_inventory.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # Memory lifecycle tests
    test(
        'memory-lifecycle',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_memory_lifecycle.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # Connection tests
    test(
        'connections',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_connections.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # CXL business rules tests
    test(
        'cxl-business-rules',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_cxl_business_rules.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # Error handling tests
    test(
        'error-handling',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_error_handling.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # Fuzzing tests
    test(
        'fuzzing',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_fuzzing.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'security',
    )

    # Adversarial tests
    test(
        'adversarial',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_adversarial.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 300,
        suite: 'security',
    )

    # Dynamic Capacity Device tests
    test(
        'dcd',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_dcd.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # Unit topology tests (all 8 CXL combinations)
    test(
        'unit-topologies',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_unit_topologies.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 300,
        suite: 'unit',
    )
endif

# Full validator tests (only if validator installed)
if pytest_available and validator_available
    test(
        'redfish-validator-sld',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            '-k', 'test_single_sld',
            meson.current_source_dir(),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 300,
        suite: 'validator',
    )

    test(
        'redfish-validator-mld',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            '-k', 'test_mld',
            meson.current_source_dir(),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 300,
        suite: 'validator',
    )
else
    if not validator_available
        warning('redfish-service-validator not found - install with "pip install redfish-service-validator" or run ./scripts/run_redfish_validator.py manually')
    endif
endif

# Summary
summary({
    'pytest': pytest_available,
    'requests': requests_available,
    'redfish_service_validator': validator_available,
}, section: 'Test Dependencies')
