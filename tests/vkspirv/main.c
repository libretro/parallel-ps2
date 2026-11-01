/*
 * The Vulkan renderer's SPIR-V (pcsx2/GS/Renderers/Vulkan/VKShaders.inc)
 * and what VKSpirvPatch.c makes of it, with no GPU: every module the
 * renderer can make is made, and read back as a driver would read it.
 *
 *  - every module in the header is SPIR-V: the magic, the bound, each
 *    instruction's length, the one entry point main;
 *  - a TFX module patched with nothing dropped is its own words, so a
 *    draw that drops nothing gets the module the compiler wrote;
 *  - for every fragment module, and its dynamic sibling, and all sixteen
 *    combinations of what may be dropped (the vertex modules: the flat
 *    colour), the patched module is SPIR-V again, and:
 *      - a dropped variable is private, of a private pointer type to what
 *        it was, has no decoration and is not in the entry point's
 *        interface; one kept is still Output or Input and still there;
 *      - every Input or Output variable is in the interface, and the
 *        interface names nothing else;
 *      - the geometry capability goes with gl_PrimitiveID and depth
 *        replacing with gl_FragDepth, and only then;
 *      - no Flat member decoration is left when the flat colour is
 *        dropped, one when it is kept;
 *      - each result ID is defined once and is under the bound;
 *  - modules it cannot read are refused (0), never patched: a truncated
 *    one, an instruction running past the end, the wrong magic.
 *
 * Fails, with what was wrong, on a patcher that leaves the capability or
 * the execution mode, the decoration or the interface entry, or that
 * writes the variable's pointer type wrong.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan_core.h>

#include "VKSpirvPatch.h"

typedef uint32_t u32;
typedef uint8_t u8;
typedef struct VKSpirv
{
   const u32 *words;
   u32 count;
} VKSpirv;
typedef struct VKSpirvNamed
{
   const char *name;
   VKSpirv spv;
} VKSpirvNamed;
#include "VKShaders.inc"

#define OP_ENTRY_POINT     15u
#define OP_EXECUTION_MODE  16u
#define OP_CAPABILITY      17u
#define OP_TYPE_POINTER    32u
#define OP_VARIABLE        59u
#define OP_DECORATE        71u
#define OP_MEMBER_DECORATE 72u

static int s_fail;

#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); s_fail = 1; return; } } while (0)

/* The result ID of an instruction, or 0: enough opcodes for these modules
 * (types, constants, variables, functions and the instructions in them
 * that define one), read from the SPIR-V grammar's shape. */
static u32 result_id(const u32 *w, u32 op, u32 len)
{
   switch (op)
   {
      /* result in word 1: types, OpExtInstImport, OpString, OpLabel */
      case 11: case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26:
      case 27: case 28: case 29: case 30: case 31: case 32: case 33: case 248:
         return len > 1 ? w[1] : 0;
      /* no result */
      case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8: case 10:
      case 14: case 15: case 16: case 17: case 56: case 62: case 63: case 71: case 72:
      case 245: case 246: case 247: case 249: case 250: case 251: case 252: case 253:
      case 254: case 255: case 317: case 330:
         return 0;
      default:
         /* result type in word 1, result in word 2 */
         return len > 2 ? w[2] : 0;
   }
}

typedef struct Module
{
   const u32 *w;
   size_t n;
} Module;

/* Walks the module; 0 if malformed. */
static int well_formed(Module m, const char *what)
{
   size_t i, len;
   u32 bound;
   unsigned char *defined;
   int entries = 0;

   if (m.n < 5 || m.w[0] != 0x07230203u)
   {
      printf("  FAIL: %s: no SPIR-V header\n", what);
      return 0;
   }
   bound = m.w[3];
   defined = (unsigned char *)calloc(bound ? bound : 1, 1);
   for (i = 5; i < m.n; i += len)
   {
      const u32 op = m.w[i] & 0xFFFFu;
      u32 id;
      len = m.w[i] >> 16;
      if (len == 0 || len > m.n - i)
      {
         printf("  FAIL: %s: instruction at word %u runs past the end\n", what, (unsigned)i);
         free(defined);
         return 0;
      }
      id = result_id(m.w + i, op, (u32)len);
      if (id)
      {
         if (id >= bound || defined[id])
         {
            printf("  FAIL: %s: ID %u %s\n", what, id, id >= bound ? "over the bound" : "defined twice");
            free(defined);
            return 0;
         }
         defined[id] = 1;
      }
      if (op == OP_ENTRY_POINT)
      {
         entries++;
         if (len < 4 || memcmp(&m.w[i + 3], "main", 5) != 0)
         {
            printf("  FAIL: %s: the entry point is not main\n", what);
            free(defined);
            return 0;
         }
      }
   }
   free(defined);
   if (entries != 1)
   {
      printf("  FAIL: %s: %d entry points\n", what, entries);
      return 0;
   }
   return 1;
}

