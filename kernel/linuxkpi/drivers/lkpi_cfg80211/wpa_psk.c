// SPDX-License-Identifier: GPL-2.0
#include "wpa_psk.h"

/* ---------------- SHA-1 ---------------- */
struct sha1_ctx { wu32 h[5]; wu64 len; wu8 buf[64]; size_t fill; };
#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
static void sha1_block(struct sha1_ctx *c, const wu8 *p)
{
	wu32 w[80], a, b, d, e, f, k, t, cc;
	int i;
	for (i = 0; i < 16; i++)
		w[i] = ((wu32)p[4*i] << 24) | ((wu32)p[4*i+1] << 16) | ((wu32)p[4*i+2] << 8) | p[4*i+3];
	for (; i < 80; i++)
		w[i] = ROL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
	a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
	for (i = 0; i < 80; i++) {
		if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
		else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
		else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
		else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
		t = ROL(a, 5) + f + e + k + w[i];
		e = d; d = cc; cc = ROL(b, 30); b = a; a = t;
	}
	c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}
static void sha1_init(struct sha1_ctx *c)
{
	c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
	c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0; c->len = 0; c->fill = 0;
}
static void sha1_update(struct sha1_ctx *c, const wu8 *d, size_t n)
{
	c->len += n;
	while (n) {
		size_t t = 64 - c->fill;
		if (t > n) t = n;
		memcpy(c->buf + c->fill, d, t);
		c->fill += t; d += t; n -= t;
		if (c->fill == 64) { sha1_block(c, c->buf); c->fill = 0; }
	}
}
static void sha1_final(struct sha1_ctx *c, wu8 out[20])
{
	wu64 bits = c->len * 8;
	wu8 pad = 0x80, z = 0, l[8];
	int i;
	sha1_update(c, &pad, 1);
	while (c->fill != 56) sha1_update(c, &z, 1);
	for (i = 0; i < 8; i++) l[i] = (wu8)(bits >> (56 - 8 * i));
	sha1_update(c, l, 8);
	for (i = 0; i < 5; i++) {
		out[4*i] = c->h[i] >> 24; out[4*i+1] = c->h[i] >> 16;
		out[4*i+2] = c->h[i] >> 8; out[4*i+3] = c->h[i];
	}
}
void wpa_sha1(const wu8 *data, size_t len, wu8 out[20])
{
	struct sha1_ctx c;
	sha1_init(&c); sha1_update(&c, data, len); sha1_final(&c, out);
}

/* ---------------- HMAC-SHA1 ---------------- */
struct hmac_ctx { struct sha1_ctx in; wu8 opad[64]; };
static void hmac_init(struct hmac_ctx *h, const wu8 *key, size_t klen)
{
	wu8 k[64], ipad[64];
	int i;
	memset(k, 0, 64);
	if (klen > 64) wpa_sha1(key, klen, k); else memcpy(k, key, klen);
	for (i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; h->opad[i] = k[i] ^ 0x5c; }
	sha1_init(&h->in); sha1_update(&h->in, ipad, 64);
}
static void hmac_final(struct hmac_ctx *h, wu8 out[20])
{
	wu8 inner[20];
	struct sha1_ctx o;
	sha1_final(&h->in, inner);
	sha1_init(&o); sha1_update(&o, h->opad, 64); sha1_update(&o, inner, 20); sha1_final(&o, out);
}
void wpa_hmac_sha1(const wu8 *key, size_t klen, const wu8 *data, size_t len, wu8 out[20])
{
	struct hmac_ctx h;
	hmac_init(&h, key, klen); sha1_update(&h.in, data, len); hmac_final(&h, out);
}

/* ---------------- PBKDF2 (802.11i H.4) ---------------- */
void wpa_pbkdf2_psk(const char *pass, const wu8 *ssid, size_t ssid_len, wu8 psk[32])
{
	size_t plen = 0;
	wu8 out[40], u[20], t[20];
	int blk, i, it;
	while (pass[plen]) plen++;
	for (blk = 1; blk <= 2; blk++) {
		struct hmac_ctx h;
		wu8 cnt[4] = { 0, 0, 0, (wu8)blk };
		hmac_init(&h, (const wu8 *)pass, plen);
		sha1_update(&h.in, ssid, ssid_len);
		sha1_update(&h.in, cnt, 4);
		hmac_final(&h, u);
		memcpy(t, u, 20);
		for (it = 1; it < 4096; it++) {
			wpa_hmac_sha1((const wu8 *)pass, plen, u, 20, u);
			for (i = 0; i < 20; i++) t[i] ^= u[i];
		}
		memcpy(out + 20 * (blk - 1), t, 20);
	}
	memcpy(psk, out, 32);
}

