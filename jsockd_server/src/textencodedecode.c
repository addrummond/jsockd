//
// This file is some code gathered together rather messily from
// https://github.com/nginx/njs/ (revision
// 82a588c17bc7673ac61ad7871280e049fb5ea353) guided by the original diff:
//     https://github.com/nginx/njs/commit/447d66d41d41504db976e900d94e75a90d388265#diff-7142db15fa33b557643f8198abb65a6842a9c9065cda2d5bc002f18aa0756f22
//

#include "quickjs.h"
#include <stdbool.h>

enum {
  NJS_UNICODE_BOM = 0xFEFF,
  NJS_UNICODE_REPLACEMENT = 0xFFFD,
  NJS_UNICODE_MAX_CODEPOINT = 0x10FFFF,
  NJS_UNICODE_ERROR = 0x1FFFFF,
  NJS_UNICODE_CONTINUE = 0x2FFFFF
};

typedef struct {
  uint32_t codepoint;

  unsigned need;
  unsigned char lower;
  unsigned char upper;
} njs_unicode_decode_t;

#define njs_surrogate_leading(cp) (((unsigned)(cp) - 0xd800) <= 0xdbff - 0xd800)

#define njs_surrogate_trailing(cp)                                             \
  (((unsigned)(cp) - 0xdc00) <= 0xdfff - 0xdc00)

#define njs_surrogate_any(cp) (((unsigned)(cp) - 0xd800) <= 0xdfff - 0xd800)

#define njs_surrogate_pair(high, low)                                          \
  (0x10000 + (((high) - 0xd800) << 10) + ((low) - 0xdc00))

typedef struct {
  size_t length;
  unsigned char *start;
} njs_str_t;

#define njs_length(s) (sizeof(s) - 1)
#define njs_str(s) {njs_length(s), (u_char *)s}
#define njs_null_str {0, NULL}
#define njs_str_value(s) (njs_str_t) njs_str(s)
#define njs_strstr_eq(s1, s2)                                                  \
  (((s1)->length == (s2)->length) &&                                           \
   (memcmp((s1)->start, (s2)->start, (s1)->length) == 0))

#define njs_fast_path(x) (x)
#define njs_slow_path(x) (x)

typedef enum {
  QJS_ENCODING_UTF8,
} qjs_encoding_t;

typedef struct {
  qjs_encoding_t encoding;
  int fatal;
  int ignore_bom;

  njs_unicode_decode_t ctx;
} qjs_text_decoder_t;

typedef struct {
  njs_str_t name;
  qjs_encoding_t encoding;
} qjs_encoding_label_t;

typedef unsigned char u_char;

static qjs_encoding_label_t qjs_encoding_labels[] = {
    {njs_str("utf-8"), QJS_ENCODING_UTF8},
    {njs_str("utf8"), QJS_ENCODING_UTF8},
    {njs_null_str, 0}};

static JSClassID text_decoder_class_id;
static JSClassID text_encoder_class_id;

static void qjs_text_decoder_finalizer(JSRuntime *rt, JSValue val) {
  qjs_text_decoder_t *td;

  JS_NewClassID(&text_decoder_class_id);

  td = JS_GetOpaque(val, text_decoder_class_id);
  if (td != NULL) {
    js_free_rt(rt, td);
  }
}

static JSClassDef qjs_text_decoder_class = {
    "TextDecoder",
    .finalizer = qjs_text_decoder_finalizer,
};

static JSValue qjs_text_encoder_encode_into(JSContext *cx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv);
static JSValue qjs_text_encoder_encode(JSContext *cx, JSValueConst this_val,
                                       int argc, JSValueConst *argv);
static JSValue qjs_text_encoder_encoding(JSContext *ctx, JSValueConst this_val);

static const JSCFunctionListEntry qjs_text_encoder_proto[] = {
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "TextEncoder",
                       JS_PROP_CONFIGURABLE),
    JS_CFUNC_DEF("encode", 1, qjs_text_encoder_encode),
    JS_CFUNC_DEF("encodeInto", 1, qjs_text_encoder_encode_into),
    JS_CGETSET_DEF("encoding", qjs_text_encoder_encoding, NULL),
};

static JSValue qjs_text_decoder_ignore_bom(JSContext *ctx,
                                           JSValueConst this_val) {
  qjs_text_decoder_t *td;

  JS_NewClassID(&text_decoder_class_id);

  td = JS_GetOpaque(this_val, text_decoder_class_id);
  if (td == NULL) {
    return JS_ThrowInternalError(ctx, "'this' is not a TextDecoder");
  }

  return JS_NewBool(ctx, td->ignore_bom);
}

