/*
 *
 * Copyright 2016, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/thd.h>
#include "src/core/lib/iomgr/load_file.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

#define SSL_CERT_PATH "src/core/lib/tsi/test_creds/server1.pem"
#define SSL_KEY_PATH "src/core/lib/tsi/test_creds/server1.key"
#define SSL_CA_PATH "src/core/lib/tsi/test_creds/ca.pem"

// Arguments for TLS server thread.
typedef struct {
  int socket;
  char *alpn_preferred;
} server_args;

// From https://wiki.openssl.org/index.php/Simple_TLS_Server.
static int create_socket(int port) {
  int s;
  struct sockaddr_in addr;

  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    perror("Unable to create socket");
    return -1;
  }

  if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("Unable to bind");
    gpr_log(GPR_ERROR, "Unable to bind to %d", port);
    close(s);
    return -1;
  }

  if (listen(s, 1) < 0) {
    perror("Unable to listen");
    close(s);
    return -1;
  }

  return s;
}

// Server callback during ALPN negotiation. See man page for
// SSL_CTX_set_alpn_select_cb.
static int alpn_select_cb(SSL *ssl, const uint8_t **out, uint8_t *out_len,
                          const uint8_t *in, unsigned in_len, void *arg) {
  const uint8_t *alpn_preferred = (const uint8_t *)arg;

  *out = alpn_preferred;
  *out_len = (uint8_t)strlen((char *)alpn_preferred);

  // Validate that the ALPN list includes "h2" and "grpc-exp", that "grpc-exp"
  // precedes "h2".
  bool grpc_exp_seen = false;
  bool h2_seen = false;
  const char *inp = (const char *)in;
  const char *in_end = inp + in_len;
  while (inp < in_end) {
    const size_t length = (size_t)*inp++;
    if (length == strlen("grpc-exp") && strncmp(inp, "grpc-exp", length) == 0) {
      grpc_exp_seen = true;
      GPR_ASSERT(!h2_seen);
    }
    if (length == strlen("h2") && strncmp(inp, "h2", length) == 0) {
      h2_seen = true;
      GPR_ASSERT(grpc_exp_seen);
    }
    inp += length;
  }

  GPR_ASSERT(inp == in_end);
  GPR_ASSERT(grpc_exp_seen);
  GPR_ASSERT(h2_seen);

  return SSL_TLSEXT_ERR_OK;
}

// Minimal TLS server. This is largely based on the example at
// https://wiki.openssl.org/index.php/Simple_TLS_Server and the gRPC core
// internals in src/core/lib/tsi/ssl_transport_security.c.
static void server_thread(void *arg) {
  const server_args *args = (server_args *)arg;

  SSL_load_error_strings();
  OpenSSL_add_ssl_algorithms();

  const SSL_METHOD *method = TLSv1_2_server_method();
  SSL_CTX *ctx = SSL_CTX_new(method);
  if (!ctx) {
    perror("Unable to create SSL context");
    ERR_print_errors_fp(stderr);
    abort();
  }

  // Load key pair.
  if (SSL_CTX_use_certificate_file(ctx, SSL_CERT_PATH, SSL_FILETYPE_PEM) < 0) {
    ERR_print_errors_fp(stderr);
    abort();
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, SSL_KEY_PATH, SSL_FILETYPE_PEM) < 0) {
    ERR_print_errors_fp(stderr);
    abort();
  }

  // Set the cipher list to match the one expressed in
  // src/core/lib/tsi/ssl_transport_security.c.
  const char *cipher_list =
      "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-SHA256:ECDHE-RSA-AES256-"
      "SHA384:ECDHE-RSA-AES256-GCM-SHA384";
  if (!SSL_CTX_set_cipher_list(ctx, cipher_list)) {
    ERR_print_errors_fp(stderr);
    gpr_log(GPR_ERROR, "Couldn't set server cipher list.");
    abort();
  }

  // Register the ALPN selection callback.
  SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, args->alpn_preferred);

  // bind/listen/accept at TCP layer.
  const int sock = args->socket;
  gpr_log(GPR_INFO, "Server listening");
  struct sockaddr_in addr;
  socklen_t len = sizeof(addr);
  const int client = accept(sock, (struct sockaddr *)&addr, &len);
  if (client < 0) {
    perror("Unable to accept");
    abort();
  }

  // Establish a SSL* and accept at SSL layer.
  SSL *ssl = SSL_new(ctx);
  GPR_ASSERT(ssl);
  SSL_set_fd(ssl, client);
  if (SSL_accept(ssl) <= 0) {
    ERR_print_errors_fp(stderr);
    gpr_log(GPR_ERROR, "Handshake failed.");
  } else {
    gpr_log(GPR_INFO, "Handshake successful.");
  }

  // Wait until the client drops its connection.
  char buf;
  while (SSL_read(ssl, &buf, sizeof(buf)) > 0)
    ;

  SSL_free(ssl);
  close(client);
  close(sock);
  SSL_CTX_free(ctx);
  EVP_cleanup();
}

// This test launches a minimal TLS server on a separate thread and then
// establishes a TLS handshake via the core library to the server. The TLS
// server validates ALPN aspects of the handshake and supplies the protocol
// specified in the server_alpn_preferred argument to the client.
static bool client_ssl_test(char *server_alpn_preferred) {
  bool success = true;

  grpc_init();

  // Find a port we can bind to. Retries added to handle flakes in port server
  // and port picking.
  int port = -1;
  int server_socket = -1;
  int socket_retries = 30;
  while (server_socket == -1 && socket_retries-- > 0) {
    port = grpc_pick_unused_port_or_die();
    server_socket = create_socket(port);
    if (server_socket == -1) {
      sleep(1);
    }
  }
  GPR_ASSERT(server_socket > 0);

  // Launch the TLS server thread.
  gpr_thd_options thdopt = gpr_thd_options_default();
  gpr_thd_id thdid;
  gpr_thd_options_set_joinable(&thdopt);
  server_args args = {.socket = server_socket,
                      .alpn_preferred = server_alpn_preferred};
  GPR_ASSERT(gpr_thd_new(&thdid, server_thread, &args, &thdopt));

  // Load key pair and establish client SSL credentials.
  grpc_ssl_pem_key_cert_pair pem_key_cert_pair;
  grpc_slice ca_slice, cert_slice, key_slice;
  GPR_ASSERT(GRPC_LOG_IF_ERROR("load_file",
                               grpc_load_file(SSL_CA_PATH, 1, &ca_slice)));
  GPR_ASSERT(GRPC_LOG_IF_ERROR("load_file",
                               grpc_load_file(SSL_CERT_PATH, 1, &cert_slice)));
  GPR_ASSERT(GRPC_LOG_IF_ERROR("load_file",
                               grpc_load_file(SSL_KEY_PATH, 1, &key_slice)));
  const char *ca_cert = (const char *)GRPC_SLICE_START_PTR(ca_slice);
  pem_key_cert_pair.private_key = (const char *)GRPC_SLICE_START_PTR(key_slice);
  pem_key_cert_pair.cert_chain = (const char *)GRPC_SLICE_START_PTR(cert_slice);
  grpc_channel_credentials *ssl_creds =
      grpc_ssl_credentials_create(ca_cert, &pem_key_cert_pair, NULL);

  // Establish a channel pointing at the TLS server. Since the gRPC runtime is
  // lazy, this won't necessarily establish a connection yet.
  char *target;
  gpr_asprintf(&target, "127.0.0.1:%d", port);
  grpc_arg ssl_name_override = {GRPC_ARG_STRING,
                                GRPC_SSL_TARGET_NAME_OVERRIDE_ARG,
                                {"foo.test.google.fr"}};
  grpc_channel_args grpc_args;
  grpc_args.num_args = 1;
  grpc_args.args = &ssl_name_override;
  grpc_channel *channel =
      grpc_secure_channel_create(ssl_creds, target, &grpc_args, NULL);
  GPR_ASSERT(channel);
  gpr_free(target);

  // Initially the channel will be idle, the
  // grpc_channel_check_connectivity_state triggers an attempt to connect.
  GPR_ASSERT(grpc_channel_check_connectivity_state(
                 channel, 1 /* try_to_connect */) == GRPC_CHANNEL_IDLE);

  // Wait a bounded number of times for the channel to be ready. When the
  // channel is ready, the initial TLS handshake will have successfully
  // completed and we know that the client's ALPN list satisfied the server.
  int retries = 10;
  grpc_connectivity_state state = GRPC_CHANNEL_IDLE;
  grpc_completion_queue *cq = grpc_completion_queue_create(NULL);
  while (state != GRPC_CHANNEL_READY && retries-- > 0) {
    grpc_channel_watch_connectivity_state(
        channel, state, GRPC_TIMEOUT_SECONDS_TO_DEADLINE(3), cq, NULL);
    gpr_timespec cq_deadline = GRPC_TIMEOUT_SECONDS_TO_DEADLINE(5);
    grpc_event ev = grpc_completion_queue_next(cq, cq_deadline, NULL);
    GPR_ASSERT(ev.type == GRPC_OP_COMPLETE);
    state =
        grpc_channel_check_connectivity_state(channel, 0 /* try_to_connect */);
  }
  grpc_completion_queue_destroy(cq);
  if (retries < 0) {
    success = false;
  }

  grpc_channel_destroy(channel);
  grpc_channel_credentials_release(ssl_creds);
  grpc_slice_unref(cert_slice);
  grpc_slice_unref(key_slice);
  grpc_slice_unref(ca_slice);

  gpr_thd_join(thdid);

  grpc_shutdown();

  return success;
}

int main(int argc, char *argv[]) {
  // Handshake succeeeds when the server has grpc-exp as the ALPN preference.
  GPR_ASSERT(client_ssl_test("grpc-exp"));
  // Handshake succeeeds when the server has h2 as the ALPN preference. This
  // covers legacy gRPC servers which don't support grpc-exp.
  GPR_ASSERT(client_ssl_test("h2"));
  // Handshake fails when the server uses a fake protocol as its ALPN
  // preference. This validates the client is correctly validating ALPN returns
  // and sanity checks the client_ssl_test.
  GPR_ASSERT(!client_ssl_test("foo"));
  return 0;
}