/* ---------------- PRF-384 (802.11i 8.5.1.1) ---------------- */
static void prf(const wu8 *key, size_t klen, const char *label, const wu8 *data, size_t dlen, wu8 *out, size_t outlen)
{
	wu8 buf[128], r[20];
	size_t ll = 0, pos = 0, done = 0;
	wu8 ctr = 0;
	while (label[ll]) ll++;
	memcpy(buf, label, ll); buf[ll] = 0;
	memcpy(buf + ll + 1, data, dlen);
	pos = ll + 1 + dlen;
	while (done < outlen) {
		size_t n = outlen - done > 20 ? 20 : outlen - done;
		buf[pos] = ctr++;
		wpa_hmac_sha1(key, klen, buf, pos + 1, r);
		memcpy(out + done, r, n); done += n;
	}
}
static int memcmp_min(const wu8 *a, const wu8 *b, size_t n)
{
	for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
	return 0;
}
void wpa_derive_ptk(const wu8 pmk[32], const wu8 aa[6], const wu8 spa[6],
		    const wu8 anonce[32], const wu8 snonce[32], wu8 ptk[48])
{
	wu8 d[76];
	if (memcmp_min(aa, spa, 6) < 0) { memcpy(d, aa, 6); memcpy(d + 6, spa, 6); }
	else { memcpy(d, spa, 6); memcpy(d + 6, aa, 6); }
	if (memcmp_min(anonce, snonce, 32) < 0) { memcpy(d + 12, anonce, 32); memcpy(d + 44, snonce, 32); }
	else { memcpy(d + 12, snonce, 32); memcpy(d + 44, anonce, 32); }
	prf(pmk, 32, "Pairwise key expansion", d, 76, ptk, 48);
}

/* ---------------- AES-128 (descifrado, para el key unwrap) ---------------- */
static wu8 sbox[256], inv_sbox[256];
static int aes_ready;
static wu8 xt(wu8 x) { return (wu8)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }
static wu8 gmul(wu8 a, wu8 b)
{
	wu8 p = 0;
	while (b) { if (b & 1) p ^= a; a = xt(a); b >>= 1; }
	return p;
}
static wu8 rotl8(wu8 x, int n) { return (wu8)((x << n) | (x >> (8 - n))); }
static void aes_init_tables(void)
{
	wu8 p = 1, q = 1;
	int i;
	if (aes_ready) return;
	do {
		wu8 x;
		p = (wu8)(p ^ (wu8)(p << 1) ^ ((p & 0x80) ? 0x1b : 0));
		q ^= (wu8)(q << 1); q ^= (wu8)(q << 2); q ^= (wu8)(q << 4);
		if (q & 0x80) q ^= 0x09;
		x = (wu8)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4));
		sbox[p] = x ^ 0x63;
	} while (p != 1);
	sbox[0] = 0x63;
	for (i = 0; i < 256; i++) inv_sbox[sbox[i]] = (wu8)i;
	aes_ready = 1;
}
static void aes_expand(const wu8 key[16], wu8 rk[176])
{
	wu8 rcon = 1;
	int i;
	memcpy(rk, key, 16);
	for (i = 16; i < 176; i += 4) {
		wu8 t[4];
		memcpy(t, rk + i - 4, 4);
		if (i % 16 == 0) {
			wu8 a = t[0];
			t[0] = sbox[t[1]] ^ rcon; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[a];
			rcon = xt(rcon);
		}
		for (int j = 0; j < 4; j++) rk[i + j] = rk[i - 16 + j] ^ t[j];
	}
}
static void aes_dec_block(const wu8 rk[176], const wu8 in[16], wu8 out[16])
{
	wu8 s[16], t[16];
	int r, i;
	memcpy(s, in, 16);
	for (i = 0; i < 16; i++) s[i] ^= rk[160 + i];
	for (r = 9; r >= 0; r--) {
		/* InvShiftRows */
		t[0]=s[0]; t[4]=s[4]; t[8]=s[8]; t[12]=s[12];
		t[1]=s[13]; t[5]=s[1]; t[9]=s[5]; t[13]=s[9];
		t[2]=s[10]; t[6]=s[14]; t[10]=s[2]; t[14]=s[6];
		t[3]=s[7]; t[7]=s[11]; t[11]=s[15]; t[15]=s[3];
		for (i = 0; i < 16; i++) s[i] = inv_sbox[t[i]] ^ rk[16 * r + i];
		if (r) { /* InvMixColumns */
			for (i = 0; i < 4; i++) {
				wu8 a = s[4*i], b = s[4*i+1], c = s[4*i+2], d = s[4*i+3];
				s[4*i]   = gmul(a,14) ^ gmul(b,11) ^ gmul(c,13) ^ gmul(d,9);
				s[4*i+1] = gmul(a,9)  ^ gmul(b,14) ^ gmul(c,11) ^ gmul(d,13);
				s[4*i+2] = gmul(a,13) ^ gmul(b,9)  ^ gmul(c,14) ^ gmul(d,11);
				s[4*i+3] = gmul(a,11) ^ gmul(b,13) ^ gmul(c,9)  ^ gmul(d,14);
			}
		}
	}
	memcpy(out, s, 16);
}
/* RFC 3394: cipher = (n+1)*8 bytes, plain = n*8 bytes. 0 si el IV es A6A6.. */
int wpa_aes_unwrap(const wu8 kek[16], int n, const wu8 *cipher, wu8 *plain)
{
	wu8 rk[176], a[8], b[16];
	int i, j;
	aes_init_tables();
	aes_expand(kek, rk);
	memcpy(a, cipher, 8);
	memcpy(plain, cipher + 8, 8 * n);
	for (j = 5; j >= 0; j--) {
		for (i = n; i >= 1; i--) {
			wu32 tt = (wu32)(n * j + i);
			memcpy(b, a, 8);
			b[7] ^= (wu8)tt; b[6] ^= (wu8)(tt >> 8); b[5] ^= (wu8)(tt >> 16); b[4] ^= (wu8)(tt >> 24);
			memcpy(b + 8, plain + 8 * (i - 1), 8);
			aes_dec_block(rk, b, b);
			memcpy(a, b, 8);
			memcpy(plain + 8 * (i - 1), b + 8, 8);
		}
	}
	for (i = 0; i < 8; i++) if (a[i] != 0xA6) return -1;
	return 0;
}

