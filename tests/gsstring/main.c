/* GSString (GS/Renderers/Common/GSString.h): the text is always
 * NUL-terminated and what was appended, through growth past the first
 * allocation, numbers at the edges of the range, clears that keep the
 * allocation, and free, after which the string is usable again. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "GS/Renderers/Common/GSString.h"

static int s_fails;
#define CHECK(c, what) do { if (!(c)) { printf("  FAIL: %s\n", what); s_fails++; } } while (0)

int main(void)
{
   gs_str_t s;
   char expect[70000];
   size_t i, n;

   gs_str_init(&s);
   CHECK(GS_STR(&s)[0] == 0 && GS_STR_LEN(&s) == 0, "an empty string reads as \"\"");

   CHECK(gs_str_cat(&s, "#define ") && gs_str_cat(&s, "PS_FST") && gs_str_cat(&s, " ")
         && gs_str_cat_int(&s, 1) && gs_str_cat(&s, "\n"), "a macro line appends");
   CHECK(strcmp(GS_STR(&s), "#define PS_FST 1\n") == 0, "and reads back as one line");
   CHECK(GS_STR_LEN(&s) == strlen(GS_STR(&s)), "the length is the text's");

   gs_str_clear(&s);
   CHECK(GS_STR(&s)[0] == 0 && GS_STR_LEN(&s) == 0 && s.chars.capacity > 0, "clear empties and keeps the room");

   CHECK(gs_str_cat_int(&s, 0) && gs_str_cat(&s, " ") && gs_str_cat_int(&s, -7) && gs_str_cat(&s, " ")
         && gs_str_cat_int(&s, LONG_MAX) && gs_str_cat(&s, " ") && gs_str_cat_int(&s, LONG_MIN), "numbers append");
   sprintf(expect, "0 -7 %ld %ld", LONG_MAX, LONG_MIN);
   CHECK(strcmp(GS_STR(&s), expect) == 0, "zero, negative and the extremes read back in decimal");

   /* Growth: past 64 KiB of text, a byte at a time and in runs. */
   gs_str_clear(&s);
   n = 0;
   for (i = 0; i < 65536 + 1000; i++)
   {
      const char piece[] = "abcdefghij";
      if (i % 7 == 0)
      {
         CHECK(gs_str_catn(&s, piece, 3), "a run appends");
         memcpy(expect + n, piece, 3); n += 3;
      }
      else
      {
         char c = (char)('a' + i % 26);
         CHECK(gs_str_catn(&s, &c, 1), "a byte appends");
         expect[n++] = c;
      }
      if (n > sizeof(expect) - 8)
         break;
   }
   expect[n] = 0;
   CHECK(GS_STR_LEN(&s) == n && memcmp(GS_STR(&s), expect, n + 1) == 0, "the text past 64 KiB is what was appended, terminated");

   gs_str_free(&s);
   CHECK(GS_STR_LEN(&s) == 0 && GS_STR(&s)[0] == 0, "freed, it is empty");
   CHECK(gs_str_cat(&s, "again") && strcmp(GS_STR(&s), "again") == 0, "and usable again");
   gs_str_free(&s);

   if (s_fails)
   {
      printf("gsstring: %d failed\n", s_fails);
      return 1;
   }
   printf("gsstring: ok\n");
   return 0;
}
