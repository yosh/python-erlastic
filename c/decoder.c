#include "erlastic.h"

#include "structmember.h"

#include "eetftags.h"


typedef struct _DecoderState {
    const unsigned char *buf;
    Py_ssize_t len;
    Py_ssize_t offset;
} DecoderState;

typedef PyObject *(*UnicodeDecode)(const char *s, Py_ssize_t size, const char *errors);

static PyObject *decode_term(DecoderState *state);


#define BYTES_AS_CSTRING ((const char *)(state->buf + state->offset))

#define CHECK_SHORT_BUFFER(count)                                       \
    do {                                                                \
        if (state->offset + (count) > state->len) {                     \
            PyErr_SetString(encoding_error,                             \
                            "Erlang term data was truncated");          \
            return NULL;                                                \
        }                                                               \
    } while (0)


#ifdef WORDS_BIGENDIAN
static inline unsigned int
read_uint4(DecoderState *state)
{
    unsigned int v;
    memcpy(&v, state->buf + state->offset, 4);
    state->offset += 4;
    return v;
}
#else
static inline unsigned int
read_uint4(DecoderState *state)
{
    unsigned int v = 0, i = 4;
    const unsigned char *p = state->buf + state->offset;

    do {
        v = (v << 8) | *p++;
    } while (--i > 0);

    state->offset += 4;

    return v;
}
#endif

static PyObject *
get_decompress_func(void)
{
    PyObject *zlib;
    PyObject *decompress;

    zlib = PyImport_ImportModule("zlib");
    if (zlib != NULL) {
        decompress = PyObject_GetAttrString(zlib, "decompress");
        Py_DECREF(zlib);
    }
    else {
        PyErr_Clear();
        decompress = NULL;
    }

    return decompress;
}

static PyObject *
translate_atom(DecoderState *state, Py_ssize_t len, UnicodeDecode unicode_decode)
{
    PyObject *s, *res;

    CHECK_SHORT_BUFFER(len);

    if (len == 4) {
        if (memcmp(BYTES_AS_CSTRING, "none", 4) == 0) {
            Py_RETURN_NONE;
        }
        else if (memcmp(BYTES_AS_CSTRING, "true", 4) == 0) {
            Py_RETURN_TRUE;
        }
    }
    else if (len == 5) {
        if (memcmp(BYTES_AS_CSTRING, "false", 5) == 0) {
            Py_RETURN_FALSE;
        }
    }

    s = unicode_decode(BYTES_AS_CSTRING, len, "strict");
    if (s == NULL)
        return NULL;

    res = PyObject_CallFunctionObjArgs((PyObject *)atom_type, s, NULL);
    Py_DECREF(s);

    state->offset += len;

    return res;
}

static PyObject *
decode_atom(DecoderState *state, UnicodeDecode unicode_decode)
{
    unsigned int len;

    CHECK_SHORT_BUFFER(2);
    len = (state->buf[state->offset] << 8) + state->buf[state->offset + 1];
    state->offset += 2;

    return translate_atom(state, len, unicode_decode);
}

static PyObject *
decode_small_atom(DecoderState *state, UnicodeDecode unicode_decode)
{
    unsigned int len;

    CHECK_SHORT_BUFFER(1);
    len = state->buf[state->offset++];

    return translate_atom(state, len, unicode_decode);
}

static PyObject *
decode_bigint(DecoderState *state, unsigned int n)
{
    unsigned int sign;
    const unsigned char *p;
    PyObject *res, *neg_res;

    CHECK_SHORT_BUFFER(1 + n);

    sign = state->buf[state->offset++];

    p = state->buf + state->offset;
    state->offset += n;

    res = _PyLong_FromByteArray(p, n, PY_LITTLE_ENDIAN, 0 /* unsigned */);
    if (res == NULL)
        return NULL;

    if (sign) {
        neg_res = PyNumber_Negative(res);
        Py_DECREF(res);
        res = neg_res;
    }

    return res;
}

static PyObject *
decode_small_int(DecoderState *state)
{
    CHECK_SHORT_BUFFER(1);
    return PyLong_FromLong(state->buf[state->offset++]);
}

