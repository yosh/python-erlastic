#include "erlastic.h"

#include "structmember.h"

#include "eetftags.h"


typedef struct _EncoderState {
    PyObject *parts;
    Py_ssize_t len;
    int compressed;
} EncoderState;

static int encode_obj(EncoderState *state, PyObject *obj);

static PyObject *
get_compress_func(void)
{
    PyObject *zlib;
    PyObject *compress;

    zlib = PyImport_ImportModule("zlib");
    if (zlib != NULL) {
        compress = PyObject_GetAttrString(zlib, "compress");
        Py_DECREF(zlib);
    }
    else {
        PyErr_Clear();
        compress = NULL;
    }

    return compress;
}

static int
append_part(EncoderState *state, PyObject *part)
{
    int res;

    state->len += PyBytes_GET_SIZE(part);

    res = PyList_Append(state->parts, part);
    Py_DECREF(part);

    return res;
}

static int
append_buffer(EncoderState *state, const unsigned char *buf, Py_ssize_t len)
{
    PyObject *b;

    b = PyBytes_FromStringAndSize((const char *)buf, len);
    if (b == NULL)
        return -1;

    return append_part(state, b);
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
    static unsigned char buf[] = { SMALL_ATOM_UTF8_EXT, 4, 'n', 'o', 'n', 'e' };
    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_false(EncoderState *state)
{
    static unsigned char buf[] = { SMALL_ATOM_UTF8_EXT, 5, 'f', 'a', 'l', 's', 'e' };
    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_true(EncoderState *state)
{
    static unsigned char buf[] = { SMALL_ATOM_UTF8_EXT, 4, 't', 'r', 'u', 'e' };
    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_long(EncoderState *state, PyObject *obj)
{
    PyObject *b = NULL;
    int sign, res = -1;
    size_t nbits, nbytes, header_size;
    unsigned char *p, buf[6];

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

    if (nbytes > 0x7fffffffL) {
        PyErr_SetString(PyExc_OverflowError,
                        "erlastic: int too large to encode");
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

    b = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)(header_size + nbytes));
    if (b == NULL)
        goto bail;

    p = (unsigned char *)PyBytes_AS_STRING(b);

    memcpy(p, buf, header_size);
    p += header_size;

    res = _PyLong_AsByteArray((PyLongObject *)obj, p, nbytes,
                              PY_LITTLE_ENDIAN, 0 /* unsigned */);
    if (res < 0)
        goto bail;

    return append_part(state, b);

bail:
    Py_XDECREF(b);
    Py_DECREF(obj);
    return res;
}

static int
encode_int(EncoderState *state, PyObject *obj)
{
    long v;
    int overflow;

    v = PyLong_AsLongAndOverflow(obj, &overflow);
    if (!overflow) {
        if (v >= 0 && v <= 255) {
            return append_tag_and_uint8(state, SMALL_INTEGER_EXT, v);
        }
        else if (v == -1 && PyErr_Occurred()) {
            return -1;
        }
        else if (v >= (-0x7fffffffL - 1) && v <= 0x7fffffffL) {
            return append_tag_and_int32(state, INTEGER_EXT, v);
        }
    }
    else if (v == -1 && PyErr_Occurred()) {
       return -1;
    }

    return encode_long(state, obj);
}

static int
encode_float(EncoderState *state, PyObject *obj)
{
    unsigned char buf[9];
    double v;

    buf[0] = NEW_FLOAT_EXT;

    v = PyFloat_AS_DOUBLE(obj);
    if (_PyFloat_Pack8(v, &buf[1], PY_BIG_ENDIAN) < 0)
        return -1;

    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_string(EncoderState *state, PyObject *obj)
{
    int res;
    PyObject *b;

    b = PyUnicode_AsUTF8String(obj);
    if (b == NULL)
        return -1;

    res = append_tag_and_uint32(state, BINARY_EXT, PyBytes_GET_SIZE(b));
    if (res < 0)
        return res;

    return append_part(state, b);
}

static int
encode_bytes(EncoderState *state, PyObject *obj)
{
    int res;

    res = append_tag_and_uint32(state, BINARY_EXT, PyBytes_GET_SIZE(obj));
    if (res < 0)
        return res;

    Py_INCREF(obj);
    return append_part(state, obj);
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
encode_dict(EncoderState *state, PyObject *obj)
{
    int res;
    PyObject *k, *v;
    Py_ssize_t pos = 0, len = PyDict_GET_SIZE(obj);

    res = append_tag_and_uint32(state, MAP_EXT, len);
    if (res < 0)
        return res;

    while (PyDict_Next(obj, &pos, &k, &v)) {
        res = encode_obj(state, k);
        if (res < 0)
            return res;

        res = encode_obj(state, v);
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
    PyObject *b;
    Py_ssize_t len;

    b = PyUnicode_AsUTF8String(obj);
    if (b == NULL)
        return -1;

    len = PyBytes_GET_SIZE(b);

    if (len < 256)
        res = append_tag_and_uint8(state, SMALL_ATOM_UTF8_EXT, len);
    else
        res = append_tag_and_uint16(state, ATOM_UTF8_EXT, len);

    if (res < 0)
        return res;

    return append_part(state, b);
}

static int
encode_reference(EncoderState *state, PyObject *obj)
{
    int res = -1;
    PyObject *node = NULL, *ref_id = NULL, *creation = NULL;
    Py_ssize_t len, i;
    unsigned char buf[13];
    long v;

    node = PyObject_GetAttrString(obj, "node");
    if (node == NULL)
        return -1;

    ref_id = PyObject_GetAttrString(obj, "ref_id");
    if (ref_id == NULL)
        goto bail;

    creation = PyObject_GetAttrString(obj, "creation");
    if (creation == NULL)
        goto bail;

    if (!PyTuple_Check(ref_id)) {
        PyErr_SetString(PyExc_TypeError, "ref_id must be a tuple");
        goto bail;
    }

    len = PyTuple_GET_SIZE(ref_id);
    if (len > 3) {
        PyErr_SetString(PyExc_TypeError, "ref_id is too big");
        goto bail;
    }

    res = append_tag_and_uint16(state, NEW_REFERENCE_EXT, len);
    if (res < 0)
        goto bail;

    res = encode_atom(state, node);
    if (res < 0)
        goto bail;

    v = PyLong_AsLong(creation);
    if (v == -1 && PyErr_Occurred())
        goto bail;

    buf[0] = v;

    for (i = 0; i < len; i++) {
        PyObject *item;
        Py_ssize_t index;

        item = PyTuple_GET_ITEM(ref_id, i);

        v = PyLong_AsLong(item);
        if (v == -1 && PyErr_Occurred())
            goto bail;

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

    v = PyLong_AsLong(id);
    Py_DECREF(id);

    if (v == -1 && PyErr_Occurred())
        return -1;

    buf[0] = v >> 24;
    buf[1] = v >> 16;
    buf[2] = v >> 8;
    buf[3] = v;

    creation = PyObject_GetAttrString(obj, "creation");
    if (creation == NULL)
        return -1;

    v = PyLong_AsLong(creation);
    Py_DECREF(creation);

    if (v == -1 && PyErr_Occurred())
        return -1;

    buf[4] = v;

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

    v = PyLong_AsLong(id);
    Py_DECREF(id);

    if (v == -1 && PyErr_Occurred())
        return -1;

    buf[0] = v >> 24;
    buf[1] = v >> 16;
    buf[2] = v >> 8;
    buf[3] = v;

    serial = PyObject_GetAttrString(obj, "serial");
    if (serial == NULL)
        return -1;

    v = PyLong_AsLong(serial);
    Py_DECREF(serial);

    if (v == -1 && PyErr_Occurred())
        return -1;

    buf[4] = v >> 24;
    buf[5] = v >> 16;
    buf[6] = v >> 8;
    buf[7] = v;

    creation = PyObject_GetAttrString(obj, "creation");
    if (creation == NULL)
        return -1;

    v = PyLong_AsLong(creation);
    Py_DECREF(creation);

    if (v == -1 && PyErr_Occurred())
        return -1;

    buf[8] = v;

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

    v = PyLong_AsLong(arity);
    Py_DECREF(arity);

    if (v == -1 && PyErr_Occurred())
        return -1;

    buf[0] = SMALL_INTEGER_EXT;
    buf[1] = v;

    return append_buffer(state, buf, sizeof(buf));
}

static int
encode_obj(EncoderState *state, PyObject *obj)
{
    PyTypeObject *type = Py_TYPE(obj);;

    switch (type->tp_name[0]) {
        case 'b':
            if (obj == Py_False)
                return encode_false(state);
            else if (obj == Py_True)
                return encode_true(state);
            else if (type == &PyBytes_Type)
                return encode_bytes(state, obj);
            break;

        case 'N':
            if (obj == Py_None)
                return encode_none(state);
            break;

        case 'i':
            if (type == &PyLong_Type)
                return encode_int(state, obj);
            break;

        case 'f':
            if (type == &PyFloat_Type)
                return encode_float(state, obj);
            break;

        case 's':
            if (type == &PyUnicode_Type)
                return encode_string(state, obj);
            break;

        case 't':
            if (type == &PyTuple_Type)
                return encode_tuple(state, obj);
            break;

        case 'd':
            if (type == &PyDict_Type)
                return encode_dict(state, obj);
            break;

        case 'l':
            if (type == &PyList_Type)
                return encode_list(state, obj);
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
    else if (PyDict_Check(obj))
        return encode_dict(state, obj);
    else if (PyList_Check(obj))
        return encode_list(state, obj);
    else if (PyObject_IsInstance(obj, (PyObject *)atom_type))
        return encode_atom(state, obj);
    else if (PyUnicode_Check(obj))
        return encode_string(state, obj);
    else if (PyBytes_Check(obj))
        return encode_bytes(state, obj);
    else if (PyLong_Check(obj))
        return encode_int(state, obj);
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

    PyErr_Format(PyExc_NotImplementedError, "Unable to serialize %R", obj);
    return -1;
}

static void
concat_parts(PyObject *parts, char *p)
{
    Py_ssize_t parts_len, i;

    parts_len = PyList_GET_SIZE(parts);

    for (i = 0; i < parts_len; i++) {
        PyObject *part;
        Py_ssize_t n;

        part = PyList_GET_ITEM(parts, i);
        n = PyBytes_GET_SIZE(part);

        memcpy(p, PyBytes_AS_STRING(part), n);
        p += n;
    }

    Py_DECREF(parts);
}

static PyObject *
build_result(EncoderState *state)
{
    PyObject *res;
    char *p;

    if (state->compressed > 0) {
       PyObject *compress, *data, *compressed_data;
       Py_ssize_t compressed_len;

       compress = get_compress_func();
       if (compress == NULL) {
           PyErr_SetString(encoding_error,
                           "can't compress data; zlib not available");
           Py_DECREF(state->parts);
           return NULL;
       }

       data = PyBytes_FromStringAndSize(NULL, state->len);
       p = PyBytes_AS_STRING(data);
       concat_parts(state->parts, p);

       compressed_data = PyObject_CallFunction(compress, "Oi",
                                               data, state->compressed);
       if (compressed_data == NULL) {
           Py_DECREF(data);
           return NULL;
       }

       compressed_len = PyBytes_Size(compressed_data);
       if (compressed_len < state->len) {
           Py_DECREF(data);

           res = PyBytes_FromStringAndSize(NULL, compressed_len + 6);
           if (res == NULL) {
               Py_DECREF(compressed_data);
               return NULL;
           }

           p = PyBytes_AS_STRING(res);
           *p++ = FORMAT_VERSION;
           *p++ = COMPRESSED;
           *p++ = (char) (state->len >> 24);
           *p++ = (char) (state->len >> 16);
           *p++ = (char) (state->len >> 8);
           *p++ = (char) state->len;

           memcpy(p, PyBytes_AS_STRING(compressed_data), compressed_len);
           Py_DECREF(compressed_data);
       }
       else {
           Py_DECREF(compressed_data);

           res = PyBytes_FromStringAndSize(NULL, state->len + 1);
           if (res == NULL) {
               Py_DECREF(data);
               return NULL;
           }

           p = PyBytes_AS_STRING(res);
           *p++ = FORMAT_VERSION;

           memcpy(p, PyBytes_AS_STRING(data), state->len);
           Py_DECREF(data);
       }
    }
    else {
        res = PyBytes_FromStringAndSize(NULL, state->len + 1);
        if (res == NULL) {
            Py_DECREF(state->parts);
            return NULL;
        }

        p = PyBytes_AS_STRING(res);
        *p++ = FORMAT_VERSION;
        concat_parts(state->parts, p);
    }

    return res;
}

PyObject *
erlastic_encode(PyObject *self, PyObject *args, PyObject *kwargs)
{
    EncoderState state = { 0, };
    PyObject *obj, *compressed = NULL;

    static char *kwlist[] = { "obj", "compressed", NULL };

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "O|O:encode", kwlist,
                                     &obj, &compressed))
        return NULL;

    if (compressed == NULL || compressed == Py_False)
        state.compressed = 0;
    else if (compressed == Py_True)
        state.compressed = 6;
    else if (PyLong_Check(compressed)) {
        long v;

        v = PyLong_AsLong(compressed);
        if ((v == -1) && PyErr_Occurred())
            return NULL;

        if (v < 0 || v > 9) {
            PyErr_SetString(PyExc_TypeError,
                            "compressed must be True, False or "
                            "an integer between 0 and 9");
            return NULL;
        }
        else
            state.compressed = v;
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "compressed must be True, False or "
                        "an integer between 0 and 9");
        return NULL;
    }

    state.parts = PyList_New(0);
    if (state.parts == NULL)
        return NULL;

    if (encode_obj(&state, obj) < 0) {
        Py_DECREF(state.parts);
        return NULL;
    }

    return build_result(&state);
}
