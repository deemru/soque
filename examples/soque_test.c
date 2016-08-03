#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#endif

#define SOQUE_WITH_LOADER
#include "soque.h"

static volatile long long g_proc_count;
unsigned long long hard = 1000;

#ifdef _WIN32
#define rdtsc() __rdtsc()
#else
#define rdtsc() __builtin_ia32_rdtsc()
#endif

static int SOQUE_CALL empty_soque_cb( void * arg, unsigned batch, char waitable )
{
    (void)arg;
    (void)batch;
    (void)waitable;

    if( hard )
    {
        unsigned long long c = rdtsc();
        unsigned long long h = hard * batch / 10;
        while( rdtsc() - c < h ){}
    }

    return batch;
}

static void SOQUE_CALL empty_soque_proc_cb( void * arg, unsigned batch, unsigned index )
{
    (void)arg;
    (void)batch;
    (void)index;

#ifdef _WIN32
    InterlockedExchangeAdd64( &g_proc_count, batch );
#else
    __sync_fetch_and_add( &g_proc_count, batch );
#endif

    if( hard )
    {
        unsigned long long c = rdtsc();
        unsigned long long h = hard * batch;
        while( rdtsc() - c < h ){}
    }
}

static soque_push_cb push_cb = &empty_soque_cb;
static soque_proc_cb proc_cb = &empty_soque_proc_cb;
static soque_pop_cb pop_cb = &empty_soque_cb;

#ifdef _WIN32
#define SLEEP_1_SEC Sleep( 1000 )
#else
#define SLEEP_1_SEC sleep( 1 )
#endif

int main( int argc, char ** argv )
{
    SOQUE_HANDLE * q;
    SOQUE_THREADS_HANDLE qt;
    int queue_size = 2048;
    int queue_count = 1;
    int threads_count = 16;
    char bind = 1;
    unsigned fast_batch = 64;
    unsigned help_batch = 64;
    unsigned threshold = 10000;
    unsigned reaction = 50;

    long long speed_save;
    double speed_change;
    double speed_approx_change;
    double speed_moment = 0;
    double speed_approx = 0;
    int n = 0;
    int i;

    if( argc > 1 )
        queue_size = atoi( argv[1] );
    if( argc > 2 )
        queue_count = atoi( argv[2] );
    if( argc > 3 )
        threads_count = atoi( argv[3] );
    if( argc > 4 )
        bind = (char)atoi( argv[4] );
    if( argc > 5 )
        fast_batch = atoi( argv[5] );
    if( argc > 6 )
        help_batch = atoi( argv[6] );
    if( argc > 7 )
        threshold = atoi( argv[7] );
    if( argc > 8 )
        reaction = atoi( argv[8] );
    if( argc > 9 )
        hard = atoi( argv[9] );

    if( !soque_load() )
        return 1;

    printf( "queue_size = %d\n", queue_size );
    printf( "queue_count = %d\n", queue_count );
    printf( "threads_count = %d\n", threads_count );
    printf( "bind = %d\n", bind );
    printf( "fast_batch = %d\n", fast_batch );
    printf( "help_batch = %d\n", help_batch );
    printf( "threshold = %d\n", threshold );
    printf( "reaction = %d\n", reaction );
    printf( "hard = %d\n\n", (int)hard );
    
    q = malloc( queue_count * sizeof( void * ) );

    for( i = 0; i < queue_count; i++ )
        q[i] = soq->soque_open( queue_size, NULL, push_cb, proc_cb, pop_cb );

    qt = soq->soque_threads_open( threads_count, bind, q, queue_count );
    soq->soque_threads_tune( qt, fast_batch, help_batch, threshold, reaction );

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
