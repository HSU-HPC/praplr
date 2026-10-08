#!/usr/bin/env python3

import csv
import json
import math
import os
import sys
from pathlib import Path


def read_energy_measurements_csv(path):
    """
    Read an energy-HOST.csv file produced by the perf-based sampler.

    CSV format:

        timestamp_ms,energy-pkg_energy,energy-cores_energy,...
        1234567890,12345,6789,...

    The energy columns contain cumulative raw perf counter values.
    They are NOT Joules or microjoules.
    """
    filename = os.path.basename(path)
    if not filename.startswith("energy-") or not filename.endswith(".csv"):
        raise ValueError("Invalid energy CSV filename: " + path)
    hostname = filename[len("energy-") : -len(".csv")]
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
                    f"{path}:{row_number}: "
                    f"expected {len(header)} columns, "
                    f"got {len(row)}"
                )
            try:
                timestamp = int(row[0])
            except ValueError as e:
                raise ValueError(
                    f"{path}:{row_number}: " f"invalid timestamp: {row[0]!r}"
                ) from e
            values = []
            for value in row[1:]:
                value = value.strip()
                if value.lower() == "nan":
                    values.append(float("nan"))
                    continue
                try:
                    values.append(int(value))
                except ValueError as e:
                    raise ValueError(
                        f"{path}:{row_number}: "
                        f"invalid perf counter value: {value!r}"
                    ) from e
            rows.append((timestamp, values))
    return hostname, header, rows


def read_rapl_metadata(path):
    with open(path) as f:
        return json.load(f)


def counter_delta(previous, current):
    """
    Calculate the delta of two cumulative perf event values.

    IMPORTANT:

    The C++ sampler reads the cumulative value returned by
    perf_event_open()/read(). We therefore do NOT assume that
    the returned value has the hardware RAPL counter width.

    A decrease is treated as a counter reset/restart rather than
    guessing a hardware wrap.
    """
    if math.isnan(previous) or math.isnan(current):
        return float("nan")
    if current >= previous:
        return current - previous
    raise ValueError("perf counter decreased: " f"{previous} -> {current}")


def energy_scale_to_joules(metadata_domain):
    """
    Return Joules represented by one raw perf counter unit.

    Linux's power PMU normally exposes:

        unit = "J"
        scale = <floating-point scale>

    Therefore:

        energy_J = counter_delta * scale
    """
    scale = float(metadata_domain.get("scale", 1.0))
    unit = metadata_domain.get(
        "unit",
        "J",
    ).strip()
    if unit.lower() in (
        "j",
        "joule",
        "joules",
    ):
        return scale
    raise ValueError(f"Unsupported RAPL energy unit: {unit!r}. " "Expected Joules.")


def calculate_power(
    rows,
    domain_index,
    metadata_domain,
):
    """
    Derive instantaneous/interval-average power from
    cumulative perf counter readings.

    Returns:

        [
            (timestamp_ms, power_W),
            ...
        ]
    """
    scale = energy_scale_to_joules(metadata_domain)
    result = []
    for i in range(1, len(rows)):
        t0, e0 = rows[i - 1]
        t1, e1 = rows[i]
        dt_ms = t1 - t0
        if dt_ms <= 0:
            continue
        delta_raw = counter_delta(
            e0[domain_index],
            e1[domain_index],
        )
        if math.isnan(delta_raw):
            power_w = float("nan")
        else:
            delta_j = delta_raw * scale
            dt_s = dt_ms / 1000.0
            power_w = delta_j / dt_s
        result.append((t1, power_w))
    return result


def calculate_total_energy(
    rows,
    domain_index,
    metadata_domain,
):
    """
    Calculate total energy in Joules from cumulative
    perf counter readings.
    """
    scale = energy_scale_to_joules(metadata_domain)
    total_raw = 0.0

    for i in range(1, len(rows)):
        e0 = rows[i - 1][1][domain_index]
        e1 = rows[i][1][domain_index]
        delta = counter_delta(
            e0,
            e1,
        )
        if math.isnan(delta):
            continue
        total_raw += delta
    return total_raw * scale


