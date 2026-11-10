# xc2cdump v9.7 -- source

Xilinx CoolRunner-II (XC2C) JEDEC fuse map decompiler by Darksoft.
Reads one .jed and writes VHDL plus a matching UCF that ISE 14.7 compiles back into a part that behaves
the same. Built for arcade PCB preservation. Use at your own risk.

## Commands

```
xc2cdump v9.7 -- CoolRunner-II .jed decompiler by Darksoft. Use at your own risk.
usage: xc2cdump <command> [options] <file.jed> [outputs]

commands:
  decode  <file.jed>                      structural report, one block per macrocell
  vhdl    <file.jed> [out.vhd] [out.ucf]  VHDL plus a matching constraints file
  info    <file.jed>                      device, fuse-map geometry, fuse counts
  dump    <file.jed>                      device and how many fuses are set
  used    <file.jed>                      which macrocells the design uses, per block
  config  <file.jed> <fb> <mc>            one macrocell's configuration fields
  diff    <a.jed> <b.jed>                 fuse ranges that differ between two bitstreams
  verify  <file.jed> <file.ucf>           check a UCF's pins against a bitstream
  pins    <device> <package>              the pin table for one part
  devices                                 supported devices and packages
  --version                               version line only

options:
  --device NAME       override the device named in the JEDEC header (same density)
  --package NAME      override the package
  --ucf FILE          read an existing UCF to recover real signal names
  --entity NAME       entity name for the generated VHDL
  --lock-macrocells   add INST LOC constraints for buried macrocells
  --no-pin-table      name ports PAD_FBx_y, not by pin (the UCF keeps its LOCs)
  --json FILE         use an already-decoded netlist

NOTE: when rebuilding the emitted VHDL in ISE, cpldfit MUST be given
      -unused float, or every unused pin is driven low.
```

Examples:

```
xc2cdump decode board.jed                          structural report on standard output
xc2cdump vhdl board.jed board.vhd board.ucf        the decompile
xc2cdump vhdl board.jed out.vhd out.ucf --ucf pins.ucf    real signal names, from the board's own UCF
xc2cdump info board.jed                            device and fuse-map geometry
xc2cdump used board.jed                            which macrocells the design uses
xc2cdump config board.jed 2 9                      every configuration field of FB2_MC9
xc2cdump diff old.jed new.jed                      which fuse ranges changed
xc2cdump verify board.jed board.ucf                is this UCF consistent with this bitstream's pins?
xc2cdump pins XC2C64A VQ100                        the pin table for one part
xc2cdump devices                                   supported devices and packages
```

- **decode** prints one block per macrocell: clock and edge, register type and power-up state, output
  enable, and the product terms of each input, with signals named from the pin table or from `--ucf`.
- **vhdl** writes the pair. The VHDL is an entity whose ports are the pads the design actually uses and
  an architecture holding the product terms, the XOR gates, the registers and the output drivers. The
  UCF carries a LOC for every one of those ports, taken from the decoded bitstream, so a recompile lands
  the design back on the same pins.
- **info** prints the device, the fuse-map geometry read from the file's own section headers, and the
  fuse counts. **dump** prints the device and how many fuses are set. **used** lists the macrocells the
  design occupies, per function block. **config** prints every configuration field of one macrocell,
  decoded and annotated.
- **diff** compares two bitstreams of the same part and prints the fuse ranges that differ, with the
  first 25 listed. It exits 1 when they differ, as diff(1) does. The percentage it reports is not a
  measure of anything: unprogrammed fuses match each other, so a decode that dropped logic still scores
  well. Judge a decode by re-fitting it, not by this number.
- **verify** checks a UCF against a bitstream in both directions. For each signal the UCF names it
  prints the macrocell its pin lands on and whether the bitstream drives that pin, reads it, or leaves
  it unused; then it lists every pin the bitstream uses that the UCF does not name. The UCF does NOT
  match, and verify exits 1, when the bitstream uses a pin the UCF does not name, when a signal is
  placed on a location the part does not have, or when one pin is named twice or one signal placed
  twice. A signal on a pin the bitstream leaves unused is reported but is not a mismatch: the fitter
  removes inputs the source declared but never read, and the board's own UCF still names them.
  Passing is a necessary condition, not proof that the UCF was written for this design. A bitstream
  carries no signal names, so on a board that uses nearly every pin, a UCF from another design with
  the same footprint passes too; the output says so.
- **pins** prints the pin table of one device and package.

