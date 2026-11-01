#!/usr/bin/env python3
"""Schematic for the OpenPrintTag dryer carrier board, as code.

The board is small enough that its connectivity fits in one readable file, and
describing it this way buys something a drawn schematic does not: the netlist is
generated rather than transcribed, and SKiDL's electrical rules check catches
the mistakes that are otherwise found with a multimeter -- an unconnected input,
two outputs on one net, a power pin nobody drives.

Run it to produce, next to this file:

    openprinttag-carrier.net   KiCad netlist, importable into the PCB editor
    openprinttag-carrier.csv   bill of materials
    netlist.md                 the same connectivity as a table, for review
    generate_netlist.erc       electrical rules check report

Every part is defined here rather than pulled from a symbol library, because
most of what is on this board -- a XIAO module, a load cell, a ribbon to a
reader module -- has no stock symbol anyway.

    pip install skidl
    python3 generate_netlist.py
"""

import builtins
import csv
import os
import sys

from skidl import ERC, KICAD8, Net, Part, Pin, SKIDL, TEMPLATE, generate_netlist, reset

HERE = os.path.dirname(os.path.abspath(__file__))

IN = Pin.types.INPUT
OUT = Pin.types.OUTPUT
BIDIR = Pin.types.BIDIR
PWRIN = Pin.types.PWRIN
PASSIVE = Pin.types.PASSIVE
POWER_DRIVE = Pin.drives.POWER


def define(name, ref_prefix, footprint, pins, value=None, description=""):
    """A part template. Pins are (number, name, function) triples."""
    return Part(
        tool=SKIDL,
        name=name,
        ref_prefix=ref_prefix,
        value=value or name,
        footprint=footprint,
        description=description,
        dest=TEMPLATE,
        pins=[Pin(num=num, name=pin_name, func=func) for num, pin_name, func in pins],
    )


def place(template, ref, value=None):
    """Instantiate a template with a fixed reference designator.

    Fixing the reference here rather than letting SKiDL number parts in
    creation order means the designators in the BOM, in this file and on the
    board all agree, and stay agreeing when something is added in the middle.
    The tag is what a PCB editor uses to keep a footprint tied to a part across
    regenerated netlists.
    """
    part = template(ref=ref, tag=ref)
    if value is not None:
        part.value = value
    return part


def two_pin(name, ref_prefix, footprint, description=""):
    return define(
        name,
        ref_prefix,
        footprint,
        [(1, "1", PASSIVE), (2, "2", PASSIVE)],
        description=description,
    )


