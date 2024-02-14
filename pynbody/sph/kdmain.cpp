#include "kd.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <functional>
#include <iostream>
#include <limits>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kd.h"
#include "smooth.h"

/*==========================================================================*/
/* Debugging tools                                                          */
/*==========================================================================*/
#define DBG_LEVEL 0
#define DBG(lvl) if (DBG_LEVEL >= lvl)

/*==========================================================================*/
/* Memory allocation wrappers.                                              */
/*==========================================================================*/

#if DBG_LEVEL >= 10000
long total_alloc = 0;
#define CALLOC(type, num)                                                                                   \
    (total_alloc += sizeof(type) * (num), \
    fprintf(stderr, "c"allocating %ld bytes [already alloc"d: %ld].\n", sizeof(type) * (num), total_alloc), \
    ((type *)calloc((num), sizeof(type))))
#else
#define CALLOC(type, num) ((type *)calloc((num), sizeof(type)))
#endif

#define MALLOC(type, num) ((type *)malloc((num) * sizeof(type)))

/*==========================================================================*/
/* Prototypes.                                                              */
/*==========================================================================*/

PyObject *kdinit(PyObject *self, PyObject *args);
PyObject *kdfree(PyObject *self, PyObject *args);
PyObject *kdbuild(PyObject *self, PyObject *args);

PyObject *nn_start(PyObject *self, PyObject *args);
PyObject *nn_next(PyObject *self, PyObject *args);
PyObject *nn_stop(PyObject *self, PyObject *args);
PyObject *nn_rewind(PyObject *self, PyObject *args);

PyObject *populate(PyObject *self, PyObject *args);

PyObject *domain_decomposition(PyObject *self, PyObject *args);
PyObject *set_arrayref(PyObject *self, PyObject *args);
PyObject *get_arrayref(PyObject *self, PyObject *args);
PyObject *get_node_count(PyObject *self, PyObject *args);

PyObject *particles_in_sphere(PyObject *self, PyObject *args);

template <typename T> int checkArray(PyObject *check, const char *name);

int getBitDepth(PyObject *check);

/*==========================================================================*/
#define PROPID_HSM 1
#define PROPID_RHO 2
#define PROPID_QTYMEAN_1D 3
#define PROPID_QTYMEAN_ND 4
#define PROPID_QTYDISP_1D 5
#define PROPID_QTYDISP_ND 6
#define PROPID_QTYDIV 7
#define PROPID_QTYCURL 8
/*==========================================================================*/

static PyMethodDef kdmain_methods[] = {
    {"init", kdinit, METH_VARARGS, "init"},
    {"free", kdfree, METH_VARARGS, "free"},
    {"build", kdbuild, METH_VARARGS, "build"},

    {"nn_start", nn_start, METH_VARARGS, "nn_start"},
    {"nn_next", nn_next, METH_VARARGS, "nn_next"},
    {"nn_stop", nn_stop, METH_VARARGS, "nn_stop"},
    {"nn_rewind", nn_rewind, METH_VARARGS, "nn_rewind"},

    {"particles_in_sphere", particles_in_sphere, METH_VARARGS,
     "particles_in_sphere"},

    {"set_arrayref", set_arrayref, METH_VARARGS, "set_arrayref"},
    {"get_arrayref", get_arrayref, METH_VARARGS, "get_arrayref"},
    {"get_node_count", get_node_count, METH_VARARGS, "get_node_count"},
    {"domain_decomposition", domain_decomposition, METH_VARARGS,
     "domain_decomposition"},

    {"populate", populate, METH_VARARGS, "populate"},

    {NULL, NULL, 0, NULL}};

#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef ourdef = {PyModuleDef_HEAD_INIT,
                                    "kdmain",
                                    "KDTree module for pynbody",
                                    -1,
                                    kdmain_methods,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL};
#endif

PyMODINIT_FUNC
PyInit_kdmain(void)
{
  import_array();
  return PyModule_Create(&ourdef);
}

