# SPDX-License-Identifier: LGPL-2.1
# cxl-fmd test configuration

# Discovery against libcxlmi mock endpoints. This is the daemon's own code,
# main() included (renamed), with the test's main() in its place.
discovery_tests = executable(
    'discovery-tests',
    [src, files('discovery-tests.c')],
    include_directories: inc,
    c_args: ['-Dmain=fmd_daemon_main'],
    dependencies: [
        libcxlmi_dep,
        microhttpd_dep,
        jansson_dep,
        threads_dep,
    ],
)

test('discovery', discovery_tests, suite: 'unit')

# Find Python
python = find_program('python3', required: true)

# Check for pytest
pytest_check = run_command(python, '-c', 'import pytest', check: false)
pytest_available = pytest_check.returncode() == 0

# Check for requests (required for API tests)
requests_check = run_command(python, '-c', 'import requests', check: false)
requests_available = requests_check.returncode() == 0

# The DMTF Redfish Service Validator. The tests run it from the virtual
# environment in .venv-validator, and make it there if it is not (see
# validator_venv in conftest.py), which needs network. So: run them when
# asked to, or when that environment is there already.
fs = import('fs')
validator_venv = meson.project_source_root() / '.venv-validator'
validator_available = fs.is_file(validator_venv / 'bin' / 'rf_service_validator')
run_validator = get_option('validator').enabled() or \
    (get_option('validator').auto() and validator_available)

if not pytest_available
    warning('pytest not found - tests will be skipped')
endif

if not requests_available
    warning('requests not found - API tests will be skipped. Install with "pip install requests"')
endif

# Mock topologies: the shipped ones are valid CXL fabrics, and one that
# breaks a rule of the specification is refused (needs only pytest)
if pytest_available
    test(
        'mock-topologies',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            meson.current_source_dir() / 'test_mock_topologies.py',
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 120,
        suite: 'unit',
    )
endif

# The man page says what the program does
if pytest_available
    test(
        'manpage',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            meson.current_source_dir() / 'test_manpage.py',
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 60,
        suite: 'unit',
    )
endif

# What the docs say of deployment, the DCD contracts and the trust boundary
if pytest_available
    test(
        'docs',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            meson.current_source_dir() / 'test_docs.py',
        ],
        timeout: 60,
        suite: 'unit',
    )
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

    # Provisioning on the larger topologies, as an orchestrator does
    test(
        'orchestration',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_orchestration.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # Binds and unbinds the switch carries out in the background
    test(
        'tasks',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_tasks.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # cxl-fmd's Oem properties against its Oem schema
    test(
        'oem-schema',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_oem_schema.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 300,
        suite: 'api',
    )

    # Status.Conditions from Get Health Info, and cxl-fmd's registry
    test(
        'conditions',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_conditions.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 300,
        suite: 'api',
    )

    # Status from Get Health Info, alike in every view
    test(
        'health',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_health.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # AddressRangeType of a chunk, alike in every view
    test(
        'range-type',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_range_type.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # Commands Table 8-308 marks O4 for MCTP: only when allowed
    test(
        'mctp-restricted',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_mctp_restricted.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # The security actions of CXLLogicalDevice, and their Memory aliases
    test(
        'ld-security',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_ld_security.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # Query parameters, and methods a resource does not take
    test(
        'protocol',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_protocol.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # Error responses, and their messages of the DMTF Base registry
    test(
        'errors',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_errors.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # The mock switch's bind and unbind outcomes, and binds made elsewhere
    test(
        'mock-bindings',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_mock_bindings.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # Devices pulled out of a switch port
    test(
        'hot-removal',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_hot_removal.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # CXL event records as Redfish log entries
    test(
        'event-logs',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_event_logs.py'),
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 180,
        suite: 'api',
    )

    # Several clients at once
    test(
        'concurrency',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            files('test_concurrency.py'),
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

# What fails a validator run, and that Oem extensions are checked (needs
# only pytest)
if pytest_available
    test(
        'redfish-validator-gate',
        python,
        args: [
            '-m', 'pytest',
            '-v',
            '--tb=short',
            '-k', 'test_validator_problems or test_no_oem_check_is_not_used',
            meson.current_source_dir() / 'test_redfish_validator.py',
        ],
        env: {
            'MESON_BUILD_ROOT': meson.project_build_root(),
            'MESON_SOURCE_ROOT': meson.project_source_root(),
        },
        timeout: 60,
        suite: 'unit',
    )
endif

# The validator over mock topologies: one for each, by its test
if pytest_available and run_validator
    foreach t : [
        ['redfish-validator-sld', 'test_single_sld_validation'],
        ['redfish-validator-mld', 'test_mld_validation'],
        ['redfish-validator-event-logs', 'test_event_logs_validation'],
        ['redfish-validator-hot-removal', 'test_hot_removal_validation'],
        ['redfish-validator-dcd', 'test_dcd_validation'],
        ['redfish-validator-dcd-perhost', 'test_dcd_perhost_validation'],
        ['redfish-validator-degraded', 'test_degraded_validation'],
        ['redfish-validator-all', 'test_every_topology_validation', 1800],
    ]
        test(
            t[0],
            python,
            args: [
                '-m', 'pytest',
                '-v',
                '--tb=short',
                '-k', t[1],
                meson.current_source_dir() / 'test_redfish_validator.py',
            ],
            env: {
                'MESON_BUILD_ROOT': meson.project_build_root(),
                'MESON_SOURCE_ROOT': meson.project_source_root(),
            },
            timeout: t.length() > 2 ? t[2] : 600,
            suite: 'validator',
            is_parallel: false,
        )
    endforeach
elif pytest_available and not validator_available
    message('the DMTF Redfish Service Validator is not run: -Dvalidator=enabled to install it in .venv-validator and run it')
endif

# Summary
summary({
    'pytest': pytest_available,
    'requests': requests_available,
    'redfish_service_validator': run_validator,
}, section: 'Test Dependencies')
