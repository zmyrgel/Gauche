/*
 * number.c - numeric functions
 *
 *   Copyright (c) 2000-2006 Shiro Kawai, All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the authors nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  $Id: number.c,v 1.138 2007-01-12 01:05:45 shirok Exp $
 */

#include <math.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>
#include <float.h>
#define LIBGAUCHE_BODY
#include "gauche.h"
#include "gauche/bignum.h"
#include "gauche/scmconst.h"

#ifdef HAVE_SUNMATH_H
#include "sunmath.h"            /* for isinf() on Solaris */
#endif /* HAVE_SUNMATH_H */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef HAVE_ISNAN
#define SCM_IS_NAN(x)  isnan(x)
#else
#define SCM_IS_NAN(x)  (!((x)==(x)))
#endif

#ifdef HAVE_ISINF
#define SCM_IS_INF(x)  isinf(x)
#else
#define SCM_IS_INF(x)  Scm_IsInf(x)
/* NB: If we inline this, some version of gcc incorrectly assumes
   the condition would never be satisfied and optimize it away. */
int Scm_IsInf(double x)
{
    return ((x) != 0 && (x) == (x)/2.0);
}
#endif

#define RADIX_MIN 2
#define RADIX_MAX 36

/* Maximum allowable range of exponent in the number litereal.
   For flonums, IEEE double can support [-323..308].  For exact
   numbers we can go futher, but it would easily consume huge
   memory.  So I assume it is reasonable to limit its range. */
#define MAX_EXPONENT  324


/* Linux gcc have those, but the declarations aren't included unless
   __USE_ISOC9X is defined.  Just in case. */
#ifdef HAVE_TRUNC
extern double trunc(double);
#endif

#ifdef HAVE_RINT
extern double rint(double);
#define roundeven rint
#else
static double roundeven(double);
#endif

/*
 * Classes of Numeric Tower
 */

static ScmClass *numeric_cpl[] = {
    SCM_CLASS_STATIC_PTR(Scm_RationalClass),
    SCM_CLASS_STATIC_PTR(Scm_RealClass),
    SCM_CLASS_STATIC_PTR(Scm_ComplexClass),
    SCM_CLASS_STATIC_PTR(Scm_NumberClass),
    SCM_CLASS_STATIC_PTR(Scm_TopClass),
    NULL
};

static void number_print(ScmObj obj, ScmPort *port, ScmWriteContext *ctx);

SCM_DEFINE_BUILTIN_CLASS(Scm_NumberClass, number_print, NULL, NULL, NULL,
                         numeric_cpl+4);
SCM_DEFINE_BUILTIN_CLASS(Scm_ComplexClass, number_print, NULL, NULL, NULL,
                         numeric_cpl+3);
SCM_DEFINE_BUILTIN_CLASS(Scm_RealClass, number_print, NULL, NULL, NULL,
                         numeric_cpl+2);
SCM_DEFINE_BUILTIN_CLASS(Scm_RationalClass, number_print, NULL, NULL, NULL,
                         numeric_cpl+1);
SCM_DEFINE_BUILTIN_CLASS(Scm_IntegerClass, number_print, NULL, NULL, NULL,
                         numeric_cpl);

/*=====================================================================
 *  Generic Arithmetic
 */

/* Some arithmetic operations calls the corresponding generic function
 * if the operand is not a number.
 */

/* Fallback Gf */
static ScmObj bad_number_method(ScmObj *args, int nargs, ScmGeneric *gf)
{
    const char *fn = (const char *)SCM_GENERIC_DATA(gf);
    if (nargs == 1) {
        Scm_Error("operation %s is not defined on object %S", fn, args[0]);
    } else if (nargs == 2) {
        Scm_Error("operation %s is not defined between %S and %S",
                  fn, args[0], args[1]);
    } else {
        Scm_Error("generic function for %s is called with args %S",
                  fn, Scm_ArrayToList(args, nargs));
    }
    return SCM_UNDEFINED;
}
static SCM_DEFINE_GENERIC(generic_add, bad_number_method, "+");
static SCM_DEFINE_GENERIC(generic_sub, bad_number_method, "-");
static SCM_DEFINE_GENERIC(generic_mul, bad_number_method, "*");
static SCM_DEFINE_GENERIC(generic_div, bad_number_method, "/");

/*=====================================================================
 *  Flonums
 */

ScmObj Scm_MakeFlonum(double d)
{
    ScmFlonum *f = SCM_NEW(ScmFlonum);
    SCM_SET_CLASS(f, SCM_CLASS_REAL);
    f->value = d;
    return SCM_OBJ(f);
}

ScmObj Scm_MakeFlonumToNumber(double d, int exact)
{
    if (exact && !SCM_IS_INF(d)) {
        /* see if d can be demoted to integer */
        double i, f;
        f = modf(d, &i);
        if (f == 0.0) {
            if (i > SCM_SMALL_INT_MAX || i < SCM_SMALL_INT_MIN) {
                return Scm_MakeBignumFromDouble(i);
            } else {
                return SCM_MAKE_INT((long)i);
            }
        }
    }
    return Scm_MakeFlonum(d);
}

/* Decompose flonum D into an integer mantissa F and exponent E, where
 *   -1022 <= E <= 1023,
 *    0 <= abs(F) < 2^53
 *    D = F * 2^(E - 53)
 * Some special cases:
 *    F = 0, E = 0 if D = 0.0 or -0.0
 *    F = #t if D is infinity (positive or negative)
 *    F = #f if D is NaN.
 * If D is normalized number, F >= 2^52.
 *
 * Cf. IEEE 754 Reference
 * http://babbage.cs.qc.edu/courses/cs341/IEEE-754references.html
 */
union ieee_double {
    double d;
    struct {
#ifdef DOUBLE_ARMENDIAN
        /* ARM's mixed endian.  TODO: what if we have LP64 ARM? */
        unsigned long mant0:20;
        unsigned int exp:11;
        unsigned int sign:1;
        unsigned long mant1:32;
#else  /*!DOUBLE_ARMENDIAN*/
#ifdef WORDS_BIGENDIAN
#if SIZEOF_LONG >= 8
        unsigned int sign:1;
        unsigned int exp:11;
        unsigned long mant:52;
#else  /*SIZEOF_LONG < 8*/
        unsigned int sign:1;
        unsigned int exp:11;
        unsigned long mant0:20;
        unsigned long mant1:32;
#endif /*SIZEOF_LONG < 8*/
#else  /*!WORDS_BIGENDIAN*/
#if SIZEOF_LONG >= 8
        unsigned long mant:52;
        unsigned int  exp:11;
        unsigned int  sign:1;
#else  /*SIZEOF_LONG < 8*/
        unsigned long mant1:32;
        unsigned long mant0:20;
        unsigned int  exp:11;
        unsigned int  sign:1;
#endif /*SIZEOF_LONG < 8*/
#endif /*!WORDS_BIGENDIAN*/
#endif /*!DOUBLE_ARMENDIAN*/
    } components;
};

ScmObj Scm_DecodeFlonum(double d, int *exp, int *sign)
{
    union ieee_double dd;
    ScmObj f;
    
    dd.d = d;

    *sign = (dd.components.sign? -1 : 1);

    /* Check exceptional cases */
    if (dd.components.exp == 0x7ff) {
        *exp = 0;
        if (
#if SIZEOF_LONG >= 8
            dd.components.mant == 0
#else  /*SIZEOF_LONG < 8*/
            dd.components.mant0 == 0 && dd.components.mant1 == 0
#endif /*SIZEOF_LONG < 8*/
            ) {
            return SCM_TRUE;  /* infinity */
        } else {
            return SCM_FALSE; /* NaN */
        }
    }

    *exp  = (dd.components.exp? dd.components.exp - 0x3ff - 52 : -0x3fe - 52);
    
#if SIZEOF_LONG >= 8
    {
        unsigned long lf = dd.components.mant;
        if (dd.components.exp > 0) {
            lf += (1L<<52);     /* hidden bit */
        }
        f = Scm_MakeInteger(lf);
    }
#else  /*SIZEOF_LONG < 8*/
    {
        unsigned long values[2];
        values[0] = dd.components.mant1;
        values[1] = dd.components.mant0;
        if (dd.components.exp > 0) {
            values[1] += (1L<<20); /* hidden bit */
        }
        f = Scm_NormalizeBignum(SCM_BIGNUM(Scm_MakeBignumFromUIArray(1, values, 2)));
    }
#endif /*SIZEOF_LONG < 8*/
    return f;
}

/*=====================================================================
 *  Ratnums
 */

/* possibly returns denomalized number */
ScmObj Scm_MakeRatnum(ScmObj numer, ScmObj denom)
{
    ScmRatnum *r;
    if (!SCM_INTEGERP(numer)) {
        Scm_Error("numerator must be an exact integer, but got %S", numer);
    }
    if (!SCM_INTEGERP(denom)) {
        Scm_Error("denominator must be an exact integer, but got %S", denom);
    }
    r = SCM_NEW(ScmRatnum);
    SCM_SET_CLASS(r, SCM_CLASS_RATIONAL);
    r->numerator = numer;
    r->denominator = denom;
    return SCM_OBJ(r);
}

#define ENSURE_RATNUM(integer) \
    SCM_RATNUM(Scm_MakeRatnum(integer, SCM_MAKE_INT(1)))

ScmObj Scm_MakeRational(ScmObj numer, ScmObj denom)
{
    if (!SCM_INTEGERP(numer)) {
        Scm_Error("numerator must be an exact integer, but got %S", numer);
    }
    if (!SCM_INTEGERP(denom)) {
        Scm_Error("denominator must be an exact integer, but got %S", denom);
    }
    if (SCM_EXACT_ONE_P(denom)) return numer;
    if (SCM_EXACT_ZERO_P(numer)) return SCM_MAKE_INT(0);
    else return Scm_ReduceRational(Scm_MakeRatnum(numer, denom));
}

ScmObj Scm_Numerator(ScmObj n)
{
    if (SCM_RATNUMP(n)) {
        return SCM_RATNUM_NUMER(n);
    }
    if (SCM_NUMBERP(n)) {
        return n;
    }
    Scm_Error("number required, but got %S", n);
    return SCM_UNDEFINED;       /* dummy */
}

ScmObj Scm_Denominator(ScmObj n)
{
    if (SCM_RATNUMP(n)) {
        return SCM_RATNUM_DENOM(n);
    }
    if (SCM_INTEGERP(n)) {
        return SCM_MAKE_INT(1);
    }
    if (SCM_NUMBERP(n)) {
        return Scm_MakeFlonum(1.0);
    }
    Scm_Error("number required, but got %S", n);
    return SCM_UNDEFINED;       /* dummy */
}

ScmObj Scm_ReduceRational(ScmObj rational)
{
    ScmObj numer, denom;
    ScmObj common;              /* common divisor */
    int negated = FALSE;
    
    if (SCM_INTEGERP(rational)) return rational;
    if (!SCM_RATNUMP(rational)) {
        Scm_Error("exact rational number required, but got %S", rational);
    }
    numer = SCM_RATNUM_NUMER(rational);
    denom = SCM_RATNUM_DENOM(rational);

    if (Scm_Sign(denom) < 0) {
        numer = Scm_Negate(numer);
        denom = Scm_Negate(denom);
        negated = TRUE;
    }

    /* special cases */
    if (SCM_EXACT_ONE_P(denom)) return numer;
    if (SCM_EXACT_ZERO_P(denom)) {
        int s = Scm_Sign(numer);
        if (s > 0) return SCM_POSITIVE_INFINITY;
        if (s < 0) return SCM_NEGATIVE_INFINITY;
        return SCM_NAN;
    }
    
    common = Scm_Gcd(numer, denom);
    if (SCM_EXACT_ONE_P(common)) {
        if (negated) {
            return Scm_MakeRatnum(numer, denom);
        } else {
            return rational;
        }
    } else {
        numer = Scm_Quotient(numer, common, NULL);
        denom = Scm_Quotient(denom, common, NULL);
        if (SCM_EQ(denom, SCM_MAKE_INT(1))) {
            return numer;
        } else {
            return Scm_MakeRatnum(numer, denom);
        }
    }
}

/* x, y must be exact numbers */
ScmObj Scm_RatnumAddSub(ScmObj x, ScmObj y, int subtract)
{
    ScmObj nx = SCM_RATNUMP(x)? SCM_RATNUM_NUMER(x) : x;
    ScmObj dx = SCM_RATNUMP(x)? SCM_RATNUM_DENOM(x) : SCM_MAKE_INT(1);
    ScmObj ny = SCM_RATNUMP(y)? SCM_RATNUM_NUMER(y) : y;
    ScmObj dy = SCM_RATNUMP(y)? SCM_RATNUM_DENOM(y) : SCM_MAKE_INT(1);
    ScmObj gcd, fx, fy, nr, dr;

    /* shortcut */
    if (Scm_NumEq(dx, dy)) {
        dr = dx;
        goto finish;
    }

    if (SCM_EXACT_ONE_P(dx)||SCM_EXACT_ONE_P(dx)) gcd = SCM_MAKE_INT(1);
    else gcd = Scm_Gcd(dx, dy);
    if (Scm_NumEq(dx, gcd)) {
        /* only factor x */
        nx = Scm_Mul(Scm_Quotient(dy, dx, NULL), nx);
        dr = dy;
        goto finish;
    }
    if (Scm_NumEq(dy, gcd)) {
        /* only factor y */
        ny = Scm_Mul(Scm_Quotient(dx, dy, NULL), ny);
        dr = dx;
        goto finish;
    }

    /* general case */
    fx = Scm_Quotient(dx, gcd, NULL);
    fy = Scm_Quotient(dy, gcd, NULL);
    nx = Scm_Mul(nx, fy);
    ny = Scm_Mul(ny, fx);
    dr = Scm_Mul(dx, fy);
  finish:
    nr = (subtract? Scm_Sub(nx, ny) : Scm_Add(nx, ny));
    return Scm_MakeRational(nr, dr);
}

ScmObj Scm_RatnumMulDiv(ScmObj x, ScmObj y, int divide)
{
    ScmObj nx, ny, dx, dy;
    nx = SCM_RATNUMP(x)? SCM_RATNUM_NUMER(x) : x;
    dx = SCM_RATNUMP(x)? SCM_RATNUM_DENOM(x) : SCM_MAKE_INT(1);
    ny = SCM_RATNUMP(y)? SCM_RATNUM_NUMER(y) : y;
    dy = SCM_RATNUMP(y)? SCM_RATNUM_DENOM(y) : SCM_MAKE_INT(1);
    
    if (divide) {
        ScmObj t = ny; ny = dy; dy = t;
    }
    return Scm_MakeRational(Scm_Mul(nx, ny),
                            Scm_Mul(dx, dy));
}

