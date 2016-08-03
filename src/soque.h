#ifndef SOQUE_H
#define SOQUE_H

#define SOQUE_MAJOR 0
#define SOQUE_MINOR 2

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define SOQUE_API __declspec( dllexport )
#define SOQUE_CALL __fastcall
#ifdef _M_IX86
#define SOQUE_LIBRARY "soque.dll"
#else // _M_IX86
#define SOQUE_LIBRARY "soque64.dll"
#endif // _M_IX86
#else // _WIN32
#define SOQUE_API __attribute__( ( visibility( "default" ) ) )
#define SOQUE_CALL 
#define SOQUE_LIBRARY "libsoque.so"
#endif // _WIN32
#define SOQUE_GET_FRAMEWORK "soque_framework"

    typedef int ( SOQUE_CALL * soque_push_cb )( void * cb_arg, unsigned batch, char waitable );
    typedef void ( SOQUE_CALL * soque_proc_cb )( void * cb_arg, unsigned batch, unsigned index );
    typedef int ( SOQUE_CALL * soque_pop_cb )( void * cb_arg, unsigned batch, char waitable );

    typedef struct SOQUE * SOQUE_HANDLE;

    typedef SOQUE_HANDLE ( SOQUE_CALL * soque_open_t )( unsigned size, void * cb_arg, soque_push_cb, soque_proc_cb, soque_pop_cb );
    typedef char ( SOQUE_CALL * soque_lock_t )( SOQUE_HANDLE );
    typedef unsigned ( SOQUE_CALL * soque_push_t )( SOQUE_HANDLE, unsigned batch );
    typedef unsigned ( SOQUE_CALL * soque_proc_open_t )( SOQUE_HANDLE, unsigned batch, unsigned * index );
    typedef char ( SOQUE_CALL * soque_proc_done_t )( SOQUE_HANDLE, unsigned batch, unsigned index );
    typedef unsigned ( SOQUE_CALL * soque_pop_t )( SOQUE_HANDLE, unsigned batch );
    typedef void ( SOQUE_CALL * soque_unlock_t )( SOQUE_HANDLE );
    typedef void ( SOQUE_CALL * soque_done_t )( SOQUE_HANDLE );

    typedef struct SOQUE_THREADS * SOQUE_THREADS_HANDLE;

    typedef SOQUE_THREADS_HANDLE ( SOQUE_CALL * soque_threads_open_t )( unsigned threads, char bind, SOQUE_HANDLE * shs, unsigned shs_count );
    typedef void ( SOQUE_CALL * soque_threads_tune_t )( SOQUE_THREADS_HANDLE, unsigned batch, unsigned threshold, unsigned reaction );
    typedef void ( SOQUE_CALL * soque_threads_done_t )( SOQUE_THREADS_HANDLE );

    typedef struct {
        unsigned soque_major;
        unsigned soque_minor;
        soque_open_t soque_open;
        soque_lock_t soque_lock;
        soque_push_t soque_push;
        soque_proc_open_t soque_proc_open;
        soque_proc_done_t soque_proc_done;
        soque_pop_t soque_pop;
        soque_unlock_t soque_unlock;
        soque_done_t soque_done;
        soque_threads_open_t soque_threads_open;
        soque_threads_tune_t soque_threads_tune;
        soque_threads_done_t soque_threads_done;
    } SOQUE_FRAMEWORK;

    typedef SOQUE_FRAMEWORK * ( * soque_framework_t )();
    SOQUE_API const SOQUE_FRAMEWORK * soque_framework();

#ifdef __cplusplus
}
#endif

#ifdef SOQUE_WITH_LOADER

#ifdef _WIN32
#define LIBLOAD( name ) LoadLibraryA( name )
#define LIBFUNC( lib, name ) (UINT_PTR)GetProcAddress( lib, name )
#else
#define LIBLOAD( name ) dlopen( name, RTLD_LAZY )
#define LIBFUNC( lib, name ) dlsym( lib, name )
#endif

static const SOQUE_FRAMEWORK * soq;

static char soque_load()
{
    soque_framework_t soque_get_framework;

    void * lib = LIBLOAD( SOQUE_LIBRARY );

    if( !lib )
    {
        printf( "ERROR: \"%s\" not loaded\n", SOQUE_LIBRARY );
        return 0;
    }

    soque_get_framework = (soque_framework_t)LIBFUNC( lib, SOQUE_GET_FRAMEWORK );

    if( !soque_get_framework )
    {
        printf( "ERROR: \"%s\" not found in \"%s\"\n", SOQUE_GET_FRAMEWORK, SOQUE_LIBRARY );
        return 0;
    }

    soq = soque_get_framework();

    if( soq->soque_major != SOQUE_MAJOR )
    {
        printf( "ERROR: soque major version %d.%d != %d.%d\n", soq->soque_major, soq->soque_minor, SOQUE_MAJOR, SOQUE_MINOR );
        return 0;
    }

#if SOQUE_MINOR
    if( soq->soque_minor < SOQUE_MINOR )
    {
        printf( "WARNING: soque minor version %d.%d < %d.%d\n", soq->soque_major, soq->soque_minor, SOQUE_MAJOR, SOQUE_MINOR );
    }
#endif

    printf( "SUCCESS: %s (%d.%d) loaded\n", SOQUE_LIBRARY, soq->soque_major, soq->soque_minor );

    return 1;
}

#endif // SOQUE_WITH_LOADER

#endif // SOQUE_H
