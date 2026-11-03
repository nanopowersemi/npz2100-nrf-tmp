#!/usr/bin/env python3
"""Build, flash and test every sample that has a ci/test.py.

Discovers samples/*/ci/test.py, and for each one: builds the sample for
--board, opens the serial port, flashes, captures the log for the time the
test asks for, then runs the test's check() on it.

Every test in the sample's sample.yaml is a variant tested on the bench,
with the same ci/test.py. The first is the default, named after the sample
(sensor_bme280); the others are named after the last part of their test
name (sample.npz2100.sensor_bme280.shadow -> sensor_bme280.shadow) and are
built with their extra_configs added.

Usage (from an NCS environment, e.g. `nrfutil sdk-manager toolchain launch
--ncs-version v3.3.4 -- python scripts/ci/run_samples.py ...`):

    python scripts/ci/run_samples.py --board nrf52dk/nrf52832 --port /dev/ttyACM0
    python scripts/ci/run_samples.py --board ... --port ... --ad3
    python scripts/ci/run_samples.py --board ... --port ad3:4 --ad3
    python scripts/ci/run_samples.py --board ... --port ad3:4 --ad3 --i2c
    python scripts/ci/run_samples.py --board ... --port ... --sample global_counter_alarm
    python scripts/ci/run_samples.py --board ... --port ... --sample sensor_bme280.shadow
    python scripts/ci/run_samples.py --board ... --build-only

With --ad3, each sample is tested from a clean start: the bench is
power-cycled through the Analog Discovery 3 supply with the nRF held in
reset (so the nPZ2100 gets a power-on reset and stays in standby), the nRF
is erased, and only then is the sample flashed.

With --i2c (SDA on AD3 DIO6, SCL on DIO7; --i2c SDA,SCL for others), the AD3
logic analyzer also sniffs the bus during the capture: every transaction
is printed as [I2C] next to the log lines, decoded as a register read or
write where it can be, followed by a count of accesses per register.

Build coverage across all supported boards comes from sample.yaml and
twister instead (`west twister -T samples --integration`).
"""

import argparse
import importlib.util
import os
import sys
from collections import namedtuple

from ci_utils import (REPO_ROOT, SAMPLES_DIR, Log, ad3_clean_erase, build, common_checks,
                      flash, open_ad3, open_capture, parse_i2c_pins)


# name: what --sample selects and the results show; extra_configs: Kconfig
# lines added to the build.
Variant = namedtuple("Variant", "name test_path extra_configs")


def variants(test_path):
    """The bench variants of a sample: one per test in its sample.yaml."""
    sample = test_path.parents[1].name
    try:
        import yaml
        with open(test_path.parents[1] / "sample.yaml", encoding="utf-8") as f:
            spec = yaml.safe_load(f) or {}
    except (ImportError, OSError):
        spec = {}
    common = (spec.get("common") or {}).get("extra_configs") or []
    found = []
    for i, (key, test) in enumerate((spec.get("tests") or {}).items()):
        name = sample if i == 0 else f"{sample}.{key.rsplit('.', 1)[-1]}"
        found.append(Variant(name, test_path, list(common) + list((test or {}).get("extra_configs") or [])))
    return found or [Variant(sample, test_path, [])]


def discover(names=None):
    found = [v for t in sorted(SAMPLES_DIR.glob("*/ci/test.py")) for v in variants(t)]
    if names:
        unknown = set(names) - {v.name for v in found}
        if unknown:
            print(f"Unknown sample or variant: {', '.join(sorted(unknown))} "
                  f"(have: {', '.join(v.name for v in found)})", flush=True)
            sys.exit(1)
        found = [v for v in found if v.name in names]
    return found


