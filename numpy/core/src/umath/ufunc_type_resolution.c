/*
 * This file implements type resolution for NumPy element-wise ufuncs.
 * This mechanism is still backwards-compatible with the pre-existing
 * legacy mechanism, so performs much slower than is necessary.
 *
 * Written by Mark Wiebe (mwwiebe@gmail.com)
 * Copyright (c) 2011 by Enthought, Inc.
 *
 * See LICENSE.txt for the license.
 */
#define _UMATHMODULE
#define NPY_NO_DEPRECATED_API

#include "Python.h"

#include "npy_config.h"
#ifdef ENABLE_SEPARATE_COMPILATION
#define PY_ARRAY_UNIQUE_SYMBOL _npy_umathmodule_ARRAY_API
#define NO_IMPORT_ARRAY
#endif

#include "numpy/npy_3kcompat.h"

#include "numpy/ufuncobject.h"
#include "ufunc_type_resolution.h"

static const char *
npy_casting_to_string(NPY_CASTING casting)
{
    switch (casting) {
        case NPY_NO_CASTING:
            return "'no'";
        case NPY_EQUIV_CASTING:
            return "'equiv'";
        case NPY_SAFE_CASTING:
            return "'safe'";
        case NPY_SAME_KIND_CASTING:
            return "'same_kind'";
        case NPY_UNSAFE_CASTING:
            return "'unsafe'";
        default:
            return "<unknown>";
    }
}
/*UFUNC_API
 *
 * Validates that the input operands can be cast to
 * the input types, and the output types can be cast to
 * the output operands where provided.
 *
 * Returns 0 on success, -1 (with exception raised) on validation failure.
 */
NPY_NO_EXPORT int
PyUFunc_ValidateCasting(PyUFuncObject *ufunc,
                            NPY_CASTING casting,
                            PyArrayObject **operands,
                            PyArray_Descr **dtypes)
{
    int i, nin = ufunc->nin, nop = nin + ufunc->nout;
    char *ufunc_name;

    ufunc_name = ufunc->name ? ufunc->name : "<unnamed ufunc>";

    for (i = 0; i < nop; ++i) {
        if (i < nin) {
            if (!PyArray_CanCastArrayTo(operands[i], dtypes[i], casting)) {
                PyObject *errmsg;
                errmsg = PyUString_FromFormat("Cannot cast ufunc %s "
                                "input from ", ufunc_name);
                PyUString_ConcatAndDel(&errmsg,
                        PyObject_Repr((PyObject *)PyArray_DESCR(operands[i])));
                PyUString_ConcatAndDel(&errmsg,
                        PyUString_FromString(" to "));
                PyUString_ConcatAndDel(&errmsg,
                        PyObject_Repr((PyObject *)dtypes[i]));
                PyUString_ConcatAndDel(&errmsg,
                        PyUString_FromFormat(" with casting rule %s",
                                        npy_casting_to_string(casting)));
                PyErr_SetObject(PyExc_TypeError, errmsg);
                return -1;
            }
        } else if (operands[i] != NULL) {
            if (!PyArray_CanCastTypeTo(dtypes[i],
                                    PyArray_DESCR(operands[i]), casting)) {
                PyObject *errmsg;
                errmsg = PyUString_FromFormat("Cannot cast ufunc %s "
                                "output from ", ufunc_name);
                PyUString_ConcatAndDel(&errmsg,
                        PyObject_Repr((PyObject *)dtypes[i]));
                PyUString_ConcatAndDel(&errmsg,
                        PyUString_FromString(" to "));
                PyUString_ConcatAndDel(&errmsg,
                        PyObject_Repr((PyObject *)PyArray_DESCR(operands[i])));
                PyUString_ConcatAndDel(&errmsg,
                        PyUString_FromFormat(" with casting rule %s",
                                        npy_casting_to_string(casting)));
                PyErr_SetObject(PyExc_TypeError, errmsg);
                return -1;
            }
        }
    }

    return 0;
}

/*
 * Returns a new reference to type if it is already NBO, otherwise
 * returns a copy converted to NBO.
 */
static PyArray_Descr *
ensure_dtype_nbo(PyArray_Descr *type)
{
    if (PyArray_ISNBO(type->byteorder)) {
        Py_INCREF(type);
        return type;
    }
    else {
        return PyArray_DescrNewByteorder(type, NPY_NATIVE);
    }
}

/*UFUNC_API
 *
 * This function applies the default type resolution rules
 * for the provided ufunc, filling out_dtypes, out_innerloop,
 * and out_innerloopdata.
 *
 * Returns 0 on success, -1 on error.
 */
NPY_NO_EXPORT int
PyUFunc_DefaultTypeResolution(PyUFuncObject *ufunc,
                                NPY_CASTING casting,
                                PyArrayObject **operands,
                                PyObject *type_tup,
                                PyArray_Descr **out_dtypes,
                                PyUFuncGenericFunction *out_innerloop,
                                void **out_innerloopdata)
{
    int i, nop = ufunc->nin + ufunc->nout;
    int retval = 0, any_object = 0;
    NPY_CASTING input_casting;

    for (i = 0; i < nop; ++i) {
        if (operands[i] != NULL &&
                PyTypeNum_ISOBJECT(PyArray_DESCR(operands[i])->type_num)) {
            any_object = 1;
            break;
        }
    }

    /*
     * Decide the casting rules for inputs and outputs.  We want
     * NPY_SAFE_CASTING or stricter, so that the loop selection code
     * doesn't choose an integer loop for float inputs, for example.
     */
    input_casting = (casting > NPY_SAFE_CASTING) ? NPY_SAFE_CASTING : casting;

    if (type_tup == NULL) {
        /* Find the best ufunc inner loop, and fill in the dtypes */
        retval = find_best_ufunc_inner_loop(ufunc, operands,
                        input_casting, casting, any_object,
                        out_dtypes, out_innerloop, out_innerloopdata);
    } else {
        /* Find the specified ufunc inner loop, and fill in the dtypes */
        retval = find_specified_ufunc_inner_loop(ufunc, type_tup,
                        operands, casting, any_object, out_dtypes,
                        out_innerloop, out_innerloopdata);
    }

    return retval;
}

/*
 * This function applies special type resolution rules for the case
 * where all the functions have the pattern XX->bool, using
 * PyArray_ResultType instead of a linear search to get the best
 * loop.
 *
 * Note that a simpler linear search through the functions loop
 * is still done, but switching to a simple array lookup for
 * built-in types would be better at some point.
 *
 * Returns 0 on success, -1 on error.
 */
NPY_NO_EXPORT int
PyUFunc_SimpleBinaryComparisonTypeResolution(PyUFuncObject *ufunc,
                                NPY_CASTING casting,
                                PyArrayObject **operands,
                                PyObject *type_tup,
                                PyArray_Descr **out_dtypes,
                                PyUFuncGenericFunction *out_innerloop,
                                void **out_innerloopdata)
{
    int i, type_num, type_num1, type_num2;
    char *ufunc_name;

    ufunc_name = ufunc->name ? ufunc->name : "<unnamed ufunc>";

    if (ufunc->nin != 2 || ufunc->nout != 1) {
        PyErr_Format(PyExc_RuntimeError, "ufunc %s is configured "
                "to use binary comparison type resolution but has "
                "the wrong number of inputs or outputs",
                ufunc_name);
        return -1;
    }

    /*
     * Use the default type resolution if there's a custom data type
     * or object arrays.
     */
    type_num1 = PyArray_DESCR(operands[0])->type_num;
    type_num2 = PyArray_DESCR(operands[1])->type_num;
    if (type_num1 >= NPY_NTYPES || type_num2 >= NPY_NTYPES ||
            type_num1 == NPY_OBJECT || type_num2 == NPY_OBJECT) {
        return PyUFunc_DefaultTypeResolution(ufunc, casting, operands,
                type_tup, out_dtypes, out_innerloop, out_innerloopdata);
    }

    if (type_tup == NULL) {
        /* Input types are the result type */
        out_dtypes[0] = PyArray_ResultType(2, operands, 0, NULL);
        if (out_dtypes[0] == NULL) {
            return -1;
        }
        out_dtypes[1] = out_dtypes[0];
        Py_INCREF(out_dtypes[1]);
    }
    else {
        /*
         * If the type tuple isn't a single-element tuple, let the
         * default type resolution handle this one.
         */
        if (!PyTuple_Check(type_tup) || PyTuple_GET_SIZE(type_tup) != 1) {
            return PyUFunc_DefaultTypeResolution(ufunc, casting,
                    operands, type_tup, out_dtypes,
                    out_innerloop, out_innerloopdata);
        }

        if (!PyArray_DescrCheck(PyTuple_GET_ITEM(type_tup, 0))) {
            PyErr_SetString(PyExc_ValueError,
                    "require data type in the type tuple");
            return -1;
        }

        out_dtypes[0] = ensure_dtype_nbo(
                            (PyArray_Descr *)PyTuple_GET_ITEM(type_tup, 0));
        if (out_dtypes[0] == NULL) {
            return -1;
        }
        out_dtypes[1] = out_dtypes[0];
        Py_INCREF(out_dtypes[1]);
    }

    /* Output type is always boolean */
    out_dtypes[2] = PyArray_DescrFromType(NPY_BOOL);
    if (out_dtypes[2] == NULL) {
        for (i = 0; i < 2; ++i) {
            Py_DECREF(out_dtypes[i]);
            out_dtypes[i] = NULL;
        }
        return -1;
    }

    /* Check against the casting rules */
    if (PyUFunc_ValidateCasting(ufunc, casting, operands, out_dtypes) < 0) {
        for (i = 0; i < 3; ++i) {
            Py_DECREF(out_dtypes[i]);
            out_dtypes[i] = NULL;
        }
        return -1;
    }

    type_num = out_dtypes[0]->type_num;

    /* If we have a built-in type, search in the functions list */
    if (type_num < NPY_NTYPES) {
        char *types = ufunc->types;
        int n = ufunc->ntypes;

        for (i = 0; i < n; ++i) {
            if (types[3*i] == type_num) {
                *out_innerloop = ufunc->functions[i];
                *out_innerloopdata = ufunc->data[i];
                return 0;
            }
        }

        PyErr_Format(PyExc_TypeError,
                "ufunc '%s' not supported for the input types",
                ufunc_name);
        return -1;
    }
    else {
        PyErr_SetString(PyExc_RuntimeError,
                "user type shouldn't have resulted from type promotion");
        return -1;
    }
}

