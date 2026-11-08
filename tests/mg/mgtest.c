/*  Keyed MagicGate: DES, the key store, the card handshake and KELF.
 *
 *  No real key material is used or needed. The key set is random; the
 *  checks are that both ends of each exchange agree -- the mechacon
 *  (pcsx2/CDVD/MechaMg.c through its SCMD entry point) and the emulated
 *  card, or the mechacon and a KELF built here by inverting each step it
 *  takes -- and that nothing reads out of bounds on malformed input.
 *  Whether the flow matches a real console is for real dumps to show.
 *
 *  Usage: tests/mg/mgtest <scratch dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MgDes.h"
#include "MechaMg.h"
#include "SaveState.h"
#include "common/Pcsx2Defs.h"

retro_log_printf_t log_cb;

static void quiet_log(enum retro_log_level level, const char* fmt, ...)
{
	(void)level;
	(void)fmt;
}

static int failures;

#define CHECK(cond, what) do { if (!(cond)) { printf("  FAIL %s\n", what); failures++; } } while (0)

static void hex(const char* s, u8* out, int n)
{
	int i;
	for (i = 0; i < n; i++)
	{
		unsigned v;
		sscanf(s + 2 * i, "%2x", &v);
		out[i] = (u8)v;
	}
}

static u32 s_seed = 12345;
static u8 rnd(void)
{
	s_seed = s_seed * 1103515245u + 12345u;
	return (u8)(s_seed >> 16);
}

static void fill(u8* p, int n)
{
	int i;
	for (i = 0; i < n; i++)
		p[i] = rnd();
}

static void xor8(u8* out, const u8* a, const u8* b)
{
	int i;
	for (i = 0; i < 8; i++)
		out[i] = (u8)(a[i] ^ b[i]);
}

static void wr16(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8* p, u32 v) { wr16(p, v); wr16(p + 2, v >> 16); }

/* ---- DES ----------------------------------------------------------------- */

static void test_des(void)
{
	/* FIPS 81 / NIST known answers; the 3DES ones from OpenSSL des-ede-ecb. */
	static const char* const v[][3] = {
		{"133457799BBCDFF1", "0123456789ABCDEF", "85E813540F0AB405"},
		{"0123456789ABCDEF", "4E6F772069732074", "3FA40E8A984D4815"},
		{"0101010101010101", "8000000000000000", "95F8A5E5DD31D900"},
		{"8001010101010101", "0000000000000000", "95A8D72813DAA94D"}};
	static const char* const v3[][3] = {
		{"00112233445566778899aabbccddeeff", "0123456789abcdef", "4cd648347c7c41f1"},
		{"fedcba98765432100123456789abcdef", "1122334455667788", "ecaa54f11cedc517"}};
	u8 k[16], p[8], w[8], b[8];
	int i;

	for (i = 0; i < 4; i++)
	{
		hex(v[i][0], k, 8);
		hex(v[i][1], p, 8);
		hex(v[i][2], w, 8);
		memcpy(b, p, 8);
		mg_des_encrypt(k, b);
		CHECK(memcmp(b, w, 8) == 0, "DES known answer");
		mg_des_decrypt(k, b);
		CHECK(memcmp(b, p, 8) == 0, "DES decrypts its known answer");
	}
	for (i = 0; i < 2; i++)
	{
		hex(v3[i][0], k, 16);
		hex(v3[i][1], p, 8);
		hex(v3[i][2], w, 8);
		memcpy(b, p, 8);
		mg_3des_encrypt(k, b);
		CHECK(memcmp(b, w, 8) == 0, "two-key 3DES known answer");
		mg_3des_decrypt(k, b);
		CHECK(memcmp(b, p, 8) == 0, "two-key 3DES decrypts its known answer");
	}
	/* EDE with K1 == K2 is single DES. */
	hex(v[0][0], k, 8);
	memcpy(k + 8, k, 8);
	hex(v[0][1], b, 8);
	mg_3des_encrypt(k, b);
	hex(v[0][2], w, 8);
	CHECK(memcmp(b, w, 8) == 0, "3DES with equal halves is DES");
}

/* ---- the key store ------------------------------------------------------- */

/* The store's layout, transcribed: where each card block of a key set
 * sits (word index, 0x1000 + card block, or 0xFFFF for zeros), the KELF
 * key run and the challenge IV. */
static const u16 s_idx[72] = {
	0x0018, 0xFFFF, 0xFFFF, 0x001C, 0xFFFF, 0xFFFF, 0x0020, 0xFFFF, 0xFFFF, 0x0024, 0xFFFF, 0xFFFF, 0x0028, 0xFFFF, 0xFFFF, 0x002C, 0xFFFF, 0xFFFF,
	0x0030, 0x0048, 0x0060, 0x0034, 0x004C, 0x0064, 0x0038, 0x0050, 0x0068, 0x003C, 0x0054, 0x006C, 0x0040, 0x0058, 0x0070, 0x0044, 0x005C, 0x0074,
	0x0000, 0x1000, 0x1001, 0x0004, 0x1002, 0x1003, 0x0008, 0x1004, 0x1005, 0x000C, 0x1006, 0x1007, 0x0010, 0x1008, 0x1009, 0x0014, 0x100A, 0x100B,
	0x0090, 0x00A8, 0x00A8, 0x0094, 0x00AC, 0x00AC, 0x0098, 0x00B0, 0x00B0, 0x009C, 0x00B4, 0x00B4, 0x00A0, 0x00B8, 0x00B8, 0x00A4, 0x00BC, 0x00BC};