/* ---------------- EAPOL-Key ---------------- */
static wu16 get16(const wu8 *p) { return (wu16)((p[0] << 8) | p[1]); }
static void put16(wu8 *p, wu16 v) { p[0] = v >> 8; p[1] = (wu8)v; }

int wpa_parse_eapol_key(const wu8 *buf, size_t len, struct wpa_eapol_key *k)
{
	/* version(1) type(1) len(2) | desc(1) info(2) keylen(2) replay(8) nonce(32) iv(16) rsc(8) id(8) mic(16) dlen(2) */
	if (len < WPA_KEY_FIXED) return -1;
	if (buf[1] != 3) return -1;          /* EAPOL-Key */
	if (buf[4] != 2 && buf[4] != 254) return -1; /* RSN o WPA */
	if ((size_t)get16(buf + 2) + 4 > len) return -1;
	k->info = get16(buf + 5);
	k->key_len = get16(buf + 7);
	memcpy(k->replay, buf + 9, 8);
	memcpy(k->nonce, buf + 17, 32);
	memcpy(k->mic, buf + 81, 16);
	k->data_len = get16(buf + 97);
	if ((size_t)99 + k->data_len > len) return -1;
	k->data = buf + 99;
	return 0;
}

size_t wpa_build_eapol_key(wu8 *out, wu16 info, wu16 key_len, const wu8 replay[8],
			   const wu8 nonce[32], const wu8 *data, wu16 data_len, const wu8 kck[16])
{
	size_t total = 99 + data_len;
	wu8 mic[20];
	memset(out, 0, total);
	out[0] = 2; out[1] = 3; put16(out + 2, (wu16)(total - 4));
	out[4] = 2;
	put16(out + 5, info); put16(out + 7, key_len);
	memcpy(out + 9, replay, 8);
	memcpy(out + 17, nonce, 32);
	put16(out + 97, data_len);
	if (data_len) memcpy(out + 99, data, data_len);
	if (info & WPA_INFO_MIC) {
		wpa_hmac_sha1(kck, 16, out, total, mic);
		memcpy(out + 81, mic, 16);
	}
	return total;
}

int wpa_check_mic(const wu8 *buf, size_t len, const wu8 kck[16])
{
	wu8 tmp[512], mic[20];
	int i, diff = 0;
	if (len > sizeof(tmp) || len < 99) return 0;
	memcpy(tmp, buf, len);
	memset(tmp + 81, 0, 16);
	wpa_hmac_sha1(kck, 16, tmp, len, mic);
	for (i = 0; i < 16; i++) diff |= mic[i] ^ buf[81 + i];
	return diff == 0;
}

int wpa_find_gtk(const wu8 *data, size_t len, wu8 gtk[32], int *gtk_len, int *keyidx)
{
	size_t p = 0;
	while (p + 2 <= len) {
		wu8 id = data[p], l = data[p + 1];
		if (id == 0xdd && p + 2 + l <= len && l >= 6 &&
		    data[p+2] == 0x00 && data[p+3] == 0x0f && data[p+4] == 0xac && data[p+5] == 1) {
			int gl = l - 6;  /* OUI(3)+tipo(1)+keyid(1)+reservado(1) */
			if (gl < 0 || gl > 32) return -1;
			*keyidx = data[p + 6] & 3;
			memcpy(gtk, data + p + 8, gl);
			*gtk_len = gl;
			return 0;
		}
		if (id == 0 && l == 0) break; /* relleno */
		p += 2 + l;
	}
	return -1;
}
