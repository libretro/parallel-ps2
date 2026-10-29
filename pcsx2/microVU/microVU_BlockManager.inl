/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
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

// Must be included after the backend-specific 'struct microVU' is defined,
// since the add() and search() methods access mVU->compareStateF.

#pragma once


/* Per-startPC block table. C89 shape: a plain struct with prefixed
 * functions. quickLookup was a std::vector<microBlockLinkRef>; it is a
 * malloc'd array with an explicit count and capacity, grown by doubling,
 * which is what the vector did. */
struct microBlockManager
{
	microBlockLink *qBlockList, *qBlockEnd; /* Quick Search */
	microBlockLink *fBlockList, *fBlockEnd; /* Full  Search */
	struct microBlockLinkRef* quickLookup;
	u32 quickCount;
	u32 quickCapacity;
};

static void mVUbm_init(struct microBlockManager* m)
{
	m->qBlockEnd = m->qBlockList = NULL;
	m->fBlockEnd = m->fBlockList = NULL;
	m->quickLookup   = NULL;
	m->quickCount    = 0;
	m->quickCapacity = 0;
}

static void mVUbm_reset(struct microBlockManager* m)
{
	microBlockLink* linkI;

	for (linkI = m->qBlockList; linkI != NULL;)
	{
		microBlockLink* freeI = linkI;
		free(linkI->block.jumpCache);
		linkI->block.jumpCache = NULL;
		linkI = linkI->next;
		memalign_free(freeI);
	}
	for (linkI = m->fBlockList; linkI != NULL;)
	{
		microBlockLink* freeI = linkI;
		free(linkI->block.jumpCache);
		linkI->block.jumpCache = NULL;
		linkI = linkI->next;
		memalign_free(freeI);
	}
	m->qBlockEnd = m->qBlockList = NULL;
	m->fBlockEnd = m->fBlockList = NULL;
	m->quickCount = 0; /* capacity and buffer are kept for reuse */
}

static void mVUbm_destroy(struct microBlockManager* m)
{
	mVUbm_reset(m);
	free(m->quickLookup);
	m->quickLookup   = NULL;
	m->quickCapacity = 0;
}

static __ri microBlock* mVUbm_search(struct microBlockManager* m, microVU* mVU, microRegInfo* pState)
{
	if (pState->needExactMatch) /* Needs Detailed Search (Exact Match of Pipeline State) */
	{
		microBlockLink* prevI = NULL;
		microBlockLink* linkI;
		for (linkI = m->fBlockList; linkI != NULL; prevI = linkI, linkI = linkI->next)
		{
			if (((u32(*)(void*, void*))mVU->compareStateF)(pState, &linkI->block.pState) == 0)
			{
				if (linkI != m->fBlockList)
				{
					prevI->next = linkI->next;
					linkI->next = m->fBlockList;
					m->fBlockList = linkI;
				}

				return &linkI->block;
			}
		}
	}
	else /* Can do Simple Search (Only Matches the Important Pipeline Stuff) */
	{
		const u64 quick64 = pState->quick64[0];
		u32 i;
		for (i = 0; i < m->quickCount; i++)
		{
			const struct microBlockLinkRef* ref = &m->quickLookup[i];
			/* If the flag hack is on, ignore the MAC flags (0x0C04) in the
			 * quick match: a non-exact-match block does not need them, so
			 * allowing blocks that differ only in MAC flags to match cuts
			 * the block count (less recompilation / stuttering). */
			if (mVUsFlagHack)
			{
				if ((ref->quick & ~0x0C04) != (quick64 & ~0x0C04)) continue;
			}
			else if (ref->quick != quick64) continue;
			if (doConstProp && (ref->pBlock->pState.vi15 != pState->vi15))  continue;
			if (doConstProp && (ref->pBlock->pState.vi15v != pState->vi15v)) continue;
			return ref->pBlock;
		}
	}
	return NULL;
}

static microBlock* mVUbm_add(struct microBlockManager* m, microVU* mVU, microBlock* pBlock)
{
	microBlock* thisBlock = mVUbm_search(m, mVU, &pBlock->pState);
	if (!thisBlock)
	{
		const u8 fullCmp = pBlock->pState.needExactMatch;

		microBlockLink** blockList = fullCmp ? &m->fBlockList : &m->qBlockList;
		microBlockLink** blockEnd  = fullCmp ? &m->fBlockEnd  : &m->qBlockEnd;
		microBlockLink*  newBlock  = (microBlockLink*)memalign_alloc(32, sizeof(microBlockLink));
		newBlock->block.jumpCache  = NULL;
		newBlock->next             = NULL;

		if (*blockEnd)
		{
			(*blockEnd)->next = newBlock;
			*blockEnd         = newBlock;
		}
		else
			*blockEnd = *blockList = newBlock;

		memcpy(&newBlock->block, pBlock, sizeof(microBlock));
		thisBlock = &newBlock->block;

		if (m->quickCount == m->quickCapacity)
		{
			const u32 newcap = m->quickCapacity ? m->quickCapacity * 2 : 64;
			m->quickLookup   = (struct microBlockLinkRef*)realloc(m->quickLookup,
					newcap * sizeof(struct microBlockLinkRef));
			m->quickCapacity = newcap;
		}
		m->quickLookup[m->quickCount].pBlock = &newBlock->block;
		m->quickLookup[m->quickCount].quick  = pBlock->pState.quick64[0];
		m->quickCount++;
	}
	return thisBlock;
}
