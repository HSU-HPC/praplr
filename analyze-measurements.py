#!/usr/bin/env python3

import csv
import json
import math
import os
import sys
from pathlib import Path


def read_energy_measurements_csv(path):
    """
    Read a energy-HOST.csv file.
    The first column is "timestamp_ms" with subsequent RAPLZONE_energy_uj columns.
    """
    filename = os.path.basename(path)
    hostname = filename[7:-4]
    with open(path, newline="") as f:
        reader = csv.reader(f)
        try:
            header = next(reader)
        except StopIteration:
            raise ValueError("Empty file: " + path)
        if len(header) < 2 or header[0] != "timestamp_ms":
            raise ValueError("Invalid header in " + path)
        rows = []
        for row_number, row in enumerate(reader, start=2):
            if not row:
                continue
            if len(row) != len(header):
                raise ValueError(
                    f"{path}:{row_number}: expected {len(header)} columns, got {len(row)}"
                )
            timestamp = int(row[0])
            values = []
            for value in row[1:]:
                if value.lower() == "nan":
                    values.append(float("nan"))
                else:
                    values.append(float(value))
            rows.append((timestamp, values))
    return hostname, header, rows


def read_rapl_metadata(path):
    with open(path) as f:
        return json.load(f)


def counter_delta(previous, current, max_energy_range_uj):
    if math.isnan(previous) or math.isnan(current):
        return float("nan")
    if current >= previous:
        return current - previous
    # Counter wrapped.
    return (max_energy_range_uj - previous) + current


def calculate_power(rows, zone, max_energy_range_uj):
    """
    Derive power from cumulative energy readings.
    Returns list of timestamp-power values.
    """
    result = []
    for i in range(1, len(rows)):
        t0, e0 = rows[i - 1]
        t1, e1 = rows[i]
        dt_ms = t1 - t0
        if dt_ms <= 0:
            continue
        delta_uj = counter_delta(
            e0[zone],
            e1[zone],
            max_energy_range_uj,
        )
        if math.isnan(delta_uj):
            power_w = float("nan")
        else:
            power_w = delta_uj / dt_ms / 1000.0
        result.append((t1, power_w))
    return result


def calculate_total_energy(rows, zone, max_energy_range_uj):
    total_uj = 0.0
    for i in range(1, len(rows)):
        e0 = rows[i - 1][1][zone]
        e1 = rows[i][1][zone]
        delta = counter_delta(e0, e1, max_energy_range_uj)
        if math.isnan(delta):
            continue
        total_uj += delta
    return total_uj / 1_000_000.0


def write_power_csv(path, energy_header, power_rows):
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            ["timestamp_ms"]
            + [
                name[:-9] + "_W"
                for name in energy_header[1:]  # strips suffix "_energy_uj"
            ]
        )
        for timestamp, values in power_rows:
            writer.writerow(
                [timestamp]
                + ["nan" if math.isnan(value) else f"{value:.6f}" for value in values]
            )


def fmt(value):
    return f"{value:.6f}"


def write_summary_yaml(path, results):
    total_energy = sum(result["energy_j"] for result in results)
    job_start = min(result["start_ms"] for result in results)
    job_end = max(result["end_ms"] for result in results)
    job_duration = (job_end - job_start) / 1000.0
    job_average_power = total_energy / job_duration if job_duration > 0 else 0.0
    with open(path, "w") as f:
        f.write("nodes:\n")
        for result in results:
            f.write("  - hostname: {}\n".format(result["hostname"]))
            f.write("    samples: {}\n".format(result["samples"]))
            f.write("    start_ms: {}\n".format(result["start_ms"]))
            f.write("    end_ms: {}\n".format(result["end_ms"]))
            f.write("    duration_s: {}\n".format(fmt(result["duration_s"])))
            f.write("    average_power_W: {}\n".format(fmt(result["average_power_W"])))
            f.write("    energy_J: {}\n".format(fmt(result["energy_j"])))
            f.write("    energy_kWh: {}\n".format(fmt(result["energy_j"] / 3600000.0)))
            f.write("    power_file: {}\n".format(result["power_file"]))
            f.write("    zones:\n")
            for zone in result["zones"]:
                f.write("      - name: {}\n".format(zone["name"]))
                f.write(
                    "        average_power_W: {}\n".format(fmt(zone["average_power_W"]))
                )
                f.write("        energy_J: {}\n".format(fmt(zone["energy_j"])))
                f.write(
                    "        energy_kWh: {}\n".format(fmt(zone["energy_j"] / 3600000.0))
                )
        f.write("\n")
        f.write("job:\n")
        f.write(f"  nodes: {len(results)}\n")
        f.write(f"  start_ms: {job_start}\n")
        f.write(f"  end_ms: {job_end}\n")
        f.write(f"  duration_s: {fmt(job_duration)}\n")
        f.write(f"  average_power_W: {fmt(job_average_power)}\n")
        f.write(f"  energy_J: {fmt(total_energy)}\n")
        f.write(f"  energy_kWh: {fmt(total_energy / 3600000.0)}\n")


