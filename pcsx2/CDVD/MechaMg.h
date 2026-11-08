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

/* MagicGate on the mechacon, keyed: the card handshake (SCMD 0x80-0x88),
 * KELF header and content decryption (SCMD 0x8C-0x98), and the emulated
 * memory card's side of the handshake.
 *
 * Keyed only when the user supplies the mechacon's key material beside
 * the BIOS (see MECHA_MG_*_FILE); nothing secret is built in. Without it
 * the CDVD keeps its existing fixed MagicGate replies. C89. */
#ifndef MECHAMG_H
#define MECHAMG_H

#include "common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SaveStateBase;

/* The key files, in the BIOS directory. */
#define MECHA_MG_ENC_KEYSTORE_FILE  "mg_keystore.bin"     /* 1024 bytes */
#define MECHA_MG_CARD_KEYSTORE_FILE "mg_card_keystore.bin" /* 96 bytes */
#define MECHA_MG_KEYSTORE_KEY_FILE  "mg_keystore_key.bin" /* 16 bytes */

/* COH (arcade) mechacons only: a KELF of application type 0x07 takes
 * its Kbit and Kc from these, not from its header. Optional; without
 * them such a KELF is refused, as the mechacon would. */
#define MECHA_MG_KELF07_KBIT_FILE   "mg_kelf07_kbit.bin"  /* 16 bytes */
#define MECHA_MG_KELF07_KC_FILE     "mg_kelf07_kc.bin"    /* 16 bytes */
#define MECHA_MG_KELF07_APP_TYPE    0x07
#define MECHA_MG_KELF07_KEY_SIZE    16

#define MECHA_MG_ENC_KEYSTORE_SIZE  1024
#define MECHA_MG_CARD_KEYSTORE_SIZE 96
#define MECHA_MG_KEYSTORE_KEY_SIZE  16

/* Which key set of the key store the mechacon answers with. */
enum
{
	MECHA_MG_MODE_DEV = 0,
	MECHA_MG_MODE_RETAIL = 1,
	MECHA_MG_MODE_PROTOTYPE = 2,
	MECHA_MG_MODE_ARCADE = 3
};

/* The decoded key set: 38 eight-byte blocks, in this order. */
typedef struct MechaMgKeyStore
{
	u8 CardKeyLow[3][8];
	u8 CardKeyHi[3][8];
	u8 CardKey2Low[3][8];
	u8 CardKey2Hi[3][8];
	u8 CardIV[3][8];
	u8 CardIV2[3][8];
	u8 KbitMasterKey[16];
	u8 KcMasterKey[16];
	u8 KbitIv[8];
	u8 KcIv[8];
	u8 icvps2LowKey[16];
	u8 icvps2HiKey[16];
	u8 icvps2LowIV[8];
	u8 icvps2HiIV[8];
	u8 SignatureMasterKey[8];
	u8 SignatureHashKey[8];
	u8 RootSigHashKey[16];
	u8 RootSigMasterKey[8];
	u8 ContentIV[8];
	u8 ContentTableIV[8];
	u8 ChallengeIV[8];
} MechaMgKeyStore;

/* Console identity the key derivation and console bans read. */
typedef struct MechaMgIds
{
	u8 console_id[8];
	u8 ilink_id[8];
} MechaMgIds;

/* Decode one key set out of the encrypted key store. */
void mecha_mg_decode_keystore(MechaMgKeyStore* out, const u8 enc_store[MECHA_MG_ENC_KEYSTORE_SIZE],
	const u8 card_store[MECHA_MG_CARD_KEYSTORE_SIZE], const u8 store_key[MECHA_MG_KEYSTORE_KEY_SIZE], int mode);

/* Arm the keyed path with a decoded key set, or disarm it (ks NULL). */
void mecha_mg_set_keys(const MechaMgKeyStore* ks, const MechaMgIds* ids);

/* After mecha_mg_set_keys: whether the keys are an arcade mechacon's,
 * and its application-type 0x07 Kbit and Kc (both NULL: none). */
void mecha_mg_set_arcade(bool arcade, const u8* kelf07_kbit, const u8* kelf07_kc);

/* Load the key files from dir and arm the keyed path; false (and the
 * keyed path off) when any is missing or the wrong size. In
 * MECHA_MG_MODE_ARCADE the application-type 0x07 key files are read
 * too, when present. */
bool mecha_mg_load_keys(const char* dir, int mode, const MechaMgIds* ids);

bool mecha_mg_keyed(void);

/* One SCMD in 0x80..0x98 on the keyed path. Writes the reply into
 * result (16 bytes) and returns its length. */
int mecha_mg_scmd(u8 cmd, const u8* param, int param_cnt, u8* result, const MechaMgIds* ids);

/* ---- the emulated memory card's side, per SIO slot (0..7) ---------------
 * Values are in protocol order; the SIO layer reverses them on the wire. */
#define MECHA_MG_CARD_SLOTS 8

/* The ID a card hands the mechacon at the end of the handshake, which
 * the mechacon keeps as that card's key: a real card's ID (an arcade
 * dongle's, say) unwraps the KELFs bound to that card. NULL puts back
 * the emulated card's own. Set as each handshake starts; not state. */
void mecha_mg_card_set_id(int slot, const u8 id[8]);

void mecha_mg_card_auth_reset(int slot);
/* Card to console: 0x01 IV (makes a fresh IV, seed and nonce), 0x02
 * seed, 0x04 nonce, 0x0f response 1 (makes the responses), 0x11
 * response 2, 0x13 response 3. False for any other mode. */
bool mecha_mg_card_get(int slot, u8 mode, u8 out[8]);
/* Console to card: 0x0b challenge 1, 0x07 challenge 2, 0x06 challenge 3. */
bool mecha_mg_card_put(int slot, u8 mode, const u8 in[8]);

void mecha_mg_reset(void);
bool mecha_mg_freeze(struct SaveStateBase* s);

#ifdef __cplusplus
}
#endif

#endif
