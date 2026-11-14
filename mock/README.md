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
| `switch-dcd.json` | One switch, two hosts: an MLD and an SLD with dynamic capacity | Dynamic capacity through a switch, an extent shared by two hosts |
| `large-fabric.json` | Two switches, six hosts: six SLDs, MLDs of 4, 8 and 16 LDs; bound and unbound capacity; links down and training | Scale, more than one switch, partly provisioned pools |
| `unit/unit-direct-*.json`, `unit/unit-switch-{sld,mld}.json` | One device each | One device type at a time |

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
- **Multi-Headed devices.** A head is a CXL port of the device. Each LD is
  mapped to at most one head and each head has 1 to 16 LDs; a head with one LD
  presents an SLD, with more an MLD (Section 2.5).
- **Dynamic capacity.** The Host ID of an LD-FAM device is the LD-ID of the
  host interface (Table 7-66). An extent lies in a region, aligned to its block
  size; one extent can be held by several hosts only in a Sharable region
  (Section 9.13.3.2).

## Not represented

- **A Multi-Headed device with heads on switch ports.** `unit/unit-switch-mhsld.json`
  and `unit/unit-switch-mhmld.json` put a two-headed device on a single switch
  port and bind the LDs of both heads through it. A head is a port of the
  device, so two heads are two links: one switch port carries one head. Those
  two files model an MLD with the bookkeeping of a Multi-Headed device, and
  not a fabric that can be built. A topology cannot say yet which port each
  head is on.
- **PBR switches, Fabric Ports and G-FAM devices.**
- **Cascaded switches.**
- **A bind or unbind in progress**, although a vPPB can be given the `busy`
  binding status.