/*
 * This function applies special type resolution rules for the case
 * where all the functions have the pattern X->X, copying
 * the input descr directly so that metadata is maintained.
 *
 * Note that a simpler linear search through the functions loop
 * is still done, but switching to a simple array lookup for
 * built-in types would be better at some point.
 *
 * Returns 0 on success, -1 on error.
 */
NPY_NO_EXPORT int
PyUFunc_SimpleUnaryOperationTypeResolution(PyUFuncObject *ufunc,
                                NPY_CASTING casting,
                                PyArrayObject **operands,
                                PyObject *type_tup,
                                PyArray_Descr **out_dtypes,
                                PyUFuncGenericFunction *out_innerloop,
                                void **out_innerloopdata)
{
    int i, type_num, type_num1;
    char *ufunc_name;

    ufunc_name = ufunc->name ? ufunc->name : "<unnamed ufunc>";

    if (ufunc->nin != 1 || ufunc->nout != 1) {
        PyErr_Format(PyExc_RuntimeError, "ufunc %s is configured "
                "to use unary operation type resolution but has "
                "the wrong number of inputs or outputs",
                ufunc_name);
        return -1;
    }

    /*
     * Use the default type resolution if there's a custom data type
     * or object arrays.
     */
    type_num1 = PyArray_DESCR(operands[0])->type_num;
    if (type_num1 >= NPY_NTYPES || type_num1 == NPY_OBJECT) {
        return PyUFunc_DefaultTypeResolution(ufunc, casting, operands,
                type_tup, out_dtypes, out_innerloop, out_innerloopdata);
    }

    if (type_tup == NULL) {
        /* Input types are the result type */
        out_dtypes[0] = ensure_dtype_nbo(PyArray_DESCR(operands[0]));
        if (out_dtypes[0] == NULL) {
            return -1;
        }
        out_dtypes[1] = out_dtypes[0];
        Py_INCREF(out_dtypes[1]);
    }
    else {
        /*
         * If the type tuple isn't a single-element tuple, let the
         * default type resolution handle this one.
         */
        if (!PyTuple_Check(type_tup) || PyTuple_GET_SIZE(type_tup) != 1) {
            return PyUFunc_DefaultTypeResolution(ufunc, casting,
                    operands, type_tup, out_dtypes,
                    out_innerloop, out_innerloopdata);
        }

        if (!PyArray_DescrCheck(PyTuple_GET_ITEM(type_tup, 0))) {
            PyErr_SetString(PyExc_ValueError,
                    "require data type in the type tuple");
            return -1;
        }

        out_dtypes[0] = ensure_dtype_nbo(
                            (PyArray_Descr *)PyTuple_GET_ITEM(type_tup, 0));
        if (out_dtypes[0] == NULL) {
            return -1;
        }
        out_dtypes[1] = out_dtypes[0];
        Py_INCREF(out_dtypes[1]);
    }

    /* Check against the casting rules */
    if (PyUFunc_ValidateCasting(ufunc, casting, operands, out_dtypes) < 0) {
        for (i = 0; i < 2; ++i) {
            Py_DECREF(out_dtypes[i]);
            out_dtypes[i] = NULL;
        }
        return -1;
    }

    type_num = out_dtypes[0]->type_num;

    /* If we have a built-in type, search in the functions list */
    if (type_num < NPY_NTYPES) {
        char *types = ufunc->types;
        int n = ufunc->ntypes;

        for (i = 0; i < n; ++i) {
            if (types[2*i] == type_num) {
                *out_innerloop = ufunc->functions[i];
                *out_innerloopdata = ufunc->data[i];
                return 0;
            }
        }

        PyErr_Format(PyExc_TypeError,
                "ufunc '%s' not supported for the input types",
                ufunc_name);
        return -1;
    }
    else {
        PyErr_SetString(PyExc_RuntimeError,
                "user type shouldn't have resulted from type promotion");
        return -1;
    }
}

/*
 * The ones_like function shouldn't really be a ufunc, but while it
 * still is, this provides type resolution that always forces UNSAFE
 * casting.
 */
NPY_NO_EXPORT int
PyUFunc_OnesLikeTypeResolution(PyUFuncObject *ufunc,
                                NPY_CASTING NPY_UNUSED(casting),
                                PyArrayObject **operands,
                                PyObject *type_tup,
                                PyArray_Descr **out_dtypes,
                                PyUFuncGenericFunction *out_innerloop,
                                void **out_innerloopdata)
{
    return PyUFunc_SimpleUnaryOperationTypeResolution(ufunc,
                        NPY_UNSAFE_CASTING,
                        operands, type_tup, out_dtypes,
                        out_innerloop, out_innerloopdata);
}


/*
 * This function applies special type resolution rules for the case
 * where all the functions have the pattern XX->X, using
 * PyArray_ResultType instead of a linear search to get the best
 * loop.
 *
 * Note that a simpler linear search through the functions loop
 * is still done, but switching to a simple array lookup for
 * built-in types would be better at some point.
 *
 * Returns 0 on success, -1 on error.
 */
NPY_NO_EXPORT int
PyUFunc_SimpleBinaryOperationTypeResolution(PyUFuncObject *ufunc,
                                NPY_CASTING casting,
                                PyArrayObject **operands,
                                PyObject *type_tup,
                                PyArray_Descr **out_dtypes,
                                PyUFuncGenericFunction *out_innerloop,
                                void **out_innerloopdata)
{
    int i, type_num, type_num1, type_num2;
    char *ufunc_name;

    ufunc_name = ufunc->name ? ufunc->name : "<unnamed ufunc>";

    if (ufunc->nin != 2 || ufunc->nout != 1) {
        PyErr_Format(PyExc_RuntimeError, "ufunc %s is configured "
                "to use binary operation type resolution but has "
                "the wrong number of inputs or outputs",
                ufunc_name);
        return -1;
    }

    /*
     * Use the default type resolution if there's a custom data type
     * or object arrays.
     */
    type_num1 = PyArray_DESCR(operands[0])->type_num;
    type_num2 = PyArray_DESCR(operands[1])->type_num;
    if (type_num1 >= NPY_NTYPES || type_num2 >= NPY_NTYPES ||
            type_num1 == NPY_OBJECT || type_num2 == NPY_OBJECT) {
        return PyUFunc_DefaultTypeResolution(ufunc, casting, operands,
                type_tup, out_dtypes, out_innerloop, out_innerloopdata);
    }

    if (type_tup == NULL) {
        /* Input types are the result type */
        out_dtypes[0] = PyArray_ResultType(2, operands, 0, NULL);
        if (out_dtypes[0] == NULL) {
            return -1;
        }
        out_dtypes[1] = out_dtypes[0];
        Py_INCREF(out_dtypes[1]);
        out_dtypes[2] = out_dtypes[0];
        Py_INCREF(out_dtypes[2]);
    }
    else {
        /*
         * If the type tuple isn't a single-element tuple, let the
         * default type resolution handle this one.
         */
        if (!PyTuple_Check(type_tup) || PyTuple_GET_SIZE(type_tup) != 1) {
            return PyUFunc_DefaultTypeResolution(ufunc, casting,
                    operands, type_tup, out_dtypes,
                    out_innerloop, out_innerloopdata);
        }

        if (!PyArray_DescrCheck(PyTuple_GET_ITEM(type_tup, 0))) {
            PyErr_SetString(PyExc_ValueError,
                    "require data type in the type tuple");
            return -1;
        }

        out_dtypes[0] = ensure_dtype_nbo(
                            (PyArray_Descr *)PyTuple_GET_ITEM(type_tup, 0));
        if (out_dtypes[0] == NULL) {
            return -1;
        }
        out_dtypes[1] = out_dtypes[0];
        Py_INCREF(out_dtypes[1]);
        out_dtypes[2] = out_dtypes[0];
        Py_INCREF(out_dtypes[2]);
    }

    /* Check against the casting rules */
    if (PyUFunc_ValidateCasting(ufunc, casting, operands, out_dtypes) < 0) {
        for (i = 0; i < 3; ++i) {
            Py_DECREF(out_dtypes[i]);
            out_dtypes[i] = NULL;
        }
        return -1;
    }

    type_num = out_dtypes[0]->type_num;

    /* If we have a built-in type, search in the functions list */
    if (type_num < NPY_NTYPES) {
        char *types = ufunc->types;
        int n = ufunc->ntypes;

        for (i = 0; i < n; ++i) {
            if (types[3*i] == type_num) {
                *out_innerloop = ufunc->functions[i];
                *out_innerloopdata = ufunc->data[i];
                return 0;
            }
        }

        PyErr_Format(PyExc_TypeError,
                "ufunc '%s' not supported for the input types",
                ufunc_name);
        return -1;
    }
    else {
        PyErr_SetString(PyExc_RuntimeError,
                "user type shouldn't have resulted from type promotion");
        return -1;
    }
}

