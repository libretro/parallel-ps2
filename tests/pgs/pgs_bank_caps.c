/* No module in the shader bank declares a 16-bit storage capability whose
 * device feature paraLLEl-GS neither requires nor checks for.
 *
 * GSRenderer::init requires storageBuffer16BitAccess and shaderInt16 and
 * nothing more of the 16-bit family, so a module needing
 * UniformAndStorageBuffer16BitAccess (a 16-bit member in a UBO),
 * StoragePushConstant16 or StorageInputOutput16 is invalid on a device
 * without that feature. sampler_feedback carried 16-bit UBO members; one
 * driver hang (NVIDIA Blackwell) was put down to them.
 *
 * Without buffer device address a module declaring
 * PhysicalStorageBufferAddresses cannot be made, so each one has a NAME_nobda
 * variant that does not declare it, or is one of the modules the renderer
 * goes without on such a device (no_bda_unused below). Likewise without
 * 8-bit storage for StorageBuffer8BitAccess: each such module has a
 * variant with _no8 or _nobda_no8 added to its name (before any index)
 * that declares neither.
 *
 * Reads slangmosh.hpp, finds each module from its spirv_bank + N offset
 * and walks the capabilities at its head.
 *
 * Usage: pgs_bank_caps slangmosh.hpp
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPV_MAGIC          0x07230203UL
#define OP_CAPABILITY      17UL
#define OP_EXTENSION       10UL
#define CAP_UBO16          4434UL
#define CAP_PUSHCONST16    4435UL
#define CAP_INPUTOUTPUT16  4436UL
#define CAP_BDA            5347UL
#define CAP_STORAGE8       4448UL
#define MAX_MODULES        64
#define MAX_NAME           64

/* Used only with buffer device address: the qword clears turn into
 * fills, and sampler feedback is left off. */
static const char *const no_bda_unused[] = { "qword_clear", "sampler_feedback" };

static char *read_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   char *buf;
   long len;

   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   len = ftell(f);
   fseek(f, 0, SEEK_SET);
   buf = (char*)malloc((size_t)len + 1);
   if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len)
   {
      free(buf);
      buf = NULL;
   }
   if (buf)
      buf[len] = '\0';
   fclose(f);
   return buf;
}

static const char *cap_name(unsigned long cap)
{
   switch (cap)
   {
      case CAP_UBO16:         return "UniformAndStorageBuffer16BitAccess";
      case CAP_PUSHCONST16:   return "StoragePushConstant16";
      case CAP_INPUTOUTPUT16: return "StorageInputOutput16";
   }
   return NULL;
}

int main(int argc, char **argv)
{
   char *text, *p, *end;
   unsigned long *words;
   size_t nwords = 0, cap = 1 << 16;
   int modules = 0, bad = 0, m, n;
   static char names[MAX_MODULES][MAX_NAME];
   static int bda[MAX_MODULES], st8[MAX_MODULES];

   if (argc < 2 || !(text = read_file(argv[1])))
   {
      fprintf(stderr, "usage: %s slangmosh.hpp\n", argv[0]);
      return 2;
   }

   p = strstr(text, "spirv_bank[] =");
   end = p ? strstr(p, "};") : NULL;
   if (!p || !end)
   {
      fprintf(stderr, "no spirv_bank in %s\n", argv[1]);
      return 2;
   }
   words = (unsigned long*)malloc(cap * sizeof(*words));
   /* A plain scan, not strstr, which under ASan measures the rest of the
    * text on every call. */
   for (; words && p + 1 < end; p++)
   {
      if (p[0] != '0' || p[1] != 'x')
         continue;
      if (nwords == cap)
         words = (unsigned long*)realloc(words, (cap *= 2) * sizeof(*words));
      if (!words)
         break;
      words[nwords++] = strtoul(p, &p, 16);
      p--;
   }
   if (!words)
      return 2;

   for (p = end; (p = strstr(p, "this->")); )
   {
      unsigned long off;
      char *call = strstr(p, "spirv_bank + ");
      size_t i, len = strcspn(p + 6, " =");
      int needs_bda = 0, needs_8 = 0;

      if (!call || modules == MAX_MODULES || len >= MAX_NAME)
         break;
      memcpy(names[modules], p + 6, len);
      names[modules][len] = '\0';
      off = strtoul(call + 13, &p, 10);
      if (off + 5 >= nwords || words[off] != SPV_MAGIC)
      {
         printf("  FAIL module at word %lu: no SPIR-V header\n", off);
         bad++;
         continue;
      }
      for (i = (size_t)off + 5; i < nwords; )
      {
         unsigned long op = words[i] & 0xffffUL;
         unsigned long wc = words[i] >> 16;
         if (wc == 0 || (op != OP_CAPABILITY && op != OP_EXTENSION))
            break;
         if (op == OP_CAPABILITY && i + 1 < nwords && cap_name(words[i + 1]))
         {
            printf("  FAIL module at word %lu declares %s\n", off, cap_name(words[i + 1]));
            bad++;
         }
         if (op == OP_CAPABILITY && i + 1 < nwords && words[i + 1] == CAP_BDA)
            needs_bda = 1;
         if (op == OP_CAPABILITY && i + 1 < nwords && words[i + 1] == CAP_STORAGE8)
            needs_8 = 1;
         i += wc;
      }
      bda[modules] = needs_bda;
      st8[modules++] = needs_8;
   }

   for (m = 0; m < modules; m++)
   {
      char want[MAX_NAME + 16];
      int found = 0;
      size_t len = strlen(names[m]);

      if (len > 6 && !strcmp(names[m] + len - 6, "_nobda") && bda[m])
      {
         printf("  FAIL %s declares PhysicalStorageBufferAddresses\n", names[m]);
         bad++;
      }
      if (st8[m] && !strstr(names[m], "_no8"))
      {
         const char *suffixes[2] = { "_no8", "_nobda_no8" };
         size_t base = strcspn(names[m], "[");
         int k, ok = 0;
         for (k = 0; k < 2; k++)
         {
            sprintf(want, "%.*s%s%s", (int)base, names[m], suffixes[k], names[m] + base);
            for (n = 0; n < modules; n++)
               if (!strcmp(names[n], want) && !st8[n] && !bda[n])
                  ok = 1;
         }
         if (!ok)
         {
            printf("  FAIL %s needs 8-bit storage and has no _no8 variant\n", names[m]);
            bad++;
         }
      }
      if (!bda[m])
         continue;
      for (n = 0; n < (int)(sizeof(no_bda_unused) / sizeof(no_bda_unused[0])); n++)
         if (!strcmp(names[m], no_bda_unused[n]))
            found = 1;
      sprintf(want, "%s_nobda", names[m]);
      for (n = 0; n < modules; n++)
         if (!strcmp(names[n], want) && !bda[n])
            found = 1;
      if (!found)
      {
         printf("  FAIL %s needs buffer device address and has no %s\n", names[m], want);
         bad++;
      }
   }

   free(words);
   free(text);
   if (!modules)
   {
      printf("pgs_bank_caps: no modules found\n");
      return 1;
   }
   if (bad)
   {
      printf("pgs_bank_caps: %d failure(s)\n", bad);
      return 1;
   }
   printf("pgs_bank_caps: %d module loads, no unchecked 16-bit capability, a no-BDA and a no-8-bit path for each\n", modules);
   return 0;
}
