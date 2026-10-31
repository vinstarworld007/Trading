"""Device and host profiles.

Numbers are representative figures drawn from public vendor material and industry
benchmarks (e.g. STAC-T0 style tick-to-trade results). They are meant to show the
relative shape of each architecture, not to be a vendor-guaranteed spec. Edit them
or override them in a scenario JSON file.
"""

SWITCHES = {
    # Arista 7130 series Layer 1 crosspoint (MetaConnect / 7130 L1): circuit, no packet processing.
    "arista_7130_l1": {"model": "Arista 7130 L1 crosspoint", "latency": {"mean": 4.5, "sd": 0.3},
                       "cut_through": True, "contention": False},
    # Arista 7130 MetaMux: FPGA-based N:1 multiplexer for merging order flow toward the exchange.
    "arista_7130_metamux": {"model": "Arista 7130 MetaMux", "latency": {"mean": 39, "sd": 1.5},
                            "cut_through": True, "contention": True},
    # Arista 7050X3: cut-through L2/L3 switch silicon.
    "arista_7050x3": {"model": "Arista 7050X3 (cut-through L2/L3)", "latency": {"mean": 800, "sd": 15},
                      "cut_through": True, "contention": True},
    # Arista 7280R3: deep-buffer VOQ platform; higher latency, absorbs bursts instead of dropping.
    "arista_7280r3": {"model": "Arista 7280R3 (deep-buffer L3)", "latency": {"mean": 3500, "sd": 120},
                      "cut_through": False, "contention": True},
}

HOSTS = {
    # Classic software stack: kernel networking, interrupts, syscalls.
    "kernel_software": {
        "name": "Linux kernel stack + C++ strategy",
        "ii_ns": {"mean": 1100, "sd": 250, "dist": "lognormal"},
        "pre_stages": [
            ("NIC rx + IRQ + kernel UDP", {"mean": 5500, "sd": 1500, "dist": "lognormal",
                                            "tail_p": 0.004, "tail_ns": [15000, 60000]}),
            ("feed decode (CPU)", {"mean": 400, "sd": 80, "dist": "lognormal"}),
        ],
        "post_stages": [
            ("strategy (CPU)", {"mean": 1500, "sd": 400, "dist": "lognormal"}),
            ("kernel tx (syscall + driver + PCIe)", {"mean": 4500, "sd": 1200, "dist": "lognormal",
                                                     "tail_p": 0.002, "tail_ns": [10000, 40000]}),
        ],
    },
    # Kernel bypass (Onload / ef_vi / ExaNIC-style userspace polling).
    "kernel_bypass": {
        "name": "Kernel-bypass NIC + busy-polling C++",
        "ii_ns": {"mean": 300, "sd": 60, "dist": "lognormal"},
        "pre_stages": [
            ("NIC rx + PCIe DMA (bypass)", {"mean": 650, "sd": 80, "dist": "lognormal",
                                             "tail_p": 0.0005, "tail_ns": [3000, 10000]}),
            ("feed decode (CPU)", {"mean": 150, "sd": 30, "dist": "lognormal"}),
        ],
        "post_stages": [
            ("strategy (CPU)", {"mean": 500, "sd": 120, "dist": "lognormal"}),
            ("bypass tx (PCIe + MAC)", {"mean": 600, "sd": 70, "dist": "lognormal"}),
        ],
    },
    # Hybrid: FPGA NIC parses and filters the feed, CPU makes the decision.
    "fpga_feed_cpu_strategy": {
        "name": "FPGA feed handler + CPU strategy",
        "ii_ns": {"mean": 6.4},
        "pre_stages": [
            ("FPGA PHY/MAC rx", {"mean": 25, "sd": 1.5}),
            ("FPGA feed decode + book build", {"mean": 45, "sd": 3}),
            ("PCIe DMA to host", {"mean": 450, "sd": 50, "dist": "lognormal"}),
        ],
        "post_stages": [
            ("strategy (CPU)", {"mean": 450, "sd": 100, "dist": "lognormal"}),
            ("bypass tx (PCIe + MAC)", {"mean": 600, "sd": 70, "dist": "lognormal"}),
        ],
    },
    # Full tick-to-trade in an FPGA SmartNIC: parse, decide and fire in fabric.
    "fpga_tick_to_trade": {
        "name": "FPGA NIC tick-to-trade",
        "ii_ns": {"mean": 6.4},  # ~2 cycles at 322 MHz: line-rate pipeline
        "pre_stages": [
            ("FPGA PHY/MAC rx", {"mean": 25, "sd": 1.5}),
            ("FPGA feed decode + book build", {"mean": 45, "sd": 3}),
        ],
        "post_stages": [
            ("FPGA strategy logic", {"mean": 20, "sd": 1.5}),
            ("FPGA order encode + MAC/PHY tx", {"mean": 30, "sd": 1.5}),
        ],
    },
    # Strategy in the user FPGA of the 7130 itself: same logic, no separate NIC or cables.
    "switch_fpga_app": {
        "name": "Strategy in 7130 user FPGA",
        "ii_ns": {"mean": 6.4},
        "pre_stages": [
            ("FPGA PHY/MAC rx", {"mean": 22, "sd": 1.5}),
            ("FPGA feed decode + book build", {"mean": 45, "sd": 3}),
        ],
        "post_stages": [
            ("FPGA strategy logic", {"mean": 20, "sd": 1.5}),
            ("FPGA order encode + MAC tx", {"mean": 18, "sd": 1.0}),
        ],
    },
}

MEDIA_NS_PER_M = {"fiber": 4.9, "dac": 4.3, "hollow_core": 3.4, "pcb": 6.0}