static const u16 s_kelf_idx[4] = {0x110, 0x110, 0xC4, 0x15C};

/* Encrypt a 304-byte plain key set into a store for one mode. Blocks the
 * layout shares (zeros, repeated indexes) must hold equal plain values,
 * so the caller's set is adjusted to match what the store can express. */
static void encode_store(u8 plain[304], u8 enc[1024], u8 card[96], const u8 key[16], int mode)
{
	u8 blk[8];
	int i;

	for (i = 0; i < 18; i++)
	{
		const u16 idx = s_idx[18 * mode + i];
		if (idx == 0xFFFF)
		{
			memset(blk, 0, 8);
			mg_3des_decrypt(key, blk);  /* plain = D(0) */
			memcpy(plain + 8 * i, blk, 8);
			continue;
		}
		memcpy(blk, plain + 8 * i, 8);
		mg_3des_encrypt(key, blk);
		if (idx >= 0x200)
			memcpy(card + 8 * (idx & 0x0f), blk, 8);
		else
			memcpy(enc + 2 * idx, blk, 8);
	}
	for (i = 0; i < 19; i++)
	{
		memcpy(blk, plain + 144 + 8 * i, 8);
		mg_3des_encrypt(key, blk);
		memcpy(enc + 2 * s_kelf_idx[mode] + 8 * i, blk, 8);
	}
	memcpy(blk, plain + 296, 8);
	mg_3des_encrypt(key, blk);
	memcpy(enc + 2 * 192, blk, 8);
}

static void test_keystore(void)
{
	int mode;
	for (mode = 0; mode < 4; mode++)
	{
		u8 plain[304], enc[1024], card[96], key[16];
		MechaMgKeyStore ks;
		fill(plain, sizeof(plain));
		fill(enc, sizeof(enc));
		fill(card, sizeof(card));
		fill(key, sizeof(key));
		/* Mode 3 reads the same word for its two key-2 halves. */
		if (mode == 3)
		{
			int i;
			for (i = 0; i < 6; i++)
				memcpy(plain + 8 * (3 * i + 2), plain + 8 * (3 * i + 1), 8);
		}
		encode_store(plain, enc, card, key, mode);
		mecha_mg_decode_keystore(&ks, enc, card, key, mode);
		CHECK(sizeof(ks) == 304, "key set is 304 bytes");
		CHECK(memcmp(&ks, plain, 304) == 0, "key store decodes to the key set it was built from");
	}
}

/* ---- the card handshake -------------------------------------------------- */

static MechaMgKeyStore s_ks;
static MechaMgIds s_ids;

static int scmd(u8 cmd, const u8* param, int cnt, u8* res)
{
	memset(res, 0, 16);
	return mecha_mg_scmd(cmd, param, cnt, res, &s_ids);
}

static void arm(void)
{
	fill((u8*)&s_ks, sizeof(s_ks));
	fill(s_ids.console_id, 8);
	fill(s_ids.ilink_id, 8);
	mecha_mg_set_keys(&s_ks, &s_ids);
}

/* Run the handshake for a key slot and key set index; corrupt = which
 * response to damage (0 none). Returns 0x8F's verdict after response 3. */
static u8 handshake(int slot, int index, int corrupt, int card_slot)
{
	u8 res[16], p[16], iv[8], seed[8], nonce[8], r[3][8];
	u8 one;

	scmd(0x80, (one = 0, &one), 1, res);
	CHECK(res[0] == 0, "0x80 accepted");
	one = (u8)(slot | (index << 6));
	scmd(0x81, &one, 1, res);
	CHECK(res[0] == 0, "0x81 takes slot and key index");

	mecha_mg_card_auth_reset(card_slot);
	CHECK(mecha_mg_card_get(card_slot, 0x01, iv), "card gives its IV");
	CHECK(mecha_mg_card_get(card_slot, 0x02, seed), "card gives its seed");
	CHECK(mecha_mg_card_get(card_slot, 0x04, nonce), "card gives its nonce");

	memcpy(p, iv, 8);
	memcpy(p + 8, seed, 8);
	scmd(0x82, p, 16, res);
	CHECK(res[0] == 0, "0x82 takes IV and seed");
	scmd(0x83, nonce, 8, res);
	CHECK(res[0] == 0, "0x83 takes the nonce");
	scmd(0x8F, NULL, 0, res);
	CHECK(res[0] == 0, "challenge generated");

	CHECK(scmd(0x84, NULL, 0, res) == 13 && res[0] == 0, "0x84 gives challenge 1 and half of 2");
	mecha_mg_card_put(card_slot, 0x0b, res + 1);
	memcpy(p, res + 9, 4);
	CHECK(scmd(0x85, NULL, 0, res) == 13 && res[0] == 0, "0x85 gives the rest of 2 and 3");
	memcpy(p + 4, res + 1, 4);
	mecha_mg_card_put(card_slot, 0x07, p);
	mecha_mg_card_put(card_slot, 0x06, res + 5);

	CHECK(mecha_mg_card_get(card_slot, 0x0f, r[0]), "card answers 1");
	CHECK(mecha_mg_card_get(card_slot, 0x11, r[1]), "card answers 2");
	CHECK(mecha_mg_card_get(card_slot, 0x13, r[2]), "card answers 3");
	if (corrupt)
		r[corrupt - 1][3] ^= 0x40;

	memcpy(p, r[0], 8);
	memcpy(p + 8, r[1], 8);
	scmd(0x86, p, 16, res);
	CHECK(res[0] == 0, "0x86 takes responses 1 and 2");
	scmd(0x87, r[2], 8, res);
	CHECK(res[0] == 0, "0x87 takes response 3");
	scmd(0x8F, NULL, 0, res);
	return res[0];
}