static JSValue qjs_text_decoder_fatal(JSContext *ctx, JSValueConst this_val) {
  qjs_text_decoder_t *td;

  JS_NewClassID(&text_decoder_class_id);

  td = JS_GetOpaque(this_val, text_decoder_class_id);
  if (td == NULL) {
    return JS_ThrowInternalError(ctx, "'this' is not a TextDecoder");
  }

  return JS_NewBool(ctx, td->fatal);
}

static JSValue qjs_text_decoder_encoding(JSContext *ctx,
                                         JSValueConst this_val) {
  qjs_text_decoder_t *td;

  JS_NewClassID(&text_decoder_class_id);

  td = JS_GetOpaque(this_val, text_decoder_class_id);
  if (td == NULL) {
    return JS_ThrowInternalError(ctx, "'this' is not a TextDecoder");
  }

  switch (td->encoding) {
  case QJS_ENCODING_UTF8:
    return JS_NewString(ctx, "utf-8");
  }

  return JS_UNDEFINED;
}

static JSValue qjs_typed_array_data(JSContext *ctx, JSValueConst value,
                                    njs_str_t *data);
static size_t njs_utf8_bom(const u_char *start, const u_char *end);
static ssize_t njs_utf8_stream_length(njs_unicode_decode_t *ctx,
                                      const u_char *p, size_t len, bool last,
                                      bool fatal, size_t *out_size);
static unsigned char *njs_utf8_stream_encode(njs_unicode_decode_t *ctx,
                                             const u_char *start,
                                             const u_char *end, u_char *dst,
                                             bool last, bool fatal);
static void njs_utf8_decode_init(njs_unicode_decode_t *ctx);

static JSValue qjs_text_decoder_decode(JSContext *cx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  int stream;
  size_t size;
  unsigned char *dst;
  JSValue ret;
  ssize_t length;
  njs_str_t data;
  const unsigned char *end;
  qjs_text_decoder_t *td;
  njs_unicode_decode_t ctx;

  JS_NewClassID(&text_decoder_class_id);

  td = JS_GetOpaque(this_val, text_decoder_class_id);
  if (td == NULL) {
    return JS_ThrowInternalError(cx, "'this' is not a TextDecoder");
  }

  ret = qjs_typed_array_data(cx, argv[0], &data);
  if (JS_IsException(ret)) {
    return ret;
  }

  stream = 0;

  if (argc > 1) {
    ret = JS_GetPropertyStr(cx, argv[1], "stream");
    if (JS_IsException(ret)) {
      return JS_EXCEPTION;
    }

    stream = JS_ToBool(cx, ret);
    JS_FreeValue(cx, ret);
  }

  ctx = td->ctx;
  end = data.start + data.length;

  if (data.start != NULL && !td->ignore_bom) {
    data.start += njs_utf8_bom(data.start, end);
  }

  length = njs_utf8_stream_length(&ctx, data.start, end - data.start, !stream,
                                  td->fatal, &size);

  if (length == -1) {
    return JS_ThrowTypeError(cx, "The encoded data was not valid");
  }

  dst = js_malloc(cx, size + 1);
  if (dst == NULL) {
    JS_ThrowOutOfMemory(cx);
    return JS_EXCEPTION;
  }

  (void)njs_utf8_stream_encode(&td->ctx, data.start, end, dst, !stream, 0);

  ret = JS_NewStringLen(cx, (const char *)dst, size);
  js_free(cx, dst);

  if (!stream) {
    njs_utf8_decode_init(&td->ctx);
  }

  return ret;
}

static const JSCFunctionListEntry qjs_text_decoder_proto[] = {
    JS_PROP_STRING_DEF("[Symbol.toStringTag]", "TextDecoder",
                       JS_PROP_CONFIGURABLE),
    JS_CFUNC_DEF("decode", 1, qjs_text_decoder_decode),
    JS_CGETSET_DEF("encoding", qjs_text_decoder_encoding, NULL),
    JS_CGETSET_DEF("fatal", qjs_text_decoder_fatal, NULL),
    JS_CGETSET_DEF("ignoreBOM", qjs_text_decoder_ignore_bom, NULL),
};