def write_power_csv(
    path,
    energy_header,
    power_rows,
):
    """
    Write derived power measurements.

    Example:

        energy-pkg_energy
            ->
        energy-pkg_W
    """
    power_header = []
    for name in energy_header[1:]:
        suffix = "_energy"
        if name.endswith(suffix):
            power_header.append(name[: -len(suffix)] + "_W")
        else:
            power_header.append(name + "_W")
    with open(
        path,
        "w",
        newline="",
    ) as f:
        writer = csv.writer(f)
        writer.writerow(["timestamp_ms"] + power_header)
        for timestamp, values in power_rows:
            writer.writerow(
                [timestamp]
                + [("nan" if math.isnan(value) else f"{value:.6f}") for value in values]
            )


def fmt(value):
    return f"{value:.6f}"


def write_summary_yaml(
    path,
    results,
):
    """
    Write the same summary.yaml structure as the old
    sysfs-based postprocessor.
    """
    total_energy = sum(result["energy_j"] for result in results)
    start = min(result["start_ms"] for result in results)
    end = max(result["end_ms"] for result in results)
    duration = (end - start) / 1000.0
    average_power = total_energy / duration if duration > 0 else 0.0
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
            f.write("    domains:\n")
            for domain in result["domains"]:
                f.write("      - name: {}\n".format(domain["name"]))
                f.write(
                    "        average_power_W: {}\n".format(
                        fmt(domain["average_power_W"])
                    )
                )
                f.write("        energy_J: {}\n".format(fmt(domain["energy_j"])))
                f.write(
                    "        energy_kWh: {}\n".format(
                        fmt(domain["energy_j"] / 3600000.0)
                    )
                )
                # f.write(
                #    "        scale: {}\n".format(
                #        domain["scale"]
                #    )
                # )
                #
                # f.write(
                #    "        unit: {}\n".format(
                #        domain["unit"]
                #    )
                # )
                f.write("        sockets: {}\n".format(domain["sockets"]))
        f.write("\n")
        f.write("total:\n")
        f.write(f"  nodes: {len(results)}\n")
        f.write(f"  start_ms: {start}\n")
        f.write(f"  end_ms: {end}\n")
        f.write(f"  duration_s: " f"{fmt(duration)}\n")
        f.write(f"  average_power_W: " f"{fmt(average_power)}\n")
        f.write(f"  energy_J: " f"{fmt(total_energy)}\n")
        f.write(f"  energy_kWh: " f"{fmt(total_energy / 3600000.0)}\n")


def transpose_power_rows(
    domain_power_rows,
):
    """
    Convert:

        [
            [(t0, p0), (t1, p1)],
            [(t0, p0), (t1, p1)],
        ]

    into:

        [
            (t0, [p0, p0]),
            (t1, [p1, p1]),
        ]
    """
    if not domain_power_rows:
        return []
    row_count = len(domain_power_rows[0])
    result = []
    for i in range(row_count):
        timestamp = domain_power_rows[0][i][0]
        values = [
            domain_power_rows[domain_index][i][1]
            for domain_index in range(len(domain_power_rows))
        ]
        result.append((timestamp, values))
    return result


def validate_metadata_domain(
    energy_domain_name,
    metadata_domain,
):
    """
    Ensure the CSV domain and metadata domain still
    correspond to each other.

    C++ writes:

        domain.name + "_energy"

    to the CSV.
    """
    metadata_name = metadata_domain.get("name")
    if not metadata_name:
        raise ValueError("Metadata domain has no name")
    expected_name = metadata_name + "_energy"
    if energy_domain_name != expected_name:
        raise ValueError(
            "Domain mismatch: "
            f"CSV has {energy_domain_name!r}, "
            f"metadata expects {expected_name!r}"
        )


