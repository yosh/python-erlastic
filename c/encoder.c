#include "erlastic.h"

#include "structmember.h"

#include "eetftags.h"


typedef enum {
    UNICODE_TYPE_BINARY,
    UNICODE_TYPE_STR
} UnicodeType;

typedef struct _EncoderObject {
    PyObject_HEAD
    PyObject *encoding;
    PyObject *unicode_type;
} EncoderObject;

typedef struct _EncoderState {
    char *encoding;
    UnicodeType unicode_type;
    PyObject *parts;
    Py_ssize_t len;
} EncoderState;

static int encode_obj(EncoderState *state, PyObject *obj);

static int
append_part(EncoderState *state, PyObject *part)
{
    int res;

    state->len += PyString_GET_SIZE(part);

    res = PyList_Append(state->parts, part);
    Py_DECREF(part);

    return res;
}

static int
append_buffer(EncoderState *state, const unsigned char *buf, Py_ssize_t len)
{
    PyObject *s;

    s = PyString_FromStringAndSize((const char *)buf, len);
    if (s == NULL)
        return -1;

    return append_part(state, s);
}

static int
append_tag_and_uint8(EncoderState *state, int tag, unsigned char val)
{
    unsigned char buf[2] = { tag, val };
    return append_buffer(state, buf, sizeof(buf));
}

static int
append_tag_and_uint16(EncoderState *state, int tag, unsigned int val)
{
    unsigned char buf[3] = { tag, val >> 8, val };
    return append_buffer(state, buf, sizeof(buf));
}

static int
append_tag_and_uint32(EncoderState *state, int tag, unsigned int val)
{
    unsigned char buf[5] = { tag, val >> 24, val >> 16, val >> 8, val };
    return append_buffer(state, buf, sizeof(buf));
}

static int
append_tag_and_int32(EncoderState *state, int tag, int val)
{
    unsigned char buf[5] = { tag, val >> 24, val >> 16, val >> 8, val };
    return append_buffer(state, buf, sizeof(buf));
}

