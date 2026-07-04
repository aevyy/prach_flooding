#!/bin/bash
set -e

CONFIG_PATH="${RA_SPOOF_CONFIG:-/etc/ra-spoof/config.yaml}"

INFLUX_HOST="${INFLUX_HOST:-influxdb}"
INFLUX_PORT="${INFLUX_PORT:-8086}"
INFLUX_ORG="${INFLUX_ORG:-oran}"
INFLUX_TOKEN="${INFLUX_TOKEN:-}"
INFLUX_BUCKET="${INFLUX_BUCKET:-rtusystem}"

if [ ! -f "$CONFIG_PATH" ]; then
    echo "ERROR: Config file not found at $CONFIG_PATH"
    echo "Set RA_SPOOF_CONFIG environment variable or mount config to /etc/ra-spoof/config.yaml"
    exit 1
fi

exec /usr/local/bin/ra-spoof \
    --config "$CONFIG_PATH" \
    --influx-host "$INFLUX_HOST" \
    --influx-port "$INFLUX_PORT" \
    --influx-org "$INFLUX_ORG" \
    --influx-token "$INFLUX_TOKEN" \
    --influx-bucket "$INFLUX_BUCKET" \
    "$@"
