#ifndef WITH_THREAD
# error "xxx no-thread configuration not tested, please report if you need that"
#endif
#include "pythread.h"


struct cffi_tls_s {
    /* The locally-made thread state.  This is only non-null in case
       we build the thread state here.  It remains null if this thread
       had already a thread state provided by CPython. */
    PyThreadState *local_thread_state;

#ifndef USE__THREAD
    /* The saved errno.  If the C compiler supports '__thread', then
       we use that instead. */
    int saved_errno;
#endif

#ifdef MS_WIN32
    /* The saved lasterror, on Windows. */
    int saved_lasterror;
#endif
};

static struct cffi_tls_s *get_cffi_tls(void);   /* in misc_thread_posix.h 
                                                   or misc_win32.h */


/* issue #362: Py_Finalize() will free any threadstate around, so in
 * that case we must not call PyThreadState_Delete() any more on them
 * from cffi_thread_shutdown().  The following mess is to give a
 * thread-safe way to know that Py_Finalize() started.
 */
#define TLS_DEL_LOCK() PyThread_acquire_lock(cffi_tls_delete_lock, WAIT_LOCK)
#define TLS_DEL_UNLOCK() PyThread_release_lock(cffi_tls_delete_lock)
static PyThread_type_lock cffi_tls_delete_lock = NULL;
static int cffi_tls_delete;
static PyObject *old_exitfunc;

static PyObject *cffi_tls_shutdown(PyObject *self, PyObject *args)
{
    /* the lock here will wait until any parallel cffi_thread_shutdown()
       is done.  Future cffi_thread_shutdown() won't touch their
       PyThreadState any more, which are all supposed to be freed anyway
       very soon after the present cffi_tls_shutdown() function is called.
     */
    TLS_DEL_LOCK();
    cffi_tls_delete = 0;   /* Py_Finalize() called */
    TLS_DEL_UNLOCK();

    PyObject *ofn = old_exitfunc;
    if (ofn == NULL)
    {
        Py_INCREF(Py_None);
        return Py_None;
    }
    else
    {
        old_exitfunc = NULL;
        return PyObject_CallFunction(ofn, "");
    }
}

static void init_cffi_tls_delete(void)
{
    static PyMethodDef mdef = {
        "cffi_tls_shutdown", cffi_tls_shutdown, METH_NOARGS,
    };
    PyObject *shutdown_fn;

    cffi_tls_delete_lock = PyThread_allocate_lock();
    if (cffi_tls_delete_lock == NULL)
    {
        PyErr_SetString(PyExc_SystemError,
                        "can't allocate cffi_tls_delete_lock");
        return;
    }

    shutdown_fn = PyCFunction_New(&mdef, NULL);
    if (shutdown_fn == NULL)
        return;

    old_exitfunc = PySys_GetObject("exitfunc");
    if (PySys_SetObject("exitfunc", shutdown_fn) == 0)
        cffi_tls_delete = 1;    /* all ready */
    Py_DECREF(shutdown_fn);
}

static void cffi_thread_shutdown(void *p)
{
    struct cffi_tls_s *tls = (struct cffi_tls_s *)p;

    if (tls->local_thread_state != NULL) {
        /*
         *  issue #362: see comments above
         */
        TLS_DEL_LOCK();
        if (cffi_tls_delete)
            PyThreadState_Delete(tls->local_thread_state);
        TLS_DEL_UNLOCK();
    }
    free(tls);
}

/* USE__THREAD is defined by setup.py if it finds that it is
   syntactically valid to use "__thread" with this C compiler. */
#ifdef USE__THREAD

static __thread int cffi_saved_errno = 0;
static void save_errno_only(void) { cffi_saved_errno = errno; }
static void restore_errno_only(void) { errno = cffi_saved_errno; }

#else

static void save_errno_only(void)
{
    int saved = errno;
    struct cffi_tls_s *tls = get_cffi_tls();
    if (tls != NULL)
        tls->saved_errno = saved;
}

static void restore_errno_only(void)
{
    struct cffi_tls_s *tls = get_cffi_tls();
    if (tls != NULL)
        errno = tls->saved_errno;
}

#endif


/* MESS.  We can't use PyThreadState_GET(), because that calls
   PyThreadState_Get() which fails an assert if the result is NULL.
   
   * in Python 2.7 and <= 3.4, the variable _PyThreadState_Current
     is directly available, so use that.

   * in Python 3.5, the variable is available too, but it might be
     the case that the headers don't define it (this changed in 3.5.1).
     In case we're compiling with 3.5.x with x >= 1, we need to
     manually define this variable.

   * in Python >= 3.6 there is _PyThreadState_UncheckedGet().
     It was added in 3.5.2 but should never be used in 3.5.x
     because it is not available in 3.5.0 or 3.5.1.
*/
#if PY_VERSION_HEX >= 0x03050100 && PY_VERSION_HEX < 0x03060000
PyAPI_DATA(void *volatile) _PyThreadState_Current;
#endif

static PyThreadState *get_current_ts(void)
{
#if PY_VERSION_HEX >= 0x03060000
    return _PyThreadState_UncheckedGet();
#elif defined(_Py_atomic_load_relaxed)
    return (PyThreadState*)_Py_atomic_load_relaxed(&_PyThreadState_Current);
#else
    return (PyThreadState*)_PyThreadState_Current;  /* assume atomic read */
#endif
}

static PyGILState_STATE gil_ensure(void)
{
    /* Called at the start of a callback.  Replacement for
       PyGILState_Ensure().
    */
    PyGILState_STATE result;
    struct cffi_tls_s *tls;
    PyThreadState *ts = PyGILState_GetThisThreadState();

    if (ts != NULL) {
        ts->gilstate_counter++;
        if (ts != get_current_ts()) {
            /* common case: 'ts' is our non-current thread state and
               we have to make it current and acquire the GIL */
            PyEval_RestoreThread(ts);
            return PyGILState_UNLOCKED;
        }
        else {
            return PyGILState_LOCKED;
        }
    }
    else {
        /* no thread state here so far. */
        result = PyGILState_Ensure();
        assert(result == PyGILState_UNLOCKED);

        ts = PyGILState_GetThisThreadState();
        assert(ts != NULL);
        assert(ts == get_current_ts());
        assert(ts->gilstate_counter >= 1);

        /* Save the now-current thread state inside our 'local_thread_state'
           field, to be removed at thread shutdown */
        tls = get_cffi_tls();
        if (tls != NULL) {
            tls->local_thread_state = ts;
            ts->gilstate_counter++;
        }

        return result;
    }
}

static void gil_release(PyGILState_STATE oldstate)
{
    PyGILState_Release(oldstate);
}