/*==========================================================================*/
/* kdinit                                                                   */
/*==========================================================================*/
PyObject *kdinit(PyObject *self, PyObject *args) {
  npy_intp nBucket;
  npy_intp i;

  PyObject *pos;  // Nx3 Numpy array of positions
  PyObject *mass; // Nx1 Numpy array of masses

  std::cerr << "Parsing tuple..." << std::endl;
  if (!PyArg_ParseTuple(args, "OOl", &pos, &mass, &nBucket))
    return NULL;
  


  int bitdepth = getBitDepth(pos);
  if (bitdepth == 0) {
    PyErr_SetString(PyExc_ValueError, "Unsupported array dtype for kdtree");
    return NULL;
  }
  if (bitdepth != getBitDepth(mass)) {
    PyErr_SetString(PyExc_ValueError,
                    "pos and mass arrays must have matching dtypes for kdtree");
    return NULL;
  }

  if (bitdepth == 64) {
    if (checkArray<double>(pos, "pos"))
      return NULL;
    if (checkArray<double>(mass, "mass"))
      return NULL;
  } else {
    if (checkArray<float>(pos, "pos"))
      return NULL;
    if (checkArray<float>(mass, "mass"))
      return NULL;
  }

  KDContext *kd = new KDContext();
  kd->nBucket = nBucket;

  std::cerr << "Getting nbodies..." << std::endl;
  npy_intp nbodies = PyArray_DIM(pos, 0);

  kd->nParticles = nbodies;
  kd->nActive = nbodies;
  kd->nBitDepth = bitdepth;
  kd->pNumpyPos = pos;
  kd->pNumpyMass = mass;

  Py_INCREF(pos);
  Py_INCREF(mass);

  std::cerr << "About to count nodes..." << std::endl;
  kdCountNodes(kd);
  std::cerr << "Counted nodes!" << std::endl;

  return PyCapsule_New((void *)kd, NULL, NULL);
}

PyObject * get_node_count(PyObject *self, PyObject *args) {
  PyObject *kdobj;
  if(!PyArg_ParseTuple(args, "O", &kdobj))
    return NULL;
  KDContext *kd = static_cast<KDContext*>(PyCapsule_GetPointer(kdobj, NULL));
  return PyLong_FromLong(kd->nNodes);
}

PyObject * kdbuild(PyObject *self, PyObject *args) {
  PyObject *kdNodeArray; // Length-N Numpy array (uninitialized) for KDNodes
  PyObject *kdobj;
  int num_threads;

  std::cerr << "Hello kdbuild!" << std::endl;

  if (!PyArg_ParseTuple(args, "OOi", &kdobj, &kdNodeArray, &num_threads))
    return NULL;

  std::cerr << "Tuple parsed!" << std::endl;

  KDContext *kd = static_cast<KDContext*>(PyCapsule_GetPointer(kdobj, NULL));

  std::cerr << "Got the context!" << std::endl;

  if(!PyArray_Check(kdNodeArray)) {
    PyErr_SetString(PyExc_ValueError, "First argument needs to be a numpy array of KDNodes");
    return nullptr;
  }

  PyArray_Descr *kdnDescr = PyArray_DESCR(kdNodeArray);

  std::cerr << "Got the descr!" << std::endl;

  std::cerr << kd->nNodes << " " << PyArray_SIZE(kdNodeArray) << std::endl;

  if(kdnDescr->elsize != sizeof(KDNode)) {
    PyErr_SetString(PyExc_ValueError, "Wrong data type passed for KDNode array");
    return nullptr;
  }

  if(PyArray_SIZE(kdNodeArray) != kd->nNodes) {
    PyErr_SetString(PyExc_ValueError, "KDNode array must have the right number of nodes in it");
    return nullptr;
  }

  if((PyArray_FLAGS(kdNodeArray) & NPY_ARRAY_C_CONTIGUOUS) == 0) {
    PyErr_SetString(PyExc_ValueError, "KDNode array must be C-contiguous");
    return nullptr;
  }

  kd->kdNodes = static_cast<KDNode*>(PyArray_DATA(kdNodeArray));
  kd->kdNodesPyObject = kdNodeArray;

  std::cerr << "got the nodes!" << std::endl;

  Py_INCREF(kd->kdNodesPyObject);

  Py_BEGIN_ALLOW_THREADS;

  // Allocate particles - TODO: This must be a numpy array too!
  std::cerr << "Allocating particles..." << kd->nParticles << std::endl;
  kd->p = (PARTICLE *)malloc(kd->nActive * sizeof(PARTICLE));
  std::cerr << "Allocated particles!" << std::endl;
  assert(kd->p != nullptr);

  for (npy_intp i = 0; i < kd->nParticles; i++) {
    kd->p[i].iOrder = i;
    kd->p[i].iMark = 1;
  }


  if (kd->nBitDepth == 64)
    kdBuildTree<double>(kd, num_threads);
  else
    kdBuildTree<float>(kd, num_threads);

  Py_END_ALLOW_THREADS;

}

