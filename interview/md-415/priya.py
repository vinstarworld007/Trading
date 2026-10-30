# Priya's solve for each MD-415 bug: (minute, kind, text). The first "run" step
# (what the existing checks say) and the last "verify" step are filled in from
# results.json by build.py, so they show what really happened.
# kinds: read, run, look, think, cause, edit, test, verify, note
# test: the regression test she adds (name, where).

P = {}


def p(id, minutes, test, steps):
    P[id] = dict(minutes=minutes, test=test, steps=steps)


# ---------------------------------------------------------------- C++
p("CPP01", 9, ("AsyncLoggerTest.StopReturnsWhileWorkerRuns", "tests/core/test_log.cpp"), [
    (1, "run", "gdb -p $(pidof itch-fh) -batch -ex 'thread apply all bt'"),
    (3, "look", "Main thread in std::thread::join from AsyncLogger::stop(); lab-logger thread still in its drain loop."),
    (5, "look", "git log -p libs/core/src/log.cpp: exchange(false) became load()."),
    (6, "cause", "load() never clears running_, so the worker never leaves while(running_)."),
    (7, "edit", "Back to running_.exchange(false): test and clear in one atomic step."),
])

p("CPP02", 7, ("AffinityTest.LongThreadNameIsTruncatedNotDropped", "tests/core/test_affinity.cpp"), [
    (1, "run", "ps -L -o tid,comm -p $(pidof trader)"),
    (2, "look", "Short names are there; 'vendor-client-rx' (16 chars) shows the binary name."),
    (4, "look", "pthread_setname_np returns ERANGE (34) when called by hand with that name."),
    (5, "cause", "name.copy(buf, sizeof buf) fills all 16 bytes, so there is no NUL: the kernel limit is 15 plus the terminator."),
    (6, "edit", "Copy sizeof buf - 1 bytes; assert the return code in a debug build."),
])

p("CPP03", 12, ("HugePagesTest.ExactlyTwoMegabytesRoundTrips", "tests/core/test_huge_pages.cpp"), [
    (1, "run", "ASAN_OPTIONS=abort_on_error=1 simex --config colo.cfg"),
    (3, "look", "ASan: attempting free on address which was not malloc()-ed, from huge_page_unmap via ~vector."),
    (5, "look", "The pool for this config is 16,384 x 128 bytes: exactly 2 MB."),
    (7, "cause", "Allocate says '< 2 MB is heap', free says '<= 2 MB is heap'. At exactly 2 MB an mmap'ed block goes to operator delete."),
    (9, "edit", "One predicate, uses_mmap(bytes), called by both functions."),
])

p("CPP04", 18, ("TcpTest.MoveAssignedStreamKeepsItsSocket", "tests/net/test_tcp.cpp"), [
    (2, "run", "strace -f -e trace=connect,close,sendto -p $(pidof trader)"),
    (5, "look", "connect(7) succeeds, then close(7) a few microseconds later, before the login is sent."),
    (8, "look", "The close comes from ~TcpStream of the temporary in sock_ = TcpStream::connect(to)."),
    (11, "cause", "Move-assignment copies fd_ but leaves it in the source too, so the temporary's destructor closes our socket."),
    (14, "edit", "Set o.fd_ = -1 in operator=, matching the move constructor."),
    (16, "note", "Asks for a check-list item: every owning type gets a move test."),
])

p("CPP05", 25, ("TraderMarketData.AskSizeChangeSendsTop", "tests/firm/test_market_data.cpp"), [
    (2, "look", "Compares trader's market CSV with itch-dump's book: ask_qty lags by many updates; bids are fine."),
    (8, "think", "Only a size-only change on the offer goes missing, so the change filter is suspect."),
    (12, "look", "memcmp(t.bid_px, last.bid_px, 24): four uint32[2] arrays are 32 bytes; 24 stops before ask_qty."),
    (16, "cause", "A hand-written size skipped the last array. Price and bid size changes still pass, so it looked fine."),
    (20, "edit", "Compare a struct of the four arrays with operator==; static_assert the TopOfBook layout."),
])

