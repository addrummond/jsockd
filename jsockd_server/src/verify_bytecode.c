#include "verify_bytecode.h"
#include <ed25519/ed25519.h>

bool verify_bytecode(const uint8_t *bytecode, size_t bytecode_size,
                     const uint8_t *public_key) {
  if (bytecode_size < ED25519_SIGNATURE_SIZE + 1)
    return false;
  // The signature is at the end of the bytecode file.
  return ed25519_verify(bytecode + bytecode_size - ED25519_SIGNATURE_SIZE,
                        bytecode, bytecode_size - ED25519_SIGNATURE_SIZE,
                        public_key);
}