static JSValue qjs_typed_array_data(JSContext *ctx, JSValueConst value,
                                    njs_str_t *data) {
  size_t byte_offset, byte_length;
  JSValue ab;

  /* GCC complains about uninitialized variables. */

  byte_offset = 0;
  byte_length = 0;

  /* TODO: DataView. */

  ab = JS_GetTypedArrayBuffer(ctx, value, &byte_offset, &byte_length, NULL);
  if (JS_IsException(ab)) {
    data->start = JS_GetArrayBuffer(ctx, &data->length, value);
    if (data->start == NULL) {
      return JS_EXCEPTION;
    }

    return JS_UNDEFINED;
  }

  data->start = JS_GetArrayBuffer(ctx, &data->length, ab);

  JS_FreeValue(ctx, ab);

  if (data->start == NULL) {
    return JS_EXCEPTION;
  }

  data->start += byte_offset;
  data->length = byte_length;

  return JS_UNDEFINED;
}

static JSValue qjs_new_uint8_array(JSContext *ctx, int argc,
                                   JSValueConst *argv) {
  JSValue ret;

  ret = JS_NewTypedArray(ctx, argc, argv, JS_TYPED_ARRAY_UINT8);

  return ret;
}

static int qjs_text_decoder_encoding_arg(JSContext *cx, int argc,
                                         JSValueConst *argv,
                                         qjs_text_decoder_t *td) {
  njs_str_t str;
  qjs_encoding_label_t *label;

  if (argc < 1) {
    td->encoding = QJS_ENCODING_UTF8;
    return 0;
  }

  str.start = (unsigned char *)JS_ToCStringLen(cx, &str.length, argv[0]);
  if (str.start == NULL) {
    JS_ThrowOutOfMemory(cx);
    return -1;
  }

  for (label = &qjs_encoding_labels[0]; label->name.length != 0; label++) {
    if (njs_strstr_eq(&str, &label->name)) {
      td->encoding = label->encoding;
      JS_FreeCString(cx, (char *)str.start);
      return 0;
    }
  }

  JS_ThrowTypeError(cx, "The \"%.*s\" encoding is not supported",
                    (int)str.length, str.start);
  JS_FreeCString(cx, (char *)str.start);

  return -1;
}

static int qjs_text_decoder_options(JSContext *cx, int argc, JSValueConst *argv,
                                    qjs_text_decoder_t *td) {
  JSValue val;

  if (argc < 2) {
    td->fatal = 0;
    td->ignore_bom = 0;

    return 0;
  }

  val = JS_GetPropertyStr(cx, argv[1], "fatal");
  if (JS_IsException(val)) {
    return -1;
  }

  td->fatal = JS_ToBool(cx, val);
  JS_FreeValue(cx, val);

  val = JS_GetPropertyStr(cx, argv[1], "ignoreBOM");
  if (JS_IsException(val)) {
    return -1;
  }

  td->ignore_bom = JS_ToBool(cx, val);
  JS_FreeValue(cx, val);

  return 0;
}

static void njs_utf8_decode_init(njs_unicode_decode_t *ctx) {
  ctx->need = 0x00;
  ctx->lower = 0x00;
  ctx->codepoint = 0;
}

static size_t njs_utf8_bom(const u_char *start, const u_char *end) {
  if (start + 3 > end) {
    return 0;
  }

  if (start[0] == 0xEF && start[1] == 0xBB && start[2] == 0xBF) {
    return 3;
  }

  return 0;
}

static int njs_utf8_boundary(njs_unicode_decode_t *ctx, const u_char **data,
                             unsigned *need, u_char lower, u_char upper) {
  u_char ch;

  ch = **data;

  if (ch < lower || ch > upper) {
    return -1 /*NJS_ERROR*/;
  }

  (*data)++;
  (*need)--;
  ctx->codepoint = (ctx->codepoint << 6) | (ch & 0x3F);

  return 0 /*NJS_OK*/;
}

static void njs_utf8_boundary_set(njs_unicode_decode_t *ctx, const u_char ch,
                                  u_char first, u_char second, u_char lower,
                                  u_char upper) {
  if (ch == first) {
    ctx->lower = lower;
    ctx->upper = 0xBF;

  } else if (ch == second) {
    ctx->lower = 0x80;
    ctx->upper = upper;
  }
}