#define Scm_RatnumAdd(x, y)  Scm_RatnumAddSub(x, y, FALSE)
#define Scm_RatnumSub(x, y)  Scm_RatnumAddSub(x, y, TRUE)
#define Scm_RatnumMul(x, y)  Scm_RatnumMulDiv(x, y, FALSE)
#define Scm_RatnumDiv(x, y)  Scm_RatnumMulDiv(x, y, TRUE)


/*=======================================================================
 *  Compnums
 */

ScmObj Scm_MakeCompnum(double r, double i)
{
    ScmCompnum *c = SCM_NEW_ATOMIC(ScmCompnum);
    SCM_SET_CLASS(c, SCM_CLASS_COMPLEX);
    c->real = r;
    c->imag = i;
    return SCM_OBJ(c);
}

ScmObj Scm_MakeComplex(double r, double i)
{
    if (i == 0.0) return Scm_MakeFlonum(r);
    else          return Scm_MakeCompnum(r, i);
}

ScmObj Scm_MakeComplexPolar(double mag, double angle)
{
    double real = mag * cos(angle);
    double imag = mag * sin(angle);
    if (imag == 0.0) return Scm_MakeFlonum(real);
    else             return Scm_MakeCompnum(real, imag);
}

double Scm_RealPart(ScmObj z)
{
    double m;
    if (SCM_REALP(z)) {
        m = Scm_GetDouble(z);
    } else if (!SCM_COMPNUMP(z)) {
        Scm_Error("number required, but got %S", z);
        m = 0.0;                /* dummy */
    } else {
        m = SCM_COMPNUM_REAL(z);
    }
    return m;
}

double Scm_ImagPart(ScmObj z)
{
    double m = 0.0;
    if (SCM_COMPNUMP(z)) {
        m = SCM_COMPNUM_IMAG(z);
    } else if (!SCM_REALP(z)) {
        Scm_Error("number required, but got %S", z);
    }
    return m;
}

double Scm_Magnitude(ScmObj z)
{
    double m;
    if (SCM_REALP(z)) {
        m = fabs(Scm_GetDouble(z));
    } else if (!SCM_COMPNUMP(z)) {
        Scm_Error("number required, but got %S", z);
        m = 0.0;                /* dummy */
    } else {
        double r = SCM_COMPNUM_REAL(z);
        double i = SCM_COMPNUM_IMAG(z);
        m = sqrt(r*r+i*i);
    }
    return m;
}

double Scm_Angle(ScmObj z)
{
    double a;
    if (SCM_REALP(z)) {
        a = (Scm_Sign(z) < 0)? M_PI : 0.0;
    } else if (!SCM_COMPNUMP(z)) {
        Scm_Error("number required, but got %S", z);
        a = 0.0;                /* dummy */
    } else {
        double r = SCM_COMPNUM_REAL(z);
        double i = SCM_COMPNUM_IMAG(z);
        a = atan2(i, r);
    }
    return a;
}

/*=======================================================================
 *  Coertion
 */

ScmObj Scm_MakeInteger(long i)
{
    if (i >= SCM_SMALL_INT_MIN && i <= SCM_SMALL_INT_MAX) {
        return SCM_MAKE_INT(i);
    } else {
        return Scm_MakeBignumFromSI(i);
    }
}

ScmObj Scm_MakeIntegerU(u_long i)
{
    if (i <= (u_long)SCM_SMALL_INT_MAX) return SCM_MAKE_INT(i);
    else return Scm_MakeBignumFromUI(i);
}

/* Convert scheme integer to C integer */
long Scm_GetIntegerClamp(ScmObj obj, int clamp, int *oor)
{
    double v = 0.0;
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) return SCM_INT_VALUE(obj);
    else if (SCM_BIGNUMP(obj)) {
        return Scm_BignumToSI(SCM_BIGNUM(obj), clamp, oor);
    }
    else if (SCM_FLONUMP(obj)) {
        v = SCM_FLONUM_VALUE(obj);
        goto flonum;
    }
    else if (SCM_RATNUMP(obj)) {
        v = Scm_GetDouble(obj);
        goto flonum;
    }
    else {
        goto err;
    }
  flonum:    
    if (v > (double)LONG_MAX) {
        if (clamp & SCM_CLAMP_HI) return LONG_MAX;
        else goto err;
    }
    if (v < (double)LONG_MIN) {
        if (clamp & SCM_CLAMP_LO) return LONG_MIN;
        else goto err;
    }
    return (long)v;
  err:
    if (clamp == SCM_CLAMP_NONE && oor != NULL) {
        *oor = TRUE;
    } else {
        Scm_Error("argument out of range: %S", obj);
    }
    return 0;
}

u_long Scm_GetIntegerUClamp(ScmObj obj, int clamp, int *oor)
{
    double v = 0.0;
    
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) {
        if (SCM_INT_VALUE(obj) < 0) {
            if (clamp & SCM_CLAMP_LO) return 0;
            else goto err;
        }
        return SCM_INT_VALUE(obj);
    }
    else if (SCM_BIGNUMP(obj)) {
        return Scm_BignumToUI(SCM_BIGNUM(obj), clamp, oor);
    }
    else if (SCM_FLONUMP(obj)) {
        v = SCM_FLONUM_VALUE(obj);
        goto flonum;
    }
    else if (SCM_RATNUMP(obj)) {
        v = Scm_GetDouble(obj);
        goto flonum;
    }
    else {
        goto err;
    }
  flonum:
    if (v > (double)ULONG_MAX) {
        if (clamp & SCM_CLAMP_HI) return ULONG_MAX;
        else goto err;
    }
    if (v < 0.0) {
        if (clamp & SCM_CLAMP_LO) return 0;
        else goto err;
    }
    return (u_long)v;
  err:
    if (clamp == SCM_CLAMP_NONE && oor != NULL) {
        *oor = TRUE;
    } else {
        Scm_Error("argument out of range: %S", obj);
    }
    return 0;
}

/* 32bit integer specific */
ScmInt32 Scm_GetInteger32Clamp(ScmObj obj, int clamp, int *oor)
{
#if SIZEOF_LONG == 4
    return (ScmInt32)Scm_GetIntegerClamp(obj, clamp, oor);
#else  /* SIZEOF_LONG >= 8 */
    
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    /* NB: we denote the constant directly here.  (1L<<31) fails on
       Alpha machines, since the compiler somehow calculates the constant
       in 32bit integer even it has 'L'.  We have to write (1LL<<31), but
       I'm afraid that it's not portable. */
    if (SCM_INTP(obj)) {
        long r = SCM_INT_VALUE(obj);
        if (r < -0x80000000L) {
            if (clamp & SCM_CLAMP_LO) return -0x80000000L;
            goto err;
        }
        if (r > 0x7fffffffL) {
            if (clamp & SCM_CLAMP_HI) return 0x7fffffffL;
            goto err;
        }
        return r;
    } else if (SCM_BIGNUMP(obj)) {
        if (SCM_BIGNUM_SIGN(obj) < 0) {
            if (clamp & SCM_CLAMP_LO) return -0x80000000L;
            goto err;
        } else {
            if (clamp & SCM_CLAMP_HI) return 0x7fffffffL;
            goto err;
        }
    }
    /*TODO: flonum and ratnum! */
  err:
    if (clamp == SCM_CLAMP_NONE && oor != NULL) {
        *oor = TRUE;
    } else {
        Scm_Error("argument out of range: %S", obj);
    }
    return 0;
#endif /* SIZEOF_LONG >= 8 */
}

ScmUInt32 Scm_GetIntegerU32Clamp(ScmObj obj, int clamp, int *oor)
{
#if SIZEOF_LONG == 4
    return (ScmUInt32)Scm_GetIntegerUClamp(obj, clamp, oor);
#else  /* SIZEOF_LONG >= 8 */
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) {
        long r = SCM_INT_VALUE(obj);
        if (r < 0) {
            if (clamp & SCM_CLAMP_LO) return 0;
            goto err;
        }
        if (r > 0xffffffffUL) {
            if (clamp & SCM_CLAMP_HI) return 0xffffffffUL;
            goto err;
        }
        return r;
    } else if (SCM_BIGNUMP(obj)) {
        if (SCM_BIGNUM_SIGN(obj) < 0) {
            if (clamp & SCM_CLAMP_LO) return 0;
            goto err;
        } else {
            if (clamp & SCM_CLAMP_HI) return 0xffffffffUL;
            goto err;
        }
    }
  err:
    if (clamp == SCM_CLAMP_NONE && oor != NULL) {
        *oor = TRUE;
    } else {
        Scm_Error("argument out of range: %S", obj);
    }
    return 0;
#endif /* SIZEOF_LONG >= 8 */
}


#if SIZEOF_LONG == 4
/* we need special routines */
ScmObj Scm_MakeInteger64(ScmInt64 i)
{
#if SCM_EMULATE_INT64
    u_long val[2];
    if (i.hi == 0) return Scm_MakeInteger(i.lo);
    val[0] = i.lo;
    val[1] = i.hi;
    return Scm_MakeBignumFromUIArray(0, val, 2); /* bignum checks sign */
#else /*SCM_EMULATE_INT64*/
    u_long val[2];
    val[0] = (uint64_t)i & ULONG_MAX;
    val[1] = (uint64_t)i >> 32;
    if (val[1] == 0 && val[0] <= LONG_MAX) return Scm_MakeInteger(val[0]);
    return Scm_NormalizeBignum(SCM_BIGNUM(Scm_MakeBignumFromUIArray(0, val, 2)));
#endif
}

ScmObj Scm_MakeIntegerU64(ScmUInt64 i)
{
#if SCM_EMULATE_INT64
    u_long val[2];
    if (i.hi == 0) return Scm_MakeIntegerU(i.lo);
    val[0] = i.lo;
    val[1] = i.hi;
    return Scm_MakeBignumFromUIArray(1, val, 2);
#else /*SCM_EMULATE_INT64*/
    u_long val[2];
    val[0] = (uint64_t)i & ULONG_MAX;
    val[1] = (uint64_t)i >> 32;
    if (val[1] == 0) return Scm_MakeIntegerU(val[0]);
    return Scm_MakeBignumFromUIArray(1, val, 2);
#endif
}

ScmInt64 Scm_GetInteger64Clamp(ScmObj obj, int clamp, int *oor)
{
#if SCM_EMULATE_INT64
    ScmInt64 r = {0, 0};
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) {
        long v = SCM_INT_VALUE(obj);
        r.lo = v;
        if (v < 0) r.hi = ULONG_MAX;
        return r;
    }
    if (SCM_BIGNUMP(obj)) {
        return Scm_BignumToSI64(SCM_BIGNUM(obj), clamp, oor);
    }
    if (SCM_FLONUMP(obj)) {
        if (Scm_NumCmp(obj, SCM_2_63) >= 0) {
            if (!(clamp&SCM_CLAMP_HI)) goto err;
            SCM_SET_INT64_MAX(r);
            return r;
        } else if (Scm_NumCmp(obj, SCM_MINUS_2_63) < 0) {
            if (!(clamp&SCM_CLAMP_LO)) goto err;
            SCM_SET_INT64_MIN(r);
            return r;
        } else {
            ScmObj b = Scm_MakeBignumFromDouble(SCM_FLONUM_VALUE(obj));
            return Scm_BignumToSI64(SCM_BIGNUM(b), clamp, oor);
        }
    }
#else /*!SCM_EMULATE_INT64*/
    ScmInt64 r = 0;
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) return (ScmInt64)SCM_INT_VALUE(obj);
    if (SCM_BIGNUMP(obj)) {
        return Scm_BignumToSI64(SCM_BIGNUM(obj), clamp, oor);
    }
    if (SCM_FLONUMP(obj)) {
        int64_t maxval, minval;
        double v;
        
        SCM_SET_INT64_MAX(maxval);
        SCM_SET_INT64_MIN(minval);
        v = SCM_FLONUM_VALUE(obj);
        if (v > (double)maxval) {
            if (!(clamp&SCM_CLAMP_HI)) goto err;
            return maxval;
        } else if (v < (double)minval) {
            if (!(clamp&SCM_CLAMP_LO)) goto err;
            return minval;
        } else {
            return (long)v;
        }
    }
#endif /*!SCM_EMULATE_INT64*/
  err:
    if (clamp == SCM_CLAMP_NONE && oor != NULL) {
        *oor = TRUE;
    } else {
        Scm_Error("argument out of range: %S", obj);
    }
    return r;
}
                               
ScmUInt64 Scm_GetIntegerU64Clamp(ScmObj obj, int clamp, int *oor)
{
#if SCM_EMULATE_INT64
    ScmUInt64 r = {0, 0};
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) {
        long v = SCM_INT_VALUE(obj);
        if (v < 0) {
            if (!(clamp&SCM_CLAMP_LO)) goto err;
        } else {
            r.lo = v;
        }
        return r;
    }
    if (SCM_BIGNUMP(obj)) {
        return Scm_BignumToUI64(SCM_BIGNUM(obj), clamp, oor);
    }
    if (SCM_FLONUMP(obj)) {
        if (Scm_NumCmp(obj, SCM_2_64) >= 0) {
            if (!(clamp&SCM_CLAMP_HI)) goto err;
            SCM_SET_UINT64_MAX(r);
            return r;
        } else if (SCM_FLONUM_VALUE(obj) < 0) {
            if (!(clamp&SCM_CLAMP_LO)) goto err;
            return r;
        } else {
            ScmObj b = Scm_MakeBignumFromDouble(SCM_FLONUM_VALUE(obj));
            return Scm_BignumToUI64(SCM_BIGNUM(b), clamp, oor);
        }
    }
#else /*!SCM_EMULATE_INT64*/
    ScmInt64 r = 0;
    if (clamp == SCM_CLAMP_NONE && oor != NULL) *oor = FALSE;
    if (SCM_INTP(obj)) {
        long v = SCM_INT_VALUE(obj);
        if (v < 0) {
            if (!(clamp&SCM_CLAMP_LO)) goto err;
            return 0;
        } else {
            return (ScmUInt64)v;
        }
    }
    if (SCM_BIGNUMP(obj)) {
        return Scm_BignumToUI64(SCM_BIGNUM(obj), clamp, oor);
    }
    if (SCM_FLONUMP(obj)) {
        double v = SCM_FLONUM_VALUE(obj);
        uint64_t maxval;

        if (v < 0) {
            if (!(clamp&SCM_CLAMP_LO)) goto err;
            return 0;
        }
        SCM_SET_UINT64_MAX(maxval);
        if (v > (double)maxval) {
            if (!(clamp&SCM_CLAMP_HI)) goto err;
            return maxval;
        } else {
            return (uint32_t)v;
        }
    }
