# My notes on MD-414

I got the project to build after installing the packages in INTERVIEW.md.
`ctest` shows 14 failing tests. One of them is about the Soup stream parser
(`StreamParserReassemblesSplitPackets`), so I looked at `soup.hpp`.

The comment says the length counts the type byte, and the check compares
with `len`. I changed it to `len + 1` to count the type byte, but the test
still fails and I am not sure why.

I did not get to the other reports. I think report 3 means the risk check is
not working.
