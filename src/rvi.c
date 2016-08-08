/* Copyright (c) 2016, Jaguar Land Rover. All Rights Reserved.
 * 
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at http://mozilla.org/MPL/2.0.
 */

/** @file rvi.c
 *
 * This is an initial prototype of the RVI library in C and is subject to
 * change. The intended use is to allow a calling application to connect to a
 * remote RVI node, discover services, register additional services, and invoke
 * remote services.
 *
 * The RVI library depends on the following libraries:
 *
 * libJWT: https://github.com/benmcollins/libjwt/
 * Jansson: http://www.digip.org/jansson/
 * OpenSSL: https://www.openssl.org/
 * mpack: http://ludocode.github.io/mpack/
 *
 * @author Tatiana Jamison &lt;tjamison@jaguarlandrover.com&gt;
 */

#include <dirent.h>
#include <errno.h>
#include <regex.h>
#include <stddef.h>
#include <stdio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "rvi.h"
#include "btree.h"

// ***************
// DATA STRUCTURES
// ***************

/** @brief RVI context */
typedef struct rvi_context_t {

    //
    // Btrees for indexing remote connections, RVI services by name, and RVI
    // services by registrant.
    //
    btree_t *remote_idx;        // Remote connections by fd
    btree_t *service_name_idx;  // Services by fully qualified service name
    btree_t *service_reg_idx;   // Services by fd of registering node -- note:
                                //   local services are associated with stdin

    // Properties set in configuration file
    char *cadir; // Directory containing the trusted certificate store
    char *creddir; // Directory containing base64-encoded JWT credentials
    char *certfile; // File containing X.509 public key certificate (PKC)
    char *keyfile;  // File containing corresponding private key
    char *cafile;   // File containing CA public key certificate(s)
    // Note: certfile, keyfile, and cafile may all point to the same file. If
    // so, it must be .PEM with base64-encoded values. The order must be ONE
    // of the following:
    //      1. Device PKC, device key, intermediate CA PKC(s), root CA PKC
    //      2. Device key, device PKC, intermediate CA PKC(s), root CA PKC
    // Only the first private key in a file will be used. All others will be
    // ignored.

    // Array of RVI credentials loaded into memory for quick access when
    // negotiating connections
    char **cred;

    // SSL context for spawning new sessions. 
    // Contains X509 certs, config settings, etc
    SSL_CTX *ssl_ctx;

    // own right_to_register
    regex_t *right_to_register;
    // own right_to_invoke
    regex_t *right_to_invoke;
} rvi_context_t, *rvi_context_p;

/** @brief Data for connection to remote node */
typedef struct rvi_remote_t {
    /** File descriptor for the connection */
    int fd;
    /** Regex(es) for remote node's right(s) to register */
    regex_t *right_to_register;
    /** Regex(es) for remote node's right(s) to invoke */
    regex_t *right_to_invoke;
    /** Pointer to data buffer for partial I/O operations */
    void *buf;
    /** Pointer to BIO chain from OpenSSL library */
    BIO *sbio;
} rvi_remote_t, *rvi_remote_p;

/** @brief Data for service */
typedef struct rvi_service_t {
    /** The fully-qualified service name */
    char *name;
    /** Array of file descriptors for remote nodes that may register service */
    int *may_register;
    /** Array of file descriptors for remote nodes that may invoke service */
    int *may_invoke;
    /** File descriptor of remote node that registered service */
    int registrant;
    /** Callback function to execute upon service invocation */
    rvi_callback_t callback;
} rvi_service_t, *rvi_service_p;

//
// Declarations for internal functions not exposed in the API
//

rvi_service_t *rvi_service_create ( const char *name, const int registrant, 
                                    const rvi_callback_t callback );

void rvi_service_destroy ( rvi_service_t *service );

rvi_remote_t *rvi_remote_create ( BIO *sbio, const int fd );

void rvi_remote_destroy ( rvi_remote_t *remote);

// Comparison functions for constructing btrees and retrieving values
int compare_fd ( void *a, void *b );

int compare_registrant ( void *a, void *b );

int compare_name ( void *a, void *b );

// Utility functions related to OpenSSL library
int ssl_verify_callback ( int ok, X509_STORE_CTX *store );

SSL_CTX *setup_client_ctx ( rvi_handle handle );