#endif
  err:
    if (clamp == SCM_CLAMP_NONE && oor != NULL) {
        *oor = TRUE;
    } else {
        Scm_Error("argument out of range: %S", obj);
    }
    return r;
}
                               
#endif /* SIZEOF_LONG == 4 */

double Scm_GetDouble(ScmObj obj)
{
    if (SCM_FLONUMP(obj)) return SCM_FLONUM_VALUE(obj);
    else if (SCM_INTP(obj)) return (double)SCM_INT_VALUE(obj);
    else if (SCM_BIGNUMP(obj)) return Scm_BignumToDouble(SCM_BIGNUM(obj));
    else if (SCM_RATNUMP(obj)) {
        return Scm_GetDouble(SCM_RATNUM_NUMER(obj))/Scm_GetDouble(SCM_RATNUM_DENOM(obj));
    }
    else return 0.0;
}

/*
 *   Generic Methods
 */

/* Predicates */

int Scm_IntegerP(ScmObj obj)
{
    if (SCM_INTP(obj) || SCM_BIGNUMP(obj)) return TRUE;
    if (SCM_RATNUMP(obj)) return FALSE; /* normalized ratnum never be integer */
    if (SCM_FLONUMP(obj)) {
        double d = SCM_FLONUM_VALUE(obj);
        double f, i;
        if ((f = modf(d, &i)) == 0.0) return TRUE;
        return FALSE;
    }
    if (SCM_COMPNUMP(obj)) return FALSE;
    Scm_Error("number required, but got %S", obj);
    return FALSE;           /* dummy */
}

int Scm_OddP(ScmObj obj)
{
    if (SCM_INTP(obj)) {
        return (SCM_INT_VALUE(obj)&1);
    }
    if (SCM_BIGNUMP(obj)) {
        return (SCM_BIGNUM(obj)->values[0] & 1);
    }
    if (SCM_FLONUMP(obj) && Scm_IntegerP(obj)) {
        return (fmod(SCM_FLONUM_VALUE(obj), 2.0) != 0.0);
    }
    Scm_Error("integer required, but got %S", obj);
    return FALSE;       /* dummy */
    
}

/* Unary Operator */

ScmObj Scm_Abs(ScmObj obj)
{
    if (SCM_INTP(obj)) {
        long v = SCM_INT_VALUE(obj);
        if (v < 0) obj = SCM_MAKE_INT(-v);
    } else if (SCM_BIGNUMP(obj)) {
        if (SCM_BIGNUM_SIGN(obj) < 0) {
            obj = Scm_BignumCopy(SCM_BIGNUM(obj));
            SCM_BIGNUM_SIGN(obj) = 1;
        }
    } else if (SCM_FLONUMP(obj)) {
        double v = SCM_FLONUM_VALUE(obj);
        if (v < 0) obj = Scm_MakeFlonum(-v);
    } else if (SCM_RATNUMP(obj)) {
        if (Scm_Sign(SCM_RATNUM_NUMER(obj)) < 0) {
            obj = Scm_MakeRational(Scm_Negate(SCM_RATNUM_NUMER(obj)),
                                   SCM_RATNUM_DENOM(obj));
        }
    } else if (SCM_COMPNUMP(obj)) {
        double r = SCM_COMPNUM_REAL(obj);
        double i = SCM_COMPNUM_IMAG(obj);
        double a = sqrt(r*r+i*i);
        return Scm_MakeFlonum(a);
    } else {
        Scm_Error("number required: %S", obj);
    }
    return obj;
}

/* Return -1, 0 or 1 when arg is minus, zero or plus, respectively.
   used to implement zero?, positive? and negative? */
int Scm_Sign(ScmObj obj)
{
    long r = 0;
    
    if (SCM_INTP(obj)) {
        r = SCM_INT_VALUE(obj);
        if (r > 0) r = 1;
        else if (r < 0) r = -1;
    } else if (SCM_BIGNUMP(obj)) {
        r = SCM_BIGNUM_SIGN(obj);
    } else if (SCM_FLONUMP(obj)) {
        double v = SCM_FLONUM_VALUE(obj);
        if (v != 0.0) {
            r = (v > 0.0)? 1 : -1;
        }
    } else if (SCM_RATNUMP(obj)) {
        return Scm_Sign(SCM_RATNUM_NUMER(obj));
    } else {
        /* NB: zero? can accept a complex number, but it is processed in
           the stub function.   see stdlib.stub */
        Scm_Error("real number required, but got %S", obj);
    }
    return r;
}

ScmObj Scm_Negate(ScmObj obj)
{
    if (SCM_INTP(obj)) {
        long v = SCM_INT_VALUE(obj);
        if (v == SCM_SMALL_INT_MIN) {
            obj = Scm_MakeBignumFromSI(-v);
        } else {
            obj = SCM_MAKE_INT(-v);
        }
    } else if (SCM_BIGNUMP(obj)) {
        obj = Scm_BignumNegate(SCM_BIGNUM(obj));
    } else if (SCM_FLONUMP(obj)) {
        obj = Scm_MakeFlonum(-SCM_FLONUM_VALUE(obj));
    } else if (SCM_RATNUMP(obj)) {
        obj = Scm_MakeRational(Scm_Negate(SCM_RATNUM_NUMER(obj)),
                               SCM_RATNUM_DENOM(obj));
    } else if (SCM_COMPNUMP(obj)) {
        obj = Scm_MakeCompnum(-SCM_COMPNUM_REAL(obj),
                              -SCM_COMPNUM_IMAG(obj));
    } else {
        obj = Scm_ApplyRec(SCM_OBJ(&generic_sub), SCM_LIST1(obj));
    }
    return obj;
}

ScmObj Scm_Reciprocal(ScmObj obj)
{
    if (SCM_INTP(obj) || SCM_BIGNUMP(obj)) {
        obj = Scm_MakeRational(SCM_MAKE_INT(1), obj);
    } else if (SCM_FLONUMP(obj)) {
        double val = SCM_FLONUM_VALUE(obj);
        obj = Scm_MakeFlonum(1.0/val);
    } else if (SCM_RATNUMP(obj)) {
        obj = Scm_MakeRational(SCM_RATNUM_DENOM(obj),
                               SCM_RATNUM_NUMER(obj));
    } else if (SCM_COMPNUMP(obj)) {
        double r = SCM_COMPNUM_REAL(obj), r1;
        double i = SCM_COMPNUM_IMAG(obj), i1;
        double d;
        d = r*r + i*i;
        r1 = r/d;
        i1 = -i/d;
        obj = Scm_MakeComplex(r1, i1);
    } else {
        obj = Scm_ApplyRec(SCM_OBJ(&generic_div), SCM_LIST1(obj));
    }
    return obj;
}

ScmObj Scm_ReciprocalInexact(ScmObj obj)
{
    if (SCM_EXACT_ZERO_P(obj)) return SCM_POSITIVE_INFINITY;
    if (SCM_EXACT_ONE_P(obj))  return obj;
    if (SCM_REALP(obj)) {
        return Scm_MakeFlonum(1.0/Scm_GetDouble(obj));
    }
    // delegate the rest to exact reciprocal
    return Scm_Reciprocal(obj);
}


/*
 * Conversion operators
 */

ScmObj Scm_ExactToInexact(ScmObj obj)
{
    if (SCM_INTP(obj)) {
        obj = Scm_MakeFlonum((double)SCM_INT_VALUE(obj));
    } else if (SCM_BIGNUMP(obj)) {
        obj = Scm_MakeFlonum(Scm_BignumToDouble(SCM_BIGNUM(obj)));
    } else if (SCM_RATNUMP(obj)) {
        obj = Scm_MakeFlonum(Scm_GetDouble(obj));
    } else if (!SCM_FLONUMP(obj) && !SCM_COMPNUMP(obj)) {
        Scm_Error("number required: %S", obj);
    }
    return obj;
}

ScmObj Scm_InexactToExact(ScmObj obj)
{
    if (SCM_FLONUMP(obj)) {
        double d = SCM_FLONUM_VALUE(obj);
        double f, i;
        if (SCM_IS_NAN(d) || SCM_IS_INF(d)) {
            Scm_Error("Exact infinity/nan is not supported: %S", obj);
        }
        if ((f = modf(d, &i)) == 0.0) {
            /* integer */
            if (d < SCM_SMALL_INT_MIN || d > SCM_SMALL_INT_MAX) {
                obj = Scm_MakeBignumFromDouble(d);
            } else {
                obj = SCM_MAKE_INT((long)d);
            }
        } else {
            ScmObj m;
            int exp, sign;
            m = Scm_DecodeFlonum(d, &exp, &sign);
            SCM_ASSERT(exp < 0); /* exp >= 0 case should be handled above */
            obj = Scm_Div(m, Scm_Ash(SCM_MAKE_INT(1), -exp));
            if (sign < 0) obj = Scm_Negate(obj);
        }
    } else if (SCM_COMPNUMP(obj)) {
        Scm_Error("exact complex is not supported: %S", obj);
    } if (!SCM_EXACTP(obj)) {
        Scm_Error("number required: %S", obj);
    }
    return obj;
}

/*===============================================================
 * Arithmetics
 */

/* NB: we used to support n-ary operations in C API, expecting
   them to be faster since we can carry around the intermediate
   results unboxed.  The newer versions of compiler, however,
   decomposes n-ary arithmetic operations into binary ones
   during optimization, so n-ary API hadn't really been used much.
   So we dropped them, in favor of simple code. */

/*
 * Addition and subtraction
 */

