# ICF: the lab's consolidated vendor feed (ICE Consolidated Feed look-alike)

**This is a lab-defined protocol.** ICE's own wire format (the CTF token
protocol behind the ICE Consolidated Feed) is licensed, so nothing here is a
copy of it. What the lab borrows is the public model, as described on ICE's
public product pages and API notes:

- clients send text commands such as `SUBSCRIBE,SYMBOL_TICKER:AAPL,ENUM_SRC_ID:…`,
  `QUERYSNAP`, `QUERYSNAPANDSUBSCRIBE`, `SUBSCRIBEWILDCARD` (with a `{regex}`)
  and `UNSUBSCRIBE`, with options such as `CONFLATION_INTERVAL`;
- each venue has a numeric source id (`ENUM_SRC_ID`);
- market data travels as numbered tokens, each a field id with a typed value.

The layouts below are fixed by this document and implemented in
`libs/vendor/include/lab/vendor/icf.hpp`; `icf-server` serves it and
`vendor-client --vendor icf` speaks it. All integers are big-endian.

## Transport

SoupBinTCP 3.0 (`soupbintcp-layout.md`), unchanged:

| Direction | SoupBinTCP packet | Carries |
| --- | --- | --- |
| client to server | Login Request `L` | any non-blank username; password and session ignored |
| server to client | Login Accepted `A` / Login Rejected `J` (`A` not authorized) | session `LABICF` |
| client to server | Unsequenced Data `U` | one or more commands, text |
| server to client | Sequenced Data `S` | a Response `R` or a Market Data `M` message |
| both | heartbeats `H` / `R` | server after 1 s idle; the server drops a client silent for 15 s |
| server to client | End of Session `Z` | the venue's feed ended |

## Commands

`VERB,PARAM:VALUE,PARAM:VALUE;VERB,...` in ASCII. Commands are separated by
`;`, parameters by `,`; a value in braces `{...}` may contain commas.

| Verb | Effect |
| --- | --- |
| `SUBSCRIBE` | Response, then an Image, then updates |
| `QUERYSNAP` | Response, then one Image |
| `QUERYSNAPANDSUBSCRIBE` | same as `SUBSCRIBE` |
| `SUBSCRIBEWILDCARD` | `SYMBOL_TICKER` is an ECMAScript regex; an Image and updates for every match |
| `UNSUBSCRIBE` | stops updates for `SYMBOL_TICKER` |

| Parameter | Meaning |
| --- | --- |
| `SYMBOL_TICKER` | venue symbol, or a regex for `SUBSCRIBEWILDCARD` |
| `REQUEST_ID` | client's id, echoed in the Response and every Market Data message (default: server-assigned) |
| `ENUM_SRC_ID` | source id; must equal the server's (`--source-id`, default 1) |
| `CONFLATION_INTERVAL` | milliseconds; at most one update per interval, carrying every field changed since the last |

An unknown symbol is refused with `unknown SYMBOL_TICKER X`; a client may
retry once the venue lists it.

## Response `R`

| Offset | Length | Field |
| --- | --- | --- |
| 0 | 1 | `R` |
| 1 | 4 | request id |
| 5 | 1 | `A` accepted, `E` error |
| 6 | 2 | text length n (at most 512) |
| 8 | n | text |

## Market Data `M`

| Offset | Length | Field |
| --- | --- | --- |
| 0 | 1 | `M` |
| 1 | 1 | event: `I` image, `Q` quote, `T` trade, `S` status |
| 2 | 2 | source id |
| 4 | 4 | request id |
| 8 | 8 | symbol, ASCII, space padded |
| 16 | 8 | exchange time: ITCH timestamp of the change, ns since midnight |
| 24 | 8 | vendor time: when the server sent it, ns since midnight |
| 32 | 4 | record sequence: changes to this record so far |
| 36 | 1 | token count n (at most 16) |
| 37 | 11 × n | tokens |

Token, 11 bytes:

| Offset | Length | Field |
| --- | --- | --- |
| 0 | 2 | token id |
| 2 | 1 | type: `P` price (Price(4), signed), `I` integer, `T` time (ns since midnight), `C` character (low byte) |
| 3 | 8 | value, signed |

An Image carries every token; an update carries those whose field changed.

| Id | Name | Type |
| --- | --- | --- |
| 1 | QUOTE.BID.PRICE | P |
| 2 | QUOTE.BID.SIZE | I |
| 3 | QUOTE.ASK.PRICE | P |
| 4 | QUOTE.ASK.SIZE | I |
| 5 | QUOTE.DATETIME | T |
| 10 | TRADE.PRICE | P |
| 11 | TRADE.SIZE | I |
| 12 | TRADE.DATETIME | T |
| 20 | DAY.VOLUME | I |
| 21 | DAY.OPEN | P |
| 22 | DAY.HIGH | P |
| 23 | DAY.LOW | P |
| 24 | DAY.TRADES | I |
| 30 | STATUS.TRADING | C: `T` trading, `H` halted, `P` paused, `Q` quotation only |

A price of 0 means no value (an empty side, no trade yet).
