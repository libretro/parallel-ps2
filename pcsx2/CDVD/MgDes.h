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

/* DES (FIPS 46-3) and the two-key EDE form MagicGate uses. C89. */
#ifndef MGDES_H
#define MGDES_H

#include "common/Pcsx2Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A DES key schedule: sixteen round keys, each eight 6-bit pieces. */
typedef struct MgDesKey
{
	u8 round[16][8];
} MgDesKey;

void mg_des_setkey(MgDesKey* ks, const u8 key[8]);
void mg_des_encrypt_ks(const MgDesKey* ks, u8 block[8]);
void mg_des_decrypt_ks(const MgDesKey* ks, u8 block[8]);

/* Single-block helpers taking the raw 8-byte key. */
void mg_des_encrypt(const u8 key[8], u8 block[8]);
void mg_des_decrypt(const u8 key[8], u8 block[8]);

/* Two-key triple DES on one block, key = K1 || K2:
 * encrypt is E(K1) D(K2) E(K1), decrypt is D(K1) E(K2) D(K1). */
void mg_3des_encrypt(const u8 key[16], u8 block[8]);
void mg_3des_decrypt(const u8 key[16], u8 block[8]);

#ifdef __cplusplus
}
#endif

#endif
