// AI-generated nonsense. Looks about right!
#ifndef TYPEOFSHIM_H
#define TYPEOFSHIM_H

/* Detect C23: its __STDC_VERSION__ will be 202311L (or later revisions). */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L)
    /* Standard typeof is available (C23). */
    #ifndef TYPEOF
    #define TYPEOF(x) typeof(x)
    #endif

#elif defined(__cplusplus)
    /* C++: use decltype */
    #ifndef TYPEOF
    #define TYPEOF(x) decltype(x)
    #endif

#elif defined(__GNUC__)      || defined(__clang__)        || \
      defined(__INTEL_COMPILER) || defined(__INTEL_LLVM_COMPILER) || \
      defined(__ICC)         || defined(__PGI)            || \
      defined(__NVCOMPILER)  || defined(__SX__)           || \
      defined(__HP_cc)       || defined(__HP_aCC)
    /* GCC/Clang/ICC and several other compilers support __typeof__ */
    #ifndef TYPEOF
    #define TYPEOF(x) __typeof__(x)
    #endif

#elif defined(_MSC_VER)
    /* MSVC:
       - In C++ mode: already handled above.
       - In C mode: historically only C89/partial C11; no typeof.
       We attempt a restricted fallback if _Generic is present. */
    #if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
        /* Limited builtin-only fallback returning a TYPE NAME usable in declarations.
           For unknown types or user-defined types, expands to void (sentinel). */
        #ifndef TYPEOF
        #define TYPEOF_BUILTIN_EXPR(x) _Generic((x), \
            _Bool: (_Bool)0, \
            char: (char)0, \
            signed char: (signed char)0, \
            unsigned char: (unsigned char)0, \
            short: (short)0, \
            unsigned short: (unsigned short)0, \
            int: (int)0, \
            unsigned int: (unsigned int)0, \
            long: (long)0, \
            unsigned long: (unsigned long)0, \
            long long: (long long)0, \
            unsigned long long: (unsigned long long)0, \
            float: (float)0, \
            double: (double)0, \
            long double: (long double)0, \
            default: (void)0 \
        )
        /* We cannot yield a raw type token; we yield an expression whose type is the desired type.
           So define TYPEOF(x) to be 'typeof_builtin_fallback_t *' pattern if you need only sizeof/type traits.
           Most places expecting a *type* (like variable declaration) cannot use this fallback.
           Provide a helper macro for sizeof. */
        #define TYPEOF_EXPR(x) TYPEOF_BUILTIN_EXPR(x)
        /* Intentionally DO NOT define TYPEOF(x) to avoid misleading usage in declarations. */
        #endif
    #else
        #error "No typeof/decltype/_Generic available for MSVC C; cannot provide TYPEOF."
    #endif

#else
    /* Unknown compiler:
       Try __typeof__ if it exists (some compilers define it silently). */
    #ifdef __has_extension
        #if __has_extension(typeof)
            #define TYPEOF(x) __typeof__(x)
        #endif
    #endif
    #ifndef TYPEOF
        /* Final fallback: supported compilers should have been caught above. */
        #error "Compiler lacks typeof/decltype; provide your own mapping or upgrade."
    #endif
#endif

/* Optional helpers */

/* TYPEOF_UNQUAL(x):
   Produces the unqualified (non-volatile) version of the type for GCC/Clang/etc.
   Trick: unary + applies usual arithmetic conversions and strips top-level qualifiers
   (and array -> pointer decay). Use carefully. */
#if defined(TYPEOF)
    #define TYPEOF_UNQUAL(x) TYPEOF(+(x))
#endif

/* Declare a variable of same type as expression 'expr'. */
#if defined(TYPEOF)
    #define TYPEOF_VAR(name, expr) TYPEOF(expr) name
#endif

/* Copy const/volatile qualifiers from source expression onto TYPEOF(expr). */
#if defined(TYPEOF)
    #define TYPEOF_SAME_QUAL(src, expr) TYPEOF(src) /* rely on typeof including qualifiers */ expr
#endif

#endif /* TYPEOF_SHIM_H */