static uint32_t njs_utf8_decode(njs_unicode_decode_t *ctx, const u_char **start,
                                const u_char *end) {
  u_char c;
  unsigned need;
  int ret;
  const u_char *p;

  if (ctx->need != 0) {
    need = ctx->need;
    ctx->need = 0;

    if (ctx->lower != 0x00) {
      ret = njs_utf8_boundary(ctx, start, &need, ctx->lower, ctx->upper);
      if (njs_slow_path(ret != 0 /*NJS_OK*/)) {
        goto failed;
      }

      ctx->lower = 0x00;
    }

    goto decode;
  }

  c = *(*start)++;

  if (c < 0x80) {
    return c;

  } else if (c <= 0xDF) {
    if (c < 0xC2) {
      return NJS_UNICODE_ERROR;
    }

    need = 1;
    ctx->codepoint = c & 0x1F;

  } else if (c < 0xF0) {
    need = 2;
    ctx->codepoint = c & 0x0F;

    if (*start == end) {
      njs_utf8_boundary_set(ctx, c, 0xE0, 0xED, 0xA0, 0x9F);
      goto next;
    }

    ret = 0 /*NJS_OK*/;

    if (c == 0xE0) {
      ret = njs_utf8_boundary(ctx, start, &need, 0xA0, 0xBF);

    } else if (c == 0xED) {
      ret = njs_utf8_boundary(ctx, start, &need, 0x80, 0x9F);
    }

    if (njs_slow_path(ret != 0 /*NJS_OK*/)) {
      goto failed;
    }

  } else if (c < 0xF5) {
    need = 3;
    ctx->codepoint = c & 0x07;

    if (*start == end) {
      njs_utf8_boundary_set(ctx, c, 0xF0, 0xF4, 0x90, 0x8F);
      goto next;
    }

    ret = 0 /*NJS_OK*/;

    if (c == 0xF0) {
      ret = njs_utf8_boundary(ctx, start, &need, 0x90, 0xBF);

    } else if (c == 0xF4) {
      ret = njs_utf8_boundary(ctx, start, &need, 0x80, 0x8F);
    }

    if (njs_slow_path(ret != 0 /*NJS_OK*/)) {
      goto failed;
    }

  } else {
    return NJS_UNICODE_ERROR;
  }

decode:

  for (p = *start; p < end; p++) {
    c = *p;

    if (c < 0x80 || c > 0xBF) {
      *start = p;

      goto failed;
    }

    ctx->codepoint = (ctx->codepoint << 6) | (c & 0x3F);

    if (--need == 0) {
      *start = p + 1;

      return ctx->codepoint;
    }
  }

  *start = p;

next:

  ctx->need = need;

  return NJS_UNICODE_CONTINUE;

failed:

  ctx->lower = 0x00;
  ctx->need = 0;

  return NJS_UNICODE_ERROR;
}

static size_t njs_utf8_size(uint32_t cp) {
  return (cp < 0x80) ? 1 : ((cp < 0x0800) ? 2 : ((cp < 0x10000) ? 3 : 4));
}

static ssize_t njs_utf8_stream_length(njs_unicode_decode_t *ctx,
                                      const u_char *p, size_t len, bool last,
                                      bool fatal, size_t *out_size) {
  size_t size, length;
  uint32_t codepoint;
  const u_char *end;

  size = 0;
  length = 0;

  if (p != NULL) {
    end = p + len;

    while (p < end) {
      codepoint = njs_utf8_decode(ctx, &p, end);

      if (codepoint > NJS_UNICODE_MAX_CODEPOINT) {
        if (codepoint == NJS_UNICODE_CONTINUE) {
          break;
        }

        if (fatal) {
          return -1;
        }

        codepoint = NJS_UNICODE_REPLACEMENT;
      }

      size += njs_utf8_size(codepoint);
      length++;
    }
  }

  if (last && ctx->need != 0x00) {
    if (fatal) {
      return -1;
    }

    size += njs_utf8_size(NJS_UNICODE_REPLACEMENT);
    length++;
  }

  if (out_size != NULL) {
    *out_size = size;
  }

  return length;
}

static unsigned char *njs_utf8_encode(u_char *p, uint32_t u) {
  if (u < 0x80) {
    *p++ = (u_char)(u & 0xFF);
    return p;
  }

  if (u < 0x0800) {
    *p++ = (u_char)((u >> 6) | 0xC0);
    *p++ = (u_char)((u & 0x3F) | 0x80);
    return p;
  }

  if (u < 0x10000) {
    *p++ = (u_char)((u >> 12) | 0xE0);
    *p++ = (u_char)(((u >> 6) & 0x3F) | 0x80);
    *p++ = (u_char)((u & 0x3F) | 0x80);
    return p;
  }

  if (u < 0x110000) {
    *p++ = (u_char)((u >> 18) | 0xF0);
    *p++ = (u_char)(((u >> 12) & 0x3F) | 0x80);
    *p++ = (u_char)(((u >> 6) & 0x3F) | 0x80);
    *p++ = (u_char)((u & 0x3F) | 0x80);
    return p;
  }

  return NULL;
}

