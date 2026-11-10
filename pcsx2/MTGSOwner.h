/* Who drains the GS ring when a thread asks to wait for it.
 *
 * One rule, in one place, because it has been got wrong three times by
 * being inferred from things that do not determine it:
 *
 *   - which thread opened the GS. Under threaded video the frontend
 *     calls create_device and context_reset on its video thread, so the
 *     opener is a thread that never drains anything;
 *   - which thread last ran MainLoop. The EE reaches it through a wait
 *     at boot, and then owns a ring it must never run;
 *   - a claim made in retro_run. context_reset resumes the EE, so the
 *     EE is already waiting before the first retro_run, and the first
 *     thing that retro_run does is wait as well.
 *
 * What does determine it is which threads produce. The EE and the MTVU
 * worker write the ring and must never run it: the GS holds a hardware
 * context that is not current on them, and a vsync calls the frontend's
 * video callback. Every other caller is on the frontend's side -- its
 * main thread, or its video thread inside a context callback while the
 * main thread is blocked on it -- and a wait from there can only be
 * served by draining, because nothing else will.
 *
 * C, no dependencies: tests/mtgsown includes this and runs the threads.
 */
#ifndef PCSX2_MTGS_OWNER_H
#define PCSX2_MTGS_OWNER_H

#include <stdint.h>

/* self:     the calling thread
 * producer: the EE thread, 0 while there is none
 * is_mtvu:  the call is from the MTVU worker
 * Nonzero when the caller must drain the ring itself, zero when it must
 * park until the ring has been drained for it. */
#define MTGS_WAIT_DRAINS(self, producer, is_mtvu) \
	(!(is_mtvu) && (self) != (producer))

/* What happens to a vsync, and where the EE stops for a pause.
 *
 * Every frame runs on the input of the retro_run that presents it.
 * retro_run polls, latches the input and lets the EE start a frame, then
 * consumes the vsync that ends it, scans the frame out and presents it.
 * The EE waits behind that vsync, at the frame gate, until the next
 * retro_run lets it start the next frame, so between two retro_runs it
 * runs nothing. Anything else that empties the ring - pausing the
 * EE for a savestate, a reset, an option change, a disc swap, the
 * frontend taking the context away - is a drain, and a drain never
 * presents: a present is a call into the frontend's video_refresh, which
 * belongs inside retro_run and nowhere else. The drained vsync is kept in
 * mtgs_pending instead, and the next retro_run presents it rather than
 * waiting for another, so every retro_run is one frame however many
 * drains came between.
 *
 * A pause stops the EE at a vsync: at the gate, behind the frame
 * retro_run presented, or at the next vsync, which a drain consumes, when
 * it is asked while the EE runs a frame. At the gate nothing is pending,
 * and the next retro_run runs a frame on its own input. Anything else
 * that interrupts the EE (stopping, a CPU change) still stops it at
 * once.
 *
 * interrupted: VMManager asks the EE to stop
 * paused:      and what it asks for is a pause
 * drained:     the vsync the EE was let go from was a drain's, or the
 *              EE was sent out of the gate to stop */
#define MTGS_EE_STOPS(interrupted, paused, drained) \
	((interrupted) && (!(paused) || (drained)))

/* A drained vsync, kept for the next retro_run: the GS's copy of the
 * privileged registers at that vsync, the field it scans out, whether the
 * registers changed since the one before, and the audio mark that ends
 * its frame. Frontend's thread only. */
struct mtgs_frame
{
	uint8_t  regs[0x2000]; /* PS2MEM_GS_REGS */
	uint32_t audio_mark;
	uint32_t field;
	uint32_t registers_written;
	uint32_t pending;
};

/* Defined in MTGS.c, with C linkage. */
#ifdef __cplusplus
extern "C" {
#endif
void mtgs_claim_ring(void);            /* frontend, first thing in retro_run */
void mtgs_set_producer_thread(int on); /* EE thread, as it starts and ends   */

/* Frontend's thread, retro_run: scan out and present the drained frame,
 * when there is one. Nonzero when it did. */
int mtgs_present_pending(void);
/* Frontend's thread, retro_run, after polling: latch the input and let
 * the EE held at the gate start its frame. Nonzero when one was held. */
int mtgs_frame_gate_open(void);
/* Frontend's thread: on as a pause is asked for, off before a resume. */
void mtgs_frame_gate_stop(int on);

extern struct mtgs_frame mtgs_pending;
/* The audio mark of the last vsync retro_run consumed. */
extern uint32_t mtgs_presented_mark;
/* Set by whoever consumed the vsync the EE was last let go from: nonzero
 * for a drain. Read by the EE after its wait. */
extern int mtgs_vsync_drained;
#ifdef __cplusplus
}
#endif

#endif
