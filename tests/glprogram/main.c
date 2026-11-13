/* GLProgram.c against a stub GL driver: a long run of compiles, links,
 * binaries, polls, uniforms, binds and moves over a few programs makes the
 * same GL calls and log lines, in the same order with the same arguments,
 * as the C++ class it replaced did (the trace below was taken from it).
 * Then what is new: the uniform locations are held in the program, and a
 * registration past the room logs and sets nothing. */

#include <stdio.h>
#include <string.h>

#include "GS/Renderers/OpenGL/GLProgram.h"
#include "stubs.h"

static gs_gl_program s_progs[4];

#define P_RESET()                gs_gl_program_reset_last()
#define P_DESTROY(i)             gs_gl_program_destroy(&s_progs[i])
#define P_COMPILE(i, v, vl, f, fl) gs_gl_program_compile(&s_progs[i], v, vl, f, fl)
#define P_HINT(i)                gs_gl_program_set_binary_retrievable_hint(&s_progs[i])
#define P_LINK(i)                gs_gl_program_link(&s_progs[i])
#define P_FROMBIN(i, d, l, f)    gs_gl_program_create_from_binary(&s_progs[i], d, l, f)
#define P_GETBIN(i, b, f)        gs_gl_program_get_binary(&s_progs[i], b, f)
#define P_START(i, v, vl, f, fl) gs_gl_program_start_compile(&s_progs[i], v, vl, f, fl)
#define P_POLL(i)                gs_gl_program_poll(&s_progs[i])
#define P_REG(i, n)              gs_gl_program_register_uniform(&s_progs[i], n)
#define P_U1UI(i, k, x)          gs_gl_program_uniform1ui(&s_progs[i], k, x)
#define P_U3UI(i, k, x, y, z)    gs_gl_program_uniform3ui(&s_progs[i], k, x, y, z)
#define P_U4UI(i, k, x, y, z, w) gs_gl_program_uniform4ui(&s_progs[i], k, x, y, z, w)
#define P_U1I(i, k, x)           gs_gl_program_uniform1i(&s_progs[i], k, x)
#define P_U2I(i, k, x, y)        gs_gl_program_uniform2i(&s_progs[i], k, x, y)
#define P_U1F(i, k, x)           gs_gl_program_uniform1f(&s_progs[i], k, x)
#define P_U2IV(i, k, v)          gs_gl_program_uniform2iv(&s_progs[i], k, v)
#define P_U4IV(i, k, v)          gs_gl_program_uniform4iv(&s_progs[i], k, v)
#define P_U2FV(i, k, v)          gs_gl_program_uniform2fv(&s_progs[i], k, v)
#define P_U4FV(i, k, v)          gs_gl_program_uniform4fv(&s_progs[i], k, v)
#define P_BIND(i)                gs_gl_program_bind(&s_progs[i])
#define P_MOVE(a, b)             (gs_gl_program_destroy(&s_progs[a]), s_progs[a] = s_progs[b], gs_gl_program_init(&s_progs[b]))
#define P_PENDING(i)             (s_progs[i].pending != 0)
#define P_VALID(i)               (s_progs[i].program_id != 0)

#include "scenario.h"

/* The C++ GLProgram's trace of this scenario */
#define EXPECTED_TRACE 0xda57b7a8u

static int s_failed = 0;

static void check(int ok, const char* what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		s_failed = 1;
	}
}

static void overflow(void)
{
	gs_gl_program p;
	u32 calls;
	int i, id = 0;

	gs_gl_program_init(&p);
	check(p.uniforms[GS_GL_PROGRAM_MAX_UNIFORMS] == -1, "the spare uniform slot is none");
	p.program_id = 99; /* (a program the stub never made: just a name) */
	for (i = 0; i < GS_GL_PROGRAM_MAX_UNIFORMS; i++)
		check(gs_gl_program_register_uniform(&p, "u") == i, "registrations count up");
	calls = g_calls;
	id = gs_gl_program_register_uniform(&p, "one too many");
	check(id == GS_GL_PROGRAM_MAX_UNIFORMS, "a registration past the room gets the spare");
	check(g_calls == calls, "and does not ask the driver");
	gs_gl_program_uniform4ui(&p, id, 1, 2, 3, 4);
	gs_gl_program_uniform1f(&p, id, 1.0f);
	check(g_calls == calls, "setting the spare calls nothing");
	check(p.uniform_count == GS_GL_PROGRAM_MAX_UNIFORMS, "the count stays at the room");
	p.program_id = 0;
	gs_gl_program_destroy(&p);
	check(p.uniform_count == 0, "destroy forgets the uniforms");
	check(p.uniforms[GS_GL_PROGRAM_MAX_UNIFORMS] == -1, "and the spare is still none");
}

int main(int argc, char** argv)
{
	u32 trace;
	int i;

	for (i = 0; i < 4; i++)
		gs_gl_program_init(&s_progs[i]);
	trace = run_scenario(20000);
	printf("trace %08x after %u GL calls\n", (unsigned)trace, (unsigned)g_calls);
	if (argc > 1 && strcmp(argv[1], "--print") == 0)
		return 0;
	check(trace == EXPECTED_TRACE, "the GL calls and logs match the C++ class");

	overflow();

	if (s_failed)
		return 1;
	printf("glprogram: ok\n");
	return 0;
}
