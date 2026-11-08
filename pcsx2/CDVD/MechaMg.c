/*  PCSX2 - PS2 Emulator for PCs
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* Keyed MagicGate, after the mechacon flow reliquary (DiscoStarslayer's
 * PCSX2 fork) implements. Multi-byte fields of KELF data are little
 * endian and read byte by byte; nothing here keeps a pointer, so the
 * whole state freezes as plain fields. C89. */
#include <stdlib.h>
#include <string.h>

#include <retro_miscellaneous.h>
#include <file/file_path.h>
#include <streams/file_stream.h>

#include "common/Pcsx2Defs.h"
#include "../SaveState.h"
#include "MgDes.h"
#include "MechaMg.h"

enum
{
	MG_STATE_0 = 0,
	MG_STATE_READY = 1,
	MG_STATE_KEY_INDEXES_SET = 2,
	MG_STATE_CARD_IV_SEED_SET = 3,
	MG_STATE_CARD_NONCE_SET = 4,
	MG_STATE_CARD_CHALLENGE_GENERATED = 5,
	MG_STATE_CARD_CHALLENGE12_SENT = 6,
	MG_STATE_CARD_CHALLENGE23_SENT = 7,
	MG_STATE_CARD_RESPONSE12_RECEIVED = 8,
	MG_STATE_CARD_RESPONSE3_RECEIVED = 9,
	MG_STATE_CARD_VERIFIED = 10,
	MG_STATE_KELF_HEADER_PARAMS_SET = 11,
	MG_STATE_KELF_HEADER_RECEIVED = 12,
	MG_STATE_KELF_HEADER_VERIFIED = 13,
	MG_STATE_BIT_LENGTH_SENT = 14,
	MG_STATE_KELF_CONTENT_DECRYPT_IN_PROGRESS = 15,
	MG_STATE_DATA_IN_LENGTH_SET = 16,
	MG_STATE_CONTENT_DECRYPTED = 17,
	MG_STATE_DATA_OUT_LENGTH_SET = 18,
	MG_STATE_KELF_CONTENT_RECEIVED = 19,
	MG_STATE_KELF_CONTENT_DECRYPT_DONE = 20,
	MG_STATE_KBIT1_SENT = 21,
	MG_STATE_KBIT2_SENT = 22,
	MG_STATE_KC1_SENT = 23,
	MG_STATE_KC2_SENT = 24
};

enum
{
	MG_RESULT_0 = 0x0,
	MG_RESULT_CARD_CHALLENGE_GENERATED = 0x204,
	MG_RESULT_CARD_VERIFIED = 0x209,
	MG_RESULT_KELF_HEADER_VERIFIED = 0x20C,
	MG_RESULT_KELF_CONTENT_DECRYPTED = 0x213,
	MG_RESULT_FAILED = 0x300
};

#define MG_DATA_MAX 0x800
#define MG_BIT_BLOCKS_MAX 64
#define MG_CARD_KEY_SLOTS 16
#define MG_BIT_BLOCK_ENCRYPTED 1
#define MG_BIT_BLOCK_SIGNED 2
#define MG_KELF_HEADER_SIZE 32
#define MG_KELF_BAN_SIZE 16

typedef struct MgBitBlock
{
	u32 size;
	u32 flags;
	u8 signature[8];
} MgBitBlock;

typedef struct MgCard
{
	u8 iv[8];
	u8 seed[8];
	u8 nonce[8];
	u8 challenge[3][8];
	u8 response[3][8];
} MgCard;

typedef struct MechaMg
{
	/* From the key files, not the state. */
	MechaMgKeyStore ks;
	u8 icvps2_key[16];
	u8 keyed;

	u8 state;
	u8 errorcode;
	u8 mode;
	u8 card_key_slot;
	u8 card_key_index;
	u8 mode3_key_index;
	u16 result;
	u8 memcard_iv[8];
	u8 memcard_seed[8];
	u8 memcard_nonce[8];
	u8 memcard_key[16];
	u8 memcard_random[8];
	u8 challenge[3][8];
	u8 response[3][8];
	u8 card_keys[MG_CARD_KEY_SLOTS][8];
	u16 data_size;
	u16 data_in_off;
	u16 data_out_off;
	u16 data_out_base;
	u16 bit_length;
	u16 bit_table_off;
	u8 data[MG_DATA_MAX];
	u8 kc[16];
	u16 block_count;
	u16 current_block;
	u32 done_bytes;
	MgBitBlock blocks[MG_BIT_BLOCKS_MAX];
	u8 pub_icvps2[8];
	u8 pub_kbit[16];
	u8 pub_kc[16];
	u8 kelf_header[MG_KELF_HEADER_SIZE];
	u8 content_last_ct[8];
	u8 signature_last_ct[8];
	u32 rng;

	MgCard cards[MECHA_MG_CARD_SLOTS];
} MechaMg;

static MechaMg s_mg;

/* Where each of the 18 card key and IV blocks of a key set sits: below
 * 0x200 a word index into the encrypted store, 0x1000 + n the n-th block
 * of the card key store, 0xFFFF zeros. One row of 18 per mode. */
static const u16 s_card_key_indexes[72] = {
	0x0018, 0xFFFF, 0xFFFF, 0x001C, 0xFFFF, 0xFFFF, 0x0020, 0xFFFF, 0xFFFF, 0x0024, 0xFFFF, 0xFFFF, 0x0028, 0xFFFF, 0xFFFF, 0x002C, 0xFFFF, 0xFFFF,
	0x0030, 0x0048, 0x0060, 0x0034, 0x004C, 0x0064, 0x0038, 0x0050, 0x0068, 0x003C, 0x0054, 0x006C, 0x0040, 0x0058, 0x0070, 0x0044, 0x005C, 0x0074,
	0x0000, 0x1000, 0x1001, 0x0004, 0x1002, 0x1003, 0x0008, 0x1004, 0x1005, 0x000C, 0x1006, 0x1007, 0x0010, 0x1008, 0x1009, 0x0014, 0x100A, 0x100B,
	0x0090, 0x00A8, 0x00A8, 0x0094, 0x00AC, 0x00AC, 0x0098, 0x00B0, 0x00B0, 0x009C, 0x00B4, 0x00B4, 0x00A0, 0x00B8, 0x00B8, 0x00A4, 0x00BC, 0x00BC};