/* The module's facts this test checks */
typedef struct Facts
{
   int geometry, depth_replacing, flat;
   u32 color1, primid, fragdepth;   /* the variables, by decoration */
   u32 storage[3];                  /* their storage class */
   u32 ptr_storage[3];              /* their pointer type's */
   u32 ptr_pointee[3];
   int decorated[3];                /* decorations left on them */
   int in_interface[3];
   int stray_interface;             /* interface IDs not Input/Output variables */
   int missing_interface;           /* Input/Output variables not in it */
} Facts;

static u32 pointer_info(Module m, u32 type, u32 *storage)
{
   size_t i, len;
   for (i = 5; i < m.n; i += len)
   {
      len = m.w[i] >> 16;
      if ((m.w[i] & 0xFFFFu) == OP_TYPE_POINTER && m.w[i + 1] == type)
      {
         *storage = m.w[i + 2];
         return m.w[i + 3];
      }
   }
   *storage = ~0u;
   return 0;
}

static void facts(Module m, const Facts *of, Facts *f)
{
   size_t i, len, k;
   u32 vars[3];
   const u32 *iface = NULL;
   size_t niface = 0;

   memset(f, 0, sizeof(*f));
   /* the variables are found in the module as compiled (of == NULL), and
    * looked for by ID after */
   for (i = 5; i < m.n; i += len)
   {
      const u32 op = m.w[i] & 0xFFFFu;
      len = m.w[i] >> 16;
      if (op == OP_CAPABILITY && m.w[i + 1] == 2u)
         f->geometry = 1;
      else if (op == OP_EXECUTION_MODE && m.w[i + 2] == 12u)
         f->depth_replacing = 1;
      else if (op == OP_MEMBER_DECORATE && len >= 4 && m.w[i + 3] == 14u)
         f->flat++;
      else if (op == OP_DECORATE && len >= 4 && !of)
      {
         if (m.w[i + 2] == 32u && m.w[i + 3] == 1u)
            f->color1 = m.w[i + 1];
         else if (m.w[i + 2] == 11u && m.w[i + 3] == 7u)
            f->primid = m.w[i + 1];
         else if (m.w[i + 2] == 11u && m.w[i + 3] == 22u)
            f->fragdepth = m.w[i + 1];
      }
      else if (op == OP_ENTRY_POINT)
      {
         k = 3;
         while (k < len && (m.w[i + k] >> 24) != 0)
            k++;
         iface  = &m.w[i + k + 1];
         niface = len - k - 1;
      }
   }
   if (of)
   {
      f->color1    = of->color1;
      f->primid    = of->primid;
      f->fragdepth = of->fragdepth;
   }
   vars[0] = f->color1;
   vars[1] = f->primid;
   vars[2] = f->fragdepth;
   for (i = 5; i < m.n; i += len)
   {
      const u32 op = m.w[i] & 0xFFFFu;
      len = m.w[i] >> 16;
      if (op == OP_DECORATE)
      {
         for (k = 0; k < 3; k++)
            if (vars[k] && m.w[i + 1] == vars[k])
               f->decorated[k]++;
      }
      else if (op == OP_VARIABLE)
      {
         const u32 sc = m.w[i + 3];
         size_t j;
         int listed = 0;
         for (k = 0; k < 3; k++)
            if (vars[k] && m.w[i + 2] == vars[k])
            {
               f->storage[k]     = sc;
               f->ptr_pointee[k] = pointer_info(m, m.w[i + 1], &f->ptr_storage[k]);
            }
         for (j = 0; j < niface; j++)
            if (iface[j] == m.w[i + 2])
               listed = 1;
         if ((sc == 1u || sc == 3u) && !listed)
            f->missing_interface++;
         if (sc != 1u && sc != 3u && listed)
            f->stray_interface++;
      }
   }
   for (k = 0; k < 3; k++)
   {
      size_t j;
      for (j = 0; j < niface; j++)
         if (vars[k] && iface[j] == vars[k])
            f->in_interface[k] = 1;
   }
   /* an interface ID that is no variable at all */
   for (k = 0; k < niface; k++)
   {
      int found = 0;
      for (i = 5; i < m.n; i += len)
      {
         len = m.w[i] >> 16;
         if ((m.w[i] & 0xFFFFu) == OP_VARIABLE && m.w[i + 2] == iface[k])
            found = 1;
      }
      if (!found)
         f->stray_interface++;
   }
}