`--ucf` is worth using whenever the board's own constraints file still exists. Without it the ports are
named `PIN_n` and the output is correct but unreadable; with it they come back as `MC_A17`, `STM_RST_N`
and the rest, and a recovered bus keeps the range the UCF names (`AD(22 downto 15)`, not `AD(22 downto 0)`).

The banner goes to standard error, first, on every run, so it never mixes with a redirected decode.

Exit codes: 0 done, 1 error, 2 refused (nothing to write). `diff` follows diff(1): 0 when the two
bitstreams are identical, 1 when they differ. `verify`: 0 consistent, 1 the UCF does not match the
bitstream, 2 could not check (unreadable file, or no pinout for the part).

## Supported parts

Decoding works on any CoolRunner-II part: the geometry is read from the fuse map's own section headers
rather than from a device table, so the six densities are handled the same way, as are all three
macrocell record layouts -- 27-bit, 29-bit, and the reduced 16-bit record of a buried macrocell.
Pin numbers and real signal names need a pinout, and 42 device/package
combinations are built in:

    XC2C32     CP56, DI44, PC44, VQ44
    XC2C32A    CP56, CV64, DI44, FG32, PC44, VQ44
    XC2C64     CP56, DI81, PC44, VQ44, VQ100
    XC2C64A    CP56, CV64, DI81, FG48, PC44, VQ44, VQ100
    XC2C128    CP132, CV100, DI126, TQ144, VQ100
    XC2C256    CP132, DI222, FT256, PQ208, TQ144, VQ100
    XC2C384    DI288, FG324, FT256, PQ208, TQ144
    XC2C512    DI324, FG324, FT256, PQ208

`xc2cdump devices` prints the same list with the bonded macrocell count of each combination.

## v9.7 vs v9.6

Two defects made v9.6 write logic that differs from the part, with no warning and exit 0. Both are fixed.

- **A macrocell's own product-term set, reset and clock were decoded wrongly.** When a register's reset or
  preset comes from its own product term, it was emitted as a constant 0; on the XC2C128 to XC2C512 a
  register clocked by its own product term was given the function block's shared clock instead. A real
  board was affected: on `u12_main` (XC2C128) the latch behind `DAT_BUF_OEn_o` came out gated by
  `STM_STROBE_i`, where the fitter's own report gives `NOT STM_RST_i AND NOT STM_MODE_i`. It now matches
  that equation, and the XC2C128 decode simulates identically to the same board fitted to an XC2C64A.
  Which product term each control uses was measured for this release from 204 designs fitted on all six
  densities, built so that each term could be told apart, before being compared with the tables the tool
  already had; they agree, and the fault was in how the decoder used them.
- **A global output enable driven by logic was decoded as always on.** When several pins share an
  enable computed from other signals, the fitter can route it through a global tri-state net driven by
  a macrocell. If nothing else read that macrocell, the decode dropped it and the pins came out
  permanently driven. They now follow the enable.
- **`--device` accepted a part of another density.** It exited 0 and took every pin from the wrong
  pinout. It now has to match the function-block count of the fuse map; the A and non-A parts of one
  density are still interchangeable.
- **`--lock-macrocells` built nothing on any part with buried registers.** It locked combinational
  signals that synthesis folds into the register, and ngdbuild stopped on the first one. It now locks
  registers only, and lists the combinational macrocells it leaves unlocked.
- **A `--ucf` net named like a VHDL reserved word or like a name the tool generates broke the output.**
  A net called `out`, `signal` or `register`, or `gsr`, `gck_0` or `gts_1`, gave VHDL that does not
  compile, with exit 0. The board's own names are now kept as written; the tool's generated names give
  way to them, and a reserved word becomes `<name>_pin` in both the VHDL and the UCF. Each rename is
  noted in a comment line of the VHDL.
- **`decode` now lists buried macrocells**, marked `buried`; it reported only macrocells with a pin.
- `config` describes several fields more accurately, and `--no-pin-table` says in its help what it does.
- **The XC2C128 CV100 pin table is right where the vendor's file is wrong.** ISE's device file gives ball
  G4 to two pads; the vendor's fitter places the GCK2 pad on K2, which is what the tool uses.

Nothing else in the output moved. Of 367 test bitstreams, nine decode differently from v9.6, and they are
the designs these fixes are about. The others give the same VHDL and UCF apart from the version line, as
do the boards decoded with their own UCF files.

## v9.6 vs v9.5

The interconnect fuse patterns are now measured rather than copied, and a pattern the tool does not know
is refused instead of being read as an unused row.

