#include <bson.h>
#include <errno.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "ssl-test.h"

#define TIMEOUT 1000

#define LOCALHOST "127.0.0.1"

typedef struct ssl_test_data
{
   mongoc_ssl_opt_t  *client;
   mongoc_ssl_opt_t  *server;
   const char        *host;
   unsigned short     server_port;
   mongoc_cond_t        cond;
   mongoc_mutex_t       cond_mutex;
   ssl_test_result_t *client_result;
   ssl_test_result_t *server_result;
} ssl_test_data_t;

/** this function is meant to be run from ssl_test as a child thread
 *
 * It:
 *    1. spins up
 *    2. binds and listens to a random port
 *    3. notifies the client of it's port through a condvar
 *    4. accepts a request
 *    5. reads a 32 bit length
 *    6. reads a string of that length
 *    7. echos it back to the client
 *    8. shuts down
 */
static void *
ssl_test_server (void * ptr)
{
   ssl_test_data_t *data = (ssl_test_data_t *)ptr;

   mongoc_stream_t *sock_stream;
   mongoc_stream_t *ssl_stream;
   mongoc_fd_t conn_fd;
   mongoc_fd_t listen_fd;
   socklen_t sock_len;
   char buf[1024];
   ssize_t r;
   struct iovec iov = { buf, sizeof(buf) };
   struct sockaddr_in server_addr = { 0 };

   listen_fd = mongoc_socket(AF_INET, SOCK_STREAM, 0);
   assert(mongoc_fd_is_valid(listen_fd));

   server_addr.sin_family = AF_INET;
   server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
   server_addr.sin_port = htons(0);

   r = mongoc_bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
   assert(r == 0);

   sock_len = sizeof(server_addr);
   r = mongoc_getsockname(listen_fd, (struct sockaddr *)&server_addr, &sock_len);
   assert(r == 0);

   r = mongoc_listen(listen_fd, 10);
   assert(r == 0);

   mongoc_mutex_lock(&data->cond_mutex);
   data->server_port = ntohs(server_addr.sin_port);
   mongoc_cond_signal(&data->cond);
   mongoc_mutex_unlock(&data->cond_mutex);

   conn_fd = mongoc_accept(listen_fd, (struct sockaddr*)NULL, NULL);
   assert(mongoc_fd_is_valid(conn_fd));

   sock_stream = mongoc_stream_unix_new(conn_fd);
   assert(sock_stream);
   ssl_stream = mongoc_stream_tls_new(sock_stream, data->server, 0);
   if (! ssl_stream) {
      unsigned long err = ERR_get_error();
      assert(err);

      data->server_result->ssl_err = err;
      data->server_result->result = SSL_TEST_SSL_INIT;

      mongoc_stream_destroy(sock_stream);

      mongoc_close(listen_fd);

      return NULL;
   }
   assert(ssl_stream);

   r = mongoc_stream_tls_do_handshake (ssl_stream, TIMEOUT);
   if (! r) {
      unsigned long err = ERR_get_error();
      assert(err);

      data->server_result->ssl_err = err;
      data->server_result->result = SSL_TEST_SSL_HANDSHAKE;

      mongoc_stream_destroy(ssl_stream);
      mongoc_close(listen_fd);

      return NULL;
   }

   int len;
   r = mongoc_stream_readv(ssl_stream, &iov, 1, 4, TIMEOUT);
   if (r < 0) {
      assert(errno == ETIMEDOUT);

      data->server_result->err = errno;
      data->server_result->result = SSL_TEST_TIMEOUT;

      mongoc_stream_destroy(ssl_stream);
      mongoc_close(listen_fd);

      return NULL;
   }

   assert(r == 4);
   memcpy(&len, iov.iov_base, r);

   r = mongoc_stream_readv(ssl_stream, &iov, 1, len, TIMEOUT);
   assert(r == len);

   iov.iov_len = r;
   mongoc_stream_writev(ssl_stream, &iov, 1, TIMEOUT);

   mongoc_stream_destroy(ssl_stream);

   mongoc_close(listen_fd);

   data->server_result->result = SSL_TEST_SUCCESS;

   return NULL;
}

/** this function is meant to be run from ssl_test as a child thread
 *
 * It:
 *    1. spins up
 *    2. waits on a condvar until the server is up
 *    3. connects to the servers port
 *    4. writes a 4 bytes length
 *    5. writes a string of length size
 *    6. reads a response back of the given length
 *    7. confirms that its the same as what was written
 *    8. shuts down
 */
