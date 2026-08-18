#!/usr/bin/env bash
# Generate disposable DDS Security credentials for CI/lab validation.
# No private CA material is committed to the repository.
set -euo pipefail

OUT=${1:?usage: generate_test_pki.sh OUT_DIR}
rm -rf "$OUT"
mkdir -p "$OUT/main" "$OUT/untrusted" "$OUT/work"
WORK="$OUT/work"

ca() {
    local name=$1 cn=$2
    openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "$WORK/$name.key" >/dev/null 2>&1
    openssl req -new -x509 -key "$WORK/$name.key" -sha256 -days 2 \
        -subj "/CN=$cn" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -out "$WORK/$name.pem" >/dev/null 2>&1
}

participant() {
    local ca_name=$1 role=$2 target=$3
    openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 \
        -out "$target/$role-key.pem" >/dev/null 2>&1
    openssl req -new -key "$target/$role-key.pem" -subj "/CN=rdtf-$role" \
        -out "$WORK/$role.csr" >/dev/null 2>&1
    cat > "$WORK/$role.ext" <<EXT
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid,issuer
EXT
    openssl x509 -req -in "$WORK/$role.csr" \
        -CA "$WORK/$ca_name.pem" -CAkey "$WORK/$ca_name.key" -CAcreateserial \
        -days 2 -sha256 -extfile "$WORK/$role.ext" \
        -out "$target/$role-cert.pem" >/dev/null 2>&1
    chmod 600 "$target/$role-key.pem"
}

permissions_xml() {
    local role=$1 action=$2 file=$3
    cat > "$file" <<XML
<dds xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:noNamespaceSchemaLocation="http://www.omg.org/spec/DDS-Security/20170801/omg_shared_ca_permissions.xsd">
  <permissions>
    <grant name="$role">
      <subject_name>CN=rdtf-$role</subject_name>
      <validity>
        <not_before>2020-01-01T00:00:00</not_before>
        <not_after>2038-01-01T00:00:00</not_after>
      </validity>
      <allow_rule>
        <domains><id_range><min>0</min><max>230</max></id_range></domains>
        <$action><topics><topic>SystemTelemetry</topic></topics></$action>
      </allow_rule>
      <default>DENY</default>
    </grant>
  </permissions>
</dds>
XML
}

sign_permissions() {
    local role=$1 target=$2 action=$3
    local xml="$WORK/$role-permissions.xml"
    permissions_xml "$role" "$action" "$xml"
    openssl smime -sign -in "$xml" -text \
        -signer "$WORK/permissions_ca.pem" -inkey "$WORK/permissions_ca.key" \
        -out "$target/$role-permissions.smime"
}

ca identity_ca "RDTF Test Identity CA"
ca rogue_identity_ca "RDTF Rogue Identity CA"
ca permissions_ca "RDTF Test Permissions CA"

cat > "$WORK/governance.xml" <<'XML'
<dds xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
     xsi:noNamespaceSchemaLocation="omg_shared_ca_domain_governance.xsd">
  <domain_access_rules>
    <domain_rule>
      <domains><id_range><min>0</min><max>230</max></id_range></domains>
      <allow_unauthenticated_participants>false</allow_unauthenticated_participants>
      <enable_join_access_control>true</enable_join_access_control>
      <discovery_protection_kind>ENCRYPT</discovery_protection_kind>
      <liveliness_protection_kind>ENCRYPT</liveliness_protection_kind>
      <rtps_protection_kind>ENCRYPT</rtps_protection_kind>
      <topic_access_rules>
        <topic_rule>
          <topic_expression>SystemTelemetry</topic_expression>
          <enable_discovery_protection>true</enable_discovery_protection>
          <enable_liveliness_protection>true</enable_liveliness_protection>
          <enable_read_access_control>true</enable_read_access_control>
          <enable_write_access_control>true</enable_write_access_control>
          <metadata_protection_kind>ENCRYPT</metadata_protection_kind>
          <data_protection_kind>ENCRYPT</data_protection_kind>
        </topic_rule>
      </topic_access_rules>
    </domain_rule>
  </domain_access_rules>
</dds>
XML

openssl smime -sign -in "$WORK/governance.xml" -text \
    -signer "$WORK/permissions_ca.pem" -inkey "$WORK/permissions_ca.key" \
    -out "$WORK/governance.smime"

cp "$WORK/identity_ca.pem" "$OUT/main/identity_ca.pem"
cp "$WORK/permissions_ca.pem" "$OUT/main/permissions_ca.pem"
cp "$WORK/governance.smime" "$OUT/main/governance.smime"
participant identity_ca publisher "$OUT/main"
participant identity_ca subscriber "$OUT/main"
participant identity_ca unauthorized-publisher "$OUT/main"
sign_permissions publisher "$OUT/main" publish
sign_permissions subscriber "$OUT/main" subscribe
# Trusted identity, but no write grant: local DataWriter creation must be denied.
sign_permissions unauthorized-publisher "$OUT/main" subscribe

cp "$WORK/rogue_identity_ca.pem" "$OUT/untrusted/identity_ca.pem"
cp "$WORK/permissions_ca.pem" "$OUT/untrusted/permissions_ca.pem"
cp "$WORK/governance.smime" "$OUT/untrusted/governance.smime"
participant rogue_identity_ca untrusted-publisher "$OUT/untrusted"
sign_permissions untrusted-publisher "$OUT/untrusted" publish

# Verify signed policy artifacts before deleting the permissions CA key. OpenSSL
# defaults to S/MIME email-signing EKU validation; DDS policy signing is not
# email, so purpose=any checks signature/trust without inventing an email EKU.
openssl smime -verify -purpose any -in "$WORK/governance.smime" \
    -CAfile "$WORK/permissions_ca.pem" -out /dev/null >/dev/null 2>&1
for signed in \
    "$OUT/main/publisher-permissions.smime" \
    "$OUT/main/subscriber-permissions.smime" \
    "$OUT/main/unauthorized-publisher-permissions.smime" \
    "$OUT/untrusted/untrusted-publisher-permissions.smime"; do
    openssl smime -verify -purpose any -in "$signed" \
        -CAfile "$WORK/permissions_ca.pem" -out /dev/null >/dev/null 2>&1
done

# CA private keys are no longer required after signing. Remove them so test
# artifacts contain only participant private keys and public/signature material.
rm -f "$WORK"/*.key "$WORK"/*.csr "$WORK"/*.ext "$WORK"/*.srl

echo "Generated and verified disposable DDS Security PKI at $OUT"
