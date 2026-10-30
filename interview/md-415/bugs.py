# MD-415 bug bank: 60 defects planted one at a time on main.
# Each entry: id -> dict(cat, level, title, edits=[(file, old, new)], report, cause, fix, checks)
# checks: extra checks beyond build + ctest: "digest" (itch-gen/itch-dump 1M day),
# "firm" (scripts/e2e-firm.sh), "live" (scripts/e2e-live-feed.sh), "fix" (scripts/e2e-fix.sh),
# "asan" (core/book/... tests under ASan+UBSan), "bench_book", "bench_ring".

BUGS = {}


def bug(id, cat, level, title, edits, report, cause, fix, checks=()):
    BUGS[id] = dict(id=id, cat=cat, level=level, title=title, edits=edits, report=report,
                    cause=cause, fix=fix, checks=list(checks))


# ---------------------------------------------------------------- C++
bug("CPP01", "cpp", "easy", "Logger stop() never clears the running flag",
    [("libs/core/src/log.cpp",
      "  if (running_.exchange(false)) {\n    if (worker_.joinable()) worker_.join();",
      "  if (running_.load(std::memory_order_acquire)) {\n    if (worker_.joinable()) worker_.join();")],
    "Every process that starts the async logger hangs on exit; Ctrl-C twice to kill it.",
    "stop() reads the flag instead of exchanging it, so the worker's loop never sees false and join() waits forever.",
    "Restore running_.exchange(false): one atomic read-modify-write that both tests and clears the flag.")

bug("CPP02", "cpp", "easy", "Thread name copied without its NUL terminator",
    [("libs/core/src/affinity.cpp", "  name.copy(buf, sizeof buf - 1);", "  name.copy(buf, sizeof buf);")],
    "perf and top -H show some threads as the binary name instead of feed-rx or trader-md.",
    "Linux thread names are 15 characters plus NUL. Copying 16 leaves no terminator for long names, and pthread_setname_np rejects them with ERANGE, which the code ignores.",
    "Copy at most sizeof buf - 1 bytes, and check the return code in a test with a 20-character name.")

bug("CPP03", "cpp", "easy", "Huge-page free path disagrees with the allocate path at exactly 2 MB",
    [("libs/core/include/lab/core/huge_pages.hpp",
      "inline void huge_page_unmap(void* p, std::size_t bytes) noexcept {\n  if (bytes < kHugePageSize) {",
      "inline void huge_page_unmap(void* p, std::size_t bytes) noexcept {\n  if (bytes <= kHugePageSize) {")],
    "simex aborts at shutdown with 'free(): invalid pointer' on one config; other configs are fine.",
    "A table of exactly 2 MB is mmap'ed by huge_page_map (bytes >= 2 MB) but released with operator delete (bytes <= 2 MB): an mmap'ed pointer handed to the heap allocator.",
    "Use the same predicate in both functions (bytes < kHugePageSize), ideally one helper both call.",
    checks=["asan"])

bug("CPP04", "cpp", "medium", "TcpStream move-assignment leaves the source owning the fd",
    [("libs/net/src/tcp.cpp",
      "    out_ = std::move(o.out_);\n    out_pos_ = o.out_pos_;\n    o.fd_ = -1;\n  }\n  return *this;",
      "    out_ = std::move(o.out_);\n    out_pos_ = o.out_pos_;\n  }\n  return *this;")],
    "Every OUCH session drops straight after login; the trader logs 'exchange closed the connection during login'.",
    "Move-assignment copies fd_ but does not reset the source, so the moved-from temporary's destructor closes the socket the new owner is using (and later a reused fd number).",
    "Reset o.fd_ to -1 in move-assignment, as the move constructor already does; add a test that moves a connected stream and still sends.",
    checks=["firm"])

bug("CPP05", "cpp", "medium", "Top-of-book change test compares 24 of 32 bytes",
    [("firm/trader/main.cpp",
      "    if (std::memcmp(t.bid_px, last.bid_px, 32) == 0 && t.trading_state == last.trading_state)",
      "    if (std::memcmp(t.bid_px, last.bid_px, 24) == 0 && t.trading_state == last.trading_state)")],
    "The strategy keeps quoting against size that is no longer there on the offer; fills on the ask look stale.",
    "The magic 32 was edited to 24 ('three arrays'), so the comparison skips ask_qty. A change in offer size alone no longer sends a new top of book.",
    "Compare with sizeof over the four arrays (or a defaulted operator== on a struct of them), and static_assert the layout.",
    checks=["firm"])

bug("CPP06", "cpp", "medium", "trimmed() takes the session by value and returns a view into it",
    [("libs/mold/include/lab/mold/mold.hpp",
      "inline std::string_view trimmed(const Session& s) noexcept {",
      "inline std::string_view trimmed(Session s) noexcept {")],
    "Feed handler reports print a garbled session name ('SIMEX\\x7f\\x02...') in about one run in five.",
    "The string_view points into the by-value parameter, which dies when trimmed() returns: a dangling view whose bytes depend on what reuses the stack.",
    "Take the array by const reference again; build with -fsanitize=address and detect_stack_use_after_return=1 in CI.",
    checks=["asan", "live"])