p("CPP06", 28, ("MoldTest.TrimmedViewOutlivesTheCall", "tests/mold/test_mold.cpp"), [
    (2, "run", "ASAN_OPTIONS=detect_stack_use_after_return=1 itch-fh ..."),
    (6, "look", "ASan: stack-use-after-return reading a frame of lab::mold::trimmed."),
    (10, "look", "Signature changed from const Session& to Session: the view now points at the parameter copy."),
    (14, "cause", "A string_view into a by-value argument dangles as soon as the function returns; what you read depends on the stack."),
    (18, "edit", "Take const Session& again; add -Wdangling and detect_stack_use_after_return to the asan preset."),
])

p("CPP07", 35, ("FixSessionTest.ClockBehindLastReceiveDoesNotTimeOut", "tests/fix/test_session.cpp"), [
    (3, "look", "Gateway log: Logout 'test request timeout' 2 s after logon, only on gw02."),
    (9, "look", "gw02 runs fixgw with --clock realtime; poll() gets wall time but accept() was given monotonic ns."),
    (15, "think", "Two clocks mixed: last_rx_ns_ can be ahead of now_ns."),
    (20, "cause", "since() became now - then. When now < then the unsigned result wraps to ~1.8e19 ns, so every timer is overdue."),
    (26, "edit", "Saturate again, and make every caller pass mono_ns()."),
])

p("CPP08", 70, ("HugePagesTest.MisalignedBaseStaysInsideMapping", "tests/core/test_huge_pages.cpp"), [
    (3, "look", "Core file: SIGSEGV in std::vector<Node> value-initialisation inside Engine::Engine."),
    (10, "look", "Faulting address is just past the mmap reported in /proc/pid/maps."),
    (20, "think", "Laptops run kernel 6.x, colo runs 5.15. What changed in mmap alignment?"),
    (32, "look", "Kernel 6.7 added THP alignment for anonymous mappings of 2 MB and up. Before that, the base is only 4 KB aligned."),
    (45, "cause", "span = size assumed an aligned base. On 5.15 the code rounds the base up and walks off the end of the mapping, after unmapping the head."),
    (55, "edit", "Map size + 2 MB and trim both ends again, with a comment on why."),
    (62, "test", "Test maps a 4 KB placeholder first so the next mapping is misaligned, then allocates and touches every page."),
])

p("CPP09", 95, ("OrderMapTest.EraseAcrossTheWrapPoint", "tests/book/test_order_map.cpp"), [
    (5, "look", "Errors appear only after ~8.4M order references, then in bursts."),
    (15, "think", "8,388,608 slots: the identity hash puts reference r at r mod 2^23. The live window crosses the end of the table there."),
    (30, "run", "Fuzz test: random insert/erase against std::unordered_map with capacity 16."),
    (40, "look", "First mismatch: erase at slot 15 with the next entry's home at 14 in slot 0."),
    (55, "cause", "Backward-shift must treat (i, j] cyclically. Without the wrap branch it moves an entry that belongs before the hole, leaving its own key unreachable."),
    (70, "edit", "Restore the cyclic interval check."),
    (80, "test", "Keep the differential fuzz test at capacity 16 so wraps happen every few operations."),
])

p("CPP10", 80, ("BookBuilderTest.ReplaceDuringRehash", "tests/book/test_book_builder.cpp"), [
    (5, "look", "Stack: OrderMap::erase_slot from BookBuilder::on(OrderReplace); slot index far beyond capacity."),
    (15, "look", "Recent change: insert the new reference before erasing the old one."),
    (25, "think", "insert() rehashes past 70% load; o points into the old vector."),
    (40, "run", "BookBuilder with expected_live_orders = 16 under ASan, replaying the 1M test day."),
    (48, "look", "ASan: heap-use-after-free in erase_found, freed by vector reallocation in rehash."),
    (55, "cause", "Pointer invalidation hidden by pre-sizing: only the busiest days pass 70% load."),
    (65, "edit", "Erase first. If the insert fails the replace is already an integrity error, counted as before."),
])

# ---------------------------------------------------------------- Market data
p("MD01", 6, ("LuldTest.TierBoundaries", "tests/book/test_luld.cpp"), [
    (1, "read", "LULD Plan, appendix A: Tier 1, reference price more than $3.00: 5%."),
    (3, "look", "luld_width uses >= 30'000."),
    (4, "cause", "At exactly $3.00 the stock is in the 20% band."),
    (5, "edit", "Strictly greater, plus a table test at 2.99, 3.00 and 3.01."),
])