static void test_card(void)
{
	u8 res[16];
	int index, c;

	arm();
	for (index = 0; index < 3; index++)
	{
		CHECK(handshake(5, index, 0, 2) == 0, "card handshake verifies, key sets 0-2");
		scmd(0x88, NULL, 0, res);
		CHECK(res[0] == 0, "0x88 after a verified card");
	}
	/* Responses 1 and 2 prove the card holds the key; response 3 only
	 * carries the card's ID, which the mechacon keeps unchecked. */
	for (c = 1; c <= 3; c++)
	{
		const u8 verdict = handshake(1, 0, c, 0);
		scmd(0x88, NULL, 0, res);
		if (c < 3)
		{
			CHECK(verdict != 0, "a damaged card response 1 or 2 is refused");
			CHECK(res[0] == 0x80, "0x88 refuses after a refused card");
		}
		else
			CHECK(verdict == 0 && res[0] == 0, "response 3 is the card ID, taken as given");
	}
	/* Out of order: 0x84 before any challenge. */
	scmd(0x80, (const u8*)"\0", 1, res);
	CHECK(scmd(0x84, NULL, 0, res) == 1 && res[0] == 0x80, "0x84 out of order is refused");
	/* Slot 16 does not exist; key set index 3 neither. */
	{
		u8 one = 16;
		scmd(0x81, &one, 1, res);
		CHECK(res[0] == 0x80, "0x81 refuses key slot 16");
		one = 3 << 6;
		scmd(0x81, &one, 1, res);
		CHECK(res[0] == 0x80, "0x81 refuses key set index 3");
	}
}

/* ---- savestate round trip mid-handshake --------------------------------- */

static void test_state(void)
{
	SaveStateBase s;
	u8* buf = (u8*)malloc(1 << 16);
	u8 res[16], p[16], iv[8], seed[8], nonce[8], r[3][8], one = 0;

	arm();
	scmd(0x80, &one, 1, res);
	scmd(0x81, &one, 1, res);
	mecha_mg_card_auth_reset(0);
	mecha_mg_card_get(0, 0x01, iv);
	mecha_mg_card_get(0, 0x02, seed);
	mecha_mg_card_get(0, 0x04, nonce);
	memcpy(p, iv, 8);
	memcpy(p + 8, seed, 8);
	scmd(0x82, p, 16, res);
	scmd(0x83, nonce, 8, res);
	scmd(0x8F, NULL, 0, res);

	/* Save here, scramble by starting over, load, and finish. */
	SaveState_Init(&s, buf, 0, 1 << 16, true);
	CHECK(mecha_mg_freeze(&s), "freeze saves");
	{
		const size_t used = s.idx;
		scmd(0x80, &one, 1, res);
		mecha_mg_card_auth_reset(0);
		mecha_mg_card_get(0, 0x01, iv);
		SaveState_Init(&s, s.memory, used, s.memory_size, false);
		CHECK(mecha_mg_freeze(&s), "freeze loads");
	}

	CHECK(scmd(0x84, NULL, 0, res) == 13, "after a load, 0x84 continues");
	mecha_mg_card_put(0, 0x0b, res + 1);
	memcpy(p, res + 9, 4);
	scmd(0x85, NULL, 0, res);
	memcpy(p + 4, res + 1, 4);
	mecha_mg_card_put(0, 0x07, p);
	mecha_mg_card_put(0, 0x06, res + 5);
	mecha_mg_card_get(0, 0x0f, r[0]);
	mecha_mg_card_get(0, 0x11, r[1]);
	mecha_mg_card_get(0, 0x13, r[2]);
	memcpy(p, r[0], 8);
	memcpy(p + 8, r[1], 8);
	scmd(0x86, p, 16, res);
	scmd(0x87, r[2], 8, res);
	scmd(0x8F, NULL, 0, res);
	CHECK(res[0] == 0, "a handshake saved and loaded half way verifies");
	free(s.memory);
}

/* ---- KELF ---------------------------------------------------------------- */

#define NBLK 3

/* The application type the builder writes at header byte 23, and, when
 * set, the Kbit|Kc it encrypts with while the header wraps others: an
 * arcade application-type 0x07 KELF, whose keys the mechacon holds. */
static u8 s_kelf_app_type;
static const u8* s_kelf_keys;

/* Build a mode-0 KELF the way the mechacon takes one apart: header,
 * its signature, Kbit/Kc under the key-encryption key, the bit table
 * chained under Kbit, its signature and the root signature; then the
 * content, CBC under Kc, with each block's signature. */
