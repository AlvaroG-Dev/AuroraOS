/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cripto y mensajes EAPOL del 4-way handshake WPA2-PSK (CCMP) sin dependencias
 * del kernel: se prueba en el host (tests/linuxkpi/wpa_psk_test.c) y se usa
 * desde aurora_wifi.c (Plan_Red.md, N7.3).
 */
#ifndef WPA_PSK_H
#define WPA_PSK_H
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/string.h>
typedef u8 wu8; typedef u16 wu16; typedef u32 wu32; typedef u64 wu64;
#else
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t wu8; typedef uint16_t wu16; typedef uint32_t wu32; typedef uint64_t wu64;
#endif

void wpa_sha1(const wu8 *data, size_t len, wu8 out[20]);
void wpa_hmac_sha1(const wu8 *key, size_t klen, const wu8 *data, size_t len, wu8 out[20]);
/* PSK de 32 bytes a partir de passphrase (8..63 chars) y SSID */
void wpa_pbkdf2_psk(const char *pass, const wu8 *ssid, size_t ssid_len, wu8 psk[32]);
/* PTK de 48 bytes (CCMP: KCK 16, KEK 16, TK 16) */
void wpa_derive_ptk(const wu8 pmk[32], const wu8 aa[6], const wu8 spa[6],
		    const wu8 anonce[32], const wu8 snonce[32], wu8 ptk[48]);
/* AES-128 (solo lo necesario para el key unwrap de la GTK) */
void wpa_aes128_unwrap_prepare(const wu8 key[16], wu32 rk[44]);
int wpa_aes_unwrap(const wu8 kek[16], int n, const wu8 *cipher, wu8 *plain);

/* ---- EAPOL-Key ---- */
#define WPA_EAPOL_HDR 4
#define WPA_KEY_FIXED 95  /* cabecera EAPOL-Key sin datos, con la cabecera EAPOL de 4 bytes */
#define WPA_INFO_VER_MASK 0x0007
#define WPA_INFO_PAIRWISE 0x0008
#define WPA_INFO_INSTALL 0x0040
#define WPA_INFO_ACK 0x0080
#define WPA_INFO_MIC 0x0100
#define WPA_INFO_SECURE 0x0200
#define WPA_INFO_ENCDATA 0x1000

struct wpa_eapol_key {
	wu16 info;
	wu16 key_len;
	wu8 replay[8];
	wu8 nonce[32];
	wu8 mic[16];
	wu16 data_len;
	const wu8 *data; /* apunta dentro del buffer analizado */
};
/* Analiza un EAPOL-Key (buf = desde la version EAPOL). 0 si valido. */
int wpa_parse_eapol_key(const wu8 *buf, size_t len, struct wpa_eapol_key *k);
/* Construye el mensaje 2 o 4 (sin MIC calculado -> lo calcula con kck). Devuelve longitud. */
size_t wpa_build_eapol_key(wu8 *out, wu16 info, wu16 key_len, const wu8 replay[8],
			   const wu8 nonce[32], const wu8 *data, wu16 data_len, const wu8 kck[16]);
/* Verifica el MIC (HMAC-SHA1-128, version 2) de un mensaje recibido. 1 = valido. */
int wpa_check_mic(const wu8 *buf, size_t len, const wu8 kck[16]);
/* Busca la GTK (KDE 00-0F-AC:1) en los datos descifrados; devuelve 0 y la rellena. */
int wpa_find_gtk(const wu8 *data, size_t len, wu8 gtk[32], int *gtk_len, int *keyidx);
#endif