p("MD02", 5, ("OrderBookTest.LockedIsNotCrossed", "tests/book/test_order_book.cpp"), [
    (1, "look", "Crossed events jumped the same night locked events stayed flat: one predicate absorbing the other."),
    (3, "cause", "crossed() uses >=, which includes locks."),
    (4, "edit", "bid > ask for crossed, == for locked."),
])

p("MD03", 5, ("BookBuilderTest.OpeningAndClosingCrossPrices", "tests/book/test_book_builder.cpp"), [
    (1, "look", "Summary open equals the official close for every symbol, and the other way round."),
    (3, "cause", "The O and C letters were swapped when the lines were reordered."),
    (4, "edit", "O opens, C closes (ITCH 5.0 section 1.10)."),
])

p("MD04", 22, ("BookBuilderTest.AttributedAddJoinsTheBook", "tests/book/test_book_builder.cpp"), [
    (3, "look", "Every unknown reference was first added by an F message (grep the reference in itch-dump --trace)."),
    (8, "look", "BookBuilder has no on(AddOrderMpid) any more; dispatch quietly uses HandlerBase's no-op."),
    (12, "cause", "Removing an overload is silent with a catch-all default handler."),
    (16, "edit", "Restore the F handler."),
    (19, "note", "Proposes a static_assert listing the message types BookBuilder must handle."),
])

p("MD05", 18, ("EndianTest.Load48AboveFourSeconds", "tests/core/test_endian.cpp"), [
    (2, "look", "Regressions come every 4.29 s of feed time: 2^32 ns."),
    (6, "look", "load_be48 shifts the top 16 bits by 16."),
    (10, "cause", "The high bits must sit above the 32-bit low word. Shifting by 16 overlaps them, so time steps back each time the low word wraps."),
    (14, "edit", "Shift by 32; test a timestamp of 34,200 s (09:30) in nanoseconds."),
])

p("MD06", 30, ("BookBuilderTest.EmptyHaltCrossRoundsMidToCent", "tests/book/test_book_builder.cpp"), [
    (4, "look", "Vendor band 10.0050 to 11.0555 against simex 10.00 to 11.05 after an empty halt cross."),
    (12, "read", "luld.hpp: the lab rule rounds the midpoint down to the cent."),
    (18, "cause", "The midpoint is (bid + ask) / 2 without rounding, so sub-penny references produce sub-penny bands."),
    (24, "edit", "/ 200 * 100 again, with a test on a 10.005 midpoint."),
])

p("MD07", 16, ("TraderMarketData.MapsLocateNotTrackingNumber", "tests/firm/test_market_data.cpp"), [
    (2, "look", "Trader report: 'tops in 0'. The feed itself is fine: itch-fh rebuilds books."),
    (6, "look", "instrument_of_ is set from R messages, but every lookup lands on locate 0."),
    (10, "cause", "load_be16(msg + 3) reads the tracking number. Synthetic days set it to 0."),
    (13, "edit", "Decode the ITCH header instead of a hand-written offset."),
])

p("MD08", 55, ("BookBuilderTest.LuldReferenceIsTheOpenNotTheLastTrade", "tests/book/test_book_builder.cpp"), [
    (5, "look", "Replays the run-up: the vendor's band upper is always 5% above the last trade."),
    (15, "read", "LULD Plan: the reference is the opening print, a reopening, or a 5-minute average. Never the last trade."),
    (25, "look", "traded() overwrites luld_reference on every print."),
    (35, "cause", "The band chases the price, so a steady climb can never reach it."),
    (45, "edit", "Set the reference only from a printing cross or the first trade of the day."),
])

p("MD09", 65, ("BookBuilderTest.RepeatedDirectoryKeepsTheBook", "tests/book/test_book_builder.cpp"), [
    (5, "look", "Inconsistencies start right after a second R message for the same locate at 10:41."),
    (15, "read", "ITCH 5.0: stock directory messages can be resent intraday (for example after a symbol change)."),
    (28, "look", "on(StockDirectory) now resets inst.book."),
    (40, "cause", "The book is cleared but the order store still holds the orders; their next cancel or execution reduces a level that no longer exists."),
    (52, "edit", "Directory messages update reference data only."),
])

