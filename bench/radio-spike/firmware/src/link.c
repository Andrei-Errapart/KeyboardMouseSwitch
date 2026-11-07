#include "link.h"

#include <errno.h>
#include <hal/nrf_ecb.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "rec.h"
#include "stamp.h"

#define NONCE_LEN 13

/* BENCH ONLY: a public constant, never the real key (~/.kmswitch-key). */
static const link_key_t psk = {{'K', 'M', 'S', ' ', 'b', 'e', 'n', 'c', 'h', ' ', 'k', 'e',
				'y', ' ', '0', '1'}};

static struct {
	uint8_t key[16];
	uint8_t clear[16];
	uint8_t cipher[16];
} __aligned(4) ecb;
static struct k_spinlock ecb_lock;

void link_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
	k_spinlock_key_t k = k_spin_lock(&ecb_lock);

	memcpy(ecb.key, key, 16);
	memcpy(ecb.clear, in, 16);
	nrf_ecb_data_pointer_set(NRF_ECB, &ecb);
	nrf_ecb_event_clear(NRF_ECB, NRF_ECB_EVENT_ENDECB);
	nrf_ecb_event_clear(NRF_ECB, NRF_ECB_EVENT_ERRORECB);
	nrf_ecb_task_trigger(NRF_ECB, NRF_ECB_TASK_STARTECB);
	while (!nrf_ecb_event_check(NRF_ECB, NRF_ECB_EVENT_ENDECB) &&
	       !nrf_ecb_event_check(NRF_ECB, NRF_ECB_EVENT_ERRORECB)) {
	}
	memcpy(out, ecb.cipher, 16);
	memset(ecb.key, 0, 16);
	k_spin_unlock(&ecb_lock, k);
}

static void xor16(uint8_t *x, const uint8_t *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		x[i] ^= b[i];
	}
}

/* RFC 3610. L = 15 - nlen bytes of length/counter; flags in B0 and A_i. */
static void ccm_mac(const uint8_t key[16], const uint8_t *nonce, size_t nlen, const uint8_t *aad,
		    size_t alen, const uint8_t *pt, size_t len, size_t mlen, uint8_t x[16])
{
	size_t L = 15 - nlen;
	uint8_t b[16];

	b[0] = (alen ? 0x40 : 0) | (((mlen - 2) / 2) << 3) | (L - 1);
	memcpy(&b[1], nonce, nlen);
	for (size_t i = 0; i < L; i++) {
		b[15 - i] = (len >> (8 * i)) & 0xff;
	}
	link_aes_block(key, b, x);

	if (alen) {
		/* a < 2^16 - 2^8: two length bytes, then the data, zero-padded. */
		size_t used = 2;

		memset(b, 0, 16);
		b[0] = alen >> 8;
		b[1] = alen & 0xff;
		for (size_t i = 0; i < alen; i++) {
			b[used++] = aad[i];
			if (used == 16) {
				xor16(x, b, 16);
				link_aes_block(key, x, x);
				memset(b, 0, 16);
				used = 0;
			}
		}
		if (used) {
			xor16(x, b, 16);
			link_aes_block(key, x, x);
		}
	}
	for (size_t off = 0; off < len; off += 16) {
		xor16(x, pt + off, MIN(16, len - off));
		link_aes_block(key, x, x);
	}
}

static void ccm_ctr_block(const uint8_t key[16], const uint8_t *nonce, size_t nlen, uint32_t i,
			  uint8_t s[16])
{
	size_t L = 15 - nlen;
	uint8_t a[16] = {0};

	a[0] = L - 1;
	memcpy(&a[1], nonce, nlen);
	for (size_t j = 0; j < L && j < 4; j++) {
		a[15 - j] = (i >> (8 * j)) & 0xff;
	}
	link_aes_block(key, a, s);
}

static void ccm_crypt(const uint8_t key[16], const uint8_t *nonce, size_t nlen,
		      const uint8_t *in, size_t len, uint8_t *out)
{
	uint8_t s[16];

	for (size_t off = 0, i = 1; off < len; off += 16, i++) {
		size_t n = MIN(16, len - off);

		ccm_ctr_block(key, nonce, nlen, i, s);
		for (size_t j = 0; j < n; j++) {
			out[off + j] = in[off + j] ^ s[j];
		}
	}
}

void link_ccm_seal(const uint8_t key[16], const uint8_t *nonce, size_t nlen, const uint8_t *aad,
		   size_t alen, const uint8_t *pt, size_t len, uint8_t *out, size_t mlen)
{
	uint8_t x[16], s0[16];

	ccm_mac(key, nonce, nlen, aad, alen, pt, len, mlen, x);
	ccm_ctr_block(key, nonce, nlen, 0, s0);
	ccm_crypt(key, nonce, nlen, pt, len, out);
	for (size_t j = 0; j < mlen; j++) {
		out[len + j] = x[j] ^ s0[j];
	}
}

