# B-PIPE look-alike: the lab's Bloomberg-style vendor

**This is a lab-defined protocol.** Bloomberg's B-PIPE wire protocol is
licensed and reachable only through the BLPAPI SDK, so the lab defines its
own frames and carries BLPAPI's public model over them, as described in the
public BLPAPI Developer's Guide:

- a session (`SESSION_STATUS` `SessionStarted` / `SessionTerminated`);
- an authorization (`AUTHORIZATION_STATUS`);
- the `//blp/mktdata` service (`SERVICE_STATUS` `ServiceOpened`);
- subscriptions to topics like `IBM US Equity?fields=BID,ASK&interval=2`,
  each under a client correlation ID (`SUBSCRIPTION_STATUS`
  `SubscriptionStarted` / `SubscriptionFailure` / `SubscriptionTerminated`);
- `SUBSCRIPTION_DATA` events holding `MarketDataEvents` messages of named
  elements (`MKTDATA_EVENT_TYPE`, `EVENT_TIME`, `BID`, `ASK`, `LAST_PRICE`,
  `VOLUME`, ...).

Implemented in `libs/vendor/include/lab/vendor/bpipe.hpp` and
`libs/vendor/src/bpipe_session.cpp`. All integers are big-endian.

## Frames

Every frame: u32 length (bytes after the length field), then a kind byte.
Strings are u16 length then bytes.

| Kind | Direction | Body |
| --- | --- | --- |
| `S` SessionStart | client | string application name |
| `A` Authorize | client | u64 cid, string user, string ip |
| `O` OpenService | client | u64 cid, string service (`//blp/mktdata`) |
| `B` Subscribe | client | u64 cid, string topic |
| `U` Unsubscribe | client | u64 cid |
| `E` Event | server | u8 event type, u64 cid, string message type, u16 element count, elements |

Event types (names from BLPAPI, codes the lab's): 1 SESSION_STATUS,
2 SERVICE_STATUS, 3 AUTHORIZATION_STATUS, 4 SUBSCRIPTION_STATUS,
5 SUBSCRIPTION_DATA.

Element: string name, type byte, value:

| Type | Value |
| --- | --- |
| `D` decimal | i64 mantissa, i8 exponent (`100.05` is 10005, -2) |
| `I` integer | i64 |
| `S` string | string |
| `E` enumeration | string |
| `T` time | u64 ns since midnight |

Prices are always decimals, never binary floating point.

## Flow

1. `S` → `SESSION_STATUS SessionStarted`.
2. `A` → `AUTHORIZATION_STATUS AuthorizationSuccess`, or `AuthorizationFailure`
   (blank user, or no session).
3. `O //blp/mktdata` → `SERVICE_STATUS ServiceOpened`, else `ServiceOpenFailure`.
4. `B cid topic` → `SubscriptionStarted` (with `exceptions.fieldId` naming
   unknown fields), then a `MarketDataEvents` initial paint (`SUMMARY` /
   `INITPAINT`), then updates. Failures: `NOT_AUTHORIZED`,
   `DUPLICATE_CORRELATIONID`, `BAD_SEC` (`Invalid security`), `BAD_FLD`
   (no valid field), with `reason.source`, `reason.errorCode`,
   `reason.category`, `reason.description`.
5. `U cid` → `SubscriptionTerminated`.
6. The venue's day ends → `SESSION_STATUS SessionTerminated`, then the server
   closes the connection.

Topic: `[//blp/mktdata/][ticker/]<SYMBOL> <EXCH> Equity[?fields=F,F&interval=S]`.
With no `fields` every field is sent; `interval` (seconds) conflates.

## MarketDataEvents

`MKTDATA_EVENT_TYPE` / `MKTDATA_EVENT_SUBTYPE`: `SUMMARY`/`INITPAINT`,
`TRADE`/`NEW`, `QUOTE`/`BID` or `ASK`, `SUMMARY`/`TRADING_STATUS`,
`SUMMARY`/`AUCTION` (imbalance) or `SUMMARY`/`PRICE_LIMITS` (LULD band). Then
`EVENT_TIME` (the ITCH time of the change), then the wanted fields that
changed (all of them in the initial paint):

| Field | Type |
| --- | --- |
| BID, ASK, LAST_PRICE, OPEN, HIGH, LOW | D (0 means no value) |
| BID_SIZE, ASK_SIZE, SIZE_LAST_TRADE, VOLUME, NUM_TRADES | I |
| TRADING_STATUS | E: `TRADING`, `HALTED`, `PAUSED`, `QUOTATION_ONLY` |
| HALT_REASON | S: the ITCH reason (`T1`, `LUDP`, `MWC1`, ...), empty when trading |
| REG_SHO_RESTRICTION | E: `NONE`, `IN_EFFECT`, `CONTINUED` |
| AUCTION_TYPE | E: `OPENING`, `CLOSING`, `HALT`, `NONE` |
| INDICATIVE_PRICE | D |
| PAIRED_SHARES, IMBALANCE_SHARES | I |
| IMBALANCE_SIDE | E: `BUY`, `SELL`, `NONE`, `INSUFFICIENT_ORDERS`, `PAUSED`, `NOT_SET` |
| LULD_LOWER_LIMIT, LULD_UPPER_LIMIT | D (0 means no band yet) |

The fields from HALT_REASON on are lab names, not Bloomberg mnemonics.
