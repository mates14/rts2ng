#!/usr/bin/env python3
"""
Simple GRB database query module for RTS2.
Uses psql command-line for simplicity and to avoid authentication issues.
Can be tested independently of RTS2.

Usage:
    from rts2.grb_query import get_grb_info

    info = get_grb_info(tar_id=12345)
    if info:
        print(f"Error box: {info['errorbox']} degrees")
"""

import subprocess
import sys
import os


def get_grb_info(tar_id=None, grb_id=None, database=None):
    """
    Query GRB information from RTS2 database.

    Args:
        tar_id: RTS2 target ID (preferred method)
        grb_id: GCN GRB ID (alternative method)
        database: Database name (default: from RTS2_DB env var or 'stars')

    Returns:
        Dictionary with keys: tar_id, grb_id, grb_seqn, grb_type, grb_ra, grb_dec,
                             grb_errorbox, grb_date, grb_last_update, grb_is_grb
        None if query fails or no results found
    """

    if tar_id is None and grb_id is None:
        print("ERROR: Must provide either tar_id or grb_id", file=sys.stderr)
        return None

    # Get database name from environment or use default
    if database is None:
        database = os.environ.get('RTS2_DB', 'stars')

    # Build SQL query
    if tar_id is not None:
        where_clause = f"tar_id = {int(tar_id)}"
    else:
        where_clause = f"grb_id = {int(grb_id)}"

    sql = f"""
    SELECT
        tar_id,
        grb_id,
        grb_seqn,
        grb_type,
        grb_ra,
        grb_dec,
        grb_errorbox,
        EXTRACT(EPOCH FROM grb_date) as grb_date,
        EXTRACT(EPOCH FROM grb_last_update) as grb_last_update,
        grb_is_grb
    FROM grb
    WHERE {where_clause}
    ORDER BY grb_last_update DESC
    LIMIT 1;
    """

    try:
        # Use psql with tuple-only output (-t) and field separator (|)
        result = subprocess.run(
            ['psql', '-d', database, '-t', '-A', '-F', '|', '-c', sql],
            capture_output=True,
            text=True,
            timeout=5
        )

        if result.returncode != 0:
            print(f"ERROR: psql failed: {result.stderr}", file=sys.stderr)
            return None

        # Parse output
        output = result.stdout.strip()
        if not output:
            print(f"WARNING: No GRB found for {where_clause}", file=sys.stderr)
            return None

        fields = output.split('|')
        if len(fields) < 10:
            print(f"ERROR: Unexpected query result: {output}", file=sys.stderr)
            return None

        # Build result dictionary
        info = {
            'tar_id': int(fields[0]),
            'grb_id': int(fields[1]),
            'grb_seqn': int(fields[2]),
            'grb_type': int(fields[3]),
            'grb_ra': float(fields[4]) if fields[4] else None,
            'grb_dec': float(fields[5]) if fields[5] else None,
            'grb_errorbox': float(fields[6]) if fields[6] else None,
            'grb_date': float(fields[7]) if fields[7] else None,
            'grb_last_update': float(fields[8]) if fields[8] else None,
            'grb_is_grb': fields[9].lower() in ('t', 'true', '1')
        }

        return info

    except subprocess.TimeoutExpired:
        print("ERROR: Database query timeout", file=sys.stderr)
        return None
    except FileNotFoundError:
        print("ERROR: psql command not found", file=sys.stderr)
        return None
    except Exception as e:
        print(f"ERROR: Query failed: {e}", file=sys.stderr)
        return None


def get_grb_errorbox(tar_id=None, grb_id=None, database=None):
    """
    Simplified function to get just the error box size.

    Args:
        tar_id: RTS2 target ID
        grb_id: GCN GRB ID
        database: Database name

    Returns:
        Error box size in degrees (float), or None if not found
    """
    info = get_grb_info(tar_id=tar_id, grb_id=grb_id, database=database)
    return info['grb_errorbox'] if info else None


def get_grb_type_name(grb_type):
    """
    Convert GRB type code to human-readable name.

    Args:
        grb_type: Integer type code

    Returns:
        String description of GRB type
    """
    # From src/grb/grbconst.h
    type_names = {
        # HETE
        40: "HETE_ALERT",
        41: "HETE_UPDATE",
        42: "HETE_FINAL",
        43: "HETE_GNDANA",
        44: "HETE_TEST",

        # INTEGRAL
        51: "INTEGRAL_POINTDIR",
        52: "INTEGRAL_SPIACS",
        53: "INTEGRAL_WAKEUP",
        54: "INTEGRAL_REFINED",
        55: "INTEGRAL_OFFLINE",

        # Swift
        60: "SWIFT_BAT_GRB_ALERT",
        61: "SWIFT_BAT_GRB_POS_ACK",
        62: "SWIFT_BAT_GRB_POS_NACK",
        67: "SWIFT_BAT_GRB_LC",
        81: "SWIFT_XRT_POSITION",
        82: "SWIFT_XRT_SPECTRUM",
        83: "SWIFT_POINTDIR",
        84: "SWIFT_XRT_IMAGE",
        85: "SWIFT_XRT_LC",
        97: "SWIFT_UVOT_POS",

        # Fermi
        110: "FERMI_GBM_ALERT",
        111: "FERMI_GBM_FLT_POS",
        112: "FERMI_GBM_GND_POS",
        115: "FERMI_GBM_LC",

        # IceCube
        173: "ICECUBE_ASTROTRACK_GOLD",
        174: "ICECUBE_ASTROTRACK_BRONZE",
    }

    return type_names.get(grb_type, f"UNKNOWN_TYPE_{grb_type}")


if __name__ == '__main__':
    """Test harness - can be run standalone"""
    import argparse

    parser = argparse.ArgumentParser(description='Query GRB information from RTS2 database')
    parser.add_argument('--tar_id', type=int, help='RTS2 target ID')
    parser.add_argument('--grb_id', type=int, help='GCN GRB ID')
    parser.add_argument('--database', help='Database name (default: stars)')
    parser.add_argument('--errorbox-only', action='store_true', help='Print only error box size')

    args = parser.parse_args()

    if not args.tar_id and not args.grb_id:
        parser.error("Must provide either --tar_id or --grb_id")

    if args.errorbox_only:
        errorbox = get_grb_errorbox(tar_id=args.tar_id, grb_id=args.grb_id, database=args.database)
        if errorbox is not None:
            print(errorbox)
            sys.exit(0)
        else:
            sys.exit(1)
    else:
        info = get_grb_info(tar_id=args.tar_id, grb_id=args.grb_id, database=args.database)
        if info:
            print("GRB Information:")
            print(f"  Target ID:      {info['tar_id']}")
            print(f"  GRB ID:         {info['grb_id']}")
            print(f"  Type:           {get_grb_type_name(info['grb_type'])} ({info['grb_type']})")
            print(f"  RA/Dec:         {info['grb_ra']:.4f} / {info['grb_dec']:.4f}")
            print(f"  Error box:      {info['grb_errorbox']:.4f} degrees")
            print(f"  Is GRB:         {info['grb_is_grb']}")
            print(f"  Trigger time:   {info['grb_date']}")
            sys.exit(0)
        else:
            print("No GRB information found", file=sys.stderr)
            sys.exit(1)
