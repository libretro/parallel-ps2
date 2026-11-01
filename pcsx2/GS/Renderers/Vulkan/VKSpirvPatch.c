#include <string.h>

#include "VKSpirvPatch.h"

#define SPV_MAGIC 0x07230203u

/* opcodes */
#define SPV_OP_ENTRY_POINT     15u
#define SPV_OP_EXECUTION_MODE  16u
#define SPV_OP_CAPABILITY      17u
#define SPV_OP_TYPE_POINTER    32u
#define SPV_OP_VARIABLE        59u
#define SPV_OP_DECORATE        71u
#define SPV_OP_MEMBER_DECORATE 72u

/* operands */
#define SPV_CAP_GEOMETRY        2u
#define SPV_MODE_DEPTH_REPLACING 12u
#define SPV_DECO_FLAT           14u
#define SPV_DECO_BUILTIN        11u
#define SPV_DECO_INDEX          32u
#define SPV_BUILTIN_PRIMITIVE_ID 7u
#define SPV_BUILTIN_FRAG_DEPTH  22u
#define SPV_STORAGE_INPUT       1u
#define SPV_STORAGE_OUTPUT      3u
#define SPV_STORAGE_PRIVATE     6u

#define VKSPV_MAX_DROP 3u

static int vkspv_dropped(const uint32_t *drop, unsigned ndrop, uint32_t id)
{
   unsigned k;
   for (k = 0; k < ndrop; k++)
      if (drop[k] == id)
         return 1;
   return 0;
}

/* The type a pointer type points to, or 0. */
static uint32_t vkspv_pointee(const uint32_t *in, size_t n, uint32_t ptr)
{
   size_t i, len;
   for (i = 5; i < n; i += len)
   {
      len = in[i] >> 16;
      if ((in[i] & 0xFFFFu) == SPV_OP_TYPE_POINTER && len == 4 && in[i + 1] == ptr)
         return in[i + 3];
   }
   return 0;
}

size_t vkspv_patch(const uint32_t *in, size_t n, unsigned flags, uint32_t *out)
{
   uint32_t drop[VKSPV_MAX_DROP];
   unsigned ndrop    = 0;
   int drop_depth    = 0;
   int drop_geometry = 0;
   uint32_t bound;
   size_t i, len, o;

   if (n < 5 || in[0] != SPV_MAGIC)
      return 0;
   bound = in[3];

   /* The variables dropped, found by their decorations; and the module
    * read through once, so the second pass may trust the lengths. */
   for (i = 5; i < n; i += len)
   {
      const uint32_t op = in[i] & 0xFFFFu;
      len = in[i] >> 16;
      if (len == 0 || len > n - i)
         return 0;
      if (op == SPV_OP_DECORATE && len >= 4)
      {
         const uint32_t deco = in[i + 2], value = in[i + 3];
         int which = 0; /* 1 depth, 2 primitive ID, 3 colour 1 */
         if (deco == SPV_DECO_BUILTIN && value == SPV_BUILTIN_FRAG_DEPTH && (flags & VKSPV_DROP_FRAGDEPTH))
            which = 1;
         else if (deco == SPV_DECO_BUILTIN && value == SPV_BUILTIN_PRIMITIVE_ID && (flags & VKSPV_DROP_PRIMID))
            which = 2;
         else if (deco == SPV_DECO_INDEX && value == 1u && (flags & VKSPV_DROP_COLOR1))
            which = 3;
         if (which && !vkspv_dropped(drop, ndrop, in[i + 1]))
         {
            if (ndrop == VKSPV_MAX_DROP)
               return 0;
            drop[ndrop++] = in[i + 1];
         }
         if (which == 1)
            drop_depth = 1;
         else if (which == 2)
            drop_geometry = 1;
      }
   }

   memcpy(out, in, 5 * sizeof(*in));
   o = 5;
   for (i = 5; i < n; i += len)
   {
      const uint32_t op = in[i] & 0xFFFFu;
      len = in[i] >> 16;
      switch (op)
      {
         case SPV_OP_CAPABILITY:
            /* gl_PrimitiveID's, in a fragment shader */
            if (drop_geometry && len == 2 && in[i + 1] == SPV_CAP_GEOMETRY)
               continue;
            break;
         case SPV_OP_ENTRY_POINT:
            if (ndrop)
            {
               /* model, function, the name (to the word that ends
                * with a zero byte), then the interface */
               size_t k = i + 3, start = o;
               if (len < 4)
                  return 0;
               while (k < i + len && (in[k] >> 24) != 0)
                  k++;
               if (k >= i + len)
                  return 0;
               k++;
               memcpy(out + o, in + i, (k - i) * sizeof(*in));
               o += k - i;
               for (; k < i + len; k++)
                  if (!vkspv_dropped(drop, ndrop, in[k]))
                     out[o++] = in[k];
               out[start] = (uint32_t)((o - start) << 16) | SPV_OP_ENTRY_POINT;
               continue;
            }
            break;
         case SPV_OP_EXECUTION_MODE:
            if (drop_depth && len >= 3 && in[i + 2] == SPV_MODE_DEPTH_REPLACING)
               continue;
            break;
         case SPV_OP_DECORATE:
            if (len >= 2 && vkspv_dropped(drop, ndrop, in[i + 1]))
               continue;
            break;
         case SPV_OP_MEMBER_DECORATE:
            if ((flags & VKSPV_DROP_FLAT) && len >= 4 && in[i + 3] == SPV_DECO_FLAT)
               continue;
            break;
         case SPV_OP_VARIABLE:
            if (len >= 4 && vkspv_dropped(drop, ndrop, in[i + 2]))
            {
               /* A pointer to the same type, private, and the variable
                * of that type; no initializer (an interface variable
                * has none). */
               const uint32_t pointee = vkspv_pointee(in, n, in[i + 1]);
               if (!pointee || (in[i + 3] != SPV_STORAGE_INPUT && in[i + 3] != SPV_STORAGE_OUTPUT))
                  return 0;
               out[o++] = (4u << 16) | SPV_OP_TYPE_POINTER;
               out[o++] = bound;
               out[o++] = SPV_STORAGE_PRIVATE;
               out[o++] = pointee;
               out[o++] = (4u << 16) | SPV_OP_VARIABLE;
               out[o++] = bound;
               out[o++] = in[i + 2];
               out[o++] = SPV_STORAGE_PRIVATE;
               bound++;
               continue;
            }
            break;
         default:
            break;
      }
      memcpy(out + o, in + i, len * sizeof(*in));
      o += len;
   }
   out[3] = bound;
   return o;
}
