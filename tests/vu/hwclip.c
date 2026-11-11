/*  VU CLIP against console captures.
 *
 *  Scores _vuCLIP (pcsx2/VUops.c) against the hardware results in
 *  ps2autotests tests/vu/upper/clip.expected. Each case there resets the
 *  clip flag, runs one CLIP, waits out the pipeline and reads the flag
 *  back with FCGET, so a single CLIP from a known state is all that has
 *  to be modelled. The expected values are inlined, so this needs no
 *  capture checkout to run.
 *
 *  CLIP is worth pinning because it does not compare floats
 *  arithmetically. It reinterprets them as integers, forces a denormal W
 *  to the largest denormal so only normals compare above it, and tests
 *  each of x, y and z against +W and -W by flipping the sign bit. A tidy
 *  rewrite in terms of float comparisons gets the zero and denormal
 *  cases wrong, and the ±0 and MIN/MAX cases here are the ones that
 *  catch it.
 *
 *  Not covered: the Multi tests in the same capture, which pair CLIP
 *  with FCGET in a single instruction and so measure the flag pipeline
 *  rather than CLIP. Those want a model of the FMAC flag stages -- the
 *  captures show FCGET reading zero for four instructions after a CLIP,
 *  and returning fewer bits than the accumulated flag holds.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t u32; typedef int32_t s32;
typedef struct { u32 x,y,z,w; } VF;

/* Transcribed from _vuCLIP in pcsx2/VUops.c. */
static u32 clip(u32 flag, const VF* fs, const VF* ft)
{
    s32 value = (s32)ft->w;
    value = (value & 0x7f800000) ? (value & 0x7fffffff) : 0x007fffff;
    flag <<= 6;
    if ((s32)(fs->x ^ 0x00000000u) > value) flag |= 0x01;
    if ((s32)(fs->x ^ 0x80000000u) > value) flag |= 0x02;
    if ((s32)(fs->y ^ 0x00000000u) > value) flag |= 0x04;
    if ((s32)(fs->y ^ 0x80000000u) > value) flag |= 0x08;
    if ((s32)(fs->z ^ 0x00000000u) > value) flag |= 0x10;
    if ((s32)(fs->z ^ 0x80000000u) > value) flag |= 0x20;
    return flag & 0xFFFFFFu;
}
static VF q(u32 a,u32 b,u32 c,u32 d){ VF v; v.x=a;v.y=b;v.z=c;v.w=d; return v; }
int main(void)
{
    const VF FULLPOS = q(0x40000000,0x40000000,0x40000000,0x40000000);
    const VF FULLNEG = q(0xC0000000,0xC0000000,0xC0000000,0xC0000000);
    const VF HALFPOS = q(0x40000000,0xC0000000,0x40000000,0xC0000000);
    const VF HALFNEG = q(0xC0000000,0x40000000,0xC0000000,0x40000000);
    const VF WZERO   = q(0,0,0,0);
    const VF WNEGZERO= q(0x80000000,0x80000000,0x80000000,0x80000000);
    const VF WNEGONE = q(0xBF800000,0xBF800000,0xBF800000,0xBF800000);
    const VF MAXV    = q(0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF,0x7FFFFFFF);
    const VF MINV    = q(0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF);
    struct { const char* name; VF s, t; u32 want; } t[] = {
        {"FULL POS x -1",       FULLPOS, WNEGONE,  0x0015},
        {"FULL NEG x -1",       FULLNEG, WNEGONE,  0x002a},
        {"HALF POS x -1",       HALFPOS, WNEGONE,  0x0019},
        {"HALF NEG x -1",       HALFNEG, WNEGONE,  0x0026},
        {"HALF NEG x  0",       HALFNEG, WZERO,    0x0026},
        {"HALF NEG x -0",       HALFNEG, WNEGZERO, 0x0026},
        {" 0 x -0",             WZERO,   WNEGZERO, 0x0000},
        {"-0 x  0",             WNEGZERO,WZERO,    0x0000},
        {"FULL POS x FULL POS", FULLPOS, FULLPOS,  0x0000},
        {"HALF NEG x HALF NEG", HALFNEG, HALFNEG,  0x0000},
        {"MIN x 0",             MINV,    WZERO,    0x002a},
        {"MAX x 0",             MAXV,    WZERO,    0x0015},
        {"0 x MIN",             WZERO,   MINV,     0x0000},
        {"0 x MAX",             WZERO,   MAXV,     0x0000},
    };
    int i, bad = 0;
    const int n = (int)(sizeof(t)/sizeof(t[0]));
    for (i = 0; i < n; i++) {
        const u32 got = clip(0, &t[i].s, &t[i].t);
        if (got != t[i].want) {
            bad++;
            printf("  %-20s console %04x  ours %04x\n", t[i].name, t[i].want, got);
        }
    }
    printf("vuclip: %d/%d console cases\n", n - bad, n);
    return bad != 0;
}