ScmObj Scm_Add(ScmObj arg0, ScmObj arg1)
{
    if (SCM_INTP(arg0)) {
        if (SCM_INTP(arg1)) {
            long r = SCM_INT_VALUE(arg0) + SCM_INT_VALUE(arg1);
            return Scm_MakeInteger(r);
        }
        if (SCM_BIGNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg1;
            return Scm_BignumAddSI(SCM_BIGNUM(arg1), SCM_INT_VALUE(arg0));
        }
        if (SCM_RATNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg1;
            return Scm_RatnumAdd(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg1;
            return Scm_MakeFlonum((double)SCM_INT_VALUE(arg0)
                                  + SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg1;
            return Scm_MakeComplex((double)SCM_INT_VALUE(arg0)
                                   + SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    else if (SCM_BIGNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_BignumAddSI(SCM_BIGNUM(arg0), SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1)) {
            return Scm_BignumAdd(SCM_BIGNUM(arg0), SCM_BIGNUM(arg1));
        }
        if (SCM_RATNUMP(arg1)) {
            return Scm_RatnumAdd(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            return Scm_MakeFlonum(Scm_GetDouble(arg0)
                                  + SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(Scm_GetDouble(arg0)
                                   + SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    else if (SCM_RATNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_RatnumAdd(arg0, arg1);
        }
        if (SCM_BIGNUMP(arg1)||SCM_RATNUMP(arg1)) {
            return Scm_RatnumAdd(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            return Scm_MakeFlonum(Scm_GetDouble(arg0)
                                  + SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(Scm_GetDouble(arg0)
                                   + SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    else if (SCM_FLONUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)
                                  + (double)SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)
                                  + Scm_GetDouble(arg1));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg0) == 0.0) return arg1;
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg0;
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)
                                  + SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg0) == 0.0) return arg1;
            return Scm_MakeComplex(SCM_FLONUM_VALUE(arg0)
                                   + SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    else if (SCM_COMPNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   + (double)SCM_INT_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   + Scm_GetDouble(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   + SCM_FLONUM_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   + SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg0)
                                   + SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    /* object-+ handling */
    return Scm_ApplyRec(SCM_OBJ(&generic_add), SCM_LIST2(arg0, arg1));
}

ScmObj Scm_Sub(ScmObj arg0, ScmObj arg1)
{
    if (SCM_INTP(arg0)) {
        if (SCM_INTP(arg1)) {
            long r = SCM_INT_VALUE(arg0) - SCM_INT_VALUE(arg1);
            return Scm_MakeInteger(r);
        }
        if (SCM_BIGNUMP(arg1)) {
            ScmObj big = Scm_MakeBignumFromSI(SCM_INT_VALUE(arg0));
            return Scm_BignumSub(SCM_BIGNUM(big), SCM_BIGNUM(arg1));
        }
        if (SCM_RATNUMP(arg1)) {
            return Scm_RatnumSub(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            return Scm_MakeFlonum((double)SCM_INT_VALUE(arg0)
                                  - SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex((double)SCM_INT_VALUE(arg0)
                                   - SCM_COMPNUM_REAL(arg1),
                                   - SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_BIGNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_BignumSubSI(SCM_BIGNUM(arg0), SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1)) {
            return Scm_BignumSub(SCM_BIGNUM(arg0), SCM_BIGNUM(arg1));
        }
        if (SCM_RATNUMP(arg1)) {
            return Scm_RatnumSub(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            return Scm_MakeFlonum(Scm_GetDouble(arg0)
                                  - SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(Scm_GetDouble(arg0)
                                   - SCM_COMPNUM_REAL(arg1),
                                   - SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_RATNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_RatnumSub(arg0, arg1);
        }
        if (SCM_BIGNUMP(arg1)||SCM_RATNUMP(arg1)) {
            return Scm_RatnumSub(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg0;
            return Scm_MakeFlonum(Scm_GetDouble(arg0)
                                  - SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(Scm_GetDouble(arg0)
                                   - SCM_COMPNUM_REAL(arg1),
                                   - SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_FLONUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)
                                  - (double)SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)
                                  - Scm_GetDouble(arg1));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg0;
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)
                                  - SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(SCM_FLONUM_VALUE(arg0)
                                   - SCM_COMPNUM_REAL(arg1),
                                   - SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_COMPNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   - (double)SCM_INT_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   - Scm_GetDouble(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   - Scm_GetDouble(arg1),
                                   SCM_COMPNUM_IMAG(arg0));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   - SCM_COMPNUM_REAL(arg1),
                                   SCM_COMPNUM_IMAG(arg0)
                                   - SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    /* object-- handling */
    return Scm_ApplyRec(SCM_OBJ(&generic_sub), SCM_LIST2(arg0, arg1));
}

/*
 * Multiplication
 */

ScmObj Scm_Mul(ScmObj arg0, ScmObj arg1)
{
    if (SCM_INTP(arg0)) {
        if (SCM_INTP(arg1)) {
            long v0 = SCM_INT_VALUE(arg0);
            long v1 = SCM_INT_VALUE(arg1);
            long k = v0 * v1;
            /* TODO: need a better way to check overflow */
            if ((v1 != 0 && k/v1 != v0) || !SCM_SMALL_INT_FITS(k)) {
                ScmObj big = Scm_MakeBignumFromSI(v0);
                return Scm_BignumMulSI(SCM_BIGNUM(big), v1);
            } else {
                return Scm_MakeInteger(k);
            }
        }
        if (SCM_BIGNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg0;
            if (SCM_EQ(arg0, SCM_MAKE_INT(1))) return arg1;
            return Scm_BignumMulSI(SCM_BIGNUM(arg1), SCM_INT_VALUE(arg0));
        }
        if (SCM_RATNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg0;
            if (SCM_EQ(arg0, SCM_MAKE_INT(1))) return arg1;
            return Scm_RatnumMul(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg0;
            if (SCM_EQ(arg0, SCM_MAKE_INT(1))) return arg1;
            return Scm_MakeFlonum((double)SCM_INT_VALUE(arg0)
                                  * SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg0;
            if (SCM_EQ(arg0, SCM_MAKE_INT(1))) return arg1;
            return Scm_MakeComplex((double)SCM_INT_VALUE(arg0)
                                   * SCM_COMPNUM_REAL(arg1),
                                   (double)SCM_INT_VALUE(arg0)
                                   * SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_BIGNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg1;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            return Scm_BignumMulSI(SCM_BIGNUM(arg0), SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1)) {
            return Scm_BignumMul(SCM_BIGNUM(arg0), SCM_BIGNUM(arg1));
        }
        if (SCM_RATNUMP(arg1)) {
            return Scm_RatnumMul(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            return Scm_MakeFlonum(Scm_GetDouble(arg0)
                                  * SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            double z = Scm_GetDouble(arg0);
            return Scm_MakeComplex(z * SCM_COMPNUM_REAL(arg1),
                                   z * SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_RATNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg1;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            return Scm_RatnumMul(arg0, arg1);
        }
        if (SCM_BIGNUMP(arg1)||SCM_RATNUMP(arg1)) {
            return Scm_RatnumMul(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) return arg1;
            return Scm_MakeFlonum(Scm_GetDouble(arg0)
                                  * SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(Scm_GetDouble(arg0)
                                   * SCM_COMPNUM_REAL(arg1),
                                   Scm_GetDouble(arg0)
                                   * SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_FLONUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            /* inexact number * exact zero makes exact zero */
            if (SCM_EXACT_ZERO_P(arg1)) return arg1;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)
                                  * (double)SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)
                                  * Scm_GetDouble(arg1));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 1.0) return arg0;
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)
                                  * SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            return Scm_MakeComplex(SCM_FLONUM_VALUE(arg0)
                                   * SCM_COMPNUM_REAL(arg1),
                                   SCM_FLONUM_VALUE(arg0)
                                   * SCM_COMPNUM_IMAG(arg1));
        }
        /* fallback to generic */
    }
    if (SCM_COMPNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) return arg1;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   * (double)SCM_INT_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0)
                                   * (double)SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   * Scm_GetDouble(arg1),
                                   SCM_COMPNUM_IMAG(arg0)
                                   * Scm_GetDouble(arg1));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 1.0) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)
                                   * SCM_FLONUM_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0)
                                   * SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            double r0 = SCM_COMPNUM_REAL(arg0);
            double i0 = SCM_COMPNUM_IMAG(arg0);
            double r1 = SCM_COMPNUM_REAL(arg1);
            double i1 = SCM_COMPNUM_IMAG(arg1);
            return Scm_MakeComplex(r0 * r1 - i0 * i1,
                                   r0 * i1 + r1 * i0);
        }
        /* fallback to generic */
    }
    return Scm_ApplyRec(SCM_OBJ(&generic_mul), SCM_LIST2(arg0, arg1));
}

/*
 * Division
 */

/* In the transient stage towards supporting the full numeric tower,
 * we provide two versions of Scm_Div --- the standard one supports
 * full tower, and a "auto coerce" version works like the old version
 * of Gauche; that is, it returns inexact number for exact integer 
 * division if the result isn't a whole integer.
 *
 *  Scm_Div            (/ 1 3) => 1/3
 *  Scm_DivInexact     (/ 1 3) => 0.333333333333333333
 *
 * NB: Scm_DivInexact does exact rational arithmetic if one of the
 * arguments is ratnum.
 */

static ScmObj div_internal(ScmObj arg0, ScmObj arg1, int autocoerce)
{
    if (SCM_INTP(arg0)) {
        if (SCM_INTP(arg1)) { 
            if (SCM_EXACT_ZERO_P(arg1)) goto ANORMAL;
            if (SCM_EXACT_ZERO_P(arg0)) return arg0;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            if (autocoerce) {
                if (SCM_INT_VALUE(arg0)%SCM_INT_VALUE(arg1) == 0) {
                    long q = SCM_INT_VALUE(arg0)/SCM_INT_VALUE(arg1);
                    return Scm_MakeInteger(q);
                } else {
                    return Scm_MakeFlonum((double)SCM_INT_VALUE(arg0)
                                          /(double)SCM_INT_VALUE(arg1));
                }
            } else {
                return Scm_MakeRational(arg0, arg1);
            }
        }
        if (SCM_BIGNUMP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg0)) return arg0;
            if (autocoerce) {
                goto COERCE_INEXACT;
            } else {
                return Scm_MakeRational(arg0, arg1);
            }
        }
        if (SCM_RATNUMP(arg1)) {
            return Scm_MakeRational(Scm_Mul(arg0,
                                                  SCM_RATNUM_DENOM(arg1)),
                                    SCM_RATNUM_NUMER(arg1));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) goto ANORMAL;
            if (SCM_EXACT_ZERO_P(arg0)) return arg0;
            return Scm_MakeFlonum(SCM_INT_VALUE(arg0)/SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            goto DO_COMPLEX1;
        }
        /* fallback to generic */
    }
    if (SCM_BIGNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) goto ANORMAL;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            if (autocoerce) {
                goto COERCE_INEXACT;
            } else {
                return Scm_MakeRational(arg0, arg1);
            }
        }
        if (SCM_BIGNUMP(arg1)) {
            if (autocoerce) {
                goto COERCE_INEXACT;
            } else {
                return Scm_MakeRational(arg0, arg1);
            }
        }
        if (SCM_RATNUMP(arg1)) {
            return Scm_MakeRational(Scm_Mul(arg0, SCM_RATNUM_DENOM(arg1)),
                                    SCM_RATNUM_NUMER(arg1));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) goto ANORMAL;
            return Scm_MakeFlonum(Scm_GetDouble(arg0)/SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            goto DO_COMPLEX1;
        }
        /* fallback to generic */
    }
    if (SCM_RATNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) goto ANORMAL;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            return Scm_MakeRational(SCM_RATNUM_NUMER(arg0),
                                    Scm_Mul(SCM_RATNUM_DENOM(arg0),
                                                  arg1));
        }
        if (SCM_BIGNUMP(arg1)) {
            return Scm_MakeRational(SCM_RATNUM_NUMER(arg0),
                                    Scm_Mul(SCM_RATNUM_DENOM(arg0),
                                                  arg1));
        }
        if (SCM_RATNUMP(arg1)) {
            return Scm_RatnumDiv(arg0, arg1);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) goto ANORMAL;
            return Scm_MakeFlonum(Scm_GetDouble(arg0)/SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            goto DO_COMPLEX1;
        }
        /* fallback to generic */
    }
    if (SCM_FLONUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) goto ANORMAL;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)/SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)/Scm_GetDouble(arg1));
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) goto ANORMAL;
            return Scm_MakeFlonum(SCM_FLONUM_VALUE(arg0)/SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            goto DO_COMPLEX1;
        }
        /* fallback to generic */
    }
    if (SCM_COMPNUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            if (SCM_EXACT_ZERO_P(arg1)) goto ANORMAL;
            if (SCM_EXACT_ONE_P(arg1)) return arg0;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)/SCM_INT_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0)/SCM_INT_VALUE(arg1));
        }
        if (SCM_BIGNUMP(arg1) || SCM_RATNUMP(arg1)) {
            double z = Scm_GetDouble(arg1);
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)/z,
                                   SCM_COMPNUM_IMAG(arg0)/z);
        }
        if (SCM_FLONUMP(arg1)) {
            if (SCM_FLONUM_VALUE(arg1) == 0.0) goto ANORMAL;
            return Scm_MakeComplex(SCM_COMPNUM_REAL(arg0)/SCM_FLONUM_VALUE(arg1),
                                   SCM_COMPNUM_IMAG(arg0)/SCM_FLONUM_VALUE(arg1));
        }
        if (SCM_COMPNUMP(arg1)) {
            double r0 = SCM_COMPNUM_REAL(arg0);
            double i0 = SCM_COMPNUM_IMAG(arg0);
            double r1 = SCM_COMPNUM_REAL(arg1);
            double i1 = SCM_COMPNUM_IMAG(arg1);
            double d = r1*r1+i1*i1;
            return Scm_MakeComplex((r0*r1 + i0*i1)/d,
                                   (i0*r1 - r0*i1)/d);
        }
        /* fallback to generic */
    }
    return Scm_ApplyRec(SCM_OBJ(&generic_div), SCM_LIST2(arg0, arg1));

  COERCE_INEXACT:
    {
        /* We have exact integer division arg0/arg1 (arg1 != 0).
           If it doesn't produce a whole integer, we coerce the
           result to flonum. */
        ScmObj rem;
        ScmObj q = Scm_Quotient(arg0, arg1, &rem);
        if (SCM_EXACT_ZERO_P(rem)) {
            return q;
        } else {
            return Scm_MakeFlonum(Scm_GetDouble(arg0)/Scm_GetDouble(arg1));
        }
    }
  ANORMAL:
    {
        int s = Scm_Sign(arg0);
        if (s == 0) return SCM_NAN;
        if (s < 0)  return SCM_NEGATIVE_INFINITY;
        else        return SCM_POSITIVE_INFINITY;
    }
  DO_COMPLEX1:
    {
        double r1 = SCM_COMPNUM_REAL(arg1);
        double i1 = SCM_COMPNUM_IMAG(arg1);
        double d = r1*r1+i1*i1;
        return Scm_MakeComplex(r1 * Scm_GetDouble(arg0) / d,
                               -i1 * Scm_GetDouble(arg0) / d);
    }    
}

ScmObj Scm_Div(ScmObj x, ScmObj y)
{
    return div_internal(x, y, FALSE);
}

ScmObj Scm_DivInexact(ScmObj x, ScmObj y)
{
    return div_internal(x, y, TRUE);
}


/*
 * Integer division
 *   Returns (quotient x y)
 *   If rem != NULL, sets *rem to be (remainder x y) as well.
 */
ScmObj Scm_Quotient(ScmObj x, ScmObj y, ScmObj *rem)
{
    double rx, ry;

    /* Trivial shortcut.  This case may seem too specific, but actually
       it appears rather often in rational operations. */
    if (SCM_EQ(y, SCM_MAKE_INT(1))) {
        if (!Scm_IntegerP(x)) goto BADARG;
        if (rem) *rem = SCM_MAKE_INT(0);
        return x;
    }
    
    
    if (SCM_INTP(x)) {
        if (SCM_INTP(y)) {
            long q, r;
            if (SCM_INT_VALUE(y) == 0) goto DIVBYZERO;
            q = SCM_INT_VALUE(x)/SCM_INT_VALUE(y);
            if (rem) {
                r = SCM_INT_VALUE(x)%SCM_INT_VALUE(y);
                *rem = SCM_MAKE_INT(r);
            }
            return SCM_MAKE_INT(q);
        }
        if (SCM_BIGNUMP(y)) {
            if (rem) *rem = x;
            return SCM_MAKE_INT(0);
        }
        if (SCM_FLONUMP(y)) {
            rx = (double)SCM_INT_VALUE(x);
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
            goto DO_FLONUM;
        }
        goto BADARGY;
    } else if (SCM_BIGNUMP(x)) {
        if (SCM_INTP(y)) {
            long r;
            ScmObj q = Scm_BignumDivSI(SCM_BIGNUM(x), SCM_INT_VALUE(y), &r);
            if (rem) *rem = SCM_MAKE_INT(r);
            return q;
        } else if (SCM_BIGNUMP(y)) {
            ScmObj qr = Scm_BignumDivRem(SCM_BIGNUM(x), SCM_BIGNUM(y));
            if (rem) *rem = SCM_CDR(qr);
            return SCM_CAR(qr);
        } else if (SCM_FLONUMP(y)) {
            rx = Scm_BignumToDouble(SCM_BIGNUM(x));
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
            goto DO_FLONUM;
        }
        goto BADARGY;
    } else if (SCM_FLONUMP(x)) {
        rx = SCM_FLONUM_VALUE(x);
        if (rx != floor(rx)) goto BADARG;
        if (SCM_INTP(y)) {
            ry = (double)SCM_INT_VALUE(y);
        } else if (SCM_BIGNUMP(y)) {
            ry = Scm_BignumToDouble(SCM_BIGNUM(y));
        } else if (SCM_FLONUMP(y)) {
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
        } else {
            goto BADARGY;
        }
      DO_FLONUM:
        {
            double q;
            if (ry == 0.0) goto DIVBYZERO;
            q = roundeven(rx/ry);
            if (rem) {
                double rr = roundeven(rx - q*ry);
                *rem = Scm_MakeFlonum(rr);
            }
            return Scm_MakeFlonum(q);
        }
    } else {
        goto BADARG;
    }
  DIVBYZERO:
    Scm_Error("attempt to calculate a quotient by zero");
  BADARGY:
    x = y;
  BADARG:
    Scm_Error("integer required, but got %S", x);
    return SCM_UNDEFINED;       /* dummy */
}

/* Modulo and Reminder.
   TODO: on gcc, % works like reminder.  I'm not sure the exact behavior
   of % is defined in ANSI C.  Need to check it later. */
