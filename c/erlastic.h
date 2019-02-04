#ifndef _ERLASTIC_H_
#define _ERLASTIC_H_

#include "Python.h"

extern PyTypeObject *atom_type, *reference_type, *port_type, *pid_type, *export_type;

extern PyObject *encoding_error;

PyObject *erlastic_encode(PyObject *self, PyObject *args, PyObject *kwargs);
PyObject *erlastic_decode(PyObject *self, PyObject *args, PyObject *kwargs);

#endif /* _ERLASTIC_H_ */
