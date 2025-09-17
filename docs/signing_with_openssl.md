# Using openssl to sign bytecode

If you don't trust JSockD to generate keys and signatures, you can use openssl to sign your module bytecode.
See `docs/signing_with_openssl.md` for details.

_**On Mac you may need to install openssl via homebrew to get support for ED25519 signatures.**_

If you don't trust JSockD to generate keys and signatures, you can use openssl to sign your module bytecode.
Generate public and private keys as follows:

```sh
openssl genpkey -algorithm ed25519 -out private_signing_key.pem
openssl pkey -inform pem -pubout -outform der -in private_signing_key.pem | tail -c 32 | xxd -p | tr -d '\n' > public_signing_key_hex
```

Now compile the module without a key:

```sh
jsockd -c my_module.mjs my_module.quickjs_bytecode
```

The last 64 bytes of the bytecode file (usually occupied by the ED25519 signature) are now filled with zeros. The preceding 128 bytes contain the jsockd compiler version string. You therefore need to sign the file **excluding the last 192 bytes** and then replace the last 64 bytes of the file with the signature. The following shell one-liner accomplishes this:

```sh
BYTECODE_FILE=my_module.quickjs_bytecode BYTECODE_FILE_SIZE=$(wc -c $BYTECODE_FILE | awk '{print $1}') && ( head -c $(($BYTECODE_FILE_SIZE - 192)) $BYTECODE_FILE | openssl pkeyutl -sign -inkey private_signing_key.pem -rawin -in /dev/stdin | dd of=$BYTECODE_FILE bs=1 seek=$(($BYTECODE_FILE_SIZE - 64)) conv=notrunc )
```

Finally, set the environment variable to the hex-encoded public key before running jsockd:

```sh
export JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=$(cat public_signing_key_hex)
jsockd -m my_module.quickjs_bytecode -s /tmp/sock
```