static int build_kelf_n(u8* out, int nblk, u8* const* plain, const u32* sizes, const u32* bflags,
	u8* const* content, u16 extra_flags, u8 root_src_out[8], u8 kbit_out[16], u8 kc_out[16])
{
	const u16 hflags = (u16)((2 << 8) | (2 << 4) | extra_flags); /* CBC, two keys */
	u8 kbit[16], kc[16], wrap_kbit[16], wrap_kc[16], kek[16], nonce[8], header_sig[8], bit_sig[8], root[8], prev[8], sig_key[16], blk_sig[16][8];
	u8 table[8 + 16 * 16];
	const int table_len = 8 + 16 * nblk;
	int i, size, off;
	u32 b;

	memset(out, 0, 0x800);
	fill(out, 16);                 /* nonce */
	wr32(out + 16, 0);
	wr16(out + 24, hflags);
	wr16(out + 26, 0);             /* no bans */
	out[23] = s_kelf_app_type;
	fill(kbit, 16);
	fill(kc, 16);
	memcpy(wrap_kbit, kbit, 16);
	memcpy(wrap_kc, kc, 16);
	if (s_kelf_keys)
	{
		memcpy(kbit, s_kelf_keys, 16);
		memcpy(kc, s_kelf_keys + 16, 16);
	}

	/* Content and block signatures first: the bit table carries them. */
	for (b = 0; b < (u32)nblk; b++)
	{
		u8 s[8] = {0};
		u8 last[8];
		memcpy(content[b], plain[b], sizes[b]);
		memcpy(last, s_ks.ContentIV, 8);
		for (i = 0; i < (int)sizes[b]; i += 8)
		{
			if (bflags[b] & 1)
			{
				u8 t[8];
				xor8(s, s, plain[b] + i);
				xor8(t, plain[b] + i, last);
				mg_3des_encrypt(kc, t);
				memcpy(content[b] + i, t, 8);
				memcpy(last, t, 8);
			}
			else
			{
				xor8(s, s, plain[b] + i);
				mg_des_encrypt(s_ks.SignatureMasterKey, s);
			}
		}
		if (bflags[b] & 1)
		{
			memcpy(sig_key, s_ks.SignatureMasterKey, 8);
			memcpy(sig_key + 8, s_ks.SignatureHashKey, 8);
			mg_3des_encrypt(sig_key, s);
		}
		else
		{
			mg_des_decrypt(s_ks.SignatureHashKey, s);
			mg_des_encrypt(s_ks.SignatureMasterKey, s);
		}
		memcpy(blk_sig[b], s, 8);
	}

	/* Plain bit table. */
	memset(table, 0, sizeof(table));
	wr32(table, 0);
	table[4] = (u8)nblk;
	for (b = 0; b < (u32)nblk; b++)
	{
		wr32(table + 8 + 16 * b, sizes[b]);
		wr32(table + 8 + 16 * b + 4, bflags[b]);
		memcpy(table + 8 + 16 * b + 8, blk_sig[b], 8);
	}

	size = 32 + 8 + 32 + table_len + 16;
	wr16(out + 20, (u32)size);

	/* Header signature over the 32 header bytes. */
	memset(header_sig, 0, 8);
	for (i = 0; i < 32; i += 8)
	{
		xor8(header_sig, out + i, header_sig);
		mg_des_encrypt(s_ks.SignatureMasterKey, header_sig);
	}
	mg_des_decrypt(s_ks.SignatureHashKey, header_sig);
	mg_des_encrypt(s_ks.SignatureMasterKey, header_sig);
	memcpy(out + 32, header_sig, 8);

	/* Kbit and Kc under the key-encryption key. */
	xor8(nonce, out, out + 8);
	xor8(kek, s_ks.KbitIv, nonce);
	mg_3des_encrypt(s_ks.KbitMasterKey, kek);
	xor8(kek + 8, s_ks.KcIv, nonce);
	mg_3des_encrypt(s_ks.KcMasterKey, kek + 8);
	memcpy(out + 40, wrap_kbit, 16);
	memcpy(out + 56, wrap_kc, 16);
	for (i = 0; i < 4; i++)
		mg_3des_encrypt(kek, out + 40 + 8 * i);

	/* Bit table signature, over the plain table. */
	memcpy(bit_sig, kbit, 8);
	if (memcmp(kbit, kbit + 8, 8) != 0)
		xor8(bit_sig, kbit + 8, bit_sig);
	xor8(bit_sig, kc, bit_sig);
	if (memcmp(kc, kc + 8, 8) != 0)
		xor8(bit_sig, kc + 8, bit_sig);
	for (i = 0; i < nblk * 2 + 1; i++)
		xor8(bit_sig, table + 8 * i, bit_sig);
	memcpy(sig_key, s_ks.SignatureMasterKey, 8);
	memcpy(sig_key + 8, s_ks.SignatureHashKey, 8);
	mg_3des_encrypt(sig_key, bit_sig);

	/* Root signature. */
	memcpy(root, header_sig, 8);
	mg_des_encrypt(s_ks.RootSigMasterKey, root);
	xor8(root, bit_sig, root);
	mg_des_encrypt(s_ks.RootSigMasterKey, root);
	for (b = 0; b < (u32)nblk; b++)
	{
		if (bflags[b] & 2)
		{
			xor8(root, blk_sig[b], root);
			mg_des_encrypt(s_ks.RootSigMasterKey, root);
		}
	}
	if (root_src_out)
		memcpy(root_src_out, root, 8);
	if (kbit_out)
		memcpy(kbit_out, kbit, 16);
	if (kc_out)
		memcpy(kc_out, kc, 16);
	mg_3des_decrypt(s_ks.RootSigHashKey, root);

	/* The table, chained: head under ContentTableIV, then each half with
	 * the previous half's ciphertext. */
	off = 72;
	xor8(prev, table, s_ks.ContentTableIV);
	mg_3des_encrypt(kbit, prev);
	memcpy(out + off, prev, 8);
	for (b = 0; b < (u32)nblk; b++)
	{
		u8 lo[8], hi[8];
		xor8(lo, table + 8 + 16 * b, prev);
		mg_3des_encrypt(kbit, lo);
		xor8(hi, table + 8 + 16 * b + 8, lo);
		mg_3des_encrypt(kbit, hi);
		memcpy(out + off + 8 + 16 * b, lo, 8);
		memcpy(out + off + 8 + 16 * b + 8, hi, 8);
		memcpy(prev, hi, 8);
	}
	memcpy(out + off + table_len, bit_sig, 8);
	memcpy(out + off + table_len + 8, root, 8);
	return size;
}

