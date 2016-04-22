/*
 * Copyright (C) 2016 Southern Storm Software, Pty Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <noise/protocol.h>
#include "echo-common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#define short_options "k:"

static struct option const long_options[] = {
    {"key-dir",                 required_argument,      NULL,       'k'},
    {NULL,                      0,                      NULL,        0 }
};

/* Parsed command-line options */
static const char *key_dir = ".";
static int port = 7000;

/* Loaded keys */
#define CURVE25519_KEY_LEN 32
#define CURVE448_KEY_LEN 56
static uint8_t client_key_25519[CURVE25519_KEY_LEN];
static uint8_t server_key_25519[CURVE25519_KEY_LEN];
static uint8_t client_key_448[CURVE448_KEY_LEN];
static uint8_t server_key_448[CURVE448_KEY_LEN];
static uint8_t psk[32];

/* Message buffer for send/receive */
#define MAX_MESSAGE_LEN 65535
static uint8_t message[MAX_MESSAGE_LEN + 2];

/* Print usage information */
static void usage(const char *progname)
{
    fprintf(stderr, "Usage: %s [options] port\n\n", progname);
    fprintf(stderr, "Options:\n\n");
    fprintf(stderr, "    --key-dir-key=directory, -k directory\n");
    fprintf(stderr, "        Directory containing the client and server keys.\n\n");
    fprintf(stderr, "    --psk=value, -p value\n");
    fprintf(stderr, "        Pre-shared key value to use.\n\n");
}

/* Parse the command-line options */
static int parse_options(int argc, char *argv[])
{
    const char *progname = argv[0];
    int index = 0;
    int ch;
    while ((ch = getopt_long(argc, argv, short_options, long_options, &index)) != -1) {
        switch (ch) {
        case 'k':   key_dir = optarg; break;
        default:
            usage(progname);
            return 0;
        }
    }
    if ((optind + 1) != argc) {
        usage(progname);
        return 0;
    }
    port = atoi(argv[optind]);
    if (port < 1 || port > 65535) {
        usage(progname);
        return 0;
    }
    return 1;
}

/* Initialize's the handshake with all necessary keys */
static int initialize_handshake
    (NoiseHandshakeState *handshake, const NoiseProtocolId *nid,
     const void *prologue, size_t prologue_len)
{
    NoiseDHState *dh;
    int dh_id;
    int err;

    /* Set the prologue first */
    err = noise_handshakestate_set_prologue(handshake, prologue, prologue_len);
    if (err != NOISE_ERROR_NONE) {
        noise_perror("prologue", err);
        return 0;
    }

    /* Set the PSK if one is needed */
    if (nid->prefix_id == NOISE_PREFIX_PSK) {
        err = noise_handshakestate_set_pre_shared_key
            (handshake, psk, sizeof(psk));
        if (err != NOISE_ERROR_NONE) {
            noise_perror("psk", err);
            return 0;
        }
    }

    /* Set the local keypair for the server based on the DH algorithm */
    if (noise_handshakestate_needs_local_keypair(handshake)) {
        dh = noise_handshakestate_get_local_keypair_dh(handshake);
        dh_id = noise_dhstate_get_dh_id(dh);
        if (dh_id == NOISE_DH_CURVE25519) {
            err = noise_dhstate_set_keypair_private
                (dh, server_key_25519, sizeof(server_key_25519));
        } else if (dh_id == NOISE_DH_CURVE448) {
            err = noise_dhstate_set_keypair_private
                (dh, server_key_448, sizeof(server_key_448));
        } else {
            err = NOISE_ERROR_UNKNOWN_ID;
        }
        if (err != NOISE_ERROR_NONE) {
            noise_perror("set server private key", err);
            return 0;
        }
    }

    /* Set the remote public key for the client */
    if (noise_handshakestate_needs_remote_public_key(handshake)) {
        dh = noise_handshakestate_get_remote_public_key_dh(handshake);
        dh_id = noise_dhstate_get_dh_id(dh);
        if (dh_id == NOISE_DH_CURVE25519) {
            err = noise_dhstate_set_public_key
                (dh, client_key_25519, sizeof(client_key_25519));
        } else if (dh_id == NOISE_DH_CURVE25519) {
            err = noise_dhstate_set_public_key
                (dh, client_key_448, sizeof(client_key_448));
        } else {
            err = NOISE_ERROR_UNKNOWN_ID;
        }
        if (err != NOISE_ERROR_NONE) {
            noise_perror("set client public key", err);
            return 0;
        }
    }

    /* Ready to go */
    return 1;
}