/*
 * This function applies special type resolution rules for the absolute
 * ufunc. This ufunc converts complex -> float, so isn't covered
 * by the simple unary type resolution.
 *
 * Returns 0 on success, -1 on error.
 */
NPY_NO_EXPORT int
PyUFunc_AbsoluteTypeResolution(PyUFuncObject *ufunc,
                                NPY_CASTING casting,
                                PyArrayObject **operands,
                                PyObject *type_tup,
                                PyArray_Descr **out_dtypes,
                                PyUFuncGenericFunction *out_innerloop,
                                void **out_innerloopdata)
{
    /* Use the default for complex types, to find the loop producing float */
    if (PyTypeNum_ISCOMPLEX(PyArray_DESCR(operands[0])->type_num)) {
        return PyUFunc_DefaultTypeResolution(ufunc, casting, operands,
                    type_tup, out_dtypes, out_innerloop, out_innerloopdata);
    }
    else {
        return PyUFunc_SimpleUnaryOperationTypeResolution(ufunc, casting,
                    operands, type_tup, out_dtypes,
                    out_innerloop, out_innerloopdata);
    }
}


/*
 * This function returns the a new reference to the
 * capsule with the datetime metadata.
 *
 * NOTE: This function is copied from datetime.c in multiarray,
 *       because umath and multiarray are not linked together.
 */
static PyObject *
get_datetime_metacobj_from_dtype(PyArray_Descr *dtype)
{
    PyObject *metacobj;

    /* Check that the dtype has metadata */
    if (dtype->metadata == NULL) {
        PyErr_SetString(PyExc_TypeError,
                "Datetime type object is invalid, lacks metadata");
        return NULL;
    }

    /* Check that the dtype has unit metadata */
    metacobj = PyDict_GetItemString(dtype->metadata, NPY_METADATA_DTSTR);
    if (metacobj == NULL) {
        PyErr_SetString(PyExc_TypeError,
                "Datetime type object is invalid, lacks unit metadata");
        return NULL;
    }

    Py_INCREF(metacobj);
    return metacobj;
}

/*
 * Creates a new NPY_TIMEDELTA dtype, copying the datetime metadata
 * from the given dtype.
 *
 * NOTE: This function is copied from datetime.c in multiarray,
 *       because umath and multiarray are not linked together.
 */
static PyArray_Descr *
timedelta_dtype_with_copied_meta(PyArray_Descr *dtype)
{
    PyArray_Descr *ret;
    PyObject *metacobj;

    ret = PyArray_DescrNewFromType(NPY_TIMEDELTA);
    if (ret == NULL) {
        return NULL;
    }
    Py_XDECREF(ret->metadata);
    ret->metadata = PyDict_New();
    if (ret->metadata == NULL) {
        Py_DECREF(ret);
        return NULL;
    }

    metacobj = get_datetime_metacobj_from_dtype(dtype);
    if (metacobj == NULL) {
        Py_DECREF(ret);
        return NULL;
    }

    if (PyDict_SetItemString(ret->metadata, NPY_METADATA_DTSTR,
                                                metacobj) < 0) {
        Py_DECREF(metacobj);
        Py_DECREF(ret);
        return NULL;
    }

    return ret;
}

/*
 * This function applies the type resolution rules for addition.
 * In particular, there are a number of special cases with datetime:
 *    m8[<A>] + m8[<B>] => m8[gcd(<A>,<B>)] + m8[gcd(<A>,<B>)]
 *    m8[<A>] + int     => m8[<A>] + m8[<A>]
 *    int     + m8[<A>] => m8[<A>] + m8[<A>]
 *    M8[<A>] + int     => M8[<A>] + m8[<A>]
 *    int     + M8[<A>] => m8[<A>] + M8[<A>]
 *    M8[<A>] + m8[<B>] => M8[gcd(<A>,<B>)] + m8[gcd(<A>,<B>)]
 *    m8[<A>] + M8[<B>] => m8[gcd(<A>,<B>)] + M8[gcd(<A>,<B>)]
 * TODO: Non-linear time unit cases require highly special-cased loops
 *    M8[<A>] + m8[Y|M|B]
 *    m8[Y|M|B] + M8[<A>]
 */