ScmObj Scm_Modulo(ScmObj x, ScmObj y, int remp)
{
    double rx, ry;
    if (SCM_INTP(x)) {
        if (SCM_INTP(y)) {
            long r;
            if (SCM_INT_VALUE(y) == 0) goto DIVBYZERO;
            r = SCM_INT_VALUE(x)%SCM_INT_VALUE(y);
            if (!remp && r) {
                if ((SCM_INT_VALUE(x) > 0 && SCM_INT_VALUE(y) < 0)
                    || (SCM_INT_VALUE(x) < 0 && SCM_INT_VALUE(y) > 0)) {
                    r += SCM_INT_VALUE(y);
                }
            }
            return SCM_MAKE_INT(r);
        }
        if (SCM_BIGNUMP(y)) {
            if (remp) {
                return x;
            } else {
                if ((SCM_INT_VALUE(x) < 0 && SCM_BIGNUM_SIGN(y) > 0)
                    || (SCM_INT_VALUE(x) > 0 && SCM_BIGNUM_SIGN(y) < 0)) {
                    return Scm_BignumAddSI(SCM_BIGNUM(y), SCM_INT_VALUE(x));
                } else {
                    return x;
                }
            }
        }
        rx = (double)SCM_INT_VALUE(x);
        if (SCM_FLONUMP(y)) {
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
            goto DO_FLONUM;
        }
        goto BADARGY;
    } else if (SCM_BIGNUMP(x)) {
        if (SCM_INTP(y)) {
            long iy = SCM_INT_VALUE(y);
            long rem;
            Scm_BignumDivSI(SCM_BIGNUM(x), iy, &rem);
            if (!remp
                && rem
                && ((SCM_BIGNUM_SIGN(x) < 0 && iy > 0)
                    || (SCM_BIGNUM_SIGN(x) > 0 && iy < 0))) {
                return SCM_MAKE_INT(iy + rem);
            }
            return SCM_MAKE_INT(rem);
        }
        if (SCM_BIGNUMP(y)) {
            ScmObj rem = SCM_CDR(Scm_BignumDivRem(SCM_BIGNUM(x), SCM_BIGNUM(y)));
            if (!remp
                && (rem != SCM_MAKE_INT(0))
                && (SCM_BIGNUM_SIGN(x) * SCM_BIGNUM_SIGN(y) < 0)) {
                if (SCM_BIGNUMP(rem)) {
                    return Scm_BignumAdd(SCM_BIGNUM(y), SCM_BIGNUM(rem));
                } else {
                    return Scm_BignumAddSI(SCM_BIGNUM(y), SCM_INT_VALUE(rem));
                }       
            }
            return rem;
        }
        rx = Scm_BignumToDouble(SCM_BIGNUM(x));
        if (SCM_FLONUMP(y)) {
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
            goto DO_FLONUM;
        }
        goto BADARGY;
    } else if (SCM_FLONUMP(x)) {
        double rem;
        rx = SCM_FLONUM_VALUE(x);
        if (rx != floor(rx)) goto BADARG;
        if (SCM_INTP(y)) {
            ry = (double)SCM_INT_VALUE(y);
        } else if (SCM_BIGNUMP(y)) {
            ry = Scm_BignumToDouble(SCM_BIGNUM(y));
        } else if (SCM_FLONUMP(y)) {
            ry = SCM_FLONUM_VALUE(y);
            if (ry != floor(ry)) goto BADARGY;
        } else {
            goto BADARGY;
        }
      DO_FLONUM:
        if (ry == 0.0) goto DIVBYZERO;
        rem = fmod(rx, ry);
        if (!remp && rem != 0.0) {
            if ((rx > 0 && ry < 0) || (rx < 0 && ry > 0)) {
                rem += ry;
            }
        }
        return Scm_MakeFlonum(rem);
    } else {
        goto BADARG;
    }
  DIVBYZERO:
    Scm_Error("attempt to take a modulo or remainder by zero");
  BADARGY:
    x = y;
  BADARG:
    Scm_Error("integer required, but got %S", x);
    return SCM_UNDEFINED;       /* dummy */
}

/*
 * Gcd
 */

/* assumes x > y >= 0 */
static u_long gcd_fixfix(u_long x, u_long y)
{
    while (y > 0) {
        u_long r = x % y;
        x = y;
        y = r;
    }
    return x;
}

static double gcd_floflo(double x, double y)
{
    if (x < 0) x = -x;
    if (y < 0) y = -y;
    if (x < y) { double t = x; x = y; y = t; }
        
    while (y > 0.0) {
        double r = fmod(x, y);
        x = y;
        y = r;
    }
    return x;
}

/* assumes y <= LONG_MAX.  curiously, the sign of x doesn't matter,
   since it only affects the remainder's sign which we adjust afterwards. */
static u_long gcd_bigfix(ScmBignum *x, u_long y)
{
    long rem;
    (void)Scm_BignumDivSI(x, (signed long)y, &rem);
    if (rem < 0) rem = -rem;
    return gcd_fixfix(y, (u_long)rem);
}

ScmObj Scm_Gcd(ScmObj x, ScmObj y)
{
    int ox = FALSE, oy = FALSE;
    long ix, iy;
    u_long ux, uy, ur;
    
    if (!Scm_IntegerP(x)) {
        Scm_Error("integer required, but got %S", x);
    }
    if (!Scm_IntegerP(y)) {
        Scm_Error("integer required, but got %S", y);
    }
    if (SCM_FLONUMP(x) || SCM_FLONUMP(y)) {
        return Scm_MakeFlonum(gcd_floflo(Scm_GetDouble(x),
                                         Scm_GetDouble(y)));
    }

    if (SCM_EXACT_ZERO_P(x)) return y;
    if (SCM_EXACT_ZERO_P(y)) return x;

    ix = Scm_GetIntegerClamp(x, SCM_CLAMP_NONE, &ox);
    iy = Scm_GetIntegerClamp(y, SCM_CLAMP_NONE, &oy);

    if (!ox && !oy) {
        ux = (ix < 0)? -ix : ix;
        uy = (iy < 0)? -iy : iy;
        if (ux >= uy) {
            ur = gcd_fixfix(ux, uy);
        } else {
            ur = gcd_fixfix(uy, ux);
        }
        return Scm_MakeIntegerU(ur);
    }

    if (!oy && iy != LONG_MIN) {
        /* x overflows long.  y doesn't.  so we know abs(x) > abs(y)
           (abs(x) == abs(y) iff LONG_MAX+1 and y == LONG_MIN, but we
           excluded it above). */
        SCM_ASSERT(SCM_BIGNUMP(x));
        uy = (iy < 0)? -iy : iy;
        ur = gcd_bigfix(SCM_BIGNUM(x), uy);
        return Scm_MakeIntegerU(ur);
    }

    if (!ox && ix != LONG_MIN) {
        /* reverse condition of above */
        SCM_ASSERT(SCM_BIGNUMP(y));
        ux = (ix < 0)? -ix : ix;
        ur = gcd_bigfix(SCM_BIGNUM(y), ux);
        return Scm_MakeIntegerU(ur);
    }
    
    /* Now we need to treat both args as bignums.  We could use
       Algorithm L in Knuth's TAOCP 4.5.2, but we assume this path
       is rarely executed, so we don't bother for now. */
    x = Scm_Abs(x);
    y = Scm_Abs(y);
    if (Scm_NumCmp(x, y) < 0) {ScmObj t = x; x = y; y = t;}

    while (!SCM_EXACT_ZERO_P(y)) {
        ScmObj r = Scm_Modulo(x, y, TRUE);
        x = y;
        y = r;
    }
    return x;
}

/*
 * Expt
 */

/* Integer power of 10.  It is extensively used during string->number
   and number->string operations.
   IEXPT10_TABLESIZ is ceil(-log10(ldexp(1.0, -1022-52))) + 2 */
/* NB: actually we need more margin here to handle denormalized numbers. */
#define IEXPT10_TABLESIZ  341
static ScmObj iexpt10_n[IEXPT10_TABLESIZ] = { NULL };
static int    iexpt10_initialized = FALSE;

static void iexpt10_init(void)
{
    int i;
    iexpt10_n[0] = SCM_MAKE_INT(1);
    iexpt10_n[1] = SCM_MAKE_INT(10);
    iexpt10_n[2] = SCM_MAKE_INT(100);
    iexpt10_n[3] = SCM_MAKE_INT(1000);
    iexpt10_n[4] = SCM_MAKE_INT(10000);
    iexpt10_n[5] = SCM_MAKE_INT(100000);
    iexpt10_n[6] = SCM_MAKE_INT(1000000);
    for (i=7; i<IEXPT10_TABLESIZ; i++) {
        iexpt10_n[i] = Scm_Mul(iexpt10_n[i-1], SCM_MAKE_INT(10));
    }
    iexpt10_initialized = TRUE;
}

#define IEXPT10_INIT() \
    do { if (!iexpt10_initialized) iexpt10_init(); } while (0)

/* short cut for exact numbers */
static ScmObj exact_expt(ScmObj x, ScmObj y)
{
    int sign = Scm_Sign(y);
    long iy;
    ScmObj r = SCM_MAKE_INT(1);

    if (sign == 0) return r;
    if (SCM_EQ(x, SCM_MAKE_INT(1))) return r;
    if (SCM_EQ(x, SCM_MAKE_INT(-1))) return Scm_OddP(y)? SCM_MAKE_INT(-1) : r;

    if (!SCM_INTP(y)) {
        /* who wants such a heavy calculation? */
        Scm_Error("exponent too big: %S", y);
    }
    iy = SCM_INT_VALUE(y);
    /* Shortcut for special cases */
    if (SCM_EQ(x, SCM_MAKE_INT(10)) && iy > 0 && iy < IEXPT10_TABLESIZ) {
        /* We have a precalculated table for 10^y */
        IEXPT10_INIT();
        r = iexpt10_n[iy];
    } else if (SCM_EQ(x, SCM_MAKE_INT(2)) && iy > 0) {
        /* Use shift operation for 2^y, y>0 */
        r = Scm_Ash(SCM_MAKE_INT(1), iy);
    } else {
        /* General case */
        if (iy < 0) iy = -iy;
        for (;;) {
            if (iy == 0) break;
            if (iy == 1) { r = Scm_Mul(r, x); break; }
            if (iy & 0x01) r = Scm_Mul(r, x);
            x = Scm_Mul(x, x);
            iy >>= 1;
        }
    }
    return (sign < 0)? Scm_Reciprocal(r) : r;
}

ScmObj Scm_Expt(ScmObj x, ScmObj y)
{
    double dx, dy;
    if (SCM_EXACTP(x) && SCM_INTEGERP(y)) return exact_expt(x, y);
    /* TODO: ratnum vs ratnum */
    if (!SCM_REALP(x)) Scm_Error("real number required, but got %S", x);
    if (!SCM_REALP(y)) Scm_Error("real number required, but got %S", y);
    dx = Scm_GetDouble(x);
    dy = Scm_GetDouble(y);
    if (dy == 0.0) {
        return Scm_MakeFlonum(1.0);
    } else if (dx < 0 && !Scm_IntegerP(y)) {
        /* x^y == exp(y * log(x)) = exp(y*log(|x|))*exp(y*arg(x)*i)
           if x is a negative real number, arg(x) == pi
        */
        double mag = exp(dy * log(-dx));
        double theta = dy * M_PI;
        return Scm_MakeComplex(mag * cos(theta), mag * sin(theta));
    } else {
        return Scm_MakeFlonum(pow(dx, dy));
    }
}

/*===============================================================
 * Comparison
 */

int Scm_NumEq(ScmObj arg0, ScmObj arg1)
{
    if (SCM_COMPNUMP(arg0)) {
        if (SCM_COMPNUMP(arg1)) {
            return ((SCM_COMPNUM_REAL(arg0) == SCM_COMPNUM_REAL(arg1))
                    && (SCM_COMPNUM_IMAG(arg0) == SCM_COMPNUM_IMAG(arg1)));
        }
        return FALSE;
    } else {
        if (SCM_COMPNUMP(arg1)) return FALSE;
        return (Scm_NumCmp(arg0, arg1) == 0);
    }
}

/* 2-arg comparison */
int Scm_NumCmp(ScmObj arg0, ScmObj arg1)
{
    ScmObj badnum;
    
    if (SCM_INTP(arg0)) {
        if (SCM_INTP(arg1)) {
            long r = SCM_INT_VALUE(arg0) - SCM_INT_VALUE(arg1);
            if (r < 0) return -1;
            if (r > 0) return 1;
            return 0;
        }
        if (SCM_FLONUMP(arg1)) {
            double r = SCM_INT_VALUE(arg0) - SCM_FLONUM_VALUE(arg1);
            if (r < 0) return -1;
            if (r > 0) return 1;
            return 0;
        }
        if (SCM_BIGNUMP(arg1))
            return Scm_BignumCmp(SCM_BIGNUM(Scm_MakeBignumFromSI(SCM_INT_VALUE(arg0))),
                                 SCM_BIGNUM(arg1));
        if (SCM_RATNUMP(arg1)) {
            /* numerical error doesn't matter for the range of fixnum */
            double r = SCM_INT_VALUE(arg0) - Scm_GetDouble(arg1);
            if (r < 0) return -1;
            if (r > 0) return 1;
            return 0;
        }
        badnum = arg1;
    }
    else if (SCM_FLONUMP(arg0)) {
        if (SCM_INTP(arg1)) {
            double r = SCM_FLONUM_VALUE(arg0) - SCM_INT_VALUE(arg1);
            if (r < 0) return -1;
            if (r > 0) return 1;
            return 0;
        }
        if (SCM_FLONUMP(arg1)) {
            double r = SCM_FLONUM_VALUE(arg0) - SCM_FLONUM_VALUE(arg1);
            if (r < 0) return -1;
            if (r > 0) return 1;
            return 0;
        }
        if (SCM_BIGNUMP(arg1))
            return Scm_BignumCmp(SCM_BIGNUM(Scm_MakeBignumFromDouble(SCM_FLONUM_VALUE(arg0))),
                                 SCM_BIGNUM(arg1));
        if (SCM_RATNUMP(arg1)) {
            double r = SCM_FLONUM_VALUE(arg0) - Scm_GetDouble(arg1);
            if (r < 0) return -1;
            if (r > 0) return 1;
            return 0;
        }
        badnum = arg1;
    }
    else if (SCM_BIGNUMP(arg0)) {
        if (SCM_INTP(arg1))
            return Scm_BignumCmp(SCM_BIGNUM(arg0),
                                 SCM_BIGNUM(Scm_MakeBignumFromSI(SCM_INT_VALUE(arg1))));
        if (SCM_FLONUMP(arg1))
            return Scm_BignumCmp(SCM_BIGNUM(arg0),
                                 SCM_BIGNUM(Scm_MakeBignumFromDouble(SCM_FLONUM_VALUE(arg1))));
        if (SCM_BIGNUMP(arg1))
            return Scm_BignumCmp(SCM_BIGNUM(arg0), SCM_BIGNUM(arg1));
        if (SCM_RATNUMP(arg1)) {
            /* we can't coerce to flonum, for it may lose precision. */
            ScmObj d1 = SCM_RATNUM_DENOM(arg1);
            return Scm_NumCmp(Scm_Mul(arg0, d1),
                              SCM_RATNUM_NUMER(arg1));
        }
        badnum = arg1;
    }
    else if (SCM_RATNUMP(arg0)) {
        if (SCM_INTP(arg1) || SCM_BIGNUMP(arg1) || SCM_FLONUMP(arg1)) {
            return -Scm_NumCmp(arg1, arg0);
        }
        if (SCM_RATNUMP(arg1)) {
            ScmObj n0 = SCM_RATNUM_NUMER(arg0), d0 = SCM_RATNUM_DENOM(arg0);
            ScmObj n1 = SCM_RATNUM_NUMER(arg1), d1 = SCM_RATNUM_DENOM(arg1);

            int d = Scm_NumCmp(d0, d1), n;
            /* screen the obvious cases */
            if (d == 0) return Scm_NumCmp(n0, n1);
            n = Scm_NumCmp(n0, n1);
            if (d > 0 && n <= 0) return -1;
            if (d < 0 && n >= 0) return 1;
            
            return Scm_NumCmp(Scm_Mul(n0, d1),
                              Scm_Mul(n1, d0));
        }
        badnum = arg1;
    }
    else badnum = arg0;
    Scm_Error("real number required: %S", badnum);
    return 0;                    /* dummy */
}

