# simex drop copy (lab protocol)

A drop copy gives a firm's back office an independent record of every
execution the exchange made for it, so positions and PnL can be reconciled
against what the trading system believes. Real venues each define their own
drop copy formats; this one is **defined by the lab**, not taken from a
Nasdaq spec. It reuses SoupBinTCP 3.00 for the session, so login, sequencing,
heartbeats and re-login replay are the same as for OUCH (see
`soupbintcp-layout.md`).

simex serves it on the order entry port to the drop copy account
(`--drop-copy USER:PASS`, default `DROP01:lab`). That account cannot send
orders; anything it sends other than heartbeats and logout is ignored. Its
sequenced stream holds one Execution Report per firm execution, for every
firm account, in the order the matching engine made them. Historical
liquidity (owner 0) is not reported.

All integers are big-endian unsigned. Prices use 4 implied decimals, as in OUCH.
Alpha fields are left-justified and space-padded.

## Execution Report (`D`), 57 bytes

| Offset | Length | Field | Notes |
| --- | --- | --- | --- |
| 0 | 1 | Message Type | `D` |
| 1 | 8 | Timestamp | nanoseconds since midnight, the exchange's feed time |
| 9 | 6 | Account | the SoupBinTCP username the order came from |
| 15 | 4 | UserRefNum | the firm's UserRefNum for the order |
| 19 | 1 | Side | `B` buy, `S` sell (short sales reported as `S`) |
| 20 | 4 | Quantity | shares executed |
| 24 | 8 | Symbol | |
| 32 | 8 | Price | execution price |
| 40 | 8 | Match Number | the same number as the OUCH Order Executed and the ITCH Order Executed |
| 48 | 1 | Liquidity Flag | `A` added, `R` removed |
| 49 | 8 | Order Reference Number | the order's number on the ITCH feed, for joining with market data |