int parse_config ( rvi_handle handle, const char * filename );

/****************************************************************************/

//
// This function compares 2 pointers to rvi_remote_t structures on the basis of
// the remote's file descriptor. It is used for building the index for remote
// connections.
//
int compare_fd ( void *a, void *b )
{
    rvi_remote_t *remote_a = a;
    rvi_remote_t *remote_b = b;

    return ( remote_a->fd - remote_b->fd );
}

//
// This function compares 2 pointers to rvi_service_t structures on the basis
// of the registrant (i.e., file descriptor). For services registered by the
// same remote RVI node, the service name is used to guarantee a unique
// position in the b-tree.
//
int compare_registrant ( void *a, void *b )
{
    rvi_service_t *service_a = a;
    rvi_service_t *service_b = b;

    int result;
    if ((( result = ( service_a->registrant - service_b->registrant)) == 0 ) &&
            service_a->name && service_b->name ) {
        result = strcmp ( service_a->name, service_b->name );
    }

    return result;
}

//
// This function will compare 2 pointers to rvi_service_t structures on the
// basis of the unique fully-qualified service name.
//
int compare_name ( void *a, void *b )
{
    rvi_service_t *service_a = a;
    rvi_service_t *service_b = b;

    return strcmp ( service_a->name, service_b->name );
}

//
// This function initializes a new service struct and sets the name,
// registrant, and callback to the specified values.
//
// If service name is null or registrant is negative, this returns NULL and
// performs no operations.
//

rvi_service_t *rvi_service_create ( const char *name, const int registrant, 
                                    const rvi_callback_t callback )
{
    // If name is NULL or registrant is negative, there's an error
    if ( !name || (registrant < 0) )
        return NULL;

    // Zero-initialize the struct
    rvi_service_t *service = malloc( sizeof ( rvi_service_t ) );
    memset(service, 0, sizeof ( rvi_service_t ) );

    // Set the service name
    service->name = strdup ( name );

    // Set the service registrant
    service->registrant = registrant;

    // Set the callback. NULL is valid.
    service->callback = callback;

    // TODO: Walk the tree of remotes to check for matches in may_register and
    // may_invoke to add to array

    // Return the address of the new service
    return service;
}

//
// This function frees all memory allocated by a service struct.
//
// If service is null, no operations are performed.
//
void rvi_service_destroy ( rvi_service_t *service )
{
     if ( !service ) {
         return;
     }

     free ( service->name );
     free ( service->may_register );
     free ( service->may_invoke );
     free ( service );
}

// 
// This function initializes a new remote struct and sets the file descriptor
// and BIO chain to the specified values.
//
// If sbio is null or fd is negative, this returns NULL and performs no
// operations.
//

rvi_remote_t *rvi_remote_create ( BIO *sbio, const int fd )
{
    // If sbio is null or fd is negative, there's a problem
    if ( !sbio || fd < 0 ) {
        return NULL;
    }
    
    // Create a new data structure and zero-initialize it
    rvi_remote_t *remote = malloc ( sizeof ( rvi_remote_t ) );
    memset ( remote, 0, sizeof ( rvi_remote_t ) );

    //
    // Set the file descriptor and BIO chain
    //
    remote->fd = fd;
    remote->sbio = sbio;

    // Note that we do NOT need to populate right_to_register or
    // right_to_invoke at this time. Those will be populated by parsing the au
    // message.

    return remote;
}

//
// This function frees all memory allocated by a remote struct.
//
// If remote is null, no operations are performed.
//
void rvi_remote_destroy ( rvi_remote_t *remote)
{
    if ( !remote ) {
        return;
    }

    BIO_free_all ( remote->sbio );
    free ( remote->right_to_register );
    free ( remote->right_to_invoke );
    free ( remote->buf );
    free ( remote );
}

// ***************************
// INITIALIZATION AND TEARDOWN
// ***************************


