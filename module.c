#include <Python.h>

#include "third-party/quickjs.h"

// The data of the type _quickjs.Context.
typedef struct {
	PyObject_HEAD JSRuntime *runtime;
	JSContext *context;
} ContextData;

// The data of the type _quickjs.Object.
typedef struct {
	PyObject_HEAD;
	ContextData *context;
	JSValue object;
} ObjectData;

// The exception raised by this module.
static PyObject *JSException = NULL;
// Converts a JSValue to a Python object.
//
// Takes ownership of the JSValue and will deallocate it (refcount reduced by 1).
static PyObject *quickjs_to_python(ContextData *context_obj, JSValue value);

// Creates an instance of the Object class.
static PyObject *object_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
	ObjectData *self;
	self = (ObjectData *)type->tp_alloc(type, 0);
	if (self != NULL) {
		self->context = NULL;
	}
	return (PyObject *)self;
}

// Deallocates an instance of the Object class.
static void object_dealloc(ObjectData *self) {
	if (self->context) {
		JS_FreeValue(self->context->context, self->object);
		// We incremented the refcount of the context when we created this object, so we should
		// decrease it now so we don't leak memory.
		Py_DECREF(self->context);
	}
	Py_TYPE(self)->tp_free((PyObject *)self);
}

// _quickjs.Object.__call__
static PyObject *object_call(ObjectData *self, PyObject *args, PyObject *kwds);

// _quickjs.Object.json
//
// Returns the JSON representation of the object as a Python string.
static PyObject *object_json(ObjectData *self) {
	// Use the JS JSON.stringify method to convert to JSON. First, we need to retrieve it via
	// API calls.
	JSContext *context = self->context->context;
	JSValue global = JS_GetGlobalObject(context);
	JSValue JSON = JS_GetPropertyStr(context, global, "JSON");
	JSValue stringify = JS_GetPropertyStr(context, JSON, "stringify");

	JSValueConst args[1] = {self->object};
	JSValue json_string = JS_Call(context, stringify, JSON, 1, args);

	JS_FreeValue(context, global);
	JS_FreeValue(context, JSON);
	JS_FreeValue(context, stringify);
	return quickjs_to_python(self->context, json_string);
}

// All methods of the _quickjs.Object class.
static PyMethodDef object_methods[] = {
    {"json", (PyCFunction)object_json, METH_NOARGS, "Converts to a JSON string."},
    {NULL} /* Sentinel */
};

// Define the quickjs.Object type.
static PyTypeObject Object = {PyVarObject_HEAD_INIT(NULL, 0).tp_name = "_quickjs.Object",
                              .tp_doc = "Quickjs object",
                              .tp_basicsize = sizeof(ObjectData),
                              .tp_itemsize = 0,
                              .tp_flags = Py_TPFLAGS_DEFAULT,
                              .tp_new = object_new,
                              .tp_dealloc = (destructor)object_dealloc,
                              .tp_call = (ternaryfunc)object_call,
                              .tp_methods = object_methods};

