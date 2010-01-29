#include "erlastic.h"

PyTypeObject *atom_type, *reference_type, *port_type, *pid_type, *export_type;
PyObject *decoding_error, *encoding_error;


#define GET_TYPE_OBJECT(type_var, type_name)                            \
    do {                                                                \
        PyObject *o = PyObject_GetAttrString(m, type_name);             \
        if (o == NULL)                                                  \
            return -1;                                                  \
        else if (!PyType_Check(o))                                      \
            return PyErr_SetString(PyExc_TypeError,                     \
                                   type_name " in erlastic.types "      \
                                   "module isn't a class!"),            \
                   -1;                                                  \
        else                                                            \
            type_var = (PyTypeObject *)o;                               \
    } while (0)

static int
init_types(void)
{
    PyObject *m;

    m = PyImport_ImportModule("erlastic.types");
    if (m == NULL)
        return -1;

    GET_TYPE_OBJECT(atom_type, "Atom");
    GET_TYPE_OBJECT(reference_type, "Reference");
    GET_TYPE_OBJECT(port_type, "Port");
    GET_TYPE_OBJECT(pid_type, "PID");
    GET_TYPE_OBJECT(export_type, "Export");

    Py_DECREF(m);
    return 0;
}


PyDoc_STRVAR(erlastic_module_doc,
"C implementation of Erlang External Term Format serializer/deserializer.");

PyMODINIT_FUNC
init_erlastic(void)
{
    PyObject *m;

    if (PyType_Ready(&Decoder_Type) < 0)
        return;

    if (PyType_Ready(&Encoder_Type) < 0)
        return;

    m = Py_InitModule3("_erlastic", NULL, erlastic_module_doc);
    if (m == NULL)
        return;

    if (init_types() < 0)
        return;

    decoding_error = PyErr_NewException("_erlastic.DecodingError", NULL, NULL);
    if (decoding_error == NULL)
        return;
    PyModule_AddObject(m, "DecodingError", decoding_error);

    encoding_error = PyErr_NewException("_erlastic.EncodingError", NULL, NULL);
    if (encoding_error == NULL)
        return;
    PyModule_AddObject(m, "EncodingError", encoding_error);

    Py_INCREF(&Decoder_Type);
    PyModule_AddObject(m, "ErlangTermDecoder", (PyObject *)&Decoder_Type);

    Py_INCREF(&Encoder_Type);
    PyModule_AddObject(m, "ErlangTermEncoder", (PyObject *)&Encoder_Type);
}