- **The ZIA fuse patterns were measured.** The table that says which fuse pattern in a ZIA row selects
  which input came, up to v9.5, from another project's notes. It has been replaced by 220 patterns
  measured from bitstreams the vendor's fitter produced for designs built so that the selected input was
  known; `ZIA_DERIVATION.md` says how. The two tables agree on every entry.
- **An unrecognised ZIA pattern was decoded as an unused row.** The old table carried one extra pattern
  per density that selects none of the row's inputs, and it and any pattern not in the table at all
  resolved to nothing, with no warning and exit 0 -- the logic came out without that input. Every ZIA row
  is now checked before anything is built: all-ones is unused, a pattern in the table selects its input,
  and anything else is reported by function block, row and bits, and the decode is refused (exit 1;
  `verify` exits 2). The vendor's fitter has not been seen to write such a pattern, so on a real
  bitstream this points to a damaged file.

Decode output is unchanged: every test file decodes to the same VHDL and UCF as v9.5, apart from the
version line.

## v9.5 vs v9.4

Two fixes. Both made the tool report success on work it had not done.

- **`--ucf` applied no names on a BGA part.** A UCF names a BGA pin by its ball ID (`D1`, `P12`), and
  the reader reduced every LOC to a number: `D1` became 0 and ball `P12` became pin 12. On CP132,
  FT256 and FG324 -- the usual packages of the XC2C128 to XC2C512 -- not one name was applied, with no
  warning and exit 0, and every port came out as `PIN_<ball>`. Pins are now compared as the strings
  they are; see the next point for how.
- **verify accepted a UCF from a different board.** It checked only that each pin the UCF named
  exists on the package, so any UCF written for the same part passed, with exit 0 and the message
  "All UCF signals map onto real macrocells of this part." It now also checks the other direction --
  every pin the bitstream uses must be named in the UCF -- reports conflicting assignments, states a
  verdict, and exits 1 when the UCF does not match. See **verify** above for what a pass does and does
  not establish.
- Pins are compared as the part's pinout spells them, by `--ucf` and by `verify` alike. On a
  wire-bond package `LOC = "1"` and `LOC = "P1"` are the same pin; on a BGA, where `P1` is a ball in
  row P, nothing is stripped. Which kind of package a part has is read from its pinout, not its name.
- verify now counts the pads of global clocks and global controls as used, as `vhdl` does.
- The UCF reader took any quoted value on a `NET` line as a pin location, whatever the constraint:
  `NET "clk" CLOCK_DEDICATED_ROUTE = "FALSE";` gave a signal on a pin named `FALSE`, and `IOSTANDARD`,
  `SLEW` and the rest were read the same way. It now matches `LOC` as a word, so other constraints are
  ignored, and it finds a `LOC` written after another constraint on the same line
  (`IOSTANDARD = "LVCMOS33" | LOC = "P1"`) or on a line whose `net` keyword is lower case.
- `decode`, `used` and `verify` refuse, rather than carry on, if the design cannot first be built
  internally (verify exit 2, the others exit 1). Without that build the pads of global clocks and
  controls are not counted as used, and verify could pass a UCF that does not match.

Decode output changes only where the first fix applies: a BGA part decoded with `--ucf` now has its
real signal names. Every other test file decodes to the same VHDL and UCF as v9.4.

## v9.4 vs v9.3

Three changes, two of them fixes to wrong output:

- **A recovered bus was widened to `(max downto 0)`.** A design whose UCF names `AD15` to `AD22` came out
  as `AD(22 downto 0)`: fifteen extra ports with no LOC constraint, which the fitter would then place on
  whatever pins were free. The declared range is now the range the UCF names.
- **The global-net fuse block was not read.** A GTS was treated as driven by its pad whenever anything
  used it, instead of being read from its own enable fuse. On a design whose GTS1 buffer is tied off --
  which means the net is driven by the buried macrocell at the GTS1 location -- three tri-state outputs
  got the pad as their output enable instead of that macrocell. The buffer's INVERT bit was lost the same
  way, which flipped the polarity of a pad-driven GSR. Both are now read from the fuse block.
- **XC2C128 and larger are no longer refused.** That refusal belonged to an older decode path. The
  present one reads the fuse map directly and handles every density; see Verified below.

## Verified