p("MD10", 110, ("ItchCodecTest.GoldenOrderReplace", "tests/itch/test_codec.cpp"), [
    (5, "look", "Synthetic day clean, Nasdaq sample day: crossed books right after U messages."),
    (15, "run", "itch-dump --trace on the real file, first bad U message."),
    (25, "look", "Decoded shares 2,500,000 and price 300: the values look swapped."),
    (40, "read", "Spec PDF, Order Replace: Shares at offset 27, Price at 31."),
    (55, "look", "visit() has price at 27 and shares at 31. Both are 4 bytes, so the length check passes."),
    (70, "cause", "Encoder and decoder share the field list, so round trips and itch-gen agree with each other. Only real data or golden bytes show it."),
    (85, "edit", "Restore the offsets."),
    (95, "test", "Golden-byte tests for every message type, built from the spec's tables, not from encode()."),
])

# ---------------------------------------------------------------- Network
p("NET01", 10, ("UdpTest.ReceiverGetsTheRequestedBuffer", "tests/net/test_udp.cpp"), [
    (1, "run", "netstat -su | grep 'receive buffer errors'"),
    (3, "look", "Errors rise with every burst."),
    (5, "run", "ss -uamp sport :31001 shows rb212992: the kernel default."),
    (7, "cause", "SO_RCVBUF is no longer set on the feed socket."),
    (8, "edit", "Set it again; the test reads SO_RCVBUF back."),
])

p("NET02", 8, ("UdpTest.MulticastLoopbackOnOneHost", "tests/net/test_udp.cpp"), [
    (1, "run", "tcpdump -i lo udp port 31001: packets are leaving the publisher."),
    (3, "look", "itch-fh joined the group (ip maddr show) but receives nothing."),
    (5, "cause", "IP_MULTICAST_LOOP is 0, so same-host members never get the packets."),
    (6, "edit", "Loopback on; add a multicast run to the e2e scripts so CI covers it."),
])

p("NET03", 9, ("UdpTest.MulticastRange", "tests/net/test_udp.cpp"), [
    (1, "look", "224.0.1.1 works, 239.1.1.1 does not: so the address class test is suspect."),
    (4, "look", "Mask 0xFF000000 only matches 224.x."),
    (6, "cause", "Multicast is 224.0.0.0/4: the mask must be 0xF0000000."),
    (7, "edit", "Fix the mask; test 223.255.255.255, 224.0.0.1, 239.255.255.255 and 240.0.0.0."),
])

p("NET04", 32, ("TcpTest.PartialSendQueuesOnlyTheTail", "tests/net/test_tcp.cpp"), [
    (3, "look", "Malformed packets happen only when the client's send buffer fills."),
    (10, "run", "Test: SO_SNDBUF 4 KB, reader sleeps, writer sends 1 MB of numbered packets."),
    (16, "look", "The reader sees a packet's first 2,880 bytes twice."),
    (22, "cause", "After a partial send() the whole buffer is queued, not data[sent..]."),
    (27, "edit", "Queue from data.begin() + sent."),
])

p("NET05", 24, ("TcpTest.PeerCloseIsSeen", "tests/net/test_tcp.cpp"), [
    (3, "look", "Gateway log: no disconnect for the trader until the idle timeout, 15 s after its crash."),
    (9, "look", "recv() returns 0 and TcpStream::recv reports 'no data'."),
    (14, "cause", "0 from recv() is an orderly close, not EAGAIN. Treating it as no data hides the disconnect, so cancel-on-disconnect waits for the timer."),
    (19, "edit", "Only n > 0 is data; 0 marks the stream failed."),
])

p("NET06", 26, ("SequencerTest.ReorderWithinDelayIsNotRequested", "tests/mold/test_sequencer.cpp"), [
    (3, "look", "Requests per gap: almost all are answered with packets the receiver already has."),
    (9, "look", "Gaps last microseconds: it is reordering, not loss."),
    (15, "look", "poll() sends the first request at once; gap_delay is no longer checked."),
    (20, "cause", "Every reorder became a retransmission request."),
    (23, "edit", "Wait gap_delay before the first request."),
])