bug("CPP07", "cpp", "medium", "FIX timer helper underflows when the clock steps back",
    [("libs/fix/include/lab/fix/session.hpp",
      "    return now > then ? now - then : 0;",
      "    return now - then;")],
    "FIX clients are logged out seconds after logon on one gateway host, never on the others.",
    "since() subtracts unsigned nanoseconds. When the time given to poll() is earlier than last_rx_ns_ (callers pass different clocks), the difference wraps to about 584 years, so the test-request timeout fires at once.",
    "Saturate at zero as before, and give every caller one monotonic clock.",
    checks=["fix"])

bug("CPP08", "cpp", "hard", "Huge-page mapping has no slack for alignment",
    [("libs/core/include/lab/core/huge_pages.hpp",
      "  const std::size_t span = size + kHugePageSize;",
      "  const std::size_t span = size;  // THP aligns large anonymous mappings already")],
    "simex segfaults at startup on the colo boxes (kernel 5.15) and runs fine on every laptop (6.x).",
    "Kernels from 6.7 return 2 MB aligned addresses for large anonymous mmaps, so the extra 2 MB looked unnecessary. Older kernels do not; the aligned pointer then runs past the end of the mapping, and the head was just unmapped.",
    "Map size + 2 MB and trim both ends, as before; add a test that forces a misaligned base with a placeholder mapping.",
    checks=["asan"])

bug("CPP09", "cpp", "hard", "Order map backward-shift ignores probe runs that wrap",
    [("libs/book/include/lab/book/order_map.hpp",
      "      const bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);",
      "      const bool stays = i < k && k <= j;")],
    "Big days only: a few hundred 'unknown order reference' errors appear late in the session, always on different symbols.",
    "Linear probing wraps from the last slot to the first. When a deletion's probe run crosses that boundary, the simplified test moves entries that should stay, so later lookups stop at a hole before reaching their key.",
    "Restore the cyclic interval test. The identity hash makes the live window sweep the whole table, so it crosses the wrap point once per capacity's worth of references.",
    checks=["digest"])

bug("CPP10", "cpp", "hard", "Replace inserts the new order before erasing through a stale pointer",
    [("libs/book/src/book_builder.cpp",
      "  orders_.erase_found(o);\n  if (LAB_UNLIKELY(!orders_.insert(m.new_order_reference_number,\n                                   Order{m.price, m.shares, original.locate, original.side}))) {\n    ++checks_.duplicate_order_reference;\n    return;\n  }",
      "  if (LAB_UNLIKELY(!orders_.insert(m.new_order_reference_number,\n                                   Order{m.price, m.shares, original.locate, original.side}))) {\n    ++checks_.duplicate_order_reference;\n    return;\n  }\n  orders_.erase_found(o);  // after the insert, so a failed insert keeps the original")],
    "Rare crash in itch-fh on the busiest days only, in OrderMap::erase_slot; never reproduces on the test day.",
    "insert() may rehash, which moves every slot. o still points into the old storage, so erase_found() computes a slot index from freed memory. With the map pre-sized, rehash only happens past 70% load.",
    "Erase first (as before), or re-find the original after the insert. Add a test with a tiny OrderMap that forces a rehash inside a replace.",
    checks=["asan", "digest"])

# ---------------------------------------------------------------- Market data
bug("MD01", "md", "easy", "LULD tier boundary: $3.00 gets the 5% band",
    [("libs/book/include/lab/book/luld.hpp", "  if (reference > 30'000)", "  if (reference >= 30'000)")],
    "Vendor LULD bands for stocks referenced at exactly $3.00 are 15 cents wide; the SIP shows 60 cents.",
    "The plan gives 5% only above $3.00. At exactly $3.00 the reference is in the 20% tier.",
    "Strictly greater than 30,000 in Price(4); add a table test at 2.99, 3.00 and 3.01.")

bug("MD02", "md", "easy", "Locked books counted as crossed",
    [("libs/book/include/lab/book/order_book.hpp",
      "    return b != nullptr && a != nullptr && b->price > a->price;",
      "    return b != nullptr && a != nullptr && b->price >= a->price;")],
    "Data quality report: crossed-book events up 40x overnight, locked count unchanged.",
    "crossed() now includes bid == ask, so every lock is also a cross.",
    "Crossed is strictly bid > ask; locked is equality.",
    checks=["digest"])

bug("MD03", "md", "easy", "Opening and closing cross prices swapped",
    [("libs/book/src/book_builder.cpp",
      "    if (m.cross_type == 'O') inst->stats.opening_cross_price = m.cross_price;\n    if (m.cross_type == 'C') inst->stats.closing_cross_price = m.cross_price;",
      "    if (m.cross_type == 'C') inst->stats.opening_cross_price = m.cross_price;\n    if (m.cross_type == 'O') inst->stats.closing_cross_price = m.cross_price;")],
    "Research: official open and close in the daily summary are each other's values.",
    "The cross type letters were swapped when the lines were reordered.",
    "O is opening and C is closing (ITCH 5.0 section 1.10); test both.")