static int
append_empty_list(EncoderState *state)
{
    static unsigned char buf[1] = { NIL_EXT };
    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_none(EncoderState *state)
{
    static unsigned char buf[] = { ATOM_EXT, 0, 4, 'n', 'o', 'n', 'e' };
    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_false(EncoderState *state)
{
    static unsigned char buf[] = { ATOM_EXT, 0, 5, 'f', 'a', 'l', 's', 'e' };
    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_true(EncoderState *state)
{
    static unsigned char buf[] = { ATOM_EXT, 0, 4, 't', 'r', 'u', 'e' };
    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_long(EncoderState *state, PyObject *obj)
{
    PyObject *err, *s = NULL;
    long v;
    int sign, res = -1;
    size_t nbits, nbytes, header_size;
    unsigned char *p, buf[6];

    v = PyLong_AsLong(obj);

    err = PyErr_Occurred();
    if (!err) {
        if (v >= 0 && v <= 255) {
            return append_tag_and_uint8(state, SMALL_INTEGER_EXT, v);
        }
        else if (v >= -0x80000000L && v <= 0x7fffffffL) {
            return append_tag_and_int32(state, INTEGER_EXT, v);
        }
    }
    else if (!PyErr_GivenExceptionMatches(PyExc_OverflowError, err))
        return -1;

    PyErr_Clear();

    sign = _PyLong_Sign(obj);
    if (sign < 0) {
        obj = PyNumber_Negative(obj);
    }
    else {
        Py_INCREF(obj);
    }

    nbits = _PyLong_NumBits(obj);
    if (nbits == (size_t)-1 && PyErr_Occurred())
        goto bail;

    nbytes = nbits >> 3;
    if (nbits & 7)
        nbytes++;

    if (nbytes > INT_MAX) {
        PyErr_SetString(PyExc_OverflowError,
                        "erlastic: long too large to encode");
        goto bail;
    }

    if (nbytes > 255) {
        header_size = 6;
        buf[0] = LARGE_BIG_EXT;
        buf[1] = nbytes >> 24;
        buf[2] = nbytes >> 16;
        buf[3] = nbytes >> 8;
        buf[4] = nbytes;
        buf[5] = sign < 0 ? 1 : 0;
    }
    else {
        header_size = 3;
        buf[0] = SMALL_BIG_EXT;
        buf[1] = nbytes;
        buf[2] = sign < 0 ? 1 : 0;
    }

    s = PyString_FromStringAndSize(NULL, header_size + nbytes);
    if (s == NULL)
        goto bail;

    p = (unsigned char *)PyString_AS_STRING(s);

    Py_MEMCPY(p, buf, header_size);
    p += header_size;

    res = _PyLong_AsByteArray((PyLongObject *)obj, p, nbytes, 1, 0);
    if (res < 0)
        goto bail;

    res = append_part(state, s);

bail:
    Py_DECREF(obj);
    return res;
}

static int
encode_int(EncoderState *state, PyObject *obj)
{
    long v;

    v = PyInt_AS_LONG(obj);

    if (v >= 0 && v <= 255) {
        return append_tag_and_uint8(state, SMALL_INTEGER_EXT, v);
    }
    else if (v >= INT_MIN && v <= INT_MAX) {
        return append_tag_and_int32(state, INTEGER_EXT, v);
    }
    else {
        PyObject *l;
        int res;

        l = PyLong_FromLong(v);
        if (l == NULL);
            return -1;

        res = encode_long(state, l);
        Py_DECREF(l);
        return res;
    }
}

static int
encode_float(EncoderState *state, PyObject *obj)
{
    char buf[32];
    memset(buf, 0, sizeof(buf));
    buf[0] = FLOAT_EXT;
    PyOS_ascii_formatd(buf + 1, sizeof(buf) - 2,
                       "%.20e", PyFloat_AS_DOUBLE(obj));
    return append_buffer(state, (const unsigned char *)buf, sizeof(buf));
}

static int
encode_string(EncoderState *state, PyObject *obj)
{
    int res;
    Py_ssize_t len = PyString_GET_SIZE(obj);

    res = append_tag_and_uint32(state, BINARY_EXT, len);
    if (res < 0)
        return res;

    Py_INCREF(obj);
    return append_part(state, obj);
}

static int
encode_unicode(EncoderState *state, PyObject *obj)
{
    int res;
    Py_ssize_t i, len;

    if (state->encoding == NULL) {
        Py_UNICODE *p;

        len = PyUnicode_GET_SIZE(obj);

        res = append_tag_and_uint32(state, LIST_EXT, len);
        if (res < 0)
            return res;

        p = PyUnicode_AS_UNICODE(obj);
        for (i = 0; i < len; i++) {
            unsigned char buf[4] = { *p >> 24, *p >> 16, *p >> 8, *p };
            res = append_buffer(state, buf, sizeof(buf));
            if (res < 0)
                return res;
            p++;
        }

        return append_empty_list(state);
    }
    else {
        PyObject *st;

        st = PyUnicode_AsEncodedString(obj, state->encoding, NULL);
        if (st == NULL)
            return -1;

        len = PyString_GET_SIZE(st);

        if (state->unicode_type == UNICODE_TYPE_BINARY) {
            return encode_string(state, st);
        }
        else if (len > 65535) {
            char *p;

            res = append_tag_and_uint32(state, LIST_EXT, len);
            if (res < 0)
                return res;

            p = PyString_AS_STRING(st);
            for (i = 0; i < len; i++) {
                res = append_tag_and_uint8(state, SMALL_INTEGER_EXT, *p++);
                if (res < 0) {
                    Py_DECREF(st);
                    return res;
                }
            }

            Py_DECREF(st);
            return append_empty_list(state);
        }
        else if (len == 0) {
            return append_empty_list(state);
        }
        else {
            res = append_tag_and_uint16(state, STRING_EXT, len);
            if (res < 0)
                return res;

            return append_part(state, st);
        }
    }
}

static int
encode_tuple(EncoderState *state, PyObject *obj)
{
    int res;
    Py_ssize_t i, len = PyTuple_GET_SIZE(obj);

    if (len < 256) {
        res = append_tag_and_uint8(state, SMALL_TUPLE_EXT, len);
    }
    else {
        res = append_tag_and_uint32(state, LARGE_TUPLE_EXT, len);
    }

    if (res < 0)
        return res;

    for (i = 0; i < len; i++) {
        res = encode_obj(state, PyTuple_GET_ITEM(obj, i));
        if (res < 0)
            return res;
    }

    return 0;
}

static int
encode_list(EncoderState *state, PyObject *obj)
{
    Py_ssize_t len = PyList_GET_SIZE(obj);

    if (len == 0) {
        return append_empty_list(state);
    }
    else {
        int res;
        Py_ssize_t i;

        res = append_tag_and_uint32(state, LIST_EXT, len);
        if (res < 0)
            return res;

        for (i = 0; i < len; i++) {
            res = encode_obj(state, PyList_GET_ITEM(obj, i));
            if (res < 0)
                return res;
        }

        return append_empty_list(state);
    }
}

static int
encode_atom(EncoderState *state, PyObject *obj)
{
    int res;
    Py_ssize_t len = PyString_GET_SIZE(obj);

    if (len > 255) {
        PyObject *s;

        s = PyObject_Repr(obj);
        if (s != NULL) {
            PyErr_Format(PyExc_ValueError,
                         "%s is too long (length %zd), maximum length is 255",
                         PyString_AS_STRING(s), len);
            Py_DECREF(s);
        }

        return -1;
    }

    res = append_tag_and_uint16(state, ATOM_EXT, len);
    if (res < 0)
        return res;

    Py_INCREF(obj);
    return append_part(state, obj);
}

static int
encode_reference(EncoderState *state, PyObject *obj)
{
    int res = -1;
    PyObject *node = NULL, *ref_id = NULL, *creation = NULL;
    Py_ssize_t len, i;
    unsigned char buf[13];

    node = PyObject_GetAttrString(obj, "node");
    if (node == NULL)
        return -1;

    ref_id = PyObject_GetAttrString(obj, "ref_id");
    if (ref_id == NULL)
        goto bail;

    creation = PyObject_GetAttrString(obj, "creation");
    if (creation == NULL)
        goto bail;

    len = PyTuple_GET_SIZE(ref_id);
    if (len > 3)
        len = 3;

    res = append_tag_and_uint16(state, NEW_REFERENCE_EXT, len);
    if (res < 0)
        goto bail;

    res = encode_atom(state, node);
    if (res < 0)
        goto bail;

    buf[0] = PyInt_AS_LONG(creation);

    for (i = 0; i < len; i++) {
        PyObject *item;
        Py_ssize_t index;
        long v;

        item = PyTuple_GET_ITEM(ref_id, i);
        v = PyInt_AS_LONG(item);

        index = i * 4 + 1;
        buf[index] = v >> 24;
        buf[index + 1] = v >> 16;
        buf[index + 2] = v >> 8;
        buf[index + 3] = v;
    }

    res = append_buffer(state, buf, len * 4 + 1);

bail:
    Py_XDECREF(creation);
    Py_XDECREF(ref_id);
    Py_XDECREF(node);

    return res;
}

static int
encode_port(EncoderState *state, PyObject *obj)
{
    int res;
    PyObject *node, *id, *creation;
    unsigned char buf[5];
    long v;

    buf[0] = PORT_EXT;
    res = append_buffer(state, buf, 1);
    if (res < 0)
        return res;

    node = PyObject_GetAttrString(obj, "node");
    if (node == NULL)
        return -1;

    res = encode_atom(state, node);
    Py_DECREF(node);
    if (res < 0)
        return res;

    id = PyObject_GetAttrString(obj, "port_id");
    if (id == NULL)
        return -1;

    v = PyInt_AS_LONG(id);
    Py_DECREF(id);

    buf[0] = v >> 24;
    buf[1] = v >> 16;
    buf[2] = v >> 8;
    buf[3] = v;

    creation = PyObject_GetAttrString(obj, "creation");
    if (creation == NULL)
        return -1;

    buf[4] = PyInt_AS_LONG(creation);
    Py_DECREF(creation);

    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_pid(EncoderState *state, PyObject *obj)
{
    int res;
    PyObject *node, *id, *serial, *creation;
    unsigned char buf[9];
    long v;

    buf[0] = PID_EXT;
    res = append_buffer(state, buf, 1);
    if (res < 0)
        return res;

    node = PyObject_GetAttrString(obj, "node");
    if (node == NULL)
        return -1;

    res = encode_atom(state, node);
    Py_DECREF(node);
    if (res < 0)
        return res;

    id = PyObject_GetAttrString(obj, "pid_id");
    if (id == NULL)
        return -1;

    v = PyInt_AS_LONG(id);
    Py_DECREF(id);

    buf[0] = v >> 24;
    buf[1] = v >> 16;
    buf[2] = v >> 8;
    buf[3] = v;

    serial = PyObject_GetAttrString(obj, "serial");
    if (serial == NULL)
        return -1;

    v = PyInt_AS_LONG(serial);
    Py_DECREF(serial);

    buf[4] = v >> 24;
    buf[5] = v >> 16;
    buf[6] = v >> 8;
    buf[7] = v;

    creation = PyObject_GetAttrString(obj, "creation");
    if (creation == NULL)
        return -1;

    buf[8] = PyInt_AS_LONG(creation);
    Py_DECREF(creation);

    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_export(EncoderState *state, PyObject *obj)
{
    int res;
    PyObject *module, *function, *arity;
    unsigned char buf[2];
    long v;

    buf[0] = EXPORT_EXT;
    res = append_buffer(state, buf, 1);
    if (res < 0)
        return res;

    module = PyObject_GetAttrString(obj, "module");
    if (module == NULL)
        return -1;

    res = encode_atom(state, module);
    Py_DECREF(module);
    if (res < 0)
        return res;

    function = PyObject_GetAttrString(obj, "function");
    if (function == NULL)
        return -1;

    res = encode_atom(state, function);
    Py_DECREF(function);
    if (res < 0)
        return res;

    arity = PyObject_GetAttrString(obj, "arity");
    if (arity == NULL)
        return -1;

    v = PyInt_AS_LONG(arity);
    Py_DECREF(arity);

    buf[0] = SMALL_INTEGER_EXT;
    buf[1] = v;

    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_obj(EncoderState *state, PyObject *obj)
{
    PyTypeObject *type;
    PyObject *repr;

    if (obj == Py_None)
        return encode_none(state);

    type = Py_TYPE(obj);

    switch (type->tp_name[0]) {
        case 'b':
            if (obj == Py_False)
                return encode_false(state);
            else if (obj == Py_True)
                return encode_true(state);
            break;

        case 'i':
            if (type == &PyInt_Type)
                return encode_int(state, obj);
            break;

        case 'f':
            if (type == &PyFloat_Type)
                return encode_float(state, obj);
            break;

        case 's':
            if (type == &PyString_Type)
                return encode_string(state, obj);
            break;

        case 'u':
            if (type == &PyUnicode_Type)
                return encode_unicode(state, obj);
            break;

        case 't':
            if (type == &PyTuple_Type)
                return encode_tuple(state, obj);
            break;

        case 'l':
            if (type == &PyList_Type)
                return encode_list(state, obj);
            else if (type == &PyLong_Type)
                return encode_long(state, obj);
            break;

        case 'A':
            if (type == atom_type)
                return encode_atom(state, obj);
            break;

        case 'R':
            if (type == reference_type)
                return encode_reference(state, obj);
            break;

        case 'P':
            if (type == port_type)
                return encode_port(state, obj);
            else if (type == pid_type)
                return encode_pid(state, obj);
            break;

        case 'E':
            if (type == export_type)
                return encode_export(state, obj);
            break;
    }

    if (PyTuple_Check(obj))
        return encode_tuple(state, obj);
    else if (PyList_Check(obj))
        return encode_list(state, obj);
    if (PyObject_IsInstance(obj, (PyObject *)atom_type))
        return encode_atom(state, obj);
    else if (PyString_Check(obj))
        return encode_string(state, obj);
    else if (PyUnicode_Check(obj))
        return encode_unicode(state, obj);
    else if (PyInt_Check(obj))
        return encode_int(state, obj);
    else if (PyLong_Check(obj))
        return encode_long(state, obj);
    else if (PyFloat_Check(obj))
        return encode_float(state, obj);
    else if (PyObject_IsInstance(obj, (PyObject *)reference_type))
        return encode_reference(state, obj);
    else if (PyObject_IsInstance(obj, (PyObject *)port_type))
        return encode_port(state, obj);
    else if (PyObject_IsInstance(obj, (PyObject *)pid_type))
        return encode_pid(state, obj);
    else if (PyObject_IsInstance(obj, (PyObject *)export_type))
        return encode_export(state, obj);

    repr = PyObject_Repr(obj);
    if (repr == NULL)
        return -1;

    PyErr_Format(PyExc_NotImplementedError,
                 "Unable to serialize %s",
                 PyString_AS_STRING(repr));
    Py_DECREF(repr);

    return -1;
}

static PyObject *
build_result(EncoderState *state)
{
    PyObject *res;
    char *p;
    Py_ssize_t parts_len, i;

    res = PyString_FromStringAndSize(NULL, state->len + 1);
    if (res == NULL) {
        Py_DECREF(state->parts);
        return NULL;
    }

    p = PyString_AS_STRING(res);
    *p++ = FORMAT_VERSION;

    parts_len = PyList_GET_SIZE(state->parts);

    for (i = 0; i < parts_len; i++) {
        PyObject *part;
        Py_ssize_t n;

        part = PyList_GET_ITEM(state->parts, i);
        n = PyString_GET_SIZE(part);

        Py_MEMCPY(p, PyString_AS_STRING(part), n);
        p += n;
    }

    Py_DECREF(state->parts);

    return res;
}

static PyObject *
encode(EncoderObject *self, PyObject *obj)
{
    EncoderState state = { 0, };
    char *unicode_type;

    if (!PyString_Check(self->unicode_type) && !PyUnicode_Check(self->unicode_type))
        return PyErr_Format(PyExc_TypeError,
                            "expected string or Unicode object for "
                            "unicode_type, %.200s found",
                            Py_TYPE(self->encoding)->tp_name);

    unicode_type = PyString_AsString(self->unicode_type);

    if (strcmp(unicode_type, "binary") == 0)
        state.unicode_type = UNICODE_TYPE_BINARY;
    else if (strcmp(unicode_type, "str") == 0)
        state.unicode_type = UNICODE_TYPE_STR;
    else
        return PyErr_Format(PyExc_TypeError,
                            "Unknown unicode encoding type %s",
                            unicode_type);

    if (PyString_Check(self->encoding) || PyUnicode_Check(self->encoding)) {
        Py_ssize_t len;

        len = PyString_Size(self->encoding);

        if (len > 0) {
            state.encoding = PyMem_Malloc(len + 1);
            if (state.encoding == NULL) {
                PyErr_SetString(PyExc_MemoryError,
                                "erlastic: can't allocate memory for encoding "
                                "type string");
                return NULL;
            }

            strcpy(state.encoding, PyString_AsString(self->encoding));
        }
        else {
            state.encoding = NULL;
        }
    }
    else if (self->encoding == Py_None) {
        state.encoding = NULL;
    }
    else {
        return PyErr_Format(PyExc_TypeError,
                            "expected string or Unicode object for "
                            "encoding, %.200s found",
                            Py_TYPE(self->encoding)->tp_name);
    }

    state.parts = PyList_New(0);
    if (state.parts == NULL)
        return NULL;

    if (encode_obj(&state, obj) < 0) {
        Py_DECREF(state.parts);
        return NULL;
    }

    PyMem_Free(state.encoding);

    return build_result(&state);
}

static struct PyMethodDef encoder_methods[] = {
    { "encode", (PyCFunction)encode, METH_O,
      PyDoc_STR("encode(string) -- encode an Erlang term")
    },
    {NULL, NULL}
};

static struct PyMemberDef encoder_members[] = {
    {"encoding", T_OBJECT, offsetof(EncoderObject, encoding), 0},
    {"unicode_type", T_OBJECT, offsetof(EncoderObject, unicode_type), 0},
    {NULL}
};

static void
encoder_dealloc(EncoderObject *self)
{
    Py_XDECREF(self->encoding);
    Py_XDECREF(self->unicode_type);
    self->ob_type->tp_free((PyObject*)self);
}

static PyObject *
encoder_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    EncoderObject *self;

    self = (EncoderObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->encoding = PyString_FromString("utf-8");
        if (self->encoding == NULL) {
            Py_DECREF(self);
            return NULL;
        }

        self->unicode_type = PyString_FromString("binary");
        if (self->unicode_type == NULL) {
            Py_DECREF(self);
            return NULL;
        }
    }

    return (PyObject *)self;
}

static int
encoder_init(EncoderObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *o, *encoding = NULL, *unicode_type = NULL;

    static char *kwlist[] = { "encoding", "unicode_type", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "|OO:ErlangTermEncoder", kwlist,
                                     &encoding, &unicode_type))
        return -1;

    if (encoding) {
        o = self->encoding;
        Py_INCREF(encoding);
        self->encoding = encoding;
        Py_XDECREF(o);
    }

    if (unicode_type) {
        o = self->unicode_type;
        Py_INCREF(unicode_type);
        self->unicode_type = unicode_type;
        Py_XDECREF(o);
    }

    return 0;
}

PyDoc_STRVAR(Encoder_Type_doc, "Erlang Binary Term encoder");

PyTypeObject Encoder_Type = {
    PyObject_HEAD_INIT(NULL)
    0,                                  /* ob_size */
    "erlastic.ErlangTermEncoder",       /* tp_name */
    sizeof(EncoderObject),              /* tp_basicsize */
    0,                                  /* tp_itemsize */
    /* methods */
    (destructor)encoder_dealloc,        /* tp_dealloc */
    0,                                  /* tp_print */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_compare */
    0,                                  /* tp_repr */
    0,                                  /* tp_as_number */
    0,                                  /* tp_as_sequence */
    0,                                  /* tp_as_mapping */
    0,                                  /* tp_hash */
    0,                                  /* tp_call */
    0,                                  /* tp_str */
    0,                                  /* tp_getattro */
    0,                                  /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,   /* tp_flags */
    Encoder_Type_doc,                   /* tp_doc */
    0,                                  /* tp_traverse */
    0,                                  /* tp_clear */
    0,                                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    0,                                  /* tp_iter */
    0,                                  /* tp_iternext */
    encoder_methods,                    /* tp_methods */
    encoder_members,                    /* tp_members */
    0,                                  /* tp_getset */
    0,                                  /* tp_base */
    0,                                  /* tp_dict */
    0,                                  /* tp_descr_get */
    0,                                  /* tp_descr_set */
    0,                                  /* tp_dictoffset */
    (initproc)encoder_init,             /* tp_init */
    0,                                  /* tp_alloc */
    encoder_new,                        /* tp_new */
};
