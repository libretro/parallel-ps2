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
   int modules = 0, bad = 0;

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

   for (p = end; (p = strstr(p, "spirv_bank + ")); )
   {
      unsigned long off = strtoul(p + 13, &p, 10);
      size_t i;

      if (off + 5 >= nwords || words[off] != SPV_MAGIC)
      {
         printf("  FAIL module at word %lu: no SPIR-V header\n", off);
         bad++;
         continue;
      }
      modules++;
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
         i += wc;
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
   printf("pgs_bank_caps: %d module loads, no unchecked 16-bit capability\n", modules);
   return 0;
}
