#!/bin/bash -x

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

if [ $DIR != "/root/revshell/ca" ]; then
echo "shit"
exit
fi

rm -f $DIR/*.pem
rm -f $DIR/*.crt
rm -f $DIR/*.old
rm -f $DIR/*.attr
rm -f $DIR/index.txt
rm -f $DIR/private/*
rm -f $DIR/signedcerts/*
touch $DIR/index.txt
echo '01' > $DIR/serial

export OPENSSL_CONF=$DIR/caconfig.cnf
openssl req -x509 -newkey rsa:2048 -out $DIR/cacert.pem -outform PEN -days 1825
openssl x509 -in $DIR/cacert.pem -out $DIR/cacert.crt

export OPENSSL_CONF=$DIR/server.cnf
openssl req -newkey rsa:1024 -keyout $DIR/tempkey.pem -keyform PEM -out $DIR/tempreq.pem -outform PEM
openssl rsa < $DIR/tempkey.pem > $DIR/shell_key.pem

export OPENSSL_CONF=$DIR/caconfig.cnf
openssl ca -in $DIR/tempreq.pem -out $DIR/shell_crt.pem
#rm -f $DIR/tempkey.pem 
#rm -f $DIR/tempreq.pem
cat $DIR/shell_key.pem $DIR/shell_crt.pem > $DIR/shell.pem

openssl req -x509 -nodes -days 365 -newkey rsa:1024 -keyout $DIR/client.pem -out $DIR/client.pem
openssl pkcs12 -export -out $DIR/client.pfx -in $DIR/client.pem -name "client cert"
