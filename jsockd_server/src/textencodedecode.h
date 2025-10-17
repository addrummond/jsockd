#ifndef TEXTENCODEDECODE_H
#define TEXTENCODEDECODE_H

#include "quickjs.h"

int qjs_add_intrinsic_text_decoder(JSContext *cx, JSValueConst global);
int qjs_add_intrinsic_text_encoder(JSContext *cx, JSValueConst global);

#endif