bug("MD04", "md", "medium", "Attributed adds (F) are no longer handled",
    [("libs/book/include/lab/book/book_builder.hpp", "  void on(const itch::AddOrderMpid& m);\n", ""),
     ("libs/book/src/book_builder.cpp",
      "void BookBuilder::on(const itch::AddOrderMpid& m) {\n  add_order(m.header, m.order_reference_number, m.buy_sell_indicator, m.shares, m.stock, m.price);\n}\n\n",
      "")],
    "itch-dump on a real day: thousands of 'unknown order reference'; market-maker books are thin.",
    "With the F handler removed, dispatch falls back to HandlerBase's no-op, so attributed orders never reach the book and their later executions and cancels look unknown.",
    "Handle F exactly like A. Add a test that HandlerBase defaults are only used for types the builder does not need.",
    checks=["digest"])

bug("MD05", "md", "medium", "48-bit timestamp decoded with the wrong shift",
    [("libs/core/include/lab/core/endian.hpp",
      "  return (std::uint64_t{load_be16(b)} << 32) | load_be32(b + 2);",
      "  return (std::uint64_t{load_be16(b)} << 16) | load_be32(b + 2);")],
    "Timestamp regressions every few seconds all day; the latency report shows negative feed ages.",
    "The top 16 bits of the ITCH nanosecond timestamp must shift by 32. Shifted by 16 they overlap the low word, so time jumps back each time the low 32 bits wrap (every 4.3 s).",
    "Shift by 32, and test load_be48 against a known value above 2^32.",
    checks=["digest"])

bug("MD06", "md", "medium", "Halt reopening midpoint not rounded to the cent",
    [("libs/book/src/book_builder.cpp",
      "            static_cast<std::uint32_t>((std::uint64_t{b->price} + a->price) / 200 * 100);",
      "            static_cast<std::uint32_t>((std::uint64_t{b->price} + a->price) / 2);")],
    "After an empty halt cross, the vendors' LULD band differs from simex's by a fraction of a cent.",
    "The lab rule rounds the reference down to the cent before computing bands. Without it the mid can be sub-penny, and every band derived from it is too.",
    "Divide by 200 and multiply by 100 again; test a 10.005 midpoint.")

bug("MD07", "md", "medium", "Trader reads the tracking number as the stock locate",
    [("firm/trader/main.cpp",
      "    const std::uint16_t locate = lab::load_be16(msg.data() + 1);",
      "    const std::uint16_t locate = lab::load_be16(msg.data() + 3);")],
    "Trader starts, logs in, and never quotes: 'tops in 0'.",
    "Bytes 1-2 are Stock Locate and 3-4 are Tracking Number. The synthetic feed's tracking numbers are 0, so every message maps to locate 0, which nobody trades.",
    "Read the locate at offset 1 through the codec (decode the header) instead of a hand-written offset.",
    checks=["firm"])

bug("MD08", "md", "hard", "LULD reference follows every trade",
    [("libs/book/include/lab/book/book_builder.hpp",
      "    if (inst.luld_reference == 0) inst.luld_reference = price;",
      "    inst.luld_reference = price;  // latest print")],
    "A stock ran 30% in ten minutes and no vendor showed a limit state; the bands moved with it.",
    "The reference should be the opening or reopening cross (else the first trade). Updating it on every print makes the band chase the price, so it can never be reached.",
    "Set it only from a printing cross or the day's first trade, and test a steady climb past the band.",
    checks=["digest"])

bug("MD09", "md", "hard", "Repeated stock directory message wipes the book",
    [("libs/book/src/book_builder.cpp",
      "  Instrument& inst = instruments_[m.header.stock_locate];\n  inst.listed = true;",
      "  Instrument& inst = instruments_[m.header.stock_locate];\n  inst.book = OrderBook{};  // start clean\n  inst.listed = true;")],
    "One morning a few symbols showed empty books until the next add; the full-day replay finished with book inconsistencies.",
    "Nasdaq can resend R messages intraday. Clearing the book there leaves the order store holding live orders whose levels are gone, so later cancels and executions cannot find them.",
    "A directory message updates reference data only; never touch the book or the orders.",
    checks=["digest"])

bug("MD10", "md", "hard", "Order Replace shares and price offsets swapped in the field list",
    [("libs/itch/include/lab/itch/messages.hpp",
      "    f(27, m.shares);\n    f(31, m.price);",
      "    f(27, m.price);\n    f(31, m.shares);")],
    "Synthetic days are clean. On the real Nasdaq sample, replaced orders land at absurd prices and books cross.",
    "Encode and decode share the field list, so round-trip tests and the synthetic generator agree with each other. Only a spec-derived golden message shows the swap, and the layout check passes because both fields are 4 bytes.",
    "Restore the spec offsets (shares at 27, price at 31) and add golden-byte tests per message type from the spec PDF.",
    checks=["digest"])

# ---------------------------------------------------------------- Network
bug("NET01", "net", "easy", "Feed socket left at the kernel's default receive buffer",
    [("libs/net/src/udp.cpp",
      "  set_int(s.fd_, SOL_SOCKET, SO_RCVBUF, rcvbuf_bytes, \"SO_RCVBUF\");\n  // Binding the group",
      "  // Binding the group")],
    "Gap requests spike at the open and on every news burst; quiet periods are clean.",
    "Without SO_RCVBUF the socket has about 200 KB, a few milliseconds of the open at full rate. Bursts overflow it and the kernel drops datagrams (netstat -su: receive buffer errors).",
    "Set SO_RCVBUF again, and have lab-check report net.core.rmem_max, which caps it.",
    checks=["live"])

