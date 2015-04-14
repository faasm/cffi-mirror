
static PyObject *all_primitives[_CFFI__NUM_PRIM];


PyObject *build_primitive_type(int num)
{
    PyObject *x;

    switch (num) {

    case _CFFI_PRIM_VOID:
        x = new_void_type();
        break;

    case _CFFI_PRIM_INT:
        x = new_primitive_type("int");
        break;

    case _CFFI_PRIM_LONG:
        x = new_primitive_type("long");
        break;

    default:
        PyErr_Format(PyExc_NotImplementedError, "prim=%d", num);
        return NULL;
    }

    all_primitives[num] = x;
    return x;
}


static PyObject *
_realize_c_type_or_func(const struct _cffi_type_context_s *ctx,
                        _cffi_opcode_t opcodes[], int index);  /* forward */


/* Interpret an opcodes[] array.  If opcodes == ctx->types, store all
   the intermediate types back in the opcodes[].  Returns a new
   reference.
*/
static PyObject *
realize_c_type(const struct _cffi_type_context_s *ctx,
               _cffi_opcode_t opcodes[], int index)
{
    PyObject *x = _realize_c_type_or_func(ctx, opcodes, index);
    if (x == NULL || CTypeDescr_Check(x)) {
        return x;
    }
    else {
        PyObject *y;
        assert(PyTuple_Check(x));
        y = PyTuple_GET_ITEM(x, 0);
        PyErr_Format(FFIError, "the type '%s' is a function type, not a "
                               "pointer-to-function type",
                     ((CTypeDescrObject *)y)->ct_name);
        Py_DECREF(x);
        return NULL;
    }
}

static PyObject *
_realize_c_type_or_func(const struct _cffi_type_context_s *ctx,
                        _cffi_opcode_t opcodes[], int index)
{
    PyObject *x, *y, *z;
    _cffi_opcode_t op = opcodes[index];
    Py_ssize_t length = -1;

    if ((((uintptr_t)op) & 1) == 0) {
        x = (PyObject *)op;
        Py_INCREF(x);
        return x;
    }

    switch (_CFFI_GETOP(op)) {

    case _CFFI_OP_PRIMITIVE:
        x = all_primitives[_CFFI_GETARG(op)];
        if (x == NULL)
            x = build_primitive_type(_CFFI_GETARG(op));
        Py_XINCREF(x);
        break;

    case _CFFI_OP_POINTER:
        y = _realize_c_type_or_func(ctx, opcodes, _CFFI_GETARG(op));
        if (y == NULL)
            return NULL;
        if (CTypeDescr_Check(y)) {
            x = new_pointer_type((CTypeDescrObject *)y);
        }
        else {
            assert(PyTuple_Check(y));   /* from _CFFI_OP_FUNCTION */
            x = PyTuple_GET_ITEM(y, 0);
            Py_INCREF(x);
        }
        Py_DECREF(y);
        break;

    case _CFFI_OP_ARRAY:
        length = (Py_ssize_t)opcodes[_CFFI_GETARG(op) + 1];
        /* fall-through */
    case _CFFI_OP_OPEN_ARRAY:
        y = realize_c_type(ctx, opcodes, _CFFI_GETARG(op));
        if (y == NULL)
            return NULL;
        z = new_pointer_type((CTypeDescrObject *)y);
        Py_DECREF(y);
        if (z == NULL)
            return NULL;
        x = new_array_type((CTypeDescrObject *)z, length);
        Py_DECREF(z);
        break;

    case _CFFI_OP_FUNCTION:
    {
        PyObject *fargs;
        int i, base_index, num_args;

        y = realize_c_type(ctx, opcodes, _CFFI_GETARG(op));
        if (y == NULL)
            return NULL;

        base_index = index + 1;
        num_args = 0;
        while (_CFFI_GETOP(opcodes[base_index + num_args]) !=
                   _CFFI_OP_FUNCTION_END)
            num_args++;

        fargs = PyTuple_New(num_args);
        if (fargs == NULL) {
            Py_DECREF(y);
            return NULL;
        }

        for (i = 0; i < num_args; i++) {
            z = realize_c_type(ctx, opcodes, base_index + i);
            if (z == NULL) {
                Py_DECREF(fargs);
                Py_DECREF(y);
                return NULL;
            }
            PyTuple_SET_ITEM(fargs, i, z);
        }

        z = new_function_type(fargs, (CTypeDescrObject *)y, 0, FFI_DEFAULT_ABI);
        Py_DECREF(fargs);
        Py_DECREF(y);
        if (z == NULL)
            return NULL;

        x = PyTuple_Pack(1, z);   /* hack: hide the CT_FUNCTIONPTR.  it will
                                     be revealed again by the OP_POINTER */
        Py_DECREF(z);
        break;
    }

    case _CFFI_OP_NOOP:
        x = _realize_c_type_or_func(ctx, opcodes, _CFFI_GETARG(op));
        break;

    default:
        PyErr_Format(PyExc_NotImplementedError, "op=%d", (int)_CFFI_GETOP(op));
        return NULL;
    }

    if (x != NULL && opcodes == ctx->types) {
        assert((((uintptr_t)x) & 1) == 0);
        Py_INCREF(x);
        opcodes[index] = x;
    }
    return x;
};