/*==========================================================================*/
/* kdfree                                                                   */
/*==========================================================================*/
PyObject *kdfree(PyObject *self, PyObject *args) {
  KDContext *kd;
  PyObject *kdobj;

  PyArg_ParseTuple(args, "O", &kdobj);
  kd = static_cast<KDContext*>(PyCapsule_GetPointer(kdobj, NULL));

  if(kd->p!=nullptr)
    free(kd->p);

  Py_XDECREF(kd->pNumpyPos);
  Py_XDECREF(kd->pNumpyMass);
  Py_XDECREF(kd->pNumpySmooth);
  Py_XDECREF(kd->pNumpyDen);
  Py_XDECREF(kd->kdNodesPyObject);

  delete kd;

  Py_INCREF(Py_None);
  return Py_None;
}

/*==========================================================================*/
/* nn_start                                                                 */
/*==========================================================================*/
PyObject *nn_start(PyObject *self, PyObject *args) {
  KDContext* kd;
  SMX smx;

  PyObject *kdobj;
  /* Nx1 Numpy arrays for smoothing length and density for calls that pass
     in those values from existing arrays
  */

  int nSmooth, nProcs;
  float period = std::numeric_limits<float>::max();

  PyArg_ParseTuple(args, "Oii|f", &kdobj, &nSmooth, &nProcs, &period);
  kd = static_cast<KDContext*>(PyCapsule_GetPointer(kdobj, NULL));

  if (period <= 0)
    period = std::numeric_limits<float>::max();



  float fPeriod[3] = {period, period, period};

  if (nSmooth > PyArray_DIM(kd->pNumpyPos, 0)) {
    PyErr_SetString(
        PyExc_ValueError,
        "Number of smoothing particles exceeds number of particles in tree");
    return NULL;
  }

  /*
   ** Check to make sure that the bounds of the simulation agree
   ** with the period specified, if not cause an error.
   */

  if (!smCheckFits(kd, fPeriod)) {
    PyErr_SetString(
        PyExc_ValueError,
        "The particles span a region larger than the specified boxsize");
    return NULL;
  }

  if (!smInit(&smx, kd, nSmooth, fPeriod)) {
    PyErr_SetString(PyExc_RuntimeError, "Unable to create smoothing context");
    return NULL;
  }

  smSmoothInitStep(smx, nProcs);

  return PyCapsule_New(smx, NULL, NULL);
}

/*==========================================================================*/
/* nn_next                                                                 */
/*==========================================================================*/
PyObject *nn_next(PyObject *self, PyObject *args) {
  long nCnt, i, pj;

  KDContext* kd;
  SMX smx;

  PyObject *kdobj, *smxobj;
  PyObject *nnList;
  PyObject *nnDist;
  PyObject *retList;

  PyArg_ParseTuple(args, "OO", &kdobj, &smxobj);
  kd = static_cast<KDContext*>(PyCapsule_GetPointer(kdobj, NULL));
  smx = (SMX)PyCapsule_GetPointer(smxobj, NULL);

  Py_BEGIN_ALLOW_THREADS;

  if (kd->nBitDepth == 32) nCnt = smSmoothStep<float>(smx, 0);
  else nCnt = smSmoothStep<double>(smx, 0);

  Py_END_ALLOW_THREADS;

  if (nCnt > 0) {
    nnList = PyList_New(nCnt); // Py_INCREF(nnList);
    nnDist = PyList_New(nCnt); // Py_INCREF(nnDist);
    retList = PyList_New(4);
    Py_INCREF(retList);

    for (i = 0; i < nCnt; i++) {
      pj = smx->pList[i];
      PyList_SetItem(nnList, i, PyLong_FromLong(smx->kd->p[pj].iOrder));
      PyList_SetItem(nnDist, i, PyFloat_FromDouble(smx->fList[i]));
    }

    PyList_SetItem(retList, 0, PyLong_FromLong(smx->kd->p[smx->pi].iOrder));
    if (kd->nBitDepth == 32)
      PyList_SetItem(retList, 1, PyFloat_FromDouble(GETSMOOTH(float, smx->pi)));
    else
      PyList_SetItem(retList, 1,
                     PyFloat_FromDouble(GETSMOOTH(double, smx->pi)));
    PyList_SetItem(retList, 2, nnList);
    PyList_SetItem(retList, 3, nnDist);

    return retList;
  }

  Py_INCREF(Py_None);
  return Py_None;
}