static int build_kelf(u8* out, u8 plain[NBLK][64], const u32 sizes[NBLK], const u32 bflags[NBLK],
	u8 content[NBLK][64], u16 extra_flags, u8 root_src_out[8], u8 kbit_out[16], u8 kc_out[16])
{
	u8* pp[NBLK];
	u8* cp[NBLK];
	int b;
	for (b = 0; b < NBLK; b++)
	{
		pp[b] = plain[b];
		cp[b] = content[b];
	}
	return build_kelf_n(out, NBLK, pp, sizes, bflags, cp, extra_flags, root_src_out, kbit_out, kc_out);
}

static void send(u8 cmd_unused, const u8* data, int len)
{
	u8 res[16];
	int i;
	(void)cmd_unused;
	for (i = 0; i < len; i += 16)
	{
		const int n = (len - i) < 16 ? (len - i) : 16;
		scmd(0x8D, data + i, n, res);
		CHECK(res[0] == 0, "0x8D takes data");
	}
}

static int receive(u8* out, int len)
{
	u8 res[16];
	int got = 0;
	while (got < len)
	{
		const int n = scmd(0x8E, NULL, 0, res);
		if (n <= 0)
			break;
		memcpy(out + got, res, (size_t)n);
		got += n;
	}
	return got;
}

static void test_kelf(void)
{
	static const u32 sizes[NBLK] = {24, 16, 40};
	static const u32 bflags[NBLK] = {3, 2, 1}; /* enc+signed, signed, enc */
	u8 plain[NBLK][64], content[NBLK][64], kelf[0x800], res[16], p[5], out[64];
	int size, b, n, tamper;

	for (tamper = 0; tamper < 3; tamper++)
	{
		arm();
		for (b = 0; b < NBLK; b++)
			fill(plain[b], 64);
		size = build_kelf(kelf, plain, sizes, bflags, content, 0, NULL, NULL, NULL);
		if (tamper == 1)
			kelf[33] ^= 1;              /* header signature */
		if (tamper == 2)
			content[0][9] ^= 1;         /* signed, encrypted content */

		scmd(0x80, (const u8*)"\0", 1, res);
		p[0] = 0;
		wr16(p + 1, (u32)size);
		p[3] = 0;
		p[4] = 0;
		scmd(0x90, p, 5, res);
		CHECK(res[0] == 0, "0x90 takes a mode-0 header of its size");
		send(0x8D, kelf, size);
		scmd(0x8F, NULL, 0, res);
		if (tamper == 1)
		{
			CHECK(res[0] == 0x84, "a bad header signature is refused with 0x84");
			continue;
		}
		CHECK(res[0] == 0, "KELF header verifies");

		n = scmd(0x91, NULL, 0, res);
		CHECK(n == 3 && res[0] == 0 && (res[1] | (res[2] << 8)) == 8 + 16 * NBLK, "0x91 gives the bit table length");
		{
			u8 table[8 + 16 * NBLK];
			CHECK(receive(table, sizeof(table)) == (int)sizeof(table), "the bit table reads back");
			CHECK(table[4] == NBLK && (table[8] | (table[9] << 8)) == sizes[0], "the bit table reads back decrypted");
		}

		for (b = 0; b < NBLK; b++)
		{
			u8 len[2];
			wr16(len, sizes[b]);
			scmd(0x92, len, 2, res);
			CHECK(res[0] == 0, "0x92 takes the block's length");
			send(0x8D, content[b], (int)sizes[b]);
			scmd(0x8F, NULL, 0, res);
			if (tamper == 2 && b == 0)
			{
				CHECK(res[0] == 0x84, "damaged signed content is refused with 0x84");
				break;
			}
			CHECK(res[0] == 0, "block decrypts and its signature holds");
			scmd(0x93, len, 2, res);
			CHECK(res[0] == 0, "0x93 takes the out length");
			memset(out, 0, sizeof(out));
			CHECK(receive(out, (int)sizes[b]) == (int)sizes[b], "the block reads back");
			CHECK(memcmp(out, plain[b], sizes[b]) == 0, "the block reads back as it was before encryption");
		}
	}
}

/* Mode 2 binds a KELF to a card: Kbit and Kc come back under the card's
 * ID, and with header flag 2 the icvps2 value after them (0x98). */