static void *
ssl_test_client (void * ptr)
{
   ssl_test_data_t *data = (ssl_test_data_t *)ptr;
   mongoc_stream_t *sock_stream;
   mongoc_stream_t *ssl_stream;
   mongoc_fd_t conn_fd;
   char buf[1024];
   ssize_t r;
   struct iovec riov = { buf, sizeof(buf) };
   struct iovec wiov = { 0 };
   struct sockaddr_in server_addr = { 0 };

   conn_fd = mongoc_socket(AF_INET, SOCK_STREAM, 0);
   assert(mongoc_fd_is_valid(conn_fd));

   mongoc_mutex_lock(&data->cond_mutex);
   while (! data->server_port) {
      mongoc_cond_wait(&data->cond, &data->cond_mutex);
   }
   mongoc_mutex_unlock(&data->cond_mutex);

   server_addr.sin_family = AF_INET;
   server_addr.sin_port = htons(data->server_port);
   r = inet_pton(AF_INET, LOCALHOST, &server_addr.sin_addr);
   assert (r > 0);

   r = mongoc_connect(conn_fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
   assert (r == 0);

   sock_stream = mongoc_stream_unix_new(conn_fd);
   assert(sock_stream);
   ssl_stream = mongoc_stream_tls_new(sock_stream, data->client, 1);
   if (! ssl_stream) {
      unsigned long err = ERR_get_error();
      assert(err);

      data->client_result->ssl_err = err;
      data->client_result->result = SSL_TEST_SSL_INIT;

      mongoc_stream_destroy(sock_stream);

      return NULL;
   }
   assert(ssl_stream);

   errno = 0;
   r = mongoc_stream_tls_do_handshake (ssl_stream, TIMEOUT);
   if (! r) {
      unsigned long err = ERR_get_error();
      assert(err || errno);

      if (err) {
         data->client_result->ssl_err = err;
      } else {
         data->client_result->err = errno;
      }

      data->client_result->result = SSL_TEST_SSL_HANDSHAKE;

      mongoc_stream_destroy(ssl_stream);
      return NULL;
   }

   r = mongoc_stream_tls_check_cert (ssl_stream, data->host);
   if (! r) {
      data->client_result->result = SSL_TEST_SSL_VERIFY;

      mongoc_stream_destroy(ssl_stream);
      return NULL;
   }

   int len = 4;

   wiov.iov_base = &len;
   wiov.iov_len = 4;
   r = mongoc_stream_writev(ssl_stream, &wiov, 1, TIMEOUT);

   assert(r == wiov.iov_len);

   wiov.iov_base = "foo";
   wiov.iov_len = 4;
   r = mongoc_stream_writev(ssl_stream, &wiov, 1, TIMEOUT);
   assert(r == wiov.iov_len);

   r = mongoc_stream_readv(ssl_stream, &riov, 1, 4, TIMEOUT);
   assert(r > 0);
   assert(r == wiov.iov_len);
   assert(strcmp(riov.iov_base, wiov.iov_base) == 0);

   mongoc_stream_destroy(ssl_stream);

   data->client_result->result = SSL_TEST_SUCCESS;

   return NULL;
}


/** This is the testing function for the ssl-test lib
 *
 * The basic idea is that you spin up a client and server, which will
 * communicate over a mongoc-stream-tls, with varrying mongoc_ssl_opt's.  The
 * client and server speak a simple echo protocol, so all we're really testing
 * here is that any given configuration suceeds or fails as it should
 */
void
ssl_test (mongoc_ssl_opt_t  *client,
          mongoc_ssl_opt_t  *server,
          const char        *host,
          ssl_test_result_t *client_result,
          ssl_test_result_t *server_result)
{
   ssl_test_data_t data = { 0 };
   mongoc_thread_t threads[2];
   int i, r;

   data.server = server;
   data.client = client;
   data.client_result = client_result;
   data.server_result = server_result;
   data.host = host;

   mongoc_mutex_init(&data.cond_mutex);
   mongoc_cond_init(&data.cond);

   r = mongoc_thread_create(threads, &ssl_test_server, &data);
   assert(r == 0);

   r = mongoc_thread_create(threads + 1, &ssl_test_client, &data);
   assert(r == 0);

   for (i = 0; i < 2; i++) {
      r = mongoc_thread_join(threads[i]);
      assert(r == 0);
   }

   mongoc_mutex_destroy(&data.cond_mutex);
   mongoc_cond_destroy(&data.cond);
}
