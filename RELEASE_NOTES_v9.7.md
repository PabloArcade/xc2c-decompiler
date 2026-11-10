# xc2cdump v9.7

CoolRunner-II (XC2C) JEDEC fuse map decompiler by Darksoft. Use at your own risk.

The zip holds the source and both binaries, Linux x86-64 and Windows x86-64.
See README.md for the commands, the supported parts and the limits.

v9.6 is withdrawn. It wrote wrong logic, with no warning and exit 0, for the designs described in the
first two sections below. Use this release instead.

## A macrocell's own product-term set, reset and clock were decoded wrongly

Each macrocell can take its reset, preset and clock from a product term of its own instead of from a
global net or a term the whole function block shares. v9.6 got both wrong:

- an own-term reset or preset was emitted as a constant 0, so the register could never be reset;
- on the XC2C128 to XC2C512, a register clocked by its own product term was given the function block's
  shared clock term instead.

A real board was affected. On `u12_main`, fitted to an XC2C128, the latch behind `DAT_BUF_OEn_o` came
out gated by `STM_STROBE_i`. The vendor's fitter report gives `NOT STM_RST_i AND NOT STM_MODE_i`, and
that is what v9.7 emits. Decoded from the XC2C128 and from the same board fitted to an XC2C64A, the
two designs now simulate identically over 4,000 vectors; with v9.6 they differ.

Which product term each control uses was measured for this release, without looking at the existing
tables: 204 designs fitted by the vendor's tools on all six densities, built so that every term could be
told apart, cover the shared clock, reset, set and output-enable terms and each macrocell's own set/reset,
output-enable and clock terms, from the first macrocell of a block to the last. The result was frozen and
then compared with the tables the tool already had. They agree; the fault was in how the decoder used
them. All 210 measurement designs now decode to logic that simulates identically to their source; 156 of
them did not with v9.6.

## A global output enable driven by logic was decoded as always on

When several pins share an output enable computed from other signals, the fitter can drive one of the
global tri-state nets from a macrocell. If nothing else read that macrocell, v9.6 dropped it and the
pins came out permanently driven. They now follow the enable. Six designs built to show it went from
thousands of mismatches against their source to none.

## --device accepted a part of another density

`--device` took any part name, exited 0 and placed every pin from the wrong part's pinout. It must now
match the number of function blocks in the fuse map. The A and non-A parts of one density share a
pinout and are still interchangeable.

## --lock-macrocells built nothing on parts with buried registers

It locked combinational signals that synthesis folds into the register, and ngdbuild stopped at the
first one. That happened on every XC2C128 to XC2C512 design tried. It now locks registers only, and lists
the combinational macrocells it leaves unlocked.

## The rest

- **A `--ucf` net named like a VHDL reserved word or like a name the tool generates broke the output.**
  A net called `out`, `signal` or `register`, or `gsr`, `gck_0` or `gts_1`, gave VHDL that does not
  compile, with exit 0. The board's own names are now kept as written and the tool's generated names give
  way to them, compared without regard to case. A reserved word cannot be a VHDL name, so it becomes
  `<name>_pin` in both the VHDL and the UCF. Every rename is noted in a comment line of the VHDL.
- **`decode` lists buried macrocells**, marked `buried`. It used to report only macrocells with a pin.
- `config` describes several fields more accurately, and `--no-pin-table` says in its help what it does.
- **The XC2C128 CV100 pin table is right where the vendor's file is wrong.** ISE's device file gives
  ball G4 to two pads. Placing a signal on each ball and reading the fitter's report settles it: G4 is
  FB4_MC5, and the GCK2 pad FB2_MC16 is on K2, which is what the tool uses.
- A new limit is stated in the README: a pin whose output enable is a global tri-state net that the
  bitstream leaves switched off is decoded as permanently floating. The vendor's fitter has not been seen
  to write that combination, so this is an assumption, not a measurement.

Nothing else in the output moved. Of 367 test bitstreams, nine decode differently from v9.6, and they are
the designs these fixes are about. The others give the same VHDL and UCF apart from the version line, as
do the boards decoded with their own UCF files.

## Correction: where the tables come from

v9.6's README said that the pin tables, the global-net locations and every macrocell field encoding had
been worked out from the vendor's device files and from measurement, and its release notes said the README
now stated where each table came from. That claimed more than was true. When v9.6 shipped, several things
the decoder relies on still came from xc2bit, the CoolRunner-II decoder in the openfpga project: which
product term each control uses, the shared control terms, several output-enable, XOR-input and clock
codes, and the three facts below.

This release measured the first group, as described above. Three facts in this tool were taken from
xc2bit and have not been independently measured here:

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

The README's Provenance section now says the same. Every table and constant in the source carries a note
saying where its values came from, and a check in the project's own tree fails if one is missing. That
check is what found the overstatement, and these three are what it now reports as third-party.

## Checked

- Every defect fixed here has a check that reads the tool's output on a design whose right answer is
  known from outside it: the fitter's own equation, the design's source, or the same board on another
  part. Each check passes on v9.7 and fails on v9.6, which shows it can see the defect at all.
- 41 designs are decoded, rebuilt by the vendor's tools and simulated against the original: 34 pass.
  Six are refused because they are test files written by another tool without the section headers the
  decoder needs (a README limit), and one fills its part so completely that the rebuild has no room.
- 17 designs decode identically here and in an independent implementation, and also simulate
  identically against it; all pass ghdl -a --std=08 --warn-error and ghdl -e. 583 of 583 pin
  constraints from the boards' own UCFs agree with the decode, 28 of 28 on the BGA designs, all eleven
  verify cases give the expected result, and all 42 built-in pin tables match the vendor's device files.
- Both binaries build with no warnings under -Wall -Wextra and rebuild to the same bytes.
- The Windows binary was checked on Windows 11, not an emulator, against the Linux build's output: 51
  checks across 26 tests, all identical. They cover every defect fixed in this release, and the v9.6
  binary fails the new ones.