p("NET07", 30, ("FeedTest.PayloadFitsReceiverSlot", "tests/mold/test_pipeline.cpp"), [
    (3, "look", "itch-fh: thousands of oversize packets, gaps on every busy packet."),
    (9, "run", "tcpdump: 1,528-byte IP datagrams, fragmented in two."),
    (15, "cause", "1500 is the MTU of the IP packet. The UDP payload is 1500 minus 28 for the IP and UDP headers, and the receiver's slots are 1,472."),
    (22, "edit", "Back to 1400, and static_assert payload <= RxPacket::kMaxLen."),
])

p("NET08", 60, ("SequencerTest.OtherSessionIsIgnored", "tests/mold/test_sequencer.cpp"), [
    (5, "look", "After the restart: packets arrive, delivered stays flat, duplicates climb."),
    (15, "look", "New session name, sequence back at 1, expected still 2.4M."),
    (28, "look", "Sequencer adopts any session it sees; wrong_session is never counted."),
    (40, "cause", "Following a new session keeps the old expected sequence, so the new session's messages all look like duplicates."),
    (50, "edit", "Latch the first session; a different one is counted and ignored. A restart is an explicit operator action."),
])

p("NET09", 105, ("RetransmitStoreTest.EveryMessageAfterWrap", "tests/mold/test_retransmit_store.cpp"), [
    (5, "look", "Digest diverges only in runs over an hour with loss enabled."),
    (20, "run", "Lossy live-feed run with a 1 MB store so it wraps quickly."),
    (35, "look", "Retransmitted message 4,190,233 has the bytes of a different message."),
    (55, "look", "append() resets at to 0 on wrap but no longer adds the padding to written_."),
    (70, "cause", "Stored start positions drift from where bytes really are after the first wrap, so get() serves the wrong bytes with the right sequence."),
    (85, "edit", "Count the padding in written_."),
    (95, "test", "Fill past several wraps, then get() every stored sequence and compare."),
])

p("NET10", 90, ("PublisherTest.HeartbeatOnlyAdvertisesSentMessages", "tests/mold/test_pipeline.cpp"), [
    (5, "look", "Lost messages only in quiet periods, followed by duplicates of the same sequences."),
    (20, "look", "A heartbeat with sequence N+3 arrives before the packet holding N..N+2."),
    (35, "look", "The heartbeat uses store_.next_sequence(), which counts messages still in the open packet."),
    (50, "cause", "Receivers open a gap for messages not yet sent; the server cannot answer, they give up, then drop the real packet as duplicates."),
    (65, "edit", "Heartbeats carry sent_next_."),
    (78, "test", "Add a message, fire service() before the batching delay, check the heartbeat's sequence."),
])

# ---------------------------------------------------------------- Latency
p("LAT01", 8, ("TcpTest.NoDelayIsSet", "tests/net/test_tcp.cpp"), [
    (1, "look", "Round trip p99 of about 40 ms: the delayed-ACK timer."),
    (3, "run", "ss -ti: no 'nodelay' on the session."),
    (5, "cause", "TCP_NODELAY is no longer set, so Nagle waits for the previous ACK."),
    (6, "edit", "Set it in the constructor; test with getsockopt."),
])

p("LAT02", 10, ("ReceiverTest.NoSleepWhileBusy", "tests/mold/test_pipeline.cpp"), [
    (1, "run", "perf trace -s -p feed-rx: nanosleep after every recvmmsg."),
    (4, "look", "The idle check was dropped from the sleep condition."),
    (6, "cause", "The thread sleeps after full batches too, so the socket backs up."),
    (8, "edit", "Sleep only when nothing was received."),
])

p("LAT03", 6, ("TradingTest.NoSyscallOnFill", "tests/firm/test_trading.cpp"), [
    (1, "run", "strace -c -f -p trader thread: write(2) on the trading thread."),
    (3, "look", "fprintf(stderr, \"fill ...\") in on_response."),
    (4, "cause", "A blocking syscall on the hot path."),
    (5, "edit", "Remove it; fills are already in the fill log."),
])

p("LAT04", 22, ("TradingTest.FillLogNeverReallocates", "tests/firm/test_trading.cpp"), [
    (3, "run", "perf record during a stall: memmove and page faults under std::vector::_M_realloc_insert."),
    (9, "look", "fills_.push_back without the capacity check."),
    (14, "cause", "Past its reservation the vector doubles on the trading thread, copying the whole log."),
    (18, "edit", "Restore the bound and count dropped fills."),
])