static void check_fragment(const VKSpirv *spv, const char *name)
{
   static const char *const what[3] = {"o_col1", "gl_PrimitiveID", "gl_FragDepth"};
   static const unsigned bit[3] = {VKSPV_DROP_COLOR1, VKSPV_DROP_PRIMID, VKSPV_DROP_FRAGDEPTH};
   Module in;
   Facts base;
   u32 *out = (u32 *)malloc((spv->count + VKSPV_PATCH_EXTRA) * sizeof(u32));
   unsigned flags;

   in.w = spv->words;
   in.n = spv->count;
   if (!well_formed(in, name))
   {
      s_fail = 1;
      free(out);
      return;
   }
   facts(in, NULL, &base);
   if (!base.color1 || !base.primid || !base.fragdepth || !base.geometry || !base.depth_replacing || base.flat != 1)
   {
      printf("  FAIL: %s: not the superset module (o_col1 %u, primitive ID %u, depth %u, geometry %d, depth replacing %d, flat %d)\n",
         name, base.color1, base.primid, base.fragdepth, base.geometry, base.depth_replacing, base.flat);
      s_fail = 1;
      free(out);
      return;
   }
   for (flags = 0; flags < 16; flags++)
   {
      char label[96];
      Module m;
      Facts f;
      unsigned k;
      size_t n = vkspv_patch(spv->words, spv->count, flags, out);

      sprintf(label, "%s dropping %s%s%s%s", name, (flags & 1) ? "colour1 " : "", (flags & 2) ? "primid " : "",
         (flags & 4) ? "fragdepth " : "", (flags & 8) ? "flat" : "");
      if (!n)
      {
         printf("  FAIL: %s: refused\n", label);
         s_fail = 1;
         continue;
      }
      if (n > spv->count + VKSPV_PATCH_EXTRA)
      {
         printf("  FAIL: %s: %u words, more than room was made for\n", label, (unsigned)n);
         s_fail = 1;
         continue;
      }
      if (flags == 0 && (n != spv->count || memcmp(out, spv->words, n * sizeof(u32))))
      {
         printf("  FAIL: %s: not the module's own words\n", label);
         s_fail = 1;
      }
      m.w = out;
      m.n = n;
      if (!well_formed(m, label))
      {
         s_fail = 1;
         continue;
      }
      facts(m, &base, &f);
      for (k = 0; k < 3; k++)
      {
         const u32 io = (k == 1) ? 1u : 3u; /* the primitive ID is an input */
         if (flags & bit[k])
         {
            if (f.storage[k] != 6u || f.ptr_storage[k] != 6u || f.decorated[k] || f.in_interface[k]
               || f.ptr_pointee[k] != base.ptr_pointee[k])
            {
               printf("  FAIL: %s: %s not made private (storage %u, pointer %u, %d decorations, %s)\n", label,
                  what[k], f.storage[k], f.ptr_storage[k], f.decorated[k], f.in_interface[k] ? "in the interface" : "");
               s_fail = 1;
            }
         }
         else if (f.storage[k] != io || f.ptr_storage[k] != io || !f.decorated[k] || !f.in_interface[k])
         {
            printf("  FAIL: %s: %s kept but changed\n", label, what[k]);
            s_fail = 1;
         }
      }
      if (f.geometry != !(flags & VKSPV_DROP_PRIMID))
      {
         printf("  FAIL: %s: the geometry capability %s\n", label, f.geometry ? "is left" : "is gone");
         s_fail = 1;
      }
      if (f.depth_replacing != !(flags & VKSPV_DROP_FRAGDEPTH))
      {
         printf("  FAIL: %s: depth replacing %s\n", label, f.depth_replacing ? "is left" : "is gone");
         s_fail = 1;
      }
      if (f.flat != ((flags & VKSPV_DROP_FLAT) ? 0 : 1))
      {
         printf("  FAIL: %s: %d Flat member decorations\n", label, f.flat);
         s_fail = 1;
      }
      if (f.stray_interface || f.missing_interface)
      {
         printf("  FAIL: %s: the interface names %d it should not and misses %d\n", label,
            f.stray_interface, f.missing_interface);
         s_fail = 1;
      }
   }
   free(out);
}