bug("NET02", "net", "easy", "Multicast loopback disabled on the publisher",
    [("libs/net/src/udp.cpp",
      "    set_int(s.fd_, IPPROTO_IP, IP_MULTICAST_LOOP, 1, \"IP_MULTICAST_LOOP\");",
      "    set_int(s.fd_, IPPROTO_IP, IP_MULTICAST_LOOP, 0, \"IP_MULTICAST_LOOP\");")],
    "On the lab box (one host, multicast group) itch-fh receives nothing. CI is green.",
    "With loopback off, a multicast sender's packets are not delivered to receivers on the same host. CI uses a unicast address, so it never notices.",
    "Turn loopback back on for the single-host lab, and add a multicast case to the e2e scripts.")

bug("NET03", "net", "easy", "Multicast test only recognises 224/8",
    [("libs/net/src/udp.cpp",
      "  return (ntohl(addr.sin_addr.s_addr) & 0xF0000000u) == 0xE0000000u;  // 224.0.0.0/4",
      "  return (ntohl(addr.sin_addr.s_addr) & 0xFF000000u) == 0xE0000000u;  // 224.0.0.0/8")],
    "Moving the feed to 239.1.1.1 (admin-scoped) gives silence; 224.0.1.1 works.",
    "Multicast is 224.0.0.0/4 (224 to 239). With a /8 mask, 239.x is treated as unicast, so the receiver never joins the group.",
    "Mask with 0xF0000000; test 224.0.0.1, 239.255.255.255 and 240.0.0.0.")

bug("NET04", "net", "medium", "Partial TCP send re-queues the whole buffer",
    [("libs/net/src/tcp.cpp",
      "  if (sent < data.size()) out_.insert(out_.end(), data.begin() + sent, data.end());",
      "  if (sent < data.size()) out_.insert(out_.end(), data.begin(), data.end());")],
    "Under heavy order flow simex occasionally logs 'malformed SoupBinTCP' from a client and drops it.",
    "When send() takes only part of a packet, the code queues the whole packet again instead of the unsent tail, so the bytes already sent go out twice and the framing breaks.",
    "Queue data[sent..]; test with a tiny SO_SNDBUF and a slow reader.",
    checks=["firm"])

bug("NET05", "net", "medium", "recv() returning 0 treated as no data",
    [("libs/net/src/tcp.cpp", "  if (n > 0) return n;\n  if (n < 0 && (errno", "  if (n >= 0) return n;\n  if (n < 0 && (errno")],
    "The trader crashed; its quotes stayed live on simex for 15 seconds until the idle timeout.",
    "recv() returns 0 when the peer closes. Treating 0 as 'nothing yet' hides the close, so cancel-on-disconnect only fires when the idle timer gives up on the session.",
    "0 means orderly close: mark the stream failed. Test that a closed peer is seen on the next recv.",
    checks=["firm"])

bug("NET06", "net", "medium", "Gap requests sent before the reorder delay",
    [("libs/mold/include/lab/mold/sequencer.hpp",
      "    const bool due = progress || (!requested_ && now - gap_opened_ >= cfg_.gap_delay) ||",
      "    const bool due = progress || !requested_ ||")],
    "The re-request server answers ten times more requests than last week, nearly all for packets that arrived anyway.",
    "Mild reordering on the new switch opens a gap for a few microseconds. Without the gap_delay wait, every reorder triggers a retransmission request.",
    "Wait gap_delay before the first request, as before.",
    checks=["live"])

bug("NET07", "net", "medium", "MoldUDP64 payload raised to the MTU",
    [("libs/mold/include/lab/mold/mold.hpp",
      "inline constexpr std::size_t kDefaultMaxPayload = 1400;",
      "inline constexpr std::size_t kDefaultMaxPayload = 1500;  // full Ethernet MTU")],
    "After the 'fewer packets' change, itch-fh reports thousands of oversize packets and the book digest no longer matches.",
    "1500 is the IP MTU, not the UDP payload; IPv4 and UDP headers take 28 bytes. Full packets are fragmented on the wire, and the receiver's 1472-byte slots drop them as oversize, so gaps open on every busy packet.",
    "Keep the payload at 1400, or derive it from the MTU minus 28 and size the receive slots from the same constant.",
    checks=["live"])

bug("NET08", "net", "hard", "Sequencer follows any session name",
    [("libs/mold/include/lab/mold/sequencer.hpp",
      "    if (LAB_UNLIKELY(!session_known_)) {\n      session_ = p.header.session;\n      session_known_ = true;\n    } else if (LAB_UNLIKELY(p.header.session != session_)) {\n      ++stats_.wrong_session;\n      return PacketResult::kWrongSession;\n    }",
      "    if (LAB_UNLIKELY(p.header.session != session_)) {\n      session_ = p.header.session;  // follow the publisher across restarts\n      session_known_ = true;\n    }")],
    "After a simex restart mid-day, itch-fh kept running with no errors and a frozen book.",
    "The new session restarts at sequence 1. Following it silently keeps the old expected sequence, so every new message is older than expected and dropped as a duplicate.",
    "Latch the first session and count packets from any other; a new session needs an explicit restart.")