/*==========================================================================*/
/* nn_stop                                                                 */
/*==========================================================================*/
PyObject *nn_stop(PyObject *self, PyObject *args) {
  KDContext* kd;
  SMX smx;

  PyObject *kdobj, *smxobj;

  PyArg_ParseTuple(args, "OO", &kdobj, &smxobj);
  kd = static_cast<KDContext*>(PyCapsule_GetPointer(kdobj, NULL));
  smx = (SMX)PyCapsule_GetPointer(smxobj, NULL);

  smFinish(smx);

  Py_INCREF(Py_None);
  return Py_None;
}

/*==========================================================================*/
/* nn_rewind                                                                */
/*==========================================================================*/
PyObject *nn_rewind(PyObject *self, PyObject *args) {
  SMX smx;
  PyObject *smxobj;

  PyArg_ParseTuple(args, "O", &smxobj);
  smx = (SMX)PyCapsule_GetPointer(smxobj, NULL);
  smSmoothInitStep(smx, 1);

  return PyCapsule_New(smx, NULL, NULL);
}

int getBitDepth(PyObject *check) {

  if (check == NULL) {
    return 0;
  }

  PyArray_Descr *descr = PyArray_DESCR(check);
  if (descr != NULL && descr->kind == 'f' && descr->elsize == sizeof(float))
    return 32;
  else if (descr != NULL && descr->kind == 'f' &&
           descr->elsize == sizeof(double))
    return 64;
  else
    return 0;
}

template <typename T> const char *c_name() { return "unknown"; }

template <> const char *c_name<double>() { return "double"; }

template <> const char *c_name<float>() { return "float"; }

template <typename T> int checkArray(PyObject *check, const char *name) {

  if (check == NULL) {
    PyErr_Format(PyExc_ValueError, "Unspecified array '%s' in kdtree", name);
    return 1;
  }

  PyArray_Descr *descr = PyArray_DESCR(check);
  if (descr == NULL || descr->kind != 'f' || descr->elsize != sizeof(T)) {
    PyErr_Format(
        PyExc_TypeError,
        "Incorrect numpy data type for %s passed to kdtree - must match C %s",
        name, c_name<T>());
    return 1;
  }
  return 0;
}

PyObject *set_arrayref(PyObject *self, PyObject *args) {
  int arid;
  PyObject *kdobj, *arobj, **existing;
  KDContext* kd;

  const char *name0 = "smooth";
  const char *name1 = "rho";
  const char *name2 = "mass";
  const char *name3 = "qty";
  const char *name4 = "qty_sm";

  const char *name;

  PyArg_ParseTuple(args, "OiO", &kdobj, &arid, &arobj);
  kd = static_cast<KDContext*>(PyCapsule_GetPointer(kdobj, NULL));
  if (!kd)
    return NULL;

  switch (arid) {
  case 0:
    existing = &(kd->pNumpySmooth);
    name = name0;
    break;
  case 1:
    existing = &(kd->pNumpyDen);
    name = name1;
    break;
  case 2:
    existing = &(kd->pNumpyMass);
    name = name2;
    break;
  case 3:
    existing = &(kd->pNumpyQty);
    name = name3;
    break;
  case 4:
    existing = &(kd->pNumpyQtySmoothed);
    name = name4;
    break;
  default:
    PyErr_SetString(PyExc_ValueError, "Unknown array to set for KD tree");
    return NULL;
  }

  int bitdepth = 0;
  if (arid <= 2)
    bitdepth = kd->nBitDepth;
  else if (arid == 3 || arid == 4)
    bitdepth = getBitDepth(arobj);

  if (bitdepth == 32) {
    if (checkArray<float>(arobj, name))
      return NULL;
  } else if (bitdepth == 64) {
    if (checkArray<double>(arobj, name))
      return NULL;
  } else {
    PyErr_SetString(PyExc_ValueError, "Unsupported array dtype for kdtree");
    return NULL;
  }

  Py_XDECREF(*existing);
  (*existing) = arobj;
  Py_INCREF(arobj);

  Py_INCREF(Py_None);
  return Py_None;
}

