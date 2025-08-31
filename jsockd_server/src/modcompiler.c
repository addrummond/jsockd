#include "config.h"
#include "hex.h"
#include "quickjs-libc.h"
#include "quickjs.h"
#include "utils.h"
#include "verify_bytecode.h"
#include "version.h"
#include <ed25519/ed25519.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

// The module file format:
//     raw QuickJS bytecode
//     128 byte version string, null-terminated
//     64 byte ed25519 signature of bytecode + version string

int compile_file(JSContext *ctx, const char *module_filename,
                 const uint8_t *public_key, const uint8_t *private_key,
                 FILE *fo) {
  int ret = 0;
  uint8_t *buf = NULL;
  int eval_flags;
  JSValue obj = JS_UNDEFINED;
  size_t buf_len;

  buf = js_load_file(ctx, &buf_len, module_filename);
  if (!buf) {
    release_logf("Could not load '%s'\n", module_filename);
    ret = -1;
    goto end;
  }
  eval_flags = JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_TYPE_MODULE;
  obj = JS_Eval(ctx, (const char *)buf, buf_len, module_filename, eval_flags);
  if (JS_IsException(obj)) {
    release_logf("Error compiling module '%s'\n", module_filename);
    js_std_dump_error(ctx);
    ret = -1;
    goto end;
  }

  size_t out_buf_len;
  uint8_t *out_buf =
      JS_WriteObject(ctx, &out_buf_len, obj, JS_WRITE_OBJ_BYTECODE);

  unsigned char signature[ED25519_SIGNATURE_SIZE];

  ed25519_sign(signature, out_buf, out_buf_len,
               (const unsigned char *)public_key,
               (const unsigned char *)private_key);

  if (fwrite(out_buf, out_buf_len, 1, fo) < 1) {
    fprintf(stderr, "Error writing bytecode to output file\n");
    ret = -1;
    goto end;
  }
  char vstring[VERSION_STRING_MAX_LENGTH] = {0};
  if (strlen(STRINGIFY(VERSION)) >= VERSION_STRING_MAX_LENGTH) {
    release_logf("VERSION string too long\n");
    ret = -1;
    goto end;
  }
  strcpy(vstring, STRINGIFY(VERSION));
  if (fwrite(vstring, sizeof(vstring) / sizeof(char), 1, fo) < 1) {
    fprintf(stderr, "Error writing version string to output file\n");
    ret = -1;
    goto end;
  }
  if (fwrite(signature, sizeof(signature) / sizeof(signature[0]), 1, fo) < 1) {
    fprintf(stderr, "Error writing signature to output file\n");
    ret = -1;
    goto end;
  }

end:
  js_free(ctx, buf);
  JS_FreeValue(ctx, obj);
  return ret;
}

int output_key_files(const char *key_file_prefix) {
  int ret = EXIT_SUCCESS;

  size_t privkey_filename_len =
      strlen(key_file_prefix) + sizeof(".privkey") + 1;
  size_t pubkey_filename_len = strlen(key_file_prefix) + sizeof(".pubkey") + 1;
  char *privkey_filename = malloc(privkey_filename_len);
  char *pubkey_filename = malloc(pubkey_filename_len);
  snprintf(privkey_filename, privkey_filename_len, "%s.privkey",
           key_file_prefix);
  snprintf(pubkey_filename, pubkey_filename_len, "%s.pubkey", key_file_prefix);
  FILE *privkey_file = fopen(privkey_filename, "wx");
  FILE *pubkey_file = fopen(pubkey_filename, "wx");
  if (!privkey_file) {
    release_logf("Error creating private key file %s: %s\n", privkey_filename,
                 strerror(errno));
  }
  if (!pubkey_file) {
    release_logf("Error creating public key file %s: %s\n", pubkey_filename,
                 strerror(errno));
  }
  if (!(privkey_file && pubkey_file)) {
    ret = EXIT_FAILURE;
    goto end;
  }

  unsigned char seed[ED25519_SEED_SIZE];
  unsigned char pubkey[ED25519_PUBLIC_KEY_SIZE];
  unsigned char privkey[ED25519_PRIVATE_KEY_SIZE];
  if (0 != ed25519_create_seed(seed)) {
    release_log("Error creating random seed.\n");
    ret = EXIT_FAILURE;
    goto end;
  }
  ed25519_create_keypair(pubkey, privkey, seed);

  if (0 != hex_encode(pubkey, ED25519_PUBLIC_KEY_SIZE, pubkey_file)) {
    release_logf("Error writing to public key file %s: %s", pubkey_filename,
                 strerror(errno));
    ret = EXIT_FAILURE;
    goto end;
  }
  if (0 != hex_encode(privkey, ED25519_PRIVATE_KEY_SIZE, privkey_file)) {
    release_logf("Error writing to private key file %s: %s", privkey_filename,
                 strerror(errno));
    ret = EXIT_FAILURE;
    goto end;
  }

end:
  if (privkey_file)
    fclose(privkey_file);
  if (pubkey_file)
    fclose(pubkey_file);
  free(privkey_filename);
  free(pubkey_filename);
  return ret;
}
