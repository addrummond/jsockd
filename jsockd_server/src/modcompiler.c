#include "modcompiler.h"
#include "config.h"
#include "hex.h"
#include "quickjs-libc.h"
#include "quickjs.h"
#include "utils.h"
#include "verify_bytecode.h"
#include <ed25519/ed25519.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

// The module file format:
//     raw QuickJS bytecode
//     128 byte version string, null-terminated
//     64 byte ed25519 signature of bytecode

int compile_module_file(const char *module_filename,
                        const char *privkey_filename,
                        const char *output_filename, const char *version) {
  int ret = EXIT_SUCCESS;
  uint8_t *buf = NULL;
  uint8_t *out_buf = NULL;
  int eval_flags;
  JSValue obj = JS_UNDEFINED;
  JSRuntime *rt = NULL;
  JSContext *ctx = NULL;
  size_t buf_len;
  FILE *mf = NULL;
  FILE *kf = NULL;

  rt = JS_NewRuntime();
  if (!rt) {
    release_log("Failed to create JS runtime when compiling module file\n");
    ret = EXIT_FAILURE;
    goto end;
  }
  ctx = JS_NewContext(rt);
  if (!ctx) {
    release_log("Failed to create JS context when compiling module file\n");
    ret = EXIT_FAILURE;
    goto end;
  }

  buf = js_load_file(ctx, &buf_len, module_filename);
  if (!buf) {
    release_logf("Could not load '%s'\n", module_filename);
    ret = EXIT_FAILURE;
    goto end;
  }
  eval_flags = JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_TYPE_MODULE;
  obj = JS_Eval(ctx, (const char *)buf, buf_len, module_filename, eval_flags);
  if (JS_IsException(obj)) {
    release_logf("Error compiling module '%s'\n", module_filename);
    js_std_dump_error(ctx);
    ret = EXIT_FAILURE;
    goto end;
  }

  size_t out_buf_len;
  out_buf = JS_WriteObject(ctx, &out_buf_len, obj, JS_WRITE_OBJ_BYTECODE);

  char public_key_hex[ED25519_PUBLIC_KEY_SIZE * 2] = {0};
  char private_key_hex[ED25519_PRIVATE_KEY_SIZE * 2] = {0};
  if (!privkey_filename) {
    release_logf("WARNING: No key file specified; module will be unsigned\n");
  } else {
    kf = fopen(privkey_filename, "r");
    if (!kf) {
      release_logf("Error opening key file %s: %s\n", privkey_filename,
                   strerror(errno));
      ret = EXIT_FAILURE;
      goto end;
    }
    if (fread(public_key_hex, sizeof(public_key_hex) / sizeof(char), 1, kf) <
        1) {
      release_logf("Error reading public key from key file %s: %s\n",
                   privkey_filename, strerror(errno));
      ret = EXIT_FAILURE;
      goto end;
    }
    if (fread(private_key_hex, sizeof(private_key_hex) / sizeof(char), 1, kf) <
        1) {
      release_logf("Error reading private key from key file %s: %s\n",
                   privkey_filename, strerror(errno));
      ret = EXIT_FAILURE;
      goto end;
    }
  }
  uint8_t public_key[ED25519_PUBLIC_KEY_SIZE] = {0};
  uint8_t private_key[ED25519_PRIVATE_KEY_SIZE] = {0};
  hex_decode(public_key, ED25519_PUBLIC_KEY_SIZE, public_key_hex);
  hex_decode(private_key, ED25519_PRIVATE_KEY_SIZE, private_key_hex);

  unsigned char signature[ED25519_SIGNATURE_SIZE] = {0};

  if (privkey_filename) {
    ed25519_sign(signature, out_buf, out_buf_len,
                 (const unsigned char *)public_key,
                 (const unsigned char *)private_key);
  }

  mf = fopen(output_filename, "w");
  if (!mf) {
    release_logf("Error creating output file %s: %s\n", output_filename,
                 strerror(errno));
    ret = EXIT_FAILURE;
    goto end;
  }

  if (fwrite(out_buf, out_buf_len, 1, mf) < 1) {
    fprintf(stderr, "Error writing bytecode to output file\n");
    ret = EXIT_FAILURE;
    goto end;
  }
  char vstring[VERSION_STRING_SIZE] = {0};
  if (strlen(version) >= VERSION_STRING_SIZE) {
    release_logf("VERSION string too long\n");
    ret = EXIT_FAILURE;
    goto end;
  }
  strcpy(vstring, version);
  if (fwrite(vstring, sizeof(vstring) / sizeof(char), 1, mf) < 1) {
    fprintf(stderr, "Error writing version string to output file\n");
    ret = EXIT_FAILURE;
    goto end;
  }
  if (fwrite(signature, sizeof(signature) / sizeof(signature[0]), 1, mf) < 1) {
    fprintf(stderr, "Error writing signature to output file\n");
    ret = EXIT_FAILURE;
    goto end;
  }

end:
  if (kf)
    fclose(kf);
  if (mf)
    fclose(mf);
  if (ctx) {
    js_free(ctx, out_buf);
    js_free(ctx, buf);
    JS_FreeValue(ctx, obj);
    JS_FreeContext(ctx);
  }
  if (rt)
    JS_FreeRuntime(rt);
  return ret;
}

static void get_key_file_filenames(const char *prefix,
                                   char **out_pubkey_filename,
                                   char **out_privkey_filename) {
  size_t privkey_filename_len =
      strlen(prefix) + sizeof(PRIVATE_KEY_FILE_SUFFIX);
  size_t pubkey_filename_len = strlen(prefix) + sizeof(PUBLIC_KEY_FILE_SUFFIX);
  char *privkey_filename = malloc(privkey_filename_len);
  char *pubkey_filename = malloc(pubkey_filename_len);
  snprintf(privkey_filename, privkey_filename_len, "%s%s", prefix,
           PRIVATE_KEY_FILE_SUFFIX);
  snprintf(pubkey_filename, pubkey_filename_len, "%s%s", prefix,
           PUBLIC_KEY_FILE_SUFFIX);
  *out_privkey_filename = privkey_filename;
  *out_pubkey_filename = pubkey_filename;
}

int output_key_file(const char *key_file_prefix) {
  int ret = EXIT_SUCCESS;

  char *privkey_filename;
  char *pubkey_filename;
  get_key_file_filenames(key_file_prefix, &pubkey_filename, &privkey_filename);

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

  // As the format used by the ed25519 library we're using doesn't make it
  // trivial to extract public keys from private keys, prepend the public key to
  // the private key.
  if (0 != hex_encode(pubkey, ED25519_PUBLIC_KEY_SIZE, privkey_file)) {
    release_logf("Error writing to private key file %s: %s", privkey_filename,
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
