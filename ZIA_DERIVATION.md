# How the ZIA encoding was worked out

This note explains how I worked out the CoolRunner-II interconnect fuse encoding that `xc2cdump` uses to
decode a bitstream. As far as I know it hasn't been written down anywhere else, so the method and the
numbers are here for anyone who wants to check them or do the same for another part.

## What the ZIA is, and why the encoding matters

A CoolRunner-II is made of function blocks: 16 macrocells each, with a programmable AND array that builds
product terms. Every function block has 40 inputs, and all of them arrive through one global switch.
Xilinx's data sheets call it the Advanced Interconnect Matrix; the fuse map's own section headers call it
the ZIA, and so does this note.

Each of a block's 40 inputs is one ZIA row. A row can select exactly one source from a fixed list: an
input pad, a macrocell output, or (on the 32-macrocell part) the dedicated input pin. The list is 6
sources long on an XC2C32, 12 on the 64, 22 on the 128, 40 on the 256, 62 on the 384 and 78 on the 512.
Which source the row selects is set by a group of fuses, 8, 16, 28, 48, 74 or 88 of them depending on the
density.

The AND array only refers to row numbers. A product term says "row 17, true" or "row 3, inverted"; it
never names a signal. So to turn a bitstream back into logic you need two things for every row: the list
of sources the row can select, and which fuse pattern selects which entry in that list. Without the
second, every product term is a formula over unknown variables.

## What the vendor's files contain, and what they don't

The source lists come straight from Xilinx. ISE 14.7 ships a device file for every part in
`xbr/data/<device>.chp`, an XML file with a `<pad>` entry for every pin and an `<imux>` table. The
`<imux>` has one line per ZIA row, listing the row's sources as numbers in a fixed order, with the columns
labelled G0, G1, and so on. Each `<pad>` says which number is that pin's pad (`PZIA`) and which is its
macrocell's output (`MZIA`). Together they give the full map, 8,800 entries over the six densities.
`xbr.dev` confirms the list lengths independently: its `ZIAGATES` column reads 6, 12, 22, 40, 62, 78.

**No vendor data file states the fuse encoding.** I read every file that looked like a candidate, so the
next person doesn't have to:

- `xc2c*.map` (one per part) is a physical layout grid. Each cell holds a JED fuse number or one of the
  labels `spare`, `sec_n` or `done`. It tells you where each fuse sits on the die. A ZIA row's fuses lie
  in one column, evenly spaced, interleaved with another block's fuses, with no gap or marker at any group
  boundary. It says nothing about what a fuse means.
- `xc2c*.chp` and the older text-format `xbr.chp` hold the pads and the `<imux>` source lists described
  above. They contain no fuse numbers at all.
- `xbr.dev` lists the enabled devices and packages, plus per-part counts (`NUMFUSES`, `ZIAGATES`,
  `MCBITS`). It holds counts only, no encoding. One useful oddity: it does not enable the FT256 package
  for the XC2C128, although the `.chp` lists pins for it.
- `xbr.cmf` is a timing cell model: paths such as input to `II_IMUX`, with delays.
- `xbr.tpf` holds timing parameters, including a "ZIA delay".
- The power-estimation schema `data/xml/Power/pwrXBRTopo.dtd` says outright that there is "no explicit
  element for ZIAs".
- The `xpla3` tree, for the older XPLA3 family, has the same kinds of files. There the ZIA appears only as
  the `PZIA`/`MZIA` pin attributes and as the names of test pins.

The encoding lives in code: `xbr/lib/lin64/libCbitXbr_bitmap.so` exports a function called
`WriteXbrBITMAP::SetZiaBit`. I didn't disassemble it. Everything below was measured instead.

## The method: force a known source, check the forcing, read the fuses

The idea is to build designs in which I know, by construction, which source each block reads; have ISE
fit them; and read the ZIA rows back out of the JED.

One fact about ISE's own `<imux>` table makes this much simpler than it sounds. **Every source appears in
exactly one column**, on all six densities. A source sits on three or four rows, but always in the same
position in each row's list. So the choice index is a property of the source alone. I don't need to know
which of its rows the fitter will pick: whichever one it picks, the fuse pattern on that row is the
pattern for that source's column.