PyObject *get_arrayref(PyObject *self, PyObject *args) {
  int arid;
  PyObject *kdobj, *arobj, **existing;
  KDContext* kd;

  PyArg_ParseTuple(args, "Oi", &kdobj, &arid);
  kd = static_cast<KDContext*>(PyCapsule_GetPointer(kdobj, NULL));
  if (!kd)
    return NULL;

  switch (arid) {
  case 0:
    existing = &(kd->pNumpySmooth);
    break;
  case 1:
    existing = &(kd->pNumpyDen);
    break;
  case 2:
    existing = &(kd->pNumpyMass);
    break;
  case 3:
    existing = &(kd->pNumpyQty);
    break;
  case 4:
    existing = &(kd->pNumpyQtySmoothed);
    break;
  default:
    PyErr_SetString(PyExc_ValueError, "Unknown array to get from KD tree");
    return NULL;
  }

  Py_INCREF(*existing);

  if (*existing == NULL) {
    Py_INCREF(Py_None);
    return Py_None;
  } else
    return (*existing);
}

PyObject *domain_decomposition(PyObject *self, PyObject *args) {
  int nproc;
  PyObject *smxobj;
  KDContext* kd;

  PyArg_ParseTuple(args, "Oi", &smxobj, &nproc);

  kd = static_cast<KDContext*>(PyCapsule_GetPointer(smxobj, NULL));
  if (!kd)
    return NULL;

  if (kd->nBitDepth == 32) {
    if (checkArray<float>(kd->pNumpySmooth, "smooth"))
      return NULL;
  } else {
    if (checkArray<double>(kd->pNumpySmooth, "smooth"))
      return NULL;
  }

  if (nproc < 0) {
    PyErr_SetString(PyExc_ValueError, "Invalid number of processors");
    return NULL;
  }

  if (kd->nBitDepth == 32)
    smDomainDecomposition<float>(kd, nproc);
  else
    smDomainDecomposition<double>(kd, nproc);

  Py_INCREF(Py_None);
  return Py_None;
}

template <typename Tf, typename Tq> struct typed_particles_in_sphere {
  static PyObject *call(PyObject *self, PyObject *args) {
    SMX smx;
    KDContext* kd;
    float r;
    float ri[3];

    PyObject *kdobj = nullptr, *smxobj = nullptr;

    PyArg_ParseTuple(args, "OOffff", &kdobj, &smxobj, &ri[0], &ri[1], &ri[2],
                     &r);

    kd = static_cast<KDContext*>(PyCapsule_GetPointer(kdobj, NULL));
    smx = (SMX)PyCapsule_GetPointer(smxobj, NULL);

    initParticleList(smx);
    smBallGather<Tf, smBallGatherStoreResultInList>(smx, r * r, ri);

    return getReturnParticleList(smx);
  }
};