def build():
    """Instantiate every part and connect it. SKiDL keeps the resulting circuit
    in a module-level default that ERC and the netlist writer read."""

    # ------------------------------------------------------------------
    # Rails. Each is marked as a power net because the parts that actually
    # feed them here are a module, a diode and a transistor rather than
    # symbols with power-output pins; without this, every power-input pin on
    # the board reads as underdriven.
    # ------------------------------------------------------------------
    vbus_raw = Net("USB_VBUS_RAW")  # USB-C receptacle, ahead of the fuse
    vbus = Net("VBUS")              # after the fuse
    v5 = Net("+5V")                 # board 5 V, diode-ORed with the XIAO's USB
    v3 = Net("+3V3")                # from the XIAO's own regulator
    gnd = Net("GND")
    agnd = Net("AGND")              # load cell return, joined to GND at R5 only
    avdd = Net("AVDD")              # 4.3 V bridge excitation from the HX711
    vled = Net("VLED")              # 5 V less a diode, so 3.3 V data is legal
    for rail in (vbus_raw, vbus, v5, v3, gnd, agnd, avdd, vled):
        rail.drive = POWER_DRIVE

    vfb = Net("HX_VFB")
    vbg = Net("HX_VBG")
    hx_base = Net("HX_BASE")

    cc1 = Net("USB_CC1")
    cc2 = Net("USB_CC2")

    sck = Net("SPI_SCK")
    sck_r = Net("SPI_SCK_R")        # after the series resistor, at the connector
    mosi = Net("SPI_MOSI")
    mosi_r = Net("SPI_MOSI_R")
    miso = Net("SPI_MISO")
    nss = Net("NFC_NSS")
    busy = Net("NFC_BUSY")
    rst = Net("NFC_RST")

    hx_dout = Net("HX_DOUT")
    hx_sck = Net("HX_SCK")
    led_data = Net("LED_DATA")
    led_data_r = Net("LED_DATA_R")
    sda = Net("I2C_SDA")
    scl = Net("I2C_SCL")

    cell_sig_p = Net("CELL_SIG+")   # load cell side of the input filter
    cell_sig_n = Net("CELL_SIG-")
    in_a_p = Net("HX_INA+")         # HX711 side of the input filter
    in_a_n = Net("HX_INA-")

    resistor = two_pin("R", "R", "Resistor_SMD:R_0603_1608Metric")
    capacitor = two_pin("C", "C", "Capacitor_SMD:C_0603_1608Metric")

    # ------------------------------------------------------------------
    # U1: XIAO ESP32-C6 module on castellated pads.
    #
    # Pin numbers follow the XIAO family's own numbering, which every footprint
    # in circulation uses: 1-7 down the left side (D0..D6), 8-14 up the right
    # side (D7..D10, 3V3, GND, 5V). The 5 V pad is bidirectional in the literal
    # sense -- it feeds the module here, and back-feeds this net when the module
    # is plugged into a computer for flashing.
    # ------------------------------------------------------------------
    u1 = place(
        define(
            "XIAO_ESP32C6",
            "U",
            "Seeed_XIAO:XIAO_ESP32C6",
            [
                (1, "D0/GPIO0", BIDIR),
                (2, "D1/GPIO1", BIDIR),
                (3, "D2/GPIO2", BIDIR),
                (4, "D3/GPIO21", BIDIR),
                (5, "D4/GPIO22", BIDIR),
                (6, "D5/GPIO23", BIDIR),
                (7, "D6/GPIO16", BIDIR),
                (8, "D7/GPIO17", BIDIR),
                (9, "D8/GPIO19", BIDIR),
                (10, "D9/GPIO20", BIDIR),
                (11, "D10/GPIO18", BIDIR),
                (12, "3V3", PWRIN),
                (13, "GND", PWRIN),
                (14, "5V", PWRIN),
            ],
            value="XIAO ESP32-C6",
            description="Seeed XIAO ESP32-C6 module",
        ),
        "U1",
    )

    u1[1] += hx_dout
    u1[2] += hx_sck
    u1[3] += led_data
    u1[4] += nss
    u1[5] += sda
    u1[6] += scl
    u1[7] += busy
    u1[8] += rst
    u1[9] += sck
    u1[10] += miso
    u1[11] += mosi
    u1[12] += v3
    u1[13] += gnd
    u1[14] += v5

    # ------------------------------------------------------------------
    # Power input. USB-C for the permanent installation, the XIAO's own USB for
    # flashing, and D1 so the two hosts never feed each other.
    # ------------------------------------------------------------------
    j1 = place(
        define(
            "USB_C_Receptacle",
            "J",
            "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
            [
                ("A1", "GND", PASSIVE),
                ("A4", "VBUS", PASSIVE),
                ("A5", "CC1", PASSIVE),
                ("A6", "DP1", PASSIVE),
                ("A7", "DN1", PASSIVE),
                ("A9", "VBUS", PASSIVE),
                ("A12", "GND", PASSIVE),
                ("B1", "GND", PASSIVE),
                ("B4", "VBUS", PASSIVE),
                ("B5", "CC2", PASSIVE),
                ("B6", "DP2", PASSIVE),
                ("B7", "DN2", PASSIVE),
                ("B9", "VBUS", PASSIVE),
                ("B12", "GND", PASSIVE),
                ("S1", "SHIELD", PASSIVE),
            ],
            value="USB-C receptacle",
            description="USB-C receptacle, power only",
        ),
        "J1",
    )
    for pin in ("A4", "A9", "B4", "B9"):
        j1[pin] += vbus_raw
    for pin in ("A1", "A12", "B1", "B12", "S1"):
        j1[pin] += gnd
    j1["A5"] += cc1
    j1["B5"] += cc2
    # No USB data on this connector: it exists only to bring in 5 V.
    for pin in ("A6", "A7", "B6", "B7"):
        j1[pin].do_erc = False

    # 5.1k on each CC pin is what makes a charger hand over 5 V at all.
    r1 = place(resistor, "R1", "5k1")
    r1[1] += cc1
    r1[2] += gnd
    r2 = place(resistor, "R2", "5k1")
    r2[1] += cc2
    r2[2] += gnd

    # Resettable, because this board lives inside a heated appliance.
    f1 = place(two_pin("Polyfuse", "F", "Fuse:Fuse_1206_3216Metric"), "F1", "500mA")
    f1[1] += vbus_raw
    f1[2] += vbus

    schottky = define(
        "D_Schottky",
        "D",
        "Diode_SMD:D_SMA",
        [(1, "K", PASSIVE), (2, "A", PASSIVE)],
        description="Schottky diode",
    )
    d1 = place(schottky, "D1", "SS34")
    d1["A"] += vbus
    d1["K"] += v5

    # Bulk sits next to the reader connector: the PN5180's field driver takes
    # its current in bursts and the load cell has to not notice.
    c1 = place(
        two_pin("C_Elec", "C", "Capacitor_SMD:CP_Elec_6.3x7.7"), "C1", "220u/10V"
    )
    c1[1] += v5
    c1[2] += gnd
    for ref, value, rail in (
        ("C2", "10u", v5),
        ("C3", "100n", v5),
        ("C4", "10u", v3),
        ("C5", "100n", v3),
    ):
        cap = place(capacitor, ref, value)
        cap[1] += rail
        cap[2] += gnd

    # ------------------------------------------------------------------
    # U2: HX711 as a bare IC rather than a module.
    #
    # That is the whole reason this board exists. A module runs everything from
    # one supply, which forces a choice between 3.3 V excitation -- throwing
    # away a third of the bridge output -- and 5 V logic into a 3.3 V part.
    # Here VSUP is 5 V, the on-chip regulator makes 4.3 V of excitation through
    # Q1, and DVDD is 3.3 V, so DOUT and PD_SCK need no level shifting.
    # ------------------------------------------------------------------
    u2 = place(
        define(
            "HX711",
            "U",
            "Package_SO:SOIC-16W_7.5x10.3mm_P1.27mm",
            [
                (1, "VSUP", PWRIN),
                (2, "BASE", OUT),
                (3, "AVDD", PWRIN),
                (4, "VFB", IN),
                (5, "AGND", PWRIN),
                (6, "VBG", OUT),
                (7, "INA-", IN),
                (8, "INA+", IN),
                (9, "INB-", IN),
                (10, "INB+", IN),
                (11, "PD_SCK", IN),
                (12, "DOUT", OUT),
                (13, "XO", OUT),
                (14, "XI", IN),
                (15, "RATE", IN),
                (16, "DVDD", PWRIN),
            ],
            description="24-bit ADC for bridge sensors",
        ),
        "U2",
    )

    u2["VSUP"] += v5
    u2["AVDD"] += avdd
    u2["AGND"] += agnd
    u2["DVDD"] += v3
    u2["VFB"] += vfb
    u2["VBG"] += vbg
    u2["BASE"] += hx_base
    u2["DOUT"] += hx_dout
    u2["PD_SCK"] += hx_sck
    u2["INA+"] += in_a_p
    u2["INA-"] += in_a_n
    # Channel B is unused, and the datasheet wants unused inputs held at AGND
    # rather than left floating. The firmware never selects the channel.
    u2["INB+"] += agnd
    u2["INB-"] += agnd
    # XI at AGND selects the internal oscillator; RATE at AGND selects 10 SPS,
    # the quieter of the two rates and the one the firmware's timing assumes.
    u2["XI"] += agnd
    u2["RATE"] += agnd
    # No crystal is fitted, so XO stays open. This is the code equivalent of a
    # no-connect flag on a drawn schematic.
    u2["XO"].do_erc = False

    q1 = place(
        define(
            "Q_NPN",
            "Q",
            "Package_TO_SOT_SMD:SOT-23",
            [(1, "B", IN), (2, "E", PASSIVE), (3, "C", PASSIVE)],
            description="NPN pass transistor for bridge excitation",
        ),
        "Q1",
        "BC817-40",
    )
    q1["B"] += hx_base
    q1["C"] += v5
    q1["E"] += avdd

    # 20k over 8.2k around the 1.25 V reference puts excitation at about 4.3 V.
    r3 = place(resistor, "R3", "20k")
    r3[1] += avdd
    r3[2] += vfb
    r4 = place(resistor, "R4", "8k2")
    r4[1] += vfb
    r4[2] += agnd

    for ref, value, plus, minus in (
        ("C6", "100n", vbg, agnd),
        ("C7", "10u", avdd, agnd),
        ("C8", "100n", avdd, agnd),
        ("C9", "100n", v3, gnd),
    ):
        cap = place(capacitor, ref, value)
        cap[1] += plus
        cap[2] += minus

    # The single point where the analog return meets digital ground. Keeping it
    # as a fitted part means the layout cannot quietly turn it into a plane
    # stitch somewhere else.
    r5 = place(
        two_pin("R_NetTie", "R", "Resistor_SMD:R_1206_3216Metric"), "R5", "0R"
    )
    r5[1] += agnd
    r5[2] += gnd

    # Differential input filter, about 800 Hz. Far below the HX711's own
    # bandwidth and well under the switching noise a heater makes.
    r6 = place(resistor, "R6", "1k")
    r6[1] += cell_sig_p
    r6[2] += in_a_p
    r7 = place(resistor, "R7", "1k")
    r7[1] += cell_sig_n
    r7[2] += in_a_n
    c10 = place(capacitor, "C10", "100n")
    c10[1] += in_a_p
    c10[2] += in_a_n
    for ref, leg in (("C11", in_a_p), ("C12", in_a_n)):
        cap = place(capacitor, ref, "10n")
        cap[1] += leg
        cap[2] += agnd

    # ------------------------------------------------------------------
    # J2: load cell, on a screw terminal because the cell has flying leads.
    # ------------------------------------------------------------------
    j2 = place(
        define(
            "Screw_Terminal_01x04",
            "J",
            "TerminalBlock:TerminalBlock_bornier-4_P5.08mm",
            [
                (1, "E+", PASSIVE),
                (2, "E-", PASSIVE),
                (3, "SIG+", PASSIVE),
                (4, "SIG-", PASSIVE),
            ],
            value="Load cell",
            description="4-pin screw terminal, 5 kg load cell",
        ),
        "J2",
    )
    j2[1] += avdd
    j2[2] += agnd
    j2[3] += cell_sig_p
    j2[4] += cell_sig_n

    # ------------------------------------------------------------------
    # J3: 2x5 IDC ribbon up to the PN5180 at the lid.
    #
    # Ground either side of the clock, series resistors on the two lines this
    # board drives, and pull-ups so a C6 that has not booted yet cannot leave
    # the reader selected or held in reset.
    # ------------------------------------------------------------------
    j3 = place(
        define(
            "Conn_02x05_Odd_Even",
            "J",
            "Connector_IDC:IDC-Header_2x05_P2.54mm_Vertical",
            [
                (1, "+5V", PASSIVE),
                (2, "GND", PASSIVE),
                (3, "SCK", PASSIVE),
                (4, "GND", PASSIVE),
                (5, "MOSI", PASSIVE),
                (6, "MISO", PASSIVE),
                (7, "NSS", PASSIVE),
                (8, "BUSY", PASSIVE),
                (9, "RST", PASSIVE),
                (10, "+3V3", PASSIVE),
            ],
            value="PN5180 ribbon",
            description="2x5 IDC header to the reader module",
        ),
        "J3",
    )
    j3[1] += v5
    j3[2] += gnd
    j3[3] += sck_r
    j3[4] += gnd
    j3[5] += mosi_r
    j3[6] += miso
    j3[7] += nss
    j3[8] += busy
    j3[9] += rst
    j3[10] += v3

    r8 = place(resistor, "R8", "33R")
    r8[1] += sck
    r8[2] += sck_r
    r9 = place(resistor, "R9", "33R")
    r9[1] += mosi
    r9[2] += mosi_r
    r10 = place(resistor, "R10", "10k")
    r10[1] += nss
    r10[2] += v3
    r11 = place(resistor, "R11", "10k")
    r11[1] += rst
    r11[2] += v3

    # ------------------------------------------------------------------
    # Status LED. A WS2812 wants data above 0.7 x VDD, which 3.3 V logic does
    # not clear against a 5 V supply, so D2 drops the LED's rail instead of
    # this board carrying a level shifter for one signal.
    # ------------------------------------------------------------------
    d2 = place(
        define(
            "D_Small",
            "D",
            "Diode_SMD:D_SOD-123",
            [(1, "K", PASSIVE), (2, "A", PASSIVE)],
            description="Signal diode, drops the LED supply",
        ),
        "D2",
        "1N4148W",
    )
    d2["A"] += v5
    d2["K"] += vled

    d3 = place(
        define(
            "WS2812B",
            "D",
            "LED_SMD:LED_WS2812B_PLCC4_5.0x5.0mm_P3.2mm",
            [
                (1, "VDD", PWRIN),
                (2, "DOUT", OUT),
                (3, "GND", PWRIN),
                (4, "DIN", IN),
            ],
            description="Addressable status LED",
        ),
        "D3",
        "WS2812B",
    )
    d3["VDD"] += vled
    d3["GND"] += gnd
    d3["DIN"] += led_data_r
    # One LED is enough for status, so its output goes nowhere.
    d3["DOUT"].do_erc = False

    r12 = place(resistor, "R12", "330R")
    r12[1] += led_data
    r12[2] += led_data_r
    c13 = place(capacitor, "C13", "100n")
    c13[1] += vled
    c13[2] += gnd

    # ------------------------------------------------------------------
    # J4: I2C header for the load cell's temperature sensor. That sensor is
    # what makes drift compensation possible, and it has to sit on the cell
    # itself rather than on this board, so it goes on a short lead.
    # ------------------------------------------------------------------
    j4 = place(
        define(
            "Conn_01x04",
            "J",
            "Connector_PinHeader_2.54mm:PinHeader_1x04_P2.54mm_Vertical",
            [
                (1, "+3V3", PASSIVE),
                (2, "GND", PASSIVE),
                (3, "SDA", PASSIVE),
                (4, "SCL", PASSIVE),
            ],
            value="Temp sensor",
            description="1x4 header, I2C temperature sensor",
        ),
        "J4",
    )
    j4[1] += v3
    j4[2] += gnd
    j4[3] += sda
    j4[4] += scl

    r13 = place(resistor, "R13", "4k7")
    r13[1] += sda
    r13[2] += v3
    r14 = place(resistor, "R14", "4k7")
    r14[1] += scl
    r14[2] += v3

    # The two nets worth probing when a weight reading looks wrong.
    test_point = define(
        "TestPoint",
        "TP",
        "TestPoint:TestPoint_Pad_D1.5mm",
        [(1, "1", PASSIVE)],
        description="Test pad",
    )
    for ref, name, net in (("TP1", "AVDD", avdd), ("TP2", "AGND", agnd)):
        place(test_point, ref, name)[1] += net


