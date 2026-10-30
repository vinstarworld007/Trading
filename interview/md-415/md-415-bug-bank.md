# MD-415 bug bank: 60 planted bugs and how Priya solves them

Every bug is a small patch against `vinstarworld007/Trading` main (5f398f4). Each was planted alone, built, and run through ctest (183 tests), `itch-dump` on a 1M-message synthetic day, and `e2e-firm`, `e2e-live-feed` and `e2e-fix`; the bigger suspects also ran a 20M-message day, ASan, or the book and ring benchmarks. "Caught by" is what actually failed. **Silent** means every existing check passed.

To plant a bug: `md-415-bugs.py` next to this file holds every patch as `BUGS[id]["edits"] = [(file, before, after)]`. In a clean checkout of main, replace `before` with `after` in each file; every `before` string occurs exactly once. `git checkout -- <file>` undoes it.

## C++

| ID | Level | Bug | Caught by | Priya's time | Her regression test |
|---|---|---|---|---|---|
| CPP01 | easy | Logger stop() never clears the running flag | Unit tests | 9 min | `AsyncLoggerTest.StopReturnsWhileWorkerRuns` |
| CPP02 | easy | Thread name copied without its NUL terminator | Unit tests | 7 min | `AffinityTest.LongThreadNameIsTruncatedNotDropped` |
| CPP03 | easy | Huge-page free path disagrees with the allocate path at exactly 2 MB | **Silent** | 12 min | `HugePagesTest.ExactlyTwoMegabytesRoundTrips` |
| CPP04 | medium | TcpStream move-assignment leaves the source owning the fd | Unit tests, e2e firm, e2e FIX | 18 min | `TcpTest.MoveAssignedStreamKeepsItsSocket` |
| CPP05 | medium | Top-of-book change test compares 24 of 32 bytes | **Silent** | 25 min | `TraderMarketData.AskSizeChangeSendsTop` |
| CPP06 | medium | trimmed() takes the session by value and returns a view into it | Unit tests | 28 min | `MoldTest.TrimmedViewOutlivesTheCall` |
| CPP07 | medium | FIX timer helper underflows when the clock steps back | **Silent** | 35 min | `FixSessionTest.ClockBehindLastReceiveDoesNotTimeOut` |
| CPP08 | hard | Huge-page mapping has no slack for alignment | **Silent** | 70 min | `HugePagesTest.MisalignedBaseStaysInsideMapping` |
| CPP09 | hard | Order map backward-shift ignores probe runs that wrap | Unit tests, Book digest | 95 min | `OrderMapTest.EraseAcrossTheWrapPoint` |
| CPP10 | hard | Replace inserts the new order before erasing through a stale pointer | **Silent** | 80 min | `BookBuilderTest.ReplaceDuringRehash` |

### CPP01 · Logger stop() never clears the running flag (easy)

- **Report:** Every process that starts the async logger hangs on exit; Ctrl-C twice to kill it.
- **Cause:** stop() reads the flag instead of exchanging it, so the worker's loop never sees false and join() waits forever.
- **Fix:** Restore running_.exchange(false): one atomic read-modify-write that both tests and clears the flag.
- **Regression test:** `AsyncLoggerTest.StopReturnsWhileWorkerRuns` in `tests/core/test_log.cpp`
- **Unit tests:** 1 of 183 tests fail; AsyncLogger.BackgroundThreadDrainsManyProducers

`libs/core/src/log.cpp`
```diff
-   if (running_.exchange(false)) {
-     if (worker_.joinable()) worker_.join();
+   if (running_.load(std::memory_order_acquire)) {
+     if (worker_.joinable()) worker_.join();
```

### CPP02 · Thread name copied without its NUL terminator (easy)

- **Report:** perf and top -H show some threads as the binary name instead of feed-rx or trader-md.
- **Cause:** Linux thread names are 15 characters plus NUL. Copying 16 leaves no terminator for long names, and pthread_setname_np rejects them with ERANGE, which the code ignores.
- **Fix:** Copy at most sizeof buf - 1 bytes, and check the return code in a test with a 20-character name.
- **Regression test:** `AffinityTest.LongThreadNameIsTruncatedNotDropped` in `tests/core/test_affinity.cpp`
- **Unit tests:** 1 of 183 tests fail; Affinity.NamesThread

`libs/core/src/affinity.cpp`
```diff
-   name.copy(buf, sizeof buf - 1);
+   name.copy(buf, sizeof buf);
```

### CPP03 · Huge-page free path disagrees with the allocate path at exactly 2 MB (easy)

- **Report:** simex aborts at shutdown with 'free(): invalid pointer' on one config; other configs are fine.
- **Cause:** A table of exactly 2 MB is mmap'ed by huge_page_map (bytes >= 2 MB) but released with operator delete (bytes <= 2 MB): an mmap'ed pointer handed to the heap allocator.
- **Fix:** Use the same predicate in both functions (bytes < kHugePageSize), ideally one helper both call.
- **Regression test:** `HugePagesTest.ExactlyTwoMegabytesRoundTrips` in `tests/core/test_huge_pages.cpp`

`libs/core/include/lab/core/huge_pages.hpp`
```diff
- inline void huge_page_unmap(void* p, std::size_t bytes) noexcept {
-   if (bytes < kHugePageSize) {
+ inline void huge_page_unmap(void* p, std::size_t bytes) noexcept {
+   if (bytes <= kHugePageSize) {
```

### CPP04 · TcpStream move-assignment leaves the source owning the fd (medium)

- **Report:** Every OUCH session drops straight after login; the trader logs 'exchange closed the connection during login'.
- **Cause:** Move-assignment copies fd_ but does not reset the source, so the moved-from temporary's destructor closes the socket the new owner is using (and later a reused fd number).
- **Fix:** Reset o.fd_ to -1 in move-assignment, as the move constructor already does; add a test that moves a connected stream and still sends.
- **Regression test:** `TcpTest.MoveAssignedStreamKeepsItsSocket` in `tests/net/test_tcp.cpp`
- **Unit tests:** 6 of 183 tests fail; FixSession.LogsOnAndDeliversInOrder; FixSession.FillsAGapWithResendAndGapFill; FixSession.ResendsWhatWasMissedWhileDisconnected; FixSession.LogsOutALogonWithASequenceNumberTooLow
- **e2e firm:** exit 1; Heartbeats   3, End of Session 20, send errors 0; FAIL: trader exited 2; FAIL: ouch-client exited 1
- **e2e FIX:** exit 1; FAIL: fix-client; FAIL: fixgw; FAIL: dropcopy

`libs/net/src/tcp.cpp`
```diff
-     out_ = std::move(o.out_);
-     out_pos_ = o.out_pos_;
-     o.fd_ = -1;
-   }
-   return *this;
+     out_ = std::move(o.out_);
+     out_pos_ = o.out_pos_;
+   }
+   return *this;
```

### CPP05 · Top-of-book change test compares 24 of 32 bytes (medium)

- **Report:** The strategy keeps quoting against size that is no longer there on the offer; fills on the ask look stale.
- **Cause:** The magic 32 was edited to 24 ('three arrays'), so the comparison skips ask_qty. A change in offer size alone no longer sends a new top of book.
- **Fix:** Compare with sizeof over the four arrays (or a defaulted operator== on a struct of them), and static_assert the layout.
- **Regression test:** `TraderMarketData.AskSizeChangeSendsTop` in `tests/firm/test_market_data.cpp`

`firm/trader/main.cpp`
```diff
-     if (std::memcmp(t.bid_px, last.bid_px, 32) == 0 && t.trading_state == last.trading_state)
+     if (std::memcmp(t.bid_px, last.bid_px, 24) == 0 && t.trading_state == last.trading_state)
```

### CPP06 · trimmed() takes the session by value and returns a view into it (medium)