int ssl_verify_callback ( int ok, X509_STORE_CTX *store )
{
    char data[256];

    if(!ok) {
        X509 *cert = X509_STORE_CTX_get_current_cert(store);
        int depth = X509_STORE_CTX_get_error_depth(store);
        int err = X509_STORE_CTX_get_error(store);

        fprintf(stderr, "-Error with certificate at depth: %i\n", depth);
        X509_NAME_oneline(X509_get_issuer_name(cert), data, 256);
        fprintf(stderr, " issuer = %s\n", data);
        X509_NAME_oneline(X509_get_subject_name(cert), data, 256);
        fprintf(stderr, " subject = %s\n", data);
        fprintf(stderr, " err %i:%s\n", err, 
                X509_verify_cert_error_string(err));
    }

    return ok;
}

//
// Set up the SSL context. Configure for outbound connections only.
//
SSL_CTX *setup_client_ctx ( rvi_handle handle )
{
    if ( !handle ) {
        return NULL;
    }

    rvi_context_t *rvi_ctx = (rvi_context_t *)handle;

    SSL_CTX *ssl_ctx;

    // Use generic SSL/TLS so we can easily add additional future protocols
    ssl_ctx = SSL_CTX_new( SSLv23_method() );
    // Do not permit the deprecated SSLv2 or SSLv3 to be used. Also prohibit
    // TLSv1.0 and TLSv1.1.
    SSL_CTX_set_options( ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
                             SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 );

    // Specify winnowed cipher list here
    // if(SSL_CTX_set_cipher_list(ctx, cipher_list) != 1) { return NULL; }

    if( SSL_CTX_load_verify_locations(ssl_ctx, rvi_ctx->cafile, 
                                      rvi_ctx->cadir) != 1 ) {
        return NULL;
    }
    if( SSL_CTX_set_default_verify_paths(ssl_ctx) != 1 ) {
        return NULL;
    }
    if( SSL_CTX_use_certificate_chain_file(ssl_ctx, 
                                           rvi_ctx->certfile) != 1 ) {
        return NULL;
    }
    if( SSL_CTX_use_PrivateKey_file(ssl_ctx, rvi_ctx->keyfile, 
                                    SSL_FILETYPE_PEM) != 1 ) {
        return NULL;
    }

    // Set internal callback for peer verification on SSL connection attempts.
    SSL_CTX_set_verify ( ssl_ctx, SSL_VERIFY_PEER, ssl_verify_callback );

    // Set the maximum depth for certificates to be used. Additional
    // certificates are ignored. Error messages will be generated as if the
    // certificates are not present.
    //
    // Permits a maximum of 4 CA certificates, i.e., 3 intermediate CAs and the
    // root CA.
    SSL_CTX_set_verify_depth ( ssl_ctx, 4 );

    return ssl_ctx;
}

//
// This function will parse a flat (sysctl-style) configuration file to
// retrieve the filenames for the device certificate and key, as well as the
// directory names for CA certificates and RVI credentials.
//
// On success, this function returns 0. On error, it will return a positive
// error code.
//
int parse_config ( rvi_handle handle, const char * filename )
{
    if ( !handle || !filename ) {
        return EINVAL;
    }

    rvi_context_t * ctx;
    char            key[100] = {0};
    char            val[100] = {0};
    char            cred[2048] = {0};
    FILE *          fp;
    DIR *           d;
    struct dirent * dir;

    ctx = (rvi_context_t *)handle;

    fp = fopen ( filename, "r" );

    if ( !fp ) {
        return errno;
    }

    while ( fscanf ( fp, "%s = %s", key, val ) == 2 ) {
        if ( strcmp ( key, "device.key" ) == 0 ) {
            ctx->keyfile = strdup ( val );
        } else if ( strcmp ( key, "device.cert" ) == 0 ) {
            ctx->certfile = strdup ( val );
        } else if ( strcmp ( key, "ca.dir" ) == 0 ) {
            ctx->cadir = strdup ( val );
        } else if ( strcmp ( key, "cred.dir" ) == 0 ) {
            ctx->creddir = strdup ( val );
        } else if ( strcmp ( key, "ca.cert" ) == 0 ) {
            ctx->cafile = strdup ( val );
        }

        memset ( key, 0, 100 );
        memset ( val, 0, 100 );
    }

    fclose ( fp );
    
    if ( !(ctx->creddir) ) {
        return RVI_ERROR_NOCRED;
    }

    d = opendir ( ctx->creddir );
    if ( !d ) {
        return RVI_ERROR_NOCRED;
    }

    int i = 0;
    while ( ( dir = readdir ( d ) ) != NULL ) {
        if ( strstr ( dir->d_name, ".jwt" ) != NULL ) {
            fp = fopen ( dir->d_name, "r" );
            fscanf ( fp, "%s", cred);
            ctx->cred[i] = strdup ( cred );
            i++;
            fclose ( fp );
        }
    }

    closedir ( d );

    return RVI_OK;
}