template <typename Tf, typename Tq> struct typed_populate {
  static PyObject *call(PyObject *self, PyObject *args) {

    long i, nCnt;
    int procid;
    KDContext* kd;
    SMX smx_global, smx_local;
    int propid;
    float ri[3];
    float hsm;
    int Wendland;

    void (*pSmFn)(SMX, npy_intp, int, npy_intp *, float *, bool) = NULL;

    PyObject *kdobj, *smxobj;

    PyArg_ParseTuple(args, "OOiii", &kdobj, &smxobj, &propid, &procid,
                     &Wendland);
    kd = static_cast<KDContext*>(PyCapsule_GetPointer(kdobj, NULL));
    smx_global = (SMX)PyCapsule_GetPointer(smxobj, NULL);

    long nbodies = PyArray_DIM(kd->pNumpyPos, 0);

    if (checkArray<Tf>(kd->pNumpySmooth, "smooth"))
      return NULL;
    if (propid > PROPID_HSM) {
      if (checkArray<Tf>(kd->pNumpyDen, "rho"))
        return NULL;
      if (checkArray<Tf>(kd->pNumpyMass, "mass"))
        return NULL;
    }
    if (propid > PROPID_RHO) {
      if (checkArray<Tq>(kd->pNumpyQty, "qty"))
        return NULL;
      if (checkArray<Tq>(kd->pNumpyQtySmoothed, "qty_sm"))
        return NULL;
    }

    smx_local = smInitThreadLocalCopy(smx_global);
    smx_local->warnings = false;
    smx_local->pi = 0;

    smx_global->warnings = false;

    npy_intp total_particles = 0;

    switch (propid) {
    case PROPID_RHO:
      pSmFn = &smDensity<Tf>;
      break;
    case PROPID_QTYMEAN_ND:
      pSmFn = &smMeanQtyND<Tf, Tq>;
      break;
    case PROPID_QTYDISP_ND:
      pSmFn = &smDispQtyND<Tf, Tq>;
      break;
    case PROPID_QTYMEAN_1D:
      pSmFn = &smMeanQty1D<Tf, Tq>;
      break;
    case PROPID_QTYDISP_1D:
      pSmFn = &smDispQty1D<Tf, Tq>;
      break;
    case PROPID_QTYDIV:
      pSmFn = &smDivQty<Tf, Tq>;
      break;
    case PROPID_QTYCURL:
      pSmFn = &smCurlQty<Tf, Tq>;
      break;
    }

    if (propid == PROPID_HSM) {
      Py_BEGIN_ALLOW_THREADS;
      for (i = 0; i < nbodies; i++) {
        nCnt = smSmoothStep<Tf>(smx_local, procid);
        if (nCnt == -1)
          break; // nothing more to do
        total_particles += 1;
      }
      Py_END_ALLOW_THREADS;

    } else {

      i = smGetNext(smx_local);

      Py_BEGIN_ALLOW_THREADS;
      while (i < nbodies) {
        // make a copy of the position of this particle
        for (int j = 0; j < 3; ++j) {
          ri[j] = GET2<Tf>(kd->pNumpyPos, kd->p[i].iOrder, j);
        }

        // retrieve the existing smoothing length
        hsm = GETSMOOTH(Tf, i);

        // use it to get nearest neighbours
        nCnt = smBallGather<Tf, smBallGatherStoreResultInSmx>(
            smx_local, 4 * hsm * hsm, ri);

        // calculate the density
        (*pSmFn)(smx_local, i, nCnt, smx_local->pList, smx_local->fList,
                 Wendland);

        // select next particle in coordination with other threads
        i = smGetNext(smx_local);

        if (smx_global->warnings)
          break;
      }
      Py_END_ALLOW_THREADS;
    }

    smFinishThreadLocalCopy(smx_local);
    if (smx_local->warnings) {
      PyErr_SetString(PyExc_RuntimeError,
                      "Buffer overflow in smoothing operation. This probably "
                      "means that your smoothing lengths are too large "
                      "compared to the number of neighbours you specified.");
      return NULL;
    } else {
      Py_INCREF(Py_None);
      return Py_None;
    }
  }
};

template <template <typename, typename> class func>
PyObject *type_dispatcher(PyObject *self, PyObject *args) {
  PyObject *kdobj = PyTuple_GetItem(args, 0);
  if (kdobj == nullptr) {
    PyErr_SetString(PyExc_ValueError, "First argument must be a kdtree object");
    return nullptr;
  }
  KDContext* kd = static_cast<KDContext*>(PyCapsule_GetPointer(kdobj, nullptr));
  int nF = kd->nBitDepth;
  int nQ = 32;

  if (kd->pNumpyQty != NULL) {
    nQ = getBitDepth(kd->pNumpyQty);
  }

  if (nF == 64 && nQ == 64)
    return func<double, double>::call(self, args);
  else if (nF == 64 && nQ == 32)
    return func<double, float>::call(self, args);
  else if (nF == 32 && nQ == 32)
    return func<float, float>::call(self, args);
  else if (nF == 32 && nQ == 64)
    return func<float, double>::call(self, args);
  else {
    PyErr_SetString(PyExc_ValueError, "Unsupported dtype combination");
    return nullptr;
  }
}

PyObject *populate(PyObject *self, PyObject *args) {
  return type_dispatcher<typed_populate>(self, args);
}

PyObject *particles_in_sphere(PyObject *self, PyObject *args) {
  return type_dispatcher<typed_particles_in_sphere>(self, args);
}
