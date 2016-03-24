#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#endif

#include "soque.h"

static volatile long long g_proc_count;

static int SOQUE_CALL empty_soque_cb( void * arg, int batch, int waitable )
{
    (void)arg;
    (void)batch;
    (void)waitable;

    return batch;
}

static void SOQUE_CALL empty_soque_proc_cb( void * arg, int batch, int index )
{
    (void)arg;
    (void)batch;
    (void)index;

#ifdef _WIN32
    InterlockedExchangeAdd64( &g_proc_count, batch );
#else
    __sync_fetch_and_add( &g_proc_count, batch );
#endif
}

static soque_push_cb push_cb = &empty_soque_cb;
static soque_proc_cb proc_cb = &empty_soque_proc_cb;
static soque_pop_cb pop_cb = &empty_soque_cb;

#ifdef _WIN32
#define SLEEP_1_SEC Sleep( 1000 )
#define LIBLOAD( name ) LoadLibraryA( name )
#define LIBFUNC( lib, name ) (UINT_PTR)GetProcAddress( lib, name )
#else
#define SLEEP_1_SEC sleep( 1 )
#define LIBLOAD( name ) dlopen( name, RTLD_LAZY )
#define LIBFUNC( lib, name ) dlsym( lib, name )
#endif

static const SOQUE_FRAMEWORK * soq;

int soque_load()
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

    if( soq->soque_major < SOQUE_MAJOR )
    {
        printf( "ERROR: soque version %d.%d < %d.%d\n", soq->soque_major, soq->soque_minor, SOQUE_MAJOR, SOQUE_MINOR );
        return 0;
    }

    printf( "%s (%d.%d) loaded\n", SOQUE_LIBRARY, soq->soque_major, soq->soque_minor );

    return 1;
}

int main( int argc, char ** argv )
{
    SOQUE_HANDLE * q;
    int queue_size = 2048;
    int threads_count = 1;
    int queue_count = 1;
    long long speed_save;
    double speed_change;
    double speed_approx_change;
    double speed_moment = 0;
    double speed_approx = 0;
    int n = 0;
    int i;

    if( argc == 4 )
    {
        queue_size = atoi( argv[1] );
        queue_count = atoi( argv[2] );
        threads_count = atoi( argv[3] );
    }

    if( !soque_load() )
        return 1;

    printf( "queue_size = %d\n", queue_size );
    printf( "queue_count = %d\n", queue_count );
    printf( "threads_count = %d\n\n", threads_count );
    
    q = malloc( queue_count * sizeof( void * ) );

    for( i = 0; i < queue_count; i++ )
        q[i] = soq->soque_open( queue_size, NULL, push_cb, proc_cb, pop_cb );

    soq->soque_threads_open( threads_count, q, queue_count );

    SLEEP_1_SEC; // warming

    for( ;; )
    {
        speed_save = g_proc_count;
        SLEEP_1_SEC;
        speed_change = speed_moment;
        speed_approx_change = speed_approx;
        speed_moment = (double)( g_proc_count - speed_save );
        speed_approx = ( speed_approx * n + speed_moment ) / ( n + 1 );
        printf( "Mpps:   %.03f (%s%0.03f)   ~   %.03f (%s%0.03f)\n", 
                speed_moment / 1000000,
                speed_change <= speed_moment ? "+" : "",
                ( speed_moment - speed_change ) / 1000000,
                speed_approx / 1000000,
                speed_approx_change <= speed_approx ? "+" : "",
                ( speed_approx - speed_approx_change ) / 1000000 );
        n++;
    }
}