Each design gives every function block a single output pin, placed in that block, that copies one chosen
source:

- For a pad source, the output is a plain copy of a bonded input pad.
- For a macrocell source, a register is placed on the chosen macrocell with a `LOC` constraint. Its data
  input comes from a spare pad and its clock from GCK0, which reaches it on the global clock net rather
  than through the ZIA. The output is then a second flip-flop clocked from that register.

The second flip-flop matters. With a plain `o <= r`, XST merged the register and the output into one net,
and cpldfit printed "Ignoring assignment" and built the register in the output's own macrocell. The
design fitted and the JED looked normal, but the block was reading a pad instead of the macrocell I had
placed. That is exactly the kind of fit that passes but doesn't exercise what you think, and it is why
I don't trust any fit without the check below.

**Every fit is checked from the fuses before any reading from it is used**:

1. For each block, the number of ZIA rows that are not all-ones must equal the number of distinct sources
   that block reads by construction. A register's block counts its data pad as well. If anything was
   optimised away, moved, duplicated or routed through the ZIA unexpectedly, some block's count is off.
2. The used rows are matched to the declared sources, allowing a source only on rows that ISE's
   `<imux>` says can carry it. A block counts only if exactly one matching is possible.
3. If the fitter's log says it ignored a placement, the fit is rejected.

Any failure rejects the whole fit, not just the block. The choice index of every reading comes from the
`<imux>` alone.

### Why the fitter's report can't be used

The obvious shortcut is to let cpldfit's report say which signal is on which row. There have been three
earlier attempts at this table, and all three took that route; none of them stands.

- The report's per-block list of signals is a set. It says which signals a block uses, not which row
  each one sits on. Turning it into row assignments mapped 86 patterns to more than one choice, and 43
  of the rest disagreed with the table it was meant to replace.
- The report also prints a 40-column map per block, which looks like exactly the per-row ground truth
  you want. **Those 40 columns are not the JED's 40 ZIA rows.** They don't follow the JED's row order,
  so a table built from them is only consistent with itself.
- Checking observed patterns against an existing table, and counting how many fit, proves only that the
  table is self-consistent. It doesn't derive anything.

The construction above needs no report at all: the source is known because the design allows only one,
and the forcing is verified from the fuses.

## The numbers

I ran two campaigns with independent random seeds, on all eight parts (the XC2C32 and XC2C32A, the XC2C64
and XC2C64A, and the four large parts). Pads were taken from the largest package ISE enables for each part:
VQ44, VQ100, TQ144 for the 128, FT256 for the 256, and FG324 for the 384 and 512.

- 358 designs fitted. 353 passed every check; 5 were rejected, because 6 blocks that read two sources could
  be matched to their rows in two ways.
- 2,936 readings, covering all 220 choices on all six densities, with no conflicts: every reading of a
  choice gave the same pattern.
- Every choice was seen on at least two different rows, from at least two different sources, in at least
  two different function blocks. The weakest density is the XC2C384, at two of each. The XC2C64 has every
  choice on ten or more rows.
- The A and non-A parts were measured separately. They share one `<imux>` table and gave identical
  patterns.

Before either campaign, a smoke test on the XC2C32A had two fits rejected by the check, and those two are
what exposed the register-merging trap. They aren't counted above.

Two independent checks:

- **The vendor's own output.** Separately, I counted the zeros in every ZIA row of 484 distinct
  bitstreams that ISE had fitted for unrelated designs: 196,080 rows. At every density the number of
  distinct non-blank patterns ISE ever writes is exactly the number of choices, and they are the same 220
  patterns. Every row is either all-ones or one of the 220.
- **The decoder itself.** With the measured table in place, `xc2cdump` decodes every test bitstream to
  the same VHDL and UCF as before, and the full test suite gives the same results.

## The structure

The measured table shows an active-low, two-level multiplexer:

