#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <stdlib.h>
#include <unistd.h>
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
#else
#define SLEEP_1_SEC sleep( 1 )
#endif

int main( int argc, char ** argv )
{
    SOQUE_HANDLE q[2];
    int queue_size = 4000;
    int threads_count = 1;
    long long speed_save;
    double speed_change;
    double speed_approx_change;
    double speed_moment = 0;
    double speed_approx = 0;
    int n = 0;

    if( argc == 3 )
    {
        queue_size = atoi( argv[1] );
        threads_count = atoi( argv[2] );
    }

    printf( "queue_size = %d\n", queue_size );
    printf( "threads_count = %d\n\n", threads_count );
    
    q[0] = soque_open( queue_size, NULL, push_cb, proc_cb, pop_cb );
    q[1] = soque_open( queue_size, NULL, push_cb, proc_cb, pop_cb );

    soque_threads_open( threads_count, q, 2 );

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
