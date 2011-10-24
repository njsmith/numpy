#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "structmember.h"

#define NPY_NO_DEPRECATED_API
#define _MULTIARRAYMODULE
#define NPY_NO_PREFIX
#include "numpy/arrayobject.h"
#include "numpy/arrayscalars.h"

#include "numpy/npy_math.h"

#include "npy_config.h"

#include "numpy/npy_3kcompat.h"

#include "common.h"
#include "ctors.h"
#include "lowlevel_strided_loops.h"

#define _check_axis PyArray_CheckAxis

/*NUMPY_API
 * Take
 */
NPY_NO_EXPORT PyObject *
PyArray_TakeFrom(PyArrayObject *self0, PyObject *indices0, int axis,
                 PyArrayObject *out, NPY_CLIPMODE clipmode)
{
    PyArray_Descr *dtype;
    PyArray_FastTakeFunc *func;
    PyArrayObject *obj = NULL, *self, *indices;
    intp nd, i, j, n, m, max_item, tmp, chunk, nelem;
    intp shape[MAX_DIMS];
    char *src, *dest;
    int err;

    indices = NULL;
    self = (PyArrayObject *)_check_axis(self0, &axis, NPY_ARRAY_CARRAY);
    if (self == NULL) {
        return NULL;
    }
    indices = (PyArrayObject *)PyArray_ContiguousFromAny(indices0,
                                                         PyArray_INTP,
                                                         1, 0);
    if (indices == NULL) {
        goto fail;
    }
    n = m = chunk = 1;
    nd = PyArray_NDIM(self) + PyArray_NDIM(indices) - 1;
    for (i = 0; i < nd; i++) {
        if (i < axis) {
            shape[i] = PyArray_DIMS(self)[i];
            n *= shape[i];
        }
        else {
            if (i < axis+PyArray_NDIM(indices)) {
                shape[i] = PyArray_DIMS(indices)[i-axis];
                m *= shape[i];
            }
            else {
                shape[i] = PyArray_DIMS(self)[i-PyArray_NDIM(indices)+1];
                chunk *= shape[i];
            }
        }
    }
    if (!out) {
        dtype = PyArray_DESCR(self);
        Py_INCREF(dtype);
        obj = (PyArrayObject *)PyArray_NewFromDescr(Py_TYPE(self),
                                                    dtype,
                                                    nd, shape,
                                                    NULL, NULL, 0,
                                                    (PyObject *)self);

        if (obj == NULL) {
            goto fail;
        }
    }
    else {
        int flags = NPY_ARRAY_CARRAY | NPY_ARRAY_UPDATEIFCOPY;

        if ((PyArray_NDIM(out) != nd) ||
            !PyArray_CompareLists(PyArray_DIMS(out), shape, nd)) {
            PyErr_SetString(PyExc_ValueError,
                            "bad shape in output array");
            goto fail;
        }

        if (clipmode == NPY_RAISE) {
            /*
             * we need to make sure and get a copy
             * so the input array is not changed
             * before the error is called
             */
            flags |= NPY_ARRAY_ENSURECOPY;
        }
        dtype = PyArray_DESCR(self);
        Py_INCREF(dtype);
        obj = (PyArrayObject *)PyArray_FromArray(out, dtype, flags);
        if (obj == NULL) {
            goto fail;
        }
    }

    max_item = PyArray_DIMS(self)[axis];
    nelem = chunk;
    chunk = chunk * PyArray_DESCR(obj)->elsize;
    src = PyArray_DATA(self);
    dest = PyArray_DATA(obj);

    func = PyArray_DESCR(self)->f->fasttake;
    if (func == NULL) {
        switch(clipmode) {
        case NPY_RAISE:
            for (i = 0; i < n; i++) {
                for (j = 0; j < m; j++) {
                    tmp = ((intp *)(PyArray_DATA(indices)))[j];
                    if (tmp < 0) {
                        tmp = tmp + max_item;
                    }
                    if ((tmp < 0) || (tmp >= max_item)) {
                        PyErr_SetString(PyExc_IndexError,
                                "index out of range "\
                                "for array");
                        goto fail;
                    }
                    memmove(dest, src + tmp*chunk, chunk);
                    dest += chunk;
                }
                src += chunk*max_item;
            }
            break;
        case NPY_WRAP:
            for (i = 0; i < n; i++) {
                for (j = 0; j < m; j++) {
                    tmp = ((intp *)(PyArray_DATA(indices)))[j];
                    if (tmp < 0) {
                        while (tmp < 0) {
                            tmp += max_item;
                        }
                    }
                    else if (tmp >= max_item) {
                        while (tmp >= max_item) {
                            tmp -= max_item;
                        }
                    }
                    memmove(dest, src + tmp*chunk, chunk);
                    dest += chunk;
                }
                src += chunk*max_item;
            }
            break;
        case NPY_CLIP:
            for (i = 0; i < n; i++) {
                for (j = 0; j < m; j++) {
                    tmp = ((intp *)(PyArray_DATA(indices)))[j];
                    if (tmp < 0) {
                        tmp = 0;
                    }
                    else if (tmp >= max_item) {
                        tmp = max_item - 1;
                    }
                    memmove(dest, src+tmp*chunk, chunk);
                    dest += chunk;
                }
                src += chunk*max_item;
            }
            break;
        }
    }
    else {
        err = func(dest, src, (intp *)(PyArray_DATA(indices)),
                    max_item, n, m, nelem, clipmode);
        if (err) {
            goto fail;
        }
    }

    PyArray_INCREF(obj);
    Py_XDECREF(indices);
    Py_XDECREF(self);
    if (out != NULL && out != obj) {
        Py_INCREF(out);
        Py_DECREF(obj);
        obj = out;
    }
    return (PyObject *)obj;

 fail:
    PyArray_XDECREF_ERR(obj);
    Py_XDECREF(indices);
    Py_XDECREF(self);
    return NULL;
}

/*NUMPY_API
 * Put values into an array
 */