bug("NET09", "net", "hard", "Retransmit store loses its padding when it wraps",
    [("libs/mold/include/lab/mold/retransmit_store.hpp",
      "    if (at + msg.size() > size) {  // pad to the start rather than split\n      written_ += size - at;\n      at = 0;\n    }",
      "    if (at + msg.size() > size) {  // pad to the start rather than split\n      at = 0;\n    }")],
    "Hours into a lossy session, the book digest diverges after a retransmission; the resent messages have the right sequence numbers and the wrong contents.",
    "After the first wrap the stored start positions no longer match where the bytes were written, so get() returns bytes from another message. It only shows once the store has wrapped and a gap is filled from it.",
    "Count the padding in written_ again; test a wrap followed by get() of every stored message.",
    checks=["live"])

bug("NET10", "net", "hard", "Heartbeat advertises messages that are still being batched",
    [("libs/feed/src/publisher.cpp",
      "    hb.heartbeat(session_, sent_next_);",
      "    hb.heartbeat(session_, store_.next_sequence());")],
    "On quiet afternoons itch-fh occasionally gives a gap up and loses a few messages, then reports duplicates.",
    "The store already holds messages added to the open packet. A heartbeat sent before that packet is flushed announces them, receivers open a gap and ask for messages the publisher cannot serve yet, give up, then discard the real packet as duplicates.",
    "Heartbeats carry the next sequence actually sent (sent_next_).",
    checks=["live"])

# ---------------------------------------------------------------- Latency
bug("LAT01", "lat", "easy", "TCP_NODELAY no longer set",
    [("libs/net/src/tcp.cpp", "  out_.reserve(1 << 16);\n  set_nodelay(fd_);", "  out_.reserve(1 << 16);")],
    "Order round trip went from 20 microseconds to 40 milliseconds at p99 after the networking refactor.",
    "Without TCP_NODELAY, Nagle holds small OUCH packets until the previous one is acknowledged, and delayed ACK on the other side adds up to 40 ms.",
    "Set TCP_NODELAY on every stream, and check it in lab-check.",
    checks=["firm"])

bug("LAT02", "lat", "easy", "Receive loop sleeps even when busy",
    [("libs/feed/src/receiver.cpp", "    if (idle && cfg_.idle_sleep_us != 0) {", "    if (cfg_.idle_sleep_us != 0) {")],
    "itch-fh 'ring full' and gap counts climb at the open; CPU use of feed-rx dropped to 20%.",
    "The receive thread now sleeps after every batch, not only after an empty one, so it stops draining the socket for idle_sleep_us at a time.",
    "Sleep only when the batch was empty.",
    checks=["live"])

bug("LAT03", "lat", "easy", "Debug print left on the trading thread",
    [("firm/trader/trading.cpp",
      "  const auto applied = oms_.apply(r);\n  if (applied.order == nullptr) return;",
      "  const auto applied = oms_.apply(r);\n  if (applied.fill) std::fprintf(stderr, \"fill %u @ %u\\n\", r.qty, r.price);\n  if (applied.order == nullptr) return;")],
    "Decide-to-wire p99.9 jumps from microseconds to milliseconds whenever we get filled.",
    "fprintf to stderr is a write() syscall, plus formatting and a lock, on the hot thread. When the terminal or pipe is slow it blocks.",
    "Log fills through lab::LogChannel (or the fill log that already exists).",
    checks=["firm"])

bug("LAT04", "lat", "medium", "Fill log grows on the hot thread",
    [("firm/trader/trading.cpp",
      "  if (applied.fill && fills_.size() < fills_.capacity()) {",
      "  if (applied.fill) {")],
    "With --fills-csv on long runs, every few minutes the trader freezes for tens of milliseconds.",
    "The capacity check kept push_back from reallocating. Without it the vector doubles past its reservation, copying the whole log and faulting new pages on the trading thread.",
    "Keep the bound (drop and count past capacity), and size the reservation for the session.",
    checks=["firm"])

bug("LAT05", "lat", "medium", "Shared memory pages no longer pre-faulted",
    [("libs/core/src/shm.cpp", "  if (opts.populate) flags |= MAP_POPULATE;\n", "")],
    "The first 30 seconds after start have p99.9 ten times worse than the rest of the day.",
    "Without MAP_POPULATE each 4 KB page of a ring faults on first touch, on the hot path. Once every page has been touched the spikes stop.",
    "Populate (or touch) every page at startup; measure the first second separately in the latency report.")

bug("LAT06", "lat", "medium", "Publisher batching delay in milliseconds, not microseconds",
    [("libs/feed/src/publisher.cpp",
      "  if (!builder_.empty() && now - opened_ns_ > cfg_.max_delay_us * 1000) flush(now);",
      "  if (!builder_.empty() && now - opened_ns_ > cfg_.max_delay_us * 1000000) flush(now);"),
     ("libs/feed/src/publisher.cpp",
      "  if (now - opened_ns_ > cfg_.max_delay_us * 1000) flush(now);",
      "  if (now - opened_ns_ > cfg_.max_delay_us * 1000000) flush(now);")],
    "Feed age is fine at the open and terrible in the lunch lull: messages arrive in bursts tens of milliseconds late.",
    "A half-full packet waits max_delay_us before it is sent. Multiplying by a million turns microseconds into milliseconds, which only matters when traffic is too light to fill a packet.",
    "Use std::chrono durations for the config so the unit is in the type.",
    checks=["live"])

