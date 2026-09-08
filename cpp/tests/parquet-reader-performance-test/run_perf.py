#!/usr/bin/env python3
"""
Performance testing script for Parquet reader with different configurations:
- 1 SSD vs 24 SSD configurations
- Direct I/O vs buffered I/O
- Data verification enabled/disabled
- Multiple thread count configurations

This script:
1. Tests both 1 SSD and 24 SSD configurations
2. Switches between direct I/O and buffered I/O modes
3. Tests with different thread counts (2, 4, 6, 8, 12, 24)
4. Generates CPU flamegraphs for detailed performance analysis
5. Summarizes performance data in a CSV file
"""

import subprocess
import os
import sys
import argparse
import shutil
import re
import csv
from pathlib import Path
from datetime import datetime

# --- Configuration (Defaults & Constants) ---
DEFAULT_THREADS = [2, 4, 6, 8, 12, 24]
DEFAULT_SSD_MODES = ["1ssd","6ssd","12ssd","24ssd"]
DEFAULT_IO_MODES = ["buffered"]

# FlameGraph script path
FLAMEGRAPH_DIR = os.path.expanduser("~/FlameGraph")
STACKCOLLAPSE = os.path.join(FLAMEGRAPH_DIR, "stackcollapse-perf.pl")
FLAMEGRAPH_PL = os.path.join(FLAMEGRAPH_DIR, "flamegraph.pl")

# Test scenarios with their configurations
TEST_SCENARIOS = {
    "direct": {
        "localfs.enable.direct.io": "true"
    },
    "buffered": {
        "localfs.enable.direct.io": "false"
    }
}


def check_dependencies():
    """Check if required tools are available."""
    # Check FlameGraph tools
    if not os.path.exists(STACKCOLLAPSE) or not os.path.exists(FLAMEGRAPH_PL):
        print("Warning: FlameGraph tools not found.")
        print(f"Please clone FlameGraph to {FLAMEGRAPH_DIR} for flamegraph generation")
        print("  git clone https://github.com/brendangregg/FlameGraph.git ~/FlameGraph")
        return False

    # Check if perf is available
    if shutil.which("perf") is None:
        print("Warning: 'perf' command not found. Flamegraphs will be skipped.")
        return False

    return True


def ensure_dir(directory):
    """Create directory if it doesn't exist."""
    Path(directory).mkdir(parents=True, exist_ok=True)


def modify_config_file(config_path, modifications):
    """
    Modify configuration parameters in a properties file.

    Args:
        config_path: Path to configuration file
        modifications: Dictionary of parameter names and their values
    """
    if not os.path.exists(config_path):
        raise FileNotFoundError(f"Configuration file not found: {config_path}")

    with open(config_path, "r") as f:
        lines = f.readlines()

    # Track which parameters have been found and modified
    modified_params = set()
    new_lines = []

    for line in lines:
        line_modified = False
        for param_name, param_value in modifications.items():
            if line.strip().startswith(param_name):
                new_lines.append(f"{param_name}={param_value}\n")
                modified_params.add(param_name)
                line_modified = True
                break

        if not line_modified:
            new_lines.append(line)

    # Add any parameters that weren't found in file
    for param_name, param_value in modifications.items():
        if param_name not in modified_params:
            new_lines.append(f"{param_name}={param_value}\n")

    with open(config_path, "w") as f:
        f.writelines(new_lines)


def clear_page_cache():
    """Clear page cache before running test."""
    try:
        print("  Clearing page cache...")
        subprocess.run(
            ["sudo", "sh", "-c", "echo 3 > /proc/sys/vm/drop_caches"],
            check=True,
            capture_output=True
        )
        print("  ✓ Page cache cleared")
    except subprocess.CalledProcessError as e:
        print(f"  ⚠ Warning: Could not clear page cache: {e}")


