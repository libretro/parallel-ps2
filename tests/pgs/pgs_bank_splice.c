/* paraLLEl-GS shader bank splice.
 *
 * shaders/slangmosh.hpp holds the renderer's SPIR-V modules end to end in
 * one uint32 array, and a table that hands each program its module by word
 * offset and byte size. A module rebuilt from its source (shaderbank_build.sh
 * does both, with the toolchain each module is built with) is put back into
 * the bank with this: the module's words
 * are replaced, the ones after it move, and every table entry is rewritten
 * with its new offset and size. Modules that are not replaced are copied
 * byte for byte, and the reflection bank is untouched, which is right as
 * long as the module's bindings and specialization constants have not
 * changed.
 *
 *   pgs_bank_splice in.hpp out.hpp NAME=module.spv [NAME=module.spv ...]
 *   pgs_bank_splice -x in.hpp NAME=module.spv [NAME=module.spv ...]
 *
 * NAME is the table's field: ubershader[0][1], sample_circuit[0],
 * triangle_setup. A NAME the table does not carry is an error, as is a
 * module without the SPIR-V magic. Words the table does not hand out
 * (slangmosh emits a module the interface never declares) stay where
 * they are relative to their neighbours. With -x the named modules are
 * written out of the bank instead, as they are.
 *
 * Build, from tests/pgs:
 *   cc -O2 -std=c89 -pedantic -Wall pgs_bank_splice.c -o pgs_bank_splice
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MODULES 64
#define MAX_REPLACE 16

struct entry
{
   char name[64];
   unsigned long off;   /* words */
   unsigned long bytes;
   long line;           /* index into the table text, for rewriting */
};

struct module
{
   unsigned long off;
   unsigned long bytes;
   unsigned long new_off;
   unsigned long new_bytes;
   unsigned long *words; /* replacement, or NULL */
};

