#include "erlastic.h"

PyTypeObject *atom_type, *reference_type, *port_type, *pid_type, *export_type;

PyObject *encoding_error;


#define GET_TYPE_OBJECT(type_var, type_name, cast_type)                 \
    do {                                                                \
        PyObject *o = PyObject_GetAttrString(m, type_name);             \
        if (o == NULL) {                                                \
            Py_DECREF(m);                                               \
            return -1;                                                  \
        }                                                               \
        else if (!PyType_Check(o)) {                                    \
            Py_DECREF(m);                                               \
            PyErr_SetString(PyExc_TypeError,                            \
                            type_name " in erlastic.types "             \
                            "module isn't a class!");                   \
            return -1;                                                  \
        }                                                               \
        else {                                                          \
            type_var = (cast_type *)o;                                  \
        }                                                               \
    } while (0)

static int
init_types(void)
{
    PyObject *m;

    m = PyImport_ImportModule("erlastic.types");
    if (m == NULL)
        return -1;

    GET_TYPE_OBJECT(atom_type, "Atom", PyTypeObject);
    GET_TYPE_OBJECT(reference_type, "Reference", PyTypeObject);
    GET_TYPE_OBJECT(port_type, "Port", PyTypeObject);
    GET_TYPE_OBJECT(pid_type, "PID", PyTypeObject);
    GET_TYPE_OBJECT(export_type, "Export", PyTypeObject);

    GET_TYPE_OBJECT(encoding_error, "EncodingError", PyObject);

    Py_DECREF(m);
    return 0;
}


static PyMethodDef erlastic_methods[] = {
    {"encode", (PyCFunction)erlastic_encode, METH_VARARGS | METH_KEYWORDS, NULL},
    {"decode", (PyCFunction)erlastic_decode, METH_VARARGS | METH_KEYWORDS, NULL},
    {NULL, NULL}
};

PyDoc_STRVAR(erlastic_module_doc,
"C implementation of Erlang External Term Format serializer/deserializer.");

static struct PyModuleDef erlasticmodule = {
    PyModuleDef_HEAD_INIT,
    "_erlastic",
    erlastic_module_doc,
    -1,
    erlastic_methods,
    NULL,
    NULL,
    NULL,
    NULL
};

PyMODINIT_FUNC
PyInit__erlastic(void)
{
    if (init_types() < 0)
        return NULL;

    PyObject *m = PyModule_Create(&erlasticmodule);
    if (!m)
        return NULL;

    return m;
}
