# Nasdaq TotalView-ITCH 5.0 message layouts

Source: [Nasdaq TotalView-ITCH 5.0 specification](https://nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf)
(latest revision in its change log: April 28, 2023, which added the Direct
Listing with Capital Raise message). `libs/itch/include/lab/itch/messages.hpp`
implements exactly these tables; a compile-time check fails the build if a
message's field list does not end at the length below.

Every message starts with the same 11 bytes:

| Offset | Length | Field |
| --- | --- | --- |
| 0 | 1 | Message Type |
| 1 | 2 | Stock Locate |
| 3 | 2 | Tracking Number |
| 5 | 6 | Timestamp (ns since midnight) |

All integers are big-endian. Price(4) is a 4-byte integer with 4 implied
decimals; Price(8) (MWCB levels only) has 8. Alpha fields are left-aligned
and space-padded. Nasdaq may append fields to a message in future, so a
longer message is accepted and the extra bytes ignored.

| Type | Message | Length | Fields after the header (offset:length) |
| --- | --- | --- | --- |
| S | System Event | 12 | event code 11:1 |
| R | Stock Directory | 39 | stock 11:8, market category 19:1, financial status 20:1, round lot size 21:4, round lots only 25:1, issue classification 26:1, issue sub-type 27:2, authenticity 29:1, short sale threshold 30:1, IPO flag 31:1, LULD tier 32:1, ETP flag 33:1, ETP leverage 34:4, inverse 38:1 |
| H | Stock Trading Action | 25 | stock 11:8, trading state 19:1, reserved 20:1, reason 21:4 |
| Y | Reg SHO Restriction | 20 | stock 11:8, action 19:1 |
| L | Market Participant Position | 26 | MPID 11:4, stock 15:8, primary MM 23:1, MM mode 24:1, participant state 25:1 |
| V | MWCB Decline Level | 35 | level 1 11:8, level 2 19:8, level 3 27:8 (Price(8)) |
| W | MWCB Status | 12 | breached level 11:1 |
| K | IPO Quoting Period | 28 | stock 11:8, release time 19:4, qualifier 23:1, IPO price 24:4 |
| J | LULD Auction Collar | 35 | stock 11:8, reference 19:4, upper 23:4, lower 27:4, extension 31:4 |
| h | Operational Halt | 21 | stock 11:8, market code 19:1, action 20:1 |
| A | Add Order | 36 | order ref 11:8, side 19:1, shares 20:4, stock 24:8, price 32:4 |
| F | Add Order with MPID | 40 | as A, plus attribution 36:4 |
| E | Order Executed | 31 | order ref 11:8, executed shares 19:4, match number 23:8 |
| C | Order Executed with Price | 36 | order ref 11:8, executed shares 19:4, match number 23:8, printable 31:1, price 32:4 |
| X | Order Cancel | 23 | order ref 11:8, cancelled shares 19:4 |
| D | Order Delete | 19 | order ref 11:8 |
| U | Order Replace | 35 | original ref 11:8, new ref 19:8, shares 27:4, price 31:4 |
| P | Trade (non-cross) | 44 | order ref 11:8, side 19:1, shares 20:4, stock 24:8, price 32:4, match number 36:8 |
| Q | Cross Trade | 40 | shares 11:8, stock 19:8, cross price 27:4, match number 31:8, cross type 39:1 |
| B | Broken Trade | 19 | match number 11:8 |
| I | NOII | 50 | paired 11:8, imbalance 19:8, direction 27:1, stock 28:8, far 36:4, near 40:4, reference 44:4, cross type 48:1, price variation 49:1 |
| N | RPII | 20 | stock 11:8, interest flag 19:1 |
| O | Direct Listing with Capital Raise | 48 | stock 11:8, eligibility 19:1, min price 20:4, max price 24:4, near price 28:4, near time 32:8, lower collar 40:4, upper collar 44:4 |

## File format of the sample days

The sample files at https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/ are gzip
"BinaryFILE" captures: each message is preceded by a 2-byte big-endian length
and there is no MoldUDP64 layer. `lab::itch::FileReader` reads them directly.