- One **enable** bit is 0 on every used row.
- The sources are split into groups, and each group has a **group-select** bit, 0 when the selected
  source is in that group.
- Within the group, one **member** bit is 0 for the selected source.
- An unused row is all-ones.

So a used row has three zeros. The XC2C32 has a single group and no select bit, so its used rows have two
zeros. The XC2C384 adds a third level: bit 0 is also 0 for the first 27 of its 62 choices, giving those
rows four zeros.

The group sizes add up to the choice count at every density:

| Part | Fuses per row | Enable bit | Groups (select bit: number of members) | Total | Bit never used |
|---|---|---|---|---|---|
| XC2C32  | 8  | 0  | one group: 6 | 6 | 1 |
| XC2C64  | 16 | 7  | 12: 6, 3: 6 | 12 | 8 |
| XC2C128 | 28 | 9  | 24: 5, 18: 5, 12: 5, 4: 7 | 22 | 8 |
| XC2C256 | 48 | 40 | 44: 5, 36: 7, 28: 7, 20: 7, 12: 7, 4: 7 | 40 | 41 |
| XC2C384 | 74 | 1  | with bit 0 also 0 -- 70: 6, 63: 7, 55: 7, 47: 7; then 39: 7, 31: 7, 23: 7, 15: 7, 7: 7 | 27 + 35 = 62 | 2 |
| XC2C512 | 88 | 44 | 87: 9, 67: 10, 66: 10, 45: 10, 42: 10, 21: 10, 20: 10, 0: 9 | 78 | 43 |

Bit numbers count from 0 at the start of the row, in the order the JED lists the row's fuses. Each
density has exactly one bit position that no choice ever uses.

## The rule

**Choice n is the n-th member bit counting down from the top of the row.** Choice 0 is the highest-numbered
member bit, choice 1 the next one down, and so on; the pattern is that member bit, its group's select bit
and the enable bit (and on the 384, bit 0 for choices 0 to 26). The rule fits all 220 measured entries,
not a sample of them. Combined with the `<imux>` column order (choice n is column G*n*), it gives the
whole decode from the vendor's device files plus one line of arithmetic.

A pattern that is neither all-ones nor one of the 220 is not something the vendor's fitter writes. In
particular, the enable bit together with the unused bit selects nothing in the `<imux>`, and ISE never
produced it in any of the bitstreams above. `xc2cdump` therefore refuses a bitstream containing any other
pattern, and reports the function block, row and bits, rather than treating the row as unused.

## Working clean-room

Before this, the project decoded the ZIA with a third-party table, checked against bitstreams but never
derived. The point of this work was to remove that dependency, and a measurement that has seen the answer
isn't independent. So the existing table was quarantined from the start. Its values weren't printed,
compared, or used to seed, check or break a tie, and neither were the outputs of tools that decode the ZIA
with it. The derived table was frozen (with a SHA-256 hash recorded) and passed the full test suite before
it was compared with the old one, once, at the end: **all 220 entries agree.**

The old table carried one extra pattern per density beyond the real choices; its authors marked the one on
the XC2C64 as not certain. It turns out to be the enable bit together with the unused bit. The vendor's
fitter never wrote it in any bitstream examined, and no source in the `<imux>` corresponds to it, so the
measured table doesn't include it.

There was one slip, and I'm recording it because the independence claim depends on it being stated. While
counting zeros across the bitstreams in the project, my first pass included 88 files that had been written
by the third-party project's own tools rather than by ISE. Chasing one odd pattern led me to one of them,
and its first eight fuse lines appeared on screen: seven ZIA rows of an XC2C32 holding seven different bit
patterns. That shows a set of patterns. It does not show which choice any of them selects, and that
assignment was the whole unknown. Afterwards:

- The count was redone using only files written by ISE.
- The XC2C32's assignment was taken purely from the constructed designs, without using that count at all.
- The one pattern seen there that ISE never writes was kept out of the table unless measurement produced
  it. It never did, and it turned out to be the old table's extra entry for that part.

The result doesn't rest on what was seen. It is recorded so that nobody has to take that on trust.