NPY_NO_EXPORT PyObject *
PyArray_PutTo(PyArrayObject *self, PyObject* values0, PyObject *indices0,
              NPY_CLIPMODE clipmode)
{
    PyArrayObject  *indices, *values;
    intp i, chunk, ni, max_item, nv, tmp;
    char *src, *dest;
    int copied = 0;

    indices = NULL;
    values = NULL;
    if (!PyArray_Check(self)) {
        PyErr_SetString(PyExc_TypeError,
                        "put: first argument must be an array");
        return NULL;
    }
    if (!PyArray_ISCONTIGUOUS(self)) {
        PyArrayObject *obj;
        int flags = NPY_ARRAY_CARRAY | NPY_ARRAY_UPDATEIFCOPY;

        if (clipmode == NPY_RAISE) {
            flags |= NPY_ARRAY_ENSURECOPY;
        }
        Py_INCREF(PyArray_DESCR(self));
        obj = (PyArrayObject *)PyArray_FromArray(self,
                                                 PyArray_DESCR(self), flags);
        if (obj != self) {
            copied = 1;
        }
        self = obj;
    }
    max_item = PyArray_SIZE(self);
    dest = PyArray_DATA(self);
    chunk = PyArray_DESCR(self)->elsize;
    indices = (PyArrayObject *)PyArray_ContiguousFromAny(indices0,
                                                         PyArray_INTP, 0, 0);
    if (indices == NULL) {
        goto fail;
    }
    ni = PyArray_SIZE(indices);
    Py_INCREF(PyArray_DESCR(self));
    values = (PyArrayObject *)PyArray_FromAny(values0, PyArray_DESCR(self), 0, 0,
                              NPY_ARRAY_DEFAULT | NPY_ARRAY_FORCECAST, NULL);
    if (values == NULL) {
        goto fail;
    }
    nv = PyArray_SIZE(values);
    if (nv <= 0) {
        goto finish;
    }
    if (PyDataType_REFCHK(PyArray_DESCR(self))) {
        switch(clipmode) {
        case NPY_RAISE:
            for (i = 0; i < ni; i++) {
                src = PyArray_DATA(values) + chunk*(i % nv);
                tmp = ((intp *)(PyArray_DATA(indices)))[i];
                if (tmp < 0) {
                    tmp = tmp + max_item;
                }
                if ((tmp < 0) || (tmp >= max_item)) {
                    PyErr_SetString(PyExc_IndexError,
                            "index out of " \
                            "range for array");
                    goto fail;
                }
                PyArray_Item_INCREF(src, PyArray_DESCR(self));
                PyArray_Item_XDECREF(dest+tmp*chunk, PyArray_DESCR(self));
                memmove(dest + tmp*chunk, src, chunk);
            }
            break;
        case NPY_WRAP:
            for (i = 0; i < ni; i++) {
                src = PyArray_DATA(values) + chunk * (i % nv);
                tmp = ((intp *)(PyArray_DATA(indices)))[i];
                if (tmp < 0) {
                    while (tmp < 0) {
                        tmp += max_item;
                    }
                }
                else if (tmp >= max_item) {
                    while (tmp >= max_item) {
                        tmp -= max_item;
                    }
                }
                PyArray_Item_INCREF(src, PyArray_DESCR(self));
                PyArray_Item_XDECREF(dest+tmp*chunk, PyArray_DESCR(self));
                memmove(dest + tmp * chunk, src, chunk);
            }
            break;
        case NPY_CLIP:
            for (i = 0; i < ni; i++) {
                src = PyArray_DATA(values) + chunk * (i % nv);
                tmp = ((intp *)(PyArray_DATA(indices)))[i];
                if (tmp < 0) {
                    tmp = 0;
                }
                else if (tmp >= max_item) {
                    tmp = max_item - 1;
                }
                PyArray_Item_INCREF(src, PyArray_DESCR(self));
                PyArray_Item_XDECREF(dest+tmp*chunk, PyArray_DESCR(self));
                memmove(dest + tmp * chunk, src, chunk);
            }
            break;
        }
    }
    else {
        switch(clipmode) {
        case NPY_RAISE:
            for (i = 0; i < ni; i++) {
                src = PyArray_DATA(values) + chunk * (i % nv);
                tmp = ((intp *)(PyArray_DATA(indices)))[i];
                if (tmp < 0) {
                    tmp = tmp + max_item;
                }
                if ((tmp < 0) || (tmp >= max_item)) {
                    PyErr_SetString(PyExc_IndexError,
                            "index out of " \
                            "range for array");
                    goto fail;
                }
                memmove(dest + tmp * chunk, src, chunk);
            }
            break;
        case NPY_WRAP:
            for (i = 0; i < ni; i++) {
                src = PyArray_DATA(values) + chunk * (i % nv);
                tmp = ((intp *)(PyArray_DATA(indices)))[i];
                if (tmp < 0) {
                    while (tmp < 0) {
                        tmp += max_item;
                    }
                }
                else if (tmp >= max_item) {
                    while (tmp >= max_item) {
                        tmp -= max_item;
                    }
                }
                memmove(dest + tmp * chunk, src, chunk);
            }
            break;
        case NPY_CLIP:
            for (i = 0; i < ni; i++) {
                src = PyArray_DATA(values) + chunk * (i % nv);
                tmp = ((intp *)(PyArray_DATA(indices)))[i];
                if (tmp < 0) {
                    tmp = 0;
                }
                else if (tmp >= max_item) {
                    tmp = max_item - 1;
                }
                memmove(dest + tmp * chunk, src, chunk);
            }
            break;
        }
    }

 finish:
    Py_XDECREF(values);
    Py_XDECREF(indices);
    if (copied) {
        Py_DECREF(self);
    }
    Py_INCREF(Py_None);
    return Py_None;

 fail:
    Py_XDECREF(indices);
    Py_XDECREF(values);
    if (copied) {
        PyArray_XDECREF_ERR(self);
    }
    return NULL;
}

/*NUMPY_API
 * Put values into an array according to a mask.
 */
NPY_NO_EXPORT PyObject *
PyArray_PutMask(PyArrayObject *self, PyObject* values0, PyObject* mask0)
{
    PyArray_FastPutmaskFunc *func;
    PyArrayObject  *mask, *values;
    PyArray_Descr *dtype;
    intp i, chunk, ni, max_item, nv, tmp;
    char *src, *dest;
    int copied = 0;

    if (DEPRECATE("putmask has been deprecated. Use copyto with 'where' as "
                  "the mask instead") < 0) {
        return NULL;
    }

    mask = NULL;
    values = NULL;
    if (!PyArray_Check(self)) {
        PyErr_SetString(PyExc_TypeError,
                        "putmask: first argument must "
                        "be an array");
        return NULL;
    }
    if (!PyArray_ISCONTIGUOUS(self)) {
        PyArrayObject *obj;

        dtype = PyArray_DESCR(self);
        Py_INCREF(dtype);
        obj = (PyArrayObject *)PyArray_FromArray(self, dtype,
                                NPY_ARRAY_CARRAY | NPY_ARRAY_UPDATEIFCOPY);
        if (obj != self) {
            copied = 1;
        }
        self = obj;
    }

    max_item = PyArray_SIZE(self);
    dest = PyArray_DATA(self);
    chunk = PyArray_DESCR(self)->elsize;
    mask = (PyArrayObject *)PyArray_FROM_OTF(mask0, NPY_BOOL,
                                NPY_ARRAY_CARRAY | NPY_ARRAY_FORCECAST);
    if (mask == NULL) {
        goto fail;
    }
    ni = PyArray_SIZE(mask);
    if (ni != max_item) {
        PyErr_SetString(PyExc_ValueError,
                        "putmask: mask and data must be "
                        "the same size");
        goto fail;
    }
    dtype = PyArray_DESCR(self);
    Py_INCREF(dtype);
    values = (PyArrayObject *)PyArray_FromAny(values0, dtype,
                                    0, 0, NPY_ARRAY_CARRAY, NULL);
    if (values == NULL) {
        goto fail;
    }
    nv = PyArray_SIZE(values); /* zero if null array */
    if (nv <= 0) {
        Py_XDECREF(values);
        Py_XDECREF(mask);
        Py_INCREF(Py_None);
        return Py_None;
    }
    if (PyDataType_REFCHK(PyArray_DESCR(self))) {
        for (i = 0; i < ni; i++) {
            tmp = ((npy_bool *)(PyArray_DATA(mask)))[i];
            if (tmp) {
                src = PyArray_DATA(values) + chunk * (i % nv);
                PyArray_Item_INCREF(src, PyArray_DESCR(self));
                PyArray_Item_XDECREF(dest+i*chunk, PyArray_DESCR(self));
                memmove(dest + i * chunk, src, chunk);
            }
        }
    }
    else {
        func = PyArray_DESCR(self)->f->fastputmask;
        if (func == NULL) {
            for (i = 0; i < ni; i++) {
                tmp = ((npy_bool *)(PyArray_DATA(mask)))[i];
                if (tmp) {
                    src = PyArray_DATA(values) + chunk*(i % nv);
                    memmove(dest + i*chunk, src, chunk);
                }
            }
        }
        else {
            func(dest, PyArray_DATA(mask), ni, PyArray_DATA(values), nv);
        }
    }

    Py_XDECREF(values);
    Py_XDECREF(mask);
    if (copied) {
        Py_DECREF(self);
    }
    Py_INCREF(Py_None);
    return Py_None;

 fail:
    Py_XDECREF(mask);
    Py_XDECREF(values);
    if (copied) {
        PyArray_XDECREF_ERR(self);
    }
    return NULL;
}

/*NUMPY_API
 * Repeat the array.
 */