NPY_NO_EXPORT int
PyUFunc_AdditionTypeResolution(PyUFuncObject *ufunc,
                                NPY_CASTING casting,
                                PyArrayObject **operands,
                                PyObject *type_tup,
                                PyArray_Descr **out_dtypes,
                                PyUFuncGenericFunction *out_innerloop,
                                void **out_innerloopdata)
{
    int type_num1, type_num2;
    char *types;
    int i, n;
    char *ufunc_name;

    ufunc_name = ufunc->name ? ufunc->name : "<unnamed ufunc>";

    type_num1 = PyArray_DESCR(operands[0])->type_num;
    type_num2 = PyArray_DESCR(operands[1])->type_num;

    /* Use the default when datetime and timedelta are not involved */
    if (!PyTypeNum_ISDATETIME(type_num1) && !PyTypeNum_ISDATETIME(type_num2)) {
        return PyUFunc_DefaultTypeResolution(ufunc, casting, operands,
                    type_tup, out_dtypes, out_innerloop, out_innerloopdata);
    }

    if (type_num1 == NPY_TIMEDELTA) {
        /* m8[<A>] + m8[<B>] => m8[gcd(<A>,<B>)] + m8[gcd(<A>,<B>)] */
        if (type_num2 == NPY_TIMEDELTA) {
            out_dtypes[0] = PyArray_PromoteTypes(PyArray_DESCR(operands[0]),
                                                PyArray_DESCR(operands[1]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = out_dtypes[0];
            Py_INCREF(out_dtypes[1]);
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);
        }
        /* m8[<A>] + M8[<B>] => m8[gcd(<A>,<B>)] + M8[gcd(<A>,<B>)] */
        else if (type_num2 == NPY_DATETIME) {
            out_dtypes[1] = PyArray_PromoteTypes(PyArray_DESCR(operands[0]),
                                                PyArray_DESCR(operands[1]));
            if (out_dtypes[1] == NULL) {
                return -1;
            }
            /* Make a new NPY_TIMEDELTA, and copy the datetime's metadata */
            out_dtypes[0] = timedelta_dtype_with_copied_meta(out_dtypes[1]);
            if (out_dtypes[0] == NULL) {
                Py_DECREF(out_dtypes[1]);
                out_dtypes[1] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[1];
            Py_INCREF(out_dtypes[2]);
        }
        /* m8[<A>] + int => m8[<A>] + m8[<A>] */
        else if (PyTypeNum_ISINTEGER(type_num2) ||
                                    PyTypeNum_ISBOOL(type_num2)) {
            out_dtypes[0] = ensure_dtype_nbo(PyArray_DESCR(operands[0]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = out_dtypes[0];
            Py_INCREF(out_dtypes[1]);
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);

            type_num2 = NPY_TIMEDELTA;
        }
        else {
            goto type_reso_error;
        }
    }
    else if (type_num1 == NPY_DATETIME) {
        /* M8[<A>] + m8[<B>] => M8[gcd(<A>,<B>)] + m8[gcd(<A>,<B>)] */
        if (type_num2 == NPY_TIMEDELTA) {
            out_dtypes[0] = PyArray_PromoteTypes(PyArray_DESCR(operands[0]),
                                                PyArray_DESCR(operands[1]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            /* Make a new NPY_TIMEDELTA, and copy the datetime's metadata */
            out_dtypes[1] = timedelta_dtype_with_copied_meta(out_dtypes[0]);
            if (out_dtypes[1] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);
        }
        /* M8[<A>] + int => M8[<A>] + m8[<A>] */
        else if (PyTypeNum_ISINTEGER(type_num2) ||
                    PyTypeNum_ISBOOL(type_num2)) {
            out_dtypes[0] = ensure_dtype_nbo(PyArray_DESCR(operands[0]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            /* Make a new NPY_TIMEDELTA, and copy type1's metadata */
            out_dtypes[1] = timedelta_dtype_with_copied_meta(
                                            PyArray_DESCR(operands[0]));
            if (out_dtypes[1] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);

            type_num2 = NPY_TIMEDELTA;
        }
        else {
            goto type_reso_error;
        }
    }
    else if (PyTypeNum_ISINTEGER(type_num1) || PyTypeNum_ISBOOL(type_num1)) {
        /* int + m8[<A>] => m8[<A>] + m8[<A>] */
        if (type_num2 == NPY_TIMEDELTA) {
            out_dtypes[0] = ensure_dtype_nbo(PyArray_DESCR(operands[1]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = out_dtypes[0];
            Py_INCREF(out_dtypes[1]);
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);

            type_num1 = NPY_TIMEDELTA;
        }
        else if (type_num2 == NPY_DATETIME) {
            /* Make a new NPY_TIMEDELTA, and copy type2's metadata */
            out_dtypes[0] = timedelta_dtype_with_copied_meta(
                                            PyArray_DESCR(operands[1]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = ensure_dtype_nbo(PyArray_DESCR(operands[1]));
            if (out_dtypes[1] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[1];
            Py_INCREF(out_dtypes[2]);

            type_num1 = NPY_TIMEDELTA;
        }
        else {
            goto type_reso_error;
        }
    }
    else {
        goto type_reso_error;
    }

    /* Check against the casting rules */
    if (PyUFunc_ValidateCasting(ufunc, casting, operands, out_dtypes) < 0) {
        for (i = 0; i < 3; ++i) {
            Py_DECREF(out_dtypes[i]);
            out_dtypes[i] = NULL;
        }
        return -1;
    }

    /* Search in the functions list */
    types = ufunc->types;
    n = ufunc->ntypes;

    for (i = 0; i < n; ++i) {
        if (types[3*i] == type_num1 && types[3*i+1] == type_num2) {
            *out_innerloop = ufunc->functions[i];
            *out_innerloopdata = ufunc->data[i];
            return 0;
        }
    }

    PyErr_Format(PyExc_TypeError,
            "internal error: could not find appropriate datetime "
            "inner loop in %s ufunc", ufunc_name);
    return -1;

type_reso_error: {
        PyObject *errmsg;
        errmsg = PyUString_FromFormat("ufunc %s cannot use operands "
                            "with types ", ufunc_name);
        PyUString_ConcatAndDel(&errmsg,
                PyObject_Repr((PyObject *)PyArray_DESCR(operands[0])));
        PyUString_ConcatAndDel(&errmsg,
                PyUString_FromString(" and "));
        PyUString_ConcatAndDel(&errmsg,
                PyObject_Repr((PyObject *)PyArray_DESCR(operands[1])));
        PyErr_SetObject(PyExc_TypeError, errmsg);
        return -1;
    }
}

/*
 * This function applies the type resolution rules for subtraction.
 * In particular, there are a number of special cases with datetime:
 *    m8[<A>] - m8[<B>] => m8[gcd(<A>,<B>)] - m8[gcd(<A>,<B>)]
 *    m8[<A>] - int     => m8[<A>] - m8[<A>]
 *    int     - m8[<A>] => m8[<A>] - m8[<A>]
 *    M8[<A>] - int     => M8[<A>] - m8[<A>]
 *    M8[<A>] - m8[<B>] => M8[gcd(<A>,<B>)] - m8[gcd(<A>,<B>)]
 * TODO: Non-linear time unit cases require highly special-cased loops
 *    M8[<A>] - m8[Y|M|B]
 */
NPY_NO_EXPORT int
PyUFunc_SubtractionTypeResolution(PyUFuncObject *ufunc,
                                NPY_CASTING casting,
                                PyArrayObject **operands,
                                PyObject *type_tup,
                                PyArray_Descr **out_dtypes,
                                PyUFuncGenericFunction *out_innerloop,
                                void **out_innerloopdata)
{
    int type_num1, type_num2;
    char *types;
    int i, n;
    char *ufunc_name;

    ufunc_name = ufunc->name ? ufunc->name : "<unnamed ufunc>";

    type_num1 = PyArray_DESCR(operands[0])->type_num;
    type_num2 = PyArray_DESCR(operands[1])->type_num;

    /* Use the default when datetime and timedelta are not involved */
    if (!PyTypeNum_ISDATETIME(type_num1) && !PyTypeNum_ISDATETIME(type_num2)) {
        return PyUFunc_DefaultTypeResolution(ufunc, casting, operands,
                    type_tup, out_dtypes, out_innerloop, out_innerloopdata);
    }

    if (type_num1 == NPY_TIMEDELTA) {
        /* m8[<A>] - m8[<B>] => m8[gcd(<A>,<B>)] - m8[gcd(<A>,<B>)] */
        if (type_num2 == NPY_TIMEDELTA) {
            out_dtypes[0] = PyArray_PromoteTypes(PyArray_DESCR(operands[0]),
                                                PyArray_DESCR(operands[1]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = out_dtypes[0];
            Py_INCREF(out_dtypes[1]);
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);
        }
        /* m8[<A>] - int => m8[<A>] - m8[<A>] */
        else if (PyTypeNum_ISINTEGER(type_num2) ||
                                        PyTypeNum_ISBOOL(type_num2)) {
            out_dtypes[0] = ensure_dtype_nbo(PyArray_DESCR(operands[0]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = out_dtypes[0];
            Py_INCREF(out_dtypes[1]);
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);

            type_num2 = NPY_TIMEDELTA;
        }
        else {
            goto type_reso_error;
        }
    }
    else if (type_num1 == NPY_DATETIME) {
        /* M8[<A>] - m8[<B>] => M8[gcd(<A>,<B>)] - m8[gcd(<A>,<B>)] */
        if (type_num2 == NPY_TIMEDELTA) {
            out_dtypes[0] = PyArray_PromoteTypes(PyArray_DESCR(operands[0]),
                                                PyArray_DESCR(operands[1]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            /* Make a new NPY_TIMEDELTA, and copy the datetime's metadata */
            out_dtypes[1] = timedelta_dtype_with_copied_meta(out_dtypes[0]);
            if (out_dtypes[1] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);
        }
        /* M8[<A>] - int => M8[<A>] - m8[<A>] */
        else if (PyTypeNum_ISINTEGER(type_num2) ||
                    PyTypeNum_ISBOOL(type_num2)) {
            out_dtypes[0] = ensure_dtype_nbo(PyArray_DESCR(operands[0]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            /* Make a new NPY_TIMEDELTA, and copy type1's metadata */
            out_dtypes[1] = timedelta_dtype_with_copied_meta(
                                            PyArray_DESCR(operands[0]));
            if (out_dtypes[1] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);

            type_num2 = NPY_TIMEDELTA;
        }
        /* M8[<A>] - M8[<B>] => M8[gcd(<A>,<B>)] - M8[gcd(<A>,<B>)] */
        else if (type_num2 == NPY_DATETIME) {
            out_dtypes[0] = PyArray_PromoteTypes(PyArray_DESCR(operands[0]),
                                                PyArray_DESCR(operands[1]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            /* Make a new NPY_TIMEDELTA, and copy type1's metadata */
            out_dtypes[2] = timedelta_dtype_with_copied_meta(out_dtypes[0]);
            if (out_dtypes[2] == NULL) {
                Py_DECREF(out_dtypes[0]);
                return -1;
            }
            out_dtypes[1] = out_dtypes[0];
            Py_INCREF(out_dtypes[1]);
        }
        else {
            goto type_reso_error;
        }
    }
    else if (PyTypeNum_ISINTEGER(type_num1) || PyTypeNum_ISBOOL(type_num1)) {
        /* int - m8[<A>] => m8[<A>] - m8[<A>] */
        if (type_num2 == NPY_TIMEDELTA) {
            out_dtypes[0] = ensure_dtype_nbo(PyArray_DESCR(operands[1]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = out_dtypes[0];
            Py_INCREF(out_dtypes[1]);
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);

            type_num1 = NPY_TIMEDELTA;
        }
        else {
            goto type_reso_error;
        }
    }
    else {
        goto type_reso_error;
    }

    /* Check against the casting rules */
    if (PyUFunc_ValidateCasting(ufunc, casting, operands, out_dtypes) < 0) {
        for (i = 0; i < 3; ++i) {
            Py_DECREF(out_dtypes[i]);
            out_dtypes[i] = NULL;
        }
        return -1;
    }

    /* Search in the functions list */
    types = ufunc->types;
    n = ufunc->ntypes;

    for (i = 0; i < n; ++i) {
        if (types[3*i] == type_num1 && types[3*i+1] == type_num2) {
            *out_innerloop = ufunc->functions[i];
            *out_innerloopdata = ufunc->data[i];
            return 0;
        }
    }

    PyErr_Format(PyExc_TypeError,
            "internal error: could not find appropriate datetime "
            "inner loop in %s ufunc", ufunc_name);
    return -1;

type_reso_error: {
        PyObject *errmsg;
        errmsg = PyUString_FromFormat("ufunc %s cannot use operands "
                            "with types ", ufunc_name);
        PyUString_ConcatAndDel(&errmsg,
                PyObject_Repr((PyObject *)PyArray_DESCR(operands[0])));
        PyUString_ConcatAndDel(&errmsg,
                PyUString_FromString(" and "));
        PyUString_ConcatAndDel(&errmsg,
                PyObject_Repr((PyObject *)PyArray_DESCR(operands[1])));
        PyErr_SetObject(PyExc_TypeError, errmsg);
        return -1;
    }
}

/*
 * This function applies the type resolution rules for multiplication.
 * In particular, there are a number of special cases with datetime:
 *    int## * m8[<A>] => int64 * m8[<A>]
 *    m8[<A>] * int## => m8[<A>] * int64
 *    float## * m8[<A>] => float64 * m8[<A>]
 *    m8[<A>] * float## => m8[<A>] * float64
 */
NPY_NO_EXPORT int
PyUFunc_MultiplicationTypeResolution(PyUFuncObject *ufunc,
                                NPY_CASTING casting,
                                PyArrayObject **operands,
                                PyObject *type_tup,
                                PyArray_Descr **out_dtypes,
                                PyUFuncGenericFunction *out_innerloop,
                                void **out_innerloopdata)
{
    int type_num1, type_num2;
    char *types;
    int i, n;
    char *ufunc_name;

    ufunc_name = ufunc->name ? ufunc->name : "<unnamed ufunc>";

    type_num1 = PyArray_DESCR(operands[0])->type_num;
    type_num2 = PyArray_DESCR(operands[1])->type_num;

    /* Use the default when datetime and timedelta are not involved */
    if (!PyTypeNum_ISDATETIME(type_num1) && !PyTypeNum_ISDATETIME(type_num2)) {
        return PyUFunc_DefaultTypeResolution(ufunc, casting, operands,
                    type_tup, out_dtypes, out_innerloop, out_innerloopdata);
    }

    if (type_num1 == NPY_TIMEDELTA) {
        /* m8[<A>] * int## => m8[<A>] * int64 */
        if (PyTypeNum_ISINTEGER(type_num2) || PyTypeNum_ISBOOL(type_num2)) {
            out_dtypes[0] = ensure_dtype_nbo(PyArray_DESCR(operands[0]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = PyArray_DescrNewFromType(NPY_LONGLONG);
            if (out_dtypes[1] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);

            type_num2 = NPY_LONGLONG;
        }
        /* m8[<A>] * float## => m8[<A>] * float64 */
        else if (PyTypeNum_ISFLOAT(type_num2)) {
            out_dtypes[0] = ensure_dtype_nbo(PyArray_DESCR(operands[0]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = PyArray_DescrNewFromType(NPY_DOUBLE);
            if (out_dtypes[1] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);

            type_num2 = NPY_DOUBLE;
        }
        else {
            goto type_reso_error;
        }
    }
    else if (PyTypeNum_ISINTEGER(type_num1) || PyTypeNum_ISBOOL(type_num1)) {
        /* int## * m8[<A>] => int64 * m8[<A>] */
        if (type_num2 == NPY_TIMEDELTA) {
            out_dtypes[0] = PyArray_DescrNewFromType(NPY_LONGLONG);
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = ensure_dtype_nbo(PyArray_DESCR(operands[1]));
            if (out_dtypes[1] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[1];
            Py_INCREF(out_dtypes[2]);

            type_num1 = NPY_LONGLONG;
        }
        else {
            goto type_reso_error;
        }
    }
    else if (PyTypeNum_ISFLOAT(type_num1)) {
        /* float## * m8[<A>] => float64 * m8[<A>] */
        if (type_num2 == NPY_TIMEDELTA) {
            out_dtypes[0] = PyArray_DescrNewFromType(NPY_DOUBLE);
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = ensure_dtype_nbo(PyArray_DESCR(operands[1]));
            if (out_dtypes[1] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[1];
            Py_INCREF(out_dtypes[2]);

            type_num1 = NPY_DOUBLE;
        }
        else {
            goto type_reso_error;
        }
    }
    else {
        goto type_reso_error;
    }

    /* Check against the casting rules */
    if (PyUFunc_ValidateCasting(ufunc, casting, operands, out_dtypes) < 0) {
        for (i = 0; i < 3; ++i) {
            Py_DECREF(out_dtypes[i]);
            out_dtypes[i] = NULL;
        }
        return -1;
    }

    /* Search in the functions list */
    types = ufunc->types;
    n = ufunc->ntypes;

    for (i = 0; i < n; ++i) {
        if (types[3*i] == type_num1 && types[3*i+1] == type_num2) {
            *out_innerloop = ufunc->functions[i];
            *out_innerloopdata = ufunc->data[i];
            return 0;
        }
    }

    PyErr_Format(PyExc_TypeError,
            "internal error: could not find appropriate datetime "
            "inner loop in %s ufunc", ufunc_name);
    return -1;

type_reso_error: {
        PyObject *errmsg;
        errmsg = PyUString_FromFormat("ufunc %s cannot use operands "
                            "with types ", ufunc_name);
        PyUString_ConcatAndDel(&errmsg,
                PyObject_Repr((PyObject *)PyArray_DESCR(operands[0])));
        PyUString_ConcatAndDel(&errmsg,
                PyUString_FromString(" and "));
        PyUString_ConcatAndDel(&errmsg,
                PyObject_Repr((PyObject *)PyArray_DESCR(operands[1])));
        PyErr_SetObject(PyExc_TypeError, errmsg);
        return -1;
    }
}

/*
 * This function applies the type resolution rules for division.
 * In particular, there are a number of special cases with datetime:
 *    m8[<A>] / m8[<B>] to  m8[gcd(<A>,<B>)] / m8[gcd(<A>,<B>)]  -> float64
 *    m8[<A>] / int##   to m8[<A>] / int64 -> m8[<A>]
 *    m8[<A>] / float## to m8[<A>] / float64 -> m8[<A>]
 */
NPY_NO_EXPORT int
PyUFunc_DivisionTypeResolution(PyUFuncObject *ufunc,
                                NPY_CASTING casting,
                                PyArrayObject **operands,
                                PyObject *type_tup,
                                PyArray_Descr **out_dtypes,
                                PyUFuncGenericFunction *out_innerloop,
                                void **out_innerloopdata)
{
    int type_num1, type_num2;
    char *types;
    int i, n;
    char *ufunc_name;

    ufunc_name = ufunc->name ? ufunc->name : "<unnamed ufunc>";

    type_num1 = PyArray_DESCR(operands[0])->type_num;
    type_num2 = PyArray_DESCR(operands[1])->type_num;

    /* Use the default when datetime and timedelta are not involved */
    if (!PyTypeNum_ISDATETIME(type_num1) && !PyTypeNum_ISDATETIME(type_num2)) {
        return PyUFunc_DefaultTypeResolution(ufunc, casting, operands,
                    type_tup, out_dtypes, out_innerloop, out_innerloopdata);
    }

    if (type_num1 == NPY_TIMEDELTA) {
        /*
         * m8[<A>] / m8[<B>] to
         * m8[gcd(<A>,<B>)] / m8[gcd(<A>,<B>)]  -> float64
         */
        if (type_num2 == NPY_TIMEDELTA) {
            out_dtypes[0] = PyArray_PromoteTypes(PyArray_DESCR(operands[0]),
                                                PyArray_DESCR(operands[1]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = out_dtypes[0];
            Py_INCREF(out_dtypes[1]);
            out_dtypes[2] = PyArray_DescrFromType(NPY_DOUBLE);
            if (out_dtypes[2] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                Py_DECREF(out_dtypes[1]);
                out_dtypes[1] = NULL;
                return -1;
            }
        }
        /* m8[<A>] / int## => m8[<A>] / int64 */
        else if (PyTypeNum_ISINTEGER(type_num2)) {
            out_dtypes[0] = ensure_dtype_nbo(PyArray_DESCR(operands[0]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = PyArray_DescrFromType(NPY_LONGLONG);
            if (out_dtypes[1] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);

            type_num2 = NPY_LONGLONG;
        }
        /* m8[<A>] / float## => m8[<A>] / float64 */
        else if (PyTypeNum_ISFLOAT(type_num2)) {
            out_dtypes[0] = ensure_dtype_nbo(PyArray_DESCR(operands[0]));
            if (out_dtypes[0] == NULL) {
                return -1;
            }
            out_dtypes[1] = PyArray_DescrNewFromType(NPY_DOUBLE);
            if (out_dtypes[1] == NULL) {
                Py_DECREF(out_dtypes[0]);
                out_dtypes[0] = NULL;
                return -1;
            }
            out_dtypes[2] = out_dtypes[0];
            Py_INCREF(out_dtypes[2]);

            type_num2 = NPY_DOUBLE;
        }
        else {
            goto type_reso_error;
        }
    }
    else {
        goto type_reso_error;
    }

    /* Check against the casting rules */
    if (PyUFunc_ValidateCasting(ufunc, casting, operands, out_dtypes) < 0) {
        for (i = 0; i < 3; ++i) {
            Py_DECREF(out_dtypes[i]);
            out_dtypes[i] = NULL;
        }
        return -1;
    }

    /* Search in the functions list */
    types = ufunc->types;
    n = ufunc->ntypes;

    for (i = 0; i < n; ++i) {
        if (types[3*i] == type_num1 && types[3*i+1] == type_num2) {
            *out_innerloop = ufunc->functions[i];
            *out_innerloopdata = ufunc->data[i];
            return 0;
        }
    }

    PyErr_Format(PyExc_TypeError,
            "internal error: could not find appropriate datetime "
            "inner loop in %s ufunc", ufunc_name);
    return -1;

type_reso_error: {
        PyObject *errmsg;
        errmsg = PyUString_FromFormat("ufunc %s cannot use operands "
                            "with types ", ufunc_name);
        PyUString_ConcatAndDel(&errmsg,
                PyObject_Repr((PyObject *)PyArray_DESCR(operands[0])));
        PyUString_ConcatAndDel(&errmsg,
                PyUString_FromString(" and "));
        PyUString_ConcatAndDel(&errmsg,
                PyObject_Repr((PyObject *)PyArray_DESCR(operands[1])));
        PyErr_SetObject(PyExc_TypeError, errmsg);
        return -1;
    }
}

typedef struct {
    NpyAuxData base;
    PyUFuncGenericFunction unmasked_innerloop;
    void *unmasked_innerloopdata;
    int nargs;
} _ufunc_masker_data;

static NpyAuxData *
ufunc_masker_data_clone(NpyAuxData *data)
{
    _ufunc_masker_data *n;

    /* Allocate a new one */
    n = (_ufunc_masker_data *)PyArray_malloc(sizeof(_ufunc_masker_data));
    if (n == NULL) {
        return NULL;
    }

    /* Copy the data (unmasked data doesn't have object semantics) */
    memcpy(n, data, sizeof(_ufunc_masker_data));

    return (NpyAuxData *)n;
}

/*
 * This function wraps a regular unmasked ufunc inner loop as a
 * masked ufunc inner loop, only calling the function for
 * elements where the mask is True.
 */
static void
unmasked_ufunc_loop_as_masked(
             char **args,
             npy_intp *dimensions,
             npy_intp *steps,
             NpyAuxData *innerloopdata)
{
    _ufunc_masker_data *data;
    int iargs, nargs;
    PyUFuncGenericFunction unmasked_innerloop;
    void *unmasked_innerloopdata;
    npy_intp loopsize, subloopsize;
    char *mask;
    npy_intp mask_stride;

    /* Put the aux data into local variables */
    data = (_ufunc_masker_data *)innerloopdata;
    unmasked_innerloop = data->unmasked_innerloop;
    unmasked_innerloopdata = data->unmasked_innerloopdata;
    nargs = data->nargs;
    loopsize = *dimensions;
    mask = args[nargs];
    mask_stride = steps[nargs];


    /* Process the data as runs of unmasked values */
    do {
        /* Skip masked values */
        subloopsize = 0;
        while (subloopsize < loopsize &&
                        !NpyMask_IsExposed(*(npy_mask *)mask)) {
            ++subloopsize;
            mask += mask_stride;
        }
        for (iargs = 0; iargs < nargs; ++iargs) {
            args[iargs] += subloopsize * steps[iargs];
        }
        loopsize -= subloopsize;
        /*
         * Process unmasked values (assumes unmasked loop doesn't
         * mess with the 'args' pointer values)
         */
        subloopsize = 0;
        while (subloopsize < loopsize &&
                        NpyMask_IsExposed(*(npy_mask *)mask)) {
            ++subloopsize;
            mask += mask_stride;
        }
        unmasked_innerloop(args, &subloopsize, steps, unmasked_innerloopdata);
        for (iargs = 0; iargs < nargs; ++iargs) {
            args[iargs] += subloopsize * steps[iargs];
        }
        loopsize -= subloopsize;
    } while (loopsize > 0);
}


/*UFUNC_API
 *
 * This function calls the unmasked type resolution function of the
 * ufunc, then wraps it with a function which only calls the inner
 * loop where the mask is True.
 *
 * Returns 0 on success, -1 on error.
 */
NPY_NO_EXPORT int
PyUFunc_DefaultTypeResolutionMasked(PyUFuncObject *ufunc,
                                NPY_CASTING casting,
                                PyArrayObject **operands,
                                PyObject *type_tup,
                                PyArray_Descr **out_dtypes,
                                PyUFuncGenericMaskedFunction *out_innerloop,
                                NpyAuxData **out_innerloopdata)
{
    int retcode;
    _ufunc_masker_data *data;

    /* Create a new NpyAuxData object for the masker data */
    data = (_ufunc_masker_data *)PyArray_malloc(sizeof(_ufunc_masker_data));
    if (data == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    memset(data, 0, sizeof(_ufunc_masker_data));
    data->base.free = (NpyAuxData_FreeFunc *)&PyArray_free;
    data->base.clone = &ufunc_masker_data_clone;
    data->nargs = ufunc->nin + ufunc->nout;

    /* Get the unmasked ufunc inner loop */
    retcode = ufunc->type_resolution_function(ufunc, casting,
                    operands, type_tup, out_dtypes,
                    &data->unmasked_innerloop, &data->unmasked_innerloopdata);
    if (retcode < 0) {
        PyArray_free(data);
        return retcode;
    }

    /* Return the loop function + aux data */
    *out_innerloop = &unmasked_ufunc_loop_as_masked;
    *out_innerloopdata = (NpyAuxData *)data;
    return 0;
}

static int
ufunc_loop_matches(PyUFuncObject *self,
                    PyArrayObject **op,
                    NPY_CASTING input_casting,
                    NPY_CASTING output_casting,
                    int any_object,
                    int use_min_scalar,
                    int *types,
                    int *out_no_castable_output,
                    char *out_err_src_typecode,
                    char *out_err_dst_typecode)
{
    npy_intp i, nin = self->nin, nop = nin + self->nout;

    /*
     * First check if all the inputs can be safely cast
     * to the types for this function
     */
    for (i = 0; i < nin; ++i) {
        PyArray_Descr *tmp;

        /*
         * If no inputs are objects and there are more than one
         * loop, don't allow conversion to object.  The rationale
         * behind this is mostly performance.  Except for custom
         * ufuncs built with just one object-parametered inner loop,
         * only the types that are supported are implemented.  Trying
         * the object version of logical_or on float arguments doesn't
         * seem right.
         */
        if (types[i] == NPY_OBJECT && !any_object && self->ntypes > 1) {
            return 0;
        }

        tmp = PyArray_DescrFromType(types[i]);
        if (tmp == NULL) {
            return -1;
        }

#if NPY_UF_DBG_TRACING
        printf("Checking type for op %d, type %d: ", (int)i, (int)types[i]);
        PyObject_Print((PyObject *)tmp, stdout, 0);
        printf(", operand type: ");
        PyObject_Print((PyObject *)PyArray_DESCR(op[i]), stdout, 0);
        printf("\n");
#endif
        /*
         * If all the inputs are scalars, use the regular
         * promotion rules, not the special value-checking ones.
         */
        if (!use_min_scalar) {
            if (!PyArray_CanCastTypeTo(PyArray_DESCR(op[i]), tmp,
                                                    input_casting)) {
                Py_DECREF(tmp);
                return 0;
            }
        }
        else {
            if (!PyArray_CanCastArrayTo(op[i], tmp, input_casting)) {
                Py_DECREF(tmp);
                return 0;
            }
        }
        Py_DECREF(tmp);
    }

    /*
     * If all the inputs were ok, then check casting back to the
     * outputs.
     */
    for (i = nin; i < nop; ++i) {
        if (op[i] != NULL) {
            PyArray_Descr *tmp = PyArray_DescrFromType(types[i]);
            if (tmp == NULL) {
                return -1;
            }
            if (!PyArray_CanCastTypeTo(tmp, PyArray_DESCR(op[i]),
                                                        output_casting)) {
                if (!(*out_no_castable_output)) {
                    *out_no_castable_output = 1;
                    *out_err_src_typecode = tmp->type;
                    *out_err_dst_typecode = PyArray_DESCR(op[i])->type;
                }
                Py_DECREF(tmp);
                return 0;
            }
            Py_DECREF(tmp);
        }
    }

    return 1;
}

static int
set_ufunc_loop_data_types(PyUFuncObject *self, PyArrayObject **op,
                    PyArray_Descr **out_dtype,
                    int *types)
{
    int i, nin = self->nin, nop = nin + self->nout;

    /* Fill the dtypes array */
    for (i = 0; i < nop; ++i) {
        out_dtype[i] = PyArray_DescrFromType(types[i]);
        if (out_dtype[i] == NULL) {
            while (--i >= 0) {
                Py_DECREF(out_dtype[i]);
                out_dtype[i] = NULL;
            }
            return -1;
        }
    }

    return 0;
}

/*
 * Does a search through the arguments and the loops
 */
static int
find_ufunc_matching_userloop(PyUFuncObject *self,
                        PyArrayObject **op,
                        NPY_CASTING input_casting,
                        NPY_CASTING output_casting,
                        int any_object,
                        int use_min_scalar,
                        PyArray_Descr **out_dtype,
                        PyUFuncGenericFunction *out_innerloop,
                        void **out_innerloopdata,
                        int *out_no_castable_output,
                        char *out_err_src_typecode,
                        char *out_err_dst_typecode)
{
    npy_intp i, nin = self->nin;
    PyUFunc_Loop1d *funcdata;

    /* Use this to try to avoid repeating the same userdef loop search */
    int last_userdef = -1;

    for (i = 0; i < nin; ++i) {
        int type_num = PyArray_DESCR(op[i])->type_num;
        if (type_num != last_userdef && PyTypeNum_ISUSERDEF(type_num)) {
            PyObject *key, *obj;

            last_userdef = type_num;

            key = PyInt_FromLong(type_num);
            if (key == NULL) {
                return -1;
            }
            obj = PyDict_GetItem(self->userloops, key);
            Py_DECREF(key);
            if (obj == NULL) {
                continue;
            }
            funcdata = (PyUFunc_Loop1d *)NpyCapsule_AsVoidPtr(obj);
            while (funcdata != NULL) {
                int *types = funcdata->arg_types;
                switch (ufunc_loop_matches(self, op,
                            input_casting, output_casting,
                            any_object, use_min_scalar,
                            types,
                            out_no_castable_output, out_err_src_typecode,
                            out_err_dst_typecode)) {
                    /* Error */
                    case -1:
                        return -1;
                    /* Found a match */
                    case 1:
                        set_ufunc_loop_data_types(self, op, out_dtype, types);

                        /* Save the inner loop and its data */
                        *out_innerloop = funcdata->func;
                        *out_innerloopdata = funcdata->data;

                        return 0;
                }

                funcdata = funcdata->next;
            }
        }
    }

    /* Didn't find a match */
    return 0;
}

/*
 * Does a search through the arguments and the loops
 */
static int
find_ufunc_specified_userloop(PyUFuncObject *self,
                        int n_specified,
                        int *specified_types,
                        PyArrayObject **op,
                        NPY_CASTING casting,
                        int any_object,
                        int use_min_scalar,
                        PyArray_Descr **out_dtype,
                        PyUFuncGenericFunction *out_innerloop,
                        void **out_innerloopdata)
{
    int i, j, nin = self->nin, nop = nin + self->nout;
    PyUFunc_Loop1d *funcdata;

    /* Use this to try to avoid repeating the same userdef loop search */
    int last_userdef = -1;

    int no_castable_output = 0;
    char err_src_typecode = '-', err_dst_typecode = '-';

    for (i = 0; i < nin; ++i) {
        int type_num = PyArray_DESCR(op[i])->type_num;
        if (type_num != last_userdef && PyTypeNum_ISUSERDEF(type_num)) {
            PyObject *key, *obj;

            last_userdef = type_num;

            key = PyInt_FromLong(type_num);
            if (key == NULL) {
                return -1;
            }
            obj = PyDict_GetItem(self->userloops, key);
            Py_DECREF(key);
            if (obj == NULL) {
                continue;
            }
            funcdata = (PyUFunc_Loop1d *)NpyCapsule_AsVoidPtr(obj);
            while (funcdata != NULL) {
                int *types = funcdata->arg_types;
                int matched = 1;

                if (n_specified == nop) {
                    for (j = 0; j < nop; ++j) {
                        if (types[j] != specified_types[j]) {
                            matched = 0;
                            break;
                        }
                    }
                } else {
                    if (types[nin] != specified_types[0]) {
                        matched = 0;
                    }
                }
                if (!matched) {
                    continue;
                }

                switch (ufunc_loop_matches(self, op,
                            casting, casting,
                            any_object, use_min_scalar,
                            types,
                            &no_castable_output, &err_src_typecode,
                            &err_dst_typecode)) {
                    /* It works */
                    case 1:
                        set_ufunc_loop_data_types(self, op, out_dtype, types);

                        /* Save the inner loop and its data */
                        *out_innerloop = funcdata->func;
                        *out_innerloopdata = funcdata->data;

                        return 0;
                    /* Didn't match */
                    case 0:
                        PyErr_Format(PyExc_TypeError,
                             "found a user loop for ufunc '%s' "
                             "matching the type-tuple, "
                             "but the inputs and/or outputs could not be "
                             "cast according to the casting rule",
                             self->name ? self->name : "(unknown)");
                        return -1;
                    /* Error */
                    case -1:
                        return -1;
                }

                funcdata = funcdata->next;
            }
        }
    }

    /* Didn't find a match */
    return 0;
}

/*
 * Provides an ordering for the dtype 'kind' character codes, to help
 * determine when to use the min_scalar_type function. This groups
 * 'kind' into boolean, integer, floating point, and everything else.
 */

static int
dtype_kind_to_simplified_ordering(char kind)
{
    switch (kind) {
        /* Boolean kind */
        case 'b':
            return 0;
        /* Unsigned int kind */
        case 'u':
        /* Signed int kind */
        case 'i':
            return 1;
        /* Float kind */
        case 'f':
        /* Complex kind */
        case 'c':
            return 2;
        /* Anything else */
        default:
            return 3;
    }
}

static int
should_use_min_scalar(PyArrayObject **op, int nop)
{
    int i, use_min_scalar, kind;
    int all_scalars = 1, max_scalar_kind = -1, max_array_kind = -1;

    /*
     * Determine if there are any scalars, and if so, whether
     * the maximum "kind" of the scalars surpasses the maximum
     * "kind" of the arrays
     */
    use_min_scalar = 0;
    if (nop > 1) {
        for(i = 0; i < nop; ++i) {
            kind = dtype_kind_to_simplified_ordering(
                                PyArray_DESCR(op[i])->kind);
            if (PyArray_NDIM(op[i]) == 0) {
                if (kind > max_scalar_kind) {
                    max_scalar_kind = kind;
                }
            }
            else {
                all_scalars = 0;
                if (kind > max_array_kind) {
                    max_array_kind = kind;
                }

            }
        }

        /* Indicate whether to use the min_scalar_type function */
        if (!all_scalars && max_array_kind >= max_scalar_kind) {
            use_min_scalar = 1;
        }
    }

    return use_min_scalar;
}

/*
 * Does a linear search for the best inner loop of the ufunc.
 *
 * Note that if an error is returned, the caller must free the non-zero
 * references in out_dtype.  This function does not do its own clean-up.
 */
NPY_NO_EXPORT int
find_best_ufunc_inner_loop(PyUFuncObject *self,
                        PyArrayObject **op,
                        NPY_CASTING input_casting,
                        NPY_CASTING output_casting,
                        int any_object,
                        PyArray_Descr **out_dtype,
                        PyUFuncGenericFunction *out_innerloop,
                        void **out_innerloopdata)
{
    npy_intp i, j, nin = self->nin, nop = nin + self->nout;
    int types[NPY_MAXARGS];
    char *ufunc_name;
    int no_castable_output, use_min_scalar;

    /* For making a better error message on coercion error */
    char err_dst_typecode = '-', err_src_typecode = '-';

    ufunc_name = self->name ? self->name : "(unknown)";

    use_min_scalar = should_use_min_scalar(op, nin);

    /* If the ufunc has userloops, search for them. */
    if (self->userloops) {
        switch (find_ufunc_matching_userloop(self, op,
                                input_casting, output_casting,
                                any_object, use_min_scalar,
                                out_dtype, out_innerloop, out_innerloopdata,
                                &no_castable_output, &err_src_typecode,
                                &err_dst_typecode)) {
            /* Error */
            case -1:
                return -1;
            /* A loop was found */
            case 1:
                return 0;
        }
    }

    /*
     * Determine the UFunc loop.  This could in general be *much* faster,
     * and a better way to implement it might be for the ufunc to
     * provide a function which gives back the result type and inner
     * loop function.
     *
     * A default fast mechanism could be provided for functions which
     * follow the most typical pattern, when all functions have signatures
     * "xx...x -> x" for some built-in data type x, as follows.
     *  - Use PyArray_ResultType to get the output type
     *  - Look up the inner loop in a table based on the output type_num
     *
     * The method for finding the loop in the previous code did not
     * appear consistent (as noted by some asymmetry in the generated
     * coercion tables for np.add).
     */
    no_castable_output = 0;
    for (i = 0; i < self->ntypes; ++i) {
        char *orig_types = self->types + i*self->nargs;

        /* Copy the types into an int array for matching */
        for (j = 0; j < nop; ++j) {
            types[j] = orig_types[j];
        }

        switch (ufunc_loop_matches(self, op,
                    input_casting, output_casting,
                    any_object, use_min_scalar,
                    types,
                    &no_castable_output, &err_src_typecode,
                    &err_dst_typecode)) {
            /* Error */
            case -1:
                return -1;
            /* Found a match */
            case 1:
                set_ufunc_loop_data_types(self, op, out_dtype, types);

                /* Save the inner loop and its data */
                *out_innerloop = self->functions[i];
                *out_innerloopdata = self->data[i];

                return 0;
        }

    }

    /* If no function was found, throw an error */
    if (no_castable_output) {
        PyErr_Format(PyExc_TypeError,
                "ufunc '%s' output (typecode '%c') could not be coerced to "
                "provided output parameter (typecode '%c') according "
                "to the casting rule '%s'",
                ufunc_name, err_src_typecode, err_dst_typecode,
                npy_casting_to_string(output_casting));
    }
    else {
        /*
         * TODO: We should try again if the casting rule is same_kind
         *       or unsafe, and look for a function more liberally.
         */
        PyErr_Format(PyExc_TypeError,
                "ufunc '%s' not supported for the input types, and the "
                "inputs could not be safely coerced to any supported "
                "types according to the casting rule '%s'",
                ufunc_name,
                npy_casting_to_string(input_casting));
    }

    return -1;
}

/*
 * Does a linear search for the inner loop of the ufunc specified by type_tup.
 *
 * Note that if an error is returned, the caller must free the non-zero
 * references in out_dtype.  This function does not do its own clean-up.
 */
NPY_NO_EXPORT int
find_specified_ufunc_inner_loop(PyUFuncObject *self,
                        PyObject *type_tup,
                        PyArrayObject **op,
                        NPY_CASTING casting,
                        int any_object,
                        PyArray_Descr **out_dtype,
                        PyUFuncGenericFunction *out_innerloop,
                        void **out_innerloopdata)
{
    npy_intp i, j, n, nin = self->nin, nop = nin + self->nout;
    int n_specified = 0;
    int specified_types[NPY_MAXARGS], types[NPY_MAXARGS];
    char *ufunc_name;
    int no_castable_output, use_min_scalar;

    /* For making a better error message on coercion error */
    char err_dst_typecode = '-', err_src_typecode = '-';

    ufunc_name = self->name ? self->name : "(unknown)";

    use_min_scalar = should_use_min_scalar(op, nin);

    /* Fill in specified_types from the tuple or string */
    if (PyTuple_Check(type_tup)) {
        n = PyTuple_GET_SIZE(type_tup);
        if (n != 1 && n != nop) {
            PyErr_Format(PyExc_ValueError,
                         "a type-tuple must be specified " \
                         "of length 1 or %d for ufunc '%s'", (int)nop,
                         self->name ? self->name : "(unknown)");
            return -1;
        }

        for (i = 0; i < n; ++i) {
            PyArray_Descr *dtype = NULL;
            if (!PyArray_DescrConverter(PyTuple_GET_ITEM(type_tup, i),
                                                                &dtype)) {
                return -1;
            }
            specified_types[i] = dtype->type_num;
            Py_DECREF(dtype);
        }

        n_specified = n;
    }
    else if (PyBytes_Check(type_tup) || PyUnicode_Check(type_tup)) {
        Py_ssize_t length;
        char *str;
        PyObject *str_obj = NULL;

        if (PyUnicode_Check(type_tup)) {
            str_obj = PyUnicode_AsASCIIString(type_tup);
            if (str_obj == NULL) {
                return -1;
            }
            type_tup = str_obj;
        }

        if (!PyBytes_AsStringAndSize(type_tup, &str, &length) < 0) {
            Py_XDECREF(str_obj);
            return -1;
        }
        if (length != 1 && (length != nop + 2 ||
                                str[nin] != '-' || str[nin+1] != '>')) {
            PyErr_Format(PyExc_ValueError,
                                 "a type-string for %s, "   \
                                 "requires 1 typecode, or "
                                 "%d typecode(s) before " \
                                 "and %d after the -> sign",
                                 self->name ? self->name : "(unknown)",
                                 self->nin, self->nout);
            Py_XDECREF(str_obj);
            return -1;
        }
        if (length == 1) {
            PyArray_Descr *dtype;
            n_specified = 1;
            dtype = PyArray_DescrFromType(str[0]);
            if (dtype == NULL) {
                Py_XDECREF(str_obj);
                return -1;
            }
            specified_types[0] = dtype->type_num;
            Py_DECREF(dtype);
        }
        else {
            PyArray_Descr *dtype;
            n_specified = (int)nop;

            for (i = 0; i < nop; ++i) {
                npy_intp istr = i < nin ? i : i+2;

                dtype = PyArray_DescrFromType(str[istr]);
                if (dtype == NULL) {
                    Py_XDECREF(str_obj);
                    return -1;
                }
                specified_types[i] = dtype->type_num;
                Py_DECREF(dtype);
            }
        }
        Py_XDECREF(str_obj);
    }

    /* If the ufunc has userloops, search for them. */
    if (self->userloops) {
        switch (find_ufunc_specified_userloop(self,
                        n_specified, specified_types,
                        op, casting,
                        any_object, use_min_scalar,
                        out_dtype, out_innerloop, out_innerloopdata)) {
            /* Error */
            case -1:
                return -1;
            /* Found matching loop */
            case 1:
                return 0;
        }
    }

    for (i = 0; i < self->ntypes; ++i) {
        char *orig_types = self->types + i*self->nargs;
        int matched = 1;

        /* Copy the types into an int array for matching */
        for (j = 0; j < nop; ++j) {
            types[j] = orig_types[j];
        }

        if (n_specified == nop) {
            for (j = 0; j < nop; ++j) {
                if (types[j] != specified_types[j]) {
                    matched = 0;
                    break;
                }
            }
        } else {
            if (types[nin] != specified_types[0]) {
                matched = 0;
            }
        }
        if (!matched) {
            continue;
        }

        switch (ufunc_loop_matches(self, op,
                    casting, casting,
                    any_object, use_min_scalar,
                    types,
                    &no_castable_output, &err_src_typecode,
                    &err_dst_typecode)) {
            /* Error */
            case -1:
                return -1;
            /* It worked */
            case 1:
                set_ufunc_loop_data_types(self, op, out_dtype, types);

                /* Save the inner loop and its data */
                *out_innerloop = self->functions[i];
                *out_innerloopdata = self->data[i];

                return 0;
            /* Didn't work */
            case 0:
                PyErr_Format(PyExc_TypeError,
                     "found a loop for ufunc '%s' "
                     "matching the type-tuple, "
                     "but the inputs and/or outputs could not be "
                     "cast according to the casting rule",
                     ufunc_name);
                return -1;
        }

    }

    /* If no function was found, throw an error */
    PyErr_Format(PyExc_TypeError,
            "No loop matching the specified signature was found "
            "for ufunc %s", ufunc_name);

    return -1;
}