p("LAT05", 25, ("ShmTest.PagesArePopulated", "tests/core/test_shm.cpp"), [
    (3, "look", "Spikes only in the first 30 s, gone after."),
    (9, "run", "perf stat -e page-faults: 60k minor faults in the first seconds."),
    (14, "look", "MAP_POPULATE no longer set in map_fd."),
    (18, "cause", "Every ring page faults on first touch, on the hot thread."),
    (21, "edit", "Populate again; the test checks mincore() right after create."),
])

p("LAT06", 20, ("PublisherTest.MaxDelayIsMicroseconds", "tests/mold/test_pipeline.cpp"), [
    (3, "look", "Feed age is fine when busy and bad when quiet: a batching timer."),
    (9, "look", "max_delay_us * 1000000 in two places."),
    (13, "cause", "Microseconds converted with a million: the delay is a thousand times too long."),
    (17, "edit", "Use std::chrono::microseconds in the config so the unit is in the type."),
])

p("LAT07", 30, ("bench: BM_ItchBookRebuild before/after", "bench/bm_itch.cpp"), [
    (3, "run", "itch_bench --benchmark_filter=BookRebuild, before and after the release."),
    (10, "run", "perf record: most time in BookSide::find."),
    (16, "look", "The scan now starts at the worst level, with no early exit."),
    (22, "cause", "Activity is at the touch, which sits at the back of the vector."),
    (26, "edit", "Scan from the back again; numbers from both runs go in the PR."),
])

p("LAT08", 60, ("bench: BM_RingCrossThreadThroughput pinned", "bench/bm_core.cpp"), [
    (5, "run", "core_bench ring benchmark, threads pinned to separate cores."),
    (15, "run", "perf c2c record: heavy HITM on the line holding head_."),
    (28, "look", "front() reloads head_ on every call; cached_head_ is no longer used as a cache."),
    (40, "cause", "Every pop reads a line the producer keeps writing, so it bounces between cores."),
    (50, "edit", "Read head_ only when the cached copy says the ring is empty."),
])

p("LAT09", 75, ("TraderMarketData.TopCarriesPacketArrivalTick", "tests/firm/test_market_data.cpp"), [
    (5, "look", "Tick-to-wire improved by 30% in a week with no hot-path change."),
    (20, "think", "When a number improves and the outcome worsens, check what is being measured."),
    (35, "look", "t.rx_tsc is now rdtsc() after the book update, not the packet arrival."),
    (50, "cause", "Queueing in the packet ring and the book rebuild moved out of the measurement."),
    (62, "edit", "Carry the arrival tick; report queueing separately."),
])

p("LAT10", 65, ("bench: match_bench with perf stat dTLB", "bench/bm_match.cpp"), [
    (5, "run", "match_bench before and after: p99 of a random-key lookup is up."),
    (18, "run", "perf stat -e dTLB-load-misses: an order of magnitude more."),
    (30, "look", "KeyIndex::slots_ is a plain std::vector now."),
    (42, "cause", "128 MB of random access on 4 KB pages misses the TLB on most lookups."),
    (52, "edit", "HugePageAllocator again; dTLB misses go into the benchmark output."),
])

# ---------------------------------------------------------------- Trading core
p("TRD01", 6, ("MarketMakerTest.ImprovesAtExactlyThreshold", "tests/firm/test_market_maker.cpp"), [
    (1, "look", "No improvement at 3-tick spreads; 4-tick spreads still improve."),
    (3, "cause", "> instead of >= in the threshold."),
    (5, "edit", ">= improve_spread_ticks, test a spread of exactly 3 ticks."),
])

p("TRD02", 8, ("MarketMakerTest.LongPositionLowersBothQuotes", "tests/firm/test_market_maker.cpp"), [
    (1, "look", "Position grows in the direction of the last fills."),
    (3, "look", "bid += lean; ask += lean."),
    (5, "cause", "Long should lower the quotes; adding raises them."),
    (6, "edit", "Subtract the lean."),
])