/** @brief Initialize the RVI library. Call before using any other functions.
 *
 * @param config_filename - Path to the file containing RVI config options:
 *                          credentials - JWT encoded string
 *                          device_cert - file with device's X.509 cert
 *                          device_key - file with device's private key
 *                          intermediateCA - file with intermediate CA certs
 *                          root_cert - file with root cert
 *
 * @return A handle for the API. On failure, a NULL pointer will be returned.
 */

rvi_handle rvi_init ( char *config_filename )
{
    // set alloc funcs for Jansson
    // json_set_alloc_funcs(s_malloc, s_free);

    // initialize OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    
    // Allocate memory for an RVI context structure. 
    // This structure contains:
    //      lookup trees for services and remote connections
    //      shared SSL context factory object for generating new SSL sessions
    //      this node's permissions in the RVI architecture
    rvi_context_t *ctx = malloc(sizeof(rvi_context_t));
    if(!ctx) {
        printf("Error allocating memory.\n");
        return NULL;
    }
    ctx = memset ( ctx, 0, sizeof ( rvi_context_t ) );

    // Allocate a block of memory for storing credentials, then initialize each
    // pointer to null
    ctx->cred = malloc ( 20 * sizeof ( char * ));
    int i;
    for ( i = 0; i < 20; i ++) {
        ctx->cred[i] = NULL;
    }

    // parse config file
    // need: device cert; root cert; device key; credential
    
    if ( parse_config ( ctx, "rvi.config" ) != 0 ) {
        fprintf(stderr, "Error reading config file\n");
        free(ctx->cred);
        free(ctx);
        return NULL;
    }

    // Create generic SSL context configured for client access
    ctx->ssl_ctx = setup_client_ctx(ctx);
    if(!ctx->ssl_ctx) {
        printf("Error setting up SSL context\n");
        return NULL;
    }

    // 
    // Create empty btrees for indexing remote connections and services.
    //
    // Since we expect that records will frequently be added and removed, use a
    // small order for each tree. This means that the tree will be deeper, but
    // addition/deletion will usually result in simply changing pointers rather
    // than copying data.
    //
    
    //
    // Remote connections will be indexed by the socket's file descriptor.
    //
    ctx->remote_idx = btree_create(2, compare_fd);

    //
    // Services will be indexed by the fully-qualified service name, which is
    // unique across the RVI infrastructure.
    //
    ctx->service_name_idx = btree_create(2, compare_name);

    //
    // Services will also be indexed by the file descriptor of the entity
    // registering the service. Service names are used as a tie-breaker to
    // ensure each record has a unique position in the tree.
    //
    ctx->service_reg_idx = btree_create(2, compare_registrant);
    
    return (rvi_handle)ctx;
}

/** @brief Tear down the API.
 *
 * Calling applications are expected to call this to cleanly tear down the API.
 *
 * @param handle - The handle for the RVI context to clean up.
 *
 * @return 0 (RVI_OK) on success
 *         Error code on failure.
 */