void Scm_MinMax(ScmObj arg0, ScmObj args, ScmObj *min, ScmObj *max)
{
    int inexact = !SCM_EXACTP(arg0);
    ScmObj mi = arg0;
    ScmObj ma = arg0;
    
    for (;;) {
        if (!SCM_REALP(arg0))
            Scm_Error("real number required, but got %S", arg0);
        if (SCM_NULLP(args)) {
            if (min) {
                if (inexact && SCM_EXACTP(mi)) {
                    *min = Scm_ExactToInexact(mi);
                } else {
                    *min = mi;
                }
            }
            if (max) {
                if (inexact && SCM_EXACTP(ma)) {
                    *max = Scm_ExactToInexact(ma);
                } else {
                    *max = ma;
                }
            }
            return;
        }
        if (!SCM_EXACTP(SCM_CAR(args))) inexact = TRUE;
        if (min && Scm_NumCmp(mi, SCM_CAR(args)) > 0) {
            mi = SCM_CAR(args);
        } 
        if (max && Scm_NumCmp(ma, SCM_CAR(args)) < 0) {
            ma = SCM_CAR(args);
        }
        args = SCM_CDR(args);
    }
}

/*===============================================================
 * ROUNDING
 */

/* NB: rint() is not in POSIX, so the alternative is provided here.
   We don't use round(), for it behaves differently when the
   argument is exactly the halfway of two whole numbers. */
#ifdef HAVE_RINT
#define roundeven rint
#else  /* !HAVE_RINT */
static double roundeven(double v)
{
    double r;
    double frac = modf(v, &r);
    if (v > 0.0) {
        if (frac > 0.5) r += 1.0;
        else if (frac == 0.5) {
            if (fmod(r,2.0) != 0.0) r += 1.0;
        }
    } else {
        if (frac < -0.5) r -= 1.0;
        else if (frac == -0.5) {
            if (fmod(r,2.0) != 0.0) r -= 1.0;
        }
    }
    return r;
}
#endif /* !HAVE_RINT */

ScmObj Scm_Round(ScmObj num, int mode)
{
    
    if (SCM_INTEGERP(num)) return num;
    if (SCM_RATNUMP(num)) {
        int offset = 0;
        ScmObj rem;
        ScmObj quot = Scm_Quotient(SCM_RATNUM_NUMER(num),
                                   SCM_RATNUM_DENOM(num), &rem);
        /* this shouldn't happen, but just in case.. */
        if (SCM_EXACT_ZERO_P(rem)) return quot;

        /* Here we have quotient, which is always closer to zero
           than the original value */
        switch (mode) {
        case SCM_ROUND_FLOOR:
            offset = (Scm_Sign(num) < 0)? -1 : 0;
            break;
        case SCM_ROUND_CEIL:
            offset = (Scm_Sign(num) < 0)? 0 : 1;
            break;
        case SCM_ROUND_TRUNC:
            offset = 0;
            break;
        case SCM_ROUND_ROUND: {
            ScmObj rem2 = Scm_Mul(Scm_Abs(rem), SCM_MAKE_INT(2));
            int cmp = Scm_NumCmp(SCM_RATNUM_DENOM(num), rem2);
            
            if (cmp > 0) {
                /* NUM is closer to zero than halfway */
                offset = 0;
            } else if (cmp < 0) {
                /* NUM is further from zero than halfway */
                offset = (Scm_Sign(num) < 0)? -1 : 1;
            } else {
                /* NUM is exactly the halfway.  We round to even */
                if (Scm_OddP(quot)) {
                    offset = (Scm_Sign(num) < 0)? -1 : 1;
                } else {
                    offset = 0;
                }
            }
            break;
        }
        default: Scm_Panic("something screwed up");
        }

        if (offset == 0) return quot;
        else return Scm_Add(quot, SCM_MAKE_INT(offset));
    }
    if (SCM_FLONUMP(num)) {
        double r = 0.0, v;
        v = SCM_FLONUM_VALUE(num);
        switch (mode) {
        case SCM_ROUND_FLOOR: r = floor(v); break;
        case SCM_ROUND_CEIL:  r = ceil(v); break;
        /* trunc is neither in ANSI nor in POSIX. */
#ifdef HAVE_TRUNC
        case SCM_ROUND_TRUNC: r = trunc(v); break;
#else
        case SCM_ROUND_TRUNC: r = (v < 0.0)? ceil(v) : floor(v); break;
#endif
        case SCM_ROUND_ROUND: r = roundeven(v); break;
        default: Scm_Panic("something screwed up");
        }
        return Scm_MakeFlonum(r);
    }
    Scm_Error("real number required, but got %S", num);
    return SCM_UNDEFINED;       /* dummy */
}

/*===============================================================
 * Logical (bitwise) operations
 */

ScmObj Scm_Ash(ScmObj x, int cnt)
{
    if (SCM_INTP(x)) {
        long ix = SCM_INT_VALUE(x);
        if (cnt <= -(SIZEOF_LONG * 8)) {
            ix = (ix < 0)? -1 : 0;
            return Scm_MakeInteger(ix);
        } else if (cnt < 0) {
            if (ix < 0) {
                ix = ~((~ix) >> (-cnt));
            } else {
                ix >>= -cnt;
            }
            return Scm_MakeInteger(ix);
        } else if (cnt < (SIZEOF_LONG*8-3)) {
            if (ix < 0) {
                if (-ix < (SCM_SMALL_INT_MAX >> cnt)) {
                    ix <<= cnt;
                    return Scm_MakeInteger(ix);
                } 
            } else {
                if (ix < (SCM_SMALL_INT_MAX >> cnt)) {
                    ix <<= cnt;
                    return Scm_MakeInteger(ix);
                } 
            }
        }
        /* Here, we know the result must be a bignum. */
        {
            ScmObj big = Scm_MakeBignumFromSI(ix);
            return Scm_BignumAsh(SCM_BIGNUM(big), cnt);
        }
    } else if (SCM_BIGNUMP(x)) {
        return Scm_BignumAsh(SCM_BIGNUM(x), cnt);
    }
    Scm_Error("exact integer required, but got %S", x);
    return SCM_UNDEFINED;
}

ScmObj Scm_LogNot(ScmObj x)
{
    if (!SCM_EXACTP(x)) Scm_Error("exact integer required, but got %S", x);
    if (SCM_INTP(x)) {
        /* this won't cause an overflow */
        return SCM_MAKE_INT(~SCM_INT_VALUE(x));
    } else {
        return Scm_Negate(Scm_BignumAddSI(SCM_BIGNUM(x), 1));
    }
}

ScmObj Scm_LogAnd(ScmObj x, ScmObj y)
{
    if (!SCM_EXACTP(x)) Scm_Error("exact integer required, but got %S", x);
    if (!SCM_EXACTP(y)) Scm_Error("exact integer required, but got %S", y);
    if (SCM_INTP(x)) {
        if (SCM_INTP(y)) {
            return SCM_MAKE_INT(SCM_INT_VALUE(x) & SCM_INT_VALUE(y));
        } else if (SCM_INT_VALUE(x) >= 0 && SCM_BIGNUM_SIGN(y) >= 0) {
            return Scm_MakeInteger(SCM_INT_VALUE(x)&SCM_BIGNUM(y)->values[0]);
        }
        x = Scm_MakeBignumFromSI(SCM_INT_VALUE(x));
    } else if (SCM_INTP(y)) {
        if (SCM_INT_VALUE(y) >= 0 && SCM_BIGNUM_SIGN(x) >= 0) {
            return Scm_MakeInteger(SCM_INT_VALUE(y)&SCM_BIGNUM(x)->values[0]);
        }
        y = Scm_MakeBignumFromSI(SCM_INT_VALUE(y));        
    }
    return Scm_BignumLogAnd(SCM_BIGNUM(x), SCM_BIGNUM(y));
}

ScmObj Scm_LogIor(ScmObj x, ScmObj y)
{
    if (!SCM_EXACTP(x)) Scm_Error("exact integer required, but got %S", x);
    if (!SCM_EXACTP(y)) Scm_Error("exact integer required, but got %S", y);
    if (SCM_INTP(x)) {
        if (SCM_INTP(y))
            return SCM_MAKE_INT(SCM_INT_VALUE(x) | SCM_INT_VALUE(y));
        else
            x = Scm_MakeBignumFromSI(SCM_INT_VALUE(x));
    } else {
        if (SCM_INTP(y)) y = Scm_MakeBignumFromSI(SCM_INT_VALUE(y));
    }
    return Scm_BignumLogIor(SCM_BIGNUM(x), SCM_BIGNUM(y));
}


ScmObj Scm_LogXor(ScmObj x, ScmObj y)
{
    if (!SCM_EXACTP(x)) Scm_Error("exact integer required, but got %S", x);
    if (!SCM_EXACTP(y)) Scm_Error("exact integer required, but got %S", y);
    if (SCM_INTP(x)) {
        if (SCM_INTP(y))
            return SCM_MAKE_INT(SCM_INT_VALUE(x) ^ SCM_INT_VALUE(y));
        else
            x = Scm_MakeBignumFromSI(SCM_INT_VALUE(x));
    } else {
        if (SCM_INTP(y)) y = Scm_MakeBignumFromSI(SCM_INT_VALUE(y));
    }
    return Scm_BignumLogXor(SCM_BIGNUM(x), SCM_BIGNUM(y));
}

/*===============================================================
 * Number I/O
 */

/* contants frequently used in number I/O */
static double dexpt2_minus_52  = 0.0;  /* 2.0^-52 */
static double dexpt2_minus_53  = 0.0;  /* 2.0^-53 */

/* max N where 10.0^N can be representable exactly in double.
   it is max N where N * log2(5) < 53. */
#define MAX_EXACT_10_EXP  23

/* fast 10^n for limited cases */
static inline ScmObj iexpt10(int e)
{
    SCM_ASSERT(e < IEXPT10_TABLESIZ);
    return iexpt10_n[e];
}

/* integer power of R by N, N is rather small.
   Assuming everything is in range. */
static inline u_long ipow(int r, int n)
{
    u_long k;
    for (k=1; n>0; n--) k *= r;
    return k;
}

/* X * 10.0^N by double.
   10.0^N can be represented _exactly_ in double-precision floating point
   number in the range 0 <= N <= 23.
   If N is out of this range, a rounding error occurs, which will be
   corrected in the algorithmR routine below. */
static double raise_pow10(double x, int n)
{
    static double dpow10[] = { 1.0, 1.0e1, 1.0e2, 1.0e3, 1.0e4,
                               1.0e5, 1.0e6, 1.0e7, 1.0e8, 1.0e9,
                               1.0e10, 1.0e11, 1.0e12, 1.0e13, 1.0e14,
                               1.0e15, 1.0e16, 1.0e17, 1.0e18, 1.0e19,
                               1.0e20, 1.0e21, 1.0e22, 1.0e23 };
    if (n >= 0) {
        while (n > 23) {
            x *= 1.0e24;
            n -= 24;
        }
        return x*dpow10[n];
    } else {
        while (n < -23) {
            x /= 1.0e24;
            n += 24;
        }
        return x/dpow10[-n];
    }
}

/*
 * Number Printer
 *
 * This version implements Burger&Dybvig algorithm (Robert G. Burger
 * and and R. Kent Dybvig, "Priting Floating-Point Numbers Quickly and 
 * Accurately", PLDI '96, pp.108--116, 1996).
 */

/* compare x+d and y */
static inline int numcmp3(ScmObj x, ScmObj d, ScmObj y)
{
    ScmObj bx = SCM_BIGNUMP(x)? x : Scm_MakeBignumFromSI(SCM_INT_VALUE(x));
    ScmObj bd = SCM_BIGNUMP(d)? d : Scm_MakeBignumFromSI(SCM_INT_VALUE(d));
    ScmObj by = SCM_BIGNUMP(y)? y : Scm_MakeBignumFromSI(SCM_INT_VALUE(y));
    return Scm_BignumCmp3U(SCM_BIGNUM(bx), SCM_BIGNUM(bd), SCM_BIGNUM(by));
}

