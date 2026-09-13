#!/usr/bin/env python3
"""Build the Wardriving offline AP-location database (apdb.bin).

The device binary-searches this file on the SD card to turn scanned BSSIDs into
lat/lon without any internet access. Copy the result to:

    /ext/apps_data/wardriving/apdb.bin

Input is any CSV whose header names a BSSID column and latitude/longitude
columns. Supported aliases (case-insensitive):

    BSSID/MAC/NetID/Address  +  Latitude/CurrentLatitude/Lat/trilat
                             +  Longitude/CurrentLongitude/Lon/trilong

That covers WiGLE wardriving exports (MAC, CurrentLatitude, CurrentLongitude)
and WiGLE network dumps (netid, trilat, trilong).

Output format (little-endian):
    8  bytes  magic "WRAPDB1\\0"
    4  bytes  uint32 record count
    N  x 14   records sorted ascending by the 6 BSSID bytes:
              6 bytes  BSSID
              4 bytes  int32 latitude  * 1e7
              4 bytes  int32 longitude * 1e7

Duplicates are averaged. Rows with an unparseable BSSID or a (0, 0) coordinate
are skipped.

Usage:
    python tools/build_apdb.py input.csv apdb.bin
"""

import argparse
import csv
import struct
import sys

MAGIC = b"WRAPDB1\x00"
HEADER_SIZE = 16
RECORD_SIZE = 14
SCALE = 10_000_000

BSSID_KEYS = ("bssid", "mac", "netid", "address", "ap")
LAT_KEYS = ("latitude", "currentlatitude", "lat", "trilat", "y")
LON_KEYS = ("longitude", "currentlongitude", "lon", "lng", "trilong", "x")


def parse_bssid(text):
    raw = "".join(c for c in text if c in "0123456789abcdefABCDEF")
    if len(raw) != 12:
        return None
    return bytes.fromhex(raw)


def parse_float(text):
    try:
        return float(text)
    except (TypeError, ValueError):
        return None


def find_column(fieldnames, keys):
    for name in fieldnames:
        if name is None:
            continue
        normalized = name.strip().lower()
        if normalized in keys:
            return name
    return None


def load_rows(path):
    points = {}
    with open(path, "r", newline="", encoding="utf-8", errors="replace") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            raise SystemExit("error: input CSV has no header row")
        bssid_col = find_column(reader.fieldnames, BSSID_KEYS)
        lat_col = find_column(reader.fieldnames, LAT_KEYS)
        lon_col = find_column(reader.fieldnames, LON_KEYS)
        if not (bssid_col and lat_col and lon_col):
            raise SystemExit(
                "error: could not find BSSID/lat/lon columns in header: "
                + ", ".join(reader.fieldnames)
            )
        for row in reader:
            bssid = parse_bssid(row.get(bssid_col) or "")
            lat = parse_float(row.get(lat_col))
            lon = parse_float(row.get(lon_col))
            if bssid is None or lat is None or lon is None:
                continue
            if lat == 0.0 and lon == 0.0:
                continue
            if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
                continue
            if bssid in points:
                count, sum_lat, sum_lon = points[bssid]
                points[bssid] = (count + 1, sum_lat + lat, sum_lon + lon)
            else:
                points[bssid] = (1, lat, lon)
    return points


def write_db(points, path):
    records = []
    for bssid, (count, sum_lat, sum_lon) in points.items():
        lat = sum_lat / count
        lon = sum_lon / count
        records.append((bssid, round(lat * SCALE), round(lon * SCALE)))
    records.sort(key=lambda item: item[0])

    with open(path, "wb") as out:
        out.write(MAGIC)
        out.write(struct.pack("<I", len(records)))
        out.write(struct.pack("<I", 0))
        for bssid, lat_e7, lon_e7 in records:
            out.write(bssid)
            out.write(struct.pack("<ii", lat_e7, lon_e7))
    return len(records)


def main():
    parser = argparse.ArgumentParser(description="Build apdb.bin for Wardriving.")
    parser.add_argument("input", help="input CSV with BSSID/lat/lon columns")
    parser.add_argument("output", help="output apdb.bin path")
    args = parser.parse_args()

    points = load_rows(args.input)
    if not points:
        raise SystemExit("error: no usable rows found")
    count = write_db(points, args.output)
    print(f"wrote {count} APs to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
