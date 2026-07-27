# Contributing

Reports and patches are welcome — especially from anyone running a P-touch model
other than the PT-P710BT.

## Building

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The tests run headless and need `poppler-utils` for the geometry checks; without
it those checks are skipped rather than failed.

## Testing without a printer

Everything except the actual printing works without hardware:

```bash
./build/ptouch-studio print --pdf /tmp/label.pdf -w 12 "Test"
pdftoppm -r 254 -gray -png /tmp/label.pdf /tmp/label   # 10 pixels per millimetre
```

At 254 dpi one millimetre is ten pixels, which makes it easy to measure the result
against what the layout claims.

## Adding a printer model

Two things may need adjusting:

1. **Printable height.** The table in `src/Engine.cpp` holds page width and print
   dots per tape width. The values there are those of the PT-P700 series; other
   print heads differ. Brother's Raster Command Reference for your model has the
   numbers.
2. **Driver selection.** `findDriver()` in `src/Provision.cpp` matches the
   Bluetooth name against the models offered by `lpinfo -m`. If your device
   reports an unusual name, please include it in the issue.

A test print with `--frame` shows immediately whether the printable area is right.

## Style

- C++20, Qt 6, four spaces, no tabs.
- Comments explain *why*, not *what* — the code already says what it does.
- English throughout, in code, comments and interface.
- New behaviour comes with a test in `tests/EngineTests.cpp` where that is
  meaningful. Measurements are verified against a rasterised PDF, not asserted
  from the same arithmetic that produced them.

## Commits

One logical change per commit, present tense, first line under 72 characters.