- **Report:** Feed handler reports print a garbled session name ('SIMEX\x7f\x02...') in about one run in five.
- **Cause:** The string_view points into the by-value parameter, which dies when trimmed() returns: a dangling view whose bytes depend on what reuses the stack.
- **Fix:** Take the array by const reference again; build with -fsanitize=address and detect_stack_use_after_return=1 in CI.
- **Regression test:** `MoldTest.TrimmedViewOutlivesTheCall` in `tests/mold/test_mold.cpp`
- **Unit tests:** 1 of 183 tests fail; Mold.SessionIsSpacePaddedAndTrimmed

`libs/mold/include/lab/mold/mold.hpp`
```diff
- inline std::string_view trimmed(const Session& s) noexcept {
+ inline std::string_view trimmed(Session s) noexcept {
```

### CPP07 · FIX timer helper underflows when the clock steps back (medium)

- **Report:** FIX clients are logged out seconds after logon on one gateway host, never on the others.
- **Cause:** since() subtracts unsigned nanoseconds. When the time given to poll() is earlier than last_rx_ns_ (callers pass different clocks), the difference wraps to about 584 years, so the test-request timeout fires at once.
- **Fix:** Saturate at zero as before, and give every caller one monotonic clock.
- **Regression test:** `FixSessionTest.ClockBehindLastReceiveDoesNotTimeOut` in `tests/fix/test_session.cpp`

`libs/fix/include/lab/fix/session.hpp`
```diff
-     return now > then ? now - then : 0;
+     return now - then;
```

### CPP08 · Huge-page mapping has no slack for alignment (hard)

- **Report:** simex segfaults at startup on the colo boxes (kernel 5.15) and runs fine on every laptop (6.x).
- **Cause:** Kernels from 6.7 return 2 MB aligned addresses for large anonymous mmaps, so the extra 2 MB looked unnecessary. Older kernels do not; the aligned pointer then runs past the end of the mapping, and the head was just unmapped.
- **Fix:** Map size + 2 MB and trim both ends, as before; add a test that forces a misaligned base with a placeholder mapping.
- **Regression test:** `HugePagesTest.MisalignedBaseStaysInsideMapping` in `tests/core/test_huge_pages.cpp`

`libs/core/include/lab/core/huge_pages.hpp`
```diff
-   const std::size_t span = size + kHugePageSize;
+   const std::size_t span = size;  // THP aligns large anonymous mappings already
```

### CPP09 · Order map backward-shift ignores probe runs that wrap (hard)

- **Report:** Big days only: a few hundred 'unknown order reference' errors appear late in the session, always on different symbols.
- **Cause:** Linear probing wraps from the last slot to the first. When a deletion's probe run crosses that boundary, the simplified test moves entries that should stay, so later lookups stop at a hole before reaching their key.
- **Fix:** Restore the cyclic interval test. The identity hash makes the live window sweep the whole table, so it crosses the wrap point once per capacity's worth of references.
- **Regression test:** `OrderMapTest.EraseAcrossTheWrapPoint` in `tests/book/test_order_map.cpp`
- **Unit tests:** 1 of 183 tests fail; OrderMap.MatchesReferenceMapUnderRandomChurnAndGrowth
- **Book digest:** 20M-message day: itch-dump exited 1; 20M-message day: digest 3895e6c2ddc65217 (main b0edde0a27a2eaa4); 20M-message day: ERROR unknown order reference 1 (main 0)

`libs/book/include/lab/book/order_map.hpp`
```diff
-       const bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
+       const bool stays = i < k && k <= j;
```

### CPP10 · Replace inserts the new order before erasing through a stale pointer (hard)

- **Report:** Rare crash in itch-fh on the busiest days only, in OrderMap::erase_slot; never reproduces on the test day.
- **Cause:** insert() may rehash, which moves every slot. o still points into the old storage, so erase_found() computes a slot index from freed memory. With the map pre-sized, rehash only happens past 70% load.
- **Fix:** Erase first (as before), or re-find the original after the insert. Add a test with a tiny OrderMap that forces a rehash inside a replace.
- **Regression test:** `BookBuilderTest.ReplaceDuringRehash` in `tests/book/test_book_builder.cpp`

`libs/book/src/book_builder.cpp`
```diff
-   orders_.erase_found(o);
-   if (LAB_UNLIKELY(!orders_.insert(m.new_order_reference_number,
-                                    Order{m.price, m.shares, original.locate, original.side}))) {
-     ++checks_.duplicate_order_reference;
-     return;
-   }
+   if (LAB_UNLIKELY(!orders_.insert(m.new_order_reference_number,
+                                    Order{m.price, m.shares, original.locate, original.side}))) {
+     ++checks_.duplicate_order_reference;
+     return;
+   }
+   orders_.erase_found(o);  // after the insert, so a failed insert keeps the original
```

## Market data

| ID | Level | Bug | Caught by | Priya's time | Her regression test |
|---|---|---|---|---|---|
| MD01 | easy | LULD tier boundary: $3.00 gets the 5% band | **Silent** | 6 min | `LuldTest.TierBoundaries` |
| MD02 | easy | Locked books counted as crossed | Unit tests | 5 min | `OrderBookTest.LockedIsNotCrossed` |
| MD03 | easy | Opening and closing cross prices swapped | Unit tests | 5 min | `BookBuilderTest.OpeningAndClosingCrossPrices` |
| MD04 | medium | Attributed adds (F) are no longer handled | Unit tests, Book digest, e2e live feed | 22 min | `BookBuilderTest.AttributedAddJoinsTheBook` |
| MD05 | medium | 48-bit timestamp decoded with the wrong shift | Unit tests, Book digest, e2e firm, e2e live feed, e2e FIX | 18 min | `EndianTest.Load48AboveFourSeconds` |
| MD06 | medium | Halt reopening midpoint not rounded to the cent | **Silent** | 30 min | `BookBuilderTest.EmptyHaltCrossRoundsMidToCent` |
| MD07 | medium | Trader reads the tracking number as the stock locate | **Silent** | 16 min | `TraderMarketData.MapsLocateNotTrackingNumber` |
| MD08 | hard | LULD reference follows every trade | **Silent** | 55 min | `BookBuilderTest.LuldReferenceIsTheOpenNotTheLastTrade` |
| MD09 | hard | Repeated stock directory message wipes the book | **Silent** | 65 min | `BookBuilderTest.RepeatedDirectoryKeepsTheBook` |
| MD10 | hard | Order Replace shares and price offsets swapped in the field list | Unit tests, Book digest | 110 min | `ItchCodecTest.GoldenOrderReplace` |

### MD01 · LULD tier boundary: $3.00 gets the 5% band (easy)

- **Report:** Vendor LULD bands for stocks referenced at exactly $3.00 are 15 cents wide; the SIP shows 60 cents.
- **Cause:** The plan gives 5% only above $3.00. At exactly $3.00 the reference is in the 20% tier.
- **Fix:** Strictly greater than 30,000 in Price(4); add a table test at 2.99, 3.00 and 3.01.
- **Regression test:** `LuldTest.TierBoundaries` in `tests/book/test_luld.cpp`

`libs/book/include/lab/book/luld.hpp`
```diff
-   if (reference > 30'000)
+   if (reference >= 30'000)
```

### MD02 · Locked books counted as crossed (easy)

- **Report:** Data quality report: crossed-book events up 40x overnight, locked count unchanged.
- **Cause:** crossed() now includes bid == ask, so every lock is also a cross.
- **Fix:** Crossed is strictly bid > ask; locked is equality.
- **Regression test:** `OrderBookTest.LockedIsNotCrossed` in `tests/book/test_order_book.cpp`
- **Unit tests:** 1 of 183 tests fail; OrderBook.DetectsLockedAndCrossed

`libs/book/include/lab/book/order_book.hpp`
```diff
-     return b != nullptr && a != nullptr && b->price > a->price;
+     return b != nullptr && a != nullptr && b->price >= a->price;
```

### MD03 · Opening and closing cross prices swapped (easy)

- **Report:** Research: official open and close in the daily summary are each other's values.
- **Cause:** The cross type letters were swapped when the lines were reordered.
- **Fix:** O is opening and C is closing (ITCH 5.0 section 1.10); test both.
- **Regression test:** `BookBuilderTest.OpeningAndClosingCrossPrices` in `tests/book/test_book_builder.cpp`
- **Unit tests:** 1 of 183 tests fail; BookBuilder.CountsTradesThatDoNotTouchTheBook