bug("LAT07", "lat", "medium", "Price level lookup scans from the worst level",
    [("libs/book/include/lab/book/order_book.hpp",
      "    for (std::size_t i = levels_.size(); i-- > 0;) {\n      if (levels_[i].price == price) return i;\n      if (better(price, levels_[i].price)) return kNone;  // walked past where it would be\n    }",
      "    for (std::size_t i = 0; i < levels_.size(); ++i) {\n      if (levels_[i].price == price) return i;\n    }")],
    "The book rebuild benchmark is down sharply and itch-fh falls behind at the open on deep symbols.",
    "Levels are stored worst to best and nearly all activity is near the touch. Scanning from the front walks the whole deep side for every reduce.",
    "Scan from the back with the early exit, and keep the before/after numbers from itch_bench in the PR.",
    checks=["bench_book"])

bug("LAT08", "lat", "hard", "Ring consumer reloads the producer index every call",
    [("libs/core/include/lab/core/spsc_ring.hpp",
      "    if (LAB_UNLIKELY(tail == cached_head_)) {\n      cached_head_ = head_.load(std::memory_order_acquire);\n      if (tail == cached_head_) return nullptr;\n    }",
      "    cached_head_ = head_.load(std::memory_order_acquire);\n    if (tail == cached_head_) return nullptr;")],
    "On the colo box, cross-core ring throughput fell by a third; nothing changes on a laptop with unpinned threads.",
    "The cached copy of head_ means the consumer reads the producer's cache line only when it runs out. Reading it every call makes the line bounce between cores on every message.",
    "Restore the cached read, and benchmark with both threads pinned to different physical cores.",
    checks=["bench_ring"])

bug("LAT09", "lat", "hard", "Tick timestamp taken after the book update",
    [("firm/trader/main.cpp", "    t.rx_tsc = rx_tsc;", "    t.rx_tsc = lab::rdtsc();  // when the top changed")],
    "Last week's tick-to-trade report looked better than ever, but fill rates fell and colo p99 rose.",
    "rx_tsc should be when the packet arrived. Stamping it after the book update leaves the ring queueing and the book rebuild out of tick-to-wire, so the number improves while the real latency grows.",
    "Carry the packet's arrival tick through, and report queueing as its own histogram.",
    checks=["firm"])

bug("LAT10", "lat", "hard", "Matching engine key index moved off huge pages",
    [("libs/match/include/lab/match/key_index.hpp",
      "  std::vector<Slot, HugePageAllocator<Slot>> slots_;  // random access over up to 128 MB",
      "  std::vector<Slot> slots_;")],
    "simex matcher p99 is up 30% with no code change in the engine itself.",
    "The key index is random-access over up to 128 MB. With 4 KB pages most lookups miss the TLB; on 2 MB pages they rarely do. perf stat shows dTLB-load-misses up by an order of magnitude.",
    "Keep the huge page allocator, and add dTLB misses to the benchmark output.",
    checks=["firm"])

# ---------------------------------------------------------------- Trading core
bug("TRD01", "trd", "easy", "Maker improves the spread one tick later than configured",
    [("firm/strategy/include/lab/strategy/market_maker.hpp",
      "    if (static_cast<std::uint32_t>((m.ask - m.bid) / p_.tick) >= p_.improve_spread_ticks) {",
      "    if (static_cast<std::uint32_t>((m.ask - m.bid) / p_.tick) > p_.improve_spread_ticks) {")],
    "We stopped stepping inside 3-cent spreads; fill share in wide names is down.",
    "The threshold is 'at least' improve_spread_ticks. Strictly greater skips the 3-tick case.",
    "Use >=, as the parameter's comment says; test a spread of exactly 3 ticks.")

bug("TRD02", "trd", "easy", "Inventory skew leans the wrong way",
    [("firm/strategy/include/lab/strategy/market_maker.hpp",
      "    bid -= lean;\n    ask -= lean;",
      "    bid += lean;\n    ask += lean;")],
    "Positions run to the cap and stay there; the maker buys more when it is already long.",
    "A long position should lower both quotes so we sell more and buy less. Adding the lean raises them, so fills push the position further out.",
    "Subtract the lean; test that a long position lowers both prices.",
    checks=["firm"])

bug("TRD03", "trd", "easy", "Position cap ignores the size of the new quote",
    [("firm/strategy/include/lab/strategy/market_maker.hpp",
      "    if (position + size <= p_.max_position) {",
      "    if (position <= p_.max_position) {")],
    "The maker reaches 1,100 shares long with a 1,000-share cap.",
    "The check must include the shares the quote could add. Comparing the current position alone lets one more full quote through at the cap.",
    "Check position + size, as the ask side already does.")

