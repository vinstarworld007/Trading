# Reuters-style vendor: OMM JSON over WebSocket

`rt-server` speaks the public Refinitiv (LSEG) WebSocket API: OMM messages
encoded as JSON (`tr_json2`) on a WebSocket. Unlike the ICE and Bloomberg
look-alikes this protocol is public, so the lab follows it rather than
inventing one. Implemented in `libs/vendor/include/lab/vendor/{ws,json,omm}.hpp`
and `libs/vendor/src/omm_session.cpp`.

## Sources

- RFC 6455 (WebSocket) and RFC 3174 (SHA-1) for the handshake and framing.
- LSEG Real-Time SDK (open source, `github.com/Refinitiv/Real-Time-SDK`):
  `Cpp-C/Eta/Impl/Converter/jsonSimpleDefs.h` for JSON attribute names and
  values; `rsslQos.h`, `rsslState.h`, `rsslRDM.h` for QoS, state and domain
  values.
- `RDMFieldDictionary` and `enumtype.def` shipped with the SDK for field
  names, field ids and enumerated values.
- The public examples at `github.com/Refinitiv/websocket-api` for the
  request and response shapes (login, item request, batch, view, ping).

## Connection

`GET /WebSocket` with `Sec-WebSocket-Protocol: tr_json2`. The server answers
`101 Switching Protocols` with `Sec-WebSocket-Accept` = base64(SHA-1(key +
`258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)). Client frames are masked; server
frames are not. Each text frame holds a JSON array of messages (a single
object is accepted from clients). WebSocket ping frames are answered with
pong; at the end of the day the server sends a close frame with status 1000.

## Messages

| Client sends | Server answers |
| --- | --- |
| `{"ID":1,"Domain":"Login","Key":{"Name":"user","Elements":{...}}}` | Login Refresh, `State` Open/Ok, `Elements` `MaxMsgSize` 61440, `PingTimeout` 30 |
| `{"ID":2,"Key":{"Name":"ACME.O"}}` | Refresh with every field, then Updates |
| `{"ID":2,"Key":{"Name":["A.O","B.O"]}}` (batch) | one stream per name, IDs 3, 4, ...; Status Closed/Ok on 2 |
| `"View":["BID","ASK"]` | only those fields |
| `"Streaming":false` | one Refresh, `State.Stream` NonStreaming |
| `{"ID":2,"Type":"Close"}` | the stream stops |
| `{"Type":"Ping"}` / `{"Type":"Pong"}` | Pong / nothing; the server pings after 20 s idle |

Domain defaults to MarketPrice; any other non-Login domain gets Status
Closed/Suspect `UnsupportedDomain`. An item request before login gets
`NotEntitled`. An unknown RIC gets Status Closed/Suspect, `Code` `NotFound`.
When the venue's day ends every open stream gets Status Closed/Suspect.

RICs are the symbol plus `.O` (Nasdaq): `ACME.O`. The service name is
`LAB_RT` (`--service`).

Refresh: `ID`, `Type`, `Key{Service,Name}`, `State{Stream,Data,Text}`,
`Qos{Timeliness:"Realtime",Rate:"TickByTick"}`, `SeqNumber`, `Fields`.
Update: `ID`, `Type`, `UpdateType` (`Quote`, `Trade`, or `Unspecified` for a
status change), `SeqNumber`, `Fields` (only those that changed).

## Fields

| Name | FID | Type | From |
| --- | --- | --- | --- |
| DSPLY_NAME | 3 | ASCII | symbol |
| TRDPRC_1 | 6 | Real | last trade price |
| HIGH_1 | 12 | Real | day high |
| LOW_1 | 13 | Real | day low |
| OPEN_PRC | 19 | Real | opening cross, else first trade |
| BID | 22 | Real | best bid |
| ASK | 25 | Real | best offer |
| BIDSIZE | 30 | Real (integer) | shares at the best bid |
| ASKSIZE | 31 | Real (integer) | shares at the best offer |
| ACVOL_1 | 32 | Real (integer) | matched volume |
| NUM_MOVES | 77 | Int | trades |
| TRDVOL_1 | 178 | Real (integer) | last trade size |
| TRD_STATUS | 6614 | Enum | 1 Normal, 2 Halted, 3 Suspended (paused) |
| QUOTIM_NS | 14265 | Time | ITCH time of the last quote change |
| SALTIM_NS | 14266 | Time | ITCH time of the last trade |

Reals are exact decimals written as JSON numbers (`101.25`), never through
binary floating point; times are `"HH:MM:SS.nnnnnnnnn"`; a field with no
value is `null`.