`libs/book/src/book_builder.cpp`
```diff
-     if (m.cross_type == 'O') inst->stats.opening_cross_price = m.cross_price;
-     if (m.cross_type == 'C') inst->stats.closing_cross_price = m.cross_price;
+     if (m.cross_type == 'C') inst->stats.opening_cross_price = m.cross_price;
+     if (m.cross_type == 'O') inst->stats.closing_cross_price = m.cross_price;
```

### MD04 · Attributed adds (F) are no longer handled (medium)

- **Report:** itch-dump on a real day: thousands of 'unknown order reference'; market-maker books are thin.
- **Cause:** With the F handler removed, dispatch falls back to HandlerBase's no-op, so attributed orders never reach the book and their later executions and cancels look unknown.
- **Fix:** Handle F exactly like A. Add a test that HandlerBase defaults are only used for types the builder does not need.
- **Regression test:** `BookBuilderTest.AttributedAddJoinsTheBook` in `tests/book/test_book_builder.cpp`
- **Unit tests:** 4 of 183 tests fail; BookBuilder.RebuildsSyntheticDayExactly; BookBuilder.SyntheticScenarioDayHasNoErrors; Pipeline.CleanFeedRebuildsTheSameBooks; Pipeline.LossyFeedRecoversEveryMessage
- **Book digest:** itch-dump exited 1; digest c6b245d5e92dfe1b (main d83084b06609fdf5); ERROR unknown order reference 59325 (main 0); INFO cross executions over its print 24 (main 27)
- **e2e live feed:** exit 1

`libs/book/include/lab/book/book_builder.hpp`
```diff
-   void on(const itch::AddOrderMpid& m);
```

`libs/book/src/book_builder.cpp`
```diff
- void BookBuilder::on(const itch::AddOrderMpid& m) {
-   add_order(m.header, m.order_reference_number, m.buy_sell_indicator, m.shares, m.stock, m.price);
- }
```

### MD05 · 48-bit timestamp decoded with the wrong shift (medium)

- **Report:** Timestamp regressions every few seconds all day; the latency report shows negative feed ages.
- **Cause:** The top 16 bits of the ITCH nanosecond timestamp must shift by 32. Shifted by 16 they overlap the low word, so time jumps back each time the low 32 bits wrap (every 4.3 s).
- **Fix:** Shift by 32, and test load_be48 against a known value above 2^32.
- **Regression test:** `EndianTest.Load48AboveFourSeconds` in `tests/core/test_endian.cpp`
- **Unit tests:** 9 of 183 tests fail; Endian.LoadsBigEndian; Endian.StoreRoundTrips; ItchCodec.DecodesAddOrderFromSpecLayout; ItchCodec.EveryMessageTypeRoundTrips
- **Book digest:** itch-dump exited 1; ERROR timestamp regressions 999931 (main 0)
- **e2e firm:** exit 1; Heartbeats   3, End of Session 20, send errors 0; FAIL: trader exited 1; FAIL: simex exited 1
- **e2e live feed:** exit 1
- **e2e FIX:** exit 1; session      new  cancel replace  accept  reject   fills    shares    cxld   rplcd cxl_rej  resent gapfill errors; Heartbeats   23, End of Session 20, send errors 0; FAIL: simex

`libs/core/include/lab/core/endian.hpp`
```diff
-   return (std::uint64_t{load_be16(b)} << 32) | load_be32(b + 2);
+   return (std::uint64_t{load_be16(b)} << 16) | load_be32(b + 2);
```

### MD06 · Halt reopening midpoint not rounded to the cent (medium)

- **Report:** After an empty halt cross, the vendors' LULD band differs from simex's by a fraction of a cent.
- **Cause:** The lab rule rounds the reference down to the cent before computing bands. Without it the mid can be sub-penny, and every band derived from it is too.
- **Fix:** Divide by 200 and multiply by 100 again; test a 10.005 midpoint.
- **Regression test:** `BookBuilderTest.EmptyHaltCrossRoundsMidToCent` in `tests/book/test_book_builder.cpp`

`libs/book/src/book_builder.cpp`
```diff
-             static_cast<std::uint32_t>((std::uint64_t{b->price} + a->price) / 200 * 100);
+             static_cast<std::uint32_t>((std::uint64_t{b->price} + a->price) / 2);
```

### MD07 · Trader reads the tracking number as the stock locate (medium)

- **Report:** Trader starts, logs in, and never quotes: 'tops in 0'.
- **Cause:** Bytes 1-2 are Stock Locate and 3-4 are Tracking Number. The synthetic feed's tracking numbers are 0, so every message maps to locate 0, which nobody trades.
- **Fix:** Read the locate at offset 1 through the codec (decode the header) instead of a hand-written offset.
- **Regression test:** `TraderMarketData.MapsLocateNotTrackingNumber` in `tests/firm/test_market_data.cpp`

`firm/trader/main.cpp`
```diff
-     const std::uint16_t locate = lab::load_be16(msg.data() + 1);
+     const std::uint16_t locate = lab::load_be16(msg.data() + 3);
```

### MD08 · LULD reference follows every trade (hard)

- **Report:** A stock ran 30% in ten minutes and no vendor showed a limit state; the bands moved with it.
- **Cause:** The reference should be the opening or reopening cross (else the first trade). Updating it on every print makes the band chase the price, so it can never be reached.
- **Fix:** Set it only from a printing cross or the day's first trade, and test a steady climb past the band.
- **Regression test:** `BookBuilderTest.LuldReferenceIsTheOpenNotTheLastTrade` in `tests/book/test_book_builder.cpp`

`libs/book/include/lab/book/book_builder.hpp`
```diff
-     if (inst.luld_reference == 0) inst.luld_reference = price;
+     inst.luld_reference = price;  // latest print
```

### MD09 · Repeated stock directory message wipes the book (hard)

- **Report:** One morning a few symbols showed empty books until the next add; the full-day replay finished with book inconsistencies.
- **Cause:** Nasdaq can resend R messages intraday. Clearing the book there leaves the order store holding live orders whose levels are gone, so later cancels and executions cannot find them.
- **Fix:** A directory message updates reference data only; never touch the book or the orders.
- **Regression test:** `BookBuilderTest.RepeatedDirectoryKeepsTheBook` in `tests/book/test_book_builder.cpp`

`libs/book/src/book_builder.cpp`
```diff
-   Instrument& inst = instruments_[m.header.stock_locate];
-   inst.listed = true;
+   Instrument& inst = instruments_[m.header.stock_locate];
+   inst.book = OrderBook{};  // start clean
+   inst.listed = true;
```

### MD10 · Order Replace shares and price offsets swapped in the field list (hard)

- **Report:** Synthetic days are clean. On the real Nasdaq sample, replaced orders land at absurd prices and books cross.
- **Cause:** Encode and decode share the field list, so round-trip tests and the synthetic generator agree with each other. Only a spec-derived golden message shows the swap, and the layout check passes because both fields are 4 bytes.
- **Fix:** Restore the spec offsets (shares at 27, price at 31) and add golden-byte tests per message type from the spec PDF.
- **Regression test:** `ItchCodecTest.GoldenOrderReplace` in `tests/itch/test_codec.cpp`
- **Unit tests:** 1 of 183 tests fail; ItchCodec.DecodesOrderReplaceFromSpecLayout
- **Book digest:** digest cdca4bc4e094b543 (main d83084b06609fdf5); INFO books became crossed 116 (main 0); INFO books became locked 2 (main 0)

`libs/itch/include/lab/itch/messages.hpp`
```diff
-     f(27, m.shares);
-     f(31, m.price);
+     f(27, m.price);
+     f(31, m.shares);
```

## Network

