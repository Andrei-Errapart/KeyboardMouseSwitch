/* Link encryption: AES-128-CCM (RFC 3610) with a 4-byte MIC, in software
 * on the nRF52840's ECB peripheral (one AES block in a few µs).
 *
 * PSA Crypto was measured at about 250 µs per seal or open of a 17-byte
 * body, on the CC310 and on Oberon alike (fixed per-call cost, about 90 µs
 * per AES block), so the bench uses the spec's "ECB peripheral with
 * software CCM" option. Safe from threads and interrupts. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LINK_DIR_TO_RECEIVER 0
#define LINK_DIR_TO_BRIDGE 1
#define LINK_MIC_LEN 4
#define LINK_SALT_LEN 8

typedef struct {
	uint8_t bytes[16];
} link_key_t;

/* Replay state of one direction of one session. Zeroed at session start:
 * no counter accepted yet, so counter 0 is accepted once. */
struct replay {
	bool any;
	uint32_t last;
};

/* One AES-128 block on the ECB peripheral. */
void link_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
/* AES-CCM, any nonce length 7..13 and MIC length 4..16: out = C ‖ MIC. */
void link_ccm_seal(const uint8_t key[16], const uint8_t *nonce, size_t nlen,
		   const uint8_t *aad, size_t alen, const uint8_t *pt, size_t len,
		   uint8_t *out, size_t mlen);
/* Returns true and writes the plaintext if the MIC verifies. */
bool link_ccm_open(const uint8_t key[16], const uint8_t *nonce, size_t nlen,
		   const uint8_t *aad, size_t alen, const uint8_t *in, size_t len,
		   uint8_t *pt, size_t mlen);

/* SK = AES-128-ECB(PSK, salt_b ‖ salt_r). */
void link_session_key(const uint8_t salt_b[8], const uint8_t salt_r[8], link_key_t *sk);
void link_key_destroy(link_key_t *key);
/* out = counter (LE) ‖ ciphertext ‖ MIC; returns len + 8. */
int link_seal(const link_key_t *key, uint32_t counter, uint8_t dir, const uint8_t *body,
	      size_t len, uint8_t *out);
/* Returns the body length, -EBADMSG (MIC), -EALREADY (replay), -EINVAL. */
int link_open(const link_key_t *key, uint8_t dir, struct replay *rp, const uint8_t *in,
	      size_t len, uint8_t *body);
/* Discovery, under the pre-shared key: out = salt ‖ ciphertext ‖ MIC. */
int link_seal_discovery(uint8_t dir, const uint8_t salt[8], const uint8_t *body,
			size_t len, uint8_t *out);
int link_open_discovery(uint8_t dir, const uint8_t *in, size_t len, uint8_t salt[8],
			uint8_t *body);
/* `kmb selftest`: writes a SELFTEST record. */
void link_selftest(void);
