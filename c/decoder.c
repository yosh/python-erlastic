#include "erlastic.h"

#include "structmember.h"

#include "eetftags.h"


typedef struct _DecoderObject {
    PyObject_HEAD
    PyObject *encoding;
} DecoderObject;

typedef struct _DecoderState {
    char *encoding;
    const unsigned char *bytes;
    Py_ssize_t len;
    Py_ssize_t offset;
} DecoderState;

static PyObject *new_atom(const char *atom_name);
static PyObject *convert_string_to_atom(PyObject *o);

static PyObject *decode_embedded_atom(DecoderState *state, const char *parent_tag_name);
static PyObject *decode_term(DecoderState *state);


#define BYTES_AS_CSTRING ((const char *)(state->bytes + state->offset))

#define CHECK_SHORT_BUFFER(count)                                       \
    do {                                                                \
        if (state->offset + (count) > state->len) {                     \
            PyErr_SetString(decoding_error,                             \
                            "Erlang term data was truncated");          \
            return NULL;                                                \
        }                                                               \
    } while (0)


#ifdef WORDS_BIGENDIAN
static inline unsigned int
read_uint4(DecoderState *state)
{
    unsigned int v;
    Py_MEMCPY(&v, state->bytes + state->offset, 4);
    state->offset += 4;
    return v;
}
#else
static inline unsigned int
read_uint4(DecoderState *state)
{
    unsigned int v = 0, i = 4;
    const unsigned char *p = state->bytes + state->offset;

    do {
        v = (v << 8) | *p++;
    } while (--i > 0);

    state->offset += 4;

    return v;
}
#endif

static PyObject *
new_atom(const char *atom_name)
{
    PyObject *s, *args, *res;

    s = PyString_FromString(atom_name);
    if (s == NULL)
        return NULL;

    args = PyTuple_Pack(1, s);
    Py_DECREF(s);
    if (args == NULL)
        return NULL;

    res = PyObject_CallObject((PyObject *)atom_type, args);
    Py_DECREF(args);

    return res;
}

static PyObject *
convert_string_to_atom(PyObject *o)
{
    char *s;

    s = PyString_AS_STRING(o);

    if (strcmp(s, "none") == 0)
        Py_RETURN_NONE;
    else if (strcmp(s, "true") == 0)
        Py_RETURN_TRUE;
    else if (strcmp(s, "false") == 0)
        Py_RETURN_FALSE;
    else
        return new_atom(s);
}

static PyObject *
decode_bigint(DecoderState *state, unsigned int n)
{
    unsigned int sign;
    const unsigned char *p;
    PyObject *res, *neg_res, *possible_int;

    CHECK_SHORT_BUFFER(1 + n);

    sign = state->bytes[state->offset++];

    p = state->bytes + state->offset;
    state->offset += n;

    res = _PyLong_FromByteArray(p, n, 1 /* little endian */, 0 /* signed */);
    if (res == NULL)
        return NULL;

    if (sign) {
        neg_res = PyNumber_Negative(res);
        if (neg_res == NULL)
            return NULL;

        possible_int = PyNumber_Int(neg_res);
        Py_DECREF(neg_res);
    }
    else {
        possible_int = PyNumber_Int(res);
    }

    Py_DECREF(res);
    return possible_int;
}


static PyObject *
decode_small_int(DecoderState *state)
{
    CHECK_SHORT_BUFFER(1);
    return PyInt_FromLong(state->bytes[state->offset++]);
}

static PyObject *
decode_int(DecoderState *state)
{
    signed int v = 0, i = 4;
    const unsigned char *p = state->bytes + state->offset;

    CHECK_SHORT_BUFFER(4);

    do {
        v = (v << 8) | *p++;
    } while (--i > 0);

    state->offset += 4;
    return PyInt_FromLong(v);
}

static PyObject *
decode_float(DecoderState *state)
{
    PyObject *s, *res;
    char buf[32];

    CHECK_SHORT_BUFFER(31);

    memset(buf, 0, sizeof(buf));
    strncpy(buf, BYTES_AS_CSTRING, sizeof(buf) - 1);

    s = PyString_FromString(buf);
    if (s == NULL)
        return NULL;

    res = PyFloat_FromString(s, NULL);
    Py_DECREF(s);

    state->offset += 31;
    return res;
}