| ID | Level | Bug | Caught by | Priya's time | Her regression test |
|---|---|---|---|---|---|
| NET01 | easy | Feed socket left at the kernel's default receive buffer | **Silent** | 10 min | `UdpTest.ReceiverGetsTheRequestedBuffer` |
| NET02 | easy | Multicast loopback disabled on the publisher | **Silent** | 8 min | `UdpTest.MulticastLoopbackOnOneHost` |
| NET03 | easy | Multicast test only recognises 224/8 | Unit tests | 9 min | `UdpTest.MulticastRange` |
| NET04 | medium | Partial TCP send re-queues the whole buffer | Unit tests | 32 min | `TcpTest.PartialSendQueuesOnlyTheTail` |
| NET05 | medium | recv() returning 0 treated as no data | Unit tests, e2e FIX | 24 min | `TcpTest.PeerCloseIsSeen` |
| NET06 | medium | Gap requests sent before the reorder delay | Unit tests | 26 min | `SequencerTest.ReorderWithinDelayIsNotRequested` |
| NET07 | medium | MoldUDP64 payload raised to the MTU | e2e firm, e2e live feed | 30 min | `FeedTest.PayloadFitsReceiverSlot` |
| NET08 | hard | Sequencer follows any session name | Unit tests | 60 min | `SequencerTest.OtherSessionIsIgnored` |
| NET09 | hard | Retransmit store loses its padding when it wraps | Unit tests | 105 min | `RetransmitStoreTest.EveryMessageAfterWrap` |
| NET10 | hard | Heartbeat advertises messages that are still being batched | **Silent** | 90 min | `PublisherTest.HeartbeatOnlyAdvertisesSentMessages` |

### NET01 · Feed socket left at the kernel's default receive buffer (easy)

- **Report:** Gap requests spike at the open and on every news burst; quiet periods are clean.
- **Cause:** Without SO_RCVBUF the socket has about 200 KB, a few milliseconds of the open at full rate. Bursts overflow it and the kernel drops datagrams (netstat -su: receive buffer errors).
- **Fix:** Set SO_RCVBUF again, and have lab-check report net.core.rmem_max, which caps it.
- **Regression test:** `UdpTest.ReceiverGetsTheRequestedBuffer` in `tests/net/test_udp.cpp`

`libs/net/src/udp.cpp`
```diff
-   set_int(s.fd_, SOL_SOCKET, SO_RCVBUF, rcvbuf_bytes, "SO_RCVBUF");
-   // Binding the group
+   // Binding the group
```

### NET02 · Multicast loopback disabled on the publisher (easy)

- **Report:** On the lab box (one host, multicast group) itch-fh receives nothing. CI is green.
- **Cause:** With loopback off, a multicast sender's packets are not delivered to receivers on the same host. CI uses a unicast address, so it never notices.
- **Fix:** Turn loopback back on for the single-host lab, and add a multicast case to the e2e scripts.
- **Regression test:** `UdpTest.MulticastLoopbackOnOneHost` in `tests/net/test_udp.cpp`

`libs/net/src/udp.cpp`
```diff
-     set_int(s.fd_, IPPROTO_IP, IP_MULTICAST_LOOP, 1, "IP_MULTICAST_LOOP");
+     set_int(s.fd_, IPPROTO_IP, IP_MULTICAST_LOOP, 0, "IP_MULTICAST_LOOP");
```

### NET03 · Multicast test only recognises 224/8 (easy)

- **Report:** Moving the feed to 239.1.1.1 (admin-scoped) gives silence; 224.0.1.1 works.
- **Cause:** Multicast is 224.0.0.0/4 (224 to 239). With a /8 mask, 239.x is treated as unicast, so the receiver never joins the group.
- **Fix:** Mask with 0xF0000000; test 224.0.0.1, 239.255.255.255 and 240.0.0.0.
- **Regression test:** `UdpTest.MulticastRange` in `tests/net/test_udp.cpp`
- **Unit tests:** 1 of 183 tests fail; Udp.ParsesEndpoints

`libs/net/src/udp.cpp`
```diff
-   return (ntohl(addr.sin_addr.s_addr) & 0xF0000000u) == 0xE0000000u;  // 224.0.0.0/4
+   return (ntohl(addr.sin_addr.s_addr) & 0xFF000000u) == 0xE0000000u;  // 224.0.0.0/8
```

### NET04 · Partial TCP send re-queues the whole buffer (medium)

- **Report:** Under heavy order flow simex occasionally logs 'malformed SoupBinTCP' from a client and drops it.
- **Cause:** When send() takes only part of a packet, the code queues the whole packet again instead of the unsent tail, so the bytes already sent go out twice and the framing breaks.
- **Fix:** Queue data[sent..]; test with a tiny SO_SNDBUF and a slow reader.
- **Regression test:** `TcpTest.PartialSendQueuesOnlyTheTail` in `tests/net/test_tcp.cpp`
- **Unit tests:** 1 of 183 tests fail; Tcp.QueuesWhatTheKernelDoesNotTake

`libs/net/src/tcp.cpp`
```diff
-   if (sent < data.size()) out_.insert(out_.end(), data.begin() + sent, data.end());
+   if (sent < data.size()) out_.insert(out_.end(), data.begin(), data.end());
```

### NET05 · recv() returning 0 treated as no data (medium)

- **Report:** The trader crashed; its quotes stayed live on simex for 15 seconds until the idle timeout.
- **Cause:** recv() returns 0 when the peer closes. Treating 0 as 'nothing yet' hides the close, so cancel-on-disconnect only fires when the idle timer gives up on the session.
- **Fix:** 0 means orderly close: mark the stream failed. Test that a closed peer is seen on the next recv.
- **Regression test:** `TcpTest.PeerCloseIsSeen` in `tests/net/test_tcp.cpp`
- **Unit tests:** 2 of 183 tests fail; Tcp.ReportsPeerClose; FixSession.ResendsWhatWasMissedWhileDisconnected
- **e2e FIX:** exit 1; Heartbeats   27, End of Session 20, send errors 0; FAIL: fix-client

`libs/net/src/tcp.cpp`
```diff
-   if (n > 0) return n;
-   if (n < 0 && (errno
+   if (n >= 0) return n;
+   if (n < 0 && (errno
```

### NET06 · Gap requests sent before the reorder delay (medium)

- **Report:** The re-request server answers ten times more requests than last week, nearly all for packets that arrived anyway.
- **Cause:** Mild reordering on the new switch opens a gap for a few microseconds. Without the gap_delay wait, every reorder triggers a retransmission request.
- **Fix:** Wait gap_delay before the first request, as before.
- **Regression test:** `SequencerTest.ReorderWithinDelayIsNotRequested` in `tests/mold/test_sequencer.cpp`
- **Unit tests:** 1 of 183 tests fail; Sequencer.ReorderedPacketFillsTheGapWithoutARequest

`libs/mold/include/lab/mold/sequencer.hpp`
```diff
-     const bool due = progress || (!requested_ && now - gap_opened_ >= cfg_.gap_delay) ||
+     const bool due = progress || !requested_ ||
```

### NET07 · MoldUDP64 payload raised to the MTU (medium)

- **Report:** After the 'fewer packets' change, itch-fh reports thousands of oversize packets and the book digest no longer matches.
- **Cause:** 1500 is the IP MTU, not the UDP payload; IPv4 and UDP headers take 28 bytes. Full packets are fragmented on the wire, and the receiver's 1472-byte slots drop them as oversize, so gaps open on every busy packet.
- **Fix:** Keep the payload at 1400, or derive it from the MTU minus 28 and size the receive slots from the same constant.
- **Regression test:** `FeedTest.PayloadFitsReceiverSlot` in `tests/mold/test_pipeline.cpp`
- **e2e firm:** exit 1; Trader checks: FAILED; Heartbeats   3, End of Session 20, send errors 0; FAIL: trader exited 1
- **e2e live feed:** exit 1; Heartbeats   1, End of Session 20, send errors 0; Requests     1197 sent, 0 send errors, 0 ring full; FAIL: itch-fh exited 3

`libs/mold/include/lab/mold/mold.hpp`
```diff
- inline constexpr std::size_t kDefaultMaxPayload = 1400;
+ inline constexpr std::size_t kDefaultMaxPayload = 1500;  // full Ethernet MTU
```