p("TRD03", 7, ("MarketMakerTest.CapIncludesQuoteSize", "tests/firm/test_market_maker.cpp"), [
    (1, "look", "Max |pos| 1,100 with a 1,000 cap: one quote too many."),
    (3, "cause", "The bid check leaves out the new quote's size; the ask check has it."),
    (5, "edit", "position + size <= max_position."),
])

p("TRD04", 28, ("OmsTest.CashUsesExecutionPrice", "tests/firm/test_oms.cpp"), [
    (3, "run", "research/report.py: PnL breaks are all on fills with liquidity O or C."),
    (10, "look", "Those fills print better than our limit."),
    (16, "look", "on_executed books q * o->price."),
    (21, "cause", "Cash at the limit, not the execution price."),
    (25, "edit", "Use r.price."),
])

p("TRD05", 18, ("RiskTest.CollarIsBasisPoints", "tests/firm/test_risk.cpp"), [
    (2, "look", "Order 30% through the mid, verdict ok."),
    (8, "look", "band = reference * collar_bps / 1'000."),
    (12, "cause", "Basis points are per 10,000: the collar is ten times too wide."),
    (15, "edit", "/ 10'000; tests at 4.9% and 5.1%."),
])

p("TRD06", 30, ("TradingTest.ReplaceUpdatesAskRef", "tests/firm/test_trading.cpp"), [
    (3, "look", "Orders still working after stop: all offers, all replacements."),
    (12, "look", "on_response updates bid_ref after a replace; the ask line is gone."),
    (20, "cause", "The ask side keeps a done order's ref, so reconcile enters a new offer and loses the live one."),
    (26, "edit", "Restore the ask update; test both sides."),
])

p("TRD07", 22, ("EngineTest.SamePriceFillsInArrivalOrder", "tests/match/test_engine.cpp"), [
    (2, "look", "At one price the newest order always fills first."),
    (8, "look", "link() puts the node at the head of the level."),
    (14, "cause", "Price-time priority is FIFO within a level; this is LIFO."),
    (19, "edit", "Append at the tail."),
])

p("TRD08", 85, ("OmsTest.ReplaceBelowExecutedAbandonsReplacement", "tests/firm/test_oms.cpp"), [
    (5, "look", "Stuck instrument: open_buy 100 with no live buy on simex."),
    (20, "run", "order-trace on the stuck UserRefNum."),
    (35, "look", "Replace to 100 on an order with 100 executed; simex answers Canceled (U), never Replaced or Rejected."),
    (50, "read", "OUCH 5.0 section 2.3: a replace for no more than executed shares cancels the order."),
    (62, "cause", "The pending replacement stays working with its exposure, so risk blocks and stop waits forever."),
    (75, "edit", "Abandon the replacement on that user cancel."),
])

p("TRD09", 70, ("EngineTest.IndicativeTieGoesToReference", "tests/match/test_engine.cpp"), [
    (5, "look", "Opening prints always at the bottom of the range of equal-volume prices."),
    (20, "read", "Nasdaq cross rules: maximise volume, then minimise imbalance, then nearest the reference."),
    (35, "look", "The distance tie-break is gone from indicative()."),
    (50, "cause", "Ties go straight to the lowest price."),
    (60, "edit", "Restore the reference tie-break."),
])

p("TRD10", 100, ("TradingTest.NoSelfTradeWhileBidReplaceInFlight", "tests/firm/test_trading.cpp"), [
    (5, "look", "Every self-trade is within a millisecond of a replace on the other side."),
    (25, "run", "Replays the trader log for one self-trade."),
    (40, "look", "Bid replace 10.02 to 10.04 in flight; ask quoted at 10.03 because the guard used 10.02."),
    (60, "cause", "With a replace in flight either price may be live, so the guard needs the more aggressive one. reach() takes the less aggressive."),
    (80, "edit", "max for bids, min for asks."),
])

# ---------------------------------------------------------------- Order gateway
p("GW01", 5, ("GatewayTest.WrongPasswordRejected", "tests/exchange/test_gateway.cpp"), [
    (1, "look", "login(): the password comparison is gone."),
    (3, "cause", "Any password is accepted for a known user."),
    (4, "edit", "Match user and password; test Login Rejected 'A'."),
])

