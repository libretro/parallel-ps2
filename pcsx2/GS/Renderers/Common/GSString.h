/* A growable string: a GSArray of char kept NUL-terminated, so the
 * text is always there to hand to a C API. Appends grow the allocation
 * and report failure; the string is unchanged by one that fails. C89,
 * header-only, like GSArray.h. */
#ifndef GS_STRING_H
#define GS_STRING_H

#include <string.h>
#include "GSArray.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gs_str
{
   gs_array_t chars; /* count is the length; chars.items[count] is NUL */
} gs_str_t;

/* The text (empty, never NULL, once init has run). */
#define GS_STR(s)     ((s)->chars.items ? (const char*)(s)->chars.items : "")
#define GS_STR_LEN(s) ((s)->chars.count)

/* An empty string. Allocates nothing. */
static void gs_str_init(gs_str_t *s)
{
   gs_array_init(&s->chars, 1);
}

static void gs_str_free(gs_str_t *s)
{
   gs_array_free(&s->chars);
}

/* Empty again; the allocation stays for the next text. */
static void gs_str_clear(gs_str_t *s)
{
   s->chars.count = 0;
   if (s->chars.items)
      s->chars.items[0] = 0;
}

/* n bytes of text onto the end. Zero if the string could not grow. */
static int gs_str_catn(gs_str_t *s, const char *text, size_t n)
{
   if (!gs_array_reserve(&s->chars, s->chars.count + n + 1))
      return 0;
   memcpy(s->chars.items + s->chars.count, text, n);
   s->chars.count += n;
   s->chars.items[s->chars.count] = 0;
   return 1;
}

static int gs_str_cat(gs_str_t *s, const char *text)
{
   return gs_str_catn(s, text, strlen(text));
}

/* A number in decimal onto the end. */
static int gs_str_cat_int(gs_str_t *s, long value)
{
   char digits[24];
   char *p = digits + sizeof(digits);
   unsigned long u = value < 0 ? 0ul - (unsigned long)value : (unsigned long)value;
   *--p = 0;
   do
   {
      *--p = (char)('0' + u % 10);
      u /= 10;
   } while (u);
   if (value < 0)
      *--p = '-';
   return gs_str_cat(s, p);
}

#ifdef __cplusplus
}
#endif

#endif