static void test_kelf_bind(const u8* real_id)
{
	static const u32 sizes[NBLK] = {24, 16, 40};
	static const u32 bflags[NBLK] = {3, 2, 1};
	u8 plain[NBLK][64], content[NBLK][64], kelf[0x800], res[16], p[5];
	u8 root_src[8], kbit[16], kc[16], want[16], icv[16], seed[8];
	u8 card_id[8] = {'L', 'R', 'P', 'S', '2', 'M', 'C', '2'};
	int size, b, i;

	arm();
	/* A card with its own ID (a dongle's file) hands that over instead. */
	mecha_mg_card_set_id(2, real_id);
	if (real_id)
		memcpy(card_id, real_id, 8);
	CHECK(handshake(5, 0, 0, 2) == 0, "card in slot 5 verifies before binding");
	for (b = 0; b < NBLK; b++)
		fill(plain[b], 64);
	size = build_kelf(kelf, plain, sizes, bflags, content, 2, root_src, kbit, kc);

	/* Slot 16 does not exist. */
	p[0] = 1;
	wr16(p + 1, (u32)size);
	p[3] = 16;
	p[4] = 0;
	scmd(0x80, (const u8*)"\0", 1, res);
	scmd(0x90, p, 5, res);
	CHECK(res[0] == 0x80, "0x90 refuses card key slot 16");

	p[0] = 2;
	p[3] = 5;
	scmd(0x80, (const u8*)"\0", 1, res);
	scmd(0x90, p, 5, res);
	CHECK(res[0] == 0, "0x90 takes a mode-2 header");
	send(0x8D, kelf, size);
	scmd(0x8F, NULL, 0, res);
	CHECK(res[0] == 0, "mode-2 header verifies");
	scmd(0x91, NULL, 0, res);
	{
		u8 table[8 + 16 * NBLK];
		receive(table, sizeof(table));
	}
	/* Mode 2 walks the signed blocks only: 0 and 1. */
	for (b = 0; b < 2; b++)
	{
		u8 len[2];
		wr16(len, sizes[b]);
		scmd(0x92, len, 2, res);
		CHECK(res[0] == 0, "0x92 takes the signed block's length");
		send(0x8D, content[b], (int)sizes[b]);
		scmd(0x8F, NULL, 0, res);
		CHECK(res[0] == 0, "signed block checks out");
	}

	memcpy(want, kbit, 16);
	for (i = 0; i < 2; i++)
		mg_des_encrypt(card_id, want + 8 * i);
	CHECK(scmd(0x94, NULL, 0, res) == 9 && memcmp(res + 1, want, 8) == 0, "0x94 gives Kbit under the card ID");
	CHECK(scmd(0x95, NULL, 0, res) == 9 && memcmp(res + 1, want + 8, 8) == 0, "0x95 gives the rest of Kbit");
	memcpy(want, kc, 16);
	for (i = 0; i < 2; i++)
		mg_des_encrypt(card_id, want + 8 * i);
	CHECK(scmd(0x96, NULL, 0, res) == 9 && memcmp(res + 1, want, 8) == 0, "0x96 gives Kc under the card ID");
	CHECK(scmd(0x97, NULL, 0, res) == 9 && memcmp(res + 1, want + 8, 8) == 0, "0x97 gives the rest of Kc");

	xor8(seed, s_ids.ilink_id, s_ids.console_id);
	xor8(icv, seed, s_ks.icvps2LowIV);
	xor8(icv + 8, seed, s_ks.icvps2HiIV);
	mg_3des_encrypt(s_ks.icvps2LowKey, icv);
	mg_3des_encrypt(s_ks.icvps2HiKey, icv + 8);
	memcpy(want, root_src, 8);
	mg_3des_decrypt(icv, want);
	CHECK(scmd(0x98, NULL, 0, res) == 9 && res[0] == 0 && memcmp(res + 1, want, 8) == 0,
		"0x98 gives the icvps2 value, nine bytes");
	CHECK(scmd(0x98, NULL, 0, res) == 1 && res[0] == 0x80, "a second 0x98 is refused");
	mecha_mg_card_set_id(2, NULL);
}

/* Header through 0x8F, then the first block; the header's verdict, or
 * 0x100 when the header holds but the block does not read back. */
static int run_kelf(const u8* kelf, int size, const u8* plain0, u32 size0, const u8* content0)
{
	u8 res[16], p[5], len[2], out[64];

	scmd(0x80, (const u8*)"\0", 1, res);
	p[0] = 0;
	wr16(p + 1, (u32)size);
	p[3] = 0;
	p[4] = 0;
	scmd(0x90, p, 5, res);
	send(0x8D, kelf, size);
	scmd(0x8F, NULL, 0, res);
	if (res[0] != 0)
		return res[0];
	scmd(0x91, NULL, 0, res);
	{
		u8 table[8 + 16 * NBLK];
		receive(table, sizeof(table));
	}
	wr16(len, size0);
	scmd(0x92, len, 2, res);
	send(0x8D, content0, (int)size0);
	scmd(0x8F, NULL, 0, res);
	if (res[0] != 0)
		return 0x100;
	scmd(0x93, len, 2, res);
	memset(out, 0, sizeof(out));
	if (receive(out, (int)size0) != (int)size0 || memcmp(out, plain0, size0) != 0)
		return 0x100;
	return 0;
}

/* An arcade mechacon decrypts application type 0x07 with Kbit and Kc of
 * its own, whatever the header wraps; any other type, and any other
 * mechacon, goes by the header. */
static void test_kelf_arcade(void)
{
	static const u32 sizes[NBLK] = {24, 16, 40};
	static const u32 bflags[NBLK] = {3, 2, 1};
	u8 plain[NBLK][64], content[NBLK][64], kelf[0x800], keys[32];
	int size, b;

	fill(keys, sizeof(keys));
	for (b = 0; b < NBLK; b++)
		fill(plain[b], 64);

	arm();
	s_kelf_app_type = MECHA_MG_KELF07_APP_TYPE;
	s_kelf_keys = keys;
	size = build_kelf(kelf, plain, sizes, bflags, content, 0, NULL, NULL, NULL);
	s_kelf_keys = NULL;
	s_kelf_app_type = 0;

	mecha_mg_set_arcade(true, keys, keys + 16);
	CHECK(run_kelf(kelf, size, plain[0], sizes[0], content[0]) == 0,
		"arcade: type 0x07 verifies and decrypts with the mechacon's own keys");

	/* Type 0x07 with its keys in the header: only an arcade mechacon
	 * without keys of its own refuses it. */
	s_kelf_app_type = MECHA_MG_KELF07_APP_TYPE;
	size = build_kelf(kelf, plain, sizes, bflags, content, 0, NULL, NULL, NULL);
	s_kelf_app_type = 0;
	mecha_mg_set_arcade(true, NULL, NULL);
	CHECK(run_kelf(kelf, size, plain[0], sizes[0], content[0]) == 0x81,
		"arcade without the type 0x07 keys: refused with 0x81");
	mecha_mg_set_arcade(false, keys, keys + 16);
	CHECK(run_kelf(kelf, size, plain[0], sizes[0], content[0]) == 0,
		"retail: type 0x07 goes by the header's keys");

	/* Any other type is the header's, arcade or not. */
	size = build_kelf(kelf, plain, sizes, bflags, content, 0, NULL, NULL, NULL);
	mecha_mg_set_arcade(true, keys, keys + 16);
	CHECK(run_kelf(kelf, size, plain[0], sizes[0], content[0]) == 0,
		"arcade: type 0 still goes by the header's keys");
	mecha_mg_set_arcade(false, NULL, NULL);
}

