/*  Copyright (C) 2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "mkc.h"

typedef struct {
  PyObject_HEAD
  mkc_index db;
  int open;
  Py_ssize_t exports;
} Index;

static PyObject * closed_error(void) {
  PyErr_SetString(PyExc_ValueError, "operation on a closed mkc index");
  return NULL;
}

static int pack(const char * s, Py_ssize_t n, unsigned k, uint64_t * key) {
  uint64_t v = 0;
  Py_ssize_t i;
  int b;
  if (n != (Py_ssize_t) k) {
    PyErr_Format(PyExc_ValueError,
                 "k-mer must have exactly %u bases, got %zd", k, n);
    return -1;
  }
  for (i = 0; i < n; i++) {
    b = mkc_base((unsigned char) s[i]);
    if (b < 0) {
      PyErr_Format(PyExc_ValueError, "invalid base %c in k-mer", s[i]);
      return -1;
    }
    v = v << 2 | (unsigned) b;
  }
  *key = v;
  return 0;
}

static const char * text(PyObject * o, Py_ssize_t * n) {
  if (PyUnicode_Check(o)) return PyUnicode_AsUTF8AndSize(o, n);
  if (PyBytes_Check(o)) {
    *n = PyBytes_GET_SIZE(o);  return PyBytes_AS_STRING(o);
  }
  PyErr_SetString(PyExc_TypeError, "expected str or bytes");
  return NULL;
}

static int Index_init(Index * self, PyObject * args, PyObject * kw) {
  static char * names[] = { "path", "mapped", NULL };
  PyObject * path;
  int mapped = 0, rc, err;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "O&|p", names,
                                   PyUnicode_FSConverter, &path, &mapped))
    return -1;
  if (self->open) { mkc_close(&self->db);  self->open = 0; }
  Py_BEGIN_ALLOW_THREADS
  rc = (mapped ? mkc_open_mapped : mkc_open)(&self->db,
                                             PyBytes_AS_STRING(path));
  err = errno;
  Py_END_ALLOW_THREADS
  if (rc) {
    errno = err;
    PyErr_SetFromErrnoWithFilenameObject(
      err == EINVAL ? PyExc_ValueError : PyExc_OSError, path);
    Py_DECREF(path);
    return -1;
  }
  Py_DECREF(path);
  self->open = 1;
  return 0;
}

static void Index_dealloc(Index * self) {
  if (self->open) mkc_close(&self->db);
  Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyObject * Index_close(Index * self, PyObject * unused) {
  (void) unused;
  if (self->exports) {
    PyErr_SetString(PyExc_BufferError,
                    "cannot close while iterators are alive");
    return NULL;
  }
  if (self->open) { mkc_close(&self->db);  self->open = 0; }
  Py_RETURN_NONE;
}

static PyObject * Index_enter(Index * self, PyObject * unused) {
  (void) unused;
  if (!self->open) return closed_error();
  Py_INCREF(self);  return (PyObject *) self;
}

static PyObject * Index_exit(Index * self, PyObject * args) {
  (void) args;  return Index_close(self, NULL);
}

static PyObject * Index_count(Index * self, PyObject * kmer) {
  Py_ssize_t n;
  const char * s;
  uint64_t key;
  if (!self->open) return closed_error();
  s = text(kmer, &n);
  if (!s || pack(s, n, self->db.k, &key)) return NULL;
  return PyLong_FromUnsignedLongLong(mkc_count_key(&self->db, key));
}

static PyObject * Index_getitem(Index * self, PyObject * kmer) {
  return Index_count(self, kmer);
}

static int Index_contains(Index * self, PyObject * kmer) {
  PyObject * c = Index_count(self, kmer);
  int r;
  if (!c) return -1;
  r = PyObject_IsTrue(c);  Py_DECREF(c);
  return r;
}

static Py_ssize_t Index_len(Index * self) {
  if (!self->open) { closed_error();  return -1; }
  return (Py_ssize_t) self->db.distinct;
}

static PyObject * Index_count_keys(Index * self, PyObject * args,
                                   PyObject * kw) {
  static char * names[] = { "keys", "out", NULL };
  PyObject * keys_obj, * out_obj = Py_None, * result;
  Py_buffer in, out;
  size_t n;
  if (!PyArg_ParseTupleAndKeywords(args, kw, "O|O", names, &keys_obj,
                                   &out_obj))
    return NULL;
  if (!self->open) return closed_error();
  if (PyObject_GetBuffer(keys_obj, &in, PyBUF_C_CONTIGUOUS | PyBUF_FORMAT))
    return NULL;
  if (in.itemsize != 8
      || (in.format && strcmp(in.format, "Q") && strcmp(in.format, "L")
          && strcmp(in.format, "<Q") && strcmp(in.format, "=Q")
          && strcmp(in.format, "q") && strcmp(in.format, "l"))) {
    PyBuffer_Release(&in);
    PyErr_SetString(PyExc_TypeError,
                    "keys must be a contiguous buffer of 64-bit integers");
    return NULL;
  }
  n = (size_t) (in.len / 8);
  if (out_obj == Py_None) {
    PyObject * mod = PyImport_ImportModule("array"), * grow, * r;
    if (!mod) { PyBuffer_Release(&in);  return NULL; }
    result = PyObject_CallMethod(mod, "array", "s", "Q");
    Py_DECREF(mod);
    if (!result) { PyBuffer_Release(&in);  return NULL; }
    grow = PyBytes_FromStringAndSize(NULL, (Py_ssize_t) (8 * n));
    if (!grow) { Py_DECREF(result);  PyBuffer_Release(&in);  return NULL; }
    memset(PyBytes_AS_STRING(grow), 0, 8 * n);
    r = PyObject_CallMethod(result, "frombytes", "O", grow);
    Py_DECREF(grow);
    if (!r) { Py_DECREF(result);  PyBuffer_Release(&in);  return NULL; }
    Py_DECREF(r);
  } else { result = out_obj;  Py_INCREF(result); }
  if (PyObject_GetBuffer(result, &out, PyBUF_C_CONTIGUOUS | PyBUF_WRITABLE)) {
    Py_DECREF(result);  PyBuffer_Release(&in);  return NULL;
  }
  if (out.len != in.len) {
    PyBuffer_Release(&out);  PyBuffer_Release(&in);  Py_DECREF(result);
    PyErr_SetString(PyExc_ValueError,
                    "out must have the same length as keys, "
                    "with 8-byte items");
    return NULL;
  }
  Py_BEGIN_ALLOW_THREADS
  mkc_count_batch(&self->db, (const uint64_t *) in.buf, (uint64_t *) out.buf,
                  n);
  Py_END_ALLOW_THREADS
  PyBuffer_Release(&out);  PyBuffer_Release(&in);
  return result;
}

static PyObject * Index_count_sequence(Index * self, PyObject * seq) {
  Py_ssize_t n;
  const char * s;
  unsigned k;
  size_t windows;
  PyObject * mod, * result, * zeros;
  Py_buffer out;
  uint64_t * counts;
  if (!self->open) return closed_error();
  s = text(seq, &n);
  if (!s) return NULL;
  k = self->db.k;
  windows = n >= (Py_ssize_t) k ? (size_t) n - k + 1 : 0;
  mod = PyImport_ImportModule("array");
  if (!mod) return NULL;
  zeros = PyBytes_FromStringAndSize(NULL, (Py_ssize_t) (8 * windows));
  if (!zeros) { Py_DECREF(mod);  return NULL; }
  memset(PyBytes_AS_STRING(zeros), 0, 8 * windows);
  result = PyObject_CallMethod(mod, "array", "sO", "Q", zeros);
  Py_DECREF(mod);  Py_DECREF(zeros);
  if (!result || !windows) return result;
  if (PyObject_GetBuffer(result, &out, PyBUF_C_CONTIGUOUS | PyBUF_WRITABLE)) {
    Py_DECREF(result);  return NULL;
  }
  counts = out.buf;
  Py_BEGIN_ALLOW_THREADS
  mkc_count_sequence(&self->db, s, (size_t) n, counts);
  Py_END_ALLOW_THREADS
  PyBuffer_Release(&out);
  return result;
}

static PyObject * Index_get_k(Index * self, void * c) {
  (void) c;
  if (!self->open) return closed_error();
  return PyLong_FromUnsignedLong(self->db.k);
}

static PyObject * Index_get_distinct(Index * self, void * c) {
  (void) c;
  if (!self->open) return closed_error();
  return PyLong_FromUnsignedLongLong(self->db.distinct);
}

static PyObject * Index_get_total(Index * self, void * c) {
  (void) c;
  if (!self->open) return closed_error();
  return PyLong_FromUnsignedLongLong(self->db.total);
}

static PyObject * Index_get_layout(Index * self, void * c) {
  (void) c;
  if (!self->open) return closed_error();
  return PyUnicode_FromString(self->db.layout == MKC_DENSE ? "dense"
                                                           : "hash");
}

static PyObject * Index_get_nbytes(Index * self, void * c) {
  (void) c;
  if (!self->open) return closed_error();
  return PyLong_FromSize_t(self->db.size);
}

static PyObject * Index_get_closed(Index * self, void * c) {
  (void) c;  return PyBool_FromLong(!self->open);
}

typedef struct { PyObject_HEAD Index * index;  mkc_iterator it; } Items;

static void Items_dealloc(Items * self) {
  if (self->index) { self->index->exports--;  Py_DECREF(self->index); }
  Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyObject * Items_next(Items * self) {
  uint64_t key, count;
  char dna[33];
  unsigned k, j;
  if (!mkc_next(&self->index->db, &self->it, &key, &count)) return NULL;
  k = self->index->db.k;
  for (j = k; j; j--) { dna[j - 1] = "ACGT"[key & 3];  key >>= 2; }
  return Py_BuildValue("(s#K)", dna, (Py_ssize_t) k,
                       (unsigned long long) count);
}

static PyTypeObject ItemsType = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "mkc._mkc.ItemIterator",
  .tp_basicsize = sizeof (Items),
  .tp_dealloc = (destructor) Items_dealloc,
  .tp_flags = Py_TPFLAGS_DEFAULT,
  .tp_iter = PyObject_SelfIter,
  .tp_iternext = (iternextfunc) Items_next,
};

static PyObject * Index_items(Index * self, PyObject * unused) {
  Items * it;
  (void) unused;
  if (!self->open) return closed_error();
  it = PyObject_New(Items, &ItemsType);
  if (!it) return NULL;
  Py_INCREF(self);  it->index = self;  it->it.at = 0;  self->exports++;
  return (PyObject *) it;
}

static PyMethodDef Index_methods[] = {
  { "count", (PyCFunction) Index_count, METH_O,
    "count(kmer) -> int: occurrences of kmer or its reverse complement." },
  { "count_keys", (PyCFunction) (void (*)(void)) Index_count_keys,
    METH_VARARGS | METH_KEYWORDS,
    "count_keys(keys, out=None): counts for a buffer of 2-bit packed uint64 "
    "k-mers (A=0 C=1 G=2 T=3,\n"
    "first base most significant). Fills out (same length, 8-byte items) "
    "or returns array('Q'). Releases the GIL." },
  { "count_sequence", (PyCFunction) Index_count_sequence, METH_O,
    "count_sequence(seq) -> array('Q'): count of every k-mer window; "
    "windows with non-ACGT bases are 0." },
  { "items", (PyCFunction) Index_items, METH_NOARGS,
    "items() -> iterator of (kmer, count), canonical k-mers in table "
    "order." },
  { "close", (PyCFunction) Index_close, METH_NOARGS, "Unmap the index." },
  { "__enter__", (PyCFunction) Index_enter, METH_NOARGS, NULL },
  { "__exit__", (PyCFunction) Index_exit, METH_VARARGS, NULL },
  { NULL, NULL, 0, NULL },
};

static PyGetSetDef Index_getset[] = {
  { "k", (getter) Index_get_k, NULL, "k-mer length", NULL },
  { "distinct", (getter) Index_get_distinct, NULL,
    "number of distinct canonical k-mers", NULL },
  { "total", (getter) Index_get_total, NULL,
    "number of k-mer occurrences counted", NULL },
  { "layout", (getter) Index_get_layout, NULL, "'hash' or 'dense'", NULL },
  { "nbytes", (getter) Index_get_nbytes, NULL, "index file size in bytes",
    NULL },
  { "closed", (getter) Index_get_closed, NULL, NULL, NULL },
  { NULL, NULL, NULL, NULL, NULL },
};

static PySequenceMethods Index_sequence = {
  .sq_contains = (objobjproc) Index_contains
};

static PyMappingMethods Index_mapping = {
  .mp_length = (lenfunc) Index_len,
  .mp_subscript = (binaryfunc) Index_getitem
};

static PyTypeObject IndexType = {
  PyVarObject_HEAD_INIT(NULL, 0)
  .tp_name = "mkc._mkc.Index",
  .tp_doc = "Index(path, mapped=False): read-only mkc k-mer count index. "
            "Large indexes that fit\n"
            "comfortably in available memory are read into huge-page memory "
            "for faster lookups;\n"
            "mapped=True always memory-maps the file instead (least memory).",
  .tp_basicsize = sizeof (Index),
  .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
  .tp_new = PyType_GenericNew,
  .tp_init = (initproc) Index_init,
  .tp_dealloc = (destructor) Index_dealloc,
  .tp_methods = Index_methods,
  .tp_getset = Index_getset,
  .tp_as_sequence = &Index_sequence,
  .tp_as_mapping = &Index_mapping,
  .tp_iter = (getiterfunc) Index_items,
};

static PyObject * encode(PyObject * m, PyObject * kmer) {
  Py_ssize_t n;
  const char * s;
  uint64_t key;
  (void) m;
  s = text(kmer, &n);
  if (!s) return NULL;
  if (n < 1 || n > 32) {
    PyErr_SetString(PyExc_ValueError, "k-mer length must be 1..32");
    return NULL;
  }
  if (pack(s, n, (unsigned) n, &key)) return NULL;
  return PyLong_FromUnsignedLongLong(key);
}

static PyObject * decode(PyObject * m, PyObject * args) {
  unsigned long long key;
  unsigned k, j;
  char dna[33];
  (void) m;
  if (!PyArg_ParseTuple(args, "KI", &key, &k)) return NULL;
  if (k < 1 || k > 32) {
    PyErr_SetString(PyExc_ValueError, "k must be 1..32");
    return NULL;
  }
  if (k < 32 && key >> (2 * k)) {
    PyErr_SetString(PyExc_ValueError, "key has bits above 2k");
    return NULL;
  }
  for (j = k; j; j--) { dna[j - 1] = "ACGT"[key & 3];  key >>= 2; }
  return PyUnicode_FromStringAndSize(dna, k);
}

static PyMethodDef module_methods[] = {
  { "encode", encode, METH_O,
    "encode(kmer) -> int: 2-bit packing used by Index.count_keys." },
  { "decode", decode, METH_VARARGS,
    "decode(key, k) -> str: inverse of encode." },
  { NULL, NULL, 0, NULL },
};

static struct PyModuleDef module = {
  PyModuleDef_HEAD_INIT, "mkc._mkc", "mkc index reader", -1, module_methods,
  NULL, NULL, NULL, NULL
};

PyMODINIT_FUNC PyInit__mkc(void) {
  PyObject * m;
  if (PyType_Ready(&IndexType) < 0 || PyType_Ready(&ItemsType) < 0)
    return NULL;
  m = PyModule_Create(&module);
  if (!m) return NULL;
  Py_INCREF(&IndexType);
  if (PyModule_AddObject(m, "Index", (PyObject *) &IndexType)) {
    Py_DECREF(&IndexType);  Py_DECREF(m);  return NULL;
  }
  return m;
}
