/* Prueba en el host de kernel/linuxkpi/drivers/lkpi_cfg80211/wpa_psk.c
 *   gcc -Wall -o /tmp/wpa_psk_test tests/linuxkpi/wpa_psk_test.c \
 *       kernel/linuxkpi/drivers/lkpi_cfg80211/wpa_psk.c && /tmp/wpa_psk_test */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../kernel/linuxkpi/drivers/lkpi_cfg80211/wpa_psk.h"
static int fails;
static void hex(const char *s, unsigned char *o, int n) { for (int i = 0; i < n; i++) { unsigned v; sscanf(s + 2*i, "%2x", &v); o[i] = v; } }
static void check(const char *name, const unsigned char *got, const char *exp, int n)
{
	unsigned char e[64]; hex(exp, e, n);
	if (memcmp(got, e, n)) { printf("FALLO %s\n", name); fails++; } else printf("ok    %s\n", name);
}
int main(void)
{
	unsigned char d[64], psk[32];
	wpa_sha1((const unsigned char *)"abc", 3, d);
	check("sha1(abc)", d, "a9993e364706816aba3e25717850c26c9cd0d89d", 20);
	unsigned char k[20]; memset(k, 0x0b, 20);
	wpa_hmac_sha1(k, 20, (const unsigned char *)"Hi There", 8, d);
	check("hmac-sha1 rfc2202 #1", d, "b617318655057264e28bc0b6fb378c8ef146be00", 20);
	wpa_hmac_sha1((const unsigned char *)"Jefe", 4, (const unsigned char *)"what do ya want for nothing?", 28, d);
	check("hmac-sha1 rfc2202 #2", d, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79", 20);
	wpa_pbkdf2_psk("password", (const unsigned char *)"IEEE", 4, psk);
	check("psk 802.11i #1", psk, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e", 32);
	wpa_pbkdf2_psk("ThisIsAPassword", (const unsigned char *)"ThisIsASSID", 11, psk);
	check("psk 802.11i #2", psk, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af", 32);
	/* RFC 3394 4.1: KEK 000102..0F, clave 00112233445566778899AABBCCDDEEFF */
	unsigned char kek[16], c[24], p[16];
	hex("000102030405060708090A0B0C0D0E0F", kek, 16);
	hex("1FA68B0A8112B447AEF34BD8FB5A7B829D3E862371D2CFE5", c, 24);
	int r = wpa_aes_unwrap(kek, 2, c, p);
	if (r) { printf("FALLO unwrap iv\n"); fails++; } else check("aes keywrap rfc3394", p, "00112233445566778899AABBCCDDEEFF", 16);
	/* Handshake simulado: AP y STA derivan la misma PTK y se verifican los MIC */
	unsigned char aa[6] = {2,0,0,0,0,1}, spa[6] = {2,0,0,0,0,2}, an[32], sn[32], ptk_s[48], ptk_a[48], rc[8] = {0,0,0,0,0,0,0,1};
	for (int i = 0; i < 32; i++) { an[i] = i; sn[i] = 0xa0 + i; }
	wpa_derive_ptk(psk, aa, spa, an, sn, ptk_s);
	wpa_derive_ptk(psk, spa, aa, sn, an, ptk_a);
	if (memcmp(ptk_s, ptk_a, 48)) { printf("FALLO ptk simetria\n"); fails++; } else printf("ok    ptk simetrica\n");
	unsigned char m2[200]; unsigned char rsn[22] = {0x30,20,1,0,0,0x0f,0xac,4,1,0,0,0x0f,0xac,4,1,0,0,0x0f,0xac,2,0,0};
	size_t l = wpa_build_eapol_key(m2, WPA_INFO_PAIRWISE | WPA_INFO_MIC | 2, 16, rc, sn, rsn, 22, ptk_s);
	struct wpa_eapol_key ek;
	if (wpa_parse_eapol_key(m2, l, &ek) || !wpa_check_mic(m2, l, ptk_a) || ek.data_len != 22) { printf("FALLO msg2\n"); fails++; } else printf("ok    msg2 mic\n");
	m2[100] ^= 1;
	if (wpa_check_mic(m2, l, ptk_a)) { printf("FALLO mic acepta alterado\n"); fails++; } else printf("ok    mic rechaza alterado\n");
	unsigned char gk[32], gtk_out[32]; int gl, ki;
	unsigned char kde[8 + 16] = {0xdd, 6 + 16, 0, 0x0f, 0xac, 1, 2, 0};
	memset(kde + 8, 0x42, 16);
	if (wpa_find_gtk(kde, sizeof(kde), gtk_out, &gl, &ki) || gl != 16 || ki != 2 || gtk_out[0] != 0x42) { printf("FALLO gtk kde\n"); fails++; } else printf("ok    gtk kde\n");
	(void)gk;
	printf(fails ? "%d FALLOS\n" : "todo ok\n", fails);
	return fails != 0;
}
