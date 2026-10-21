# MoldUDP64 packet layouts

Source: [Nasdaq MoldUDP64 Protocol Specification V1.00](https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/moldudp64.pdf)
(revision history from 11/2/04 to 8/2/24, the last entry a formatting update).
`libs/mold/include/lab/mold/mold.hpp` implements exactly these tables.

All numeric fields are big-endian. Message Data is opaque to MoldUDP64; for
the lab it is one ITCH 5.0 message.

## Downstream packet (exchange to firm, multicast)

Header, 20 bytes:

| Offset | Length | Field | Notes |
| --- | --- | --- | --- |
| 0 | 10 | Session | Alphanumeric session name |
| 10 | 8 | Sequence Number | Sequence number of the first message in the packet |
| 18 | 2 | Message Count | Number of message blocks that follow |

Then `Message Count` message blocks, each:

| Offset | Length | Field | Notes |
| --- | --- | --- | --- |
| 0 | 2 | Message Length | Length of Message Data, not counting these 2 bytes |
| 2 | Message Length | Message Data | May be zero length |

Rules:

- Only the first message's sequence number is carried; the rest of the packet
  is numbered consecutively from it.
- Message Count 0 is a heartbeat. It is sent about once a second when idle and
  carries the next expected sequence number, so a receiver can spot loss in a
  quiet period.
- Message Count 0xFFFF is End of Session. It replaces heartbeats for a limited
  time, also carries the next expected sequence number, and re-requests for
  the session are still served while it is being sent.

## Request packet (firm to re-request server, unicast)

20 bytes:

| Offset | Length | Field | Notes |
| --- | --- | --- | --- |
| 0 | 10 | Session | Session being requested |
| 10 | 8 | Sequence Number | First sequence number wanted |
| 18 | 2 | Requested Message Count | Number of messages wanted |

The server answers with an ordinary downstream packet, unicast to the
requester. If the messages asked for do not fit in one UDP packet, it sends
only the messages that fit, and the receiver must ask again for the rest.

## Lab conventions (not in the spec)

- Sequence numbers start at 1 in each session.
- Payload limit 1400 bytes by default, so header plus blocks fit a 1500 byte
  MTU with IP and UDP headers.
- The re-request server keeps a bounded window of recent messages; older
  requests get no answer and the receiver eventually gives up on them.