NPY_NO_EXPORT PyObject *
PyArray_Repeat(PyArrayObject *aop, PyObject *op, int axis)
{
    intp *counts;
    intp n, n_outer, i, j, k, chunk, total;
    intp tmp;
    int nd;
    PyArrayObject *repeats = NULL;
    PyObject *ap = NULL;
    PyArrayObject *ret = NULL;
    char *new_data, *old_data;

    repeats = (PyArrayObject *)PyArray_ContiguousFromAny(op, PyArray_INTP, 0, 1);
    if (repeats == NULL) {
        return NULL;
    }
    nd = PyArray_NDIM(repeats);
    counts = (npy_intp *)PyArray_DATA(repeats);

    if ((ap=_check_axis(aop, &axis, NPY_ARRAY_CARRAY))==NULL) {
        Py_DECREF(repeats);
        return NULL;
    }

    aop = (PyArrayObject *)ap;
    if (nd == 1) {
        n = PyArray_DIMS(repeats)[0];
    }
    else {
        /* nd == 0 */
        n = PyArray_DIMS(aop)[axis];
    }
    if (PyArray_DIMS(aop)[axis] != n) {
        PyErr_SetString(PyExc_ValueError,
                        "a.shape[axis] != len(repeats)");
        goto fail;
    }

    if (nd == 0) {
        total = counts[0]*n;
    }
    else {

        total = 0;
        for (j = 0; j < n; j++) {
            if (counts[j] < 0) {
                PyErr_SetString(PyExc_ValueError, "count < 0");
                goto fail;
            }
            total += counts[j];
        }
    }


    /* Construct new array */
    PyArray_DIMS(aop)[axis] = total;
    Py_INCREF(PyArray_DESCR(aop));
    ret = (PyArrayObject *)PyArray_NewFromDescr(Py_TYPE(aop),
                                                PyArray_DESCR(aop),
                                                PyArray_NDIM(aop),
                                                PyArray_DIMS(aop),
                                                NULL, NULL, 0,
                                                (PyObject *)aop);
    PyArray_DIMS(aop)[axis] = n;
    if (ret == NULL) {
        goto fail;
    }
    new_data = PyArray_DATA(ret);
    old_data = PyArray_DATA(aop);

    chunk = PyArray_DESCR(aop)->elsize;
    for(i = axis + 1; i < PyArray_NDIM(aop); i++) {
        chunk *= PyArray_DIMS(aop)[i];
    }

    n_outer = 1;
    for (i = 0; i < axis; i++) {
        n_outer *= PyArray_DIMS(aop)[i];
    }
    for (i = 0; i < n_outer; i++) {
        for (j = 0; j < n; j++) {
            tmp = nd ? counts[j] : counts[0];
            for (k = 0; k < tmp; k++) {
                memcpy(new_data, old_data, chunk);
                new_data += chunk;
            }
            old_data += chunk;
        }
    }

    Py_DECREF(repeats);
    PyArray_INCREF(ret);
    Py_XDECREF(aop);
    return (PyObject *)ret;

 fail:
    Py_DECREF(repeats);
    Py_XDECREF(aop);
    Py_XDECREF(ret);
    return NULL;
}

/*NUMPY_API
 */
NPY_NO_EXPORT PyObject *
PyArray_Choose(PyArrayObject *ip, PyObject *op, PyArrayObject *out,
               NPY_CLIPMODE clipmode)
{
    PyArrayObject *obj = NULL;
    PyArray_Descr *dtype;
    int n, elsize;
    intp i;
    char *ret_data;
    PyArrayObject **mps, *ap;
    PyArrayMultiIterObject *multi = NULL;
    intp mi;
    ap = NULL;

    /*
     * Convert all inputs to arrays of a common type
     * Also makes them C-contiguous
     */
    mps = PyArray_ConvertToCommonType(op, &n);
    if (mps == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (mps[i] == NULL) {
            goto fail;
        }
    }
    ap = (PyArrayObject *)PyArray_FROM_OT((PyObject *)ip, NPY_INTP);
    if (ap == NULL) {
        goto fail;
    }
    /* Broadcast all arrays to each other, index array at the end. */
    multi = (PyArrayMultiIterObject *)
        PyArray_MultiIterFromObjects((PyObject **)mps, n, 1, ap);
    if (multi == NULL) {
        goto fail;
    }
    /* Set-up return array */
    if (out == NULL) {
        dtype = PyArray_DESCR(mps[0]);
        Py_INCREF(dtype);
        obj = (PyArrayObject *)PyArray_NewFromDescr(Py_TYPE(ap),
                                                    dtype,
                                                    multi->nd,
                                                    multi->dimensions,
                                                    NULL, NULL, 0,
                                                    (PyObject *)ap);
    }
    else {
        int flags = NPY_ARRAY_CARRAY |
                    NPY_ARRAY_UPDATEIFCOPY |
                    NPY_ARRAY_FORCECAST;

        if ((PyArray_NDIM(out) != multi->nd)
                    || !PyArray_CompareLists(PyArray_DIMS(out),
                                             multi->dimensions,
                                             multi->nd)) {
            PyErr_SetString(PyExc_TypeError,
                            "choose: invalid shape for output array.");
            goto fail;
        }
        if (clipmode == NPY_RAISE) {
            /*
             * we need to make sure and get a copy
             * so the input array is not changed
             * before the error is called
             */
            flags |= NPY_ARRAY_ENSURECOPY;
        }
        dtype = PyArray_DESCR(mps[0]);
        Py_INCREF(dtype);
        obj = (PyArrayObject *)PyArray_FromArray(out, dtype, flags);
    }

    if (obj == NULL) {
        goto fail;
    }
    elsize = PyArray_DESCR(obj)->elsize;
    ret_data = PyArray_DATA(obj);

    while (PyArray_MultiIter_NOTDONE(multi)) {
        mi = *((intp *)PyArray_MultiIter_DATA(multi, n));
        if (mi < 0 || mi >= n) {
            switch(clipmode) {
            case NPY_RAISE:
                PyErr_SetString(PyExc_ValueError,
                        "invalid entry in choice "\
                        "array");
                goto fail;
            case NPY_WRAP:
                if (mi < 0) {
                    while (mi < 0) {
                        mi += n;
                    }
                }
                else {
                    while (mi >= n) {
                        mi -= n;
                    }
                }
                break;
            case NPY_CLIP:
                if (mi < 0) {
                    mi = 0;
                }
                else if (mi >= n) {
                    mi = n - 1;
                }
                break;
            }
        }
        memmove(ret_data, PyArray_MultiIter_DATA(multi, mi), elsize);
        ret_data += elsize;
        PyArray_MultiIter_NEXT(multi);
    }

    PyArray_INCREF(obj);
    Py_DECREF(multi);
    for (i = 0; i < n; i++) {
        Py_XDECREF(mps[i]);
    }
    Py_DECREF(ap);
    PyDataMem_FREE(mps);
    if (out != NULL && out != obj) {
        Py_INCREF(out);
        Py_DECREF(obj);
        obj = out;
    }
    return (PyObject *)obj;

 fail:
    Py_XDECREF(multi);
    for (i = 0; i < n; i++) {
        Py_XDECREF(mps[i]);
    }
    Py_XDECREF(ap);
    PyDataMem_FREE(mps);
    PyArray_XDECREF_ERR(obj);
    return NULL;
}

/*
 * These algorithms use special sorting.  They are not called unless the
 * underlying sort function for the type is available.  Note that axis is
 * already valid. The sort functions require 1-d contiguous and well-behaved
 * data.  Therefore, a copy will be made of the data if needed before handing
 * it to the sorting routine.  An iterator is constructed and adjusted to walk
 * over all but the desired sorting axis.
 */