int rvi_cleanup(rvi_handle handle)
{
    rvi_context_t * ctx = (rvi_context_p)handle;
    rvi_remote_t    rkey = {0};
    rvi_remote_t *  rtmp;
    rvi_service_t   skey = {0};
    rvi_service_t * stmp;
    btree_iter      iter;

    // free all SSL structs
    SSL_CTX_free(ctx->ssl_ctx);

    // 
    // Destroy each tree, including all structs pointed to
    //
    
    // 
    // As long as the context contains remote connections, find the first
    // struct in the tree, and disconnect the corresponding file descriptor.
    // The disconnect function removes the entry from the tree and frees the
    // underlying memory.
    // 
    while(ctx->remote_idx->count != 0) {
        rkey.fd = 0;
        if((iter = btree_find(ctx->remote_idx, &rkey))) {
            rtmp = btree_iter_data ( iter );
            if(!rtmp) {
                perror("Getting remote data in cleanup"); 
                break;
            }
            // Disconnect the remote SSL connection, delete the entry from the
            // tree & free the remote struct
            rvi_disconnect(handle, rtmp->fd);
        }
        free(iter);
    }
    btree_destroy(ctx->remote_idx);

    //
    // As long as the context contains services, find the first struct from
    // either service tree. Delete the entry from each service tree, then free
    // the underlying memory.
    //
    while(ctx->service_name_idx->count != 0) {
        skey.name = "";
        if (( iter = btree_find ( ctx->service_name_idx, &skey ))) {
            // We found a service at the beginning of the tree
            stmp = btree_iter_data ( iter );
            if ( !stmp ) {
                perror("Getting service data in cleanup"); 
                break;
            }
            // Delete the entry from the service name index
            btree_delete ( ctx->service_name_idx, 
                           ctx->service_name_idx->root, stmp);
            // Delete the entry from the service registrant index
            btree_delete ( ctx->service_reg_idx, 
                           ctx->service_reg_idx->root, stmp);
            // Free the service memory
            rvi_service_destroy ( stmp );
        }
    }

    //
    // Both trees should now be empty, so destroy the trees
    //
    btree_destroy(ctx->service_name_idx);

    btree_destroy(ctx->service_reg_idx);

    //
    // Free all credentials and other entities set when parsing config
    //
    int i = 0;
    while ( ctx->cred[i] != NULL ) {
        free ( ctx->cred[i] );
        i++;
    }
    free ( ctx->cred );

    free ( ctx->certfile );
    free ( ctx->keyfile );
    free ( ctx->cafile );
    free ( ctx->cadir );
    free ( ctx->creddir );

    // 
    // Free the memory allocated to the rvi_context_t struct
    //
    free(ctx);

    return RVI_OK;
}

// *************************
// RVI CONNECTION MANAGEMENT
// *************************

/** @brief Connect to a remote node at a specified address and port. 
 *
 * This function will attempt to connect to a remote node at the specified addr
 * and port. It will spawn a new connection and block until all handshake and
 * RVI negotiations are complete. On success, it will return the file
 * descriptor for the new socket. On failure, it will return a negative error
 * value. 
 *
 * New services may become immediately available upon connecting to a remote
 * node. To discover the services that are currently available, use the
 * rvi_get_services() function. Services may be invoked via
 * rvi_invoke_remote_service() using the fully-qualified service name.
 *
 * @param handle - The handle to the RVI context.
 * @param addr - The address of the remote connection.
 * @param port - The target port for the connection.
 *
 * @return A file descriptor for the new socket on success.
 *         A negative error value on failure.
 */