static void double_print(char *buf, int buflen, double val, int plus_sign)
{
    /* Handle a few special cases first.
       The notation of infinity is provisional; see how srfi-70 becomes. */
    if (val == 0.0) {
        if (plus_sign) strcpy(buf, "+0.0");
        else strcpy(buf, "0.0");
        return;
    } else if (SCM_IS_INF(val)) {
        if (val < 0.0) strcpy(buf, "#i-1/0");
        else if (plus_sign) strcpy(buf, "#i+1/0");
        else strcpy(buf, "#i1/0");
        return;
    } else if (SCM_IS_NAN(val)) {
        strcpy(buf, "#<nan>");
        return;
    }
    
    if (val < 0.0) *buf++ = '-', buflen--;
    else if (plus_sign) *buf++ = '+', buflen--;
    {
        /* variable names follows Burger&Dybvig paper. mp, mm for m+, m-.
           note that m+ == m- for most cases, and m+ == 2*m- for the rest.
           so we calculate m+ from m- for each iteration, using the flag
           mp2 as   m+ = mp? m- : 2*m-. */
        ScmObj f, r, s, mp, mm, q;
        int exp, sign, est, tc1, tc2, tc3, digs, point, round;
        int mp2 = FALSE, fixup = FALSE;

        IEXPT10_INIT();
        if (val < 0) val = -val;
        
        /* initialize r, s, m+ and m- */
        f = Scm_DecodeFlonum(val, &exp, &sign);
        round = !Scm_OddP(f);
        if (exp >= 0) {
            ScmObj be = Scm_Ash(SCM_MAKE_INT(1), exp);
            if (Scm_NumCmp(f, SCM_2_52) != 0) {
                r = Scm_Ash(f, exp+1);
                s = SCM_MAKE_INT(2);
                mp2= FALSE;
                mm = be;
            } else {
                r = Scm_Ash(f, exp+2);
                s = SCM_MAKE_INT(4);
                mp2 = TRUE;
                mm = be;
            }
        } else {
            if (exp == -1023 || Scm_NumCmp(f, SCM_2_52) != 0) {
                r = Scm_Ash(f, 1);
                s = Scm_Ash(SCM_MAKE_INT(1), -exp+1);
                mp2 = FALSE;
                mm = SCM_MAKE_INT(1);
            } else {
                r = Scm_Ash(f, 2);
                s = Scm_Ash(SCM_MAKE_INT(1), -exp+2);
                mp2 = TRUE;
                mm = SCM_MAKE_INT(1);
            }
        }

        /* estimate scale */
        est = (int)ceil(log10(val) - 0.1);
        if (est >= 0) {
            s = Scm_Mul(s, iexpt10(est));
        } else {
            ScmObj scale = iexpt10(-est);
            r =  Scm_Mul(r, scale);
            mm = Scm_Mul(mm, scale);
        }

        /* fixup.  avoid calculating m+ for obvious case. */
        if (Scm_NumCmp(r, s) >= 0) {
            fixup = TRUE;
        } else {
            mp = (mp2? Scm_Ash(mm, 1) : mm);
            if (round) {
                fixup = (numcmp3(r, mp, s) >= 0);
            } else {
                fixup = (numcmp3(r, mp, s) > 0);
            }
        }
        if (fixup) {
            s = Scm_Mul(s, SCM_MAKE_INT(10));
            est++;
        }
        
        /* Scm_Printf(SCM_CURERR, "est=%d, r=%S, s=%S, mp=%S, mm=%S\n",
           est, r, s, mp, mm); */

        /* determine position of decimal point.  we avoid exponential
           notation if exponent is small, i.e. 0.9 and 30.0 instead of
           9.0e-1 and 3.0e1.   The magic number 10 is arbitrary. */
        if (est < 10 && est > -3) {
            point = est; est = 1;
        } else {
            point = 1;
        }

        /* generate */
        if (point <= 0) {
            *buf++ = '0'; buflen--;
            *buf++ = '.', buflen--;
            for (digs=point;digs<0 && buflen>5;digs++) {
                *buf++ = '0'; buflen--;
            }
        }
        for (digs=1;buflen>5;digs++) {
            ScmObj r10 = Scm_Mul(r, SCM_MAKE_INT(10));
            q = Scm_Quotient(r10, s, &r);
            mm = Scm_Mul(mm, SCM_MAKE_INT(10));
            mp = (mp2? Scm_Ash(mm, 1) : mm);
            
            /* Scm_Printf(SCM_CURERR, "q=%S, r=%S, mp=%S, mm=%S\n",
               q, r, mp, mm);*/

            SCM_ASSERT(SCM_INTP(q));
            if (round) {
                tc1 = (Scm_NumCmp(r, mm) <= 0);
                tc2 = (numcmp3(r, mp, s) >= 0);
            } else {
                tc1 = (Scm_NumCmp(r, mm) < 0);
                tc2 = (numcmp3(r, mp, s) > 0);
            }
            if (!tc1) {
                if (!tc2) {
                    *buf++ = SCM_INT_VALUE(q) + '0';
                    if (digs == point) *buf++ = '.', buflen--;
                    continue;
                } else {
                    *buf++ = SCM_INT_VALUE(q) + '1';
                    break;
                }
            } else {
                if (!tc2) {
                    *buf++ = SCM_INT_VALUE(q) + '0';
                    break;
                } else {
                    tc3 = numcmp3(r, r, s); /* r*2 <=> s */
                    if ((round && tc3 <= 0) || (!round && tc3 < 0)) {
                        *buf++ = SCM_INT_VALUE(q) + '0';
                        break;
                    } else {
                        *buf++ = SCM_INT_VALUE(q) + '1';
                        break;
                    }
                }
            }
        }

        if (digs <= point) {
            for (;digs<point&&buflen>5;digs++) {
                *buf++ = '0', buflen--;
            }
            *buf++ = '.';
            *buf++ = '0';
        }

        /* prints exponent.  we shifted decimal point, so -1. */
        est--;
        if (est != 0) {
            *buf++ = 'e';
            sprintf(buf, "%d", (int)est);
        } else {
            *buf++ = 0;
        }
    }
}

static void number_print(ScmObj obj, ScmPort *port, ScmWriteContext *ctx)
{
    ScmObj s = Scm_NumberToString(obj, 10, FALSE);
    SCM_PUTS(SCM_STRING(s), port);
}

#define FLT_BUF 50

ScmObj Scm_NumberToString(ScmObj obj, int radix, int use_upper)
{
    ScmObj r = SCM_NIL;
    char buf[FLT_BUF];
    
    if (SCM_INTP(obj)) {
        char buf[50], *pbuf = buf;
        long value = SCM_INT_VALUE(obj);
        if (value < 0) {
            *pbuf++ = '-';
            value = -value;     /* this won't overflow */
        }
        if (radix == 10) {
            snprintf(pbuf, 49, "%ld", value);
        } else if (radix == 16) {
            snprintf(pbuf, 49, (use_upper? "%lX" : "%lx"), value);
        } else if (radix == 8) {
            snprintf(pbuf, 49, "%lo", value);
        } else {
            /* sloppy way ... */
            r = Scm_BignumToString(SCM_BIGNUM(Scm_MakeBignumFromSI(SCM_INT_VALUE(obj))),
                                   radix, use_upper);
        }
        if (r == SCM_NIL) r = SCM_MAKE_STR_COPYING(buf);
    } else if (SCM_BIGNUMP(obj)) {
        r = Scm_BignumToString(SCM_BIGNUM(obj), radix, use_upper);
    } else if (SCM_FLONUMP(obj)) {
        double_print(buf, FLT_BUF, SCM_FLONUM_VALUE(obj), FALSE);
        r = SCM_MAKE_STR_COPYING(buf);
    } else if (SCM_RATNUMP(obj)) {
        ScmDString ds; ScmObj s;
        Scm_DStringInit(&ds);
        s = Scm_NumberToString(SCM_RATNUM_NUMER(obj), radix, use_upper);
        Scm_DStringAdd(&ds, SCM_STRING(s));
        Scm_DStringPutc(&ds, '/');
        s = Scm_NumberToString(SCM_RATNUM_DENOM(obj), radix, use_upper);
        Scm_DStringAdd(&ds, SCM_STRING(s));
        return Scm_DStringGet(&ds, 0);
    } else if (SCM_COMPNUMP(obj)) {
        ScmObj p = Scm_MakeOutputStringPort(TRUE);
        double_print(buf, FLT_BUF, SCM_COMPNUM_REAL(obj), FALSE);
        SCM_PUTZ(buf, -1, SCM_PORT(p));
        double_print(buf, FLT_BUF, SCM_COMPNUM_IMAG(obj), TRUE);
        SCM_PUTZ(buf, -1, SCM_PORT(p));
        SCM_PUTC('i', SCM_PORT(p));
        r = Scm_GetOutputString(SCM_PORT(p));
    } else {
        Scm_Error("number required: %S", obj);
    }
    return r;
}

/* utility to expose Burger&Dybvig algorithm.  FLAGS is not used yet,
   but reserved for future extension. */
void Scm_PrintDouble(ScmPort *port, double d, int flags)
{
    char buf[FLT_BUF];
    double_print(buf, FLT_BUF, d, FALSE);
    Scm_Putz(buf, strlen(buf), port);
}

/*
 * Number Parser
 *
 *  <number> : <prefix> <complex>
 *  <prefix> : <radix> <exactness> | <exactness> <radix>
 *  <radix>  : <empty> | '#b' | '#o' | '#d' | '#x'
 *  <exactness> : <empty> | '#e' | '#i'
 *  <complex> : <real>
 *            | <real> '@' <real>
 *            | <real> '+' <ureal> 'i'
 *            | <real> '-' <ureal> 'i'
 *            | <real> '+' 'i'
 *            | <real> '-' 'i'
 *            | '+' <ureal> 'i'
 *            | '-' <ureal> 'i'
 *            | '+' 'i'
 *            | '-' 'i'
 *  <real>   : <sign> <ureal>
 *  <sign>   : <empty> | '+' | '-'
 *  <ureal>  : <uinteger>
 *           | <uinteger> '/' <uinteger>
 *           | <decimal>
 *  <uinteger> : <digit>+ '#'*
 *  <decimal> : <digit10>+ '#'* <suffix>
 *            | '.' <digit10>+ '#'* <suffix>
 *            | <digit10>+ '.' <digit10>+ '#'* <suffix>
 *            | <digit10>+ '#'+ '.' '#'* <suffix>
 *  <suffix>  : <empty> | <exponent-marker> <sign> <digit10>+
 *  <exponent-marker> : 'e' | 's' | 'f' | 'd' | 'l'
 *
 * The parser reads characters from on-memory buffer.
 * Multibyte strings are filtered out in the early stage of
 * parsing, so the subroutines assume the buffer contains
 * only ASCII chars.
 */

struct numread_packet {
    const char *buffer;         /* original buffer */
    int buflen;                 /* original length */
    int radix;                  /* radix */
    int exactness;              /* exactness; see enum below */
    int padread;                /* '#' padding has been read */
    int strict;                 /* when true, reports an error if the
                                   input violates implementation limitation;
                                   otherwise, the routine returns #f. */
};

enum { /* used in the exactness flag */
    NOEXACT, EXACT, INEXACT
};

/* Max digits D such that all D-digit radix R integers fit in signed
   long, i.e. R^(D+1)-1 <= LONG_MAX */
static long longdigs[RADIX_MAX-RADIX_MIN+1] = { 0 };

/* Max integer I such that reading next digit (in radix R) will overflow
   long integer.   floor(LONG_MAX/R - R). */
static u_long longlimit[RADIX_MAX-RADIX_MIN+1] = { 0 };

/* An integer table of R^D, which is a "big digit" to be added
   into bignum. */
static u_long bigdig[RADIX_MAX-RADIX_MIN+1] = { 0 };

static ScmObj numread_error(const char *msg, struct numread_packet *context);

/* Returns either small integer or bignum.
   initval may be a Scheme integer that will be 'concatenated' before
   the integer to be read; it is used to read floating-point number.
   Note that value_big may keep denormalized bignum. */
