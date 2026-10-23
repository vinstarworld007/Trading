# SoupBinTCP packet layouts

Source: [Nasdaq SoupBinTCP specification](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/soupbintcp.pdf),
version 3.00 (revision 3.0, 02/22/2017), the current version Nasdaq publishes.
`libs/soup/include/lab/soup/soup.hpp` implements these tables.

Every packet starts with a 3-byte header:

| Offset | Length | Field | Notes |
| --- | --- | --- | --- |
| 0 | 2 | Packet Length | Big-endian; bytes after this field, including the type byte |
| 2 | 1 | Packet Type | ASCII character |

| Type | Packet | Direction | Payload after the header |
| --- | --- | --- | --- |
| `+` | Debug | either | free text; ignored |
| `L` | Login Request | client to server | Username 3:6, Password 9:10, Requested Session 19:10, Requested Sequence Number 29:20 |
| `A` | Login Accepted | server to client | Session 3:10, Sequence Number 13:20 |
| `J` | Login Rejected | server to client | Reject Reason Code 3:1 (`A` not authorized, `S` session not available) |
| `S` | Sequenced Data | server to client | one higher-level message |
| `H` | Server Heartbeat | server to client | none |
| `Z` | End of Session | server to client | none |
| `U` | Unsequenced Data | client to server | one higher-level message |
| `R` | Client Heartbeat | client to server | none |
| `O` | Logout Request | client to server | none |

Field rules:

- Alphanumeric fields (username, password, session) are left-justified and
  padded with spaces. A blank Requested Session means the current session.
- Sequence numbers are ASCII digits, right-justified and left-padded with
  spaces. Requested Sequence Number is the next message the client wants;
  `0` means start from the most recently generated message.
- Sequence numbers are never sent with Sequenced Data: both sides count, from
  1 at the start of each session.
- Login Accepted is always the first non-debug packet after a good login;
  Login Rejected is the only one after a bad one, and the server then closes
  the socket.
- Heartbeats: each side sends one after 1 second without sending anything.
  The server drops a client silent for about 15 seconds, and a connection
  that sends no Login Request within about 30 seconds.
- End of Session means the session is over and cannot be rejoined.
