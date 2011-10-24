import sys

import numpy as np
from numpy.testing import *
from numpy.testing.utils import WarningManager
import warnings

def test_fastCopyAndTranspose():
    # 0D array
    a = np.array(2)
    b = np.fastCopyAndTranspose(a)
    assert_equal(b, a.T)
    assert_(b.flags.owndata)

    # 1D array
    a = np.array([3,2,7,0])
    b = np.fastCopyAndTranspose(a)
    assert_equal(b, a.T)
    assert_(b.flags.owndata)

    # 2D array
    a = np.arange(6).reshape(2,3)
    b = np.fastCopyAndTranspose(a)
    assert_equal(b, a.T)
    assert_(b.flags.owndata)

def test_array_astype():
    a = np.arange(6, dtype='f4').reshape(2,3)
    # Default behavior: allows unsafe casts, keeps memory layout,
    #                   always copies.
    b = a.astype('i4')
    assert_equal(a, b)
    assert_equal(b.dtype, np.dtype('i4'))
    assert_equal(a.strides, b.strides)
    b = a.T.astype('i4')
    assert_equal(a.T, b)
    assert_equal(b.dtype, np.dtype('i4'))
    assert_equal(a.T.strides, b.strides)
    b = a.astype('f4')
    assert_equal(a, b)
    assert_(not (a is b))

    # copy=False parameter can sometimes skip a copy
    b = a.astype('f4', copy=False)
    assert_(a is b)

    # order parameter allows overriding of the memory layout,
    # forcing a copy if the layout is wrong
    b = a.astype('f4', order='F', copy=False)
    assert_equal(a, b)
    assert_(not (a is b))
    assert_(b.flags.f_contiguous)

    b = a.astype('f4', order='C', copy=False)
    assert_equal(a, b)
    assert_(a is b)
    assert_(b.flags.c_contiguous)

    # casting parameter allows catching bad casts
    b = a.astype('c8', casting='safe')
    assert_equal(a, b)
    assert_equal(b.dtype, np.dtype('c8'))

    assert_raises(TypeError, a.astype, 'i4', casting='safe')

    # subok=False passes through a non-subclassed array
    b = a.astype('f4', subok=0, copy=False)
    assert_(a is b)

    a = np.matrix([[0,1,2],[3,4,5]], dtype='f4')

    # subok=True passes through a matrix
    b = a.astype('f4', subok=True, copy=False)
    assert_(a is b)

    # subok=True is default, and creates a subtype on a cast
    b = a.astype('i4', copy=False)
    assert_equal(a, b)
    assert_equal(type(b), np.matrix)

    # subok=False never returns a matrix
    b = a.astype('f4', subok=False, copy=False)
    assert_equal(a, b)
    assert_(not (a is b))
    assert_(type(b) != np.matrix)

def test_copyto():
    a = np.arange(6, dtype='i4').reshape(2,3)

    # Simple copy
    np.copyto(a, [[3,1,5], [6,2,1]])
    assert_equal(a, [[3,1,5], [6,2,1]])

    # Overlapping copy should work
    np.copyto(a[:,:2], a[::-1, 1::-1])
    assert_equal(a, [[2,6,5], [1,3,1]])

    # Defaults to 'same_kind' casting
    assert_raises(TypeError, np.copyto, a, 1.5)

    # Force a copy with 'unsafe' casting, truncating 1.5 to 1
    np.copyto(a, 1.5, casting='unsafe')
    assert_equal(a, 1)

    # Copying with a mask
    np.copyto(a, 3, where=[True,False,True])
    assert_equal(a, [[3,1,3],[3,1,3]])

    # Casting rule still applies with a mask
    assert_raises(TypeError, np.copyto, a, 3.5, where=[True,False,True])

    # Lists of integer 0's and 1's is ok too
    np.copyto(a, 4.0, casting='unsafe', where=[[0,1,1], [1,0,0]])
    assert_equal(a, [[3,4,4], [4,1,3]])

    # Overlapping copy with mask should work
    np.copyto(a[:,:2], a[::-1, 1::-1], where=[[0,1],[1,1]])
    assert_equal(a, [[3,4,4], [4,3,3]])

    # 'dst' must be an array
    assert_raises(TypeError, np.copyto, [1,2,3], [2,3,4])

if __name__ == "__main__":
    run_module_suite()