bool link_ccm_open(const uint8_t key[16], const uint8_t *nonce, size_t nlen, const uint8_t *aad,
		   size_t alen, const uint8_t *in, size_t len, uint8_t *pt, size_t mlen)
{
	uint8_t x[16], s0[16], diff = 0;
	size_t plen = len - mlen;

	ccm_crypt(key, nonce, nlen, in, plen, pt);
	ccm_mac(key, nonce, nlen, aad, alen, pt, plen, mlen, x);
	ccm_ctr_block(key, nonce, nlen, 0, s0);
	for (size_t j = 0; j < mlen; j++) {
		diff |= (x[j] ^ s0[j]) ^ in[plen + j];
	}
	if (diff) {
		memset(pt, 0, plen);
	}
	return diff == 0;
}

void link_session_key(const uint8_t salt_b[8], const uint8_t salt_r[8], link_key_t *sk)
{
	uint8_t in[16];

	memcpy(in, salt_b, 8);
	memcpy(in + 8, salt_r, 8);
	link_aes_block(psk.bytes, in, sk->bytes);
}

void link_key_destroy(link_key_t *key)
{
	memset(key->bytes, 0, sizeof(key->bytes));
}

static void nonce_session(uint8_t n[NONCE_LEN], uint32_t counter, uint8_t dir)
{
	sys_put_le32(counter, n);
	n[4] = dir;
	memset(&n[5], 0, 8);
}

static void nonce_discovery(uint8_t n[NONCE_LEN], uint8_t dir, const uint8_t salt[8])
{
	memset(n, 0, 4);
	n[4] = dir;
	memcpy(&n[5], salt, 8);
}

int link_seal(const link_key_t *key, uint32_t counter, uint8_t dir, const uint8_t *body,
	      size_t len, uint8_t *out)
{
	uint8_t n[NONCE_LEN];

	nonce_session(n, counter, dir);
	sys_put_le32(counter, out);
	link_ccm_seal(key->bytes, n, NONCE_LEN, NULL, 0, body, len, out + 4, LINK_MIC_LEN);
	return 4 + len + LINK_MIC_LEN;
}

int link_open(const link_key_t *key, uint8_t dir, struct replay *rp, const uint8_t *in, size_t len,
	      uint8_t *body)
{
	uint8_t n[NONCE_LEN];

	if (len < 4 + LINK_MIC_LEN) {
		return -EINVAL;
	}
	uint32_t counter = sys_get_le32(in);

	/* State changes only after the MIC verifies, so a forged counter
	 * can't block the session. */
	if (rp->any && counter <= rp->last) {
		return -EALREADY;
	}
	nonce_session(n, counter, dir);
	if (!link_ccm_open(key->bytes, n, NONCE_LEN, NULL, 0, in + 4, len - 4, body,
			   LINK_MIC_LEN)) {
		return -EBADMSG;
	}
	rp->any = true;
	rp->last = counter;
	return len - 4 - LINK_MIC_LEN;
}

int link_seal_discovery(uint8_t dir, const uint8_t salt[8], const uint8_t *body, size_t len,
			uint8_t *out)
{
	uint8_t n[NONCE_LEN];

	nonce_discovery(n, dir, salt);
	memcpy(out, salt, LINK_SALT_LEN);
	link_ccm_seal(psk.bytes, n, NONCE_LEN, NULL, 0, body, len, out + LINK_SALT_LEN,
		      LINK_MIC_LEN);
	return LINK_SALT_LEN + len + LINK_MIC_LEN;
}

int link_open_discovery(uint8_t dir, const uint8_t *in, size_t len, uint8_t salt[8],
			uint8_t *body)
{
	uint8_t n[NONCE_LEN];

	if (len < LINK_SALT_LEN + LINK_MIC_LEN) {
		return -EINVAL;
	}
	memcpy(salt, in, LINK_SALT_LEN);
	nonce_discovery(n, dir, salt);
	if (!link_ccm_open(psk.bytes, n, NONCE_LEN, NULL, 0, in + LINK_SALT_LEN,
			   len - LINK_SALT_LEN, body, LINK_MIC_LEN)) {
		return -EBADMSG;
	}
	return len - LINK_SALT_LEN - LINK_MIC_LEN;
}

/* Self-test constants, computed independently by
 * bench/radio-spike/host/tests/test_vectors.py. */
static const uint8_t nist_key[16] = {0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
				     0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f};
static const uint8_t nist1_ct[8] = {0x71, 0x62, 0x01, 0x5b, 0x4d, 0xac, 0x25, 0x5d};
static const uint8_t nist2_ct[22] = {0xd2, 0xa1, 0xf0, 0xe0, 0x51, 0xea, 0x5f, 0x62,
				     0x08, 0x1a, 0x77, 0x92, 0x07, 0x3d, 0x59, 0x3d,
				     0x1f, 0xc6, 0x4f, 0xbf, 0xac, 0xcd};