### NET08 · Sequencer follows any session name (hard)

- **Report:** After a simex restart mid-day, itch-fh kept running with no errors and a frozen book.
- **Cause:** The new session restarts at sequence 1. Following it silently keeps the old expected sequence, so every new message is older than expected and dropped as a duplicate.
- **Fix:** Latch the first session and count packets from any other; a new session needs an explicit restart.
- **Regression test:** `SequencerTest.OtherSessionIsIgnored` in `tests/mold/test_sequencer.cpp`
- **Unit tests:** 1 of 183 tests fail; Sequencer.IgnoresOtherSessionsAndMalformedPackets

`libs/mold/include/lab/mold/sequencer.hpp`
```diff
-     if (LAB_UNLIKELY(!session_known_)) {
-       session_ = p.header.session;
-       session_known_ = true;
-     } else if (LAB_UNLIKELY(p.header.session != session_)) {
-       ++stats_.wrong_session;
-       return PacketResult::kWrongSession;
-     }
+     if (LAB_UNLIKELY(p.header.session != session_)) {
+       session_ = p.header.session;  // follow the publisher across restarts
+       session_known_ = true;
+     }
```

### NET09 · Retransmit store loses its padding when it wraps (hard)

- **Report:** Hours into a lossy session, the book digest diverges after a retransmission; the resent messages have the right sequence numbers and the wrong contents.
- **Cause:** After the first wrap the stored start positions no longer match where the bytes were written, so get() returns bytes from another message. It only shows once the store has wrapped and a gap is filled from it.
- **Fix:** Count the padding in written_ again; test a wrap followed by get() of every stored message.
- **Regression test:** `RetransmitStoreTest.EveryMessageAfterWrap` in `tests/mold/test_retransmit_store.cpp`
- **Unit tests:** 1 of 183 tests fail; RetransmitStore.ForgetsMessagesWhoseBytesWereOverwritten

`libs/mold/include/lab/mold/retransmit_store.hpp`
```diff
-     if (at + msg.size() > size) {  // pad to the start rather than split
-       written_ += size - at;
-       at = 0;
-     }
+     if (at + msg.size() > size) {  // pad to the start rather than split
+       at = 0;
+     }
```

### NET10 · Heartbeat advertises messages that are still being batched (hard)

- **Report:** On quiet afternoons itch-fh occasionally gives a gap up and loses a few messages, then reports duplicates.
- **Cause:** The store already holds messages added to the open packet. A heartbeat sent before that packet is flushed announces them, receivers open a gap and ask for messages the publisher cannot serve yet, give up, then discard the real packet as duplicates.
- **Fix:** Heartbeats carry the next sequence actually sent (sent_next_).
- **Regression test:** `PublisherTest.HeartbeatOnlyAdvertisesSentMessages` in `tests/mold/test_pipeline.cpp`

`libs/feed/src/publisher.cpp`
```diff
-     hb.heartbeat(session_, sent_next_);
+     hb.heartbeat(session_, store_.next_sequence());
```

## Latency

| ID | Level | Bug | Caught by | Priya's time | Her regression test |
|---|---|---|---|---|---|
| LAT01 | easy | TCP_NODELAY no longer set | Unit tests | 8 min | `TcpTest.NoDelayIsSet` |
| LAT02 | easy | Receive loop sleeps even when busy | **Silent** | 10 min | `ReceiverTest.NoSleepWhileBusy` |
| LAT03 | easy | Debug print left on the trading thread | **Silent** | 6 min | `TradingTest.NoSyscallOnFill` |
| LAT04 | medium | Fill log grows on the hot thread | **Silent** | 22 min | `TradingTest.FillLogNeverReallocates` |
| LAT05 | medium | Shared memory pages no longer pre-faulted | **Silent** | 25 min | `ShmTest.PagesArePopulated` |
| LAT06 | medium | Publisher batching delay in milliseconds, not microseconds | **Silent** | 20 min | `PublisherTest.MaxDelayIsMicroseconds` |
| LAT07 | medium | Price level lookup scans from the worst level | **Silent** | 30 min | `bench: BM_ItchBookRebuild before/after` |
| LAT08 | hard | Ring consumer reloads the producer index every call | **Silent** | 60 min | `bench: BM_RingCrossThreadThroughput pinned` |
| LAT09 | hard | Tick timestamp taken after the book update | **Silent** | 75 min | `TraderMarketData.TopCarriesPacketArrivalTick` |
| LAT10 | hard | Matching engine key index moved off huge pages | **Silent** | 65 min | `bench: match_bench with perf stat dTLB` |

### LAT01 · TCP_NODELAY no longer set (easy)

- **Report:** Order round trip went from 20 microseconds to 40 milliseconds at p99 after the networking refactor.
- **Cause:** Without TCP_NODELAY, Nagle holds small OUCH packets until the previous one is acknowledged, and delayed ACK on the other side adds up to 40 ms.
- **Fix:** Set TCP_NODELAY on every stream, and check it in lab-check.
- **Regression test:** `TcpTest.NoDelayIsSet` in `tests/net/test_tcp.cpp`
- **Unit tests:** 2 of 183 tests fail; FixSession.FillsAGapWithResendAndGapFill; FixSession.ResendsWhatWasMissedWhileDisconnected

`libs/net/src/tcp.cpp`
```diff
-   out_.reserve(1 << 16);
-   set_nodelay(fd_);
+   out_.reserve(1 << 16);
```

### LAT02 · Receive loop sleeps even when busy (easy)

- **Report:** itch-fh 'ring full' and gap counts climb at the open; CPU use of feed-rx dropped to 20%.
- **Cause:** The receive thread now sleeps after every batch, not only after an empty one, so it stops draining the socket for idle_sleep_us at a time.
- **Fix:** Sleep only when the batch was empty.
- **Regression test:** `ReceiverTest.NoSleepWhileBusy` in `tests/mold/test_pipeline.cpp`

`libs/feed/src/receiver.cpp`
```diff
-     if (idle && cfg_.idle_sleep_us != 0) {
+     if (cfg_.idle_sleep_us != 0) {
```

### LAT03 · Debug print left on the trading thread (easy)

- **Report:** Decide-to-wire p99.9 jumps from microseconds to milliseconds whenever we get filled.
- **Cause:** fprintf to stderr is a write() syscall, plus formatting and a lock, on the hot thread. When the terminal or pipe is slow it blocks.
- **Fix:** Log fills through lab::LogChannel (or the fill log that already exists).
- **Regression test:** `TradingTest.NoSyscallOnFill` in `tests/firm/test_trading.cpp`

`firm/trader/trading.cpp`
```diff
-   const auto applied = oms_.apply(r);
-   if (applied.order == nullptr) return;
+   const auto applied = oms_.apply(r);
+   if (applied.fill) std::fprintf(stderr, "fill %u @ %u\n", r.qty, r.price);
+   if (applied.order == nullptr) return;
```

### LAT04 · Fill log grows on the hot thread (medium)

- **Report:** With --fills-csv on long runs, every few minutes the trader freezes for tens of milliseconds.
- **Cause:** The capacity check kept push_back from reallocating. Without it the vector doubles past its reservation, copying the whole log and faulting new pages on the trading thread.
- **Fix:** Keep the bound (drop and count past capacity), and size the reservation for the session.
- **Regression test:** `TradingTest.FillLogNeverReallocates` in `tests/firm/test_trading.cpp`

`firm/trader/trading.cpp`
```diff
-   if (applied.fill && fills_.size() < fills_.capacity()) {
+   if (applied.fill) {
```

### LAT05 · Shared memory pages no longer pre-faulted (medium)

- **Report:** The first 30 seconds after start have p99.9 ten times worse than the rest of the day.
- **Cause:** Without MAP_POPULATE each 4 KB page of a ring faults on first touch, on the hot path. Once every page has been touched the spikes stop.
- **Fix:** Populate (or touch) every page at startup; measure the first second separately in the latency report.
- **Regression test:** `ShmTest.PagesArePopulated` in `tests/core/test_shm.cpp`