def load(test_path):
    name = f"ci_test_{test_path.parents[1].name}"
    spec = importlib.util.spec_from_file_location(name, test_path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_sample(variant, args):
    test_path = variant.test_path
    sample_dir = test_path.parents[1]
    test = load(test_path)
    build_dir = REPO_ROOT / "build" / "ci" / variant.name

    build(sample_dir, args.board, build_dir, quiet=not args.verbose,
          extra_configs=variant.extra_configs)
    if args.build_only:
        return []

    # A test can be disabled for a while by setting SKIP in its ci/test.py
    # to the reason. It is still built, and the skip is flagged as a
    # warning on the GitHub run so it is not forgotten.
    skip = getattr(test, "SKIP", None)
    if skip and not args.no_skip:
        print(f"::warning title=Bench test disabled: {variant.name}::{skip} "
              f"Re-enable it by removing SKIP from samples/{sample_dir.name}/ci/test.py.",
              flush=True)
        return None

    # One AD3 session from the power cycle to the end of the capture, and
    # no rebuild before the flash: the nRF is only powered while the
    # nPZ2100 is in standby, so the flash follows the erase without a gap.
    ad3 = open_ad3() if args.ad3 or args.i2c else None
    try:
        if args.ad3:
            ad3_clean_erase(args.dev_id, ad3=ad3)

        capture = open_capture(args.port, ad3=ad3, i2c=args.i2c)
        try:
            flash(build_dir, args.dev_id, no_rebuild=True)
            print(f"Capturing log for {test.CAPTURE_S} s...", flush=True)
            capture.wait(test.CAPTURE_S)
        finally:
            capture.close()
    finally:
        if ad3:
            ad3.close()  # without a device reset, so the supply stays on

    log = Log(capture.lines, getattr(capture, "i2c", ()))
    common_checks(log)
    test.check(log)
    return log.failures


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--board", default=os.environ.get("NPZ_CI_BOARD"),
                   help="board target (env NPZ_CI_BOARD)")
    p.add_argument("--port", default=os.environ.get("NPZ_CI_PORT"),
                   help="serial port with the nRF log, or ad3:<DIO pin> to receive it on the AD3 (env NPZ_CI_PORT)")
    p.add_argument("--dev-id", default=os.environ.get("NPZ_CI_DEV_ID"),
                   help="J-Link serial number, if more than one is attached (env NPZ_CI_DEV_ID)")
    p.add_argument("--sample", action="append",
                   help="only run this sample, or variant such as sensor_bme280.shadow "
                        "(repeatable); default: all with ci/test.py, every variant")
    p.add_argument("--ad3", action="store_true", default=bool(os.environ.get("NPZ_CI_AD3")),
                   help="erase and power-cycle the bench with the AD3 before each sample (env NPZ_CI_AD3)")
    p.add_argument("--i2c", nargs="?", const="6,7", default=os.environ.get("NPZ_CI_AD3_I2C"),
                   metavar="SDA,SCL",
                   help="also sniff the I2C bus on these AD3 DIO pins (default 6,7) (env NPZ_CI_AD3_I2C)")
    p.add_argument("--verbose", action="store_true",
                   help="show the full build output (default: only on failure)")
    p.add_argument("--build-only", action="store_true", help="build, do not flash or test")
    p.add_argument("--no-skip", action="store_true",
                   help="also run tests that are disabled with SKIP in their ci/test.py")
    args = p.parse_args(argv)
    if not args.board:
        p.error("--board (or NPZ_CI_BOARD) is required")
    if not args.build_only and not args.port:
        p.error("--port (or NPZ_CI_PORT) is required unless --build-only")
    if args.i2c:
        try:
            args.i2c = parse_i2c_pins(args.i2c)
        except ValueError as e:
            p.error(f"--i2c: {e}")
    return args


def main(argv=None):
    args = parse_args(argv)
    tests = discover(args.sample)
    if not tests:
        print("No samples with ci/test.py found", flush=True)
        return 1

    results = {}
    samples = {}
    for variant in tests:
        name = variant.name
        samples[name] = variant.test_path.parents[1].name
        configs = f" ({', '.join(variant.extra_configs)})" if variant.extra_configs else ""
        print(f"\n#### {name}{configs} ####", flush=True)
        results[name] = run_sample(variant, args)

    print("\n==================== Summary ====================", flush=True)
    for name, failures in results.items():
        if failures is None:
            print(f"SKIP  {name}", flush=True)
            continue
        print(f"{'PASS' if not failures else 'FAIL'}  {name}", flush=True)
        for f in failures:
            print(f"      - {f}", flush=True)

    # Also list the results on the GitHub Actions run page.
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as f:
            for name, failures in results.items():
                if failures is None:
                    f.write(f"- ⚠️ **{name}**: bench test disabled, see SKIP in "
                            f"`samples/{samples[name]}/ci/test.py`\n")
                    continue
                f.write(f"- {'✅' if not failures else '❌'} **{name}**\n")
                for failure in failures:
                    f.write(f"  - `{failure}`\n")

    return 1 if any(results.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
