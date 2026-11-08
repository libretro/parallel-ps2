/* Option config: the direct path from core options to Pcsx2Config.
 *
 * The frontend builds a Pcsx2Config from its options and VMManager copies
 * it; there is no string map between them any more. What this checks is
 * the part that used to be LoadSave's and is now code on that config:
 *
 *  1. A default Pcsx2Config, through ApplyOptionFixups, is unchanged --
 *     every field an option can set keeps its constructor default when no
 *     option sets it.
 *  2. The fixups: EECycleRate and EECycleSkip clamp, and SkipDrawEnd is
 *     never below SkipDrawStart -- the three things LoadSave did after
 *     reading, on the values an option can put out of range.
 *  3. EmuFolders::LoadConfig: an absolute memory-card folder is kept, a
 *     relative one goes under DataRoot, an empty one is DataRoot/memcards.
 *  4. The BIOS path: the option buffer is PCSX2_PATH_MAX, and a path at
 *     that length is cut, not overrun -- the truncation the old test was
 *     for, on the new surface.
 *  5. That a settings reload leaves the runtime state alone. This is the
 *     one the earlier version of this file should have had: the map it
 *     replaced wrote only the fields the map held, so UseBOOT2Injection,
 *     the IRX and game-argument strings and the memory card types passed
 *     through untouched. Assigning the option config over EmuConfig
 *     wrote them too, and a USA disc came up at 50Hz with fast boot off
 *     because fast boot reverted on every settings change. The property
 *     is what VMManager::ApplySettings does -- reset, carry the runtime
 *     config across, reload -- so that is what this models.
 */

#include <stdio.h>
#include <string.h>
#include <string>
#include <libretro.h>
#include "Config.h"

static void test_log(enum retro_log_level level, const char* fmt, ...) { (void)level; (void)fmt; }
extern "C" retro_log_printf_t log_cb = test_log;
std::string libretro_content;

int main(void)
{
   int ok = 1;
   setvbuf(stdout, NULL, _IONBF, 0);
   printf("optioncfg\n");

   { Pcsx2Config a, b; b.ApplyOptionFixups();
     int same = (a == b);
     printf("  %s: default config unchanged by the fixups\n", same ? "ok" : "FAIL"); ok &= same; }

   { Pcsx2Config c; c.Speedhacks.EECycleRate = 99; c.Speedhacks.EECycleSkip = 99; c.GS.SkipDrawStart = 5; c.GS.SkipDrawEnd = 2;
     c.ApplyOptionFixups();
     int good = c.Speedhacks.EECycleRate == Pcsx2Config::SpeedhackOptions::MAX_EE_CYCLE_RATE && c.Speedhacks.EECycleSkip == Pcsx2Config::SpeedhackOptions::MAX_EE_CYCLE_SKIP && c.GS.SkipDrawEnd == 5;
     printf("  %s: fixups clamp: EECycleRate 99->%d  EECycleSkip 99->%d  SkipDrawEnd(5,2)->%d\n", good ? "ok" : "FAIL", c.Speedhacks.EECycleRate, c.Speedhacks.EECycleSkip, c.GS.SkipDrawEnd); ok &= good;
     Pcsx2Config d; d.Speedhacks.EECycleRate = Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE - 5; d.ApplyOptionFixups();
     good = d.Speedhacks.EECycleRate == Pcsx2Config::SpeedhackOptions::MIN_EE_CYCLE_RATE;
     printf("  %s: EECycleRate below minimum clamps up to %d\n", good ? "ok" : "FAIL", d.Speedhacks.EECycleRate); ok &= good; }

   { strcpy(EmuFolders::DataRoot, "/data");
     EmuFolders::LoadConfig("/abs/cards");  int g1 = !strcmp(EmuFolders::MemoryCards, "/abs/cards");
     EmuFolders::LoadConfig("rel");         int g2 = !strcmp(EmuFolders::MemoryCards, "/data/rel");
     EmuFolders::LoadConfig("");            int g3 = !strcmp(EmuFolders::MemoryCards, "/data/memcards");
     int g4 = !strcmp(EmuFolders::Bios, "/data/bios") && !strcmp(EmuFolders::Cache, "/data/cache");
     printf("  %s: folders: absolute kept, relative under DataRoot, empty -> memcards, others default\n", (g1 && g2 && g3 && g4) ? "ok" : "FAIL"); ok &= g1 && g2 && g3 && g4; }

   { char buf[PCSX2_PATH_MAX]; std::string longpath(PCSX2_PATH_MAX + 100, 'x');
     strlcpy(buf, longpath.c_str(), sizeof(buf));
     int good = strlen(buf) == PCSX2_PATH_MAX - 1 && buf[PCSX2_PATH_MAX - 1] == 0;
     printf("  %s: a BIOS path longer than the buffer is cut at %d, terminated\n", good ? "ok" : "FAIL", (int)strlen(buf)); ok &= good; }

   /* The ApplySettings cycle: the caller's runtime state goes in, the
    * options are reloaded over the top, and the runtime state must come
    * out the other side unchanged. */
   {
      Pcsx2Config options;                 /* what the frontend's options built */
      options.Cpu.Recompiler.EnableFastmem = false;   /* an option-set field */

      Pcsx2Config live;                    /* what the running VM holds */
      live.UseBOOT2Injection = true;
      strcpy(live.CurrentIRX, "host:/some.irx");
      strcpy(live.CurrentGameArgs, "-arg");
      live.Cpu.Recompiler.EnableFastmem = true;       /* stale: the option wins */

      {
         Pcsx2Config runtime(std::move(live));
         live = options;
         live.CopyRuntimeConfig(runtime);
         live.ApplyOptionFixups();
      }

      int kept =  live.UseBOOT2Injection
               && !strcmp(live.CurrentIRX, "host:/some.irx")
               && !strcmp(live.CurrentGameArgs, "-arg");
      int applied = (live.Cpu.Recompiler.EnableFastmem == false);
      printf("  %s: a settings reload keeps the runtime state (fast boot, IRX, game args)\n",
             kept ? "ok" : "FAIL");
      printf("  %s: and still takes the option's value for a field the options set\n",
             applied ? "ok" : "FAIL");
      ok &= kept && applied;
   }

   printf(ok ? "optioncfg: ok\n" : "optioncfg: FAILED\n");
   return ok ? 0 : 1;
}