static PyObject *
decode_new_float(DecoderState *state)
{
    double v;

    CHECK_SHORT_BUFFER(8);

    v = _PyFloat_Unpack8(state->bytes + state->offset, 0);
    if (v == -1.0 && PyErr_Occurred())
        return NULL;
    return PyFloat_FromDouble(v);
}

static PyObject *
decode_atom(DecoderState *state)
{
    unsigned int len;
    PyObject *s, *res;

    CHECK_SHORT_BUFFER(2);
    len = (state->bytes[state->offset] << 8) + state->bytes[state->offset + 1];
    state->offset += 2;

    CHECK_SHORT_BUFFER(len);
    s = PyString_FromStringAndSize(BYTES_AS_CSTRING, len);
    if (s == NULL)
        return NULL;

    res = convert_string_to_atom(s);
    Py_DECREF(s);

    state->offset += len;
    return res;
}

static PyObject *
decode_small_atom(DecoderState *state)
{
    unsigned int len;
    PyObject *s, *res;

    CHECK_SHORT_BUFFER(1);
    len = state->bytes[state->offset++];

    CHECK_SHORT_BUFFER(len);
    s = PyString_FromStringAndSize(BYTES_AS_CSTRING, len);
    if (s == NULL)
        return NULL;

    res = convert_string_to_atom(s);
    Py_DECREF(s);

    state->offset += len;
    return res;
}

static PyObject *
decode_reference(DecoderState *state)
{
    PyObject *node, *args, *res;
    unsigned int id;
    unsigned char creation;

    node = decode_embedded_atom(state, "REFERENCE_EXT");
    if (node == NULL)
        return NULL;

    CHECK_SHORT_BUFFER(5);
    id = read_uint4(state);
    creation = state->bytes[state->offset++];

    args = Py_BuildValue("(O(I)B)", node, id, creation);
    Py_DECREF(node);
    if (args == NULL)
        return NULL;

    res = PyObject_CallObject((PyObject *)reference_type, args);
    Py_DECREF(args);

    return res;
}

static PyObject *
decode_new_reference(DecoderState *state)
{
    PyObject *node, *t, *id, *args, *res;
    unsigned int len, i;
    unsigned char creation;

    CHECK_SHORT_BUFFER(2);
    len = (state->bytes[state->offset] << 8) + state->bytes[state->offset + 1];
    state->offset += 2;

    node = decode_embedded_atom(state, "NEW_REFERENCE_EXT");
    if (node == NULL)
        return NULL;

    CHECK_SHORT_BUFFER(1 + len * 4);

    t = PyTuple_New(len);
    if (t == NULL) {
        Py_DECREF(node);
        return NULL;
    }

    creation = state->bytes[state->offset++];

    for (i = 0; i < len; i++) {
        id = PyInt_FromLong(read_uint4(state));
        if (id != NULL) {
            PyTuple_SET_ITEM(t, i, id);
        }
        else {
            Py_DECREF(t);
            Py_DECREF(node);
            return NULL;
        }
    }

    args = Py_BuildValue("(OOB)", node, t, creation);
    Py_DECREF(node);
    Py_DECREF(t);
    if (args == NULL)
        return NULL;

    res = PyObject_CallObject((PyObject *)reference_type, args);
    Py_DECREF(args);

    return res;
}

static PyObject *
decode_port(DecoderState *state)
{
    PyObject *node, *args, *res;
    unsigned int id;
    unsigned char creation;

    node = decode_embedded_atom(state, "PORT_EXT");
    if (node == NULL)
        return NULL;

    CHECK_SHORT_BUFFER(5);
    id = read_uint4(state);
    creation = state->bytes[state->offset++];

    args = Py_BuildValue("(OIB)", node, id, creation);
    Py_DECREF(node);
    if (args == NULL)
        return NULL;

    res = PyObject_CallObject((PyObject *)port_type, args);
    Py_DECREF(args);

    return res;
}

