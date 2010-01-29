#ifndef _ERLASTIC_H_
#define _ERLASTIC_H_

#define PY_SSIZE_T_CLEAN

#include "Python.h"

#if (PY_VERSION_HEX < 0x02050000)
typedef int Py_ssize_t;
#endif

#ifndef Py_TYPE
#define Py_TYPE(ob) (((PyObject*)(ob))->ob_type)
#endif

extern PyTypeObject *atom_type, *reference_type, *port_type, *pid_type, *export_type;
extern PyObject *decoding_error, *encoding_error;

extern PyTypeObject Decoder_Type, Encoder_Type;

#endif /* _ERLASTIC_H_ */