static int
_new_sort(PyArrayObject *op, int axis, NPY_SORTKIND which)
{
    PyArrayIterObject *it;
    int needcopy = 0, swap;
    intp N, size;
    int elsize;
    intp astride;
    PyArray_SortFunc *sort;
    BEGIN_THREADS_DEF;

    it = (PyArrayIterObject *)PyArray_IterAllButAxis((PyObject *)op, &axis);
    swap = !PyArray_ISNOTSWAPPED(op);
    if (it == NULL) {
        return -1;
    }

    NPY_BEGIN_THREADS_DESCR(PyArray_DESCR(op));
    sort = PyArray_DESCR(op)->f->sort[which];
    size = it->size;
    N = PyArray_DIMS(op)[axis];
    elsize = PyArray_DESCR(op)->elsize;
    astride = PyArray_STRIDES(op)[axis];

    needcopy = !(PyArray_FLAGS(op) & NPY_ARRAY_ALIGNED) ||
                (astride != (intp) elsize) || swap;
    if (needcopy) {
        char *buffer = PyDataMem_NEW(N*elsize);

        while (size--) {
            _unaligned_strided_byte_copy(buffer, (intp) elsize, it->dataptr,
                                         astride, N, elsize);
            if (swap) {
                _strided_byte_swap(buffer, (intp) elsize, N, elsize);
            }
            if (sort(buffer, N, op) < 0) {
                PyDataMem_FREE(buffer);
                goto fail;
            }
            if (swap) {
                _strided_byte_swap(buffer, (intp) elsize, N, elsize);
            }
            _unaligned_strided_byte_copy(it->dataptr, astride, buffer,
                                         (intp) elsize, N, elsize);
            PyArray_ITER_NEXT(it);
        }
        PyDataMem_FREE(buffer);
    }
    else {
        while (size--) {
            if (sort(it->dataptr, N, op) < 0) {
                goto fail;
            }
            PyArray_ITER_NEXT(it);
        }
    }
    NPY_END_THREADS_DESCR(PyArray_DESCR(op));
    Py_DECREF(it);
    return 0;

 fail:
    NPY_END_THREADS;
    Py_DECREF(it);
    return 0;
}

static PyObject*
_new_argsort(PyArrayObject *op, int axis, NPY_SORTKIND which)
{

    PyArrayIterObject *it = NULL;
    PyArrayIterObject *rit = NULL;
    PyArrayObject *ret;
    int needcopy = 0, i;
    intp N, size;
    int elsize, swap;
    intp astride, rstride, *iptr;
    PyArray_ArgSortFunc *argsort;
    BEGIN_THREADS_DEF;

    ret = (PyArrayObject *)PyArray_New(Py_TYPE(op),
                            PyArray_NDIM(op),
                            PyArray_DIMS(op),
                            NPY_INTP,
                            NULL, NULL, 0, 0, (PyObject *)op);
    if (ret == NULL) {
        return NULL;
    }
    it = (PyArrayIterObject *)PyArray_IterAllButAxis((PyObject *)op, &axis);
    rit = (PyArrayIterObject *)PyArray_IterAllButAxis((PyObject *)ret, &axis);
    if (rit == NULL || it == NULL) {
        goto fail;
    }
    swap = !PyArray_ISNOTSWAPPED(op);

    NPY_BEGIN_THREADS_DESCR(PyArray_DESCR(op));
    argsort = PyArray_DESCR(op)->f->argsort[which];
    size = it->size;
    N = PyArray_DIMS(op)[axis];
    elsize = PyArray_DESCR(op)->elsize;
    astride = PyArray_STRIDES(op)[axis];
    rstride = PyArray_STRIDE(ret,axis);

    needcopy = swap || !(PyArray_FLAGS(op) & NPY_ARRAY_ALIGNED) ||
                         (astride != (intp) elsize) ||
            (rstride != sizeof(intp));
    if (needcopy) {
        char *valbuffer, *indbuffer;

        valbuffer = PyDataMem_NEW(N*elsize);
        indbuffer = PyDataMem_NEW(N*sizeof(intp));
        while (size--) {
            _unaligned_strided_byte_copy(valbuffer, (intp) elsize, it->dataptr,
                                         astride, N, elsize);
            if (swap) {
                _strided_byte_swap(valbuffer, (intp) elsize, N, elsize);
            }
            iptr = (intp *)indbuffer;
            for (i = 0; i < N; i++) {
                *iptr++ = i;
            }
            if (argsort(valbuffer, (intp *)indbuffer, N, op) < 0) {
                PyDataMem_FREE(valbuffer);
                PyDataMem_FREE(indbuffer);
                goto fail;
            }
            _unaligned_strided_byte_copy(rit->dataptr, rstride, indbuffer,
                                         sizeof(intp), N, sizeof(intp));
            PyArray_ITER_NEXT(it);
            PyArray_ITER_NEXT(rit);
        }
        PyDataMem_FREE(valbuffer);
        PyDataMem_FREE(indbuffer);
    }
    else {
        while (size--) {
            iptr = (intp *)rit->dataptr;
            for (i = 0; i < N; i++) {
                *iptr++ = i;
            }
            if (argsort(it->dataptr, (intp *)rit->dataptr, N, op) < 0) {
                goto fail;
            }
            PyArray_ITER_NEXT(it);
            PyArray_ITER_NEXT(rit);
        }
    }

    NPY_END_THREADS_DESCR(PyArray_DESCR(op));

    Py_DECREF(it);
    Py_DECREF(rit);
    return (PyObject *)ret;

 fail:
    NPY_END_THREADS;
    Py_DECREF(ret);
    Py_XDECREF(it);
    Py_XDECREF(rit);
    return NULL;
}


/* Be sure to save this global_compare when necessary */
static PyArrayObject *global_obj;

static int
qsortCompare (const void *a, const void *b)
{
    return PyArray_DESCR(global_obj)->f->compare(a,b,global_obj);
}

/*
 * Consumes reference to ap (op gets it) op contains a version of
 * the array with axes swapped if local variable axis is not the
 * last dimension.  Origin must be defined locally.
 */
#define SWAPAXES(op, ap) { \
        orign = PyArray_NDIM(ap)-1; \
        if (axis != orign) { \
            (op) = (PyArrayObject *)PyArray_SwapAxes((ap), axis, orign); \
            Py_DECREF((ap)); \
            if ((op) == NULL) return NULL; \
        } \
        else (op) = (ap); \
    }

/*
 * Consumes reference to ap (op gets it) origin must be previously
 * defined locally.  SWAPAXES must have been called previously.
 * op contains the swapped version of the array.
 */
#define SWAPBACK(op, ap) {                                      \
        if (axis != orign) {                                    \
            (op) = (PyArrayObject *)PyArray_SwapAxes((ap), axis, orign); \
            Py_DECREF((ap));                                    \
            if ((op) == NULL) return NULL;                      \
        }                                                       \
        else (op) = (ap);                                       \
    }

/* These swap axes in-place if necessary */
#define SWAPINTP(a,b) {intp c; c=(a); (a) = (b); (b) = c;}
#define SWAPAXES2(ap) { \
        orign = PyArray_NDIM(ap)-1; \
        if (axis != orign) { \
            SWAPINTP(PyArray_DIMS(ap)[axis], PyArray_DIMS(ap)[orign]); \
            SWAPINTP(PyArray_STRIDES(ap)[axis], PyArray_STRIDES(ap)[orign]); \
            PyArray_UpdateFlags(ap, NPY_ARRAY_C_CONTIGUOUS | \
                                    NPY_ARRAY_F_CONTIGUOUS); \
        } \
    }

#define SWAPBACK2(ap) { \
        if (axis != orign) { \
            SWAPINTP(PyArray_DIMS(ap)[axis], PyArray_DIMS(ap)[orign]); \
            SWAPINTP(PyArray_STRIDES(ap)[axis], PyArray_STRIDES(ap)[orign]); \
            PyArray_UpdateFlags(ap, NPY_ARRAY_C_CONTIGUOUS | \
                                    NPY_ARRAY_F_CONTIGUOUS); \
        } \
    }

/*NUMPY_API
 * Sort an array in-place
 */
