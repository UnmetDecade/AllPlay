#!/bin/sh
# Generate a throwaway self-signed identity for the protocol self-tests.
#
# This is NOT an Apple-issued MFi identity and cannot be used with a real
# device. It only lets iap2_test exercise the certificate and signature paths
# on machines that do not have identity/ (CI in particular).
#
# Usage: scripts/make-test-identity.sh [output-dir]
set -eu

DIR=${1:-build/test-identity}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$DIR"
openssl ecparam -name prime256v1 -genkey -noout -out "$TMP/key.pem"
openssl req -new -x509 -key "$TMP/key.pem" -out "$TMP/cert.pem" -days 2 \
    -subj "/CN=AllPlay CI test identity"
openssl pkcs8 -topk8 -nocrypt -in "$TMP/key.pem" -outform DER \
    -out "$DIR/identity.pk8"
openssl crl2pkcs7 -nocrl -certfile "$TMP/cert.pem" -outform DER \
    -out "$DIR/certificate.p7b"

chmod 0600 "$DIR/identity.pk8"
printf 'test identity written to %s (self-signed, test use only)\n' "$DIR"
