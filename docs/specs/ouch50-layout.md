# Nasdaq OUCH 5.0 message layouts

Source: [Nasdaq OUCH 5.0 specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/TradingProducts/OUCH5.0.pdf).
`libs/ouch/include/lab/ouch/messages.hpp` implements the messages marked
"lab" below, each as one field list that drives both encode and decode; a
compile-time check fails the build if a list does not end at the length here.
OUCH messages travel inside SoupBinTCP Unsequenced (client to exchange) and
Sequenced (exchange to client) packets.

Types: UserRefNum is a 4-byte unsigned integer, unique and strictly
increasing per account for the day (lower ones are ignored as
retransmissions). Price is 8 bytes with 4 implied decimals; the maximum is
199,999.9900, and 214,748.3647 (0x7FFFFFFF) marks a market order. Alpha fields
are left-justified and space-padded. Integers are big-endian. Timestamps are
nanoseconds since midnight. Every message ends with a 2-byte Appendage Length
followed by that many bytes of optional TagValue fields; the lab sends none
and skips any it receives.

## Inbound (firm to exchange)

| Type | Message | Fixed length | Fields (offset:length) | Lab |
| --- | --- | --- | --- | --- |
| O | Enter Order | 47 | UserRefNum 1:4, Side 5:1, Quantity 6:4, Symbol 10:8, Price 18:8, Time In Force 26:1, Display 27:1, Capacity 28:1, ISO 29:1, CrossType 30:1, ClOrdID 31:14, Appendage Length 45:2 | yes |
| U | Replace Order | 40 | OrigUserRefNum 1:4, UserRefNum 5:4, Quantity 9:4, Price 13:8, Time In Force 21:1, Display 22:1, ISO 23:1, ClOrdID 24:14, Appendage Length 38:2 | yes |
| X | Cancel Order | 11 | UserRefNum 1:4, Quantity 5:4, Appendage Length 9:2 | yes |
| M | Modify Order | 12 | UserRefNum 1:4, Side 5:1, Quantity 6:4, Appendage Length 10:2 | no |
| Q | Account Query | 3 | Appendage Length 1:2 | yes |

## Outbound (exchange to firm)

| Type | Message | Fixed length | Fields (offset:length) | Lab |
| --- | --- | --- | --- | --- |
| S | System Event | 10 | Timestamp 1:8, Event Code 9:1 (S start of day, E end of day) | yes |
| A | Order Accepted | 64 | Timestamp 1:8, UserRefNum 9:4, Side 13:1, Quantity 14:4, Symbol 18:8, Price 26:8, Time In Force 34:1, Display 35:1, Order Reference Number 36:8, Capacity 44:1, ISO 45:1, CrossType 46:1, Order State 47:1, ClOrdID 48:14, Appendage Length 62:2 | yes |
| U | Order Replaced | 68 | Timestamp 1:8, OrigUserRefNum 9:4, UserRefNum 13:4, Side 17:1, Quantity 18:4, Symbol 22:8, Price 30:8, Time In Force 38:1, Display 39:1, Order Reference Number 40:8, Capacity 48:1, ISO 49:1, CrossType 50:1, Order State 51:1, ClOrdID 52:14, Appendage Length 66:2 | yes |
| C | Order Canceled | 20 | Timestamp 1:8, UserRefNum 9:4, Quantity 13:4, Reason 17:1, Appendage Length 18:2 | yes |
| E | Order Executed | 36 | Timestamp 1:8, UserRefNum 9:4, Quantity 13:4, Price 17:8, Liquidity Flag 25:1, Match Number 26:8, Appendage Length 34:2 | yes |
| B | Broken Trade | 38 | Timestamp 1:8, UserRefNum 9:4, Match Number 13:8, Reason 21:1, ClOrdID 22:14, Appendage Length 36:2 | yes |
| J | Rejected | 31 | Timestamp 1:8, UserRefNum 9:4, Reason 13:2, ClOrdID 15:14, Appendage Length 29:2 | yes |
| Q | Account Query Response | 15 | Timestamp 1:8, NextUserRefNum 9:4, Appendage Length 13:2 | yes |
| D, P, I, T, M, R | AIQ Canceled, Cancel Pending, Cancel Reject, Priority Update, Modified, Restated | | | no |

## Codes the lab uses

- Side: B buy, S sell, T sell short, E sell short exempt.
- Time In Force: 0 day, 3 immediate or cancel (IOC), 5 GTX, 6 GTT, E after hours.
  simex supports 0 and 3; it accepts the others as day orders.
- Display: Y visible, N hidden, A attributable. simex shows every order.
- Order State: L live, D dead (accepted, then canceled at once, e.g. IOC).
- Cancel reasons: U user requested, I immediate or cancel, S supervisory,
  Z system, T time in force expired, Q self-match prevention.
- Liquidity flags (Appendix D): A added, R removed.
- Reject reasons (2-byte): 0x0006 fat finger, 0x0007 halted, 0x0009 invalid
  side, 0x000A processing error, 0x000F other, 0x0013 invalid quantity,
  0x0015 replace not allowed, 0x0017 invalid symbol, 0x001D invalid price.

## Lab interpretations

- Cancel Order Quantity is the new intended remaining size: 0 cancels the
  order, a smaller number reduces it, a larger one changes nothing.
- Replace Order Quantity counts the whole chain including shares already
  executed (spec wording); the replacement rests with Quantity minus the
  chain's executed shares, and always loses time priority.