static unsigned char *njs_utf8_stream_encode(njs_unicode_decode_t *ctx,
                                             const u_char *start,
                                             const u_char *end, u_char *dst,
                                             bool last, bool fatal) {
  uint32_t cp;

  while (start < end) {
    cp = njs_utf8_decode(ctx, &start, end);

    if (cp > NJS_UNICODE_MAX_CODEPOINT) {
      if (cp == NJS_UNICODE_CONTINUE) {
        break;
      }

      if (fatal) {
        return NULL;
      }

      cp = NJS_UNICODE_REPLACEMENT;
    }

    dst = njs_utf8_encode(dst, cp);
  }

  if (last && ctx->need != 0x00) {
    if (fatal) {
      return NULL;
    }

    dst = njs_utf8_encode(dst, NJS_UNICODE_REPLACEMENT);
  }

  return dst;
}

static JSValue qjs_text_decoder_ctor(JSContext *cx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  JSValue obj;
  qjs_text_decoder_t *td;

  JS_NewClassID(&text_decoder_class_id);

  obj = JS_NewObjectClass(cx, text_decoder_class_id);
  if (JS_IsException(obj)) {
    return JS_EXCEPTION;
  }

  td = js_mallocz(cx, sizeof(qjs_text_decoder_t));
  if (td == NULL) {
    JS_ThrowOutOfMemory(cx);
    JS_FreeValue(cx, obj);
    return JS_EXCEPTION;
  }

  if (qjs_text_decoder_encoding_arg(cx, argc, argv, td) < 0) {
    js_free(cx, td);
    JS_FreeValue(cx, obj);
    return JS_EXCEPTION;
  }

  if (qjs_text_decoder_options(cx, argc, argv, td) < 0) {
    js_free(cx, td);
    JS_FreeValue(cx, obj);
    return JS_EXCEPTION;
  }

  njs_utf8_decode_init(&td->ctx);

  JS_SetOpaque(obj, td);

  return obj;
}

#define njs_nitems(x) (sizeof(x) / sizeof((x)[0]))

int qjs_add_intrinsic_text_decoder(JSContext *cx, JSValueConst global) {
  JSValue ctor, proto;

  JS_NewClassID(&text_decoder_class_id);

  if (JS_NewClass(JS_GetRuntime(cx), text_decoder_class_id,
                  &qjs_text_decoder_class) < 0) {
    return -1;
  }

  proto = JS_NewObject(cx);
  if (JS_IsException(proto)) {
    return -1;
  }

  JS_SetPropertyFunctionList(cx, proto, qjs_text_decoder_proto,
                             njs_nitems(qjs_text_decoder_proto));

  JS_SetClassProto(cx, text_decoder_class_id, proto);

  ctor = JS_NewCFunction2(cx, qjs_text_decoder_ctor, "TextDecoder", 2,
                          JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) {
    return -1;
  }

  JS_SetConstructor(cx, ctor, proto);

  return JS_SetPropertyStr(cx, global, "TextDecoder", ctor);
}

static JSValue qjs_text_encoder_ctor(JSContext *cx, JSValueConst this_val,
                                     int argc, JSValueConst *argv) {
  JSValue obj;

  JS_NewClassID(&text_encoder_class_id);

  obj = JS_NewObjectClass(cx, text_encoder_class_id);
  if (JS_IsException(obj)) {
    return JS_EXCEPTION;
  }

  JS_SetOpaque(obj, (void *)1);

  return obj;
}

int qjs_add_intrinsic_text_encoder(JSContext *cx, JSValueConst global) {
  JSValue ctor, proto;

  proto = JS_NewObject(cx);
  if (JS_IsException(proto)) {
    return -1;
  }

  JS_SetPropertyFunctionList(cx, proto, qjs_text_encoder_proto,
                             njs_nitems(qjs_text_encoder_proto));

  JS_NewClassID(&text_encoder_class_id);

  JS_SetClassProto(cx, text_encoder_class_id, proto);

  ctor = JS_NewCFunction2(cx, qjs_text_encoder_ctor, "TextEncoder", 0,
                          JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) {
    return -1;
  }

  JS_SetConstructor(cx, ctor, proto);

  return JS_SetPropertyStr(cx, global, "TextEncoder", ctor);
}