def transpose_power_rows(zone_power_rows):
    """
    Convert rows from list of timestamp-power pairs to pair of timestamp and list of power values
    """
    if not zone_power_rows:
        return []
    return [
        (
            zone_power_rows[0][i][0],
            [
                zone_power_rows[zone_index][i][1]
                for zone_index in range(len(zone_power_rows))
            ],
        )
        for i in range(len(zone_power_rows[0]))
    ]


def main():
    if len(sys.argv) != 2:
        print(
            f"Usage: {sys.argv[0]} OUTPUT_PATH",
            file=sys.stderr,
        )
        sys.exit(1)
    output_path = Path(sys.argv[1])
    files = sorted(output_path.glob("energy-*.csv"))
    if not files:
        print(
            f"No energy-*.csv files found in {output_path}",
            file=sys.stderr,
        )
        sys.exit(1)
    results = []
    # Process result for each host
    for energy_path in files:
        hostname, energy_header, energy_rows = read_energy_measurements_csv(energy_path)
        if len(energy_rows) < 2:
            print(
                f"Warning: {energy_path} contains fewer than 2 samples",
                file=sys.stderr,
            )
            continue
        zone_names = energy_header[1:]
        metadata_path = output_path / f"rapl-{hostname}.json"
        metadata = read_rapl_metadata(metadata_path)
        metadata_zones = metadata["zones"]
        if len(metadata_zones) != len(zone_names):
            raise ValueError(
                f"{energy_path}: CSV has {len(zone_names)} zones, "
                f"metadata has {len(metadata_zones)} zones"
            )
        zones = []
        zone_power_rows = []
        duration_s = (energy_rows[-1][0] - energy_rows[0][0]) / 1000.0
        # Calculate power and energy for each zone
        for zone_index, zone_name in enumerate(zone_names):
            metadata_zone = metadata_zones[zone_index]
            max_energy_range_uj = metadata_zone["max_energy_range_uj"]
            zone_power = calculate_power(
                energy_rows,
                zone_index,
                max_energy_range_uj,
            )
            zone_power_rows.append(zone_power)
            energy_j = calculate_total_energy(
                energy_rows,
                zone_index,
                max_energy_range_uj,
            )
            average_power = energy_j / duration_s if duration_s > 0 else 0.0
            zones.append(
                {
                    "name": zone_name,
                    "energy_j": energy_j,
                    "average_power_W": average_power,
                }
            )
        # Compute node statistics
        energy_j = sum(zone["energy_j"] for zone in zones)
        start_ms = energy_rows[0][0]
        end_ms = energy_rows[-1][0]
        duration_s = (end_ms - start_ms) / 1000.0
        average_power = energy_j / duration_s if duration_s > 0 else 0.0
        # Write power-HOST.csv
        power_path = output_path / f"power-{hostname}.csv"
        power_rows = transpose_power_rows(zone_power_rows)
        write_power_csv(
            power_path,
            energy_header,
            power_rows,
        )
        results.append(
            {
                "hostname": hostname,
                "samples": len(energy_rows),
                "start_ms": start_ms,
                "end_ms": end_ms,
                "duration_s": duration_s,
                "average_power_W": average_power,
                "energy_j": energy_j,
                "power_file": power_path.name,
                "zones": zones,
            }
        )
    if not results:
        print(
            "No usable measurement files found.",
            file=sys.stderr,
        )
        sys.exit(1)
    # Write summary
    summary_path = output_path / "summary.yaml"
    write_summary_yaml(
        summary_path,
        results,
    )
    print("#", summary_path)
    print(summary_path.read_text())


if __name__ == "__main__":
    main()