static PyObject *
decode_int(DecoderState *state)
{
    signed int v = 0, i = 4;
    const unsigned char *p = state->buf + state->offset;

    CHECK_SHORT_BUFFER(4);

    do {
        v = (v << 8) | *p++;
    } while (--i > 0);

    state->offset += 4;
    return PyLong_FromLong(v);
}

static PyObject *
decode_float(DecoderState *state)
{
    PyObject *s, *res;
    char buf[32];

    CHECK_SHORT_BUFFER(31);

    memset(buf, 0, sizeof(buf));
    strncpy(buf, BYTES_AS_CSTRING, sizeof(buf) - 1);

    s = PyBytes_FromString(buf);
    if (s == NULL)
        return NULL;

    res = PyFloat_FromString(s);
    Py_DECREF(s);

    state->offset += 31;
    return res;
}

static PyObject *
decode_new_float(DecoderState *state)
{
    double v;

    CHECK_SHORT_BUFFER(8);

    v = _PyFloat_Unpack8(state->buf + state->offset, 0 /* big endian */ );
    if (v == -1.0 && PyErr_Occurred())
        return NULL;

    state->offset += 8;
    return PyFloat_FromDouble(v);
}

static PyObject *
decode_atom_latin1(DecoderState *state)
{
    return decode_atom(state, PyUnicode_DecodeLatin1);
}

static PyObject *
decode_small_atom_latin1(DecoderState *state)
{
    return decode_small_atom(state, PyUnicode_DecodeLatin1);
}

static PyObject *
decode_atom_utf8(DecoderState *state)
{
    return decode_atom(state, PyUnicode_DecodeUTF8);
}

static PyObject *
decode_small_atom_utf8(DecoderState *state)
{
    return decode_small_atom(state, PyUnicode_DecodeUTF8);
}