`libs/core/src/shm.cpp`
```diff
-   if (opts.populate) flags |= MAP_POPULATE;
```

### LAT06 · Publisher batching delay in milliseconds, not microseconds (medium)

- **Report:** Feed age is fine at the open and terrible in the lunch lull: messages arrive in bursts tens of milliseconds late.
- **Cause:** A half-full packet waits max_delay_us before it is sent. Multiplying by a million turns microseconds into milliseconds, which only matters when traffic is too light to fill a packet.
- **Fix:** Use std::chrono durations for the config so the unit is in the type.
- **Regression test:** `PublisherTest.MaxDelayIsMicroseconds` in `tests/mold/test_pipeline.cpp`

`libs/feed/src/publisher.cpp`
```diff
-   if (!builder_.empty() && now - opened_ns_ > cfg_.max_delay_us * 1000) flush(now);
+   if (!builder_.empty() && now - opened_ns_ > cfg_.max_delay_us * 1000000) flush(now);
```

`libs/feed/src/publisher.cpp`
```diff
-   if (now - opened_ns_ > cfg_.max_delay_us * 1000) flush(now);
+   if (now - opened_ns_ > cfg_.max_delay_us * 1000000) flush(now);
```

### LAT07 · Price level lookup scans from the worst level (medium)

- **Report:** The book rebuild benchmark is down sharply and itch-fh falls behind at the open on deep symbols.
- **Cause:** Levels are stored worst to best and nearly all activity is near the touch. Scanning from the front walks the whole deep side for every reduce.
- **Fix:** Scan from the back with the early exit, and keep the before/after numbers from itch_bench in the PR.
- **Regression test:** `bench: BM_ItchBookRebuild before/after` in `bench/bm_itch.cpp`

`libs/book/include/lab/book/order_book.hpp`
```diff
-     for (std::size_t i = levels_.size(); i-- > 0;) {
-       if (levels_[i].price == price) return i;
-       if (better(price, levels_[i].price)) return kNone;  // walked past where it would be
-     }
+     for (std::size_t i = 0; i < levels_.size(); ++i) {
+       if (levels_[i].price == price) return i;
+     }
```

### LAT08 · Ring consumer reloads the producer index every call (hard)

- **Report:** On the colo box, cross-core ring throughput fell by a third; nothing changes on a laptop with unpinned threads.
- **Cause:** The cached copy of head_ means the consumer reads the producer's cache line only when it runs out. Reading it every call makes the line bounce between cores on every message.
- **Fix:** Restore the cached read, and benchmark with both threads pinned to different physical cores.
- **Regression test:** `bench: BM_RingCrossThreadThroughput pinned` in `bench/bm_core.cpp`

`libs/core/include/lab/core/spsc_ring.hpp`
```diff
-     if (LAB_UNLIKELY(tail == cached_head_)) {
-       cached_head_ = head_.load(std::memory_order_acquire);
-       if (tail == cached_head_) return nullptr;
-     }
+     cached_head_ = head_.load(std::memory_order_acquire);
+     if (tail == cached_head_) return nullptr;
```

### LAT09 · Tick timestamp taken after the book update (hard)

- **Report:** Last week's tick-to-trade report looked better than ever, but fill rates fell and colo p99 rose.
- **Cause:** rx_tsc should be when the packet arrived. Stamping it after the book update leaves the ring queueing and the book rebuild out of tick-to-wire, so the number improves while the real latency grows.
- **Fix:** Carry the packet's arrival tick through, and report queueing as its own histogram.
- **Regression test:** `TraderMarketData.TopCarriesPacketArrivalTick` in `tests/firm/test_market_data.cpp`

`firm/trader/main.cpp`
```diff
-     t.rx_tsc = rx_tsc;
+     t.rx_tsc = lab::rdtsc();  // when the top changed
```

### LAT10 · Matching engine key index moved off huge pages (hard)

- **Report:** simex matcher p99 is up 30% with no code change in the engine itself.
- **Cause:** The key index is random-access over up to 128 MB. With 4 KB pages most lookups miss the TLB; on 2 MB pages they rarely do. perf stat shows dTLB-load-misses up by an order of magnitude.
- **Fix:** Keep the huge page allocator, and add dTLB misses to the benchmark output.
- **Regression test:** `bench: match_bench with perf stat dTLB` in `bench/bm_match.cpp`

`libs/match/include/lab/match/key_index.hpp`
```diff
-   std::vector<Slot, HugePageAllocator<Slot>> slots_;  // random access over up to 128 MB
+   std::vector<Slot> slots_;
```

## Trading core

| ID | Level | Bug | Caught by | Priya's time | Her regression test |
|---|---|---|---|---|---|
| TRD01 | easy | Maker improves the spread one tick later than configured | **Silent** | 6 min | `MarketMakerTest.ImprovesAtExactlyThreshold` |
| TRD02 | easy | Inventory skew leans the wrong way | Unit tests | 8 min | `MarketMakerTest.LongPositionLowersBothQuotes` |
| TRD03 | easy | Position cap ignores the size of the new quote | Unit tests | 7 min | `MarketMakerTest.CapIncludesQuoteSize` |
| TRD04 | medium | Cash booked at our limit price, not the fill price | Unit tests | 28 min | `OmsTest.CashUsesExecutionPrice` |
| TRD05 | medium | Price collar computed in tenths of a percent | Unit tests | 18 min | `RiskTest.CollarIsBasisPoints` |
| TRD06 | medium | Replace confirmation updates only the bid's reference | e2e firm | 30 min | `TradingTest.ReplaceUpdatesAskRef` |
| TRD07 | medium | New orders join the front of their price level | Unit tests | 22 min | `EngineTest.SamePriceFillsInArrivalOrder` |
| TRD08 | hard | Replace for no more than the executed shares leaves a ghost order | Unit tests | 85 min | `OmsTest.ReplaceBelowExecutedAbandonsReplacement` |
| TRD09 | hard | Auction price ignores the reference on ties | **Silent** | 70 min | `EngineTest.IndicativeTieGoesToReference` |
| TRD10 | hard | Self-match guard uses the less aggressive price of a replace in flight | e2e firm | 100 min | `TradingTest.NoSelfTradeWhileBidReplaceInFlight` |

### TRD01 · Maker improves the spread one tick later than configured (easy)

- **Report:** We stopped stepping inside 3-cent spreads; fill share in wide names is down.
- **Cause:** The threshold is 'at least' improve_spread_ticks. Strictly greater skips the 3-tick case.
- **Fix:** Use >=, as the parameter's comment says; test a spread of exactly 3 ticks.
- **Regression test:** `MarketMakerTest.ImprovesAtExactlyThreshold` in `tests/firm/test_market_maker.cpp`

`firm/strategy/include/lab/strategy/market_maker.hpp`
```diff
-     if (static_cast<std::uint32_t>((m.ask - m.bid) / p_.tick) >= p_.improve_spread_ticks) {
+     if (static_cast<std::uint32_t>((m.ask - m.bid) / p_.tick) > p_.improve_spread_ticks) {
```

### TRD02 · Inventory skew leans the wrong way (easy)

- **Report:** Positions run to the cap and stay there; the maker buys more when it is already long.
- **Cause:** A long position should lower both quotes so we sell more and buy less. Adding the lean raises them, so fills push the position further out.
- **Fix:** Subtract the lean; test that a long position lowers both prices.
- **Regression test:** `MarketMakerTest.LongPositionLowersBothQuotes` in `tests/firm/test_market_maker.cpp`
- **Unit tests:** 1 of 183 tests fail; MarketMaker.LeansAgainstPositionWithoutCrossing

`firm/strategy/include/lab/strategy/market_maker.hpp`
```diff
-     bid -= lean;
-     ask -= lean;
+     bid += lean;
+     ask += lean;
```

### TRD03 · Position cap ignores the size of the new quote (easy)

