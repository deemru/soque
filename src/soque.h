#ifdef __cplusplus
extern "C" {
#endif

#ifdef WIN32
#define EXPORT_FUNCTION __declspec( dllexport )
#else
#define EXPORT_FUNCTION __attribute__( ( visibility( "default" ) ) )
#endif

#define SOQUE_CALL __fastcall

    typedef int ( SOQUE_CALL * soque_push_batch_cb )( void * cb_arg, int push_count );
    typedef int ( SOQUE_CALL * soque_proc_batch_cb )( void * cb_arg, int proc_count );
    typedef int ( SOQUE_CALL * soque_pop_batch_cb )( void * cb_arg, int pop_count );

    typedef struct SOQUE * SOQUE_HANDLE;

    EXPORT_FUNCTION SOQUE_HANDLE SOQUE_CALL soque_open( int queue_size, int max_threads,
                                                        void * cb_arg,
                                                        soque_push_batch_cb push_cb,
                                                        soque_push_batch_cb proc_cb,
                                                        soque_push_batch_cb pop_cb );
    EXPORT_FUNCTION int SOQUE_CALL soque_push_batch( SOQUE_HANDLE sh, int push_count );
    EXPORT_FUNCTION int SOQUE_CALL soque_proc_get( SOQUE_HANDLE sh );
    EXPORT_FUNCTION int SOQUE_CALL soque_proc_sync( SOQUE_HANDLE sh, int proc_done );    
    EXPORT_FUNCTION int SOQUE_CALL soque_pop_batch( SOQUE_HANDLE sh, int pop_count );
    EXPORT_FUNCTION void SOQUE_CALL soque_close( SOQUE_HANDLE sh );

    typedef struct SOQUE_THREADS * SOQUE_THREADS_HANDLE;

    EXPORT_FUNCTION SOQUE_THREADS_HANDLE SOQUE_CALL soque_threads_open( int threads_count, SOQUE_HANDLE * shs, int shs_count );
    EXPORT_FUNCTION void SOQUE_CALL soque_threads_close( SOQUE_THREADS_HANDLE sth );

#ifdef __cplusplus
}
#endif