/* A block larger than 64 KiB arrives in 0x800-byte pieces; the count of
 * bytes done must not wrap at 16 bits, or the block never completes. */
static void test_kelf_large(void)
{
	static const u32 sizes[2] = {0x10008, 0x9000};
	static const u32 bflags[2] = {3, 2};
	u8* plain[2];
	u8* content[2];
	u8 kelf[0x800], res[16], p[5];
	int size, b;
	u32 done;

	arm();
	for (b = 0; b < 2; b++)
	{
		plain[b] = (u8*)malloc(sizes[b]);
		content[b] = (u8*)malloc(sizes[b]);
		fill(plain[b], (int)sizes[b]);
	}
	size = build_kelf_n(kelf, 2, plain, sizes, bflags, content, 0, NULL, NULL, NULL);
	scmd(0x80, (const u8*)"\0", 1, res);
	p[0] = 0;
	wr16(p + 1, (u32)size);
	p[3] = p[4] = 0;
	scmd(0x90, p, 5, res);
	send(0x8D, kelf, size);
	scmd(0x8F, NULL, 0, res);
	CHECK(res[0] == 0, "large KELF header verifies");
	scmd(0x91, NULL, 0, res);
	{
		u8 table[8 + 32];
		receive(table, sizeof(table));
	}
	for (b = 0; b < 2; b++)
	{
		int ok = 1;
		for (done = 0; done < sizes[b]; )
		{
			u8 len[2], out[0x800];
			const u32 n = (sizes[b] - done) > 0x800 ? 0x800 : (sizes[b] - done);
			wr16(len, n);
			scmd(0x92, len, 2, res);
			if (res[0] != 0) { ok = 0; break; }
			send(0x8D, content[b] + done, (int)n);
			scmd(0x8F, NULL, 0, res);
			if (res[0] != 0) { ok = 0; break; }
			scmd(0x93, len, 2, res);
			if (receive(out, (int)n) != (int)n || memcmp(out, plain[b] + done, n) != 0) { ok = 0; break; }
			done += n;
		}
		CHECK(ok, "a block over 64 KiB decrypts piece by piece and its signature holds");
	}
	for (b = 0; b < 2; b++)
	{
		free(plain[b]);
		free(content[b]);
	}
}

/* Malformed input must neither crash nor read out of bounds (run this
 * under ASan): random headers, sizes, block counts and command orders. */
static void test_fuzz(void)
{
	u8 buf[0x800], res[16], p[16];
	int round, i;

	arm();
	for (round = 0; round < 20000; round++)
	{
		const u8 cmd = (u8)(0x80 + rnd() % 0x19);
		const int cnt = rnd() % 17;
		fill(p, 16);
		if (cmd == 0x90 && (rnd() & 1))
		{
			p[0] = rnd() & 3;
			wr16(p + 1, 32 + rnd() % 0x7e0);
		}
		scmd(cmd, p, cnt, res);
		if ((round & 255) == 0)
		{
			/* A full header of random bytes with plausible fields. */
			u16 size = (u16)(32 + rnd() % 0x7e0);
			p[0] = rnd() & 3;
			wr16(p + 1, size);
			p[3] = rnd() & 15;
			p[4] = rnd() & 15;
			scmd(0x80, (const u8*)"\0", 1, res);
			scmd(0x90, p, 5, res);
			fill(buf, sizeof(buf));
			wr16(buf + 20, size);
			wr16(buf + 26, rnd() % 4);
			for (i = 0; i < size; i += 16)
				scmd(0x8D, buf + i, (size - i) < 16 ? (size - i) : 16, res);
			scmd(0x8F, NULL, 0, res);
			scmd(0x91, NULL, 0, res);
			for (i = 0; i < 300; i++)
				scmd(0x8E, NULL, 0, res);
		}
	}
	CHECK(1, "fuzz survived");
}

/* ---- key files ----------------------------------------------------------- */

static void put_file(const char* dir, const char* name, const u8* data, int n)
{
	char path[1024];
	FILE* f;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (f)
	{
		fwrite(data, 1, (size_t)n, f);
		fclose(f);
	}
}

static void drop_file(const char* dir, const char* name)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	remove(path);
}

