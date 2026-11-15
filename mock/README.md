# Mock topologies

Each file here is a CXL fabric that `cxl-fmd --mock FILE` serves in place of
hardware. Every one is read strictly and checked against the CXL specification
before it is served (see *Mock Topologies* in [docs/testing.md](../docs/testing.md)):
`cxl-fmd --mock FILE --check` says whether a file is valid, and
`meson test -C build mock-topologies` checks all of them.

## What each topology is for

From the simplest to the largest. "Bound" is what the hosts already have;
"unbound" is what an orchestrator can still give them.

| File | Fabric | Use it for |
|------|--------|-----------|
| `single-sld.json` | One SLD, no switch | The smallest case |
| `multi-device.json` | Three SLDs, no switch | Several devices on one host |
| `degraded-device.json` | Seven SLDs, each in a different health or media state | Status reporting |
| `mld-device.json` | Four MLDs (3, 4, 4 and 16 LDs) and an SLD, no switch | LD inventory, LD allocations, granularity, QoS |
| `mhd-device.json` | Four Multi-Headed devices, each head to a host of its own | MH-SLD, MH-MLD, heads of mixed presentation, pool LDs mapped to no head |
| `dcd-device.json` | An SLD and an MLD with dynamic capacity, no switch | DC regions, extents, add and release, tags, a Sharable region |
| `switch-attached.json` | One switch, one host: five SLDs and an MLD, two ports empty | A single VCS |
| `multi-host-vcs.json` | One switch, two hosts: three SLDs and an MLD, everything bound | Two VCSs sharing an MLD by LD |
| `fm-boots-first.json` | One switch, two hosts: two SLDs, an MLD, an empty port, a PCIe device; **nothing bound** | Where an orchestrator starts (Section 7.2.1.2) |
| `background-bind.json` | `fm-boots-first.json` with a switch that takes 1.5 s over a bind or unbind | Binds and unbinds in the background: 202 Accepted, tasks, a switch that is busy |
| `hot-removal.json` | `fm-boots-first.json` with `sld_0`, then `mld_0`, pulled out of their switch ports, one each time the daemon is sent `SIGUSR1` | Devices removed from a switch, and what becomes of their connections |
| `event-logs.json` | `multi-host-vcs.json` with records in the event logs of the switch and of two devices | CXL event records as Redfish log entries |
| `switch-dcd.json` | One switch, two hosts: an MLD and an SLD with dynamic capacity | Dynamic capacity through a switch, an extent shared by two hosts |
| `large-fabric.json` | Two switches, six hosts: six SLDs, MLDs of 4, 8 and 16 LDs; bound and unbound capacity; links down and training | Scale, more than one switch, partly provisioned pools |
| `unit/unit-direct-*.json`, `unit/unit-switch-{sld,mld}.json` | One device each | One device type at a time |
| `unit/unit-switch-mhsld.json` | One switch, two hosts: an MH-SLD with each head on a port of its own | A Multi-Headed device through a switch |

## What the specification says a fabric is

The checker enforces these; they are also what to keep to when writing a new
topology. Section and table numbers are CXL r4.0.

- **Hosts and switches.** A host is on a VCS of one switch, through that VCS's
  upstream port (Section 7.1). It is given only devices on that switch.
- **SLD ports.** A port with an SLD is bound to only one VCS, as a whole
  (Section 7.2.3.1). "The Fabric Manager can bind any unused physical port to
  the unbound vPPB."
- **MLD ports.** An MLD has up to 16 LDs for hosts and one for the FM
  (Section 2.4). Its port is bound by LD: each LD to one host, or to none, in
  which case it is FM-owned (Section 7.3.4). The LDs of one MLD can be on
  different VCSs.
- **Multi-Headed devices.** A head is a CXL port of the device, on a host or
  on a switch port of its own. Each LD is mapped to at most one head and each
  head has 1 to 16 LDs; a head with one LD presents an SLD, with more an MLD
  (Section 2.5). The port a head is on sees what the head presents.
- **Dynamic capacity.** The Host ID of an LD-FAM device is the LD-ID of the
  host interface (Table 7-66). An extent lies in a region, aligned to its block
  size; one extent can be held by several hosts only in a Sharable region
  (Section 9.13.3.2).

## Not represented

- **A head that presents an MLD, on a switch port.** A head is a CXL port of
  the device, so each head of a Multi-Headed device is on a port of its own:
  a head gives its `parent_switch` and `parent_port`, and the device itself is
  on no port. A head that presents an SLD is bound as a whole port, like any
  SLD. A head that presents an MLD is bound by LD, with LD-IDs "relative to
  that head" (Section 2.5.1), and no command reports which LD of the pool each
  of them is: such a head on a switch is refused when the file is loaded.
  MH-MLDs are represented with their heads on hosts, in `mhd-device.json`.
- **Where the heads of a discovered Multi-Headed device are.** This is a
  property of mock topologies only. Get Head Info gives the port number of a
  head as the device sees it, not the switch port it is plugged into, so on
  hardware a head is not placed on a port.
- **PBR switches, Fabric Ports and G-FAM devices.**
- **Cascaded switches.**
- **A bind or unbind that fails in the background.** `background-bind.json`
  has a switch that takes its time and then succeeds. One that fails is tested
  against Background Operation Status answers in `tests/discovery-tests.c`.