NPY_NO_EXPORT int
PyArray_Sort(PyArrayObject *op, int axis, NPY_SORTKIND which)
{
    PyArrayObject *ap = NULL, *store_arr = NULL;
    char *ip;
    int i, n, m, elsize, orign;

    n = PyArray_NDIM(op);
    if ((n == 0) || (PyArray_SIZE(op) == 1)) {
        return 0;
    }
    if (axis < 0) {
        axis += n;
    }
    if ((axis < 0) || (axis >= n)) {
        PyErr_Format(PyExc_ValueError, "axis(=%d) out of bounds", axis);
        return -1;
    }
    if (!PyArray_ISWRITEABLE(op)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "attempted sort on unwriteable array.");
        return -1;
    }

    /* Determine if we should use type-specific algorithm or not */
    if (PyArray_DESCR(op)->f->sort[which] != NULL) {
        return _new_sort(op, axis, which);
    }
    if ((which != PyArray_QUICKSORT)
        || PyArray_DESCR(op)->f->compare == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "desired sort not supported for this type");
        return -1;
    }

    SWAPAXES2(op);

    ap = (PyArrayObject *)PyArray_FromAny((PyObject *)op,
                          NULL, 1, 0,
                          NPY_ARRAY_DEFAULT | NPY_ARRAY_UPDATEIFCOPY, NULL);
    if (ap == NULL) {
        goto fail;
    }
    elsize = PyArray_DESCR(ap)->elsize;
    m = PyArray_DIMS(ap)[PyArray_NDIM(ap)-1];
    if (m == 0) {
        goto finish;
    }
    n = PyArray_SIZE(ap)/m;

    /* Store global -- allows re-entry -- restore before leaving*/
    store_arr = global_obj;
    global_obj = ap;
    for (ip = PyArray_DATA(ap), i = 0; i < n; i++, ip += elsize*m) {
        qsort(ip, m, elsize, qsortCompare);
    }
    global_obj = store_arr;

    if (PyErr_Occurred()) {
        goto fail;
    }

 finish:
    Py_DECREF(ap);  /* Should update op if needed */
    SWAPBACK2(op);
    return 0;

 fail:
    Py_XDECREF(ap);
    SWAPBACK2(op);
    return -1;
}


static char *global_data;

static int
argsort_static_compare(const void *ip1, const void *ip2)
{
    int isize = PyArray_DESCR(global_obj)->elsize;
    const intp *ipa = ip1;
    const intp *ipb = ip2;
    return PyArray_DESCR(global_obj)->f->compare(global_data + (isize * *ipa),
                                         global_data + (isize * *ipb),
                                         global_obj);
}

/*NUMPY_API
 * ArgSort an array
 */
NPY_NO_EXPORT PyObject *
PyArray_ArgSort(PyArrayObject *op, int axis, NPY_SORTKIND which)
{
    PyArrayObject *ap = NULL, *ret = NULL, *store, *op2;
    intp *ip;
    intp i, j, n, m, orign;
    int argsort_elsize;
    char *store_ptr;

    n = PyArray_NDIM(op);
    if ((n == 0) || (PyArray_SIZE(op) == 1)) {
        ret = (PyArrayObject *)PyArray_New(Py_TYPE(op), PyArray_NDIM(op),
                                           PyArray_DIMS(op),
                                           PyArray_INTP,
                                           NULL, NULL, 0, 0,
                                           (PyObject *)op);
        if (ret == NULL) {
            return NULL;
        }
        *((intp *)PyArray_DATA(ret)) = 0;
        return (PyObject *)ret;
    }

    /* Creates new reference op2 */
    if ((op2=(PyArrayObject *)_check_axis(op, &axis, 0)) == NULL) {
        return NULL;
    }
    /* Determine if we should use new algorithm or not */
    if (PyArray_DESCR(op2)->f->argsort[which] != NULL) {
        ret = (PyArrayObject *)_new_argsort(op2, axis, which);
        Py_DECREF(op2);
        return (PyObject *)ret;
    }

    if ((which != PyArray_QUICKSORT) || PyArray_DESCR(op2)->f->compare == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "requested sort not available for type");
        Py_DECREF(op2);
        op = NULL;
        goto fail;
    }

    /* ap will contain the reference to op2 */
    SWAPAXES(ap, op2);
    op = (PyArrayObject *)PyArray_ContiguousFromAny((PyObject *)ap,
                                                    PyArray_NOTYPE,
                                                    1, 0);
    Py_DECREF(ap);
    if (op == NULL) {
        return NULL;
    }
    ret = (PyArrayObject *)PyArray_New(Py_TYPE(op), PyArray_NDIM(op),
                                       PyArray_DIMS(op), PyArray_INTP,
                                       NULL, NULL, 0, 0, (PyObject *)op);
    if (ret == NULL) {
        goto fail;
    }
    ip = (intp *)PyArray_DATA(ret);
    argsort_elsize = PyArray_DESCR(op)->elsize;
    m = PyArray_DIMS(op)[PyArray_NDIM(op)-1];
    if (m == 0) {
        goto finish;
    }
    n = PyArray_SIZE(op)/m;
    store_ptr = global_data;
    global_data = PyArray_DATA(op);
    store = global_obj;
    global_obj = op;
    for (i = 0; i < n; i++, ip += m, global_data += m*argsort_elsize) {
        for (j = 0; j < m; j++) {
            ip[j] = j;
        }
        qsort((char *)ip, m, sizeof(intp), argsort_static_compare);
    }
    global_data = store_ptr;
    global_obj = store;

 finish:
    Py_DECREF(op);
    SWAPBACK(op, ret);
    return (PyObject *)op;

 fail:
    Py_XDECREF(op);
    Py_XDECREF(ret);
    return NULL;

}


/*NUMPY_API
 *LexSort an array providing indices that will sort a collection of arrays
 *lexicographically.  The first key is sorted on first, followed by the second key
 *-- requires that arg"merge"sort is available for each sort_key
 *
 *Returns an index array that shows the indexes for the lexicographic sort along
 *the given axis.
 */