int main(int argc, char *argv[])
{
    NoiseHandshakeState *handshake = 0;
    NoiseCipherState *send_cipher = 0;
    NoiseCipherState *recv_cipher = 0;
    EchoProtocolId id;
    NoiseProtocolId nid;
    size_t message_size;
    size_t payload_size;
    int fd;
    int err;
    int ok = 1;
    int action;

    /* Parse the command-line options */
    if (!parse_options(argc, argv))
        return 1;

    /* Change into the key directory and load all of the keys we'll need */
    if (chdir(key_dir) < 0) {
        perror(key_dir);
        return 1;
    }
    if (!echo_load_private_key
            ("server_key_25519", server_key_25519, sizeof(server_key_25519))) {
        return 1;
    }
    if (!echo_load_private_key
            ("server_key_448", server_key_448, sizeof(server_key_448))) {
        return 1;
    }
    if (!echo_load_public_key
            ("client_key_25519.pub", client_key_25519, sizeof(client_key_25519))) {
        return 1;
    }
    if (!echo_load_public_key
            ("client_key_448.pub", client_key_448, sizeof(client_key_448))) {
        return 1;
    }
    if (!echo_load_public_key("psk", psk, sizeof(psk))) {
        return 1;
    }

    /* Accept an incoming connection */
    fd = echo_accept(port);

    /* Read the echo protocol identifier sent by the client */
    if (ok && !echo_recv_exact(fd, (uint8_t *)&id, sizeof(id))) {
        fprintf(stderr, "Did not receive the echo protocol identifier\n");
        ok = 0;
    }

    /* Convert the echo protocol identifier into a Noise protocol identifier */
    if (ok && !echo_to_noise_protocol_id(&nid, &id)) {
        fprintf(stderr, "Unknown echo protocol identifier\n");
        ok = 0;
    }

    /* Create a HandshakeState object to manage the server's handshake */
    if (ok) {
        err = noise_handshakestate_new_by_id
            (&handshake, &nid, NOISE_ROLE_RESPONDER);
        if (err != NOISE_ERROR_NONE) {
            noise_perror("create handshake", err);
            ok = 0;
        }
    }

    /* Set all keys that are needed by the client's requested echo protocol */
    if (ok) {
        if (!initialize_handshake(handshake, &nid, &id, sizeof(id))) {
            ok = 0;
        }
    }

    /* Start the handshake */
    if (ok) {
        err = noise_handshakestate_start(handshake);
        if (err != NOISE_ERROR_NONE) {
            noise_perror("start handshake", err);
            ok = 0;
        }
    }

    /* Run the handshake until we run out of things to read or write */
    while (ok) {
        action = noise_handshakestate_get_action(handshake);
        if (action == NOISE_ACTION_WRITE_MESSAGE) {
            /* Write the next handshake message with a zero-length payload */
            message_size = sizeof(message) - 2;
            err = noise_handshakestate_write_message
                (handshake, NULL, 0, message + 2, &message_size);
            if (err != NOISE_ERROR_NONE) {
                noise_perror("write handshake", err);
                ok = 0;
                break;
            }
            message[0] = (uint8_t)(message_size >> 8);
            message[1] = (uint8_t)message_size;
            if (!echo_send(fd, message, message_size + 2)) {
                ok = 0;
                break;
            }
        } else if (action == NOISE_ACTION_READ_MESSAGE) {
            /* Read the next handshake message and discard the payload */
            message_size = echo_recv(fd, message, sizeof(message));
            if (!message_size) {
                ok = 0;
                break;
            }
            message_size -= 2;  /* Overhead of the packet length field */
            err = noise_handshakestate_read_message
                (handshake, message + 2, message_size, NULL, &payload_size);
            if (err != NOISE_ERROR_NONE) {
                noise_perror("read handshake", err);
                ok = 0;
                break;
            }
        } else {
            /* Either the handshake has finished or it has failed */
            break;
        }
    }

    /* If the action is not "split", then the handshake has failed */
    if (ok && noise_handshakestate_get_action(handshake) != NOISE_ACTION_SPLIT) {
        fprintf(stderr, "protocol handshake failed\n");
        ok = 0;
    }

    /* Split out the two CipherState objects for send and receive */
    if (ok) {
        err = noise_handshakestate_split(handshake, &recv_cipher, &send_cipher);
        if (err != NOISE_ERROR_NONE) {
            noise_perror("split to start data transfer", err);
            ok = 0;
        }
    }

    /* We no longer need the HandshakeState */
    noise_handshakestate_free(handshake);
    handshake = 0;

    /* Process all incoming data packets and echo them back to the client */
    while (ok) {
        /* Read the next message, including the two byte length prefix */
        message_size = echo_recv(fd, message, sizeof(message));
        if (!message_size)
            break;

        /* Decrypt the message */
        err = noise_cipherstate_decrypt_with_ad
            (recv_cipher, NULL, 0, message + 2, message_size - 2, &payload_size);
        if (err != NOISE_ERROR_NONE) {
            noise_perror("read", err);
            ok = 0;
            break;
        }

        /* Re-encrypt it with the sending cipher and send back to the client */
        err = noise_cipherstate_encrypt_with_ad
            (send_cipher, NULL, 0, message + 2, payload_size, &message_size);
        if (err != NOISE_ERROR_NONE) {
            noise_perror("write", err);
            ok = 0;
            break;
        }
        message[0] = (uint8_t)(message_size >> 8);
        message[1] = (uint8_t)message_size;
        if (!echo_send(fd, message, message_size + 2)) {
            ok = 0;
            break;
        }
    }

    /* Clean up and exit */
    noise_cipherstate_free(send_cipher);
    noise_cipherstate_free(recv_cipher);
    echo_close(fd);
    return ok ? 0 : 1;
}
