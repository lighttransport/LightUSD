# Next USDC Crash Regression Inputs

This directory stores minimized binary inputs found by fuzzing. Files are saved
without the leading `PXR-USDC` magic; `test_crash_regressions` prepends it before
calling the next USDC reader.

Prefer generated cases in `tests/next/test_usdc_malformed.cc` when the malformed
shape is easy to describe: bad bootstrap, TOC bounds, section counts, table caps,
or truncated synthetic sections. Add a `.bin` input here only when it is a
minimized crash/UB reproducer whose byte layout is not worth rebuilding by hand.