- **Report:** The maker reaches 1,100 shares long with a 1,000-share cap.
- **Cause:** The check must include the shares the quote could add. Comparing the current position alone lets one more full quote through at the cap.
- **Fix:** Check position + size, as the ask side already does.
- **Regression test:** `MarketMakerTest.CapIncludesQuoteSize` in `tests/firm/test_market_maker.cpp`
- **Unit tests:** 1 of 183 tests fail; MarketMaker.LeansAgainstPositionWithoutCrossing

`firm/strategy/include/lab/strategy/market_maker.hpp`
```diff
-     if (position + size <= p_.max_position) {
+     if (position <= p_.max_position) {
```

### TRD04 · Cash booked at our limit price, not the fill price (medium)

- **Report:** Firm PnL differs from the drop copy every day, by more on auction days.
- **Cause:** Fills can print at a better price than our limit (price improvement, crosses). Using the order's own price books the wrong cash, so PnL is off by the improvement.
- **Fix:** Book cash at the execution price; reconcile against the drop copy in the test.
- **Regression test:** `OmsTest.CashUsesExecutionPrice` in `tests/firm/test_oms.cpp`
- **Unit tests:** 1 of 183 tests fail; Oms.EnterAcceptFillTracksPositionCashAndExposure

`firm/oms/include/lab/oms/oms.hpp`
```diff
-     const auto notional = q * static_cast<std::int64_t>(r.price);
+     const auto notional = q * static_cast<std::int64_t>(o->price);
```

### TRD05 · Price collar computed in tenths of a percent (medium)

- **Report:** A fat-finger order 30% through the market passed pre-trade risk.
- **Cause:** Basis points are per 10,000. Dividing by 1,000 makes a 500 bps (5%) collar into 50%.
- **Fix:** Divide by 10,000; test 4.9% passes and 5.1% fails.
- **Regression test:** `RiskTest.CollarIsBasisPoints` in `tests/firm/test_risk.cpp`
- **Unit tests:** 1 of 183 tests fail; Risk.OrderSizeNotionalAndCollar

`firm/risk/include/lab/risk/risk.hpp`
```diff
-     const std::uint64_t band = static_cast<std::uint64_t>(reference) * limits_.collar_bps / 10'000;
+     const std::uint64_t band = static_cast<std::uint64_t>(reference) * limits_.collar_bps / 1'000;
```

### TRD06 · Replace confirmation updates only the bid's reference (medium)

- **Report:** At shutdown: 'orders still working after stop', and offers we no longer track keep trading.
- **Cause:** After a replace the live order has the new UserRefNum. The ask side keeps the old one, which is done, so the next reconcile enters a fresh offer while the replaced one stays on the book.
- **Fix:** Update whichever side the replaced order belongs to; test a replace of each side.
- **Regression test:** `TradingTest.ReplaceUpdatesAskRef` in `tests/firm/test_trading.cpp`
- **e2e firm:** exit 1; Trader checks: FAILED; Heartbeats   2, End of Session 20, send errors 0; FAIL: trader exited 1

`firm/trader/trading.cpp`
```diff
-     if (in.ask_ref == r.orig_ref) in.ask_ref = r.user_ref;
```

### TRD07 · New orders join the front of their price level (medium)

- **Report:** Simulation: our queue position never matters; the newest order at a price always trades first.
- **Cause:** link() now pushes at the head of the queue, so simex runs price-LIFO instead of price-time priority.
- **Fix:** Append at the tail; test that two orders at one price fill in arrival order.
- **Regression test:** `EngineTest.SamePriceFillsInArrivalOrder` in `tests/match/test_engine.cpp`
- **Unit tests:** 3 of 183 tests fail; EngineTest.MatchesAtTheRestingPriceInTimePriority; EngineTest.ReplaceLosesPriorityAndCanTrade; EngineFuzz.MatchesAReferenceModel

`libs/match/include/lab/match/engine.hpp`
```diff
-     x.prev = lv.tail;
-     x.next = kNil;
-     if (lv.tail != kNil)
-       nodes_[lv.tail].next = n;
-     else
-       lv.head = n;
-     lv.tail = n;
+     x.prev = kNil;
+     x.next = lv.head;
+     if (lv.head != kNil)
+       nodes_[lv.head].prev = n;
+     else
+       lv.tail = n;
+     lv.head = n;
```

### TRD08 · Replace for no more than the executed shares leaves a ghost order (hard)

- **Report:** Once or twice a day an instrument stops quoting with 'position limit' while flat, and stop reports one order still working.
- **Cause:** When a replace asks for no more than what has already executed, the exchange cancels the original instead. The pending replacement is never answered, so it stays working with its exposure counted.
- **Fix:** Abandon the replacement on that user cancel; test a replace down to the executed quantity.
- **Regression test:** `OmsTest.ReplaceBelowExecutedAbandonsReplacement` in `tests/firm/test_oms.cpp`
- **Unit tests:** 1 of 183 tests fail; Oms.ReplaceForNoMoreThanExecutedIsAnsweredByCancel

`firm/oms/include/lab/oms/oms.hpp`
```diff
-     if (r.flag == 'U' && o->replacement != 0 && !o->cancel_pending) abandon_replacement(*o);
```

### TRD09 · Auction price ignores the reference on ties (hard)

- **Report:** simex opening prints sit at the bottom of the indicative range; Nasdaq's are near the previous close.
- **Cause:** When several prices pair the same shares with the same imbalance, the rule picks the one nearest the reference. Dropping the distance test always takes the lowest.
- **Fix:** Restore the reference tie-break; test a flat range of equal candidates.
- **Regression test:** `EngineTest.IndicativeTieGoesToReference` in `tests/match/test_engine.cpp`

`libs/match/include/lab/match/engine.hpp`
```diff
-             (imb == best_imb && (dist < best_dist || (dist == best_dist && p < out.price)))));
+             (imb == best_imb && p < out.price)));
```

### TRD10 · Self-match guard uses the less aggressive price of a replace in flight (hard)

- **Report:** Compliance: a few self-trades a week, always within a millisecond of a replace.
- **Cause:** While a replace is in flight the order may trade at either price, so the guard must use the more aggressive one. Taking the less aggressive one lets the other side cross into the new price.
- **Fix:** Use the more aggressive price for each side; test with a bid replace in flight.
- **Regression test:** `TradingTest.NoSelfTradeWhileBidReplaceInFlight` in `tests/firm/test_trading.cpp`
- **e2e firm:** exit 1; Trader checks: FAILED; Heartbeats   3, End of Session 20, send errors 0; FAIL: trader exited 1

`firm/trader/trading.cpp`
```diff
-     return side == 'B' ? std::max(px, n->price) : std::min(px, n->price);
+     return side == 'B' ? std::min(px, n->price) : std::max(px, n->price);
```

## Order gateway

| ID | Level | Bug | Caught by | Priya's time | Her regression test |
|---|---|---|---|---|---|
| GW01 | easy | Login no longer checks the password | **Silent** | 5 min | `GatewayTest.WrongPasswordRejected` |
| GW02 | easy | Sell short exempt rejected as an invalid side | **Silent** | 6 min | `GatewayTest.EverySideCode` |
| GW03 | easy | Drop copy passes short-sale side codes through | **Silent** | 12 min | `GatewayTest.DropCopySideIsBuyOrSell` |
| GW04 | medium | Duplicate UserRefNum accepted | **Silent** | 20 min | `GatewayTest.RepeatedUserRefNumIgnored` |
| GW05 | medium | Reconnect with sequence 0 replays the last message | **Silent** | 26 min | `GatewayTest.ReconnectWithZeroSendsOnlyNew` |
| GW06 | medium | Client heartbeat interval longer than the server's idle timeout | **Silent** | 24 min | `OrderEntryTest.HeartbeatWithinIdleTimeout` |
| GW07 | medium | FIX checksum skips BeginString and BodyLength | Unit tests, e2e FIX | 15 min | `FixCodecTest.ChecksumMatchesSpecExample` |
| GW08 | hard | Gap fill points one sequence number short | Unit tests | 60 min | `FixSessionTest.GapFillNewSeqNoIsNextAfterGap` |
| GW09 | hard | Possible duplicates delivered to the application | **Silent** | 70 min | `FixSessionTest.PossDupBelowExpectedNotDelivered` |
| GW10 | hard | Partial cancel forgets the order's details | **Silent** | 80 min | `GatewayTest.ReduceThenReplace` |