NPY_NO_EXPORT PyObject *
PyArray_LexSort(PyObject *sort_keys, int axis)
{
    PyArrayObject **mps;
    PyArrayIterObject **its;
    PyArrayObject *ret = NULL;
    PyArrayIterObject *rit = NULL;
    int n;
    int nd;
    int needcopy = 0, i,j;
    intp N, size;
    int elsize;
    int maxelsize;
    intp astride, rstride, *iptr;
    int object = 0;
    PyArray_ArgSortFunc *argsort;
    NPY_BEGIN_THREADS_DEF;

    if (!PySequence_Check(sort_keys)
           || ((n = PySequence_Size(sort_keys)) <= 0)) {
        PyErr_SetString(PyExc_TypeError,
                "need sequence of keys with len > 0 in lexsort");
        return NULL;
    }
    mps = (PyArrayObject **) _pya_malloc(n*NPY_SIZEOF_PYARRAYOBJECT);
    if (mps == NULL) {
        return PyErr_NoMemory();
    }
    its = (PyArrayIterObject **) _pya_malloc(n*sizeof(PyArrayIterObject));
    if (its == NULL) {
        _pya_free(mps);
        return PyErr_NoMemory();
    }
    for (i = 0; i < n; i++) {
        mps[i] = NULL;
        its[i] = NULL;
    }
    for (i = 0; i < n; i++) {
        PyObject *obj;
        obj = PySequence_GetItem(sort_keys, i);
        mps[i] = (PyArrayObject *)PyArray_FROM_O(obj);
        Py_DECREF(obj);
        if (mps[i] == NULL) {
            goto fail;
        }
        if (i > 0) {
            if ((PyArray_NDIM(mps[i]) != PyArray_NDIM(mps[0]))
                || (!PyArray_CompareLists(PyArray_DIMS(mps[i]),
                                       PyArray_DIMS(mps[0]),
                                       PyArray_NDIM(mps[0])))) {
                PyErr_SetString(PyExc_ValueError,
                                "all keys need to be the same shape");
                goto fail;
            }
        }
        if (!PyArray_DESCR(mps[i])->f->argsort[PyArray_MERGESORT]) {
            PyErr_Format(PyExc_TypeError,
                         "merge sort not available for item %d", i);
            goto fail;
        }
        if (!object
            && PyDataType_FLAGCHK(PyArray_DESCR(mps[i]), NPY_NEEDS_PYAPI)) {
            object = 1;
        }
        its[i] = (PyArrayIterObject *)PyArray_IterAllButAxis(
                (PyObject *)mps[i], &axis);
        if (its[i] == NULL) {
            goto fail;
        }
    }

    /* Now we can check the axis */
    nd = PyArray_NDIM(mps[0]);
    if ((nd == 0) || (PyArray_SIZE(mps[0]) == 1)) {
        /* single element case */
        ret = (PyArrayObject *)PyArray_New(&PyArray_Type, PyArray_NDIM(mps[0]),
                                           PyArray_DIMS(mps[0]),
                                           PyArray_INTP,
                                           NULL, NULL, 0, 0, NULL);

        if (ret == NULL) {
            goto fail;
        }
        *((intp *)(PyArray_DATA(ret))) = 0;
        goto finish;
    }
    if (axis < 0) {
        axis += nd;
    }
    if ((axis < 0) || (axis >= nd)) {
        PyErr_Format(PyExc_ValueError,
                "axis(=%d) out of bounds", axis);
        goto fail;
    }

    /* Now do the sorting */
    ret = (PyArrayObject *)PyArray_New(&PyArray_Type, PyArray_NDIM(mps[0]),
                                       PyArray_DIMS(mps[0]), PyArray_INTP,
                                       NULL, NULL, 0, 0, NULL);
    if (ret == NULL) {
        goto fail;
    }
    rit = (PyArrayIterObject *)
            PyArray_IterAllButAxis((PyObject *)ret, &axis);
    if (rit == NULL) {
        goto fail;
    }
    if (!object) {
        NPY_BEGIN_THREADS;
    }
    size = rit->size;
    N = PyArray_DIMS(mps[0])[axis];
    rstride = PyArray_STRIDE(ret, axis);
    maxelsize = PyArray_DESCR(mps[0])->elsize;
    needcopy = (rstride != sizeof(intp));
    for (j = 0; j < n; j++) {
        needcopy = needcopy
            || PyArray_ISBYTESWAPPED(mps[j])
            || !(PyArray_FLAGS(mps[j]) & NPY_ARRAY_ALIGNED)
            || (PyArray_STRIDES(mps[j])[axis] != (intp)PyArray_DESCR(mps[j])->elsize);
        if (PyArray_DESCR(mps[j])->elsize > maxelsize) {
            maxelsize = PyArray_DESCR(mps[j])->elsize;
        }
    }

    if (needcopy) {
        char *valbuffer, *indbuffer;
        int *swaps;

        valbuffer = PyDataMem_NEW(N*maxelsize);
        indbuffer = PyDataMem_NEW(N*sizeof(intp));
        swaps = malloc(n*sizeof(int));
        for (j = 0; j < n; j++) {
            swaps[j] = PyArray_ISBYTESWAPPED(mps[j]);
        }
        while (size--) {
            iptr = (intp *)indbuffer;
            for (i = 0; i < N; i++) {
                *iptr++ = i;
            }
            for (j = 0; j < n; j++) {
                elsize = PyArray_DESCR(mps[j])->elsize;
                astride = PyArray_STRIDES(mps[j])[axis];
                argsort = PyArray_DESCR(mps[j])->f->argsort[PyArray_MERGESORT];
                _unaligned_strided_byte_copy(valbuffer, (intp) elsize,
                                             its[j]->dataptr, astride, N, elsize);
                if (swaps[j]) {
                    _strided_byte_swap(valbuffer, (intp) elsize, N, elsize);
                }
                if (argsort(valbuffer, (intp *)indbuffer, N, mps[j]) < 0) {
                    PyDataMem_FREE(valbuffer);
                    PyDataMem_FREE(indbuffer);
                    free(swaps);
                    goto fail;
                }
                PyArray_ITER_NEXT(its[j]);
            }
            _unaligned_strided_byte_copy(rit->dataptr, rstride, indbuffer,
                                         sizeof(intp), N, sizeof(intp));
            PyArray_ITER_NEXT(rit);
        }
        PyDataMem_FREE(valbuffer);
        PyDataMem_FREE(indbuffer);
        free(swaps);
    }
    else {
        while (size--) {
            iptr = (intp *)rit->dataptr;
            for (i = 0; i < N; i++) {
                *iptr++ = i;
            }
            for (j = 0; j < n; j++) {
                argsort = PyArray_DESCR(mps[j])->f->argsort[PyArray_MERGESORT];
                if (argsort(its[j]->dataptr, (intp *)rit->dataptr,
                            N, mps[j]) < 0) {
                    goto fail;
                }
                PyArray_ITER_NEXT(its[j]);
            }
            PyArray_ITER_NEXT(rit);
        }
    }

    if (!object) {
        NPY_END_THREADS;
    }

 finish:
    for (i = 0; i < n; i++) {
        Py_XDECREF(mps[i]);
        Py_XDECREF(its[i]);
    }
    Py_XDECREF(rit);
    _pya_free(mps);
    _pya_free(its);
    return (PyObject *)ret;

 fail:
    NPY_END_THREADS;
    Py_XDECREF(rit);
    Py_XDECREF(ret);
    for (i = 0; i < n; i++) {
        Py_XDECREF(mps[i]);
        Py_XDECREF(its[i]);
    }
    _pya_free(mps);
    _pya_free(its);
    return NULL;
}


/** @brief Use bisection of sorted array to find first entries >= keys.
 *
 * For each key use bisection to find the first index i s.t. key <= arr[i].
 * When there is no such index i, set i = len(arr). Return the results in ret.
 * All arrays are assumed contiguous on entry and both arr and key must be of
 * the same comparable type.
 *
 * @param arr contiguous sorted array to be searched.
 * @param key contiguous array of keys.
 * @param ret contiguous array of intp for returned indices.
 * @return void
 */
static void
local_search_left(PyArrayObject *arr, PyArrayObject *key, PyArrayObject *ret)
{
    PyArray_CompareFunc *compare = PyArray_DESCR(key)->f->compare;
    intp nelts = PyArray_DIMS(arr)[PyArray_NDIM(arr) - 1];
    intp nkeys = PyArray_SIZE(key);
    char *parr = PyArray_DATA(arr);
    char *pkey = PyArray_DATA(key);
    intp *pret = (intp *)PyArray_DATA(ret);
    int elsize = PyArray_DESCR(arr)->elsize;
    intp i;

    for (i = 0; i < nkeys; ++i) {
        intp imin = 0;
        intp imax = nelts;
        while (imin < imax) {
            intp imid = imin + ((imax - imin) >> 1);
            if (compare(parr + elsize*imid, pkey, key) < 0) {
                imin = imid + 1;
            }
            else {
                imax = imid;
            }
        }
        *pret = imin;
        pret += 1;
        pkey += elsize;
    }
}


/** @brief Use bisection of sorted array to find first entries > keys.
 *
 * For each key use bisection to find the first index i s.t. key < arr[i].
 * When there is no such index i, set i = len(arr). Return the results in ret.
 * All arrays are assumed contiguous on entry and both arr and key must be of
 * the same comparable type.
 *
 * @param arr contiguous sorted array to be searched.
 * @param key contiguous array of keys.
 * @param ret contiguous array of intp for returned indices.
 * @return void
 */