p("GW02", 6, ("GatewayTest.EverySideCode", "tests/exchange/test_gateway.cpp"), [
    (1, "look", "Reject 0x0009 is 'invalid side'; only side E is rejected."),
    (3, "cause", "case kSellShortExempt fell out of the switch."),
    (5, "edit", "Restore it; test B, S, T, E and an invalid code."),
])

p("GW03", 12, ("GatewayTest.DropCopySideIsBuyOrSell", "tests/exchange/test_gateway.cpp"), [
    (2, "run", "research/report.py: unmatched drop copy rows all have side T."),
    (6, "cause", "The drop copy passes OUCH side codes through; its format only has B and S."),
    (9, "edit", "Map every sell code to S."),
])

p("GW04", 20, ("GatewayTest.RepeatedUserRefNumIgnored", "tests/exchange/test_gateway.cpp"), [
    (2, "look", "Two accepted orders with one UserRefNum, the second right after reconnect."),
    (9, "read", "OUCH 5.0: UserRefNum must increase; a repeat is ignored."),
    (14, "cause", "admit() uses <, so the last reference can be entered again."),
    (17, "edit", "<= last_ref is stale."),
])

p("GW05", 26, ("GatewayTest.ReconnectWithZeroSendsOnlyNew", "tests/exchange/test_gateway.cpp"), [
    (3, "look", "The duplicate fill arrived right after Login Accepted."),
    (10, "read", "SoupBinTCP: sequence 0 asks for new messages only."),
    (16, "look", "first = count for 0: the last message is resent."),
    (21, "edit", "count + 1; test a reconnect after a fill."),
])

p("GW06", 24, ("OrderEntryTest.HeartbeatWithinIdleTimeout", "tests/firm/test_order_entry.cpp"), [
    (3, "look", "Disconnects only after 15 s with no orders."),
    (9, "look", "Client heartbeat after 20 s idle; gateway timeout is 15 s."),
    (15, "cause", "A quiet but healthy trader looks dead to the exchange."),
    (20, "edit", "Heartbeat after 1 s idle (SoupBinTCP), one config for both sides."),
])

p("GW07", 15, ("FixCodecTest.ChecksumMatchesSpecExample", "tests/fix/test_codec.cpp"), [
    (2, "look", "Counterparty logs: every message from us dropped, bad CheckSum."),
    (6, "look", "finish() sums from kHeadRoom, after 8= and 9=."),
    (10, "cause", "FIX CheckSum covers everything from 8= up to 10=."),
    (13, "edit", "Sum from start; test a spec example message."),
])

p("GW08", 60, ("FixSessionTest.GapFillNewSeqNoIsNextAfterGap", "tests/fix/test_session.cpp"), [
    (5, "look", "ResendRequest, gap fill, ResendRequest again, forever."),
    (20, "look", "Gap fill for 5..9 says NewSeqNo 9; the client then expects 9 and receives 10."),
    (35, "read", "FIX 4.4: NewSeqNo is the next sequence number to be expected after the gap."),
    (45, "cause", "upto - 1 is one short, so the counterparty always sees a new gap."),
    (52, "edit", "NewSeqNo = upto."),
])

p("GW09", 70, ("FixSessionTest.PossDupBelowExpectedNotDelivered", "tests/fix/test_session.cpp"), [
    (5, "look", "Two fills for one ExecID, the second with PossDupFlag Y."),
    (25, "look", "Session hands PossDup messages below the expected sequence to on_app."),
    (40, "read", "FIX 4.4: a message below the expected number with PossDupFlag is ignored."),
    (55, "cause", "Relying on every handler to dedup by ExecID; the order handler does not."),
    (62, "edit", "Drop it at the session layer, and add ExecID dedup in the client as a second line."),
])

p("GW10", 80, ("GatewayTest.ReduceThenReplace", "tests/exchange/test_gateway.cpp"), [
    (5, "look", "Rejects 0x0015 only on orders that had a partial cancel first."),
    (20, "run", "order-trace on one: reduce to 100, then replace: 'unknown original'."),
    (40, "look", "drain_events marks every cancel as done and erases the order's info."),
    (58, "cause", "A partial cancel leaves the order live; erasing its ClOrdID and side breaks the next replace and blanks later fills."),
    (70, "edit", "done only when remaining is 0."),
])