int rvi_connect(rvi_handle handle, const char *addr, const char *port)
{
    //
    // Ensure that we have received valid arguments
    //
    if( !handle || !addr || !port ) {
        return -EINVAL;
    }

    BIO*            sbio;
    SSL*            ssl;
    rvi_remote_t*   remote;
    rvi_context_t*  rvi;
    int             len;
    char            tmpbuf[2048];

    rvi = (rvi_context_t *)handle;

    //
    // Spawn new SSL session from handle->ctx. BIO_new_ssl_connect spawns a new
    // chain including a BIO and an SSL object
    //
    sbio = BIO_new_ssl_connect(rvi->ssl_ctx);
    if(!sbio) {
        fprintf(stderr, "Can't locate BIO chain\n");
        ERR_print_errors_fp(stderr);
        return -RVI_ERROR_OPENSSL;
    }
    BIO_get_ssl(sbio, &ssl);
    if(!ssl) {
        fprintf(stderr, "Can't locate SSL pointer\n");
        ERR_print_errors_fp(stderr);
        BIO_free_all(sbio);
        return -RVI_ERROR_OPENSSL;
    }

    //
    // When performing I/O, automatically retry all reads and complete
    // negotiations before returning. Note that all BIOs have their I/O flag
    // set to blocking by default.
    //
    SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);

    //
    // Set the addr and port
    //
    BIO_set_conn_hostname(sbio, addr);
    BIO_set_conn_port(sbio, port);

    if(BIO_do_connect(sbio) <= 0) {
        fprintf(stderr, "Error connecting to server\n");
        ERR_print_errors_fp(stderr);
        BIO_free_all(sbio);
        return -1;
    }

    if(BIO_do_handshake(sbio) <= 0) {
        fprintf(stderr, "Error establishing SSL connection\n");
        ERR_print_errors_fp(stderr);
        BIO_free_all(sbio);
        return -1;
    }

    remote = rvi_remote_create ( sbio, SSL_get_fd ( ssl ) );

    //
    // Add this data structure to our lookup tree
    //
    btree_insert(rvi->remote_idx, remote);
    
    // set remote->rvi_log_id
    
    // prepare "au" message
    //      populate cmd
    //      populate version
    //      fill with handle->credentials
    //      fill with remote->rvi_log_id
    
    char au[500] = {0};
    sprintf(au, "{\"cmd\":\"au\",\"ver\":\"1.1\",\"creds\":  [\"%s\"]"
            "}", rvi->cred[0]);

    // send "au" message
    BIO_puts(sbio, au);

    BIO*            out; 

    // parse incoming "au" message
    len = BIO_read(sbio, tmpbuf, 2048);
    if( len <= 0 ) {
        printf("Error receiving server reply\n");
    } else {
        // We received a reply from the server, we expect it to be an "au" msg
        // For debug purposes, echo all messages we get from the server to stdout
        out = BIO_new_fp(stdout, BIO_NOCLOSE);
        BIO_write(out, tmpbuf, len);
        BIO_free(out);
        //      parse right_to_register to regex_t
        //      set remote->right_to_register to returned value
        //      parse right_to_invoke to regex_t
        //      set remote->right_to_invoke to returned value
    }

    // prepare "sa" reply
    //      search services_by_may_register to match remote->right_to_invoke
    //      if the registrant is local, add service name to "sa" reply
    // send "sa" reply
    // parse incoming "sa" message
    //      for each service in services array, create new rvi_service_t
    //      set new_service->name to service string
    //      set new_service->registrant to remote->fd
    //      search connections_by_right_to_register to match name
    //          for each match, add to new_service->may_register
    //      search connections_by_right_to_invoke to match name
    
//          for each match, add to new_service->may_invoke
    // return remote->fd
    return remote->fd;
}

/** @brief Disconnect from a remote node with a specified file descriptor. 
 *
 * @param handle - The handle to the RVI context.
 * @param fd - The file descriptor for the connection to terminate.
 *
 * @return 0 (RVI_OK)  on success.
 *         Error code on failure.
 */
int rvi_disconnect(rvi_handle handle, int fd)
{
    rvi_context_t * ctx = (rvi_context_t *)handle;
    rvi_remote_t    rkey = {0};
    rvi_remote_t *  rtmp;
    rvi_service_t   skey = {0};
    rvi_service_t * stmp;
    int             res;
    
    rkey.fd = fd;

    rtmp = btree_search(ctx->remote_idx, &rkey);
    if(!rtmp) {
        printf("No such connection\n");
        return -1;
    }

    if( ( res = btree_delete(ctx->remote_idx, 
                             ctx->remote_idx->root, rtmp ) ) < 0 ) {
        printf("Error deleting remote key from tree\n");
        return -1;
    } else {
        // Close the connection and free the OpenSSL BIO chain
        BIO_free_all(rtmp->sbio);

        // Search the service tree for any services registered by the remote
        skey.registrant = fd;
        while((stmp = btree_search(ctx->service_reg_idx, &skey))) {
            // We have a match, so delete the service and free the node from the tree
            if((res = btree_delete(ctx->service_reg_idx, 
                                   ctx->service_reg_idx->root, stmp)) < 0) {
                printf("Error deleting service key from tree\n");
            } else {
                // Also free all memory for the service structure
                rvi_service_destroy(stmp);
            }
        }

       // free ( rtmp->right_to_register );
       // free ( rtmp->right_to_invoke );
        free ( rtmp );
    }

    return RVI_OK;
}

/** @brief Return all file descriptors in the RVI context
 *
 * @param handle    - The handle to the RVI context.
 * @param conn      - Pointer to a buffer to store file descriptors (small
 *                    integers) for each remote RVI node.  
 * @param conn_size - Pointer to size of 'conn' buffer. This should be
 *                    initialized to the size of the conn buffer. On success,
 *                    it will be updated with the number of file descriptors
 *                    updated.
 *
 * This function will fill the conn buffer with active file descriptors from
 * the RVI context and update conn_size to indicate the final size.
 *
 * @return 0 (RVI_OK) on success.
 *         Error code on failure.
 */