def run_test(binary, config_file, output_dir, test_name):
    """
    Run a single test with given configuration.

    Returns:
        Dictionary with test results or None if test failed
    """
    print(f"\n{'='*70}")
    print(f"Running test: {test_name}")
    print(f"{'='*70}")
    
    # Clear page cache before test
    clear_page_cache()

    # Run with perf stat - use same events as Pixels reader test
    stat_file = os.path.join(output_dir, f"{test_name}.stat.txt")
    output_file = os.path.join(output_dir, f"{test_name}.output.txt")

    cmd = [
        "sudo", "-E", "perf", "stat",
        "-e", "cycles,instructions,cache-references,cache-misses,branches,branch-misses",
        "-e", "page-faults,minor-faults,major-faults",
        "-e", "task-clock,context-switches",
        "-e", "stalled-cycles-backend,cpu-migrations",
        "-o", stat_file,
        binary, config_file
    ]

    print(f"Command: {' '.join(cmd)}")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)

        if result.returncode != 0:
            print(f"❌ Test failed with return code {result.returncode}")
            return None

        # Save program output
        with open(output_file, "w") as f:
            f.write(result.stdout)
            f.write(result.stderr)

        # Print output
        print(result.stdout)

        # Extract metrics - match Pixels reader format
        metrics = {
            "rows": 0,
            "row_groups": 0,
            "time_ms": 0,
            "throughput_mb_s": 0.0,
            "row_throughput": 0.0,
            "total_bytes_mb": 0.0
        }

        # Parse results from output
        output = result.stdout
        
        # Total rows read
        rows_match = re.search(r"Total rows read:\s+([\d,]+)", output)
        if rows_match:
            metrics["rows"] = int(rows_match.group(1).replace(',', ''))

        # Total row groups read
        rg_match = re.search(r"Total row groups read:\s+([\d,]+)", output)
        if rg_match:
            metrics["row_groups"] = int(rg_match.group(1).replace(',', ''))

        # Elapsed time
        time_match = re.search(r"Elapsed time:\s+([\d,]+)\s+ms", output)
        if time_match:
            metrics["time_ms"] = int(time_match.group(1).replace(',', ''))

        # Throughput in MB/s
        throughput_match = re.search(r"Throughput:\s+([\d.]+)\s+MB/s", output)
        if throughput_match:
            metrics["throughput_mb_s"] = float(throughput_match.group(1))

        # Row throughput
        row_throughput_match = re.search(r"Row throughput:\s+([\d.]+)\s+rows/sec", output)
        if row_throughput_match:
            metrics["row_throughput"] = float(row_throughput_match.group(1))

        # Total bytes
        bytes_match = re.search(r"Total bytes read:\s+([\d.]+)\s+MB", output)
        if bytes_match:
            metrics["total_bytes_mb"] = float(bytes_match.group(1))

        print(f"✅ Test completed: {metrics['throughput_mb_s']:.2f} MB/s, {metrics['row_throughput']:.0f} rows/sec")
        return metrics

    except subprocess.TimeoutExpired:
        print(f"❌ Test timed out after 1 hour")
        return None
    except Exception as e:
        print(f"❌ Test failed with error: {e}")
        return None


def run_perf_record(binary, config_file, output_dir, test_name):
    """
    Run test with perf record for flamegraph generation.

    Returns:
        Path to perf.data file or None if failed
    """
    print(f"\n🔥 Recording performance data for flamegraph: {test_name}")
    
    # Clear page cache before recording
    clear_page_cache()

    perf_data = os.path.join(output_dir, f"{test_name}.perf.data")

    cmd = [
        "sudo", "-E", "perf", "record",
        "-F", "99",
        "-g",
        "--call-graph=dwarf",
        "-o", perf_data,
        binary, config_file
    ]

    try:
        result = subprocess.run(cmd, capture_output=True, timeout=3600)
        if result.returncode == 0 and os.path.exists(perf_data):
            print(f"✅ Performance data recorded: {perf_data}")
            return perf_data
        else:
            print(f"❌ Failed to record performance data")
            return None
    except Exception as e:
        print(f"❌ perf record failed: {e}")
        return None


