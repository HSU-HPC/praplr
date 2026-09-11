#!/usr/bin/env -S uv run

# /// script
# requires-python = ">=3.10"
# dependencies = [
#     "pandas",
#     "plotly",
# ]
# ///

import sys
from pathlib import Path

import pandas as pd
import plotly.graph_objects as go


def read_power_file(path):
    """
    Read power-HOST.csv
    """
    hostname = path.stem[len("power-") :]
    df = pd.read_csv(path)

    if "timestamp_ms" not in df.columns:
        raise ValueError(f"{path}: missing timestamp_ms column")

    return hostname, df


def main():
    if len(sys.argv) not in (2, 3):
        print(
            f"Usage: {sys.argv[0]} MEASUREMENTS_PATH [HTML_PATH]",
            file=sys.stderr,
        )
        sys.exit(1)
    measurements_path = Path(sys.argv[1])
    if len(sys.argv) == 3:
        html_path = Path(sys.argv[2])
    else:
        html_path = measurements_path / "power.html"
    power_files = sorted(measurements_path.glob("power-*.csv"))
    if not power_files:
        print(
            f"No power-*.csv files found in {measurements_path}",
            file=sys.stderr,
        )
        sys.exit(1)
    node_data = {}
    node_data = {}
    for power_path in power_files:
        hostname, df = read_power_file(power_path)
        if len(df) == 0:
            print(
                f"Warning: {power_path} contains no samples",
                file=sys.stderr,
            )
            continue
        node_data[hostname] = df
    if not node_data:
        print(
            "No usable power files found.",
            file=sys.stderr,
        )
        sys.exit(1)

    # Make timestamps relative
    start_timestamp_ms = min(df["timestamp_ms"].min() for df in node_data.values())
    for df in node_data.values():
        df["time_s"] = (df["timestamp_ms"] - start_timestamp_ms) / 1000.0
    fig = go.Figure()

    # Plot power per node
    node_totals = {}
    for hostname, df in node_data.items():
        domain_columns = [column for column in df.columns if column.endswith("_W")]
        if not domain_columns:
            continue
        total = df[domain_columns].sum(axis=1, skipna=True)
        node_totals[hostname] = pd.DataFrame(
            {
                "time_s": df["time_s"],
                "power_W": total,
            }
        )
        fig.add_trace(
            go.Scatter(
                x=df["time_s"],
                y=total,
                mode="lines",
                name=f"{hostname} total",
                legendgroup=hostname,
                hovertemplate=(
                    f"{hostname} total"
                    "<br>time: %{x:.3f} s"
                    "<br>power: %{y:.3f} W"
                    "<extra></extra>"
                ),
            )
        )

    # Plot individual domains
    for hostname, df in node_data.items():
        domain_columns = [column for column in df.columns if column.endswith("_W")]
        for column in domain_columns:
            fig.add_trace(
                go.Scatter(
                    x=df["time_s"],
                    y=df[column],
                    mode="lines",
                    name=f"{hostname} / {column}",
                    legendgroup=hostname + "_domains",
                    hovertemplate=(
                        f"{hostname} / {column}"
                        "<br>time: %{x:.3f} s"
                        "<br>power: %{y:.3f} W"
                        "<extra></extra>"
                    ),
                    visible="legendonly",
                )
            )

    # Plot total
    if node_totals:
        all_times = sorted(
            set().union(*[set(data["time_s"]) for data in node_totals.values()])
        )
        total = pd.Series(0.0, index=all_times)
        for data in node_totals.values():
            series = pd.Series(
                data["power_W"].values,
                index=data["time_s"],
            )
            # Interpolate onto the common timeline.
            series = series.reindex(all_times).interpolate(method="index")
            # Don't extrapolate outside the node's actual range.
            series = series.where(
                series.index.to_series().between(
                    data["time_s"].iloc[0],
                    data["time_s"].iloc[-1],
                )
            )
            total = total.add(
                series.fillna(0.0),
                fill_value=0.0,
            )
        fig.add_trace(
            go.Scatter(
                x=all_times,
                y=total,
                mode="lines",
                name="TOTAL",
                line={
                    "width": 3,
                    "color": "black",
                },
                hovertemplate=(
                    "TOTAL"
                    "<br>time: %{x:.3f} s"
                    "<br>power: %{y:.3f} W"
                    "<extra></extra>"
                ),
            )
        )

    # Update layout
    fig.update_layout(
        title="RAPL Power",
        xaxis={
            "title": "Time [s]",
            "rangeslider": {
                "visible": True,
            },
            "showgrid": True,
        },
        yaxis={
            "title": "Power [W]",
            "rangemode": "tozero",
            "showgrid": True,
        },
        hovermode="x unified",
        legend={
            "title": "Traces",
            "groupclick": "togglegroup",
        },
        template="ggplot2",
        margin={
            "l": 70,
            "r": 30,
            "t": 70,
            "b": 70,
        },
    )

    # Write standalone HTML
    fig.write_html(
        html_path,
        include_plotlyjs=True,
        auto_open=True,
    )
    print(f"Wrote {html_path}")


if __name__ == "__main__":
    main()