static void check_vertex(const VKSpirv *spv, const char *name)
{
   u32 *out = (u32 *)malloc((spv->count + VKSPV_PATCH_EXTRA) * sizeof(u32));
   Module in, m;
   Facts f;
   size_t n;

   in.w = spv->words;
   in.n = spv->count;
   if (!well_formed(in, name))
   {
      s_fail = 1;
      free(out);
      return;
   }
   n = vkspv_patch(spv->words, spv->count, VKSPV_DROP_FLAT, out);
   m.w = out;
   m.n = n;
   if (!n || !well_formed(m, name))
   {
      printf("  FAIL: %s: the flat colour not dropped\n", name);
      s_fail = 1;
      free(out);
      return;
   }
   facts(m, NULL, &f);
   if (f.flat != 0 || n != spv->count - 4)
   {
      printf("  FAIL: %s: dropping the flat colour left %d (%u words of %u)\n", name, f.flat, (unsigned)n, spv->count);
      s_fail = 1;
   }
   free(out);
}

static void check_module(const VKSpirv *spv, const char *name)
{
   Module m;
   m.w = spv->words;
   m.n = spv->count;
   if (!well_formed(m, name))
      s_fail = 1;
}

static void check_refused(void)
{
   const VKSpirv *spv = &vkspv_tfx_fs_mod[0];
   u32 *copy = (u32 *)malloc(spv->count * sizeof(u32));
   u32 *out  = (u32 *)malloc((spv->count + VKSPV_PATCH_EXTRA) * sizeof(u32));
   size_t i, len;

   memcpy(copy, spv->words, spv->count * sizeof(u32));
   CHECK(vkspv_patch(copy, 4, 15, out) == 0, "a module shorter than its header was patched");
   copy[0] ^= 1;
   CHECK(vkspv_patch(copy, spv->count, 15, out) == 0, "a module of the wrong magic was patched");
   copy[0] ^= 1;
   /* the last instruction made to run past the end */
   for (i = 5; i < spv->count; i += len)
   {
      len = copy[i] >> 16;
      if (i + len == spv->count)
         break;
   }
   copy[i] += 1u << 16;
   CHECK(vkspv_patch(copy, spv->count, 15, out) == 0, "a module whose last instruction runs past its end was patched");
   copy[i] -= 1u << 16;
   copy[5] &= 0xFFFFu; /* a zero length */
   CHECK(vkspv_patch(copy, spv->count, 15, out) == 0, "a module with an instruction of no length was patched");
   free(copy);
   free(out);
}

int main(void)
{
   static const char *const fs_names[3] = {"fragment (target unread)", "fragment (input attachment)", "fragment (sampled target)"};
   static const char *const dyn_names[3] = {"dynamic sibling (target unread)", "dynamic sibling (input attachment)",
      "dynamic sibling (sampled target)"};
   unsigned i;

   for (i = 0; i < 3; i++)
      check_fragment(&vkspv_tfx_fs_mod[i], fs_names[i]);
   for (i = 0; i < 3; i++)
      check_fragment(&vkspv_tfx_fs_dynamic_mod[i], dyn_names[i]);
   check_vertex(&vkspv_tfx_vs_mod[0], "vertex");
   check_vertex(&vkspv_tfx_vs_mod[1], "vertex (expand)");
   for (i = 0; i < 3; i++)
      check_module(&vkspv_util_vs_mod[i], "a utility vertex shader");
   for (i = 0; i < sizeof(vkspv_convert_mod) / sizeof(vkspv_convert_mod[0]); i++)
      check_module(&vkspv_convert_mod[i].spv, vkspv_convert_mod[i].name);
   for (i = 0; i < 4; i++)
      check_module(&vkspv_stencil_init_mod[i], "ps_stencil_image_init");
   for (i = 0; i < sizeof(vkspv_interlace_mod) / sizeof(vkspv_interlace_mod[0]); i++)
      check_module(&vkspv_interlace_mod[i], "interlace");
   for (i = 0; i < 2; i++)
      check_module(&vkspv_merge_mod[i], "merge");
   for (i = 0; i < 2; i++)
      check_module(&vkspv_vr_mod[i], "vr");
   check_refused();

   if (s_fail)
      return 1;
   printf("  ok: %u fragment modules x 16 interfaces, 2 vertex modules, %u utility modules, the refusals\n", 6u,
      (unsigned)(3 + sizeof(vkspv_convert_mod) / sizeof(vkspv_convert_mod[0]) + 4
         + sizeof(vkspv_interlace_mod) / sizeof(vkspv_interlace_mod[0]) + 4));
   return 0;
}