def generate_flamegraph(perf_data, output_dir, test_name):
    """Generate flamegraph from perf data."""
    print(f"🔥 Generating flamegraph for: {test_name}")

    perf_txt = os.path.join(output_dir, f"{test_name}.perf.txt")
    folded_file = os.path.join(output_dir, f"{test_name}.folded")
    svg_file = os.path.join(output_dir, f"{test_name}.svg")

    try:
        # Convert perf.data to text
        print("  Converting perf data to text...")
        with open(perf_txt, 'w') as f:
            subprocess.run(["sudo", "perf", "script", "-i", perf_data], 
                         stdout=f, check=True)
        
        # Collapse stacks
        print("  Collapsing stacks...")
        with open(perf_txt, 'r') as f_in, open(folded_file, 'w') as f_out:
            subprocess.run([STACKCOLLAPSE], stdin=f_in, stdout=f_out, check=True)
        
        # Generate flame graph
        print("  Generating SVG flame graph...")
        with open(folded_file, 'r') as f_in, open(svg_file, 'w') as f_out:
            subprocess.run([
                FLAMEGRAPH_PL,
                "--title", f"{test_name} CPU Hotspots",
                "--countname", "samples",
                "--color", "hot"
            ], stdin=f_in, stdout=f_out, check=True)
        
        print(f"✅ Flamegraph generated: {svg_file}")
        
        # Cleanup intermediate files
        os.remove(perf_txt)
        os.remove(folded_file)
        subprocess.run(["sudo", "rm", "-f", perf_data], check=True)
        
        return True
    except subprocess.CalledProcessError as e:
        print(f"❌ Error generating flame graph: {e}")
        return False