int rvi_get_connections(rvi_handle handle, int *conn, int *conn_size)
{
    printf("write the get connections function...\n");
    return RVI_OK;
}


// **********************
// RVI SERVICE MANAGEMENT
// **********************

/** @brief Register a service with a callback function
 *
 * @param handle - The handle to the RVI context.
 * @param service_name - The fully-qualified service name to register
 * @param callback - The callback function to be executed upon service
 *                   invocation.
 *
 * @return 0 (RVI_OK) on success 
 *         Error code on failure.
 */
int rvi_register_service(rvi_handle handle, const char *service_name, 
                         rvi_callback_t callback, void *service_data)
{
    // Compare service name to handle->right_to_register
    // If no match, return error
    // Create a new rvi_service_t structure
    // Set service->name to service_name
    // Set service->callback to callback
    // Set service->registrant to stdin
    // Add stdin to service->may_register
    // Search remotes by right_to_invoke; add to service->may_invoke
    // Search remotes by right_to_register; add to service->may_register
    // If service->may_invoke is non-empty, prepare sa message
    //      For each fd in service->may_invoke,
    //      send sa message making service stat av
    // Add service to services_by_name
    // Add service to services_by_registrant
    printf("Write the register service function.\n");
    return 0;
}

/** @brief Unregister a previously registered service
 *
 * @param handle - The handle to the RVI context
 * @param service_name The fully-qualified service name to deregister
 *
 * @return 0 (RVI_OK) on success. 
 *         Error code on failure.
 */
int rvi_unregister_service(rvi_handle handle, const char *service_name)
{
    // if service_name is not in services_by_name, return error
    // if service->registrant is not stdin, return error
    // if service->may_invoke is not empty, prepare sa message
    //      for each fd in service->may_invoke
    //      send sa message making service_name stat un
    // Remove service from services_by_name
    // Remove service from services_by_fd
    // Free service
    printf("Write the unregister service function.\n");
    return 0;
}



/** @brief Get list of services available
 *
 * This function fills the buffer at result with pointers to strings, up to the
 * value indicated by len. Memory for each string is dynamically allocated by
 * the library and must be freed by the calling application. Before returning,
 * len is updated with the actual number of strings.
 * 
 * @param handle - The handle to the RVI context.
 * @param result - A pointer to a block of pointers for storing strings
 * @param len - The maximum number of pointers allocated in result
 *
 * @return 0 (RVI_OK) on success
 *         Error code on failure.
 */
int rvi_get_services(rvi_handle handle, char **result, int *len)
{
    // for (i = 0; i < len; i++)
    //      get next service name from service name index
    //      *result++ = strdup service name
    // *len = i;
    // return RVI_OK;
    printf("Write the get services function.\n");
    return RVI_OK;
}

/** @brief Invoke a remote service
 *
 * @param handle - The handle to the RVI context.
 * @param service_name - The fully-qualified service name to invoke 
 * @param parameters - A JSON structure containing the named parameter pairs
 *
 * @return 0 on success. Error code on failure.
 */
int rvi_invoke_remote_service(rvi_handle handle, const char *service_name, 
                              const json_t *parameters)
{
    // get service from service name index
    //  if not found, return error
    // identify registrant, get SSL session from remote index
    // prepare rcv message
    // send rcv message to registrant
    printf("Write the invoke service function.\n");
    return 0;
}

// **************
// I/O MANAGEMENT
// **************

int rvi_process_input(rvi_handle handle, int *fd_arr, int fd_len)
{
    // For each fd...
    //      Look up SSL session by fd from remote_idx
    //      Store its current blocking/nonblocking status
    //      Set it to blocking
    //
    //      Perform SSL_read
    //          if au:
    //              perform all au work
    //          if sa:
    //              validate service name(s) against remote's right_to_register
    //              update service list
    //          if rcv:
    //              validate service name(s) against remote's right_to_invoke
    //              look up service by name
    //              invoke callback, passing parameters (if any)
    //          if ping:
    //              return ping
    //      
    //      Set fd to stored blocking/nonblocking status
    printf("Finish the process input function...\n");
    return 0;
}