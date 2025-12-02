// AI-generated nonsense, edited down to remove some heroic attempts to add
// legacy MSVC support.
#ifndef TYPEOFSHIM_H
#define TYPEOFSHIM_H

/* Detect C23: its __STDC_VERSION__ will be 202311L (or later revisions). */
#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L)
/* Standard typeof is available (C23). */
#ifndef TYPEOF
#define TYPEOF(x) typeof(x)
#endif
#elif defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER) ||  \
    defined(__INTEL_LLVM_COMPILER) || defined(__ICC) || defined(__PGI) ||      \
    defined(__NVCOMPILER) || defined(__SX__) || defined(__HP_cc) ||            \
    defined(__HP_aCC)
/* GCC/Clang/ICC and several other compilers support __typeof__ */
#ifndef TYPEOF
#define TYPEOF(x) __typeof__(x)
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

#endif /* TYPEOF_SHIM_H */