// _quickjs.Object.__call__
static PyObject *object_call(ObjectData *self, PyObject *args, PyObject *kwds) {
	if (self->context == NULL) {
		// This object does not have a context and has not been created by this module.
		Py_RETURN_NONE;
	}

	// We first loop through all arguments and check that they are supported without doing anything.
	// This makes the cleanup code simpler for the case where we have to raise an error.
	const int nargs = PyTuple_Size(args);
	for (int i = 0; i < nargs; ++i) {
		PyObject *item = PyTuple_GetItem(args, i);
		if (PyBool_Check(item)) {
		} else if (PyLong_Check(item)) {
		} else if (PyFloat_Check(item)) {
		} else if (item == Py_None) {
		} else if (PyUnicode_Check(item)) {
		} else if (PyObject_IsInstance(item, (PyObject *)&Object)) {
		} else {
			PyErr_Format(PyExc_ValueError,
			             "Unsupported type of argument %d when calling quickjs object: %s.",
			             i,
			             Py_TYPE(item)->tp_name);
			return NULL;
		}
	}

	// Now we know that all arguments are supported and we can convert them.
	JSValueConst *jsargs = malloc(nargs * sizeof(JSValueConst));
	for (int i = 0; i < nargs; ++i) {
		PyObject *item = PyTuple_GetItem(args, i);
		if (PyBool_Check(item)) {
			jsargs[i] = JS_MKVAL(JS_TAG_BOOL, item == Py_True ? 1 : 0);
		} else if (PyLong_Check(item)) {
			jsargs[i] = JS_MKVAL(JS_TAG_INT, PyLong_AsLong(item));
		} else if (PyFloat_Check(item)) {
			jsargs[i] = JS_NewFloat64(self->context->context, PyFloat_AsDouble(item));
		} else if (item == Py_None) {
			jsargs[i] = JS_NULL;
		} else if (PyUnicode_Check(item)) {
			jsargs[i] = JS_NewString(self->context->context, PyUnicode_AsUTF8(item));
		} else if (PyObject_IsInstance(item, (PyObject *)&Object)) {
			jsargs[i] = JS_DupValue(self->context->context, ((ObjectData *)item)->object);
		}
	}

	// Perform the actual function call. We release the GIL in order to speed things up for certain
	// use cases. If this module becomes more complicated and gains the capability to call Python
	// function from JS, this needs to be reversed or improved.
	JSValue value;
	Py_BEGIN_ALLOW_THREADS;
	value = JS_Call(self->context->context, self->object, JS_NULL, nargs, jsargs);
	Py_END_ALLOW_THREADS;

	for (int i = 0; i < nargs; ++i) {
		JS_FreeValue(self->context->context, jsargs[i]);
	}
	free(jsargs);
	return quickjs_to_python(self->context, value);
}

// Converts a JSValue to a Python object.
//
// Takes ownership of the JSValue and will deallocate it (refcount reduced by 1).
static PyObject *quickjs_to_python(ContextData *context_obj, JSValue value) {
	JSContext *context = context_obj->context;
	int tag = JS_VALUE_GET_TAG(value);
	// A return value of NULL means an exception.
	PyObject *return_value = NULL;

	if (tag == JS_TAG_INT) {
		return_value = Py_BuildValue("i", JS_VALUE_GET_INT(value));
	} else if (tag == JS_TAG_BOOL) {
		return_value = Py_BuildValue("O", JS_VALUE_GET_BOOL(value) ? Py_True : Py_False);
	} else if (tag == JS_TAG_NULL) {
		return_value = Py_None;
	} else if (tag == JS_TAG_UNDEFINED) {
		return_value = Py_None;
	} else if (tag == JS_TAG_EXCEPTION) {
		// We have a Javascript exception. We convert it to a Python exception via a C string.
		JSValue exception = JS_GetException(context);
		JSValue error_string = JS_ToString(context, exception);
		const char *cstring = JS_ToCString(context, error_string);
		PyErr_Format(JSException, "%s", cstring);
		JS_FreeCString(context, cstring);
		JS_FreeValue(context, error_string);
		JS_FreeValue(context, exception);
	} else if (tag == JS_TAG_FLOAT64) {
		return_value = Py_BuildValue("d", JS_VALUE_GET_FLOAT64(value));
	} else if (tag == JS_TAG_STRING) {
		const char *cstring = JS_ToCString(context, value);
		return_value = Py_BuildValue("s", cstring);
		JS_FreeCString(context, cstring);
	} else if (tag == JS_TAG_OBJECT) {
		// This is a Javascript object or function. We wrap it in a _quickjs.Object.
		return_value = PyObject_CallObject((PyObject *)&Object, NULL);
		ObjectData *object = (ObjectData *)return_value;
		// This is important. Otherwise, the context may be deallocated before the object, which
		// will result in a segfault with high probability.
		Py_INCREF(context_obj);
		object->context = context_obj;
		object->object = JS_DupValue(context, value);
	} else {
		PyErr_Format(PyExc_ValueError, "Unknown quickjs tag: %d", tag);
	}

	JS_FreeValue(context, value);
	if (return_value == Py_None) {
		// Can not simply return PyNone for refcounting reasons.
		Py_RETURN_NONE;
	}
	return return_value;
}

static PyObject *test(PyObject *self, PyObject *args) {
	return Py_BuildValue("i", 42);
}

// Global state of the module. Currently none.
struct module_state {};

// Creates an instance of the _quickjs.Context class.
static PyObject *context_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
	ContextData *self;
	self = (ContextData *)type->tp_alloc(type, 0);
	if (self != NULL) {
		// We never have different contexts for the same runtime. This way, different
		// _quickjs.Context can be used concurrently.
		self->runtime = JS_NewRuntime();
		self->context = JS_NewContext(self->runtime);
	}
	return (PyObject *)self;
}