def process_host(
    energy_path,
    measurements_path,
):
    (
        hostname,
        energy_header,
        energy_rows,
    ) = read_energy_measurements_csv(str(energy_path))
    if len(energy_rows) < 2:
        print(
            f"Warning: {energy_path} contains " "fewer than 2 samples",
            file=sys.stderr,
        )
        return None
    metadata_path = measurements_path / f"rapl-{hostname}.json"
    if not metadata_path.exists():
        raise ValueError(f"Metadata file not found: " f"{metadata_path}")
    metadata = read_rapl_metadata(metadata_path)
    metadata_domains = metadata.get("domains")
    if metadata_domains is None:
        raise ValueError(f"{metadata_path}: missing 'domains'")
    domain_names = energy_header[1:]
    if len(metadata_domains) != len(domain_names):
        raise ValueError(
            f"{energy_path}: CSV has "
            f"{len(domain_names)} domains, "
            f"metadata has "
            f"{len(metadata_domains)} domains"
        )
    start_ms = energy_rows[0][0]
    end_ms = energy_rows[-1][0]
    duration_s = (end_ms - start_ms) / 1000.0
    domains = []
    domain_power_rows = []
    for domain_index, domain_name in enumerate(domain_names):
        metadata_domain = metadata_domains[domain_index]
        validate_metadata_domain(
            domain_name,
            metadata_domain,
        )
        scale = float(
            metadata_domain.get(
                "scale",
                1.0,
            )
        )
        unit = metadata_domain.get(
            "unit",
            "J",
        )
        if not math.isfinite(scale) or scale <= 0:
            raise ValueError(
                f"{metadata_path}: domain "
                f"{domain_name}: invalid scale "
                f"{scale!r}"
            )
        domain_power = calculate_power(
            energy_rows,
            domain_index,
            metadata_domain,
        )
        domain_power_rows.append(domain_power)
        energy_j = calculate_total_energy(
            energy_rows,
            domain_index,
            metadata_domain,
        )
        average_power = energy_j / duration_s if duration_s > 0 else 0.0
        domains.append(
            {
                "name": domain_name,
                "energy_j": energy_j,
                "average_power_W": average_power,
                "scale": scale,
                "unit": unit,
                "sockets": metadata_domain.get(
                    "sockets",
                    [],
                ),
            }
        )
    energy_j = sum(domain["energy_j"] for domain in domains)
    average_power = energy_j / duration_s if duration_s > 0 else 0.0
    power_path = measurements_path / f"power-{hostname}.csv"
    power_rows = transpose_power_rows(domain_power_rows)
    write_power_csv(
        str(power_path),
        energy_header,
        power_rows,
    )
    return {
        "hostname": hostname,
        "samples": len(energy_rows),
        "start_ms": start_ms,
        "end_ms": end_ms,
        "duration_s": duration_s,
        "average_power_W": average_power,
        "energy_j": energy_j,
        "power_file": power_path.name,
        "domains": domains,
    }


def main():
    if len(sys.argv) != 2:
        print(
            f"Usage: {sys.argv[0]} MEASUREMENTS_PATH",
            file=sys.stderr,
        )
        sys.exit(1)
    measurements_path = Path(sys.argv[1])
    if not measurements_path.is_dir():
        print(
            f"Not a directory: " f"{measurements_path}",
            file=sys.stderr,
        )
        sys.exit(1)
    files = sorted(measurements_path.glob("energy-*.csv"))
    if not files:
        print(
            f"No energy-*.csv files found in " f"{measurements_path}",
            file=sys.stderr,
        )
        sys.exit(1)
    results = []
    for energy_path in files:
        result = process_host(
            energy_path,
            measurements_path,
        )
        if result is not None:
            results.append(result)
    if not results:
        print(
            "No usable measurement files found.",
            file=sys.stderr,
        )
        sys.exit(1)
    summary_path = measurements_path / "summary.yaml"
    write_summary_yaml(
        str(summary_path),
        results,
    )
    print("#", summary_path)
    print(summary_path.read_text())


if __name__ == "__main__":
    main()