def summarize_results(results, output_dir):
    """Generate CSV summary of all test results."""
    csv_file = os.path.join(output_dir, "performance_summary.csv")

    print(f"\n{'='*60}")
    print("Creating performance summary...")
    print(f"{'='*60}")
    
    with open(csv_file, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow([
            'SSD Mode', 'I/O Mode', 'Threads',
            'Total Rows', 'Total Row Groups', 'Total MB', 
            'Time (ms)', 'Throughput (MB/s)', 'Throughput (rows/sec)'
        ])
        
        for result in results:
            writer.writerow([
                result.get('ssd_mode', 'unknown'),
                result.get('io_mode', 'unknown'),
                result.get('threads', 0),
                result.get('rows', 0),
                result.get('row_groups', 0),
                f"{result.get('total_bytes_mb', 0.0):.2f}",
                result.get('time_ms', 0),
                f"{result.get('throughput_mb_s', 0.0):.2f}",
                f"{result.get('row_throughput', 0.0):.0f}"
            ])
    
    print(f"✅ Performance summary saved to: {csv_file}")
    
    # Print summary table
    print(f"\n{'='*90}")
    print("Performance Summary")
    print(f"{'='*90}")
    print(f"{'SSD':<8} {'I/O':<10} {'Threads':<8} {'Time(ms)':<10} {'MB/s':<12} {'Rows/sec':<15}")
    print("-" * 90)
    
    for result in results:
        print(f"{result.get('ssd_mode', 'unknown'):<8} "
              f"{result.get('io_mode', 'unknown'):<10} "
              f"{result.get('threads', 0):<8} "
              f"{result.get('time_ms', 0):<10} "
              f"{result.get('throughput_mb_s', 0.0):<12.2f} "
              f"{result.get('row_throughput', 0.0):<15.0f}")


def main():
    parser = argparse.ArgumentParser(
        description="Parquet Reader Performance Testing Script"
    )
    parser.add_argument(
        "--binary",
        default="../../build/release/extension/pixels/tests/parquet-reader-performance-test/parquet_reader_performance_test",
        help="Path to test binary"
    )
    parser.add_argument(
        "--config-dir",
        default="./",
        help="Directory containing config files"
    )
    parser.add_argument(
        "--output-dir",
        default="./tests/parquet-reader-performance-test/perf-results",
        help="Directory to store results"
    )
    parser.add_argument(
        "--ssd-modes",
        nargs="+",
        default=DEFAULT_SSD_MODES,
        choices=["1ssd", "6ssd", "12ssd", "24ssd"],
        help="SSD configurations to test"
    )
    parser.add_argument(
        "--io-modes",
        nargs="+",
        default=DEFAULT_IO_MODES,
        choices=["direct", "buffered"],
        help="I/O modes to test"
    )
    parser.add_argument(
        "--threads",
        type=int,
        nargs='+',
        default=DEFAULT_THREADS,
        help=f"List of thread counts to test (default: {DEFAULT_THREADS})"
    )
    parser.add_argument(
        "--skip-flamegraph",
        action='store_true',
        help="Skip flamegraph generation"
    )
    parser.add_argument(
        "--verify-data",
        action='store_true',
        help="Enable data verification"
    )

    args = parser.parse_args()

    # Check dependencies
    has_flamegraph = check_dependencies()
    if not has_flamegraph:
        args.skip_flamegraph = True

    # Ensure output directory exists
    ensure_dir(args.output_dir)

    # Check if binary exists
    if not os.path.exists(args.binary):
        print(f"Error: Binary not found: {args.binary}")
        print("Please build project first:")
        print("  mkdir -p build && cd build")
        print("  cmake .. && make parquet_reader_performance_test")
        return 1

    print("=" * 70)
    print("Parquet Reader Performance Testing")
    print("=" * 70)
    print(f"Binary: {args.binary}")
    print(f"Config directory: {args.config_dir}")
    print(f"Output directory: {args.output_dir}")
    print(f"SSD modes: {args.ssd_modes}")
    print(f"I/O modes: {args.io_modes}")
    print(f"Threads: {args.threads}")
    print(f"Flamegraphs: {'disabled' if args.skip_flamegraph else 'enabled'}")
    print(f"Data verification: {'enabled' if args.verify_data else 'disabled'}")
    print("=" * 70)

    all_results = []

    # Run tests for each configuration
    for ssd_mode in args.ssd_modes:
        # Determine config file
        if ssd_mode == "6ssd":
            base_config = os.path.join(args.config_dir, "config-6ssd.properties")
        elif ssd_mode == "12ssd":
            base_config = os.path.join(args.config_dir, "config-12ssd.properties")
        elif ssd_mode == "24ssd":
            base_config = os.path.join(args.config_dir, "config-24ssd.properties")
        else:
            base_config = os.path.join(args.config_dir, "config.properties")

        if not os.path.exists(base_config):
            print(f"Warning: Config file not found: {base_config}, skipping {ssd_mode}")
            continue

        for io_mode in args.io_modes:
            for threads in args.threads:
                test_name = f"{ssd_mode}_{io_mode}-threads{threads}"

                # Create temporary config with modifications
                temp_config = os.path.join(args.output_dir, f"{test_name}.properties")
                shutil.copy(base_config, temp_config)

                # Apply I/O mode settings and thread count
                modifications = TEST_SCENARIOS[io_mode].copy()
                modifications["test.threads"] = str(threads)
                if args.verify_data:
                    modifications["test.verify.data"] = "true"

                modify_config_file(temp_config, modifications)

                print(f"\n🔧 Configuration for {test_name}:")
                for key, value in modifications.items():
                    print(f"   → {key}={value}")

                # Run test
                metrics = run_test(args.binary, temp_config, args.output_dir, test_name)
                if metrics:
                    # Add metadata
                    metrics['ssd_mode'] = ssd_mode
                    metrics['io_mode'] = io_mode
                    metrics['threads'] = threads
                    all_results.append(metrics)

                    # Generate flame graph
                    if not args.skip_flamegraph:
                        data_file = run_perf_record(
                            args.binary, temp_config, args.output_dir, test_name
                        )
                        if data_file:
                            generate_flamegraph(data_file, args.output_dir, test_name)

                # Cleanup temp config
                if os.path.exists(temp_config):
                    os.remove(temp_config)

    # Generate summary
    if all_results:
        summarize_results(all_results, args.output_dir)

    print("\n" + "=" * 70)
    print("🎉 All tests completed!")
    print("=" * 70)
    print(f"\nResults saved to: {args.output_dir}/")
    print("\nGenerated files:")
    print("  - *.stat.txt: perf stat statistics")
    print("  - *.output.txt: program output")
    if not args.skip_flamegraph:
        print("  - *.svg: CPU flame graphs")
    print("  - performance_summary.csv: performance summary")
    print("\nNext steps:")
    print("  1. Analyze CPU flamegraphs to identify performance bottlenecks")
    print("  2. Compare performance across different thread counts")
    print("  3. Compare direct I/O vs buffered I/O performance")
    print("  4. Compare 1 SSD vs 24 SSD performance")
    print("  5. Review performance_summary.csv for overall trends")

    return 0


if __name__ == "__main__":
    sys.exit(main())