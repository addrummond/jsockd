#include "ed25519.h"

#ifndef ED25519_NO_SEED

#ifdef _WIN32
#include <wincrypt.h>
#include <windows.h>
#else
#include <stdio.h>
#endif

int ed25519_create_seed(unsigned char *seed) {
#ifdef _WIN32
  HCRYPTPROV prov;

  if (!CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_FULL,
                           CRYPT_VERIFYCONTEXT)) {
    return 1;
  }

  if (!CryptGenRandom(prov, 32, seed)) {
    CryptReleaseContext(prov, 0);
    return 1;
  }

  CryptReleaseContext(prov, 0);

  return 0;
#else
  FILE *f = fopen("/dev/urandom", "rb");

  if (f == NULL) {
    return 1;
  }

  if (0 == fread(seed, 1, 32, f))
    return -1;
  if (0 != fclose(f))
    return -1;
  return 0;
#endif
}

#endif