static const uint8_t salt_b[8] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
static const uint8_t salt_r[8] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27};
static const uint8_t sk_expected[16] = {0xb4, 0x83, 0x52, 0x9c, 0xc3, 0x1c, 0x16, 0xb5,
					0x98, 0xdf, 0x0b, 0x30, 0x13, 0x5f, 0x30, 0x83};
static const uint8_t layout_sealed[4 + 21] = {
	0x05, 0x00, 0x00, 0x00, /* counter 5 in the clear */
	0x55, 0x64, 0xa6, 0x20, 0x93, 0xc9, 0x65, 0xa7, 0xaa, 0x7f, 0x9a,
	0xac, 0x0f, 0xde, 0x39, 0xe6, 0xbf, 0x0f, 0xf0, 0x53, 0xfb};

static void seq(uint8_t *p, size_t n, uint8_t first)
{
	for (size_t i = 0; i < n; i++) {
		p[i] = first + i;
	}
}

/* SP 800-38C examples 1 (partial blocks, 4-byte MIC) and 2 (full
 * blocks, 6-byte MIC), both ways. */
static bool check_nist(void)
{
	uint8_t nonce[8], aad[16], pt[16], out[22], back[16];

	seq(nonce, 7, 0x10);
	seq(aad, 8, 0x00);
	seq(pt, 4, 0x20);
	link_ccm_seal(nist_key, nonce, 7, aad, 8, pt, 4, out, 4);
	bool ok = memcmp(out, nist1_ct, 8) == 0 &&
		  link_ccm_open(nist_key, nonce, 7, aad, 8, out, 8, back, 4) &&
		  memcmp(back, pt, 4) == 0;

	seq(nonce, 8, 0x10);
	seq(aad, 16, 0x00);
	seq(pt, 16, 0x20);
	link_ccm_seal(nist_key, nonce, 8, aad, 16, pt, 16, out, 6);
	return ok && memcmp(out, nist2_ct, 22) == 0 &&
	       link_ccm_open(nist_key, nonce, 8, aad, 16, out, 22, back, 6) &&
	       memcmp(back, pt, 16) == 0;
}

void link_selftest(void)
{
	struct rec_selftest r = {.type = REC_SELFTEST};
	uint8_t pt[17], out[4 + 21], back[17];
	link_key_t sk;
	struct replay rp = {0}, fresh = {0}, timing = {0};
	uint32_t t0;
	const int rounds = 20;

	seq(pt, 17, 0x30);
	r.nist_ok = check_nist();

	link_session_key(salt_b, salt_r, &sk);
	r.layout_ok = memcmp(sk.bytes, sk_expected, 16) == 0 &&
		      link_seal(&sk, 5, LINK_DIR_TO_BRIDGE, pt, 17, out) == sizeof(out) &&
		      memcmp(out, layout_sealed, sizeof(out)) == 0;

	/* A fresh session accepts counter 0 once, then rejects it. */
	link_seal(&sk, 0, LINK_DIR_TO_BRIDGE, pt, 17, out);
	r.replay_ok = link_open(&sk, LINK_DIR_TO_BRIDGE, &rp, out, sizeof(out), back) == 17 &&
		      memcmp(back, pt, 17) == 0 &&
		      link_open(&sk, LINK_DIR_TO_BRIDGE, &rp, out, sizeof(out), back) == -EALREADY;

	out[4 + 3] ^= 0x01; /* flip a ciphertext bit */
	r.tamper_ok = link_open(&sk, LINK_DIR_TO_BRIDGE, &fresh, out, sizeof(out), back) == -EBADMSG;

	t0 = stamp_now();
	for (int i = 0; i < rounds; i++) {
		link_seal(&sk, 100 + i, LINK_DIR_TO_RECEIVER, pt, 17, out);
	}
	r.enc_us = (stamp_now() - t0) / rounds;

	t0 = stamp_now();
	for (int i = 0; i < rounds; i++) {
		/* Sealed fresh each time: a repeated counter would stop at the
		 * replay check without decrypting. */
		link_seal(&sk, 1000 + i, LINK_DIR_TO_RECEIVER, pt, 17, out);
		link_open(&sk, LINK_DIR_TO_RECEIVER, &timing, out, sizeof(out), back);
	}
	r.dec_us = (stamp_now() - t0) / rounds - r.enc_us;

	t0 = stamp_now();
	for (int i = 0; i < rounds; i++) {
		link_session_key(salt_b, salt_r, &sk);
	}
	r.sk_us = (stamp_now() - t0) / rounds;
	link_key_destroy(&sk);
	rec_put(&r, sizeof(r));
}