// Deallocates an instance of the _quickjs.Context class.
static void context_dealloc(ContextData *self) {
	JS_FreeContext(self->context);
	JS_FreeRuntime(self->runtime);
	Py_TYPE(self)->tp_free((PyObject *)self);
}

// _quickjs.Context.eval
//
// Evaluates a Python string as JS and returns the result as a Python object. Will return
// _quickjs.Object for complex types (other than e.g. str, int).
static PyObject *context_eval(ContextData *self, PyObject *args) {
	const char *code;
	if (!PyArg_ParseTuple(args, "s", &code)) {
		return NULL;
	}

	// Perform the actual evaluation. We release the GIL in order to speed things up for certain
	// use cases. If this module becomes more complicated and gains the capability to call Python
	// function from JS, this needs to be reversed or improved.
	JSValue value;
	Py_BEGIN_ALLOW_THREADS;
	value = JS_Eval(self->context, code, strlen(code), "<input>", JS_EVAL_TYPE_GLOBAL);
	Py_END_ALLOW_THREADS;
	return quickjs_to_python(self, value);
}

// _quickjs.Context.get
//
// Retrieves a global variable from the JS context.
static PyObject *context_get(ContextData *self, PyObject *args) {
	const char *name;
	if (!PyArg_ParseTuple(args, "s", &name)) {
		return NULL;
	}
	JSValue global = JS_GetGlobalObject(self->context);
	JSValue value = JS_GetPropertyStr(self->context, global, name);
	JS_FreeValue(self->context, global);
	return quickjs_to_python(self, value);
}

// _quickjs.Context.set_memory_limit
//
// Retrieves a global variable from the JS context.
static PyObject *context_set_memory_limit(ContextData *self, PyObject *args) {
	Py_ssize_t limit;
	if (!PyArg_ParseTuple(args, "n", &limit)) {
		return NULL;
	}
	JS_SetMemoryLimit(self->runtime, limit);
	Py_RETURN_NONE;
}

// All methods of the _quickjs.Context class.
static PyMethodDef context_methods[] = {
    {"eval", (PyCFunction)context_eval, METH_VARARGS, "Evaluates a Javascript string."},
    {"get", (PyCFunction)context_get, METH_VARARGS, "Gets a Javascript global variable."},
	{"set_memory_limit", (PyCFunction)context_set_memory_limit, METH_VARARGS, "Sets the memory limit in bytes."},
    {NULL} /* Sentinel */
};

// Define the _quickjs.Context type.
static PyTypeObject Context = {PyVarObject_HEAD_INIT(NULL, 0).tp_name = "_quickjs.Context",
                               .tp_doc = "Quickjs context",
                               .tp_basicsize = sizeof(ContextData),
                               .tp_itemsize = 0,
                               .tp_flags = Py_TPFLAGS_DEFAULT,
                               .tp_new = context_new,
                               .tp_dealloc = (destructor)context_dealloc,
                               .tp_methods = context_methods};

// All global methods in _quickjs.
static PyMethodDef myextension_methods[] = {{"test", (PyCFunction)test, METH_NOARGS, NULL},
                                            {NULL, NULL}};

// Define the _quickjs module.
static struct PyModuleDef moduledef = {PyModuleDef_HEAD_INIT,
                                       "quickjs",
                                       NULL,
                                       sizeof(struct module_state),
                                       myextension_methods,
                                       NULL,
                                       NULL,
                                       NULL,
                                       NULL};

// This function runs when the module is first imported.
PyMODINIT_FUNC PyInit__quickjs(void) {
	if (PyType_Ready(&Context) < 0) {
		return NULL;
	}
	if (PyType_Ready(&Object) < 0) {
		return NULL;
	}

	PyObject *module = PyModule_Create(&moduledef);
	if (module == NULL) {
		return NULL;
	}

	JSException = PyErr_NewException("_quickjs.JSException", NULL, NULL);
	if (JSException == NULL) {
		return NULL;
	}

	Py_INCREF(&Context);
	PyModule_AddObject(module, "Context", (PyObject *)&Context);
	Py_INCREF(&Object);
	PyModule_AddObject(module, "Object", (PyObject *)&Object);
	PyModule_AddObject(module, "JSException", JSException);
	return module;
}