static JSValue qjs_text_encoder_encoding(JSContext *ctx,
                                         JSValueConst this_val) {
  return JS_NewString(ctx, "utf-8");
}

static JSValue qjs_text_encoder_encode(JSContext *cx, JSValueConst this_val,
                                       int argc, JSValueConst *argv) {
  void *te;
  JSValue len, ta, ret;
  njs_str_t utf8, dst;

  JS_NewClassID(&text_encoder_class_id);

  te = JS_GetOpaque(this_val, text_encoder_class_id);
  if (te == NULL) {
    return JS_ThrowInternalError(cx, "'this' is not a TextEncoder");
  }

  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(cx, "The input argument must be a string");
  }

  utf8.start = (unsigned char *)JS_ToCStringLen(cx, &utf8.length, argv[0]);
  if (utf8.start == NULL) {
    return JS_EXCEPTION;
  }

  len = JS_NewInt64(cx, utf8.length);

  ta = qjs_new_uint8_array(cx, 1, &len);
  if (JS_IsException(ta)) {
    JS_FreeCString(cx, (char *)utf8.start);
    return ta;
  }

  ret = qjs_typed_array_data(cx, ta, &dst);
  if (JS_IsException(ret)) {
    JS_FreeCString(cx, (char *)utf8.start);
    return ret;
  }

  memcpy(dst.start, utf8.start, utf8.length);
  JS_FreeCString(cx, (char *)utf8.start);

  return ta;
}

static int qjs_is_uint8_array(JSContext *cx, JSValueConst value) {
  int ret;
  JSValue ctor, global;

  global = JS_GetGlobalObject(cx);

  ctor = JS_GetPropertyStr(cx, global, "Uint8Array");
  if (JS_IsException(ctor)) {
    JS_FreeValue(cx, global);
    return -1;
  }

  ret = JS_IsInstanceOf(cx, value, ctor);
  JS_FreeValue(cx, ctor);
  JS_FreeValue(cx, global);

  return ret;
}

static JSValue qjs_text_encoder_encode_into(JSContext *cx,
                                            JSValueConst this_val, int argc,
                                            JSValueConst *argv) {
  int read, written;
  void *te;
  size_t size;
  unsigned char *to, *to_end;
  JSValue ret;
  uint32_t cp;
  njs_str_t utf8, dst;
  const unsigned char *start, *end;
  njs_unicode_decode_t ctx;

  JS_NewClassID(&text_encoder_class_id);

  te = JS_GetOpaque(this_val, text_encoder_class_id);
  if (te == NULL) {
    return JS_ThrowInternalError(cx, "'this' is not a TextEncoder");
  }

  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(cx, "The input argument must be a string");
  }

  ret = qjs_typed_array_data(cx, argv[1], &dst);
  if (JS_IsException(ret)) {
    return ret;
  }

  if (!qjs_is_uint8_array(cx, argv[1])) {
    return JS_ThrowTypeError(cx, "The output argument must be a"
                                 " Uint8Array");
  }

  utf8.start = (unsigned char *)JS_ToCStringLen(cx, &utf8.length, argv[0]);
  if (utf8.start == NULL) {
    return JS_EXCEPTION;
  }

  read = 0;
  written = 0;

  start = utf8.start;
  end = start + utf8.length;

  to = dst.start;
  to_end = to + dst.length;

  njs_utf8_decode_init(&ctx);

  while (start < end) {
    cp = njs_utf8_decode(&ctx, &start, end);

    if (cp > NJS_UNICODE_MAX_CODEPOINT) {
      cp = NJS_UNICODE_REPLACEMENT;
    }

    size = njs_utf8_size(cp);

    if (to + size > to_end) {
      break;
    }

    read += (cp > 0xFFFF) ? 2 : 1;
    written += size;

    to = njs_utf8_encode(to, cp);
  }

  JS_FreeCString(cx, (char *)utf8.start);

  ret = JS_NewObject(cx);
  if (JS_IsException(ret)) {
    return ret;
  }

  if (JS_DefinePropertyValueStr(cx, ret, "read", JS_NewInt32(cx, read),
                                JS_PROP_C_W_E) < 0) {
    JS_FreeValue(cx, ret);
    return JS_EXCEPTION;
  }

  if (JS_DefinePropertyValueStr(cx, ret, "written", JS_NewInt32(cx, written),
                                JS_PROP_C_W_E) < 0) {
    JS_FreeValue(cx, ret);
    return JS_EXCEPTION;
  }

  return ret;
}