static PyObject *
decode_pid(DecoderState *state)
{
    PyObject *node, *args, *res;
    unsigned int id, serial;
    unsigned char creation;

    node = decode_embedded_atom(state, "PID_EXT");
    if (node == NULL)
        return NULL;

    CHECK_SHORT_BUFFER(9);
    id = read_uint4(state);
    serial = read_uint4(state);
    creation = state->bytes[state->offset++];

    args = Py_BuildValue("(OIIB)", node, id, serial, creation);
    Py_DECREF(node);
    if (args == NULL)
        return NULL;

    res = PyObject_CallObject((PyObject *)pid_type, args);
    Py_DECREF(args);

    return res;
}

static PyObject *
decode_export(DecoderState *state)
{
    PyObject *module, *function, *args, *res;
    unsigned char tag, arity;

    module = decode_embedded_atom(state, "EXPORT_EXT");
    if (module == NULL)
        return NULL;

    function = decode_embedded_atom(state, "EXPORT_EXT");
    if (function == NULL) {
        Py_DECREF(module);
        return NULL;
    }

    CHECK_SHORT_BUFFER(2);

    tag = state->bytes[state->offset++];
    arity = state->bytes[state->offset++];

    if (tag == SMALL_INTEGER_EXT) {
        args = Py_BuildValue("(OOB)", module, function, arity);
    }
    else {
        PyErr_Format(decoding_error,
                     "Expected small integer while parsing EXPORT_EXT, "
                     "found '%c' tag instead",
                     tag);
        args = NULL;
    }

    Py_DECREF(function);
    Py_DECREF(module);

    if (args == NULL)
        return NULL;

    res = PyObject_CallObject((PyObject *)export_type, args);
    Py_DECREF(args);

    return res;
}

static PyObject *
decode_small_tuple(DecoderState *state)
{
    unsigned char i, arity;
    PyObject *t;

    CHECK_SHORT_BUFFER(1);
    arity = state->bytes[state->offset++];

    t = PyTuple_New(arity);
    if (t == NULL)
        return NULL;

    for (i = 0; i < arity; i++) {
        PyObject *item;

        item = decode_term(state);
        if (item == NULL) {
            Py_DECREF(t);
            return NULL;
        }

        PyTuple_SET_ITEM(t, i, item);
    }

    return t;
}

static PyObject *
decode_large_tuple(DecoderState *state)
{
    unsigned int arity, i;
    PyObject *t;

    CHECK_SHORT_BUFFER(4);
    arity = read_uint4(state);

    t = PyTuple_New(arity);
    if (t == NULL)
        return NULL;

    for (i = 0; i < arity; i++) {
        PyObject *item;

        item = decode_term(state);
        if (item == NULL) {
            Py_DECREF(t);
            return NULL;
        }

        PyTuple_SET_ITEM(t, i, item);
    }

    return t;
}

static PyObject *
decode_nil(DecoderState *state)
{
    return PyList_New(0);
}

static PyObject *
decode_string(DecoderState *state)
{
    unsigned int len, i;
    PyObject *l;
    const unsigned char *p;

    CHECK_SHORT_BUFFER(2);
    len = (state->bytes[state->offset] << 8) + state->bytes[state->offset + 1];
    state->offset += 2;

    CHECK_SHORT_BUFFER(len);

    if (state->encoding) {
        PyObject *s, *d;

        s = PyString_FromStringAndSize(BYTES_AS_CSTRING, len);
        if (s == NULL)
            return NULL;

        d = PyString_AsDecodedObject(s, state->encoding, NULL);
        if (d == NULL) {
            if (PyErr_ExceptionMatches(PyExc_UnicodeError)) {
                goto generate_list;
            }
        }
        else if (!PyString_Check(d) && !PyUnicode_Check(d)) {
            PyErr_Format(PyExc_TypeError,
                         "decoder did not return a string/unicode object "
                         "(type=%.400s)",
                         Py_TYPE(d)->tp_name);
            Py_DECREF(d);
            d = NULL;
        }

        Py_DECREF(s);

        state->offset += len;
        return d;
    }

generate_list:
    l = PyList_New(len);
    if (l == NULL)
        return NULL;

    p = state->bytes + state->offset;

    for (i = 0; i < len; i++) {
        PyObject *item;

        item = PyInt_FromLong(*p++);
        if (item == NULL) {
            Py_DECREF(l);
            return NULL;
        }

        PyList_SET_ITEM(l, i, item);
    }

    state->offset += len;
    return l;
}

