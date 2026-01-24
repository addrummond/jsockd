#ifndef VERIFY_BYTECODE_H_
#define VERIFY_BYTECODE_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define ED25519_SIGNATURE_SIZE 64
#define ED25519_PUBLIC_KEY_SIZE 32
#define ED25519_PRIVATE_KEY_SIZE 64
#define ED25519_SEED_SIZE 32

bool verify_bytecode(const uint8_t *bytecode, size_t bytecode_size,
                     const uint8_t *public_key);

#endif