BOM_ROWS = [
    ("U1", "XIAO ESP32-C6", "Seeed 113991135", 1,
     "Solder to the castellated pads, or fit 2x7 sockets to keep it removable"),
    ("U2", "HX711", "SOIC-16W", 1,
     "Bare IC, not a module: VSUP 5 V, DVDD 3.3 V"),
    ("Q1", "BC817-40", "SOT-23", 1,
     "Pass transistor for the 4.3 V bridge excitation"),
    ("D1", "SS34", "SMA", 1,
     "Schottky, keeps the board USB and the XIAO USB apart"),
    ("D2", "1N4148W", "SOD-123", 1,
     "Drops the WS2812 rail so 3.3 V data clears its threshold"),
    ("D3", "WS2812B", "PLCC4 5.0x5.0", 1, "Status LED"),
    ("F1", "500 mA PTC", "1206", 1, "Resettable, on the USB-C input"),
    ("J1", "USB-C receptacle, power only", "16 pin SMD", 1,
     "5 V input for the permanent installation"),
    ("J2", "Screw terminal, 4 pin", "5.08 mm pitch", 1, "Load cell"),
    ("J3", "IDC header, 2x5", "2.54 mm pitch", 1,
     "Ribbon to the PN5180 at the lid"),
    ("J4", "Pin header, 1x4", "2.54 mm pitch", 1,
     "I2C temperature sensor on the load cell"),
    ("R1, R2", "5.1 k", "0603", 2, "USB-C CC pull-downs"),
    ("R3", "20 k", "0603", 1, "Excitation feedback, upper"),
    ("R4", "8.2 k", "0603", 1, "Excitation feedback, lower: sets 4.3 V"),
    ("R5", "0 R", "1206", 1, "The single join between AGND and GND"),
    ("R6, R7", "1 k", "0603", 2, "Load cell input filter"),
    ("R8, R9", "33 R", "0603", 2, "Series termination on SCK and MOSI"),
    ("R10, R11", "10 k", "0603", 2, "Pull-ups on NSS and RST"),
    ("R12", "330 R", "0603", 1, "WS2812 data series resistor"),
    ("R13, R14", "4.7 k", "0603", 2, "I2C pull-ups"),
    ("C1", "220 uF, 10 V", "6.3x7.7 electrolytic", 1,
     "Bulk, next to J3 for the reader's field bursts"),
    ("C2, C4, C7", "10 uF, 16 V", "0603", 3, "Rail decoupling"),
    ("C3, C5, C6, C8, C9, C13", "100 nF", "0603", 6, "Local decoupling"),
    ("C10", "100 nF", "0603", 1, "Differential, across the cell inputs"),
    ("C11, C12", "10 nF", "0603", 2, "Common mode, on the cell inputs"),
    ("TP1, TP2", "Test pad", "1.5 mm", 2, "AVDD and AGND"),
    ("-", "PN5180 module with antenna", "off board", 1,
     "At the lid, on the ribbon"),
    ("-", "5 kg single point load cell", "TAL220 class", 1, "Under the dryer"),
    ("-", "LM75B or TMP102 breakout", "off board", 1,
     "Bonded to the load cell body"),
]


