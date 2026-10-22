# FIX 4.4 subset spoken by `fixgw`

Source: the FIX 4.4 specification as published in the FIX dictionary at
[OnixS FIX 4.4](https://www.onixs.biz/fix-dictionary/4.4/index.html) (message
pages `msgType_*`, field pages `tagnum_*`, component `compBlock_StandardHeader`)
and [InfoReach FIX 4.4](https://www.inforeachinc.com/fix-dictionary/index.html),
cross-checked against the [FIX article](https://en.wikipedia.org/wiki/Financial_Information_eXchange)
for framing. FIX is tag=value, so there are no offsets: this file fixes which
tags the lab sends and accepts, in which order, and what each value means.
`libs/fix` implements the framing and session layer; `exchange/fixgw` maps
the application messages onto OUCH 5.0 for simex.

## Framing

A message is a run of `tag=value<SOH>` fields (SOH is byte `0x01`).

| Position | Tag | Field | Rule |
| --- | --- | --- | --- |
| first | 8 | BeginString | `FIX.4.4` |
| second | 9 | BodyLength | characters from the first byte after the `9=...<SOH>` field (the `3` of `35=`) up to and including the SOH before `10=` |
| third | 35 | MsgType | |
| last | 10 | CheckSum | sum of every byte before `10=`, modulo 256, as exactly three digits (`10=062`) |

Standard header, in order: 8, 9, 35, 49 SenderCompID, 56 TargetCompID,
34 MsgSeqNum, then 43 PossDupFlag (`Y` on retransmissions), 52 SendingTime,
122 OrigSendingTime (required on retransmissions). Of the optional header
fields only 43 and 122 are used. SendingTime is a UTCTimestamp,
`YYYYMMDD-HH:MM:SS.sss`; the lab accepts the whole-second form
`YYYYMMDD-HH:MM:SS` too.

Data types: Price is a decimal string such as `100.25`; the lab converts it to
an integer with 4 implied decimals (OUCH/ITCH Price(4)) and rejects more than
four decimal places. Qty and SeqNum are non-negative integers. Strings may not
contain SOH. The lab limits ClOrdID to 20 characters.

## Session messages

| MsgType | Message | Body fields (R = required) |
| --- | --- | --- |
| `A` | Logon | 98 EncryptMethod R (`0`, none), 108 HeartBtInt R (seconds; the acceptor echoes the initiator's), 141 ResetSeqNumFlag (`Y`: both sides restart at 1), 553 Username, 554 Password |
| `0` | Heartbeat | 112 TestReqID (R when answering a Test Request: echo its 112) |
| `1` | TestRequest | 112 TestReqID R |
| `2` | ResendRequest | 7 BeginSeqNo R, 16 EndSeqNo R (`0` = to the end) |
| `3` | Reject | 45 RefSeqNum R, 371 RefTagID, 372 RefMsgType, 373 SessionRejectReason, 58 Text |
| `4` | SequenceReset | 123 GapFillFlag (`Y` gap fill, `N` or absent reset), 36 NewSeqNo R (the next sequence number to expect; may only move forward) |
| `5` | Logout | 58 Text |

SessionRejectReason (373) values used: `0` invalid tag number, `1` required
tag missing, `5` value out of range, `6` incorrect data format, `9` CompID
problem, `11` invalid MsgType, `99` other.

Session rules the lab implements:

- The first message on a connection must be a Logon with MsgSeqNum the
  receiver expects (or 1 with ResetSeqNumFlag `Y`). The acceptor answers with
  its own Logon. Anything else first gets a Logout and a disconnect.
- Sequence numbers persist across reconnects for the day unless a Logon
  carries ResetSeqNumFlag `Y`.
- MsgSeqNum higher than expected: the receiver sends a ResendRequest from the
  expected number with EndSeqNo 0 and does not process later messages until
  the gap is filled. Lower than expected with PossDupFlag `Y`: ignored. Lower
  without PossDupFlag: Logout ("MsgSeqNum too low") and disconnect.
- A ResendRequest is answered by resending stored application messages with
  PossDupFlag `Y` and OrigSendingTime, and by SequenceReset-GapFill over
  session messages, which are never resent.
- With no traffic for HeartBtInt seconds a side sends a Heartbeat; with
  nothing received for HeartBtInt plus a grace period it sends a TestRequest,
  and disconnects if the TestRequest goes unanswered.

## Application messages, firm to venue

| MsgType | Message | Body fields (R = required) |
| --- | --- | --- |
| `D` | NewOrderSingle | 11 ClOrdID R, 1 Account, 55 Symbol R (Instrument), 54 Side R, 60 TransactTime R, 38 OrderQty R (OrderQtyData), 40 OrdType R, 44 Price (R for limit), 59 TimeInForce (absent = Day) |
| `F` | OrderCancelRequest | 41 OrigClOrdID R, 11 ClOrdID R, 37 OrderID, 55 Symbol R, 54 Side R, 60 TransactTime R, 38 OrderQty R |
| `G` | OrderCancelReplaceRequest | 41 OrigClOrdID R, 11 ClOrdID R, 37 OrderID, 55 Symbol R, 54 Side R, 60 TransactTime R, 38 OrderQty R (the new total size, executed shares included), 40 OrdType R, 44 Price, 59 TimeInForce |

Accepted values: Side (54) `1` Buy, `2` Sell, `5` Sell short, `6` Sell short
exempt. OrdType (40) `1` Market, `2` Limit. TimeInForce (59) `0` Day,
`3` Immediate or Cancel; market orders are always IOC.

## Application messages, venue to firm

| MsgType | Message | Body fields sent |
| --- | --- | --- |
| `8` | ExecutionReport | 37 OrderID, 11 ClOrdID, 41 OrigClOrdID (replaces and cancels), 17 ExecID, 150 ExecType, 39 OrdStatus, 55 Symbol, 54 Side, 38 OrderQty, 44 Price, 59 TimeInForce, 32 LastQty and 31 LastPx (fills), 151 LeavesQty, 14 CumQty, 6 AvgPx, 60 TransactTime, 103 OrdRejReason and 58 Text (rejects) |
| `9` | OrderCancelReject | 37 OrderID R (`NONE` if unknown), 11 ClOrdID R, 41 OrigClOrdID R, 39 OrdStatus R, 434 CxlRejResponseTo R (`1` cancel, `2` cancel/replace), 102 CxlRejReason, 58 Text |
| `j` | BusinessMessageReject | 45 RefSeqNum, 372 RefMsgType R, 380 BusinessRejectReason R (`3` unsupported message type), 58 Text |

ExecType (150): `0` New, `4` Canceled, `5` Replaced, `8` Rejected, `F` Trade.
OrdStatus (39): `0` New, `1` Partially filled, `2` Filled, `4` Canceled,
`8` Rejected. (FIX 4.4 has no OrdStatus Replaced; a replaced order reports
`0` or `1`.)

OrdRejReason (103): `0` broker/exchange option, `1` unknown symbol,
`6` duplicate order (duplicate ClOrdID), `11` unsupported order
characteristic, `13` incorrect quantity, `99` other.

CxlRejReason (102): `0` too late to cancel, `1` unknown order, `3` already
pending cancel or replace, `6` duplicate ClOrdID, `99` other.

## How fixgw maps FIX onto OUCH

| FIX in | OUCH out | Notes |
| --- | --- | --- |
| D | Enter Order | UserRefNum assigned by fixgw; FIX ClOrdID kept in fixgw |
| F | Cancel Order (quantity 0) | unknown or finished OrigClOrdID: OrderCancelReject 102=`1` or `0`, no OUCH |
| G | Replace Order | the order's side and symbol cannot change: 58 explains, 102=`99` |

| OUCH in | FIX out |
| --- | --- |
| Order Accepted | 8 with 150=`0`, 39=`0` |
| Order Replaced | 8 with 150=`5`, 39=`0` or `1`, 11 = new ClOrdID, 41 = old |
| Order Executed | 8 with 150=`F`, 39=`1` or `2`, 32/31 |
| Order Canceled | 8 with 150=`4`, 39=`4` (11 = the cancel's ClOrdID when one is pending) |
| Rejected (new order) | 8 with 150=`8`, 39=`8`, 103 |
| Rejected (replacement) | 9 with 434=`2` |

A cancel that finds the order already filled gets OrderCancelReject 102=`0`
after the final fill's ExecutionReport. A replace to no more than the executed
shares cancels what is left (OUCH answers with Order Canceled), reported as
150=`4`.