static PyObject *
decode_list(DecoderState *state)
{
    unsigned int len, i;
    PyObject *l, *tail;

    CHECK_SHORT_BUFFER(4);
    len = read_uint4(state);

    l = PyList_New(len);
    if (l == NULL)
        return NULL;

    for (i = 0; i < len; i++) {
        PyObject *item;

        item = decode_term(state);
        if (item == NULL) {
            Py_DECREF(l);
            return NULL;
        }

        PyList_SET_ITEM(l, i, item);
    }

    tail = decode_term(state);
    if (tail == NULL) {
        Py_DECREF(l);
        return NULL;
    }

    if (!PyList_CheckExact(tail) || PyList_GET_SIZE(tail) != 0) {
        /* TODO: Not sure what to do with the tail */
        PyErr_Format(PyExc_NotImplementedError,
                     "Lists with non empty tails are not supported");
        Py_DECREF(l);
        l = NULL;
    }

    Py_DECREF(tail);
    return l;
}

static PyObject *
decode_binary(DecoderState *state)
{
    unsigned int len;
    const unsigned char *p;

    CHECK_SHORT_BUFFER(4);
    len = read_uint4(state);

    CHECK_SHORT_BUFFER(len);
    p = state->bytes + state->offset;
    state->offset += len;

    return PyString_FromStringAndSize((const char *)p, len);
}

static PyObject *
decode_small_big(DecoderState *state)
{
    unsigned char n;

    CHECK_SHORT_BUFFER(1);
    n = state->bytes[state->offset++];

    return decode_bigint(state, n);
}

static PyObject *
decode_large_big(DecoderState *state)
{
    unsigned int n;

    CHECK_SHORT_BUFFER(4);
    n = read_uint4(state);

    return decode_bigint(state, n);
}


static PyObject *
decode_embedded_atom(DecoderState *state, const char *parent_tag_name)
{
    unsigned char tag;

    CHECK_SHORT_BUFFER(1);
    tag = state->bytes[state->offset++];

    switch (tag) {
        /*
        case ATOM_CACHE_REF:
            return decode_atom_cache_ref(state);
        */
        case ATOM_EXT:
            return decode_atom(state);
        case SMALL_ATOM_EXT:
            return decode_small_atom(state);
        default:
            return PyErr_Format(decoding_error,
                                "Expected atom while parsing %s, "
                                "found '%c' tag instead",
                                parent_tag_name, tag);
    }
}

static PyObject *
decode_term(DecoderState *state)
{
    unsigned char tag;

    CHECK_SHORT_BUFFER(1);
    tag = state->bytes[state->offset++];

    switch (tag) {
        /*
        case ATOM_CACHE_REF:
            return decode_atom_cache_ref(state);
        */
        case SMALL_INTEGER_EXT:
            return decode_small_int(state);
        case INTEGER_EXT:
            return decode_int(state);
        case FLOAT_EXT:
            return decode_float(state);
        case ATOM_EXT:
            return decode_atom(state);
        case REFERENCE_EXT:
            return decode_reference(state);
        case PORT_EXT:
            return decode_port(state);
        case PID_EXT:
            return decode_pid(state);
        case SMALL_TUPLE_EXT:
            return decode_small_tuple(state);
        case LARGE_TUPLE_EXT:
            return decode_large_tuple(state);
        case NIL_EXT:
            return decode_nil(state);
        case STRING_EXT:
            return decode_string(state);
        case LIST_EXT:
            return decode_list(state);
        case BINARY_EXT:
            return decode_binary(state);
        case SMALL_BIG_EXT:
            return decode_small_big(state);
        case LARGE_BIG_EXT:
            return decode_large_big(state);
        case NEW_REFERENCE_EXT:
            return decode_new_reference(state);
        case SMALL_ATOM_EXT:
            return decode_small_atom(state);
        /*
        case FUN_EXT:
            return decode_fun(state);
        case NEW_FUN_EXT:
            return decode_new_fun(state);
        */
        case EXPORT_EXT:
            return decode_export(state);
        /*
        case BIT_BINARY_EXT:
            return decode_bit_binary(state);
        */
        case NEW_FLOAT_EXT:
            return decode_new_float(state);
        default:
            return PyErr_Format(decoding_error, "Unsupported tag %d", tag);
    }
}

