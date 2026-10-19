#!/bin/sh
# Makes a throwaway CA and one server certificate for the local benchmark server.
# Usage: gen-pki.sh OUT_DIR  ->  OUT_DIR/ca.pem, server.pem, server.key
set -eu
out=${1:?usage: gen-pki.sh OUT_DIR}
mkdir -p "$out"
cd "$out"
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 30 -subj "/CN=pxsteamdl bench CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr -subj "/CN=bench server" 2>/dev/null
cat > ext.cnf <<EOT
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:api.steampowered.com,DNS:cm1.bench.test,DNS:cdn1.bench.test,DNS:cdn2.bench.test,DNS:cdn3.bench.test,DNS:cdn4.bench.test
EOT
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -out server.pem -days 30 -extfile ext.cnf 2>/dev/null
rm -f server.csr ext.cnf ca.srl
echo "wrote $out/ca.pem $out/server.pem $out/server.key"
