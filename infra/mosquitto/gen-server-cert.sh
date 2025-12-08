#gen-server-cert.sh
#!/usr/bin/env bash
set -euo pipefail

# Generates /etc/mosquitto/certs/server.key + server.crt using a SAN config.
# Usage:
#   sudo ./infra/mosquitto/gen-server-cert.sh --config /etc/mosquitto/certs/openssl-san.cnf
# If /etc/mosquitto/certs/ca.{key,crt} are missing, a new CA is created.

CERT_DIR=/etc/mosquitto/certs
RUNTIME_USER=${RUNTIME_USER:-mosq}
CFG=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --config) CFG="$2"; shift 2;;
    *) echo "Unknown arg: $1"; exit 1;;
  esac
done

mkdir -p "$CERT_DIR"
cd "$CERT_DIR"

# Default to $CERT_DIR/openssl-san.cnf if --config not provided
if [[ -z "${CFG}" ]]; then
  CFG="$CERT_DIR/openssl-san.cnf"
fi

if [[ ! -f "$CFG" ]]; then
  echo "SAN config not found: $CFG" >&2
  echo "Copy and edit ./infra/mosquitto/openssl-san.cnf to $CERT_DIR/openssl-san.cnf, then re-run." >&2
  exit 1
fi

# Create CA if missing
if [[ ! -f ca.key || ! -f ca.crt ]]; then
  echo "Generating local CA (10y)"
  openssl genrsa -out ca.key 4096
  openssl req -new -x509 -days 3650 -key ca.key -out ca.crt -subj "/CN=Local MQTT CA"
fi

# Generate server key if missing
if [[ ! -f server.key ]]; then
  openssl genrsa -out server.key 2048
fi

# Use the provided full OpenSSL req config with req_ext
CFG_DST="openssl-san.cnf"
cp "$CFG" "$CFG_DST"
openssl req -new -key server.key -out server.csr -config "$CFG_DST"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key \
  -CAcreateserial -out server.crt -days 3650 \
  -extfile "$CFG_DST" -extensions req_ext
rm -f server.csr

# Ownership and permissions
chown "$RUNTIME_USER":"$RUNTIME_USER" server.key server.crt ca.crt || true
chmod 600 server.key
chmod 644 server.crt ca.crt

# Summary
echo "Generated server cert with SAN from: $CFG"
openssl x509 -in server.crt -noout -text | sed -n '/Subject Alternative Name/,+1p'