/* Word index of the 19 KELF key blocks of each mode, and of the challenge IV. */
static const u16 s_kelf_key_index[4] = {0x110, 0x110, 0xC4, 0x15C};
#define MG_CHALLENGE_IV_INDEX 192

/* The emulated card's key source; any fixed value works, since the card
 * key is derived from it and the key set (see card_session_key). */
static const u8 s_card_keysource[8] = {0x4c, 0x52, 0x50, 0x53, 0x32, 0x4d, 0x47, 0x31};

static u16 rd16(const u8* p)
{
	return (u16)(p[0] | (p[1] << 8));
}

static u32 rd32(const u8* p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void xor8(u8* out, const u8* a, const u8* b)
{
	int i;
	for (i = 0; i < 8; i++)
		out[i] = (u8)(a[i] ^ b[i]);
}

static u8 rng_byte(void)
{
	u32 x = s_mg.rng ? s_mg.rng : 0x2545F491u;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	s_mg.rng = x;
	return (u8)(x >> 24);
}

void mecha_mg_decode_keystore(MechaMgKeyStore* out, const u8 enc_store[MECHA_MG_ENC_KEYSTORE_SIZE],
	const u8 card_store[MECHA_MG_CARD_KEYSTORE_SIZE], const u8 store_key[MECHA_MG_KEYSTORE_KEY_SIZE], int mode)
{
	u8 raw[304];
	u8* p = raw;
	u16 idx;
	int i;

	if (mode < 0 || mode > 3)
		mode = MECHA_MG_MODE_RETAIL;

	for (i = 0; i < 18; i++, p += 8)
	{
		idx = s_card_key_indexes[18 * mode + i];
		if (idx == 0xFFFF)
			memset(p, 0, 8);
		else if (idx >= 0x200)
			memcpy(p, card_store + 8 * (idx & 0x0f), 8);
		else
			memcpy(p, enc_store + 2 * idx, 8);
	}
	memcpy(p, enc_store + 2 * s_kelf_key_index[mode], 19 * 8);
	p += 19 * 8;
	memcpy(p, enc_store + 2 * MG_CHALLENGE_IV_INDEX, 8);

	for (i = 0; i < 38; i++)
		mg_3des_decrypt(store_key, raw + 8 * i);

	p = raw;
#define MG_TAKE(field) do { memcpy(&out->field, p, sizeof(out->field)); p += sizeof(out->field); } while (0)
	MG_TAKE(CardKeyLow);
	MG_TAKE(CardKeyHi);
	MG_TAKE(CardKey2Low);
	MG_TAKE(CardKey2Hi);
	MG_TAKE(CardIV);
	MG_TAKE(CardIV2);
	MG_TAKE(KbitMasterKey);
	MG_TAKE(KcMasterKey);
	MG_TAKE(KbitIv);
	MG_TAKE(KcIv);
	MG_TAKE(icvps2LowKey);
	MG_TAKE(icvps2HiKey);
	MG_TAKE(icvps2LowIV);
	MG_TAKE(icvps2HiIV);
	MG_TAKE(SignatureMasterKey);
	MG_TAKE(SignatureHashKey);
	MG_TAKE(RootSigHashKey);
	MG_TAKE(RootSigMasterKey);
	MG_TAKE(ContentIV);
	MG_TAKE(ContentTableIV);
	MG_TAKE(ChallengeIV);
#undef MG_TAKE
}

void mecha_mg_set_keys(const MechaMgKeyStore* ks, const MechaMgIds* ids)
{
	u8 seed[8];

	mecha_mg_reset();
	if (!ks)
		return;

	s_mg.ks = *ks;
	xor8(seed, ids->ilink_id, ids->console_id);
	xor8(s_mg.icvps2_key, seed, ks->icvps2LowIV);
	xor8(s_mg.icvps2_key + 8, seed, ks->icvps2HiIV);
	mg_3des_encrypt(ks->icvps2LowKey, s_mg.icvps2_key);
	mg_3des_encrypt(ks->icvps2HiKey, s_mg.icvps2_key + 8);
	s_mg.keyed = 1;
	s_mg.state = MG_STATE_READY;
}

static bool read_key_file(const char* dir, const char* name, u8* out, int size)
{
	char path[PATH_MAX_LENGTH];
	void* buf = NULL;
	int64_t len = 0;
	bool ok;

	fill_pathname_join(path, dir, name, sizeof(path));
	if (!path_is_valid(path) || !filestream_read_file(path, &buf, &len))
		return false;
	ok = (len == size);
	if (ok)
		memcpy(out, buf, (size_t)size);
	else
		log_cb(RETRO_LOG_WARN, "[MG] %s is %d bytes, not %d; MagicGate keys not loaded\n", path, (int)len, size);
	free(buf);
	return ok;
}

bool mecha_mg_load_keys(const char* dir, int mode, const MechaMgIds* ids)
{
	u8 enc_store[MECHA_MG_ENC_KEYSTORE_SIZE];
	u8 card_store[MECHA_MG_CARD_KEYSTORE_SIZE];
	u8 store_key[MECHA_MG_KEYSTORE_KEY_SIZE];
	MechaMgKeyStore ks;

	if (!read_key_file(dir, MECHA_MG_ENC_KEYSTORE_FILE, enc_store, sizeof(enc_store)) ||
		!read_key_file(dir, MECHA_MG_CARD_KEYSTORE_FILE, card_store, sizeof(card_store)) ||
		!read_key_file(dir, MECHA_MG_KEYSTORE_KEY_FILE, store_key, sizeof(store_key)))
	{
		mecha_mg_set_keys(NULL, ids);
		return false;
	}

	mecha_mg_decode_keystore(&ks, enc_store, card_store, store_key, mode);
	mecha_mg_set_keys(&ks, ids);
	memset(&ks, 0, sizeof(ks));
	memset(store_key, 0, sizeof(store_key));
	log_cb(RETRO_LOG_INFO, "[MG] MagicGate keys loaded from %s\n", dir);
	return true;
}

bool mecha_mg_keyed(void)
{
	return s_mg.keyed != 0;
}

/* ---- the card handshake -------------------------------------------------- */

static void session_key(u8 key[16], const u8 iv_seed[8], int index)
{
	u8 k[16];

	xor8(key, s_mg.ks.CardIV[index], iv_seed);
	xor8(key + 8, s_mg.ks.CardIV2[index], iv_seed);
	memcpy(k, s_mg.ks.CardKeyLow[index], 8);
	memcpy(k + 8, s_mg.ks.CardKeyHi[index], 8);
	mg_3des_encrypt(k, key);
	memcpy(k, s_mg.ks.CardKey2Low[index], 8);
	memcpy(k + 8, s_mg.ks.CardKey2Hi[index], 8);
	mg_3des_encrypt(k, key + 8);
}

static u16 generate_card_challenge(void)
{
	u8 iv_seed[8];
	int i;

	xor8(iv_seed, s_mg.memcard_iv, s_mg.memcard_seed);
	session_key(s_mg.memcard_key, iv_seed, s_mg.card_key_index);

	for (i = 0; i < 8; i++)
		s_mg.memcard_random[i] = rng_byte();

	xor8(s_mg.challenge[0], s_mg.ks.ChallengeIV, s_mg.memcard_random);
	mg_3des_encrypt(s_mg.memcard_key, s_mg.challenge[0]);
	xor8(s_mg.challenge[1], s_mg.memcard_nonce, s_mg.challenge[0]);
	mg_3des_encrypt(s_mg.memcard_key, s_mg.challenge[1]);
	xor8(s_mg.challenge[2], s_mg.memcard_iv, s_mg.challenge[1]);
	mg_3des_encrypt(s_mg.memcard_key, s_mg.challenge[2]);
	return MG_RESULT_CARD_CHALLENGE_GENERATED;
}

static u16 verify_card_challenge(void)
{
	u8 rp[8];

	memcpy(rp, s_mg.response[0], 8);
	mg_3des_decrypt(s_mg.memcard_key, rp);
	xor8(rp, rp, s_mg.ks.ChallengeIV);
	if (memcmp(s_mg.memcard_nonce, rp, 8) != 0)
		return MG_RESULT_FAILED;

	memcpy(rp, s_mg.response[1], 8);
	mg_3des_decrypt(s_mg.memcard_key, rp);
	xor8(rp, rp, s_mg.response[0]);
	if (memcmp(s_mg.memcard_random, rp, 8) != 0)
		return MG_RESULT_FAILED;

	memcpy(rp, s_mg.response[2], 8);
	mg_3des_decrypt(s_mg.memcard_key, rp);
	xor8(s_mg.card_keys[s_mg.card_key_slot], s_mg.response[1], rp);
	return MG_RESULT_CARD_VERIFIED;
}

/* ---- KELF ---------------------------------------------------------------- */

static u16 kelf_fail(u8 code)
{
	s_mg.errorcode = code;
	return MG_RESULT_FAILED;
}

static void des_ecb_key_halves(const u8 key[8], u8* data, int blocks, int decrypt)
{
	int i;
	for (i = 0; i < blocks; i++)
	{
		if (decrypt)
			mg_des_decrypt(key, data + 8 * i);
		else
			mg_des_encrypt(key, data + 8 * i);
	}
}

static u16 decrypt_kelf_header(const MechaMgIds* ids)
{
	u8* const d = s_mg.data;
	const u32 size = s_mg.data_size;
	u32 header_size, offset, bit_table_len, remaining, ban_count, i;
	u16 flags;
	u8 header_sig[8], kbit[16], bit_sig[8], root[8], root_src[8], even_ct[8], odd_ct[8], sig_key[16];
	u8* table;
	int block_count, signed_blocks = 0;

	if (size < MG_KELF_HEADER_SIZE)
		return kelf_fail(0x81);

	flags = rd16(d + 24);
	ban_count = rd16(d + 26);
	header_size = MG_KELF_HEADER_SIZE + MG_KELF_BAN_SIZE * ban_count;
	if (header_size > size)
		return kelf_fail(0x81);
	if (flags & 1)
	{
		if (header_size >= size)
			return kelf_fail(0x81);
		header_size += d[header_size] + 1u;
		if (header_size > size)
			return kelf_fail(0x81);
	}
	if (header_size > size - 8)
		return kelf_fail(0x81);

	memset(header_sig, 0, 8);
	for (i = 0; i < (header_size & ~7u); i += 8)
	{
		xor8(header_sig, d + i, header_sig);
		mg_des_encrypt(s_mg.ks.SignatureMasterKey, header_sig);
	}
	mg_des_decrypt(s_mg.ks.SignatureHashKey, header_sig);
	mg_des_encrypt(s_mg.ks.SignatureMasterKey, header_sig);
	if (memcmp(header_sig, d + header_size, 8) != 0)
		return kelf_fail(0x84);

	if (rd16(d + 20) != size)
		return kelf_fail(0x81);
	if (s_mg.mode == 3 && !(flags & 4) && !(flags & 8))
		return kelf_fail(0x82);

	for (i = 0; i < ban_count; i++)
	{
		const u8* ban = d + MG_KELF_HEADER_SIZE + MG_KELF_BAN_SIZE * i;
		if (memcmp(ban, ids->ilink_id, 8) == 0 && memcmp(ban + 8, ids->console_id, 8) == 0)
			return kelf_fail(0x85);
	}

	offset = header_size + 8;
	if (offset > size - 32)
		return kelf_fail(0x81);

	memcpy(kbit, d + offset, 16);
	memcpy(s_mg.kc, d + offset + 16, 16);
	offset += 32;
	if (s_mg.mode == 1 || s_mg.mode == 3)
	{
		const u8* ck = s_mg.card_keys[s_mg.card_key_slot];
		des_ecb_key_halves(ck, kbit, 2, 1);
		des_ecb_key_halves(ck, s_mg.kc, 2, 1);
	}
	else
	{
		u8 nonce[8], kek[16];
		xor8(nonce, d, d + 8);
		xor8(kek, s_mg.ks.KbitIv, nonce);
		mg_3des_encrypt(s_mg.ks.KbitMasterKey, kek);
		xor8(kek + 8, s_mg.ks.KcIv, nonce);
		mg_3des_encrypt(s_mg.ks.KcMasterKey, kek + 8);
		mg_3des_decrypt(kek, kbit);
		mg_3des_decrypt(kek, kbit + 8);
		mg_3des_decrypt(kek, s_mg.kc);
		mg_3des_decrypt(kek, s_mg.kc + 8);
	}

	if (offset > size - 8)
		return kelf_fail(0x81);

	/* The bit table: an 8-byte head and 16 bytes a block, each half
	 * decrypted with the previous half's ciphertext as its IV. */
	table = d + offset;
	memcpy(even_ct, table, 8);
	mg_3des_decrypt(kbit, table);
	xor8(table, s_mg.ks.ContentTableIV, table);
	block_count = table[4];
	bit_table_len = 8 + 16u * (u32)block_count;
	remaining = size - offset;
	if (bit_table_len > remaining || remaining - bit_table_len < 16)
		return kelf_fail(0x81);

	s_mg.block_count = 0;
	for (i = 0; i < (u32)block_count; i++)
	{
		u8* blk = table + 8 + 16 * i;
		u32 bflags;

		memcpy(odd_ct, blk, 8);
		mg_3des_decrypt(kbit, blk);
		xor8(blk, even_ct, blk);
		memcpy(even_ct, blk + 8, 8);
		mg_3des_decrypt(kbit, blk + 8);
		xor8(blk + 8, odd_ct, blk + 8);

		bflags = rd32(blk + 4);
		if (bflags & (MG_BIT_BLOCK_SIGNED | MG_BIT_BLOCK_ENCRYPTED))
		{
			MgBitBlock* b;
			if (s_mg.block_count >= MG_BIT_BLOCKS_MAX)
				return kelf_fail(0x81);
			b = &s_mg.blocks[s_mg.block_count++];
			b->size = rd32(blk);
			b->flags = bflags;
			memcpy(b->signature, blk + 8, 8);
			if (bflags & MG_BIT_BLOCK_SIGNED)
				signed_blocks++;
		}
	}
	if (!signed_blocks)
		return kelf_fail(0x81);

	memcpy(bit_sig, kbit, 8);
	if (memcmp(kbit, kbit + 8, 8) != 0)
		xor8(bit_sig, kbit + 8, bit_sig);
	xor8(bit_sig, s_mg.kc, bit_sig);
	if (memcmp(s_mg.kc, s_mg.kc + 8, 8) != 0)
		xor8(bit_sig, s_mg.kc + 8, bit_sig);
	for (i = 0; i < (u32)block_count * 2 + 1; i++)
		xor8(bit_sig, table + 8 * i, bit_sig);
	memcpy(sig_key, s_mg.ks.SignatureMasterKey, 8);
	memcpy(sig_key + 8, s_mg.ks.SignatureHashKey, 8);
	mg_3des_encrypt(sig_key, bit_sig);
	if (memcmp(table + bit_table_len, bit_sig, 8) != 0)
		return kelf_fail(0x84);

	s_mg.bit_length = (u16)bit_table_len;
	s_mg.bit_table_off = (u16)offset;

	memcpy(root, header_sig, 8);
	mg_des_encrypt(s_mg.ks.RootSigMasterKey, root);
	xor8(root, bit_sig, root);
	mg_des_encrypt(s_mg.ks.RootSigMasterKey, root);
	for (i = 0; i < s_mg.block_count; i++)
	{
		if (s_mg.blocks[i].flags & MG_BIT_BLOCK_SIGNED)
		{
			xor8(root, s_mg.blocks[i].signature, root);
			mg_des_encrypt(s_mg.ks.RootSigMasterKey, root);
		}
	}
	memcpy(root_src, root, 8);
	if ((s_mg.mode == 1 || s_mg.mode == 3) && (flags & 2))
		mg_3des_decrypt(s_mg.icvps2_key, root);
	else
		mg_3des_decrypt(s_mg.ks.RootSigHashKey, root);
	if (memcmp(table + bit_table_len + 8, root, 8) != 0)
		return kelf_fail(((s_mg.mode == 1 || s_mg.mode == 3) && (flags & 2)) ? 0x83 : 0x84);

	if (s_mg.mode == 2 && (flags & 2))
	{
		memcpy(s_mg.pub_icvps2, root_src, 8);
		mg_3des_decrypt(s_mg.icvps2_key, s_mg.pub_icvps2);
	}
	if (s_mg.mode == 2 || s_mg.mode == 3)
	{
		const u8* ck = s_mg.card_keys[s_mg.mode == 2 ? s_mg.card_key_slot : s_mg.mode3_key_index];
		memcpy(s_mg.pub_kbit, kbit, 16);
		memcpy(s_mg.pub_kc, s_mg.kc, 16);
		des_ecb_key_halves(ck, s_mg.pub_kbit, 2, 0);
		des_ecb_key_halves(ck, s_mg.pub_kc, 2, 0);
	}

	memcpy(s_mg.kelf_header, d, MG_KELF_HEADER_SIZE);
	s_mg.done_bytes = 0;
	s_mg.current_block = 0;
	if (s_mg.mode == 2 || s_mg.mode == 3)
	{
		while (s_mg.current_block < s_mg.block_count &&
			!(s_mg.blocks[s_mg.current_block].flags & MG_BIT_BLOCK_SIGNED))
			s_mg.current_block++;
	}
	return MG_RESULT_KELF_HEADER_VERIFIED;
}

static u16 decrypt_kelf_content(void)
{
	const u16 hflags = rd16(s_mg.kelf_header + 24);
	const int crypto = (hflags >> 8) & 0xF;
	const int key_count = (hflags >> 4) & 0xF;
	MgBitBlock* b;
	MgDesKey k1, k2;
	u8 sig_key[16];
	u32 v;

	if (s_mg.current_block >= s_mg.block_count)
		return kelf_fail(0x81);
	b = &s_mg.blocks[s_mg.current_block];

	if (!s_mg.done_bytes)
	{
		memcpy(s_mg.content_last_ct, s_mg.ks.ContentIV, 8);
		memset(s_mg.signature_last_ct, 0, 8);
	}

	mg_des_setkey(&k1, s_mg.kc);
	mg_des_setkey(&k2, s_mg.kc + 8);
	for (v = 0; v + 8 <= s_mg.data_size; v += 8)
	{
		u8* p = s_mg.data + v;
		if (b->flags & MG_BIT_BLOCK_ENCRYPTED)
		{
			u8 tmp[8];
			if (crypto != 1 && crypto != 2)
				return kelf_fail(0x81);
			memcpy(tmp, p, 8);
			if (key_count == 1)
				mg_des_decrypt_ks(&k1, tmp);
			else if (key_count == 2)
			{
				mg_des_decrypt_ks(&k1, tmp);
				mg_des_encrypt_ks(&k2, tmp);
				mg_des_decrypt_ks(&k1, tmp);
			}
			if (crypto == 2) /* CBC */
			{
				xor8(tmp, s_mg.content_last_ct, tmp);
				memcpy(s_mg.content_last_ct, p, 8);
			}
			memcpy(p, tmp, 8);
			if (b->flags & MG_BIT_BLOCK_SIGNED)
				xor8(s_mg.signature_last_ct, p, s_mg.signature_last_ct);
		}
		else
		{
			xor8(s_mg.signature_last_ct, p, s_mg.signature_last_ct);
			mg_des_encrypt(s_mg.ks.SignatureMasterKey, s_mg.signature_last_ct);
		}
	}
	s_mg.done_bytes += v;

	if (b->size <= s_mg.done_bytes)
	{
		s_mg.done_bytes = 0;
		if (b->flags & MG_BIT_BLOCK_ENCRYPTED)
		{
			memcpy(sig_key, s_mg.ks.SignatureMasterKey, 8);
			memcpy(sig_key + 8, s_mg.ks.SignatureHashKey, 8);
			mg_3des_encrypt(sig_key, s_mg.signature_last_ct);
		}
		else
		{
			mg_des_decrypt(s_mg.ks.SignatureHashKey, s_mg.signature_last_ct);
			mg_des_encrypt(s_mg.ks.SignatureMasterKey, s_mg.signature_last_ct);
		}
		if ((b->flags & MG_BIT_BLOCK_SIGNED) && memcmp(b->signature, s_mg.signature_last_ct, 8) != 0)
			return kelf_fail(0x84);

		s_mg.current_block++;
		if (s_mg.mode == 2 || s_mg.mode == 3)
		{
			while (s_mg.current_block < s_mg.block_count &&
				!(s_mg.blocks[s_mg.current_block].flags & MG_BIT_BLOCK_SIGNED))
				s_mg.current_block++;
		}
	}
	return MG_RESULT_KELF_CONTENT_DECRYPTED;
}

static void execute_handler(const MechaMgIds* ids)
{
	switch (s_mg.state)
	{
		case MG_STATE_CARD_NONCE_SET:
			s_mg.result = generate_card_challenge();
			break;
		case MG_STATE_CARD_RESPONSE3_RECEIVED:
			memset(s_mg.card_keys[s_mg.card_key_slot], 0xAA, 8);
			s_mg.result = verify_card_challenge();
			break;
		case MG_STATE_KELF_HEADER_RECEIVED:
			s_mg.result = decrypt_kelf_header(ids);
			break;
		case MG_STATE_DATA_IN_LENGTH_SET:
		case MG_STATE_KELF_CONTENT_RECEIVED:
			s_mg.result = decrypt_kelf_content();
			break;
		default:
			break;
	}
}

/* ---- the commands -------------------------------------------------------- */

static int reply_key_half(u8* result, const u8* half, u8 next_state, u8 want_state, int param_cnt)
{
	if (s_mg.state == want_state && param_cnt == 0)
	{
		result[0] = 0;
		memcpy(result + 1, half, 8);
		s_mg.state = next_state;
		return 9;
	}
	result[0] = 0x80;
	s_mg.state = MG_STATE_READY;
	return 1;
}

int mecha_mg_scmd(u8 cmd, const u8* param, int param_cnt, u8* result, const MechaMgIds* ids)
{
	if (!s_mg.keyed)
		return -1;

	result[0] = 0x80;
	switch (cmd)
	{
		case 0x80:
			if (s_mg.state && param_cnt == 1)
			{
				s_mg.state = MG_STATE_READY;
				s_mg.result = MG_RESULT_0;
				s_mg.errorcode = 0;
				if (param[0] < 0x10)
					result[0] = 0;
			}
			return 1;

		case 0x81:
			if (s_mg.state && param_cnt == 1)
			{
				const u8 slot = param[0] & 0x3F;
				const u8 index = (param[0] >> 6) & 3;
				s_mg.state = MG_STATE_READY;
				if (slot < MG_CARD_KEY_SLOTS && index != 3)
				{
					s_mg.card_key_slot = slot;
					s_mg.card_key_index = index;
					s_mg.state = MG_STATE_KEY_INDEXES_SET;
					result[0] = 0;
				}
			}
			return 1;

		case 0x82:
			if (s_mg.state == MG_STATE_KEY_INDEXES_SET && param_cnt == 16)
			{
				memcpy(s_mg.memcard_iv, param, 8);
				memcpy(s_mg.memcard_seed, param + 8, 8);
				s_mg.state = MG_STATE_CARD_IV_SEED_SET;
				result[0] = 0;
			}
			else
				s_mg.state = MG_STATE_READY;
			return 1;

		case 0x83:
			if (s_mg.state == MG_STATE_CARD_IV_SEED_SET && param_cnt == 8)
			{
				memcpy(s_mg.memcard_nonce, param, 8);
				s_mg.state = MG_STATE_CARD_NONCE_SET;
				execute_handler(ids);
				result[0] = 0;
			}
			else
				s_mg.state = MG_STATE_READY;
			return 1;

		case 0x84:
			if (s_mg.state == MG_STATE_CARD_CHALLENGE_GENERATED && param_cnt == 0)
			{
				result[0] = 0;
				memcpy(result + 1, s_mg.challenge[0], 8);
				memcpy(result + 9, s_mg.challenge[1], 4);
				s_mg.state = MG_STATE_CARD_CHALLENGE12_SENT;
				return 13;
			}
			s_mg.state = MG_STATE_READY;
			return 1;

		case 0x85:
			if (s_mg.state == MG_STATE_CARD_CHALLENGE12_SENT && param_cnt == 0)
			{
				result[0] = 0;
				memcpy(result + 1, s_mg.challenge[1] + 4, 4);
				memcpy(result + 5, s_mg.challenge[2], 8);
				s_mg.state = MG_STATE_CARD_CHALLENGE23_SENT;
				return 13;
			}
			return 1;

		case 0x86:
			if (s_mg.state == MG_STATE_CARD_CHALLENGE23_SENT && param_cnt == 16)
			{
				memcpy(s_mg.response[0], param, 8);
				memcpy(s_mg.response[1], param + 8, 8);
				s_mg.state = MG_STATE_CARD_RESPONSE12_RECEIVED;
				result[0] = 0;
			}
			else
				s_mg.state = MG_STATE_READY;
			return 1;

		case 0x87:
			if (s_mg.state == MG_STATE_CARD_RESPONSE12_RECEIVED && param_cnt == 8)
			{
				memcpy(s_mg.response[2], param, 8);
				s_mg.state = MG_STATE_CARD_RESPONSE3_RECEIVED;
				execute_handler(ids);
				result[0] = 0;
			}
			else
				s_mg.state = MG_STATE_READY;
			return 1;

		case 0x88:
			if (s_mg.state == MG_STATE_CARD_VERIFIED && param_cnt == 0)
				result[0] = 0;
			return 1;

		case 0x8C:
			execute_handler(ids);
			s_mg.result = MG_RESULT_0;
			s_mg.state = MG_STATE_READY;
			result[0] = 0;
			return 1;

		case 0x8D:
			if (param_cnt > 0 &&
				(s_mg.state == MG_STATE_KELF_HEADER_PARAMS_SET || s_mg.state == MG_STATE_DATA_IN_LENGTH_SET) &&
				s_mg.data_in_off + param_cnt <= MG_DATA_MAX)
			{
				memcpy(s_mg.data + s_mg.data_in_off, param, (size_t)param_cnt);
				s_mg.data_in_off = (u16)(s_mg.data_in_off + param_cnt);
				if (s_mg.data_size <= s_mg.data_in_off)
				{
					s_mg.state = (s_mg.state == MG_STATE_KELF_HEADER_PARAMS_SET) ?
						MG_STATE_KELF_HEADER_RECEIVED : MG_STATE_KELF_CONTENT_RECEIVED;
					execute_handler(ids);
				}
				result[0] = 0;
			}
			return 1;

		case 0x8E:
			if (param_cnt == 0 &&
				(s_mg.state == MG_STATE_BIT_LENGTH_SENT || s_mg.state == MG_STATE_DATA_OUT_LENGTH_SET) &&
				s_mg.data_out_off <= s_mg.data_size)
			{
				int len = s_mg.data_size - s_mg.data_out_off;
				if (len > 16)
					len = 16;
				memcpy(result, s_mg.data + s_mg.data_out_base + s_mg.data_out_off, (size_t)len);
				s_mg.data_out_off = (u16)(s_mg.data_out_off + len);
				if (s_mg.data_size <= s_mg.data_out_off)
				{
					if (s_mg.state == MG_STATE_BIT_LENGTH_SENT)
						s_mg.state = MG_STATE_KELF_CONTENT_DECRYPT_IN_PROGRESS;
					else
						s_mg.state = (s_mg.current_block >= s_mg.block_count) ?
							MG_STATE_READY : MG_STATE_KELF_CONTENT_DECRYPT_IN_PROGRESS;
				}
				return len;
			}
			s_mg.state = MG_STATE_READY;
			return 0;

		case 0x8F:
			if (param_cnt)
				return 1;
			switch (s_mg.state)
			{
				case MG_STATE_CARD_NONCE_SET:
				case MG_STATE_CARD_CHALLENGE_GENERATED:
					if (s_mg.result == MG_RESULT_CARD_CHALLENGE_GENERATED)
					{
						s_mg.state = MG_STATE_CARD_CHALLENGE_GENERATED;
						result[0] = 0;
					}
					break;
				case MG_STATE_CARD_RESPONSE3_RECEIVED:
				case MG_STATE_CARD_VERIFIED:
					if (s_mg.result == MG_RESULT_CARD_VERIFIED)
					{
						s_mg.state = MG_STATE_CARD_VERIFIED;
						result[0] = 0;
					}
					break;
				case MG_STATE_KELF_HEADER_RECEIVED:
				case MG_STATE_KELF_HEADER_VERIFIED:
					if (s_mg.result == MG_RESULT_KELF_HEADER_VERIFIED)
					{
						s_mg.state = MG_STATE_KELF_HEADER_VERIFIED;
						result[0] = 0;
					}
					else if (s_mg.result == MG_RESULT_FAILED)
					{
						s_mg.state = MG_STATE_READY;
						result[0] = s_mg.errorcode;
					}
					break;
				case MG_STATE_DATA_IN_LENGTH_SET:
				case MG_STATE_CONTENT_DECRYPTED:
				case MG_STATE_KELF_CONTENT_RECEIVED:
					if (s_mg.result == MG_RESULT_KELF_CONTENT_DECRYPTED)
					{
						if (s_mg.mode == 2 || s_mg.mode == 3)
							s_mg.state = (s_mg.current_block >= s_mg.block_count) ?
								MG_STATE_KELF_CONTENT_DECRYPT_DONE : MG_STATE_KELF_CONTENT_DECRYPT_IN_PROGRESS;
						else
							s_mg.state = MG_STATE_CONTENT_DECRYPTED;
						result[0] = 0;
					}
					else if (s_mg.result == MG_RESULT_FAILED)
					{
						s_mg.state = MG_STATE_READY;
						result[0] = s_mg.errorcode;
					}
					break;
				default:
					break;
			}
			return 1;

		case 0x90:
			s_mg.state = s_mg.state ? MG_STATE_READY : MG_STATE_0;
			if (s_mg.state && param_cnt == 5)
			{
				s_mg.mode = param[0];
				s_mg.data_size = rd16(param + 1);
				s_mg.card_key_slot = param[3];
				s_mg.mode3_key_index = param[4];
				s_mg.data_in_off = 0;
				/* The card key slot indexes a 16-entry table in modes 1-3. */
				if (s_mg.mode <= 3 && s_mg.data_size <= MG_DATA_MAX &&
					(s_mg.mode == 0 || (s_mg.card_key_slot < MG_CARD_KEY_SLOTS &&
						(s_mg.mode != 3 || s_mg.mode3_key_index < MG_CARD_KEY_SLOTS))))
				{
					s_mg.state = MG_STATE_KELF_HEADER_PARAMS_SET;
					result[0] = 0;
				}
			}
			return 1;

		case 0x91:
			if (s_mg.state == MG_STATE_KELF_HEADER_VERIFIED && param_cnt == 0)
			{
				result[0] = 0;
				result[1] = (u8)s_mg.bit_length;
				result[2] = (u8)(s_mg.bit_length >> 8);
				s_mg.data_size = s_mg.bit_length;
				s_mg.data_out_off = 0;
				s_mg.data_out_base = s_mg.bit_table_off;
				s_mg.state = MG_STATE_BIT_LENGTH_SENT;
				return 3;
			}
			s_mg.state = MG_STATE_READY;
			return 1;

		case 0x92:
			if (s_mg.state == MG_STATE_KELF_CONTENT_DECRYPT_IN_PROGRESS && param_cnt == 2)
			{
				s_mg.data_size = rd16(param);
				if (s_mg.current_block < s_mg.block_count)
				{
					u32 len = s_mg.blocks[s_mg.current_block].size - s_mg.done_bytes;
					if (len > MG_DATA_MAX)
						len = MG_DATA_MAX;
					if (s_mg.data_size == len)
					{
						s_mg.data_in_off = 0;
						s_mg.state = MG_STATE_DATA_IN_LENGTH_SET;
						result[0] = 0;
					}
				}
			}
			else
				s_mg.state = MG_STATE_READY;
			return 1;

		case 0x93:
			if (s_mg.state == MG_STATE_CONTENT_DECRYPTED && param_cnt == 2 && rd16(param) == s_mg.data_size)
			{
				s_mg.data_out_off = 0;
				s_mg.data_out_base = 0;
				s_mg.state = MG_STATE_DATA_OUT_LENGTH_SET;
				result[0] = 0;
			}
			return 1;

		case 0x94:
			return reply_key_half(result, s_mg.pub_kbit, MG_STATE_KBIT1_SENT, MG_STATE_KELF_CONTENT_DECRYPT_DONE, param_cnt);
		case 0x95:
			return reply_key_half(result, s_mg.pub_kbit + 8, MG_STATE_KBIT2_SENT, MG_STATE_KBIT1_SENT, param_cnt);
		case 0x96:
			return reply_key_half(result, s_mg.pub_kc, MG_STATE_KC1_SENT, MG_STATE_KBIT2_SENT, param_cnt);
		case 0x97:
		{
			const int n = reply_key_half(result, s_mg.pub_kc + 8, MG_STATE_KC2_SENT, MG_STATE_KC1_SENT, param_cnt);
			if (n == 9 && !(s_mg.mode == 2 && (rd16(s_mg.kelf_header + 24) & 2)))
				s_mg.state = MG_STATE_READY;
			return n;
		}
		case 0x98:
		{
			const int n = reply_key_half(result, s_mg.pub_icvps2, MG_STATE_READY, MG_STATE_KC2_SENT, param_cnt);
			s_mg.state = MG_STATE_READY;
			return n;
		}

		default:
			return -1;
	}
}

/* ---- the emulated card --------------------------------------------------- */

/* The session key the mechacon derives for a card from the card's IV
 * XOR seed. The card hands out seed = keysource XOR IV, so that XOR is
 * its keysource, and the card's matching key comes from the same key
 * set the mechacon holds: no card key is stored anywhere. */
static void card_session_key(u8 key[16])
{
	session_key(key, s_card_keysource, s_mg.card_key_index < 3 ? s_mg.card_key_index : 0);
}

void mecha_mg_card_auth_reset(int slot)
{
	if (slot >= 0 && slot < MECHA_MG_CARD_SLOTS)
		memset(&s_mg.cards[slot], 0, sizeof(s_mg.cards[slot]));
}

bool mecha_mg_card_get(int slot, u8 mode, u8 out[8])
{
	MgCard* c;
	int i;

	if (slot < 0 || slot >= MECHA_MG_CARD_SLOTS)
		return false;
	c = &s_mg.cards[slot];

	switch (mode)
	{
		case 0x01:
			for (i = 0; i < 8; i++)
			{
				c->iv[i] = rng_byte();
				c->seed[i] = (u8)(s_card_keysource[i] ^ c->iv[i]);
				c->nonce[i] = rng_byte();
			}
			memcpy(out, c->iv, 8);
			return true;
		case 0x02:
			memcpy(out, c->seed, 8);
			return true;
		case 0x04:
			memcpy(out, c->nonce, 8);
			return true;
		case 0x0f:
		{
			/* challenge 1 carries the mechacon's random; challenges 2 and
			 * 3 let a card check the console, which this one does not. */
			u8 key[16], random[8], card_id[8];
			card_session_key(key);
			memcpy(random, c->challenge[0], 8);
			mg_3des_decrypt(key, random);
			xor8(random, random, s_mg.ks.ChallengeIV);

			xor8(c->response[0], c->nonce, s_mg.ks.ChallengeIV);
			mg_3des_encrypt(key, c->response[0]);
			xor8(c->response[1], random, c->response[0]);
			mg_3des_encrypt(key, c->response[1]);
			/* The card ID the mechacon keeps for the slot. */
			memcpy(card_id, "LRPS2MC0", 8);
			card_id[7] = (u8)('0' + slot);
			xor8(c->response[2], card_id, c->response[1]);
			mg_3des_encrypt(key, c->response[2]);
			memcpy(out, c->response[0], 8);
			return true;
		}
		case 0x11:
			memcpy(out, c->response[1], 8);
			return true;
		case 0x13:
			memcpy(out, c->response[2], 8);
			return true;
		default:
			return false;
	}
}

bool mecha_mg_card_put(int slot, u8 mode, const u8 in[8])
{
	if (slot < 0 || slot >= MECHA_MG_CARD_SLOTS)
		return false;
	switch (mode)
	{
		case 0x0b: memcpy(s_mg.cards[slot].challenge[0], in, 8); return true;
		case 0x07: memcpy(s_mg.cards[slot].challenge[1], in, 8); return true;
		case 0x06: memcpy(s_mg.cards[slot].challenge[2], in, 8); return true;
		default: return false;
	}
}

/* ---- reset and state ----------------------------------------------------- */

void mecha_mg_reset(void)
{
	memset(&s_mg, 0, sizeof(s_mg));
}

bool mecha_mg_freeze(SaveStateBase* s)
{
	int i;

	if (!SaveState_FreezeTag(s, "mechaMG"))
		return false;

	SaveState_Freeze(s, s_mg.state);
	SaveState_Freeze(s, s_mg.errorcode);
	SaveState_Freeze(s, s_mg.mode);
	SaveState_Freeze(s, s_mg.card_key_slot);
	SaveState_Freeze(s, s_mg.card_key_index);
	SaveState_Freeze(s, s_mg.mode3_key_index);
	SaveState_Freeze(s, s_mg.result);
	SaveState_Freeze(s, s_mg.memcard_iv);
	SaveState_Freeze(s, s_mg.memcard_seed);
	SaveState_Freeze(s, s_mg.memcard_nonce);
	SaveState_Freeze(s, s_mg.memcard_key);
	SaveState_Freeze(s, s_mg.memcard_random);
	SaveState_Freeze(s, s_mg.challenge);
	SaveState_Freeze(s, s_mg.response);
	SaveState_Freeze(s, s_mg.card_keys);
	SaveState_Freeze(s, s_mg.data_size);
	SaveState_Freeze(s, s_mg.data_in_off);
	SaveState_Freeze(s, s_mg.data_out_off);
	SaveState_Freeze(s, s_mg.data_out_base);
	SaveState_Freeze(s, s_mg.bit_length);
	SaveState_Freeze(s, s_mg.bit_table_off);
	SaveState_Freeze(s, s_mg.data);
	SaveState_Freeze(s, s_mg.kc);
	SaveState_Freeze(s, s_mg.block_count);
	SaveState_Freeze(s, s_mg.current_block);
	SaveState_Freeze(s, s_mg.done_bytes);
	for (i = 0; i < MG_BIT_BLOCKS_MAX; i++)
	{
		SaveState_Freeze(s, s_mg.blocks[i].size);
		SaveState_Freeze(s, s_mg.blocks[i].flags);
		SaveState_Freeze(s, s_mg.blocks[i].signature);
	}
	SaveState_Freeze(s, s_mg.pub_icvps2);
	SaveState_Freeze(s, s_mg.pub_kbit);
	SaveState_Freeze(s, s_mg.pub_kc);
	SaveState_Freeze(s, s_mg.kelf_header);
	SaveState_Freeze(s, s_mg.content_last_ct);
	SaveState_Freeze(s, s_mg.signature_last_ct);
	SaveState_Freeze(s, s_mg.rng);
	for (i = 0; i < MECHA_MG_CARD_SLOTS; i++)
		SaveState_Freeze(s, s_mg.cards[i]);

	if (SaveState_IsLoading(s))
	{
		/* Every offset into data[] is bounded at run time by the commands
		 * that set it; a state is held to the same bounds. */
		if (s_mg.data_size > MG_DATA_MAX || s_mg.data_in_off > MG_DATA_MAX ||
			(u32)s_mg.data_out_base + s_mg.data_size > MG_DATA_MAX || s_mg.data_out_off > s_mg.data_size ||
			s_mg.block_count > MG_BIT_BLOCKS_MAX || s_mg.current_block > s_mg.block_count ||
			s_mg.card_key_slot >= MG_CARD_KEY_SLOTS || s_mg.mode3_key_index >= MG_CARD_KEY_SLOTS ||
			s_mg.card_key_index > 2)
		{
			memset(s_mg.data, 0, sizeof(s_mg.data));
			s_mg.data_size = s_mg.data_in_off = s_mg.data_out_off = s_mg.data_out_base = 0;
			s_mg.block_count = s_mg.current_block = 0;
			s_mg.card_key_slot = s_mg.mode3_key_index = s_mg.card_key_index = 0;
			s_mg.state = s_mg.keyed ? MG_STATE_READY : MG_STATE_0;
		}
	}
	return SaveState_IsOkay(s);
}