/* Arcade mode reads the application-type 0x07 keys beside the store. */
static void test_files_arcade(const char* dir)
{
	static const u32 sizes[NBLK] = {24, 16, 40};
	static const u32 bflags[NBLK] = {3, 2, 1};
	u8 plain[304], enc[1024], card[96], key[16], keys[32];
	u8 bplain[NBLK][64], content[NBLK][64], kelf[0x800];
	int i, size;

	fill(plain, sizeof(plain));
	/* Mode 3 reads the same word for its two key-2 halves. */
	for (i = 0; i < 6; i++)
		memcpy(plain + 8 * (3 * i + 2), plain + 8 * (3 * i + 1), 8);
	fill(enc, sizeof(enc));
	fill(card, sizeof(card));
	fill(key, sizeof(key));
	fill(keys, sizeof(keys));
	encode_store(plain, enc, card, key, MECHA_MG_MODE_ARCADE);
	memcpy(&s_ks, plain, sizeof(s_ks));
	for (i = 0; i < NBLK; i++)
		fill(bplain[i], 64);
	s_kelf_app_type = MECHA_MG_KELF07_APP_TYPE;
	s_kelf_keys = keys;
	size = build_kelf(kelf, bplain, sizes, bflags, content, 0, NULL, NULL, NULL);
	s_kelf_keys = NULL;
	s_kelf_app_type = 0;

	put_file(dir, MECHA_MG_ENC_KEYSTORE_FILE, enc, sizeof(enc));
	put_file(dir, MECHA_MG_CARD_KEYSTORE_FILE, card, sizeof(card));
	put_file(dir, MECHA_MG_KEYSTORE_KEY_FILE, key, sizeof(key));
	put_file(dir, MECHA_MG_KELF07_KBIT_FILE, keys, 16);
	put_file(dir, MECHA_MG_KELF07_KC_FILE, keys + 16, 16);
	CHECK(mecha_mg_load_keys(dir, MECHA_MG_MODE_ARCADE, &s_ids) &&
			run_kelf(kelf, size, bplain[0], sizes[0], content[0]) == 0,
		"arcade key files: type 0x07 decrypts with the keys beside the store");

	/* From here a type 0x07 KELF with its keys in the header: refused
	 * only for want of the mechacon's own. */
	s_kelf_app_type = MECHA_MG_KELF07_APP_TYPE;
	size = build_kelf(kelf, bplain, sizes, bflags, content, 0, NULL, NULL, NULL);
	s_kelf_app_type = 0;
	put_file(dir, MECHA_MG_KELF07_KC_FILE, keys + 16, 15);
	CHECK(mecha_mg_load_keys(dir, MECHA_MG_MODE_ARCADE, &s_ids) &&
			run_kelf(kelf, size, bplain[0], sizes[0], content[0]) == 0x81,
		"a short type 0x07 key file: keyed, and type 0x07 refused");

	drop_file(dir, MECHA_MG_KELF07_KBIT_FILE);
	drop_file(dir, MECHA_MG_KELF07_KC_FILE);
	CHECK(mecha_mg_load_keys(dir, MECHA_MG_MODE_ARCADE, &s_ids) &&
			run_kelf(kelf, size, bplain[0], sizes[0], content[0]) == 0x81,
		"arcade without the type 0x07 key files: keyed, and type 0x07 refused");
	mecha_mg_set_keys(NULL, &s_ids);
}

static void test_files(const char* dir)
{
	u8 plain[304], enc[1024], card[96], key[16];
	MechaMgIds ids;

	fill(plain, sizeof(plain));
	fill(enc, sizeof(enc));
	fill(card, sizeof(card));
	fill(key, sizeof(key));
	encode_store(plain, enc, card, key, MECHA_MG_MODE_RETAIL);
	memset(&ids, 0, sizeof(ids));

	put_file(dir, MECHA_MG_ENC_KEYSTORE_FILE, enc, sizeof(enc));
	put_file(dir, MECHA_MG_CARD_KEYSTORE_FILE, card, sizeof(card));
	CHECK(!mecha_mg_load_keys(dir, MECHA_MG_MODE_RETAIL, &ids) && !mecha_mg_keyed(),
		"a missing key file leaves MagicGate unkeyed");
	put_file(dir, MECHA_MG_KEYSTORE_KEY_FILE, key, 15);
	CHECK(!mecha_mg_load_keys(dir, MECHA_MG_MODE_RETAIL, &ids) && !mecha_mg_keyed(),
		"a short key file leaves MagicGate unkeyed");
	put_file(dir, MECHA_MG_KEYSTORE_KEY_FILE, key, sizeof(key));
	CHECK(mecha_mg_load_keys(dir, MECHA_MG_MODE_RETAIL, &ids) && mecha_mg_keyed(),
		"all three key files key MagicGate");
	{
		u8 res[16];
		CHECK(scmd(0x84, NULL, 0, res) == 1, "keyed, the SCMDs answer");
	}
	mecha_mg_set_keys(NULL, &ids);
	{
		u8 res[16];
		CHECK(scmd(0x84, NULL, 0, res) == -1, "unkeyed, the SCMDs are left to the CDVD");
	}
	test_files_arcade(dir);
}

int main(int argc, char** argv)
{
	log_cb = quiet_log;
	test_des();
	test_keystore();
	test_card();
	test_state();
	test_kelf();
	test_kelf_bind(NULL);
	{
		static const u8 dongle[8] = {0x31, 0x9a, 0x05, 0xc7, 0x4e, 0x12, 0xb8, 0x60};
		test_kelf_bind(dongle);
	}
	test_kelf_arcade();
	test_kelf_large();
	test_fuzz();
	test_files(argc > 1 ? argv[1] : ".");

	if (failures)
	{
		printf("mgtest: %d failure(s)\n", failures);
		return 1;
	}
	printf("mgtest: DES, key store, card handshake, savestate, KELF and fuzz pass\n");
	return 0;
}