**All six densities, two implementations.** Every test file was decoded twice, once by this C tool and
once by an independent implementation written separately, and the two outputs compared after normalising
comments and VHDL that differs only in shape (`elsif rising_edge` against a nested `if`, and the order of
a process sensitivity list). 17 of 17 agree, with no difference in any signal, equation, port, range,
register or output enable:

    11 boards      ten XC2C64A and one XC2C128, each decoded with its board's UCF (one of
                   the eleven is an earlier revision of another design with the same
                   pinout, and is decoded with that design's UCF)
     6 packed       one per density, each filled as full as the fitter would place it:

                        XC2C32A    28 macrocells,   6 registers,  28 pins
                        XC2C64A    64 macrocells,  12 registers,  57 pins
                        XC2C128   128 macrocells,  24 registers,  97 pins
                        XC2C256   255 macrocells,  48 registers, 165 pins
                        XC2C384   373 macrocells,  72 registers, 225 pins
                        XC2C512   512 macrocells,  81 registers, 270 pins

**Pin placement checked against the boards' own constraint files.** For the 10 boards whose original
UCF survives, every pin the decode recovers lands on the pin that file assigns: 583 constraints, 583
agreeing, none disagreeing. Those files were written for the boards, not produced by this tool.

**BGA parts checked the same way.** Every board above is a VQ100, which numbers its pins, so two BGA
designs were written for the purpose -- an XC2C256 in FT256 and an XC2C128 in CP132, each with signals on
row-P balls and on lettered balls -- and fitted by ISE 14.7 from their source and a UCF. The fitter's own
report places all 28 signals where the UCF says. Decoded with that UCF, both implementations name all 28
ports and place all 28 pins correctly, and the two outputs agree.

**Every output analyses and elaborates.** 17 of 17 pass `ghdl -a --std=08 --warn-error` followed by
`ghdl -e`, so nothing emitted is merely plausible VHDL.

**The Windows binary was run, not just built.** It decodes all 17 files to output identical to the Linux
binary's, byte for byte, apart from line endings: the Windows build writes CRLF, which is what the tools
on that side expect.

## Known limits

- No measurement on a programmed part or a board is part of this release. Equivalence here means the
  emitted design decodes and re-fits to the same structure, not that a chip was cut and tested.
- A recompile needs `cpldfit -unused float`. Without it every pin the design does not drive is pulled
  low, and no UCF can undo that.
- A bitstream that drives no pin at all is refused rather than written as an entity with no ports. That
  is normal for a configuration probe; `decode` still reports it.
- A .jed with no `Note Block` section headers cannot be decoded: the geometry comes from those headers.
- A pin whose output enable is a global tri-state net the bitstream leaves switched off is decoded as
  permanently floating. The vendor's fitter has not been seen to write that combination, so this is an
  assumption, not a measurement.
- Signal names, comments and the structure of the original RTL are not recoverable and are not pretended
  to be. What comes back is the logic the part actually holds.

## Rebuild

```
cd src && make             Linux (any c99 compiler, C library only): xc2cdump
cd src && make windows     Windows x86-64 with x86_64-w64-mingw32-gcc: xc2cdump.exe
```

Both build with no warnings under `-Wall -Wextra`. The Windows binary is linked with a 32 MB stack
(`-Wl,--stack,33554432`, needed by the largest designs), stripped and without a link time stamp, so a
rebuild gives the same bytes -- the released .exe was checked that way, rebuilt from the released source
to the same SHA-256. Every device table is compiled in (`src/pintable.c`, `src/zia_tables_gen.h`,
`src/globalnets_gen.h`): decoding needs no other program and no data file.

## Provenance

The interconnect tables, which say what each ZIA row can select, are read from the vendor's own device
files. The fuse patterns that make those selections were measured from bitstreams the vendor's fitter
produced for designs built for the purpose; `ZIA_DERIVATION.md` describes how. The pin tables and the
global-net pad locations are read from the vendor's device files. The macrocell record layout, which
product term each control uses, and the field values the decoder acts on were measured the same way.

Three facts in this tool were taken from xc2bit, the CoolRunner-II decoder in the openfpga project, and
have not been independently measured here:

- where the global-net fuses sit: the enable and polarity bits of the global set/reset and of the four
  global tri-state nets, and the bit that selects pull-up termination;
- the clock-source codes 100, 110 and 101;
- feedback code 01, read as "no feedback".

They are not equally supported. The global-net fuse addresses are confirmed in effect: reading those bits
is what corrected the driver and polarity of the global output enables, and the boards that use those
nets decode to logic that matches their sources. The clock codes and the feedback code have never been
exercised: none of the 1,482 vendor-fitted bitstreams in the project's test set carries them, so nothing
has tested how they are read.

No xc2bit code is included in this release.

Every table and constant in the source carries a note saying where its values came from, and a check in
the project's own tree fails if one is missing. These three are what it reports as third-party.