static PyObject *
decode_embedded_atom(DecoderState *state, const char *parent_tag_name)
{
    unsigned char tag;

    CHECK_SHORT_BUFFER(1);
    tag = state->buf[state->offset++];

    switch (tag) {
        /*
        case ATOM_CACHE_REF:
            return decode_atom_cache_ref(state);
        */
        case ATOM_EXT:
            return decode_atom_latin1(state);
        case SMALL_ATOM_EXT:
            return decode_small_atom_latin1(state);
        case ATOM_UTF8_EXT:
            return decode_atom_utf8(state);
        case SMALL_ATOM_UTF8_EXT:
            return decode_small_atom_utf8(state);
        default:
            return PyErr_Format(encoding_error,
                                "Expected atom while parsing %s, "
                                "found '%c' tag instead",
                                parent_tag_name, tag);
    }
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
    creation = state->buf[state->offset++];

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
    len = (state->buf[state->offset] << 8) + state->buf[state->offset + 1];
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

    creation = state->buf[state->offset++];

    for (i = 0; i < len; i++) {
        id = PyLong_FromLong(read_uint4(state));
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
    creation = state->buf[state->offset++];

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
    creation = state->buf[state->offset++];

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

    tag = state->buf[state->offset++];
    arity = state->buf[state->offset++];

    if (tag == SMALL_INTEGER_EXT) {
        args = Py_BuildValue("(OOB)", module, function, arity);
    }
    else {
        PyErr_Format(encoding_error,
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
    arity = state->buf[state->offset++];

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
decode_map(DecoderState *state)
{
    unsigned int arity, i;
    PyObject *d;

    CHECK_SHORT_BUFFER(4);
    arity = read_uint4(state);

    d = PyDict_New();
    if (d == NULL)
        return NULL;

    for (i = 0; i < arity; i++) {
        PyObject *k, *v;

        k = decode_term(state);
        if (k == NULL) {
            Py_DECREF(d);
            return NULL;
        }

        v = decode_term(state);
        if (v == NULL) {
            Py_DECREF(k);
            Py_DECREF(d);
            return NULL;
        }

        if (PyDict_SetItem(d, k, v) != 0) {
            Py_DECREF(k);
            Py_DECREF(v);
            Py_DECREF(d);
            return NULL;
        }
    }

    return d;
}

static PyObject *
decode_nil(DecoderState *state)
{
    return PyList_New(0);
}

static PyObject *
decode_string(DecoderState *state)
{
    unsigned int len;
    const unsigned char *p;

    CHECK_SHORT_BUFFER(2);
    len = (state->buf[state->offset] << 8) + state->buf[state->offset + 1];
    state->offset += 2;

    CHECK_SHORT_BUFFER(len);
    p = state->buf + state->offset;
    state->offset += len;

    return PyBytes_FromStringAndSize((const char *)p, len);
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
    p = state->buf + state->offset;
    state->offset += len;

    return PyBytes_FromStringAndSize((const char *)p, len);
}

static PyObject *
decode_small_big(DecoderState *state)
{
    unsigned char n;

    CHECK_SHORT_BUFFER(1);
    n = state->buf[state->offset++];

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
decode_term(DecoderState *state)
{
    unsigned char tag;

    CHECK_SHORT_BUFFER(1);
    tag = state->buf[state->offset++];

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
            return decode_atom_latin1(state);
        case SMALL_ATOM_EXT:
            return decode_small_atom_latin1(state);
        case ATOM_UTF8_EXT:
            return decode_atom_utf8(state);
        case SMALL_ATOM_UTF8_EXT:
            return decode_small_atom_utf8(state);
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
        case MAP_EXT:
            return decode_map(state);
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
            return PyErr_Format(encoding_error, "Unsupported tag %d", tag);
    }
}

static PyObject *
decode_compressed(DecoderState *state)
{
    PyObject *decompress, *compressed_data, *data, *res;
    unsigned int expected_len;
    Py_ssize_t len;
    DecoderState uncompressed_state;

    CHECK_SHORT_BUFFER(5);
    expected_len = read_uint4(state);

    compressed_data = PyBytes_FromStringAndSize(BYTES_AS_CSTRING,
                                                state->len - state->offset);
    if (compressed_data == NULL)
        return NULL;

    decompress = get_decompress_func();
    if (decompress == NULL) {
        Py_DECREF(compressed_data);
        PyErr_SetString(encoding_error,
                        "can't decompress data; zlib not available");
        return NULL;
    }

    data = PyObject_CallFunctionObjArgs(decompress, compressed_data, NULL);
    Py_DECREF(decompress);
    Py_DECREF(compressed_data);
    if (data == NULL)
        return NULL;

    len = PyBytes_Size(data);
    if (len != expected_len) {
        PyErr_SetString(encoding_error,
                        "uncompressed data length does not match "
                        "expected length");
        Py_DECREF(data);
        return NULL;
    }

    uncompressed_state.buf = (const unsigned char *) PyBytes_AS_STRING(data);
    uncompressed_state.len = len;
    uncompressed_state.offset = 0;

    res = decode_term(&uncompressed_state);

    Py_DECREF(data);
    return res;
}

PyObject *
erlastic_decode(PyObject *self, PyObject *args, PyObject *kwargs)
{
    unsigned char version;
    DecoderState state = { 0, };
    Py_buffer data;
    PyObject *res;

    static char *kwlist[] = { "buf", "offset", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "y*|n:decode", kwlist,
                                     &data, &state.offset))
        return NULL;

    state.buf = data.buf;
    state.len = data.len;

    if (state.offset + 1 >= state.len) {
        PyErr_SetString(encoding_error, "Erlang term data was truncated");
        res = NULL;
        goto cleanup;
    }

    version = state.buf[state.offset];
    if (version != FORMAT_VERSION) {
        PyErr_Format(encoding_error,
                     "Bad version number. Expected %d found %d",
                     FORMAT_VERSION, version);
        res = NULL;
        goto cleanup;
    }

    state.offset++;

    if (state.buf[state.offset] == COMPRESSED) {
        state.offset++;
        res = decode_compressed(&state);
    }
    else {
        res = decode_term(&state);
    }

  cleanup:
    PyBuffer_Release(&data);
    return res;
}