### GW01 · Login no longer checks the password (easy)

- **Report:** Security review: a session logged in as FIRM02 with a mistyped password.
- **Cause:** The password comparison was dropped from the login match, so any password is accepted for a known user.
- **Fix:** Match user and password; test a wrong password gets Login Rejected 'A'.
- **Regression test:** `GatewayTest.WrongPasswordRejected` in `tests/exchange/test_gateway.cpp`

`exchange/simex/gateway.cpp`
```diff
-       if (same_ignoring_case(req->user, a.cfg.user) &&
-           same_ignoring_case(req->password, a.cfg.password)) {
+       if (same_ignoring_case(req->user, a.cfg.user)) {
```

### GW02 · Sell short exempt rejected as an invalid side (easy)

- **Report:** Every short-exempt order from the desk is rejected with reason 0x0009.
- **Cause:** OUCH side E (sell short exempt) fell out of the switch and hits the default, which rejects it as an invalid side.
- **Fix:** Map E to a sell like S and T; test every side code.
- **Regression test:** `GatewayTest.EverySideCode` in `tests/exchange/test_gateway.cpp`

`exchange/simex/gateway.cpp`
```diff
-         case ouch::side::kSellShortExempt:
```

### GW03 · Drop copy passes short-sale side codes through (easy)

- **Report:** Reconciliation shows short sales missing from the drop copy; the totals by side do not add up.
- **Cause:** The drop copy format has B or S only. Passing T and E through puts short sales in neither bucket.
- **Fix:** Map every sell code to S at the boundary.
- **Regression test:** `GatewayTest.DropCopySideIsBuyOrSell` in `tests/exchange/test_gateway.cpp`

`exchange/simex/gateway.cpp`
```diff
-   r.side = oi.side == ouch::side::kBuy ? 'B' : 'S';
+   r.side = oi.side;
```

### GW04 · Duplicate UserRefNum accepted (medium)

- **Report:** After a client resent its last order on reconnect, simex held two live orders with the same UserRefNum.
- **Cause:** OUCH requires strictly increasing UserRefNum and says a repeat is ignored. With < the most recent reference can be entered twice.
- **Fix:** Reject any ref that is not greater than the last one; test a resend of the last message.
- **Regression test:** `GatewayTest.RepeatedUserRefNumIgnored` in `tests/exchange/test_gateway.cpp`

`exchange/simex/gateway.cpp`
```diff
-       if (ref <= a.last_ref) {
+       if (ref < a.last_ref) {
```

### GW05 · Reconnect with sequence 0 replays the last message (medium)

- **Report:** After a reconnect the trader booked one fill twice; the exchange shows a single execution.
- **Cause:** SoupBinTCP sequence 0 means 'only new messages'. Starting at count resends the last message, which after a fill is that execution.
- **Fix:** Start at count + 1 for 0; test a reconnect after a fill.
- **Regression test:** `GatewayTest.ReconnectWithZeroSendsOnlyNew` in `tests/exchange/test_gateway.cpp`

`exchange/simex/gateway.cpp`
```diff
-       req->next_sequence == 0 ? count + 1 : std::min<std::uint64_t>(req->next_sequence, count + 1);
+       req->next_sequence == 0 ? count : std::min<std::uint64_t>(req->next_sequence, count + 1);
```

### GW06 · Client heartbeat interval longer than the server's idle timeout (medium)

- **Report:** During the lunch lull the trader's session is dropped and every quote is cancelled; busy days are fine.
- **Cause:** SoupBinTCP clients must send a heartbeat after a second of silence. Every 20 s is longer than simex's 15 s idle timeout, so a quiet trader looks dead.
- **Fix:** Heartbeat after one second idle, and derive both timeouts from one config.
- **Regression test:** `OrderEntryTest.HeartbeatWithinIdleTimeout` in `tests/firm/test_order_entry.cpp`

`firm/trader/order_entry.cpp`
```diff
-     if (!stats_.session_ended && lab::mono_ns() - last_tx_ns_ > 1'000'000'000) {
+     if (!stats_.session_ended && lab::mono_ns() - last_tx_ns_ > 20'000'000'000) {
```

### GW07 · FIX checksum skips BeginString and BodyLength (medium)

- **Report:** No FIX client can log on; their logs show every message from us dropped as garbled.
- **Cause:** CheckSum (10) covers every byte from 8= up to the delimiter before 10=. Starting at the body leaves out the 8= and 9= fields, so the sum is wrong on every message.
- **Fix:** Sum from the start of 8=; test against a message from the FIX spec's example.
- **Regression test:** `FixCodecTest.ChecksumMatchesSpecExample` in `tests/fix/test_codec.cpp`
- **Unit tests:** 9 of 183 tests fail; FixWriter.MatchesThePublishedExample; FixStreamParser.SplitsAStreamInAnyChunking; FixStreamParser.DropsGarbledMessagesAndResynchronizes; FixSession.LogsOnAndDeliversInOrder
- **e2e FIX:** exit 1; Heartbeats   31, End of Session 20, send errors 0; FAIL: fix-client

`libs/fix/include/lab/fix/codec.hpp`
```diff
-     for (std::size_t i = start; i < pos_; ++i) sum += static_cast<unsigned char>(buf_[i]);
+     for (std::size_t i = kHeadRoom; i < pos_; ++i) sum += static_cast<unsigned char>(buf_[i]);
```

### GW08 · Gap fill points one sequence number short (hard)

- **Report:** After any FIX reconnect, the session loops on ResendRequests until someone resets sequence numbers.
- **Cause:** NewSeqNo is the next sequence number after the gap. One short, the counterparty expects a number it never receives and asks again, forever.
- **Fix:** NewSeqNo = first number after the filled range; test a resend that mixes gap fills and stored messages.
- **Regression test:** `FixSessionTest.GapFillNewSeqNoIsNextAfterGap` in `tests/fix/test_session.cpp`
- **Unit tests:** 1 of 183 tests fail; FixSession.FillsAGapWithResendAndGapFill

`libs/fix/include/lab/fix/session.hpp`
```diff
-       w.add_uint(tag::kNewSeqNo, upto);
+       w.add_uint(tag::kNewSeqNo, upto - 1);
```

### GW09 · Possible duplicates delivered to the application (hard)

- **Report:** A client booked two fills for one execution after a resend; positions disagree with the exchange.
- **Cause:** A PossDupFlag message below the expected sequence was already processed. Handing it to the application relies on every handler deduplicating, and the order handler does not.
- **Fix:** Drop it at the session layer; test that a resent execution report reaches the app once.
- **Regression test:** `FixSessionTest.PossDupBelowExpectedNotDelivered` in `tests/fix/test_session.cpp`

`libs/fix/include/lab/fix/session.hpp`
```diff
-         ++stats_.duplicates_ignored;
-         return;
+         ++stats_.duplicates_ignored;
+         if (state_ == State::kActive) h.on_app(m);  // let the app dedup by ExecID
+         return;
```

### GW10 · Partial cancel forgets the order's details (hard)

- **Report:** Replaces are rejected with 0x0015 on orders that were partly cancelled, and their later fills have a blank ClOrdID.
- **Cause:** A cancel that reduces an order still leaves it live. Treating every cancel as final erases its ClOrdID, side and capacity, so a later replace cannot find the original.
- **Fix:** Forget an order only when nothing remains; test a reduce followed by a replace.
- **Regression test:** `GatewayTest.ReduceThenReplace` in `tests/exchange/test_gateway.cpp`

`exchange/simex/gateway.cpp`
```diff
-         n = ouch::encode(m, buf.data());
-         done = e.remaining == 0;
-         break;
-       }
-       case SessionEvent::kExecuted: {
+         n = ouch::encode(m, buf.data());
+         done = true;
+         break;
+       }
+       case SessionEvent::kExecuted: {
```