static ScmObj read_uint(const char **strp, int *lenp,
                        struct numread_packet *ctx,
                        ScmObj initval)
{
    const char *str = *strp;
    int digread = FALSE;
    int len = *lenp;
    int radix = ctx->radix;
    int digits = 0, diglimit = longdigs[radix-RADIX_MIN];
    u_long limit = longlimit[radix-RADIX_MIN], bdig = bigdig[radix-RADIX_MIN];
    u_long value_int = 0;
    ScmBignum *value_big = NULL;
    char c;
    static const char tab[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    const char *ptab;

    if (!SCM_FALSEP(initval)) {
        if (SCM_INTP(initval)) {
            if (SCM_INT_VALUE(initval) > limit) {
                value_big = Scm_MakeBignumWithSize(4, SCM_INT_VALUE(initval));
            } else {
                value_int = SCM_INT_VALUE(initval);
            }
        } else if (SCM_BIGNUMP(initval)) {
            value_big = SCM_BIGNUM(Scm_BignumCopy(SCM_BIGNUM(initval)));
        }
        digread = TRUE;
    } else if (*str == '0') {
        /* Ignore leading 0's, to avoid unnecessary bignum operations. */
        while (len > 0 && *str == '0') { str++; len--; }
        digread = TRUE;
    }

    while (len--) {
        int digval = -1;
        c = tolower(*str++);
        if (ctx->padread) {
            if (c == '#') digval = 0;
            else break;
        } else if (digread && c == '#') {
            digval = 0;
            ctx->padread = TRUE;
            if (ctx->exactness == NOEXACT) {
                ctx->exactness = INEXACT;
            }
        } else {
            for (ptab = tab; ptab < tab+radix; ptab++) {
                if (c == *ptab) {
                    digval = ptab-tab;
                    digread = TRUE;
                    break;
                }
            }
        }
        if (digval < 0) break;
        value_int = value_int * radix + digval;
        digits++;
        if (value_big == NULL) {
            if (value_int >= limit) {
                value_big = Scm_MakeBignumWithSize(4, value_int);
                value_int = digits = 0;
            }
        } else if (digits > diglimit) {
            value_big = Scm_BignumAccMultAddUI(value_big, bdig, value_int);
            value_int = digits = 0;
        }
    }
    *strp = str-1;
    *lenp = len+1;

    if (value_big == NULL) return Scm_MakeInteger(value_int);
    if (digits > 0) {
        value_big = Scm_BignumAccMultAddUI(value_big, 
                                           ipow(radix, digits),
                                           value_int);
    }
    return Scm_NormalizeBignum(SCM_BIGNUM(value_big));
}

/*
 * Find a double number closest to f * 10^e, using z as the starting
 * approximation.  The algorithm (and its name) is taken from Will Clinger's
 * paper "How to Read Floating Point Numbers Accurately", in the ACM
 * SIGPLAN '90, pp.92--101.
 * The algorithm is modified to take advantage of coherency between loops.
 */
static double algorithmR(ScmObj f, int e, double z)
{
    ScmObj m, x, y, abs_d, d2;
    int k, s, kprev, sign_d;
    m = Scm_DecodeFlonum(z, &k, &s);
    IEXPT10_INIT();
  retry:
    if (k >= 0) {
        if (e >= 0) {
            x = Scm_Mul(f, iexpt10(e));
            y = Scm_Ash(m, k);
        } else {
            x = f;
            y = Scm_Ash(Scm_Mul(m, iexpt10(-e)), k);
        }
    } else {
        if (e >= 0) {
            x = Scm_Ash(Scm_Mul(f, iexpt10(e)), -k);
            y = m;
        } else {
            x = Scm_Ash(f, -k);
            y = Scm_Mul(m, iexpt10(-e));
        }
    }
    kprev = k;

    /* compare  */
    for (;;) {
        /*Scm_Printf(SCM_CURERR, "z=%.20lg,\nx=%S,\ny=%S\nf=%S\nm=%S\ne=%d, k=%d\n", z, x, y, f, m, e, k);*/
        /* compare */
        sign_d = Scm_NumCmp(x, y);
        abs_d = (sign_d > 0)? Scm_Sub(x, y) : Scm_Sub(y, x);
        d2 = Scm_Ash(Scm_Mul(m, abs_d), 1);
        switch (Scm_NumCmp(d2, y)) {
        case -1: /* d2 < y */
            if (Scm_NumCmp(m, SCM_2_52) == 0
                && sign_d < 0
                && Scm_NumCmp(Scm_Ash(d2, 1), y) > 0) {
                goto prevfloat;
            } else {
                return ldexp(Scm_GetDouble(m), k);
            }
        case 0: /* d2 == y */
            if (!Scm_OddP(m)) {
                if (Scm_NumCmp(m, SCM_2_52) == 0
                    && sign_d < 0) {
                    goto prevfloat;
                } else {
                    return ldexp(Scm_GetDouble(m), k);
                }
            } else if (sign_d < 0) {
                goto prevfloat;
            } else {
                goto nextfloat;
            }
        default:
            if (sign_d < 0) goto prevfloat;
            else            goto nextfloat;
        }
      prevfloat:
        m = Scm_Sub(m, SCM_MAKE_INT(1));
        if (k > -1074 && Scm_NumCmp(m, SCM_2_52) < 0) {
            m = Scm_Ash(m, 1);
            k--;
        }
        goto next;
      nextfloat:
        m = Scm_Add(m, SCM_MAKE_INT(1));
        if (Scm_NumCmp(m, SCM_2_53) >= 0) {
            m = Scm_Ash(m, -1);
            k++;
        }
        /*FALLTHROUGH*/
      next:
        if (kprev >= 0) {
            if (k >= 0) {
                /* k stays positive. x is invariant */
                if (e >= 0) {
                    y = Scm_Ash(m, k);
                } else {
                    y = Scm_Ash(Scm_Mul(m, iexpt10(-e)), k);
                }
            } else {
                /* k turned to negative */
                goto retry;
            }
        } else {
            if (k < 0) {
                /* k stays negative. */
                if (e >= 0) {
                    if (k != kprev) x = Scm_Ash(Scm_Mul(f, iexpt10(e)), -k);
                    y = m;
                } else {
                    if (k != kprev) x = Scm_Ash(f, -k);
                    y = Scm_Mul(m, iexpt10(-e));
                }
            } else {
                /* k turned to positive */
                goto retry;
            }
        }
    }
    /*NOTREACHED*/
}

static ScmObj read_real(const char **strp, int *lenp,
                        struct numread_packet *ctx)
{
    int minusp = FALSE, exp_minusp = FALSE, exp_overflow = FALSE;
    int fracdigs = 0;
    long exponent = 0;
    ScmObj intpart, fraction;

    switch (**strp) {
    case '-': minusp = TRUE;
        /* FALLTHROUGH */
    case '+':
        (*strp)++; (*lenp)--;
    }
    if ((*lenp) <= 0) return SCM_FALSE;

    /* Read integral part */
    if (**strp != '.') {
        intpart = read_uint(strp, lenp, ctx, SCM_FALSE);
        if ((*lenp) <= 0) {
            if (minusp) intpart = Scm_Negate(intpart);
            if (ctx->exactness == INEXACT) {
                return Scm_ExactToInexact(intpart);
            } else {
                return intpart;
            }
        }
        if (**strp == '/') {
            /* possibly rational */
            ScmObj denom;
            int lensave;
            
            if ((*lenp) <= 1) return SCM_FALSE;
            (*strp)++; (*lenp)--;
            lensave = *lenp;
            denom = read_uint(strp, lenp, ctx, SCM_FALSE);
            if (SCM_FALSEP(denom)) return SCM_FALSE;
            if (SCM_EXACT_ZERO_P(denom)) {
                if (lensave > *lenp) {
                    if (ctx->exactness == EXACT) {
                        return numread_error("(exact infinity/nan is not supported.)",
                                             ctx);
                    }
                    if (SCM_EXACT_ZERO_P(intpart)) return SCM_NAN;
                    return minusp? SCM_NEGATIVE_INFINITY:SCM_POSITIVE_INFINITY;
                } else {
                    return SCM_FALSE;
                }
            }
            if (minusp) intpart = Scm_Negate(intpart);
            if (ctx->exactness == INEXACT) {
                return Scm_ExactToInexact(Scm_Div(intpart, denom));
            } else {
                return Scm_MakeRational(intpart, denom);
            }
        }
        /* fallthrough */
    } else {
        intpart = SCM_FALSE; /* indicate there was no intpart */
    }

    /* Read fractional part.
       At this point, simple integer is already eliminated. */
    if (**strp == '.') {
        int lensave;
        if (ctx->radix != 10) {
            return numread_error("(only 10-based fraction is supported)", ctx);
        }
        (*strp)++; (*lenp)--;
        lensave = *lenp;
        fraction = read_uint(strp, lenp, ctx, intpart);
        fracdigs = lensave - *lenp;
    } else {
        fraction = intpart;
    }

    if (SCM_FALSEP(intpart)) {
        if (fracdigs == 0) return SCM_FALSE; /* input was "." */
    }

    /* Read exponent.  */
    if (*lenp > 0 && strchr("eEsSfFdDlL", (int)**strp)) {
        (*strp)++;
        if (--(*lenp) <= 0) return SCM_FALSE;
        switch (**strp) {
        case '-': exp_minusp = TRUE;
            /*FALLTHROUGH*/
        case '+':
            (*strp)++;
            if (--(*lenp) <= 0) return SCM_FALSE;
        }
        while (*lenp > 0) {
            int c = **strp;
            if (!isdigit(c)) break;
            (*strp)++, (*lenp)--;
            if (isdigit(c) && !exp_overflow) {
                exponent = exponent * 10 + (c - '0');
                /* Check obviously wrong exponent range.  More subtle check
                   will be done later. */
                if (exponent >= MAX_EXPONENT) {
                    exp_overflow = TRUE;
                }
            }
        }
        if (exp_minusp) exponent = -exponent;
    }
    if (exp_overflow) {
        if (ctx->exactness == EXACT) {
            /* Although we can represent such a number using bignum and
               ratnum, such large (or small) exponent is highly unusual
               and we assume we can report implementation limitation
               violation. */
            return numread_error("(such an exact number is out of implementation limitation)",
                                 ctx);
        }
        if (exp_minusp) {
            return Scm_MakeFlonum(0.0);
        } else {
            return minusp? SCM_NEGATIVE_INFINITY : SCM_POSITIVE_INFINITY;
        }
    }

    /*Scm_Printf(SCM_CURERR, "fraction=%S, exponent=%d\n", fraction, exponent);*/

    /* Compose the number. */
    if (ctx->exactness == EXACT) {
        /* Explicit exact number.  We can continue exact arithmetic
           (it may end up ratnum) */
        ScmObj e = Scm_Mul(fraction,
                           exact_expt(SCM_MAKE_INT(10),
                                      Scm_MakeInteger(exponent-fracdigs)));
        if (minusp) return Scm_Negate(e);
        else        return e;
    } else {
        double realnum = Scm_GetDouble(fraction);

        realnum = raise_pow10(realnum, exponent-fracdigs);

        if (SCM_IS_INF(realnum)) {
            /* Special case.  We catch too big exponent here. */
            return (minusp? SCM_NEGATIVE_INFINITY : SCM_POSITIVE_INFINITY);
        }

        if (realnum > 0.0
            && (Scm_NumCmp(fraction, SCM_2_52) > 0
                || exponent-fracdigs > MAX_EXACT_10_EXP
                || exponent-fracdigs < -MAX_EXACT_10_EXP)) {
            realnum = algorithmR(fraction, exponent-fracdigs, realnum);
        }
        if (minusp) realnum = -realnum;
        return Scm_MakeFlonum(realnum);
    }
}

/* Entry point */
static ScmObj read_number(const char *str, int len, int radix, int strict)
{
    struct numread_packet ctx;
    int radix_seen = 0, exactness_seen = 0, sign_seen = 0;
    ScmObj realpart;

    ctx.buffer = str;
    ctx.buflen = len;
    ctx.exactness = NOEXACT;
    ctx.padread = FALSE;
    ctx.strict = strict;

#define CHK_EXACT_COMPLEX()                                                 \
    do {                                                                    \
        if (ctx.exactness == EXACT) {                                       \
            return numread_error("(exact complex number is not supported)", \
                                 &ctx);                                     \
        }                                                                   \
    } while (0)

    /* suggested radix.  may be overridden by prefix. */
    if (radix <= 1 || radix > 36) return SCM_FALSE;
    ctx.radix = radix;
    
    /* start from prefix part */
    for (; len >= 0; len-=2) {
        if (*str != '#') break;
        str++;
        switch (*str++) {
        case 'x':; case 'X':;
            if (radix_seen) return SCM_FALSE;
            ctx.radix = 16; radix_seen++;
            continue;
        case 'o':; case 'O':;
            if (radix_seen) return SCM_FALSE;
            ctx.radix = 8; radix_seen++;
            continue;
        case 'b':; case 'B':;
            if (radix_seen) return SCM_FALSE;
            ctx.radix = 2; radix_seen++;
            continue;
        case 'd':; case 'D':;
            if (radix_seen) return SCM_FALSE;
            ctx.radix = 10; radix_seen++;
            continue;
        case 'e':; case 'E':;
            if (exactness_seen) return SCM_FALSE;
            ctx.exactness = EXACT; exactness_seen++;
            continue;
        case 'i':; case 'I':;
            if (exactness_seen) return SCM_FALSE;
            ctx.exactness = INEXACT; exactness_seen++;
            continue;
        }
        return SCM_FALSE;
    }
    if (len <= 0) return SCM_FALSE;

    /* number body.  need to check the special case of pure imaginary */
    if (*str == '+' || *str == '-') {
        if (len == 1) return SCM_FALSE;
        if (len == 2 && (str[1] == 'i' || str[1] == 'I')) {
            CHK_EXACT_COMPLEX();
            return Scm_MakeComplex(0.0, (*str == '+')? 1.0 : -1.0);
        }
        sign_seen = TRUE;
    }

    realpart = read_real(&str, &len, &ctx);
    if (SCM_FALSEP(realpart) || len == 0) return realpart;

    switch (*str) {
    case '@':
        /* polar representation of complex*/
        if (len <= 1) {
            return SCM_FALSE;
        } else {
            ScmObj angle;
            double dmag, dangle;
            str++; len--;
            angle = read_real(&str, &len, &ctx);
            if (SCM_FALSEP(angle) || len != 0) return SCM_FALSE;
            CHK_EXACT_COMPLEX();
            dmag = Scm_GetDouble(realpart);
            dangle = Scm_GetDouble(angle);
            return Scm_MakeComplexPolar(dmag, dangle);
        }
    case '+':;
    case '-':
        /* rectangular representation of complex */
        if (len <= 1) {
            return SCM_FALSE;
        } else if (len == 2 && str[1] == 'i') {
            return Scm_MakeComplex(Scm_GetDouble(realpart),
                                   (*str == '+' ? 1.0 : -1.0));
        } else {
            ScmObj imagpart = read_real(&str, &len, &ctx);
            if (SCM_FALSEP(imagpart) || len != 1 || *str != 'i') {
                return SCM_FALSE;
            }
            CHK_EXACT_COMPLEX();
            if (Scm_Sign(imagpart) == 0) return realpart;
            return Scm_MakeComplex(Scm_GetDouble(realpart), 
                                   Scm_GetDouble(imagpart));
        }
    case 'i':
        /* '+' <ureal> 'i'  or '-' <ureal> 'i' */
        if (!sign_seen || len != 1) return SCM_FALSE;
        CHK_EXACT_COMPLEX();
        if (Scm_Sign(realpart) == 0) return Scm_MakeFlonum(0.0);
        else return Scm_MakeComplex(0.0, Scm_GetDouble(realpart));
    default:
        return SCM_FALSE;
    }
}

static ScmObj numread_error(const char *msg, struct numread_packet *context)
{
    if (context->strict) {
        Scm_Error("bad number format %s: %A", msg,
                  Scm_MakeString(context->buffer, context->buflen,
                                 context->buflen, 0));
    }
    return SCM_FALSE;
}


ScmObj Scm_StringToNumber(ScmString *str, int radix, int strict)
{
    u_int len, size;
    const char *p = Scm_GetStringContent(str, &size, &len, NULL);
    if (size != len) {
        /* This can't be a proper number. */
        return SCM_FALSE;
    } else {
        return read_number(p, size, radix, strict);
    }
}

/*
 * Initialization
 */

ScmObj Scm__ConstObjs[SCM_NUM_CONST_OBJS] = { SCM_FALSE };

void Scm__InitNumber(void)
{
    ScmModule *mod = Scm_GaucheModule();
    int radix, i;
    u_long n;
    
    for (radix = RADIX_MIN; radix <= RADIX_MAX; radix++) {
        longlimit[radix-RADIX_MIN] =
            (u_long)floor((double)LONG_MAX/radix - radix);
        /* Find max D where R^(D+1)-1 <= LONG_MAX */
        for (i = 0, n = 1; ; i++, n *= radix) {
            if (n >= LONG_MAX/radix) {
                longdigs[radix-RADIX_MIN] = i-1;
                bigdig[radix-RADIX_MIN] = n;
                break;
            }
        }
    }
    
    SCM_2_63 = Scm_Ash(SCM_MAKE_INT(1), 63);
    SCM_2_64 = Scm_Ash(SCM_MAKE_INT(1), 64);
    SCM_2_64_MINUS_1 = Scm_Sub(SCM_2_64, SCM_MAKE_INT(1));
    SCM_2_52 = Scm_Ash(SCM_MAKE_INT(1), 52);
    SCM_2_53 = Scm_Ash(SCM_MAKE_INT(1), 53);
    SCM_MINUS_2_63 = Scm_Negate(SCM_2_63);
    SCM_2_32 = Scm_Ash(SCM_MAKE_INT(1), 32);
    SCM_2_31 = Scm_Ash(SCM_MAKE_INT(1), 31);
    SCM_MINUS_2_31 = Scm_Negate(SCM_2_31);

    SCM_POSITIVE_INFINITY = Scm_MakeFlonum(1.0/0.0);
    SCM_NEGATIVE_INFINITY = Scm_MakeFlonum(-1.0/0.0);
    SCM_NAN               = Scm_MakeFlonum(0.0/0.0);
    
    dexpt2_minus_52 = ldexp(1.0, -52);
    dexpt2_minus_53 = ldexp(1.0, -53);

    Scm_InitBuiltinGeneric(&generic_add, "object-+", mod);
    Scm_InitBuiltinGeneric(&generic_sub, "object--", mod);
    Scm_InitBuiltinGeneric(&generic_mul, "object-*", mod);
    Scm_InitBuiltinGeneric(&generic_div, "object-/", mod);
}