bug("TRD04", "trd", "medium", "Cash booked at our limit price, not the fill price",
    [("firm/oms/include/lab/oms/oms.hpp",
      "    const auto notional = q * static_cast<std::int64_t>(r.price);",
      "    const auto notional = q * static_cast<std::int64_t>(o->price);")],
    "Firm PnL differs from the drop copy every day, by more on auction days.",
    "Fills can print at a better price than our limit (price improvement, crosses). Using the order's own price books the wrong cash, so PnL is off by the improvement.",
    "Book cash at the execution price; reconcile against the drop copy in the test.",
    checks=["firm"])

bug("TRD05", "trd", "medium", "Price collar computed in tenths of a percent",
    [("firm/risk/include/lab/risk/risk.hpp",
      "    const std::uint64_t band = static_cast<std::uint64_t>(reference) * limits_.collar_bps / 10'000;",
      "    const std::uint64_t band = static_cast<std::uint64_t>(reference) * limits_.collar_bps / 1'000;")],
    "A fat-finger order 30% through the market passed pre-trade risk.",
    "Basis points are per 10,000. Dividing by 1,000 makes a 500 bps (5%) collar into 50%.",
    "Divide by 10,000; test 4.9% passes and 5.1% fails.")

bug("TRD06", "trd", "medium", "Replace confirmation updates only the bid's reference",
    [("firm/trader/trading.cpp", "    if (in.ask_ref == r.orig_ref) in.ask_ref = r.user_ref;\n", "")],
    "At shutdown: 'orders still working after stop', and offers we no longer track keep trading.",
    "After a replace the live order has the new UserRefNum. The ask side keeps the old one, which is done, so the next reconcile enters a fresh offer while the replaced one stays on the book.",
    "Update whichever side the replaced order belongs to; test a replace of each side.",
    checks=["firm"])

bug("TRD07", "trd", "medium", "New orders join the front of their price level",
    [("libs/match/include/lab/match/engine.hpp",
      "    x.prev = lv.tail;\n    x.next = kNil;\n    if (lv.tail != kNil)\n      nodes_[lv.tail].next = n;\n    else\n      lv.head = n;\n    lv.tail = n;",
      "    x.prev = kNil;\n    x.next = lv.head;\n    if (lv.head != kNil)\n      nodes_[lv.head].prev = n;\n    else\n      lv.tail = n;\n    lv.head = n;")],
    "Simulation: our queue position never matters; the newest order at a price always trades first.",
    "link() now pushes at the head of the queue, so simex runs price-LIFO instead of price-time priority.",
    "Append at the tail; test that two orders at one price fill in arrival order.")

bug("TRD08", "trd", "hard", "Replace for no more than the executed shares leaves a ghost order",
    [("firm/oms/include/lab/oms/oms.hpp",
      "    if (r.flag == 'U' && o->replacement != 0 && !o->cancel_pending) abandon_replacement(*o);\n",
      "")],
    "Once or twice a day an instrument stops quoting with 'position limit' while flat, and stop reports one order still working.",
    "When a replace asks for no more than what has already executed, the exchange cancels the original instead. The pending replacement is never answered, so it stays working with its exposure counted.",
    "Abandon the replacement on that user cancel; test a replace down to the executed quantity.",
    checks=["firm"])

bug("TRD09", "trd", "hard", "Auction price ignores the reference on ties",
    [("libs/match/include/lab/match/engine.hpp",
      "            (imb == best_imb && (dist < best_dist || (dist == best_dist && p < out.price)))));",
      "            (imb == best_imb && p < out.price)));")],
    "simex opening prints sit at the bottom of the indicative range; Nasdaq's are near the previous close.",
    "When several prices pair the same shares with the same imbalance, the rule picks the one nearest the reference. Dropping the distance test always takes the lowest.",
    "Restore the reference tie-break; test a flat range of equal candidates.",
    checks=["firm"])

bug("TRD10", "trd", "hard", "Self-match guard uses the less aggressive price of a replace in flight",
    [("firm/trader/trading.cpp",
      "    return side == 'B' ? std::max(px, n->price) : std::min(px, n->price);",
      "    return side == 'B' ? std::min(px, n->price) : std::max(px, n->price);")],
    "Compliance: a few self-trades a week, always within a millisecond of a replace.",
    "While a replace is in flight the order may trade at either price, so the guard must use the more aggressive one. Taking the less aggressive one lets the other side cross into the new price.",
    "Use the more aggressive price for each side; test with a bid replace in flight.",
    checks=["firm"])

# ---------------------------------------------------------------- Order gateway
bug("GW01", "gw", "easy", "Login no longer checks the password",
    [("exchange/simex/gateway.cpp",
      "      if (same_ignoring_case(req->user, a.cfg.user) &&\n          same_ignoring_case(req->password, a.cfg.password)) {",
      "      if (same_ignoring_case(req->user, a.cfg.user)) {")],
    "Security review: a session logged in as FIRM02 with a mistyped password.",
    "The password comparison was dropped from the login match, so any password is accepted for a known user.",
    "Match user and password; test a wrong password gets Login Rejected 'A'.")

bug("GW02", "gw", "easy", "Sell short exempt rejected as an invalid side",
    [("exchange/simex/gateway.cpp", "        case ouch::side::kSellShortExempt:\n", "")],
    "Every short-exempt order from the desk is rejected with reason 0x0009.",
    "OUCH side E (sell short exempt) fell out of the switch and hits the default, which rejects it as an invalid side.",
    "Map E to a sell like S and T; test every side code.")