static PyObject *
decode(DecoderObject *self, PyObject *args, PyObject *kwargs)
{
    unsigned char version;
    DecoderState state = { 0, };
    PyObject *res;

    static char *kwlist[] = { "bytes", "offset", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "t#|n:decode", kwlist,
                                     &state.bytes, &state.len, &state.offset))
        return NULL;

    if (state.offset >= state.len)
        return PyErr_Format(decoding_error,
                            "Erlang term data was truncated");

    version = state.bytes[state.offset];
    if (version != FORMAT_VERSION) {
        PyErr_Format(decoding_error,
                     "Bad version number. Expected %d found %d",
                     FORMAT_VERSION, version);
        return NULL;
    }

    if (self->encoding != Py_None) {
        if (!PyString_Check(self->encoding) && !PyUnicode_Check(self->encoding))
            return PyErr_Format(PyExc_TypeError,
                                "expected string or Unicode object for "
                                "encoding, %.200s found",
                                Py_TYPE(self->encoding)->tp_name);

        state.encoding = PyMem_Malloc(PyString_Size(self->encoding) + 1);
        if (state.encoding == NULL) {
            PyErr_SetString(PyExc_MemoryError,
                            "erlastic: can't allocate memory for encoding "
                            "type string");
            return NULL;
        }

        strcpy(state.encoding, PyString_AsString(self->encoding));
    }

    state.offset++;
    res = decode_term(&state);

    PyMem_Free(state.encoding);
    return res;
}

static struct PyMethodDef decoder_methods[] = {
    { "decode", (PyCFunction)decode, METH_VARARGS | METH_KEYWORDS,
      PyDoc_STR("decode(string) -- decode an Erlang term")
    },
    {NULL, NULL}
};

static struct PyMemberDef decoder_members[] = {
    {"encoding", T_OBJECT, offsetof(DecoderObject, encoding), 0},
    {NULL}
};

static void
decoder_dealloc(DecoderObject *self)
{
    Py_XDECREF(self->encoding);
    self->ob_type->tp_free((PyObject*)self);
}

static PyObject *
decoder_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    DecoderObject *self;

    self = (DecoderObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        Py_INCREF(Py_None);
        self->encoding = Py_None;
    }

    return (PyObject *)self;
}

static int
decoder_init(DecoderObject *self, PyObject *args, PyObject *kwargs)
{
    PyObject *o, *encoding = Py_None;

    static char *kwlist[] = { "encoding", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "|O:ErlangTermDecoder", kwlist,
                                     &encoding))
        return -1;

    o = self->encoding;
    Py_INCREF(encoding);
    self->encoding = encoding;
    Py_XDECREF(o);

    return 0;
}

PyDoc_STRVAR(Decoder_Type_doc, "Erlang Binary Term decoder");

PyTypeObject Decoder_Type = {
    PyObject_HEAD_INIT(NULL)
    0,                                  /* ob_size */
    "erlastic.ErlangTermDecoder",       /* tp_name */
    sizeof(DecoderObject),              /* tp_basicsize */
    0,                                  /* tp_itemsize */
    /* methods */
    (destructor)decoder_dealloc,        /* tp_dealloc */
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
    Decoder_Type_doc,                   /* tp_doc */
    0,                                  /* tp_traverse */
    0,                                  /* tp_clear */
    0,                                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    0,                                  /* tp_iter */
    0,                                  /* tp_iternext */
    decoder_methods,                    /* tp_methods */
    decoder_members,                    /* tp_members */
    0,                                  /* tp_getset */
    0,                                  /* tp_base */
    0,                                  /* tp_dict */
    0,                                  /* tp_descr_get */
    0,                                  /* tp_descr_set */
    0,                                  /* tp_dictoffset */
    (initproc)decoder_init,             /* tp_init */
    0,                                  /* tp_alloc */
    decoder_new,                        /* tp_new */
};
