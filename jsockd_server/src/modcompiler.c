#include "modcompiler.h"
#include "config.h"
#include "hex.h"
#include "quickjs-libc.h"
#include "quickjs.h"
#include "verify_bytecode.h"
#include <ed25519/ed25519.h>
#include <ed25519/ge.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

// The module file format:
//     raw QuickJS bytecode
//     128 byte version string, null-terminated, right padded with zeros
//     64 byte ed25519 signature of bytecode + version string

static void extract_public_key(const unsigned char *private_key,
                               unsigned char *public_key) {
  ge_p3 A;

  // The first 32 bytes of the private key contain the scalar
  ge_scalarmult_base(&A, private_key);
  ge_p3_tobytes(public_key, &A);
}

int compile_module_file(const char *module_filename,
                        const char *privkey_filename,
                        const char *output_filename, const char *version,
                        int qjsc_strip_flags) {
  int ret = EXIT_SUCCESS;
  uint8_t *buf = NULL;
  uint8_t *out_buf = NULL;
  JSValue obj = JS_UNDEFINED;
  JSRuntime *rt = NULL;
  JSContext *ctx = NULL;
  FILE *mf = NULL;
  FILE *kf = NULL;

  if (strlen(version) >= VERSION_STRING_SIZE) {
    fprintf(stderr, "VERSION string too long\n");
    ret = EXIT_FAILURE;
    goto end;
  }

  rt = JS_NewRuntime();
  if (!rt) {
    fprintf(stderr, "Failed to create JS runtime when compiling module file\n");
    ret = EXIT_FAILURE;
    goto end;
  }
  JS_SetStripInfo(rt, qjsc_strip_flags);
  ctx = JS_NewContext(rt);
  if (!ctx) {
    fprintf(stderr, "Failed to create JS context when compiling module file\n");
    ret = EXIT_FAILURE;
    goto end;
  }
  js_init_module_std(ctx, "std");
  js_init_module_os(ctx, "os");

  size_t buf_len;
  buf = js_load_file(ctx, &buf_len, module_filename);
  if (!buf) {
    fprintf(stderr, "Could not load '%s'\n", module_filename);
    ret = EXIT_FAILURE;
    goto end;
  }
  obj = JS_Eval(ctx, (const char *)buf, buf_len, module_filename,
                JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_TYPE_MODULE);
  if (JS_IsException(obj)) {
    fprintf(stderr, "Error compiling module '%s'\n", module_filename);
    js_std_dump_error(ctx);
    ret = EXIT_FAILURE;
    goto end;
  }

  size_t out_buf_len;
  out_buf = JS_WriteObject(ctx, &out_buf_len, obj, JS_WRITE_OBJ_BYTECODE);
  out_buf = js_realloc(ctx, out_buf, out_buf_len + VERSION_STRING_SIZE);
  memset(out_buf + out_buf_len, 0, VERSION_STRING_SIZE);
  strcpy((char *)(out_buf + out_buf_len), version);
  out_buf_len += VERSION_STRING_SIZE;

  char private_key_hex[ED25519_PRIVATE_KEY_SIZE * 2] = {0};
  if (!privkey_filename) {
    fprintf(stderr,
            "WARNING: No key file specified; module will be unsigned\n");
  } else {
    kf = fopen(privkey_filename, "r");
    if (!kf) {
      fprintf(stderr, "Error opening key file %s: %s\n", privkey_filename,
              strerror(errno));
      ret = EXIT_FAILURE;
      goto end;
    }
    if (fread(private_key_hex, sizeof(private_key_hex) / sizeof(char), 1, kf) <
        1) {
      fprintf(stderr, "Error reading private key from key file %s: %s\n",
              privkey_filename, strerror(errno));
      ret = EXIT_FAILURE;
      goto end;
    }
  }
  uint8_t public_key[ED25519_PUBLIC_KEY_SIZE] = {0};
  uint8_t private_key[ED25519_PRIVATE_KEY_SIZE] = {0};
  hex_decode(private_key, ED25519_PRIVATE_KEY_SIZE, private_key_hex);
  extract_public_key(private_key, public_key);

  unsigned char signature[ED25519_SIGNATURE_SIZE] = {0};

  if (privkey_filename) {
    ed25519_sign(signature, out_buf, out_buf_len,
                 (const unsigned char *)public_key,
                 (const unsigned char *)private_key);
  }

  mf = fopen(output_filename, "w");
  if (!mf) {
    fprintf(stderr, "Error creating output file %s: %s\n", output_filename,
            strerror(errno));
    ret = EXIT_FAILURE;
    goto end;
  }

  if (fwrite(out_buf, out_buf_len, 1, mf) < 1) {
    fprintf(stderr, "Error writing bytecode to output file\n");
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
    fprintf(stderr, "Error creating private key file %s: %s\n",
            privkey_filename, strerror(errno));
  }
  if (!pubkey_file) {
    fprintf(stderr, "Error creating public key file %s: %s\n", pubkey_filename,
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
    fprintf(stderr, "Error creating random seed.\n");
    ret = EXIT_FAILURE;
    goto end;
  }
  ed25519_create_keypair(pubkey, privkey, seed);

  if (0 != hex_encode(pubkey, ED25519_PUBLIC_KEY_SIZE, pubkey_file)) {
    fprintf(stderr, "Error writing to public key file %s: %s", pubkey_filename,
            strerror(errno));
    ret = EXIT_FAILURE;
    goto end;
  }

  if (0 != hex_encode(privkey, ED25519_PRIVATE_KEY_SIZE, privkey_file)) {
    fprintf(stderr, "Error writing to private key file %s: %s",
            privkey_filename, strerror(errno));
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