def write_bom(path):
    with open(path, "w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(["Reference", "Value", "Package", "Quantity", "Note"])
        writer.writerows(BOM_ROWS)


def sort_key(reference):
    """Sort R2 before R10, and put U1 before the passives."""
    prefix = reference.rstrip("0123456789")
    digits = reference[len(prefix):]
    return (prefix, int(digits) if digits else 0)


def describe_pin(pin):
    """`U2.3 AVDD`, or just `R3.1` where the pin has no name of its own."""
    if str(pin.name) == str(pin.num):
        return f"{pin.part.ref}.{pin.num}"
    return f"{pin.part.ref}.{pin.num} {pin.name}"


def write_netlist_table(path):
    """Connectivity as a table, so reviewing it needs no KiCad."""
    circuit = builtins.default_circuit

    lines = [
        "# Carrier board netlist",
        "",
        "Generated by `generate_netlist.py`. Do not edit by hand.",
        "",
        "| Net | Connections |",
        "| --- | --- |",
    ]
    for net in sorted(circuit.nets, key=lambda n: n.name):
        pins = sorted(
            (describe_pin(pin) for pin in net.pins),
            key=lambda text: sort_key(text.split(".")[0]),
        )
        if not pins:
            continue
        lines.append(f"| `{net.name}` | {', '.join(pins)} |")

    lines += ["", "| Part | Value | Footprint |", "| --- | --- | --- |"]
    for part in sorted(circuit.parts, key=lambda p: sort_key(p.ref)):
        lines.append(f"| {part.ref} | {part.value} | `{part.footprint}` |")
    lines.append("")
    with open(path, "w") as handle:
        handle.write("\n".join(lines))


def main():
    reset()
    build()
    ERC()
    generate_netlist(file_=os.path.join(HERE, "openprinttag-carrier.net"), tool=KICAD8)
    write_bom(os.path.join(HERE, "openprinttag-carrier.csv"))
    write_netlist_table(os.path.join(HERE, "netlist.md"))
    print("wrote openprinttag-carrier.net, openprinttag-carrier.csv, netlist.md")
    return 0


if __name__ == "__main__":
    sys.exit(main())