static void
local_search_right(PyArrayObject *arr, PyArrayObject *key, PyArrayObject *ret)
{
    PyArray_CompareFunc *compare = PyArray_DESCR(key)->f->compare;
    intp nelts = PyArray_DIMS(arr)[PyArray_NDIM(arr) - 1];
    intp nkeys = PyArray_SIZE(key);
    char *parr = PyArray_DATA(arr);
    char *pkey = PyArray_DATA(key);
    intp *pret = (intp *)PyArray_DATA(ret);
    int elsize = PyArray_DESCR(arr)->elsize;
    intp i;

    for(i = 0; i < nkeys; ++i) {
        intp imin = 0;
        intp imax = nelts;
        while (imin < imax) {
            intp imid = imin + ((imax - imin) >> 1);
            if (compare(parr + elsize*imid, pkey, key) <= 0) {
                imin = imid + 1;
            }
            else {
                imax = imid;
            }
        }
        *pret = imin;
        pret += 1;
        pkey += elsize;
    }
}

/*NUMPY_API
 * Numeric.searchsorted(a,v)
 */
NPY_NO_EXPORT PyObject *
PyArray_SearchSorted(PyArrayObject *op1, PyObject *op2, NPY_SEARCHSIDE side)
{
    PyArrayObject *ap1 = NULL;
    PyArrayObject *ap2 = NULL;
    PyArrayObject *ret = NULL;
    PyArray_Descr *dtype;
    NPY_BEGIN_THREADS_DEF;

    dtype = PyArray_DescrFromObject((PyObject *)op2, PyArray_DESCR(op1));
    /* need ap1 as contiguous array and of right type */
    Py_INCREF(dtype);
    ap1 = (PyArrayObject *)PyArray_CheckFromAny((PyObject *)op1, dtype,
                                1, 1, NPY_ARRAY_DEFAULT |
                                      NPY_ARRAY_NOTSWAPPED, NULL);
    if (ap1 == NULL) {
        Py_DECREF(dtype);
        return NULL;
    }

    /* need ap2 as contiguous array and of right type */
    ap2 = (PyArrayObject *)PyArray_CheckFromAny(op2, dtype,
                                0, 0, NPY_ARRAY_DEFAULT |
                                      NPY_ARRAY_NOTSWAPPED, NULL);
    if (ap2 == NULL) {
        goto fail;
    }
    /* ret is a contiguous array of intp type to hold returned indices */
    ret = (PyArrayObject *)PyArray_New(Py_TYPE(ap2), PyArray_NDIM(ap2),
                                       PyArray_DIMS(ap2), PyArray_INTP,
                                       NULL, NULL, 0, 0, (PyObject *)ap2);
    if (ret == NULL) {
        goto fail;
    }
    /* check that comparison function exists */
    if (PyArray_DESCR(ap2)->f->compare == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "compare not supported for type");
        goto fail;
    }

    if (side == NPY_SEARCHLEFT) {
        NPY_BEGIN_THREADS_DESCR(PyArray_DESCR(ap2));
        local_search_left(ap1, ap2, ret);
        NPY_END_THREADS_DESCR(PyArray_DESCR(ap2));
    }
    else if (side == NPY_SEARCHRIGHT) {
        NPY_BEGIN_THREADS_DESCR(PyArray_DESCR(ap2));
        local_search_right(ap1, ap2, ret);
        NPY_END_THREADS_DESCR(PyArray_DESCR(ap2));
    }
    Py_DECREF(ap1);
    Py_DECREF(ap2);
    return (PyObject *)ret;

 fail:
    Py_XDECREF(ap1);
    Py_XDECREF(ap2);
    Py_XDECREF(ret);
    return NULL;
}

/*NUMPY_API
 * Diagonal
 */
NPY_NO_EXPORT PyObject *
PyArray_Diagonal(PyArrayObject *self, int offset, int axis1, int axis2)
{
    int n = PyArray_NDIM(self);
    PyObject *new;
    PyArray_Dims newaxes;
    intp dims[MAX_DIMS];
    int i, pos;

    newaxes.ptr = dims;
    if (n < 2) {
        PyErr_SetString(PyExc_ValueError,
                        "array.ndim must be >= 2");
        return NULL;
    }
    if (axis1 < 0) {
        axis1 += n;
    }
    if (axis2 < 0) {
        axis2 += n;
    }
    if ((axis1 == axis2) || (axis1 < 0) || (axis1 >= n) ||
        (axis2 < 0) || (axis2 >= n)) {
        PyErr_Format(PyExc_ValueError, "axis1(=%d) and axis2(=%d) "\
                     "must be different and within range (nd=%d)",
                     axis1, axis2, n);
        return NULL;
    }

    newaxes.len = n;
    /* insert at the end */
    newaxes.ptr[n-2] = axis1;
    newaxes.ptr[n-1] = axis2;
    pos = 0;
    for (i = 0; i < n; i++) {
        if ((i==axis1) || (i==axis2)) {
            continue;
        }
        newaxes.ptr[pos++] = i;
    }
    new = PyArray_Transpose(self, &newaxes);
    if (new == NULL) {
        return NULL;
    }
    self = (PyArrayObject *)new;

    if (n == 2) {
        PyObject *a = NULL, *ret = NULL;
        PyArrayObject *indices = NULL;
        intp n1, n2, start, stop, step, count;
        intp *dptr;

        n1 = PyArray_DIMS(self)[0];
        n2 = PyArray_DIMS(self)[1];
        step = n2 + 1;
        if (offset < 0) {
            start = -n2 * offset;
            stop = MIN(n2, n1+offset)*(n2+1) - n2*offset;
        }
        else {
            start = offset;
            stop = MIN(n1, n2-offset)*(n2+1) + offset;
        }

        /* count = ceil((stop-start)/step) */
        count = ((stop-start) / step) + (((stop-start) % step) != 0);
        indices = (PyArrayObject *)PyArray_New(&PyArray_Type, 1, &count,
                              PyArray_INTP, NULL, NULL, 0, 0, NULL);
        if (indices == NULL) {
            Py_DECREF(self);
            return NULL;
        }
        dptr = (intp *)PyArray_DATA(indices);
        for (n1 = start; n1 < stop; n1 += step) {
            *dptr++ = n1;
        }
        a = PyArray_IterNew((PyObject *)self);
        Py_DECREF(self);
        if (a == NULL) {
            Py_DECREF(indices);
            return NULL;
        }
        ret = PyObject_GetItem(a, (PyObject *)indices);
        Py_DECREF(a);
        Py_DECREF(indices);
        return ret;
    }

    else {
        /*
         * my_diagonal = []
         * for i in range (s [0]) :
         * my_diagonal.append (diagonal (a [i], offset))
         * return array (my_diagonal)
         */
        PyObject *mydiagonal = NULL, *ret = NULL, *sel = NULL;
        intp n1;
        int res;
        PyArray_Descr *typecode;

        new = NULL;

        typecode = PyArray_DESCR(self);
        mydiagonal = PyList_New(0);
        if (mydiagonal == NULL) {
            Py_DECREF(self);
            return NULL;
        }
        n1 = PyArray_DIMS(self)[0];
        for (i = 0; i < n1; i++) {
            new = PyInt_FromLong((long) i);
            sel = PyArray_EnsureAnyArray(PyObject_GetItem((PyObject *)self, new));
            Py_DECREF(new);
            if (sel == NULL) {
                Py_DECREF(self);
                Py_DECREF(mydiagonal);
                return NULL;
            }
            new = PyArray_Diagonal((PyArrayObject *)sel, offset, n-3, n-2);
            Py_DECREF(sel);
            if (new == NULL) {
                Py_DECREF(self);
                Py_DECREF(mydiagonal);
                return NULL;
            }
            res = PyList_Append(mydiagonal, new);
            Py_DECREF(new);
            if (res < 0) {
                Py_DECREF(self);
                Py_DECREF(mydiagonal);
                return NULL;
            }
        }
        Py_DECREF(self);
        Py_INCREF(typecode);
        ret =  PyArray_FromAny(mydiagonal, typecode, 0, 0, 0, NULL);
        Py_DECREF(mydiagonal);
        return ret;
    }
}

