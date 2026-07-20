#!/bin/bash
# Re-sign a locally built Snapmaker Orca.app with a stable self-signed identity.
#
# Why: an ad-hoc (unsigned) build gets a new cdhash on every build, so macOS
# privacy grants tied to the code signature (Documents folder access, Local
# Network, etc.) are revoked and re-prompted after each rebuild. Signing with a
# persistent certificate keeps the identity stable, so permissions are granted
# once and survive rebuilds.
#
# First run shows one keychain prompt ("codesign wants to use key ...") —
# click "Always Allow" and it never asks again.
#
# Usage:  scripts/codesign_local.sh [path/to/Snapmaker Orca.app]

set -e

APP="${1:-$(dirname "$0")/../build/arm64/Snapmaker_Orca/Snapmaker Orca.app}"
IDENTITY="Snapmaker Orca Local Dev"

# Create the self-signed code-signing identity once (kept in the login keychain).
if ! security find-certificate -c "$IDENTITY" ~/Library/Keychains/login.keychain-db >/dev/null 2>&1; then
    echo "Creating self-signed code-signing identity '$IDENTITY' in login keychain..."
    WORK="$(mktemp -d)"
    cat > "$WORK/codesign.cnf" <<'EOF'
[ req ]
distinguished_name = dn
x509_extensions = ext
prompt = no
[ dn ]
CN = Snapmaker Orca Local Dev
[ ext ]
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
basicConstraints = critical, CA:false
EOF
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$WORK/key.pem" -out "$WORK/cert.pem" -days 3650 -config "$WORK/codesign.cnf"
    # -legacy: macOS `security import` rejects the default OpenSSL 3.x PKCS#12 algorithms.
    openssl pkcs12 -export -legacy -out "$WORK/cert.p12" -inkey "$WORK/key.pem" -in "$WORK/cert.pem" -passout pass:local
    security import "$WORK/cert.p12" -k ~/Library/Keychains/login.keychain-db -P local -T /usr/bin/codesign
    rm -rf "$WORK"
fi

echo "Signing '$APP' with '$IDENTITY'..."
codesign --force --deep --sign "$IDENTITY" --timestamp=none "$APP"
codesign -dv "$APP" 2>&1 | head -4