static char *read_file(const char *path, unsigned long *len)
{
   FILE *f = fopen(path, "rb");
   char *buf;
   long n;

   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   buf = (char *)malloc((size_t)n + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   if (fread(buf, 1, (size_t)n, f) != (size_t)n)
   {
      fclose(f);
      free(buf);
      return NULL;
   }
   fclose(f);
   buf[n] = 0;
   *len = (unsigned long)n;
   return buf;
}

/* The table entries, in file order. */
static int parse_table(const char *text, struct entry *entries, int max)
{
   const char *p = text;
   int n = 0;

   for (;;)
   {
      const char *call, *name_start, *name_end;
      unsigned long off, bytes;

      p = strstr(p, "this->");
      if (!p)
         break;
      name_start = p + 6;
      name_end = strstr(name_start, " = device.request_");
      if (!name_end)
         break;
      call = strstr(name_end, "(spirv_bank + ");
      if (!call || call > name_end + 40)
      {
         p = name_end;
         continue;
      }
      if (sscanf(call + 14, "%lu, %lu", &off, &bytes) != 2)
      {
         p = name_end;
         continue;
      }
      if (n == max || name_end - name_start >= (long)sizeof(entries[0].name))
         return -1;
      memcpy(entries[n].name, name_start, (size_t)(name_end - name_start));
      entries[n].name[name_end - name_start] = 0;
      entries[n].off = off;
      entries[n].bytes = bytes;
      entries[n].line = (long)(call - text);
      n++;
      p = call;
   }
   return n;
}

static int cmp_module(const void *a, const void *b)
{
   const struct module *ma = (const struct module *)a;
   const struct module *mb = (const struct module *)b;
   return ma->off < mb->off ? -1 : ma->off > mb->off;
}

int main(int argc, char **argv)
{
   char *text;
   unsigned long len;
   const char *bank_start, *bank_body, *bank_end;
   unsigned long *words;
   unsigned long num_words = 0, cap;
   struct entry entries[MAX_MODULES];
   struct module modules[MAX_MODULES];
   int num_entries, num_modules = 0;
   int i, j;
   const char *p;
   FILE *out;
   unsigned long off;
   long written;

   int extract = argc > 1 && !strcmp(argv[1], "-x");

   if (argc < 3)
   {
      fprintf(stderr, "usage: %s in.hpp out.hpp NAME=module.spv ...\n"
                      "       %s -x in.hpp NAME=module.spv ...\n", argv[0], argv[0]);
      return 2;
   }

   text = read_file(argv[1 + extract], &len);
   if (!text)
   {
      fprintf(stderr, "cannot read %s\n", argv[1 + extract]);
      return 1;
   }

   bank_start = strstr(text, "static const uint32_t spirv_bank[] =");
   bank_body = bank_start ? strstr(bank_start, "{\n") : NULL;
   bank_end = bank_body ? strstr(bank_body, "\n};") : NULL;
   if (!bank_end)
   {
      fprintf(stderr, "%s: no spirv_bank[]\n", argv[1 + extract]);
      return 1;
   }
   bank_body += 2;

   cap = (unsigned long)(bank_end - bank_body) / 12 + 16;
   words = (unsigned long *)malloc(cap * sizeof(*words));
   if (!words)
      return 1;
   for (p = bank_body; p < bank_end;)
   {
      const char *hex = strstr(p, "0x");
      char *end;
      if (!hex || hex >= bank_end)
         break;
      words[num_words++] = strtoul(hex + 2, &end, 16);
      p = end;
   }

   num_entries = parse_table(bank_end, entries, MAX_MODULES);
   if (num_entries <= 0)
   {
      fprintf(stderr, "%s: no program table\n", argv[1 + extract]);
      return 1;
   }

   /* Distinct modules: two entries may name the same module. */
   for (i = 0; i < num_entries; i++)
   {
      for (j = 0; j < num_modules; j++)
         if (modules[j].off == entries[i].off)
            break;
      if (j == num_modules)
      {
         modules[j].off = entries[i].off;
         modules[j].bytes = entries[i].bytes;
         modules[j].words = NULL;
         num_modules++;
      }
      else if (modules[j].bytes != entries[i].bytes)
      {
         fprintf(stderr, "%s: two sizes for the module at %lu\n", argv[1 + extract], entries[i].off);
         return 1;
      }
   }
   qsort(modules, (size_t)num_modules, sizeof(modules[0]), cmp_module);

   /* The table's modules must not overlap. Words between them, and after
    * the last, are modules the table does not hand out (slangmosh emits
    * one more than the interface declares); they are carried as they are
    * so that nothing moves that need not. */
   off = 0;
   for (j = 0; j < num_modules; j++)
   {
      if (modules[j].off < off || modules[j].bytes % 4 || modules[j].off + modules[j].bytes / 4 > num_words)
      {
         fprintf(stderr, "%s: module at %lu overlaps the one before or runs past the bank\n",
               argv[1 + extract], modules[j].off);
         return 1;
      }
      if (modules[j].off > off)
      {
         /* A gap: an unnamed module, kept. */
         if (num_modules == MAX_MODULES)
            return 1;
         memmove(&modules[j + 1], &modules[j], (size_t)(num_modules - j) * sizeof(modules[0]));
         modules[j].off = off;
         modules[j].bytes = (modules[j + 1].off - off) * 4;
         modules[j].words = NULL;
         num_modules++;
      }
      off = modules[j].off + modules[j].bytes / 4;
   }
   if (off < num_words)
   {
      if (num_modules == MAX_MODULES)
         return 1;
      modules[num_modules].off = off;
      modules[num_modules].bytes = (num_words - off) * 4;
      modules[num_modules].words = NULL;
      num_modules++;
   }

   /* Replacements, or with -x the modules to write out. */
   for (i = 3; i < argc; i++)
   {
      const char *eq = strchr(argv[i], '=');
      char name[64];
      char *blob;
      unsigned long blob_len, k;
      unsigned long *nw;

      if (!eq || eq - argv[i] >= (long)sizeof(name))
      {
         fprintf(stderr, "bad argument %s\n", argv[i]);
         return 2;
      }
      memcpy(name, argv[i], (size_t)(eq - argv[i]));
      name[eq - argv[i]] = 0;

      for (j = 0; j < num_entries; j++)
         if (!strcmp(entries[j].name, name))
            break;
      if (j == num_entries)
      {
         fprintf(stderr, "no module named %s in the table\n", name);
         return 1;
      }

      if (extract)
      {
         out = fopen(eq + 1, "wb");
         if (!out)
         {
            fprintf(stderr, "cannot write %s\n", eq + 1);
            return 1;
         }
         for (k = 0; k < entries[j].bytes / 4; k++)
         {
            unsigned long w = words[entries[j].off + k];
            fputc((int)(w & 0xff), out);
            fputc((int)((w >> 8) & 0xff), out);
            fputc((int)((w >> 16) & 0xff), out);
            fputc((int)((w >> 24) & 0xff), out);
         }
         fclose(out);
         printf("%-24s %8lu bytes -> %s\n", name, entries[j].bytes, eq + 1);
         continue;
      }

      blob = read_file(eq + 1, &blob_len);
      if (!blob || blob_len < 20 || blob_len % 4)
      {
         fprintf(stderr, "cannot read a SPIR-V module from %s\n", eq + 1);
         return 1;
      }
      nw = (unsigned long *)malloc(blob_len / 4 * sizeof(*nw));
      if (!nw)
         return 1;
      for (k = 0; k < blob_len / 4; k++)
      {
         const unsigned char *b = (const unsigned char *)blob + 4 * k;
         nw[k] = (unsigned long)b[0] | ((unsigned long)b[1] << 8) |
                 ((unsigned long)b[2] << 16) | ((unsigned long)b[3] << 24);
      }
      free(blob);
      if (nw[0] != 0x07230203ul)
      {
         fprintf(stderr, "%s is not SPIR-V\n", eq + 1);
         return 1;
      }

      for (k = 0; k < (unsigned long)num_modules; k++)
         if (modules[k].off == entries[j].off)
            break;
      if (modules[k].words)
         free(modules[k].words);
      modules[k].words = nw;
      modules[k].new_bytes = blob_len;
      printf("%-24s %8lu -> %8lu bytes\n", name, modules[k].bytes, blob_len);
   }

   if (extract)
   {
      free(words);
      free(text);
      return 0;
   }

   /* New layout. */
   off = 0;
   for (j = 0; j < num_modules; j++)
   {
      modules[j].new_off = off;
      if (!modules[j].words)
         modules[j].new_bytes = modules[j].bytes;
      off += modules[j].new_bytes / 4;
   }

   out = fopen(argv[2], "wb");
   if (!out)
   {
      fprintf(stderr, "cannot write %s\n", argv[2]);
      return 1;
   }
   fwrite(text, 1, (size_t)(bank_body - text), out);
   written = 0;
   for (j = 0; j < num_modules; j++)
   {
      const unsigned long *src = modules[j].words ? modules[j].words : words + modules[j].off;
      unsigned long n = modules[j].new_bytes / 4, k;
      for (k = 0; k < n; k++)
      {
         if (written % 8 == 0)
            fputc('\t', out);
         fprintf(out, "0x%08lxu,", src[k]);
         written++;
         fputc(written % 8 == 0 ? '\n' : ' ', out);
      }
   }
   /* The text after the bank opens with the newline that ends the last
    * line, so the separator after the last word goes. */
   if (written)
      fseek(out, -1, SEEK_CUR);

   /* The text after the bank, with each table entry's numbers rewritten. */
   p = bank_end;
   for (i = 0; i < num_entries; i++)
   {
      const char *call = bank_end + entries[i].line;
      const char *after = strchr(call + 14, ')');
      unsigned long k;

      fwrite(p, 1, (size_t)(call - p), out);
      for (k = 0; k < (unsigned long)num_modules; k++)
         if (modules[k].off == entries[i].off)
            break;
      fprintf(out, "(spirv_bank + %lu, %lu, &layout", modules[k].new_off, modules[k].new_bytes);
      p = after;
   }
   fwrite(p, 1, strlen(p), out);
   fclose(out);

   printf("bank: %lu -> %lu words, %d modules\n", num_words, off, num_modules);
   for (j = 0; j < num_modules; j++)
      free(modules[j].words);
   free(words);
   free(text);
   return 0;
}