bug("GW03", "gw", "easy", "Drop copy passes short-sale side codes through",
    [("exchange/simex/gateway.cpp",
      "  r.side = oi.side == ouch::side::kBuy ? 'B' : 'S';",
      "  r.side = oi.side;")],
    "Reconciliation shows short sales missing from the drop copy; the totals by side do not add up.",
    "The drop copy format has B or S only. Passing T and E through puts short sales in neither bucket.",
    "Map every sell code to S at the boundary.",
    checks=["firm"])

bug("GW04", "gw", "medium", "Duplicate UserRefNum accepted",
    [("exchange/simex/gateway.cpp", "      if (ref <= a.last_ref) {", "      if (ref < a.last_ref) {")],
    "After a client resent its last order on reconnect, simex held two live orders with the same UserRefNum.",
    "OUCH requires strictly increasing UserRefNum and says a repeat is ignored. With < the most recent reference can be entered twice.",
    "Reject any ref that is not greater than the last one; test a resend of the last message.")

bug("GW05", "gw", "medium", "Reconnect with sequence 0 replays the last message",
    [("exchange/simex/gateway.cpp",
      "      req->next_sequence == 0 ? count + 1 : std::min<std::uint64_t>(req->next_sequence, count + 1);",
      "      req->next_sequence == 0 ? count : std::min<std::uint64_t>(req->next_sequence, count + 1);")],
    "After a reconnect the trader booked one fill twice; the exchange shows a single execution.",
    "SoupBinTCP sequence 0 means 'only new messages'. Starting at count resends the last message, which after a fill is that execution.",
    "Start at count + 1 for 0; test a reconnect after a fill.")

bug("GW06", "gw", "medium", "Client heartbeat interval longer than the server's idle timeout",
    [("firm/trader/order_entry.cpp",
      "    if (!stats_.session_ended && lab::mono_ns() - last_tx_ns_ > 1'000'000'000) {",
      "    if (!stats_.session_ended && lab::mono_ns() - last_tx_ns_ > 20'000'000'000) {")],
    "During the lunch lull the trader's session is dropped and every quote is cancelled; busy days are fine.",
    "SoupBinTCP clients must send a heartbeat after a second of silence. Every 20 s is longer than simex's 15 s idle timeout, so a quiet trader looks dead.",
    "Heartbeat after one second idle, and derive both timeouts from one config.")

bug("GW07", "gw", "medium", "FIX checksum skips BeginString and BodyLength",
    [("libs/fix/include/lab/fix/codec.hpp",
      "    for (std::size_t i = start; i < pos_; ++i) sum += static_cast<unsigned char>(buf_[i]);",
      "    for (std::size_t i = kHeadRoom; i < pos_; ++i) sum += static_cast<unsigned char>(buf_[i]);")],
    "No FIX client can log on; their logs show every message from us dropped as garbled.",
    "CheckSum (10) covers every byte from 8= up to the delimiter before 10=. Starting at the body leaves out the 8= and 9= fields, so the sum is wrong on every message.",
    "Sum from the start of 8=; test against a message from the FIX spec's example.",
    checks=["fix"])

bug("GW08", "gw", "hard", "Gap fill points one sequence number short",
    [("libs/fix/include/lab/fix/session.hpp",
      "      w.add_uint(tag::kNewSeqNo, upto);",
      "      w.add_uint(tag::kNewSeqNo, upto - 1);")],
    "After any FIX reconnect, the session loops on ResendRequests until someone resets sequence numbers.",
    "NewSeqNo is the next sequence number after the gap. One short, the counterparty expects a number it never receives and asks again, forever.",
    "NewSeqNo = first number after the filled range; test a resend that mixes gap fills and stored messages.",
    checks=["fix"])

bug("GW09", "gw", "hard", "Possible duplicates delivered to the application",
    [("libs/fix/include/lab/fix/session.hpp",
      "        ++stats_.duplicates_ignored;\n        return;",
      "        ++stats_.duplicates_ignored;\n        if (state_ == State::kActive) h.on_app(m);  // let the app dedup by ExecID\n        return;")],
    "A client booked two fills for one execution after a resend; positions disagree with the exchange.",
    "A PossDupFlag message below the expected sequence was already processed. Handing it to the application relies on every handler deduplicating, and the order handler does not.",
    "Drop it at the session layer; test that a resent execution report reaches the app once.",
    checks=["fix"])

bug("GW10", "gw", "hard", "Partial cancel forgets the order's details",
    [("exchange/simex/gateway.cpp",
      "        n = ouch::encode(m, buf.data());\n        done = e.remaining == 0;\n        break;\n      }\n      case SessionEvent::kExecuted: {",
      "        n = ouch::encode(m, buf.data());\n        done = true;\n        break;\n      }\n      case SessionEvent::kExecuted: {")],
    "Replaces are rejected with 0x0015 on orders that were partly cancelled, and their later fills have a blank ClOrdID.",
    "A cancel that reduces an order still leaves it live. Treating every cancel as final erases its ClOrdID, side and capacity, so a later replace cannot find the original.",
    "Forget an order only when nothing remains; test a reduce followed by a replace.",
    checks=["firm"])