/*NUMPY_API
 * Compress
 */
NPY_NO_EXPORT PyObject *
PyArray_Compress(PyArrayObject *self, PyObject *condition, int axis,
                 PyArrayObject *out)
{
    PyArrayObject *cond;
    PyObject *res, *ret;

    cond = (PyArrayObject *)PyArray_FROM_O(condition);
    if (cond == NULL) {
        return NULL;
    }
    if (PyArray_NDIM(cond) != 1) {
        Py_DECREF(cond);
        PyErr_SetString(PyExc_ValueError,
                        "condition must be 1-d array");
        return NULL;
    }

    res = PyArray_Nonzero(cond);
    Py_DECREF(cond);
    if (res == NULL) {
        return res;
    }
    ret = PyArray_TakeFrom(self, PyTuple_GET_ITEM(res, 0), axis,
                           out, NPY_RAISE);
    Py_DECREF(res);
    return ret;
}

/*NUMPY_API
 * Counts the number of non-zero elements in the array
 *
 * Returns -1 on error.
 */
NPY_NO_EXPORT npy_intp
PyArray_CountNonzero(PyArrayObject *self)
{
    PyArray_NonzeroFunc *nonzero = PyArray_DESCR(self)->f->nonzero;
    char *data;
    npy_intp stride, count;
    npy_intp nonzero_count = 0;

    NpyIter *iter;
    NpyIter_IterNextFunc *iternext;
    char **dataptr;
    npy_intp *strideptr, *innersizeptr;

    /* If it's a trivial one-dimensional loop, don't use an iterator */
    if (PyArray_TRIVIALLY_ITERABLE(self)) {
        PyArray_PREPARE_TRIVIAL_ITERATION(self, count, data, stride);

        while (count--) {
            if (nonzero(data, self)) {
                ++nonzero_count;
            }
            data += stride;
        }

        return nonzero_count;
    }

    /*
     * If the array has size zero, return zero (the iterator rejects
     * size zero arrays)
     */
    if (PyArray_SIZE(self) == 0) {
        return 0;
    }

    /* Otherwise create and use an iterator to count the nonzeros */
    iter = NpyIter_New(self, NPY_ITER_READONLY|
                             NPY_ITER_EXTERNAL_LOOP|
                             NPY_ITER_REFS_OK,
                        NPY_KEEPORDER, NPY_NO_CASTING,
                        NULL);
    if (iter == NULL) {
        return -1;
    }

    /* Get the pointers for inner loop iteration */
    iternext = NpyIter_GetIterNext(iter, NULL);
    if (iternext == NULL) {
        NpyIter_Deallocate(iter);
        return -1;
    }
    dataptr = NpyIter_GetDataPtrArray(iter);
    strideptr = NpyIter_GetInnerStrideArray(iter);
    innersizeptr = NpyIter_GetInnerLoopSizePtr(iter);

    /* Iterate over all the elements to count the nonzeros */
    do {
        data = *dataptr;
        stride = *strideptr;
        count = *innersizeptr;

        while (count--) {
            if (nonzero(data, self)) {
                ++nonzero_count;
            }
            data += stride;
        }

    } while(iternext(iter));

    NpyIter_Deallocate(iter);

    return nonzero_count;
}

/*NUMPY_API
 * Nonzero
 *
 * TODO: In NumPy 2.0, should make the iteration order a parameter.
 */
NPY_NO_EXPORT PyObject *
PyArray_Nonzero(PyArrayObject *self)
{
    int i, ndim = PyArray_NDIM(self);
    PyArrayObject *ret = NULL;
    PyObject *ret_tuple;
    npy_intp ret_dims[2];
    PyArray_NonzeroFunc *nonzero = PyArray_DESCR(self)->f->nonzero;
    char *data;
    npy_intp stride, count;
    npy_intp nonzero_count = PyArray_CountNonzero(self);
    npy_intp *multi_index;

    NpyIter *iter;
    NpyIter_IterNextFunc *iternext;
    NpyIter_GetMultiIndexFunc *get_multi_index;
    char **dataptr;

    /* Allocate the result as a 2D array */
    ret_dims[0] = nonzero_count;
    ret_dims[1] = (ndim == 0) ? 1 : ndim;
    ret = (PyArrayObject *)PyArray_New(&PyArray_Type, 2, ret_dims,
                       NPY_INTP, NULL, NULL, 0, 0,
                       NULL);
    if (ret == NULL) {
        return NULL;
    }

    /* If it's a one-dimensional result, don't use an iterator */
    if (ndim <= 1) {
        npy_intp j;

        multi_index = (npy_intp *)PyArray_DATA(ret);
        data = PyArray_BYTES(self);
        stride = (ndim == 0) ? 0 : PyArray_STRIDE(self, 0);
        count = (ndim == 0) ? 1 : PyArray_DIM(self, 0);

        for (j = 0; j < count; ++j) {
            if (nonzero(data, self)) {
                *multi_index++ = j;
            }
            data += stride;
        }

        goto finish;
    }

    /* Build an iterator tracking a multi-index, in C order */
    iter = NpyIter_New(self, NPY_ITER_READONLY|
                             NPY_ITER_MULTI_INDEX|
                             NPY_ITER_ZEROSIZE_OK|
                             NPY_ITER_REFS_OK,
                        NPY_CORDER, NPY_NO_CASTING,
                        NULL);

    if (iter == NULL) {
        Py_DECREF(ret);
        return NULL;
    }

    if (NpyIter_GetIterSize(iter) != 0) {
        /* Get the pointers for inner loop iteration */
        iternext = NpyIter_GetIterNext(iter, NULL);
        if (iternext == NULL) {
            NpyIter_Deallocate(iter);
            Py_DECREF(ret);
            return NULL;
        }
        get_multi_index = NpyIter_GetGetMultiIndex(iter, NULL);
        if (get_multi_index == NULL) {
            NpyIter_Deallocate(iter);
            Py_DECREF(ret);
            return NULL;
        }
        dataptr = NpyIter_GetDataPtrArray(iter);

        multi_index = (npy_intp *)PyArray_DATA(ret);

        /* Get the multi-index for each non-zero element */
        do {
            if (nonzero(*dataptr, self)) {
                get_multi_index(iter, multi_index);
                multi_index += ndim;
            }
        } while(iternext(iter));
    }

    NpyIter_Deallocate(iter);

finish:
    /* Treat zero-dimensional as shape (1,) */
    if (ndim == 0) {
        ndim = 1;
    }

    ret_tuple = PyTuple_New(ndim);
    if (ret_tuple == NULL) {
        Py_DECREF(ret);
        return NULL;
    }

    /* Create views into ret, one for each dimension */
    if (ndim == 1) {
        /* Directly switch to one dimensions (dimension 1 is 1 anyway) */
        ((PyArrayObject_fieldaccess *)ret)->nd = 1;
        PyTuple_SET_ITEM(ret_tuple, 0, (PyObject *)ret);
    }
    else {
        for (i = 0; i < ndim; ++i) {
            PyArrayObject *view;
            stride = ndim*NPY_SIZEOF_INTP;

            view = (PyArrayObject *)PyArray_New(Py_TYPE(self), 1,
                                &nonzero_count,
                                NPY_INTP, &stride,
                                PyArray_BYTES(ret) + i*NPY_SIZEOF_INTP,
                                0, 0, (PyObject *)self);
            if (view == NULL) {
                Py_DECREF(ret);
                Py_DECREF(ret_tuple);
                return NULL;
            }
            Py_INCREF(ret);
            if (PyArray_SetBaseObject(view, (PyObject *)ret) < 0) {
                Py_DECREF(ret);
                Py_DECREF(ret_tuple);
            }
            PyTuple_SET_ITEM(ret_tuple, i, (PyObject *)view);
        }

        Py_DECREF(ret);
    }

    return ret_tuple;
}
